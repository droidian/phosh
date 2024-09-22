/*
 * Copyright (C) 2018 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Author: Guido Günther <agx@sigxcpu.org>
 */

#define G_LOG_DOMAIN "phosh-lockscreen-manager"

#include "lockscreen-manager.h"
#include "lockscreen.h"
#include "lockshield.h"
#include "monitor-manager.h"
#include "monitor/monitor.h"
#include "phosh-wayland.h"
#include "shell.h"
#include "util.h"
#include <gdk/gdkwayland.h>

/**
 * PhoshLockscreenManager:
 *
 * The singleton that manages screen locking
 *
 * The #PhoshLockscreenManager is responsible for putting the #PhoshLockscreen
 * on the primary output and a #PhoshLockshield on other outputs when the session
 * becomes idle or when invoked explicitly via phosh_lockscreen_manager_set_locked().
 */

enum {
  WAKEUP_OUTPUTS,
  N_SIGNALS
};
static guint signals[N_SIGNALS] = { 0 };

enum {
  PROP_0,
  PROP_LOCKED,
  PROP_CALLS_MANAGER,
  PROP_LAST_PROP
};
static GParamSpec *props[PROP_LAST_PROP];


struct _PhoshLockscreenManager {
  GObject parent;

  PhoshLockscreen      *lockscreen;     /* phone display lock screen */
  GPtrArray             *shields;       /* other outputs */

  gboolean locked;
  gboolean locking;
  gint64 active_time;                   /* when lock was activated (in us) */

  PhoshCallsManager    *calls_manager;  /* Calls DBus Interface */
};

G_DEFINE_TYPE (PhoshLockscreenManager, phosh_lockscreen_manager, G_TYPE_OBJECT)


static void
lockscreen_unlock_cb (PhoshLockscreenManager *self, PhoshLockscreen *lockscreen)
{
  PhoshShell *shell = phosh_shell_get_default ();
  PhoshMonitorManager *monitor_manager = phosh_shell_get_monitor_manager (shell);
  PhoshMonitor *primary_monitor = phosh_shell_get_primary_monitor (shell);

  g_return_if_fail (PHOSH_IS_LOCKSCREEN (lockscreen));
  g_return_if_fail (lockscreen == PHOSH_LOCKSCREEN (self->lockscreen));

  g_signal_handlers_disconnect_by_data (monitor_manager, self);
  g_signal_handlers_disconnect_by_data (primary_monitor, self);
  g_signal_handlers_disconnect_by_data (shell, self);
  g_clear_pointer (&self->lockscreen, phosh_cp_widget_destroy);

  /* Unlock all other outputs */
  g_clear_pointer (&self->shields, g_ptr_array_unref);

  self->locked = FALSE;
  self->active_time = 0;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_LOCKED]);
}


static void
lockscreen_wakeup_output_cb (PhoshLockscreenManager *self, PhoshLockscreen *lockscreen)
{
  g_return_if_fail (PHOSH_IS_LOCKSCREEN_MANAGER (self));
  g_return_if_fail (PHOSH_IS_LOCKSCREEN (lockscreen));

  /* we just proxy the signal here */
  g_signal_emit (self, signals[WAKEUP_OUTPUTS], 0);
}


/* Lock a non primary monitor bringing up a shield */
static void
lock_monitor (PhoshLockscreenManager *self,
              PhoshMonitor           *monitor)
{
  PhoshShell *shell = phosh_shell_get_default ();
  PhoshWayland *wl = phosh_wayland_get_default ();
  GtkWidget *shield;

  /* Primary monitor is handled via on_primary_monitor_changed */
  if (phosh_shell_get_primary_monitor (shell) == monitor)
    return;

  g_debug ("Adding shield for %s", monitor->name);
  shield = phosh_lockshield_new (phosh_wayland_get_zwlr_layer_shell_v1 (wl), monitor);

  g_object_set_data (G_OBJECT (shield), "phosh-monitor", monitor);

  g_ptr_array_add (self->shields, shield);
  gtk_widget_show (shield);
}


