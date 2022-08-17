/*
 * Copyright (C) 2018 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define PHOSH_TYPE_OVERVIEW (phosh_overview_get_type())

G_DECLARE_FINAL_TYPE (PhoshOverview, phosh_overview, PHOSH, OVERVIEW, GtkBox)


GtkWidget *phosh_overview_new (void);
void       phosh_overview_reset (PhoshOverview *self, gboolean reset_thumbnails);
void       phosh_overview_focus_app_search (PhoshOverview *self);
gboolean   phosh_overview_has_running_activities (PhoshOverview *self);
gboolean   phosh_overview_search_activated (PhoshOverview *self);
gboolean   phosh_overview_handle_search (PhoshOverview *self, GdkEvent *event);
gboolean   phosh_overview_get_scrolled (PhoshOverview *self);
gboolean   phosh_overview_get_activity_swiped (PhoshOverview *self);

G_END_DECLS
