/*
 * Copyright (C) 2021 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Author: Guido Günther <agx@sigxcpu.org>
 */

#define G_LOG_DOMAIN "phosh-drag-surface"

#include "config.h"

#include "phosh-enums.h"
#include "drag-surface.h"

/**
 * SECTION:drag-surface
 * @short_description: A drgable layer surface
 * @Title: PhoshDragSurface
 *
 * A layer surface that can be dragged in ne direction via gestreus.
 */

/* TODO: override properties of layersurface that shouldn't be set directly */
/* (exclusive_zone, margins) */

enum {
  PROP_0,
  PROP_LAYER_SHELL_EFFECTS,
  PROP_MARGIN_FOLDED,
  PROP_MARGIN_UNFOLDED,
  PROP_THRESHOLD,
  PROP_DRAG_STATE,
  PROP_LAST_PROP
};
static GParamSpec *props[PROP_LAST_PROP];


enum {
  SIGNAL_DRAGGED,
  N_SIGNALS
};
static guint signals[N_SIGNALS] = { 0 };


typedef struct _PhoshDragSurfacePrivate {
  struct zphoc_layer_shell_effects_v1    *layer_shell_effects;
  struct zphoc_dragable_layer_surface_v1 *drag_surface;

  int                                     margin_folded;
  int                                     margin_unfolded;
  double                                  threshold;
  PhoshDragSurfaceState                   drag_state;

} PhoshDragSurfacePrivate;

G_DEFINE_TYPE_WITH_PRIVATE (PhoshDragSurface, phosh_drag_surface, PHOSH_TYPE_LAYER_SURFACE)


static void
phosh_drag_surface_set_property (GObject      *object,
                                 guint         property_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  PhoshDragSurface *self = PHOSH_DRAG_SURFACE (object);
  PhoshDragSurfacePrivate *priv = phosh_drag_surface_get_instance_private (self);

  switch (property_id) {
  case PROP_LAYER_SHELL_EFFECTS:
    priv->layer_shell_effects = g_value_get_pointer (value);
    break;
  case PROP_MARGIN_FOLDED:
    phosh_drag_surface_set_margin (self, g_value_get_double (value), priv->margin_unfolded);
    break;
  case PROP_MARGIN_UNFOLDED:
    phosh_drag_surface_set_margin (self, priv->margin_folded, g_value_get_double (value));
    break;
  case PROP_THRESHOLD:
    phosh_drag_surface_set_threshold (self, g_value_get_double (value));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}


static void
phosh_drag_surface_get_property (GObject    *object,
                                 guint       property_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  PhoshDragSurface *self = PHOSH_DRAG_SURFACE (object);
  PhoshDragSurfacePrivate *priv = phosh_drag_surface_get_instance_private (self);

  switch (property_id) {
  case PROP_LAYER_SHELL_EFFECTS:
    g_value_set_pointer (value, priv->layer_shell_effects);
    break;
  case PROP_MARGIN_FOLDED:
    g_value_set_int (value, priv->margin_folded);
    break;
  case PROP_MARGIN_UNFOLDED:
    g_value_set_int (value, priv->margin_unfolded);
    break;
  case PROP_THRESHOLD:
    g_value_set_double (value, priv->threshold);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}


static void
drag_surface_handle_drag_end (void                                   *data,
                              struct zphoc_dragable_layer_surface_v1 *drag_surface_,
                              uint32_t                                state)
{
  PhoshDragSurface *self = PHOSH_DRAG_SURFACE (data);
  PhoshDragSurfacePrivate *priv;

  g_return_if_fail (PHOSH_IS_DRAG_SURFACE (self));

  priv = phosh_drag_surface_get_instance_private (self);

  if (state == priv->drag_state)
    return;

  priv->drag_state = state;
  g_debug ("DragSurface %p: state, %d", self, priv->drag_state);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_DRAG_STATE]);
}


