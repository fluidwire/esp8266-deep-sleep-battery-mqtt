/*
 * ESP8266 Deep-Sleep Battery Sensor Node (MQTT)
 * ---------------------------------------------
 * Wakes, connects to WiFi as fast as possible, publishes ONE MQTT message
 * with a reading + battery voltage + boot counter + awake time, then goes
 * back to deep sleep. Designed to run for weeks on a single 18650 cell.
 *
 * Fast-reconnect tricks used:
 *   - Static IP (no DHCP round trip)
 *   - Saved BSSID + channel in RTC memory (no channel scan)
 *   - Short, bounded timeouts so a dead network can't drain the battery
 *
 * Required libraries (install via Library Manager):
 *   - "PubSubClient" by Nick O'Leary
 *   ESP8266WiFi ships with the ESP8266 Arduino core (Boards Manager).
 *
 * Board: ESP8266 (NodeMCU / Wemos D1 mini), ESP8266 core installed.
 *
 * Wiring (REQUIRED for wake-up):
 *   GPIO16 (D0) -> RST      (the RTC timer pulls D0 low to wake the chip)
 *   Battery + -> divider -> A0 (A0 accepts 0..1.0 V max on a bare chip!)
 *   Remove/disconnect the wire from D0 to RST while flashing if upload fails.
 *
 * Setup:
 *   1. Copy config.h.example to config.h
 *   2. Edit config.h: WiFi, static IP, MQTT broker, sleep time, divider ratio
 *   3. Flash, then wire D0 -> RST. Serial Monitor at 115200.
 *
 * Sample MQTT message:
 *   {"boot":12,"temp_c":0.0,"batt_v":3.97,"awake_ms":1840}
 *
 * (c) 2026 Fluidwire. Released under the MIT License.
 * https://github.com/fluidwire
 */

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include "config.h"

const uint32_t WIFI_TIMEOUT_MS = 10000;
const uint32_t MQTT_TIMEOUT_MS = 5000;
const uint32_t RTC_MAGIC       = 0xF1D3A5E1;
const uint32_t RTC_OFFSET      = 0;   // user RTC memory block index (4-byte words)

struct RtcState {
  uint32_t magic;
  uint32_t bootCount;
  uint8_t  channel;
  uint8_t  bssid[6];
  uint8_t  valid;       // 1 = channel/bssid saved
  uint8_t  pad;
  uint32_t crc;
};

RtcState     rtc;
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);
uint32_t     wakeMs;

uint32_t calcCrc(const RtcState &s) {
  // Simple additive checksum over the struct minus the crc field.
  const uint8_t *p = (const uint8_t *)&s;
  uint32_t sum = 0x1234;
  for (size_t i = 0; i < sizeof(RtcState) - sizeof(uint32_t); i++) sum = sum * 31 + p[i];
  return sum;
}

bool loadRtc() {
  if (!ESP.rtcUserMemoryRead(RTC_OFFSET, (uint32_t *)&rtc, sizeof(rtc))) return false;
  return rtc.magic == RTC_MAGIC && rtc.crc == calcCrc(rtc);
}

void saveRtc() {
  rtc.magic = RTC_MAGIC;
  rtc.crc   = calcCrc(rtc);
  ESP.rtcUserMemoryWrite(RTC_OFFSET, (uint32_t *)&rtc, sizeof(rtc));
}

float readBatteryVolts() {
  // Average a few samples; ESP8266 ADC is noisy. 10-bit, 0..1.0 V full scale.
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) { sum += analogRead(A0); delay(2); }
  float v = (sum / 8.0f) / 1023.0f;   // volts at the A0 pin
  return v * BATT_DIVIDER_RATIO;
}

// Replace with your real sensor. Returns a placeholder here so the sketch
// runs with zero extra hardware.
float readSensor() {
  return 0.0f;
}

bool connectWiFi(bool useSaved) {
  WiFi.persistent(false);   // don't wear out flash on every wake
  WiFi.mode(WIFI_STA);
#if USE_STATIC_IP
  WiFi.config(IPAddress(STATIC_IP), IPAddress(STATIC_GATEWAY),
              IPAddress(STATIC_SUBNET), IPAddress(STATIC_DNS));
#endif
  if (useSaved) WiFi.begin(WIFI_SSID, WIFI_PASSWORD, rtc.channel, rtc.bssid, true);
  else          WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_TIMEOUT_MS) return false;
    delay(10);
  }
  return true;
}

bool publishReading() {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  uint32_t start = millis();
  bool ok = false;
  while (!mqtt.connected() && millis() - start < MQTT_TIMEOUT_MS) {
    ok = strlen(MQTT_USER) > 0
      ? mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)
      : mqtt.connect(MQTT_CLIENT_ID);
    if (!ok) delay(200);
  }
  if (!mqtt.connected()) return false;

  char payload[128];
  snprintf(payload, sizeof(payload),
           "{\"boot\":%lu,\"temp_c\":%.1f,\"batt_v\":%.2f,\"awake_ms\":%lu}",
           (unsigned long)rtc.bootCount, readSensor(), readBatteryVolts(),
           (unsigned long)(millis() - wakeMs));
  ok = mqtt.publish(MQTT_TOPIC, payload);
  mqtt.loop();
  delay(50);          // let the TCP buffer flush before we cut power
  mqtt.disconnect();
  Serial.printf("MQTT pub %s -> %s (%s)\n", MQTT_TOPIC, payload, ok ? "ok" : "FAIL");
  return ok;
}

void goToSleep() {
  Serial.printf("Awake %lu ms. Sleeping %d s...\n",
                (unsigned long)(millis() - wakeMs), SLEEP_SECONDS);
  Serial.flush();
  WiFi.disconnect(true);
  delay(1);
  ESP.deepSleep((uint64_t)SLEEP_SECONDS * 1000000ULL, WAKE_RF_DEFAULT);
  delay(100);         // deepSleep returns before the chip actually sleeps
}

void setup() {
  wakeMs = millis();
  Serial.begin(115200);
  Serial.println();

  bool haveRtc = loadRtc();
  if (!haveRtc) {
    memset(&rtc, 0, sizeof(rtc));
    Serial.println("RTC memory empty (first boot / power loss)");
  }
  rtc.bootCount++;
  Serial.printf("Boot #%lu\n", (unsigned long)rtc.bootCount);

  bool ok = connectWiFi(haveRtc && rtc.valid);
  if (!ok && haveRtc && rtc.valid) {
    // Saved channel/BSSID went stale (router changed channel): retry with a scan.
    Serial.println("Fast reconnect failed, retrying with full scan");
    WiFi.disconnect();
    ok = connectWiFi(false);
  }

  if (ok) {
    // Save the AP details for next time's fast reconnect.
    rtc.channel = WiFi.channel();
    memcpy(rtc.bssid, WiFi.BSSID(), 6);
    rtc.valid = 1;
    Serial.printf("WiFi OK ip=%s ch=%d\n", WiFi.localIP().toString().c_str(), rtc.channel);
    publishReading();
  } else {
    rtc.valid = 0;
    Serial.println("WiFi failed, skipping publish");
  }

  saveRtc();
  goToSleep();
}

void loop() {
  // Never reached: the chip resets on every wake.
}
