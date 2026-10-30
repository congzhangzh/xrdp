/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * AV2 encoder (libavm), for FreeRDP's GFX AV2 extension
 *
 * FreeRDP clients that offer the capability set RDPGFX_CAPVERSION_FRDP_2
 * decode RDPGFX_CODECID_AV2: an AV2 bitstream after an
 * RFX_AVC420_METABLOCK, framed exactly as AVC420 and as AV1 (FreeRDP's
 * channels/rdpgfx/client/rdpgfx_codec.c). This encoder is the AV1 one
 * (xrdp_encoder_av1.c) with libavm, the AV2 reference, in place of
 * libaom:
 *
 *   - the same input: NV12, BT.709 full range, with the rects that
 *     changed, copied into a 4:2:0 picture (I420) kept per monitor; or
 *     XRDP_yuv444_709fr (wlup) into an I444 picture. 4:4:4 is AV2
 *     profile 4 (MAIN_444_10_IP1), not 1 as in AV1: AV2 profile 1 is
 *     another 4:2:0 profile, and libavm refuses I444 pictures with it
 *   - 8 bits a sample in and out, as FreeRDP's own AV2 encoder
 *   - libavm in its real-time usage, tuned for screen content, rows and
 *     tile columns on several threads: the settings measured at
 *     2560x1440 on 4 cores. No lag, constant quality (qp), key frames
 *     only when the encoder is new
 *   - the client converts with BT.709 full range whatever the bitstream
 *     says; the colour description is set to match anyway
 */

#if defined(HAVE_CONFIG_H)
#include <config_ac.h>
#endif

#include <stdint.h>
#include <string.h>
#include <avm/avm_encoder.h>
#include <avm/avmcx.h>

#include "xrdp.h"
#include "arch.h"
#include "os_calls.h"
#include "xrdp_encoder_av2.h"
#include "xrdp_tconfig.h"

/* One per monitor, as the H.264 encoders: the session argument */
#define AV2_MAX_ENCODERS 16

/* [AV2] Annex A, Table A.1 */
#define AV2_PROFILE_MAIN_420_10_IP0 0
#define AV2_PROFILE_MAIN_444_10_IP1 4

/* The real-time usage is newer than libavm v1.0.0 (FreeRDP's
 * libfreerdp/codec/av2.c does the same) */
#if !defined(AVM_USAGE_REALTIME)
#define AVM_USAGE_REALTIME AVM_USAGE_GOOD_QUALITY
#endif

/* libavm v1.0.0 takes cpu_used 0 to 9; newer ones 10 too */
#define AV2_OLD_MAX_CPU_USED 9

/* Timestamps are in milliseconds. xrdp encodes when the screen changes,
 * so frames come at any rate: each one lasts from the previous one to
 * now, which is what the rate control plans with */
#define AV2_TIMEBASE_DEN 1000
/* The first frame has no previous one: as if at 60 frames a second */
#define AV2_FIRST_DURATION_MS 16
/* After an idle screen, a frame counts as 1 s at most */
#define AV2_MAX_DURATION_MS 1000

struct av2_encoder
{
    avm_codec_ctx_t ctx;
    int open;
    int yuv444;         /* I444 (profile 4), else I420 */
    uint8_t *yuvdata;   /* Y, then U, then V */
    avm_image_t image;  /* yuvdata, as libavm sees it */
    int width;          /* the size asked for */
    int height;
    int pic_width;      /* rounded up to even, for 4:2:0 */
    int pic_height;
    int64_t pts;            /* the frames' durations so far, in ms */
    unsigned int last_ms;   /* g_get_elapsed_ms() at the previous frame */
};

struct av2_global
{
    struct av2_encoder encoders[AV2_MAX_ENCODERS];
    /* from gfx.toml, for each connection type */
    struct xrdp_tconfig_gfx_av2_param av2_param[NUM_CONNECTION_TYPES];
};

/*****************************************************************************/
void *
xrdp_encoder_av2_create(void)
{
    struct av2_global *ag;
    struct xrdp_tconfig_gfx gfxconfig;

    LOG_DEVEL(LOG_LEVEL_TRACE, "xrdp_encoder_av2_create:");
    ag = g_new0(struct av2_global, 1);
    if (ag == NULL)
    {
        return NULL;
    }
    /* the AV2 parameters keep their defaults if this fails */
    tconfig_load_gfx(GFX_CONF, &gfxconfig);
    memcpy(ag->av2_param, gfxconfig.av2_param, sizeof(ag->av2_param));
    return ag;
}

