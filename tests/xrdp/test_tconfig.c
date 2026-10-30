#if defined(HAVE_CONFIG_H)
#include "config_ac.h"
#endif

#include <stdlib.h>

#include "xrdp_tconfig.h"
#include "test_xrdp.h"
#include "xrdp.h"

#define GFXCONF_STUBDIR XRDP_TOP_SRCDIR "/tests/xrdp/gfx/"

START_TEST(test_tconfig_gfx_always_success)
{
    ck_assert_int_eq(1, 1);
}
END_TEST

START_TEST(test_tconfig_gfx_h264_oh264)
{
    struct xrdp_tconfig_gfx gfxconfig;
    tconfig_load_gfx(GFXCONF_STUBDIR "/gfx_h264_encoder_openh264.toml", &gfxconfig);

    /* H.264 encoder is OpenH264 */
    ck_assert_int_eq(gfxconfig.h264_encoder, XTC_H264_OPENH264);
}

START_TEST(test_tconfig_gfx_h264_x264)
{
    struct xrdp_tconfig_gfx gfxconfig;
    tconfig_load_gfx(GFXCONF_STUBDIR "/gfx_h264_encoder_x264.toml", &gfxconfig);

    /* H.264 encoder is x264 */
    ck_assert_int_eq(gfxconfig.h264_encoder, XTC_H264_X264);
}

START_TEST(test_tconfig_gfx_h264_undefined)
{
    struct xrdp_tconfig_gfx gfxconfig;
    tconfig_load_gfx(GFXCONF_STUBDIR "/gfx_h264_encoder_undefined.toml", &gfxconfig);

    /* H.264 encoder is x264 if undefined */
    ck_assert_int_eq(gfxconfig.h264_encoder, XTC_H264_X264);
}

START_TEST(test_tconfig_gfx_h264_invalid)
{
    struct xrdp_tconfig_gfx gfxconfig;
    tconfig_load_gfx(GFXCONF_STUBDIR "/gfx_h264_encoder_invalid.toml", &gfxconfig);

    /* H.264 encoder is x264 if invalid, unknown encoder specified */
    ck_assert_int_eq(gfxconfig.h264_encoder, XTC_H264_X264);
}

START_TEST(test_tconfig_gfx_oh264_load_basic)
{
    struct xrdp_tconfig_gfx gfxconfig;
    int rv = tconfig_load_gfx(GFXCONF_STUBDIR "/gfx.toml", &gfxconfig);

    ck_assert_int_eq(rv, 0);

    /* default */
    ck_assert_int_eq(gfxconfig.openh264_param[0].EnableFrameSkip, 0);
    ck_assert_int_eq(gfxconfig.openh264_param[0].TargetBitrate, 20000000);
    ck_assert_int_eq(gfxconfig.openh264_param[0].MaxBitrate, 0);
    ck_assert_float_eq(gfxconfig.openh264_param[0].MaxFrameRate, 60.0);
}

START_TEST(test_tconfig_gfx_x264_load_basic)
{
    struct xrdp_tconfig_gfx gfxconfig;
    int rv = tconfig_load_gfx(GFXCONF_STUBDIR "/gfx.toml", &gfxconfig);

    ck_assert_int_eq(rv, 0);

    /* default */
    ck_assert_str_eq(gfxconfig.x264_param[0].preset, "ultrafast");
    ck_assert_str_eq(gfxconfig.x264_param[0].tune, "zerolatency");
    ck_assert_str_eq(gfxconfig.x264_param[0].profile, "main");
    ck_assert_int_eq(gfxconfig.x264_param[0].vbv_max_bitrate, 0);
    ck_assert_int_eq(gfxconfig.x264_param[0].vbv_buffer_size, 0);
    ck_assert_int_eq(gfxconfig.x264_param[0].fps_num, 60);
    ck_assert_int_eq(gfxconfig.x264_param[0].fps_den, 1);

}
END_TEST

