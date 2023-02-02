/*
 * Copyright (C) 2020 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <wayland-client-protocol.h>

#include <gtk/gtk.h>

#define PHOSH_TYPE_THUMBNAIL (phosh_thumbnail_get_type())

G_DECLARE_DERIVABLE_TYPE (PhoshThumbnail,
                          phosh_thumbnail,
                          PHOSH,
                          THUMBNAIL,
                          GObject)

/**
 * PhoshThumbnailClass:
 * @parent_class: the parent class
 * @get_image: Get the current image data
 * @get_size: get current image size and stride
 * @is_ready: whether the image is ready to be fetched
 * @set_ready: Set image as ready. Must chain up.
 */
struct _PhoshThumbnailClass
{
  GObjectClass parent_class;
  void*        (*get_image) (PhoshThumbnail *self);
  void         (*get_size)  (PhoshThumbnail *self, guint *width, guint *height, guint *stride);
  void         (*get_format)(PhoshThumbnail *self, enum wl_shm_format *format);
  gboolean     (*is_ready)  (PhoshThumbnail *self);
  void         (*set_ready) (PhoshThumbnail *self, gboolean ready);
};

void     *phosh_thumbnail_get_image (PhoshThumbnail *self);
void      phosh_thumbnail_get_size  (PhoshThumbnail *self, guint *width, guint *height, guint *stride);
void      phosh_thumbnail_get_format(PhoshThumbnail *self, enum wl_shm_format *format);
gboolean  phosh_thumbnail_is_ready  (PhoshThumbnail *self);
