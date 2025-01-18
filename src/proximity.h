/*
 * Copyright (C) 2020 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "calls-manager.h"
#include "sensor-proxy-manager.h"

#define PHOSH_SHELL_PROXIMITY_SCHEMA_ID "mobi.phosh.shell.proximity"

G_BEGIN_DECLS

#define PHOSH_TYPE_PROXIMITY (phosh_proximity_get_type ())

G_DECLARE_FINAL_TYPE (PhoshProximity, phosh_proximity, PHOSH, PROXIMITY, GObject);

PhoshProximity *phosh_proximity_new (PhoshSensorProxyManager *sensor_proxy_manager,
                                     PhoshCallsManager       *calls_manager);
gboolean        phosh_proximity_has_fader (PhoshProximity *sensor_proxy_manager);
gboolean        phosh_proximity_sensor_enabled (PhoshProximity *sensor_proxy_manager);

G_END_DECLS