START_TEST(test_tconfig_gfx_av1_threads)
{
    struct xrdp_tconfig_gfx gfxconfig;

    /* No [AV1] section: 2 threads everywhere */
    tconfig_load_gfx(GFXCONF_STUBDIR "/gfx_codec_av1_preferred.toml", &gfxconfig);
    ck_assert_int_eq(gfxconfig.av1_param[0].threads, 2);
    ck_assert_int_eq(gfxconfig.av1_param[CONNECTION_TYPE_LAN].threads, 2);

    /* default, inherited, overridden, invalid */
    tconfig_load_gfx(GFXCONF_STUBDIR "/gfx_av1_threads.toml", &gfxconfig);
    ck_assert_int_eq(gfxconfig.av1_param[0].threads, 4);
    ck_assert_int_eq(gfxconfig.av1_param[CONNECTION_TYPE_LAN].threads, 4);
    ck_assert_int_eq(gfxconfig.av1_param[CONNECTION_TYPE_WAN].threads, 1);
    ck_assert_int_eq(gfxconfig.av1_param[CONNECTION_TYPE_MODEM].threads, 2);

    /* Missing file: still the defaults */
    tconfig_load_gfx(GFXCONF_STUBDIR "/no_such_file.toml", &gfxconfig);
    ck_assert_int_eq(gfxconfig.av1_param[CONNECTION_TYPE_LAN].threads, 2);
}
END_TEST

START_TEST(test_tconfig_gfx_av1_threads_env)
{
    struct xrdp_tconfig_gfx gfxconfig;

    /* The environment overrides the file for every connection type */
    g_setenv("XRDP_GFX_AV1_THREADS", "3", 1);
    tconfig_load_gfx(GFXCONF_STUBDIR "/gfx_av1_threads.toml", &gfxconfig);
    ck_assert_int_eq(gfxconfig.av1_param[0].threads, 3);
    ck_assert_int_eq(gfxconfig.av1_param[CONNECTION_TYPE_LAN].threads, 3);
    ck_assert_int_eq(gfxconfig.av1_param[CONNECTION_TYPE_WAN].threads, 3);

    /* ... and the defaults when there is no file */
    tconfig_load_gfx(GFXCONF_STUBDIR "/no_such_file.toml", &gfxconfig);
    ck_assert_int_eq(gfxconfig.av1_param[CONNECTION_TYPE_LAN].threads, 3);

    /* An invalid value is ignored: the file decides */
    g_setenv("XRDP_GFX_AV1_THREADS", "0", 1);
    tconfig_load_gfx(GFXCONF_STUBDIR "/gfx_av1_threads.toml", &gfxconfig);
    ck_assert_int_eq(gfxconfig.av1_param[CONNECTION_TYPE_LAN].threads, 4);
    ck_assert_int_eq(gfxconfig.av1_param[CONNECTION_TYPE_WAN].threads, 1);

    g_setenv("XRDP_GFX_AV1_THREADS", "65", 1);
    tconfig_load_gfx(GFXCONF_STUBDIR "/gfx_av1_threads.toml", &gfxconfig);
    ck_assert_int_eq(gfxconfig.av1_param[CONNECTION_TYPE_LAN].threads, 4);

    unsetenv("XRDP_GFX_AV1_THREADS");
}
END_TEST

START_TEST(test_tconfig_gfx_av1_params)
{
    struct xrdp_tconfig_gfx gfxconfig;
    const struct xrdp_tconfig_gfx_av1_param *p;

    /* No [AV1] section: the defaults everywhere */
    tconfig_load_gfx(GFXCONF_STUBDIR "/gfx_codec_av1_preferred.toml", &gfxconfig);
    p = &gfxconfig.av1_param[CONNECTION_TYPE_LAN];
    ck_assert_int_eq(p->cpu_used, 10);
    ck_assert_int_eq(p->cq_level, 28);
    ck_assert_int_eq(p->target_bitrate, 0);
    ck_assert_int_eq(p->chroma, XTC_AV1_CHROMA_AUTO);

    tconfig_load_gfx(GFXCONF_STUBDIR "/gfx_av1_params.toml", &gfxconfig);

    /* [AV1.default], inherited by LAN */
    p = &gfxconfig.av1_param[CONNECTION_TYPE_LAN];
    ck_assert_int_eq(p->cpu_used, 11);
    ck_assert_int_eq(p->cq_level, 30);
    ck_assert_int_eq(p->target_bitrate, 0);
    ck_assert_int_eq(p->chroma, XTC_AV1_CHROMA_420);

    /* WAN: a bitrate, on top of the default section */
    p = &gfxconfig.av1_param[CONNECTION_TYPE_WAN];
    ck_assert_int_eq(p->target_bitrate, 15000);
    ck_assert_int_eq(p->cpu_used, 11);
    ck_assert_int_eq(p->chroma, XTC_AV1_CHROMA_420);

    /* broadband_low: chroma as a number */
    p = &gfxconfig.av1_param[CONNECTION_TYPE_BROADBAND_LOW];
    ck_assert_int_eq(p->chroma, XTC_AV1_CHROMA_444);

    /* modem: invalid values take the defaults */
    p = &gfxconfig.av1_param[CONNECTION_TYPE_MODEM];
    ck_assert_int_eq(p->cpu_used, 10);
    ck_assert_int_eq(p->cq_level, 28);
    ck_assert_int_eq(p->chroma, XTC_AV1_CHROMA_AUTO);

    ck_assert_str_eq(tconfig_av1_chroma_to_str(XTC_AV1_CHROMA_AUTO), "auto");
    ck_assert_str_eq(tconfig_av1_chroma_to_str(XTC_AV1_CHROMA_420), "420");
    ck_assert_str_eq(tconfig_av1_chroma_to_str(XTC_AV1_CHROMA_444), "444");
}
END_TEST

