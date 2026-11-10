// db.cpp
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

#include "db.h"

#include <sqlite3.h>

#include <cstring>
#include <ctime>

using json = nlohmann::json;

DirigeraDb::~DirigeraDb()
{
  close();
}

void
DirigeraDb::close()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (nullptr != m_db) {
    sqlite3_close(m_db);
    m_db = nullptr;
  }
}

bool
DirigeraDb::exec(const char *sql)
{
  char *errmsg = nullptr;
  if (SQLITE_OK != sqlite3_exec(m_db, sql, nullptr, nullptr, &errmsg)) {
    if (errmsg) {
      sqlite3_free(errmsg);
    }
    return false;
  }
  return true;
}

bool
DirigeraDb::open(const std::string &path, std::string &err)
{
  std::lock_guard<std::mutex> lock(m_mutex);

  if (nullptr != m_db) {
    sqlite3_close(m_db);
    m_db = nullptr;
  }

  if (SQLITE_OK != sqlite3_open(path.c_str(), &m_db)) {
    err = (nullptr != m_db) ? sqlite3_errmsg(m_db) : "out of memory";
    if (nullptr != m_db) {
      sqlite3_close(m_db);
      m_db = nullptr;
    }
    return false;
  }

  static const char *schema =
    "CREATE TABLE IF NOT EXISTS home ("
    "  id INTEGER PRIMARY KEY CHECK (id = 1),"
    "  fetched_at TEXT,"
    "  json TEXT);"
    "CREATE TABLE IF NOT EXISTS hub ("
    "  id TEXT PRIMARY KEY,"
    "  name TEXT,"
    "  firmware TEXT,"
    "  json TEXT);"
    "CREATE TABLE IF NOT EXISTS devices ("
    "  id TEXT PRIMARY KEY,"
    "  name TEXT,"
    "  device_type TEXT,"
    "  room TEXT,"
    "  reachable INTEGER,"
    "  json TEXT,"
    "  vscp_index INTEGER,"
    "  vscp_zone INTEGER,"
    "  vscp_subzone INTEGER,"
    "  vscp_sensor_index INTEGER,"
    "  vscp_guid TEXT);"
    "CREATE TABLE IF NOT EXISTS rooms ("
    "  id TEXT PRIMARY KEY,"
    "  name TEXT,"
    "  json TEXT);"
    "CREATE TABLE IF NOT EXISTS scenes ("
    "  id TEXT PRIMARY KEY,"
    "  name TEXT,"
    "  json TEXT);"
    "CREATE TABLE IF NOT EXISTS users ("
    "  id TEXT PRIMARY KEY,"
    "  name TEXT,"
    "  json TEXT);"
    "CREATE TABLE IF NOT EXISTS device_sets ("
    "  id TEXT PRIMARY KEY,"
    "  name TEXT,"
    "  json TEXT);"
    "CREATE TABLE IF NOT EXISTS mdf ("
    "  device_type TEXT PRIMARY KEY,"
    "  xml TEXT);"
    "CREATE TABLE IF NOT EXISTS dm ("
    "  device_id TEXT PRIMARY KEY,"
    "  matrix BLOB);";

  if (!exec(schema)) {
    err = sqlite3_errmsg(m_db);
    sqlite3_close(m_db);
    m_db = nullptr;
    return false;
  }

  return true;
}

// Insert id/name/json rows for one home collection
static void
insertCollection(sqlite3 *db,
                 const json &home,
                 const char *key,
                 const char *table,
                 const char *idKey,
                 const char *nameKey)
{
  if (!home.contains(key) || !home[key].is_array()) {
    return;
  }

  std::string sql = std::string("INSERT OR REPLACE INTO ") + table +
                    " (id, name, json) VALUES (?1, ?2, ?3)";
  sqlite3_stmt *stmt = nullptr;
  if (SQLITE_OK != sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr)) {
    return;
  }

  for (const auto &item : home[key]) {
    if (!item.is_object()) {
      continue;
    }

    std::string id = item.value(idKey, "");
    if (id.empty()) {
      continue;
    }

    std::string name = item.value(nameKey, "");
    // Scenes keep their name in info.name
    if (name.empty() && item.contains("info") && item["info"].is_object()) {
      name = item["info"].value("name", "");
    }

    const std::string jsonStr = item.dump();
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, jsonStr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_reset(stmt);
  }

  sqlite3_finalize(stmt);
}

