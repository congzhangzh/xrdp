/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Copyright (C) Koichiro Iwao
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
 */

/**
 *
 * @file xrdp_tconfig.c
 * @brief TOML config loader
 * @author Koichiro Iwao
 *
 */

#if defined(HAVE_CONFIG_H)
#include <config_ac.h>
#endif

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arch.h"
#include "os_calls.h"
#include "parse.h"
#include "toml.h"
#include "ms-rdpbcgr.h"
#include "xrdp_tconfig.h"
#include "string_calls.h"

#define TCLOG(log_level, args...) LOG(log_level, "TConfig: " args)

#define X264_DEFAULT_PRESET  "ultrafast"
#define X264_DEFAULT_TUNE    "zerolatency"
#define X264_DEFAULT_PROFILE "main"
#define X264_DEFAULT_FPS_NUM 24
#define X264_DEFAULT_FPS_DEN 1
#define X264_DEFAULT_THREADS 1 /* not to exhaust CPU threads for 1 user */

#define AV1_DEFAULT_THREADS 2 /* rows are encoded in parallel (row-mt) */
#define AV1_MAX_THREADS 64 /* libaom refuses more */
/* libaom's fastest real-time speed when xrdp's AV1 was written. 8 makes
 * about 10% smaller frames, at 1.6 times the encoding time */
#define AV1_DEFAULT_CPU_USED 10
/* gnome-remote-desktop's choice: text about as sharp as its H.264 at
 * QP 22 */
#define AV1_DEFAULT_CQ_LEVEL 28

/* AV2 (libavm). The settings measured at 2560x1440 on 4 cores */
#define AV2_DEFAULT_THREADS 4 /* rows (row-mt) and tiles in parallel */
#define AV2_MAX_THREADS 64
/* The fastest real-time speed of libavm 1.0. 10 is only valid in newer
 * libavm: where it is not, the encoder takes 9 */
#define AV2_DEFAULT_CPU_USED 9
/* A quantizer index, 0 to 255 at 8 bits. 160 is the one of the 1440p
 * measurements: a desktop at 54 dB PSNR, about 0.5 Mbps at 30 fps */
#define AV2_DEFAULT_QP 160
/* log2: 2 tile columns, for the threads to work on */
#define AV2_DEFAULT_TILE_COLUMNS 1

/* The integer parameters of [AV1.<connection type>] and
 * [AV2.<connection type>]: the key, the environment variable that
 * overrides it for every connection type, the valid range and the
 * default */
struct video_param_def
{
    const char *key;
    const char *env;
    size_t offset;
    int min;
    int max;
    int def;
};

/* A video codec's section of gfx.toml: its parameters, kept in a struct
 * for each connection type, one of which is the chroma setting */
struct video_codec_def
{
    const char *section;            /* "AV1": [AV1.<connection type>] */
    const struct video_param_def *params;
    unsigned int param_count;
    size_t param_size;              /* the struct of one connection type */
    size_t chroma_offset;           /* its enum xrdp_tconfig_av1_chroma */
    const char *chroma_env;         /* XRDP_GFX_AV1_CHROMA */
};

#define AV1_PARAM_OFFSET(member) \
    offsetof(struct xrdp_tconfig_gfx_av1_param, member)

static const struct video_param_def av1_params[] =
{
    {
        "threads", "XRDP_GFX_AV1_THREADS", AV1_PARAM_OFFSET(threads),
        1, AV1_MAX_THREADS, AV1_DEFAULT_THREADS
    },
    {
        "cpu_used", "XRDP_GFX_AV1_CPU_USED", AV1_PARAM_OFFSET(cpu_used),
        5, 12, AV1_DEFAULT_CPU_USED
    },
    {
        "cq_level", "XRDP_GFX_AV1_CQ_LEVEL", AV1_PARAM_OFFSET(cq_level),
        0, 63, AV1_DEFAULT_CQ_LEVEL
    },
    {
        "target_bitrate", "XRDP_GFX_AV1_TARGET_BITRATE",
        AV1_PARAM_OFFSET(target_bitrate), 0, 1000000, 0
    }
};

#define AV1_PARAM_COUNT (sizeof(av1_params) / sizeof(av1_params[0]))
#define AV1_PARAM_SIZE sizeof(struct xrdp_tconfig_gfx_av1_param)

