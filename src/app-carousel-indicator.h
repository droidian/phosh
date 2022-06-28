/*
 * Copyright (C) 2020 Alexander Mikhaylenko <alexm@gnome.org>
 * Copyright (C) 2022 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* Based on hdy-carousel-indicator-dots.h from Libhandy 1.6 published
 * as LGPL-2.1-or-later.
 */

#pragma once

#include <handy.h>

G_BEGIN_DECLS

#define PHOSH_TYPE_APP_CAROUSEL_INDICATOR (phosh_app_carousel_indicator_get_type())

G_DECLARE_FINAL_TYPE (PhoshAppCarouselIndicator, phosh_app_carousel_indicator, PHOSH, APP_CAROUSEL_INDICATOR, GtkDrawingArea)

GtkWidget *phosh_app_carousel_indicator_new (void);

HdyCarousel *phosh_app_carousel_indicator_get_carousel (PhoshAppCarouselIndicator *self);
void         phosh_app_carousel_indicator_set_carousel (PhoshAppCarouselIndicator *self,
                                                        HdyCarousel               *carousel);

guint phosh_app_carousel_indicator_get_favorites (PhoshAppCarouselIndicator *self);
void  phosh_app_carousel_indicator_set_favorites (PhoshAppCarouselIndicator *self,
                                                  guint                      favorites);

G_END_DECLS
