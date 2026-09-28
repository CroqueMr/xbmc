/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Validate parsed Dolby Vision metadata before handing reshaping parameters to libplacebo. */
#include "dvbridge_placebo.h"
#include "dvbridge_metadata.h"
#include <libplacebo/utils/libav.h>
#include <math.h>

bool dvbridge_hdr10_target(struct pl_color_space *out, const void *metadata, size_t bytes)
{
    if (!out || !metadata || (uintptr_t)metadata % _Alignof(AVDOVIMetadata) ||
        !dvbridge_dv_bounds(metadata, bytes))
        return false;
    const AVDOVIColorMetadata *color = av_dovi_get_color(metadata);
    const AVDOVIDmData *l6 = av_dovi_find_level(metadata, 6);
    float peak = l6 && l6->l6.max_luminance > 0 && l6->l6.max_luminance <= 10000
        ? l6->l6.max_luminance : 0;
    if (!peak && color->source_max_pq > 0 && color->source_max_pq <= 4095)
        peak = pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS, color->source_max_pq / 4095.0f);
    if (!isfinite(peak) || peak < 1 || peak > 10000)
        return false;
    *out = (struct pl_color_space){
        .primaries = PL_COLOR_PRIM_BT_2020, .transfer = PL_COLOR_TRC_PQ,
        .hdr = {.min_luma = PL_COLOR_HDR_BLACK, .max_luma = peak},
    };
    return true;
}

bool dvbridge_get_hdr10_metadata(struct dvbridge_hdr10_metadata *out,
                                const void *metadata, size_t bytes)
{
    if (!out || !metadata || (uintptr_t)metadata % _Alignof(AVDOVIMetadata) ||
        !dvbridge_dv_bounds(metadata, bytes))
        return false;
    *out = (struct dvbridge_hdr10_metadata){0};
    struct pl_color_space target;
    if (!dvbridge_hdr10_target(&target, metadata, bytes))
        return false;
    out->max_luminance = (unsigned)ceilf(target.hdr.max_luma);
    const AVDOVIDmData *l6 = av_dovi_find_level(metadata, 6);
    if (l6 && l6->l6.max_luminance > 0 && l6->l6.max_luminance <= 10000) {
        out->level6 = true;
    }
    /* Source CLL/FALL do not measure the tone-mapped output. Zero means unknown. */
    return true;
}

bool dvbridge_map_color(struct dvbridge_color *out, const void *metadata,
                       size_t bytes, bool enhancement_layer_paired)
{
    if (!out || !metadata || (uintptr_t)metadata % _Alignof(AVDOVIMetadata))
        return false;
    const AVDOVIMetadata *m = metadata;
    if (!dvbridge_dv_bounds(m, bytes))
        return false;
    const AVDOVIRpuDataHeader *h = av_dovi_get_header(m);
    const AVDOVIDataMapping *map = av_dovi_get_mapping(m);
    const AVDOVIColorMetadata *color = av_dovi_get_color(m);
    if (h->bl_bit_depth < 8 || h->bl_bit_depth > 16 || h->coef_log2_denom > 30)
        return false;
    if (!h->disable_residual_flag &&
        (h->el_bit_depth < 8 || h->el_bit_depth > 16 ||
         map->nlq_method_idc != AV_DOVI_NLQ_LINEAR_DZ))
        return false;
    for (int c = 0; c < 3; ++c) {
        const AVDOVIReshapingCurve *curve = &map->curves[c];
        if (curve->num_pivots < 2 || curve->num_pivots > 9)
            return false;
        if (!color->ycc_to_rgb_offset[c].den)
            return false;
        for (int p = 0; p < curve->num_pivots; ++p) {
            if (curve->pivots[p] >= (1u << h->bl_bit_depth) ||
                (p && curve->pivots[p] <= curve->pivots[p - 1]))
                return false;
        }
        for (int p = 0; p + 1 < curve->num_pivots; ++p) {
            if (curve->mapping_idc[p] == AV_DOVI_MAPPING_POLYNOMIAL) {
                if (curve->poly_order[p] > 2)
                    return false;
            } else if (curve->mapping_idc[p] == AV_DOVI_MAPPING_MMR) {
                if (curve->mmr_order[p] < 1 || curve->mmr_order[p] > 3)
                    return false;
            } else {
                return false;
            }
        }
    }
    for (int i = 0; i < 9; ++i)
        if (!color->ycc_to_rgb_matrix[i].den || !color->rgb_to_lms_matrix[i].den)
            return false;
    struct dvbridge_color candidate = {0};
    pl_map_avdovi_metadata(&candidate.color, &candidate.repr, &candidate.dovi, m);
    if (candidate.dovi.nlq_active && !enhancement_layer_paired)
        return false;
    *out = candidate;
    out->repr.dovi = &out->dovi;
    return true;
}
