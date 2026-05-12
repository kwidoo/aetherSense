#!/usr/bin/env python3
"""
tools/collector.py – minimal host-side collector for ESP32 Wi-Fi RSSI/CSI sensor.

Usage:
    python tools/collector.py --port /dev/tty.usbmodemXXXX --baud 921600 --out rssi.csv

The script reads the binary record stream from the firmware, decodes RSSI,
CSI header, and STATUS records, prints a human-readable summary, and
optionally writes RSSI records to a CSV file.

Protocol summary (little-endian, all structs packed):
  Every record starts with: magic=0xA55A (2B), version (1B), record_type (1B)
  record_type 1 = RSSI    (24 bytes total, no trailing payload)
  record_type 2 = CSI     (22-byte header + csi_len raw bytes)
  record_type 3 = STATUS  (23 bytes total)
"""

import argparse
import csv
import struct
import sys
import time
from pathlib import Path

try:
    import serial
except ImportError:
    print("pyserial not found. Install with:  pip install pyserial")
    sys.exit(1)

# ── Record formats (must match record_protocol.h) ────────────────────────────
MAGIC           = 0xA55A
PROTO_VERSION   = 1

RECORD_TYPE_RSSI   = 1
RECORD_TYPE_CSI    = 2
RECORD_TYPE_STATUS = 3

# struct rssi_record (24 bytes)
# magic(H) version(B) record_type(B) seq(I) timestamp_us(I)
# channel(B) rssi(b) frame_type(B) frame_subtype(B)
# mac(6s) payload_len(H) crc16(H)
RSSI_FMT  = "<HBBIIBbBB6sHH"
RSSI_SIZE = struct.calcsize(RSSI_FMT)  # 24

# struct csi_record_header (22 bytes)
# magic(H) version(B) record_type(B) seq(I) timestamp_us(I)
# channel(B) rssi(b) mac(6s) csi_len(H) crc16(H)
CSI_HDR_FMT  = "<HBBIIBb6sHH"
CSI_HDR_SIZE = struct.calcsize(CSI_HDR_FMT)  # 22

# struct status_record (23 bytes)
# magic(H) version(B) record_type(B) uptime_ms(I) rssi_sent(I)
# csi_sent(I) dropped(I) high_watermark(I) channel(B) crc16(H)
STATUS_FMT  = "<HBBIIIIIbH"
STATUS_SIZE = struct.calcsize(STATUS_FMT)  # 23

assert RSSI_SIZE   == 24, f"RSSI size mismatch: {RSSI_SIZE}"
assert CSI_HDR_SIZE == 22, f"CSI header size mismatch: {CSI_HDR_SIZE}"
assert STATUS_SIZE  == 23, f"STATUS size mismatch: {STATUS_SIZE}"

# ── CRC-16/CCITT-FALSE ────────────────────────────────────────────────────────
def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = (crc << 1) ^ 0x1021 if (crc & 0x8000) else (crc << 1)
        crc &= 0xFFFF
    return crc


