/*
 * Copyright (C) 2025 Droidian Project
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "backlight-priv.h"

G_BEGIN_DECLS

#define PHOSH_TYPE_BACKLIGHT_LIBDROID (phosh_backlight_libdroid_get_type ())

G_DECLARE_FINAL_TYPE (PhoshBacklightLibdroid, phosh_backlight_libdroid, PHOSH, BACKLIGHT_LIBDROID,
                      PhoshBacklight)

PhoshBacklightLibdroid *phosh_backlight_libdroid_new (GError **error);

G_END_DECLS
