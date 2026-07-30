# M11 — self-hosted translation server: work order

> **STATUS (2026-07-30): this plan happened — as `retroglot`, in its own
> repo (`~/dev/retroglot`, gitea `shane/retroglot`). S0–S4 are DONE; S5
> (packaging) and S6 (share) remain. The maintained truth is retroglot's
> `docs/ROADMAP.md`, README and `config.py` — the tables and config block
> below are the ORIGINAL plan and several choices changed in the building:**
>
> - **PaddleOCR was dropped** (no Python 3.14 wheels) for **RapidOCR**
>   (same PP-OCR models on onnxruntime); comic-text-detector was demoted,
>   never integrated; **meikiocr** (trained on rendered game text) was
>   added and benchmarked — better on kana-only Game Boy frames, slightly
>   worse on dense Saturn dialogue.
> - **The server ships with `PROFILE=echo`** — a wiring test that returns
>   every frame unchanged. Set `PROFILE=light` (or `quality`) and, for
>   the CT2 backends, `MT_MODEL_PATH`, or you get silent untranslated
>   freeze-frames and wonder why. This is the one thing a fresh install
>   trips over.
> - `CACHE_ENTRIES` is the **text** translation memory; the frame cache
>   is `CACHE_FRAMES`/`CACHE_DISTANCE` and is perceptual (dHash), not
>   identical-hash. No font is bundled; host fonts are discovered.
> - Configuration is env-vars only; no config.yml exists.

A shareable docker image anyone can run (unraid / Pi / mini-PC / VPS)
that speaks the RetroArch AI-Service protocol: OCR + translation + in-place
rendering, no cloud account, nothing leaves the LAN. Drop-in for
ztranslate: the MiSTer side changes one ini line (`SERVER=http://box:4404`).
Because it speaks the standard protocol, stock RetroArch users can point
their AI Service at it too — it is a community artifact in its own right.

`dev/mock_server.py` is the protocol seed: request/response shapes are
already implemented and hardware-proven against our daemon.

## Architecture

```
POST /?output=image,png&source_lang=ja&target_lang=en
  body {"image": <b64 png>}
        │
        ▼
  [1] decode + preprocess (cap size, grayscale copy for detection)
  [2] DETECT text regions        (detector model)
  [3] RECOGNIZE each region      (OCR model)
  [4] TRANSLATE (batched)        (MT backend)
  [5] RENDER into the frame      (PIL: fill original boxes, fit text)
        │
        ▼
  {"image": <b64 png>}   (or {"text": ...} for output=text)
```

Detection and recognition are separate stages on purpose: the best JP
recognizer (manga-ocr) is recognition-only, and mixing/matching stages is
where the quality tuning lives.

## Model choices

### OCR recognition

| Model | Size | Why / why not |
|---|---|---|
| **manga-ocr** (default) | ~450MB | Trained on Japanese manga: vertical text, furigana, stylized fonts — the closest thing to game-text-tuned that is freely available. Recognition-only. |
| PaddleOCR rec (PP-OCRv5) | ~small, 106 langs | The multilingual fallback and the non-JA path; weaker on stylized JP than manga-ocr. |
| Tesseract | tiny | Ruled out: needs ~300dpi-clean text, fails pixel fonts (researched + known). |

### Detection (region finding)

| Model | Notes |
|---|---|
| **comic-text-detector** (default) | Pairs with manga-ocr in the proven mokuro pipeline; built for manga/game-style text blocks incl. vertical. |
| PaddleOCR det stage | Comes free with Paddle; the light-profile detector. |

### Translation

| Backend | Size / RAM | Quality | Notes |
|---|---|---|---|
| **Sugoi V4 (CTranslate2)** (default) | ~1.1GB, wants 4-8GB RAM | Best offline JA→EN, the fan-translation standard | JA→EN only |
| NLLB-200 distilled 600M int8 | ~600MB | Good, 200 languages | The multilingual profile |
| Argos Translate | ~150MB/pair | Adequate | The Pi-class light profile |
| **OpenAI-compatible endpoint** | none local | Potentially best (context-aware) | Points at ollama / LM Studio / OpenRouter / any `/v1/chat/completions`; turns any local LLM into the translator. The vgtranslate_local precedent. |

