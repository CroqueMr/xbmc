/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Reconstruct video at full precision, then pack the HDMI transport with frame-matched metadata. */
#include "dvbridge_render.h"
#include "dvbridge_placebo.h"
#include <libplacebo/shaders/custom.h>
#include <libplacebo/opengl.h>
#include <GLES3/gl31.h>
#include <math.h>
#include <stdlib.h>

struct dvbridge_renderer {
    pl_gpu gpu;
    pl_renderer renderer;
    pl_tex rgb;
    struct dvbridge_context *context;
    struct dvbridge_candidate *pending;
    bool ready;
    bool packed, fragment_quad;
};

struct active_mask {
    float rect[4];
    bool called, failed;
    const uint32_t *packets;
    uint32_t count, flip;
    pl_shader native_shader;
    bool early_mask;
};

static struct pl_hook_res skip_inactive_area(struct active_mask *mask,
                                             const struct pl_hook_params *p)
{
    mask->native_shader = p->sh;
    struct pl_shader_var vars[] = {
        {.var=pl_var_vec4("early_area"), .data=mask->rect, .dynamic=true},
        {.var={.name="early_words", .type=PL_VAR_UINT, .dim_v=4, .dim_m=1, .dim_a=128},
         .data=mask->packets, .dynamic=true},
        {.var=pl_var_uint("early_count"), .data=&mask->count, .dynamic=true},
        {.var=pl_var_uint("early_flip"), .data=&mask->flip, .dynamic=true}};
    struct pl_custom_shader shader = {
        .input=PL_SHADER_SIG_COLOR, .output=PL_SHADER_SIG_COLOR,
        .description="Skip reconstruction outside the DV active picture",
        .variables=vars, .num_variables=4,
        .body=
        "ivec2 ep=ivec2(gl_FragCoord.xy); if(early_flip!=0u) ep.y=2159-ep.y;\n"
        // Keep boundary pairs together: their chroma also uses the active neighbor.
        "int ex=ep.x&~1;\n"
        "if(float(ex+1)<early_area.x || float(ex)>=early_area.z || float(ep.y)<early_area.y || float(ep.y)>=early_area.w){\n"
        "uint ei=uint(ep.y*3840+ep.x), ek=ei/3072u, ev=0u;\n"
        "if(ek<early_count){uint eb=ei%1024u, ew=ek*128u+eb/8u;\n"
        "ev=(early_words[ew/4u][int(ew%4u)]>>(7u-eb%8u))&1u;}\n"
        "return vec4(128.0,16.0,float(ev*16u),255.0)/255.0;}\n"};
    mask->failed = pl_shader_is_compute(p->sh) || !pl_shader_custom(p->sh, &shader);
    return (struct pl_hook_res){.failed=mask->failed, .output=PL_HOOK_SIG_NONE};
}

/* Same arithmetic/packing as pack_gles300.frag. Subgroup exchange replaces
 * only the full-precision intermediate texture fetch of the adjacent pixel.
 * Adapted from the existing mpv_dvbridge_fused1.h implementation. */