bool
DirigeraDb::saveHome(const json &home)
{
  std::lock_guard<std::mutex> lock(m_mutex);

  if (nullptr == m_db) {
    return false;
  }

  exec("BEGIN TRANSACTION;");
  exec("DELETE FROM home; DELETE FROM hub; DELETE FROM devices;"
       "DELETE FROM rooms; DELETE FROM scenes; DELETE FROM users;"
       "DELETE FROM device_sets;");

  // Raw complete document
  {
    char timestamp[32];
    time_t now = time(nullptr);
    struct tm tmUtc;
#if defined(_WIN32)
    gmtime_s(&tmUtc, &now);
#else
    gmtime_r(&now, &tmUtc);
#endif
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &tmUtc);

    sqlite3_stmt *stmt = nullptr;
    if (SQLITE_OK ==
        sqlite3_prepare_v2(m_db,
                           "INSERT INTO home (id, fetched_at, json) "
                           "VALUES (1, ?1, ?2)",
                           -1,
                           &stmt,
                           nullptr)) {
      const std::string jsonStr = home.dump();
      sqlite3_bind_text(stmt, 1, timestamp, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 2, jsonStr.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
    }
  }

  // Hub
  if (home.contains("hub") && home["hub"].is_object()) {
    const json &hub = home["hub"];
    sqlite3_stmt *stmt = nullptr;
    if (SQLITE_OK ==
        sqlite3_prepare_v2(m_db,
                           "INSERT OR REPLACE INTO hub "
                           "(id, name, firmware, json) "
                           "VALUES (?1, ?2, ?3, ?4)",
                           -1,
                           &stmt,
                           nullptr)) {
      std::string name;
      std::string firmware;
      if (hub.contains("attributes") && hub["attributes"].is_object()) {
        name = hub["attributes"].value("customName", "");
        firmware = hub["attributes"].value("firmwareVersion", "");
      }
      const std::string id = hub.value("id", "");
      const std::string jsonStr = hub.dump();
      sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 3, firmware.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 4, jsonStr.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
    }
  }

  // Devices
  if (home.contains("devices") && home["devices"].is_array()) {
    sqlite3_stmt *stmt = nullptr;
    if (SQLITE_OK ==
        sqlite3_prepare_v2(m_db,
                           "INSERT OR REPLACE INTO devices "
                           "(id, name, device_type, room, reachable, json) "
                           "VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
                           -1,
                           &stmt,
                           nullptr)) {
      for (const auto &dev : home["devices"]) {
        if (!dev.is_object()) {
          continue;
        }
        const std::string id = dev.value("id", "");
        if (id.empty()) {
          continue;
        }

        std::string name;
        if (dev.contains("attributes") && dev["attributes"].is_object()) {
          name = dev["attributes"].value("customName", "");
        }
        std::string room;
        if (dev.contains("room") && dev["room"].is_object()) {
          room = dev["room"].value("name", "");
        }
        const std::string deviceType = dev.value("deviceType", "");
        const std::string jsonStr = dev.dump();

        sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, deviceType.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, room.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 5, dev.value("isReachable", true) ? 1 : 0);
        sqlite3_bind_text(stmt, 6, jsonStr.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
      }
      sqlite3_finalize(stmt);
    }
  }

  insertCollection(m_db, home, "rooms", "rooms", "id", "name");
  insertCollection(m_db, home, "scenes", "scenes", "id", "name");
  insertCollection(m_db, home, "users", "users", "uid", "name");
  insertCollection(m_db, home, "deviceSets", "device_sets", "id", "name");

  exec("COMMIT;");

  return true;
}

