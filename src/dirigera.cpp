// dirigera.cpp
//
// VSCP level II driver for the IKEA Dirigera hub
// (Matter/Zigbee devices exposed through the hub REST API).
//
// This file is part of the VSCP (https://www.vscp.org)
//
// The MIT License (MIT)
//
// Copyright (C) 2000-2026 Ake Hedman and contributors,
// the VSCP project <info@vscp.org>

#include "dirigera.h"

#include <canal.h>
#include <vscp-class.h>
#include <vscp-type.h>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <curl/curl.h>
#include <libwebsockets.h>

#include <arpa/inet.h>
#include <netdb.h>

#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

using json = nlohmann::json;

// Max number of events held waiting for the daemon to read them
static constexpr size_t MAX_QUEUE_SIZE = 2048;

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------

static void
deleteEvent(vscpEvent *pEvent)
{
  if (nullptr != pEvent) {
    delete[] pEvent->pdata;
    delete pEvent;
  }
}

// Parse a GUID on string form "FF:FF:...:00" into 16 bytes
static bool
parseGuid(const char *str, uint8_t *guid)
{
  memset(guid, 0, 16);
  if ((nullptr == str) || (0 == *str)) {
    return false;
  }

  int pos = 0;
  const char *p = str;
  while ((pos < 16) && *p) {
    char *end = nullptr;
    long val = strtol(p, &end, 16);
    if (end == p) {
      return false;
    }
    guid[pos++] = static_cast<uint8_t>(val & 0xff);
    p = (*end == ':') ? end + 1 : end;
  }

  return (16 == pos);
}

static std::string
guidToString(const uint8_t *guid)
{
  char buf[64];
  char *p = buf;
  for (int i = 0; i < 16; i++) {
    p += sprintf(p, i ? ":%02X" : "%02X", guid[i]);
  }
  return buf;
}

// Resolve a host name (or dotted quad) to an IPv4 address string
static bool
resolveIpv4(const std::string &host, std::string &ip)
{
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo *res = nullptr;
  if ((0 != getaddrinfo(host.c_str(), nullptr, &hints, &res)) ||
      (nullptr == res)) {
    return false;
  }

  char buf[INET_ADDRSTRLEN];
  const struct sockaddr_in *sin =
    reinterpret_cast<const struct sockaddr_in *>(res->ai_addr);
  const char *p = inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
  freeaddrinfo(res);

  if (nullptr == p) {
    return false;
  }

  ip = buf;
  return true;
}

// Look up the MAC address for an IPv4 address in the kernel neighbour
// table (/proc/net/arp). The hub has just been talked to over TCP so an
// entry exists when the hub is on the local subnet.
static bool
lookupArpMac(const std::string &ip, uint8_t mac[6])
{
  std::ifstream in("/proc/net/arp");
  if (!in.is_open()) {
    return false;
  }

  std::string line;
  std::getline(in, line); // skip the header line

  while (std::getline(in, line)) {
    char addr[64] = { 0 };
    char hwtype[16] = { 0 };
    char flags[16] = { 0 };
    char hwaddr[64] = { 0 };
    if (sscanf(line.c_str(), "%63s %15s %15s %63s", addr, hwtype, flags,
               hwaddr) < 4) {
      continue;
    }
    if (ip != addr) {
      continue;
    }
    if (0 == strtol(flags, nullptr, 16)) {
      continue; // incomplete entry
    }

    unsigned m[6];
    if (6 != sscanf(hwaddr, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3],
                    &m[4], &m[5])) {
      continue;
    }
    if (0 == (m[0] | m[1] | m[2] | m[3] | m[4] | m[5])) {
      continue; // all zero hardware address
    }

    for (int i = 0; i < 6; i++) {
      mac[i] = static_cast<uint8_t>(m[i]);
    }
    return true;
  }

  return false;
}

// Uppercase a UTF-8 string for case insensitive comparison. Handles ASCII
// and the Latin-1 supplement (å, ä, ö, é, ü, ...). Other code points are
// left unchanged.
static std::string
utf8ToUpper(const std::string &s)
{
  std::string out;
  out.reserve(s.size());

  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);

    if (c < 0x80) {
      out += static_cast<char>(toupper(c));
      i++;
      continue;
    }

    // Two byte sequence 0xC3 0x80-0xBF = U+00C0 - U+00FF
    if ((0xC3 == c) && (i + 1 < s.size())) {
      uint32_t cp = 0xC0 + (static_cast<unsigned char>(s[i + 1]) & 0x3F);
      // U+00E0 - U+00FE map to uppercase by -0x20, except the division
      // sign U+00F7 (and U+00FF which has no Latin-1 uppercase)
      if ((cp >= 0xE0) && (cp <= 0xFE) && (0xF7 != cp)) {
        cp -= 0x20;
      }
      out += static_cast<char>(0xC3);
      out += static_cast<char>(0x80 | (cp & 0x3F));
      i += 2;
      continue;
    }

    out += static_cast<char>(c);
    i++;
  }

  return out;
}

// Device names are ALWAYS compared case insensitively (UTF-8,
// ASCII + Latin-1). Use this helper for every device name comparison.
static bool
nameEqual(const std::string &a, const std::string &b)
{
  return utf8ToUpper(a) == utf8ToUpper(b);
}

