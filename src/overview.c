/*
 * Copyright (C) 2018 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Author: Guido Günther <agx@sigxcpu.org>
 */

#define G_LOG_DOMAIN "phosh-overview"

#define SEARCH_DEBOUNCE 350
#define DEFAULT_GTK_DEBOUNCE 150

#include "phosh-config.h"

#include "activity.h"
#include "app-grid-button.h"
#include "app-search.h"
#include "overview.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "phosh-private-client-protocol.h"
#include "phosh-wayland.h"
#include "shell.h"
#include "toplevel-manager.h"
#include "toplevel-thumbnail.h"
#include "util.h"

#include <gio/gdesktopappinfo.h>

#include <handy.h>

#define OVERVIEW_ICON_SIZE 64

/**
 * SECTION:overview
 * @short_description: The overview shows running apps and the
 * app grid to launch new applications.
 * @Title: PhoshOverview
 *
 * The #PhoshOverview shows running apps (#PhoshActivity) and
 * allows searching for apps (FIXME PhoshAppSearch).
 */

enum {
  ACTIVITY_LAUNCHED,
  ACTIVITY_RAISED,
  ACTIVITY_CLOSED,
  SELECTION_ABORTED,
  N_SIGNALS
};
static guint signals[N_SIGNALS] = { 0 };

enum {
  PROP_0,
  PROP_HAS_ACTIVITIES,
  PROP_SEARCH_ACTIVATED,
  PROP_SCROLLED,
  PROP_ACTIVITY_SWIPED,
  LAST_PROP,
};
static GParamSpec *props[LAST_PROP];


typedef struct
{
  /* Running activities */
  GtkWidget *carousel_running_activities;
  GtkWidget *page_running_activities;
  GtkWidget *page_empty_activities;
  GtkWidget *search;
  GtkWidget *search_close_revealer;
  GtkWidget *stack_running_activities;
  GtkWidget *app_search;
  PhoshActivity *activity;

  int       has_activities;
  char *search_string;
  guint debounce;
  gboolean activity_swiped;
} PhoshOverviewPrivate;


struct _PhoshOverview
{
  GtkBoxClass parent;
};

G_DEFINE_TYPE_WITH_PRIVATE (PhoshOverview, phosh_overview, GTK_TYPE_BOX)


static void
update_search_close_button (PhoshOverview  *self)
{
  PhoshOverviewPrivate *priv = phosh_overview_get_instance_private (self);
  gboolean has_search_string = priv->search_string && *priv->search_string != '\0';

  gtk_revealer_set_reveal_child (GTK_REVEALER (priv->search_close_revealer),
                                 gtk_widget_has_focus (priv->search) || has_search_string);
}


static void
phosh_overview_get_property (GObject    *object,
                             guint       property_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
  PhoshOverview *self = PHOSH_OVERVIEW (object);
  PhoshOverviewPrivate *priv = phosh_overview_get_instance_private (self);

  switch (property_id) {
  case PROP_HAS_ACTIVITIES:
    g_value_set_boolean (value, priv->has_activities);
    break;
  case PROP_SEARCH_ACTIVATED:
    g_value_set_boolean (value, phosh_overview_search_activated (self));
    break;
  case PROP_SCROLLED:
    g_value_set_boolean (value, phosh_overview_get_scrolled (self));
    break;
  case PROP_ACTIVITY_SWIPED:
    g_value_set_boolean (value, phosh_overview_get_activity_swiped (self));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}


static PhoshToplevel *
get_toplevel_from_activity (PhoshActivity *activity)
{
  PhoshToplevel *toplevel;
  g_return_val_if_fail (PHOSH_IS_ACTIVITY (activity), NULL);
  toplevel = g_object_get_data (G_OBJECT (activity), "toplevel");
  g_return_val_if_fail (PHOSH_IS_TOPLEVEL (toplevel), NULL);

  return toplevel;
}


static PhoshActivity *
find_activity_by_toplevel (PhoshOverview        *self,
                           PhoshToplevel        *needle)
{
  g_autoptr(GList) children;
  PhoshActivity *activity = NULL;
  PhoshOverviewPrivate *priv = phosh_overview_get_instance_private (self);

  children = gtk_container_get_children (GTK_CONTAINER (priv->carousel_running_activities));
  for (GList *l = children; l; l = l->next) {
    PhoshToplevel *toplevel;

    activity = PHOSH_ACTIVITY (l->data);
    toplevel = get_toplevel_from_activity (activity);
    if (toplevel == needle)
      break;
  }

  g_return_val_if_fail (activity, NULL);
  return activity;
}


static void
update_view (PhoshOverview *self)
{
  PhoshOverviewPrivate *priv = phosh_overview_get_instance_private (self);
  GtkWidget *old_view, *new_view;

  old_view = gtk_stack_get_visible_child (GTK_STACK (priv->stack_running_activities));
  if (priv->search_string && *priv->search_string != '\0')
    new_view = priv->app_search;
  else if (priv->has_activities)
    new_view = priv->page_running_activities;
  else
    new_view = priv->page_empty_activities;

  if (old_view == new_view)
    return;

  gtk_stack_set_visible_child (GTK_STACK (priv->stack_running_activities), new_view);

  if (old_view == priv->app_search || new_view == priv->app_search)
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SEARCH_ACTIVATED]);
}


