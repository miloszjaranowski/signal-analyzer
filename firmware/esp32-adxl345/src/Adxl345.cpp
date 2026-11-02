#include "Adxl345.hpp"

bool Adxl345::begin() {
    pinMode(cs_, OUTPUT);
    digitalWrite(cs_, HIGH);
    delay(5);  // power-up

    if (deviceId() != 0xE5) {
        return false;
    }
    writeRegister(kPowerCtl, 0x00);    // standby while configuring
    writeRegister(kDataFormat, 0x0B);  // full resolution, +-16 g, 4-wire SPI, INT active high
    writeRegister(kBwRate, 0x0F);      // 3200 Hz output data rate
    writeRegister(kFifoCtl, 0x00);     // bypass: empties the FIFO
    writeRegister(kFifoCtl, 0x80 | kWatermark);  // stream mode, watermark
    writeRegister(kIntMap, 0x00);      // all interrupts to INT1
    writeRegister(kIntEnable, 0x02);   // watermark interrupt
    writeRegister(kPowerCtl, 0x08);    // measure
    return true;
}

uint8_t Adxl345::fifoEntries() { return readRegister(kFifoStatus) & 0x3F; }

void Adxl345::readSample(int16_t xyz[3]) {
    // One burst over DATAX0..DATAZ1 pops exactly one FIFO entry.
    uint8_t tx[7] = {kRead | kMultiByte | kDataX0, 0, 0, 0, 0, 0, 0};
    uint8_t rx[7];
    spi_.beginTransaction(settings_);
    digitalWrite(cs_, LOW);
    spi_.transferBytes(tx, rx, sizeof tx);
    digitalWrite(cs_, HIGH);
    spi_.endTransaction();
    for (int i = 0; i < 3; ++i) {
        xyz[i] = static_cast<int16_t>(rx[1 + 2 * i] | (rx[2 + 2 * i] << 8));
    }
    delayMicroseconds(5);  // datasheet: at least 5 us before the next FIFO read
}

uint8_t Adxl345::readRegister(uint8_t reg) {
    uint8_t tx[2] = {static_cast<uint8_t>(kRead | reg), 0};
    uint8_t rx[2];
    spi_.beginTransaction(settings_);
    digitalWrite(cs_, LOW);
    spi_.transferBytes(tx, rx, sizeof tx);
    digitalWrite(cs_, HIGH);
    spi_.endTransaction();
    return rx[1];
}

void Adxl345::writeRegister(uint8_t reg, uint8_t value) {
    uint8_t tx[2] = {reg, value};
    uint8_t rx[2];
    spi_.beginTransaction(settings_);
    digitalWrite(cs_, LOW);
    spi_.transferBytes(tx, rx, sizeof tx);
    digitalWrite(cs_, HIGH);
    spi_.endTransaction();
}
