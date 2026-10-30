# deskmate

A desk display for the CYD: a large NTP-synced clock with seconds, the date, current weather with today's high and low from Open-Meteo, and a 25/5 minute Pomodoro timer driven by touch. Location and UTC offset come from the public IP, so nothing is hard-coded and no API key is needed.

Needs an ESP32-2432S028 and a 2.4 GHz Wi-Fi network with internet access.

## Build and flash

```sh
cd deskmate
pio run -e cyd -t upload --upload-port <SERIAL_PORT>
pio device monitor --port <SERIAL_PORT>      # 115200 baud
```

On first boot the board opens an access point named `CYD-Setup` (WiFiManager). Join it from a phone and pick your network; credentials go to NVS and are reused. The portal times out after 180 s and the firmware carries on without network; if Wi-Fi comes up later, the 30 s geolocation retry in `loop()` starts the clock and weather.

## Usage

| Touch | Effect |
|---|---|
| Tap | idle: start a 25 min focus block. Focus: pause. Paused: resume. Break: back to idle. |
| Hold > 1.2 s | reset to idle |

The RGB LED shows the state: green idle, red focus, off paused, blue break. When a phase ends the timer card flashes three times and the next phase starts (focus, then break, then idle). The touch panel is only read as pressed / not pressed plus duration, so it needs no calibration.

## How it works

At boot the board asks ip-api.com for lat, lon, city and offset (up to 5 tries), calls `configTime(offset)`, then asks Open-Meteo for the forecast with `timezone=auto`. Open-Meteo returns `utc_offset_seconds` for the coordinates, and that value wins if the two disagree. Weather refreshes every 15 minutes, so a daylight-saving change is picked up within 15 minutes without shipping a POSIX TZ table.

The timer runs on `millis()`, not wall-clock time. A phase stores $t_\text{end} = t_\text{start} + T$, the remaining time is $\max(0, t_\text{end} - t)$, and pausing freezes that value, so NTP corrections never make it jump.

Each text field remembers the last string it drew and is redrawn only when that string changes. The loop runs at 4 Hz, which keeps the seconds smooth with little SPI traffic and no full-screen flicker.

## Limitations

- ip-api.com is plain HTTP (free tier has no HTTPS). With a VPN, city and weather follow the exit node.
- Open-Meteo uses HTTPS with `setInsecure()`: encrypted, certificate not validated.
- Weather codes are reduced to nine labels; TFT_eSPI built-in fonts, ASCII only.
- Focus and break lengths are compile-time constants (`FOCUS_MS`, `BREAK_MS`).

## License

MIT, see [LICENSE](../LICENSE). Libraries (TFT_eSPI, XPT2046_Touchscreen, WiFiManager, ArduinoJson) are downloaded by PlatformIO at build time under their own licenses.
