#!/usr/bin/env python3
"""MiSTer on-the-fly translation daemon.

Captures the current core frame straight from the scaler buffer in DDR3
(passive mmap - the core is untouched) and POSTs it to a RetroArch
AI-Service-protocol endpoint. The de-facto backend is the hosted
ztranslate.net (free account + API key -> full IMAGE mode, translation
rendered server-side); any protocol-compatible server works the same:

    SERVER=https://ztranslate.net/service   (+ API_KEY=...)
    SERVER=http://<lan-box>:4404            (vgtranslate / mock / own server)

Replies route through Main's FIFO verbs: text -> osd_msg (live game),
image -> overlay_show of a rotating /tmp/translated_N.png (freeze-frame).

Runs on the stock MiSTer rootfs python3, stdlib only, as root. Settings
come from translate.ini (see --config); CLI args override it.

Usage:
    python3 translate_daemon.py                  # settings from translate.ini
    python3 translate_daemon.py --server http://<pc>:4404 &
    echo image > /tmp/translate_cmd   # or: text/go/hide/pause/resume/quit

Timings land in /tmp/overlay_perf.log on the same monotonic timebase as
Main's telemetry.
"""

import argparse
import base64
import json
import mmap
import os
import re
import signal
import ssl
import stat
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
# unique path per translation: Main's imlib2 caches decoded images BY FILE
# PATH, so rewriting one filename displayed translation #1 forever (fixed
# properly in Main overlaypoc5 via decache; rotating names keeps older
# binaries honest too, and never reuses a path within a boot)
TRANSLATED_PNG_FMT = "/tmp/translated_%06d.png"
CA_FALLBACK = "/etc/ssl/certs/cacert.pem"  # MiSTer rootfs CA bundle


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