// Sensor index from a trailing "-<n>" or "_<n>" on the unique device id.
// Only short (1-2 digit) all-numeric suffixes are accepted so that the last
// group of a plain UUID is not mistaken for an index.
static uint8_t
sensorIndexFromId(const std::string &id)
{
  size_t pos = id.find_last_of("-_");
  if ((pos == std::string::npos) || (pos + 1 >= id.size())) {
    return 0;
  }

  const std::string suffix = id.substr(pos + 1);
  if (suffix.size() > 2 ||
      suffix.find_first_not_of("0123456789") != std::string::npos) {
    return 0;
  }

  return static_cast<uint8_t>(atoi(suffix.c_str()) & 0x07);
}

// Append an IEEE-754 single precision float MSB first
static void
appendFloat(std::vector<uint8_t> &data, double value)
{
  float f = static_cast<float>(value);
  uint32_t bits;
  memcpy(&bits, &f, sizeof(bits));
  data.push_back(static_cast<uint8_t>((bits >> 24) & 0xff));
  data.push_back(static_cast<uint8_t>((bits >> 16) & 0xff));
  data.push_back(static_cast<uint8_t>((bits >> 8) & 0xff));
  data.push_back(static_cast<uint8_t>(bits & 0xff));
}

// ----------------------------------------------------------------------------
// DirigeraClient
// ----------------------------------------------------------------------------

static size_t
curlWriteCallback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
  auto *out = static_cast<std::string *>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

void
DirigeraClient::init(const std::string &host,
                     uint16_t port,
                     const std::string &token,
                     bool verifyTls)
{
  static std::once_flag curlInitFlag;
  std::call_once(curlInitFlag, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });

  m_baseUrl = "https://" + host + ":" + std::to_string(port) + "/v1";
  m_token = token;
  m_verifyTls = verifyTls;
}

bool
DirigeraClient::perform(const std::string &method,
                        const std::string &path,
                        const std::string &body,
                        std::string &response,
                        std::string &err)
{
  CURL *curl = curl_easy_init();
  if (nullptr == curl) {
    err = "Failed to init curl";
    return false;
  }

  response.clear();

  struct curl_slist *headers = nullptr;
  headers = curl_slist_append(headers,
                              ("Authorization: Bearer " + m_token).c_str());
  headers = curl_slist_append(headers, "Content-Type: application/json");

  curl_easy_setopt(curl, CURLOPT_URL, (m_baseUrl + path).c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  if (!m_verifyTls) {
    // The Dirigera hub uses a self signed certificate
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  }

  if (method != "GET") {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  }

  CURLcode rv = curl_easy_perform(curl);
  long httpCode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (CURLE_OK != rv) {
    err = curl_easy_strerror(rv);
    return false;
  }

  if ((httpCode < 200) || (httpCode >= 300)) {
    err = "HTTP status " + std::to_string(httpCode) + " - " + response;
    return false;
  }

  return true;
}

bool
DirigeraClient::getDevices(json &devices, std::string &err)
{
  std::string response;
  if (!perform("GET", "/devices", "", response, err)) {
    return false;
  }

  try {
    devices = json::parse(response);
  }
  catch (const std::exception &e) {
    err = std::string("Failed to parse device list: ") + e.what();
    return false;
  }

  if (!devices.is_array()) {
    err = "Device list is not a JSON array";
    return false;
  }

  return true;
}

bool
DirigeraClient::getHome(json &home, std::string &err)
{
  std::string response;
  if (!perform("GET", "/home", "", response, err)) {
    return false;
  }

  try {
    home = json::parse(response);
  }
  catch (const std::exception &e) {
    err = std::string("Failed to parse home state: ") + e.what();
    return false;
  }

  if (!home.is_object()) {
    err = "Home state is not a JSON object";
    return false;
  }

  return true;
}

bool
DirigeraClient::setAttributes(const std::string &deviceId,
                              const json &attributes,
                              std::string &err)
{
  json body = json::array({ { { "attributes", attributes } } });
  std::string response;
  return perform("PATCH", "/devices/" + deviceId, body.dump(), response, err);
}

// ----------------------------------------------------------------------------
// CIkeaDirigera
// ----------------------------------------------------------------------------

CIkeaDirigera::CIkeaDirigera() = default;

CIkeaDirigera::~CIkeaDirigera()
{
  close();
}

void
CIkeaDirigera::setupLogging(const json &cfg)
{
  std::vector<spdlog::sink_ptr> sinks;

  bool consoleEnable = true;
  std::string consoleLevel = "info";
  bool fileEnable = false;
  std::string fileLevel = "info";
  std::string filePath = "/var/log/vscp/vscpl2drv-ikea-dirigera.log";
  size_t fileMaxSize = 5 * 1024 * 1024;
  size_t fileMaxFiles = 7;

  if (cfg.contains("logging") && cfg["logging"].is_object()) {
    const json &lcfg = cfg["logging"];
    consoleEnable = lcfg.value("console-enable", consoleEnable);
    consoleLevel = lcfg.value("console-level", consoleLevel);
    fileEnable = lcfg.value("file-enable", fileEnable);
    fileLevel = lcfg.value("file-level", fileLevel);
    filePath = lcfg.value("file-path", filePath);
    fileMaxSize = lcfg.value("file-max-size", fileMaxSize);
    fileMaxFiles = lcfg.value("file-max-files", fileMaxFiles);
  }

  if (consoleEnable) {
    auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    sink->set_level(spdlog::level::from_str(consoleLevel));
    sinks.push_back(sink);
  }

  if (fileEnable) {
    try {
      auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        filePath,
        fileMaxSize,
        fileMaxFiles);
      sink->set_level(spdlog::level::from_str(fileLevel));
      sinks.push_back(sink);
    }
    catch (const std::exception &e) {
      fprintf(stderr,
              "vscpl2drv-ikea-dirigera: failed to open log file %s: %s\n",
              filePath.c_str(),
              e.what());
    }
  }

  m_log = std::make_shared<spdlog::logger>("ikea-dirigera",
                                           sinks.begin(),
                                           sinks.end());
  m_log->set_level(spdlog::level::trace); // sinks do the filtering
  m_log->flush_on(spdlog::level::err);
}

