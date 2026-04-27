#!/usr/bin/env python3
"""
JTDX CLI / debug bridge.

Listens on the WSJT-X UDP "server" port for outbound JTDX messages
(Status, Decode, QSO Logged, Close), and can also send inbound
commands (HaltTx, Replay, Reply).

JTDX must be configured to send UDP to 127.0.0.1:2237 (default).

Usage:
  jtdx_cli.py status          # wait for next Status message and print
  jtdx_cli.py watch           # stream Status/Decode/Logged messages
  jtdx_cli.py halt            # send Halt Tx (graceful, halts after this TX)
  jtdx_cli.py halt-now        # send Halt Tx (auto-only=False — immediate halt)
  jtdx_cli.py replay          # request JTDX to replay buffered decodes
  jtdx_cli.py raw <bytes>     # dump hex of next packet for debugging
  jtdx_cli.py listen-port [port]  # change UDP server port (default 2237)

Env:
  JTDX_HOST (default 127.0.0.1)
  JTDX_PORT (default 2237)
"""

import os
import socket
import struct
import sys
import time
from datetime import datetime, timezone

MAGIC = 0xadbccbda
SCHEMA = 3
DEFAULT_HOST = os.environ.get("JTDX_HOST", "127.0.0.1")
DEFAULT_PORT = int(os.environ.get("JTDX_PORT", "2237"))

MSG_NAMES = {
    0: "Heartbeat", 1: "Status", 2: "Decode", 3: "Clear", 4: "Reply",
    5: "QSOLogged", 6: "Close", 7: "Replay", 8: "HaltTx", 9: "FreeText",
    10: "WSPRDecode", 11: "Location", 12: "LoggedADIF",
    50: "SetTxDeltaFreq",
}


# --- QDataStream big-endian primitives ---

def _u8(buf, o):  return buf[o], o + 1
def _u32(buf, o): return struct.unpack(">I", buf[o:o+4])[0], o + 4
def _i32(buf, o): return struct.unpack(">i", buf[o:o+4])[0], o + 4
def _u64(buf, o): return struct.unpack(">Q", buf[o:o+8])[0], o + 8
def _bool(buf, o): return (buf[o] != 0), o + 1
def _f64(buf, o): return struct.unpack(">d", buf[o:o+8])[0], o + 8


def _qstr(buf, o):
    """Qt utf8 QString: 4-byte length, 0xffffffff = null. Empty = 0."""
    length, o = _u32(buf, o)
    if length == 0xffffffff:
        return None, o
    if length == 0:
        return "", o
    s = buf[o:o+length].decode("utf-8", errors="replace")
    return s, o + length


def _qbytes(buf, o):
    length, o = _u32(buf, o)
    if length == 0xffffffff:
        return b"", o
    return buf[o:o+length], o + length


