/*
 * Copyright (C) 2020 Purism SPC
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Author: Guido Günther <agx@sigxcpu.org>
 */

#define G_LOG_DOMAIN "phosh-rotation-manager"

#include "config.h"
#include "rotation-manager.h"
#include "shell.h"
#include "sensor-proxy-manager.h"
#include "util.h"

#define ORIENTATION_LOCK_SCHEMA_ID "org.gnome.settings-daemon.peripherals.touchscreen"
#define ORIENTATION_LOCK_KEY       "orientation-lock"

/**
 * SECTION:rotation-manager
 * @short_description: The Rotation Manager
 * @Title: PhoshRotationManager
 *
 * #PhoshRotationManager is responsible for interfacing with
 * #PhoshSensorProxyManager to set the correct orientation of the *
 * #built-in display taking the #PhoshLockscreenManager's
 * #PhoshLockscreenManager:locked status and the orientation-lock
 * #GSetting into account.
 */

enum {
  PROP_0,
  PROP_SENSOR_PROXY_MANAGER,
  PROP_LOCKSCREEN_MANAGER,
  PROP_ORIENTATION_LOCKED,
  LAST_PROP,
};
static GParamSpec *props[LAST_PROP];

typedef struct _PhoshRotationManager {
  GObject                  parent;

  gboolean                 claimed;
  PhoshSensorProxyManager *sensor_proxy_manager;
  PhoshLockscreenManager  *lockscreen_manager;

  GSettings               *settings;
  gboolean                 orientation_locked;
} PhoshRotationManager;

G_DEFINE_TYPE (PhoshRotationManager, phosh_rotation_manager, G_TYPE_OBJECT);

/**
 * match_orientation:
 * @self: The #PhoshRotationManager
 *
 * Match the screen orientation to the sensor output.
 * Do nothing if orientation lock is on or there's no
 * sensor claimed.
 */
static void
match_orientation (PhoshRotationManager *self)
{
  PhoshShell *shell = phosh_shell_get_default ();
  const gchar *orient;
  PhoshMonitorTransform degree = PHOSH_MONITOR_TRANSFORM_NORMAL;

  orient = phosh_dbus_sensor_proxy_get_accelerometer_orientation (
    PHOSH_DBUS_SENSOR_PROXY (self->sensor_proxy_manager));

  g_debug ("Orientation changed: %s, locked: %d, claimed: %d",
           orient, self->orientation_locked, self->claimed);

  if (self->orientation_locked || !self->claimed)
    return;

  if (!g_strcmp0 ("normal", orient)) {
    degree = PHOSH_MONITOR_TRANSFORM_NORMAL;
  } else if (!g_strcmp0 ("right-up", orient)) {
    degree = PHOSH_MONITOR_TRANSFORM_90;
  } else if (!g_strcmp0 ("bottom-up", orient)) {
    degree = PHOSH_MONITOR_TRANSFORM_180;
  } else if (!g_strcmp0 ("left-up", orient)) {
    degree = PHOSH_MONITOR_TRANSFORM_270;
  }
  phosh_shell_set_transform (shell, degree);
}

static void
on_accelerometer_claimed (PhoshSensorProxyManager *sensor_proxy_manager,
                          GAsyncResult            *res,
                          PhoshRotationManager    *self)
{
  g_autoptr (GError) err = NULL;
  gboolean success;

  g_return_if_fail (PHOSH_IS_SENSOR_PROXY_MANAGER (sensor_proxy_manager));
  g_return_if_fail (PHOSH_IS_ROTATION_MANAGER (self));
  g_return_if_fail (sensor_proxy_manager == self->sensor_proxy_manager);

  success = phosh_dbus_sensor_proxy_call_claim_accelerometer_finish (
    PHOSH_DBUS_SENSOR_PROXY (sensor_proxy_manager),
    res, &err);
  if (success) {
    g_debug ("Claimed accelerometer");
    self->claimed = TRUE;
  } else {
    g_warning ("Failed to claim accelerometer: %s", err->message);
  }
  match_orientation (self);
  g_object_unref (self);
}

