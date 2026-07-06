/*
 * waterfall.c — see waterfall.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "waterfall.h"

#include <stdlib.h>
#include <string.h>

/* Colour-map range (dBm) and history depth (rows in the backing image). */
#define WF_LOW   -140.0
#define WF_HIGH   -50.0
#define WF_ROWS   256

struct Waterfall {
  cairo_surface_t *surf;   /* ARGB32, cols x WF_ROWS; row 0 is newest */
  int              cols;
  uint32_t         lut[256];
};

/* Build a classic waterfall palette LUT: black-blue -> cyan -> green ->
 * yellow -> orange -> red as intensity rises. */
static void build_lut(uint32_t *lut) {
  /* stops: position 0..1 and R,G,B */
  static const double stop[][4] = {
    {0.00,   0,   0,  12},
    {0.20,   0,  18, 110},
    {0.40,   0, 130, 180},
    {0.58,   0, 200, 120},
    {0.74, 180, 220,   0},
    {0.88, 255, 165,   0},
    {1.00, 255,  40,  30},
  };
  const int nstops = (int)(sizeof(stop) / sizeof(stop[0]));

  for (int i = 0; i < 256; i++) {
    double t = i / 255.0;
    int s = 0;
    while (s < nstops - 2 && t > stop[s + 1][0]) {
      s++;
    }
    double t0 = stop[s][0], t1 = stop[s + 1][0];
    double f = (t1 > t0) ? (t - t0) / (t1 - t0) : 0.0;
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    int r = (int)(stop[s][1] + f * (stop[s + 1][1] - stop[s][1]));
    int g = (int)(stop[s][2] + f * (stop[s + 1][2] - stop[s][2]));
    int b = (int)(stop[s][3] + f * (stop[s + 1][3] - stop[s][3]));
    lut[i] = 0xFFu << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
  }
}

Waterfall *waterfall_new(void) {
  Waterfall *wf = calloc(1, sizeof(Waterfall));
  build_lut(wf->lut);
  return wf;
}

void waterfall_free(Waterfall *wf) {
  if (!wf) {
    return;
  }
  if (wf->surf) {
    cairo_surface_destroy(wf->surf);
  }
  free(wf);
}

static void ensure_surface(Waterfall *wf, int n) {
  if (wf->surf && wf->cols == n) {
    return;
  }
  if (wf->surf) {
    cairo_surface_destroy(wf->surf);
  }
  wf->surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, n, WF_ROWS);
  wf->cols = n;
  /* Clear to the palette's lowest colour. */
  cairo_surface_flush(wf->surf);
  uint32_t *data = (uint32_t *)cairo_image_surface_get_data(wf->surf);
  int stride = cairo_image_surface_get_stride(wf->surf) / 4;
  for (int r = 0; r < WF_ROWS; r++) {
    for (int x = 0; x < n; x++) {
      data[r * stride + x] = wf->lut[0];
    }
  }
  cairo_surface_mark_dirty(wf->surf);
}

void waterfall_push(Waterfall *wf, const uint8_t *dbm, int n) {
  if (n < 1) {
    return;
  }
  ensure_surface(wf, n);

  cairo_surface_flush(wf->surf);
  uint8_t *base = cairo_image_surface_get_data(wf->surf);
  int stride = cairo_image_surface_get_stride(wf->surf); /* bytes */

  /* Scroll everything down by one row, then write the new row at the top. */
  memmove(base + stride, base, (size_t)stride * (WF_ROWS - 1));

  uint32_t *row = (uint32_t *)base;
  const double scale = 255.0 / (WF_HIGH - WF_LOW);
  for (int x = 0; x < n; x++) {
    double d = (double)dbm[x] - 200.0;
    int idx = (int)((d - WF_LOW) * scale);
    if (idx < 0) idx = 0;
    if (idx > 255) idx = 255;
    row[x] = wf->lut[idx];
  }

  cairo_surface_mark_dirty(wf->surf);
}

void waterfall_draw(Waterfall *wf, cairo_t *cr, int x, int y, int w, int h) {
  if (!wf->surf || wf->cols < 1) {
    return;
  }
  cairo_save(cr);
  cairo_rectangle(cr, x, y, w, h);
  cairo_clip(cr);
  cairo_translate(cr, x, y);
  cairo_scale(cr, (double)w / wf->cols, (double)h / WF_ROWS);
  cairo_set_source_surface(cr, wf->surf, 0, 0);
  /* NEAREST keeps signal streaks crisp (like piHPSDR) instead of blurring. */
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
  cairo_paint(cr);
  cairo_restore(cr);
}