def _qtime(buf, o):
    """QTime: ms-since-midnight as quint32."""
    ms, o = _u32(buf, o)
    if ms == 0xffffffff:
        return None, o
    h = ms // 3600000
    m = (ms // 60000) % 60
    s = (ms // 1000) % 60
    return f"{h:02d}:{m:02d}:{s:02d}", o


# --- Encoders for outbound (from us → JTDX) ---

def encode_qstr(s):
    if s is None:
        return struct.pack(">I", 0xffffffff)
    b = s.encode("utf-8")
    return struct.pack(">I", len(b)) + b


def make_packet(msg_type, client_id, *fields):
    body = struct.pack(">III", MAGIC, SCHEMA, msg_type)
    body += encode_qstr(client_id)
    for f in fields:
        body += f
    return body


# --- Status message decoder (the most useful one) ---

def decode_status(buf, o):
    out = {}
    out["id"], o = _qstr(buf, o)
    out["dial_freq"], o = _u64(buf, o)
    out["mode"], o = _qstr(buf, o)
    out["dx_call"], o = _qstr(buf, o)
    out["report"], o = _qstr(buf, o)
    out["tx_mode"], o = _qstr(buf, o)
    out["tx_enabled"], o = _bool(buf, o)
    out["transmitting"], o = _bool(buf, o)
    out["decoding"], o = _bool(buf, o)
    out["rx_df"], o = _i32(buf, o)
    out["tx_df"], o = _i32(buf, o)
    out["de_call"], o = _qstr(buf, o)
    out["de_grid"], o = _qstr(buf, o)
    out["dx_grid"], o = _qstr(buf, o)
    out["tx_watchdog"], o = _bool(buf, o)
    if o < len(buf):
        out["sub_mode"], o = _qstr(buf, o)
    if o < len(buf):
        out["fast_mode"], o = _bool(buf, o)
    if o < len(buf):
        out["tx_first"], o = _bool(buf, o)
    return out


def decode_decode(buf, o):
    out = {}
    out["id"], o = _qstr(buf, o)
    out["new"], o = _bool(buf, o)
    out["time"], o = _qtime(buf, o)
    out["snr"], o = _i32(buf, o)
    out["dt"], o = _f64(buf, o)
    out["df"], o = _u32(buf, o)
    out["mode"], o = _qstr(buf, o)
    out["msg"], o = _qstr(buf, o)
    if o < len(buf):
        out["low_conf"], o = _bool(buf, o)
    if o < len(buf):
        out["off_air"], o = _bool(buf, o)
    return out


def decode_packet(buf):
    if len(buf) < 12:
        return None, "too short"
    magic, _ = _u32(buf, 0)
    if magic != MAGIC:
        return None, f"bad magic 0x{magic:08x}"
    schema, _ = _u32(buf, 4)
    msg_type, _ = _u32(buf, 8)
    name = MSG_NAMES.get(msg_type, f"Type{msg_type}")
    o = 12
    payload = {"_type": name, "_schema": schema}
    try:
        if msg_type == 1:
            payload.update(decode_status(buf, o))
        elif msg_type == 2:
            payload.update(decode_decode(buf, o))
        else:
            cid, o = _qstr(buf, o)
            payload["id"] = cid
            payload["_raw_after_id"] = buf[o:].hex()
    except Exception as e:
        payload["_decode_error"] = repr(e)
    return payload, None


# --- Commands ---

def cmd_watch(host, port, only_status=False, timeout=None):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("", port))
    if timeout is not None:
        sock.settimeout(timeout)
    print(f"# Listening on udp/{port} (host filter: {host})", flush=True)
    try:
        while True:
            try:
                data, addr = sock.recvfrom(8192)
            except socket.timeout:
                print("# timeout, nothing received", flush=True)
                return 1
            pkt, err = decode_packet(data)
            if err:
                print(f"# {addr} error: {err} bytes={len(data)}", flush=True)
                continue
            if only_status and pkt.get("_type") != "Status":
                continue
            ts = datetime.now().strftime("%H:%M:%S")
            print(f"[{ts}] {addr[0]}:{addr[1]} {pkt}", flush=True)
            if only_status and pkt.get("_type") == "Status":
                return 0
    finally:
        sock.close()


def cmd_halt(host, port, auto_only=True):
    """Send a HaltTx (8) message. Needs a known client id, so we sniff one
    Status packet first to learn it."""
    listen = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    listen.bind(("", port))
    listen.settimeout(15)
    print(f"# Sniffing for JTDX client id on udp/{port}...")
    client_id = None
    src = None
    while client_id is None:
        try:
            data, addr = listen.recvfrom(8192)
        except socket.timeout:
            print("# no packet from JTDX in 15s — is UDP server enabled?", file=sys.stderr)
            return 2
        pkt, err = decode_packet(data)
        if err or "id" not in pkt:
            continue
        client_id = pkt["id"]
        src = addr
    listen.close()
    print(f"# Got client id={client_id!r} from {src}, sending HaltTx auto_only={auto_only}")
    pkt = make_packet(8, client_id, struct.pack(">B", 1 if auto_only else 0))
    sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    # Send back to whoever JTDX sent us status from — that's the server
    # endpoint configured in JTDX, so JTDX will be listening there.
    sender.sendto(pkt, src)
    sender.close()
    print("# HaltTx sent")
    return 0


def cmd_replay(host, port):
    listen = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    listen.bind(("", port))
    listen.settimeout(15)
    print(f"# Sniffing for JTDX client id on udp/{port}...")
    while True:
        try:
            data, addr = listen.recvfrom(8192)
        except socket.timeout:
            print("# no packet from JTDX in 15s", file=sys.stderr)
            return 2
        pkt, err = decode_packet(data)
        if err or "id" not in pkt:
            continue
        cid, src = pkt["id"], addr
        break
    listen.close()
    print(f"# Got client id={cid!r}, sending Replay")
    sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sender.sendto(make_packet(7, cid), src)
    sender.close()
    return 0


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    cmd = sys.argv[1]
    host, port = DEFAULT_HOST, DEFAULT_PORT
    if cmd == "status":
        return cmd_watch(host, port, only_status=True, timeout=15)
    if cmd == "watch":
        return cmd_watch(host, port)
    if cmd == "halt":
        return cmd_halt(host, port, auto_only=True)
    if cmd == "halt-now":
        return cmd_halt(host, port, auto_only=False)
    if cmd == "replay":
        return cmd_replay(host, port)
    print(__doc__)
    return 1


if __name__ == "__main__":
    sys.exit(main())
