/*
 * Copyright (C) 2020 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Author: Guido Günther <agx@sigxpcpu.org>
 */

#define G_LOG_DOMAIN "phosh-feedback-manager"

#include "phosh-config.h"

#include "feedback-manager.h"
#include "shell.h"

#include <libfeedback.h>

/**
 * PhoshFeedbackManager:
 *
 * Sends and configures user feedback
 */

#define PHOSH_FEEDBACK_ICON_FULL "preferences-system-notifications-symbolic"
#define PHOSH_FEEDBACK_ICON_QUIET "feedback-quiet-symbolic"
#define PHOSH_FEEDBACK_ICON_SILENT "notifications-disabled-symbolic"

enum {
  PROP_0,
  PROP_ICON_NAME,
  PROP_PROFILE,
  PROP_PRESENT,
  PROP_LAST_PROP
};
static GParamSpec *props[PROP_LAST_PROP];

struct _PhoshFeedbackManager {
  GObject parent;

  const char *profile;
  const char *icon_name;
  gboolean    inited;
};

G_DEFINE_TYPE (PhoshFeedbackManager, phosh_feedback_manager, G_TYPE_OBJECT);


static void
on_event_triggered (LfbEvent      *event,
                    GAsyncResult  *res,
                    LfbEvent     **cmp)
{
  g_autoptr (GError) err = NULL;

  if (!lfb_event_trigger_feedback_finish (event, res, &err)) {
    g_warning_once ("Failed to trigger feedback for '%s': %s",
                    lfb_event_get_event (event), err->message);
  }
}


static gboolean
on_button_event_triggered (const char* event)
{
  phosh_trigger_feedback (event);

  return GDK_EVENT_PROPAGATE;
}


static void
phosh_feedback_manager_get_property (GObject *object,
                                     guint property_id,
                                     GValue *value,
                                     GParamSpec *pspec)
{
  PhoshFeedbackManager *self = PHOSH_FEEDBACK_MANAGER (object);

  switch (property_id) {
  case PROP_ICON_NAME:
    g_value_set_string (value, self->icon_name);
    break;
  case PROP_PROFILE:
    g_value_set_string (value, self->profile);
    break;
  case PROP_PRESENT:
    g_value_set_boolean (value, self->inited);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}


static void
phosh_feedback_manager_update (PhoshFeedbackManager *self)
{
  const char *icon_name = self->icon_name;
  const char *profile = self->profile;

  self->profile = lfb_get_feedback_profile ();
  if (g_strcmp0 (self->profile, "quiet") == 0)
    self->icon_name = PHOSH_FEEDBACK_ICON_QUIET;
  else if (g_strcmp0 (self->profile, "silent") == 0)
    self->icon_name = PHOSH_FEEDBACK_ICON_SILENT;
  else
    self->icon_name = PHOSH_FEEDBACK_ICON_FULL;

  g_debug("Feedback profile set to: '%s', icon '%s'", self->profile,  self->icon_name);

  if (profile != self->profile)
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_PROFILE]);
  if (icon_name != self->icon_name)
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ICON_NAME]);
}


static void
on_profile_changed (PhoshFeedbackManager *self, GParamSpec *psepc, LfbGdbusFeedback *proxy)
{
  g_return_if_fail (PHOSH_IS_FEEDBACK_MANAGER (self));

  phosh_feedback_manager_update (self);
}


static void
phosh_feedback_manager_constructed (GObject *object)
{
  PhoshFeedbackManager *self = PHOSH_FEEDBACK_MANAGER (object);
  g_autoptr(GError) error = NULL;

  G_OBJECT_CLASS (phosh_feedback_manager_parent_class)->constructed (object);

  if (lfb_init (PHOSH_APP_ID, &error)) {
    g_debug ("Libfeedback inited");
    self->inited = TRUE;
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_PROFILE]);
  } else {
    g_warning ("Failed to init libfeedback: %s", error->message);
    return;
  }

  g_signal_connect_swapped (lfb_get_proxy (),
                            "notify::profile",
                            (GCallback)on_profile_changed,
                            self);
  phosh_feedback_manager_update (self);
}


