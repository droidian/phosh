/*
 * Copyright (C) 2018-2022 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Author: Guido Günther <agx@sigxcpu.org>
 *
 * Somewhat based on maynard's panel which is
 * Copyright (C) 2014 Collabora Ltd. *
 * Author: Jonny Lamb <jonny.lamb@collabora.co.uk>
 */

#define G_LOG_DOMAIN "phosh-top-panel"

#include "phosh-config.h"

#include "layout-manager.h"
#include "shell.h"
#include "session-manager.h"
#include "settings.h"
#include "top-panel.h"
#include "arrow.h"
#include "util.h"
#include "wall-clock.h"

#define GNOME_DESKTOP_USE_UNSTABLE_API
#include <libgnome-desktop/gnome-xkb-info.h>

#include <glib/gi18n.h>

#define KEYBINDINGS_SCHEMA_ID "org.gnome.shell.keybindings"
#define KEYBINDING_KEY_TOGGLE_MESSAGE_TRAY "toggle-message-tray"

#define PHOSH_TOP_PANEL_DRAG_THRESHOLD 0.3

/**
 * PhoshTopPanel:
 *
 * The top panel
 *
 * The top panel containing the top-bar (clock and status indicators) and, when activated
 * unfolds the #PhoshSettings.
 */
enum {
  PROP_0,
  PROP_TOP_PANEL_STATE,
  PROP_ON_LOCKSCREEN,
  PROP_LAST_PROP,
};
static GParamSpec *props[PROP_LAST_PROP];

enum {
  ACTIVATED,
  N_SIGNALS
};
static guint signals[N_SIGNALS] = { 0 };

typedef struct _PhoshTopPanel {
  PhoshDragSurface parent;

  PhoshTopPanelState state;
  gboolean           on_lockscreen;

  GtkWidget *box;            /* main content box */

  /* Top row above settings */
  GtkWidget *btn_power;
  GtkWidget *menu_system;
  GtkWidget *box_clock;
  GtkWidget *btn_lock;
  GtkWidget *lbl_clock2;
  GtkWidget *lbl_date;

  GtkWidget *stack;
  GtkWidget *arrow;

  GtkWidget *box_top_bar;
  GtkWidget *lbl_clock;      /* top-bar clock */
  GtkWidget *lbl_lang;

  GtkWidget *settings;       /* settings menu */
  GtkWidget *batteryinfo;

  GnomeXkbInfo *xkbinfo;
  GSettings *input_settings;
  GSettings *interface_settings;
  GdkSeat *seat;

  GSimpleActionGroup *actions;

  /* Keybinding */
  GStrv           action_names;
  GSettings      *kb_settings;

  GtkGesture     *click_gesture; /* needed so that the gesture isn't destroyed immediately */
} PhoshTopPanel;

G_DEFINE_TYPE (PhoshTopPanel, phosh_top_panel, PHOSH_TYPE_DRAG_SURFACE)

