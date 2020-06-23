/*
 * Copyright (C) 2020 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Author: Julian Sparber <julian.sparber@puri.sm>
 */

#define G_LOG_DOMAIN "phosh-rotateinfo"

#include "config.h"

#include "rotateinfo.h"
#include "shell.h"

/**
 * SECTION:rotateinfo
 * @short_description: A widget to display the rotate lock status
 * @Title: PhoshRotateInfo
 *
 * A #PhoshStatusIcon to display the rotation lock status.
 * It can either display whether a rotation lock is currently active or
 * if the output is in portrait/landscape mode.
 */

typedef struct _PhoshRotateInfo {
  PhoshStatusIcon     parent;

  PhoshRotateInfoMode mode;
} PhoshRotateInfo;


G_DEFINE_TYPE (PhoshRotateInfo, phosh_rotate_info, PHOSH_TYPE_STATUS_ICON)

static void
set_toggle_state (PhoshRotateInfo *self)
{
  PhoshShell *shell = phosh_shell_get_default ();
  PhoshMonitor *monitor = phosh_shell_get_primary_monitor (shell);
  gboolean monitor_is_landscape;
  gboolean portrait;

  switch (phosh_shell_get_transform (shell)) {
  case PHOSH_MONITOR_TRANSFORM_NORMAL:
  case PHOSH_MONITOR_TRANSFORM_FLIPPED:
  case PHOSH_MONITOR_TRANSFORM_180:
  case PHOSH_MONITOR_TRANSFORM_FLIPPED_180:
    portrait = TRUE;
    break;
  case PHOSH_MONITOR_TRANSFORM_90:
  case PHOSH_MONITOR_TRANSFORM_FLIPPED_90:
  case PHOSH_MONITOR_TRANSFORM_270:
  case PHOSH_MONITOR_TRANSFORM_FLIPPED_270:
    portrait = FALSE;
    break;
  default:
    g_warn_if_reached();
    portrait = TRUE;
  }

  if (self->mode != PHOSH_ROTATE_INFO_MODE_TOGGLE)
    return;

  /* If we have a landscape monitor (tv, laptop) flip the rotation */
  monitor_is_landscape = ((double)monitor->width / (double)monitor->height) > 1.0;
  portrait = monitor_is_landscape ? !portrait : portrait;

  g_debug ("Potrait: %d, width: %d, height: %d", portrait, monitor->width, monitor->height);
  if (portrait) {
    phosh_status_icon_set_icon_name (PHOSH_STATUS_ICON (self), "screen-rotation-portrait-symbolic");
    phosh_status_icon_set_info (PHOSH_STATUS_ICON (self), _("Portrait"));
  } else {
    phosh_status_icon_set_icon_name (PHOSH_STATUS_ICON (self), "screen-rotation-landscape-symbolic");
    phosh_status_icon_set_info (PHOSH_STATUS_ICON (self), _("Landscape"));
  }
}

static gboolean
binding_set_lock_icon (GBinding        *binding,
                       const GValue    *from_value,
                       GValue          *to_value,
                       PhoshRotateInfo *self)
{
  gboolean locked = g_value_get_boolean (from_value);
  const char *icon_name;

  icon_name = locked ? "rotation-locked-symbolic" : "rotation-allowed-symbolic";
  g_value_set_string (to_value, icon_name);

  return TRUE;
}

static gboolean
binding_set_lock_info (GBinding        *binding,
                       const GValue    *from_value,
                       GValue          *to_value,
                       PhoshRotateInfo *self)

{
  gboolean locked = g_value_get_boolean (from_value);
  const gchar *info;

  if (self->mode != PHOSH_ROTATE_INFO_MODE_LOCK)
    return TRUE;

  /* Translators: Automatic screen orientation is either on (enabled) or off (locked/disabled) */
  info = locked ? _("Off") : _("On");
  g_value_set_string (to_value, info);

  return TRUE;
}


static void
phosh_rotate_info_class_init (PhoshRotateInfoClass *klass)
{
}


static void
on_rotation_manager_changed (PhoshRotateInfo *self, GParamSpec *pspec, PhoshShell *shell)
{
  PhoshRotationManager *rotation_manager;

  g_return_if_fail (PHOSH_IS_ROTATE_INFO (self));
  g_return_if_fail (PHOSH_SHELL (shell));

  rotation_manager = phosh_shell_get_rotation_manager (shell);
  if (!rotation_manager) {
    phosh_rotate_info_set_mode (self, PHOSH_ROTATE_INFO_MODE_TOGGLE);
    return;
  }

  g_object_bind_property_full (rotation_manager,
                               "orientation-locked",
                               self,
                               "icon-name",
                               G_BINDING_SYNC_CREATE
                               | G_BINDING_DEFAULT,
                               (GBindingTransformFunc)binding_set_lock_icon,
                               NULL,
                               self,
                               NULL);
  g_object_bind_property_full (rotation_manager,
                               "orientation-locked",
                               self,
                               "info",
                               G_BINDING_SYNC_CREATE
                               | G_BINDING_DEFAULT,
                               (GBindingTransformFunc)binding_set_lock_info,
                               NULL,
                               self,
                               NULL);
  phosh_rotate_info_set_mode (self, PHOSH_ROTATE_INFO_MODE_LOCK);
}

static void
phosh_rotate_info_init (PhoshRotateInfo *self)
{
  self->mode = PHOSH_ROTATE_INFO_MODE_TOGGLE;

  g_signal_connect_object (phosh_shell_get_default (),
                           "notify::rotation",
                           G_CALLBACK (set_toggle_state),
                           self,
                           G_CONNECT_SWAPPED);
  set_toggle_state (self);

  /* Rotation manager might not be there when iio-sensor-proxy is missing
     so only connect to it once valid */
  g_signal_connect_object (phosh_shell_get_default (),
                           "notify::rotation-manager",
                           G_CALLBACK (on_rotation_manager_changed),
                           self,
                           G_CONNECT_SWAPPED);
}

GtkWidget *
phosh_rotate_info_new (void)
{
  return g_object_new (PHOSH_TYPE_ROTATE_INFO, NULL);
}

PhoshRotateInfoMode
phosh_rotate_info_get_mode (PhoshRotateInfo *self)
{
  g_return_val_if_fail (PHOSH_IS_ROTATE_INFO (self), PHOSH_ROTATE_INFO_MODE_LOCK);

  return self->mode;
}

void
phosh_rotate_info_set_mode (PhoshRotateInfo *self, PhoshRotateInfoMode mode)
{
  PhoshRotationManager *rotation_manager;
  PhoshShell *shell  = phosh_shell_get_default ();

  g_return_if_fail (PHOSH_IS_ROTATE_INFO (self));

  if (mode == self->mode)
    return;

  self->mode = mode;

  g_debug ("Setting mode: %d", mode);
  switch (mode) {
  case PHOSH_ROTATE_INFO_MODE_TOGGLE:
    set_toggle_state (self);
    break;
  case PHOSH_ROTATE_INFO_MODE_LOCK:
    rotation_manager = phosh_shell_get_rotation_manager (shell);
    if (rotation_manager)
      g_object_notify (G_OBJECT (rotation_manager), "orientation-locked");
    break;
  default:
    g_assert_not_reached ();
  }
}
