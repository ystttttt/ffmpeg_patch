/*
 * Copyright (C) 2007 Richard Spindler (author of frei0r plugin from which this was derived)
 * Copyright (C) 2014 Daniel Oberhoff
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * Lenscorrection filter, algorithm from the frei0r plugin with the same name
*/
#include <stdlib.h>
#include <math.h>

#include "libavutil/colorspace.h"
#include "libavutil/opt.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/pixdesc.h"

#include "avfilter.h"
#include "drawutils.h"
#include "internal.h"
#include "video.h"

typedef struct LenscorrectionCtx {
    const AVClass *av_class;
    int width;
    int height;
    int hsub, vsub;
    int depth;
    int nb_planes;
    double cx, cy, k1, k2;
    int interpolation;
    uint8_t fill_rgba[4];
    int fill_color[4];

    int32_t *correction[4];

    int (*filter_slice)(AVFilterContext *ctx, void *arg, int job, int nb_jobs);
} LenscorrectionCtx;

#define OFFSET(x) offsetof(LenscorrectionCtx, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM
static const AVOption lenscorrection_options[] = {
    { "cx", "set relative center x", OFFSET(cx), AV_OPT_TYPE_DOUBLE, {.dbl=0.5}, 0, 1, .flags=FLAGS },
    { "cy", "set relative center y", OFFSET(cy), AV_OPT_TYPE_DOUBLE, {.dbl=0.5}, 0, 1, .flags=FLAGS },
    { "k1", "set quadratic distortion factor", OFFSET(k1), AV_OPT_TYPE_DOUBLE, {.dbl=0.0}, -1, 1, .flags=FLAGS },
    { "k2", "set double quadratic distortion factor", OFFSET(k2), AV_OPT_TYPE_DOUBLE, {.dbl=0.0}, -1, 1, .flags=FLAGS },
    { "i",  "set interpolation type", OFFSET(interpolation), AV_OPT_TYPE_INT, {.i64=0}, 0, 64, .flags=FLAGS, "i" },
    {  "nearest",  "nearest neighbour", 0,                   AV_OPT_TYPE_CONST, {.i64=0},0, 0, .flags=FLAGS, "i" },
    {  "bilinear", "bilinear",          0,                   AV_OPT_TYPE_CONST, {.i64=1},0, 0, .flags=FLAGS, "i" },
    { "fc", "set the color of the unmapped pixels", OFFSET(fill_rgba), AV_OPT_TYPE_COLOR, {.str="black@0"}, .flags = FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(lenscorrection);

typedef struct ThreadData {
    AVFrame *in, *out;
    int w, h;
    int depth;
    int plane;
    int fill_color;
    int xcenter, ycenter;
    int32_t *correction;
} ThreadData;

#define NEAREST(type, name)                                                    \
static int filter##name##_slice(AVFilterContext *ctx, void *arg, int job,      \
                                int nb_jobs)                                   \
{                                                                              \
    ThreadData *td = arg;                                                      \
    AVFrame *in = td->in;                                                      \
    AVFrame *out = td->out;                                                    \
                                                                               \
    const int fill_color = td->fill_color;                                     \
    const int w = td->w, h = td->h;                                            \
    const int xcenter = td->xcenter;                                           \
    const int ycenter = td->ycenter;                                           \
    const int start = (h *  job   ) / nb_jobs;                                 \
    const int end   = (h * (job+1)) / nb_jobs;                                 \
    const int plane = td->plane;                                               \
    const int inlinesize = in->linesize[plane] / sizeof(type);                 \
    const int outlinesize = out->linesize[plane] / sizeof(type);               \
    const type *indata = (const type *)in->data[plane];                        \
    type *outrow = (type *)out->data[plane] + start * outlinesize;             \
    for (int i = start; i < end; i++, outrow += outlinesize) {                 \
        const int off_y = i - ycenter;                                         \
        type *out = outrow;                                                    \
        for (int j = 0; j < w; j++) {                                          \
            const int off_x = j - xcenter;                                     \
            const int64_t radius_mult = td->correction[j + i*w];               \
            const int x = xcenter + ((radius_mult * off_x + (1<<23))>>24);     \
            const int y = ycenter + ((radius_mult * off_y + (1<<23))>>24);     \
            const char isvalid = x >= 0 && x < w && y >= 0 && y < h;           \
            *out++ =  isvalid ? indata[y * inlinesize + x] : fill_color;       \
        }                                                                      \
    }                                                                          \
    return 0;                                                                  \
}


NEAREST(uint8_t, 8)
NEAREST(uint16_t, 16)

#define BILINEAR(type, name)                                                   \
static int filter##name##_slice_bilinear(AVFilterContext *ctx, void *arg,      \
                                         int job, int nb_jobs)                 \
{                                                                              \
    ThreadData *td = arg;                                                      \
    AVFrame *in = td->in;                                                      \
    AVFrame *out = td->out;                                                    \
                                                                               \
    const int fill_color = td->fill_color;                                     \
    const int depth = td->depth;                                               \
    const uint64_t max = (1 << 24) - 1;                                        \
    const uint64_t add = (1 << 23);                                            \
    const int w = td->w, h = td->h;                                            \
    const int xcenter = td->xcenter;                                           \
    const int ycenter = td->ycenter;                                           \
    const int start = (h *  job   ) / nb_jobs;                                 \
    const int end   = (h * (job+1)) / nb_jobs;                                 \
    const int plane = td->plane;                                               \
    const int inlinesize = in->linesize[plane] / sizeof(type);                 \
    const int outlinesize = out->linesize[plane] / sizeof(type);               \
    const type *indata = (const type *)in->data[plane];                        \
    type *outrow = (type *)out->data[plane] + start * outlinesize;             \
                                                                               \
    for (int i = start; i < end; i++, outrow += outlinesize) {                 \
        const int off_y = i - ycenter;                                         \
        type *out = outrow;                                                    \
                                                                               \
        for (int j = 0; j < w; j++) {                                          \
            const int off_x = j - xcenter;                                     \
            const int64_t radius_mult = td->correction[j + i*w];               \
            const int x = xcenter + ((radius_mult * off_x + (1<<23)) >> 24);   \
            const int y = ycenter + ((radius_mult * off_y + (1<<23)) >> 24);   \
            const char isvalid = x >= 0 && x <= w - 1 && y >= 0 && y <= h - 1; \
                                                                               \
            if (isvalid) {                                                     \
                const int nx = FFMIN(x + 1, w - 1);                            \
                const int ny = FFMIN(y + 1, h - 1);                            \
                const uint64_t du = off_x >= 0 ? (radius_mult * off_x + add) & max : max - ((radius_mult * -off_x + add) & max); \
                const uint64_t dv = off_y >= 0 ? (radius_mult * off_y + add) & max : max - ((radius_mult * -off_y + add) & max); \
                const uint64_t p0 = indata[ y * inlinesize +  x];              \
                const uint64_t p1 = indata[ y * inlinesize + nx];              \
                const uint64_t p2 = indata[ny * inlinesize +  x];              \
                const uint64_t p3 = indata[ny * inlinesize + nx];              \
                uint64_t sum = 0;                                              \
                                                                               \
                sum += (max - du) * (max - dv) * p0;                           \
                sum += (      du) * (max - dv) * p1;                           \
                sum += (max - du) * (      dv) * p2;                           \
                sum += (      du) * (      dv) * p3;                           \
                                                                               \
                out[j] = av_clip_uintp2_c((sum + (1ULL << 47)) >> 48, depth);  \
            } else {                                                           \
                out[j] = fill_color;                                           \
            }                                                                  \
        }                                                                      \
    }                                                                          \
                                                                               \
    return 0;                                                                  \
}

