# piHPSDR client/server wire protocol

This document describes the network protocol spoken between a piHPSDR **server**
(the instance connected to the radio, doing WDSP and audio) and a piHPSDR
**client** (a thin remote head). `pihpsdr-client` implements the *client* side.

It was reverse-engineered from the piHPSDR sources — primarily:

- `src/client_server.h` — the contract: message enum, packed structs, encoders
- `src/client_server.c` — transport helpers (`recv_tcp`, `send_tcp`, `generate_pwd_hash`)
- `src/client_thread.c` — the client receive loop and command senders
- `src/server_thread.c` — the server side

> ⚠️ **This is an internal, version-pinned ABI, not a stable API.** piHPSDR
> checks `CLIENT_SERVER_VERSION` for an *exact* match and all payloads are
> `__attribute__((packed))` structs whose size is implied by message type
> (there is no length field on the wire). Vendor `client_server.h` from the
> exact piHPSDR build you target and re-sync on every update.

Reference constants (from `client_server.h`):

| Constant | Value |
|---|---|
| `CLIENT_SERVER_VERSION` | `0x01300005` |
| `SPECTRUM_DATA_SIZE` | `4096` (max panadapter width) |
| `AUDIO_DATA_SIZE` | `512` (PCM samples per packet) |
| `OPUS_MAX_PACKET` | `4000` |
| `OPUS_FRAME_SIZE` | `960` (20 ms @ 48 kHz) |
| `OPUS_SAMPLE_RATE` | `48000` |
| `SHA512_DIGEST_LENGTH` | `64` (challenge / response length) |
| sync bytes | `0xFA 0xFA 0xAF 0xAF` |

---

## 1. Transport

Two sockets to the **same** server address/port:

| Socket | Direction | Carries | Loss policy |
|---|---|---|---|
| **TCP** | both ways | handshake, control commands, large state structs (`INFO_*`), command replies | must not be lost |
| **UDP** | both ways | RX/TX spectrum, RX/TX audio, `INFO_DISPLAY`, `INFO_PS` | discardable on a brief hang |

The client opens the UDP socket, `connect()`s it to the server, and sends its
64-byte auth hash first so the server learns the client's UDP source
address/port (NAT / port learning). Thereafter it uses `send()`.

`recv_tcp()` loops until the exact requested byte count is read; `send_tcp()` is
mutex-guarded and loops until all bytes are sent.

---

## 2. Byte order and encodings

Everything is **big-endian** (network order). There are **three** numeric
encodings — and note the two different 64-bit ones, which must not be confused:

| Type | Encode | Decode | Used for |
|---|---|---|---|
| int16 / int32 | `htons` / `htonl` | `ntohs` / `ntohl` | widths, counts, filter edges |
| **int64 "plain"** | `htobe64(x)` | `be64toh` | **frequencies in Hz**, counters |
| **double "fixed-point"** | `htobe64((x + 9e8) * 1e10)` | `1e-10 * u64 - 9e8` | gains, levels, physical doubles |

The double mapping (`to_double` / `from_double`) covers roughly ±9·10⁸ with a
1·10⁻¹⁰ resolution; `0.0` maps to `9·10¹⁸`. It is **not** IEEE-754 on the wire.
Mixing up `to_64` (frequencies) with `to_double` (levels) is the classic bug.

```c
// from client_server.h
static inline uint64_t to_double(double x) { return htobe64((uint64_t)((x + 9.0E8) * 1.0E10)); }
static inline double   from_double(uint64_t y){ return 1.0E-10 * (double)be64toh(y) - 9.0E8; }
static inline uint64_t to_64(long long x)     { return htobe64((uint64_t)x); }
static inline long long from_64(uint64_t y)   { return (long long)be64toh(y); }
```

---

## 3. Frame format

Every message begins with a 12-byte header:

```
offset  size  field
 0       4    sync[4]  = { 0xFA, 0xFA, 0xAF, 0xAF }
 4       2    data_type   (uint16 BE — see message enum)
 6       1    b1          ┐ small inline payload; commands that need
 7       1    b2          │ only a few bytes fit entirely in the header
 8       2    s1 (u16 BE) │
10       2    s2 (u16 BE) ┘
```

**There is no length field.** The size of any payload following the header is
implied by `data_type`: either the command fits entirely in `b1/b2/s1/s2`
(header only), or it is a fixed-size packed struct. The receiver must therefore
know `sizeof(struct)` for each type and read `sizeof(struct) - 12` payload
bytes.

Structs order fields 64 → 32 → 16 → 8 bit for natural alignment, so the packed
layout has no hidden padding.

**Resync:** if the four sync bytes do not match, read one byte at a time until
`FA FA AF AF` is seen again, then read the remaining 8 header bytes.

---

## 4. Handshake (TCP)

