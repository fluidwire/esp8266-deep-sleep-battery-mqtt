# ESP8266-DEEP-SLEEP-BATTERY-MQTT — wake, publish one reading plus battery voltage, sleep again

IoT example by [Fluidwire](https://fluidwire.com/).

Squeeze weeks out of an 18650 on the cheapest WiFi board. The ESP8266 wakes from deep sleep, reconnects to WiFi in about a second, publishes one MQTT message, and goes straight back to sleep.

## What it does

- Wakes every `SLEEP_SECONDS` (default 300 s) via the GPIO16 → RST wiring
- Reconnects fast using a **static IP** and the **BSSID + channel saved in RTC memory** (no DHCP, no channel scan)
- Publishes `{"boot":12,"temp_c":0.0,"batt_v":3.97,"awake_ms":1840}` to your broker, then deep-sleeps
- Keeps a boot counter in RTC memory and reports awake time so you can tune battery life
- Falls back to a full scan if the router changes channel, and gives up after bounded timeouts so a dead network cannot drain the cell

`readSensor()` returns a placeholder so it runs with no extra hardware. Replace it with your real sensor.

## Required hardware

- 1x ESP8266 board (NodeMCU or Wemos D1 mini)
- 1x 18650 Li-ion cell + a suitable holder/regulator path (3.3 V LDO or a board that accepts 3.7 V on the 3V3/5V pin as designed)
- 1x jumper wire from **D0 (GPIO16) to RST**
- Voltage divider from the battery to **A0** (see Setup)
- An MQTT broker (public test broker is fine to start)

## Required libraries

- **PubSubClient** by Nick O'Leary (Library Manager)
- `ESP8266WiFi` ships with the **ESP8266 Arduino core** (Boards Manager → `esp8266`)

## Setup

1. Copy `config.h.example` to `config.h` and fill in WiFi, a free static IP, MQTT broker and topic.
2. **Battery divider:** the bare ESP8266 ADC only reads 0–1.0 V. NodeMCU and D1 mini boards already have a 220k/100k divider on A0 (0–3.2 V). To read up to 4.2 V, add an external resistor from the battery to A0 (e.g. 100k) and set `BATT_DIVIDER_RATIO` = (total series R + 100k) / 100k. Calibrate against a multimeter.
3. Flash the sketch **with D0 not connected to RST** (the wire can block uploads on some boards).
4. Connect **D0 → RST**, power from the battery, and open the Serial Monitor at **115200** to watch the first cycle.

## Expected output

```
Boot #1
RTC memory empty (first boot / power loss)
WiFi OK ip=192.168.1.150 ch=6
MQTT pub fluidwire/demo/sleeper -> {"boot":1,"temp_c":0.0,"batt_v":3.97,"awake_ms":2310} (ok)
Awake 2312 ms. Sleeping 300 s...
```

Later wakes are faster (often under 1.5 s awake) thanks to the saved channel/BSSID.

## Troubleshooting

- **Board never wakes** — D0 must be wired to RST. Some boards need the wire on the `RST` pin, not `EN`.
- **Upload fails with the wire attached** — disconnect D0–RST while flashing, reconnect after.
- **`batt_v` is wrong** — your divider ratio differs from `BATT_DIVIDER_RATIO`. Measure the cell with a multimeter and adjust.
- **Fast reconnect never helps** — mesh/multi-AP networks change BSSID; the sketch retries with a full scan automatically, but awake time rises.
- **Awake time is long** — use an IP address for `MQTT_HOST` (no DNS lookup) and keep `USE_STATIC_IP` on.
- **Battery drains in days** — on dev boards the USB-serial chip and LDO draw 5–20 mA even in sleep; use a bare module or a low-quiescent board for real battery life.

## Security note

Don't commit `config.h`; the `.gitignore` excludes it. This example uses plain MQTT (port 1883) with no TLS, which is fine for testing on a trusted network. Use authentication and a TLS-enabled broker for anything real. Li-ion cells need protection circuitry; never charge or short them carelessly.

## License

MIT — see [LICENSE](LICENSE).

## Got stuck?

Drop by [fluidwire.com](https://fluidwire.com/) — we ship IoT building blocks and are happy to help.