### Recommended profiles (one env var: `PROFILE=`)

- `quality` (default, unraid-class): comic-text-detector + manga-ocr + Sugoi ct2. ~2GB models, ~4GB RAM, CPU-only fine.
- `light` (Pi-4-class): Paddle det+rec + Argos. ~1GB total.
- `llm`: comic-text-detector + manga-ocr + OpenAI-compatible endpoint (your GPU box or a paid API does the translating).

All stages hot-swappable via config regardless of profile.

## Configuration surface (env vars / config.yml, env wins)

```
PORT=4404                  BIND=0.0.0.0
PROFILE=quality            # quality | light | llm (sets the three below)
DETECTOR=comic-text-detector | paddle
OCR=mangaocr | paddle
MT=sugoi | nllb | argos | openai
OPENAI_BASE_URL= OPENAI_MODEL= OPENAI_API_KEY=   # for MT=openai (ollama etc.)
DEFAULT_SOURCE=ja          DEFAULT_TARGET=en     # request params override
DEVICE=cpu                 # cpu | cuda
API_KEY=                   # optional shared secret (?api_key=); empty = open.
                           # matters the moment someone exposes this beyond LAN
CACHE_ENTRIES=64           # identical-frame hash -> cached reply (repeat
                           # presses on the same screen cost 0 model time)
MAX_IMAGE_DIM=2048         REQUEST_TIMEOUT=30
RENDER_FONT=/fonts/NotoSans   # bundled; override for taste
RENDER_STYLE=inplace       # inplace (fill original boxes) | banner (bottom bar)
LOG_LEVEL=info             # request timing per stage, perf-log philosophy
```

## Work order (phases, each hardware-testable like the display spike)

**S0 — protocol skeleton.** FastAPI + uvicorn app lifted from
mock_server.py: request parse, output negotiation, /health, config
loading, echo-image mode. *Test: MiSTer daemon round-trips against it
with zero models installed.* (small)

**S1 — OCR leg.** Detector + recognizer stages behind interfaces, both
default models wired, region merge/ordering (vertical text!). `output=text`
returns the raw Japanese. *Test: fixture PNGs — we have the session's real
captures (Fire Emblem dialogue + menus) as ground truth.* (the big one)

**S2 — MT leg.** Pluggable MT with batching; Sugoi ct2 + openai backends
first, argos/nllb after. `output=text` now returns English. *Test: same
fixtures, eyeball quality vs ztranslate's output.* (medium)

**S3 — render leg.** `output=image,png`: fill detected boxes with the
sampled background shade, fit wrapped translated text (shrink-to-fit,
outline for contrast), vertical-source→horizontal-target handling.
*Test: the freeze-frame on the MiSTer looks as good as ztranslate's.* (medium)

**S4 — hardening.** Frame-hash cache, single-flight per client, model
warm-up at boot (first press must not eat a 20s model load), graceful
OOM/timeouts → protocol `error` replies, per-stage timing log line.
(small-medium)

**S5 — packaging.** Dockerfile (CPU base; models downloaded on first run
into a volume — keeps the image small), docker-compose.yml, unraid
template XML, README for strangers (incl. RetroArch setup, not just
MiSTer), license audit task: manga-ocr (Apache-2.0) ✓, comic-text-detector
(check), Sugoi weights (check redistribution — may need download-from-
source-on-first-run rather than bundling). (medium)

**S6 — integration + share.** translate/README gains the self-hosted
option; decide the public home (own repo? name?) — Shane's call, ships
only on his sign-off. (small)

Order: S0 → S1 → S2 → S3 are strictly sequential; S4/S5 interleave after.
S0 can be validated on hardware the same evening it is written.

## Open decisions for Shane

1. **Framework**: RESOLVED — FastAPI+uvicorn, as recommended.
2. **Default profile**: `quality` assumes the unraid box gives it ~4GB.
3. **Sugoi licensing/redistribution**: if the weights can't ship in an
   image, first-run download is the fallback (task in S5).
4. **Where the code lives**: RESOLVED — its own repo from the start,
   named **retroglot** (AGPL-3.0 + attribution, commercial licences
   reserved).
5. **GPU**: CPU-first everywhere; CUDA path is config-gated bonus work,
   not on the critical path.