static const struct video_codec_def av1_codec =
{
    "AV1", av1_params, AV1_PARAM_COUNT,
    AV1_PARAM_SIZE, AV1_PARAM_OFFSET(chroma),
    "XRDP_GFX_AV1_CHROMA"
};

#define AV2_PARAM_OFFSET(member) \
    offsetof(struct xrdp_tconfig_gfx_av2_param, member)

static const struct video_param_def av2_params[] =
{
    {
        "threads", "XRDP_GFX_AV2_THREADS", AV2_PARAM_OFFSET(threads),
        1, AV2_MAX_THREADS, AV2_DEFAULT_THREADS
    },
    {
        "cpu_used", "XRDP_GFX_AV2_CPU_USED", AV2_PARAM_OFFSET(cpu_used),
        0, 10, AV2_DEFAULT_CPU_USED
    },
    {
        "qp", "XRDP_GFX_AV2_QP", AV2_PARAM_OFFSET(qp),
        0, 255, AV2_DEFAULT_QP
    },
    {
        "target_bitrate", "XRDP_GFX_AV2_TARGET_BITRATE",
        AV2_PARAM_OFFSET(target_bitrate), 0, 1000000, 0
    },
    {
        "tile_columns", "XRDP_GFX_AV2_TILE_COLUMNS",
        AV2_PARAM_OFFSET(tile_columns), 0, 6, AV2_DEFAULT_TILE_COLUMNS
    }
};

#define AV2_PARAM_COUNT (sizeof(av2_params) / sizeof(av2_params[0]))
#define AV2_PARAM_SIZE sizeof(struct xrdp_tconfig_gfx_av2_param)

static const struct video_codec_def av2_codec =
{
    "AV2", av2_params, AV2_PARAM_COUNT,
    AV2_PARAM_SIZE, AV2_PARAM_OFFSET(chroma),
    "XRDP_GFX_AV2_CHROMA"
};

/* The struct of connection type ct in an array of them */
static void *
video_param_at(const struct video_codec_def *cd, void *params, int ct)
{
    return (char *)params + cd->param_size * ct;
}

static int *
video_param_ptr(const struct video_codec_def *cd, void *param,
                unsigned int i)
{
    return (int *)((char *)param + cd->params[i].offset);
}

static enum xrdp_tconfig_av1_chroma *
video_chroma_ptr(const struct video_codec_def *cd, void *param)
{
    char *p = (char *)param + cd->chroma_offset;

    return (enum xrdp_tconfig_av1_chroma *)p;
}

const char *
tconfig_av1_chroma_to_str(enum xrdp_tconfig_av1_chroma chroma)
{
    switch (chroma)
    {
        case XTC_AV1_CHROMA_420:
            return "420";
        case XTC_AV1_CHROMA_444:
            return "444";
        default:
            return "auto";
    }
}

/* "auto", "420" or "444". Returns 0 if s is one of them */
static int
av1_chroma_from_str(const char *s, enum xrdp_tconfig_av1_chroma *chroma)
{
    if (g_strcasecmp(s, "auto") == 0)
    {
        *chroma = XTC_AV1_CHROMA_AUTO;
    }
    else if (g_strcmp(s, "420") == 0)
    {
        *chroma = XTC_AV1_CHROMA_420;
    }
    else if (g_strcmp(s, "444") == 0)
    {
        *chroma = XTC_AV1_CHROMA_444;
    }
    else
    {
        return 1;
    }
    return 0;
}

const char *
tconfig_codec_order_to_str(
    const struct xrdp_tconfig_gfx_codec_order *codec_order,
    char *buff,
    unsigned int bufflen)
{
    if (bufflen < (8 * codec_order->codec_count))
    {
        snprintf(buff, bufflen, "???");
    }
    else
    {
        unsigned int p = 0;
        int i;
        for (i = 0 ; i < codec_order->codec_count; ++i)
        {
            if (p > 0)
            {
                buff[p++] = ',';
                buff[p++] = ' ';
            }

            switch (codec_order->codecs[i])
            {
                case XTC_H264:
                    buff[p++] = 'H';
                    buff[p++] = '2';
                    buff[p++] = '6';
                    buff[p++] = '4';
                    break;

                case XTC_RFX:
                    buff[p++] = 'R';
                    buff[p++] = 'F';
                    buff[p++] = 'X';
                    break;

                case XTC_AV1:
                    buff[p++] = 'A';
                    buff[p++] = 'V';
                    buff[p++] = '1';
                    break;

                case XTC_AV2:
                    buff[p++] = 'A';
                    buff[p++] = 'V';
                    buff[p++] = '2';
                    break;

                default:
                    buff[p++] = '?';
                    buff[p++] = '?';
                    buff[p++] = '?';
            }
        }
        buff[p++] = '\0';
    }

    return buff;
}

