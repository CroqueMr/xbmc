/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Split and hardware-decode the enhancement layer; pair surfaces by exact presentation time. */
#include "dvbridge_fel.h"
#include <libavcodec/bsf.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <stdlib.h>
#include <string.h>

#define QUEUE_MAX 32
#define PACKET_BYTES_MAX (64u * 1024u * 1024u)

struct dvbridge_fel {
    AVBSFContext *split;
    AVCodecContext *decoder;
    AVPacket *packets[QUEUE_MAX];
    AVFrame *frames[QUEUE_MAX];
    unsigned num_packets, num_frames;
    size_t packet_bytes;
    bool opened, failed, draining, sent_eof, eof;
};

static enum AVPixelFormat hardware_format(AVCodecContext *ctx, const enum AVPixelFormat *formats)
{
    bool found = false;
    for (; *formats != AV_PIX_FMT_NONE; ++formats)
        found |= *formats == AV_PIX_FMT_VAAPI;
    if (!found || !ctx->hw_device_ctx)
        return AV_PIX_FMT_NONE;
    AVBufferRef *frames = NULL;
    if (avcodec_get_hw_frames_parameters(ctx, ctx->hw_device_ctx, AV_PIX_FMT_VAAPI, &frames) < 0)
        return AV_PIX_FMT_NONE;
    AVHWFramesContext *hw = (void *)frames->data;
    hw->initial_pool_size += QUEUE_MAX + 4;
    if (av_hwframe_ctx_init(frames) < 0) {
        av_buffer_unref(&frames);
        return AV_PIX_FMT_NONE;
    }
    av_buffer_unref(&ctx->hw_frames_ctx);
    ctx->hw_frames_ctx = frames;
    return AV_PIX_FMT_VAAPI;
}

void dvbridge_fel_reset(struct dvbridge_fel *f)
{
    if (!f)
        return;
    for (unsigned i = 0; i < f->num_packets; ++i)
        av_packet_free(&f->packets[i]);
    for (unsigned i = 0; i < f->num_frames; ++i)
        av_frame_free(&f->frames[i]);
    f->num_packets = f->num_frames = 0;
    f->packet_bytes = 0;
    f->failed = f->draining = f->sent_eof = f->eof = false;
    if (f->split)
        av_bsf_flush(f->split);
    if (f->opened)
        avcodec_flush_buffers(f->decoder);
}

void dvbridge_fel_destroy(struct dvbridge_fel *f)
{
    if (!f)
        return;
    dvbridge_fel_reset(f);
    avcodec_free_context(&f->decoder);
    av_bsf_free(&f->split);
    free(f);
}

struct dvbridge_fel *dvbridge_fel_create(const AVCodecParameters *p, AVRational time_base)
{
    if (!p || p->codec_id != AV_CODEC_ID_HEVC || time_base.num <= 0 || time_base.den <= 0)
        return NULL;
    const AVBitStreamFilter *split = av_bsf_get_by_name("dovi_split");
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    if (!split || !codec)
        return NULL;
    struct dvbridge_fel *f = calloc(1, sizeof(*f));
    if (!f)
        return NULL;
    if (av_bsf_alloc(split, &f->split) < 0 ||
        avcodec_parameters_copy(f->split->par_in, p) < 0)
        goto fail;
    f->split->time_base_in = time_base;
    if (av_opt_set(f->split->priv_data, "mode", "el", 0) < 0 || av_bsf_init(f->split) < 0)
        goto fail;
    f->decoder = avcodec_alloc_context3(codec);
    if (!f->decoder || avcodec_parameters_to_context(f->decoder, f->split->par_out) < 0)
        goto fail;
    f->decoder->pkt_timebase = time_base;
    f->decoder->get_format = hardware_format;
    f->decoder->thread_count = 1;
    return f;
fail:
    dvbridge_fel_destroy(f);
    return NULL;
}

static bool receive_frames(struct dvbridge_fel *f)
{
    while (f->num_frames < QUEUE_MAX) {
        AVFrame *frame = av_frame_alloc();
        if (!frame)
            return false;
        int result = avcodec_receive_frame(f->decoder, frame);
        if (result < 0) {
            av_frame_free(&frame);
            if (result == AVERROR_EOF)
                f->eof = true;
            return result == AVERROR(EAGAIN) || result == AVERROR_EOF;
        }
        /* Preserve the packet's presentation identity. FFmpeg's best-effort
         * timestamp can substitute DTS when EL has different reordering from
         * BL; that estimate must not replace a valid exact pairing key. */
        if (frame->pts == AV_NOPTS_VALUE)
            frame->pts = frame->best_effort_timestamp;
        if (frame->format != AV_PIX_FMT_VAAPI || frame->pts == AV_NOPTS_VALUE) {
            av_frame_free(&frame);
            return false;
        }
        for (unsigned i = 0; i < f->num_frames; ++i) {
            if (f->frames[i]->pts == frame->pts) {
                av_frame_free(&frame);
                return false;
            }
        }
        f->frames[f->num_frames++] = frame;
    }
    return true;
}

