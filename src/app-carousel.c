/*
 * Copyright © 2019 Zander Brown <zbrown@gnome.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "phosh-app-carousel"

#include "feedback-manager.h"
#include "app-carousel.h"
#include "app-carousel-indicator.h"
#include "app-grid-button.h"
#include "app-list-model.h"
#include "favorite-list-model.h"
#include "paginated-model.h"
#include "shell.h"
#include "util.h"

#include "gtk-list-models/gtksortlistmodel.h"
#include "gtk-list-models/gtkfilterlistmodel.h"

#define APPS_PER_PAGE 24

/**
 * SECTION:app-carousel
 * @short_description: A carousel presenting the available applications in pages
 * @Title: PhoshAppCarousel
 *
 * Presents the available applications in a paginated carousel.
 */

enum {
  PROP_0,
  PROP_FILTER_ADAPTIVE,
  PROP_LAST_PROP
};
static GParamSpec *props[PROP_LAST_PROP];

enum {
  APP_LAUNCHED,
  N_SIGNALS
};
static guint signals[N_SIGNALS] = { 0 };

typedef struct _PhoshAppCarouselPrivate PhoshAppCarouselPrivate;
struct _PhoshAppCarouselPrivate {
  GtkFilterListModel *favorites_model;
  GtkFilterListModel *model;

  GtkWidget *carousel;
  GtkWidget *indicator;
  GtkWidget *btn_adaptive;
  GtkWidget *btn_adaptive_img;

  gboolean filter_adaptive;
  GSettings *settings;
  GStrv force_adaptive;
  GSimpleActionGroup *actions;
  PhoshAppFilterModeFlags filter_mode;
  guint debounce;
  gsize n_regular_pages;
  gsize n_favorites_pages;
};

G_DEFINE_TYPE_WITH_PRIVATE (PhoshAppCarousel, phosh_app_carousel, GTK_TYPE_BOX)

static void
app_launched_cb (GtkWidget    *widget,
                 GAppInfo     *info,
                 PhoshAppCarousel *self)
{
  phosh_trigger_feedback ("button-pressed");
  g_signal_emit (self, signals[APP_LAUNCHED], 0, info);
}


static void
update_app_button_adaptive (PhoshAppCarousel   *self,
                            PhoshAppGridButton *button)
{
  PhoshAppCarouselPrivate *priv = phosh_app_carousel_get_instance_private (self);
  GAppInfo *info;
  gboolean adaptive;

  g_return_if_fail (PHOSH_IS_APP_CAROUSEL (self));
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
  update_app_button_adaptive (PHOSH_APP_CAROUSEL (self), PHOSH_APP_GRID_BUTTON (btn));

  return btn;
}


static GtkFlowBox *
flowbox_new (void)
{
  return g_object_new (GTK_TYPE_FLOW_BOX,
                       "visible", TRUE,
                       "hexpand", FALSE,
                       "vexpand", FALSE,
                       "valign", GTK_ALIGN_START,
                       "homogeneous", TRUE,
                       "selection-mode", GTK_SELECTION_NONE,
                       "activate-on-single-click", FALSE,
                       "margin-start", 4,
                       "margin-end", 4,
                       "column-spacing", 4,
                       "row-spacing", 6,
                       "max-children-per-line", 4,
                       "min-children-per-line", 4,
                       NULL);
}


static void
remove_pages (GtkContainer *container,
              GList        *children,
              gsize         from,
              gsize         to,
              gsize         shift)
{
  for (gsize i = from; i < to; i++) {
    GList *child_item = g_list_nth (children, i + shift);
    GtkWidget *child_widget = GTK_WIDGET (child_item->data);
    gtk_container_remove (container, child_widget);
  }
}


static void
add_pages (PhoshAppCarousel *self,
           GListModel       *model,
           gsize             from,
           gsize             to,
           gsize             shift)
{
  PhoshAppCarouselPrivate *priv = phosh_app_carousel_get_instance_private (self);

  for (gsize i = from; i < to; i++) {
    GtkFlowBox *flowbox;
    /* FIXME the model should likely be freed */
    PhoshPaginatedModel *paginated_model;

    flowbox = flowbox_new ();

    paginated_model = phosh_paginated_model_new (model, APPS_PER_PAGE, i);

    hdy_carousel_insert (HDY_CAROUSEL (priv->carousel), GTK_WIDGET (flowbox), i + shift);

    gtk_flow_box_bind_model (flowbox,
                             G_LIST_MODEL (paginated_model),
                             create_launcher, self, NULL);
  }
}