/*****************************************************************************/
static void
av2_close(struct av2_encoder *ae)
{
    if (ae->open)
    {
        avm_codec_destroy(&ae->ctx);
        ae->open = 0;
    }
    g_free(ae->yuvdata);
    ae->yuvdata = NULL;
}

/*****************************************************************************/
int
xrdp_encoder_av2_delete(void *handle)
{
    struct av2_global *ag = (struct av2_global *)handle;
    int index;

    if (ag == NULL)
    {
        return 0;
    }
    for (index = 0; index < AV2_MAX_ENCODERS; index++)
    {
        av2_close(&ag->encoders[index]);
    }
    g_free(ag);
    return 0;
}

/*****************************************************************************/
/* Returns 0 on success */
static int
av2_control(avm_codec_ctx_t *ctx, int id, const char *name, int value)
{
    if (avm_codec_control(ctx, id, value) != AVM_CODEC_OK)
    {
        LOG(LOG_LEVEL_WARNING, "xrdp_encoder_av2: %s %d failed: %s", name,
            value, avm_codec_error(ctx));
        return 1;
    }
    return 0;
}

#define AV2_CONTROL(_ctx, _id, _value) \
    av2_control(_ctx, _id, #_id, _value)

/* Opens an encoder for width x height. Returns 0 on success */
static int
av2_open(struct av2_encoder *ae, int width, int height, int yuv444,
         const struct xrdp_tconfig_gfx_av2_param *param)
{
    avm_codec_iface_t *iface = avm_codec_av2_cx();
    avm_codec_enc_cfg_t cfg;
    size_t y_bytes;
    size_t uv_bytes;
    avm_codec_err_t err;
    int cpu_used = param->cpu_used;
    char rc[32];

    /* even for 4:2:0; the client crops what is beyond the surface */
    ae->pic_width = (width + 1) & ~1;
    ae->pic_height = (height + 1) & ~1;
    ae->yuv444 = yuv444;
    err = avm_codec_enc_config_default(iface, &cfg, AVM_USAGE_REALTIME);
    if (err != AVM_CODEC_OK)
    {
        LOG(LOG_LEVEL_ERROR, "xrdp_encoder_av2: no default config: %s",
            avm_codec_err_to_string(err));
        return 1;
    }
    cfg.g_usage = AVM_USAGE_REALTIME;
    cfg.g_w = ae->pic_width;
    cfg.g_h = ae->pic_height;
    cfg.g_profile = yuv444 ? AV2_PROFILE_MAIN_444_10_IP1 :
                    AV2_PROFILE_MAIN_420_10_IP0;
    cfg.g_bit_depth = AVM_BITS_8;
    cfg.g_input_bit_depth = 8;
    cfg.g_timebase.num = 1;
    cfg.g_timebase.den = AV2_TIMEBASE_DEN;
    cfg.g_threads = param->threads; /* rows and tiles in parallel */
    cfg.g_lag_in_frames = 0;    /* a frame out for each frame in */
    if (param->target_bitrate > 0)
    {
        /* libavm's CBR, with its default buffer */
        cfg.rc_end_usage = AVM_CBR;
        cfg.rc_target_bitrate = param->target_bitrate;
    }
    else
    {
        cfg.rc_end_usage = AVM_Q; /* constant quality, qp */
    }
    cfg.kf_mode = AVM_KF_DISABLED;
    err = avm_codec_enc_init(&ae->ctx, iface, &cfg, 0);
    if (err != AVM_CODEC_OK)
    {
        LOG(LOG_LEVEL_ERROR, "xrdp_encoder_av2: avm_codec_enc_init: %s",
            avm_codec_err_to_string(err));
        return 1;
    }
    ae->open = 1;
    if (AV2_CONTROL(&ae->ctx, AVME_SET_CPUUSED, cpu_used) != 0 &&
            cpu_used > AV2_OLD_MAX_CPU_USED)
    {
        /* An older libavm: its fastest speed instead of its slowest */
        cpu_used = AV2_OLD_MAX_CPU_USED;
        LOG(LOG_LEVEL_WARNING, "xrdp_encoder_av2: cpu_used %d instead",
            cpu_used);
        AV2_CONTROL(&ae->ctx, AVME_SET_CPUUSED, cpu_used);
    }
    if (param->target_bitrate <= 0)
    {
        AV2_CONTROL(&ae->ctx, AVME_SET_QP, param->qp);
    }
    AV2_CONTROL(&ae->ctx, AV2E_SET_TUNE_CONTENT, AVM_CONTENT_SCREEN);
    AV2_CONTROL(&ae->ctx, AV2E_SET_ROW_MT, 1);
    AV2_CONTROL(&ae->ctx, AV2E_SET_TILE_COLUMNS, param->tile_columns);
    AV2_CONTROL(&ae->ctx, AV2E_SET_COLOR_RANGE, AVM_CR_FULL_RANGE);
    AV2_CONTROL(&ae->ctx, AV2E_SET_COLOR_PRIMARIES, AVM_CICP_CP_BT_709);
    AV2_CONTROL(&ae->ctx, AV2E_SET_TRANSFER_CHARACTERISTICS,
                AVM_CICP_TC_BT_709);
    AV2_CONTROL(&ae->ctx, AV2E_SET_MATRIX_COEFFICIENTS, AVM_CICP_MC_BT_709);

    /* Black until painted: the first frame covers everything anyway */
    y_bytes = (size_t)ae->pic_width * ae->pic_height;
    uv_bytes = yuv444 ? y_bytes * 2 : y_bytes / 2;
    ae->yuvdata = g_new(uint8_t, y_bytes + uv_bytes);
    if (ae->yuvdata == NULL)
    {
        av2_close(ae);
        return 1;
    }
    memset(ae->yuvdata, 0, y_bytes);
    memset(ae->yuvdata + y_bytes, 128, uv_bytes);
    avm_img_wrap(&ae->image, yuv444 ? AVM_IMG_FMT_I444 : AVM_IMG_FMT_I420,
                 ae->pic_width, ae->pic_height, 1, ae->yuvdata);
    ae->image.bit_depth = 8;
    ae->width = width;
    ae->height = height;
    ae->pts = 0;
    ae->last_ms = g_get_elapsed_ms();
    if (param->target_bitrate > 0)
    {
        g_snprintf(rc, sizeof(rc), "CBR %d kbps", param->target_bitrate);
    }
    else
    {
        g_snprintf(rc, sizeof(rc), "qp %d", param->qp);
    }
    LOG(LOG_LEVEL_INFO, "xrdp_encoder_av2: encoder for %dx%d (picture %dx%d, "
        "%s, cpu-used %d, %s, %d thread%s, %d tile column%s)", width, height,
        ae->pic_width, ae->pic_height, yuv444 ? "4:4:4" : "4:2:0", cpu_used,
        rc, param->threads, param->threads == 1 ? "" : "s",
        1 << param->tile_columns, param->tile_columns == 0 ? "" : "s");
    return 0;
}

/*****************************************************************************/
/* Copies a rect of NV12 (Y plane, then interleaved UV, with a stride of
 * twidth) into the I420 picture. x, y, cx and cy are even, as the H.264
 * copy rects are, and relative to the picture once left, top is taken
 * off */
static void
copy_nv12_rect(struct av2_encoder *ae, const char *data, int twidth,
               int theight, int left, int top, int x, int y, int cx, int cy)
{
    const uint8_t *src_y = (const uint8_t *)data;
    const uint8_t *src_uv = src_y + (size_t)twidth * theight;
    int pw = ae->pic_width;
    uint8_t *dst_u = ae->yuvdata + (size_t)pw * ae->pic_height;
    uint8_t *dst_v = dst_u + (size_t)(pw / 2) * (ae->pic_height / 2);
    int dx = x - left;
    int dy = y - top;
    int row;
    int i;

    /* What does not fit the picture is dropped */
    cx = MIN(cx, pw - dx);
    cy = MIN(cy, ae->pic_height - dy);
    if (dx < 0 || dy < 0 || cx <= 0 || cy <= 0)
    {
        return;
    }
    for (row = 0; row < cy; row++)
    {
        memcpy(ae->yuvdata + (size_t)(dy + row) * pw + dx,
               src_y + (size_t)(y + row) * twidth + x, cx);
    }
    for (row = 0; row < cy / 2; row++)
    {
        const uint8_t *s = src_uv + (size_t)(y / 2 + row) * twidth + x;
        uint8_t *u = dst_u + (size_t)(dy / 2 + row) * (pw / 2) + dx / 2;
        uint8_t *v = dst_v + (size_t)(dy / 2 + row) * (pw / 2) + dx / 2;

        for (i = 0; i < cx / 2; i++)
        {
            u[i] = s[i * 2];
            v[i] = s[i * 2 + 1];
        }
    }
}

/*****************************************************************************/
/* Copies a rect of XRDP_yuv444_709fr (a 32-bit pixel each, with a stride
 * of twidth pixels) into the I444 picture */
static void
copy_yuv444_rect(struct av2_encoder *ae, const char *data, int twidth,
                 int left, int top, int x, int y, int cx, int cy)
{
    int pw = ae->pic_width;
    size_t plane = (size_t)pw * ae->pic_height;
    int dx = x - left;
    int dy = y - top;
    int row;
    int i;

    cx = MIN(cx, pw - dx);
    cy = MIN(cy, ae->pic_height - dy);
    if (dx < 0 || dy < 0 || cx <= 0 || cy <= 0)
    {
        return;
    }
    for (row = 0; row < cy; row++)
    {
        const uint32_t *s = (const uint32_t *)data +
                            (size_t)(y + row) * twidth + x;
        uint8_t *d = ae->yuvdata + (size_t)(dy + row) * pw + dx;

        for (i = 0; i < cx; i++)
        {
            d[i] = s[i] & 0xff;                     /* Y */
            d[plane + i] = (s[i] >> 8) & 0xff;      /* U */
            d[plane * 2 + i] = (s[i] >> 16) & 0xff; /* V */
        }
    }
}

/*****************************************************************************/
/* Returns 0 on success, as xrdp_encoder_x264_encode(): 1 or 2 when no
 * encoder could be made, 3 when it gave no frame, 4 when the frame did
 * not fit cdata. flags: 1 a new encoder (a key frame), 2 the old one
 * was closed */
int
xrdp_encoder_av2_encode(void *handle, int session, int left, int top,
                        int width, int height, int twidth, int theight,
                        int format, const char *data,
                        short *crects, int num_crects,
                        char *cdata, int *cdata_bytes, int connection_type,
                        int *flags_ptr)
{
    struct av2_global *ag = (struct av2_global *)handle;
    struct av2_encoder *ae = &ag->encoders[session % AV2_MAX_ENCODERS];
    const avm_codec_cx_pkt_t *pkt;
    avm_codec_iter_t iter = NULL;
    avm_codec_err_t err;
    int flags = 0;
    int out_bytes = 0;
    int index;
    int yuv444 = format == XRDP_yuv444_709fr;
    int ct = connection_type;
    unsigned int now_ms;
    unsigned int duration;

    /* as xrdp_encoder_x264_encode() */
    if (ct > CONNECTION_TYPE_LAN || ct < CONNECTION_TYPE_MODEM)
    {
        ct = CONNECTION_TYPE_LAN;
    }

    if (!ae->open || ae->width != width || ae->height != height ||
            ae->yuv444 != yuv444)
    {
        if (ae->open)
        {
            av2_close(ae);
            flags |= 2;
        }
        if (width <= 0 || height <= 0)
        {
            return 1;
        }
        if (av2_open(ae, width, height, yuv444, &ag->av2_param[ct]) != 0)
        {
            return 2;
        }
        flags |= 1;
    }
    if (data != NULL)
    {
        for (index = 0; index < num_crects; index++)
        {
            if (yuv444)
            {
                copy_yuv444_rect(ae, data, twidth, left, top,
                                 crects[index * 4], crects[index * 4 + 1],
                                 crects[index * 4 + 2], crects[index * 4 + 3]);
            }
            else
            {
                copy_nv12_rect(ae, data, twidth, theight, left, top,
                               crects[index * 4], crects[index * 4 + 1],
                               crects[index * 4 + 2], crects[index * 4 + 3]);
            }
        }
        now_ms = g_get_elapsed_ms();
        if (flags & 1)
        {
            duration = AV2_FIRST_DURATION_MS;
        }
        else
        {
            duration = MAX(1, MIN(now_ms - ae->last_ms, AV2_MAX_DURATION_MS));
            ae->pts += duration;
        }
        ae->last_ms = now_ms;
        err = avm_codec_encode(&ae->ctx, &ae->image, ae->pts, duration, 0);
        if (err != AVM_CODEC_OK)
        {
            LOG(LOG_LEVEL_ERROR, "xrdp_encoder_av2: avm_codec_encode: %s",
                avm_codec_error(&ae->ctx));
            return 3;
        }
        /* A temporal unit: all the packets of the frame, one after the
         * other, as the client's decoder reads them */
        while ((pkt = avm_codec_get_cx_data(&ae->ctx, &iter)) != NULL)
        {
            if (pkt->kind != AVM_CODEC_CX_FRAME_PKT)
            {
                continue;
            }
            if (out_bytes + (int)pkt->data.frame.sz > *cdata_bytes)
            {
                return 4;
            }
            memcpy(cdata + out_bytes, pkt->data.frame.buf,
                   pkt->data.frame.sz);
            out_bytes += (int)pkt->data.frame.sz;
        }
        if (out_bytes < 1)
        {
            return 3;
        }
        *cdata_bytes = out_bytes;
    }
    if (flags_ptr != NULL)
    {
        *flags_ptr = flags;
    }
    return 0;
}