static int
tconfig_load_gfx_openh264_ct(toml_table_t *tfile, const int connection_type,
                             struct xrdp_tconfig_gfx_openh264_param *param)
{
    TCLOG(LOG_LEVEL_TRACE, "[OpenH264]");

    if (connection_type > NUM_CONNECTION_TYPES)
    {
        TCLOG(LOG_LEVEL_ERROR, "[OpenH264] Invalid connection type is given");
        return 1;
    }

    toml_table_t *oh264 = toml_table_in(tfile, "OpenH264");
    if (!oh264)
    {
        TCLOG(LOG_LEVEL_WARNING, "[OpenH264] OpenH264 params are not defined");
        return 1;
    }

    toml_table_t *oh264_ct =
        toml_table_in(oh264, rdpbcgr_connection_type_names[connection_type]);
    toml_datum_t datum;

    if (!oh264_ct)
    {
        TCLOG(LOG_LEVEL_WARNING, "OpenH264 params for connection type [%s] is not defined",
              rdpbcgr_connection_type_names[connection_type]);
        return 1;
    }

    /* EnableFrameSkip */
    datum = toml_bool_in(oh264_ct, "EnableFrameSkip");
    if (datum.ok)
    {
        param[connection_type].EnableFrameSkip = datum.u.b;
    }
    else if (connection_type == 0)
    {
        TCLOG(LOG_LEVEL_WARNING,
              "[OpenH264.%s] EnableFrameSkip is not set, adopting the default value [0]",
              rdpbcgr_connection_type_names[connection_type]);
        param[connection_type].EnableFrameSkip = 0;
    }

    /* TargetBitrate */
    datum = toml_int_in(oh264_ct, "TargetBitrate");
    if (datum.ok)
    {
        param[connection_type].TargetBitrate = datum.u.i;
    }
    else if (connection_type == 0)
    {
        TCLOG(LOG_LEVEL_WARNING,
              "[OpenH264.%s] TargetBitrate is not set, adopting the default value [0]",
              rdpbcgr_connection_type_names[connection_type]);
        param[connection_type].TargetBitrate = 0;
    }

    /* MaxBitrate */
    datum = toml_int_in(oh264_ct, "MaxBitrate");
    if (datum.ok)
    {
        param[connection_type].MaxBitrate = datum.u.i;
    }
    else if (connection_type == 0)
    {
        TCLOG(LOG_LEVEL_WARNING,
              "[OpenH264.%s] MaxBitrate is not set, adopting the default value [0]",
              rdpbcgr_connection_type_names[connection_type]);
        param[connection_type].MaxBitrate = 0;
    }

    /* MaxFrameRate */
    datum = toml_double_in(oh264_ct, "MaxFrameRate");
    if (datum.ok)
    {
        param[connection_type].MaxFrameRate = (float)datum.u.d;
    }
    else if (connection_type == 0)
    {
        TCLOG(LOG_LEVEL_WARNING,
              "[OpenH264.%s] MaxFrameRate is not set, adopting the default value [0]",
              rdpbcgr_connection_type_names[connection_type]);
        param[connection_type].MaxFrameRate = 0;
    }

    return 0;
}

static int
tconfig_load_gfx_x264_ct(toml_table_t *tfile, const int connection_type,
                         struct xrdp_tconfig_gfx_x264_param *param)
{
    TCLOG(LOG_LEVEL_TRACE, "[x264]");

    if (connection_type > NUM_CONNECTION_TYPES)
    {
        TCLOG(LOG_LEVEL_ERROR, "[x264] Invalid connection type is given");
        return 1;
    }

