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

/**
 * PhoshAppFilterModeFlags:
 * @PHOSH_APP_FILTER_MODE_FLAGS_NONE: No filtering
 * @PHOSH_APP_FILTER_MODE_FLAGS_ADAPTIVE: Only show apps in mobile mode that adapt
 *    to smalls screen sizes.
 *
 * Controls what kind of app filtering is done.
*/
typedef enum {
  PHOSH_APP_FILTER_MODE_FLAGS_NONE      = 0,
  PHOSH_APP_FILTER_MODE_FLAGS_ADAPTIVE  = (1 << 0),
} PhoshAppFilterModeFlags;

#define PHOSH_TYPE_APP_CAROUSEL phosh_app_carousel_get_type()
G_DECLARE_DERIVABLE_TYPE (PhoshAppCarousel, phosh_app_carousel, PHOSH, APP_CAROUSEL, GtkBox)

struct _PhoshAppCarouselClass
{
  GtkBoxClass parent_class;
};

GtkWidget *phosh_app_carousel_new (void);
void       phosh_app_carousel_set_filter_adaptive (PhoshAppCarousel *self, gboolean enable);
void       phosh_app_carousel_reset (PhoshAppCarousel *self);


G_END_DECLS