static bool pump(struct dvbridge_fel *f)
{
    if (f->failed)
        return false;
    if (!f->opened)
        return true;
    if (!receive_frames(f))
        goto fail;
    while (f->num_packets && f->num_frames < QUEUE_MAX) {
        AVPacket *packet = f->packets[0];
        int result = avcodec_send_packet(f->decoder, packet);
        if (result == AVERROR(EAGAIN))
            break;
        if (result < 0)
            goto fail;
        f->packet_bytes -= packet->size;
        av_packet_free(&packet);
        --f->num_packets;
        memmove(f->packets, f->packets + 1, f->num_packets * sizeof(*f->packets));
        if (!receive_frames(f))
            goto fail;
    }
    if (f->draining && !f->num_packets && !f->sent_eof && f->num_frames < QUEUE_MAX) {
        int result = avcodec_send_packet(f->decoder, NULL);
        if (result != AVERROR(EAGAIN)) {
            if (result < 0 && result != AVERROR_EOF)
                goto fail;
            f->sent_eof = true;
        }
        if (!receive_frames(f))
            goto fail;
    }
    return true;
fail:
    f->failed = true;
    return false;
}

bool dvbridge_fel_device(struct dvbridge_fel *f, AVBufferRef *device)
{
    if (!f || !device || ((AVHWDeviceContext *)device->data)->type != AV_HWDEVICE_TYPE_VAAPI)
        return false;
    if (f->opened)
        return f->decoder->hw_device_ctx->data == device->data && pump(f);
    f->decoder->hw_device_ctx = av_buffer_ref(device);
    if (!f->decoder->hw_device_ctx || avcodec_open2(f->decoder, f->decoder->codec, NULL) < 0) {
        f->failed = true;
        return false;
    }
    f->opened = true;
    return pump(f);
}

bool dvbridge_fel_submit(struct dvbridge_fel *f, const AVPacket *packet)
{
    if (!f || !packet || f->failed || f->draining || packet->size <= 0 ||
        packet->pts == AV_NOPTS_VALUE)
        return false;
    AVPacket *copy = av_packet_clone(packet);
    if (!copy)
        goto fail;
    int result = av_bsf_send_packet(f->split, copy);
    av_packet_free(&copy);
    if (result < 0)
        goto fail;
    while (true) {
        copy = av_packet_alloc();
        if (!copy)
            goto fail;
        result = av_bsf_receive_packet(f->split, copy);
        if (result < 0) {
            av_packet_free(&copy);
            if (result == AVERROR(EAGAIN))
                return pump(f);
            goto fail;
        }
        if (f->num_packets == QUEUE_MAX || copy->size <= 0 ||
            (size_t)copy->size > PACKET_BYTES_MAX - f->packet_bytes) {
            av_packet_free(&copy);
            goto fail;
        }
        f->packet_bytes += copy->size;
        f->packets[f->num_packets++] = copy;
    }
fail:
    f->failed = true;
    return false;
}

bool dvbridge_fel_drain(struct dvbridge_fel *f)
{
    if (!f)
        return false;
    f->draining = true;
    return pump(f);
}

int dvbridge_fel_take(struct dvbridge_fel *f, int64_t pts, AVFrame **frame)
{
    if (!f || !frame || *frame || pts == AV_NOPTS_VALUE || !pump(f))
        return -1;
    for (unsigned i = 0; i < f->num_frames;) {
        AVFrame *candidate = f->frames[i];
        if (candidate->pts > pts) {
            ++i;
            continue;
        }
        --f->num_frames;
        memmove(f->frames + i, f->frames + i + 1,
                (f->num_frames - i) * sizeof(*f->frames));
        if (candidate->pts == pts) {
            *frame = candidate;
            return 1;
        }
        av_frame_free(&candidate);
    }
    /* A later decoded EL frame does not prove the requested one is missing.
     * Keep bounded lookahead and never substitute a neighbouring surface. */
    return f->eof || f->num_frames == QUEUE_MAX ? -1 : 0;
}

bool dvbridge_fel_failed(const struct dvbridge_fel *f)
{
    return !f || f->failed;
}
