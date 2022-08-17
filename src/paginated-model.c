/*
 * Copyright © 2022 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Author: Adrien Plazas <adrien.plazas@puri.sm>
 */

#include "paginated-model.h"

/**
 * SECTION:paginated-model
 * @short_description: A model offering a paginated view
 * @Title: PhoshPaginatedModel
 *
 * This splits a model into pages and presents the items contained in
 * one of these pages.
 */

typedef struct _PhoshPaginatedModel
{
  GObject parent_instance;

  GListModel *model;
  guint page_size;
  guint page_index;
  guint n_items;
} PhoshPaginatedModel;

static void list_iface_init (GListModelInterface *iface);

G_DEFINE_TYPE_WITH_CODE (PhoshPaginatedModel, phosh_paginated_model, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_LIST_MODEL, list_iface_init))

enum {
  PROP_0,
  PROP_MODEL,
  PROP_PAGE_SIZE,
  PROP_PAGE_INDEX,
  LAST_PROP
};

static GParamSpec *props [LAST_PROP];

static void
update_n_items (PhoshPaginatedModel *self)
{
  guint n_items = g_list_model_get_n_items (self->model);
  guint pages = (n_items + self->page_size - 1) / self->page_size;

  if (self->page_index < pages - 1)
    n_items = self->page_size;
  else if (self->page_index > pages - 1)
    n_items = 0;
  else
    n_items -= self->page_size * self->page_index;

  self->n_items = n_items;
}

static void
items_changed_cb (PhoshPaginatedModel *self,
                  guint                position,
                  guint                removed,
                  guint                added)
{
  guint start, end, previous_n_items, new_n_items;

  /* start is inclusive, end is exclusive */
  start = self->page_index * self->page_size;
  end = start + self->page_size;

  /* The change only affects following pages. */
  if (position >= end)
    return;

  start = MIN (start, g_list_model_get_n_items (self->model));
  end   = MIN (end,   g_list_model_get_n_items (self->model));

  previous_n_items = self->n_items;
  new_n_items = end - start;

  self->n_items = new_n_items;

  g_list_model_items_changed (G_LIST_MODEL (self), 0, previous_n_items, new_n_items);
}

static void
phosh_paginated_model_constructed (GObject *object)
{
  PhoshPaginatedModel *self = (PhoshPaginatedModel *)object;

  G_OBJECT_CLASS (phosh_paginated_model_parent_class)->constructed (object);

  update_n_items (self);

  g_signal_connect_swapped (self->model,
                            "items-changed",
                            (GCallback) items_changed_cb,
                            self);
}

static void
phosh_paginated_model_dispose (GObject *object)
{
  PhoshPaginatedModel *self = (PhoshPaginatedModel *)object;

  g_clear_object (&self->model);

  G_OBJECT_CLASS (phosh_paginated_model_parent_class)->dispose (object);
}

static void
phosh_paginated_model_get_property (GObject    *object,
                                    guint       prop_id,
                                    GValue     *value,
                                    GParamSpec *pspec)
{
  PhoshPaginatedModel *self = PHOSH_PAGINATED_MODEL (object);

  switch (prop_id) {
  case PROP_MODEL:
    g_value_set_object (value, self->model);
    break;
  case PROP_PAGE_SIZE:
    g_value_set_uint (value, self->page_size);
    break;
  case PROP_PAGE_INDEX:
    g_value_set_uint (value, self->page_index);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }
}

static void
phosh_paginated_model_set_property (GObject      *object,
                                    guint         prop_id,
                                    const GValue *value,
                                    GParamSpec   *pspec)
{
  PhoshPaginatedModel *self = PHOSH_PAGINATED_MODEL (object);

  switch (prop_id) {
  case PROP_MODEL:
    g_set_object (&self->model, g_value_get_object (value));
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_MODEL]);
    break;
  case PROP_PAGE_SIZE:
    self->page_size = g_value_get_uint (value);
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_PAGE_SIZE]);
    break;
  case PROP_PAGE_INDEX:
    self->page_index = g_value_get_uint (value);
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_PAGE_INDEX]);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }
}

static void
phosh_paginated_model_class_init (PhoshPaginatedModelClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = phosh_paginated_model_constructed;
  object_class->dispose = phosh_paginated_model_dispose;
  object_class->get_property = phosh_paginated_model_get_property;
  object_class->set_property = phosh_paginated_model_set_property;

  /**
   * PhoshAppCarousel:model:
   *
   * The #GListModel to paginate.
   */
  props[PROP_MODEL] =
    g_param_spec_object (
      "model",
      "Model",
      "The model to paginate",
      G_TYPE_LIST_MODEL,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE |
      G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

  /**
   * PhoshAppCarousel:page-size:
   *
   * The number of items per page.
   */
  props[PROP_PAGE_SIZE] =
    g_param_spec_uint (
      "page-size",
      "Page size",
      "The number of items per page",
      0,
      G_MAXUINT,
      0,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE |
      G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

  /**
   * PhoshAppCarousel:page-index:
   *
   * The index of the page.
   */
  props[PROP_PAGE_INDEX] =
    g_param_spec_uint (
      "page-index",
      "Page index",
      "The index of the page",
      0,
      G_MAXUINT,
      1,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE |
      G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

  g_object_class_install_properties (object_class, LAST_PROP, props);
}

static void
phosh_paginated_model_init (PhoshPaginatedModel *self)
{
  self->page_index = 1;
}

static GType
list_get_item_type (GListModel *list)
{
  PhoshPaginatedModel *self = PHOSH_PAGINATED_MODEL (list);

  return g_list_model_get_item_type (self->model);
}

static gpointer
list_get_item (GListModel *list,
               guint       position)
{
  PhoshPaginatedModel *self = PHOSH_PAGINATED_MODEL (list);

  return g_list_model_get_item (self->model, position + self->page_index * self->page_size);
}

static guint
list_get_n_items (GListModel *list)
{
  PhoshPaginatedModel *self = PHOSH_PAGINATED_MODEL (list);

  update_n_items (self);

  return self->n_items;
}

static void
list_iface_init (GListModelInterface *iface)
{
  iface->get_item_type = list_get_item_type;
  iface->get_item = list_get_item;
  iface->get_n_items = list_get_n_items;
}

/**
 * phosh_paginated_model_new:
 * @model: A #GListModel
 * @page_size: The number of items per page
 * @page_index: The index of the page
 *
 * Return Value: The newly created #PhoshPaginatedModel
 */
PhoshPaginatedModel *
phosh_paginated_model_new (GListModel *model,
                           guint       page_size,
                           guint       page_index)
{
  g_return_val_if_fail (G_IS_LIST_MODEL (model), NULL);
  g_return_val_if_fail (page_size > 0, NULL);

  return g_object_new (PHOSH_TYPE_PAGINATED_MODEL,
                       "model", model,
                       "page-size", page_size,
                       "page-index", page_index,
                       NULL);
}