def capture(stale_timeout=0.35):
    """Read the current frame from the scaler DDR3 buffer.

    Returns (width, height, out_w, out_h, rows[RGB888 bytes per row]).
    Retries a few times if the frame counter moved mid-copy (tearing).

    The scaler rewrites this buffer every core frame, so its counter must
    tick. A static counter means we'd ship a stale frame - seen on
    hardware as capturing our own displayed overlay and re-translating it
    forever. Better to refuse (and say so) than to spend quota on it.

    O_SYNC matters: Main's shmem_map opens /dev/mem with it, which maps
    the region uncached. The FPGA writes this RAM behind the CPU's back,
    so a cacheable mapping can serve stale lines indefinitely - even
    across daemon restarts (hardware caches outlive processes). Suspected
    cause of the frozen-capture bug seen on hardware 2026-07-29.
    """
    fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
    try:
        m = mmap.mmap(fd, SCALER_SIZE, mmap.MAP_SHARED,
                      mmap.PROT_READ, offset=SCALER_BASE)
    finally:
        os.close(fd)

    try:
        if m[0] != 1 or m[1] != 1:
            raise RuntimeError("scaler buffer not valid - is a core running?")
        fc = m[5]
        deadline = time.monotonic() + stale_timeout
        while m[5] == fc:
            if time.monotonic() > deadline:
                raise RuntimeError(
                    "scaler buffer not updating (frame counter static at %d)"
                    " - overlay still shown, or the scaler dump is wedged" % fc)
            time.sleep(0.01)

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
    Returns (png_bytes, w, h, rowdoubled)."""
    w, h, ow, oh, rows = capture()
    rowdoubled = False
    # anamorphic hi-res modes (e.g. SNES 512x224) squish glyphs; give the
    # OCR square-ish pixels by doubling rows. Cheap: pure row duplication.
    if w >= 2 * h:
        rows = [r for r in rows for _ in (0, 1)]
        h *= 2
        rowdoubled = True
    return png_encode(w, h, rows, level=args.png_level), w, h, rowdoubled


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


def osd_show(args, text):
    mister("osd_msg -f 1 -t %d %s" % (args.osd_ms, sanitize_osd(text)))


# ---------------------------------------------------------------- backend

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
    if args.zt_mode:
        q["mode"] = args.zt_mode  # ztranslate: normal (default) | fast
    url = args.server + ("&" if "?" in args.server else "?") + urllib.parse.urlencode(q)

    reply = http_post_json(url, body, args.timeout)

    if reply.get("error") and reply["error"] != "No text found.":
        osd_show(args, "AI: " + reply["error"])
        return "error"
    if mode == "image" and reply.get("image"):
        _shot_seq[0] += 1
        path = TRANSLATED_PNG_FMT % _shot_seq[0]
        with open(path, "wb") as f:
            f.write(base64.b64decode(reply["image"]))
        mister("overlay_show " + path)
        _overlay_shown[0] = True
        if _shot_seq[0] > 1:
            try:
                os.unlink(TRANSLATED_PNG_FMT % (_shot_seq[0] - 1))
            except OSError:
                pass
        return "image(%dB) -> %s" % (len(reply["image"]), path)
    if reply.get("text"):
        osd_show(args, wrap_for_osd(reply["text"]))
        return "text(%dch)" % len(reply["text"])
    if reply.get("error"):
        mister("osd_msg -t 3000 AI: no text found")
        return "notext"
    return "nothing"


# ---------------------------------------------------------------- pipeline

def ensure_fifo(path):
    """Make sure the trigger path is a REAL fifo.

    If `echo cmd > path` runs before the daemon starts, the shell creates a
    plain file - and reading a plain file in the trigger loop replays its
    content forever, one API call per round trip (the ztranslate hammer
    incident). A stale non-fifo gets replaced, so start order is harmless.
    """
    try:
        if not stat.S_ISFIFO(os.stat(path).st_mode):
            os.unlink(path)
            os.mkfifo(path)
    except FileNotFoundError:
        os.mkfifo(path)


_last_translate = [0.0]
_overlay_shown = [False]
_shot_seq = [0]
_paused_until = [0.0]  # pause window: hotkey-setup etc. suppress triggers


def translate_once(args, mode):
    # paused (e.g. SetTranslateHotkey is listening for buttons - the Main
    # hotkey still fires while the user presses candidates)
    now = time.monotonic()
    if now < _paused_until[0]:
        plog("translate: SKIPPED (paused, %.0fs left)" % (_paused_until[0] - now))
        return

    # insurance for API-key backends: no trigger storm (replayed file,
    # double echo, script bug) may ever hammer a paid/quota'd service
    if now - _last_translate[0] < args.min_interval:
        plog("translate: SKIPPED (repeat within %.1fs min-interval)" % args.min_interval)
        return
    _last_translate[0] = now

    # translating the NEXT screen means un-freezing the last one first -
    # otherwise the capture sees our own overlay (translation recursion)
    if _overlay_shown[0]:
        mister("overlay_hide")
        _overlay_shown[0] = False
        time.sleep(0.08)  # ~2 frames for core video to resume

    t0 = time.monotonic()
    try:
        png, w, h, rowdoubled = capture_png(args)
    except (RuntimeError, OSError) as e:
        plog("translate: capture FAILED: %s" % e)
        mister("osd_msg -t 4000 AI: capture failed")
        return
    t_png = time.monotonic()

    try:
        routed = backend_service(args, mode, png)
    except Exception as e:  # HTTP/ssl/JSON shape - all end the same way
        plog("translate: backend FAILED: %s" % e)
        osd_show(args, "AI: server error\n" + str(e)[:80])
        return

    t_done = time.monotonic()
    plog("translate: %dx%d%s png=%dB cap+png=%dms route=%s total=%dms"
         % (w, h, " rowdoubled" if rowdoubled else "", len(png),
            (t_png - t0) * 1000, routed, (t_done - t0) * 1000))


def load_ini(path):
    """Flat KEY=VALUE settings file, '#' comments. Shell-sourceable too, so
    translate_start.sh can grep ENABLED without a python round trip."""
    vals = {}
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                vals[k.strip().upper()] = v.split("#", 1)[0].strip()
    except OSError:
        pass
    return vals


def redact(url):
    return re.sub(r"(api_key=)[^&]+", r"\1***", url)


def default_config():
    # /media/fat/translate is the shipped home; /media/fat/overlay was the
    # PoC-era path - honor an existing one so migrations can't strand a setup
    for p in ("/media/fat/translate/translate.ini",
              "/media/fat/overlay/translate.ini"):
        if os.path.exists(p):
            return p
    return "/media/fat/translate/translate.ini"


def main():
    # settings precedence: CLI args > translate.ini > built-in defaults.
    # The ini is the user-facing surface (autostart has no CLI).
    pre = argparse.ArgumentParser(add_help=False)
    pre.add_argument("--config", default=default_config(),
                     help="settings file (KEY=VALUE); CLI args override it")
    cargs, _ = pre.parse_known_args()
    ini = load_ini(cargs.config)

    def d(key, fallback, cast=str):
        try:
            return cast(ini.get(key, fallback))
        except ValueError:
            sys.exit("translate.ini: bad value for %s: %r" % (key, ini.get(key)))

    ap = argparse.ArgumentParser(description="MiSTer on-the-fly translation daemon",
                                 parents=[pre])
    ap.add_argument("--server", default=d("SERVER", "https://ztranslate.net/service"),
                    help="AI-Service-protocol endpoint (ztranslate/vgtranslate/own)")
    ap.add_argument("--api-key", default=d("API_KEY", ""),
                    help="appended to --server as api_key=")
    ap.add_argument("--zt-mode", default=d("ZT_MODE", ""),
                    help="ztranslate speed/quality: normal (default) or fast")
    ap.add_argument("--mode", choices=["text", "image"], default=d("MODE", "image"),
                    help="default output mode for 'go' (default: image)")
    ap.add_argument("--source", default=d("SOURCE_LANG", "ja"),
                    help="source language ('' = service auto-detect)")
    ap.add_argument("--target", default=d("TARGET_LANG", "en"), help="target language")
    ap.add_argument("--timeout", type=float, default=d("TIMEOUT", 15.0, float),
                    help="per-request timeout (s)")
    ap.add_argument("--png-level", type=int, default=d("PNG_LEVEL", 1, int),
                    help="zlib level (1=fast)")
    ap.add_argument("--osd-ms", type=int, default=d("OSD_MS", 8000, int),
                    help="osd_msg display time")
    ap.add_argument("--fifo", default=d("FIFO", "/tmp/translate_cmd"), help="trigger fifo")
    ap.add_argument("--min-interval", type=float, default=d("MIN_INTERVAL", 2.0, float),
                    help="minimum seconds between translations (quota guard)")
    ap.add_argument("--once", action="store_true",
                    help="single translate (using --mode) then exit; no fifo")
    args = ap.parse_args()

    if args.api_key and "api_key=" not in args.server:
        args.server += ("&" if "?" in args.server else "?") + "api_key=" + args.api_key

    if args.once:
        translate_once(args, args.mode)
        return

    signal.signal(signal.SIGTERM, lambda *a: sys.exit(0))
    ensure_fifo(args.fifo)
    plog("translate: daemon up pid=%d server=%s mode=%s lang=%s->%s%s" %
         (os.getpid(), redact(args.server), args.mode,
          args.source or "auto", args.target,
          (" zt_mode=" + args.zt_mode) if args.zt_mode else ""))
    plog("translate: trigger: echo image|text|go|hide > %s   stop: echo quit > %s (or kill %d)"
         % (args.fifo, args.fifo, os.getpid()))

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
                    _overlay_shown[0] = False
                elif cmd.startswith("pause"):
                    # pause [seconds] - suppress triggers (auto-resumes so a
                    # crashed hotkey-setup script can't wedge the daemon)
                    secs = 120.0
                    parts = cmd.split()
                    if len(parts) > 1:
                        try:
                            secs = float(parts[1])
                        except ValueError:
                            pass
                    _paused_until[0] = time.monotonic() + secs
                    plog("translate: PAUSED %.0fs (echo resume to end early)" % secs)
                elif cmd == "resume":
                    _paused_until[0] = 0.0
                    plog("translate: resumed")
                elif cmd in ("go", "text", "image"):
                    translate_once(args, args.mode if cmd == "go" else cmd)
                else:
                    plog("translate: unknown cmd '%s'" % cmd)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
