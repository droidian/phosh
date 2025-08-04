/*
 * Copyright (C) 2020 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Author: Guido Günther <agx@sigxcpu.org>
 */

#define G_LOG_DOMAIN "phosh-proximity"

#include "phosh-config.h"
#include "proximity.h"
#include "shell-priv.h"
#include "sensor-proxy-manager.h"
#include "util.h"

#include <cui-call.h>

/**
 * PhoshProximity:
 *
 * Proximity sensor handling
 *
 * #PhoshProximity handles enabling and disabling the proximity detection
 * based on e.g. active calls.
 */


enum {
  PROP_0,
  PROP_SENSOR_PROXY_MANAGER,
  PROP_CALLS_MANAGER,
  PROP_NEAR,
  LAST_PROP,
};
static GParamSpec *props[LAST_PROP];


typedef struct _PhoshProximity {
  GObject parent;

  gboolean has_proximity;
  gboolean claimed;
  PhoshSensorProxyManager *sensor_proxy_manager;
  PhoshCallsManager *calls_manager;
  gboolean near;
  guint timeout_id;

  GCancellable *cancel;
  GSettings      *settings;
} PhoshProximity;

G_DEFINE_TYPE (PhoshProximity, phosh_proximity, G_TYPE_OBJECT);


static void
on_proximity_claimed (PhoshSensorProxyManager *sensor_proxy_manager,
                      GAsyncResult            *res,
                      PhoshProximity          *self)
{
  g_autoptr (GError) err = NULL;
  gboolean success;

  g_return_if_fail (PHOSH_IS_SENSOR_PROXY_MANAGER (sensor_proxy_manager));

  success = phosh_dbus_sensor_proxy_call_claim_proximity_finish (
    PHOSH_DBUS_SENSOR_PROXY (sensor_proxy_manager),
    res, &err);

  if (success == FALSE) {
    phosh_async_error_warn (err, "Failed to claim proximity sensor");
    return;
  }

  g_return_if_fail (PHOSH_IS_PROXIMITY (self));
  g_return_if_fail (sensor_proxy_manager == self->sensor_proxy_manager);

  g_debug ("Claimed proximity sensor");
  self->claimed = TRUE;
}


static void
on_proximity_released (PhoshSensorProxyManager *sensor_proxy_manager,
                       GAsyncResult            *res,
                       PhoshProximity          *self)
{
  g_autoptr (GError) err = NULL;
  gboolean success;

  g_return_if_fail (PHOSH_IS_SENSOR_PROXY_MANAGER (sensor_proxy_manager));

  success = phosh_dbus_sensor_proxy_call_release_proximity_finish (
    PHOSH_DBUS_SENSOR_PROXY(sensor_proxy_manager),
    res, &err);

  if (success == FALSE) {
    if (!phosh_async_error_warn (err, "Failed to release proximity sensor")) {
      self->near = FALSE;
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_NEAR]);
    }
    return;
  }

  g_debug ("Released proximity sensor");
  self->claimed = FALSE;

  self->near = FALSE;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_NEAR]);
}


static void
phosh_proximity_claim_proximity (PhoshProximity *self, gboolean claim)
{
  if (claim == self->claimed)
    return;

  if (claim) {
    phosh_dbus_sensor_proxy_call_claim_proximity (
      PHOSH_DBUS_SENSOR_PROXY (self->sensor_proxy_manager),
      self->cancel,
      (GAsyncReadyCallback)on_proximity_claimed,
      self);
  } else {
    phosh_dbus_sensor_proxy_call_release_proximity (
      PHOSH_DBUS_SENSOR_PROXY (self->sensor_proxy_manager),
      self->cancel,
      (GAsyncReadyCallback)on_proximity_released,
      self);
  }
}


static void
on_has_proximity_changed (PhoshProximity          *self,
                          GParamSpec              *pspec,
                          PhoshSensorProxyManager *proxy)
{
  self->has_proximity = phosh_dbus_sensor_proxy_get_has_proximity (
    PHOSH_DBUS_SENSOR_PROXY (self->sensor_proxy_manager));

  g_debug ("Found %s proximity sensor", self->has_proximity ? "a" : "no");

  /* If the proxy went a way we always unclaim but only claim on ongoing calls: */
  if (!phosh_calls_manager_get_active_call_handle (self->calls_manager) && self->has_proximity)
    return;

  phosh_proximity_claim_proximity (self, self->has_proximity);
}

static void
on_call_state_changed (PhoshProximity *self,
                       GParamSpec     *pspec,
                       PhoshCall      *call)
{
  if (cui_call_get_state (CUI_CALL (call)) == CUI_CALL_STATE_ACTIVE)
    phosh_shell_enable_power_save (phosh_shell_get_default (), TRUE);
}

static void
on_calls_manager_active_call_changed (PhoshProximity    *self,
                                      GParamSpec        *pspec,
                                      PhoshCallsManager *calls_manager)
{
  gboolean active;
  const char *handle;
  PhoshCall *call;

  g_return_if_fail (PHOSH_IS_PROXIMITY (self));
  g_return_if_fail (PHOSH_IS_CALLS_MANAGER (calls_manager));

  handle = phosh_calls_manager_get_active_call_handle (self->calls_manager);
  active = !!handle;

  if (g_settings_get_boolean (self->settings, "enable-proximity-sensor") &&
      self->has_proximity) {
    phosh_proximity_claim_proximity (self, active);
  } else {
    if (active) {
      call = phosh_calls_manager_get_call (self->calls_manager, handle);
      g_signal_connect_swapped (call,
                                "notify::state",
                                G_CALLBACK (on_call_state_changed),
                                self);
    }
  }
  /* TODO: if call is over wait until we hit the threshold */
}

