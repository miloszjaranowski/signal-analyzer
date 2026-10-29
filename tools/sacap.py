#!/usr/bin/env python3
"""Signal Analyzer capture files (.sacap): inspect, convert to CSV / HDF5 and back.

    python tools/sacap.py info      run.sacap
    python tools/sacap.py to-csv    run.sacap run.csv
    python tools/sacap.py to-hdf5   run.sacap run.h5      # needs h5py
    python tools/sacap.py from-csv  run.csv   back.sacap
    python tools/sacap.py from-hdf5 run.h5    back.sacap  # needs h5py

A capture holds every received datagram with its arrival time, see
core/include/sa/Capture.hpp:

    file header   "SACAP001"
    record        uint64 arrival time [us], uint32 length L, L bytes datagram

Conversion turns the SAP1 packets into one sample stream per channel. A sample's
time is ((sequence - first sequence) * samples per packet + i) / sample rate, so
lost packets stay visible as gaps in time_s. Malformed datagrams are counted and
left out.

CSV (long format, one row per sample):
    channel,sample_rate,time_s,value

HDF5: one group per channel, /channel_<n>, with datasets `samples` (float32) and
`time_s` (float64) and attributes sample_rate, packets, lost_packets.

from-csv / from-hdf5 cut the streams back into packets of --block samples (500,
the simulator's default) so the GUI and sa_cli can replay them. Only .sacap is
lossless: arrival times become regular and malformed datagrams are gone.
"""

from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

CAPTURE_MAGIC = b"SACAP001"
RECORD = struct.Struct("<QI")
PACKET_MAGIC = 0x31504153
HEADER = struct.Struct("<IIIHH")
MAX_CHANNELS = 8


@dataclass
class Packet:
    time_us: int
    channel: int
    sequence: int
    sample_rate: int
    samples: np.ndarray


@dataclass
class Stream:
    """All samples of one channel, with the time of each one."""
    sample_rate: int
    time_s: np.ndarray
    samples: np.ndarray
    packets: int = 0
    lost_packets: int = 0


@dataclass
class Capture:
    packets: list[Packet] = field(default_factory=list)
    malformed: int = 0
    records: int = 0
    duration_s: float = 0.0


def parse_packet(datagram: bytes) -> tuple[int, int, int, np.ndarray] | None:
    """(channel, sequence, sample_rate, samples) for a valid SAP1 packet, else None."""
    if len(datagram) < HEADER.size:
        return None
    magic, sequence, sample_rate, count, channel = HEADER.unpack_from(datagram)
    if (magic != PACKET_MAGIC or sample_rate == 0 or channel >= MAX_CHANNELS
            or len(datagram) != HEADER.size + 4 * count):
        return None
    samples = np.frombuffer(datagram, dtype="<f4", count=count, offset=HEADER.size)
    return channel, sequence, sample_rate, samples


def read_capture(path: Path) -> Capture:
    data = path.read_bytes()
    if data[:len(CAPTURE_MAGIC)] != CAPTURE_MAGIC:
        raise SystemExit(f"{path} is not a capture file (.sacap)")
    capture = Capture()
    pos = len(CAPTURE_MAGIC)
    first_us = last_us = None
    while pos + RECORD.size <= len(data):
        time_us, length = RECORD.unpack_from(data, pos)
        pos += RECORD.size
        if pos + length > len(data):
            break  # truncated last record, e.g. after a crash
        datagram = data[pos:pos + length]
        pos += length
        capture.records += 1
        first_us = time_us if first_us is None else first_us
        last_us = time_us
        parsed = parse_packet(datagram)
        if parsed is None:
            capture.malformed += 1
            continue
        channel, sequence, sample_rate, samples = parsed
        capture.packets.append(Packet(time_us, channel, sequence, sample_rate, samples))
    if first_us is not None:
        capture.duration_s = (last_us - first_us) * 1e-6
    return capture


def to_streams(capture: Capture) -> dict[int, Stream]:
    """Per-channel sample streams; lost packets leave gaps in time."""
    by_channel: dict[int, list[Packet]] = {}
    for packet in capture.packets:
        by_channel.setdefault(packet.channel, []).append(packet)

    streams = {}
    for channel, packets in sorted(by_channel.items()):
        sample_rate = packets[0].sample_rate
        position = 0  # sample index of the current packet's first sample
        lost = 0
        times, values = [], []
        for i, packet in enumerate(packets):
            if i > 0:
                previous = packets[i - 1]
                gap = (packet.sequence - previous.sequence) & 0xFFFF_FFFF
                if 0 < gap < 0x8000_0000:  # forward: gap - 1 packets were lost
                    lost += gap - 1
                    position += len(previous.samples) * gap
                else:  # sender restarted: carry on without a gap
                    position += len(previous.samples)
            n = len(packet.samples)
            times.append((position + np.arange(n)) / sample_rate)
            values.append(packet.samples)
        streams[channel] = Stream(sample_rate, np.concatenate(times),
                                  np.concatenate(values).astype(np.float32),
                                  packets=len(packets), lost_packets=lost)
    return streams


