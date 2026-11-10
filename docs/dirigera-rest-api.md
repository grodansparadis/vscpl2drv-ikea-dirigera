# IKEA DIRIGERA hub REST API: known commands

IKEA publishes no official documentation for this API. This list is compiled from the source code of the three main reverse-engineered clients (TypeScript, Python, Java). The endpoints were read from their code; nothing here was tested against a hub.

Compiled 2026-11-09.

## Basics

- **Base URL:** `https://<hub-ip>:8443/v1/`
- **TLS:** the clients either pin IKEA's CA certificate or disable verification.
- **Auth:** `Authorization: Bearer <token>` on every call except the OAuth ones.
- **Body format:** JSON. Device PATCH bodies are an *array* of objects:
  ```json
  [{"attributes": {"lightLevel": 50}, "transitionTime": 500}]
  ```

## Authentication (PKCE pairing)

| Method | Path | Notes |
|---|---|---|
| GET | `/oauth/authorize` | Query: `audience=homesmart.local`, `response_type=code`, `code_challenge`, `code_challenge_method=S256`. Returns `code`. |
| POST | `/oauth/token` | Sent after pressing the hub's action button. Form-encoded: `code`, `name`, `grant_type=authorization_code`, `code_verifier`. Returns `access_token`. |
| POST | `/oauth/authorize` | Cancels pairing (form: `code`, `code_verifier`). Java client only. |
| GET | `/oauth/authorize/{transactionId}` | Polls pairing status. Java client only. |
| POST | `/oauth/token` | With `refresh_token` to refresh. Java client only. |

The code challenge is the base64url-encoded SHA-256 hash of the code verifier.

## Hub and home

| Method | Path | Notes |
|---|---|---|
| GET | `/home` | Full dump: hub, devices, rooms, scenes, device sets, users, music, adaptive profiles. |
| GET | `/hub/status` | Hub status and attributes. |
| PUT | `/hub/ota/check` | Checks for a firmware update. |
| PUT | `/hub/ota/update` | Installs a firmware update. |
| PUT | `/hub/ota/environment` | Sets the firmware channel. Java client only. |
| PUT | `/hub/cloud-integration` | Sets cloud "persistent mode". Java client only. |

## Devices

| Method | Path | Notes |
|---|---|---|
| GET | `/devices` | Lists all devices; clients filter by `deviceType` locally. |
| GET | `/devices/{id}` | One device. |
| PATCH | `/devices/{id}` | Sets attributes (see the table below). |
| PUT | `/devices/{id}/identify` | `{"period": 30}` starts blink/identify, `{"period": 0}` stops it. |
| PATCH | `/devices/room/{roomId}?deviceType=light` | Sets attributes on all devices in a room. |
| PATCH | `/devices/set/{setId}?deviceType=...` | Sets attributes on a device set. |
| PATCH | `/devices/all?deviceType=...` | Sets attributes on every device of a type. Java client only. |
| POST | `/step/{targetId}?deviceType=...` | Relative dimming: `{"type": "lightLevel", "stepSize": n, "transitionTime": ms}`. Java client only; the `type` string is inferred from its enum. |
| PUT | `/remoteLinks/{remoteId}/targets` | Links a remote to targets: `{"targetIds": [...]}`. Java client only. |

### Known `deviceType` values

`light`, `outlet`, `blinds`, `airPurifier`, `speaker`, `repeater`, `gateway`, `lightController`, `blindsController`, `soundController`, `shortcutController`, `genericSwitch`, `motionSensor`, `occupancySensor`, `openCloseSensor`, `environmentSensor`, `lightSensor`, `waterSensor`

### Writable attributes for `PATCH /devices/{id}`

The names and enum values come from the client code. The numeric ranges are not from the code, so check them against your own devices.

| Device | Attribute | Values |
|---|---|---|
| Any | `customName` | string |
| Light | `isOn` | boolean |
| Light | `lightLevel` | 1–100 |
| Light | `colorTemperature` | Kelvin, within the device's min/max |
| Light | `colorHue` + `colorSaturation` | 0–360 and 0–1, sent together |
| Light, outlet | `startupOnOff` | `startOn`, `startPrevious`, `startToggle` |
| Outlet | `isOn`, `statusLight`, `childLock` | boolean |
| Outlet | `energyConsumedAtLastReset` | `0` resets the energy counter |
| Blinds | `blindsTargetLevel` | 0–100 |
| Blinds | `blindsState` | `stopped`, `up`, `down` |
| Air purifier | `fanMode` | `auto`, `low`, `medium`, `high`, `on`, `off` |
| Air purifier | `motorState` | fan speed number |
| Air purifier | `childLock`, `statusLight` | boolean |
| Speaker | `volume` | 0–100 |
| Speaker | `playback` | `playbackPlaying`, `playbackPaused`, `playbackNext`, `playbackPrevious` |
| Controller | `controlMode` | `light`, `speaker`, `blind`, `shortcut` |
| Motion/occupancy sensor | `sensorConfig.onDuration` | seconds |
| Motion/occupancy sensor | `sensorConfig.scheduleOn` | boolean |
| Motion/occupancy sensor | `sensorConfig.schedule` | on/off time object |