    toml_table_t *x264 = toml_table_in(tfile, "x264");
    if (!x264)
    {
        TCLOG(LOG_LEVEL_WARNING, "[x264] x264 params are not defined");
        return 1;
    }

    toml_table_t *x264_ct =
        toml_table_in(x264, rdpbcgr_connection_type_names[connection_type]);
    toml_datum_t datum;

    if (!x264_ct)
    {
        TCLOG(LOG_LEVEL_WARNING, "x264 params for connection type [%s] is not defined",
              rdpbcgr_connection_type_names[connection_type]);
        return 1;
    }

    /* preset */
    datum = toml_string_in(x264_ct, "preset");
    if (datum.ok)
    {
        g_strncpy(param[connection_type].preset,
                  datum.u.s,
                  sizeof(param[connection_type].preset) - 1);
        free(datum.u.s);
        datum.u.s = NULL;
    }
    else if (connection_type == 0)
    {
        TCLOG(LOG_LEVEL_WARNING,
              "[x264.%s] preset is not set, adopting the default value \""
              X264_DEFAULT_PRESET "\"",
              rdpbcgr_connection_type_names[connection_type]);
        g_strncpy(param[connection_type].preset,
                  X264_DEFAULT_PRESET,
                  sizeof(param[connection_type].preset) - 1);
    }

    /* tune */
    datum = toml_string_in(x264_ct, "tune");
    if (datum.ok)
    {
        g_strncpy(param[connection_type].tune,
                  datum.u.s,
                  sizeof(param[connection_type].tune) - 1);
        free(datum.u.s);
        datum.u.s = NULL;
    }
    else if (connection_type == 0)
    {
        TCLOG(LOG_LEVEL_WARNING,
              "[x264.%s] tune is not set, adopting the default value \""
              X264_DEFAULT_TUNE"\"",
              rdpbcgr_connection_type_names[connection_type]);
        g_strncpy(param[connection_type].tune,
                  X264_DEFAULT_TUNE,
                  sizeof(param[connection_type].tune) - 1);
    }

    /* profile */
    datum = toml_string_in(x264_ct, "profile");
    if (datum.ok)
    {
        g_strncpy(param[connection_type].profile,
                  datum.u.s,
                  sizeof(param[connection_type].profile) - 1);
        free(datum.u.s);
        datum.u.s = NULL; // Prevent double-free warning with cppcheck 2.18.0
    }
    else if (connection_type == 0)
    {
        TCLOG(LOG_LEVEL_WARNING,
              "[x264.%s] profile is not set, adopting the default value \""
              X264_DEFAULT_PROFILE"\"",
              rdpbcgr_connection_type_names[connection_type]);
        g_strncpy(param[connection_type].profile,
                  X264_DEFAULT_PROFILE,
                  sizeof(param[connection_type].profile) - 1);
    }

    /* vbv_max_bitrate */
    datum = toml_int_in(x264_ct, "vbv_max_bitrate");
    if (datum.ok)
    {
        param[connection_type].vbv_max_bitrate = datum.u.i;
    }
    else if (connection_type == 0)
    {
        TCLOG(LOG_LEVEL_WARNING,
              "[x264.%s] vbv_max_bitrate is not set, adopting the default value [0]",
              rdpbcgr_connection_type_names[connection_type]);
        param[connection_type].vbv_max_bitrate = 0;
    }

    /* vbv_buffer_size */
    datum = toml_int_in(x264_ct, "vbv_buffer_size");
    if (datum.ok)
    {
        param[connection_type].vbv_buffer_size = datum.u.i;
    }
    else if (connection_type == 0)
    {
        TCLOG(LOG_LEVEL_WARNING,
              "[x264.%s] vbv_buffer_size is not set, adopting the default value [0]",
              rdpbcgr_connection_type_names[connection_type]);
        param[connection_type].vbv_buffer_size = 0;
    }

    /* fps_num */
    datum = toml_int_in(x264_ct, "fps_num");
    if (datum.ok)
    {
        param[connection_type].fps_num = datum.u.i;
    }
    else if (connection_type == 0)
    {
        TCLOG(LOG_LEVEL_WARNING,
              "[x264.%s] fps_num is not set, adopting the default value [%d]",
              rdpbcgr_connection_type_names[connection_type],
              X264_DEFAULT_FPS_NUM);
        param[connection_type].fps_num = X264_DEFAULT_FPS_NUM;
    }