bool
CIkeaDirigera::loadConfig(const std::string &path)
{
  json cfg;

  try {
    std::ifstream in(path);
    if (!in.is_open()) {
      fprintf(stderr,
              "vscpl2drv-ikea-dirigera: unable to open configuration file "
              "%s\n",
              path.c_str());
      return false;
    }
    in >> cfg;
  }
  catch (const std::exception &e) {
    fprintf(stderr,
            "vscpl2drv-ikea-dirigera: failed to parse configuration file "
            "%s: %s\n",
            path.c_str(),
            e.what());
    return false;
  }

  setupLogging(cfg);

  m_host = cfg.value("hub", "");
  m_port = static_cast<uint16_t>(cfg.value("port", 8443));
  m_token = cfg.value("token", "");
  m_verifyTls = cfg.value("verify-tls", false);
  m_useWebsocket = cfg.value("use-websocket", false);
  m_pollInterval = cfg.value("poll-interval", 5u);
  m_heartbeatInterval = cfg.value("heartbeat-interval", 30u);
  m_zone = static_cast<uint8_t>(cfg.value("zone", 0));
  m_dbPath = cfg.value("database",
                       "/var/lib/vscp/vscpl2drv-ikea-dirigera.sqlite3");

  if (m_pollInterval < 1) {
    m_pollInterval = 1;
  }

  if (m_host.empty()) {
    m_log->error("No hub address ('hub') in configuration file {}", path);
    return false;
  }

  if (m_token.empty()) {
    m_log->error("No access token ('token') in configuration file {}", path);
    return false;
  }

  // Optional device mappings: match on name (or unique id) and assign
  // GUID, zone, subzone and expected type
  if (cfg.contains("devices") && cfg["devices"].is_array()) {
    for (const auto &jdev : cfg["devices"]) {
      if (!jdev.is_object()) {
        continue;
      }

      DeviceMapping map;
      map.name = jdev.value("name", "");
      map.id = jdev.value("id", "");
      map.type = jdev.value("type", "");
      map.index = jdev.value("index", -1);
      map.zone = jdev.value("zone", -1);
      map.subzone = jdev.value("subzone", -1);

      if (map.name.empty() && map.id.empty()) {
        m_log->warn("Device mapping without 'name' or 'id' ignored");
        continue;
      }

      if (map.index > 255) {
        m_log->warn("Index {} in device mapping for '{}' out of range "
                    "(0-255) - ignored",
                    map.index,
                    map.name.empty() ? map.id : map.name);
        map.index = -1;
      }
      if (map.index >= 0) {
        if (m_indexReserved[map.index]) {
          m_log->warn("Index {} in device mapping for '{}' is already "
                      "mapped to another device - ignored",
                      map.index,
                      map.name.empty() ? map.id : map.name);
          map.index = -1;
        }
        else {
          m_indexReserved[map.index] = true;
        }
      }

      if (jdev.contains("guid") && jdev["guid"].is_string()) {
        map.hasGuid =
          parseGuid(jdev["guid"].get<std::string>().c_str(), map.guid);
        if (!map.hasGuid) {
          m_log->warn("Invalid GUID '{}' in device mapping for '{}' ignored",
                      jdev["guid"].get<std::string>(),
                      map.name.empty() ? map.id : map.name);
        }
      }

      m_mappings.push_back(map);
    }
  }

  return true;
}

bool
CIkeaDirigera::open(const std::string &pathConfig, const char *pGuid)
{
  if (!loadConfig(pathConfig)) {
    return false;
  }

  if (!parseGuid(pGuid, m_guid)) {
    m_log->warn("No valid interface GUID given - using all zero GUID");
  }

  m_client.init(m_host, m_port, m_token, m_verifyTls);

  // Open the driver database
  if (!m_dbPath.empty()) {
    std::string dberr;
    if (m_db.open(m_dbPath, dberr)) {
      m_log->info("Database {} opened", m_dbPath);
    }
    else {
      m_log->error("Failed to open database {}: {} - continuing without "
                   "persistent storage",
                   m_dbPath,
                   dberr);
    }
  }

  // Fetch the complete home state (devices, rooms, scenes, users, hub info)
  // and persist it. Fall back to the plain device list if /home fails.
  json devices = json::array();
  std::string err;
  json home;
  if (m_client.getHome(home, err)) {
    m_log->info("Fetched complete home state from hub {}:{}", m_host, m_port);
    if (m_db.isOpen()) {
      if (m_db.saveHome(home)) {
        m_log->info("Home state saved to database");
      }
      else {
        m_log->warn("Failed to save home state to database");
      }
    }
    if (home.contains("devices") && home["devices"].is_array()) {
      devices = home["devices"];
    }
  }
  else {
    m_log->warn("Failed to fetch home state ({}) - falling back to the "
                "device list",
                err);
    if (!m_client.getDevices(devices, err)) {
      m_log->error("Failed to connect to Dirigera hub {}:{} - {}",
                   m_host,
                   m_port,
                   err);
      return false;
    }
  }

  m_log->info("Connected to Dirigera hub {}:{} - {} device(s) found",
              m_host,
              m_port,
              devices.size());

  // Build the device GUID base from the hub MAC address on the standard
  // VSCP Ethernet form FF:FF:FF:FF:FF:FF:FF:FE:<mac>:nn:nn. The hub has
  // just been talked to over TCP so its neighbour table entry is fresh.
  {
    std::string hubIp;
    uint8_t mac[6];
    if (resolveIpv4(m_host, hubIp) && lookupArpMac(hubIp, mac)) {
      memset(m_hubGuid, 0xFF, 8);
      m_hubGuid[7] = 0xFE;
      memcpy(m_hubGuid + 8, mac, 6);
      m_hubGuid[14] = 0;
      m_hubGuid[15] = 0;
      m_hasHubGuid = true;
      m_log->info("Hub MAC {:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X} - "
                  "device GUID base {}",
                  mac[0],
                  mac[1],
                  mac[2],
                  mac[3],
                  mac[4],
                  mac[5],
                  guidToString(m_hubGuid));
    }
    else {
      m_log->warn("Unable to resolve the hub MAC address - using the "
                  "interface GUID as device GUID base");
    }
  }

  {
    std::lock_guard<std::mutex> lock(m_deviceMutex);
    for (const auto &jdev : devices) {
      registerDevice(jdev);
    }
  }

  m_run = true;
  if (m_useWebsocket) {
    m_log->info("Using the hub websocket event stream for status updates");
    m_worker = std::thread(&CIkeaDirigera::websocketLoop, this);
  }
  else {
    m_log->info("Polling the hub for status updates every {} second(s)",
                m_pollInterval);
    m_worker = std::thread(&CIkeaDirigera::workerLoop, this);
  }

  return true;
}

