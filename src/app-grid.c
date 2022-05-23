/*
 * Copyright © 2019 Zander Brown <zbrown@gnome.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "phosh-app-grid"

#define ACTIVE_SEARCH_CLASS "search-active"

#define SEARCH_DEBOUNCE 350
#define DEFAULT_GTK_DEBOUNCE 150

#define _GNU_SOURCE
#include <string.h>

#include "feedback-manager.h"
#include "app-carousel.h"
#include "app-grid.h"
#include "app-grid-button.h"
#include "app-list-model.h"
#include "favorite-list-model.h"
#include "shell.h"

#include "gtk-list-models/gtksortlistmodel.h"
#include "gtk-list-models/gtkfilterlistmodel.h"

enum {
  APP_LAUNCHED,
  N_SIGNALS
};
static guint signals[N_SIGNALS] = { 0 };

typedef struct _PhoshAppGridPrivate PhoshAppGridPrivate;
struct _PhoshAppGridPrivate {
  GtkFilterListModel *model;

  GtkWidget *search;
  GtkWidget *favs;
  GtkWidget *favs_revealer;

  char *search_string;
  GSettings *settings;
  GStrv force_adaptive;
  PhoshAppFilterModeFlags filter_mode;
  guint debounce;
};

G_DEFINE_TYPE_WITH_PRIVATE (PhoshAppGrid, phosh_app_grid, GTK_TYPE_BOX)

static void
app_launched_cb (GtkWidget    *widget,
                 GAppInfo     *info,
                 PhoshAppGrid *self)
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


static gboolean
filter_adaptive (PhoshAppGrid *self, GDesktopAppInfo *info)
{
  PhoshAppGridPrivate *priv = phosh_app_grid_get_instance_private (self);
  g_autofree char *mobile = NULL;
  const char *id;

  if (!(priv->filter_mode & PHOSH_APP_FILTER_MODE_FLAGS_ADAPTIVE))
    return TRUE;

  mobile = g_desktop_app_info_get_string (G_DESKTOP_APP_INFO (info),
                                          "X-Purism-FormFactor");
  if (mobile && strcasestr (mobile, "mobile;"))
    return TRUE;

  g_free (mobile);
  mobile = g_desktop_app_info_get_string (G_DESKTOP_APP_INFO (info),
                                          "X-KDE-FormFactor");
  if (mobile && strcasestr (mobile, "handset;"))
    return TRUE;

  id = g_app_info_get_id (G_APP_INFO (info));
  if (id && g_strv_contains ((const char * const*)priv->force_adaptive, id))
    return TRUE;

  return FALSE;
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
  PhoshAppGrid *self = data;
  PhoshAppGridPrivate *priv = phosh_app_grid_get_instance_private (self);
  GAppInfo *info = item;
  const char *search = NULL;
  const char *str = NULL;

  g_return_val_if_fail (priv != NULL, TRUE);
  g_return_val_if_fail (priv->search != NULL, TRUE);

  search = priv->search_string;

  if (G_IS_DESKTOP_APP_INFO (info)) {
    if (!filter_adaptive (self, G_DESKTOP_APP_INFO (info)))
      return FALSE;
  }

  /* filter out favorites when not searching */
  if (search == NULL || strlen (search) == 0) {
    if (phosh_favorite_list_model_app_is_favorite (NULL, info))
      return FALSE;

    return TRUE;
  }

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


static GtkWidget *
create_favorite_launcher (gpointer item,
                          gpointer self)
{
  GtkWidget *btn = phosh_app_grid_button_new_favorite (G_APP_INFO (item));

  g_signal_connect (btn, "app-launched",
                    G_CALLBACK (app_launched_cb), self);

  gtk_widget_show (btn);

  return btn;
}


static void
favorites_changed (GListModel   *list,
                   guint         position,
                   guint         removed,
                   guint         added,
                   PhoshAppGrid *self)
{
  PhoshAppGridPrivate *priv = phosh_app_grid_get_instance_private (self);

  /* We don't show favorites in the main list, filter them out */
  gtk_filter_list_model_refilter (priv->model);
}


static void
phosh_app_grid_init (PhoshAppGrid *self)
{
  PhoshAppGridPrivate *priv = phosh_app_grid_get_instance_private (self);
  GtkSortListModel *sorted;
  PhoshFavoriteListModel *favorites;

  gtk_widget_init_template (GTK_WIDGET (self));

  favorites = phosh_favorite_list_model_get_default ();

  gtk_flow_box_bind_model (GTK_FLOW_BOX (priv->favs),
                           G_LIST_MODEL (favorites),
                           create_favorite_launcher, self, NULL);
  g_signal_connect (favorites,
                    "items-changed",
                    G_CALLBACK (favorites_changed),
                    self);

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
}


static void
phosh_app_grid_dispose (GObject *object)
{
  PhoshAppGrid *self = PHOSH_APP_GRID (object);
  PhoshAppGridPrivate *priv = phosh_app_grid_get_instance_private (self);

  g_clear_object (&priv->model);
  g_clear_object (&priv->settings);
  g_clear_handle_id (&priv->debounce, g_source_remove);

  G_OBJECT_CLASS (phosh_app_grid_parent_class)->dispose (object);
}


static void
phosh_app_grid_finalize (GObject *object)
{
  PhoshAppGrid *self = PHOSH_APP_GRID (object);
  PhoshAppGridPrivate *priv = phosh_app_grid_get_instance_private (self);

  g_clear_pointer (&priv->search_string, g_free);
  g_strfreev (priv->force_adaptive);

  G_OBJECT_CLASS (phosh_app_grid_parent_class)->finalize (object);
}