`transitionTime` (milliseconds) sits beside `attributes` for light changes.

## Rooms

| Method | Path | Notes |
|---|---|---|
| GET | `/rooms` | Lists rooms. |
| GET | `/rooms/{id}` | One room. |
| POST | `/rooms` | `{name, icon, color}`. Returns `id`. |
| PUT | `/rooms/{id}` | Updates `{name, icon, color}`. |
| DELETE | `/rooms/{id}` | Deletes the room. |
| PATCH | `/rooms/{id}/devices` | Moves devices into the room: `{"deviceIds": [...]}`. |

## Device sets

| Method | Path | Notes |
|---|---|---|
| GET | `/home` | No list endpoint; sets are read from `deviceSets` in the dump. |
| POST | `/device-set` | `{name, icon}`. Returns `id`. |
| PUT | `/device-set/{id}` | Updates `{name, icon}`. |
| DELETE | `/device-set/{id}` | Deletes the set. |
| PATCH | `/device-set/{id}/configuration` | `{deviceIds, roomId?, remoteLinkIds?}`. |

## Scenes

| Method | Path | Notes |
|---|---|---|
| GET | `/scenes` | Lists scenes. |
| GET | `/scenes/{id}` | One scene. |
| POST | `/scenes` | `{info, type, actions, triggers}`. Returns `id`. |
| PUT | `/scenes/{id}` | Updates the same fields. |
| DELETE | `/scenes/{id}` | Deletes the scene. |
| POST | `/scenes/{id}/trigger` | Runs the scene. |
| POST | `/scenes/{id}/undo` | Undoes the scene. |

## Users

| Method | Path | Notes |
|---|---|---|
| GET | `/users` | Lists users (paired clients). |
| GET | `/users/me` | Current token's user. |
| PATCH | `/users/me` | `{"name": "..."}`. |
| DELETE | `/users/{id}` | Removes a user and revokes its token. |

## Music

| Method | Path | Notes |
|---|---|---|
| GET | `/music` | Playlists, favourites and now-playing data. |

## WebSocket events

Connect to `wss://<hub-ip>:8443/v1` with the same Bearer header. Messages are JSON with a `type` field.

| Group | Event types |
|---|---|
| Devices | `deviceAdded`, `deviceRemoved`, `deviceDiscovered`, `deviceStateChanged`, `deviceConfigurationChanged`, `remotePressEvent` |
| Device sets | `deviceSetCreated`, `deviceSetUpdated`, `deviceSetDeleted` |
| Rooms | `roomCreated`, `roomUpdated`, `roomDeleted` |
| Scenes | `sceneCreated`, `sceneUpdated`, `sceneDeleted`, `sceneTriggered`, `sceneCompleted` |
| Other | `musicUpdated`, `ping`, `pong`, `termination` |

## Caveats

- **Java-only rows are the least reliable.** The Java client's author warns that most of its interfaces are barely tested and a few are known not to work.
- **Device deletion is unconfirmed.** The Java client's `deleteDevice` issues a GET, which looks like a bug. `DELETE /devices/{id}` is the obvious guess, but neither other client implements it.
- **Room on/off path differs between clients.** The TypeScript client sends a room's `isOn` to `/devices/set/{id}` but other room attributes to `/devices/room/{id}`. The Java client uses `/devices/room/{id}` for both, which is the safer choice.
- **The Thread border router API is gone.** Firmware 2.805.7 (July 2025) shut off access to the hub's separate OpenThread Border Router REST API.
- **The API can change without notice.** It is private to IKEA's own app, and any firmware update may alter or remove endpoints.

## Sources

- [lpgera/dirigera (TypeScript client)](https://github.com/lpgera/dirigera)
- [Leggin/dirigera (Python client)](https://github.com/Leggin/dirigera)
- [dvdgeisler/DirigeraClient (Java client)](https://www.github.com/dvdgeisler/DirigeraClient)
- [Matter Alpha: IKEA patched OTBR API on Dirigera](https://www.matteralpha.com/explainer/ikea-patched-openthread-border-router-api-on-dirigera-hub)

[filename](./bottom-copyright.md ':include')