void
CIkeaDirigera::close()
{
  m_run = false;

  // Wake the websocket service loop so it notices the stop request
  {
    std::lock_guard<std::mutex> lock(m_wsCtxMutex);
    if (nullptr != m_wsContext) {
      lws_cancel_service(m_wsContext);
    }
  }

  if (m_worker.joinable()) {
    m_worker.join();
  }

  {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    for (vscpEvent *pEvent : m_queue) {
      deleteEvent(pEvent);
    }
    m_queue.clear();
  }

  if (m_log) {
    m_log->info("Driver closed");
    m_log->flush();
  }
}

// ----------------------------------------------------------------------------
// Device registry / polling
// ----------------------------------------------------------------------------

// m_deviceMutex must be held
void
CIkeaDirigera::registerDevice(const json &jdev)
{
  if (!jdev.is_object() || !jdev.contains("id")) {
    return;
  }

  const std::string id = jdev["id"].get<std::string>();
  if (m_devices.count(id)) {
    return;
  }

  DirigeraDevice dev;
  dev.id = id;
  dev.deviceType = jdev.value("deviceType", jdev.value("type", "unknown"));
  dev.reachable = jdev.value("isReachable", true);
  dev.sensorIndex = sensorIndexFromId(id);

  if (jdev.contains("attributes") && jdev["attributes"].is_object()) {
    dev.attributes = jdev["attributes"];
    dev.name = dev.attributes.value("customName", "");
  }

  // Find a configured mapping - by name first (always case insensitive,
  // UTF-8), then by unique id
  const DeviceMapping *map = nullptr;
  for (const auto &m : m_mappings) {
    if (!m.name.empty() && nameEqual(m.name, dev.name)) {
      map = &m;
      break;
    }
  }
  if (nullptr == map) {
    for (const auto &m : m_mappings) {
      if (!m.id.empty() && (m.id == dev.id)) {
        map = &m;
        break;
      }
    }
  }

  // Node index (nickname): a mapping may pin it; otherwise the next free
  // index in discovery order is used, skipping indices pinned by mappings
  int idx = -1;
  if ((nullptr != map) && (map->index >= 0)) {
    if (m_indexUsed[map->index]) {
      m_log->warn("Mapped index {} for device '{}' ({}) already in use - "
                  "assigning the next free index",
                  map->index,
                  dev.name,
                  dev.id);
    }
    else {
      idx = map->index;
    }
  }
  if (idx < 0) {
    while ((m_nextIndex < 256) &&
           (m_indexUsed[m_nextIndex] || m_indexReserved[m_nextIndex])) {
      m_nextIndex++;
    }
    if (m_nextIndex > 255) {
      m_log->error("No free device index for '{}' ({}) - device ignored",
                   dev.name,
                   dev.id);
      return;
    }
    idx = m_nextIndex++;
  }
  m_indexUsed[idx] = true;
  dev.index = static_cast<uint8_t>(idx);

  // Defaults for unmapped devices: the Ethernet GUID built from the hub
  // MAC address (FF:FF:FF:FF:FF:FF:FF:FE:<mac>:ii:ss) with the device
  // index in byte 14 and the sensor index in byte 15 - the interface GUID
  // is used as base if the hub MAC is unknown. Zone is the global zone and
  // subzone the index.
  memcpy(dev.guid, m_hasHubGuid ? m_hubGuid : m_guid, 16);
  dev.guid[14] = dev.index;
  dev.guid[15] = dev.sensorIndex;
  dev.zone = m_zone;
  dev.subzone = dev.index;

  if (nullptr != map) {
    if (map->hasGuid) {
      memcpy(dev.guid, map->guid, 16);
    }
    if (map->zone >= 0) {
      dev.zone = static_cast<uint8_t>(map->zone);
    }
    if (map->subzone >= 0) {
      dev.subzone = static_cast<uint8_t>(map->subzone);
    }
    if (!map->type.empty()) {
      dev.configuredType = map->type;
      if (map->type != dev.deviceType) {
        m_log->warn("Device '{}' ({}) has type '{}' but is mapped as '{}'",
                    dev.name,
                    dev.id,
                    dev.deviceType,
                    map->type);
      }
    }
  }

  m_log->info("Device '{}' ({}, type={}) registered - index={} zone={} "
              "subzone={} sensor-index={} guid={}",
              dev.name,
              dev.id,
              dev.deviceType,
              dev.index,
              dev.zone,
              dev.subzone,
              dev.sensorIndex,
              guidToString(dev.guid));

  m_devices[id] = dev;
  DirigeraDevice &node = m_devices[id];

  // Simulated node: restore the decision matrix and the writable node
  // registers (user id, page select), and persist the VSCP mapping and
  // the internally generated MDF
  if (m_db.isOpen()) {
    m_db.loadDm(node.id, node.dm, sizeof(node.dm));
    m_db.loadNodeState(node.id, node.pageSelect, node.userId);
    m_db.saveDeviceMapping(node.id,
                           node.name,
                           node.deviceType,
                           node.index,
                           node.zone,
                           node.subzone,
                           node.sensorIndex,
                           guidToString(node.guid));
    m_db.saveMdf(node.deviceType, generateMdf(node));
  }

  // Announce the initial state
  compareAndEmit(node, node.attributes, true);
}