bool
DirigeraDb::saveDeviceMapping(const std::string &id,
                              const std::string &name,
                              const std::string &deviceType,
                              int index,
                              int zone,
                              int subzone,
                              int sensorIndex,
                              const std::string &guid)
{
  std::lock_guard<std::mutex> lock(m_mutex);

  if (nullptr == m_db) {
    return false;
  }

  sqlite3_stmt *stmt = nullptr;
  if (SQLITE_OK !=
      sqlite3_prepare_v2(m_db,
                         "INSERT INTO devices (id, name, device_type) "
                         "VALUES (?1, ?2, ?3) "
                         "ON CONFLICT(id) DO NOTHING",
                         -1,
                         &stmt,
                         nullptr)) {
    return false;
  }
  sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, deviceType.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (SQLITE_OK !=
      sqlite3_prepare_v2(m_db,
                         "UPDATE devices SET vscp_index = ?2, "
                         "vscp_zone = ?3, vscp_subzone = ?4, "
                         "vscp_sensor_index = ?5, vscp_guid = ?6 "
                         "WHERE id = ?1",
                         -1,
                         &stmt,
                         nullptr)) {
    return false;
  }
  sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 2, index);
  sqlite3_bind_int(stmt, 3, zone);
  sqlite3_bind_int(stmt, 4, subzone);
  sqlite3_bind_int(stmt, 5, sensorIndex);
  sqlite3_bind_text(stmt, 6, guid.c_str(), -1, SQLITE_TRANSIENT);
  bool ok = (SQLITE_DONE == sqlite3_step(stmt));
  sqlite3_finalize(stmt);

  return ok;
}

bool
DirigeraDb::saveMdf(const std::string &deviceType, const std::string &xml)
{
  std::lock_guard<std::mutex> lock(m_mutex);

  if (nullptr == m_db) {
    return false;
  }

  sqlite3_stmt *stmt = nullptr;
  if (SQLITE_OK !=
      sqlite3_prepare_v2(m_db,
                         "INSERT OR REPLACE INTO mdf (device_type, xml) "
                         "VALUES (?1, ?2)",
                         -1,
                         &stmt,
                         nullptr)) {
    return false;
  }
  sqlite3_bind_text(stmt, 1, deviceType.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, xml.c_str(), -1, SQLITE_TRANSIENT);
  bool ok = (SQLITE_DONE == sqlite3_step(stmt));
  sqlite3_finalize(stmt);

  return ok;
}

bool
DirigeraDb::saveDm(const std::string &deviceId, const uint8_t *dm, size_t len)
{
  std::lock_guard<std::mutex> lock(m_mutex);

  if ((nullptr == m_db) || (nullptr == dm)) {
    return false;
  }

  sqlite3_stmt *stmt = nullptr;
  if (SQLITE_OK !=
      sqlite3_prepare_v2(m_db,
                         "INSERT OR REPLACE INTO dm (device_id, matrix) "
                         "VALUES (?1, ?2)",
                         -1,
                         &stmt,
                         nullptr)) {
    return false;
  }
  sqlite3_bind_text(stmt, 1, deviceId.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 2, dm, static_cast<int>(len), SQLITE_TRANSIENT);
  bool ok = (SQLITE_DONE == sqlite3_step(stmt));
  sqlite3_finalize(stmt);

  return ok;
}

bool
DirigeraDb::loadDm(const std::string &deviceId, uint8_t *dm, size_t len)
{
  std::lock_guard<std::mutex> lock(m_mutex);

  if ((nullptr == m_db) || (nullptr == dm)) {
    return false;
  }

  sqlite3_stmt *stmt = nullptr;
  if (SQLITE_OK !=
      sqlite3_prepare_v2(m_db,
                         "SELECT matrix FROM dm WHERE device_id = ?1",
                         -1,
                         &stmt,
                         nullptr)) {
    return false;
  }
  sqlite3_bind_text(stmt, 1, deviceId.c_str(), -1, SQLITE_TRANSIENT);

  bool ok = false;
  if (SQLITE_ROW == sqlite3_step(stmt)) {
    const void *blob = sqlite3_column_blob(stmt, 0);
    const int blobLen = sqlite3_column_bytes(stmt, 0);
    if ((nullptr != blob) && (blobLen == static_cast<int>(len))) {
      memcpy(dm, blob, len);
      ok = true;
    }
  }
  sqlite3_finalize(stmt);

  return ok;
}
