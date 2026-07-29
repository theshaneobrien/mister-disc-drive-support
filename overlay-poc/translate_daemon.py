#!/usr/bin/env python3
"""MiSTer AI-translation PoC daemon (milestone 4).

Captures the current core frame straight from the scaler buffer in DDR3
(the same mmap Main's screenshot uses - passive, the core is untouched),
encodes it as PNG with nothing but stdlib, POSTs it to a RetroArch
AI-Service-compatible endpoint (vgtranslate / ztranslate / mock_server.py),
and routes the reply back through Main's FIFO verbs:

    "text"  -> osd_msg   (over the live game)
    "image" -> overlay_show /tmp/translated.png   (freeze-frame)

Runs on the stock MiSTer rootfs python3, stdlib only, as root.

Usage (on the MiSTer):
    python3 translate_daemon.py --server http://<pc>:4404 &
    echo image > /tmp/translate_cmd     # or: text / go / hide / quit

    python3 translate_daemon.py --server http://<pc>:4404 --once --mode text

Timings land in /tmp/overlay_perf.log alongside Main's own telemetry
(same CLOCK_MONOTONIC timebase, so the two interleave meaningfully).
"""

import argparse
import base64
import json
import mmap
import os
import struct
import sys
import time
import urllib.parse
import urllib.request
import zlib

SCALER_BASE = 0x20000000
SCALER_SIZE = 2048 * 3 * 1024
MISTER_CMD = "/dev/MiSTer_cmd"
PERF_LOG = "/tmp/overlay_perf.log"
TRANSLATED_PNG = "/tmp/translated.png"


def plog(msg):
    t = time.monotonic()
    line = "[%7d.%06d] %s\n" % (int(t), int((t % 1) * 1e6), msg)
    try:
        with open(PERF_LOG, "a") as f:
            f.write(line)
    except OSError:
        pass
    print(line, end="", flush=True)


def mister(cmd):
    with open(MISTER_CMD, "w") as f:
        f.write(cmd + "\n")


def capture():
    """Read the current frame from the scaler DDR3 buffer.

    Returns (width, height, out_w, out_h, rows[RGB888 bytes per row]).
    Retries a few times if the frame counter moved mid-copy (tearing).
    """
    with open("/dev/mem", "rb") as f:
        m = mmap.mmap(f.fileno(), SCALER_SIZE, mmap.MAP_SHARED,
                      mmap.PROT_READ, offset=SCALER_BASE)
        try:
            rows = None
            for _ in range(4):
                hdr = m[0:16]
                if hdr[0] != 1 or hdr[1] != 1:
                    raise RuntimeError("scaler buffer not valid - is a core running?")
                off = (hdr[2] << 8) | hdr[3]
                fc0 = hdr[5]
                w = (hdr[6] << 8) | hdr[7]
                h = (hdr[8] << 8) | hdr[9]
                line = (hdr[10] << 8) | hdr[11]
                ow = (hdr[12] << 8) | hdr[13]
                oh = (hdr[14] << 8) | hdr[15]
                if not (0 < w <= 2048 and 0 < h <= 1024):
                    raise RuntimeError("implausible frame %dx%d" % (w, h))
                rows = [m[off + y * line: off + y * line + w * 3] for y in range(h)]
                if m[5] == fc0:  # no new frame landed mid-copy
                    break
            return w, h, ow, oh, rows
        finally:
            m.close()


def png_encode(w, h, rows, level=1):
    """Minimal PNG writer (8-bit RGB, no filter). stdlib only."""
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    raw = b"".join(b"\x00" + r for r in rows)
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", zlib.compress(raw, level))
            + chunk(b"IEND", b""))


def sanitize_osd(text):
    """The OSD charfont is 8x8 ASCII - fold anything else, escape for osd_msg."""
    t = text.encode("ascii", "replace").decode()
    t = t.replace("\r", "").replace("\n", "\\n")
    return t.strip() or "(empty reply)"