// Send a node heartbeat for every simulated device that is active
// (registered) and reachable through the hub
void
CIkeaDirigera::sendHeartbeats()
{
  std::lock_guard<std::mutex> lock(m_deviceMutex);

  for (auto &item : m_devices) {
    DirigeraDevice &dev = item.second;
    if (!dev.reachable) {
      continue;
    }
    queueEvent(VSCP_CLASS1_INFORMATION,
               VSCP_TYPE_INFORMATION_NODE_HEARTBEAT,
               dev,
               { 0, dev.zone, dev.subzone });
  }
}

void
CIkeaDirigera::workerLoop()
{
  using namespace std::chrono;

  auto nextPoll = steady_clock::now();
  auto nextHeartbeat = steady_clock::now();

  while (m_run) {
    if (steady_clock::now() >= nextPoll) {
      pollDevices();
      nextPoll = steady_clock::now() + seconds(m_pollInterval);
    }
    if ((m_heartbeatInterval > 0) && (steady_clock::now() >= nextHeartbeat)) {
      sendHeartbeats();
      nextHeartbeat = steady_clock::now() + seconds(m_heartbeatInterval);
    }
    std::this_thread::sleep_for(milliseconds(100));
  }
}

void
CIkeaDirigera::pollDevices()
{
  json devices;
  std::string err;

  if (!m_client.getDevices(devices, err)) {
    m_log->warn("Failed to fetch devices from hub: {}", err);
    return;
  }

  std::lock_guard<std::mutex> lock(m_deviceMutex);

  for (const auto &jdev : devices) {
    if (!jdev.is_object() || !jdev.contains("id")) {
      continue;
    }

    const std::string id = jdev["id"].get<std::string>();
    auto it = m_devices.find(id);
    if (it == m_devices.end()) {
      registerDevice(jdev);
      continue;
    }

    DirigeraDevice &dev = it->second;

    bool reachable = jdev.value("isReachable", true);
    if (reachable != dev.reachable) {
      dev.reachable = reachable;
      m_log->info("Device '{}' is now {}",
                  dev.name,
                  reachable ? "reachable" : "unreachable");
    }

    if (jdev.contains("attributes") && jdev["attributes"].is_object()) {
      compareAndEmit(dev, jdev["attributes"], false);
    }
  }
}

// ----------------------------------------------------------------------------
// Websocket event stream (wss://<hub>:8443/v1)
// ----------------------------------------------------------------------------

static int
wsCallback(struct lws *wsi,
           enum lws_callback_reasons reason,
           void *user,
           void *in,
           size_t len)
{
  (void)user;
  auto *self = static_cast<CIkeaDirigera *>(
    lws_context_user(lws_get_context(wsi)));

  switch (reason) {

    case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER: {
      auto **p = reinterpret_cast<unsigned char **>(in);
      unsigned char *end = (*p) + len;
      const std::string auth = self->wsAuthHeader();
      if (lws_add_http_header_by_name(
            wsi,
            reinterpret_cast<const unsigned char *>("authorization:"),
            reinterpret_cast<const unsigned char *>(auth.c_str()),
            static_cast<int>(auth.size()),
            p,
            end)) {
        return -1;
      }
      break;
    }

    case LWS_CALLBACK_CLIENT_ESTABLISHED:
      self->onWsConnected();
      break;

    case LWS_CALLBACK_CLIENT_RECEIVE:
      self->onWsData(static_cast<const char *>(in),
                     len,
                     lws_is_final_fragment(wsi));
      break;

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
      self->onWsDisconnected(
        in ? std::string(static_cast<const char *>(in), len)
           : "connection error");
      break;

    case LWS_CALLBACK_CLIENT_CLOSED:
      self->onWsDisconnected("connection closed");
      break;

    default:
      break;
  }

  return 0;
}

void
CIkeaDirigera::onWsConnected()
{
  m_wsPendingConnect = false;
  m_wsConnected = true;
  m_wsRxBuf.clear();
  // Resync the registry - state may have changed while disconnected
  m_wsNeedResync = true;
  m_log->info("Websocket event stream connected");
}

void
CIkeaDirigera::onWsData(const char *data, size_t len, bool final)
{
  m_wsRxBuf.append(data, len);
  if (final) {
    handleWsMessage(m_wsRxBuf);
    m_wsRxBuf.clear();
  }
}

