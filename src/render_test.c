/*
 * render_test.c — headless render check for the panadapter.
 *
 * Connects to a live server, grabs one decoded frame, renders it with
 * panadapter_draw() into a Cairo image surface and writes a PNG. No GTK main
 * loop and no display server needed — used to verify the rendering visually
 * without opening a window on the operator's desktop.
 *
 * Usage:  RENDER_OUT=/path.png pihpsdr-render-test [host] [port] [password]
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <cairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "client.h"
#include "panadapter.h"

int main(int argc, char **argv) {
  const char *host = (argc > 1) ? argv[1] : "127.0.0.1";
  int         port = (argc > 2) ? atoi(argv[2]) : 50000;
  const char *pwd  = (argc > 3) ? argv[3] : getenv("PIHPSDR_PWD");
  if (!pwd) {
    pwd = "";
  }
  const char *out = getenv("RENDER_OUT");
  if (!out) {
    out = "panadapter.png";
  }
  const int W = 1200, H = 480;

  Client *c = client_new(host, port, pwd);
  int rc = client_connect(c);

  ClientFrame f;
  memset(&f, 0, sizeof(f));
  const char *status = NULL;

  if (rc != CLIENT_OK) {
    status = client_strerror(rc);
    fprintf(stderr, "connect: %s\n", status);
  } else {
    client_start(c);
    uint64_t seq = 0;
    int got = 0;
    for (int i = 0; i < 50 && !got; i++) { /* up to ~5s */
      if (client_latest(c, &f, &seq)) {
        got = 1;
      } else {
        usleep(100000);
      }
    }
    if (!got) {
      status = "connected — no spectrum";
    } else {
      fprintf(stderr, "frame: width=%d vfoA=%lld Hz S=%.1f dBm\n",
              f.width, (long long)f.vfo_a_freq, f.s_dbm);
    }
  }

  cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
  cairo_t *cr = cairo_create(surf);
  panadapter_draw(cr, W, H, status ? NULL : &f, status);
  cairo_surface_flush(surf);
  cairo_status_t st = cairo_surface_write_to_png(surf, out);
  fprintf(stderr, "wrote %s (%s)\n", out, cairo_status_to_string(st));

  cairo_destroy(cr);
  cairo_surface_destroy(surf);
  client_free(c);
  return (st == CAIRO_STATUS_SUCCESS) ? 0 : 1;
}
