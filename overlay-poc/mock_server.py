#!/usr/bin/env python3
"""Mock RetroArch-AI-Service server for the MiSTer overlay PoC.

Speaks just enough of the vgtranslate protocol to prove the MiSTer-side
loop end to end with no cloud keys: accepts the daemon's POST (JSON with a
base64 PNG), and answers per the requested ?output= mode:

    text  -> {"text": canned string incl. the received frame size}
    image -> {"image": the frame with a fake translation bar drawn on it}
             (needs Pillow: pip install pillow. Without it, the frame is
              echoed back unchanged - still proves the whole pipeline.)

Runs on the default vgtranslate port (4404), so pointing the daemon at a
REAL vgtranslate/ztranslate later is a URL swap and nothing else.

    python3 mock_server.py [port]
"""

import base64
import io
import json
import struct
import sys
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, HTTPServer

try:
    from PIL import Image, ImageDraw
    HAVE_PIL = True
except ImportError:
    HAVE_PIL = False

MOCK_TEXT = "HERO: I found the\nancient sword!\n(mock translation)"


def png_size(png):
    """Width/height straight out of the IHDR chunk."""
    if len(png) >= 24 and png[12:16] == b"IHDR":
        return struct.unpack(">II", png[16:24])
    return (0, 0)


def draw_mock_translation(png):
    """Fake 'image mode': dialogue bar with translated text over the frame."""
    img = Image.open(io.BytesIO(png)).convert("RGB")
    w, h = img.size
    bar_h = max(40, h // 4)
    band = Image.new("RGB", (w, bar_h), (12, 12, 48))
    img.paste(band, (0, h - bar_h))
    d = ImageDraw.Draw(img)
    d.rectangle([(4, h - bar_h + 4), (w - 5, h - 5)], outline=(200, 200, 255))
    y = h - bar_h + 10
    for line in MOCK_TEXT.split("\n"):
        d.text((14, y), line, fill=(255, 255, 255))
        y += 14
    out = io.BytesIO()
    img.save(out, "PNG")
    return out.getvalue()


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        t0 = time.monotonic()
        length = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(length).decode())
        png = base64.b64decode(body.get("image", ""))
        w, h = png_size(png)

        q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
        output = q.get("output", ["text"])[0]

        if output.startswith("image"):
            if HAVE_PIL:
                reply = {"image": base64.b64encode(draw_mock_translation(png)).decode()}
                what = "image+bar"
            else:
                reply = {"image": body.get("image", "")}
                what = "image-echo (pip install pillow for the drawn bar)"
        else:
            reply = {"text": "%s\n[frame %dx%d]" % (MOCK_TEXT, w, h)}
            what = "text"

        data = json.dumps(reply).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)
        print("mock: %dx%d frame (%d B png) -> %s  src=%s tgt=%s  %.0fms"
              % (w, h, len(png), what,
                 q.get("source_lang", ["?"])[0], q.get("target_lang", ["?"])[0],
                 (time.monotonic() - t0) * 1000))

    def log_message(self, *a):
        pass  # our own print above is enough


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 4404
    print("mock AI-Service on 0.0.0.0:%d  (PIL %s)"
          % (port, "available - image mode draws the bar" if HAVE_PIL
             else "MISSING - image mode echoes the frame"))
    HTTPServer(("0.0.0.0", port), Handler).serve_forever()
