// main.cpp
//
// Test application for the vscpl2drv-ikea-dirigera VSCP level II driver.
//
// Loads the driver shared library the same way the VSCP daemon does (through
// the exported VSCPOpen/VSCPClose/VSCPRead/VSCPWrite interface), prints every
// event the driver generates and lets the user send CLASS1.CONTROL events to
// switch and dim devices.
//
// Usage:
//   testapp <driver.so> <config.json> [guid]
//
// This file is part of the VSCP (https://www.vscp.org)
//
// The MIT License (MIT)
//
// Copyright (C) 2000-2026 Ake Hedman and contributors,
// the VSCP project <info@vscp.org>

#include <canal.h>
#include <level2drvdef.h>
#include <vscp.h>
#include <vscp-class.h>
#include <vscp-type.h>

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

// ----------------------------------------------------------------------------
// Dynamic library helpers
// ----------------------------------------------------------------------------

#if defined(_WIN32)
typedef HMODULE lib_handle_t;
static lib_handle_t
libOpen(const char *path)
{
  return LoadLibraryA(path);
}
static void *
libSym(lib_handle_t h, const char *name)
{
  return reinterpret_cast<void *>(GetProcAddress(h, name));
}
static void
libClose(lib_handle_t h)
{
  FreeLibrary(h);
}
static std::string
libError()
{
  return "error " + std::to_string(GetLastError());
}
#else
typedef void *lib_handle_t;
static lib_handle_t
libOpen(const char *path)
{
  return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}
static void *
libSym(lib_handle_t h, const char *name)
{
  return dlsym(h, name);
}
static void
libClose(lib_handle_t h)
{
  dlclose(h);
}
static std::string
libError()
{
  const char *err = dlerror();
  return err ? err : "unknown error";
}
#endif

// ----------------------------------------------------------------------------
// Event pretty printing
// ----------------------------------------------------------------------------

static const char *
classToString(uint16_t vscpClass)
{
  switch (vscpClass) {
    case VSCP_CLASS1_MEASUREMENT:
      return "CLASS1.MEASUREMENT";
    case VSCP_CLASS1_INFORMATION:
      return "CLASS1.INFORMATION";
    case VSCP_CLASS1_CONTROL:
      return "CLASS1.CONTROL";
    default:
      return "";
  }
}

static const char *
typeToString(uint16_t vscpClass, uint16_t vscpType)
{
  if (VSCP_CLASS1_INFORMATION == vscpClass) {
    switch (vscpType) {
      case VSCP_TYPE_INFORMATION_ON:
        return "ON";
      case VSCP_TYPE_INFORMATION_OFF:
        return "OFF";
      case VSCP_TYPE_INFORMATION_DETECT:
        return "DETECT";
      default:
        return "";
    }
  }

  if (VSCP_CLASS1_MEASUREMENT == vscpClass) {
    switch (vscpType) {
      case VSCP_TYPE_MEASUREMENT_TEMPERATURE:
        return "Temperature";
      case VSCP_TYPE_MEASUREMENT_HUMIDITY:
        return "Humidity";
      case VSCP_TYPE_MEASUREMENT_ILLUMINANCE:
        return "Illuminance";
      case VSCP_TYPE_MEASUREMENT_CHEMICAL_CONCENTRATION_MASS:
        return "Mass concentration";
      case VSCP_TYPE_MEASUREMENT_RELATIVE_LEVEL:
        return "Relative level";
      default:
        return "";
    }
  }

  return "";
}

