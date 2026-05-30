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
#include "float.h"

typedef struct VEAIStbContext {
    const AVClass *class;
    char *model, *filename, *filler;
    int device, extraThreads;
    int canDownloadModels;
    double vram;
    void* pFrameProcessor;
    double smoothness;
    int postFlight, windowSize, cacheSize, stabDOF, enableRSC, enableFullFrame;
    double readStartTime, writeStartTime, canvasScaleX, canvasScaleY;
    AVFrame* previousFrame;
} VEAIStbContext;

#define OFFSET(x) offsetof(VEAIStbContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM
static const AVOption veai_stb_options[] = {
    { "model", "Model short name", OFFSET(model), AV_OPT_TYPE_STRING, {.str="ref-1"}, .flags = FLAGS },
    { "device",  "Device index (Auto: -2, CPU: -1, GPU0: 0, ...)",  OFFSET(device),  AV_OPT_TYPE_INT, {.i64=-2}, -2, 8, FLAGS, "device" },
    { "threads",  "Number of extra threads to use on device",  OFFSET(extraThreads),  AV_OPT_TYPE_INT, {.i64=0}, 0, 3, FLAGS, "extraThreads" },
    { "download",  "Enable model downloading",  OFFSET(canDownloadModels),  AV_OPT_TYPE_INT, {.i64=1}, 0, 1, FLAGS, "canDownloadModels" },
    { "vram", "Max memory usage", OFFSET(vram), AV_OPT_TYPE_DOUBLE, {.dbl=1.0}, 0.1, 1, .flags = FLAGS, "vram"},
    { "full", "Perform full-frame stabilization. If disabled, performs auto-crop (ignores full-reame related options)", OFFSET(enableFullFrame), AV_OPT_TYPE_INT, {.i64=1}, 0, 1, .flags = FLAGS, "full" },
    { "filename", "CPE output filename", OFFSET(filename), AV_OPT_TYPE_STRING, {.str="cpe.json"}, .flags = FLAGS, "filename"},
    { "rst", "Read start time relative to CPE", OFFSET(readStartTime), AV_OPT_TYPE_DOUBLE, {.dbl=0}, 0, DBL_MAX, .flags = FLAGS, "rst" },
    { "wst", "Write start time relative to read start time (rst)", OFFSET(writeStartTime), AV_OPT_TYPE_DOUBLE, {.dbl=0}, 0, DBL_MAX, .flags = FLAGS, "wst" },
    { "postFlight", "Enable postflight", OFFSET(postFlight), AV_OPT_TYPE_INT, {.i64=1}, 0, 1, .flags = FLAGS, "postFlight"  },
    { "ws", "Window size for full-frame synthesis", OFFSET(windowSize), AV_OPT_TYPE_INT, {.i64=64}, 0, 512, .flags = FLAGS, "ws"  },
    { "csx", "Scale of the canvas relative to input width", OFFSET(canvasScaleX), AV_OPT_TYPE_DOUBLE, {.dbl=2}, 1, 8, .flags = FLAGS, "csx"  },
    { "csy", "Scale of the canvas relative to input height", OFFSET(canvasScaleY), AV_OPT_TYPE_DOUBLE, {.dbl=2}, 1, 8, .flags = FLAGS, "csy"  },
    { "smoothness", "Amount of smoothness to be applied on the camera trajectory to stabilize the video",  OFFSET(smoothness),  AV_OPT_TYPE_DOUBLE, {.dbl=6.0}, 0.0, 16.0, FLAGS, "smoothness" },
    { "cache", "Set memory cache size", OFFSET(cacheSize), AV_OPT_TYPE_INT, {.i64=128}, 0, 256, .flags = FLAGS, "cache" },
    { "dof", "Enable/Disable stabilization of different motions - rotation (1st digit), horizontal pan (2nd), vertical pan (3rd), scale/zoom (4th digit). Non-zero digit enables corresponding motions", OFFSET(stabDOF), AV_OPT_TYPE_INT, {.i64=1111}, 0, 1111, .flags = FLAGS, "dof" },
    { "roll", "Enable rolling shutter correction", OFFSET(enableRSC), AV_OPT_TYPE_INT, {.i64=0}, 0, 1, .flags = FLAGS, "roll" },

    { NULL }
};

AVFILTER_DEFINE_CLASS(veai_stb);

static av_cold int init(AVFilterContext *ctx) {
  VEAIStbContext *veai = ctx->priv;
  av_log(ctx, AV_LOG_VERBOSE, "Here init with params: %s %d %s %s %lf\n", veai->model, veai->device, veai->filename, veai->filler, veai->smoothness);
  veai->previousFrame = NULL;
  return 0;
}

