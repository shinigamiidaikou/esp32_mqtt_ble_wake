// ESP32 MQTT BLE Wake
// ---------------------------------------------------------------------------
// A BLE HID keyboard Emulator that sends a space keypress to wake a sleeping
// host, triggered by a physical button or an MQTT message.
//
// Requires bonding to the host so it can be added to the controller's wake-
// allow list. Once bonded, short-pressing the button or sending "WAKE" to
// topic "jupiter/power" (rename topic as needed) will start fast advertising
// for 2 minutes. If the host connects within that window, the device will
// send a space keypress after a brief delay to allow link-layer encryption
// to complete. Long-pressing the button for 2 seconds clears all bonds and
// restarts the device, allowing it to be re-paired if something goes wrong.
//
// Developed and tested on an "ESP32 DevKitC" Generic variant against a
// CachyOS KDE Linux Distribution, but should work on any ESP32 with minor
// pin adjustments and possibly any OS supporting HID wake-up. Uses the
// NimBLE-Arduino library for BLE and PubSubClient for MQTT.
//
// Note: This code is for demonstration purposes & personal use and may need
// adjustments for production use, such as better error handling, security
// considerations, and support for multiple wake keys or hosts for extra
// features.
//
// Developed with iterative AI assistance (Anthropic Claude Opus 4.7) for the
// BLE HID architecture, NimBLE integration, and documentation. Final code
// review and integration by the author.
//
// ---------------------------------------------------------------------------
// Copyright (C) 2026 Shafin Ahmed
//
// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
// or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program.  If not, see <https://www.gnu.org/licenses/>.
// ===========================================================================

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>

#include "secrets.h"

const char* ssid = SECRET_SSID;
const char* password = SECRET_PASS;
const char* mqtt_server = SECRET_MQTT_SERVER;
const int mqtt_port = 8883;
const char* mqtt_user = SECRET_MQTT_USER;
const char* mqtt_pass = SECRET_MQTT_PASS;

// Button configuration
const int BUTTON_PIN = 14;
const int DEBOUNCE_MS = 50;
const int LONG_PRESS_MS = 2000;
const unsigned long ADV_WINDOW_MS = 120000;

// Time to wait after a connection completes before firing the wake keypress.
// Lets link-layer encryption finish; HID input notifications require it.
const unsigned long WAKE_KEYPRESS_DELAY_MS = 500;

// BLE advertising intervals (units of 0.625ms)
const uint16_t ADV_FAST_MIN = 0x20;
const uint16_t ADV_FAST_MAX = 0x40;

// HID identifiers
const uint16_t APPEARANCE_HID_KEYBOARD = 0x03C1;
const uint8_t  HID_KEY_SPACE = 0x2C;

// Standard boot-protocol keyboard descriptor with Report ID 1.
// 8-byte input report: [modifier][reserved][keycode 1..6]; 1-byte LED output.
static const uint8_t HID_REPORT_MAP[] = {
  0x05, 0x01,        // Usage Page (Generic Desktop)
  0x09, 0x06,        // Usage (Keyboard)
  0xA1, 0x01,        // Collection (Application)
  0x85, 0x01,        //   Report ID (1)
  0x05, 0x07,        //   Usage Page (Key Codes)
  0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
  0x75, 0x01, 0x95, 0x08, 0x81, 0x02,           //   Modifier byte (LCtrl..RGUI)
  0x95, 0x01, 0x75, 0x08, 0x81, 0x01,           //   Reserved byte
  0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
  0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, // 6 keycode slots
  0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05,
  0x91, 0x02,                                     // LED output (5 bits)
  0x95, 0x01, 0x75, 0x03, 0x91, 0x01,             // LED output padding
  0xC0
};

// Button state
unsigned long buttonPressTime = 0;
bool buttonPressed = false;

// BLE runtime state (loop-driven)
bool advertisingActive = false;
unsigned long advertisingUntilMs = 0;
bool bleLinkUp = false;
unsigned long wakeFireAt = 0;  // 0 = idle, else millis() at which to send keypress

// Flags set by NimBLE callbacks; consumed in loop()
volatile bool bleConnectEvent = false;
volatile bool bleDisconnectEvent = false;
// Latched intent: was this advertising/connection cycle requested for wake?
volatile bool wakeIntentPending = false;

WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);

NimBLEServer* bleServer = nullptr;
NimBLEHIDDevice* bleHid = nullptr;
NimBLECharacteristic* bleHidInput = nullptr;

class WakerServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo&) override {
    bleConnectEvent = true;
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
    bleDisconnectEvent = true;
  }
};

WakerServerCallbacks bleCallbacks;

// ===== BLE control (all driven from loop or loop-consumed callbacks) =====

void start_advertising_window() {
  if (bleLinkUp) {
    Serial.println("-> BLE already connected; host is awake. No-op.");
    return;
  }

  wakeIntentPending = true;

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  if (!advertisingActive) {
    adv->setMinInterval(ADV_FAST_MIN);
    adv->setMaxInterval(ADV_FAST_MAX);
    adv->start();
    advertisingActive = true;
    Serial.println("-> BLE fast advertising started (2-minute window)");
  } else {
    Serial.println("-> BLE advertising window extended");
  }
  advertisingUntilMs = millis() + ADV_WINDOW_MS;
}

void stop_advertising(const char* reason) {
  if (!advertisingActive) return;
  NimBLEDevice::getAdvertising()->stop();
  advertisingActive = false;
  advertisingUntilMs = 0;
  // Window closed without a connection — drop the queued wake intent so a
  // later, unrelated reconnect doesn't inject a stray keypress.
  wakeIntentPending = false;
  Serial.print("-> BLE advertising stopped (");
  Serial.print(reason);
  Serial.println(")");
}

void send_wake_keypress() {
  if (!bleHidInput) return;
  uint8_t press[8]   = {0, 0, HID_KEY_SPACE, 0, 0, 0, 0, 0};
  uint8_t release[8] = {0};
  bleHidInput->setValue(press, sizeof(press));
  bleHidInput->notify();
  delay(20);
  bleHidInput->setValue(release, sizeof(release));
  bleHidInput->notify();
  Serial.println("-> Wake keypress sent (space).");
}

void handle_ble_events() {
  if (bleConnectEvent) {
    bleConnectEvent = false;
    bleLinkUp = true;
    advertisingActive = false;     // NimBLE auto-stops adv on connect
    advertisingUntilMs = 0;
    Serial.println("BLE connected.");
    if (wakeIntentPending) {
      wakeFireAt = millis() + WAKE_KEYPRESS_DELAY_MS;
    }
  }

  if (bleDisconnectEvent) {
    bleDisconnectEvent = false;
    bleLinkUp = false;
    wakeFireAt = 0;
    Serial.println("BLE disconnected. Staying silent until button/WAKE.");
  }

  if (wakeFireAt && bleLinkUp && (long)(millis() - wakeFireAt) >= 0) {
    send_wake_keypress();
    wakeFireAt = 0;
    wakeIntentPending = false;
  }

  if (advertisingActive && advertisingUntilMs &&
      (long)(millis() - advertisingUntilMs) >= 0) {
    stop_advertising("window expired");
  }
}

void clear_bonds_and_reset() {
  Serial.println("\nCLEARING ALL BONDS AND RESTARTING...");
  delay(500);
  NimBLEDevice::deleteAllBonds();
  Serial.println("=> All bonds cleared");
  delay(1000);
  ESP.restart();
}

void print_ble_status() {
  Serial.println("\n--- BLE Status ---");
  Serial.print("Connected?: ");
  Serial.println(bleLinkUp ? "YES (I)" : "NO (X)");
  Serial.print("Advertising?: ");
  if (advertisingActive) {
    long remaining = (long)(advertisingUntilMs - millis());
    Serial.print("ON (");
    Serial.print(remaining > 0 ? remaining / 1000 : 0);
    Serial.println("s left)");
  } else {
    Serial.println("OFF");
  }
  Serial.print("Wake intent queued?: ");
  Serial.println(wakeIntentPending ? "YES" : "NO");
  Serial.println("-------------------\n");
}

// ===== Button =====

void handle_button() {
  static unsigned long lastDebounceTime = 0;
  int buttonState = digitalRead(BUTTON_PIN);

  if (buttonState == LOW) {  // active-low
    if (!buttonPressed && (millis() - lastDebounceTime) > DEBOUNCE_MS) {
      buttonPressed = true;
      buttonPressTime = millis();
      Serial.println("(o) Button pressed...");
    }
  } else {
    if (buttonPressed) {
      unsigned long pressDuration = millis() - buttonPressTime;
      if (pressDuration >= LONG_PRESS_MS) {
        Serial.println("(o) Long press detected!");
        clear_bonds_and_reset();
      } else {
        Serial.println("(o) Short press detected");
        start_advertising_window();
        print_ble_status();
      }
      buttonPressed = false;
      lastDebounceTime = millis();
    }
  }
}