static void
printEvent(const vscpEvent &ev)
{
  printf("RX class=%u", ev.vscp_class);
  const char *cls = classToString(ev.vscp_class);
  if (*cls) {
    printf(" (%s)", cls);
  }

  printf(" type=%u", ev.vscp_type);
  const char *type = typeToString(ev.vscp_class, ev.vscp_type);
  if (*type) {
    printf(" (%s)", type);
  }

  printf(" index=%u", ev.GUID[15]);

  printf(" guid=");
  for (int i = 0; i < 16; i++) {
    printf("%s%02X", i ? ":" : "", ev.GUID[i]);
  }

  printf(" data=[");
  for (uint16_t i = 0; i < ev.sizeData; i++) {
    printf("%s0x%02X", i ? "," : "", ev.pdata[i]);
  }
  printf("]");

  // Decode single precision float measurement data coding
  if ((VSCP_CLASS1_MEASUREMENT == ev.vscp_class) && (5 == ev.sizeData) &&
      (0xA0 == (ev.pdata[0] & 0xE0))) {
    uint32_t bits = (static_cast<uint32_t>(ev.pdata[1]) << 24) |
                    (static_cast<uint32_t>(ev.pdata[2]) << 16) |
                    (static_cast<uint32_t>(ev.pdata[3]) << 8) |
                    static_cast<uint32_t>(ev.pdata[4]);
    float value;
    memcpy(&value, &bits, sizeof(value));
    printf(" value=%g unit=%u", value, (ev.pdata[0] >> 3) & 0x03);
  }

  // CLASS1.INFORMATION on/off/detect carry index, zone, subzone
  if ((VSCP_CLASS1_INFORMATION == ev.vscp_class) && (3 == ev.sizeData)) {
    printf(" zone=%u subzone=%u", ev.pdata[1], ev.pdata[2]);
  }

  printf("\n");
  fflush(stdout);
}

// ----------------------------------------------------------------------------
// Hub REST access (used by the 'devices' command)
// ----------------------------------------------------------------------------

struct HubConfig {
  std::string host;
  int port = 8443;
  std::string token;
  bool verifyTls = false;
  bool valid = false;
};

static HubConfig
loadHubConfig(const char *path)
{
  HubConfig cfg;

  try {
    std::ifstream in(path);
    if (!in.is_open()) {
      return cfg;
    }
    nlohmann::json j;
    in >> j;
    cfg.host = j.value("hub", "");
    cfg.port = j.value("port", 8443);
    cfg.token = j.value("token", "");
    cfg.verifyTls = j.value("verify-tls", false);
    cfg.valid = !cfg.host.empty() && !cfg.token.empty();
  }
  catch (...) {
    // leave cfg.valid false
  }

  return cfg;
}

static size_t
curlWriteCallback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
  static_cast<std::string *>(userdata)->append(ptr, size * nmemb);
  return size * nmemb;
}

static void
printHubDevices(const HubConfig &cfg)
{
  if (!cfg.valid) {
    printf("No hub address/token found in the configuration file\n");
    return;
  }

  CURL *curl = curl_easy_init();
  if (nullptr == curl) {
    printf("Failed to init curl\n");
    return;
  }

  std::string url = "https://" + cfg.host + ":" + std::to_string(cfg.port) +
                    "/v1/devices";
  std::string response;

  struct curl_slist *headers = nullptr;
  headers =
    curl_slist_append(headers, ("Authorization: Bearer " + cfg.token).c_str());

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  if (!cfg.verifyTls) {
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  }

  CURLcode rv = curl_easy_perform(curl);
  long httpCode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (CURLE_OK != rv) {
    printf("Failed to fetch devices: %s\n", curl_easy_strerror(rv));
    return;
  }
  if ((httpCode < 200) || (httpCode >= 300)) {
    printf("Failed to fetch devices: HTTP status %ld\n", httpCode);
    return;
  }

  nlohmann::json devices;
  try {
    devices = nlohmann::json::parse(response);
  }
  catch (const std::exception &e) {
    printf("Failed to parse device list: %s\n", e.what());
    return;
  }
  if (!devices.is_array()) {
    printf("Unexpected device list format\n");
    return;
  }

  printf("%zu device(s) on hub %s:%d\n\n",
         devices.size(),
         cfg.host.c_str(),
         cfg.port);
  printf("%-4s %-38s %-20s %-20s %-5s %s\n",
         "idx",
         "id",
         "type",
         "name",
         "reach",
         "attributes");

  int idx = 0;
  for (const auto &dev : devices) {
    if (!dev.is_object()) {
      continue;
    }
    std::string name;
    std::string attributes;
    if (dev.contains("attributes") && dev["attributes"].is_object()) {
      name = dev["attributes"].value("customName", "");
      nlohmann::json attr = dev["attributes"];
      attr.erase("customName");
      attributes = attr.dump();
    }
    printf("%-4d %-38s %-20s %-20s %-5s %s\n",
           idx++,
           dev.value("id", "").c_str(),
           dev.value("deviceType", dev.value("type", "")).c_str(),
           name.c_str(),
           dev.value("isReachable", false) ? "yes" : "no",
           attributes.c_str());
  }

  printf("\nNote: idx matches the index/default subzone assigned by the "
         "driver when the\ndevice set is unchanged since the driver was "
         "opened.\n");
}

