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
 * AV1 encoder (libaom), for FreeRDP's GFX AV1 extension
 *
 * FreeRDP clients that offer the capability set RDPGFX_CAPVERSION_FRDP_1
 * decode RDPGFX_CODECID_AV1: an AV1 bitstream after an
 * RFX_AVC420_METABLOCK, framed exactly as AVC420 (FreeRDP's
 * channels/rdpgfx/client/rdpgfx_codec.c). xrdp therefore runs its H.264
 * pipeline unchanged, and this encoder takes the place of x264:
 *
 *   - the input is the same: NV12, BT.709 full range, with the rects that
 *     changed. They are copied into a 4:2:0 picture (I420) kept per
 *     monitor, which is then encoded whole
 *   - libaom in real-time mode, tuned for screen content, as
 *     gnome-remote-desktop's AV1 encode session: no lag, constant
 *     quality, key frames only when the encoder is new
 *   - the client converts with BT.709 full range whatever the bitstream
 *     says; the colour description is set to match anyway
 */

#if defined(HAVE_CONFIG_H)
#include <config_ac.h>
#endif

#include <stdint.h>
#include <string.h>
#include <aom/aom_encoder.h>
#include <aom/aomcx.h>

#include "xrdp.h"
#include "arch.h"
#include "os_calls.h"
#include "xrdp_encoder_av1.h"

/* One per monitor, as the H.264 encoders: the session argument */
#define AV1_MAX_ENCODERS 16

/* Quality, 0 (best) to 63. gnome-remote-desktop chose 28 to give text
 * about the quality of its H.264 at QP 22 */
#define AV1_CQ_LEVEL 28

/* libaom's fastest real-time speed */
#define AV1_CPU_USED 10

/* Threads per encoder: rows are encoded in parallel (row-mt) */
#define AV1_THREADS 2

/* The frame rate the encoder plans its rate control for */
#define AV1_FPS 60

struct av1_encoder
{
    aom_codec_ctx_t ctx;
    int open;
    uint8_t *yuvdata;   /* I420: Y, then U, then V */
    aom_image_t image;  /* yuvdata, as libaom sees it */
    int width;          /* the size asked for */
    int height;
    int pic_width;      /* rounded up to even, for 4:2:0 */
    int pic_height;
    int64_t pts;
};

struct av1_global
{
    struct av1_encoder encoders[AV1_MAX_ENCODERS];
};

/*****************************************************************************/
void *
xrdp_encoder_av1_create(void)
{
    LOG_DEVEL(LOG_LEVEL_TRACE, "xrdp_encoder_av1_create:");
    return g_new0(struct av1_global, 1);
}

/*****************************************************************************/
static void
av1_close(struct av1_encoder *ae)
{
    if (ae->open)
    {
        aom_codec_destroy(&ae->ctx);
        ae->open = 0;
    }
    g_free(ae->yuvdata);
    ae->yuvdata = NULL;
}

/*****************************************************************************/
int
xrdp_encoder_av1_delete(void *handle)
{
    struct av1_global *ag = (struct av1_global *)handle;
    int index;

    if (ag == NULL)
    {
        return 0;
    }
    for (index = 0; index < AV1_MAX_ENCODERS; index++)
    {
        av1_close(&ag->encoders[index]);
    }
    g_free(ag);
    return 0;
}

/*****************************************************************************/
#define AV1_CONTROL(_ctx, _id, _value) \
    if (aom_codec_control(_ctx, _id, _value) != AOM_CODEC_OK) \
    { \
        LOG(LOG_LEVEL_WARNING, "xrdp_encoder_av1: %s failed: %s", #_id, \
            aom_codec_error(_ctx)); \
    }