START_TEST(test_tconfig_gfx_av1_params_env)
{
    struct xrdp_tconfig_gfx gfxconfig;

    /* XRDP_GFX_AV1_<NAME> overrides the file for every connection type */
    g_setenv("XRDP_GFX_AV1_CPU_USED", "12", 1);
    g_setenv("XRDP_GFX_AV1_TARGET_BITRATE", "3000", 1);
    g_setenv("XRDP_GFX_AV1_CHROMA", "444", 1);
    tconfig_load_gfx(GFXCONF_STUBDIR "/gfx_av1_params.toml", &gfxconfig);
    for (int ct = 0; ct < NUM_CONNECTION_TYPES; ct++)
    {
        ck_assert_int_eq(gfxconfig.av1_param[ct].cpu_used, 12);
        ck_assert_int_eq(gfxconfig.av1_param[ct].target_bitrate, 3000);
        ck_assert_int_eq(gfxconfig.av1_param[ct].chroma, XTC_AV1_CHROMA_444);
    }

    /* Invalid values are ignored: the file decides */
    g_setenv("XRDP_GFX_AV1_CPU_USED", "13", 1);
    g_setenv("XRDP_GFX_AV1_CHROMA", "422", 1);
    tconfig_load_gfx(GFXCONF_STUBDIR "/gfx_av1_params.toml", &gfxconfig);
    ck_assert_int_eq(gfxconfig.av1_param[CONNECTION_TYPE_LAN].cpu_used, 11);
    ck_assert_int_eq(gfxconfig.av1_param[CONNECTION_TYPE_LAN].chroma,
                     XTC_AV1_CHROMA_420);

    unsetenv("XRDP_GFX_AV1_CPU_USED");
    unsetenv("XRDP_GFX_AV1_TARGET_BITRATE");
    unsetenv("XRDP_GFX_AV1_CHROMA");
}
END_TEST

