// registers.cpp
//
// Simulated VSCP node (register space, decision matrix and internal MDF)
// for each IKEA Dirigera device handled by the driver.
//
// Each device is simulated as a VSCP level I node:
//
//   Page 0, 0x00-0x7F   User registers exposing device status/functionality
//     0x00  isOn (0/1)                        rw (lights/outlets)
//     0x01  lightLevel 0-100                  rw (lights)
//     0x02  isDetected (0/1)                  r  (motion sensors)
//     0x03  isOpen (0/1)                      r  (open/close sensors)
//     0x04  temperature * 100, int16 MSB      r  (environment sensors)
//     0x05  temperature * 100, int16 LSB      r
//     0x06  relative humidity %               r
//     0x07  PM2.5 ug/m3, uint16 MSB           r
//     0x08  PM2.5 ug/m3, uint16 LSB           r
//     0x09  illuminance lux, uint16 MSB       r  (light sensors)
//     0x0A  illuminance lux, uint16 LSB       r
//     0x0B  battery %                         r
//     0x10  zone                              r
//     0x11  subzone                           r
//     0x12  sensor index                      r
//     0x13  reachable (0/1)                   r
//
//   Page 1, 0x00-0x1F   Decision matrix (4 rows x 8 bytes)
//     Row: oaddr, flags, class-mask, class-filter, type-mask, type-filter,
//          action, action-parameter
//     Flags: 0x80 enabled, 0x10 check zone, 0x08 check subzone
//     Actions: 1 = turn on, 2 = turn off, 3 = dim (parameter = 0-100)
//
//   0x80-0xFF           VSCP standard registers (all pages)
//
// This file is part of the VSCP (https://www.vscp.org)
//
// The MIT License (MIT)
//
// Copyright (C) 2000-2026 Ake Hedman and contributors,
// the VSCP project <info@vscp.org>

#include "dirigera.h"
#include "version.h"

#include <canal.h>
#include <vscp-class.h>
#include <vscp-type.h>

#include <cstring>

using json = nlohmann::json;

// User register offsets (page 0)
namespace reg {
constexpr uint8_t IS_ON = 0x00;
constexpr uint8_t LIGHT_LEVEL = 0x01;
constexpr uint8_t IS_DETECTED = 0x02;
constexpr uint8_t IS_OPEN = 0x03;
constexpr uint8_t TEMPERATURE_MSB = 0x04;
constexpr uint8_t TEMPERATURE_LSB = 0x05;
constexpr uint8_t HUMIDITY = 0x06;
constexpr uint8_t PM25_MSB = 0x07;
constexpr uint8_t PM25_LSB = 0x08;
constexpr uint8_t ILLUMINANCE_MSB = 0x09;
constexpr uint8_t ILLUMINANCE_LSB = 0x0A;
constexpr uint8_t BATTERY = 0x0B;
constexpr uint8_t ZONE = 0x10;
constexpr uint8_t SUBZONE = 0x11;
constexpr uint8_t SENSOR_INDEX = 0x12;
constexpr uint8_t REACHABLE = 0x13;
} // namespace reg

// ----------------------------------------------------------------------------
// Attribute helpers
// ----------------------------------------------------------------------------

static bool
attrBool(const json &attributes, const char *key)
{
  return attributes.contains(key) && attributes[key].is_boolean() &&
         attributes[key].get<bool>();
}

static double
attrNum(const json &attributes, const char *key)
{
  if (attributes.contains(key) && attributes[key].is_number()) {
    return attributes[key].get<double>();
  }
  return 0;
}

// ----------------------------------------------------------------------------
// Register read
// ----------------------------------------------------------------------------

