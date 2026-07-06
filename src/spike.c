/*
 * spike.c — headless RX protocol spike for pihpsdr-client.
 *
 * Proves the hard part of the client/server protocol against a live piHPSDR
 * server, with no GUI: TCP handshake + SHA-512 KDF, audio-compression
 * negotiation, UDP registration, ingest of the INFO_* startup burst up to
 * CMD_START_RADIO, then enable the RX panadapter and decode the first
 * INFO_RX_SPECTRUM frames (sample[i] - 200 => dBm).
 *
 * With SPIKE_SEND_CONTROL=1 it also runs a closed-loop control-path proof:
 * CMD_MOVE +1000 Hz, observe the shift in the spectrum stream, then CMD_MOVE
 * -1000 Hz to restore — leaving the radio untouched.
 *
 * Usage:  pihpsdr-spike [host] [port] [password]
 * Default host/port 127.0.0.1:50000, empty password (or $PIHPSDR_PWD).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <zlib.h>

#include "protocol.h"
#include "transport.h"

/* Spike parameters. */
#define SPIKE_FRAMES    20   /* stop after decoding this many frames    */
#define SPIKE_UDP_TIMEOUT_MS 4000

/*
 * NB: this spike deliberately does NOT send CMD_SCREEN (panadapter width) or
 * CMD_RX_FPS. In piHPSDR server mode those reconfigure the *shared* analyzer,
 * clobbering the operator's local display width/frame-rate. We instead consume
 * whatever the server already produces (spectrum_data.width, server-side fps) —
 * the decoder is width-agnostic. Only CMD_RX_SPECTRUM (start/stop streaming to
 * this client) is sent, which touches a per-client flag only.
 */

static const uint8_t SYNCBYTES[4] = { 0xFA, 0xFA, 0xAF, 0xAF };

/* ------------------------------------------------------------------ */
/* Command senders (exact wire layout, cf. piHPSDR client_server.c).  */
/* ------------------------------------------------------------------ */

static int sp_send_startstop_rxspectrum(int fd, int id, int state) {
  HEADER h;
  SYNC(h.sync);
  h.data_type = to_16(CMD_RX_SPECTRUM);
  h.b1 = id;
  h.b2 = state;
  h.s1 = 0;
  h.s2 = 0;
  return tp_send_all(fd, &h, sizeof(h));
}

/* CMD_MOVE is a RELATIVE tune by `hz` (vfo_id_move). NB: CMD_MOVETO is
 * ABSOLUTE (sets the frequency to `hz`) — do not confuse them. */
static int sp_send_move(int fd, int id, long long hz, int round) {
  U64_COMMAND c;
  SYNC(c.header.sync);
  c.header.data_type = to_16(CMD_MOVE);
  c.header.b1 = id;
  c.header.b2 = round;
  c.header.s1 = 0;
  c.header.s2 = 0;
  c.u64 = to_64(hz);
  return tp_send_all(fd, &c, sizeof(c));
}

/* ------------------------------------------------------------------ */
/* Handshake                                                          */
/* ------------------------------------------------------------------ */

/* Returns 0 on success and fills udp_hash[64] (the registration hash). */
static int do_handshake(int tcp, const char *pwd, uint8_t udp_hash[SHA512_DIGEST_LENGTH]) {
  uint8_t buf[SHA512_DIGEST_LENGTH];

  /* 1. Version (4 bytes, MSB first). */
  uint8_t ver[4];
  if (tp_recv_all(tcp, ver, 4) < 0) {
    fprintf(stderr, "handshake: no version\n");
    return -1;
  }
  uint32_t got = ((uint32_t)ver[0] << 24) | ((uint32_t)ver[1] << 16) |
                 ((uint32_t)ver[2] << 8) | (uint32_t)ver[3];
  printf("  server version   0x%08x (expect 0x%08x)\n", got, CLIENT_SERVER_VERSION);
  if (got != CLIENT_SERVER_VERSION) {
    fprintf(stderr, "handshake: version mismatch — re-vendor client_server.h\n");
    return -1;
  }

  /* 2. 64-byte challenge -> KDF response. */
  if (tp_recv_all(tcp, buf, SHA512_DIGEST_LENGTH) < 0) {
    fprintf(stderr, "handshake: no challenge\n");
    return -1;
  }
  cs_pwd_hash(buf, pwd, udp_hash);
  if (tp_send_all(tcp, udp_hash, SHA512_DIGEST_LENGTH) < 0) {
    return -1;
  }

  /* 3. Accept byte. */
  uint8_t accept = 0;
  if (tp_recv_all(tcp, &accept, 1) < 0) {
    return -1;
  }
  if (accept != 0x7F) {
    fprintf(stderr, "handshake: server REJECTED password (0x%02x). "
                    "Pass the server password as arg 3.\n", accept);
    return -1;
  }
  printf("  password         accepted\n");

  /* 4. Audio compression: propose PCM (0x40 | 0). */
  uint8_t comp = 0x40 | 0;
  if (tp_send_all(tcp, &comp, 1) < 0) {
    return -1;
  }
  if (tp_recv_all(tcp, &comp, 1) < 0) {
    return -1;
  }
  printf("  audio comp       %d (0=PCM)\n", comp & 0x3F);

  return 0;
}