    /* fps_den */
    datum = toml_int_in(x264_ct, "fps_den");
    if (datum.ok)
    {
        param[connection_type].fps_den = datum.u.i;
    }
    else if (connection_type == 0)
    {
        TCLOG(LOG_LEVEL_WARNING,
              "[x264.%s] fps_den is not set, adopting the default value [%d]",
              rdpbcgr_connection_type_names[connection_type],
              X264_DEFAULT_FPS_DEN);
        param[connection_type].fps_den = X264_DEFAULT_FPS_DEN;
    }

    /* threads */
    datum = toml_int_in(x264_ct, "threads");
    if (datum.ok)
    {
        if (datum.u.i >= 0)
        {
            param[connection_type].threads = datum.u.i;
        }
        else
        {
            TCLOG(LOG_LEVEL_WARNING,
                  "[x264.%s] an invalid value (< 0) is specified for threads, "
                  "adopting the default value [%d]",
                  rdpbcgr_connection_type_names[connection_type],
                  X264_DEFAULT_THREADS);
            param[connection_type].threads = X264_DEFAULT_THREADS;
        }
    }
    else if (connection_type == 0)
    {
        TCLOG(LOG_LEVEL_WARNING,
              "[x264.%s] threads is not set, adopting the default value [%d]",
              rdpbcgr_connection_type_names[connection_type],
              X264_DEFAULT_THREADS);
        param[connection_type].threads = X264_DEFAULT_THREADS;
    }

    return 0;
}

/* The [AV1] and [AV2] sections are optional: without them, the defaults
 * stand. param is the struct of the connection type */
static int
tconfig_load_gfx_video_ct(toml_table_t *tfile, const struct video_codec_def *cd,
                          const int connection_type, void *param)
{
    TCLOG(LOG_LEVEL_TRACE, "[%s]", cd->section);

    if (connection_type >= NUM_CONNECTION_TYPES)
    {
        TCLOG(LOG_LEVEL_ERROR, "[%s] Invalid connection type is given",
              cd->section);
        return 1;
    }

    toml_table_t *section = toml_table_in(tfile, cd->section);
    if (!section)
    {
        return 1;
    }

    toml_table_t *section_ct =
        toml_table_in(section, rdpbcgr_connection_type_names[connection_type]);
    toml_datum_t datum;

    if (!section_ct)
    {
        return 1;
    }

    for (unsigned int i = 0; i < cd->param_count; i++)
    {
        const struct video_param_def *pd = &cd->params[i];

        datum = toml_int_in(section_ct, pd->key);
        if (!datum.ok)
        {
            continue;
        }
        if (datum.u.i >= pd->min && datum.u.i <= pd->max)
        {
            *video_param_ptr(cd, param, i) = (int)datum.u.i;
        }
        else
        {
            TCLOG(LOG_LEVEL_WARNING,
                  "[%s.%s] %s must be %d to %d, "
                  "adopting the default value [%d]", cd->section,
                  rdpbcgr_connection_type_names[connection_type],
                  pd->key, pd->min, pd->max, pd->def);
            *video_param_ptr(cd, param, i) = pd->def;
        }
    }

    /* chroma: "auto", "420" or "444", also as the number 420 or 444 */
    datum = toml_string_in(section_ct, "chroma");
    if (!datum.ok)
    {
        toml_datum_t num = toml_int_in(section_ct, "chroma");
        if (num.ok)
        {
            datum.ok = 1;
            datum.u.s = g_strdup(num.u.i == 420 ? "420" :
                                 num.u.i == 444 ? "444" : "invalid");
        }
    }
    if (datum.ok)
    {
        if (av1_chroma_from_str(datum.u.s, video_chroma_ptr(cd, param)) != 0)
        {
            TCLOG(LOG_LEVEL_WARNING,
                  "[%s.%s] chroma must be \"auto\", \"420\" or \"444\", "
                  "adopting the default value \"auto\"", cd->section,
                  rdpbcgr_connection_type_names[connection_type]);
            *video_chroma_ptr(cd, param) = XTC_AV1_CHROMA_AUTO;
        }
        free(datum.u.s);
    }

    return 0;
}