START_TEST(test_tconfig_gfx_av2_params)
{
    struct xrdp_tconfig_gfx gfxconfig;
    const struct xrdp_tconfig_gfx_av2_param *p;
    char buff[64];

    /* No [AV2] section: the defaults everywhere */
    tconfig_load_gfx(GFXCONF_STUBDIR "/gfx_codec_av2_preferred.toml", &gfxconfig);
    for (int ct = 0; ct < NUM_CONNECTION_TYPES; ct++)
    {
        p = &gfxconfig.av2_param[ct];
        ck_assert_int_eq(p->threads, 4);
        ck_assert_int_eq(p->cpu_used, 9);
        ck_assert_int_eq(p->qp, 160);
        ck_assert_int_eq(p->target_bitrate, 0);
        ck_assert_int_eq(p->tile_columns, 1);
        ck_assert_int_eq(p->chroma, XTC_AV1_CHROMA_AUTO);
    }
    ck_assert_str_eq(tconfig_codec_order_to_str(&gfxconfig.codec, buff,
                     sizeof(buff)), "AV2, AV1, H264, RFX");

    tconfig_load_gfx(GFXCONF_STUBDIR "/gfx_av2_params.toml", &gfxconfig);

    /* [AV2.default], inherited by LAN */
    p = &gfxconfig.av2_param[CONNECTION_TYPE_LAN];
    ck_assert_int_eq(p->threads, 6);
    ck_assert_int_eq(p->cpu_used, 10);
    ck_assert_int_eq(p->qp, 120);
    ck_assert_int_eq(p->target_bitrate, 0);
    ck_assert_int_eq(p->tile_columns, 2);
    ck_assert_int_eq(p->chroma, XTC_AV1_CHROMA_420);

    /* WAN: a bitrate and one tile column, on top of the default section */
    p = &gfxconfig.av2_param[CONNECTION_TYPE_WAN];
    ck_assert_int_eq(p->target_bitrate, 15000);
    ck_assert_int_eq(p->tile_columns, 0);
    ck_assert_int_eq(p->qp, 120);
    ck_assert_int_eq(p->chroma, XTC_AV1_CHROMA_420);

    /* broadband_low: chroma as a number */
    p = &gfxconfig.av2_param[CONNECTION_TYPE_BROADBAND_LOW];
    ck_assert_int_eq(p->chroma, XTC_AV1_CHROMA_444);
    ck_assert_int_eq(p->threads, 6);

    /* modem: invalid values take the defaults */
    p = &gfxconfig.av2_param[CONNECTION_TYPE_MODEM];
    ck_assert_int_eq(p->threads, 4);
    ck_assert_int_eq(p->cpu_used, 9);
    ck_assert_int_eq(p->qp, 160);
    ck_assert_int_eq(p->tile_columns, 1);
    ck_assert_int_eq(p->chroma, XTC_AV1_CHROMA_AUTO);

    /* AV1 is not in the order: its section is not read */
    ck_assert_int_eq(gfxconfig.av1_param[CONNECTION_TYPE_LAN].threads, 2);
    ck_assert_int_eq(gfxconfig.av1_param[CONNECTION_TYPE_LAN].cq_level, 28);

    /* Missing file: still the defaults */
    tconfig_load_gfx(GFXCONF_STUBDIR "/no_such_file.toml", &gfxconfig);
    ck_assert_int_eq(gfxconfig.av2_param[CONNECTION_TYPE_LAN].qp, 160);
    ck_assert_int_eq(gfxconfig.av2_param[CONNECTION_TYPE_LAN].threads, 4);
}
END_TEST

START_TEST(test_tconfig_gfx_av2_params_env)
{
    struct xrdp_tconfig_gfx gfxconfig;

    /* XRDP_GFX_AV2_<NAME> overrides the file for every connection type,
     * and leaves AV1 alone */
    g_setenv("XRDP_GFX_AV2_THREADS", "3", 1);
    g_setenv("XRDP_GFX_AV2_CPU_USED", "8", 1);
    g_setenv("XRDP_GFX_AV2_QP", "200", 1);
    g_setenv("XRDP_GFX_AV2_TARGET_BITRATE", "3000", 1);
    g_setenv("XRDP_GFX_AV2_TILE_COLUMNS", "3", 1);
    g_setenv("XRDP_GFX_AV2_CHROMA", "444", 1);
    tconfig_load_gfx(GFXCONF_STUBDIR "/gfx_av2_params.toml", &gfxconfig);
    for (int ct = 0; ct < NUM_CONNECTION_TYPES; ct++)
    {
        ck_assert_int_eq(gfxconfig.av2_param[ct].threads, 3);
        ck_assert_int_eq(gfxconfig.av2_param[ct].cpu_used, 8);
        ck_assert_int_eq(gfxconfig.av2_param[ct].qp, 200);
        ck_assert_int_eq(gfxconfig.av2_param[ct].target_bitrate, 3000);
        ck_assert_int_eq(gfxconfig.av2_param[ct].tile_columns, 3);
        ck_assert_int_eq(gfxconfig.av2_param[ct].chroma, XTC_AV1_CHROMA_444);
        ck_assert_int_eq(gfxconfig.av1_param[ct].threads, 2);
        ck_assert_int_eq(gfxconfig.av1_param[ct].chroma, XTC_AV1_CHROMA_AUTO);
    }

    /* Invalid values are ignored: the file decides */
    g_setenv("XRDP_GFX_AV2_CPU_USED", "11", 1);
    g_setenv("XRDP_GFX_AV2_QP", "-1", 1);
    g_setenv("XRDP_GFX_AV2_TILE_COLUMNS", "7", 1);
    g_setenv("XRDP_GFX_AV2_CHROMA", "422", 1);
    tconfig_load_gfx(GFXCONF_STUBDIR "/gfx_av2_params.toml", &gfxconfig);
    ck_assert_int_eq(gfxconfig.av2_param[CONNECTION_TYPE_LAN].cpu_used, 10);
    ck_assert_int_eq(gfxconfig.av2_param[CONNECTION_TYPE_LAN].qp, 120);
    ck_assert_int_eq(gfxconfig.av2_param[CONNECTION_TYPE_LAN].tile_columns, 2);
    ck_assert_int_eq(gfxconfig.av2_param[CONNECTION_TYPE_LAN].chroma,
                     XTC_AV1_CHROMA_420);

    unsetenv("XRDP_GFX_AV2_THREADS");
    unsetenv("XRDP_GFX_AV2_CPU_USED");
    unsetenv("XRDP_GFX_AV2_QP");
    unsetenv("XRDP_GFX_AV2_TARGET_BITRATE");
    unsetenv("XRDP_GFX_AV2_TILE_COLUMNS");
    unsetenv("XRDP_GFX_AV2_CHROMA");
}
END_TEST

