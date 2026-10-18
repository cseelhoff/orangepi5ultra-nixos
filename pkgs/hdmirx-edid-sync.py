"""hdmirx-edid-sync: make the HDMI-in EDID carry the identity of the monitor
on HDMI-out (manufacturer, model, serial, date, name), or a generic identity
when no monitor is connected. Capabilities (4K60, audio, ...) always come from
the base EDID, because the board scales whatever arrives to fit the output.

The receiver drops and re-raises hotplug when its EDID is written, so the
EDID is only written when it changes.

Usage: hdmirx-edid-sync [--base FILE] [--dry-run] [--print]
"""
import argparse
import glob
import os
import subprocess
import sys
import tempfile

GENERIC_MFG = "LNX"   # PNP ID used by the Linux kernel's generic EDIDs
GENERIC_PRODUCT = 0x0001
GENERIC_NAME = "Generic 4K60"


def log(msg):
    print(msg, file=sys.stderr, flush=True)


def checksum(block):
    block[127] = (-sum(block[:127])) & 0xFF


def pnp(code):
    v = 0
    for c in code:
        v = (v << 5) | (ord(c) - 64)
    return bytes([v >> 8, v & 0xFF])


def mfg_str(edid):
    v = (edid[8] << 8) | edid[9]
    return "".join(chr(((v >> s) & 0x1F) + 64) for s in (10, 5, 0))


def descriptor_text(edid, tag):
    for off in range(54, 126, 18):
        d = edid[off:off + 18]
        if d[0:3] == b"\0\0\0" and d[3] == tag:
            return d[5:18].split(b"\n")[0].decode("ascii", "replace").strip()
    return None


def set_descriptor_text(block, tag, text):
    """Replace the text of descriptor `tag`; returns False if there is none."""
    raw = text.encode("ascii", "replace")[:13]
    if len(raw) < 13:
        raw = raw + b"\n" + b" " * (12 - len(raw))
    for off in range(54, 126, 18):
        if block[off:off + 3] == b"\0\0\0" and block[off + 3] == tag:
            block[off + 5:off + 18] = raw
            return True
    return False


def monitor_edid():
    """EDID of the first connected HDMI output, or None."""
    for status in sorted(glob.glob("/sys/class/drm/card*-HDMI-A-*/status")):
        conn = os.path.dirname(status)
        try:
            if open(status).read().strip() != "connected":
                continue
            data = open(os.path.join(conn, "edid"), "rb").read()
        except OSError:
            continue
        if len(data) >= 128 and data[:8] == bytes.fromhex("00ffffffffffff00"):
            return os.path.basename(conn), data
    return None, None


def hdmirx_device():
    for name in sorted(glob.glob("/sys/class/video4linux/*/name")):
        if "hdmirx" in open(name).read():
            return "/dev/" + os.path.basename(os.path.dirname(name))
    return None


def build(base, mon):
    edid = bytearray(base)
    b0 = edid[:128]
    if mon is not None:
        # manufacturer, product code, serial number, week/year of manufacture
        b0[8:18] = mon[8:18]
        name = descriptor_text(mon, 0xFC) or "HDMI"
        serial = descriptor_text(mon, 0xFF)
    else:
        b0[8:10] = pnp(GENERIC_MFG)
        b0[10:12] = GENERIC_PRODUCT.to_bytes(2, "little")
        b0[12:16] = b"\0\0\0\0"
        name, serial = GENERIC_NAME, None
    set_descriptor_text(b0, 0xFC, name)
    if serial:
        set_descriptor_text(b0, 0xFF, serial)  # only if the base has one
    checksum(b0)
    edid[:128] = b0
    return bytes(edid), name


def v4l2(*args):
    return subprocess.run(["v4l2-ctl", *args], capture_output=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default=os.environ.get("HDMIRX_EDID_BASE"))
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--print", action="store_true", help="write the new EDID to stdout")
    a = ap.parse_args()
    if not a.base:
        sys.exit("no base EDID (--base or HDMIRX_EDID_BASE)")
    base = open(a.base, "rb").read()

    conn, mon = monitor_edid()
    new, name = build(base, mon)
    who = f"{conn}: {mfg_str(mon)} {int.from_bytes(mon[10:12], 'little')} '{name}'" if mon else f"no monitor: generic '{name}'"
    if a.print:
        sys.stdout.buffer.write(new)
        return

    dev = hdmirx_device()
    if not dev:
        sys.exit("no HDMI-in device")
    with tempfile.TemporaryDirectory() as tmp:
        cur = os.path.join(tmp, "cur.bin")
        r = v4l2("-d", dev, f"--get-edid=file={cur},format=raw")
        if r.returncode == 0 and os.path.exists(cur) and open(cur, "rb").read() == new:
            log(f"{dev} EDID already matches ({who})")
            return
        if a.dry_run:
            log(f"would set {dev} EDID ({who})")
            return
        path = os.path.join(tmp, "new.bin")
        open(path, "wb").write(new)
        r = v4l2("-d", dev, f"--set-edid=file={path},format=raw")
        if r.returncode:
            sys.exit(f"setting EDID failed: {r.stderr.decode().strip()}")
    log(f"set {dev} EDID ({who})")


if __name__ == "__main__":
    main()