static int config_props(AVFilterLink *outlink) {
  AVFilterContext *ctx = outlink->src;
  VEAIStbContext *veai = ctx->priv;
  AVFilterLink *inlink = ctx->inputs[0];
  VideoProcessorInfo info;
  info.options[0] = veai->filename;
  info.options[1] = veai->filler;
  float smoothness = veai->smoothness;
  float params[10] = {veai->smoothness, veai->windowSize, veai->postFlight, veai->canvasScaleX, veai->canvasScaleY, veai->cacheSize, veai->stabDOF, veai->enableRSC, veai->readStartTime, veai->writeStartTime};
  if(ff_veai_verifyAndSetInfo(&info, inlink, outlink, (veai->enableFullFrame > 0) ? (char*)"st" : (char*)"stx", veai->model, ModelTypeStabilization, veai->device, veai->extraThreads, veai->vram, 1, veai->canDownloadModels, params, 10, ctx)) {
    return AVERROR(EINVAL);
  }
  veai->pFrameProcessor = veai_create(&info);
  if(veai->pFrameProcessor == NULL) {
    return AVERROR(EINVAL);
  }
  if(!veai->enableFullFrame) {
    veai_stabilize_get_output_size(veai->pFrameProcessor, &(outlink->w), &(outlink->h));
    av_log(NULL, AV_LOG_VERBOSE, "Auto-crop stabilization output size: %d x %d\n", outlink->w, outlink->h);
  }
  return 0;
}

static const enum AVPixelFormat pix_fmts[] = {
    AV_PIX_FMT_BGR48,
    AV_PIX_FMT_NONE
};

static int filter_frame(AVFilterLink *inlink, AVFrame *in) {
    AVFilterContext *ctx = inlink->dst;
    VEAIStbContext *veai = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    AVFrame *out;
    IOBuffer ioBuffer;
    ff_veai_prepareIOBufferInput(&ioBuffer, in, FrameTypeNormal, veai->previousFrame == NULL);
    out = ff_veai_prepareBufferOutput(outlink, &ioBuffer.output);
    if(veai->pFrameProcessor == NULL || out == NULL || veai_process(veai->pFrameProcessor,  &ioBuffer)) {
        av_log(NULL, AV_LOG_ERROR, "The processing has failed");
        av_frame_free(&in);
        return AVERROR(ENOSYS);
    }
    double its = TS2T(in->pts, inlink->time_base);
    av_frame_copy_props(out, in);
    out->pts = ioBuffer.output.timestamp;
    if(veai->previousFrame)
      av_frame_free(&veai->previousFrame);
    veai->previousFrame = in;
    if(ioBuffer.output.timestamp < 0) {
      av_frame_free(&out);
      av_log(ctx, AV_LOG_DEBUG, "Ignoring frame %s %lf %lf\n", veai->model, its, TS2T(ioBuffer.output.timestamp, outlink->time_base));
      return 0;
    }
    av_log(ctx, AV_LOG_DEBUG, "Finished processing frame %s %lf %lf\n", veai->model, its, TS2T(ioBuffer.output.timestamp, outlink->time_base));
    return ff_filter_frame(outlink, out);
}

static int request_frame(AVFilterLink *outlink) {
    AVFilterContext *ctx = outlink->src;
    VEAIStbContext *veai = ctx->priv;
    int ret = ff_request_frame(ctx->inputs[0]);
    if (ret == AVERROR_EOF) {
        if(ff_veai_handlePostFlight(veai->pFrameProcessor, outlink, veai->previousFrame, ctx)) {
          av_log(NULL, AV_LOG_ERROR, "The postflight processing has failed");
          av_frame_free(&veai->previousFrame);
          return AVERROR(ENOSYS);
        }
        av_frame_free(&veai->previousFrame);
        av_log(ctx, AV_LOG_DEBUG, "End of file reached %s %d\n", veai->model, veai->pFrameProcessor == NULL);
    }
    return ret;
}

static av_cold void uninit(AVFilterContext *ctx) {
    VEAIStbContext *veai = ctx->priv;
    av_log(ctx, AV_LOG_DEBUG, "Uninit called for %s %d\n", veai->model, veai->pFrameProcessor == NULL);
    if(veai->pFrameProcessor)
        veai_destroy(veai->pFrameProcessor);
}

static const AVFilterPad veai_stb_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    },
};

static const AVFilterPad veai_stb_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .config_props = config_props,
        .request_frame = request_frame,
    },
};

const AVFilter ff_vf_veai_stb = {
    .name          = "veai_stb",
    .description   = NULL_IF_CONFIG_SMALL("Apply Video Enhance AI stabilization models"),
    .priv_size     = sizeof(VEAIStbContext),
    .init          = init,
    .uninit        = uninit,
    FILTER_INPUTS(veai_stb_inputs),
    FILTER_OUTPUTS(veai_stb_outputs),
    FILTER_PIXFMTS_ARRAY(pix_fmts),
    .priv_class    = &veai_stb_class,
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
};