/* ------------------------------------------------------------------ */
/* TCP frame reader with resync                                       */
/* ------------------------------------------------------------------ */

/* Read one HEADER, resyncing on the 4 sync bytes if needed. 0 on success. */
static int read_header(int tcp, HEADER *h) {
  if (tp_recv_all(tcp, h, sizeof(HEADER)) < 0) {
    return -1;
  }
  if (memcmp(h->sync, SYNCBYTES, 4) == 0) {
    return 0;
  }

  fprintf(stderr, "  header sync mismatch %02x %02x %02x %02x — resyncing\n",
          h->sync[0], h->sync[1], h->sync[2], h->sync[3]);
  int matched = 0;
  while (matched != 4) {
    uint8_t c;
    if (tp_recv_all(tcp, &c, 1) < 0) {
      return -1;
    }
    if (c == SYNCBYTES[matched]) {
      matched++;
    } else {
      matched = (c == SYNCBYTES[0]) ? 1 : 0;
    }
  }
  memcpy(h->sync, SYNCBYTES, 4);
  if (tp_recv_all(tcp, (char *)h + 4, sizeof(HEADER) - 4) < 0) {
    return -1;
  }
  fprintf(stderr, "  resync ok\n");
  return 0;
}

/* ------------------------------------------------------------------ */
/* Ingest the startup burst up to CMD_START_RADIO                     */
/* ------------------------------------------------------------------ */

static int ingest_until_start(int tcp) {
  uint8_t payload[8192]; /* larger than the biggest struct trailing part */
  int count = 0;

  for (;;) {
    HEADER h;
    if (read_header(tcp, &h) < 0) {
      return -1;
    }
    uint16_t type = from_16(h.data_type);
    int psize = cs_tcp_payload_size(type);

    if (psize > 0) {
      if (psize > (int)sizeof(payload)) {
        fprintf(stderr, "  %s: payload %d exceeds buffer\n", cs_type_name(type), psize);
        return -1;
      }
      if (tp_recv_all(tcp, payload, psize) < 0) {
        return -1;
      }
    }

    count++;
    printf("  [%2d] %-16s payload=%d\n", count, cs_type_name(type), psize);

    /* Peek at a couple of interesting fields for a sanity check. */
    if (type == INFO_VFO && psize >= (int)(sizeof(VFO_DATA) - sizeof(HEADER))) {
      /* VFO_DATA begins right after HEADER; its first field is the frequency. */
      VFO_DATA v;
      memcpy(&v, &h, sizeof(HEADER));
      memcpy((char *)&v + sizeof(HEADER), payload, sizeof(VFO_DATA) - sizeof(HEADER));
      printf("        VFO id=%d frequency=%lld Hz\n", v.vfo, (long long)from_64(v.frequency));
    }

    if (type == CMD_START_RADIO) {
      printf("  -> CMD_START_RADIO after %d messages\n", count);
      return 0;
    }
  }
}

/* ------------------------------------------------------------------ */
/* Spectrum                                                           */
/* ------------------------------------------------------------------ */

static void render_sparkline(const uint8_t *dbm_bytes, int width) {
  static const char *ramp = " .:-=+*#%@";
  const int cols = 72;
  int lo = 255, hi = 0;
  for (int i = 0; i < width; i++) {
    if (dbm_bytes[i] < lo) lo = dbm_bytes[i];
    if (dbm_bytes[i] > hi) hi = dbm_bytes[i];
  }
  int span = (hi > lo) ? (hi - lo) : 1;
  printf("        dBm range [%d .. %d]\n        ", lo - 200, hi - 200);
  for (int c = 0; c < cols; c++) {
    /* Max over the bucket, so peaks stay visible. */
    int a = (int)((long)c * width / cols);
    int b = (int)((long)(c + 1) * width / cols);
    if (b <= a) b = a + 1;
    int mx = 0;
    for (int i = a; i < b && i < width; i++) {
      if (dbm_bytes[i] > mx) mx = dbm_bytes[i];
    }
    int level = (mx - lo) * 9 / span;
    if (level < 0) level = 0;
    if (level > 9) level = 9;
    putchar(ramp[level]);
  }
  putchar('\n');
}

