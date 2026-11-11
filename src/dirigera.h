// dirigera.h
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
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to
// deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
// sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.

#ifndef VSCPL2DRV_IKEA_DIRIGERA_H
#define VSCPL2DRV_IKEA_DIRIGERA_H

#include "db.h"

#include <vscp.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct lws;
struct lws_context;

// ----------------------------------------------------------------------------
// REST client for the IKEA Dirigera hub HTTPS API (https://<hub>:8443/v1).
// The hub uses a self signed certificate so TLS peer verification is
// disabled by default (can be enabled from the configuration file).
// ----------------------------------------------------------------------------

class DirigeraClient {
public:
  DirigeraClient() = default;

  void init(const std::string &host,
            uint16_t port,
            const std::string &token,
            bool verifyTls);

  // GET /v1/devices
  bool getDevices(nlohmann::json &devices, std::string &err);

  // GET /v1/home - the complete home state
  bool getHome(nlohmann::json &home, std::string &err);

  // PATCH /v1/devices/<id> with [{ "attributes": { ... } }]
  bool setAttributes(const std::string &deviceId,
                     const nlohmann::json &attributes,
                     std::string &err);

private:
  bool perform(const std::string &method,
               const std::string &path,
               const std::string &body,
               std::string &response,
               std::string &err);

  std::string m_baseUrl;   // https://host:port/v1
  std::string m_token;     // Bearer access token
  bool m_verifyTls = false;
};

// ----------------------------------------------------------------------------
// A configured device mapping (from the 'devices' list in the config file).
// Matched against hub devices by name first, then by unique id.
// ----------------------------------------------------------------------------

struct DeviceMapping {
  std::string name;         // match on customName
  std::string id;           // or match on the unique device id
  std::string type;         // expected device type (validated against hub)
  bool hasGuid = false;
  uint8_t guid[16] = { 0 }; // GUID to use for events from/to the device
  int index = -1;           // node index/nickname (-1 = discovery order)
  int zone = -1;            // -1 = use the global zone
  int subzone = -1;         // -1 = use the device index
};

// ----------------------------------------------------------------------------
// A device known to the hub. Each device is simulated as a VSCP node with
// a register space (status/functionality), a decision matrix and an
// internally generated MDF.
// ----------------------------------------------------------------------------

// Decision matrix geometry: rows of 8 bytes at page 1, offset 0
// Row layout: oaddr, flags, class-mask, class-filter, type-mask,
//             type-filter, action, action-parameter
constexpr int DM_ROW_COUNT = 4;
constexpr int DM_ROW_SIZE = 8;
constexpr uint16_t DM_PAGE = 1;

// Decision matrix flag bits
constexpr uint8_t DM_FLAG_ENABLED = 0x80;
constexpr uint8_t DM_FLAG_CHECK_ZONE = 0x10;
constexpr uint8_t DM_FLAG_CHECK_SUBZONE = 0x08;

// Decision matrix actions
constexpr uint8_t DM_ACTION_NOOP = 0;
constexpr uint8_t DM_ACTION_TURNON = 1;
constexpr uint8_t DM_ACTION_TURNOFF = 2;
constexpr uint8_t DM_ACTION_DIM = 3; // action parameter = level 0-100

struct DirigeraDevice {
  std::string id;           // Dirigera device id
  std::string name;         // customName from the hub
  std::string deviceType;   // light/outlet/motionSensor/environmentSensor/...
  std::string configuredType; // type from the config mapping (if any)
  uint8_t index = 0;        // event index (discovery order) = node nickname
  uint8_t guid[16] = { 0 }; // GUID used in events for this device
  uint8_t zone = 0;         // VSCP zone for this device
  uint8_t subzone = 0;      // VSCP subzone for this device
  uint8_t sensorIndex = 0;  // sensor index from the id suffix (-n/_n)
  bool reachable = true;
  nlohmann::json attributes; // last seen attribute set

  // --- simulated VSCP node state -------------------------------------------
  uint16_t pageSelect = 0;                      // standard page select regs
  uint8_t userId[5] = { 0 };                    // standard user id regs
  uint8_t dm[DM_ROW_COUNT * DM_ROW_SIZE] = { 0 }; // decision matrix
};

// ----------------------------------------------------------------------------
// The driver
// ----------------------------------------------------------------------------

class CIkeaDirigera {
public:
  CIkeaDirigera();
  ~CIkeaDirigera();

  // Read configuration, connect to the hub and start the worker thread.
  // pGuid is the interface GUID on string form ("FF:FF:...") or nullptr.
  bool open(const std::string &pathConfig, const char *pGuid);
  void close();

