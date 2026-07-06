/*
 * panadapter.h — Cairo rendering of the RX panadapter.
 *
 * Pure drawing: given a cairo context, a size and a decoded frame, it paints
 * the dark background, dB grid, gradient-filled spectrum trace, VFO centre line
 * and readouts. Kept free of GTK so it can be driven both by the live GUI and
 * by a headless render test.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PIHPSDR_CLIENT_PANADAPTER_H
#define PIHPSDR_CLIENT_PANADAPTER_H

#include <cairo.h>

#include "client.h"

/*
 * Render into `cr` at size w x h.
 *   - if `status` is non-NULL, the background + grid are drawn with that message
 *     centred (used for "connecting" / error states) and `frame` is ignored;
 *   - otherwise `frame` (non-NULL) is drawn as a spectrum with readouts.
 */
void panadapter_draw(cairo_t *cr, int w, int h,
                     const ClientFrame *frame, const char *status);

#endif /* PIHPSDR_CLIENT_PANADAPTER_H */
