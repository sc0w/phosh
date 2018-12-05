/*
 * Copyright (C) 2018 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0+
 */

#ifndef PHOSH_SETTINGS_H
#define PHOSH_SETTINGS_H

#include <gtk/gtk.h>

#define PHOSH_TYPE_SETTINGS (phosh_settings_get_type())

G_DECLARE_FINAL_TYPE (PhoshSettings, phosh_settings, PHOSH, SETTINGS, GtkWindow)

GtkWidget * phosh_settings_new (void);

#endif /* PHOSH_SETTINGS_H */