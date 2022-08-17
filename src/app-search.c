/*
 * Copyright © 2019 Zander Brown <zbrown@gnome.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "phosh-app-search"

#include "feedback-manager.h"
#include "app-carousel.h"
#include "app-search.h"
#include "app-grid-button.h"
#include "app-list-model.h"
#include "shell.h"
#include "util.h"

#include "gtk-list-models/gtksortlistmodel.h"
#include "gtk-list-models/gtkfilterlistmodel.h"

/**
 * SECTION:app-search
 * @short_description: A grid presenting allowing to search applications
 * @Title: PhoshAppSearch
 *
 * Presents the available applications in a grid, with the ability to
 * search.
 */

enum {
  PROP_0,
  PROP_TEXT,
  PROP_SCROLLED,
  PROP_LAST_PROP
};
static GParamSpec *props[PROP_LAST_PROP];

enum {
  APP_LAUNCHED,
  N_SIGNALS
};
static guint signals[N_SIGNALS] = { 0 };

typedef struct _PhoshAppSearchPrivate PhoshAppSearchPrivate;
struct _PhoshAppSearchPrivate {
  GtkFilterListModel *model;

  GtkWidget *apps;
  GtkWidget *scrolled_window;

  char *search_string;
  GSettings *settings;
  GStrv force_adaptive;
  gboolean scrolled;
};

G_DEFINE_TYPE_WITH_PRIVATE (PhoshAppSearch, phosh_app_search, GTK_TYPE_BOX)

static void
phosh_app_search_set_property (GObject      *object,
                             guint         property_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
  PhoshAppSearch *self = PHOSH_APP_SEARCH (object);

  switch (property_id) {
  case PROP_TEXT:
    phosh_app_search_set_text (self, g_value_get_string (value));
    break;
  case PROP_SCROLLED:
    g_assert_not_reached ();
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}


static void
phosh_app_search_get_property (GObject    *object,
                             guint       property_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
  PhoshAppSearch *self = PHOSH_APP_SEARCH (object);
  PhoshAppSearchPrivate *priv = phosh_app_search_get_instance_private (self);

  switch (property_id) {
  case PROP_TEXT:
    g_value_set_string (value, priv->search_string);
    break;
  case PROP_SCROLLED:
    g_value_set_boolean (value, priv->scrolled);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}


static void
app_launched_cb (GtkWidget    *widget,
                 GAppInfo     *info,
                 PhoshAppSearch *self)
{
  phosh_trigger_feedback ("button-pressed");
  g_signal_emit (self, signals[APP_LAUNCHED], 0, info);
}


static int
sort_apps (gconstpointer a,
           gconstpointer b,
           gpointer      data)
{
  const char *empty = "";
  GAppInfo *info1 = G_APP_INFO (a);
  GAppInfo *info2 = G_APP_INFO (b);

  g_autofree char *s1 = g_utf8_casefold (g_app_info_get_name (info1), -1);
  g_autofree char *s2 = g_utf8_casefold (g_app_info_get_name (info2), -1);

  return g_utf8_collate (s1 ?: empty, s2 ?: empty);
}


static void
on_filter_setting_changed (PhoshAppSearch *self,
                           GParamSpec   *pspec,
                           gpointer     *unused)
{
  PhoshAppSearchPrivate *priv;

  g_return_if_fail (PHOSH_IS_APP_SEARCH (self));

  priv = phosh_app_search_get_instance_private (self);

  g_strfreev (priv->force_adaptive);
  priv->force_adaptive = g_settings_get_strv (priv->settings,
                                              "force-adaptive");

  gtk_filter_list_model_refilter (priv->model);
}


static const char *(*app_attr[]) (GAppInfo *info) = {
  g_app_info_get_display_name,
  g_app_info_get_name,
  g_app_info_get_description,
  g_app_info_get_executable,
};


static const char *(*desktop_attr[]) (GDesktopAppInfo *info) = {
  g_desktop_app_info_get_generic_name,
  g_desktop_app_info_get_categories,
};


static gboolean
search_apps (gpointer item, gpointer data)
{
  PhoshAppSearch *self = data;
  PhoshAppSearchPrivate *priv = phosh_app_search_get_instance_private (self);
  GAppInfo *info = item;
  const char *search = NULL;
  const char *str = NULL;

  g_return_val_if_fail (priv != NULL, TRUE);

  search = priv->search_string;

  if (search == NULL || strlen (search) == 0)
    return TRUE;

  for (int i = 0; i < G_N_ELEMENTS (app_attr); i++) {
    g_autofree char *folded = NULL;

    str = app_attr[i] (info);

    if (!str || *str == '\0')
      continue;

    folded = g_utf8_casefold (str, -1);

    if (strstr (folded, search))
      return TRUE;
  }

  if (G_IS_DESKTOP_APP_INFO (info)) {
    const char * const *kwds;

    for (int i = 0; i < G_N_ELEMENTS (desktop_attr); i++) {
      g_autofree char *folded = NULL;

      str = desktop_attr[i] (G_DESKTOP_APP_INFO (info));

      if (!str || *str == '\0')
        continue;

      folded = g_utf8_casefold (str, -1);

      if (strstr (folded, search))
        return TRUE;
    }

    kwds = g_desktop_app_info_get_keywords (G_DESKTOP_APP_INFO (info));

    if (kwds) {
      int i = 0;

      while ((str = kwds[i])) {
        g_autofree char *folded = g_utf8_casefold (str, -1);
        if (strstr (folded, search))
          return TRUE;
        i++;
      }
    }
  }

  return FALSE;
}


static void
update_app_button_adaptive (PhoshAppSearch   *self,
                            PhoshAppGridButton *button)
{
  PhoshAppSearchPrivate *priv = phosh_app_search_get_instance_private (self);
  GAppInfo *info;
  gboolean adaptive;

  g_return_if_fail (PHOSH_IS_APP_SEARCH (self));
  g_return_if_fail (PHOSH_IS_APP_GRID_BUTTON (button));

  info = phosh_app_grid_button_get_app_info (button);
  adaptive = phosh_util_get_app_is_adaptive (info, (const char * const *) priv->force_adaptive);

  phosh_app_grid_button_set_adaptive (button, adaptive);
}


static GtkWidget *
create_launcher (gpointer item,
                 gpointer self)
{
  GtkWidget *btn = phosh_app_grid_button_new (G_APP_INFO (item));

  g_signal_connect (btn, "app-launched",
                    G_CALLBACK (app_launched_cb), self);

  gtk_widget_show (btn);
  update_app_button_adaptive (PHOSH_APP_SEARCH (self), PHOSH_APP_GRID_BUTTON (btn));

  return btn;
}


static void
vertical_adjustment_changed (PhoshAppSearch *self)
{
  PhoshAppSearchPrivate *priv = phosh_app_search_get_instance_private (self);
  GtkAdjustment *adjustment;
  gboolean scrolled;

  adjustment = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (priv->scrolled_window));
  scrolled = gtk_adjustment_get_value (adjustment) > 0.0;

  if (priv->scrolled == scrolled)
    return;

  priv->scrolled = scrolled;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SCROLLED]);
}