static void
phosh_feedback_manager_finalize (GObject *object)
{
  PhoshFeedbackManager *self = PHOSH_FEEDBACK_MANAGER (object);

  if (self->inited) {
    g_signal_handlers_disconnect_by_data (lfb_get_proxy (), self);
    lfb_uninit ();
    self->inited = FALSE;
  }
  G_OBJECT_CLASS (phosh_feedback_manager_parent_class)->finalize (object);
}


static void
phosh_feedback_manager_class_init (PhoshFeedbackManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = phosh_feedback_manager_constructed;
  object_class->finalize = phosh_feedback_manager_finalize;

  object_class->get_property = phosh_feedback_manager_get_property;

  /**
   * PhoshFeedbackManager:icon-name:
   *
   * The feedback icon name
   */
  props[PROP_ICON_NAME] =
    g_param_spec_string ("icon-name", "", "",
                         PHOSH_FEEDBACK_ICON_FULL,
                         G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY);
  /**
   * PhoshFeedbackManager:profile:
   *
   * The feedback profile name
   */
  props[PROP_PROFILE] =
    g_param_spec_string ("profile", "", "",
                         "",
                         G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY);
  /**
   * PhoshFeedbackManager:present:
   *
   * Whether feedback manager is present
   */
  props[PROP_PRESENT] =
    g_param_spec_boolean ("present", "", "",
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY);

  g_object_class_install_properties (object_class, PROP_LAST_PROP, props);
}


static void
phosh_feedback_manager_init (PhoshFeedbackManager *self)
{
  self->icon_name = PHOSH_FEEDBACK_ICON_FULL;
}


PhoshFeedbackManager *
phosh_feedback_manager_new (void)
{
  return g_object_new (PHOSH_TYPE_FEEDBACK_MANAGER, NULL);
}


const char *
phosh_feedback_manager_get_icon_name (PhoshFeedbackManager *self)
{
  g_return_val_if_fail (PHOSH_IS_FEEDBACK_MANAGER (self), NULL);

  return self->icon_name;
}


const char *
phosh_feedback_manager_get_profile (PhoshFeedbackManager *self)
{
  g_return_val_if_fail (PHOSH_IS_FEEDBACK_MANAGER (self), NULL);

  return self->profile;
}


void
phosh_feedback_manager_set_profile (PhoshFeedbackManager *self, const char *profile)
{
  g_return_if_fail (PHOSH_IS_FEEDBACK_MANAGER (self));

  g_debug ("Setting feedback profile to %s", profile);
  lfb_set_feedback_profile (profile);
}


void
phosh_feedback_manager_toggle (PhoshFeedbackManager *self)
{
  const char *profile = "silent", *old;

  g_return_if_fail (PHOSH_IS_FEEDBACK_MANAGER (self));
  g_return_if_fail (self->inited);

  old = lfb_get_feedback_profile ();
  if (!g_strcmp0 (old, "silent"))
    profile = "full";
  else if (!g_strcmp0 (old, "full"))
    profile = "quiet";

  phosh_feedback_manager_set_profile (self, profile);
}


/**
 * phosh_trigger_feedback:
 * @name: The event's name to trigger feedback for
 *
 * Trigger feedback for the given event asynchronously
 */
void
phosh_trigger_feedback (const char *name)
{
  g_autoptr (LfbEvent) event = NULL;

  g_return_if_fail (lfb_is_initted ());
  g_return_if_fail (name);

  event = lfb_event_new (name);
  lfb_event_trigger_feedback_async (event,
                                    NULL,
                                    (GAsyncReadyCallback)on_event_triggered,
                                    NULL);
}


/**
 * phosh_connect_feedback:
 * @widget: The widget that should trigger feedback
 *
 * Installs "pressed" and "released" signal handlers
 * for haptic feedback.
 */
void
phosh_connect_feedback (GtkWidget *widget)
{
  g_return_if_fail (GTK_IS_WIDGET (widget));

  g_signal_connect_swapped (widget,
                            "button-press-event",
                            G_CALLBACK (on_button_event_triggered),
                            "button-pressed");

  g_signal_connect_swapped (widget,
                            "button-release-event",
                            G_CALLBACK (on_button_event_triggered),
                            "button-released");
}
