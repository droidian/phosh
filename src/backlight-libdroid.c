/*
 * Copyright (C) 2025 Droidian Project
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Author: Eugenio Paolantonio <eugenio@droidian.org>
 */

#define G_LOG_DOMAIN "phosh-backlight-libdroid"

#include "phosh-config.h"

#include "backlight-libdroid.h"

#include <libdroid/leds.h>

#define LIBDROID_BACKLIGHT_MIN 10
#define LIBDROID_BACKLIGHT_MAX 255

/**
 * PhoshBacklightLibdroid:
 *
 * A backlight managed via libdroid
 */

struct _PhoshBacklightLibdroid {
  PhoshBacklight parent;

  DroidLeds     *droid_leds;
};

static void initable_iface_init (GInitableIface *iface);

G_DEFINE_TYPE_WITH_CODE (PhoshBacklightLibdroid, phosh_backlight_libdroid, PHOSH_TYPE_BACKLIGHT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, initable_iface_init))


static int
phosh_backlight_libdroid_set_brightness_finish (PhoshBacklight  *backlight,
                                                GAsyncResult    *result,
                                                GError         **error)
{
  PhoshBacklightLibdroid *self = PHOSH_BACKLIGHT_LIBDROID (backlight);

  g_return_val_if_fail (g_task_is_valid (result, self), -1);

  return g_task_propagate_int (G_TASK (result), error);
}


static void
phosh_backlight_libdroid_set_brightness (PhoshBacklight      *backlight,
                                         int                  brightness,
                                         GCancellable        *cancellable,
                                         GAsyncReadyCallback  callback,
                                         gpointer             user_data)
{
  PhoshBacklightLibdroid *self = PHOSH_BACKLIGHT_LIBDROID (backlight);
  g_autoptr (GTask) task = NULL;

  g_return_if_fail (PHOSH_IS_BACKLIGHT_LIBDROID (self));

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_task_data (task, GINT_TO_POINTER (brightness), NULL);
  g_task_set_source_tag (task, phosh_backlight_libdroid_set_brightness);

  g_debug ("Setting brightness via libdroid: %d", brightness);
  droid_leds_set_backlight (self->droid_leds, brightness, TRUE);

  g_task_return_int (task, brightness);
}


static gboolean
initable_init (GInitable *initable, GCancellable *cancel, GError **error)
{
  PhoshBacklightLibdroid *self = PHOSH_BACKLIGHT_LIBDROID (initable);
  g_autoptr (GError) libdroid_err = NULL;
  guint brightness;

  self->droid_leds = droid_leds_new (&libdroid_err);

  if (libdroid_err || !droid_leds_is_kind_supported (self->droid_leds, DROID_LEDS_KIND_BACKLIGHT)) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "Unable to get backlight via libdroid");
    return FALSE;
  }

  phosh_backlight_set_range (PHOSH_BACKLIGHT (self), LIBDROID_BACKLIGHT_MIN, LIBDROID_BACKLIGHT_MAX);

  /* Reset backlight during startup */
  brightness = MAX (LIBDROID_BACKLIGHT_MIN, droid_leds_get_backlight (self->droid_leds));
  droid_leds_set_backlight (self->droid_leds, brightness, FALSE);
  phosh_backlight_backend_update_brightness (PHOSH_BACKLIGHT (self), brightness);

  return TRUE;
}


static void
initable_iface_init (GInitableIface *iface)
{
  iface->init = initable_init;
}


static void
phosh_backlight_libdroid_dispose (GObject *object)
{
  PhoshBacklightLibdroid *self = PHOSH_BACKLIGHT_LIBDROID (object);

  g_clear_object (&self->droid_leds);

  G_OBJECT_CLASS (phosh_backlight_libdroid_parent_class)->dispose (object);
}


static void
phosh_backlight_libdroid_class_init (PhoshBacklightLibdroidClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  PhoshBacklightClass *backlight_class = PHOSH_BACKLIGHT_CLASS (klass);

  object_class->dispose = phosh_backlight_libdroid_dispose;

  backlight_class->set_brightness = phosh_backlight_libdroid_set_brightness;
  backlight_class->set_brightness_finish = phosh_backlight_libdroid_set_brightness_finish;
}


static void
phosh_backlight_libdroid_init (PhoshBacklightLibdroid *self)
{
}


PhoshBacklightLibdroid *
phosh_backlight_libdroid_new (GError **error)
{
  return g_initable_new (PHOSH_TYPE_BACKLIGHT_LIBDROID, NULL, error,
                         NULL);
}
