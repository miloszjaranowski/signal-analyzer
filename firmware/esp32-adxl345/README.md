# ESP32 + ADXL345 vibration sensor

Firmware that turns an ESP32 and an ADXL345 accelerometer into a WiFi vibration sensor
for Signal Analyzer. It samples X, Y and Z at 3200 Hz and streams them over UDP in the
same SAP1 packets as the simulator, as channels 0, 1 and 2. The analyzer needs no changes.

```text
ADXL345 --(3200 Hz, own clock)--> FIFO --INT1--> sampling task (core 1)
    -> blocks of 256 samples per axis -> queue (~2 s) -> sender task (core 0)
    -> 3 UDP packets per block (X, Y, Z) -> PC
```

- The sensor paces the samples; the ESP32 only empties its 32-entry FIFO when INT1
  says it holds 16. No polling, no jitter.
- The real sample rate is measured against the ESP32 crystal and sent in the packets:
  the ADXL345's internal oscillator can be off by a few percent, which would shift
  every frequency in the spectrum.
- WiFi stalls are absorbed by a ~2 s queue. Blocks that still cannot be sent are dropped
  whole and the sequence numbers keep counting, so the analyzer reports them as lost
  packets instead of splicing the signal.

Built and checked with PlatformIO, `espressif32` 8.0 (Arduino-ESP32 3.3). Not yet tested
on hardware.

## Parts

- ESP32 board with an ESP32-WROOM-32 module: ESP32 DevKit V1, YD-ESP32-23 or similar
- ADXL345 breakout, e.g. GY-291
- 7 jumper wires, a USB data cable (some cables only charge)

## Wiring (SPI)

| ADXL345 (GY-291) | ESP32   | Note                             |
|------------------|---------|----------------------------------|
| VCC / 3V3        | 3V3     | 3.3 V, not 5 V                   |
| GND              | GND     |                                  |
| CS               | GPIO 5  | a CS driven low selects SPI mode |
| SDO              | GPIO 19 | MISO                             |
| SDA              | GPIO 23 | MOSI (SDI in SPI mode)           |
| SCL              | GPIO 18 | SCLK                             |
| INT1             | GPIO 4  | FIFO watermark interrupt         |

Keep the wires short (up to 10-15 cm). Other pins can be set in `config.h`. Board pin
labels differ between makers, so go by the GPIO numbers printed on the board.

## Build and flash

1. **PlatformIO**: install the *PlatformIO IDE* extension in VS Code, or
   `pip install platformio` for the command line.
2. **USB driver**: plug the board in and open Device Manager → *Ports (COM & LPT)*. It
   should show *Silicon Labs CP210x* or *USB-SERIAL CH340* with a COM port. If it shows
   an unknown device, install the driver for the USB chip on your board (CP210x from
   Silicon Labs, or CH340 from WCH).
3. **Configuration**: copy `src/config.example.h` to `src/config.h` and fill in:
   - `WIFI_SSID`, `WIFI_PASSWORD`: a 2.4 GHz network (the ESP32 has no 5 GHz radio)
   - `TARGET_HOST`: the IPv4 address of the PC running the analyzer (`ipconfig`), on the
     same network

   `config.h` is git-ignored because it holds your password.
4. **Flash**:
   - VS Code: *File → Open Folder* on `firmware/esp32-adxl345` (this folder, not the
     repository root), then *PlatformIO: Upload* (→ in the status bar).
   - Command line: `pio run -d firmware/esp32-adxl345 -t upload`

   The first build downloads the ESP32 platform and toolchain: about 5.5 GB for
   `espressif32` 8.0, into `%USERPROFILE%\.platformio`. Later builds take seconds.
   If the upload stops at `Connecting...____`, hold the board's **BOOT** button until
   `Writing at 0x...` appears. If it still fails, lower `upload_speed` in
   `platformio.ini` to 460800 or 115200.
5. **Serial monitor**: *PlatformIO: Serial Monitor*, or
   `pio device monitor -d firmware/esp32-adxl345`. You should see:

   ```text
   Signal Analyzer sensor: ESP32 + ADXL345
   ADXL345 found: 3200 Hz, +-16 g, FIFO stream mode
   Connecting to WiFi "my-network"...
   WiFi connected: 192.168.1.57, RSSI -52 dBm. Sending to 192.168.1.100:5005, channels 0-2 (X, Y, Z)
   rate 3187.2 Hz (sent as 3187) | samples +15936 | packets 563, send errors 0 | dropped blocks: queue 0, offline 0 | FIFO full 0 | RSSI -52 dBm
   ```

   The on-board LED is on while connected, blinks slowly while connecting, and blinks
   fast if the sensor is not found.

## Receive on the PC

1. Stop the Python simulator if it is running on the same port.
2. Quick check: `sa_cli --port 5005` prints a line per axis (`ch=0`, `ch=1`, `ch=2`). At
   rest the vertical axis reads about 1 g RMS and the others about 0.
3. GUI: press *Start*. The channel table shows X, Y and Z as channels 0, 1 and 2.
   Settings that suit the ADXL345:
   - **Max frequency** up to 1600 Hz (Nyquist of 3200 Hz)
   - **Envelope band** e.g. 300–1500 Hz (the default 1000–4000 Hz is above Nyquist)
   - FFT 4096 gives 0.78 Hz resolution
4. *Record…* saves the stream for later (`.sacap`).

**Windows firewall:** the first time `signal_analyzer` or `sa_cli` listens, Windows asks
whether to allow it. Allow it on private networks. If nothing arrives although the ESP32
reports packets sent, add a rule in an administrator PowerShell:

```powershell
New-NetFirewallRule -DisplayName "Signal Analyzer UDP 5005" -Direction Inbound -Protocol UDP -LocalPort 5005 -Action Allow -Profile Private
```

Also check that the network is set to *Private* in Windows, and that the router does not
isolate WiFi clients (guest networks usually do).

## Troubleshooting

| Symptom | Likely cause |
| ------- | ------------ |
| `ADXL345 not found (device ID 0x00)` or `0xFF` | Wiring: CS, SDO→MISO, power. A floating CS leaves the chip in I2C mode. |
| `Connecting...` never ends | Wrong SSID or password, or a 5 GHz-only network |
| `packets` grows, PC receives nothing | Firewall, wrong `TARGET_HOST`, client isolation, PC on another network |
| `send errors` or `dropped blocks: queue` grow | Weak WiFi (RSSI below about -75 dBm): move closer or use an external-antenna board |
| `FIFO full` grows | Samples were lost because the sampling task was late. Should stay 0. |
| Spectrum lines a few % off | Expected with the nominal rate; the firmware sends the measured one after a few seconds |

## Mounting

The mounting decides what you measure. Screw the breakout down, glue it with
cyanoacrylate, or use a magnet. Double-sided tape and hand-held sensors damp everything
above a few hundred Hz. A first test object: a PC fan with a piece of tape on one blade
(unbalance) should show a clear line at the rotation frequency.

## Limits of the ADXL345

Bandwidth is about 1.6 kHz and its anti-aliasing filtering is weak, so vibration above
that can fold back into the spectrum. That is fine for unbalance, misalignment, looseness
and fatigue. For bearing envelope analysis a sensor with a few kHz of flat bandwidth (e.g.
ST IIS3DWB) is the next step; only `Adxl345.*` and the sample rate would change.