uint8_t
CIkeaDirigera::readRegister(DirigeraDevice &dev, uint16_t page, uint8_t offset)
{
  // VSCP standard registers - same on every page
  if (offset >= 0x80) {

    switch (offset) {
      case 0x80: // Alarm status
        return 0;
      case 0x81: // VSCP specification major version
        return 1;
      case 0x82: // VSCP specification minor version
        return 0;
      case 0x83: // Error counter
        return 0;
      case 0x84:
      case 0x85:
      case 0x86:
      case 0x87:
      case 0x88: // User id
        return dev.userId[offset - 0x84];
      case 0x91: // Nickname
        return dev.index;
      case 0x92: // Page select MSB
        return static_cast<uint8_t>(dev.pageSelect >> 8);
      case 0x93: // Page select LSB
        return static_cast<uint8_t>(dev.pageSelect & 0xff);
      case 0x94: // Firmware major version (two digit year of year.month.patch)
        return VSCPL2DRV_IKEA_DIRIGERA_MAJOR_VERSION % 100;
      case 0x95: // Firmware minor version (month)
        return VSCPL2DRV_IKEA_DIRIGERA_MINOR_VERSION;
      case 0x96: // Firmware build version (patch)
        return VSCPL2DRV_IKEA_DIRIGERA_PATCH_VERSION % 256;
      case 0x97: // Boot loader algorithm (none)
        return 0xff;
      case 0x98: // Buffer size
        return 8;
      case 0x99: // Number of register pages used
        return 2;
      default:
        break;
    }

    // GUID 0xD0 - 0xDF (MSB first)
    if ((offset >= 0xD0) && (offset <= 0xDF)) {
      return dev.guid[offset - 0xD0];
    }

    // MDF URL 0xE0 - 0xFF. The MDF is internal (stored in the driver
    // database) so no external URL is exposed.
    if (offset >= 0xE0) {
      return 0;
    }

    return 0;
  }

  // Decision matrix page
  if (DM_PAGE == page) {
    if (offset < DM_ROW_COUNT * DM_ROW_SIZE) {
      return dev.dm[offset];
    }
    return 0;
  }

  if (0 != page) {
    return 0;
  }

  // Page 0 - device status
  const json &attr = dev.attributes;

  switch (offset) {
    case reg::IS_ON:
      return attrBool(attr, "isOn") ? 1 : 0;
    case reg::LIGHT_LEVEL:
      return static_cast<uint8_t>(attrNum(attr, "lightLevel"));
    case reg::IS_DETECTED:
      return attrBool(attr, "isDetected") ? 1 : 0;
    case reg::IS_OPEN:
      return attrBool(attr, "isOpen") ? 1 : 0;
    case reg::TEMPERATURE_MSB:
    case reg::TEMPERATURE_LSB: {
      int16_t t = static_cast<int16_t>(attrNum(attr, "currentTemperature") * 100.0);
      return (reg::TEMPERATURE_MSB == offset)
               ? static_cast<uint8_t>((t >> 8) & 0xff)
               : static_cast<uint8_t>(t & 0xff);
    }
    case reg::HUMIDITY:
      return static_cast<uint8_t>(attrNum(attr, "currentRH"));
    case reg::PM25_MSB:
    case reg::PM25_LSB: {
      uint16_t pm = static_cast<uint16_t>(attrNum(attr, "currentPM25"));
      return (reg::PM25_MSB == offset) ? static_cast<uint8_t>(pm >> 8)
                                       : static_cast<uint8_t>(pm & 0xff);
    }
    case reg::ILLUMINANCE_MSB:
    case reg::ILLUMINANCE_LSB: {
      uint16_t lux = static_cast<uint16_t>(attrNum(attr, "illuminance"));
      return (reg::ILLUMINANCE_MSB == offset) ? static_cast<uint8_t>(lux >> 8)
                                              : static_cast<uint8_t>(lux & 0xff);
    }
    case reg::BATTERY:
      return static_cast<uint8_t>(attrNum(attr, "batteryPercentage"));
    case reg::ZONE:
      return dev.zone;
    case reg::SUBZONE:
      return dev.subzone;
    case reg::SENSOR_INDEX:
      return dev.sensorIndex;
    case reg::REACHABLE:
      return dev.reachable ? 1 : 0;
    default:
      return 0;
  }
}

// ----------------------------------------------------------------------------
// Register write. Returns the value a subsequent read returns.
// ----------------------------------------------------------------------------