static void
remove_shield_by_monitor (PhoshLockscreenManager *self,
                          PhoshMonitor           *monitor)
{
  if (!self->shields) {
    g_debug ("Shields already gone");
    return;
  }

  for (int i = 0; i < self->shields->len; i++) {
    PhoshMonitor *shield_monitor;
    PhoshLockshield *shield = g_ptr_array_index (self->shields, i);

    shield_monitor = g_object_get_data (G_OBJECT (shield), "phosh-monitor");
    g_return_if_fail (PHOSH_IS_MONITOR (shield_monitor));
    if (shield_monitor == monitor) {
      g_debug ("Removing shield %p", shield);
      g_ptr_array_remove (self->shields, shield);
      break;
    }
  }
}


static void
on_monitor_removed (PhoshLockscreenManager *self,
                    PhoshMonitor           *monitor,
                    PhoshMonitorManager    *monitormanager)
{


  g_return_if_fail (PHOSH_IS_MONITOR (monitor));
  g_return_if_fail (PHOSH_IS_LOCKSCREEN_MANAGER (self));

  g_debug ("Monitor '%s' removed", monitor->name);
  remove_shield_by_monitor (self, monitor);
}


static void
on_monitor_added (PhoshLockscreenManager *self,
                  PhoshMonitor           *monitor,
                  PhoshMonitorManager    *monitormanager)
{
  g_return_if_fail (PHOSH_IS_MONITOR (monitor));
  g_return_if_fail (PHOSH_IS_LOCKSCREEN_MANAGER (self));

  g_debug ("Monitor '%s' added", monitor->name);
  lock_monitor (self, monitor);
}


static void
lock_primary_monitor (PhoshLockscreenManager *self)
{
  GType lockscreen_type;
  PhoshMonitor *primary_monitor;
  PhoshWayland *wl = phosh_wayland_get_default ();
  PhoshShell *shell = phosh_shell_get_default ();

  lockscreen_type = phosh_shell_get_lockscreen_type (shell);
  primary_monitor = phosh_shell_get_primary_monitor (shell);
  g_assert (PHOSH_IS_MONITOR (primary_monitor));

  /* The primary output gets the clock, keypad, ... */
  self->lockscreen = PHOSH_LOCKSCREEN (phosh_lockscreen_new (
                                         lockscreen_type,
                                         phosh_wayland_get_zwlr_layer_shell_v1 (wl),
                                         primary_monitor->wl_output,
                                         self->calls_manager));
  g_object_connect (
    self->lockscreen,
    "swapped-object-signal::lockscreen-unlock", G_CALLBACK (lockscreen_unlock_cb), self,
    "swapped-object-signal::wakeup-output", G_CALLBACK (lockscreen_wakeup_output_cb), self,
    NULL);

  gtk_widget_show (GTK_WIDGET (self->lockscreen));
  /* Old lockscreen gets remove due to `layer_surface_closed` */
}


static void
on_primary_monitor_changed (PhoshLockscreenManager *self,
                            GParamSpec *pspec,
                            PhoshShell *shell)
{
  PhoshMonitor *monitor;

  g_return_if_fail (PHOSH_IS_SHELL (shell));
  g_return_if_fail (PHOSH_IS_LOCKSCREEN_MANAGER (self));

  monitor = phosh_shell_get_primary_monitor (shell);

  if (monitor) {
    g_debug ("primary monitor changed to %s, need to move lockscreen", monitor->name);
    lock_primary_monitor (self);
    /* We don't remove a shield that might exist to avoid the screen
       content flickering in. The shield will be removed on unlock */
  } else {
    g_debug ("Primary monitor gone, doing nothing");
  }
}


