// db.h
//
// SQLite storage of the Dirigera home state for the
// vscpl2drv-ikea-dirigera VSCP level II driver.
//
// This file is part of the VSCP (https://www.vscp.org)
//
// The MIT License (MIT)
//
// Copyright (C) 2000-2026 Ake Hedman and contributors,
// the VSCP project <info@vscp.org>

#ifndef VSCPL2DRV_IKEA_DIRIGERA_DB_H
#define VSCPL2DRV_IKEA_DIRIGERA_DB_H

#include <nlohmann/json.hpp>

#include <cstdint>
#include <mutex>
#include <string>

struct sqlite3;

class DirigeraDb {
public:
  DirigeraDb() = default;
  ~DirigeraDb();

  // Open (or create) the database and make sure the schema exists
  bool open(const std::string &path, std::string &err);
  bool isOpen() const { return nullptr != m_db; }
  void close();

  // Save the complete home state fetched from the hub - the raw JSON
  // document plus structured hub/devices/rooms/scenes/users/deviceSets
  // tables. Existing home data is replaced.
  bool saveHome(const nlohmann::json &home);

  // Update the VSCP addressing columns for a registered device
  bool saveDeviceMapping(const std::string &id,
                         const std::string &name,
                         const std::string &deviceType,
                         int index,
                         int zone,
                         int subzone,
                         int sensorIndex,
                         const std::string &guid);

  // Internally generated MDF per device type
  bool saveMdf(const std::string &deviceType, const std::string &xml);

  // Decision matrix persistence (one blob per device)
  bool saveDm(const std::string &deviceId, const uint8_t *dm, size_t len);
  bool loadDm(const std::string &deviceId, uint8_t *dm, size_t len);

private:
  bool exec(const char *sql);

  sqlite3 *m_db = nullptr;
  std::mutex m_mutex;
};

#endif // VSCPL2DRV_IKEA_DIRIGERA_DB_H