static void
phosh_app_search_init (PhoshAppSearch *self)
{
  PhoshAppSearchPrivate *priv = phosh_app_search_get_instance_private (self);
  GtkSortListModel *sorted;
  GtkAdjustment *adjustment;

  gtk_widget_init_template (GTK_WIDGET (self));

  /* fill the grid with apps */
  sorted = gtk_sort_list_model_new (G_LIST_MODEL (phosh_app_list_model_get_default ()),
                                    sort_apps,
                                    NULL,
                                    NULL);
  priv->model = gtk_filter_list_model_new (G_LIST_MODEL (sorted),
                                           search_apps,
                                           self,
                                           NULL);
  g_object_unref (sorted);
  gtk_flow_box_bind_model (GTK_FLOW_BOX (priv->apps),
                           G_LIST_MODEL (priv->model),
                           create_launcher, self, NULL);

  priv->settings = g_settings_new ("sm.puri.phosh");
  g_object_connect (priv->settings,
                    "swapped-signal::changed::force-adaptive",
                    G_CALLBACK (on_filter_setting_changed), self,
                    "swapped-signal::changed::app-filter-mode",
                    G_CALLBACK (on_filter_setting_changed), self,
                    NULL);
  on_filter_setting_changed (self, NULL, NULL);

  adjustment = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (priv->scrolled_window));
  g_signal_connect_swapped (adjustment, "notify::value",
                            G_CALLBACK (vertical_adjustment_changed), self);
}


static void
phosh_app_search_dispose (GObject *object)
{
  PhoshAppSearch *self = PHOSH_APP_SEARCH (object);
  PhoshAppSearchPrivate *priv = phosh_app_search_get_instance_private (self);

  g_clear_object (&priv->model);
  g_clear_object (&priv->settings);

  G_OBJECT_CLASS (phosh_app_search_parent_class)->dispose (object);
}