static void
populate (PhoshAppCarousel *self)
{
  PhoshAppCarouselPrivate *priv = phosh_app_carousel_get_instance_private (self);
  g_autoptr(GList) children = gtk_container_get_children (GTK_CONTAINER (priv->carousel));
  gsize n_favorites_items, old_n_favorites_pages, new_n_favorites_pages;
  gsize n_regular_items, old_n_regular_pages, new_n_regular_pages;

  n_favorites_items = g_list_model_get_n_items (G_LIST_MODEL (priv->favorites_model));
  old_n_favorites_pages = priv->n_favorites_pages;
  new_n_favorites_pages = (n_favorites_items + APPS_PER_PAGE - 1) / APPS_PER_PAGE;
  priv->n_favorites_pages = new_n_favorites_pages;

  n_regular_items = g_list_model_get_n_items (G_LIST_MODEL (priv->model));
  old_n_regular_pages = priv->n_regular_pages;
  new_n_regular_pages = (n_regular_items + APPS_PER_PAGE - 1) / APPS_PER_PAGE;
  priv->n_regular_pages = new_n_regular_pages;

  remove_pages (GTK_CONTAINER (priv->carousel), children,
                new_n_favorites_pages, old_n_favorites_pages,
                0);
  remove_pages (GTK_CONTAINER (priv->carousel), children,
                new_n_regular_pages, old_n_regular_pages,
                old_n_favorites_pages);

  add_pages (self, G_LIST_MODEL (priv->favorites_model),
             old_n_favorites_pages, new_n_favorites_pages,
             0);
  add_pages (self, G_LIST_MODEL (priv->model),
             old_n_regular_pages, new_n_regular_pages,
             new_n_favorites_pages);

  phosh_app_carousel_indicator_set_favorites (PHOSH_APP_CAROUSEL_INDICATOR (priv->indicator),
                                              new_n_favorites_pages);
}