static struct pl_hook_res pack_active_area(void *priv, const struct pl_hook_params *p)
{
    struct active_mask *mask = priv;
    if (p->stage == PL_HOOK_NATIVE)
        return skip_inactive_area(mask, p);
    mask->called = true;
    struct pl_shader_var vars[] = {
        {.var = pl_var_vec4("active_area"), .data = mask->rect, .dynamic = true},
        {.var = {.name="metadata_words", .type=PL_VAR_UINT, .dim_v=4, .dim_m=1, .dim_a=128},
         .data=mask->packets, .dynamic=true},
        {.var=pl_var_uint("packet_count"), .data=&mask->count, .dynamic=true},
        {.var=pl_var_uint("flip_y"), .data=&mask->flip, .dynamic=true}};
    struct pl_custom_shader shader = {
        .prelude="#extension GL_KHR_shader_subgroup_basic : require\n"
                 "#extension GL_KHR_shader_subgroup_quad : require\n",
        .input=PL_SHADER_SIG_COLOR, .output=PL_SHADER_SIG_COLOR,
        .description="Exact full-raster DV transport without intermediate image",
        .variables=vars, .num_variables=4,
        .body=
        "ivec2 p=ivec2(gl_FragCoord.xy); if(flip_y!=0u) p.y=2159-p.y;\n"
        "if(float(p.x)<active_area.x || float(p.x)>=active_area.z || float(p.y)<active_area.y || float(p.y)>=active_area.w) color=vec4(0,0,0,1);\n"
        "vec3 self_rgb=clamp(color.rgb,0.0,1.0);\n"
        "vec3 peer_rgb=subgroupQuadSwapHorizontal(self_rgb);\n"
        "vec3 a=(p.x&1)==0?self_rgb:peer_rgb, b=(p.x&1)==0?peer_rgb:self_rgb;\n"
        "float ya=dot(a,vec3(.2627,.6780,.0593)), yb=dot(b,vec3(.2627,.6780,.0593));\n"
        "uint y=uint(floor(256.0+3504.0*((p.x&1)==0?ya:yb)+.5));\n"
        "float cb=2048.0+3584.0*((a.b-ya)+(b.b-yb))/(2.0*1.8814);\n"
        "float cr=2048.0+3584.0*((a.r-ya)+(b.r-yb))/(2.0*1.4746);\n"
        "uint c=uint(floor(((p.x&1)==0?cb:cr)+.5));\n"
        "uint index=uint(p.y*3840+p.x), packet=index/3072u;\n"
        "if(packet<packet_count){ uint bit=index%1024u, byte_index=packet*128u+bit/8u;\n"
        "uint value=(metadata_words[byte_index/4u][int(byte_index%4u)]>>(7u-bit%8u))&1u;\n"
        "uint parity=uint(bitCount(c>>1u)+bitCount(y))&1u; c=(c&4094u)|(value^parity); }\n"
        "color=vec4(float(c>>4u),float(y>>4u),float((y&15u)|((c&15u)<<4u)),255.0)/255.0;\n"};
    // An early return must reach the transport target, never an intermediate image.
    mask->failed = mask->failed || p->stage != PL_HOOK_OUTPUT ||
                   (mask->early_mask && mask->native_shader != p->sh) || pl_shader_is_compute(p->sh) ||
                   !pl_shader_custom(p->sh, &shader);
    return (struct pl_hook_res){.failed=mask->failed, .output=PL_HOOK_SIG_NONE};
}

static struct pl_hook_res mask_active_area(void *priv, const struct pl_hook_params *p)
{
    struct active_mask *mask = priv;
    mask->called = true;
    struct pl_shader_var variable = {
        .var = pl_var_vec4("dvbridge_active"), .data = mask->rect, .dynamic = true};
    struct pl_custom_shader shader = {
        .input = PL_SHADER_SIG_COLOR, .output = PL_SHADER_SIG_COLOR,
        .body = "if(gl_FragCoord.x < dvbridge_active.x || gl_FragCoord.x >= dvbridge_active.z || "
                "gl_FragCoord.y < dvbridge_active.y || gl_FragCoord.y >= dvbridge_active.w) "
                "color = vec4(0.0, 0.0, 0.0, 1.0);",
        .variables = &variable, .num_variables = 1,
        .description = "DV active picture blanking before target overlays"};
    mask->failed = p->stage != PL_HOOK_OUTPUT || !pl_shader_custom(p->sh, &shader);
    return (struct pl_hook_res){.failed = mask->failed, .output = PL_HOOK_SIG_NONE};
}

struct dvbridge_renderer *dvbridge_renderer_create(pl_gpu gpu)
{
    if (!gpu)
        return NULL;
    struct dvbridge_renderer *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->gpu = gpu;
    // Query just the operation used here. libplacebo's global subgroup flag
    // requires arithmetic/clustered features that this fragment hook never uses.
    pl_opengl gl = pl_opengl_get(gpu);
    if (gl && pl_opengl_has_ext(gl, "GL_KHR_shader_subgroup")) {
        GLint size=0, stages=0, features=0;
        glGetIntegerv(0x9532, &size);
        glGetIntegerv(0x9533, &stages);
        glGetIntegerv(0x9534, &features);
        r->fragment_quad = size >= 4 && size <= 128 && !(size & (size-1)) &&
                           (stages & GL_FRAGMENT_SHADER_BIT) && (features & 0x81) == 0x81;
    }
    r->renderer = pl_renderer_create(NULL, gpu);
    r->context = dvbridge_create();
    pl_fmt fmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
                             PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_SAMPLEABLE);
    if (fmt)
        r->rgb = pl_tex_create(gpu, pl_tex_params(.w = 3840, .h = 2160, .format = fmt,
            .sampleable = true, .renderable = true, .blit_dst = true));
    if (!r->renderer || !r->context || !r->rgb) {
        dvbridge_renderer_destroy(r);
        return NULL;
    }
    return r;
}