static void
phosh_app_search_finalize (GObject *object)
{
  PhoshAppSearch *self = PHOSH_APP_SEARCH (object);
  PhoshAppSearchPrivate *priv = phosh_app_search_get_instance_private (self);

  g_clear_pointer (&priv->search_string, g_free);
  g_strfreev (priv->force_adaptive);

  G_OBJECT_CLASS (phosh_app_search_parent_class)->finalize (object);
}


static void
phosh_app_search_class_init (PhoshAppSearchClass *klass)
{
  GObjectClass   *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = phosh_app_search_dispose;
  object_class->finalize = phosh_app_search_finalize;

  object_class->set_property = phosh_app_search_set_property;
  object_class->get_property = phosh_app_search_get_property;

  /**
   * PhoshAppSearch:text:
   *
   * Whether only adaptive apps should be shown
   */
  props[PROP_TEXT] =
    g_param_spec_string ("text",
                         "",
                         "",
                         "",
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  /**
   * PhoshAppSearch:scrolled:
   *
   * Whether the window is scrolled
   */
  props[PROP_SCROLLED] =
    g_param_spec_boolean ("scrolled",
                          "",
                          "",
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, PROP_LAST_PROP, props);

  gtk_widget_class_set_template_from_resource (widget_class, "/sm/puri/phosh/ui/app-search.ui");

  gtk_widget_class_bind_template_child_private (widget_class, PhoshAppSearch, apps);
  gtk_widget_class_bind_template_child_private (widget_class, PhoshAppSearch, scrolled_window);

  signals[APP_LAUNCHED] = g_signal_new ("app-launched",
                                        G_TYPE_FROM_CLASS (klass),
                                        G_SIGNAL_RUN_LAST,
                                        0, NULL, NULL, NULL,
                                        G_TYPE_NONE, 1, G_TYPE_APP_INFO);

  gtk_widget_class_set_css_name (widget_class, "phosh-app-search");
}


GtkWidget *
phosh_app_search_new (void)
{
  return g_object_new (PHOSH_TYPE_APP_SEARCH, NULL);
}


void
phosh_app_search_reset (PhoshAppSearch *self)
{
  PhoshAppSearchPrivate *priv;
  GtkAdjustment *adjustment;

  g_return_if_fail (PHOSH_IS_APP_SEARCH (self));

  priv = phosh_app_search_get_instance_private (self);

  adjustment = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (priv->scrolled_window));

  gtk_adjustment_set_value (adjustment, 0);
  g_clear_pointer (&priv->search_string, g_free);
}


void
phosh_app_search_set_text (PhoshAppSearch *self,
                           const gchar    *text)
{
  PhoshAppSearchPrivate *priv;

  g_return_if_fail (PHOSH_IS_APP_SEARCH (self));
  priv = phosh_app_search_get_instance_private (self);

  if (g_strcmp0 (priv->search_string, text) == 0)
    return;

  g_clear_pointer (&priv->search_string, g_free);
  priv->search_string = g_strdup (text);

  if (!priv->search_string || *priv->search_string == '\0') {
    GtkAdjustment *adjustment;
    adjustment = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (priv->scrolled_window));
    gtk_adjustment_set_value (adjustment, 0);
  }

  gtk_filter_list_model_refilter (priv->model);

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_TEXT]);
}


void
phosh_app_search_activate (PhoshAppSearch *self)
{
  PhoshAppSearchPrivate *priv;
  GtkFlowBoxChild *child;

  g_return_if_fail (PHOSH_IS_APP_SEARCH (self));
  priv = phosh_app_search_get_instance_private (self);

  if (!priv->search_string || *priv->search_string == '\0')
    return;

  child = gtk_flow_box_get_child_at_index (GTK_FLOW_BOX (priv->apps), 0);

  /* No results */
  if (child == NULL)
    return;

  if (G_LIKELY (PHOSH_IS_APP_GRID_BUTTON (child))) {
    gtk_widget_activate (GTK_WIDGET (child));
  } else {
    g_critical ("Unexpected child type, %s",
                g_type_name (G_TYPE_FROM_INSTANCE (child)));
  }
}


gboolean
phosh_app_search_get_scrolled (PhoshAppSearch *self)
{
  PhoshAppSearchPrivate *priv;

  g_return_val_if_fail (PHOSH_IS_APP_SEARCH (self), FALSE);

  priv = phosh_app_search_get_instance_private (self);

  return priv->scrolled;
}