/* Reads [<section>.default], then copies it to the other connection
 * types and overrides it with their own sections */
static void
tconfig_load_gfx_video(toml_table_t *tfile, const struct video_codec_def *cd,
                       void *params)
{
    tconfig_load_gfx_video_ct(tfile, cd, 0, params);
    for (int ct = CONNECTION_TYPE_MODEM; ct < NUM_CONNECTION_TYPES; ct++)
    {
        void *param = video_param_at(cd, params, ct);

        memcpy(param, params, cd->param_size);
        tconfig_load_gfx_video_ct(tfile, cd, ct, param);
    }
}

/* The defaults, for every connection type */
static void
tconfig_video_defaults(const struct video_codec_def *cd, void *params)
{
    for (int ct = 0; ct < NUM_CONNECTION_TYPES; ct++)
    {
        void *param = video_param_at(cd, params, ct);

        for (unsigned int i = 0; i < cd->param_count; i++)
        {
            *video_param_ptr(cd, param, i) = cd->params[i].def;
        }
        *video_chroma_ptr(cd, param) = XTC_AV1_CHROMA_AUTO;
    }
}

static int tconfig_load_gfx_h264_encoder(toml_table_t *tfile, struct xrdp_tconfig_gfx *config)
{
    TCLOG(LOG_LEVEL_TRACE, "[codec]");

    toml_table_t *codec;
    int valid_encoder_found = 0;

    if ((codec = toml_table_in(tfile, "codec")) != NULL)
    {
        toml_datum_t h264_encoder = toml_string_in(codec, "h264_encoder");

        if (h264_encoder.ok)
        {
            if (g_strcasecmp(h264_encoder.u.s, "x264") == 0)
            {
                TCLOG(LOG_LEVEL_DEBUG, "[codec] h264_encoder = x264");
                valid_encoder_found = 1;
                config->h264_encoder = XTC_H264_X264;
            }
            if (g_strcasecmp(h264_encoder.u.s, "OpenH264") == 0)
            {
                TCLOG(LOG_LEVEL_DEBUG, "[codec] h264_encoder = OpenH264");
                valid_encoder_found = 1;
                config->h264_encoder = XTC_H264_OPENH264;
            }

            free(h264_encoder.u.s);
        }
    }

    if (valid_encoder_found == 0)
    {
        TCLOG(LOG_LEVEL_WARNING, "[codec] could not get valid H.264 encoder, "
              "using default \"x264\"");

        /* default to x264 */
        config->h264_encoder = XTC_H264_X264;
        return 1;
    }

    return 0;
}

static int tconfig_load_gfx_order(toml_table_t *tfile, struct xrdp_tconfig_gfx *config)
{
    char buff[64];

    /*
     * This config loader is not responsible to check if xrdp is built with
     * H264/RFX support. Just loads configurations as-is.
     */

    TCLOG(LOG_LEVEL_TRACE, "[codec]");

    int h264_found = 0;
    int rfx_found = 0;
    int av1_found = 0;
    int av2_found = 0;

    config->codec.codec_count = 0;

    toml_table_t *codec;
    toml_array_t *order;

    if ((codec = toml_table_in(tfile, "codec")) != NULL &&
            (order = toml_array_in(codec, "order")) != NULL)
    {
        for (int i = 0; ; i++)
        {
            toml_datum_t datum = toml_string_at(order, i);

            if (datum.ok)
            {
                if (h264_found == 0 &&
                        (g_strcasecmp(datum.u.s, "h264") == 0 ||
                         g_strcasecmp(datum.u.s, "h.264") == 0))
                {
                    h264_found = 1;
                    config->codec.codecs[config->codec.codec_count] = XTC_H264;
                    ++config->codec.codec_count;
                }
                if (rfx_found == 0 &&
                        g_strcasecmp(datum.u.s, "rfx") == 0)
                {
                    rfx_found = 1;
                    config->codec.codecs[config->codec.codec_count] = XTC_RFX;
                    ++config->codec.codec_count;
                }
                if (av1_found == 0 &&
                        g_strcasecmp(datum.u.s, "av1") == 0)
                {
                    av1_found = 1;
                    config->codec.codecs[config->codec.codec_count] = XTC_AV1;
                    ++config->codec.codec_count;
                }
                if (av2_found == 0 &&
                        g_strcasecmp(datum.u.s, "av2") == 0)
                {
                    av2_found = 1;
                    config->codec.codecs[config->codec.codec_count] = XTC_AV2;
                    ++config->codec.codec_count;
                }
                free(datum.u.s);
            }
            else
            {
                break;
            }
        }
    }

    if (config->codec.codec_count == 0)
    {
        /* prefer H264 if no priority found */
        config->codec.codecs[0] = XTC_H264;
        config->codec.codecs[1] = XTC_RFX;
        config->codec.codec_count = 2;

        TCLOG(LOG_LEVEL_WARNING, "[codec] could not get GFX codec order, "
              "using default order %s",
              tconfig_codec_order_to_str(&config->codec, buff, sizeof(buff)));

        return 1;
    }

    TCLOG(LOG_LEVEL_DEBUG, "[codec] %s",
          tconfig_codec_order_to_str(&config->codec, buff, sizeof(buff)));
    return 0;
}

