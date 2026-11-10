// vscpl2drv-ikea-dirigera.cpp
//
// Exported VSCP level II driver interface for the IKEA Dirigera driver.
//
// This file is part of the VSCP (https://www.vscp.org)
//
// The MIT License (MIT)
//
// Copyright (C) 2000-2026 Ake Hedman and contributors,
// the VSCP project <info@vscp.org>

#include "dirigera.h"

#include <canal.h>
#include <vscp.h>

#include <limits>
#include <map>
#include <memory>
#include <mutex>

#if defined(_WIN32)
#define VSCP_DRIVER_EXPORT __declspec(dllexport)
#else
#define VSCP_DRIVER_EXPORT __attribute__((visibility("default")))
#endif

namespace {

std::mutex g_driver_mutex;
std::map<long, std::unique_ptr<CIkeaDirigera>> g_drivers;
long g_next_handle = 1;

CIkeaDirigera *
get_driver(long handle)
{
    std::lock_guard<std::mutex> lock(g_driver_mutex);
    auto it = g_drivers.find(handle);
    return (it != g_drivers.end()) ? it->second.get() : nullptr;
}

} // namespace

extern "C" {

VSCP_DRIVER_EXPORT long
VSCPOpen(const char *config_path, const char *guid)
{
    if (nullptr == config_path) {
        return 0;
    }

    try {
        auto driver = std::make_unique<CIkeaDirigera>();
        if (!driver->open(config_path, guid)) {
            return 0;
        }

        std::lock_guard<std::mutex> lock(g_driver_mutex);
        long handle = g_next_handle;
        while (g_drivers.find(handle) != g_drivers.end()) {
            handle = handle == std::numeric_limits<long>::max() ? 1 : handle + 1;
        }
        g_next_handle = handle == std::numeric_limits<long>::max() ? 1 : handle + 1;
        g_drivers.emplace(handle, std::move(driver));
        return handle;
    }
    catch (...) {
        return 0;
    }
}

VSCP_DRIVER_EXPORT int
VSCPClose(long handle)
{
    std::lock_guard<std::mutex> lock(g_driver_mutex);
    return g_drivers.erase(handle) ? CANAL_ERROR_SUCCESS : CANAL_ERROR_CHANNEL;
}

VSCP_DRIVER_EXPORT int
VSCPWrite(long handle, const vscpEvent *event, unsigned long timeout)
{
    (void)timeout;
    if (nullptr == event) {
        return CANAL_ERROR_PARAMETER;
    }

    CIkeaDirigera *driver = get_driver(handle);
    if (nullptr == driver) {
        return CANAL_ERROR_CHANNEL;
    }

    return driver->writeEvent(event);
}

VSCP_DRIVER_EXPORT int
VSCPRead(long handle, vscpEvent *event, unsigned long timeout)
{
    if (nullptr == event) {
        return CANAL_ERROR_PARAMETER;
    }

    CIkeaDirigera *driver = get_driver(handle);
    if (nullptr == driver) {
        return CANAL_ERROR_CHANNEL;
    }

    return driver->readEvent(event, timeout);
}

VSCP_DRIVER_EXPORT unsigned long
VSCPGetVersion(void)
{
    // The version scheme is year.month.patch - the year is reported as
    // two digits so each component fits its 8-bit field
    return ((static_cast<unsigned long>(DRIVER_VERSION_MAJOR % 100) << 16) |
            (static_cast<unsigned long>(DRIVER_VERSION_MINOR) << 8) |
            static_cast<unsigned long>(DRIVER_VERSION_PATCH % 256))
           << 8;
}

VSCP_DRIVER_EXPORT const char *
VSCPGetVendorString(void)
{
    return "VSCP IKEA Dirigera driver";
}

} // extern "C"