static void
on_accelerometer_released (PhoshSensorProxyManager *sensor_proxy_manager,
                           GAsyncResult            *res,
                           PhoshRotationManager    *self)
{
  g_autoptr (GError) err = NULL;
  gboolean success;

  g_return_if_fail (PHOSH_IS_SENSOR_PROXY_MANAGER (sensor_proxy_manager));
  g_return_if_fail (PHOSH_IS_ROTATION_MANAGER (self));
  g_return_if_fail (sensor_proxy_manager == self->sensor_proxy_manager);

  success = phosh_dbus_sensor_proxy_call_release_accelerometer_finish (
    PHOSH_DBUS_SENSOR_PROXY (sensor_proxy_manager),
    res, &err);
  if (success) {
    g_debug ("Released rotation sensor");
    self->claimed = FALSE;
  } else {
    g_warning ("Failed to release rotation sensor: %s", err->message);
  }
  g_object_unref (self);
}

static void
phosh_rotation_manager_claim_accelerometer (PhoshRotationManager *self, gboolean claim)
{
  if (claim == self->claimed)
    return;

  if (claim) {
    phosh_dbus_sensor_proxy_call_claim_accelerometer (
      PHOSH_DBUS_SENSOR_PROXY (self->sensor_proxy_manager),
      NULL,
      (GAsyncReadyCallback)on_accelerometer_claimed,
      g_object_ref (self));
  } else {
    phosh_dbus_sensor_proxy_call_release_accelerometer (
      PHOSH_DBUS_SENSOR_PROXY (self->sensor_proxy_manager),
      NULL,
      (GAsyncReadyCallback)on_accelerometer_released,
      g_object_ref (self));
  }
}

static void
on_has_accelerometer_changed (PhoshRotationManager    *self,
                              GParamSpec              *pspec,
                              PhoshSensorProxyManager *proxy)
{
  gboolean has_accel;

  /* Don't claim on screen lock, enables runtime pm */
  if (phosh_lockscreen_manager_get_locked (self->lockscreen_manager))
    return;

  has_accel = phosh_dbus_sensor_proxy_get_has_accelerometer (
    PHOSH_DBUS_SENSOR_PROXY (self->sensor_proxy_manager));

  g_debug ("Found %s accelerometer", has_accel ? "a" : "no");
  phosh_rotation_manager_claim_accelerometer (self, has_accel);
}

static void
on_lockscreen_manager_locked (PhoshRotationManager *self, GParamSpec *pspec,
                              PhoshLockscreenManager *lockscreen_manager)
{
  gboolean locked;

  g_return_if_fail (PHOSH_IS_ROTATION_MANAGER (self));
  g_return_if_fail (PHOSH_IS_LOCKSCREEN_MANAGER (lockscreen_manager));

  locked = phosh_lockscreen_manager_get_locked (self->lockscreen_manager);
  phosh_rotation_manager_claim_accelerometer (self, !locked);
}

static void
on_accelerometer_orientation_changed (PhoshRotationManager    *self,
                                      GParamSpec              *pspec,
                                      PhoshSensorProxyManager *sensor)
{
  g_return_if_fail (PHOSH_IS_ROTATION_MANAGER (self));
  g_return_if_fail (self->sensor_proxy_manager == sensor);

  match_orientation (self);
}

