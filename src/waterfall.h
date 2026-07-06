/*
 * waterfall.h — scrolling waterfall for the RX spectrum.
 *
 * Keeps a colour-mapped image of recent spectrum rows. Each pushed frame adds
 * a new line at the top and scrolls the rest down. Rendered with Cairo. GTK-free
 * so it can also be driven by the headless render test.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PIHPSDR_CLIENT_WATERFALL_H
#define PIHPSDR_CLIENT_WATERFALL_H

#include <stdint.h>
#include <cairo.h>

typedef struct Waterfall Waterfall;

Waterfall *waterfall_new(void);
void       waterfall_free(Waterfall *wf);

/*
 * Add one spectrum row. `dbm` holds `n` raw column bytes (dBm = dbm[i] - 200).
 * The waterfall auto-sizes to `n` columns; a width change clears the history.
 */
void waterfall_push(Waterfall *wf, const uint8_t *dbm, int n);

/* Blit the waterfall into the rectangle (x,y,w,h), scaled to fit. */
void waterfall_draw(Waterfall *wf, cairo_t *cr, int x, int y, int w, int h);

#endif /* PIHPSDR_CLIENT_WATERFALL_H */