static gboolean
phosh_app_grid_key_press_event (GtkWidget   *widget,
                              GdkEventKey *event)
{
  PhoshAppGrid *self = PHOSH_APP_GRID (widget);
  PhoshAppGridPrivate *priv = phosh_app_grid_get_instance_private (self);

  return gtk_search_entry_handle_event (GTK_SEARCH_ENTRY (priv->search),
                                        (GdkEvent *) event);
}


static gboolean
do_search (PhoshAppGrid *self)
{
  PhoshAppGridPrivate *priv = phosh_app_grid_get_instance_private (self);

  if (priv->search_string && *priv->search_string != '\0') {
    gtk_revealer_set_reveal_child (GTK_REVEALER (priv->favs_revealer), FALSE);
  } else {
    gtk_revealer_set_reveal_child (GTK_REVEALER (priv->favs_revealer), TRUE);
  }

  gtk_filter_list_model_refilter (priv->model);

  priv->debounce = 0;
  return G_SOURCE_REMOVE;
}


static void
search_changed (GtkSearchEntry *entry,
                PhoshAppGrid   *self)
{
  PhoshAppGridPrivate *priv = phosh_app_grid_get_instance_private (self);
  const char *search = gtk_entry_get_text (GTK_ENTRY (entry));

  g_clear_pointer (&priv->search_string, g_free);

  g_clear_handle_id (&priv->debounce, g_source_remove);

  if (search && *search != '\0') {
    priv->search_string = g_utf8_casefold (search, -1);

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
                        PhoshAppGrid   *self)
{
  PhoshAppGridPrivate *priv = phosh_app_grid_get_instance_private (self);

  g_clear_pointer (&priv->search_string, g_free);

  if (preedit && *preedit != '\0')
    priv->search_string = g_utf8_casefold (preedit, -1);

  g_clear_handle_id (&priv->debounce, g_source_remove);

  priv->debounce = g_timeout_add (SEARCH_DEBOUNCE + DEFAULT_GTK_DEBOUNCE, (GSourceFunc) do_search, self);
  g_source_set_name_by_id (priv->debounce, "[phosh] debounce app grid search (preedit-changed)");
}


static void
search_activated (GtkSearchEntry *entry,
                  PhoshAppGrid   *self)
{
  PhoshAppGridPrivate *priv = phosh_app_grid_get_instance_private (self);
  GtkFlowBoxChild *child = NULL;

  if (!gtk_widget_has_focus (GTK_WIDGET (entry)))
    return;

  /* Don't activate when there isn't an active search */
  if (!priv->search_string || *priv->search_string == '\0') {
    return;
  }

  /* No results */
  if (child == NULL) {
    return;
  }

  if (G_LIKELY (PHOSH_IS_APP_GRID_BUTTON (child))) {
    gtk_widget_activate (GTK_WIDGET (child));
  } else {
    g_critical ("Unexpected child type, %s",
                g_type_name (G_TYPE_FROM_INSTANCE (child)));
  }
}


static void
phosh_app_grid_class_init (PhoshAppGridClass *klass)
{
  GObjectClass   *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = phosh_app_grid_dispose;
  object_class->finalize = phosh_app_grid_finalize;

  widget_class->key_press_event = phosh_app_grid_key_press_event;

  gtk_widget_class_set_template_from_resource (widget_class, "/sm/puri/phosh/ui/app-grid.ui");

  gtk_widget_class_bind_template_child_private (widget_class, PhoshAppGrid, favs);
  gtk_widget_class_bind_template_child_private (widget_class, PhoshAppGrid, favs_revealer);
  gtk_widget_class_bind_template_child_private (widget_class, PhoshAppGrid, search);

  gtk_widget_class_bind_template_callback (widget_class, search_changed);
  gtk_widget_class_bind_template_callback (widget_class, search_preedit_changed);
  gtk_widget_class_bind_template_callback (widget_class, search_activated);

  signals[APP_LAUNCHED] = g_signal_new ("app-launched",
                                        G_TYPE_FROM_CLASS (klass),
                                        G_SIGNAL_RUN_LAST,
                                        0, NULL, NULL, NULL,
                                        G_TYPE_NONE, 1, G_TYPE_APP_INFO);

  gtk_widget_class_set_css_name (widget_class, "phosh-app-grid");
}


GtkWidget *
phosh_app_grid_new (void)
{
  return g_object_new (PHOSH_TYPE_APP_GRID, NULL);
}


void
phosh_app_grid_reset (PhoshAppGrid *self)
{
  PhoshAppGridPrivate *priv;

  g_return_if_fail (PHOSH_IS_APP_GRID (self));

  priv = phosh_app_grid_get_instance_private (self);

  gtk_entry_set_text (GTK_ENTRY (priv->search), "");
  g_clear_pointer (&priv->search_string, g_free);
}


void
phosh_app_grid_focus_search (PhoshAppGrid *self)
{
  PhoshAppGridPrivate *priv;

  g_return_if_fail (PHOSH_IS_APP_GRID (self));
  priv = phosh_app_grid_get_instance_private (self);
  gtk_widget_grab_focus (priv->search);
}


gboolean
phosh_app_grid_handle_search (PhoshAppGrid *self, GdkEvent *event)
{
  PhoshAppGridPrivate *priv;
  gboolean ret;

  g_return_val_if_fail (PHOSH_IS_APP_GRID (self), GDK_EVENT_PROPAGATE);
  priv = phosh_app_grid_get_instance_private (self);
  ret = gtk_search_entry_handle_event (GTK_SEARCH_ENTRY (priv->search), event);
  if (ret == GDK_EVENT_STOP)
    gtk_entry_grab_focus_without_selecting (GTK_ENTRY (priv->search));

  return ret;
}
