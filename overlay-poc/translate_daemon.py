#!/usr/bin/env python3
"""MiSTer AI-translation PoC daemon (milestones 4+).

Captures the current core frame straight from the scaler buffer in DDR3
(passive mmap - the core is untouched), and translates it via one of two
backends, both usable WITHOUT self-hosting anything:

  --backend service   (default) RetroArch-AI-Service protocol POST.
                      Works with the hosted ztranslate.net (free account +
                      API key -> full IMAGE mode, translation rendered
                      server-side), a LAN vgtranslate, or mock_server.py:
                        --server "https://ztranslate.net/service?api_key=KEY"
                        --server "http://<pc>:4404"

  --backend google    Direct-to-cloud: Google Vision OCR + Google Translate
                      REST APIs, called straight from the MiSTer. No server
                      anywhere. TEXT mode: result appears as an OSD toast
                      over the running game, auto-placed opposite the
                      detected text. Needs --google-key (free tier: 1k OCR
                      calls + 500k translated chars/month).

Replies route through Main's FIFO verbs: text -> osd_msg (live game),
image -> overlay_show /tmp/translated.png (freeze-frame).

Runs on the stock MiSTer rootfs python3, stdlib only, as root.

Usage:
    python3 translate_daemon.py --server http://<pc>:4404 &
    python3 translate_daemon.py --backend google --google-key AIza... &
    echo image > /tmp/translate_cmd     # or: text / go / hide / quit

Timings land in /tmp/overlay_perf.log on the same monotonic timebase as
Main's telemetry.
"""

import argparse
import base64
import json
import mmap
import os
import ssl
import struct
import sys
import textwrap
import time
import urllib.error
import urllib.parse
import urllib.request
import zlib

SCALER_BASE = 0x20000000
SCALER_SIZE = 2048 * 3 * 1024
MISTER_CMD = "/dev/MiSTer_cmd"
PERF_LOG = "/tmp/overlay_perf.log"
TRANSLATED_PNG = "/tmp/translated.png"
CA_FALLBACK = "/etc/ssl/certs/cacert.pem"  # MiSTer rootfs CA bundle

VISION_URL = "https://vision.googleapis.com/v1/images:annotate?key=%s"
GTRANSLATE_URL = "https://translation.googleapis.com/language/translate/v2?key=%s"


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