static void
drag_surface_handle_dragged (void                                   *data,
                             struct zphoc_dragable_layer_surface_v1 *drag_surface_,
                             int                                     margin)
{
  PhoshDragSurface *self = PHOSH_DRAG_SURFACE (data);
  PhoshDragSurfacePrivate *priv;

  g_return_if_fail (PHOSH_IS_DRAG_SURFACE (self));

  priv = phosh_drag_surface_get_instance_private (self);

  g_signal_emit (self, signals[SIGNAL_DRAGGED], 0, margin);

  if (priv->drag_state == DRAGGED)
    return;

  priv->drag_state = DRAGGED;
  g_debug ("DragSurface %p: state, %d", self, priv->drag_state);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_DRAG_STATE]);
}


const struct zphoc_dragable_layer_surface_v1_listener drag_surface_listener = {
  .drag_end = drag_surface_handle_drag_end,
  .dragged = drag_surface_handle_dragged,
};


static void
phosh_drag_surface_configured (PhoshLayerSurface *layer_surface)
{
  PhoshDragSurface *self = PHOSH_DRAG_SURFACE (layer_surface);
  PhoshLayerSurfaceClass *parent_class = PHOSH_LAYER_SURFACE_CLASS (phosh_drag_surface_parent_class);
  PhoshDragSurfacePrivate *priv = phosh_drag_surface_get_instance_private (self);
  struct zwlr_layer_surface_v1 *wl_layer_surface = phosh_layer_surface_get_layer_surface (layer_surface);

  if (parent_class->configured)
    parent_class->configured (layer_surface);

  if (priv->drag_surface)
    return;

  /* Configure drag surface if not done yet */
  /* FIXME: can we do that earlier ? */
  priv->drag_surface = zphoc_layer_shell_effects_v1_get_dragable_layer_surface (priv->layer_shell_effects,
                                                                                wl_layer_surface);
  zphoc_dragable_layer_surface_v1_add_listener (priv->drag_surface, &drag_surface_listener, self);

  phosh_drag_surface_set_margin (self, priv->margin_folded, priv->margin_unfolded);
  phosh_drag_surface_set_threshold (self, priv->threshold);
}


static void
phosh_drag_surface_dispose (GObject *object)
{
  PhoshDragSurface *self = PHOSH_DRAG_SURFACE (object);
  PhoshDragSurfacePrivate *priv = phosh_drag_surface_get_instance_private (self);

  g_clear_pointer (&priv->drag_surface, zphoc_dragable_layer_surface_v1_destroy);

  G_OBJECT_CLASS (phosh_drag_surface_parent_class)->dispose (object);
}


static void
phosh_drag_surface_class_init (PhoshDragSurfaceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  PhoshLayerSurfaceClass *layer_surface_class = PHOSH_LAYER_SURFACE_CLASS (klass);

  object_class->get_property = phosh_drag_surface_get_property;
  object_class->set_property = phosh_drag_surface_set_property;
  object_class->dispose = phosh_drag_surface_dispose;

  layer_surface_class->configured = phosh_drag_surface_configured;

  props[PROP_LAYER_SHELL_EFFECTS] = g_param_spec_pointer ("layer-shell-effects",
                                                          "",
                                                          "",
                                                          G_PARAM_READWRITE |
                                                          G_PARAM_CONSTRUCT_ONLY |
                                                          G_PARAM_STATIC_STRINGS);

  props[PROP_MARGIN_FOLDED] = g_param_spec_int ("margin-folded",
                                                "",
                                                "",
                                                G_MININT,
                                                G_MAXINT,
                                                0,
                                                G_PARAM_READWRITE |
                                                G_PARAM_STATIC_STRINGS |
                                                G_PARAM_EXPLICIT_NOTIFY);

  props[PROP_MARGIN_UNFOLDED] = g_param_spec_int ("margin-unfolded",
                                                  "",
                                                  "",
                                                  G_MININT,
                                                  G_MAXINT,
                                                  0,
                                                  G_PARAM_READWRITE |
                                                  G_PARAM_STATIC_STRINGS |
                                                  G_PARAM_EXPLICIT_NOTIFY);

  props[PROP_THRESHOLD] = g_param_spec_double ("threshold",
                                               "",
                                               "",
                                               G_MINDOUBLE,
                                               G_MAXDOUBLE,
                                               1.0,
                                               G_PARAM_READWRITE |
                                               G_PARAM_STATIC_STRINGS |
                                               G_PARAM_EXPLICIT_NOTIFY);

  props[PROP_DRAG_STATE] = g_param_spec_enum ("drag-state",
                                              "",
                                              "",
                                              PHOSH_TYPE_DRAG_SURFACE_STATE,
                                              0,
                                              G_PARAM_READABLE |
                                              G_PARAM_STATIC_STRINGS |
                                              G_PARAM_EXPLICIT_NOTIFY);

  g_object_class_install_properties (object_class, PROP_LAST_PROP, props);

  signals[SIGNAL_DRAGGED] = g_signal_new (
    "dragged",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
    NULL, G_TYPE_NONE, 1, G_TYPE_INT);
}


