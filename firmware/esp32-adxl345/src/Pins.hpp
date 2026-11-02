#pragma once

// Default wiring per chip. To wire differently, #define any of these in config.h.

#include <sdkconfig.h>

#if CONFIG_IDF_TARGET_ESP32S3
// ESP32-S3 (YD-ESP32-S3 / YD-ESP32-23, ESP32-S3-DevKitC-1). GPIO 22-25 do not exist,
// 19/20 are the native USB port and 26-37 belong to flash / PSRAM, so SPI uses the
// S3's default FSPI pins.
#ifndef PIN_SCK
#define PIN_SCK 12
#endif
#ifndef PIN_MISO
#define PIN_MISO 13  // ADXL345 SDO
#endif
#ifndef PIN_MOSI
#define PIN_MOSI 11  // ADXL345 SDA (SDI in SPI mode)
#endif
#ifndef PIN_CS
#define PIN_CS 10
#endif
#ifndef PIN_INT1
#define PIN_INT1 4
#endif
#ifndef PIN_LED
#define PIN_LED -1  // the on-board LED is an addressable RGB LED, not a plain GPIO LED
#endif

namespace pins {
// Free GPIOs on S3 modules with octal PSRAM (e.g. N16R8): 0-18, 21, 38-48.
constexpr bool usable(int gpio) {
    return gpio == -1 || (gpio >= 0 && gpio <= 18) || gpio == 21 || (gpio >= 38 && gpio <= 48);
}
}  // namespace pins
static_assert(pins::usable(PIN_SCK) && pins::usable(PIN_MISO) && pins::usable(PIN_MOSI) &&
                  pins::usable(PIN_CS) && pins::usable(PIN_INT1) && pins::usable(PIN_LED),
              "A pin is not usable on the ESP32-S3 (GPIO 22-25 do not exist, 19/20 are USB, "
              "26-37 are flash/PSRAM). Remove the PIN_ lines from config.h to use the S3 defaults.");

#elif CONFIG_IDF_TARGET_ESP32
// Classic ESP32 (ESP32 DevKit V1 and other ESP32-WROOM-32 boards): VSPI pins.
#ifndef PIN_SCK
#define PIN_SCK 18
#endif
#ifndef PIN_MISO
#define PIN_MISO 19  // ADXL345 SDO
#endif
#ifndef PIN_MOSI
#define PIN_MOSI 23  // ADXL345 SDA (SDI in SPI mode)
#endif
#ifndef PIN_CS
#define PIN_CS 5
#endif
#ifndef PIN_INT1
#define PIN_INT1 4
#endif
#ifndef PIN_LED
#define PIN_LED 2  // on-board blue LED of the DevKit V1
#endif

#else
#error "Unsupported chip: add default pins for it in Pins.hpp"
#endif