void dvbridge_renderer_reset(struct dvbridge_renderer *r)
{
    if (!r)
        return;
    dvbridge_candidate_destroy(r->pending);
    r->pending = NULL;
    r->ready = false;
    r->packed = false;
    dvbridge_reset(r->context);
    if (r->renderer) {
        pl_renderer_flush_cache(r->renderer);
        pl_renderer_reset_errors(r->renderer, NULL);
    }
}

void dvbridge_renderer_destroy(struct dvbridge_renderer *r)
{
    if (!r)
        return;
    dvbridge_candidate_destroy(r->pending);
    dvbridge_destroy(r->context);
    pl_tex_destroy(r->gpu, &r->rgb);
    pl_renderer_destroy(&r->renderer);
    free(r);
}

static bool render(struct dvbridge_renderer *r, const struct pl_frame *source,
                        const void *metadata, size_t bytes, double pts, double el_pts,
                        struct dvbridge_geometry geometry, pl_tex packed, bool flip)
{
    if (!r)
        return false;
    r->ready = false;
    r->packed = packed != NULL;
    dvbridge_candidate_destroy(r->pending);
    r->pending = NULL;
    if (!source || !isfinite(pts) || source->num_planes < 1 || source->num_planes > 4 || source->rotation ||
        source->crop.x0 != 0 || source->crop.y0 != 0 ||
        source->crop.x1 != geometry.source_width || source->crop.y1 != geometry.source_height)
        return false;
    for (int p = 0; p < source->num_planes; ++p)
        if (!source->planes[p].texture)
            return false;
    const struct pl_frame *el = source->enhancement_layer;
    if (el) {
        if (el->num_planes < 1 || el->num_planes > 4 || el->rotation || el->enhancement_layer)
            return false;
        for (int p = 0; p < el->num_planes; ++p)
            if (!el->planes[p].texture)
                return false;
    }
    bool paired = source->enhancement_layer && isfinite(el_pts) && fabs(pts - el_pts) < 0.00001;
    struct dvbridge_color mapped;
    if (!dvbridge_map_color(&mapped, metadata, bytes, paired))
        return false;
    r->pending = dvbridge_prepare(r->context, metadata, bytes, pts, geometry,
                                 paired && mapped.dovi.nlq_active);
    if (!r->pending)
        return false;
    struct pl_frame image = *source;
    mapped.repr.bits = source->repr.bits;
    mapped.repr.alpha = source->repr.alpha;
    image.repr = mapped.repr;
    image.color = mapped.color;
    image.profile = (struct pl_icc_profile){0};
    image.icc = NULL;
    image.lut = NULL;
    struct pl_frame target = {
        .num_planes = 1,
        .planes = {{.texture = packed ? packed : r->rgb, .flipped = packed && flip,
                    .components = 4, .component_mapping = {0, 1, 2, 3}}},
        .repr = pl_color_repr_rgb,
        .color = mapped.color,
        .crop = {geometry.x, geometry.y, geometry.x + geometry.width, geometry.y + geometry.height},
    };
    struct pl_render_params params = pl_render_default_params;
    params.min_fbo_precision = 32;
    params.peak_detect_params = NULL;
    params.dither_params = NULL;
    params.frame_mixer = NULL;
    params.skip_caching_single_frame = true;
    // Own exact PQ-zero borders below. Do not translate an sRGB clear color
    // through the output transfer function or clear a full-raster frame twice.
    params.border = PL_CLEAR_SKIP;
    unsigned margins[4];
    if (!dvbridge_active_area(r->pending, margins))
        return false;
    struct active_mask mask = {.rect = {margins[0], margins[2],
                                       3840 - margins[1], 2160 - margins[3]}};
    mask.flip = flip;
    mask.packets = dvbridge_packets(r->pending, &mask.count);
    mask.early_mask = packed && geometry.source_width == 3840 && geometry.source_height == 2160 &&
                      (margins[0] || margins[1] || margins[2] || margins[3]);
    const struct pl_hook hook = {
        .stages = PL_HOOK_OUTPUT | (mask.early_mask ? PL_HOOK_NATIVE : 0),
        .input = PL_HOOK_SIG_COLOR,
        .priv = &mask, .hook = packed ? pack_active_area : mask_active_area,
        .signature = packed ? 0x4456425246555302ULL : 0x445642524c350001ULL};
    const struct pl_hook *hooks[] = {&hook};
    params.hooks = hooks;
    params.num_hooks = 1;
    if (!packed && (geometry.x || geometry.y || geometry.width != 3840 || geometry.height != 2160))
        pl_tex_clear(r->gpu, r->rgb, (float[4]){0, 0, 0, 1});
    r->ready = pl_render_image(r->renderer, &image, &target, &params) && mask.called && !mask.failed;
    /* An EL sampling failure can otherwise return a successfully rendered BL-only frame. */
    struct pl_render_errors errors = pl_renderer_get_errors(r->renderer);
    r->ready = r->ready && !errors.errors && !errors.num_disabled_hooks;
    if (!r->ready) {
        dvbridge_candidate_destroy(r->pending);
        r->pending = NULL;
    }
    return r->ready;
}