void
CIkeaDirigera::onWsDisconnected(const std::string &reason)
{
  m_wsPendingConnect = false;
  if (m_wsConnected.exchange(false)) {
    m_log->warn("Websocket event stream disconnected: {}", reason);
  }
  else if (m_run) {
    m_log->warn("Websocket connect failed: {}", reason);
  }
}

void
CIkeaDirigera::handleWsMessage(const std::string &msg)
{
  json j;
  try {
    j = json::parse(msg);
  }
  catch (const std::exception &e) {
    m_log->warn("Failed to parse websocket message: {}", e.what());
    return;
  }

  const std::string type = j.value("type", "");
  if (!j.contains("data") || !j["data"].is_object()) {
    return;
  }
  const json &data = j["data"];
  const std::string id = data.value("id", "");

  std::lock_guard<std::mutex> lock(m_deviceMutex);

  if ("deviceStateChanged" == type) {
    auto it = m_devices.find(id);
    if (it == m_devices.end()) {
      registerDevice(data);
      return;
    }

    DirigeraDevice &dev = it->second;

    if (data.contains("isReachable") && data["isReachable"].is_boolean()) {
      bool reachable = data["isReachable"].get<bool>();
      if (reachable != dev.reachable) {
        dev.reachable = reachable;
        m_log->info("Device '{}' is now {}",
                    dev.name,
                    reachable ? "reachable" : "unreachable");
      }
    }

    if (data.contains("attributes") && data["attributes"].is_object()) {
      compareAndEmit(dev, data["attributes"], false);
    }
  }
  else if ("deviceAdded" == type) {
    registerDevice(data);
  }
  else if ("deviceRemoved" == type) {
    auto it = m_devices.find(id);
    if (it != m_devices.end()) {
      m_log->info("Device '{}' ({}) removed from hub",
                  it->second.name,
                  it->second.id);
      m_devices.erase(it);
    }
  }
  else {
    m_log->debug("Unhandled websocket message type '{}'", type);
  }
}

// Periodic 1 s tick that keeps the websocket service loop turning so
// heartbeats and shutdown are handled promptly even on an idle connection
namespace {
struct WsTick {
  lws_sorted_usec_list_t sul;
  struct lws_context *ctx;
};

void
wsTickCallback(lws_sorted_usec_list_t *sul)
{
  WsTick *tick = lws_container_of(sul, WsTick, sul);
  lws_sul_schedule(tick->ctx, 0, &tick->sul, wsTickCallback, LWS_US_PER_SEC);
}
} // namespace

void
CIkeaDirigera::websocketLoop()
{
  using namespace std::chrono;

  lws_set_log_level(LLL_ERR | LLL_WARN, nullptr);

  static const struct lws_protocols protocols[] = {
    { "dirigera", wsCallback, 0, 65536, 0, nullptr, 0 },
    { nullptr, nullptr, 0, 0, 0, nullptr, 0 } // terminator
  };

  struct lws_context_creation_info info;
  memset(&info, 0, sizeof(info));
  info.port = CONTEXT_PORT_NO_LISTEN;
  info.protocols = protocols;
  info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
  info.user = this;

  struct lws_context *ctx = lws_create_context(&info);
  if (nullptr == ctx) {
    m_log->error("Failed to create websocket context - falling back to "
                 "polling");
    workerLoop();
    return;
  }

  {
    std::lock_guard<std::mutex> lock(m_wsCtxMutex);
    m_wsContext = ctx;
  }

  WsTick tick {};
  tick.ctx = ctx;
  lws_sul_schedule(ctx, 0, &tick.sul, wsTickCallback, LWS_US_PER_SEC);

  auto nextConnect = steady_clock::now();
  auto nextHeartbeat = steady_clock::now();
  constexpr auto RECONNECT_DELAY = seconds(5);

  while (m_run) {

    if (!m_wsConnected && !m_wsPendingConnect &&
        (steady_clock::now() >= nextConnect)) {

      struct lws_client_connect_info ci;
      memset(&ci, 0, sizeof(ci));
      ci.context = ctx;
      ci.address = m_host.c_str();
      ci.port = m_port;
      ci.path = "/v1";
      ci.host = ci.address;
      ci.origin = ci.address;
      ci.local_protocol_name = "dirigera";
      ci.ssl_connection = LCCSCF_USE_SSL;
      if (!m_verifyTls) {
        // The Dirigera hub uses a self signed certificate
        ci.ssl_connection |= LCCSCF_ALLOW_SELFSIGNED |
                             LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
      }

      m_wsPendingConnect = true;
      nextConnect = steady_clock::now() + RECONNECT_DELAY;
      if (nullptr == lws_client_connect_via_info(&ci)) {
        m_wsPendingConnect = false;
        m_log->warn("Failed to initiate websocket connection to {}:{}",
                    m_host,
                    m_port);
      }
    }

    lws_service(ctx, 100);

    // Resync the device registry over REST after each (re)connect to pick
    // up changes that happened while the event stream was down
    if (m_wsNeedResync.exchange(false)) {
      pollDevices();
    }

    if ((m_heartbeatInterval > 0) && (steady_clock::now() >= nextHeartbeat)) {
      sendHeartbeats();
      nextHeartbeat = steady_clock::now() + seconds(m_heartbeatInterval);
    }
  }

  {
    std::lock_guard<std::mutex> lock(m_wsCtxMutex);
    m_wsContext = nullptr;
  }
  lws_context_destroy(ctx);
}

