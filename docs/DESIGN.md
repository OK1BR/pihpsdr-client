# Design notes & decisions

Captures the reasoning behind `pihpsdr-client` so a fresh start has the full
context. Companion to [`PROTOCOL.md`](PROTOCOL.md).

## Problem

piHPSDR is a first-class radio *engine* (HPSDR Protocol 1/2, WDSP DSP, audio,
CAT, TCI, TX with full processing), but:

- its panadapter/waterfall is drawn entirely in **Cairo on the CPU** — no GPU
  path anywhere in the codebase;
- the controls are functional rather than ergonomic/beautiful.

Goal: a **better UX** — a high-detail, GPU-accelerated wideband spectrum +
waterfall and a comfortable control surface (volume, filter, AGC, NR, zoom,
mode, …) — **operable remotely**, without reimplementing any DSP.

## Deployment model

```
Radio (ANAN)  ──Ethernet──►  piHPSDR "server"  ──client/server proto──►  pihpsdr-client
                              (near the radio,       (TCP + UDP)          (this; on the desktop)
                               e.g. a Raspberry Pi)
```

piHPSDR "server mode" is a **runtime toggle** in the full GUI app (Server menu),
not a separate/headless binary. It keeps doing DSP/audio/CAT/TCI/TX; it also
computes the panadapter pixels and streams them. Because it can run on a
separate machine near the radio, its own window is irrelevant — this client is
the only face the operator looks at, and there is **no duplicated DSP load**
(spectrum binned once, audio demodulated once, server-side).

## Options considered

| Option | New code | piHPSDR runs? | Single app? | Verdict |
|---|---|---|---|---|
| **A** — fork piHPSDR, port GTK3→4, re-skin in-process | medium (86k LOC C) | no, becomes this | yes, clean | Lots of tedious porting for the same UX unless also redesigned. dl1ycf stays on GTK3 deliberately. |
| **B** — client over piHPSDR's client/server protocol | least | **yes, as the engine** | no (two processes) | **Chosen.** Cheapest path to the actual goal; server on a Pi removes the "two processes" smell entirely. |
| **C** — greenfield app straight to the radio + link WDSP | most | no, replaced | yes, clean | Maximum freedom (own IQ/FFT), but months to parity (protocol + WDSP + threading + audio + TX). |

**Decision: Option B.** The operator explicitly does not mind piHPSDR running as
a background engine (ideally on a Pi by the radio) and handling audio, CAT and
TCI. The scope is a nicer remote panadapter/waterfall and controls.

## Scope

**In:** RX panadapter + waterfall rendering (GPU where practical); control
surface (volume, AF/RF gain, filter, mode, AGC, NB/NR/ANF, zoom/pan, RIT, band,
squelch…); S-meter / TX meters; RX1/RX2.

**Out (piHPSDR handles; client only sends commands):** DSP/WDSP; HPSDR P1/P2 to
the radio; audio I/O and TX audio processing; CAT (rigctl/Hamlib) and TCI;
PureSignal, diversity internals. A nice side effect: the operator's other tools
(e.g. logging over CAT/TCI) keep talking to the same engine in parallel.

## Inherent limitations (from the protocol)

The server sends **rendered spectrum, not raw IQ**, so:

- **Amplitude quantised to 1 dB** (1 byte/column). Smooth gradients must be
  interpolated/dithered at render time.
- **FFT size & averaging are server-side.** The client can request parameters
  but cannot run its own FFT/windowing — that needs raw IQ (Option C territory).
- **≤ 4096 columns per frame.** Wider panadapters upscale visually, not with
  more data. So a 4096-px-wide panadapter is this protocol's real detail ceiling
  per frame.
- **Zoom/pan is a server round-trip.**
- **Sample rate = span, not detail.** piHPSDR P2 offers up to **1 536 000 Sa/s**
  per RX. Perceived detail ≈ `sample_rate / (zoom × pixels)`. Max sample rate
  gives a whole-band *panorama* but a *coarser* pixel; fine detail comes from a
  smaller visible span (lower rate or zoom in). Sweet spot: choose the rate for
  the span you want, push width to 4096, keep `fft_size ≥ 2 × pixels`, zoom for
  close-ups.

These are acceptable for a gorgeous wideband display — it is the same data
piHPSDR itself draws, just rendered better.

## Practical constraints

- **CPU on the Pi is the real ceiling**, not the protocol. Load scales roughly
  as `sample_rate × fft_size × fps`, all on the server. 1536 kHz + large FFT +
  high FPS + 4096 columns is heavy for a Pi.
- **No headless mode** in the current piHPSDR: the server always draws its own
  (unwatched) Cairo panadapter on its FPS timer, wasting cycles on a Pi.
  Mitigate with lower server-side FPS/width, or a dummy display.

## Risks / open questions

- **Which Pi?** Does the target ANAN G2E expose an internal CM4 that can just run
  server mode, or does a separate Pi need to sit by the radio (which also moves
  DSP off the desktop onto that Pi)? Affects setup and CPU budget.
- **GTK4 + GL on NVIDIA (client host).** The intended waterfall uses `GtkGLArea`;
  the developer's desktop is NVIDIA + Wayland, where GTK4's GPU renderer has been
  known to crash (worked around elsewhere with `GSK_RENDERER=cairo`). De-risk the
  GL path early; have a Cairo fallback.
- **Version pinning.** The protocol is an exact-match, packed-struct ABI. Vendor
  `client_server.h` and re-sync against the specific piHPSDR build in use.

## Stack

- **C + GTK4.** Chosen for proximity to piHPSDR: can vendor `client_server.h`
  verbatim for exact ABI parity and crib rendering approaches. (Rust + gtk4-rs
  was considered — zero C-interop since it is a pure socket protocol — but C
  keeps the header/ABI reuse trivial.)
- GPU-accelerated waterfall via `GtkGLArea` where practical, Cairo fallback.

## Roadmap

1. **Spike (RX-only):** TCP handshake + KDF → ingest `INFO_*` to
   `CMD_START_RADIO` → parse `INFO_RX_SPECTRUM` → render one panadapter frame →
   send `CMD_VOLUME` and `CMD_MOVE` to prove the control path.
2. Waterfall (GL), colour maps, smooth scroll; amplitude interpolation.
3. Real control surface: volume, filter, mode, AGC, NR, zoom/pan, RIT, band.
4. S-meter / TX meters; RX2.
5. Polish: theming, layout, input ergonomics (wheel-on-panadapter, gestures).

TX, CAT and TCI stay with piHPSDR and are out of scope for this client.
