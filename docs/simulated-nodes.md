# Simulated VSCP nodes

Every hub device is simulated as a VSCP level I node. Status and
functionality are exposed through registers; behaviour can be programmed
through a per-device decision matrix. The node is addressed by its
**nickname**, which equals the device index (discovery order, also shown in
the driver log and the `devices` table of the driver database).

## Register access

The driver acts on CLASS1.PROTOCOL events — also when wrapped as Class2
Level I (class 512), where the first 16 data bytes carry the destination
GUID:

| Event | Type | Data | Response |
| ----- | ---- | ---- | -------- |
| Read register | 9 | `nickname, register` | Read/Write response (10): `register, value` |
| Write register | 11 | `nickname, register, value` | Read/Write response (10): `register, value-after-write` |
| Extended page read | 37 | `nickname, pageMSB, pageLSB, offset[, count]` (`count` omitted = 1, `0` = 256) | One or more Extended page responses (39): `seq, pageMSB, pageLSB, offset, up to 4 values` |
| Extended page write | 38 | `nickname, pageMSB, pageLSB, offset, 1-4 values` | Extended page response (39) with the values after write |

Classic read/write (9/11) use the page currently selected through the
standard page select registers (0x92/0x93).

## Page 0 — status and control registers

| Register | Access | Content |
| -------- | ------ | ------- |
| 0x00 | rw | `isOn` 0/1 — **writing switches the device** |
| 0x01 | rw | `lightLevel` 0-100 — **writing dims the device** (0 turns it off) |
| 0x02 | r | `isDetected` 0/1 (motion sensors) |
| 0x03 | r | `isOpen` 0/1 (open/close sensors) |
| 0x04-0x05 | r | temperature in centidegrees Celsius, int16 MSB first (21.5 °C = 2150 = `0x08 0x66`) |
| 0x06 | r | relative humidity % |
| 0x07-0x08 | r | PM2.5 µg/m³, uint16 MSB first |
| 0x09-0x0A | r | illuminance lux, uint16 MSB first |
| 0x0B | r | battery % |
| 0x10 | r | VSCP zone |
| 0x11 | r | VSCP subzone |
| 0x12 | r | sensor index |
| 0x13 | r | reachable 0/1 |

Registers that do not apply to a device type read as 0.

## Page 1 — decision matrix

Four rows of eight bytes at offset 0 (0x00-0x1F). All bytes are writable and
the matrix is **persisted in the driver database** and restored when the
driver starts.

Row layout:

| Byte | Content |
| ---- | ------- |
| 0 | oaddr (unused, reserved) |
| 1 | flags |
| 2 | class mask |
| 3 | class filter |
| 4 | type mask |
| 5 | type filter |
| 6 | action |
| 7 | action parameter |

Flags:

| Bit | Meaning |
| --- | ------- |
| 0x80 | row enabled |
| 0x10 | require event zone (data byte 1) to match the device zone (255 matches) |
| 0x08 | require event subzone (data byte 2) to match the device subzone (255 matches) |

Class/type match: `(event ^ filter) & mask == 0` on the low 8 bits (events
with class or type above 255 never match).

Actions:

| Action | Parameter | Effect |
| ------ | --------- | ------ |
| 0 | — | no operation |
| 1 | — | turn device on |
| 2 | — | turn device off |
| 3 | 0-100 | dim to parameter % (0 turns off) |

Example — dim a lamp to 50 % whenever CLASS1.INFORMATION ON (20/3) with the
lamp's zone/subzone is seen. Program row 0 with two extended page writes
(node nickname 2):

```
type 38, data: 02 00 01 00  00 98 FF 14    ; oaddr 0, flags enable+zone+subzone, class mask FF, filter 20
type 38, data: 02 00 01 04  FF 03 03 32    ; type mask FF, filter 3, action 3 (dim), parameter 50
```

## Standard registers (0x80-0xFF, every page)

| Register | Access | Content |
| -------- | ------ | ------- |
| 0x80 | r | alarm status (always 0) |
| 0x81/0x82 | r | VSCP specification version major/minor |
| 0x83 | r | error counter (always 0) |
| 0x84-0x88 | rw | user id — **persisted in the driver database** |
| 0x91 | r | nickname (= device index) |
| 0x92/0x93 | rw | page select MSB/LSB — **persisted in the driver database** |
| 0x94-0x96 | r | firmware version (driver version) |
| 0x97 | r | boot loader algorithm (0xFF = none) |
| 0x98 | r | buffer size (8) |
| 0x99 | r | number of register pages (2) |
| 0xD0-0xDF | r | device GUID, MSB first |
| 0xE0-0xFF | r | MDF URL — reads 0, the MDF is internal (see below) |

All writable registers keep their values between driver runs: the decision
matrix and the standard user id / page select registers are persisted in
the driver database and restored when the device registers on startup,
while the page 0 control registers (`isOn`, `lightLevel`) write through to
the hub which holds their state.

## Internal MDF

No external MDF is published (the MDF URL registers read as zero). Instead
the driver generates an MDF document per device type describing the page 0
registers that apply to the type, the standard registers and the decision
matrix, and stores it in the `mdf` table of the driver database:

```sh
sqlite3 /var/lib/vscp/vscpl2drv-ikea-dirigera.sqlite3 \
  "SELECT xml FROM mdf WHERE device_type='light'"
```

[filename](./bottom-copyright.md ':include')