  // Event from the VSCP daemon that the driver should act on.
  // Returns a CANAL_ERROR_... code.
  int writeEvent(const vscpEvent *pEvent);

  // Blocking fetch of the next event from the driver (timeout in
  // milliseconds). Returns a CANAL_ERROR_... code. On success the caller
  // owns pEvent->pdata (allocated with new[]).
  int readEvent(vscpEvent *pEvent, unsigned long timeoutMs);

  // --- libwebsockets callback hooks (used by the C callback) ---------------
  void onWsConnected();
  void onWsData(const char *data, size_t len, bool final);
  void onWsDisconnected(const std::string &reason);
  std::string wsAuthHeader() const { return "Bearer " + m_token; }

private:
  bool loadConfig(const std::string &path);
  void setupLogging(const nlohmann::json &cfg);

  void workerLoop();
  void websocketLoop();
  void handleWsMessage(const std::string &msg);
  void pollDevices();
  void sendHeartbeats();
  void registerDevice(const nlohmann::json &jdev);
  void compareAndEmit(DirigeraDevice &dev,
                      const nlohmann::json &newAttr,
                      bool emitAll);

  void emitBoolean(const DirigeraDevice &dev,
                   bool value,
                   uint16_t typeTrue,
                   uint16_t typeFalse);
  void emitMeasurement(const DirigeraDevice &dev,
                       uint16_t vscpType,
                       uint8_t unit,
                       double value);
  void queueEvent(uint16_t vscpClass,
                  uint16_t vscpType,
                  const DirigeraDevice &dev,
                  const std::vector<uint8_t> &data);

  int handleControl(uint16_t vscpType,
                    const uint8_t *pdata,
                    uint16_t size,
                    const uint8_t *destGuid);
  std::vector<DirigeraDevice *> matchDevices(uint8_t zone, uint8_t subzone);

  // --- simulated VSCP node (registers / DM / MDF) ---------------------------
  // Implemented in registers.cpp. m_deviceMutex must be held.
  uint8_t readRegister(DirigeraDevice &dev, uint16_t page, uint8_t offset);
  uint8_t writeRegister(DirigeraDevice &dev,
                        uint16_t page,
                        uint8_t offset,
                        uint8_t value);
  int handleProtocolEvent(uint16_t vscpType,
                          const uint8_t *pdata,
                          uint16_t size);
  void checkDecisionMatrix(uint16_t vscpClass,
                           uint16_t vscpType,
                           const uint8_t *pdata,
                           uint16_t size);
  void executeDmAction(DirigeraDevice &dev, uint8_t action, uint8_t param);
  std::string generateMdf(const DirigeraDevice &dev);

  // --- configuration -------------------------------------------------------
  std::string m_host;
  uint16_t m_port = 8443;
  std::string m_token;
  bool m_verifyTls = false;
  bool m_useWebsocket = false;   // subscribe to the hub event stream instead
                                 // of polling
  unsigned m_pollInterval = 5;   // seconds (poll mode)
  unsigned m_heartbeatInterval = 30; // seconds, 0 = no heartbeats
  uint8_t m_zone = 0;
  std::string m_dbPath;          // sqlite database path ("" = disabled)
  std::vector<DeviceMapping> m_mappings; // configured device mappings

  // --- state ---------------------------------------------------------------
  uint8_t m_guid[16] = { 0 };     // interface GUID (from the daemon)
  uint8_t m_hubGuid[16] = { 0 };  // Ethernet GUID built from the hub MAC
  bool m_hasHubGuid = false;      // m_hubGuid is valid
  DirigeraClient m_client;
  DirigeraDb m_db;
  std::shared_ptr<spdlog::logger> m_log;

  std::mutex m_deviceMutex;
  std::map<std::string, DirigeraDevice> m_devices; // keyed on device id
  int m_nextIndex = 0;                   // next auto-assigned device index
  bool m_indexUsed[256] = { false };     // taken by registered devices
  bool m_indexReserved[256] = { false }; // pinned by config mappings

  // Outgoing (driver -> daemon) event queue
  std::mutex m_queueMutex;
  std::condition_variable m_queueCond;
  std::deque<vscpEvent *> m_queue;

  std::thread m_worker;
  std::atomic<bool> m_run { false };

  // --- websocket state -----------------------------------------------------
  std::string m_wsRxBuf;
  std::atomic<bool> m_wsConnected { false };
  std::atomic<bool> m_wsPendingConnect { false };
  std::atomic<bool> m_wsNeedResync { false };
  std::mutex m_wsCtxMutex;
  struct lws_context *m_wsContext = nullptr; // guarded by m_wsCtxMutex
};

#endif // VSCPL2DRV_IKEA_DIRIGERA_H