uint8_t
CIkeaDirigera::writeRegister(DirigeraDevice &dev,
                             uint16_t page,
                             uint8_t offset,
                             uint8_t value)
{
  // Standard registers
  if (offset >= 0x80) {
    if ((offset >= 0x84) && (offset <= 0x88)) {
      dev.userId[offset - 0x84] = value;
      return value;
    }
    if (0x92 == offset) {
      dev.pageSelect =
        static_cast<uint16_t>((value << 8) | (dev.pageSelect & 0xff));
      return value;
    }
    if (0x93 == offset) {
      dev.pageSelect = static_cast<uint16_t>((dev.pageSelect & 0xff00) | value);
      return value;
    }
    // Everything else is read only
    return readRegister(dev, page, offset);
  }

  // Decision matrix page
  if (DM_PAGE == page) {
    if (offset < DM_ROW_COUNT * DM_ROW_SIZE) {
      dev.dm[offset] = value;
      if (m_db.isOpen()) {
        m_db.saveDm(dev.id, dev.dm, sizeof(dev.dm));
      }
      return value;
    }
    return 0;
  }

  if (0 != page) {
    return 0;
  }

  // Page 0 - writable control registers act on the hub device
  std::string err;

  if (reg::IS_ON == offset) {
    const bool on = (0 != value);
    json attributes;
    attributes["isOn"] = on;
    if (m_client.setAttributes(dev.id, attributes, err)) {
      dev.attributes["isOn"] = on;
    }
    else {
      m_log->error("Register write: failed to set isOn for '{}': {}",
                   dev.name,
                   err);
    }
    return readRegister(dev, page, offset);
  }

  if (reg::LIGHT_LEVEL == offset) {
    const int level = (value > 100) ? 100 : value;
    json attributes;
    if (0 == level) {
      attributes["isOn"] = false;
    }
    else {
      attributes["lightLevel"] = level;
    }
    if (m_client.setAttributes(dev.id, attributes, err)) {
      if (0 == level) {
        dev.attributes["isOn"] = false;
      }
      else {
        dev.attributes["lightLevel"] = level;
      }
    }
    else {
      m_log->error("Register write: failed to set lightLevel for '{}': {}",
                   dev.name,
                   err);
    }
    return readRegister(dev, page, offset);
  }

  // Everything else is read only
  return readRegister(dev, page, offset);
}

// ----------------------------------------------------------------------------
// CLASS1.PROTOCOL register access events
// ----------------------------------------------------------------------------

int
CIkeaDirigera::handleProtocolEvent(uint16_t vscpType,
                                   const uint8_t *pdata,
                                   uint16_t size)
{
  if ((nullptr == pdata) || (size < 2)) {
    return CANAL_ERROR_PARAMETER;
  }

  std::lock_guard<std::mutex> lock(m_deviceMutex);

  // All register events address the node by nickname (= device index)
  const uint8_t nickname = pdata[0];
  DirigeraDevice *dev = nullptr;
  for (auto &item : m_devices) {
    if (item.second.index == nickname) {
      dev = &item.second;
      break;
    }
  }
  if (nullptr == dev) {
    return CANAL_ERROR_SUCCESS; // not for one of our nodes
  }

  switch (vscpType) {

    case VSCP_TYPE_PROTOCOL_READ_REGISTER: {
      // data: nickname, register
      const uint8_t offset = pdata[1];
      const uint8_t value = readRegister(*dev, dev->pageSelect, offset);
      queueEvent(VSCP_CLASS1_PROTOCOL,
                 VSCP_TYPE_PROTOCOL_RW_RESPONSE,
                 *dev,
                 { offset, value });
      return CANAL_ERROR_SUCCESS;
    }

    case VSCP_TYPE_PROTOCOL_WRITE_REGISTER: {
      // data: nickname, register, value
      if (size < 3) {
        return CANAL_ERROR_PARAMETER;
      }
      const uint8_t offset = pdata[1];
      const uint8_t value = writeRegister(*dev, dev->pageSelect, offset, pdata[2]);
      queueEvent(VSCP_CLASS1_PROTOCOL,
                 VSCP_TYPE_PROTOCOL_RW_RESPONSE,
                 *dev,
                 { offset, value });
      return CANAL_ERROR_SUCCESS;
    }

    case VSCP_TYPE_PROTOCOL_EXTENDED_PAGE_READ: {
      // data: nickname, page MSB, page LSB, offset, (count, 0 = 256)
      if (size < 4) {
        return CANAL_ERROR_PARAMETER;
      }
      const uint16_t page = static_cast<uint16_t>((pdata[1] << 8) | pdata[2]);
      const uint8_t offset = pdata[3];
      unsigned count = 1;
      if (size >= 5) {
        count = (0 == pdata[4]) ? 256 : pdata[4];
      }
      if (offset + count > 256) {
        count = 256 - offset;
      }

      // Respond with frames carrying up to four register values each
      uint8_t seq = 0;
      for (unsigned i = 0; i < count; i += 4, seq++) {
        std::vector<uint8_t> data = { seq,
                                      static_cast<uint8_t>(page >> 8),
                                      static_cast<uint8_t>(page & 0xff),
                                      static_cast<uint8_t>(offset + i) };
        for (unsigned k = i; (k < count) && (k < i + 4); k++) {
          data.push_back(
            readRegister(*dev, page, static_cast<uint8_t>(offset + k)));
        }
        queueEvent(VSCP_CLASS1_PROTOCOL,
                   VSCP_TYPE_PROTOCOL_EXTENDED_PAGE_RESPONSE,
                   *dev,
                   data);
      }
      return CANAL_ERROR_SUCCESS;
    }

    case VSCP_TYPE_PROTOCOL_EXTENDED_PAGE_WRITE: {
      // data: nickname, page MSB, page LSB, offset, value(s)
      if (size < 5) {
        return CANAL_ERROR_PARAMETER;
      }
      const uint16_t page = static_cast<uint16_t>((pdata[1] << 8) | pdata[2]);
      const uint8_t offset = pdata[3];
      const unsigned count = (size - 4 > 4) ? 4 : size - 4;

      std::vector<uint8_t> data = { 0,
                                    static_cast<uint8_t>(page >> 8),
                                    static_cast<uint8_t>(page & 0xff),
                                    offset };
      for (unsigned i = 0; i < count; i++) {
        data.push_back(writeRegister(*dev,
                                     page,
                                     static_cast<uint8_t>(offset + i),
                                     pdata[4 + i]));
      }
      queueEvent(VSCP_CLASS1_PROTOCOL,
                 VSCP_TYPE_PROTOCOL_EXTENDED_PAGE_RESPONSE,
                 *dev,
                 data);
      return CANAL_ERROR_SUCCESS;
    }

    default:
      return CANAL_ERROR_SUCCESS;
  }
}

