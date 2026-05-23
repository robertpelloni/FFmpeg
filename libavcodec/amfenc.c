/*
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

#include "config.h"

#include "libavutil/avassert.h"
#include "libavutil/imgutils.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_amf.h"
#if CONFIG_D3D11VA
#include "libavutil/hwcontext_d3d11va.h"
#endif
#if CONFIG_DXVA2
#define COBJMACROS
#include "libavutil/hwcontext_dxva2.h"
#endif
#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"
#include "libavutil/time.h"

#include "amfenc.h"
#include "encode.h"

#define AMF_AV_FRAME_REF    L"av_frame_ref"
#define PTS_PROP            L"PtsProp"

#if CONFIG_D3D11VA
#include <d3d11.h>
#endif

#ifdef _WIN32
#include "compat/w32dlfcn.h"
#else
#include <dlfcn.h>
#endif

#define FFMPEG_AMF_WRITER_ID L"ffmpeg_amf"


const enum AVPixelFormat ff_amf_pix_fmts[] = {
    AV_PIX_FMT_NV12,
    AV_PIX_FMT_YUV420P,
#if CONFIG_D3D11VA
    AV_PIX_FMT_D3D11,
#endif
#if CONFIG_DXVA2
    AV_PIX_FMT_DXVA2_VLD,
#endif
    AV_PIX_FMT_P010,
    AV_PIX_FMT_NONE
};

static int64_t next_encoder_index = 0;

static int amf_init_encoder(AVCodecContext *avctx)
{
    AMFEncoderContext  *ctx = avctx->priv_data;
    const wchar_t      *codec_id = NULL;
    AMF_RESULT          res;
    enum AVPixelFormat  pix_fmt;
    AVHWDeviceContext  *hw_device_ctx = (AVHWDeviceContext*)ctx->device_ctx_ref->data;
    AVAMFDeviceContext *amf_device_ctx = (AVAMFDeviceContext *)hw_device_ctx->hwctx;
    int                 alloc_size;
    wchar_t             name[512];


    alloc_size = swprintf(name, amf_countof(name), L"%s%lld",PTS_PROP, next_encoder_index) + 1;
    ctx->pts_property_name = av_memdup(name, alloc_size * sizeof(wchar_t));
    if(!ctx->pts_property_name)
        return AVERROR(ENOMEM);

    alloc_size = swprintf(name, amf_countof(name), L"%s%lld",AMF_AV_FRAME_REF, next_encoder_index) + 1;
    ctx->av_frame_property_name = av_memdup(name, alloc_size * sizeof(wchar_t));
    if(!ctx->av_frame_property_name)
        return AVERROR(ENOMEM);

    next_encoder_index++;

    switch (avctx->codec->id) {
        case AV_CODEC_ID_H264:
            codec_id = AMFVideoEncoderVCE_AVC;
            break;
        case AV_CODEC_ID_HEVC:
            codec_id = AMFVideoEncoder_HEVC;
            break;
        case AV_CODEC_ID_AV1 :
            codec_id = AMFVideoEncoder_AV1;
            break;
        default:
            break;
    }
    AMF_RETURN_IF_FALSE(ctx, codec_id != NULL, AVERROR(EINVAL), "Codec %d is not supported\n", avctx->codec->id);

    if (ctx->hw_frames_ctx)
        pix_fmt = ((AVHWFramesContext*)ctx->hw_frames_ctx->data)->sw_format;
    else
        pix_fmt = avctx->pix_fmt;

    if (pix_fmt == AV_PIX_FMT_P010) {
        AMF_RETURN_IF_FALSE(ctx, ctx->version >= AMF_MAKE_FULL_VERSION(1, 4, 32, 0), AVERROR_UNKNOWN, "10-bit encoder is not supported by AMD GPU drivers versions lower than 23.30.\n");
    }

    ctx->format = amf_av_to_amf_format(pix_fmt);
    AMF_RETURN_IF_FALSE(ctx, ctx->format != AMF_SURFACE_UNKNOWN, AVERROR(EINVAL),
                        "Format %s is not supported\n", av_get_pix_fmt_name(pix_fmt));

    res = ctx->factory->pVtbl->CreateComponent(ctx->factory, ctx->context, codec_id, &ctx->encoder);
    AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_ENCODER_NOT_FOUND, "CreateComponent(%ls) failed with error %d\n", codec_id, res);

    ctx->submitted_frame = 0;

    return 0;
}

int av_cold ff_amf_encode_close(AVCodecContext *avctx)
{
    AmfContext *ctx = avctx->priv_data;

    if (ctx->delayed_surface) {
        ctx->delayed_surface->pVtbl->Release(ctx->delayed_surface);
        ctx->delayed_surface = NULL;
    }

    if (ctx->encoder) {
        ctx->encoder->pVtbl->Terminate(ctx->encoder);
        ctx->encoder->pVtbl->Release(ctx->encoder);
        ctx->encoder = NULL;
    }

    if (ctx->context) {
        ctx->context->pVtbl->Terminate(ctx->context);
        ctx->context->pVtbl->Release(ctx->context);
        ctx->context = NULL;
    }
    av_buffer_unref(&ctx->hw_device_ctx);
    av_buffer_unref(&ctx->hw_frames_ctx);

    if (ctx->trace) {
        ctx->trace->pVtbl->UnregisterWriter(ctx->trace, FFMPEG_AMF_WRITER_ID);
    }
    if (ctx->library) {
        dlclose(ctx->library);
        ctx->library = NULL;
    }
    ctx->trace = NULL;
    ctx->debug = NULL;
    ctx->factory = NULL;
    ctx->version = 0;
    ctx->delayed_drain = 0;
    av_frame_free(&ctx->delayed_frame);
    av_fifo_freep2(&ctx->timestamp_list);

    if (ctx->output_list) {
        // release remaining AMF output buffers
        while(av_fifo_can_read(ctx->output_list)) {
            AMFBuffer* buffer = NULL;
            av_fifo_read(ctx->output_list, &buffer, 1);
            if(buffer != NULL)
                buffer->pVtbl->Release(buffer);
        }
        av_fifo_freep2(&ctx->output_list);
    }
    av_freep(&ctx->pts_property_name);
    av_freep(&ctx->av_frame_property_name);

    return 0;
}

static int amf_copy_surface(AVCodecContext *avctx, const AVFrame *frame,
    AMFSurface* surface)
{
    AMFPlane *plane;
    uint8_t  *dst_data[4];
    int       dst_linesize[4];
    int       planes;
    int       i;

    planes = surface->pVtbl->GetPlanesCount(surface);
    av_assert0(planes < FF_ARRAY_ELEMS(dst_data));

    for (i = 0; i < planes; i++) {
        plane = surface->pVtbl->GetPlaneAt(surface, i);
        dst_data[i] = plane->pVtbl->GetNative(plane);
        dst_linesize[i] = plane->pVtbl->GetHPitch(plane);
    }
    av_image_copy2(dst_data, dst_linesize,
                   frame->data, frame->linesize, frame->format,
                   avctx->width, avctx->height);

    return 0;
}

static int amf_copy_buffer(AVCodecContext *avctx, AVPacket *pkt, AMFBuffer *buffer)
{
    AmfContext      *ctx = avctx->priv_data;
    int              ret;
    AMFVariantStruct var = {0};
    int64_t          timestamp = AV_NOPTS_VALUE;
    int64_t          size = buffer->pVtbl->GetSize(buffer);
    enum AVPictureType pict_type = 0;
    int              average_qp = -1;

    if ((ret = ff_get_encode_buffer(avctx, pkt, size, 0)) < 0) {
        return ret;
    }
    memcpy(pkt->data, buffer->pVtbl->GetNative(buffer), size);

    switch (avctx->codec->id) {
    case AV_CODEC_ID_H264:
        buffer->pVtbl->GetProperty(buffer, AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, &var);
        pkt->flags |= AV_PKT_FLAG_KEY * (var.int64Value == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR);
        pict_type = var.int64Value == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR ? AV_PICTURE_TYPE_I :
                    var.int64Value == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_I ? AV_PICTURE_TYPE_I :
                    var.int64Value == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_P ? AV_PICTURE_TYPE_P :
                    var.int64Value == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_B ? AV_PICTURE_TYPE_B : 0;

        var.int64Value = -1;
        if ((buffer->pVtbl->GetProperty(buffer, AMF_VIDEO_ENCODER_STATISTIC_AVERAGE_QP, &var)) == AMF_OK) {
            average_qp = FFMAX((int)var.int64Value, -1);
        }
        break;
    case AV_CODEC_ID_HEVC:
        buffer->pVtbl->GetProperty(buffer, AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE, &var);
        pkt->flags |= AV_PKT_FLAG_KEY * (var.int64Value == AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_IDR);
        pict_type = var.int64Value == AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_IDR ? AV_PICTURE_TYPE_I :
                    var.int64Value == AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_I ? AV_PICTURE_TYPE_I :
                    var.int64Value == AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_P ? AV_PICTURE_TYPE_P : 0;

        var.int64Value = -1;
        if ((buffer->pVtbl->GetProperty(buffer, AMF_VIDEO_ENCODER_HEVC_STATISTIC_AVERAGE_QP, &var)) == AMF_OK) {
            average_qp = FFMAX((int)var.int64Value, -1);
        }
        break;
    case AV_CODEC_ID_AV1:
        buffer->pVtbl->GetProperty(buffer, AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE, &var);
        pkt->flags |= AV_PKT_FLAG_KEY * (var.int64Value == AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE_KEY);
        pict_type = var.int64Value == AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE_KEY ? AV_PICTURE_TYPE_I :
                    var.int64Value == AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE_INTRA_ONLY ? AV_PICTURE_TYPE_I :
                    var.int64Value == AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE_INTER ? AV_PICTURE_TYPE_P : 0;

        var.int64Value = -1;
        if ((buffer->pVtbl->GetProperty(buffer, AMF_VIDEO_ENCODER_AV1_STATISTIC_AVERAGE_Q_INDEX, &var)) == AMF_OK) {
            average_qp = FFMAX((int)var.int64Value, -1); // av1 qindex
            if (average_qp >= 0) {
                average_qp = (average_qp > 244) ? (average_qp <= 249 ? 62 : 63) : (average_qp + 3) >> 2; // av1 quantizer
            }
        }
        break;
    default:
        break;
    }

    if (average_qp >= 0) {
        ff_encode_add_stats_side_data(pkt, average_qp * FF_QP2LAMBDA, NULL, 0, pict_type);
    }

    buffer->pVtbl->GetProperty(buffer, ctx->pts_property_name, &var);

    pkt->pts = var.int64Value; // original pts


    AMF_RETURN_IF_FALSE(ctx, av_fifo_read(ctx->timestamp_list, &timestamp, 1) >= 0,
                        AVERROR_UNKNOWN, "timestamp_list is empty\n");

    // calc dts shift if max_b_frames > 0
    if ((ctx->max_b_frames > 0 || ((ctx->pa_adaptive_mini_gop == 1) ? true : false)) && ctx->dts_delay == 0) {
        int64_t timestamp_last = AV_NOPTS_VALUE;
        size_t can_read = av_fifo_can_read(ctx->timestamp_list);
        AMF_RETURN_IF_FALSE(ctx, can_read > 0, AVERROR_UNKNOWN,
            "timestamp_list is empty while max_b_frames = %d\n", avctx->max_b_frames);
        av_fifo_peek(ctx->timestamp_list, &timestamp_last, 1, can_read - 1);
        if (timestamp < 0 || timestamp_last < AV_NOPTS_VALUE) {
            return AVERROR(ERANGE);
        }
        ctx->dts_delay = timestamp_last - timestamp;
    }
    pkt->dts = timestamp - ctx->dts_delay;
    return 0;
}

// amfenc API implementation
int ff_amf_encode_init(AVCodecContext *avctx)
{
    int ret;

    // hardcoded to current HW queue size - will auto-realloc if too small
    ctx->timestamp_list = av_fifo_alloc2(avctx->max_b_frames + 16, sizeof(int64_t),
                                         AV_FIFO_FLAG_AUTO_GROW);
    if (!ctx->timestamp_list) {
        return AVERROR(ENOMEM);
    }
    ctx->output_list = av_fifo_alloc2(2, sizeof(AMFBuffer*), AV_FIFO_FLAG_AUTO_GROW);
    if (!ctx->output_list)
        return AVERROR(ENOMEM);

    ctx->dts_delay = 0;

    ctx->hwsurfaces_in_queue = 0;

    if (avctx->hw_device_ctx) {
        hwdev_ctx = (AVHWDeviceContext*)avctx->hw_device_ctx->data;
        if (hwdev_ctx->type == AV_HWDEVICE_TYPE_AMF)
        {
            ctx->device_ctx_ref = av_buffer_ref(avctx->hw_device_ctx);
        }
        else {
            ret = av_hwdevice_ctx_create_derived(&ctx->device_ctx_ref, AV_HWDEVICE_TYPE_AMF, avctx->hw_device_ctx, 0);
            AMF_RETURN_IF_FALSE(ctx, ret == 0, ret, "Failed to create derived AMF device context: %s\n", av_err2str(ret));
        }
    } else if (avctx->hw_frames_ctx) {
        AVHWFramesContext *frames_ctx = (AVHWFramesContext*)avctx->hw_frames_ctx->data;
        if (frames_ctx->device_ref ) {
            if (frames_ctx->format == AV_PIX_FMT_AMF_SURFACE) {
                ctx->device_ctx_ref = av_buffer_ref(frames_ctx->device_ref);
            }
            else {
                ret = av_hwdevice_ctx_create_derived(&ctx->device_ctx_ref, AV_HWDEVICE_TYPE_AMF, frames_ctx->device_ref, 0);
                AMF_RETURN_IF_FALSE(ctx, ret == 0, ret, "Failed to create derived AMF device context: %s\n", av_err2str(ret));
            }
        }
    }
    else {
        ret = av_hwdevice_ctx_create(&ctx->device_ctx_ref, AV_HWDEVICE_TYPE_AMF, NULL, NULL, 0);
        AMF_RETURN_IF_FALSE(ctx, ret == 0, ret, "Failed to create  hardware device context (AMF) : %s\n", av_err2str(ret));
    }

    if (ctx->pa_lookahead_buffer_depth >= ctx->hwsurfaces_in_queue_max) {
        av_log(avctx, AV_LOG_WARNING,
               "async_depth (%d) too small for lookahead (%d), increasing to (%d)\n",
                ctx->hwsurfaces_in_queue_max,
                ctx->pa_lookahead_buffer_depth,
                ctx->pa_lookahead_buffer_depth + 1);
        ctx->hwsurfaces_in_queue_max = ctx->pa_lookahead_buffer_depth + 1;
    }

    if ((ret = amf_init_encoder(avctx)) == 0) {
        return 0;
    }

    ff_amf_encode_close(avctx);
    return ret;
}

static AMF_RESULT amf_set_property_buffer(AMFSurface *object, const wchar_t *name, AMFBuffer *val)
{
    AMF_RESULT res;
    AMFVariantStruct var;
    res = AMFVariantInit(&var);
    if (res == AMF_OK) {
        AMFGuid guid_AMFInterface = IID_AMFInterface();
        AMFInterface *amf_interface;
        res = val->pVtbl->QueryInterface(val, &guid_AMFInterface, (void**)&amf_interface);

        if (res == AMF_OK) {
            res = AMFVariantAssignInterface(&var, amf_interface);
            amf_interface->pVtbl->Release(amf_interface);
        }
        if (res == AMF_OK) {
            res = object->pVtbl->SetProperty(object, name, var);
        }
        AMFVariantClear(&var);
    }
    return res;
}

static AMF_RESULT amf_store_attached_frame_ref(AMFEncoderContext *ctx, const AVFrame *frame, AMFSurface *surface)
{
    AMF_RESULT res = AMF_FAIL;
    int64_t data;
    AVFrame *frame_ref = av_frame_clone(frame);
    if (frame_ref) {
        memcpy(&data, &frame_ref, sizeof(frame_ref)); // store pointer in 8 bytes
        AMF_ASSIGN_PROPERTY_INT64(res, surface, ctx->av_frame_property_name, data);
    }
    return res;
}

static AMF_RESULT amf_release_attached_frame_ref(AMFEncoderContext *ctx, AMFBuffer *buffer)
{
    AMFVariantStruct var = {0};
    AMF_RESULT res = buffer->pVtbl->GetProperty(buffer, ctx->av_frame_property_name, &var);
    if(res == AMF_OK && var.int64Value){
        AVFrame *frame_ref;
        memcpy(&frame_ref, &var.int64Value, sizeof(frame_ref));
        av_frame_free(&frame_ref);
    }
    return frame_ref_storage_buffer;
}

static void amf_release_buffer_with_frame_ref(AMFBuffer *frame_ref_storage_buffer)
{
    AVFrame *frame_ref;
    memcpy(&frame_ref, frame_ref_storage_buffer->pVtbl->GetNative(frame_ref_storage_buffer), sizeof(frame_ref));
    av_frame_free(&frame_ref);
    frame_ref_storage_buffer->pVtbl->Release(frame_ref_storage_buffer);
}

static int amf_submit_frame(AVCodecContext *avctx, AVFrame    *frame, AMFSurface **surface_resubmit)
{
    AMFEncoderContext      *ctx = avctx->priv_data;
    AVHWDeviceContext      *hw_device_ctx = (AVHWDeviceContext*)ctx->device_ctx_ref->data;
    AVAMFDeviceContext     *amf_device_ctx = (AVAMFDeviceContext *)hw_device_ctx->hwctx;
    AMFSurface             *surface;
    AMF_RESULT              res;
    int                     ret;
    int                     hw_surface = 0;
    int                     output_delay = FFMAX(ctx->max_b_frames, 0) + ((avctx->flags & AV_CODEC_FLAG_LOW_DELAY) ? 0 : 1);

// prepare surface from frame
    switch (frame->format) {
#if CONFIG_D3D11VA
    case AV_PIX_FMT_D3D11:
        {
            static const GUID AMFTextureArrayIndexGUID = { 0x28115527, 0xe7c3, 0x4b66, { 0x99, 0xd3, 0x4f, 0x2a, 0xe6, 0xb4, 0x7f, 0xaf } };
            ID3D11Texture2D *texture = (ID3D11Texture2D*)frame->data[0]; // actual texture
            int index = (intptr_t)frame->data[1]; // index is a slice in texture array is - set to tell AMF which slice to use
                av_assert0(frame->hw_frames_ctx       && avctx->hw_frames_ctx &&
                    frame->hw_frames_ctx->data == avctx->hw_frames_ctx->data);
                texture->lpVtbl->SetPrivateData(texture, &AMFTextureArrayIndexGUID, sizeof(index), &index);
                res = amf_device_ctx->context->pVtbl->CreateSurfaceFromDX11Native(amf_device_ctx->context, texture, &surface, NULL); // wrap to AMF surface
            AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR(ENOMEM), "CreateSurfaceFromDX11Native() failed  with error %d\n", res);
                hw_surface = 1;
        }
        break;
#endif
#if CONFIG_DXVA2
    case AV_PIX_FMT_DXVA2_VLD:
        {
            IDirect3DSurface9 *texture = (IDirect3DSurface9 *)frame->data[3]; // actual texture
                res = amf_device_ctx->context->pVtbl->CreateSurfaceFromDX9Native(amf_device_ctx->context, texture, &surface, NULL); // wrap to AMF surface
            AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR(ENOMEM), "CreateSurfaceFromDX9Native() failed  with error %d\n", res);
                hw_surface = 1;
        }
        break;
#endif
    case AV_PIX_FMT_AMF_SURFACE:
        {
            surface = (AMFSurface*)frame->data[0];
            surface->pVtbl->Acquire(surface);
            hw_surface = 1;
        }
        break;
    default:
        {
            res = amf_device_ctx->context->pVtbl->AllocSurface(amf_device_ctx->context, AMF_MEMORY_HOST, ctx->format, avctx->width, avctx->height, &surface);
            AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR(ENOMEM), "AllocSurface() failed  with error %d\n", res);
            amf_copy_surface(avctx, frame, surface);
        }
        break;
    }
    if (hw_surface) {
        amf_store_attached_frame_ref(ctx, frame, surface);
        ctx->hwsurfaces_in_queue++;
        // input HW surfaces can be vertically aligned by 16; tell AMF the real size
        surface->pVtbl->SetCrop(surface, 0, 0, frame->width, frame->height);
    }
        // HDR10 metadata
    if (frame->color_trc == AVCOL_TRC_SMPTE2084) {
        AMFBuffer * hdrmeta_buffer = NULL;
        res = amf_device_ctx->context->pVtbl->AllocBuffer(amf_device_ctx->context, AMF_MEMORY_HOST, sizeof(AMFHDRMetadata), &hdrmeta_buffer);
        if (res == AMF_OK) {
            AMFHDRMetadata * hdrmeta = (AMFHDRMetadata*)hdrmeta_buffer->pVtbl->GetNative(hdrmeta_buffer);
            if (av_amf_extract_hdr_metadata(frame, hdrmeta) == 0) {
                switch (avctx->codec->id) {
                case AV_CODEC_ID_H264:
                    AMF_ASSIGN_PROPERTY_INTERFACE(res, ctx->encoder, AMF_VIDEO_ENCODER_INPUT_HDR_METADATA, hdrmeta_buffer); break;
                case AV_CODEC_ID_HEVC:
                    AMF_ASSIGN_PROPERTY_INTERFACE(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_INPUT_HDR_METADATA, hdrmeta_buffer); break;
                case AV_CODEC_ID_AV1:
                    AMF_ASSIGN_PROPERTY_INTERFACE(res, ctx->encoder, AMF_VIDEO_ENCODER_AV1_INPUT_HDR_METADATA, hdrmeta_buffer); break;
                }
                res = amf_set_property_buffer(surface, L"av_frame_hdrmeta", hdrmeta_buffer);
                AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "SetProperty failed for \"av_frame_hdrmeta\" with error %d\n", res);
            }
            hdrmeta_buffer->pVtbl->Release(hdrmeta_buffer);
        }
    }
    surface->pVtbl->SetPts(surface, frame->pts);

    AMF_ASSIGN_PROPERTY_INT64(res, surface, ctx->pts_property_name, frame->pts);

    switch (avctx->codec->id) {
    case AV_CODEC_ID_H264:
        AMF_ASSIGN_PROPERTY_BOOL(res, surface, AMF_VIDEO_ENCODER_STATISTICS_FEEDBACK, 1);
        AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_INSERT_AUD, !!ctx->aud);
        switch (frame->pict_type) {
        case AV_PICTURE_TYPE_I:
            if (ctx->forced_idr) {
                AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_INSERT_SPS, 1);
                AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_INSERT_PPS, 1);
                AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR);
            } else {
                AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_PICTURE_TYPE_I);
            }
            break;
        case AV_PICTURE_TYPE_P:
            AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_PICTURE_TYPE_P);
            break;
        case AV_PICTURE_TYPE_B:
            AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_PICTURE_TYPE_B);
            break;
        }
        break;
    case AV_CODEC_ID_HEVC:
        AMF_ASSIGN_PROPERTY_BOOL(res, surface, AMF_VIDEO_ENCODER_HEVC_STATISTICS_FEEDBACK, 1);
        AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_HEVC_INSERT_AUD, !!ctx->aud);
        switch (frame->pict_type) {
        case AV_PICTURE_TYPE_I:
            if (ctx->forced_idr) {
                AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_HEVC_INSERT_HEADER, 1);
                AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_IDR);
            } else {
                AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_I);
            }
            break;
        case AV_PICTURE_TYPE_P:
            AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_P);
            break;
        }
        break;
    case AV_CODEC_ID_AV1:
        AMF_ASSIGN_PROPERTY_BOOL(res, surface, AMF_VIDEO_ENCODER_AV1_STATISTICS_FEEDBACK, 1);
        if (frame->pict_type == AV_PICTURE_TYPE_I) {
            if (ctx->forced_idr) {
                AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_AV1_FORCE_INSERT_SEQUENCE_HEADER, 1);
                AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE, AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE_KEY);
            } else {
                AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE, AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE_INTRA_ONLY);
            }
        }
        break;
    default:
        break;
    }
    // submit surface
    res = ctx->encoder->pVtbl->SubmitInput(ctx->encoder, (AMFData*)surface);
    if (res == AMF_INPUT_FULL) { // handle full queue
        //store surface for later submission
        *surface_resubmit = surface;
    } else {
        surface->pVtbl->Release(surface);
        AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "SubmitInput() failed with error %d\n", res);
            ctx->submitted_frame++;
        ret = av_fifo_write(ctx->timestamp_list, &frame->pts, 1);
            if (ret < 0)
            return ret;
        if(ctx->submitted_frame <= ctx->encoded_frame + output_delay)
            return AVERROR(EAGAIN); // too soon to poll or wait
    }
    return 0;
}

static int amf_submit_frame_locked(AVCodecContext *avctx, AVFrame *frame, AMFSurface **surface_resubmit)
{
    int ret;
    AMFEncoderContext     *ctx = avctx->priv_data;
    AVHWDeviceContext     *hw_device_ctx = (AVHWDeviceContext*)ctx->device_ctx_ref->data;
    AVAMFDeviceContext    *amf_device_ctx = (AVAMFDeviceContext *)hw_device_ctx->hwctx;

    if (amf_device_ctx->lock)
        amf_device_ctx->lock(amf_device_ctx->lock_ctx);
    ret = amf_submit_frame(avctx, frame, surface_resubmit);
    if (amf_device_ctx->unlock)
        amf_device_ctx->unlock(amf_device_ctx->lock_ctx);

    return ret;
}
static AMF_RESULT amf_query_output(AVCodecContext *avctx, AMFBuffer **buffer)
{
    AMFEncoderContext     *ctx = avctx->priv_data;
    AMFData    *data = NULL;
    AMF_RESULT ret = ctx->encoder->pVtbl->QueryOutput(ctx->encoder, &data);
    *buffer = NULL;
    if (data) {
        AMFGuid guid = IID_AMFBuffer();
        data->pVtbl->QueryInterface(data, &guid, (void**)buffer); // query for buffer interface
        data->pVtbl->Release(data);
        if (amf_release_attached_frame_ref(ctx, *buffer) == AMF_OK)
            ctx->hwsurfaces_in_queue--;
        ctx->encoded_frame++;
    }
    return ret;
}

int ff_amf_receive_packet(AVCodecContext *avctx, AVPacket *avpkt)
{
    AMFEncoderContext     *ctx = avctx->priv_data;
    AMFSurface *surface = NULL;
    AMF_RESULT  res;
    int         ret;
    AMF_RESULT  res_query;
    AMFBuffer* buffer = NULL;
    AVFrame    *frame = av_frame_alloc();
    int         block_and_wait;
    int64_t     pts = 0;
    int output_delay = FFMAX(ctx->max_b_frames, 0) + ((avctx->flags & AV_CODEC_FLAG_LOW_DELAY) ? 0 : 1);

    if (!ctx->encoder)
        return AVERROR(EINVAL);

    if (!frame->buf[0]) {
        ret = ff_encode_get_frame(avctx, frame);
        if (ret < 0 && ret != AVERROR_EOF)
            return ret;
    }
    // check if some outputs are available
    av_fifo_read(ctx->output_list, &buffer, 1);
    if (buffer != NULL) { // return already retrieved output
        ret = amf_copy_buffer(avctx, avpkt, buffer);
        buffer->pVtbl->Release(buffer);
        return ret;
    }

    ret = ff_encode_get_frame(avctx, frame);
    if(ret < 0){
        if(ret != AVERROR_EOF){
            av_frame_free(&frame);
            if(ret == AVERROR(EAGAIN)){
                if(ctx->submitted_frame <= ctx->encoded_frame + output_delay) // too soon to poll
                    return ret;
            }
        }
    } else if (!ctx->delayed_surface) { // submit frame
        int hw_surface = 0;

        // prepare surface from frame
        switch (frame->format) {
#if CONFIG_D3D11VA
        case AV_PIX_FMT_D3D11:
            {
                static const GUID AMFTextureArrayIndexGUID = { 0x28115527, 0xe7c3, 0x4b66, { 0x99, 0xd3, 0x4f, 0x2a, 0xe6, 0xb4, 0x7f, 0xaf } };
                ID3D11Texture2D *texture = (ID3D11Texture2D*)frame->data[0]; // actual texture
                int index = (intptr_t)frame->data[1]; // index is a slice in texture array is - set to tell AMF which slice to use

                av_assert0(frame->hw_frames_ctx       && ctx->hw_frames_ctx &&
                           frame->hw_frames_ctx->data == ctx->hw_frames_ctx->data);

                texture->lpVtbl->SetPrivateData(texture, &AMFTextureArrayIndexGUID, sizeof(index), &index);

                res = ctx->context->pVtbl->CreateSurfaceFromDX11Native(ctx->context, texture, &surface, NULL); // wrap to AMF surface
                AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR(ENOMEM), "CreateSurfaceFromDX11Native() failed  with error %d\n", res);

                hw_surface = 1;
            }
        } else { // submit frame
            ret = amf_submit_frame_locked(avctx, frame, &surface);
            if(ret < 0){
                av_frame_free(&frame);
                return ret;
            }
            pts = frame->pts;
        }

        surface->pVtbl->SetPts(surface, frame->pts);
        AMF_ASSIGN_PROPERTY_INT64(res, surface, PTS_PROP, frame->pts);

        switch (avctx->codec->id) {
        case AV_CODEC_ID_H264:
            AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_INSERT_AUD, !!ctx->aud);
            switch (frame->pict_type) {
            case AV_PICTURE_TYPE_I:
                if (ctx->forced_idr) {
                    AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_INSERT_SPS, 1);
                    AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_INSERT_PPS, 1);
                    AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR);
                } else {
                    AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_PICTURE_TYPE_I);
                }
                break;
            case AV_PICTURE_TYPE_P:
                AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_PICTURE_TYPE_P);
                break;
            case AV_PICTURE_TYPE_B:
                AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_PICTURE_TYPE_B);
                break;
            }
            break;
        case AV_CODEC_ID_HEVC:
            AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_HEVC_INSERT_AUD, !!ctx->aud);
            switch (frame->pict_type) {
            case AV_PICTURE_TYPE_I:
                if (ctx->forced_idr) {
                    AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_HEVC_INSERT_HEADER, 1);
                    AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_IDR);
                } else {
                    AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_I);
                }
                break;
            case AV_PICTURE_TYPE_P:
                AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_P);
                break;
            }
            break;
        case AV_CODEC_ID_AV1:
            if (frame->pict_type == AV_PICTURE_TYPE_I) {
                if (ctx->forced_idr) {
                    AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_AV1_FORCE_INSERT_SEQUENCE_HEADER, 1);
                    AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE, AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE_KEY);
                } else {
                    AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE, AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE_INTRA_ONLY);
                }
            }
            break;
        default:
            break;
        }

        // submit surface
        res = ctx->encoder->pVtbl->SubmitInput(ctx->encoder, (AMFData*)surface);
        if (res == AMF_INPUT_FULL) { // handle full queue
            //store surface for later submission
            ctx->delayed_surface = surface;
        } else {
            int64_t pts = frame->pts;
            surface->pVtbl->Release(surface);
            AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "SubmitInput() failed with error %d\n", res);

            av_frame_unref(frame);
            ret = av_fifo_write(ctx->timestamp_list, &pts, 1);

            if (ctx->submitted_frame == 0)
            {
                ctx->use_b_frame = (ctx->max_b_frames > 0 || ((ctx->pa_adaptive_mini_gop == 1) ? true : false));
            }
            ctx->submitted_frame++;

            if (ret < 0)
                return ret;
        }
    }


    do {
        block_and_wait = 0;
        // poll data
        res_query = amf_query_output(avctx, &buffer);
        if (buffer) {
            ret = amf_copy_buffer(avctx, avpkt, buffer);
            buffer->pVtbl->Release(buffer);

                if (data->pVtbl->HasProperty(data, L"av_frame_ref")) {
                    AMFBuffer* frame_ref_storage_buffer;
                    res = amf_get_property_buffer(data, L"av_frame_ref", &frame_ref_storage_buffer);
                    AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "GetProperty failed for \"av_frame_ref\" with error %d\n", res);
                    amf_release_buffer_with_frame_ref(frame_ref_storage_buffer);
                    ctx->hwsurfaces_in_queue--;
                }

                data->pVtbl->Release(data);

                AMF_RETURN_IF_FALSE(ctx, ret >= 0, ret, "amf_copy_buffer() failed with error %d\n", ret);
            }
        } else if (ctx->delayed_drain || (ctx->eof && res_query != AMF_EOF) || (ctx->hwsurfaces_in_queue >= ctx->hwsurfaces_in_queue_max) || surface) {
            block_and_wait = 1;
            // Only sleep if the driver doesn't support waiting in QueryOutput()
            // or if we already have output data so we will skip calling it.
            if (!ctx->query_timeout_supported || avpkt->data || avpkt->buf) {
                av_usleep(1000);
            }
        }
    } while (block_and_wait);

    if (res_query == AMF_EOF) {
        ret = AVERROR_EOF;
    } else if (buffer == NULL) {
        ret = AVERROR(EAGAIN);
    } else {
        if(surface) {
            // resubmit surface
            do {
                res = ctx->encoder->pVtbl->SubmitInput(ctx->encoder, (AMFData*)surface);
                if (res != AMF_INPUT_FULL)
                    break;

                if (!ctx->query_timeout_supported)
                    av_usleep(1000);

                // Need to free up space in the encoder queue.
                // The number of retrieved outputs is limited currently to 21
                amf_query_output(avctx, &buffer);
                if (buffer != NULL) {
                    ret = av_fifo_write(ctx->output_list, &buffer, 1);
                    if (ret < 0)
                        return ret;
                }
            } while(res == AMF_INPUT_FULL);

            surface->pVtbl->Release(surface);
            if (res == AMF_INPUT_FULL) {
                av_log(avctx, AV_LOG_WARNING, "Data acquired but delayed SubmitInput returned AMF_INPUT_FULL- should not happen\n");
            } else {
                AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "SubmitInput() failed with error %d\n", res);

                ret = av_fifo_write(ctx->timestamp_list, &pts, 1);

                ctx->submitted_frame++;

                if (ret < 0)
                    return ret;
            }
        }
        ret = 0;
    }
    return ret;
}

const AVCodecHWConfigInternal *const ff_amfenc_hw_configs[] = {
#if CONFIG_D3D11VA
    HW_CONFIG_ENCODER_FRAMES(D3D11, D3D11VA),
    HW_CONFIG_ENCODER_DEVICE(NONE,  D3D11VA),
#endif
#if CONFIG_DXVA2
    HW_CONFIG_ENCODER_FRAMES(DXVA2_VLD, DXVA2),
    HW_CONFIG_ENCODER_DEVICE(NONE,      DXVA2),
#endif
    NULL,
};
