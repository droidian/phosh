/*
 * Copyright (C) 2018 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Author: Guido Günther <agx@sigxcpu.org>
 */

#include "util.h"
#include <gtk/gtk.h>
#include <wayland-client-protocol.h>

#include <systemd/sd-login.h>

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
char *
phosh_fix_app_id (const char *app_id)
{
  if (strchr (app_id, '.') == NULL && !g_ascii_isupper (app_id[0])) {
    int first_char = 0;
    if (g_str_has_prefix (app_id, "gnome-")) {
      first_char = strlen ("gnome-");
    }
    return g_strdup_printf ("org.gnome.%c%s", app_id[first_char] - 32, &(app_id[first_char + 1]));
  }
  return g_strdup (app_id);
}

/**
 * phosh_munge_app_id:
 * @app_id: the app_id
 *
 * Munges an app_id according to the rules used by
 * gnome-shell, feedbackd and phoc:
 *
 * Returns: The munged_app id
 */
char *
phosh_munge_app_id (const char *app_id)
{
  char *id = g_strdup (app_id);
  int i;

  if (g_str_has_suffix (id, ".desktop")) {
    char *c = g_strrstr (id, ".desktop");
    if (c)
      *c = '\0';
  }

  g_strcanon (id,
              "0123456789"
              "abcdefghijklmnopqrstuvwxyz"
              "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
              "-",
              '-');
  for (i = 0; id[i] != '\0'; i++)
    id[i] = g_ascii_tolower (id[i]);

  return id;
}

gboolean
phosh_find_systemd_session (char **session_id)
{
  int n_sessions;

  g_auto (GStrv) sessions = NULL;
  char *session;
  int i;

  n_sessions = sd_uid_get_sessions (getuid (), 0, &sessions);

  if (n_sessions < 0) {
    g_debug ("Failed to get sessions for user %d", getuid ());
    return FALSE;
  }

  session = NULL;
  for (i = 0; i < n_sessions; i++) {
    int r;
    g_autofree char *type = NULL;
    g_autofree char *desktop = NULL;

    r = sd_session_get_desktop (sessions[i], &desktop);
    if (r < 0) {
      g_debug ("Couldn't get desktop for session '%s': %s",
               sessions[i], strerror (-r));
      continue;
    }

    if (g_strcmp0 (desktop, "phosh") != 0)
      continue;

    r = sd_session_get_type (sessions[i], &type);
    if (r < 0) {
      g_debug ("Couldn't get type for session '%s': %s",
               sessions[i], strerror (-r));
      continue;
    }

    if (g_strcmp0 (type, "wayland") != 0)
      continue;

    session = sessions[i];
    break;
  }

  if (session != NULL)
    *session_id = g_strdup (session);

  return session != NULL;
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

/**
 * phosh_async_error_warn:
 * @err: (nullable): The error to check and print
 * @...: Format string followed by parameters to insert
 *       into the format string (as with printf())
 *
 * Prints a warning when @err is 'real' error. If it merely represents
 * a canceled operation it just logs a debug message. This is useful
 * to avoid this common pattern in async callbacks.
 *
 * Returns: TRUE if #err is cancellation.
 */

/**
 * phosh_dbus_service_error_warn
 * @err: (nullable): The error to check and print
 * @...: Format string followed by parameters to insert
 *       into the format string (as with printf())
 *
 * Prints a warning when @err is 'real' error. If it merely indicates
 * that the DBus service is not present at all it just logs a debug
 * message.
 *
 * Returns: TRUE if #err is cancellation.
 */

/* Helper since phosh_async_error_warn needs to be a macro to capture log_domain */
gboolean
phosh_error_warnv (const char *log_domain,
                   GError     *err,
                   GQuark      domain,
                   gint        code,
                   const gchar *fmt, ...)
{
  g_autofree char *msg = NULL;
  gboolean matched = FALSE;
  va_list args;

  if (err == NULL)
    return FALSE;

  va_start (args, fmt);
  msg = g_strdup_vprintf(fmt, args);
  va_end (args);

  if (g_error_matches (err, domain, code))
    matched = TRUE;

  g_log (log_domain,
         matched ? G_LOG_LEVEL_DEBUG : G_LOG_LEVEL_WARNING,
         "%s: %s", msg, err->message);

  return matched;
}