static void
scroll_to_activity (PhoshOverview *self, PhoshActivity *activity)
{
  PhoshOverviewPrivate *priv = phosh_overview_get_instance_private (self);
  hdy_carousel_scroll_to (HDY_CAROUSEL (priv->carousel_running_activities), GTK_WIDGET (activity));
  gtk_widget_grab_focus (GTK_WIDGET (activity));
}

static void
on_activity_clicked (PhoshOverview *self, PhoshActivity *activity)
{
  PhoshToplevel *toplevel;
  g_return_if_fail (PHOSH_IS_OVERVIEW (self));
  g_return_if_fail (PHOSH_IS_ACTIVITY (activity));

  toplevel = get_toplevel_from_activity (activity);
  g_return_if_fail (toplevel);

  g_debug("Will raise %s (%s)",
          phosh_activity_get_app_id (activity),
          phosh_toplevel_get_title (toplevel));

  phosh_toplevel_activate (toplevel, phosh_wayland_get_wl_seat (phosh_wayland_get_default ()));
  g_signal_emit (self, signals[ACTIVITY_RAISED], 0);
}


static void
on_activity_closed (PhoshOverview *self, PhoshActivity *activity)
{
  PhoshToplevel *toplevel;

  g_return_if_fail (PHOSH_IS_OVERVIEW (self));
  g_return_if_fail (PHOSH_IS_ACTIVITY (activity));

  toplevel = g_object_get_data (G_OBJECT (activity), "toplevel");
  g_return_if_fail (PHOSH_IS_TOPLEVEL (toplevel));

  g_debug ("Will close %s (%s)",
           phosh_activity_get_app_id (activity),
           phosh_toplevel_get_title (toplevel));

  phosh_toplevel_close (toplevel);
  phosh_trigger_feedback ("window-close");
  g_signal_emit (self, signals[ACTIVITY_CLOSED], 0);
}


static void
on_activity_swiping_changed (PhoshOverview *self)
{
  PhoshOverviewPrivate *priv = phosh_overview_get_instance_private (self);
  GList *children, *l;
  gboolean activity_swiped = FALSE;

  children = gtk_container_get_children (GTK_CONTAINER (priv->carousel_running_activities));

  for (l = children; l && !activity_swiped; l = l->next)
    activity_swiped = phosh_activity_get_swiping (PHOSH_ACTIVITY (l->data));

  g_list_free (children);

  if (priv->activity_swiped == activity_swiped)
    return;

  priv->activity_swiped = activity_swiped;

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ACTIVITY_SWIPED]);
}


static void
on_toplevel_closed (PhoshToplevel *toplevel, PhoshOverview *overview)
{
  PhoshActivity *activity;
  PhoshOverviewPrivate *priv;

  g_return_if_fail (PHOSH_IS_TOPLEVEL (toplevel));
  g_return_if_fail (PHOSH_IS_OVERVIEW (overview));
  priv = phosh_overview_get_instance_private (overview);

  activity = find_activity_by_toplevel (overview, toplevel);
  g_return_if_fail (PHOSH_IS_ACTIVITY (activity));
  gtk_widget_destroy (GTK_WIDGET (activity));

  if (priv->activity == activity)
    priv->activity = NULL;

  /* If an activity is closed by a swipe we may end up in an incoherent state
   * as the destroyed widget didn't have the time to be non-swipped. */
  on_activity_swiping_changed (overview);
}