START_TEST(test_tconfig_gfx_codec_order)
{
    struct xrdp_tconfig_gfx gfxconfig;

    /* H264 earlier */
    tconfig_load_gfx(GFXCONF_STUBDIR "/gfx_codec_h264_preferred.toml", &gfxconfig);
    ck_assert_int_eq(gfxconfig.codec.codec_count, 2);
    ck_assert_int_eq(gfxconfig.codec.codecs[0], XTC_H264);
    ck_assert_int_eq(gfxconfig.codec.codecs[1], XTC_RFX);

    /* H264 only */
    tconfig_load_gfx(GFXCONF_STUBDIR "/gfx_codec_h264_only.toml", &gfxconfig);
    ck_assert_int_eq(gfxconfig.codec.codec_count, 1);
    ck_assert_int_eq(gfxconfig.codec.codecs[0], XTC_H264);

    /* RFX earlier */
    tconfig_load_gfx(GFXCONF_STUBDIR "/gfx_codec_rfx_preferred.toml", &gfxconfig);
    ck_assert_int_eq(gfxconfig.codec.codec_count, 2);
    ck_assert_int_eq(gfxconfig.codec.codecs[0], XTC_RFX);
    ck_assert_int_eq(gfxconfig.codec.codecs[1], XTC_H264);

    /* RFX appears twice like: RFX, H264, RFX */
    tconfig_load_gfx(GFXCONF_STUBDIR "/gfx_codec_rfx_preferred_odd.toml", &gfxconfig);
    ck_assert_int_eq(gfxconfig.codec.codec_count, 2);
    ck_assert_int_eq(gfxconfig.codec.codecs[0], XTC_RFX);
    ck_assert_int_eq(gfxconfig.codec.codecs[1], XTC_H264);

    /* RFX only */
    tconfig_load_gfx(GFXCONF_STUBDIR "/gfx_codec_rfx_only.toml", &gfxconfig);
    ck_assert_int_eq(gfxconfig.codec.codec_count, 1);
    ck_assert_int_eq(gfxconfig.codec.codecs[0], XTC_RFX);

    /* AV1, FreeRDP's extension, first */
    tconfig_load_gfx(GFXCONF_STUBDIR "/gfx_codec_av1_preferred.toml", &gfxconfig);
    ck_assert_int_eq(gfxconfig.codec.codec_count, 3);
    ck_assert_int_eq(gfxconfig.codec.codecs[0], XTC_AV1);
    ck_assert_int_eq(gfxconfig.codec.codecs[1], XTC_H264);
    ck_assert_int_eq(gfxconfig.codec.codecs[2], XTC_RFX);

    /* AV2, FreeRDP's other extension, before AV1 */
    tconfig_load_gfx(GFXCONF_STUBDIR "/gfx_codec_av2_preferred.toml", &gfxconfig);
    ck_assert_int_eq(gfxconfig.codec.codec_count, 4);
    ck_assert_int_eq(gfxconfig.codec.codecs[0], XTC_AV2);
    ck_assert_int_eq(gfxconfig.codec.codecs[1], XTC_AV1);
    ck_assert_int_eq(gfxconfig.codec.codecs[2], XTC_H264);
    ck_assert_int_eq(gfxconfig.codec.codecs[3], XTC_RFX);

    /* AV1 alone is kept, not replaced by the default order */
    tconfig_load_gfx(GFXCONF_STUBDIR "/gfx_codec_av1_only.toml", &gfxconfig);
    ck_assert_int_eq(gfxconfig.codec.codec_count, 1);
    ck_assert_int_eq(gfxconfig.codec.codecs[0], XTC_AV1);

    /* H264 is preferred if order undefined */
    tconfig_load_gfx(GFXCONF_STUBDIR "/gfx_codec_order_undefined.toml", &gfxconfig);
    ck_assert_int_eq(gfxconfig.codec.codec_count, 2);
    ck_assert_int_eq(gfxconfig.codec.codecs[0], XTC_H264);
    ck_assert_int_eq(gfxconfig.codec.codecs[1], XTC_RFX);
}
END_TEST

