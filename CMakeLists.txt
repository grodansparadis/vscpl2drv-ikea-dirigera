cmake_minimum_required(VERSION 3.16)

# Refuse in-source builds - they pollute the source tree and a stray
# CMakeCache.txt in the source directory breaks later out-of-source builds
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR
        "In-source builds are not allowed. Remove CMakeCache.txt and "
        "CMakeFiles/ from the source directory, then build out of source:\n"
        "  mkdir build && cd build && cmake .. && make")
endif()

# Parse the version from src/version.h (single source of truth, also parsed
# by the CI workflows)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/version.h" VERSION_H_CONTENT)
foreach(COMPONENT MAJOR MINOR PATCH)
    string(REGEX MATCH
        "#define[ \t]+VSCPL2DRV_IKEA_DIRIGERA_${COMPONENT}_VERSION[ \t]+([0-9]+)"
        VERSION_MATCH "${VERSION_H_CONTENT}")
    if(NOT VERSION_MATCH)
        message(FATAL_ERROR
            "Missing VSCPL2DRV_IKEA_DIRIGERA_${COMPONENT}_VERSION in src/version.h")
    endif()
    set(VERSION_${COMPONENT} "${CMAKE_MATCH_1}")
endforeach()

project(vscpl2drv-ikea-dirigera
    VERSION ${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

set(VSCP_FETCH_CONTENT_OPTIONS)
if(NOT CMAKE_VERSION VERSION_LESS "3.18")
    list(APPEND VSCP_FETCH_CONTENT_OPTIONS SOURCE_SUBDIR "__vscp_sources_only__")
endif()
if(NOT CMAKE_VERSION VERSION_LESS "3.24")
    list(APPEND VSCP_FETCH_CONTENT_OPTIONS DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
endif()

FetchContent_Declare(
    vscp
    # Only the VSCP driver API headers are used by this driver, so there is no
    # need to clone the repository. A pinned source tarball avoids fetching
    # ~1.2GB of git history plus ~400MB of submodules that this project never
    # uses.
    # Get URL_HASH
    # curl -sL https://codeload.github.com/grodansparadis/vscp/tar.gz/<commit> | sha256sum
    URL https://codeload.github.com/grodansparadis/vscp/tar.gz/898fcc1ce6a89b5f4b2b8a3a233cad663dcd278f
    URL_HASH SHA256=5485966dab085bab3ce4fb815de0a4bc8056cf11afc35b1531cc9cb8bcec6b16
    ${VSCP_FETCH_CONTENT_OPTIONS}
)
# Make the VSCP sources available without adding its standalone library targets
# to this build. Only the headers in src/vscp/common are used.
if(CMAKE_VERSION VERSION_LESS "3.18")
    FetchContent_GetProperties(vscp)
    if(NOT vscp_POPULATED)
        FetchContent_Populate(vscp)
    endif()
else()
    FetchContent_MakeAvailable(vscp)
endif()

set(VSCP_INCLUDE_DIR "${vscp_SOURCE_DIR}/src/vscp/common")

FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.17.0
)
FetchContent_MakeAvailable(spdlog)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
)
FetchContent_MakeAvailable(nlohmann_json)

find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(libwebsockets CONFIG REQUIRED)
# Prefer the shared libwebsockets target; some package managers (e.g. vcpkg
# static triplets) only provide the plain 'websockets' target
if(TARGET websockets_shared)
    set(LWS_LIBRARIES websockets_shared)
else()
    set(LWS_LIBRARIES websockets)
endif()
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_library(vscpl2drv-ikea-dirigera SHARED
    src/vscpl2drv-ikea-dirigera.cpp
    src/dirigera.cpp
    src/registers.cpp
    src/db.cpp
)

target_include_directories(vscpl2drv-ikea-dirigera PRIVATE "${VSCP_INCLUDE_DIR}")
target_link_libraries(vscpl2drv-ikea-dirigera PRIVATE
    CURL::libcurl
    SQLite::SQLite3
    ${LWS_LIBRARIES}
    Threads::Threads
    spdlog::spdlog_header_only
    nlohmann_json::nlohmann_json
)
target_compile_definitions(vscpl2drv-ikea-dirigera PRIVATE
    DRIVER_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    DRIVER_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    DRIVER_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

if(WIN32)
    set_target_properties(vscpl2drv-ikea-dirigera PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS ON
        PREFIX ""
    )
endif()

add_subdirectory(testapp)

# ----------------------------------------------------------------------------
# Install
# ----------------------------------------------------------------------------

include(GNUInstallDirs)

if(WIN32)
    install(TARGETS vscpl2drv-ikea-dirigera RUNTIME DESTINATION bin)
    install(PROGRAMS $<TARGET_FILE:testapp>
            RENAME vscpl2drv-ikea-dirigera-test.exe
            DESTINATION bin)
    # Dependency DLLs copied next to the build output (vcpkg applocal)
    file(GLOB deps_files "${CMAKE_BINARY_DIR}/Release/*.dll")
    install(FILES ${deps_files} DESTINATION bin)
    install(FILES LICENSE README.md resources/example-config.json DESTINATION .)
else()
    # VSCP level II drivers are expected in /var/lib/vscp/drivers/level2
    set(VSCP_LEVEL2_DRV_DIR "/var/lib/vscp/drivers/level2"
        CACHE PATH "Install directory for VSCP level II drivers")
    install(TARGETS vscpl2drv-ikea-dirigera
            LIBRARY DESTINATION ${VSCP_LEVEL2_DRV_DIR})
    # Install the test tool under a non-generic name
    install(PROGRAMS $<TARGET_FILE:testapp>
            RENAME vscpl2drv-ikea-dirigera-test
            DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES resources/example-config.json
            DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/vscpl2drv-ikea-dirigera)
endif()

# ----------------------------------------------------------------------------
# Packaging (CPack)
# ----------------------------------------------------------------------------

set(CPACK_PACKAGE_NAME "vscpl2drv-ikea-dirigera")
set(CPACK_PACKAGE_DESCRIPTION "VSCP level II driver for the IKEA Dirigera hub")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "VSCP level II driver that interacts with Matter/Zigbee devices connected to an IKEA Dirigera hub")
set(CPACK_PACKAGE_VENDOR "the VSCP Project")
set(CPACK_PACKAGE_CONTACT "akhe@vscp.org")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
set(CPACK_PACKAGE_FILE_NAME
    "${CPACK_PACKAGE_NAME}_${PROJECT_VERSION}_${CMAKE_SYSTEM_PROCESSOR}")

if(WIN32)
    set(CPACK_GENERATOR "NSIS" "ZIP")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "VSCP\\\\${CPACK_PACKAGE_NAME}")
    set(CPACK_NSIS_PACKAGE_NAME "vscpl2drv-ikea-dirigera")
    set(CPACK_NSIS_DISPLAY_NAME "VSCP level II IKEA Dirigera driver")
    set(CPACK_NSIS_HELP_LINK
        "https://github.com/grodansparadis/vscpl2drv-ikea-dirigera")
    set(CPACK_NSIS_URL_INFO_ABOUT
        "https://github.com/grodansparadis/vscpl2drv-ikea-dirigera")
    set(CPACK_NSIS_CONTACT "akhe@vscp.org")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MODIFY_PATH OFF)
elseif(APPLE)
    set(CPACK_GENERATOR "TGZ" "ZIP")
    set(CPACK_SET_DESTDIR ON)
else()
    set(CPACK_GENERATOR "DEB" "TGZ" "ZIP")
    # Absolute install paths (driver directory) must be packaged as-is
    set(CPACK_SET_DESTDIR ON)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    set(CPACK_DEBIAN_PACKAGE_SECTION "network")
    set(CPACK_RPM_PACKAGE_LICENSE "MIT")
endif()

include(CPack)
