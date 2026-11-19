/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DVBRIDGE_RENDER_H
#define DVBRIDGE_RENDER_H
#include <libplacebo/renderer.h>
#include "dvbridge_core.h"

struct dvbridge_renderer;
struct dvbridge_renderer *dvbridge_renderer_create(pl_gpu gpu);
void dvbridge_renderer_destroy(struct dvbridge_renderer *renderer);
void dvbridge_renderer_reset(struct dvbridge_renderer *renderer);

/* Produces an intermediate PQ RGB image, not a scanout-ready DV frame. */
bool dvbridge_render_rgb(struct dvbridge_renderer *renderer, const struct pl_frame *source,
                        const void *metadata, size_t bytes, double pts, double el_pts,
                        struct dvbridge_geometry geometry);
/* Apply L1-guided tone mapping to the source mastering target before overlays. */
bool dvbridge_render_hdr10_rgb(struct dvbridge_renderer *renderer, const struct pl_frame *source,
                              const void *metadata, size_t bytes, double pts, double el_pts,
                              struct dvbridge_geometry geometry);
/* Optional full-raster, overlay-free fast path. The caller owns an exact RGBA8
 * linear target; all other layouts keep the separate full-precision path. */
bool dvbridge_render_packed(struct dvbridge_renderer *renderer, const struct pl_frame *source,
                           const void *metadata, size_t bytes, double pts, double el_pts,
                           struct dvbridge_geometry geometry, pl_tex target, bool flip_y);
pl_tex dvbridge_render_texture(const struct dvbridge_renderer *renderer);
/* Quantize the composed PQ image without applying tone mapping again. */
bool dvbridge_render_hdr10(struct dvbridge_renderer *renderer, pl_tex target,
                          bool flip_y, bool limited, unsigned bits);
const struct dvbridge_candidate *dvbridge_render_candidate(const struct dvbridge_renderer *renderer);
bool dvbridge_render_commit(struct dvbridge_renderer *renderer);
#endif
