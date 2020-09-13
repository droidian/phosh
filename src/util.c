/*
 * Copyright (C) 2018 Purism SPC
 * SPDX-License-Identifier: GPL-3.0+
 * Author: Guido Günther <agx@sigxcpu.org>
 */

#include "util.h"
#include <gtk/gtk.h>
#include <wayland-client-protocol.h>

/* Just wraps gtk_widget_destroy so we can use it with g_clear_pointer */
void
phosh_cp_widget_destroy (void *widget)
{
  gtk_widget_destroy (GTK_WIDGET (widget));
}

/* For GTK+3 apps the desktop_id and the app_id often don't match
   because the app_id is incorrectly just $(basename argv[0]). If we
   detect this case (no dot in app_id and starts with
   lowercase) work around this by trying org.gnome.<capitalized
   app_id>.
   Applications with "gnome-" prefix in their name also need to be
   handled there ("gnome-software" -> "org.gnome.Software").
*/
gchar*
phosh_fix_app_id (const gchar* app_id)
{
  if (strchr (app_id, '.') == NULL && !g_ascii_isupper (app_id[0])) {
    gint first_char = 0;
    if (g_str_has_prefix (app_id, "gnome-")) {
      first_char = strlen ("gnome-");
    }
    return g_strdup_printf ("org.gnome.%c%s", app_id[first_char] - 32, &(app_id[first_char + 1]));
  }
  return g_strdup (app_id);
}


/**
 * phosh_clear_handler:
 * @handler: the signal handler to disconnect
 * @object: the #GObject to remove the handler from
 *
 * Emulates g_clear_signal_handler for pre-2.62 GLib
 *
 * @handler is zerod when removed, can be called on a zerod @handler
 */
void
phosh_clear_handler (gulong *handler, gpointer object)
{
  g_return_if_fail (handler);
  g_return_if_fail (G_IS_OBJECT (object));

  if (*handler > 0) {
    g_signal_handler_disconnect (object, *handler);
    *handler = 0;
  }
}

/**
 * phosh_convert_buffer:
 * @data: the buffer data to modify
 * @format: the current pixel format
 * @width: image width
 * @height: image height
 * @stride: image stride
 *
 * Converts the buffer to ARGB format so that
 * is suitable for usage in Cairo.
 * If the buffer is already ARGB (or the conversion
 * is not implemented), nothing happens.
*/
void
phosh_convert_buffer (void *data, enum wl_shm_format format, guint width, guint height, guint stride)
{
  switch (format) {
  case WL_SHM_FORMAT_ABGR8888:
  case WL_SHM_FORMAT_XBGR8888:
    ;
    guint8 *_data = (guint8 *)data;
    for (int i = 0; i < height; i++) {
      for (int j = 0; j < width; j++) {
          guint32 *px = (guint32 *)(_data + i * stride + j * 4);
          guint8 r, g, b, a;

          a = (*px >> 24) & 0xFF;
          b = (*px >> 16) & 0xFF;
          g = (*px >> 8) & 0xFF;
          r = *px & 0xFF;
          *px = (a << 24) | (r << 16) | (g << 8) | b;
      }
    }
    break;
  default:
    break;
  }
}
