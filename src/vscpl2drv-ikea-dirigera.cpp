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

struct Driver {
};

std::mutex g_driver_mutex;
std::map<long, std::unique_ptr<Driver>> g_drivers;
long g_next_handle = 1;

bool has_driver(long handle)
{
    std::lock_guard<std::mutex> lock(g_driver_mutex);
    return g_drivers.find(handle) != g_drivers.end();
}

} // namespace

extern "C" {

VSCP_DRIVER_EXPORT long
VSCPOpen(const char *config_path, const char *guid)
{
    (void)config_path;
    (void)guid;

    try {
        auto driver = std::make_unique<Driver>();
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

    if (!has_driver(handle)) {
        return CANAL_ERROR_CHANNEL;
    }

    return CANAL_ERROR_NOT_SUPPORTED;
}

VSCP_DRIVER_EXPORT int
VSCPRead(long handle, vscpEvent *event, unsigned long timeout)
{
    (void)timeout;
    if (nullptr == event) {
        return CANAL_ERROR_PARAMETER;
    }

    if (!has_driver(handle)) {
        return CANAL_ERROR_CHANNEL;
    }

    return CANAL_ERROR_TIMEOUT;
}

VSCP_DRIVER_EXPORT unsigned long
VSCPGetVersion(void)
{
    return ((static_cast<unsigned long>(DRIVER_VERSION_MAJOR) << 16) |
            (static_cast<unsigned long>(DRIVER_VERSION_MINOR) << 8) |
            static_cast<unsigned long>(DRIVER_VERSION_PATCH))
           << 8;
}

VSCP_DRIVER_EXPORT const char *
VSCPGetVendorString(void)
{
    return "VSCP IKEA Dirigera driver";
}

} // extern "C"