static void
on_toplevel_activated_changed (PhoshToplevel *toplevel, GParamSpec *pspec, PhoshOverview *overview)
{
  PhoshActivity *activity;
  PhoshOverviewPrivate *priv;
  g_return_if_fail (PHOSH_IS_OVERVIEW (overview));
  g_return_if_fail (PHOSH_IS_TOPLEVEL (toplevel));
  priv = phosh_overview_get_instance_private (overview);

  if (phosh_toplevel_is_activated (toplevel)) {
    activity = find_activity_by_toplevel (overview, toplevel);
    priv->activity = activity;
    scroll_to_activity (overview, activity);
  }
}


static void
on_thumbnail_ready_changed (PhoshThumbnail *thumbnail, GParamSpec *pspec, PhoshActivity *activity)
{
  g_return_if_fail (PHOSH_IS_THUMBNAIL (thumbnail));
  g_return_if_fail (PHOSH_IS_ACTIVITY (activity));

  phosh_activity_set_thumbnail (activity, thumbnail);
}


static void
request_thumbnail (PhoshActivity *activity, PhoshToplevel *toplevel)
{
  PhoshToplevelThumbnail *thumbnail;
  GtkAllocation allocation;
  int scale;
  g_return_if_fail (PHOSH_IS_ACTIVITY (activity));
  g_return_if_fail (PHOSH_IS_TOPLEVEL (toplevel));
  scale = gtk_widget_get_scale_factor (GTK_WIDGET (activity));
  phosh_activity_get_thumbnail_allocation (activity, &allocation);
  thumbnail = phosh_toplevel_thumbnail_new_from_toplevel (toplevel, allocation.width * scale, allocation.height * scale);
  g_signal_connect_object (thumbnail, "notify::ready", G_CALLBACK (on_thumbnail_ready_changed), activity, 0);
}


static void
on_activity_resized (PhoshActivity *activity, GtkAllocation *alloc, PhoshToplevel *toplevel)
{
  request_thumbnail (activity, toplevel);
}


static void
on_activity_has_focus_changed (PhoshOverview *self, GParamSpec *pspec, PhoshActivity *activity)
{
  PhoshOverviewPrivate *priv;

  g_return_if_fail (PHOSH_IS_ACTIVITY (activity));
  g_return_if_fail (PHOSH_IS_OVERVIEW (self));
  priv = phosh_overview_get_instance_private (self);

  if (gtk_widget_has_focus (GTK_WIDGET (activity)))
    hdy_carousel_scroll_to (HDY_CAROUSEL (priv->carousel_running_activities), GTK_WIDGET (activity));
}


static void
add_activity (PhoshOverview *self, PhoshToplevel *toplevel)
{
  PhoshOverviewPrivate *priv;
  GtkWidget *activity;
  const char *app_id, *title;
  int width, height;

  g_return_if_fail (PHOSH_IS_OVERVIEW (self));
  priv = phosh_overview_get_instance_private (self);

  app_id = phosh_toplevel_get_app_id (toplevel);
  title = phosh_toplevel_get_title (toplevel);

  g_debug ("Building activator for '%s' (%s)", app_id, title);
  activity = phosh_activity_new (app_id);
  phosh_shell_get_usable_area (phosh_shell_get_default (), NULL, NULL, &width, &height);
  g_object_set (activity,
                "win-width", width,
                "win-height", height,
                "maximized", phosh_toplevel_is_maximized (toplevel),
                "fullscreen", phosh_toplevel_is_fullscreen (toplevel),
                NULL);
  g_object_set_data (G_OBJECT (activity), "toplevel", toplevel);

  gtk_container_add (GTK_CONTAINER (priv->carousel_running_activities), activity);
  gtk_widget_show (activity);

  g_signal_connect_swapped (activity, "clicked", G_CALLBACK (on_activity_clicked), self);
  g_signal_connect_swapped (activity, "closed",
                            G_CALLBACK (on_activity_closed), self);
  g_signal_connect_swapped (activity, "notify::swiping", G_CALLBACK (on_activity_swiping_changed), self);

  g_signal_connect_object (toplevel, "closed", G_CALLBACK (on_toplevel_closed), self, 0);
  g_signal_connect_object (toplevel, "notify::activated", G_CALLBACK (on_toplevel_activated_changed), self, 0);
  g_object_bind_property (toplevel, "maximized", activity, "maximized", G_BINDING_DEFAULT);
  g_object_bind_property (toplevel, "fullscreen", activity, "fullscreen", G_BINDING_DEFAULT);

  g_signal_connect (activity, "resized", G_CALLBACK (on_activity_resized), toplevel);
  g_signal_connect_swapped (activity, "notify::has-focus", G_CALLBACK (on_activity_has_focus_changed), self);

  phosh_connect_feedback (activity);

  if (phosh_toplevel_is_activated (toplevel)) {
    scroll_to_activity (self, PHOSH_ACTIVITY (activity));
    priv->activity = PHOSH_ACTIVITY (activity);
  }
}