static gboolean
notify_near_state (PhoshProximity *self)
{

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_NEAR]);

  return FALSE;
}

static void
on_proximity_near_changed (PhoshProximity          *self,
                           GParamSpec              *pspec,
                           PhoshSensorProxyManager *sensor)
{
  if (!self->claimed)
    return;

  self->near = phosh_dbus_sensor_proxy_get_proximity_near (
    PHOSH_DBUS_SENSOR_PROXY (self->sensor_proxy_manager));
  g_clear_handle_id (&self->timeout_id, g_source_remove);
  self->timeout_id = g_timeout_add (250, (GSourceFunc) notify_near_state, self);
  g_debug ("Proximity near changed: %d", self->near);
}

static void
phosh_proximity_set_property (GObject *object,
                             guint property_id,
                             const GValue *value,
                             GParamSpec *pspec)
{
  PhoshProximity *self = PHOSH_PROXIMITY (object);

  switch (property_id) {
    case PROP_SENSOR_PROXY_MANAGER:
      /* construct only */
      self->sensor_proxy_manager = g_value_dup_object (value);
      break;
    case PROP_CALLS_MANAGER:
      /* construct only */
      self->calls_manager = g_value_dup_object (value);
      break;
    case PROP_NEAR:
      /* construct only */
      self->near = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}


static void
phosh_proximity_get_property (GObject *object,
                             guint property_id,
                             GValue *value,
                             GParamSpec *pspec)
{
  PhoshProximity *self = PHOSH_PROXIMITY (object);

  switch (property_id) {
  case PROP_SENSOR_PROXY_MANAGER:
    g_value_set_object (value, self->sensor_proxy_manager);
    break;
  case PROP_CALLS_MANAGER:
    g_value_set_object (value, self->calls_manager);
    break;
  case PROP_NEAR:
    g_value_set_boolean (value, self->near);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}


static void
phosh_proximity_constructed (GObject *object)
{
  PhoshProximity *self = PHOSH_PROXIMITY (object);

  self->near = FALSE;
  self->settings = g_settings_new (PHOSH_SHELL_PROXIMITY_SCHEMA_ID);

  g_signal_connect_swapped (self->calls_manager,
                            "notify::active-call",
                            G_CALLBACK (on_calls_manager_active_call_changed),
                            self);

  g_signal_connect_swapped (self->sensor_proxy_manager,
                            "notify::proximity-near",
                            (GCallback) on_proximity_near_changed,
                            self);

  g_signal_connect_swapped (self->sensor_proxy_manager,
                            "notify::has-proximity",
                            (GCallback) on_has_proximity_changed,
                            self);
  on_has_proximity_changed (self, NULL, self->sensor_proxy_manager);

  G_OBJECT_CLASS (phosh_proximity_parent_class)->constructed (object);
}


static void
phosh_proximity_dispose (GObject *object)
{
  PhoshProximity *self = PHOSH_PROXIMITY (object);

  g_cancellable_cancel (self->cancel);
  g_clear_object (&self->cancel);

  if (self->sensor_proxy_manager) {
    g_signal_handlers_disconnect_by_data (self->sensor_proxy_manager,
                                          self);
    phosh_dbus_sensor_proxy_call_release_proximity_sync (
      PHOSH_DBUS_SENSOR_PROXY(self->sensor_proxy_manager), NULL, NULL);
    g_clear_object (&self->sensor_proxy_manager);
  }

  if (self->calls_manager) {
     g_signal_handlers_disconnect_by_data (self->calls_manager,
                                           self);
     g_clear_object (&self->calls_manager);
  }

  g_clear_handle_id (&self->timeout_id, g_source_remove);

  g_clear_object (&self->settings);

  G_OBJECT_CLASS (phosh_proximity_parent_class)->dispose (object);
}


static void
phosh_proximity_class_init (PhoshProximityClass *klass)
{
  GObjectClass *object_class = (GObjectClass *)klass;

  object_class->constructed = phosh_proximity_constructed;
  object_class->dispose = phosh_proximity_dispose;

  object_class->set_property = phosh_proximity_set_property;
  object_class->get_property = phosh_proximity_get_property;

  /* PhoshProximity:sensor-proxy-manager:
   *
   * The sensor proxy manager
   */
  props[PROP_SENSOR_PROXY_MANAGER] =
    g_param_spec_object ("sensor-proxy-manager", "", "",
                         PHOSH_TYPE_SENSOR_PROXY_MANAGER,
                         G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  /* PhoshProximity:calls-manager:
   *
   * The calls manager
   */
  props[PROP_CALLS_MANAGER] =
    g_param_spec_object ("calls-manager", "", "",
                         PHOSH_TYPE_CALLS_MANAGER,
                         G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  props[PROP_NEAR] =
    g_param_spec_boolean ("near", "", "",
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, LAST_PROP, props);

}


static void
phosh_proximity_init (PhoshProximity *self)
{
  self->cancel = g_cancellable_new ();
}


PhoshProximity *
phosh_proximity_new (PhoshSensorProxyManager *sensor_proxy_manager,
                     PhoshCallsManager *calls_manager)
{
  return g_object_new (PHOSH_TYPE_PROXIMITY,
                       "sensor-proxy-manager", sensor_proxy_manager,
                       "calls-manager", calls_manager,
                       "near", FALSE,
                       NULL);
}

gboolean
phosh_proximity_near (PhoshProximity *self)
{
  g_return_val_if_fail (PHOSH_IS_PROXIMITY (self), FALSE);

  return self->near;
}

gboolean
phosh_proximity_sensor_enabled (PhoshProximity *self)
{
  return self->has_proximity && g_settings_get_boolean (self->settings, "enable-proximity-sensor");
}