# ── Read exactly n bytes from serial ─────────────────────────────────────────
def read_exact(port: serial.Serial, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = port.read(n - len(buf))
        if not chunk:
            raise EOFError("Serial port closed or timed out")
        buf += chunk
    return buf


# ── Sync to next magic ────────────────────────────────────────────────────────
def sync_to_magic(port: serial.Serial) -> bytes:
    """Read one byte at a time until we see 0x5A 0xA5 (little-endian 0xA55A)."""
    buf = b"\x00\x00"
    discarded = 0
    while True:
        b = port.read(1)
        if not b:
            raise EOFError("Serial port closed while syncing")
        buf = buf[1:] + b
        if buf == b"\x5a\xa5":
            return buf
        discarded += 1
        if discarded % 100 == 0:
            print(f"[sync] discarded {discarded} bytes searching for magic...",
                  file=sys.stderr)


# ── Record parsers ────────────────────────────────────────────────────────────
def parse_rssi(raw: bytes):
    fields = struct.unpack(RSSI_FMT, raw)
    magic, version, rtype, seq, ts_us, ch, rssi, ftype, fsubtype, mac_bytes, plen, crc = fields
    mac = ":".join(f"{b:02x}" for b in mac_bytes)
    return {
        "type": "RSSI",
        "seq": seq,
        "timestamp_us": ts_us,
        "channel": ch,
        "rssi": rssi,
        "frame_type": ftype,
        "frame_subtype": fsubtype,
        "mac": mac,
        "payload_len": plen,
        "crc_ok": crc == crc16_ccitt(raw[:-2]),
    }


def parse_csi_header(raw_hdr: bytes, payload: bytes):
    fields = struct.unpack(CSI_HDR_FMT, raw_hdr)
    magic, version, rtype, seq, ts_us, ch, rssi, mac_bytes, csi_len, crc = fields
    mac = ":".join(f"{b:02x}" for b in mac_bytes)
    return {
        "type": "CSI",
        "seq": seq,
        "timestamp_us": ts_us,
        "channel": ch,
        "rssi": rssi,
        "mac": mac,
        "csi_len": csi_len,
        "crc_ok": crc == crc16_ccitt(raw_hdr[:-2]),
        # raw CSI bytes stored but not decoded here
        "csi_payload": payload,
    }


def parse_status(raw: bytes):
    fields = struct.unpack(STATUS_FMT, raw)
    magic, version, rtype, uptime_ms, rssi_sent, csi_sent, dropped, hwm, ch, crc = fields
    return {
        "type": "STATUS",
        "uptime_ms": uptime_ms,
        "rssi_records_sent": rssi_sent,
        "csi_records_sent": csi_sent,
        "records_dropped": dropped,
        "queue_high_watermark": hwm,
        "channel": ch,
        "crc_ok": crc == crc16_ccitt(raw[:-2]),
    }


# ── Main loop ─────────────────────────────────────────────────────────────────
def run(port_name: str, baud: int, out_csv: str | None):
    csv_file = None
    writer = None
    if out_csv:
        csv_file = open(out_csv, "w", newline="")
        writer = csv.writer(csv_file)
        writer.writerow(["timestamp_us", "seq", "channel", "rssi",
                         "frame_type", "frame_subtype", "mac"])

    print(f"Opening {port_name} at {baud} baud …")
    with serial.Serial(port_name, baud, timeout=2.0) as port:
        print("Connected. Waiting for records …\n")
        rssi_count = csi_count = status_count = bad_crc = 0
        start = time.time()

        while True:
            # Sync to magic bytes
            magic_bytes = sync_to_magic(port)

            # Read version + record_type
            hdr2 = read_exact(port, 2)
            version, rtype = struct.unpack("BB", hdr2)

            if rtype == RECORD_TYPE_RSSI:
                rest = read_exact(port, RSSI_SIZE - 4)
                raw = magic_bytes + hdr2 + rest
                rec = parse_rssi(raw)
                rssi_count += 1
                if not rec["crc_ok"]:
                    bad_crc += 1
                print(f"RSSI  seq={rec['seq']:6d} ch={rec['channel']:2d} "
                      f"rssi={rec['rssi']:4d} dBm  mac={rec['mac']}  "
                      f"type={rec['frame_type']}/{rec['frame_subtype']}  "
                      f"crc={'ok' if rec['crc_ok'] else 'BAD'}")
                if writer:
                    writer.writerow([rec["timestamp_us"], rec["seq"],
                                     rec["channel"], rec["rssi"],
                                     rec["frame_type"], rec["frame_subtype"],
                                     rec["mac"]])
                    csv_file.flush()

            elif rtype == RECORD_TYPE_CSI:
                rest_hdr = read_exact(port, CSI_HDR_SIZE - 4)
                raw_hdr = magic_bytes + hdr2 + rest_hdr
                # csi_len is the last 4 bytes before crc16 in header
                csi_len = struct.unpack_from("<H", raw_hdr, CSI_HDR_SIZE - 4)[0]
                payload = read_exact(port, csi_len)
                rec = parse_csi_header(raw_hdr, payload)
                csi_count += 1
                if not rec["crc_ok"]:
                    bad_crc += 1
                print(f"CSI   seq={rec['seq']:6d} ch={rec['channel']:2d} "
                      f"rssi={rec['rssi']:4d} dBm  mac={rec['mac']}  "
                      f"len={rec['csi_len']}  crc={'ok' if rec['crc_ok'] else 'BAD'}")

            elif rtype == RECORD_TYPE_STATUS:
                rest = read_exact(port, STATUS_SIZE - 4)
                raw = magic_bytes + hdr2 + rest
                rec = parse_status(raw)
                status_count += 1
                if not rec["crc_ok"]:
                    bad_crc += 1
                elapsed = time.time() - start
                print(
                    f"\n── STATUS  uptime={rec['uptime_ms']/1000:.1f}s  "
                    f"rssi_sent={rec['rssi_records_sent']}  "
                    f"csi_sent={rec['csi_records_sent']}  "
                    f"dropped={rec['records_dropped']}  "
                    f"hwm={rec['queue_high_watermark']}  "
                    f"ch={rec['channel']}  "
                    f"[host: {rssi_count}R {csi_count}C {bad_crc}bad "
                    f"in {elapsed:.1f}s]\n"
                )
            else:
                print(f"[warn] unknown record_type={rtype}, re-syncing …",
                      file=sys.stderr)

    if csv_file:
        csv_file.close()


# ── CLI ───────────────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(
        description="ESP32 Wi-Fi RSSI/CSI sensor collector"
    )
    parser.add_argument("--port", required=True,
                        help="Serial port, e.g. /dev/tty.usbmodemXXXX")
    parser.add_argument("--baud", type=int, default=921600,
                        help="Baud rate (default: 921600)")
    parser.add_argument("--out", default=None,
                        help="CSV output file for RSSI records (optional)")
    args = parser.parse_args()

    try:
        run(args.port, args.baud, args.out)
    except KeyboardInterrupt:
        print("\nStopped by user.")
    except EOFError as e:
        print(f"\nSerial error: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
