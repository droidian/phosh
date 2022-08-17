/*
 * Copyright © 2022 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gio/gio.h>
#include <glib-object.h>

G_BEGIN_DECLS

#define PHOSH_TYPE_PAGINATED_MODEL (phosh_paginated_model_get_type())

G_DECLARE_FINAL_TYPE (PhoshPaginatedModel, phosh_paginated_model, PHOSH, PAGINATED_MODEL, GObject)

PhoshPaginatedModel *phosh_paginated_model_new (GListModel *model,
                                                guint       page_size,
                                                guint       page_index);

G_END_DECLS