// m_deviceMutex must be held. If emitAll is true all known attributes are
// reported, otherwise only changed ones.
void
CIkeaDirigera::compareAndEmit(DirigeraDevice &dev,
                              const json &newAttr,
                              bool emitAll)
{
  auto changed = [&](const char *key) {
    if (!newAttr.contains(key)) {
      return false;
    }
    return emitAll || !dev.attributes.contains(key) ||
           (dev.attributes[key] != newAttr[key]);
  };

  // On/off state (lights, outlets)
  if (changed("isOn") && newAttr["isOn"].is_boolean()) {
    emitBoolean(dev,
                newAttr["isOn"].get<bool>(),
                VSCP_TYPE_INFORMATION_ON,
                VSCP_TYPE_INFORMATION_OFF);
  }

  // Motion sensors
  if (changed("isDetected") && newAttr["isDetected"].is_boolean()) {
    emitBoolean(dev,
                newAttr["isDetected"].get<bool>(),
                VSCP_TYPE_INFORMATION_DETECT,
                VSCP_TYPE_INFORMATION_OFF);
  }

  // Open/close sensors
  if (changed("isOpen") && newAttr["isOpen"].is_boolean()) {
    emitBoolean(dev,
                newAttr["isOpen"].get<bool>(),
                VSCP_TYPE_INFORMATION_ON,
                VSCP_TYPE_INFORMATION_OFF);
  }

  // Environment sensors
  if (changed("currentTemperature") &&
      newAttr["currentTemperature"].is_number()) {
    // Unit 1 = degrees Celsius
    emitMeasurement(dev,
                    VSCP_TYPE_MEASUREMENT_TEMPERATURE,
                    1,
                    newAttr["currentTemperature"].get<double>());
  }

  if (changed("currentRH") && newAttr["currentRH"].is_number()) {
    // Unit 0 = percent relative humidity
    emitMeasurement(dev,
                    VSCP_TYPE_MEASUREMENT_HUMIDITY,
                    0,
                    newAttr["currentRH"].get<double>());
  }

  if (changed("currentPM25") && newAttr["currentPM25"].is_number()) {
    // Hub reports ug/m3, VSCP unit 0 for mass concentration is kg/m3
    emitMeasurement(dev,
                    VSCP_TYPE_MEASUREMENT_CHEMICAL_CONCENTRATION_MASS,
                    0,
                    newAttr["currentPM25"].get<double>() * 1e-9);
  }

  // Light sensors
  if (changed("illuminance") && newAttr["illuminance"].is_number()) {
    // Unit 0 = lux
    emitMeasurement(dev,
                    VSCP_TYPE_MEASUREMENT_ILLUMINANCE,
                    0,
                    newAttr["illuminance"].get<double>());
  }

  // Dim level for lights (1-100)
  if (changed("lightLevel") && newAttr["lightLevel"].is_number()) {
    // Unit 0 = relative level, reported as 0.0 - 1.0
    emitMeasurement(dev,
                    VSCP_TYPE_MEASUREMENT_RELATIVE_LEVEL,
                    0,
                    newAttr["lightLevel"].get<double>() / 100.0);
  }

  // Merge so that partial attribute updates (websocket events) do not
  // discard attributes that were not part of the update
  dev.attributes.update(newAttr);
}

// ----------------------------------------------------------------------------
// Event generation
// ----------------------------------------------------------------------------

void
CIkeaDirigera::emitBoolean(const DirigeraDevice &dev,
                           bool value,
                           uint16_t typeTrue,
                           uint16_t typeFalse)
{
  std::vector<uint8_t> data = { dev.index, dev.zone, dev.subzone };
  queueEvent(VSCP_CLASS1_INFORMATION,
             value ? typeTrue : typeFalse,
             dev,
             data);
}

void
CIkeaDirigera::emitMeasurement(const DirigeraDevice &dev,
                               uint16_t vscpType,
                               uint8_t unit,
                               double value)
{
  // Data coding: single precision float (0b101), unit in bits 4-3,
  // sensor index from the device id suffix in bits 2-0
  std::vector<uint8_t> data;
  data.push_back(static_cast<uint8_t>(0xA0 | ((unit & 0x03) << 3) |
                                      (dev.sensorIndex & 0x07)));
  appendFloat(data, value);

  queueEvent(VSCP_CLASS1_MEASUREMENT, vscpType, dev, data);
}

void
CIkeaDirigera::queueEvent(uint16_t vscpClass,
                          uint16_t vscpType,
                          const DirigeraDevice &dev,
                          const std::vector<uint8_t> &data)
{
  auto *pEvent = new vscpEvent;
  memset(pEvent, 0, sizeof(vscpEvent));

  pEvent->head = VSCP_PRIORITY_NORMAL;
  pEvent->vscp_class = vscpClass;
  pEvent->vscp_type = vscpType;
  memcpy(pEvent->GUID, dev.guid, 16);

  pEvent->sizeData = static_cast<uint16_t>(data.size());
  if (!data.empty()) {
    pEvent->pdata = new uint8_t[data.size()];
    memcpy(pEvent->pdata, data.data(), data.size());
  }

  {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    if (m_queue.size() >= MAX_QUEUE_SIZE) {
      m_log->warn("Receive queue full - dropping oldest event");
      deleteEvent(m_queue.front());
      m_queue.pop_front();
    }
    m_queue.push_back(pEvent);
  }

  m_queueCond.notify_one();
}

// ----------------------------------------------------------------------------
// Daemon interface
// ----------------------------------------------------------------------------