static void
lockscreen_lock (PhoshLockscreenManager *self)
{
  PhoshMonitor *primary_monitor;
  PhoshShell *shell = phosh_shell_get_default ();
  PhoshMonitorManager *monitor_manager = phosh_shell_get_monitor_manager (shell);

  g_return_if_fail (!self->locked);

  /* Locking already in progress */
  if (self->locking)
    return;

  self->locking = TRUE;
  primary_monitor = phosh_shell_get_primary_monitor (shell);

  /* Listen for monitor changes */
  g_signal_connect_object (monitor_manager, "monitor-added",
                           G_CALLBACK (on_monitor_added),
                           self,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (monitor_manager, "monitor-removed",
                           G_CALLBACK (on_monitor_removed),
                           self,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (shell,
                           "notify::primary-monitor",
                           G_CALLBACK (on_primary_monitor_changed),
                           self,
                           G_CONNECT_SWAPPED);

  if (primary_monitor)
    lock_primary_monitor (self);
  else
    g_message ("No primary monitor to lock");

  /* Lock all other outputs */
  self->shields = g_ptr_array_new_with_free_func ((GDestroyNotify) (gtk_widget_destroy));
  for (int i = 0; i < phosh_monitor_manager_get_num_monitors (monitor_manager); i++) {
    PhoshMonitor *monitor = phosh_monitor_manager_get_monitor (monitor_manager, i);

    if (monitor == NULL || monitor == primary_monitor)
      continue;
    lock_monitor (self, monitor);
  }

  self->locked = TRUE;
  self->locking = FALSE;
  self->active_time = g_get_monotonic_time ();
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_LOCKED]);
}