static void
phosh_top_panel_set_property (GObject      *object,
                              guint         property_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
  PhoshTopPanel *self = PHOSH_TOP_PANEL (object);

  switch (property_id) {
  case PROP_ON_LOCKSCREEN:
    self->on_lockscreen = g_value_get_boolean (value);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}


static void
phosh_top_panel_get_property (GObject    *object,
                              guint       property_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
  PhoshTopPanel *self = PHOSH_TOP_PANEL (object);

  switch (property_id) {
  case PROP_TOP_PANEL_STATE:
    g_value_set_enum (value, self->state);
    break;
  case PROP_ON_LOCKSCREEN:
    g_value_set_boolean (value, self->on_lockscreen);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}


static void
update_drag_handle (PhoshTopPanel *self, gboolean commit)
{
  int handle, offset;

  /* By default only the bottom bar handle is draggable */
  handle = phosh_layer_surface_get_configured_height (PHOSH_LAYER_SURFACE (self));
  handle -= PHOSH_TOP_BAR_HEIGHT;

  /* Settings might enlarge the draggable area */
  offset = phosh_settings_get_drag_handle_offset (PHOSH_SETTINGS (self->settings));
  handle -= offset;

  g_debug ("Drag Handle: %d", handle);
  if (handle < 0)
    return;

  phosh_drag_surface_set_drag_mode (PHOSH_DRAG_SURFACE (self),
                                    PHOSH_DRAG_SURFACE_DRAG_MODE_HANDLE);
  phosh_drag_surface_set_drag_handle (PHOSH_DRAG_SURFACE (self), handle);

  if (commit)
    phosh_layer_surface_wl_surface_commit (PHOSH_LAYER_SURFACE (self));
}


static void
on_settings_drag_handle_offset_changed (PhoshTopPanel *self, GParamSpec   *pspec)
{
  update_drag_handle (self, TRUE);
}


static void
on_shutdown_action (GSimpleAction *action,
                    GVariant      *parameter,
                    gpointer       data)
{
  PhoshTopPanel *self = PHOSH_TOP_PANEL(data);
  PhoshSessionManager *sm = phosh_shell_get_session_manager (phosh_shell_get_default ());

  g_return_if_fail (PHOSH_IS_TOP_PANEL (self));
  phosh_session_manager_shutdown (sm);
  phosh_top_panel_fold (self);
}


static void
on_restart_action (GSimpleAction *action,
                   GVariant      *parameter,
                   gpointer       data)
{
  PhoshTopPanel *self = PHOSH_TOP_PANEL(data);
  PhoshSessionManager *sm = phosh_shell_get_session_manager (phosh_shell_get_default ());

  g_return_if_fail (PHOSH_IS_TOP_PANEL (self));
  g_return_if_fail (PHOSH_IS_SESSION_MANAGER (sm));

  phosh_session_manager_reboot (sm);
  phosh_top_panel_fold (self);
}


static void
on_suspend_action (GSimpleAction *action,
                   GVariant      *parameter,
                   gpointer       data)
{
  PhoshTopPanel *self = PHOSH_TOP_PANEL (data);
  g_return_if_fail (PHOSH_IS_TOP_PANEL (self));
  g_action_group_activate_action (G_ACTION_GROUP (phosh_shell_get_default ()),
                                  "suspend.trigger-suspend", NULL);
  phosh_top_panel_fold (self);
}


static void
on_lockscreen_action (GSimpleAction *action,
                      GVariant      *parameter,
                      gpointer      data)
{
  PhoshTopPanel *self = PHOSH_TOP_PANEL(data);

  g_return_if_fail (PHOSH_IS_TOP_PANEL (self));
  phosh_shell_lock (phosh_shell_get_default ());
  phosh_top_panel_fold (self);
}


static void
on_logout_action (GSimpleAction *action,
                  GVariant      *parameter,
                  gpointer      data)
{
  PhoshTopPanel *self = PHOSH_TOP_PANEL(data);
  PhoshSessionManager *sm = phosh_shell_get_session_manager (phosh_shell_get_default ());

  g_return_if_fail (PHOSH_IS_TOP_PANEL (self));
  g_return_if_fail (PHOSH_IS_SESSION_MANAGER (sm));
  phosh_session_manager_logout (sm);
  phosh_top_panel_fold (self);
}


static void
wall_clock_notify_cb (PhoshTopPanel  *self,
                      GParamSpec     *pspec,
                      PhoshWallClock *wall_clock)
{
  const char *str;
  g_autofree char *date = NULL;

  g_return_if_fail (PHOSH_IS_TOP_PANEL (self));
  g_return_if_fail (PHOSH_IS_WALL_CLOCK (wall_clock));

  str = phosh_wall_clock_get_clock (wall_clock, TRUE);
  gtk_label_set_text (GTK_LABEL (self->lbl_clock2), str);

  date = phosh_wall_clock_local_date (wall_clock);
  gtk_label_set_label (GTK_LABEL (self->lbl_date), date);
}


static gboolean
needs_keyboard_label (PhoshTopPanel *self)
{
  GList *slaves;
  g_autoptr(GVariant) sources = NULL;

  g_return_val_if_fail (GDK_IS_SEAT (self->seat), FALSE);
  g_return_val_if_fail (G_IS_SETTINGS (self->input_settings), FALSE);

  if (!phosh_shell_get_docked (phosh_shell_get_default ())) {
      return FALSE;
  }

  sources = g_settings_get_value(self->input_settings, "sources");
  if (g_variant_n_children (sources) < 2)
    return FALSE;

  slaves = gdk_seat_get_slaves (self->seat, GDK_SEAT_CAPABILITY_KEYBOARD);
  if (!slaves)
    return FALSE;

  g_list_free (slaves);
  return TRUE;
}


static gboolean
transform_docked_mode_to_lang_label_visible (GBinding     *binding,
                                             const GValue *from_value,
                                             GValue       *to_value,
                                             gpointer      data)
{
  gboolean visible = FALSE;
  gboolean docked = g_value_get_boolean (from_value);
  PhoshTopPanel *self = PHOSH_TOP_PANEL (data);

  if (docked && needs_keyboard_label (self))
    visible = TRUE;

  g_value_set_boolean (to_value, visible);
  return TRUE;
}


static void
on_seat_device_changed (PhoshTopPanel *self, GdkDevice  *device, GdkSeat *seat)
{
  gboolean visible;

  g_return_if_fail (PHOSH_IS_TOP_PANEL (self));
  g_return_if_fail (GDK_IS_SEAT (seat));

  visible = needs_keyboard_label (self);
  gtk_widget_set_visible (self->lbl_lang, visible);
}


static void
on_input_setting_changed (PhoshTopPanel *self,
                          const char    *key,
                          GSettings     *settings)
{
  g_autoptr(GVariant) sources = NULL;
  gboolean visible;
  GVariantIter iter;
  g_autofree char *id = NULL;
  g_autofree char *type = NULL;
  const char *name;

  visible = needs_keyboard_label (self);

  sources = g_settings_get_value(settings, "sources");
  g_variant_iter_init (&iter, sources);
  g_variant_iter_next (&iter, "(ss)", &type, &id);

  if (g_strcmp0 (type, "xkb")) {
    g_debug ("Not a xkb layout: '%s' - ignoring", id);
    goto out;
  }

  if (!gnome_xkb_info_get_layout_info (self->xkbinfo, id,
                                       NULL, &name, NULL, NULL)) {
    g_debug ("Failed to get layout info for %s", id);
    name = id;
  }
  g_debug ("Layout is %s", name);
  gtk_label_set_text (GTK_LABEL (self->lbl_lang), name);
 out:
  gtk_widget_set_visible (self->lbl_lang, visible);
}


static gboolean
on_key_press_event (PhoshTopPanel *self, GdkEventKey *event, gpointer data)
{
  gboolean handled = FALSE;

  g_return_val_if_fail (PHOSH_IS_TOP_PANEL (self), FALSE);

  if (!self->settings)
    return handled;

  switch (event->keyval) {
    case GDK_KEY_Escape:
      phosh_top_panel_fold (self);
      handled = TRUE;
      break;
    default:
      /* nothing to do */
      break;
    }
  return handled;
}


static void
released_cb (PhoshTopPanel *self, int n_press, double x, double y, GtkGestureMultiPress *gesture)
{
  /*
   * The popover has to be popdown manually as it doesn't happen
   * automatically when the power button is tapped with touch
   */
  if (gtk_widget_is_visible (self->menu_system)) {
    gtk_popover_popdown (GTK_POPOVER (self->menu_system));
    return;
  }

  if (phosh_util_gesture_is_touch (GTK_GESTURE_SINGLE (gesture)) == FALSE)
    g_signal_emit (self, signals[ACTIVATED], 0);
}


static void
toggle_message_tray_action (GSimpleAction *action, GVariant *param, gpointer data)
{
  PhoshTopPanel *self = PHOSH_TOP_PANEL (data);

  g_return_if_fail (PHOSH_IS_TOP_PANEL (self));

  phosh_top_panel_toggle_fold (self);
  /* TODO: focus message tray */
}


static void
add_keybindings (PhoshTopPanel *self)
{
  g_auto (GStrv) keybindings = NULL;
  g_autoptr (GArray) actions = g_array_new (FALSE, TRUE, sizeof (GActionEntry));

  keybindings = g_settings_get_strv (self->kb_settings, KEYBINDING_KEY_TOGGLE_MESSAGE_TRAY);
  for (int i = 0; i < g_strv_length (keybindings); i++) {
    GActionEntry entry = { .name = keybindings[i], .activate = toggle_message_tray_action };
    g_array_append_val (actions, entry);
  }
  phosh_shell_add_global_keyboard_action_entries (phosh_shell_get_default (),
                                                  (GActionEntry*) actions->data,
                                                  actions->len,
                                                  self);
  self->action_names = g_steal_pointer (&keybindings);
}


static void
on_keybindings_changed (PhoshTopPanel *self,
                        gchar         *key,
                        GSettings     *settings)
{
  /* For now just redo all keybindings */
  g_debug ("Updating keybindings");
  phosh_shell_remove_global_keyboard_action_entries (phosh_shell_get_default (),
                                                     self->action_names);
  g_clear_pointer (&self->action_names, g_strfreev);
  add_keybindings (self);
}


static void
phosh_top_panel_dragged (PhoshDragSurface *drag_surface, int margin)
{
  PhoshTopPanel *self = PHOSH_TOP_PANEL (drag_surface);
  int width, height;
  gtk_window_get_size (GTK_WINDOW (self), &width, &height);
  phosh_arrow_set_progress (PHOSH_ARROW (self->arrow), -margin / (double)(height - PHOSH_TOP_BAR_HEIGHT));
  g_debug ("Margin: %d", margin);
}


static void
on_drag_state_changed (PhoshTopPanel *self)
{
  PhoshTopPanelState state = self->state;
  const char *visible;
  gboolean kbd_interactivity = FALSE;
  double arrow = -1.0;

  /* Close the popover on any drag */
  gtk_widget_hide (self->menu_system);

  switch (phosh_drag_surface_get_drag_state (PHOSH_DRAG_SURFACE (self))) {
  case PHOSH_DRAG_SURFACE_STATE_UNFOLDED:
    state = PHOSH_TOP_PANEL_STATE_UNFOLDED;
    update_drag_handle (self, TRUE);
    kbd_interactivity = TRUE;
    visible = "arrow";
    arrow = 0.0;
    break;
  case PHOSH_DRAG_SURFACE_STATE_FOLDED:
    state = PHOSH_TOP_PANEL_STATE_FOLDED;
    visible = "top-bar";
    arrow = 1.0;
    break;
  case PHOSH_DRAG_SURFACE_STATE_DRAGGED:
    visible = "arrow";
    arrow = phosh_arrow_get_progress (PHOSH_ARROW (self->arrow));
    break;
  default:
    g_return_if_reached ();
  }

  g_debug ("%s: state: %d, visible: %s", __func__, self->state, visible);
  gtk_stack_set_visible_child_name (GTK_STACK (self->stack), visible);
  phosh_arrow_set_progress (PHOSH_ARROW (self->arrow), arrow);

  if (self->state != state) {
    self->state = state;
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_TOP_PANEL_STATE]);
  }

  phosh_layer_surface_set_kbd_interactivity (PHOSH_LAYER_SURFACE (self), kbd_interactivity);
  phosh_layer_surface_wl_surface_commit (PHOSH_LAYER_SURFACE (self));
}