/*
 * Decode one INFO_RX_SPECTRUM datagram in `buf` (len bytes). If `show`, prints
 * a line and the sparkline. Fills *out_freq / *out_ctun (VFO-A dial and CTUN
 * frequencies) when non-NULL. 0 on success.
 */
static int handle_spectrum(const uint8_t *buf, int len, int frameno, int show,
                           long long *out_freq, long long *out_ctun) {
  if (len < (int)(sizeof(SPECTRUM_DATA) - SPECTRUM_DATA_SIZE)) {
    fprintf(stderr, "  spectrum datagram too short (%d)\n", len);
    return -1;
  }
  const SPECTRUM_DATA *sd = (const SPECTRUM_DATA *)buf;
  int width = from_16(sd->width);
  int compressed = sd->compressed;
  int cwidth = from_16(sd->compressed_width);

  if (width < 0 || width > SPECTRUM_DATA_SIZE) {
    fprintf(stderr, "  bogus width %d\n", width);
    return -1;
  }

  uint8_t dbm[SPECTRUM_DATA_SIZE];
  if (compressed) {
    uLongf dst = SPECTRUM_DATA_SIZE;
    if (uncompress(dbm, &dst, sd->sample, cwidth) != Z_OK || (int)dst != width) {
      fprintf(stderr, "  zlib uncompress failed (cwidth=%d -> %lu, want %d)\n",
              cwidth, (unsigned long)dst, width);
      return -1;
    }
  } else {
    memcpy(dbm, sd->sample, width);
  }

  if (out_freq) { *out_freq = (long long)from_64(sd->vfo_a_freq); }
  if (out_ctun) { *out_ctun = (long long)from_64(sd->vfo_a_ctun_freq); }

  if (show) {
    printf("  frame %2d: id=%d width=%d compressed=%d vfoA=%lld Hz S=%.1f dBm\n",
           frameno, sd->id, width, compressed,
           (long long)from_64(sd->vfo_a_freq), from_double(sd->rxlvl));
    render_sparkline(dbm, width);
  }
  return 0;
}

/*
 * Block until one INFO_RX_SPECTRUM is decoded off the UDP socket (ignoring
 * other datagram types), or the socket times out. 0 on success (fills
 * freq/ctun when non-NULL); -1 on timeout / error.
 */
