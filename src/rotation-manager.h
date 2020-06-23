/*
 * Copyright (C) 2020 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "lockscreen-manager.h"
#include "sensor-proxy-manager.h"

G_BEGIN_DECLS

#define PHOSH_TYPE_ROTATION_MANAGER (phosh_rotation_manager_get_type ())

G_DECLARE_FINAL_TYPE (PhoshRotationManager, phosh_rotation_manager, PHOSH, ROTATION_MANAGER, GObject);

PhoshRotationManager *phosh_rotation_manager_new (PhoshSensorProxyManager *sensor_proxy_manager,
                                                  PhoshLockscreenManager  *lockscreen_manager);
void                  phosh_rotation_manager_set_orientation_locked (PhoshRotationManager *self,
                                                                     gboolean              locked);
gboolean              phosh_rotation_manager_get_orientation_locked (PhoshRotationManager *self);

G_END_DECLS
