# Driver database

On startup the driver fetches the complete home state from the hub
(`GET /v1/home`) and stores it in the SQLite database given by the
`database` configuration key (default
`/var/lib/vscp/vscpl2drv-ikea-dirigera.sqlite3`, `""` disables). If `/home`
is unavailable the driver falls back to the plain device list and the home
tables stay empty. If the database cannot be opened the driver logs an error
and keeps running without persistent storage.

## Tables

| Table | Content |
| ----- | ------- |
| `home` | One row: `fetched_at` (UTC ISO timestamp) and the complete raw JSON document as returned by the hub. Replaced on every driver start. |
| `hub` | Hub id, name, firmware version and raw JSON. |
| `devices` | One row per device: `id`, `name`, `device_type`, `room`, `reachable`, raw JSON — plus the VSCP addressing assigned by the driver: `vscp_index`, `vscp_zone`, `vscp_subzone`, `vscp_sensor_index`, `vscp_guid`. |
| `rooms` | Room id, name and raw JSON. |
| `scenes` | Scene id, name and raw JSON. |
| `users` | User uid, name and raw JSON. |
| `device_sets` | Device set id, name and raw JSON. |
| `mdf` | Internally generated MDF document (XML) per device type. See [simulated-nodes.md](./simulated-nodes.md). |
| `dm` | Decision matrix blob (32 bytes) per device id. Written whenever a DM register is written and restored when the device registers on startup. |
| `node_state` | Writable simulated-node registers per device id: `page_select` and the 5-byte `user_id` blob. Written whenever one of these registers is written and restored when the device registers on startup. |

## Examples

```sh
# What devices are mapped to which VSCP addresses?
sqlite3 /var/lib/vscp/vscpl2drv-ikea-dirigera.sqlite3 \
  "SELECT vscp_index, name, device_type, vscp_zone, vscp_subzone, vscp_guid
   FROM devices ORDER BY vscp_index"

# The complete raw home document
sqlite3 /var/lib/vscp/vscpl2drv-ikea-dirigera.sqlite3 \
  "SELECT json FROM home" | jq .

# MDF for lights
sqlite3 /var/lib/vscp/vscpl2drv-ikea-dirigera.sqlite3 \
  "SELECT xml FROM mdf WHERE device_type='light'"
```

[filename](./bottom-copyright.md ':include')
