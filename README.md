# vscpl2drv-ikea-dirigera

[![Linux build](https://github.com/grodansparadis/vscpl2drv-ikea-dirigera/actions/workflows/linux-build-status.yml/badge.svg)](https://github.com/grodansparadis/vscpl2drv-ikea-dirigera/actions/workflows/linux-build-status.yml)
[![Windows build](https://github.com/grodansparadis/vscpl2drv-ikea-dirigera/actions/workflows/windows-build-status.yml/badge.svg)](https://github.com/grodansparadis/vscpl2drv-ikea-dirigera/actions/workflows/windows-build-status.yml)
[![macOS build](https://github.com/grodansparadis/vscpl2drv-ikea-dirigera/actions/workflows/macos-build-status.yml/badge.svg)](https://github.com/grodansparadis/vscpl2drv-ikea-dirigera/actions/workflows/macos-build-status.yml)
[![Raspberry Pi 32-bit build](https://github.com/grodansparadis/vscpl2drv-ikea-dirigera/actions/workflows/raspberry-pi-32-build-status.yml/badge.svg)](https://github.com/grodansparadis/vscpl2drv-ikea-dirigera/actions/workflows/raspberry-pi-32-build-status.yml)
[![Raspberry Pi 64-bit build](https://github.com/grodansparadis/vscpl2drv-ikea-dirigera/actions/workflows/raspberry-pi-64-build-status.yml/badge.svg)](https://github.com/grodansparadis/vscpl2drv-ikea-dirigera/actions/workflows/raspberry-pi-64-build-status.yml)

A VSCP level II driver that lets the [VSCP daemon](https://github.com/grodansparadis/vscp)
interact with Matter/Zigbee devices connected to an IKEA Dirigera hub.

The driver talks to the hub's REST API (`https://<hub>:8443/v1`) and gets
status updates either by polling or, optionally, by subscribing to the hub's
websocket event stream (`wss://<hub>:8443/v1`). Attribute changes are
translated to VSCP events. Incoming CLASS1.CONTROL events are translated to
hub commands so lights and outlets can be switched and dimmed from the VSCP
side.

## Build

All dependencies (the VSCP headers, spdlog and nlohmann-json) are fetched
automatically by CMake. libcurl and libwebsockets development packages must be
installed (`apt install libcurl4-openssl-dev libwebsockets-dev` on
Debian/Ubuntu).

```sh
cmake -S . -B build
cmake --build build
```

The shared library `libvscpl2drv-ikea-dirigera.so` is written to the build
directory. The exported API functions are `VSCPOpen`, `VSCPClose`, `VSCPRead`,
`VSCPWrite`, `VSCPGetVersion`, and `VSCPGetVendorString`.

The driver version is maintained in [src/version.h](./src/version.h).

### Install and packages

`cmake --build build --target install` installs the driver to
`/var/lib/vscp/drivers/level2/`, the test tool as
`vscpl2drv-ikea-dirigera-test` and the example configuration to
`/usr/share/vscpl2drv-ikea-dirigera/`. Packages can be created with CPack
(`cpack -G DEB`, `-G RPM`, `-G TGZ`, `-G ZIP` from the build directory).

CI builds install artifacts for Linux (x64 plus Raspberry Pi 32/64-bit),
Windows and macOS on every push — see the Actions tab. Tagged pushes publish
the artifacts as a GitHub release.

## Test application

The build also produces `testapp`, an interactive tool that loads the driver
shared library the same way the VSCP daemon does and can be used to test it
against a real hub:

```sh
./build/testapp/testapp ./build/libvscpl2drv-ikea-dirigera.so /path/to/config.json [guid]
```

It can also run the one-time hub pairing flow to obtain an access token
(see "Pairing with the Dirigera hub" above):

```sh
./build/testapp/testapp --get-token <hub-ip> [port]
```

It prints every event the driver generates (with decoded measurement values)
and accepts the following commands on stdin:

| Command | Action |
| ------- | ------ |
| `on <subzone> [zone]` | Send CLASS1.CONTROL TurnOn (255 = all devices) |
| `off <subzone> [zone]` | Send CLASS1.CONTROL TurnOff |
| `dim <subzone> <0-100> [zone]` | Send CLASS1.CONTROL Dim lamp(s) |
| `devices` | List the devices on the hub (id, type, name, reachability, attributes) — useful for finding device ids for the config |
| `help` | Show help |
| `quit` | Close the driver and exit |

If `zone` is omitted it defaults to 255 (match any zone).

## Pairing with the Dirigera hub

Pairing is a one-time OAuth2 PKCE flow that needs a physical press of the
action button on the hub. You need the hub's IP address first. You can find it
in your router's device list, and the hub also announces itself as
`gw2-<id>.local`.

The bundled test application can run the pairing flow:

```sh
./build/testapp/testapp --get-token <hub-ip> [port]
```

Press the action button on the bottom of the hub when prompted, then press
ENTER, and the access token is printed. Alternatives:

- Python: `pip install dirigera`, then run `generate-token <hub-ip>`, press the
  button on the bottom of the hub, and hit ENTER.
- Node: `npx dirigera authenticate` prompts you to press the button within 60
  seconds and then prints the token.

Put the printed access token in the driver configuration file (see below).

## Configuration

The path to a JSON configuration file is given to the driver by the VSCP
daemon (the path set for the driver in the daemon configuration). See
[resources/example-config.json](./resources/example-config.json).

```json
{
    "hub": "192.168.1.200",
    "port": 8443,
    "token": "<access token obtained from the hub>",
    "verify-tls": false,
    "use-websocket": false,
    "poll-interval": 5,
    "zone": 0,
    "logging": {
        "console-enable": true,
        "console-level": "info",
        "file-enable": false,
        "file-path": "/var/log/vscp/vscpl2drv-ikea-dirigera.log",
        "file-level": "info",
        "file-max-size": 5242880,
        "file-max-files": 7
    },
    "devices": [
        {
            "name": "Desk lamp",
            "guid": "00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:01",
            "zone": 1,
            "subzone": 2,
            "type": "light"
        },
        {
            "id": "dirigera-device-id",
            "subzone": 3
        }
    ]
}
```

| Key | Description |
| --- | ----------- |
| `hub` | IP address or hostname of the Dirigera hub. **Required.** |
| `port` | Hub API port. Default `8443`. |
| `token` | Access token from the pairing step. **Required.** |
| `verify-tls` | Verify the hub TLS certificate. The hub uses a self-signed certificate so the default is `false`. |
| `use-websocket` | Subscribe to the hub websocket event stream (`wss://<hub>:8443/v1`) for status updates instead of polling. The driver reconnects automatically and re-syncs the device registry over REST after every (re)connect. Default `false` (poll). |
| `poll-interval` | Device state poll interval in seconds (poll mode). Default `5`. |
| `zone` | Default VSCP zone used in generated events and matched in control events. Default `0`. |
| `logging` | Console and rotating-file log settings (spdlog levels: `trace`, `debug`, `info`, `warn`, `err`, `off`). |
| `devices` | Optional list of device mappings, see below. |

### Device mappings

Each entry in the `devices` list maps a hub device to VSCP addressing:

| Key | Description |
| --- | ----------- |
| `name` | Device name (`customName`) on the hub to match. The comparison is case insensitive (UTF-8, ASCII + Latin-1). |
| `id` | Unique device id to match. Used for devices whose names are not mapped. `name` matches take precedence. |
| `guid` | GUID (string form) used for events from the device. A Class2 Level I CONTROL event carrying this GUID as destination addresses the device directly. |
| `zone` | VSCP zone for the device. Defaults to the global `zone`. |
| `subzone` | VSCP subzone for the device. Defaults to the device index. |
| `type` | Expected device type. The driver warns if the hub reports a different type for the matched device. |

Each discovered device is assigned an index (in discovery order). Devices
without a mapped `guid` use the interface GUID with the index as LSB; the
index is also the default subzone.

If the unique device id ends in a short `-<n>`/`_<n>` suffix (as hub device
ids do, e.g. `...-6e1be6a3bc1f_1`), that number is used as the sensor index
in measurement events for the device.

## Events generated by the driver

Device attribute changes are picked up from the hub (polled, or pushed over
the websocket event stream when `use-websocket` is enabled) and reported as
VSCP events. Measurements use single precision floating point data coding.

| Hub attribute | VSCP event |
| ------------- | ---------- |
| `isOn` (lights, outlets) | CLASS1.INFORMATION ON (3) / OFF (4), data = index, zone, subzone |
| `isDetected` (motion sensors) | CLASS1.INFORMATION DETECT (49) / OFF (4) |
| `isOpen` (open/close sensors) | CLASS1.INFORMATION ON (3) / OFF (4) |
| `currentTemperature` | CLASS1.MEASUREMENT Temperature (6), unit 1 (°C) |
| `currentRH` | CLASS1.MEASUREMENT Humidity (35), unit 0 (%) |
| `currentPM25` | CLASS1.MEASUREMENT Chemical mass concentration (46), unit 0 (kg/m³) |
| `illuminance` | CLASS1.MEASUREMENT Illuminance (25), unit 0 (lux) |
| `lightLevel` | CLASS1.MEASUREMENT Relative level (50), unit 0 (0.0–1.0) |

The full initial state of every device is reported when the driver starts and
when a new device appears on the hub.

## Controlling devices

The driver acts on CLASS1.CONTROL events (also when wrapped as Class2 Level I,
class 542). For Class2 Level I events a non-zero destination GUID addresses
the device mapped to that GUID directly. Otherwise the event zone must match
the device zone (or be 255), and the subzone selects the device (255 = all
devices).

| Event | Action |
| ----- | ------ |
| TurnOn (5) | `isOn = true` for matching lights/outlets |
| TurnOff (6) | `isOn = false` for matching lights/outlets |
| Dim lamp(s) (20) | data byte 0 = 0 turns off, 1–100 sets `lightLevel` in percent |

All other events are silently accepted.

## License

MIT - see [LICENSE](./LICENSE).
