/*
 * Copyright (C) 2021 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "layersurface.h"
#include "phoc-layer-shell-effects-unstable-v1-client-protocol.h"

G_BEGIN_DECLS

#define PHOSH_TYPE_DRAG_SURFACE (phosh_drag_surface_get_type ())

G_DECLARE_DERIVABLE_TYPE (PhoshDragSurface, phosh_drag_surface, PHOSH, DRAG_SURFACE, PhoshLayerSurface)


typedef enum _PhoshDragSurfaceState {
  FOLDED,
  UNFOLDED,
  DRAGGED,
} PhoshDragSurfaceState;


/**
 * PhoshDragSurfaceClass
 * @parent_class: The parent class
 */
struct _PhoshDragSurfaceClass {
  PhoshLayerSurfaceClass parent_class;

  gint                   padding[4];
};

PhoshDragSurface     *phosh_drag_surface_new (void);
void                  phosh_drag_surface_set_margin (PhoshDragSurface *self, int margin_folded, int margin_unfolded);
void                  phosh_drag_surface_set_threshold (PhoshDragSurface *self, double threshold);
PhoshDragSurfaceState phosh_drag_surface_get_drag_state (PhoshDragSurface *self);
void                  phosh_drag_surface_set_drag_state (PhoshDragSurface *self, PhoshDragSurfaceState state);
void                  phosh_drag_surface_set_exclusive (PhoshDragSurface *self, guint exclusive);

G_END_DECLS
