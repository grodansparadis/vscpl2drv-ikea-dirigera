# IKEA DIRIGERA hub: curl examples

These examples follow the endpoint list in `dirigera-rest-api.md`. The API is unofficial and the commands are untested against a real hub.

`-k` skips certificate verification, since the hub's certificate is signed by IKEA's own CA rather than a public one. The examples need `curl`, `openssl` and `jq`.

## 1. Pair and get a token (once)

```bash
HUB=192.168.1.50   # your hub's IP

# PKCE verifier and challenge
VERIFIER=$(openssl rand -base64 96 | tr -d '\n=' | tr '+/' '-_')
CHALLENGE=$(printf '%s' "$VERIFIER" | openssl dgst -sha256 -binary \
  | openssl base64 | tr -d '\n=' | tr '+/' '-_')

# Request an authorization code
CODE=$(curl -sk -G "https://$HUB:8443/v1/oauth/authorize" \
  --data-urlencode "audience=homesmart.local" \
  --data-urlencode "response_type=code" \
  --data-urlencode "code_challenge=$CHALLENGE" \
  --data-urlencode "code_challenge_method=S256" | jq -r .code)

# Now press the action button on the bottom of the hub, then:
TOKEN=$(curl -sk -X POST "https://$HUB:8443/v1/oauth/token" \
  --data-urlencode "code=$CODE" \
  --data-urlencode "name=$(hostname)" \
  --data-urlencode "grant_type=authorization_code" \
  --data-urlencode "code_verifier=$VERIFIER" | jq -r .access_token)

echo "$TOKEN"   # save this; it is reused for every later call
```

The token exchange must happen shortly after the button press (believed to be within about a minute).

## 2. Read state

```bash
AUTH="Authorization: Bearer $TOKEN"
API="https://$HUB:8443/v1"

# Everything in one dump
curl -sk -H "$AUTH" "$API/home" | jq .

# Hub status
curl -sk -H "$AUTH" "$API/hub/status" | jq .

# Device IDs, types and names
curl -sk -H "$AUTH" "$API/devices" \
  | jq -r '.[] | [.id, .deviceType, .attributes.customName] | @tsv'

# One device
curl -sk -H "$AUTH" "$API/devices/<device-id>" | jq .attributes

# Rooms
curl -sk -H "$AUTH" "$API/rooms" | jq -r '.[] | [.id, .name] | @tsv'
```

## 3. Control devices

The PATCH body is always a JSON array.

```bash
ID=<device-id>

# Light or outlet on
curl -sk -X PATCH "$API/devices/$ID" -H "$AUTH" \
  -H "Content-Type: application/json" \
  -d '[{"attributes": {"isOn": true}}]'

# Dim to 40% over 2 seconds
curl -sk -X PATCH "$API/devices/$ID" -H "$AUTH" \
  -H "Content-Type: application/json" \
  -d '[{"attributes": {"lightLevel": 40}, "transitionTime": 2000}]'

# Colour temperature (Kelvin)
curl -sk -X PATCH "$API/devices/$ID" -H "$AUTH" \
  -H "Content-Type: application/json" \
  -d '[{"attributes": {"colorTemperature": 2700}}]'

# Colour (hue and saturation go together)
curl -sk -X PATCH "$API/devices/$ID" -H "$AUTH" \
  -H "Content-Type: application/json" \
  -d '[{"attributes": {"colorHue": 120, "colorSaturation": 0.8}}]'

# Blinds to 50%
curl -sk -X PATCH "$API/devices/$ID" -H "$AUTH" \
  -H "Content-Type: application/json" \
  -d '[{"attributes": {"blindsTargetLevel": 50}}]'

# Rename a device
curl -sk -X PATCH "$API/devices/$ID" -H "$AUTH" \
  -H "Content-Type: application/json" \
  -d '[{"attributes": {"customName": "Desk lamp"}}]'

# All lights in a room off
curl -sk -X PATCH "$API/devices/room/<room-id>?deviceType=light" -H "$AUTH" \
  -H "Content-Type: application/json" \
  -d '[{"attributes": {"isOn": false}}]'

# Blink a device for 30 seconds to find it
curl -sk -X PUT "$API/devices/$ID/identify" -H "$AUTH" \
  -H "Content-Type: application/json" -d '{"period": 30}'
```

## 4. Scenes

```bash
# List scenes
curl -sk -H "$AUTH" "$API/scenes" | jq -r '.[] | [.id, .info.name] | @tsv'

# Run a scene
curl -sk -X POST -H "$AUTH" "$API/scenes/<scene-id>/trigger"

# Undo it
curl -sk -X POST -H "$AUTH" "$API/scenes/<scene-id>/undo"
```

## 5. Events

curl is a poor fit for the WebSocket stream; `websocat` handles it:

```bash
websocat -k -H "$AUTH" "wss://$HUB:8443/v1" | jq .
```

## Troubleshooting

Add `-i` to a call to see the HTTP status.

| Status | Likely cause |
|---|---|
| 401 | Bad or revoked token. |
| 400 | The body is not an array, or the device does not support that attribute. |
| 404 | Wrong device, room or scene ID. |
