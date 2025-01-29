/*
 * Copyright (C) 2018 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Author: Guido Günther <agx@sigxcpu.org>
 */

#pragma once

#include "lockscreen-manager.h"

#include "dbus/phosh-screen-saver-dbus.h"

#include <glib-object.h>

G_BEGIN_DECLS

#define PHOSH_TYPE_SCREEN_SAVER_MANAGER             (phosh_screen_saver_manager_get_type ())

G_DECLARE_FINAL_TYPE (PhoshScreenSaverManager, phosh_screen_saver_manager, PHOSH, SCREEN_SAVER_MANAGER,
                      PhoshDBusScreenSaverSkeleton)

PhoshScreenSaverManager *phosh_screen_saver_manager_new (PhoshLockscreenManager *lockscreen_manager);
void phosh_screen_saver_manager_suspend_autolock        (PhoshScreenSaverManager *self,
                                                         gboolean                 suspend);

G_END_DECLS