def translate_once(args, mode):
    t0 = time.monotonic()
    try:
        w, h, ow, oh, rows = capture()
    except (RuntimeError, OSError) as e:
        plog("translate: capture FAILED: %s" % e)
        mister("osd_msg -t 4000 AI: capture failed")
        return
    t_cap = time.monotonic()

    # anamorphic hi-res modes (e.g. SNES 512x224) squish glyphs; give the
    # OCR square-ish pixels by doubling rows. Cheap: pure row duplication.
    aspect = ""
    if w >= 2 * h:
        rows = [r for r in rows for _ in (0, 1)]
        h *= 2
        aspect = " rowdoubled"

    png = png_encode(w, h, rows, level=args.png_level)
    t_png = time.monotonic()

    body = json.dumps({
        "image": base64.b64encode(png).decode(),
        "label": "MiSTer__overlay_poc",
        "state": {"paused": 0},
    }).encode()

    q = {"output": "image,png" if mode == "image" else "text"}
    if args.source:
        q["source_lang"] = args.source
    if args.target:
        q["target_lang"] = args.target
    url = args.server + ("&" if "?" in args.server else "?") + urllib.parse.urlencode(q)

    try:
        req = urllib.request.Request(url, data=body,
                                     headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=args.timeout) as resp:
            reply = json.loads(resp.read().decode())
    except Exception as e:  # URLError, timeout, bad JSON - all end the same way
        plog("translate: POST FAILED: %s" % e)
        mister("osd_msg -t 5000 AI: server error\\n%s" % sanitize_osd(str(e))[:60])
        return
    t_post = time.monotonic()

    routed = "nothing"
    if reply.get("error") and reply["error"] != "No text found.":
        mister("osd_msg -t 5000 AI: %s" % sanitize_osd(reply["error"]))
        routed = "error"
    elif mode == "image" and reply.get("image"):
        with open(TRANSLATED_PNG, "wb") as f:
            f.write(base64.b64decode(reply["image"]))
        mister("overlay_show " + TRANSLATED_PNG)
        routed = "image(%dB)" % len(reply["image"])
    elif reply.get("text"):
        mister("osd_msg -f 1 -t %d %s" % (args.osd_ms, sanitize_osd(reply["text"])))
        routed = "text(%dch)" % len(reply["text"])
    elif reply.get("error"):  # "No text found."
        mister("osd_msg -t 3000 AI: no text found")
        routed = "notext"

    t_done = time.monotonic()
    plog("translate: %dx%d%s png=%dB cap=%dms png=%dms post=%dms route=%s total=%dms"
         % (w, h, aspect, len(png),
            (t_cap - t0) * 1000, (t_png - t_cap) * 1000,
            (t_post - t_png) * 1000, routed, (t_done - t0) * 1000))


def main():
    ap = argparse.ArgumentParser(description="MiSTer AI-translation PoC daemon")
    ap.add_argument("--server", default="http://127.0.0.1:4404",
                    help="AI-Service endpoint (vgtranslate/ztranslate/mock)")
    ap.add_argument("--mode", choices=["text", "image"], default="image",
                    help="default output mode for 'go' (default: image)")
    ap.add_argument("--source", default="ja", help="source language ('' = auto)")
    ap.add_argument("--target", default="en", help="target language")
    ap.add_argument("--timeout", type=float, default=15.0, help="server timeout (s)")
    ap.add_argument("--png-level", type=int, default=1, help="zlib level (1=fast)")
    ap.add_argument("--osd-ms", type=int, default=8000, help="osd_msg display time")
    ap.add_argument("--fifo", default="/tmp/translate_cmd", help="trigger fifo")
    ap.add_argument("--once", action="store_true",
                    help="single translate (using --mode) then exit; no fifo")
    args = ap.parse_args()

    if args.once:
        translate_once(args, args.mode)
        return

    if not os.path.exists(args.fifo):
        os.mkfifo(args.fifo)
    plog("translate: daemon up server=%s mode=%s fifo=%s" %
         (args.server, args.mode, args.fifo))

    while True:
        # open() blocks until a writer appears; EOF when it closes - reopen
        with open(args.fifo) as f:
            for raw in f:
                cmd = raw.strip()
                if not cmd:
                    continue
                if cmd == "quit":
                    plog("translate: daemon quit")
                    return
                elif cmd == "hide":
                    mister("overlay_hide")
                elif cmd in ("go", "text", "image"):
                    translate_once(args, args.mode if cmd == "go" else cmd)
                else:
                    plog("translate: unknown cmd '%s'" % cmd)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