/**
 * Determines whether a codec is enabled
 * @param co Ordered codec list
 * @param code Code of codec to look for
 * @return boolean
 */
static int
codec_enabled(const struct xrdp_tconfig_gfx_codec_order *co,
              enum xrdp_tconfig_codecs code)
{
    for (unsigned short i = 0; i < co->codec_count; ++i)
    {
        if (co->codecs[i] == code)
        {
            return 1;
        }
    }

    return 0;
}

/**
 * Disables a Codec by removing it from the codec list
 * @param co Ordered codec list
 * @param code Code of codec to remove from list
 *
 * The order of the passed-in codec list is preserved.
 */
static void
disable_codec(struct xrdp_tconfig_gfx_codec_order *co,
              enum xrdp_tconfig_codecs code)
{
    unsigned short j = 0;
    for (unsigned short i = 0; i < co->codec_count; ++i)
    {
        if (co->codecs[i] != code)
        {
            co->codecs[j++] = co->codecs[i];
        }
    }
    co->codec_count = j;
}

/* XRDP_GFX_AV1_<KEY> (XRDP_GFX_AV1_THREADS, XRDP_GFX_AV1_CPU_USED, ...,
 * XRDP_GFX_AV1_CHROMA), and XRDP_GFX_AV2_<KEY> for AV2, if set,
 * override gfx.toml for every connection type. Invalid values are
 * ignored */
static void
tconfig_load_gfx_video_env(const struct video_codec_def *cd, void *params)
{
    const char *chroma_env = g_getenv(cd->chroma_env);
    enum xrdp_tconfig_av1_chroma chroma;

    for (unsigned int i = 0; i < cd->param_count; i++)
    {
        const struct video_param_def *pd = &cd->params[i];
        const char *env_var = g_getenv(pd->env);
        int value;

        if (env_var == NULL)
        {
            continue;
        }
        value = g_atoix(env_var);
        if (value < pd->min || value > pd->max)
        {
            TCLOG(LOG_LEVEL_WARNING, "%s set but invalid %s "
                  "(must be %d to %d), ignoring it", pd->env, env_var,
                  pd->min, pd->max);
            continue;
        }
        TCLOG(LOG_LEVEL_INFO, "%s set to %d", pd->env, value);
        for (int ct = 0; ct < NUM_CONNECTION_TYPES; ct++)
        {
            *video_param_ptr(cd, video_param_at(cd, params, ct), i) = value;
        }
    }

    if (chroma_env == NULL)
    {
        return;
    }
    if (av1_chroma_from_str(chroma_env, &chroma) != 0)
    {
        TCLOG(LOG_LEVEL_WARNING, "%s set but invalid %s "
              "(must be auto, 420 or 444), ignoring it", cd->chroma_env,
              chroma_env);
        return;
    }
    TCLOG(LOG_LEVEL_INFO, "%s set to %s", cd->chroma_env, chroma_env);
    for (int ct = 0; ct < NUM_CONNECTION_TYPES; ct++)
    {
        *video_chroma_ptr(cd, video_param_at(cd, params, ct)) = chroma;
    }
}

