# Espisstant

An open-source, **local-first voice assistant** for the ESP32 that replaces a cloud
assistant for basic smart-home control. No cloud, no accounts, no telemetry —
everything runs on the chip and your LAN.

What it controls:

- **Philips Hue** bulbs through your Hue Bridge (local REST API, works with the
  old round v1 bridge as well as the square v2 one).
- **Switcher** devices over their local TCP protocol: the water heater / smart
  heater line (Touch / V2 / V4 / Mini), the **Power Plug**, and the **Breeze**
  (IR air-conditioner bridge).
- All of it by **offline voice commands** ("Hi ESP … turn on the lights"),
  a built-in **web UI**, or plain HTTP calls you can script.

```
        ┌────────────────────────────── LAN ──────────────────────────────┐
        │                                                                 │
 ┌──────┴──────┐   HTTP (v1 REST)   ┌────────────┐   Zigbee   ┌─────────┐ │
 │   ESP32-S3  │ ─────────────────► │ Hue Bridge │ ─────────► │  Bulbs  │ │
 │ "Espisstant"│                    └────────────┘            └─────────┘ │
 │  I2S mic 🎤 │   TCP :9957 / :10002 (signed packets)   ┌──────────────┐ │
 │  web UI 🌐  │ ───────────────────────────────────────►│ Switcher     │ │
 └─────────────┘                                         │ Heater/Plug/ │ │
                                                         │ Breeze       │ │
                                                         └──────────────┘ │
        └─────────────────────────────────────────────────────────────────┘
```

## Hardware

| Part | Why |
|---|---|
| **ESP32-S3** dev board, ≥8 MB flash + 8 MB PSRAM (e.g. ESP32-S3-DevKitC-1 N8R8) | Offline wake-word + speech-command models (ESP-SR) need the S3's vector instructions and PSRAM |
| **INMP441** I2S MEMS microphone (~$2) | Voice input |
| optional: plain **ESP32** (WROOM/WROVER) | Everything works except voice — web UI/HTTP control only |

Default mic wiring (changeable in `menuconfig` → *Espisstant configuration*):

| INMP441 | ESP32-S3 |
|---|---|
| VDD | 3V3 |
| GND, L/R | GND |
| SCK | GPIO 4 |
| WS  | GPIO 5 |
| SD  | GPIO 6 |

## Build & flash

Requires [ESP-IDF v5.5](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/get-started/index.html)
(the ESP-SR/esp-dl dependency needs IDF ≥ 5.4; the no-voice ESP32 build also
works on 5.3).

```sh
. $IDF_PATH/export.sh

idf.py set-target esp32s3        # or: esp32 (no voice)
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

The ESP-SR models for the S3 build are pulled automatically by the IDF
component manager on the first build.

## First-time setup

1. **Wi-Fi** — on first boot the device opens an access point
   `Espisstant-Setup` (password `espisstant`). Connect and browse to
   `http://192.168.4.1`, enter your Wi-Fi credentials. It reboots and joins
   your LAN; from then on the web UI is at `http://espisstant.local`.
2. **Hue** — in the web UI, enter your bridge IP (or let discovery find it),
   press the physical **link button** on the bridge, then click *Pair* within
   30 s. The app key is stored in flash.
3. **Switcher** — nothing to pair. Switcher devices broadcast their presence
   on UDP :20002/:20003; the web UI lists everything it hears and
   auto-assigns them to roles by model (plug → Plug, Breeze → AC, other →
   Heater). Assignments persist; to override one, `POST /api/settings` with
   e.g. `{"heater":{"ip":"...","id":"...","key":".."}}`. The heater/plug lines are controlled directly; see
   below for the Breeze.
4. **Breeze (AC)** — the Breeze transmits IR using a *remote set* matching
   your AC. Find your remote ID in the web UI (the Breeze reports it, e.g.
   `ELEC7001`), then:

   ```sh
   python tools/fetch_breeze_remote.py ELEC7001   # writes spiffs_data/breeze_remote.json
   idf.py build flash                             # re-flashes the storage partition
   ```

## Voice commands (ESP32-S3 only)

Wake word **"Hi ESP"**, then within ~6 s one of:

| Phrase | Action |
|---|---|
| "turn on/off the lights" | Hue group on/off |
| "dim the lights" / "brighten the lights" | Hue brightness −/+ |
| "turn on/off the heater" | Switcher heater |
| "turn on/off the plug" | Switcher power plug |
| "turn on/off the air conditioner" | Switcher Breeze |
| "make it warmer/cooler" | Breeze target temp ±1 °C |

The phrase list lives in `components/voice/include/voice_commands.h` — edit,
rebuild, done. Recognition is fully offline (Espressif WakeNet + MultiNet).

## HTTP API

Everything the voice/web UI can do is one `GET`/`POST` away — handy for
scripts, Home Assistant `rest_command`, cron on a server, etc.

```
GET  /api/status                     → JSON: devices, states
POST /api/intent  {"intent":"lights_on"}
POST /api/hue/pair                   (press link button first)
POST /api/settings {...}             see webui.c for fields
```

All POSTs require `Content-Type: application/json`. Optionally set an API
token in the web UI; every control request must then carry it:

```sh
curl -X POST http://espisstant.local/api/intent \
  -H 'Content-Type: application/json' -H 'X-Api-Key: <token>' \
  -d '{"intent":"lights_on"}'
```

Intents: `lights_on lights_off lights_dim lights_bright heater_on heater_off
plug_on plug_off ac_on ac_off ac_warmer ac_cooler`.

## Repository layout

```
main/                  app wiring: Wi-Fi manager, settings, intents, web UI
components/hue/        Hue Bridge local API client
components/switcher/   Switcher local protocol (see below) + UDP discovery
components/voice/      ESP-SR wake word + command recognition (S3 only)
tools/                 host-side helpers + protocol unit tests
spiffs_data/           files flashed to the storage partition (Breeze IR set)
```

## The Switcher protocol

Switcher devices speak a proprietary-but-reverse-engineered hex protocol on
TCP :9957 (heater/plug, "type 1") and :10002 (Breeze, "type 2"), with packets
signed by a double CRC-16/CCITT pass. The implementation in
`components/switcher/switcher_proto.c` is a C port of the excellent
[aioswitcher](https://github.com/TomerFi/aioswitcher) project (Apache-2.0) and
is verified byte-for-byte against it by `tools/test_switcher_proto.py`.

Run the protocol tests on your host (no hardware needed):

```sh
python tools/test_switcher_proto.py
```

Note: the newest token-based Switcher devices (e.g. "Switcher Heater" model
`031f`, Runner S11/S12) need a per-account token from Switcher and are not
supported yet.

## License

MIT © razmagal. Switcher protocol knowledge derived from
[aioswitcher](https://github.com/TomerFi/aioswitcher) (Apache-2.0).