BILINEAR(uint8_t, 8)
BILINEAR(uint16_t, 16)

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_GRAY8, AV_PIX_FMT_GRAY9,
        AV_PIX_FMT_GRAY10, AV_PIX_FMT_GRAY12, AV_PIX_FMT_GRAY14,
        AV_PIX_FMT_GRAY16,
        AV_PIX_FMT_YUV410P, AV_PIX_FMT_YUV411P,
        AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV422P,
        AV_PIX_FMT_YUV440P, AV_PIX_FMT_YUV444P,
        AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUVJ422P,
        AV_PIX_FMT_YUVJ440P, AV_PIX_FMT_YUVJ444P,
        AV_PIX_FMT_YUVJ411P,
        AV_PIX_FMT_YUV420P9, AV_PIX_FMT_YUV422P9, AV_PIX_FMT_YUV444P9,
        AV_PIX_FMT_YUV420P10, AV_PIX_FMT_YUV422P10, AV_PIX_FMT_YUV444P10,
        AV_PIX_FMT_YUV440P10,
        AV_PIX_FMT_YUV444P12, AV_PIX_FMT_YUV422P12, AV_PIX_FMT_YUV420P12,
        AV_PIX_FMT_YUV440P12,
        AV_PIX_FMT_YUV444P14, AV_PIX_FMT_YUV422P14, AV_PIX_FMT_YUV420P14,
        AV_PIX_FMT_YUV420P16, AV_PIX_FMT_YUV422P16, AV_PIX_FMT_YUV444P16,
        AV_PIX_FMT_GBRP, AV_PIX_FMT_GBRP9, AV_PIX_FMT_GBRP10,
        AV_PIX_FMT_GBRP12, AV_PIX_FMT_GBRP14, AV_PIX_FMT_GBRP16,
        AV_PIX_FMT_YUVA420P,  AV_PIX_FMT_YUVA422P,   AV_PIX_FMT_YUVA444P,
        AV_PIX_FMT_YUVA444P9, AV_PIX_FMT_YUVA444P10, AV_PIX_FMT_YUVA444P12, AV_PIX_FMT_YUVA444P16,
        AV_PIX_FMT_YUVA422P9, AV_PIX_FMT_YUVA422P10, AV_PIX_FMT_YUVA422P12, AV_PIX_FMT_YUVA422P16,
        AV_PIX_FMT_YUVA420P9, AV_PIX_FMT_YUVA420P10, AV_PIX_FMT_YUVA420P16,
        AV_PIX_FMT_GBRAP,     AV_PIX_FMT_GBRAP10,    AV_PIX_FMT_GBRAP12,    AV_PIX_FMT_GBRAP16,
        AV_PIX_FMT_NONE
    };
    AVFilterFormats *fmts_list = ff_make_format_list(pix_fmts);
    if (!fmts_list)
        return AVERROR(ENOMEM);
    return ff_set_common_formats(ctx, fmts_list);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    LenscorrectionCtx *rect = ctx->priv;
    int i;

    for (i = 0; i < FF_ARRAY_ELEMS(rect->correction); i++) {
        av_freep(&rect->correction[i]);
    }
}