// ===== Wi-Fi / MQTT =====

void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  int timeout = 40; // 20 seconds
  while (WiFi.status() != WL_CONNECTED && timeout > 0) {
    delay(500);
    Serial.print(".");
    timeout--;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi connection failed. Restarting...");
    ESP.restart();
  }

  Serial.println("\nWiFi connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

void mqtt_callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");

  String message;
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.println(message);

  if (String(topic) == "jupiter/power" && message == "WAKE") {
    Serial.println("WAKE request received.");
    start_advertising_window();
  }
}

void mqtt_reconnect() {
  int attempts = 0;
  const int maxAttempts = 10;

  while (!mqttClient.connected()) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi lost during MQTT reconnect. Reconnecting WiFi...");
      setup_wifi();
    }

    Serial.print("Attempting MQTT connection... ");
    String clientId = "ESP32Client-";
    clientId += String(random(0xffff), HEX);

    if (mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
      Serial.println("connected");
      attempts = 0;
      mqttClient.subscribe("jupiter/power");
    } else {
      attempts++;
      Serial.print("failed, rc=");
      Serial.print(mqttClient.state());
      Serial.print(" (attempt ");
      Serial.print(attempts);
      Serial.print("/");
      Serial.print(maxAttempts);
      Serial.println(")");

      if (attempts >= maxAttempts) {
        Serial.println("Max MQTT reconnect attempts reached. Restarting...");
        ESP.restart();
      }
      delay(5000);
    }
  }
}

// ===== Setup / Loop =====

void setup() {
  Serial.begin(115200);
  delay(1000); // Give serial time to initialize

  Serial.println("\n\n=== ESP32 MQTT BLE Wake - Starting ===");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Serial.println("Button initialized on GPIO " + String(BUTTON_PIN));

  setup_wifi();
  espClient.setInsecure();
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqtt_callback);
  Serial.println("Wifi & MQTT setup complete.");

  Serial.println("Initializing BLE HID (idle, advertising OFF)...");
  NimBLEDevice::init("ESP32_Waker");
  // (bonding, MITM, secure-connections). Bonding ON so the host registers us
  // on its BT controller's wake-allow list. No MITM since this device has no
  // PIN entry. SC enabled where the host supports it.
  NimBLEDevice::setSecurityAuth(true, false, true);
  // No display, no input — Just Works pairing.
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEDevice::setSecurityInitKey(BLE_HS_KEY_DIST_ENC_KEY | BLE_HS_KEY_DIST_ID_KEY);
  NimBLEDevice::setSecurityRespKey(BLE_HS_KEY_DIST_ENC_KEY | BLE_HS_KEY_DIST_ID_KEY);

  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(&bleCallbacks);
  // Never auto-resume advertising after a disconnect.
  bleServer->advertiseOnDisconnect(false);

  // Build the HID-over-GATT keyboard.
  bleHid = new NimBLEHIDDevice(bleServer);
  bleHidInput = bleHid->getInputReport(1);
  bleHid->setManufacturer("Espressif");
  bleHid->setPnp(0x02, 0xE502, 0xA111, 0x0210);  // sig=USB, vid, pid, version
  bleHid->setHidInfo(0x00, 0x01);                // country=none, flags=remote-wake
  bleHid->setBatteryLevel(100);
  bleHid->setReportMap((uint8_t*)HID_REPORT_MAP, sizeof(HID_REPORT_MAP));
  bleHid->startServices();

  // Tell scanners we're a keyboard so the host treats us as a wake-capable HID.
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setAppearance(APPEARANCE_HID_KEYBOARD);
  adv->addServiceUUID(bleHid->getHidService()->getUUID());

  Serial.println("Device name: ESP32_Waker (HID Keyboard)");
  Serial.println("Short-press GPIO " + String(BUTTON_PIN) + ": fast advertising for 2 minutes");
  Serial.println("Long-press 2s: clear bonds + restart");
  Serial.println("MQTT 'jupiter/power' = WAKE: start advertising window");

  print_ble_status();
}

void loop() {
  handle_button();
  handle_ble_events();

  if (!mqttClient.connected()) {
    mqtt_reconnect();
  }
  mqttClient.loop();
}