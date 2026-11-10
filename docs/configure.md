# Configure the driver

The VSCP daemon hands the driver the path to a JSON configuration file in
`VSCPOpen()`. A complete example is found in
[resources/example-config.json](https://github.com/grodansparadis/vscpl2drv-ikea-dirigera/blob/main/resources/example-config.json).

```json
{
    "hub": "192.168.1.100",
    "port": 8443,
    "token": "<access token obtained from the hub>",
    "verify-tls": false,
    "use-websocket": false,
    "poll-interval": 5,
    "heartbeat-interval": 30,
    "zone": 0,
    "database": "/var/lib/vscp/vscpl2drv-ikea-dirigera.sqlite3",
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

## Top level keys

| Key | Type | Default | Description |
| --- | ---- | ------- | ----------- |
| `hub` | string | — | IP address or hostname of the Dirigera hub. **Required.** The hub announces itself on mDNS as `gw2-<id>.local`. |
| `port` | number | `8443` | Hub API port. |
| `token` | string | — | Access token from the one-time pairing step. **Required.** See [Test and use the driver](./using.md). |
| `verify-tls` | bool | `false` | Verify the hub TLS certificate. The hub uses a certificate signed by IKEA's own CA, so verification is off by default. |
| `use-websocket` | bool | `false` | Use the hub websocket event stream (`wss://<hub>:8443/v1`) for status updates instead of polling. The driver reconnects automatically (5 s backoff) and re-syncs the device registry over REST after every (re)connect. |
| `poll-interval` | number | `5` | Seconds between device state polls (poll mode only). Minimum 1. |
| `heartbeat-interval` | number | `30` | Seconds between VSCP node heartbeats (CLASS1.INFORMATION Node Heartbeat, type 9) sent for every simulated device that is active and reachable through the hub. `0` disables heartbeats. |
| `zone` | number | `0` | Default VSCP zone for devices without a mapped zone. |
| `database` | string | `/var/lib/vscp/vscpl2drv-ikea-dirigera.sqlite3` | SQLite database path. The complete home state is stored here on startup; decision matrices and internal MDFs are persisted here as well. Set to `""` to disable. If the database cannot be opened the driver logs an error and continues without persistent storage. |
| `logging` | object | see below | Log settings. |
| `devices` | array | `[]` | Device mappings, see below. |

## Logging

The driver logs through spdlog. Levels: `trace`, `debug`, `info`, `warn`,
`err`, `critical`, `off`.

| Key | Type | Default | Description |
| --- | ---- | ------- | ----------- |
| `console-enable` | bool | `true` | Log to the console (the daemon's stdout). |
| `console-level` | string | `info` | Console log level. |
| `file-enable` | bool | `false` | Log to a rotating file. |
| `file-path` | string | `/var/log/vscp/vscpl2drv-ikea-dirigera.log` | Log file path. |
| `file-level` | string | `info` | File log level. |
| `file-max-size` | number | `5242880` | Max size per log file in bytes. |
| `file-max-files` | number | `7` | Number of rotated files to keep. |

## Device mappings

Each entry in `devices` maps one hub device to VSCP addressing. A hub device
is matched against the mappings **by name first** (the hub `customName`,
compared case insensitively as UTF-8, ASCII + Latin-1 so å/ä/ö/é/ü fold
correctly), **then by unique device id**.

| Key | Type | Description |
| --- | ---- | ----------- |
| `name` | string | Device name on the hub to match. |
| `id` | string | Unique device id to match (used when the name is not mapped). Find ids with the test application's `devices` command. |
| `guid` | string | GUID (`FF:FF:...` string form) used for events **from** the device. A Class2 Level I CONTROL event carrying this GUID as destination addresses the device **to** directly. |
| `zone` | number | VSCP zone for the device. Defaults to the global `zone`. |
| `subzone` | number | VSCP subzone for the device. Defaults to the device index. |
| `type` | string | Expected hub device type (`light`, `outlet`, `motionSensor`, `environmentSensor`, `lightSensor`, `openCloseSensor`, ...). The driver warns if the hub reports a different type. |

Unmapped devices get defaults:

* **index** — assigned in discovery order, also the node nickname
* **GUID** — the interface GUID with the index as LSB
* **zone** — the global `zone`
* **subzone** — the device index

## Sensor index

If a unique device id ends in a short (1-2 digit) `-<n>` or `_<n>` suffix —
as Dirigera ids do, e.g. `...-6e1be6a3bc1f_1` — that number is used as the
sensor index (0-7) in measurement event data codings for the device. The
suffix guard ensures the last group of a plain UUID is never misread as an
index.

[filename](./bottom-copyright.md ':include')
