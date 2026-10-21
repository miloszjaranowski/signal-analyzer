#!/usr/bin/env python3
"""Vibration sensor simulator: streams float32 samples over UDP in real time.

Packet format (little-endian), matching core/include/sa/Protocol.hpp:

    uint32 magic = 0x31504153 ("SAP1")
    uint32 sequence
    uint32 sample_rate
    uint16 sample_count
    uint16 channel
    float32 samples[sample_count]

Examples:
    python sensor_sim.py                               # 50 Hz + harmonics, light noise
    python sensor_sim.py --tones 120:1,240:0.3 --noise 0.1
    python sensor_sim.py --fault-hz 87 --fault-amp 2   # bearing-like impacts
    python sensor_sim.py --drop 0.02                   # drop 2 % of packets
"""

from __future__ import annotations

import argparse
import socket
import struct
import sys
import time
from dataclasses import dataclass

import numpy as np

MAGIC = 0x31504153
HEADER = struct.Struct("<IIIHH")
MAX_SAMPLES_PER_PACKET = 4096


@dataclass
class Tone:
    freq_hz: float
    amplitude: float


def parse_tones(spec: str) -> list[Tone]:
    """Parses 'freq:amp,freq:amp' into tones, e.g. '50:1,150:0.3'."""
    tones = []
    for part in filter(None, (p.strip() for p in spec.split(","))):
        freq, _, amp = part.partition(":")
        tones.append(Tone(float(freq), float(amp) if amp else 1.0))
    return tones


class SignalGenerator:
    """Phase-continuous generator: block N continues exactly where N-1 ended."""

    def __init__(self, fs: int, tones: list[Tone], noise_rms: float,
                 fault_hz: float, fault_amp: float, resonance_hz: float, seed: int | None):
        self.fs = fs
        self.tones = tones
        self.noise_rms = noise_rms
        self.fault_hz = fault_hz
        self.fault_amp = fault_amp
        self.resonance_hz = resonance_hz
        self.rng = np.random.default_rng(seed)
        self.sample_index = 0

    def next_block(self, n: int) -> np.ndarray:
        t = (self.sample_index + np.arange(n)) / self.fs
        x = np.zeros(n)
        for tone in self.tones:
            x += tone.amplitude * np.sin(2 * np.pi * tone.freq_hz * t)
        if self.noise_rms > 0:
            x += self.rng.normal(0.0, self.noise_rms, n)
        if self.fault_hz > 0 and self.fault_amp > 0:
            x += self._impacts(t)
        self.sample_index += n
        return x.astype("<f4")

    def _impacts(self, t: np.ndarray) -> np.ndarray:
        # A localised bearing defect excites a structural resonance once per
        # defect passage: periodic, exponentially decaying bursts. Their spacing
        # (1 / fault_hz) is what envelope analysis would later detect.
        period = 1.0 / self.fault_hz
        since_impact = np.mod(t, period)
        decay = np.exp(-since_impact * 800.0)
        return self.fault_amp * decay * np.sin(2 * np.pi * self.resonance_hz * since_impact)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5005)
    ap.add_argument("--fs", type=int, default=10_000, help="sample rate [Hz]")
    ap.add_argument("--block", type=int, default=500, help="samples per packet")
    ap.add_argument("--tones", default="50:1.0,150:0.3,250:0.15",
                    help="comma-separated freq:amplitude pairs")
    ap.add_argument("--noise", type=float, default=0.05, help="white noise RMS")
    ap.add_argument("--fault-hz", type=float, default=0.0,
                    help="impact repetition rate [Hz], 0 = healthy machine")
    ap.add_argument("--fault-amp", type=float, default=1.5)
    ap.add_argument("--resonance-hz", type=float, default=2_500.0)
    ap.add_argument("--drop", type=float, default=0.0,
                    help="probability of dropping a packet (tests loss detection)")
    ap.add_argument("--duration", type=float, default=0.0, help="seconds, 0 = forever")
    ap.add_argument("--seed", type=int, default=None)
    args = ap.parse_args()

    if not 0 < args.block <= MAX_SAMPLES_PER_PACKET:
        ap.error(f"--block must be in 1..{MAX_SAMPLES_PER_PACKET}")
    if args.resonance_hz >= args.fs / 2:
        ap.error("--resonance-hz must be below Nyquist (fs/2)")

    gen = SignalGenerator(args.fs, parse_tones(args.tones), args.noise,
                          args.fault_hz, args.fault_amp, args.resonance_hz, args.seed)
    drop_rng = np.random.default_rng(None if args.seed is None else args.seed + 1)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dest = (args.host, args.port)

    packet_period = args.block / args.fs
    print(f"Streaming to {args.host}:{args.port}  fs={args.fs} Hz  "
          f"{args.block} samples/packet ({1 / packet_period:.1f} packets/s). Ctrl+C to stop.")

    start = time.perf_counter()
    seq = sent = dropped = 0
    last_report = start
    try:
        while args.duration <= 0 or seq * packet_period < args.duration:
            samples = gen.next_block(args.block)
            if args.drop > 0 and drop_rng.random() < args.drop:
                dropped += 1
            else:
                header = HEADER.pack(MAGIC, seq & 0xFFFF_FFFF, args.fs, args.block, 0)
                sock.sendto(header + samples.tobytes(), dest)
                sent += 1
            seq += 1

            # Absolute-time pacing: no drift accumulates over long runs.
            deadline = start + seq * packet_period
            delay = deadline - time.perf_counter()
            if delay > 0:
                time.sleep(delay)

            now = time.perf_counter()
            if now - last_report >= 2.0:
                print(f"  t={now - start:7.1f}s  sent={sent}  dropped={dropped}", flush=True)
                last_report = now
    except KeyboardInterrupt:
        pass
    finally:
        sock.close()

    print(f"Done: sent {sent} packets, dropped {dropped} on purpose.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
