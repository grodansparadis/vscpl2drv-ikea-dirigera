# vscpl2drv-ikea-dirigera

Starter code for a VSCP Level II driver for IKEA Dirigera. This initial
scaffold exports the standard driver entry points and manages open handles;
it does not yet connect to a Dirigera hub or exchange VSCP events. Reads time
out and writes return `CANAL_ERROR_NOT_SUPPORTED` until the hub integration is
implemented.

## Build

Install or check out the VSCP source tree, then configure with the directory
that contains `vscp.h` and `canal.h`:

```sh
cmake -S . -B build -DVSCP_INCLUDE_DIR=/path/to/vscp/src/vscp/common
cmake --build build
```

The shared library is written to the build directory. The exported API
functions are `VSCPOpen`, `VSCPClose`, `VSCPRead`, `VSCPWrite`,
`VSCPGetVersion`, and `VSCPGetVendorString`.