static void
phosh_rotation_manager_set_property (GObject      *object,
                                     guint         property_id,
                                     const GValue *value,
                                     GParamSpec   *pspec)
{
  PhoshRotationManager *self = PHOSH_ROTATION_MANAGER (object);

  switch (property_id) {
  case PROP_SENSOR_PROXY_MANAGER:
    /* construct only */
    self->sensor_proxy_manager = g_value_dup_object (value);
    break;
  case PROP_LOCKSCREEN_MANAGER:
    /* construct only */
    self->lockscreen_manager = g_value_dup_object (value);
    break;
  case PROP_ORIENTATION_LOCKED:
    phosh_rotation_manager_set_orientation_locked (self,
                                                   g_value_get_boolean (value));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}

static void
phosh_rotation_manager_get_property (GObject    *object,
                                     guint       property_id,
                                     GValue     *value,
                                     GParamSpec *pspec)
{
  PhoshRotationManager *self = PHOSH_ROTATION_MANAGER (object);

  switch (property_id) {
  case PROP_SENSOR_PROXY_MANAGER:
    g_value_set_object (value, self->sensor_proxy_manager);
    break;
  case PROP_LOCKSCREEN_MANAGER:
    g_value_set_object (value, self->lockscreen_manager);
    break;
  case PROP_ORIENTATION_LOCKED:
    g_value_set_boolean (value, self->orientation_locked);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}

static void
phosh_rotation_manager_constructed (GObject *object)
{
  PhoshRotationManager *self = PHOSH_ROTATION_MANAGER (object);

  g_signal_connect_swapped (self->lockscreen_manager,
                            "notify::locked",
                            (GCallback) on_lockscreen_manager_locked,
                            self);

  g_signal_connect_swapped (self->sensor_proxy_manager,
                            "notify::accelerometer-orientation",
                            (GCallback) on_accelerometer_orientation_changed,
                            self);

  g_signal_connect_swapped (self->sensor_proxy_manager,
                            "notify::has-accelerometer",
                            (GCallback) on_has_accelerometer_changed,
                            self);
  on_has_accelerometer_changed (self, NULL, self->sensor_proxy_manager);

  self->settings = g_settings_new (ORIENTATION_LOCK_SCHEMA_ID);

  g_settings_bind (self->settings,
                   ORIENTATION_LOCK_KEY,
                   self,
                   "orientation-locked",
                   G_BINDING_SYNC_CREATE
                   | G_BINDING_BIDIRECTIONAL);

  G_OBJECT_CLASS (phosh_rotation_manager_parent_class)->constructed (object);
}

static void
phosh_rotation_manager_dispose (GObject *object)
{
  PhoshRotationManager *self = PHOSH_ROTATION_MANAGER (object);

  g_clear_object (&self->settings);

  if (self->sensor_proxy_manager) {
    g_signal_handlers_disconnect_by_data (self->sensor_proxy_manager,
                                          self);
    /* Sync call since we're going away */
    phosh_dbus_sensor_proxy_call_release_accelerometer_sync (
      PHOSH_DBUS_SENSOR_PROXY (self->sensor_proxy_manager), NULL, NULL);
    g_clear_object (&self->sensor_proxy_manager);
  }

  if (self->lockscreen_manager) {
    g_signal_handlers_disconnect_by_data (self->lockscreen_manager,
                                          self);
    g_clear_object (&self->lockscreen_manager);
  }

  G_OBJECT_CLASS (phosh_rotation_manager_parent_class)->dispose (object);
}

static void
phosh_rotation_manager_class_init (PhoshRotationManagerClass *klass)
{
  GObjectClass *object_class = (GObjectClass *)klass;

  object_class->constructed = phosh_rotation_manager_constructed;
  object_class->dispose = phosh_rotation_manager_dispose;

  object_class->set_property = phosh_rotation_manager_set_property;
  object_class->get_property = phosh_rotation_manager_get_property;

  props[PROP_SENSOR_PROXY_MANAGER] =
    g_param_spec_object (
      "sensor-proxy-manager",
      "Sensor proxy manager",
      "The object inerfacing with iio-sensor-proxy",
      PHOSH_TYPE_SENSOR_PROXY_MANAGER,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  props[PROP_LOCKSCREEN_MANAGER] =
    g_param_spec_object (
      "lockscreen-manager",
      "Lockscren manager",
      "The object managing the lock screen",
      PHOSH_TYPE_LOCKSCREEN_MANAGER,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  props[PROP_ORIENTATION_LOCKED] =
    g_param_spec_boolean (
      "orientation-locked",
      "Screen orientation locked",
      "Whether the screen orientation is locked",
      TRUE,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, LAST_PROP, props);

}

static void
phosh_rotation_manager_init (PhoshRotationManager *self)
{
}


PhoshRotationManager *
phosh_rotation_manager_new (PhoshSensorProxyManager *sensor_proxy_manager,
                            PhoshLockscreenManager  *lockscreen_manager)
{
  return g_object_new (PHOSH_TYPE_ROTATION_MANAGER,
                       "sensor-proxy-manager", sensor_proxy_manager,
                       "lockscreen-manager", lockscreen_manager,
                       NULL);
}

void
phosh_rotation_manager_set_orientation_locked (PhoshRotationManager *self, gboolean locked)
{
  g_return_if_fail (PHOSH_IS_ROTATION_MANAGER (self));

  if (locked == self->orientation_locked)
    return;

  self->orientation_locked = locked;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ORIENTATION_LOCKED]);
  match_orientation (self);
}

gboolean
phosh_rotation_manager_get_orientation_locked (PhoshRotationManager *self)
{
  g_return_val_if_fail (PHOSH_IS_ROTATION_MANAGER (self), TRUE);

  return self->orientation_locked;
}