static void
get_running_activities (PhoshOverview *self)
{
  PhoshOverviewPrivate *priv;
  PhoshToplevelManager *toplevel_manager = phosh_shell_get_toplevel_manager (phosh_shell_get_default ());
  guint toplevels_num = phosh_toplevel_manager_get_num_toplevels (toplevel_manager);
  g_return_if_fail (PHOSH_IS_OVERVIEW (self));
  priv = phosh_overview_get_instance_private (self);

  priv->has_activities = !!toplevels_num;
  update_view (self);

  for (guint i = 0; i < toplevels_num; i++) {
    PhoshToplevel *toplevel = phosh_toplevel_manager_get_toplevel (toplevel_manager, i);
    add_activity (self, toplevel);
  }
}


static void
toplevel_added_cb (PhoshOverview        *self,
                   PhoshToplevel        *toplevel,
                   PhoshToplevelManager *manager)
{
  g_return_if_fail (PHOSH_IS_OVERVIEW (self));
  g_return_if_fail (PHOSH_IS_TOPLEVEL (toplevel));
  g_return_if_fail (PHOSH_IS_TOPLEVEL_MANAGER (manager));
  add_activity (self, toplevel);
}


static void
toplevel_changed_cb (PhoshOverview        *self,
                     PhoshToplevel        *toplevel,
                     PhoshToplevelManager *manager)
{
  PhoshActivity *activity;

  g_return_if_fail (PHOSH_IS_OVERVIEW (self));
  g_return_if_fail (PHOSH_IS_TOPLEVEL (toplevel));
  g_return_if_fail (PHOSH_IS_TOPLEVEL_MANAGER (manager));

  if (phosh_shell_get_state (phosh_shell_get_default ()) & PHOSH_STATE_OVERVIEW)
    return;

  activity = find_activity_by_toplevel (self, toplevel);
  g_return_if_fail (activity);

  request_thumbnail (activity, toplevel);
}


static void
num_toplevels_cb (PhoshOverview        *self,
                  GParamSpec           *pspec,
                  PhoshToplevelManager *manager)
{
  PhoshOverviewPrivate *priv;
  gboolean has_activities;

  g_return_if_fail (PHOSH_IS_OVERVIEW (self));
  g_return_if_fail (PHOSH_IS_TOPLEVEL_MANAGER (manager));
  priv = phosh_overview_get_instance_private (self);

  has_activities = !!phosh_toplevel_manager_get_num_toplevels (manager);
  if (priv->has_activities == has_activities)
    return;

  priv->has_activities = has_activities;
  update_view (self);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_HAS_ACTIVITIES]);
}


static void
phosh_overview_size_allocate (GtkWidget     *widget,
                              GtkAllocation *alloc)
{
  PhoshOverview *self = PHOSH_OVERVIEW (widget);
  PhoshOverviewPrivate *priv = phosh_overview_get_instance_private (self);
  GList *children, *l;
  int width, height;
  phosh_shell_get_usable_area (phosh_shell_get_default (), NULL, NULL, &width, &height);

  children = gtk_container_get_children (GTK_CONTAINER (priv->carousel_running_activities));

  for (l = children; l; l = l->next) {
    g_object_set (l->data,
                  "win-width", width,
                  "win-height", height,
                  NULL);
  }

  g_list_free (children);

  GTK_WIDGET_CLASS (phosh_overview_parent_class)->size_allocate (widget, alloc);
}


