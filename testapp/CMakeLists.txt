# CMakeLists.txt
#
# Test application for the vscpl2drv-ikea-dirigera driver.
#
# Copyright (C) 2000-2026 Ake Hedman and contributors,
# the VSCP project <info@vscp.org>
#
# Licensed under the MIT license. See the LICENSE file in the project root.

add_executable(testapp main.cpp)

find_package(OpenSSL REQUIRED)

target_include_directories(testapp PRIVATE "${VSCP_INCLUDE_DIR}")
target_link_libraries(testapp PRIVATE
    ${CMAKE_DL_LIBS}
    Threads::Threads
    CURL::libcurl
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
)