def http_post_json(url, obj, timeout):
    """POST JSON, return parsed JSON reply. Falls back to the rootfs CA
    bundle if python's default CA path is empty on this image."""
    data = json.dumps(obj).encode()
    req = urllib.request.Request(url, data=data,
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        detail = ""
        try:
            detail = e.read(300).decode(errors="replace")
        except OSError:
            pass
        raise RuntimeError("HTTP %d: %s" % (e.code, detail)) from None
    except urllib.error.URLError as e:
        if isinstance(getattr(e, "reason", None), ssl.SSLCertVerificationError) \
                and os.path.exists(CA_FALLBACK):
            ctx = ssl.create_default_context(cafile=CA_FALLBACK)
            with urllib.request.urlopen(req, timeout=timeout, context=ctx) as r:
                return json.loads(r.read().decode())
        raise


def capture_png(args):
    """Grab a frame, fix anamorphic aspect, PNG it.
    Returns (png_bytes, w, h, rowdoubled, native_h)."""
    w, h, ow, oh, rows = capture()
    native_h = h
    rowdoubled = False
    # anamorphic hi-res modes (e.g. SNES 512x224) squish glyphs; give the
    # OCR square-ish pixels by doubling rows. Cheap: pure row duplication.
    if w >= 2 * h:
        rows = [r for r in rows for _ in (0, 1)]
        h *= 2
        rowdoubled = True
    return png_encode(w, h, rows, level=args.png_level), w, h, rowdoubled, native_h


def wrap_for_osd(text, width=30, max_lines=14):
    """The OSD info window is 32 chars wide, 16 lines tall (minus frame)."""
    lines = []
    for para in text.replace("\r", "").split("\n"):
        lines.extend(textwrap.wrap(para, width) or [""])
    if len(lines) > max_lines:
        lines = lines[:max_lines - 1] + ["..."]
    return "\n".join(lines).strip()


def sanitize_osd(text):
    """The OSD charfont is 8x8 ASCII - fold anything else, escape for osd_msg."""
    t = text.encode("ascii", "replace").decode()
    t = t.replace("\r", "").replace("\n", "\\n")
    return t.strip() or "(empty reply)"


def osd_show(args, text, y=None):
    pos = ("-y %d " % y) if y is not None else ""
    mister("osd_msg -f 1 -t %d %s%s" % (args.osd_ms, pos, sanitize_osd(text)))


# ---------------------------------------------------------------- backends

def backend_service(args, mode, png):
    """RetroArch-AI-Service protocol (ztranslate / vgtranslate / mock)."""
    body = {
        "image": base64.b64encode(png).decode(),
        "label": "MiSTer__overlay_poc",
        "state": {"paused": 0},
    }
    q = {"output": "image,png" if mode == "image" else "text"}
    if args.source:
        q["source_lang"] = args.source
    if args.target:
        q["target_lang"] = args.target
    url = args.server + ("&" if "?" in args.server else "?") + urllib.parse.urlencode(q)

    reply = http_post_json(url, body, args.timeout)

    if reply.get("error") and reply["error"] != "No text found.":
        osd_show(args, "AI: " + reply["error"])
        return "error"
    if mode == "image" and reply.get("image"):
        with open(TRANSLATED_PNG, "wb") as f:
            f.write(base64.b64decode(reply["image"]))
        mister("overlay_show " + TRANSLATED_PNG)
        return "image(%dB)" % len(reply["image"])
    if reply.get("text"):
        osd_show(args, wrap_for_osd(reply["text"]))
        return "text(%dch)" % len(reply["text"])
    if reply.get("error"):
        mister("osd_msg -t 3000 AI: no text found")
        return "notext"
    return "nothing"


def backend_google(args, mode, png, rowdoubled, native_h):
    """Direct-to-cloud: Vision OCR then Translate, result as OSD text.
    (image mode needs server-side rendering - use ztranslate for that.)"""
    t0 = time.monotonic()

    vreq = {"requests": [{
        "image": {"content": base64.b64encode(png).decode()},
        "features": [{"type": "TEXT_DETECTION"}],
    }]}
    if args.source:
        vreq["requests"][0]["imageContext"] = {"languageHints": [args.source]}

    vresp = http_post_json(VISION_URL % args.google_key, vreq, args.timeout)
    r0 = (vresp.get("responses") or [{}])[0]
    if "error" in r0:
        raise RuntimeError("Vision: %s" % r0["error"].get("message", "?"))
    full = r0.get("fullTextAnnotation", {}).get("text", "").strip()
    t_vision = time.monotonic()

    if not full:
        mister("osd_msg -t 3000 AI: no text found")
        return "notext (vision=%dms)" % ((t_vision - t0) * 1000)

    # where is the text? place the OSD toast in the opposite half so the
    # translation doesn't sit on top of the original dialogue box
    y_osd = None
    try:
        verts = r0["textAnnotations"][0]["boundingPoly"]["vertices"]
        ys = [v.get("y", 0) for v in verts]
        mid = (min(ys) + max(ys)) / 2.0
        if rowdoubled:
            mid /= 2.0
        y_osd = 10 if mid > native_h / 2.0 else max(10, native_h - 70)
    except (KeyError, IndexError):
        pass

    treq = {"q": full, "target": args.target or "en", "format": "text"}
    if args.source:
        treq["source"] = args.source
    tresp = http_post_json(GTRANSLATE_URL % args.google_key, treq, args.timeout)
    translated = tresp["data"]["translations"][0]["translatedText"]
    t_trans = time.monotonic()

    osd_show(args, wrap_for_osd(translated), y=y_osd)
    return "text(%dch) vision=%dms translate=%dms osd_y=%s" % (
        len(translated), (t_vision - t0) * 1000, (t_trans - t_vision) * 1000, y_osd)


# ---------------------------------------------------------------- pipeline

def translate_once(args, mode):
    t0 = time.monotonic()
    try:
        png, w, h, rowdoubled, native_h = capture_png(args)
    except (RuntimeError, OSError) as e:
        plog("translate: capture FAILED: %s" % e)
        mister("osd_msg -t 4000 AI: capture failed")
        return
    t_png = time.monotonic()

    try:
        if args.backend == "google":
            routed = backend_google(args, mode, png, rowdoubled, native_h)
        else:
            routed = backend_service(args, mode, png)
    except Exception as e:  # HTTP/ssl/JSON shape - all end the same way
        plog("translate: backend FAILED: %s" % e)
        osd_show(args, "AI: server error\n" + str(e)[:80])
        return

    t_done = time.monotonic()
    plog("translate: %dx%d%s png=%dB cap+png=%dms backend=%s route=%s total=%dms"
         % (w, h, " rowdoubled" if rowdoubled else "", len(png),
            (t_png - t0) * 1000, args.backend, routed, (t_done - t0) * 1000))


def main():
    ap = argparse.ArgumentParser(description="MiSTer AI-translation PoC daemon")
    ap.add_argument("--backend", choices=["service", "google"], default="service",
                    help="service = AI-Service protocol URL (ztranslate/vgtranslate/"
                         "mock); google = direct Vision+Translate, no server")
    ap.add_argument("--server", default="http://127.0.0.1:4404",
                    help="AI-Service endpoint for --backend service")
    ap.add_argument("--google-key", default="",
                    help="Google Cloud API key for --backend google")
    ap.add_argument("--mode", choices=["text", "image"], default="image",
                    help="default output mode for 'go' (default: image)")
    ap.add_argument("--source", default="ja", help="source language ('' = auto)")
    ap.add_argument("--target", default="en", help="target language")
    ap.add_argument("--timeout", type=float, default=15.0, help="per-request timeout (s)")
    ap.add_argument("--png-level", type=int, default=1, help="zlib level (1=fast)")
    ap.add_argument("--osd-ms", type=int, default=8000, help="osd_msg display time")
    ap.add_argument("--fifo", default="/tmp/translate_cmd", help="trigger fifo")
    ap.add_argument("--once", action="store_true",
                    help="single translate (using --mode) then exit; no fifo")
    args = ap.parse_args()

    if args.backend == "google":
        if not args.google_key:
            sys.exit("--backend google needs --google-key (console.cloud.google.com,"
                     " enable Vision + Translation APIs)")
        args.mode = "text"  # image mode needs server-side rendering

    if args.once:
        translate_once(args, args.mode)
        return

    if not os.path.exists(args.fifo):
        os.mkfifo(args.fifo)
    plog("translate: daemon up backend=%s server=%s mode=%s fifo=%s" %
         (args.backend, args.server if args.backend == "service" else "google-api",
          args.mode, args.fifo))

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