/* Opens an encoder for width x height. Returns 0 on success */
static int
av1_open(struct av1_encoder *ae, int width, int height)
{
    aom_codec_iface_t *iface = aom_codec_av1_cx();
    aom_codec_enc_cfg_t cfg;
    size_t y_bytes;
    aom_codec_err_t err;

    ae->pic_width = (width + 1) & ~1;
    ae->pic_height = (height + 1) & ~1;
    err = aom_codec_enc_config_default(iface, &cfg, AOM_USAGE_REALTIME);
    if (err != AOM_CODEC_OK)
    {
        LOG(LOG_LEVEL_ERROR, "xrdp_encoder_av1: no default config: %s",
            aom_codec_err_to_string(err));
        return 1;
    }
    cfg.g_w = ae->pic_width;
    cfg.g_h = ae->pic_height;
    cfg.g_profile = 0;          /* 4:2:0 */
    cfg.g_timebase.num = 1;
    cfg.g_timebase.den = AV1_FPS;
    cfg.g_threads = AV1_THREADS;
    cfg.g_lag_in_frames = 0;    /* a frame out for each frame in */
    cfg.rc_end_usage = AOM_Q;   /* constant quality, AV1_CQ_LEVEL */
    cfg.kf_mode = AOM_KF_DISABLED;
    err = aom_codec_enc_init(&ae->ctx, iface, &cfg, 0);
    if (err != AOM_CODEC_OK)
    {
        LOG(LOG_LEVEL_ERROR, "xrdp_encoder_av1: aom_codec_enc_init: %s",
            aom_codec_err_to_string(err));
        return 1;
    }
    ae->open = 1;
    AV1_CONTROL(&ae->ctx, AOME_SET_CPUUSED, AV1_CPU_USED);
    AV1_CONTROL(&ae->ctx, AOME_SET_CQ_LEVEL, AV1_CQ_LEVEL);
    AV1_CONTROL(&ae->ctx, AV1E_SET_TUNE_CONTENT, AOM_CONTENT_SCREEN);
    AV1_CONTROL(&ae->ctx, AV1E_SET_ROW_MT, 1);
    AV1_CONTROL(&ae->ctx, AV1E_SET_COLOR_RANGE, AOM_CR_FULL_RANGE);
    AV1_CONTROL(&ae->ctx, AV1E_SET_COLOR_PRIMARIES, AOM_CICP_CP_BT_709);
    AV1_CONTROL(&ae->ctx, AV1E_SET_TRANSFER_CHARACTERISTICS,
                AOM_CICP_TC_BT_709);
    AV1_CONTROL(&ae->ctx, AV1E_SET_MATRIX_COEFFICIENTS, AOM_CICP_MC_BT_709);

    /* Black until painted: the first frame covers everything anyway */
    y_bytes = (size_t)ae->pic_width * ae->pic_height;
    ae->yuvdata = g_new(uint8_t, y_bytes + y_bytes / 2);
    if (ae->yuvdata == NULL)
    {
        av1_close(ae);
        return 1;
    }
    memset(ae->yuvdata, 0, y_bytes);
    memset(ae->yuvdata + y_bytes, 128, y_bytes / 2);
    aom_img_wrap(&ae->image, AOM_IMG_FMT_I420, ae->pic_width,
                 ae->pic_height, 1, ae->yuvdata);
    ae->width = width;
    ae->height = height;
    ae->pts = 0;
    LOG(LOG_LEVEL_INFO, "xrdp_encoder_av1: encoder for %dx%d (picture %dx%d)",
        width, height, ae->pic_width, ae->pic_height);
    return 0;
}

/*****************************************************************************/
/* Copies a rect of NV12 (Y plane, then interleaved UV, with a stride of
 * twidth) into the I420 picture. x, y, cx and cy are even, as the H.264
 * copy rects are, and relative to the picture once left, top is taken
 * off */
static void
copy_nv12_rect(struct av1_encoder *ae, const char *data, int twidth,
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
/* Returns 0 on success, as xrdp_encoder_x264_encode(): 1 or 2 when no
 * encoder could be made, 3 when it gave no frame, 4 when the frame did
 * not fit cdata. flags: 1 a new encoder (a key frame), 2 the old one
 * was closed */
int
xrdp_encoder_av1_encode(void *handle, int session, int left, int top,
                        int width, int height, int twidth, int theight,
                        int format, const char *data,
                        short *crects, int num_crects,
                        char *cdata, int *cdata_bytes, int connection_type,
                        int *flags_ptr)
{
    struct av1_global *ag = (struct av1_global *)handle;
    struct av1_encoder *ae = &ag->encoders[session % AV1_MAX_ENCODERS];
    const aom_codec_cx_pkt_t *pkt;
    aom_codec_iter_t iter = NULL;
    aom_codec_err_t err;
    int flags = 0;
    int out_bytes = 0;
    int index;

    if (!ae->open || ae->width != width || ae->height != height)
    {
        if (ae->open)
        {
            av1_close(ae);
            flags |= 2;
        }
        if (width <= 0 || height <= 0)
        {
            return 1;
        }
        if (av1_open(ae, width, height) != 0)
        {
            return 2;
        }
        flags |= 1;
    }
    if (data != NULL)
    {
        for (index = 0; index < num_crects; index++)
        {
            copy_nv12_rect(ae, data, twidth, theight, left, top,
                           crects[index * 4], crects[index * 4 + 1],
                           crects[index * 4 + 2], crects[index * 4 + 3]);
        }
        err = aom_codec_encode(&ae->ctx, &ae->image, ae->pts++, 1, 0);
        if (err != AOM_CODEC_OK)
        {
            LOG(LOG_LEVEL_ERROR, "xrdp_encoder_av1: aom_codec_encode: %s",
                aom_codec_error(&ae->ctx));
            return 3;
        }
        /* A temporal unit: all the packets of the frame, one after the
         * other, as dav1d reads them */
        while ((pkt = aom_codec_get_cx_data(&ae->ctx, &iter)) != NULL)
        {
            if (pkt->kind != AOM_CODEC_CX_FRAME_PKT)
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
