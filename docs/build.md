# Build the driver from source

## Dependencies

Fetched automatically by CMake: the VSCP headers (pinned tarball), spdlog
and nlohmann-json. Required from the system:

| Dependency | Debian/Ubuntu/Raspberry Pi OS | macOS (brew) | Windows (vcpkg) |
| ---------- | ----------------------------- | ------------ | --------------- |
| libcurl | `libcurl4-openssl-dev` | `curl` | `curl:x64-windows` |
| libwebsockets | `libwebsockets-dev` | `libwebsockets` | `libwebsockets:x64-windows` |
| OpenSSL | `libssl-dev` | `openssl@3` | `openssl:x64-windows` |
| SQLite3 | `libsqlite3-dev` | `sqlite` | `sqlite3:x64-windows` |

## Build

Out-of-source builds only (enforced by CMake):

```sh
mkdir build && cd build
cmake ..
make
```

The driver version is maintained in
[src/version.h](https://github.com/grodansparadis/vscpl2drv-ikea-dirigera/blob/main/src/version.h) and
parsed by CMake and the CI workflows.

## Install

```sh
sudo cmake --build build --target install
```

| What | Where (Linux) |
| ---- | ------------- |
| `libvscpl2drv-ikea-dirigera.so` | `/var/lib/vscp/drivers/level2/` (override with `-DVSCP_LEVEL2_DRV_DIR=...`) |
| `vscpl2drv-ikea-dirigera-test` | `<prefix>/bin/` |
| `example-config.json` | `<prefix>/share/vscpl2drv-ikea-dirigera/` |

On Windows everything installs under `Program Files\VSCP\vscpl2drv-ikea-dirigera`.

## Packages

From the build directory:

```sh
cpack -G DEB   # Debian package (automatic shared library dependencies)
cpack -G RPM   # RPM package
cpack -G TGZ   # tarball
cpack -G ZIP   # zip
# Windows: cpack -G NSIS produces an installer .exe
```

## Continuous integration

[cmake_cross_platform_build.yml](https://github.com/grodansparadis/vscpl2drv-ikea-dirigera/blob/main/.github/workflows/cmake_cross_platform_build.yml)
builds and uploads install artifacts on every push, pull request and tag:

| Job | Platform | Artifacts |
| --- | -------- | --------- |
| `build (ubuntu-latest)` | Linux x64 | `.so`, DEB, RPM, TGZ, ZIP |
| `build (windows-latest)` | Windows x64 (MSVC + vcpkg) | DLLs, NSIS installer `.exe`, ZIP |
| `build (macos-latest)` | macOS arm64 | `.dylib`, TGZ, ZIP |
| `Build (raspberry-pi-32)` | linux/arm/v7 (QEMU, Debian bookworm) | `.so`, DEB, TGZ |
| `Build (raspberry-pi-64)` | linux/arm64/v8 (QEMU, Debian bookworm) | `.so`, DEB, TGZ |

Tagged pushes publish all artifacts as a GitHub release. The per-platform
status badges in the main README are driven by the
`*-build-status.yml` workflows.

[filename](./bottom-copyright.md ':include')
