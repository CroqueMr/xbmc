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

/* Output owns its mapping; do not copy it without rebinding repr.dovi. */
bool dvbridge_map_color(struct dvbridge_color *out, const void *metadata,
                       size_t bytes, bool enhancement_layer_paired);
#endif