static int
tconfig_load_gfx_file(const char *filename, struct xrdp_tconfig_gfx *config)
{
    FILE *fp;
    char errbuf[200];
    toml_table_t *tfile;
    int rv = 0;

    /* Default to just RFX support. in case we can't load anything */
    config->codec.codec_count = 1;
    config->codec.codecs[0] = XTC_RFX;
    memset(config->x264_param, 0, sizeof(config->x264_param));
    tconfig_video_defaults(&av1_codec, config->av1_param);
    tconfig_video_defaults(&av2_codec, config->av2_param);

    if ((fp = fopen(filename, "r")) == NULL)
    {
        TCLOG(LOG_LEVEL_ERROR, "Error loading GFX config file %s (%s)",
              filename, g_get_strerror());
        return 1;
    }

    if ((tfile = toml_parse_file(fp, errbuf, sizeof(errbuf))) == NULL)
    {
        TCLOG(LOG_LEVEL_ERROR, "Error in GFX config file %s - %s", filename, errbuf);
        fclose(fp);
        return 1;
    }

    TCLOG(LOG_LEVEL_INFO, "Loading GFX config file %s", filename);
    fclose(fp);

    /* Load GFX codec order */
    tconfig_load_gfx_order(tfile, config);
    /* Load H.264 encoder */
    tconfig_load_gfx_h264_encoder(tfile, config);

    /* H.264 configuration */
    if (codec_enabled(&config->codec, XTC_H264))
    {
        /* First of all, read the default params */
        int x264_loaded;
        int oh264_loaded;

        x264_loaded = tconfig_load_gfx_x264_ct(tfile, 0, config->x264_param);
        oh264_loaded = tconfig_load_gfx_openh264_ct(tfile, 0, config->openh264_param);

        if (x264_loaded == 0)
        {
            /* Copy default params to other connection types, and
             * then override them */
            for (int ct = CONNECTION_TYPE_MODEM; ct < NUM_CONNECTION_TYPES;
                    ct++)
            {
                config->x264_param[ct] = config->x264_param[0];
                tconfig_load_gfx_x264_ct(tfile, ct, config->x264_param);
            }
        }

        if (oh264_loaded == 0)
        {
            /* Copy default params to other connection types, and
             * then override them */
            for (int ct = CONNECTION_TYPE_MODEM; ct < NUM_CONNECTION_TYPES;
                    ct++)
            {
                config->openh264_param[ct] = config->openh264_param[0];
                tconfig_load_gfx_openh264_ct(tfile, ct, config->openh264_param);
            }
        }

        if (x264_loaded != 0 && config->h264_encoder == XTC_H264_X264)
        {
            /* We can't get x264 defaults. Disable H.264. */
            TCLOG(LOG_LEVEL_WARNING, "x264 is selected as H.264 encoder but "
                  "cannot load default config for x264, disabling H.264");
            disable_codec(&config->codec, XTC_H264);
            rv = 1;
        }

        if (oh264_loaded != 0 && config->h264_encoder == XTC_H264_OPENH264)
        {
            /* We can't get OpenH264 defaults. Disable H.264. */
            TCLOG(LOG_LEVEL_WARNING, "OpenH264 is selected as H.264 encoder but "
                  "cannot load default config for OpenH264, disabling H.264");
            disable_codec(&config->codec, XTC_H264);
            rv = 1;
        }
    }

    /* AV1 and AV2 configuration */
    if (codec_enabled(&config->codec, XTC_AV1))
    {
        tconfig_load_gfx_video(tfile, &av1_codec, config->av1_param);
    }
    if (codec_enabled(&config->codec, XTC_AV2))
    {
        tconfig_load_gfx_video(tfile, &av2_codec, config->av2_param);
    }
    toml_free(tfile);

    return rv;
}

int
tconfig_load_gfx(const char *filename, struct xrdp_tconfig_gfx *config)
{
    int rv = tconfig_load_gfx_file(filename, config);

    /* The environment comes before the file, and the file before the
     * defaults, even when the file could not be loaded */
    tconfig_load_gfx_video_env(&av1_codec, config->av1_param);
    tconfig_load_gfx_video_env(&av2_codec, config->av2_param);

    return rv;
}

