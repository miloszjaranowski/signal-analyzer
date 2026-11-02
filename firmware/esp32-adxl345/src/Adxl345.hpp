#pragma once

#include <Arduino.h>
#include <SPI.h>

// Minimal ADXL345 driver over 4-wire SPI: 3200 Hz output rate, +-16 g at full
// resolution, FIFO in stream mode with a watermark interrupt on INT1. Register
// values follow the Analog Devices ADXL345 datasheet.
//
// The sensor's own oscillator paces the samples; the FIFO (32 entries, 10 ms at
// 3200 Hz) lets the reader be late by a few milliseconds without losing any.
class Adxl345 {
public:
    static constexpr float kNominalRateHz = 3200.0f;
    static constexpr float kGPerLsb = 0.0039f;     // full resolution: 3.9 mg/LSB (typical)
    static constexpr uint8_t kFifoSize = 32;
    static constexpr uint8_t kWatermark = 16;      // INT1 fires at this many samples

    Adxl345(SPIClass& spi, int csPin) : spi_(spi), cs_(csPin) {}

    // Configures and starts measuring. False if the device ID is wrong, which
    // almost always means wiring (CS, MISO) or a sensor in I2C mode.
    bool begin();
    uint8_t deviceId() { return readRegister(kDevId); }

    uint8_t fifoEntries();                // samples waiting, 0..32
    void readSample(int16_t xyz[3]);      // pops the oldest FIFO entry

private:
    static constexpr uint8_t kDevId = 0x00;
    static constexpr uint8_t kBwRate = 0x2C;
    static constexpr uint8_t kPowerCtl = 0x2D;
    static constexpr uint8_t kIntEnable = 0x2E;
    static constexpr uint8_t kIntMap = 0x2F;
    static constexpr uint8_t kDataFormat = 0x31;
    static constexpr uint8_t kDataX0 = 0x32;
    static constexpr uint8_t kFifoCtl = 0x38;
    static constexpr uint8_t kFifoStatus = 0x39;
    static constexpr uint8_t kRead = 0x80;
    static constexpr uint8_t kMultiByte = 0x40;

    uint8_t readRegister(uint8_t reg);
    void writeRegister(uint8_t reg, uint8_t value);

    SPIClass& spi_;
    int cs_;
    const SPISettings settings_{5'000'000, MSBFIRST, SPI_MODE3};  // max 5 MHz, CPOL = CPHA = 1
};
