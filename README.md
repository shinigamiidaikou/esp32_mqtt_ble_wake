# ESP32 MQTT BLE HID Waker

ESP32 firmware that impersonates a BLE HID keyboard so it can wake a sleeping host on demand — triggered by a physical button or by an MQTT `WAKE` command. Designed as a workaround for machines that can't do **wake-on-LAN**: Wi-Fi-only hosts where WoL is unreliable, devices with WoL disabled in BIOS/firmware, or any host you only reach over a VPN or remote-access tunnel where WoL's layer-2 magic packets can't traverse. Works with any host that supports Bluetooth HID wake.

## How it works

1. The device boots silent — Wi-Fi + MQTT come up, BLE stack initializes, no advertising.
2. A short button press or an MQTT message of `WAKE` on the configured topic acts as a wake trigger. What happens next depends on the BLE link state:
   - **If the host is currently connected**, the firmware sends a single space keypress over BLE HID immediately — handy for waking the screen / dismissing a lockscreen while the link is up.
   - **If the host is disconnected** (typically because it's asleep), the firmware kicks off a 2-minute fast-advertising window and latches a "wake intent" flag.
3. If the host connects during that advertising window, the firmware waits ~500 ms for link-layer encryption to settle and then sends the space keypress. That input is what actually wakes the host (most BT controllers wake the CPU on a connection from a bonded HID, the keypress confirms it across chipsets).
4. On disconnect, the radio goes silent again — no auto re-advertising. This avoids unintended wakes from bonded hosts that might re-establish a link on their own.
5. A long press (2 s) wipes all BLE bonds and reboots, which is how you recover from a pairing mismatch.

## Hardware

- ESP32 dev board (developed/tested on a generic ESP32 DevKitC).
- One momentary push-button to GND on `GPIO 14` (configurable via `BUTTON_PIN`). The pin uses `INPUT_PULLUP`, so wire button between the pin and GND, no external resistor needed.
- USB power or any stable 3.3 V supply.

## Dependencies

All dependencies are installed via the Arduino IDE Library Manager or `arduino-cli`. Their licenses are GPLv3-compatible (see [Licensing of dependencies](#licensing-of-dependencies)).

| Library | Tested with | License | Source |
| --- | --- | --- | --- |
| arduino-esp32 core (Espressif) | 3.x | LGPL-2.1 (with underlying ESP-IDF under Apache-2.0) | https://github.com/espressif/arduino-esp32 |
| NimBLE-Arduino (h2zero) | 1.4.x and 2.x both work | Apache-2.0 | https://github.com/h2zero/NimBLE-Arduino |
| PubSubClient (knolleary) | 2.8+ | MIT | https://github.com/knolleary/pubsubclient |

`WiFi`, `WiFiClientSecure`, `NimBLEDevice`, `NimBLEHIDDevice` are headers from those packages — no separate install.

## Setup

1. **Clone** this repository.
2. **Create `secrets.h`** ***(NEVER COMMIT THIS FILE)*** alongside the `.ino` with the following content, filling in your Wi-Fi and MQTT credentials:
   ```cpp
   #define SECRET_SSID        "your-wifi-ssid"
   #define SECRET_PASS        "your-wifi-password"
   #define SECRET_MQTT_SERVER "broker.example.com"
   #define SECRET_MQTT_USER   "mqtt-username"
   #define SECRET_MQTT_PASS   "mqtt-password"
   ```
   The MQTT port is hardcoded to `8883` (TLS) in the sketch — change it there if your broker uses a different port. `WiFiClientSecure::setInsecure()` is used, so no CA cert is required; if you want certificate validation, replace that call with `setCACert(...)`.
3. **Install the libraries** listed in [Dependencies](#dependencies) via the Arduino IDE Library Manager.
4. **Flash** the sketch to your ESP32.
5. **Pair from the host once.** The device advertises as `ESP32_Waker` (HID Keyboard appearance). Pair using your OS's Bluetooth settings while the device is in its 2-minute advertising window (press the button to start one).
6. **Allow wake from this device on the host** (see [Host-side setup](#host-side-setup)).

## Usage

| Action | Result |
| --- | --- |
| Short-press the button | If the host is already connected, sends a space keypress immediately. If disconnected, starts a 2-minute fast advertising window and queues the keypress to fire ~500 ms after the host connects. Pressing again during an active window resets the 2-minute timer. |
| Long-press (≥ 2 s) | Clears all BLE bonds and reboots. Use this if the host's pairing record and the ESP32's bond record have diverged (e.g., you removed the device from the host but the bond is still on the ESP32). |
| MQTT publish `WAKE` to `jupiter/power` | Same effect as a short button press. |

*The MQTT topic (`jupiter/power`) is hardcoded; rename it inside the `.ino` (`mqttClient.subscribe(...)` in `mqtt_reconnect()` and the comparison in `mqtt_callback()`) if you want a different topic. The expected payload is the literal text `WAKE` — no JSON.*

### Why MQTT?

The MQTT path is the remote-wake half of this project — it's what fills the gap WoL can't cover. WoL magic packets are layer-2 broadcasts confined to the local subnet, so they don't cross routed networks, VPNs, or remote-access tunnels. An MQTT publish does. As long as the ESP32 has Wi-Fi and can reach a broker, a single message from anywhere that can also reach the broker (your phone on cellular, a laptop on another network, a remote-access tunnel into your home network, a home-automation system, etc.) is enough to wake the paired host.

Secure your broker — TLS and credentials are already required by the sketch — and consider a non-obvious topic name to discourage casual or accidental triggers.

## Host-side setup

Pairing alone is not enough — the host's Bluetooth controller has to be told it's allowed to be woken by this specific device.

### Linux / BlueZ

After pairing, run:
```bash
bluetoothctl
[bluetooth]# info <ESP32_MAC>      # confirm "Trusted: yes" and look at "WakeAllowed"
[bluetooth]# set-wake-allowed <ESP32_MAC> true
```
On most distros this is already `yes` for newly-bonded HIDs, but it's worth verifying. Also check that **"Wake on Bluetooth"** (or similarly named) is enabled in your BIOS/UEFI — without it the controller is fully powered down during sleep and can't wake the SoC.

### Windows

In **Device Manager → Human Interface Devices**, find `ESP32_Waker` (HID-compliant device), open Properties → **Power Management**, and tick **"Allow this device to wake the computer."**

### macOS

macOS handles BT HID wake automatically for paired keyboards; no extra step.

## Troubleshooting

- **Pairing succeeds but waking does nothing.** Verify the host marks the device as wake-allowed (above). Confirm BIOS/firmware allows BT wake. On Linux, look at `dmesg` after a wake attempt to confirm the BT controller saw a connection.
- **Pairing fails or the host can't reconnect after a removal.** The host's bond record and the ESP32's bond record are out of sync. Long-press the button to wipe ESP32-side bonds, then re-pair from the host.
- **Stray space keypress on a fresh pairing.** Shouldn't happen — the wake-intent flag is only set by button/MQTT, not by host-initiated connects. If you see it, file an issue.

## License

This project is licensed under the **GNU General Public License v3.0 or later** (GPL-3.0-or-later). See [LICENSE](LICENSE) for the full text.

```
Copyright (C) 2026 Shafin Ahmed
This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
```

### Licensing of dependencies

This program links against third-party libraries when built. Their licenses are compatible with redistribution under GPLv3:

- **NimBLE-Arduino** — Apache License 2.0. Compatible with GPLv3 (one-way: Apache 2.0 code can be incorporated into a GPLv3 work).
- **PubSubClient** — MIT License. Permissive and compatible.
- **arduino-esp32 core / ESP-IDF** — LGPL-2.1 / Apache-2.0 respectively. Compatible.

When you redistribute a binary built from this project, you must also provide (or offer) the corresponding source code under GPLv3, including any modifications you've made. You do not need to relicense the third-party libraries — keep their original license notices intact.

## Acknowledgments

Developed with iterative assistance from Anthropic's Claude (claude-opus-4-7) for refactoring, BLE HID implementation guidance, and documentation.
