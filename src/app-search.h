/*
 * Copyright © 2019 Zander Brown <zbrown@gnome.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>

#include <handy.h>

#pragma once

G_BEGIN_DECLS

#define PHOSH_TYPE_APP_SEARCH phosh_app_search_get_type()
G_DECLARE_DERIVABLE_TYPE (PhoshAppSearch, phosh_app_search, PHOSH, APP_SEARCH, GtkBox)

struct _PhoshAppSearchClass
{
  GtkBoxClass parent_class;
};

GtkWidget *phosh_app_search_new (void);
void       phosh_app_search_reset (PhoshAppSearch *self);
void       phosh_app_search_set_text (PhoshAppSearch *self, const gchar *text);
void       phosh_app_search_activate (PhoshAppSearch *self);
gboolean   phosh_app_search_get_scrolled (PhoshAppSearch *self);


G_END_DECLS