static GActionEntry entries[] = {
  { .name = "poweroff", .activate = on_shutdown_action },
  { .name = "restart", .activate = on_restart_action },
  { .name = "suspend", .activate = on_suspend_action },
  { .name = "lockscreen", .activate = on_lockscreen_action },
  { .name = "logout", .activate = on_logout_action },
};


static void
phosh_top_panel_constructed (GObject *object)
{
  PhoshTopPanel *self = PHOSH_TOP_PANEL (object);
  GdkDisplay *display = gdk_display_get_default ();
  PhoshWallClock *wall_clock = phosh_wall_clock_get_default ();

  g_autoptr (GSettings) phosh_settings = g_settings_new ("sm.puri.phosh");

  G_OBJECT_CLASS (phosh_top_panel_parent_class)->constructed (object);

  g_object_bind_property (phosh_shell_get_default (), "locked",
                          self, "on-lockscreen",
                          G_BINDING_SYNC_CREATE);

  g_object_bind_property (wall_clock, "date-time",
                          self->lbl_clock, "label",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (self, "on-lockscreen",
                          self->lbl_clock,
                          "visible",
                          G_BINDING_SYNC_CREATE | G_BINDING_INVERT_BOOLEAN);
  g_signal_connect_object (wall_clock,
                           "notify::time",
                           G_CALLBACK (wall_clock_notify_cb),
                           self,
                           G_CONNECT_SWAPPED);

  wall_clock_notify_cb (self, NULL, wall_clock);

  gtk_window_set_title (GTK_WINDOW (self), "phosh panel");

  /* language indicator */
  if (display) {
    self->input_settings = g_settings_new ("org.gnome.desktop.input-sources");
    self->xkbinfo = gnome_xkb_info_new ();
    self->seat = gdk_display_get_default_seat (display);
    g_object_connect (self->seat,
                      "swapped_signal::device-added", G_CALLBACK (on_seat_device_changed), self,
                      "swapped_signal::device-removed", G_CALLBACK (on_seat_device_changed), self,
                      NULL);
    g_signal_connect_swapped (self->input_settings,
                              "changed::sources", G_CALLBACK (on_input_setting_changed),
                              self);
    on_input_setting_changed (self, NULL, self->input_settings);

    g_object_bind_property_full (phosh_shell_get_default (),
                                 "docked",
                                 self->lbl_lang,
                                 "visible",
                                 G_BINDING_SYNC_CREATE,
                                 transform_docked_mode_to_lang_label_visible,
                                 NULL, self, NULL);
  }

  /* Settings menu and it's top-panel / menu */
  gtk_widget_add_events (GTK_WIDGET (self), GDK_ALL_EVENTS_MASK);
  g_signal_connect (G_OBJECT (self),
                    "key-press-event",
                    G_CALLBACK (on_key_press_event),
                    NULL);

  self->actions = g_simple_action_group_new ();
  gtk_widget_insert_action_group (GTK_WIDGET (self), "panel",
                                  G_ACTION_GROUP (self->actions));
  g_action_map_add_action_entries (G_ACTION_MAP (self->actions),
                                   entries, G_N_ELEMENTS (entries),
                                   self);
  if (!phosh_shell_started_by_display_manager (phosh_shell_get_default ())) {
    GAction *action = g_action_map_lookup_action (G_ACTION_MAP (self->actions),
                                                  "logout");
    g_simple_action_set_enabled (G_SIMPLE_ACTION(action), FALSE);
  }

  g_settings_bind (phosh_settings,
                   "enable-suspend",
                   g_action_map_lookup_action(G_ACTION_MAP (self->actions), "suspend"),
                   "enabled",
                   G_SETTINGS_BIND_GET);


  self->interface_settings = g_settings_new ("org.gnome.desktop.interface");
  g_settings_bind (self->interface_settings,
                   "show-battery-percentage",
                   self->batteryinfo,
                   "show-detail",
                   G_SETTINGS_BIND_GET);

  g_signal_connect_swapped (self->kb_settings,
                            "changed::" KEYBINDING_KEY_TOGGLE_MESSAGE_TRAY,
                            G_CALLBACK (on_keybindings_changed),
                            self);
  add_keybindings (self);

  g_signal_connect (self, "notify::drag-state", G_CALLBACK (on_drag_state_changed), NULL);
}


static void
phosh_top_panel_dispose (GObject *object)
{
  PhoshTopPanel *self = PHOSH_TOP_PANEL (object);

  g_clear_object (&self->kb_settings);
  g_clear_object (&self->xkbinfo);
  g_clear_object (&self->input_settings);
  g_clear_object (&self->interface_settings);
  g_clear_object (&self->actions);
  g_clear_pointer (&self->action_names, g_strfreev);
  if (self->seat) {
    /* language indicator */
    g_signal_handlers_disconnect_by_data (self->seat, self);
    self->seat = NULL;
  }

  G_OBJECT_CLASS (phosh_top_panel_parent_class)->dispose (object);
}


static int
get_margin (gint height)
{
  return (-1 * height) + PHOSH_TOP_BAR_HEIGHT;
}


static gboolean
on_configure_event (PhoshTopPanel *self, GdkEventConfigure *event)
{
  guint margin;

  margin = get_margin (event->height);

  /* ignore popovers like the power menu */
  if (gtk_widget_get_window (GTK_WIDGET (self)) != event->window)
    return FALSE;

  g_debug ("%s: %dx%d margin: %d", __func__, event->height, event->width, margin);

  /* If the size changes we need to update the folded margin */
  phosh_drag_surface_set_margin (PHOSH_DRAG_SURFACE (self), margin, 0);
  /* Update drag handle since top-panel size might have changed */
  update_drag_handle (self, FALSE);
  /* Trigger redraw and surface commit */
  gtk_widget_queue_draw (GTK_WIDGET (self));

  return FALSE;
}


static void
phosh_top_panel_configured (PhoshLayerSurface *layer_surface)
{
  guint width, height;

  width = phosh_layer_surface_get_configured_width  (layer_surface);
  height = phosh_layer_surface_get_configured_height (layer_surface);

  g_debug ("%s: %dx%d", __func__, width, height);

  PHOSH_LAYER_SURFACE_CLASS (phosh_top_panel_parent_class)->configured (layer_surface);
}


static void
phosh_top_panel_class_init (PhoshTopPanelClass *klass)
{
  GObjectClass *object_class = (GObjectClass *)klass;
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  PhoshLayerSurfaceClass *layer_surface_class = PHOSH_LAYER_SURFACE_CLASS (klass);
  PhoshDragSurfaceClass *drag_surface_class = PHOSH_DRAG_SURFACE_CLASS (klass);

  object_class->constructed = phosh_top_panel_constructed;
  object_class->dispose = phosh_top_panel_dispose;
  object_class->set_property = phosh_top_panel_set_property;
  object_class->get_property = phosh_top_panel_get_property;

  layer_surface_class->configured = phosh_top_panel_configured;

  drag_surface_class->dragged = phosh_top_panel_dragged;

  gtk_widget_class_set_css_name (widget_class, "phosh-top-panel");

  /**
   * PhoshTopPanel:state:
   *
   * Whether the top-panel is currently `folded` (only top-bar is
   * visible) or `unfolded` (settings and notification area are
   * visible). The property is updated when the widget reaches its
   * target state.
   */
  props[PROP_TOP_PANEL_STATE] =
    g_param_spec_enum ("state", "", "",
                       PHOSH_TYPE_TOP_PANEL_STATE,
                       PHOSH_TOP_PANEL_STATE_UNFOLDED,
                       G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  /**
   * PhoshTopPanel:on-lockscreen:
   *
   * Whether top-panel is shown on lockscreen (%TRUE) or in the unlocked shell
   * (%FALSE).
   *
   * Consider this property to be read only. It's only r/w so we can
   * use a property binding with the [type@Shell]s "locked" property.
   */
  props[PROP_ON_LOCKSCREEN] =
    g_param_spec_boolean ("on-lockscreen", "", "",
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, PROP_LAST_PROP, props);

  signals[ACTIVATED] = g_signal_new ("activated",
      G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
      NULL, G_TYPE_NONE, 0);

  g_type_ensure (PHOSH_TYPE_ARROW);

  gtk_widget_class_set_template_from_resource (widget_class,
                                               "/sm/puri/phosh/ui/top-panel.ui");
  gtk_widget_class_bind_template_child (widget_class, PhoshTopPanel, arrow);
  gtk_widget_class_bind_template_child (widget_class, PhoshTopPanel, batteryinfo);
  gtk_widget_class_bind_template_child (widget_class, PhoshTopPanel, box);
  gtk_widget_class_bind_template_child (widget_class, PhoshTopPanel, box_clock);
  gtk_widget_class_bind_template_child (widget_class, PhoshTopPanel, box_top_bar);
  gtk_widget_class_bind_template_child (widget_class, PhoshTopPanel, btn_lock);
  gtk_widget_class_bind_template_child (widget_class, PhoshTopPanel, btn_power);
  gtk_widget_class_bind_template_child (widget_class, PhoshTopPanel, click_gesture);
  gtk_widget_class_bind_template_child (widget_class, PhoshTopPanel, lbl_clock);
  gtk_widget_class_bind_template_child (widget_class, PhoshTopPanel, lbl_clock2);
  gtk_widget_class_bind_template_child (widget_class, PhoshTopPanel, lbl_date);
  gtk_widget_class_bind_template_child (widget_class, PhoshTopPanel, lbl_lang);
  gtk_widget_class_bind_template_child (widget_class, PhoshTopPanel, menu_system);
  gtk_widget_class_bind_template_child (widget_class, PhoshTopPanel, settings);
  gtk_widget_class_bind_template_child (widget_class, PhoshTopPanel, stack);
  gtk_widget_class_bind_template_callback (widget_class, on_settings_drag_handle_offset_changed);
  gtk_widget_class_bind_template_callback (widget_class, phosh_top_panel_fold);
  gtk_widget_class_bind_template_callback (widget_class, released_cb);
}


static void
set_clock_position (PhoshTopPanel *self, PhoshLayoutManager *layout_manager)
{
  PhoshLayoutClockPosition pos;
  guint top_margin = 0;

  pos = phosh_layout_manager_get_clock_pos (layout_manager);

  /* Top-bar clock */
  gtk_container_remove (GTK_CONTAINER (self->box_top_bar), self->lbl_clock);
  phosh_util_toggle_style_class (self->lbl_clock, "left", FALSE);
  phosh_util_toggle_style_class (self->lbl_clock, "right", FALSE);

  switch (pos) {
  case PHOSH_LAYOUT_CLOCK_POS_LEFT:
    gtk_box_pack_start (GTK_BOX (self->box_top_bar), self->lbl_clock, FALSE, FALSE, 0);
    gtk_box_reorder_child (GTK_BOX (self->box_top_bar), self->lbl_clock, 0);
    phosh_util_toggle_style_class (self->lbl_clock, "left", TRUE);
    top_margin = phosh_layout_manager_get_clock_shift (layout_manager);
    break;
  case PHOSH_LAYOUT_CLOCK_POS_RIGHT:
    gtk_box_pack_end (GTK_BOX (self->box_top_bar), self->lbl_clock, FALSE, FALSE, 0);
    gtk_box_reorder_child (GTK_BOX (self->box_top_bar), self->lbl_clock, 1);
    phosh_util_toggle_style_class (self->lbl_clock, "right", TRUE);
    top_margin = phosh_layout_manager_get_clock_shift (layout_manager);
    break;
  case PHOSH_LAYOUT_CLOCK_POS_CENTER:
    gtk_box_set_center_widget (GTK_BOX (self->box_top_bar), self->lbl_clock);
    break;
  default:
    g_assert_not_reached ();
  }

  /* Shift down settings menu clock */
  gtk_widget_set_margin_top (self->box_clock, top_margin);
}


static void
set_margin (PhoshTopPanel *self, PhoshLayoutManager *layout_manager)
{
  guint shift;

  shift = phosh_layout_manager_get_corner_shift (layout_manager);
  g_debug ("Shifting UI elements %d pixels to center ", shift);

  gtk_widget_set_margin_start (GTK_WIDGET (self->box_top_bar), shift);
  gtk_widget_set_margin_end (GTK_WIDGET (self->box_top_bar), shift);
}


static void
on_layout_changed (PhoshTopPanel *self, PhoshLayoutManager *layout_manager)
{
  g_return_if_fail (PHOSH_IS_TOP_PANEL (self));
  g_return_if_fail (PHOSH_IS_LAYOUT_MANAGER (layout_manager));

  set_margin (self, layout_manager);
  set_clock_position (self, layout_manager);
}


static void
phosh_top_panel_init (PhoshTopPanel *self)
{
  PhoshLayoutManager *layout_manager;

  gtk_widget_init_template (GTK_WIDGET (self));

  self->state = PHOSH_TOP_PANEL_STATE_UNFOLDED;
  self->kb_settings = g_settings_new (KEYBINDINGS_SCHEMA_ID);
  g_signal_connect (self, "configure-event", G_CALLBACK (on_configure_event), NULL);

  layout_manager = phosh_shell_get_layout_manager (phosh_shell_get_default ());
  g_signal_connect_object (layout_manager,
                           "layout-changed",
                           G_CALLBACK (on_layout_changed),
                           self,
                           G_CONNECT_SWAPPED);
  on_layout_changed (self, layout_manager);
}


GtkWidget *
phosh_top_panel_new (struct zwlr_layer_shell_v1          *layer_shell,
                     struct zphoc_layer_shell_effects_v1 *layer_shell_effects,
                     PhoshMonitor                        *monitor,
                     guint32                              layer)
{
  return g_object_new (PHOSH_TYPE_TOP_PANEL,
                       /* layer-surface */
                       "layer-shell", layer_shell,
                       "wl-output", monitor->wl_output,
                       "height", PHOSH_TOP_BAR_HEIGHT,
                       "anchor", ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                 ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                 ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
                       "layer", layer,
                       "kbd-interactivity", FALSE,
                       "namespace", "phosh top-panel",
                       /* drag-surface */
                       "layer-shell-effects", layer_shell_effects,
                       "exclusive", PHOSH_TOP_BAR_HEIGHT,
                       "threshold", PHOSH_TOP_PANEL_DRAG_THRESHOLD,
                       NULL);
}


void
phosh_top_panel_fold (PhoshTopPanel *self)
{
  g_return_if_fail (PHOSH_IS_TOP_PANEL (self));

  if (self->state == PHOSH_TOP_PANEL_STATE_FOLDED)
	return;

  phosh_drag_surface_set_drag_state (PHOSH_DRAG_SURFACE (self),
                                     PHOSH_DRAG_SURFACE_STATE_FOLDED);
}


void
phosh_top_panel_unfold (PhoshTopPanel *self)
{
  g_return_if_fail (PHOSH_IS_TOP_PANEL (self));

  if (self->state == PHOSH_TOP_PANEL_STATE_UNFOLDED)
	return;

  phosh_drag_surface_set_drag_state (PHOSH_DRAG_SURFACE (self),
                                     PHOSH_DRAG_SURFACE_STATE_UNFOLDED);
}


void
phosh_top_panel_toggle_fold (PhoshTopPanel *self)
{
  g_return_if_fail (PHOSH_IS_TOP_PANEL (self));

  if (self->state == PHOSH_TOP_PANEL_STATE_UNFOLDED) {
    phosh_top_panel_fold (self);
  } else {
    phosh_top_panel_unfold (self);
  }
}


PhoshTopPanelState
phosh_top_panel_get_state (PhoshTopPanel *self)
{
  g_return_val_if_fail (PHOSH_IS_TOP_PANEL (self), PHOSH_TOP_PANEL_STATE_FOLDED);

  return self->state;
}