// ----------------------------------------------------------------------------
// Decision matrix
// ----------------------------------------------------------------------------

void
CIkeaDirigera::executeDmAction(DirigeraDevice &dev,
                               uint8_t action,
                               uint8_t param)
{
  json attributes;

  switch (action) {
    case DM_ACTION_TURNON:
      attributes["isOn"] = true;
      break;
    case DM_ACTION_TURNOFF:
      attributes["isOn"] = false;
      break;
    case DM_ACTION_DIM:
      if (0 == param) {
        attributes["isOn"] = false;
      }
      else {
        attributes["lightLevel"] = (param > 100) ? 100 : param;
      }
      break;
    default:
      return;
  }

  std::string err;
  if (!m_client.setAttributes(dev.id, attributes, err)) {
    m_log->error("DM action {} failed for device '{}': {}",
                 action,
                 dev.name,
                 err);
  }
  else {
    m_log->debug("DM action {} executed for device '{}' ({})",
                 action,
                 dev.name,
                 attributes.dump());
  }
}

void
CIkeaDirigera::checkDecisionMatrix(uint16_t vscpClass,
                                   uint16_t vscpType,
                                   const uint8_t *pdata,
                                   uint16_t size)
{
  // The simulated decision matrix matches 8-bit class/type values
  if ((vscpClass > 0xff) || (vscpType > 0xff)) {
    return;
  }
  const uint8_t cls = static_cast<uint8_t>(vscpClass);
  const uint8_t type = static_cast<uint8_t>(vscpType);

  std::lock_guard<std::mutex> lock(m_deviceMutex);

  for (auto &item : m_devices) {
    DirigeraDevice &dev = item.second;

    for (int row = 0; row < DM_ROW_COUNT; row++) {
      const uint8_t *r = &dev.dm[row * DM_ROW_SIZE];
      const uint8_t flags = r[1];

      if (!(flags & DM_FLAG_ENABLED)) {
        continue;
      }
      // class/type match: (value ^ filter) & mask == 0
      if (((cls ^ r[3]) & r[2]) != 0) {
        continue;
      }
      if (((type ^ r[5]) & r[4]) != 0) {
        continue;
      }
      if ((flags & DM_FLAG_CHECK_ZONE) &&
          !((size >= 2) && ((0xff == pdata[1]) || (pdata[1] == dev.zone)))) {
        continue;
      }
      if ((flags & DM_FLAG_CHECK_SUBZONE) &&
          !((size >= 3) && ((0xff == pdata[2]) || (pdata[2] == dev.subzone)))) {
        continue;
      }

      executeDmAction(dev, r[6], r[7]);
    }
  }
}

// ----------------------------------------------------------------------------
// Internal MDF
// ----------------------------------------------------------------------------

