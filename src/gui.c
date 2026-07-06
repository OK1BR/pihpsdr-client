/*
 * gui.c — GTK4 panadapter window for pihpsdr-client (Milestone 2, step 1).
 *
 * Connects via client.{h,c}, then renders the live RX0 spectrum in a
 * GtkDrawingArea (drawing delegated to panadapter.{h,c}). A tick callback pulls
 * the latest decoded frame from the receive thread and redraws at display rate.
 *
 * Usage:  pihpsdr-client-gui [host] [port] [password]   (or $PIHPSDR_PWD)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "panadapter.h"

typedef struct {
  Client     *client;
  int         connected;
  int         conn_err;

  ClientFrame frame;
  uint64_t    last_seq;
  int         have_frame;

  GtkWidget  *area;
} App;

static void draw_cb(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data) {
  (void)area;
  App *app = (App *)data;

  if (!app->connected) {
    char msg[128];
    snprintf(msg, sizeof(msg), "Not connected: %s", client_strerror(app->conn_err));
    panadapter_draw(cr, w, h, NULL, msg);
    return;
  }
  if (!app->have_frame) {
    panadapter_draw(cr, w, h, NULL, "Connected — waiting for spectrum…");
    return;
  }
  panadapter_draw(cr, w, h, &app->frame, NULL);
}

static gboolean tick_cb(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
  (void)clock;
  App *app = (App *)data;
  if (app->connected &&
      client_latest(app->client, &app->frame, &app->last_seq)) {
    app->have_frame = 1;
    gtk_widget_queue_draw(widget);
  }
  return G_SOURCE_CONTINUE;
}

static void on_activate(GtkApplication *gtkapp, gpointer data) {
  App *app = (App *)data;

  GtkWidget *win = gtk_application_window_new(gtkapp);
  gtk_window_set_title(GTK_WINDOW(win), "pihpsdr-client — panadapter");
  gtk_window_set_default_size(GTK_WINDOW(win), 1200, 480);

  app->area = gtk_drawing_area_new();
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(app->area), draw_cb, app, NULL);
  gtk_window_set_child(GTK_WINDOW(win), app->area);

  gtk_widget_add_tick_callback(app->area, tick_cb, app, NULL);
  gtk_window_present(GTK_WINDOW(win));
}

int main(int argc, char **argv) {
  const char *host = (argc > 1) ? argv[1] : "127.0.0.1";
  int         port = (argc > 2) ? atoi(argv[2]) : 50000;
  const char *pwd  = (argc > 3) ? argv[3] : getenv("PIHPSDR_PWD");
  if (!pwd) {
    pwd = "";
  }

  App app;
  memset(&app, 0, sizeof(app));
  app.client = client_new(host, port, pwd);

  app.conn_err = client_connect(app.client);
  if (app.conn_err == CLIENT_OK) {
    app.connected = 1;
    client_start(app.client);
    printf("Connected to %s:%d — streaming.\n", host, port);
  } else {
    fprintf(stderr, "connect failed: %s\n", client_strerror(app.conn_err));
    /* Still open the window to show the error. */
  }

  /* Do not hand argv to GTK (we parsed our own args). */
  GtkApplication *gtkapp = gtk_application_new("cz.ok1br.pihpsdr_client",
                                               G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(gtkapp, "activate", G_CALLBACK(on_activate), &app);
  int status = g_application_run(G_APPLICATION(gtkapp), 0, NULL);

  g_object_unref(gtkapp);
  client_free(app.client);
  return status;
}