static void
phosh_lockscreen_manager_set_property (GObject      *object,
                                       guint         property_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
  PhoshLockscreenManager *self = PHOSH_LOCKSCREEN_MANAGER (object);

  switch (property_id) {
  case PROP_LOCKED:
    phosh_lockscreen_manager_set_locked (self, g_value_get_boolean (value));
    break;
  case PROP_CALLS_MANAGER:
    self->calls_manager = g_value_dup_object (value);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}


static void
phosh_lockscreen_manager_get_property (GObject    *object,
                                       guint       property_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
  PhoshLockscreenManager *self = PHOSH_LOCKSCREEN_MANAGER (object);

  switch (property_id) {
  case PROP_LOCKED:
    g_value_set_boolean (value, self->locked);
    break;
  case PROP_CALLS_MANAGER:
    g_value_set_object (value, self->calls_manager);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}


static void
on_calls_call_added (PhoshLockscreen *self)
{
  g_return_if_fail (PHOSH_IS_LOCKSCREEN_MANAGER (self));

  g_signal_emit (self, signals[WAKEUP_OUTPUTS], 0);
}


static void
phosh_lockscreen_manager_dispose (GObject *object)
{
  PhoshLockscreenManager *self = PHOSH_LOCKSCREEN_MANAGER (object);

  g_clear_pointer (&self->shields, g_ptr_array_unref);
  g_clear_pointer (&self->lockscreen, phosh_cp_widget_destroy);
  g_clear_object (&self->calls_manager);

  G_OBJECT_CLASS (phosh_lockscreen_manager_parent_class)->dispose (object);
}


static void
phosh_lockscreen_manager_constructed (GObject *object)
{
  PhoshLockscreenManager *self = PHOSH_LOCKSCREEN_MANAGER (object);

  G_OBJECT_CLASS (phosh_lockscreen_manager_parent_class)->constructed (object);

  g_signal_connect_object (self->calls_manager,
                           "call-added",
                           G_CALLBACK (on_calls_call_added),
                           self,
                           G_CONNECT_SWAPPED);
}


static void
phosh_lockscreen_manager_class_init (PhoshLockscreenManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = phosh_lockscreen_manager_constructed;
  object_class->dispose = phosh_lockscreen_manager_dispose;

  object_class->set_property = phosh_lockscreen_manager_set_property;
  object_class->get_property = phosh_lockscreen_manager_get_property;

  props[PROP_LOCKED] =
    g_param_spec_boolean ("locked",
                          "Locked",
                          "Whether the screen is locked",
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  props[PROP_CALLS_MANAGER] =
    g_param_spec_object ("calls-manager",
                         "",
                         "",
                         PHOSH_TYPE_CALLS_MANAGER,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT_ONLY);

  g_object_class_install_properties (object_class, PROP_LAST_PROP, props);

  /**
   * PhoshLockscreenManager::wakeup-outputs
   * @self: The #PhoshLockscreenManager emitting this signal
   *
   * Emitted when the outputs should be woken up.
   */
  signals[WAKEUP_OUTPUTS] = g_signal_new (
    "wakeup-outputs",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
    NULL, G_TYPE_NONE, 0);
}


static void
phosh_lockscreen_manager_init (PhoshLockscreenManager *self)
{
}


PhoshLockscreenManager *
phosh_lockscreen_manager_new (PhoshCallsManager *calls_manager)
{
  return g_object_new (PHOSH_TYPE_LOCKSCREEN_MANAGER,
                       "calls-manager", calls_manager,
                       NULL);
}

/**
 * phosh_lockscreen_set_locked:
 * @self: The #PhoshLockscreenManager
 * @lock: %TRUE to lock %FALSE to unlock
 *
 * Lock or unlock the screen.
 */
void
phosh_lockscreen_manager_set_locked (PhoshLockscreenManager *self, gboolean lock)
{
  g_return_if_fail (PHOSH_IS_LOCKSCREEN_MANAGER (self));
  if (lock == self->locked)
    return;

  if (lock)
    lockscreen_lock (self);
  else
    lockscreen_unlock_cb (self, PHOSH_LOCKSCREEN (self->lockscreen));
}


gboolean
phosh_lockscreen_manager_get_locked (PhoshLockscreenManager *self)
{
  g_return_val_if_fail (PHOSH_IS_LOCKSCREEN_MANAGER (self), FALSE);

  return self->locked;
}

/**
 * phosh_lockscreen_manager_get_page
 * @self: The #PhoshLockscreenManager
 *
 * Returns: The currently shown #PhoshLockscreenPage in the #PhoshLockscreen
 */
PhoshLockscreenPage
phosh_lockscreen_manager_get_page (PhoshLockscreenManager *self)
{
  g_return_val_if_fail (PHOSH_IS_LOCKSCREEN_MANAGER (self), FALSE);

  return phosh_lockscreen_get_page (self->lockscreen);
}


gint64
phosh_lockscreen_manager_get_active_time (PhoshLockscreenManager *self)
{
  g_return_val_if_fail (PHOSH_IS_LOCKSCREEN_MANAGER (self), 0);

  return self->active_time;
}


gboolean
phosh_lockscreen_manager_set_page  (PhoshLockscreenManager *self,
                                    PhoshLockscreenPage     page)
{
  g_return_val_if_fail (PHOSH_IS_LOCKSCREEN_MANAGER (self), FALSE);

  if (!self->lockscreen)
    return FALSE;

  g_return_val_if_fail (PHOSH_IS_LOCKSCREEN (self->lockscreen), FALSE);

  phosh_lockscreen_set_page (self->lockscreen, page);
  return TRUE;
}


/**
 * phosh_lockscreen_manager_get_lockscreen:
 * @self: The lockscreen manager
 *
 * Gets the current [type@Lockscreen], if one exists (NULL otherwise).
 *
 * Returns:(transfer none)(nullable): The lockscreen
 */
PhoshLockscreen*
phosh_lockscreen_manager_get_lockscreen (PhoshLockscreenManager *self)
{
  g_return_val_if_fail (PHOSH_IS_LOCKSCREEN_MANAGER (self), NULL);

  if (!self->lockscreen)
    return NULL;

  g_return_val_if_fail (PHOSH_IS_LOCKSCREEN (self->lockscreen), FALSE);

  return self->lockscreen;
}
