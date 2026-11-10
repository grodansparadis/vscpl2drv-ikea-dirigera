# Test and use the driver

The build produces `testapp` (installed as `vscpl2drv-ikea-dirigera-test`),
an interactive tool that loads the driver shared library exactly the way the
VSCP daemon does — through the exported `VSCPOpen`/`VSCPClose`/`VSCPRead`/
`VSCPWrite` interface.

## Usage

```sh
testapp <driver.so> <config.json> [guid]       # interactive driver session
testapp --get-token <hub-ip> [port]            # one-time hub pairing
testapp --get-home <config.json>               # dump complete home JSON
```

### Pairing

Runs the OAuth2 PKCE pairing flow against the hub: press the action button
on the bottom of the hub when prompted, then press ENTER, and the access
token to put in the configuration file is printed.

### Home dump (`--get-home`)

Fetches `GET /v1/home` with the hub/token from the configuration file and
prints the complete home state as pretty formatted JSON (pipe to `jq` or a
file as needed).

## Interactive session

The session prints every event the driver generates, with decoded
measurement values, for example:

```
RX class=10 (CLASS1.MEASUREMENT) type=6 (Temperature) index=1
   guid=FF:...:01 data=[0xA8,0x41,0xAC,0x00,0x00] value=21.5 unit=1
```

Commands on stdin:

| Command | Action |
| ------- | ------ |
| `on <subzone> [zone]` | Send CLASS1.CONTROL TurnOn (255 = all devices) |
| `off <subzone> [zone]` | Send CLASS1.CONTROL TurnOff |
| `dim <subzone> <0-100> [zone]` | Send CLASS1.CONTROL Dim lamp(s) |
| `devices` | List the devices on the hub (id, type, name, reachability, attributes) — useful for finding device ids for the config |
| `home` | Print the complete home state JSON |
| `help` | Show help |
| `quit` | Close the driver and exit |

If `zone` is omitted it defaults to 255 (match any zone).

[filename](./bottom-copyright.md ':include')
