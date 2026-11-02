// ESP32 + ADXL345 vibration sensor for Signal Analyzer.
//
//   ADXL345 --(3200 Hz, own clock)--> FIFO --INT1--> sampling task (core 1)
//       -> blocks of 256 samples per axis -> queue -> sender task (core 0)
//       -> 3 UDP packets per block (SAP1, channels X, Y, Z) -> PC
//
// The sensor paces the samples; the queue absorbs WiFi stalls of up to ~2 s.
// Whatever cannot be sent is dropped as whole blocks, and the sequence numbers
// keep counting, so the analyzer reports the loss instead of splicing the signal.

#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#include <atomic>
#include <cinttypes>
#include <cmath>

#include "Adxl345.hpp"
#include "Sap1.hpp"

#if __has_include("config.h")
#include "config.h"
#else
#error "Copy src/config.example.h to src/config.h and fill in your WiFi and PC address"
#endif
#include "Pins.hpp"  // after config.h, which may override the defaults

namespace {

constexpr uint16_t kBlockSamples = 256;  // per axis: 80 ms at 3200 Hz, 1040-byte packets
constexpr int kQueueBlocks = 24;         // ~1.9 s of buffering, ~74 kB of RAM
constexpr int kAxes = 3;

struct Block {
    uint32_t sequence = 0;
    float axis[kAxes][kBlockSamples];  // in g
};

struct Stats {
    std::atomic<uint32_t> samples{0};        // read from the sensor
    std::atomic<uint32_t> fifoFull{0};       // reads that found the FIFO full: samples may be lost
    std::atomic<uint32_t> queueDrops{0};     // blocks dropped because the sender fell behind
    std::atomic<uint32_t> notConnected{0};   // blocks dropped while WiFi was down
    std::atomic<uint32_t> packetsSent{0};
    std::atomic<uint32_t> sendErrors{0};
};

Adxl345 sensor(SPI, PIN_CS);
QueueHandle_t blockQueue = nullptr;
TaskHandle_t samplingTaskHandle = nullptr;
IPAddress target;
Stats stats;

// Sample rate measured against the ESP32 crystal; the sensor's internal
// oscillator can be off by a few percent, which would shift every frequency.
std::atomic<uint32_t> rateMilliHz{static_cast<uint32_t>(Adxl345::kNominalRateHz * 1000)};
std::atomic<uint32_t> sentRateHz{static_cast<uint32_t>(Adxl345::kNominalRateHz)};

void IRAM_ATTR onFifoWatermark() {
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(samplingTaskHandle, &woken);
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

// Reads every sample the FIFO holds, fills blocks and hands full ones to the sender.
void samplingTask(void*) {
    static Block block;  // static: 3 kB would crowd the task stack
    uint16_t fill = 0;
    uint32_t sequence = 0;

    // Rate measurement: samples counted between two points in time, starting
    // after a second so the FIFO start-up does not skew it.
    uint32_t refMicros = 0;
    uint32_t refSamples = 0;
    const uint32_t startMicros = micros();

    for (;;) {
        // Woken by INT1; the timeout is a fallback in case an edge is ever missed.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));

        uint8_t entries;
        while ((entries = sensor.fifoEntries()) > 0) {
            if (entries >= Adxl345::kFifoSize) {
                ++stats.fifoFull;
            }
            for (uint8_t i = 0; i < entries; ++i) {
                int16_t raw[kAxes];
                sensor.readSample(raw);
                for (int a = 0; a < kAxes; ++a) {
                    block.axis[a][fill] = raw[a] * Adxl345::kGPerLsb;
                }
                if (++fill == kBlockSamples) {
                    block.sequence = sequence++;  // counts dropped blocks too
                    if (xQueueSend(blockQueue, &block, 0) != pdTRUE) {
                        ++stats.queueDrops;
                    }
                    fill = 0;
                }
            }
            stats.samples += entries;
        }

        const uint32_t now = micros();
        const uint32_t samples = stats.samples.load();
        if (refMicros == 0 && now - startMicros > 1'000'000) {
            refMicros = now;
            refSamples = samples;
        } else if (refMicros != 0 && now - refMicros > 2'000'000) {
            // micros() wraps after ~71 min; unsigned subtraction handles one wrap,
            // and the reference restarts long before a second one.
            const double seconds = (now - refMicros) * 1e-6;
            rateMilliHz = static_cast<uint32_t>((samples - refSamples) / seconds * 1000.0);
            if (now - refMicros > 600'000'000) {  // restart every 10 min
                refMicros = now;
                refSamples = samples;
            }
        }
    }
}

// The rate sent in the packets changes only on a real difference (> 0.2 %), so
// the analyzer does not reset its spectrogram for every rounding wobble.
uint32_t rateToSend() {
    const double measured = rateMilliHz.load() / 1000.0;
    const uint32_t current = sentRateHz.load();
    if (std::fabs(measured - current) > 0.002 * current) {
        sentRateHz = static_cast<uint32_t>(std::lround(measured));
    }
    return sentRateHz.load();
}

void senderTask(void*) {
    static Block block;
    static uint8_t packet[sap1::packetSize(kBlockSamples)];
    WiFiUDP udp;

    for (;;) {
        if (xQueueReceive(blockQueue, &block, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (WiFi.status() != WL_CONNECTED) {
            ++stats.notConnected;
            continue;
        }
        const uint32_t rate = rateToSend();
        for (int a = 0; a < kAxes; ++a) {
            const size_t size = sap1::build(packet, block.sequence, rate,
                                            static_cast<uint16_t>(FIRST_CHANNEL + a), block.axis[a],
                                            kBlockSamples);
            udp.beginPacket(target, TARGET_PORT);
            udp.write(packet, size);
            if (udp.endPacket()) {
                ++stats.packetsSent;
            } else {
                ++stats.sendErrors;  // usually lwIP out of buffers: WiFi too slow right now
            }
        }
    }
}

void setLed(bool on) {
    if (PIN_LED >= 0) {
        digitalWrite(PIN_LED, on ? HIGH : LOW);
    }
}

// Sensor not found: blink fast forever and keep explaining on the serial port.
[[noreturn]] void sensorError(uint8_t deviceId) {
    for (;;) {
        Serial.printf("ADXL345 not found (device ID 0x%02X, expected 0xE5). Check the wiring:\n"
                      "  VCC-3V3, GND-GND, CS-GPIO%d, SDO-GPIO%d, SDA-GPIO%d, SCL-GPIO%d\n",
                      deviceId, PIN_CS, PIN_MISO, PIN_MOSI, PIN_SCK);
        for (int i = 0; i < 20; ++i) {
            setLed(i % 2);
            delay(100);
        }
    }
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\nSignal Analyzer sensor: ESP32 + ADXL345");
    if (PIN_LED >= 0) {
        pinMode(PIN_LED, OUTPUT);
    }

    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
    if (!sensor.begin()) {
        sensorError(sensor.deviceId());
    }
    Serial.println("ADXL345 found: 3200 Hz, +-16 g, FIFO stream mode");

    if (!target.fromString(TARGET_HOST)) {
        Serial.printf("TARGET_HOST \"%s\" is not an IPv4 address; fix config.h\n", TARGET_HOST);
    }

    blockQueue = xQueueCreate(kQueueBlocks, sizeof(Block));
    // Sampling on core 1 at high priority (Arduino's loop() also runs there, at
    // priority 1); WiFi and sending on core 0 with the WiFi stack.
    xTaskCreatePinnedToCore(samplingTask, "sampling", 4096, nullptr, configMAX_PRIORITIES - 2,
                            &samplingTaskHandle, 1);
    pinMode(PIN_INT1, INPUT);
    attachInterrupt(digitalPinToInterrupt(PIN_INT1), onFifoWatermark, RISING);

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);  // power save delays and bunches packets
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("Connecting to WiFi \"%s\"...\n", WIFI_SSID);
    xTaskCreatePinnedToCore(senderTask, "sender", 4096, nullptr, 3, nullptr, 0);
}

// Status: LED on while streaming, slow blink while WiFi is down; a report every 5 s.
void loop() {
    static bool wasConnected = false;
    static uint32_t lastReport = 0;
    static uint32_t lastSamples = 0;

    const bool connected = WiFi.status() == WL_CONNECTED;
    if (connected != wasConnected) {
        wasConnected = connected;
        if (connected) {
            Serial.printf("WiFi connected: %s, RSSI %d dBm. Sending to %s:%d, channels %d-%d (X, Y, Z)\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI(), TARGET_HOST, TARGET_PORT,
                          FIRST_CHANNEL, FIRST_CHANNEL + 2);
        } else {
            Serial.println("WiFi disconnected, reconnecting...");
        }
    }
    setLed(connected || (millis() / 500) % 2);

    const uint32_t now = millis();
    if (now - lastReport >= 5000) {
        const uint32_t samples = stats.samples.load();
        Serial.printf("rate %.1f Hz (sent as %" PRIu32 ") | samples +%" PRIu32 " | packets %" PRIu32
                      ", send errors %" PRIu32 " | dropped blocks: queue %" PRIu32 ", offline %" PRIu32
                      " | FIFO full %" PRIu32 " | RSSI %d dBm\n",
                      rateMilliHz.load() / 1000.0, sentRateHz.load(), samples - lastSamples,
                      stats.packetsSent.load(), stats.sendErrors.load(), stats.queueDrops.load(),
                      stats.notConnected.load(), stats.fifoFull.load(), connected ? WiFi.RSSI() : 0);
        lastSamples = samples;
        lastReport = now;
    }
    delay(50);
}