```
1. client → connect() TCP
2. server → 4 bytes: version (0x01300005, MSB first). Client must match exactly.
3. server → 64 bytes: random challenge
4. client → 64 bytes: response = KDF(challenge, password)      (see §4.1)
5. server → 1 byte: 0x7F = accepted, anything else = rejected
6. client → 1 byte: 0x40 | audio_compression                  (see §4.2)
7. server → 1 byte: agreed compression = byte & 0x3F
8. client → open UDP, connect(), send the 64-byte hash (registration)
9. server → burst of INFO_* state over TCP                     (see §5)
10. server → CMD_START_RADIO → client brings up UI + starts UDP/CW threads
```

### 4.1 Password KDF (`generate_pwd_hash`)

```
s    = challenge(64 bytes) ‖ password        (password truncated to ≤ 64 bytes)
hash = SHA512(s)
repeat 99 999 times:
    s    = hash(64 bytes) ‖ password
    hash = SHA512(s)
response = hash                              (64 bytes)
```

That is **100 000** SHA-512 rounds total. An empty password is allowed but the
hash is still computed. Must match byte-for-byte.

### 4.2 Audio-compression negotiation

Client proposes `0x40 | comp`; the server replies and the agreed value is
`reply & 0x3F`:

| `comp` | Meaning |
|---|---|
| 0 | No compression — 16-bit PCM |
| 1 | Opus, 32 kbps, VOIP/voice |
| 2 | Opus, 64 kbps, AUDIO/music |
| 3 | Opus, 96 kbps, AUDIO/music (server may cap to 2) |

Opus is always **48 kHz mono**, complexity 5. If either endpoint fails to
initialise its codecs, both fall back to PCM.

---

## 5. Startup burst and CMD_START_RADIO

After the handshake the server pushes, over TCP, the full initial state:
`INFO_RADIO`, `INFO_ADC`, `INFO_RECEIVER` (per RX), `INFO_TRANSMITTER`,
`INFO_VFO` (per VFO), `INFO_BAND` / `INFO_BANDSTACK` (per band/stack),
`INFO_MEMORY` (per slot), `CMD_FILTER_VAR`, … and finally **`CMD_START_RADIO`**.

`CMD_START_RADIO` is the client's cue to build its UI and start the UDP receive
thread (and the CW sidetone thread). Until then the client is only ingesting
state.

---

## 6. Server → client streams (UDP)

### 6.1 Spectrum — `INFO_RX_SPECTRUM` / `INFO_TX_SPECTRUM` (`SPECTRUM_DATA`)

The workhorse. One packet per panadapter frame. Carries meters, "quick" VFO
frequencies, and the pixel column array.

```
HEADER
mydouble rxlvl, curragc, currout          // RX meters (from_double)
mydouble alc, micpeak, outavg, fwd, swr   // TX meters (from_double)
mydouble cA, cB, cAp, cBp                  // filter/notch edges
uint64   vfo_a_freq, vfo_b_freq            // "quick" VFO update (from_64)
uint64   vfo_a_ctun_freq, vfo_b_ctun_freq
uint64   vfo_a_offset, vfo_b_offset
uint16   width                             // number of valid columns (≤ 4096)
uint16   compressed_width                  // byte count if compressed
uint8    id                                // receiver id (0/1) or TX (8)
uint8    avail                             // pixels available flag
uint8    compressed                        // 1 = zlib-compressed sample[]
uint8    sample[SPECTRUM_DATA_SIZE]        // column magnitudes
```

**Pixel decode (the key formula):**

```
dBm = (int)sample[i] - 200          // integer dBm, i.e. 1 dB quantisation
```

If `compressed == 1`, zlib-`uncompress()` the first `compressed_width` bytes of
`sample[]` into `width` bytes, then apply the same decode.

The client picks the horizontal resolution: it tells the server the panadapter
`width` via `CMD_SCREEN` (capped at 4096). Perceived frequency detail is
`sample_rate / (zoom × width)` — sample rate sets the *span*, not per-Hz
detail. See `DESIGN.md`.

### 6.2 RX audio — `INFO_RXAUDIO` (`RXAUDIO_DATA`)

`header.b1` = receiver id, `header.s1` = sample count, then `int16` BE samples.

```
double sample = from_16(raw) * 0.00003051   // ≈ 1/32768; mono, duplicated L/R
```

### 6.3 RX audio (Opus) — `INFO_RXAUDIO_OPUS` (`OPUS_AUDIO_DATA`)

`header.b1` = id, `header.s1` = encoded byte count, `payload` = one Opus frame.
Decode with the per-RX decoder into ≤ `OPUS_FRAME_SIZE` samples; scale by
`1/32768`.

### 6.4 `INFO_DISPLAY` (`DISPLAY_DATA`)

Periodic status affecting the display: ADC0/1 overload, high-SWR flag, TX FIFO
under/overrun, TX inhibit, exciter power, capture state, sequence errors.

### 6.5 `INFO_PS` (`PS_DATA`)

PureSignal feedback (`psinfo[16]`, attenuation, `ps_getmx`).

---