bool dvbridge_render_rgb(struct dvbridge_renderer *r, const struct pl_frame *source,
                        const void *metadata, size_t bytes, double pts, double el_pts,
                        struct dvbridge_geometry geometry)
{
    return render(r, source, metadata, bytes, pts, el_pts, geometry, NULL, false);
}

bool dvbridge_render_packed(struct dvbridge_renderer *r, const struct pl_frame *source,
                           const void *metadata, size_t bytes, double pts, double el_pts,
                           struct dvbridge_geometry geometry, pl_tex target, bool flip)
{
    if (r)
        r->ready = false;
    if (!r || !source || !target || !r->fragment_quad ||
        target->params.w != 3840 || target->params.h != 2160 ||
        !target->params.renderable || target->params.format->type != PL_FMT_UNORM ||
        target->params.format->num_components != 4 ||
        geometry.x || geometry.y || geometry.width != 3840 || geometry.height != 2160 ||
        source->num_overlays || (source->enhancement_layer && source->enhancement_layer->num_overlays))
        return false;
    for (int c = 0; c < 4; ++c)
        if (target->params.format->component_depth[c] != 8)
            return false;
    // Transport bytes must not inherit GUI blending, dithering or write masks.
    // libplacebo owns viewport/scissor while rendering; preserve the caller's
    // enable state, just as the separate transport packer does.
    if (glGetError() != GL_NO_ERROR)
        return false;
    const GLenum caps[] = {GL_BLEND, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_SCISSOR_TEST,
        GL_DITHER, GL_CULL_FACE, GL_RASTERIZER_DISCARD, GL_SAMPLE_ALPHA_TO_COVERAGE,
        GL_SAMPLE_COVERAGE};
    GLboolean enabled[sizeof(caps)/sizeof(*caps)], mask[4];
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    for (unsigned i=0; i<sizeof(caps)/sizeof(*caps); ++i) {
        enabled[i] = glIsEnabled(caps[i]);
        glDisable(caps[i]);
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    bool ok = render(r, source, metadata, bytes, pts, el_pts, geometry, target, flip);
    ok = glGetError() == GL_NO_ERROR && ok;
    glColorMask(mask[0], mask[1], mask[2], mask[3]);
    for (unsigned i=0; i<sizeof(caps)/sizeof(*caps); ++i) {
        if (enabled[i]) glEnable(caps[i]);
        else glDisable(caps[i]);
    }
    r->ready = ok && glGetError() == GL_NO_ERROR;
    return r->ready;
}

pl_tex dvbridge_render_texture(const struct dvbridge_renderer *r)
{
    return r && r->ready && !r->packed ? r->rgb : NULL;
}

const struct dvbridge_candidate *dvbridge_render_candidate(const struct dvbridge_renderer *r)
{
    return r && r->ready ? r->pending : NULL;
}

bool dvbridge_render_commit(struct dvbridge_renderer *r)
{
    return r && r->ready && r->pending && dvbridge_commit(r->context, r->pending);
}
