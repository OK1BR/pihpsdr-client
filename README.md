# pihpsdr-client

**A modern GTK4 remote panadapter, waterfall and control head for [piHPSDR](https://github.com/dl1ycf/pihpsdr).**

`pihpsdr-client` is an alternative front-end that talks to piHPSDR's built-in
**client/server network protocol**. piHPSDR — running next to the radio (for
example on a Raspberry Pi) — keeps doing all the heavy lifting: the HPSDR
Protocol 1/2 link to the radio, WDSP DSP, audio, CAT and TCI. This client
connects over the network and provides a nicer, high-detail spectrum and
waterfall plus an ergonomic control surface, operable remotely.

> Status: **early / planning.** The protocol has been reverse-engineered from
> the piHPSDR sources; implementation begins with a receive-only spike.

## Why

piHPSDR is an excellent radio *engine*, but its panadapter is drawn entirely in
Cairo on the CPU and its controls are functional rather than polished. This
project keeps piHPSDR as the backend and replaces only the part worth
improving: a beautiful, GPU-accelerated wideband panadapter and waterfall, and
comfortable controls (volume, filter, AGC, noise reduction, zoom, mode, …) that
can be driven from another machine.

Explicit non-goal: **do not reimplement any DSP or radio protocol.** piHPSDR
already does that well.

## Architecture

```
  Radio (ANAN, HPSDR P1/P2)
        │  Ethernet
        ▼
  piHPSDR  ── server mode (a runtime toggle) ──────────────┐
  ├─ HPSDR Protocol 1/2 to the radio                        │
  ├─ WDSP DSP (demod, filters, AGC, NR, S-meter, panadapter FFT)
  ├─ audio I/O, TX audio processing, PureSignal, diversity  │
  └─ CAT (rigctl/Hamlib) + TCI                               │
        │  client/server protocol (TCP control + UDP bulk)   │
        ▼                                                    │
  pihpsdr-client  (this project)  ◄────────────────────────┘
  ├─ RX panadapter + waterfall rendering
  ├─ control surface (volume, filter, AGC, NR, zoom, mode, …)
  └─ S-meter / TX meter display
```

The spectrum is binned **once** on the server and the audio demodulated once —
this client only renders and sends control commands, so there is no duplicated
DSP load. Because the server can run on a separate machine near the radio, the
"engine" needs no screen of its own; this client is the only face you look at.

## Scope

**In scope (this client):**

- RX panadapter (spectrum) rendering, up to 4096 columns per frame
- Waterfall rendering — GPU-accelerated, good colour maps, smooth scrolling
- Control surface: volume, AF/RF gain, filter, mode, AGC, NB/NR/ANF, zoom/pan,
  RIT, band, squelch, … (each a one-line command over TCP)
- S-meter and TX (power/ALC/SWR) meters
- Multiple receivers (RX1 / RX2)

**Out of scope (handled by piHPSDR, this client only sends commands):**

- DSP / demodulation / noise-reduction algorithms (WDSP)
- The HPSDR Protocol 1/2 link to the radio
- Audio I/O to the radio and TX audio processing
- CAT (rigctl / Hamlib) and TCI
- PureSignal, diversity internals

## Protocol

Built on piHPSDR's internal client/server protocol
(`src/client_server.{h,c}` and `src/client_thread.c` in piHPSDR).

- **Two sockets:** TCP for the handshake, control commands and reliable state;
  UDP for the discardable bulk streams (spectrum, audio, display info).
- **Framing:** everything big-endian. A 12-byte header
  (`sync = FA FA AF AF`, `type`, and four small fields `b1/b2/s1/s2`) is
  followed by a payload whose size is **implied by the message type** — there
  is no length field.
- **Handshake:** exact version match, SHA-512 challenge/response (iterated
  100 000×), audio-compression negotiation (raw PCM or Opus, 48 kHz mono), then
  UDP registration.
- **Spectrum:** pre-binned bytes, one per column; `dBm = byte − 200`, optionally
  zlib-compressed. The client chooses the width (≤ 4096) and the frame rate.

A full wire specification lives in [`docs/PROTOCOL.md`](docs/PROTOCOL.md).

## Known limitations (inherent to the protocol)

These follow from the fact that the server sends *rendered spectrum*, not raw
IQ. They are acceptable for a gorgeous wideband display, but worth stating up
front:

- **Amplitude is quantised to 1 dB** (one byte per pixel).
- **FFT size and averaging are decided server-side.** The client can request
  parameters but cannot run its own FFT/windowing — that would require raw IQ.
- **At most 4096 spectrum columns per frame.** Panadapters wider than 4096 px
  are upscaled visually, not with more data.
- **Zoom/pan is re-binned on the server** (a network round-trip).
- **Sample rate sets the *span*, not per-Hz detail.** Perceived detail is
  roughly `sample_rate / (zoom × pixels)`. The maximum sample rate gives a
  whole-band panorama but a *coarser* pixel, not a finer one.
- **The protocol is an internal, version-pinned ABI.** piHPSDR checks its
  `CLIENT_SERVER_VERSION` exactly and the structs are packed, so this client
  must be kept in sync with a specific piHPSDR build.

## Requirements

- A radio supported by piHPSDR (e.g. an Apache Labs ANAN)
- piHPSDR running in server mode near the radio (server mode is a runtime toggle)
- Client host: Linux + GTK4

## Planned implementation

- **C + GTK4**
- Vendors piHPSDR's `client_server.h` for exact struct / ABI parity
- GPU-accelerated waterfall via `GtkGLArea` where practical
- First milestone (spike): connect → parse `SPECTRUM_DATA` → render one
  panadapter frame → send `CMD_VOLUME`

## Credits

- **piHPSDR** by Christoph van Wüllen (DL1YCF) and John Melton (G0ORX) — the
  engine this client talks to.
- **WDSP** by Warren Pratt (NR0V) — the DSP library behind it.

## License

[GPLv3](LICENSE). This project builds on and derives from piHPSDR, which is
GPLv3.

## Author

Richard Fakenberg — **OK1BR**
