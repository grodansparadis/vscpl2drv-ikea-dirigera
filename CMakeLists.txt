cmake_minimum_required(VERSION 3.16)
project(vscpl2drv-ikea-dirigera VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_path(VSCP_INCLUDE_DIR
    NAMES vscp.h canal.h
    DOC "Directory containing the VSCP driver API headers"
)

if(NOT VSCP_INCLUDE_DIR OR
   NOT EXISTS "${VSCP_INCLUDE_DIR}/vscp.h" OR
   NOT EXISTS "${VSCP_INCLUDE_DIR}/canal.h")
    message(FATAL_ERROR
        "VSCP headers were not found. Set VSCP_INCLUDE_DIR to the directory "
        "containing vscp.h and canal.h (usually vscp/src/vscp/common).")
endif()

add_library(vscpl2drv-ikea-dirigera SHARED
    src/vscpl2drv-ikea-dirigera.cpp
)

target_include_directories(vscpl2drv-ikea-dirigera PRIVATE "${VSCP_INCLUDE_DIR}")
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
