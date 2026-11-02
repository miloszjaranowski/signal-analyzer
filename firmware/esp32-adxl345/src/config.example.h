#pragma once

// Copy this file to config.h (same directory) and fill it in. config.h is
// git-ignored because it holds your WiFi password.

// --- WiFi: 2.4 GHz only, the ESP32 has no 5 GHz radio ------------------------
#define WIFI_SSID "your-network"
#define WIFI_PASSWORD "your-password"

// --- Where to send the samples ---------------------------------------------------
// The PC running signal_analyzer or sa_cli: `ipconfig` on Windows shows its
// IPv4 address. The port is the "UDP port" field in the app (default 5005).
#define TARGET_HOST "192.168.1.100"
#define TARGET_PORT 5005

// Channel of the X axis; Y and Z follow. With a second sensor board use 3, so the
// app shows it as channels 3..5 (the protocol allows channels 0..7).
#define FIRST_CHANNEL 0

// --- Wiring (ESP32 VSPI) -----------------------------------------------------------
#define PIN_SCK 18
#define PIN_MISO 19   // ADXL345 SDO
#define PIN_MOSI 23   // ADXL345 SDA (SDI in SPI mode)
#define PIN_CS 5      // ADXL345 CS
#define PIN_INT1 4    // ADXL345 INT1
#define PIN_LED 2     // on-board LED of the DevKit V1; -1 if your board has none