## 7. Client → server audio (UDP)

A dedicated thread fires every **2 ms** and pulls 96 mic samples per tick,
buffering them:

- **PCM:** into `AUDIO_DATA_SIZE` (512) `int16` samples → `INFO_TXAUDIO`,
  `header.s1` = count.
- **Opus:** into `OPUS_FRAME_SIZE` (960) samples, encode → `INFO_TXAUDIO_OPUS`,
  `header.s1` = encoded bytes.

Audio is only actually sent while transmitting (and not for CW/tune/twotone);
otherwise the buffer's first half is dropped so some data is ready at RX→TX.

`pihpsdr-client` will not implement TX initially (piHPSDR handles TX), so this
path is documented for completeness.

---

## 8. Control commands (client → server, TCP)

Every UI action maps to a `send_*()` in piHPSDR (declared in `client_server.h`,
lines ~968–1079). Patterns:

- **Header-only** commands pack their data into `b1/b2/s1/s2`.
- **Wrapped** commands use generic payload structs: `U32_COMMAND`,
  `U64_COMMAND`, `DOUBLE_COMMAND`, or a dedicated struct (`AGC_COMMAND`,
  `EQUALIZER_COMMAND`, `NOTCH_COMMAND`, `NOISE_COMMAND`, `DIVERSITY_COMMAND`,
  `COMPRESSOR_DATA`, `DEXP_DATA`, `PS_PARAMS`).

Examples relevant to a control head:

| Action | data_type | Layout |
|---|---|---|
| Volume | `CMD_VOLUME` | rx id + double (via `send_volume`) |
| VFO move (Hz) | `CMD_MOVE` | `b1`=id, `b2`=round, `u64`=Hz (plain `to_64`) |
| VFO step (clicks) | `CMD_STEP` | `b1`=id, steps |
| Set frequency | `CMD_FREQ` | id + `u64` Hz |
| Zoom / Pan | `CMD_ZOOM` / `CMD_PAN` | server re-bins (round-trip) |
| Panadapter width | `CMD_SCREEN` | horizontal-stack flag + width |
| Frame rate | `CMD_RX_FPS` / `CMD_TX_FPS` | id + fps |
| Mode / Filter | `CMD_MODE` / `CMD_FILTER_SEL` | id + value |
| AGC | `CMD_AGC` | `AGC_COMMAND` |
| Noise (NB/NR/ANF) | `CMD_NOISE` | `NOISE_COMMAND` |
| MOX / Tune | `CMD_MOX` / `CMD_TUNE` | state in header |

VFO frequency changes are **accumulated** and flushed every 100 ms (see §9),
rather than one packet per encoder tick.

Command replies come back as `INFO_*` (full re-sent state) or as small `CMD_*`
echoes (`CMD_MOX`, `CMD_TUNE`, `CMD_AGC` with server-recomputed hang/thresh,
`CMD_PAN`, `CMD_RX_FILTER_CUT`, …), which the client applies to keep its state
in sync.

---

## 9. Keepalive and latency

A 100 ms client timer:

- flushes accumulated VFO moves/steps;
- every 10th tick (~1 s) sends `CMD_PING`.

The server echoes `CMD_PONG` carrying the send timestamp in `s1` (ms & 0xFFFF);
the client computes round-trip latency from it (used for the audio jitter
buffer, `remote_latency_ms`).

---

## 10. Message enum (for reference)

Commands (client-initiated, `CMD_*`) and server-pushed state (`INFO_*`) share
one enum `_header_type_enum` in `client_server.h`. Notable `INFO_*`:
`INFO_RADIO`, `INFO_RECEIVER`, `INFO_TRANSMITTER`, `INFO_VFO`, `INFO_ADC`,
`INFO_BAND`, `INFO_BANDSTACK`, `INFO_MEMORY`, `INFO_DISPLAY`, `INFO_PS`,
`INFO_RX_SPECTRUM`, `INFO_TX_SPECTRUM`, `INFO_RXAUDIO`, `INFO_RXAUDIO_OPUS`,
`INFO_TXAUDIO`, `INFO_TXAUDIO_OPUS`. The exhaustive, authoritative list is the
enum in the vendored `client_server.h`.

---

## Minimum viable RX client

To get a first panadapter on screen:

1. TCP connect → version check → KDF challenge/response → compression = 0 (PCM).
2. Open UDP, `connect()`, send the 64-byte hash.
3. Ingest the TCP `INFO_*` burst until `CMD_START_RADIO` (at minimum parse
   `INFO_RADIO`, `INFO_RECEIVER`, `INFO_VFO`).
4. Send `CMD_SCREEN` with your panadapter width; enable RX spectrum
   (`CMD_RX_SPECTRUM`) and set `CMD_RX_FPS`.
5. On UDP, parse `INFO_RX_SPECTRUM`: decode `sample[i] - 200` → render.
6. Send `CMD_MOVE` / `CMD_VOLUME` to prove the control path.