static int config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    LenscorrectionCtx *rect = ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];
    const AVPixFmtDescriptor *pixdesc = av_pix_fmt_desc_get(inlink->format);
    int is_rgb = !!(pixdesc->flags & AV_PIX_FMT_FLAG_RGB);
    uint8_t rgba_map[4];
    int factor;

    ff_fill_rgba_map(rgba_map, inlink->format);
    rect->depth = pixdesc->comp[0].depth;
    factor = 1 << (rect->depth - 8);
    rect->hsub = pixdesc->log2_chroma_w;
    rect->vsub = pixdesc->log2_chroma_h;
    outlink->w = rect->width = inlink->w;
    outlink->h = rect->height = inlink->h;
    rect->nb_planes = av_pix_fmt_count_planes(inlink->format);
    rect->filter_slice = rect->depth <= 8 ? filter8_slice : filter16_slice;
    if (rect->interpolation)
        rect->filter_slice = rect->depth <= 8 ? filter8_slice_bilinear : filter16_slice_bilinear;

    if (is_rgb) {
        rect->fill_color[rgba_map[0]] = rect->fill_rgba[0] * factor;
        rect->fill_color[rgba_map[1]] = rect->fill_rgba[1] * factor;
        rect->fill_color[rgba_map[2]] = rect->fill_rgba[2] * factor;
        rect->fill_color[rgba_map[3]] = rect->fill_rgba[3] * factor;
    } else {
        rect->fill_color[0] = RGB_TO_Y_BT709(rect->fill_rgba[0], rect->fill_rgba[1], rect->fill_rgba[2]) * factor;
        rect->fill_color[1] = RGB_TO_U_BT709(rect->fill_rgba[0], rect->fill_rgba[1], rect->fill_rgba[2], 0) * factor;
        rect->fill_color[2] = RGB_TO_V_BT709(rect->fill_rgba[0], rect->fill_rgba[1], rect->fill_rgba[2], 0) * factor;
        rect->fill_color[3] = rect->fill_rgba[3] * factor;
    }
     return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    LenscorrectionCtx *rect = (LenscorrectionCtx*)ctx->priv;
    AVFrame *out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    int plane;

    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }

    av_frame_copy_props(out, in);

    for (plane = 0; plane < rect->nb_planes; ++plane) {
        int hsub = plane == 1 || plane == 2 ? rect->hsub : 0;
        int vsub = plane == 1 || plane == 2 ? rect->vsub : 0;
        int w = AV_CEIL_RSHIFT(rect->width, hsub);
        int h = AV_CEIL_RSHIFT(rect->height, vsub);
        int xcenter = rect->cx * w;
        int ycenter = rect->cy * h;
        int k1 = rect->k1 * (1<<24);
        int k2 = rect->k2 * (1<<24);
        ThreadData td = {
            .in = in,
            .out  = out,
            .w  = w,
            .h  = h,
            .xcenter = xcenter,
            .ycenter = ycenter,
            .plane = plane,
            .depth = rect->depth,
            .fill_color = rect->fill_color[plane],
        };

        if (!rect->correction[plane]) {
            int i,j;
            const int64_t r2inv = (4LL<<60) / (w * w + h * h);

            rect->correction[plane] = av_malloc_array(w, h * sizeof(**rect->correction));
            if (!rect->correction[plane])
                return AVERROR(ENOMEM);
            for (j = 0; j < h; j++) {
                const int off_y = j - ycenter;
                const int off_y2 = off_y * off_y;
                for (i = 0; i < w; i++) {
                    const int off_x = i - xcenter;
                    const int64_t r2 = ((off_x * off_x + off_y2) * r2inv + (1LL<<31)) >> 32;
                    const int64_t r4 = (r2 * r2 + (1<<27)) >> 28;
                    const int radius_mult = (r2 * k1 + r4 * k2 + (1LL<<27) + (1LL<<52))>>28;
                    rect->correction[plane][j * w + i] = radius_mult;
                }
            }
        }

        td.correction = rect->correction[plane];
        ctx->internal->execute(ctx, rect->filter_slice, &td, NULL, FFMIN(h, ff_filter_get_nb_threads(ctx)));
    }

    av_frame_free(&in);
    return ff_filter_frame(outlink, out);
}

static const AVFilterPad lenscorrection_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad lenscorrection_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_props,
    },
    { NULL }
};

AVFilter ff_vf_lenscorrection = {
    .name          = "lenscorrection",
    .description   = NULL_IF_CONFIG_SMALL("Rectify the image by correcting for lens distortion."),
    .priv_size     = sizeof(LenscorrectionCtx),
    .query_formats = query_formats,
    .inputs        = lenscorrection_inputs,
    .outputs       = lenscorrection_outputs,
    .priv_class    = &lenscorrection_class,
    .uninit        = uninit,
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC | AVFILTER_FLAG_SLICE_THREADS,
};
