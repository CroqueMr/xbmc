/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DVBRIDGE_PLACEBO_H
#define DVBRIDGE_PLACEBO_H
#include <stdbool.h>
#include <stddef.h>
#include <libplacebo/colorspace.h>

struct dvbridge_color {
    struct pl_color_space color;
    struct pl_color_repr repr;
    struct pl_dovi_metadata dovi;
};

struct dvbridge_hdr10_metadata {
    bool level6;
    unsigned max_luminance, min_luminance, max_cll, max_fall;
};
bool dvbridge_get_hdr10_metadata(struct dvbridge_hdr10_metadata *out,
                                const void *metadata, size_t bytes);
/* Build a static PQ target in nits; dynamic L1 remains on the source only. */
bool dvbridge_hdr10_target(struct pl_color_space *out, const void *metadata, size_t bytes);

/* Output owns its mapping; do not copy it without rebinding repr.dovi. */
bool dvbridge_map_color(struct dvbridge_color *out, const void *metadata,
                       size_t bytes, bool enhancement_layer_paired);
#endif