static void
phosh_drag_surface_init (PhoshDragSurface *self)
{
}


PhoshDragSurface *
phosh_drag_surface_new (void)
{
  return PHOSH_DRAG_SURFACE (g_object_new (PHOSH_TYPE_DRAG_SURFACE, NULL));
}

void
phosh_drag_surface_set_margin (PhoshDragSurface *self, int margin_folded, int margin_unfolded)
{
  PhoshDragSurfacePrivate *priv;
  gboolean changed = FALSE;

  g_return_if_fail (PHOSH_IS_DRAG_SURFACE (self));

  priv = phosh_drag_surface_get_instance_private (self);

  if (priv->margin_folded != margin_folded) {
    priv->margin_folded = margin_folded;
    changed = TRUE;
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_MARGIN_FOLDED]);
  }

  if (priv->margin_unfolded != margin_unfolded) {
    priv->margin_folded = margin_folded;
    changed = TRUE;
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_MARGIN_UNFOLDED]);
  }

  if (changed && priv->drag_surface) {
    zphoc_dragable_layer_surface_v1_set_margins (priv->drag_surface,
                                                 priv->margin_folded,
                                                 priv->margin_unfolded);
  }

}


void
phosh_drag_surface_set_threshold (PhoshDragSurface *self, double threshold)
{
  PhoshDragSurfacePrivate *priv;

  g_return_if_fail (PHOSH_IS_DRAG_SURFACE (self));

  priv = phosh_drag_surface_get_instance_private (self);

  if (G_APPROX_VALUE (priv->threshold, threshold, FLT_EPSILON) == FALSE &&
      priv->drag_surface) {
    priv->threshold = threshold;
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_THRESHOLD]);
    zphoc_dragable_layer_surface_v1_set_threshold (priv->drag_surface,
                                                   wl_fixed_from_double (priv->threshold));
  }
}


PhoshDragSurfaceState
phosh_drag_surface_get_drag_state (PhoshDragSurface *self)
{
  PhoshDragSurfacePrivate *priv;

  g_return_val_if_fail (PHOSH_IS_DRAG_SURFACE (self), 0);
  priv = phosh_drag_surface_get_instance_private (self);

  return priv->drag_state;
}


void
phosh_drag_surface_set_drag_state (PhoshDragSurface     *self,
                                   PhoshDragSurfaceState state)
{
  PhoshDragSurfacePrivate *priv;

  g_return_if_fail (state >= FOLDED && state <= UNFOLDED);
  g_return_if_fail (PHOSH_IS_DRAG_SURFACE (self));
  priv = phosh_drag_surface_get_instance_private (self);

  zphoc_dragable_layer_surface_v1_set_state (priv->drag_surface, state);

}


void
phosh_drag_surface_set_exclusive (PhoshDragSurface *self, guint exclusive)

{
  PhoshDragSurfacePrivate *priv;

  g_return_if_fail (PHOSH_IS_DRAG_SURFACE (self));
  priv = phosh_drag_surface_get_instance_private (self);

  zphoc_dragable_layer_surface_v1_set_exclusive (priv->drag_surface, exclusive);

}