def write_capture(streams: dict[int, Stream], path: Path, block: int) -> int:
    """Cuts every stream into packets of `block` samples, interleaved by time."""
    records = []
    for channel, stream in streams.items():
        index = np.rint(stream.time_s * stream.sample_rate).astype(np.int64)
        order = np.argsort(index, kind="stable")
        index = index[order] - index[order][0]
        samples = stream.samples[order].astype("<f4")
        packet_of = index // block  # a whole missing block becomes a missing sequence number
        starts = np.flatnonzero(np.diff(packet_of, prepend=-1))
        ends = np.append(starts[1:], len(samples))
        for start, end in zip(starts, ends):
            sequence = int(packet_of[start])
            header = HEADER.pack(PACKET_MAGIC, sequence & 0xFFFF_FFFF, stream.sample_rate,
                                 end - start, channel)
            time_us = int(round(sequence * block / stream.sample_rate * 1e6))
            records.append((time_us, channel, header + samples[start:end].tobytes()))
    records.sort(key=lambda r: (r[0], r[1]))

    with path.open("wb") as f:
        f.write(CAPTURE_MAGIC)
        for time_us, _, datagram in records:
            f.write(RECORD.pack(time_us, len(datagram)))
            f.write(datagram)
    return len(records)


def cmd_info(args: argparse.Namespace) -> None:
    capture = read_capture(args.capture)
    streams = to_streams(capture)
    print(f"{args.capture}: {capture.records} datagrams over {capture.duration_s:.2f} s, "
          f"{capture.malformed} malformed")
    for channel, s in streams.items():
        print(f"  channel {channel}: {s.sample_rate} Hz, {s.packets} packets, "
              f"{len(s.samples)} samples ({len(s.samples) / s.sample_rate:.2f} s), "
              f"{s.lost_packets} lost, RMS {np.sqrt(np.mean(s.samples.astype(np.float64) ** 2)):.4f}")


def cmd_to_csv(args: argparse.Namespace) -> None:
    streams = to_streams(read_capture(args.capture))
    with args.output.open("w", newline="") as f:
        f.write("channel,sample_rate,time_s,value\n")
        for channel, s in streams.items():
            rows = np.column_stack([np.full(len(s.samples), channel), np.full(len(s.samples), s.sample_rate),
                                    s.time_s, s.samples])
            np.savetxt(f, rows, fmt=["%d", "%d", "%.7f", "%.9g"], delimiter=",")
    print(f"Wrote {sum(len(s.samples) for s in streams.values())} samples of "
          f"{len(streams)} channel(s) to {args.output}")


def import_h5py():
    try:
        import h5py
    except ImportError:
        raise SystemExit("HDF5 needs h5py: pip install h5py")
    return h5py


def cmd_to_hdf5(args: argparse.Namespace) -> None:
    h5py = import_h5py()
    capture = read_capture(args.capture)
    streams = to_streams(capture)
    with h5py.File(args.output, "w") as f:
        f.attrs["source"] = str(args.capture)
        f.attrs["format"] = "Signal Analyzer recording, one group per channel"
        f.attrs["malformed_datagrams"] = capture.malformed
        for channel, s in streams.items():
            group = f.create_group(f"channel_{channel}")
            group.create_dataset("samples", data=s.samples, compression="gzip")
            group.create_dataset("time_s", data=s.time_s, compression="gzip")
            group.attrs["sample_rate"] = s.sample_rate
            group.attrs["packets"] = s.packets
            group.attrs["lost_packets"] = s.lost_packets
    print(f"Wrote {len(streams)} channel(s) to {args.output}")


def cmd_from_csv(args: argparse.Namespace) -> None:
    table = np.loadtxt(args.input, delimiter=",", skiprows=1, ndmin=2)
    streams = {}
    for channel in np.unique(table[:, 0]).astype(int):
        rows = table[table[:, 0] == channel]
        streams[int(channel)] = Stream(int(rows[0, 1]), rows[:, 2], rows[:, 3].astype(np.float32))
    count = write_capture(streams, args.output, args.block)
    print(f"Wrote {count} packets of {len(streams)} channel(s) to {args.output}")


def cmd_from_hdf5(args: argparse.Namespace) -> None:
    h5py = import_h5py()
    streams = {}
    with h5py.File(args.input, "r") as f:
        for name, group in f.items():
            if not name.startswith("channel_"):
                continue
            streams[int(name.removeprefix("channel_"))] = Stream(
                int(group.attrs["sample_rate"]), group["time_s"][()], group["samples"][()])
    count = write_capture(streams, args.output, args.block)
    print(f"Wrote {count} packets of {len(streams)} channel(s) to {args.output}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command", required=True)

    p = sub.add_parser("info", help="summary of a capture")
    p.add_argument("capture", type=Path)
    p.set_defaults(func=cmd_info)

    for name, func, help_text in (("to-csv", cmd_to_csv, "capture -> CSV"),
                                  ("to-hdf5", cmd_to_hdf5, "capture -> HDF5")):
        p = sub.add_parser(name, help=help_text)
        p.add_argument("capture", type=Path)
        p.add_argument("output", type=Path)
        p.set_defaults(func=func)

    for name, func, help_text in (("from-csv", cmd_from_csv, "CSV -> capture"),
                                  ("from-hdf5", cmd_from_hdf5, "HDF5 -> capture")):
        p = sub.add_parser(name, help=help_text)
        p.add_argument("input", type=Path)
        p.add_argument("output", type=Path)
        p.add_argument("--block", type=int, default=500, help="samples per packet (default 500)")
        p.set_defaults(func=func)

    args = ap.parse_args()
    args.func(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