static void
app_launched_cb (PhoshOverview *self,
                 GAppInfo      *info,
                 GtkWidget     *widget)
{
  g_return_if_fail (PHOSH_IS_OVERVIEW (self));

  g_signal_emit (self, signals[ACTIVITY_LAUNCHED], 0);
}


static gboolean
do_search (PhoshOverview *self)
{
  PhoshOverviewPrivate *priv = phosh_overview_get_instance_private (self);

  update_view (self);
  phosh_app_search_set_text (PHOSH_APP_SEARCH (priv->app_search), priv->search_string);

  priv->debounce = 0;
  return G_SOURCE_REMOVE;
}


static void
search_changed (GtkSearchEntry *entry,
                PhoshOverview  *self)
{
  PhoshOverviewPrivate *priv = phosh_overview_get_instance_private (self);
  const char *search = gtk_entry_get_text (GTK_ENTRY (entry));

  g_clear_pointer (&priv->search_string, g_free);

  g_clear_handle_id (&priv->debounce, g_source_remove);

  if (search && *search != '\0') {
    priv->search_string = g_utf8_casefold (search, -1);

    update_search_close_button (self);

    /* GtkSearchEntry already adds 150ms of delay, but it's too little
     * so add a bit more until searching is faster and/or non-blocking */
    priv->debounce = g_timeout_add (SEARCH_DEBOUNCE, (GSourceFunc) do_search, self);
    g_source_set_name_by_id (priv->debounce, "[phosh] debounce app grid search (search-changed)");
  } else {
    /* don't add the delay when the entry got cleared */
    do_search (self);
  }
}


static void
search_preedit_changed (GtkSearchEntry *entry,
                        const char     *preedit,
                        PhoshOverview  *self)
{
  PhoshOverviewPrivate *priv = phosh_overview_get_instance_private (self);

  g_clear_pointer (&priv->search_string, g_free);

  if (preedit && *preedit != '\0') {
    priv->search_string = g_utf8_casefold (preedit, -1);

    update_search_close_button (self);
  }

  g_clear_handle_id (&priv->debounce, g_source_remove);

  priv->debounce = g_timeout_add (SEARCH_DEBOUNCE + DEFAULT_GTK_DEBOUNCE, (GSourceFunc) do_search, self);
  g_source_set_name_by_id (priv->debounce, "[phosh] debounce app grid search (preedit-changed)");
}


static void
search_activated (GtkSearchEntry *entry,
                  PhoshOverview  *self)
{
  PhoshOverviewPrivate *priv = phosh_overview_get_instance_private (self);

  if (!gtk_widget_has_focus (GTK_WIDGET (entry)))
    return;

  /* Don't activate when there isn't an active search */
  if (!priv->search_string || *priv->search_string == '\0') {
    return;
  }

  phosh_app_search_activate (PHOSH_APP_SEARCH (priv->app_search));
}


static void
search_focus_changed (PhoshOverview  *self)
{
  update_search_close_button (self);
}


static void
search_close_clicked (PhoshOverview  *self)
{
  phosh_overview_reset (self, FALSE);
}


static void
search_scrolled_changed (PhoshOverview  *self)
{
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SCROLLED]);
}


static void
page_changed_cb (PhoshOverview *self,
                 guint          index,
                 HdyCarousel   *carousel)
{
  PhoshActivity *activity;
  PhoshToplevel *toplevel;
  GList *list;
  g_return_if_fail (PHOSH_IS_OVERVIEW (self));
  g_return_if_fail (HDY_IS_CAROUSEL (carousel));

  /* don't raise on scroll in docked mode */
  if (phosh_shell_get_docked (phosh_shell_get_default ()))
    return;

  list = gtk_container_get_children (GTK_CONTAINER (carousel));
  activity = PHOSH_ACTIVITY (g_list_nth_data (list, index));
  toplevel = get_toplevel_from_activity (activity);
  phosh_toplevel_activate (toplevel, phosh_wayland_get_wl_seat (phosh_wayland_get_default ()));
  gtk_widget_grab_focus (GTK_WIDGET (activity));
}