std::string
CIkeaDirigera::generateMdf(const DirigeraDevice &dev)
{
  std::string regs;

  auto addReg = [&regs](int page, int offset, const char *name,
                        const char *access, const char *description) {
    char buf[512];
    snprintf(buf,
             sizeof(buf),
             "      <reg page=\"%d\" offset=\"%d\" access=\"%s\">\n"
             "        <name>%s</name>\n"
             "        <description>%s</description>\n"
             "      </reg>\n",
             page,
             offset,
             access,
             name,
             description);
    regs += buf;
  };

  const std::string &type = dev.deviceType;

  if (("light" == type) || ("outlet" == type)) {
    addReg(0, reg::IS_ON, "on-state", "rw",
           "0 = off, 1 = on. Writing controls the device.");
  }
  if ("light" == type) {
    addReg(0, reg::LIGHT_LEVEL, "light-level", "rw",
           "Dim level 0-100 percent. Writing controls the device.");
  }
  if ("motionSensor" == type) {
    addReg(0, reg::IS_DETECTED, "motion-detected", "r",
           "1 when motion is detected.");
  }
  if ("openCloseSensor" == type) {
    addReg(0, reg::IS_OPEN, "open-state", "r", "1 when open.");
  }
  if ("environmentSensor" == type) {
    addReg(0, reg::TEMPERATURE_MSB, "temperature-msb", "r",
           "Temperature in centidegrees Celsius, MSB of int16.");
    addReg(0, reg::TEMPERATURE_LSB, "temperature-lsb", "r",
           "Temperature in centidegrees Celsius, LSB of int16.");
    addReg(0, reg::HUMIDITY, "humidity", "r", "Relative humidity percent.");
    addReg(0, reg::PM25_MSB, "pm25-msb", "r", "PM2.5 ug/m3, MSB of uint16.");
    addReg(0, reg::PM25_LSB, "pm25-lsb", "r", "PM2.5 ug/m3, LSB of uint16.");
  }
  if ("lightSensor" == type) {
    addReg(0, reg::ILLUMINANCE_MSB, "illuminance-msb", "r",
           "Illuminance in lux, MSB of uint16.");
    addReg(0, reg::ILLUMINANCE_LSB, "illuminance-lsb", "r",
           "Illuminance in lux, LSB of uint16.");
  }

  addReg(0, reg::BATTERY, "battery", "r", "Battery level percent.");
  addReg(0, reg::ZONE, "zone", "r", "VSCP zone for the device.");
  addReg(0, reg::SUBZONE, "subzone", "r", "VSCP subzone for the device.");
  addReg(0, reg::SENSOR_INDEX, "sensor-index", "r",
         "Sensor index used in measurement events.");
  addReg(0, reg::REACHABLE, "reachable", "r",
         "1 when the device is reachable through the hub.");

  char mdf[4096];
  snprintf(
    mdf,
    sizeof(mdf),
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<vscp>\n"
    "  <module>\n"
    "    <name>vscpl2drv-ikea-dirigera %s node</name>\n"
    "    <model>IKEA Dirigera %s</model>\n"
    "    <version>%d.%d.%d</version>\n"
    "    <description>Simulated VSCP node for an IKEA Dirigera %s device. "
    "Status and functionality are exposed through page 0 user registers. "
    "A decision matrix with %d rows of %d bytes is located at page 1 "
    "offset 0 (row: oaddr, flags, class-mask, class-filter, type-mask, "
    "type-filter, action, action-parameter; flags: 0x80 enable, 0x10 check "
    "zone, 0x08 check subzone; actions: 1 turn on, 2 turn off, 3 dim with "
    "parameter 0-100).</description>\n"
    "    <registers>\n%s"
    "    </registers>\n"
    "    <dmatrix>\n"
    "      <level>1</level>\n"
    "      <start page=\"%d\" offset=\"0\"/>\n"
    "      <rowcnt>%d</rowcnt>\n"
    "      <rowsize>%d</rowsize>\n"
    "    </dmatrix>\n"
    "  </module>\n"
    "</vscp>\n",
    dev.deviceType.c_str(),
    dev.deviceType.c_str(),
    VSCPL2DRV_IKEA_DIRIGERA_MAJOR_VERSION,
    VSCPL2DRV_IKEA_DIRIGERA_MINOR_VERSION,
    VSCPL2DRV_IKEA_DIRIGERA_PATCH_VERSION,
    dev.deviceType.c_str(),
    DM_ROW_COUNT,
    DM_ROW_SIZE,
    regs.c_str(),
    DM_PAGE,
    DM_ROW_COUNT,
    DM_ROW_SIZE);

  return mdf;
}