START_TEST(test_tconfig_gfx_missing_file)
{
    struct xrdp_tconfig_gfx gfxconfig;

    /* Check RFX config is returned if the file doesn't exist */
    tconfig_load_gfx(GFXCONF_STUBDIR "/no_such_file.toml", &gfxconfig);
    ck_assert_int_eq(gfxconfig.codec.codec_count, 1);
    ck_assert_int_eq(gfxconfig.codec.codecs[0], XTC_RFX);
}
END_TEST

START_TEST(test_tconfig_gfx_missing_h264)
{
    struct xrdp_tconfig_gfx gfxconfig;

    /* Check RFX config only is returned if H.264 parameters are missing */
    tconfig_load_gfx(GFXCONF_STUBDIR "/gfx_missing_h264.toml", &gfxconfig);
    ck_assert_int_eq(gfxconfig.codec.codec_count, 1);
    ck_assert_int_eq(gfxconfig.codec.codecs[0], XTC_RFX);
}
END_TEST

/******************************************************************************/
Suite *
make_suite_tconfig_load_gfx(void)
{
    Suite *s;
    TCase *tc_tconfig_load_gfx;

    s = suite_create("GfxLoad");

    tc_tconfig_load_gfx = tcase_create("xrdp_tconfig_load_gfx");
    tcase_add_test(tc_tconfig_load_gfx, test_tconfig_gfx_always_success);
    tcase_add_test(tc_tconfig_load_gfx, test_tconfig_gfx_x264_load_basic);
    tcase_add_test(tc_tconfig_load_gfx, test_tconfig_gfx_codec_order);
    tcase_add_test(tc_tconfig_load_gfx, test_tconfig_gfx_av1_threads);
    tcase_add_test(tc_tconfig_load_gfx, test_tconfig_gfx_av1_threads_env);
    tcase_add_test(tc_tconfig_load_gfx, test_tconfig_gfx_av1_params);
    tcase_add_test(tc_tconfig_load_gfx, test_tconfig_gfx_av1_params_env);
    tcase_add_test(tc_tconfig_load_gfx, test_tconfig_gfx_av2_params);
    tcase_add_test(tc_tconfig_load_gfx, test_tconfig_gfx_av2_params_env);
    tcase_add_test(tc_tconfig_load_gfx, test_tconfig_gfx_missing_file);
    tcase_add_test(tc_tconfig_load_gfx, test_tconfig_gfx_missing_h264);

    /* OpenH264 */
    tcase_add_test(tc_tconfig_load_gfx, test_tconfig_gfx_oh264_load_basic);

    /* H.264 encoder */
    tcase_add_test(tc_tconfig_load_gfx, test_tconfig_gfx_h264_oh264);
    tcase_add_test(tc_tconfig_load_gfx, test_tconfig_gfx_h264_x264);
    tcase_add_test(tc_tconfig_load_gfx, test_tconfig_gfx_h264_undefined);
    tcase_add_test(tc_tconfig_load_gfx, test_tconfig_gfx_h264_invalid);

    suite_add_tcase(s, tc_tconfig_load_gfx);

    return s;
}