static void
phosh_overview_constructed (GObject *object)
{
  PhoshOverview *self = PHOSH_OVERVIEW (object);
  PhoshOverviewPrivate *priv = phosh_overview_get_instance_private (self);
  PhoshToplevelManager *toplevel_manager =
      phosh_shell_get_toplevel_manager (phosh_shell_get_default ());

  G_OBJECT_CLASS (phosh_overview_parent_class)->constructed (object);

  g_signal_connect_object (toplevel_manager, "toplevel-added",
                           G_CALLBACK (toplevel_added_cb),
                           self,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (toplevel_manager, "toplevel-changed",
                           G_CALLBACK (toplevel_changed_cb),
                           self,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (toplevel_manager, "notify::num-toplevels",
                           G_CALLBACK (num_toplevels_cb),
                           self,
                           G_CONNECT_SWAPPED);

  get_running_activities (self);

  /* FIXME Do it with the carousel in the shell too */
  g_signal_connect_swapped (priv->app_search, "app-launched",
                            G_CALLBACK (app_launched_cb), self);

  g_signal_connect_swapped (priv->carousel_running_activities, "page-changed",
                            G_CALLBACK (page_changed_cb), self);
}


static void
phosh_overview_dispose (GObject *object)
{
  PhoshOverview *self = PHOSH_OVERVIEW (object);
  PhoshOverviewPrivate *priv = phosh_overview_get_instance_private (self);

  g_clear_handle_id (&priv->debounce, g_source_remove);

  G_OBJECT_CLASS (phosh_overview_parent_class)->dispose (object);
}



static void
phosh_overview_finalize (GObject *object)
{
  PhoshOverview *self = PHOSH_OVERVIEW (object);
  PhoshOverviewPrivate *priv = phosh_overview_get_instance_private (self);

  g_clear_pointer (&priv->search_string, g_free);

  G_OBJECT_CLASS (phosh_overview_parent_class)->finalize (object);
}


static void
phosh_overview_class_init (PhoshOverviewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->constructed = phosh_overview_constructed;
  object_class->finalize = phosh_overview_dispose;
  object_class->finalize = phosh_overview_finalize;
  object_class->get_property = phosh_overview_get_property;
  widget_class->size_allocate = phosh_overview_size_allocate;

  props[PROP_HAS_ACTIVITIES] =
    g_param_spec_boolean (
      "has-activities",
      "Has activities",
      "Whether the overview has running activities",
      FALSE,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  props[PROP_SEARCH_ACTIVATED] =
    g_param_spec_boolean (
      "search-activated",
      "Search activated",
      "Whether app search is activated",
      FALSE,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  props[PROP_SCROLLED] =
    g_param_spec_boolean (
      "scrolled",
      "Scrolled",
      "Whether app search is scrolled",
      FALSE,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  props[PROP_ACTIVITY_SWIPED] =
    g_param_spec_boolean (
      "activity-swiped",
      "Activity Swiped",
      "Whether an activity is being swiped",
      FALSE,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, LAST_PROP, props);

  /* ensure used custom types */
  gtk_widget_class_set_template_from_resource (widget_class,
                                               "/sm/puri/phosh/ui/overview.ui");

  gtk_widget_class_bind_template_child_private (widget_class, PhoshOverview, carousel_running_activities);
  gtk_widget_class_bind_template_child_private (widget_class, PhoshOverview, page_running_activities);
  gtk_widget_class_bind_template_child_private (widget_class, PhoshOverview, page_empty_activities);
  gtk_widget_class_bind_template_child_private (widget_class, PhoshOverview, stack_running_activities);
  gtk_widget_class_bind_template_child_private (widget_class, PhoshOverview, app_search);
  gtk_widget_class_bind_template_child_private (widget_class, PhoshOverview, search);
  gtk_widget_class_bind_template_child_private (widget_class, PhoshOverview, search_close_revealer);

  gtk_widget_class_bind_template_callback (widget_class, search_changed);
  gtk_widget_class_bind_template_callback (widget_class, search_preedit_changed);
  gtk_widget_class_bind_template_callback (widget_class, search_activated);
  gtk_widget_class_bind_template_callback (widget_class, search_focus_changed);
  gtk_widget_class_bind_template_callback (widget_class, search_close_clicked);
  gtk_widget_class_bind_template_callback (widget_class, search_scrolled_changed);

  signals[ACTIVITY_LAUNCHED] = g_signal_new ("activity-launched",
      G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
      NULL, G_TYPE_NONE, 0);
  signals[ACTIVITY_RAISED] = g_signal_new ("activity-raised",
      G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
      NULL, G_TYPE_NONE, 0);
  signals[SELECTION_ABORTED] = g_signal_new ("selection-aborted",
      G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
      NULL, G_TYPE_NONE, 0);
  signals[ACTIVITY_CLOSED] = g_signal_new ("activity-closed",
      G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
      NULL, G_TYPE_NONE, 0);

  gtk_widget_class_set_css_name (widget_class, "phosh-overview");
}


static void
phosh_overview_init (PhoshOverview *self)
{
  g_type_ensure (PHOSH_TYPE_APP_SEARCH);

  gtk_widget_init_template (GTK_WIDGET (self));
}


GtkWidget *
phosh_overview_new (void)
{
  return g_object_new (PHOSH_TYPE_OVERVIEW, NULL);
}


void
phosh_overview_reset (PhoshOverview *self,
                      gboolean       reset_thumbnails)
{
  PhoshOverviewPrivate *priv;
  g_return_if_fail(PHOSH_IS_OVERVIEW (self));
  priv = phosh_overview_get_instance_private (self);
  gtk_entry_set_text (GTK_ENTRY (priv->search), "");
  phosh_app_search_reset (PHOSH_APP_SEARCH (priv->app_search));

  if (priv->activity) {
    gtk_widget_grab_focus (GTK_WIDGET (priv->activity));
    if (reset_thumbnails)
      request_thumbnail (priv->activity, get_toplevel_from_activity (priv->activity));
  } else {
    /* Needed to ensure we unfocus the search entry. */
    gtk_widget_grab_focus (GTK_WIDGET (self));
  }

  update_search_close_button (self);
}

void
phosh_overview_focus_app_search (PhoshOverview *self)
{
  PhoshOverviewPrivate *priv;

  g_return_if_fail(PHOSH_IS_OVERVIEW (self));
  priv = phosh_overview_get_instance_private (self);
  gtk_widget_grab_focus (priv->search);
}


gboolean
phosh_overview_handle_search (PhoshOverview *self, GdkEvent *event)
{
  PhoshOverviewPrivate *priv;
  gboolean ret;

  g_return_val_if_fail(PHOSH_IS_OVERVIEW (self), GDK_EVENT_PROPAGATE);
  priv = phosh_overview_get_instance_private (self);
  ret = gtk_search_entry_handle_event (GTK_SEARCH_ENTRY (priv->search), event);
  if (ret == GDK_EVENT_STOP)
    gtk_entry_grab_focus_without_selecting (GTK_ENTRY (priv->search));

  return ret;
}


gboolean
phosh_overview_has_running_activities (PhoshOverview *self)
{
  PhoshOverviewPrivate *priv;

  g_return_val_if_fail (PHOSH_IS_OVERVIEW (self), FALSE);
  priv = phosh_overview_get_instance_private (self);

  return priv->has_activities;
}


gboolean
phosh_overview_search_activated (PhoshOverview *self)
{
  PhoshOverviewPrivate *priv;

  g_return_val_if_fail (PHOSH_IS_OVERVIEW (self), FALSE);
  priv = phosh_overview_get_instance_private (self);

  return gtk_stack_get_visible_child (GTK_STACK (priv->stack_running_activities)) == priv->app_search;
}


gboolean
phosh_overview_get_scrolled (PhoshOverview *self)
{
  PhoshOverviewPrivate *priv;

  g_return_val_if_fail (PHOSH_IS_OVERVIEW (self), FALSE);
  priv = phosh_overview_get_instance_private (self);

  return phosh_app_search_get_scrolled (PHOSH_APP_SEARCH (priv->app_search));
}


gboolean
phosh_overview_get_activity_swiped (PhoshOverview *self)
{
  PhoshOverviewPrivate *priv;

  g_return_val_if_fail (PHOSH_IS_OVERVIEW (self), FALSE);
  priv = phosh_overview_get_instance_private (self);

  return priv->activity_swiped;
}
