/*
 * Copyright (c) 2012-2014 Clément Bœsch <u pkh me>
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
 * Video Enhance AI filter
 *
 * @see https://www.topazlabs.com/video-enhance-ai
 */

#include "libavutil/avassert.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libavutil/avutil.h"
#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"
#include "veai.h"
#include "veai_common.h"

typedef struct VEAICPEContext {
    const AVClass *class;
    char *model, *filename;
    int device;
    int canDownloadModels;
    void* pFrameProcessor;
    unsigned int counter;
    int rsc;
} VEAICPEContext;

#define OFFSET(x) offsetof(VEAICPEContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM
static const AVOption veai_cpe_options[] = {
    { "model", "Model short name", OFFSET(model), AV_OPT_TYPE_STRING, {.str="cpe-1"}, .flags = FLAGS },
    { "filename", "CPE output filename", OFFSET(filename), AV_OPT_TYPE_STRING, {.str="cpe.json"}, .flags = FLAGS },
    { "device",  "Device index (Auto: -2, CPU: -1, GPU0: 0, ...)",  OFFSET(device),  AV_OPT_TYPE_INT, {.i64=-2}, -2, 8, FLAGS, "device" },
    { "download",  "Enable model downloading",  OFFSET(canDownloadModels),  AV_OPT_TYPE_INT, {.i64=1}, 0, 1, FLAGS, "canDownloadModels" },
    { NULL }
};

AVFILTER_DEFINE_CLASS(veai_cpe);

static av_cold int init(AVFilterContext *ctx) {
  VEAICPEContext *veai = ctx->priv;
  av_log(ctx, AV_LOG_DEBUG, "Here init with params: %s %d\n", veai->model, veai->device);
  veai->counter = 0;
  return 0;
}

static int config_props(AVFilterLink *outlink) {
    AVFilterContext *ctx = outlink->src;
    VEAICPEContext *veai = ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];
    VideoProcessorInfo info;
    info.options[0] = veai->filename;
    if(ff_veai_verifyAndSetInfo(&info, inlink, outlink, (char*)"cpe", veai->model, ModelTypeCamPoseEstimation, veai->device, 0, 1, 1, veai->canDownloadModels, &veai->rsc, 1, ctx)) {
      return AVERROR(EINVAL);
    }
    veai->pFrameProcessor = veai_create(&info);
    return veai->pFrameProcessor == NULL ? AVERROR(EINVAL) : 0;
}

static const enum AVPixelFormat pix_fmts[] = {
    AV_PIX_FMT_BGR48,
    AV_PIX_FMT_NONE
};

static int filter_frame(AVFilterLink *inlink, AVFrame *in) {
    AVFilterContext *ctx = inlink->dst;
    VEAICPEContext *veai = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    IOBuffer ioBuffer;
    ff_veai_prepareIOBufferInput(&ioBuffer, in, FrameTypeNormal, veai->counter==0);

    float transform[4] = {0,0,0,0};
    ioBuffer.output.pBuffer = (unsigned char *)transform;
    ioBuffer.output.lineSize = sizeof(float)*4;

    if(veai->pFrameProcessor == NULL || veai_process(veai->pFrameProcessor,  &ioBuffer)) {
        av_log(ctx, AV_LOG_ERROR, "The processing has failed");
        av_frame_free(&in);
        return AVERROR(ENOSYS);
    }
    av_log(ctx, AV_LOG_DEBUG, "%u CPE: %f\t%f\t%f\t%f\n", veai->counter++, transform[0], transform[1], transform[2], transform[3]);
    return ff_filter_frame(outlink, in);
}

static int request_frame(AVFilterLink *outlink) {
    AVFilterContext *ctx = outlink->src;
    VEAICPEContext *veai = ctx->priv;
    int ret = ff_request_frame(ctx->inputs[0]);
    if (ret == AVERROR_EOF) {
        int i, n = veai_remaining_frames(veai->pFrameProcessor);
        for(i=0;i<n;i++) {
            VEAIBuffer oBuffer;
            float transform[4] = {0,0,0,0};
            oBuffer.pBuffer = (unsigned char *)transform;
            oBuffer.lineSize = sizeof(float)*4;
            if(veai->pFrameProcessor == NULL || veai_process_back(veai->pFrameProcessor, &oBuffer)) {
                av_log(ctx, AV_LOG_ERROR, "The post flight processing has failed");
                return AVERROR(ENOSYS);
            }
        }
        av_log(ctx, AV_LOG_DEBUG, "End of file reached %s %d\n", veai->model, veai->pFrameProcessor == NULL);
    }

    return ret;
}

static av_cold void uninit(AVFilterContext *ctx) {
    VEAICPEContext *veai = ctx->priv;
    av_log(ctx, AV_LOG_DEBUG, "Uninit called for %s\n", veai->model);
    if(veai->pFrameProcessor)
        veai_destroy(veai->pFrameProcessor);
}

static const AVFilterPad veai_cpe_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    },
};

static const AVFilterPad veai_cpe_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .config_props = config_props,
        .request_frame = request_frame,
    },
};

const AVFilter ff_vf_veai_cpe = {
    .name          = "veai_cpe",
    .description   = NULL_IF_CONFIG_SMALL("Apply Video Enhance AI upscale models, parameters will only be applied to appropriate models"),
    .priv_size     = sizeof(VEAICPEContext),
    .init          = init,
    .uninit        = uninit,
    FILTER_INPUTS(veai_cpe_inputs),
    FILTER_OUTPUTS(veai_cpe_outputs),
    FILTER_PIXFMTS_ARRAY(pix_fmts),
    .priv_class    = &veai_cpe_class,
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
};