static int next_spectrum(int udp, int frameno, int show,
                         long long *freq, long long *ctun) {
  static uint8_t dg[sizeof(SPECTRUM_DATA) + 64];
  for (;;) {
    ssize_t n = recv(udp, dg, sizeof(dg), 0);
    if (n < 0) {
      return -1; /* timeout (SO_RCVTIMEO) or error */
    }
    if (n < (int)sizeof(HEADER)) {
      continue;
    }
    HEADER *h = (HEADER *)dg;
    if (memcmp(h->sync, SYNCBYTES, 4) != 0) {
      continue;
    }
    if (from_16(h->data_type) != INFO_RX_SPECTRUM) {
      continue; /* INFO_RXAUDIO / INFO_DISPLAY / INFO_PS are ignored here */
    }
    if (handle_spectrum(dg, (int)n, frameno, show, freq, ctun) == 0) {
      return 0;
    }
    /* decode error: keep reading */
  }
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
  const char *host = (argc > 1) ? argv[1] : "127.0.0.1";
  int port         = (argc > 2) ? atoi(argv[2]) : 50000;
  /* Password: arg 3, else $PIHPSDR_PWD (keeps it out of argv / shell history),
   * else empty. */
  const char *pwd  = (argc > 3) ? argv[3] : getenv("PIHPSDR_PWD");
  if (pwd == NULL) {
    pwd = "";
  }

  printf("pihpsdr-client spike -> %s:%d\n", host, port);

  int tcp = tp_tcp_connect(host, port, 5000);
  if (tcp < 0) {
    fprintf(stderr, "cannot connect TCP\n");
    return 1;
  }
  printf("TCP connected.\n");

  uint8_t udp_hash[SHA512_DIGEST_LENGTH];
  printf("Handshake (KDF is 100000 SHA-512 rounds, ~1s)...\n");
  if (do_handshake(tcp, pwd, udp_hash) != 0) {
    close(tcp);
    return 1;
  }

  /* UDP registration: open, connect, then send the 64-byte auth hash so the
   * server learns our UDP source address/port. */
  int udp = tp_udp_connect(host, port);
  if (udp < 0) {
    close(tcp);
    return 1;
  }
  usleep(100000); /* let the server set up its UDP side (same as piHPSDR) */
  if (send(udp, udp_hash, SHA512_DIGEST_LENGTH, 0) < 0) {
    perror("udp register");
    close(tcp);
    close(udp);
    return 1;
  }
  printf("UDP registered.\n");

  printf("Ingesting startup burst:\n");
  if (ingest_until_start(tcp) != 0) {
    fprintf(stderr, "ingest failed\n");
    close(tcp);
    close(udp);
    return 1;
  }

  /* Start streaming RX0 spectrum to us (server keeps its own width/fps). */
  printf("Enabling RX0 spectrum stream (server-native width/fps)...\n");
  sp_send_startstop_rxspectrum(tcp, 0, 1);

  /* Receive spectrum on UDP with a timeout. */
  struct timeval tv = { SPIKE_UDP_TIMEOUT_MS / 1000, (SPIKE_UDP_TIMEOUT_MS % 1000) * 1000 };
  setsockopt(udp, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  int frames = 0;
  const char *ctl = getenv("SPIKE_SEND_CONTROL");

  if (!ctl) {
    /* Read-only: just decode frames, touch nothing on the radio. */
    printf("Waiting for spectrum (read-only)...\n");
    for (int i = 0; i < SPIKE_FRAMES; i++) {
      if (next_spectrum(udp, i, 1, NULL, NULL) != 0) {
        fprintf(stderr, "no more UDP after %d frames\n", frames);
        break;
      }
      frames++;
    }
    if (frames > 0) {
      printf("Control path NOT sent (read-only). Set SPIKE_SEND_CONTROL=1 to test it.\n");
    }
  } else {
    /*
     * Closed-loop control-path proof: nudge VFO-A +1000 Hz, observe the change
     * in the spectrum stream, then nudge it back -1000 Hz and observe the
     * restore. This leaves the radio exactly as it was. In CTUN mode the dial
     * frequency stays put and the CTUN frequency moves, so we watch both.
     */
    printf("Waiting for spectrum, then control-path test...\n");
    long long base_freq = -1, base_ctun = -1;
    for (int i = 0; i < 6; i++) {
      if (next_spectrum(udp, i, 1, &base_freq, &base_ctun) != 0) {
        break;
      }
      frames++;
    }

    if (frames == 0) {
      fprintf(stderr, "no spectrum — cannot run control test\n");
    } else {
      printf("\n[control] baseline VFO-A dial=%lld ctun=%lld Hz\n", base_freq, base_ctun);

      printf("[control] send CMD_MOVE +1000 Hz\n");
      sp_send_move(tcp, 0, +1000, 0);
      long long f = base_freq, c = base_ctun;
      int moved = 0;
      for (int i = 0; i < 20; i++) {
        if (next_spectrum(udp, frames, 0, &f, &c) != 0) { break; }
        frames++;
        if (f != base_freq || c != base_ctun) { moved = 1; break; }
      }
      printf("[control] after +1000: dial=%lld ctun=%lld Hz => %s\n",
             f, c, moved ? "MOVED" : "no change");

      printf("[control] send CMD_MOVE -1000 Hz (restore)\n");
      sp_send_move(tcp, 0, -1000, 0);
      long long f2 = f, c2 = c;
      int restored = 0;
      for (int i = 0; i < 20; i++) {
        if (next_spectrum(udp, frames, 0, &f2, &c2) != 0) { break; }
        frames++;
        if (f2 == base_freq && c2 == base_ctun) { restored = 1; break; }
      }
      printf("[control] after -1000: dial=%lld ctun=%lld Hz => %s\n",
             f2, c2, restored ? "RESTORED" : "not restored");

      printf("[control] CMD_MOVE path: %s\n",
             (moved && restored) ? "PROVEN (closed loop, VFO left unchanged)"
                                 : "INCONCLUSIVE");
    }
  }

  printf("Done: decoded %d spectrum frame(s).\n", frames);
  sp_send_startstop_rxspectrum(tcp, 0, 0);
  close(tcp);
  close(udp);
  return frames > 0 ? 0 : 2;
}