static void
phosh_app_carousel_set_property (GObject      *object,
                             guint         property_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
  PhoshAppCarousel *self = PHOSH_APP_CAROUSEL (object);

  switch (property_id) {
  case PROP_FILTER_ADAPTIVE:
    phosh_app_carousel_set_filter_adaptive (self, g_value_get_boolean (value));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}


static void
phosh_app_carousel_get_property (GObject    *object,
                             guint       property_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
  PhoshAppCarousel *self = PHOSH_APP_CAROUSEL (object);
  PhoshAppCarouselPrivate *priv = phosh_app_carousel_get_instance_private (self);

  switch (property_id) {
  case PROP_FILTER_ADAPTIVE:
    g_value_set_boolean (value, priv->filter_adaptive);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}


static int
sort_apps (gconstpointer a,
           gconstpointer b,
           gpointer      data)
{
  const char *empty = "";
  GAppInfo *info1 = G_APP_INFO (a);
  GAppInfo *info2 = G_APP_INFO (b);
  gboolean fav1;
  gboolean fav2;
  g_autofree char *s1 = NULL;
  g_autofree char *s2 = NULL;

  fav1 = info1 ? phosh_favorite_list_model_app_is_favorite (NULL, info1) : FALSE;
  fav2 = info2 ? phosh_favorite_list_model_app_is_favorite (NULL, info2) : FALSE;

  if (fav1 != fav2)
    return fav1 ? -1 : 1;

  s1 = g_utf8_casefold (g_app_info_get_name (info1), -1);
  s2 = g_utf8_casefold (g_app_info_get_name (info2), -1);

  return g_utf8_collate (s1 ?: empty, s2 ?: empty);
}


static void
update_filter_adaptive_button (PhoshAppCarousel *self)
{
  PhoshAppCarouselPrivate *priv;
  const char *icon_name;

  priv = phosh_app_carousel_get_instance_private (self);
  if (priv->filter_adaptive)
    icon_name = "phone-docked-symbolic";
  else
    icon_name = "phone-undocked-symbolic";

  gtk_image_set_from_icon_name (GTK_IMAGE (priv->btn_adaptive_img), icon_name, GTK_ICON_SIZE_BUTTON);
}


static void
on_filter_setting_changed (PhoshAppCarousel *self,
                           GParamSpec   *pspec,
                           gpointer     *unused)
{
  PhoshAppCarouselPrivate *priv;
  gboolean show;

  g_return_if_fail (PHOSH_IS_APP_CAROUSEL (self));

  priv = phosh_app_carousel_get_instance_private (self);

  g_strfreev (priv->force_adaptive);
  priv->force_adaptive = g_settings_get_strv (priv->settings,
                                              "force-adaptive");
  priv->filter_mode = g_settings_get_flags (priv->settings,
                                            "app-filter-mode");

  show = !!(priv->filter_mode & PHOSH_APP_FILTER_MODE_FLAGS_ADAPTIVE);
  gtk_widget_set_visible (priv->btn_adaptive, show);

  gtk_filter_list_model_refilter (priv->favorites_model);
  gtk_filter_list_model_refilter (priv->model);
}


static gboolean
filter_apps (PhoshAppCarousel *self,
             GAppInfo         *info)
{
  PhoshAppCarouselPrivate *priv = phosh_app_carousel_get_instance_private (self);

  if (!(priv->filter_mode & PHOSH_APP_FILTER_MODE_FLAGS_ADAPTIVE))
    return TRUE;

  if (!priv->filter_adaptive)
    return TRUE;

  return phosh_util_get_app_is_adaptive (info, (const char * const *) priv->force_adaptive);
}


static gboolean
filter_favorites_apps (gpointer item, gpointer data)
{
  PhoshAppCarousel *self = data;
  PhoshAppCarouselPrivate *priv = phosh_app_carousel_get_instance_private (self);
  GAppInfo *info = item;

  g_return_val_if_fail (priv != NULL, TRUE);

  if (!phosh_favorite_list_model_app_is_favorite (NULL, info))
    return FALSE;

  return filter_apps (self, info);
}


static gboolean
filter_regular_apps (gpointer item, gpointer data)
{
  PhoshAppCarousel *self = data;
  PhoshAppCarouselPrivate *priv = phosh_app_carousel_get_instance_private (self);
  GAppInfo *info = item;

  g_return_val_if_fail (priv != NULL, TRUE);

  if (phosh_favorite_list_model_app_is_favorite (NULL, info))
    return FALSE;

  return filter_apps (self, info);
}


static void
favorites_changed (GListModel   *list,
                   guint         position,
                   guint         removed,
                   guint         added,
                   PhoshAppCarousel *self)
{
  PhoshAppCarouselPrivate *priv = phosh_app_carousel_get_instance_private (self);
  GListModel *sort_model;

  sort_model = gtk_filter_list_model_get_model (GTK_FILTER_LIST_MODEL (priv->model));
  gtk_sort_list_model_resort (GTK_SORT_LIST_MODEL (sort_model));
}


static void
phosh_app_carousel_init (PhoshAppCarousel *self)
{
  PhoshAppCarouselPrivate *priv = phosh_app_carousel_get_instance_private (self);
  PhoshFavoriteListModel *favorites;
  PhoshAppListModel *apps;
  g_autoptr (GtkSortListModel) sorted = NULL;
  g_autoptr (GAction) action = NULL;

  g_type_ensure (PHOSH_TYPE_APP_CAROUSEL_INDICATOR);

  gtk_widget_init_template (GTK_WIDGET (self));

  favorites = phosh_favorite_list_model_get_default ();
  priv->favorites_model = gtk_filter_list_model_new (G_LIST_MODEL (favorites), filter_favorites_apps, self, NULL);

  g_signal_connect (priv->favorites_model,
                    "items-changed",
                    G_CALLBACK (favorites_changed),
                    self);

  /* fill the grid with apps */
  apps = phosh_app_list_model_get_default ();
  sorted = gtk_sort_list_model_new (G_LIST_MODEL (apps), sort_apps, NULL, NULL);
  priv->model = gtk_filter_list_model_new (G_LIST_MODEL (sorted), filter_regular_apps, self, NULL);

  g_signal_connect_swapped (priv->model,
                            "items-changed",
                            (GCallback) populate,
                            self);

  populate (self);

  priv->settings = g_settings_new ("sm.puri.phosh");
  g_object_connect (priv->settings,
                    "swapped-signal::changed::force-adaptive",
                    G_CALLBACK (on_filter_setting_changed), self,
                    "swapped-signal::changed::app-filter-mode",
                    G_CALLBACK (on_filter_setting_changed), self,
                    NULL);
  on_filter_setting_changed (self, NULL, NULL);

  priv->actions = g_simple_action_group_new ();
  gtk_widget_insert_action_group (GTK_WIDGET (self), "app-grid",
                                  G_ACTION_GROUP (priv->actions));
  action = (GAction*) g_property_action_new ("filter-adaptive", self, "filter-adaptive");
  g_action_map_add_action (G_ACTION_MAP (priv->actions), action);
}


static void
phosh_app_carousel_dispose (GObject *object)
{
  PhoshAppCarousel *self = PHOSH_APP_CAROUSEL (object);
  PhoshAppCarouselPrivate *priv = phosh_app_carousel_get_instance_private (self);

  g_clear_object (&priv->actions);
  g_clear_object (&priv->favorites_model);
  g_clear_object (&priv->model);
  g_clear_object (&priv->settings);
  g_clear_handle_id (&priv->debounce, g_source_remove);

  G_OBJECT_CLASS (phosh_app_carousel_parent_class)->dispose (object);
}


static void
phosh_app_carousel_finalize (GObject *object)
{
  PhoshAppCarousel *self = PHOSH_APP_CAROUSEL (object);
  PhoshAppCarouselPrivate *priv = phosh_app_carousel_get_instance_private (self);

  g_strfreev (priv->force_adaptive);

  G_OBJECT_CLASS (phosh_app_carousel_parent_class)->finalize (object);
}


static void
phosh_app_carousel_class_init (PhoshAppCarouselClass *klass)
{
  GObjectClass   *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = phosh_app_carousel_dispose;
  object_class->finalize = phosh_app_carousel_finalize;

  object_class->set_property = phosh_app_carousel_set_property;
  object_class->get_property = phosh_app_carousel_get_property;

  /**
   * PhoshAppCarousel:filter-adaptive:
   *
   * Whether only adaptive apps should be shown.
   */
  props[PROP_FILTER_ADAPTIVE] =
    g_param_spec_boolean ("filter-adaptive",
                          "Filter adaptive",
                          "Whether only adaptive apps should be shown",
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, PROP_LAST_PROP, props);

  gtk_widget_class_set_template_from_resource (widget_class, "/sm/puri/phosh/ui/app-carousel.ui");

  gtk_widget_class_bind_template_child_private (widget_class, PhoshAppCarousel, btn_adaptive);
  gtk_widget_class_bind_template_child_private (widget_class, PhoshAppCarousel, btn_adaptive_img);
  gtk_widget_class_bind_template_child_private (widget_class, PhoshAppCarousel, carousel);
  gtk_widget_class_bind_template_child_private (widget_class, PhoshAppCarousel, indicator);

  signals[APP_LAUNCHED] = g_signal_new ("app-launched",
                                        G_TYPE_FROM_CLASS (klass),
                                        G_SIGNAL_RUN_LAST,
                                        0, NULL, NULL, NULL,
                                        G_TYPE_NONE, 1, G_TYPE_APP_INFO);

  gtk_widget_class_set_css_name (widget_class, "phosh-app-carousel");
}


GtkWidget *
phosh_app_carousel_new (void)
{
  return g_object_new (PHOSH_TYPE_APP_CAROUSEL, NULL);
}


void
phosh_app_carousel_set_filter_adaptive (PhoshAppCarousel *self, gboolean enable)
{
  PhoshAppCarouselPrivate *priv;

  g_debug ("Filter-adaptive: %d", enable);

  g_return_if_fail (PHOSH_IS_APP_CAROUSEL (self));
  priv = phosh_app_carousel_get_instance_private (self);

  if (priv->filter_adaptive == enable)
    return;

  priv->filter_adaptive = enable;
  update_filter_adaptive_button (self);

  gtk_filter_list_model_refilter (priv->favorites_model);
  gtk_filter_list_model_refilter (priv->model);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_FILTER_ADAPTIVE]);
}


void
phosh_app_carousel_reset (PhoshAppCarousel *self)
{
  PhoshAppCarouselPrivate *priv;
  g_autoptr(GList) children = NULL;

  g_return_if_fail (PHOSH_IS_APP_CAROUSEL (self));
  priv = phosh_app_carousel_get_instance_private (self);

  children = gtk_container_get_children (GTK_CONTAINER (priv->carousel));
  if (children && GTK_IS_WIDGET (children->data))
    hdy_carousel_scroll_to_full (HDY_CAROUSEL (priv->carousel), GTK_WIDGET (children->data), 0);
}