// ----------------------------------------------------------------------------
// Token pairing (OAuth2 PKCE flow against the hub)
// ----------------------------------------------------------------------------

static std::string
base64url(const unsigned char *data, size_t len)
{
  static const char tbl[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

  std::string out;
  for (size_t i = 0; i < len; i += 3) {
    uint32_t n = static_cast<uint32_t>(data[i]) << 16;
    if (i + 1 < len) {
      n |= static_cast<uint32_t>(data[i + 1]) << 8;
    }
    if (i + 2 < len) {
      n |= static_cast<uint32_t>(data[i + 2]);
    }
    out += tbl[(n >> 18) & 0x3f];
    out += tbl[(n >> 12) & 0x3f];
    if (i + 1 < len) {
      out += tbl[(n >> 6) & 0x3f];
    }
    if (i + 2 < len) {
      out += tbl[n & 0x3f];
    }
  }
  return out; // no padding (base64url)
}

static std::string
randomCodeVerifier()
{
  static const char charset[] = "abcdefghijklmnopqrstuvwxyz"
                                "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                "1234567890_.-~";
  std::random_device rd;
  std::uniform_int_distribution<size_t> dist(0, sizeof(charset) - 2);

  std::string verifier;
  for (int i = 0; i < 128; i++) {
    verifier += charset[dist(rd)];
  }
  return verifier;
}

// Simple HTTPS request towards the hub (TLS verification off - the hub uses
// a self signed certificate). Returns true and the response body on success.
static bool
hubRequest(const std::string &method,
           const std::string &url,
           const std::string &body,
           const char *contentType,
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
  if (nullptr != contentType) {
    headers = curl_slist_append(
      headers,
      (std::string("Content-Type: ") + contentType).c_str());
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  if (nullptr != headers) {
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  }
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

  if (method != "GET") {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  }

  CURLcode rv = curl_easy_perform(curl);
  long httpCode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
  if (nullptr != headers) {
    curl_slist_free_all(headers);
  }
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

// Run the one-time pairing flow and print the access token.
// Returns a process exit code.
static int
getTokenFlow(const std::string &host, int port)
{
  const std::string base =
    "https://" + host + ":" + std::to_string(port) + "/v1";

  // PKCE code verifier and S256 challenge
  const std::string verifier = randomCodeVerifier();
  unsigned char hash[EVP_MAX_MD_SIZE];
  unsigned int hashLen = 0;
  EVP_Digest(verifier.data(),
             verifier.size(),
             hash,
             &hashLen,
             EVP_sha256(),
             nullptr);
  const std::string challenge = base64url(hash, hashLen);

  // Step 1: request an authorization code
  std::string response;
  std::string err;
  if (!hubRequest("GET",
                  base + "/oauth/authorize?audience=homesmart.local"
                         "&response_type=code"
                         "&code_challenge=" + challenge +
                         "&code_challenge_method=S256",
                  "",
                  nullptr,
                  response,
                  err)) {
    fprintf(stderr, "Authorization request failed: %s\n", err.c_str());
    return 1;
  }

  std::string code;
  try {
    code = nlohmann::json::parse(response).value("code", "");
  }
  catch (...) {
  }
  if (code.empty()) {
    fprintf(stderr,
            "Hub did not return an authorization code: %s\n",
            response.c_str());
    return 1;
  }

  // Step 2: the user must prove physical access to the hub
  printf("Press the action button on the bottom of the Dirigera hub,\n"
         "then press ENTER here (within 60 seconds)...");
  fflush(stdout);
  std::string dummy;
  std::getline(std::cin, dummy);

  // Step 3: exchange the code for an access token
  char hostname[256] = "vscp-testapp";
#if !defined(_WIN32)
  gethostname(hostname, sizeof(hostname) - 1);
#endif

  const std::string body = "code=" + code +
                           "&name=" + hostname +
                           "&grant_type=authorization_code"
                           "&code_verifier=" + verifier;
  if (!hubRequest("POST",
                  base + "/oauth/token",
                  body,
                  "application/x-www-form-urlencoded",
                  response,
                  err)) {
    fprintf(stderr,
            "Token request failed (did you press the action button?): %s\n",
            err.c_str());
    return 1;
  }

  std::string token;
  try {
    token = nlohmann::json::parse(response).value("access_token", "");
  }
  catch (...) {
  }
  if (token.empty()) {
    fprintf(stderr,
            "Hub did not return an access token: %s\n",
            response.c_str());
    return 1;
  }

  printf("\nAccess token:\n\n%s\n\n"
         "Put it in the driver configuration file as:\n\n"
         "  \"token\": \"%s\"\n",
         token.c_str(),
         token.c_str());

  return 0;
}

// ----------------------------------------------------------------------------
// main
// ----------------------------------------------------------------------------

static void
usage(const char *prog)
{
  fprintf(stderr,
          "Usage: %s <driver.so> <config.json> [guid]\n"
          "       %s --get-token <hub-ip> [port]\n"
          "\n"
          "--get-token runs the one-time pairing flow against the hub and\n"
          "prints the access token to put in the configuration file.\n"
          "\n"
          "Otherwise the vscpl2drv-ikea-dirigera driver is loaded; every\n"
          "event it generates is printed and the following commands are\n"
          "accepted on stdin:\n"
          "\n"
          "  on <subzone> [zone]         Turn device(s) on  (255 = all)\n"
          "  off <subzone> [zone]        Turn device(s) off (255 = all)\n"
          "  dim <subzone> <0-100> [zone] Set dim level in percent\n"
          "  devices                     List the devices on the hub\n"
          "  help                        Show this help\n"
          "  quit                        Close the driver and exit\n",
          prog,
          prog);
}

int
main(int argc, char *argv[])
{
  if ((argc >= 3) && (0 == strcmp(argv[1], "--get-token"))) {
    return getTokenFlow(argv[2], (argc > 3) ? atoi(argv[3]) : 8443);
  }

  if (argc < 3) {
    usage(argv[0]);
    return 1;
  }

  const char *libPath = argv[1];
  const char *configPath = argv[2];
  const char *guid = (argc > 3)
                       ? argv[3]
                       : "FF:FF:FF:FF:FF:FF:FF:F5:00:00:00:00:00:00:00:00";

  lib_handle_t lib = libOpen(libPath);
  if (!lib) {
    fprintf(stderr, "Failed to load %s: %s\n", libPath, libError().c_str());
    return 1;
  }

  auto fnOpen =
    reinterpret_cast<LPFNDLL_VSCPOPEN>(libSym(lib, "VSCPOpen"));
  auto fnClose =
    reinterpret_cast<LPFNDLL_VSCPCLOSE>(libSym(lib, "VSCPClose"));
  auto fnRead =
    reinterpret_cast<LPFNDLL_VSCPREAD>(libSym(lib, "VSCPRead"));
  auto fnWrite =
    reinterpret_cast<LPFNDLL_VSCPWRITE>(libSym(lib, "VSCPWrite"));
  auto fnVersion =
    reinterpret_cast<LPFNDLL_VSCPGETVERSION>(libSym(lib, "VSCPGetVersion"));
  auto fnVendor = reinterpret_cast<LPFNDLL_VSCPGETVENDORSTRING>(
    libSym(lib, "VSCPGetVendorString"));

  if (!fnOpen || !fnClose || !fnRead || !fnWrite || !fnVersion || !fnVendor) {
    fprintf(stderr, "Driver does not export the level II interface\n");
    libClose(lib);
    return 1;
  }

  unsigned long version = fnVersion();
  printf("Driver: %s\n", fnVendor());
  printf("Version: %lu.%lu.%lu\n",
         (version >> 24) & 0xff,
         (version >> 16) & 0xff,
         (version >> 8) & 0xff);

  printf("Opening driver with config %s\n", configPath);
  long handle =
    fnOpen(configPath, reinterpret_cast<const unsigned char *>(guid));
  if (0 == handle) {
    fprintf(stderr, "VSCPOpen failed - check the configuration file\n");
    libClose(lib);
    return 1;
  }
  printf("Driver open (handle=%ld). Type 'help' for commands.\n\n", handle);

  // Reader thread: print everything the driver sends
  std::atomic<bool> run { true };
  std::thread reader([&]() {
    while (run) {
      vscpEvent ev;
      memset(&ev, 0, sizeof(ev));
      if (CANAL_ERROR_SUCCESS == fnRead(handle, &ev, 500)) {
        printEvent(ev);
        delete[] ev.pdata;
      }
    }
  });

  // Command loop
  std::string line;
  while (std::getline(std::cin, line)) {

    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;

    if (cmd.empty()) {
      continue;
    }

    if (("quit" == cmd) || ("exit" == cmd) || ("q" == cmd)) {
      break;
    }

    if ("help" == cmd) {
      usage(argv[0]);
      continue;
    }

    if ("devices" == cmd) {
      printHubDevices(loadHubConfig(configPath));
      continue;
    }

    uint16_t vscpType = 0;
    int value = 0;
    int subzone = -1;
    int zone = 255;

    if ("on" == cmd) {
      vscpType = VSCP_TYPE_CONTROL_TURNON;
      iss >> subzone >> zone;
    }
    else if ("off" == cmd) {
      vscpType = VSCP_TYPE_CONTROL_TURNOFF;
      iss >> subzone >> zone;
    }
    else if ("dim" == cmd) {
      vscpType = VSCP_TYPE_CONTROL_DIM_LAMPS;
      iss >> subzone >> value >> zone;
      if ((value < 0) || (value > 100)) {
        printf("Dim level must be 0-100\n");
        continue;
      }
    }
    else {
      printf("Unknown command '%s' - type 'help'\n", cmd.c_str());
      continue;
    }

    if ((subzone < 0) || (subzone > 255)) {
      printf("Give a subzone 0-255 (255 = all devices)\n");
      continue;
    }

    uint8_t data[3] = { static_cast<uint8_t>(value),
                        static_cast<uint8_t>(zone),
                        static_cast<uint8_t>(subzone) };

    vscpEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.vscp_class = VSCP_CLASS1_CONTROL;
    ev.vscp_type = vscpType;
    ev.sizeData = sizeof(data);
    ev.pdata = data;

    int rv = fnWrite(handle, &ev, 500);
    printf("TX class=%u (CLASS1.CONTROL) type=%u zone=%d subzone=%d -> %s\n",
           ev.vscp_class,
           ev.vscp_type,
           zone,
           subzone,
           (CANAL_ERROR_SUCCESS == rv) ? "OK"
                                       : ("error " + std::to_string(rv)).c_str());
  }

  printf("Closing driver...\n");
  run = false;
  reader.join();
  fnClose(handle);
  libClose(lib);

  return 0;
}