int
CIkeaDirigera::readEvent(vscpEvent *pEvent, unsigned long timeoutMs)
{
  if (nullptr == pEvent) {
    return CANAL_ERROR_PARAMETER;
  }

  std::unique_lock<std::mutex> lock(m_queueMutex);
  if (!m_queueCond.wait_for(lock,
                            std::chrono::milliseconds(timeoutMs),
                            [this]() { return !m_queue.empty(); })) {
    return CANAL_ERROR_TIMEOUT;
  }

  vscpEvent *pQueued = m_queue.front();
  m_queue.pop_front();
  lock.unlock();

  // Hand over the event (the caller takes ownership of pdata)
  *pEvent = *pQueued;
  delete pQueued;

  return CANAL_ERROR_SUCCESS;
}

int
CIkeaDirigera::writeEvent(const vscpEvent *pEvent)
{
  if (nullptr == pEvent) {
    return CANAL_ERROR_PARAMETER;
  }

  uint16_t vscpClass = pEvent->vscp_class;
  const uint8_t *pdata = pEvent->pdata;
  uint16_t size = pEvent->sizeData;
  const uint8_t *destGuid = nullptr;

  // Class2 level I events carry the destination GUID in the first
  // 16 data bytes
  if ((vscpClass >= 512) && (vscpClass < 1024)) {
    if (size < 16) {
      return CANAL_ERROR_PARAMETER;
    }
    destGuid = pdata;
    vscpClass -= 512;
    pdata += 16;
    size -= 16;
  }

  // Register access events for the simulated nodes
  if (VSCP_CLASS1_PROTOCOL == vscpClass) {
    return handleProtocolEvent(pEvent->vscp_type, pdata, size);
  }

  // Give the per-device decision matrices a chance to act on the event
  checkDecisionMatrix(vscpClass, pEvent->vscp_type, pdata, size);

  if (VSCP_CLASS1_CONTROL == vscpClass) {
    return handleControl(pEvent->vscp_type, pdata, size, destGuid);
  }

  // Not an event this driver acts on - silently accept
  return CANAL_ERROR_SUCCESS;
}

std::vector<DirigeraDevice *>
CIkeaDirigera::matchDevices(uint8_t zone, uint8_t subzone)
{
  std::vector<DirigeraDevice *> result;

  for (auto &item : m_devices) {
    DirigeraDevice &dev = item.second;
    if ((zone != 0xff) && (zone != dev.zone)) {
      continue;
    }
    if ((subzone != 0xff) && (subzone != dev.subzone)) {
      continue;
    }
    result.push_back(&dev);
  }

  return result;
}

int
CIkeaDirigera::handleControl(uint16_t vscpType,
                             const uint8_t *pdata,
                             uint16_t size,
                             const uint8_t *destGuid)
{
  // CLASS1.CONTROL events: data[0] = optional value, data[1] = zone,
  // data[2] = subzone
  if ((nullptr == pdata) || (size < 3)) {
    return CANAL_ERROR_PARAMETER;
  }

  const uint8_t value = pdata[0];
  const uint8_t zone = pdata[1];
  const uint8_t subzone = pdata[2];

  json attributes;

  switch (vscpType) {

    case VSCP_TYPE_CONTROL_TURNON:
      attributes["isOn"] = true;
      break;

    case VSCP_TYPE_CONTROL_TURNOFF:
      attributes["isOn"] = false;
      break;

    case VSCP_TYPE_CONTROL_DIM_LAMPS: {
      // 0 = off, 1-100 = dim level in percent
      if (value > 100) {
        return CANAL_ERROR_PARAMETER;
      }
      if (0 == value) {
        attributes["isOn"] = false;
      }
      else {
        attributes["lightLevel"] = static_cast<int>(value);
      }
      break;
    }

    default:
      // Control type not supported by this driver - silently accept
      return CANAL_ERROR_SUCCESS;
  }

  std::lock_guard<std::mutex> lock(m_deviceMutex);

  std::vector<DirigeraDevice *> targets;

  // A non-zero destination GUID (Class2 Level I events) addresses a
  // mapped device directly; otherwise match on zone/subzone
  bool guidAddressed = false;
  if (nullptr != destGuid) {
    for (int i = 0; i < 16; i++) {
      if (destGuid[i]) {
        guidAddressed = true;
        break;
      }
    }
  }

  if (guidAddressed) {
    for (auto &item : m_devices) {
      if (0 == memcmp(item.second.guid, destGuid, 16)) {
        targets.push_back(&item.second);
        break;
      }
    }
    if (targets.empty()) {
      m_log->debug("Control event (type={}) matched no device guid", vscpType);
      return CANAL_ERROR_SUCCESS;
    }
  }
  else {
    targets = matchDevices(zone, subzone);
    if (targets.empty()) {
      m_log->debug("Control event (type={}) matched no device "
                   "(zone={} subzone={})",
                   vscpType,
                   zone,
                   subzone);
      return CANAL_ERROR_SUCCESS;
    }
  }

  int rv = CANAL_ERROR_SUCCESS;
  for (DirigeraDevice *dev : targets) {
    // Only act on devices that can be switched/dimmed
    if (!dev->attributes.contains("isOn")) {
      continue;
    }
    if (attributes.contains("lightLevel") &&
        !dev->attributes.contains("lightLevel")) {
      continue;
    }

    std::string err;
    if (!m_client.setAttributes(dev->id, attributes, err)) {
      m_log->error("Failed to control device '{}' ({}): {}",
                   dev->name,
                   dev->id,
                   err);
      rv = CANAL_ERROR_GENERIC;
    }
    else {
      m_log->debug("Device '{}' set {}", dev->name, attributes.dump());
    }
  }

  return rv;
}
