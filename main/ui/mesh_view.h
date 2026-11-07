/*
 * Bed mesh viewer: a rotatable 3D surface (like Mainsail's heightmap) or a
 * top-down grid with values, drawn on one canvas with a colour scale.
 * Drag to rotate / tilt (3D), tap a point to read its X / Y / Z.
 */
#pragma once
#include "lvgl.h"
#include "machine.h"

/* Creates the view (toolbar + canvas) filling the width of `parent`. */
lv_obj_t *mesh_view_create(lv_obj_t *parent, int height);
/* New data; redraws only when the mesh actually changed. */
void      mesh_view_set(lv_obj_t *view, const mesh_t *m);
