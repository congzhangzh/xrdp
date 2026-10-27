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
 * wlup: KWin (KDE Plasma) backend
 *
 * KWin has none of the wlroots capture and virtual input protocols. It
 * has its own, which KRdp (Plasma's RDP server) uses too:
 *   frames   : zkde_screencast_unstable_v1 casts an output to a
 *              PipeWire node (received by pipewire/capture.c)
 *   input    : org_kde_kwin_fake_input
 *   clipboard: ext-data-control-v1 (wlroots/clip.c)
 *   resize   : kde_output_management_v2 gives the output a custom mode,
 *              and the stream follows the output's size
 *
 * KWin offers zkde_screencast and fake_input only to trusted clients:
 * from KWin 6.8 to every client that is not sandboxed (Flatpak, Snap),
 * before 6.8 to programs listed in a desktop file or with
 * KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 in KWin's environment, which
 * startplasma.sh sets. Casting also needs KWin to composite with OpenGL:
 * KWin 6.8 and later do so in software with /dev/udmabuf, older KWin
 * needs a DRM render node on its virtual backend (vgem without a GPU).
 *
 * Two file descriptors go into xrdp's poll loop: the Wayland connection
 * and the PipeWire loop.
 */

#if defined(HAVE_CONFIG_H)
#include <config_ac.h>
#endif

#include <poll.h>
#include <string.h>
#include <time.h>

#include <linux/input-event-codes.h>
#include <wayland-client.h>

/* cppcheck-suppress missingInclude ; generated at build time */
#include "fake-input-client-protocol.h"
/* cppcheck-suppress missingInclude ; generated at build time */
#include "kde-output-device-v2-client-protocol.h"
/* cppcheck-suppress missingInclude ; generated at build time */
#include "kde-output-management-v2-client-protocol.h"
/* cppcheck-suppress missingInclude ; generated at build time */
#include "zkde-screencast-unstable-v1-client-protocol.h"

#include "wlup.h"
#include "wlup_backend.h"
#include "kwin.h"
#include "../pipewire/capture.h"
#include "../wlroots/clip.h"
#include "log.h"
#include "os_calls.h"
#include "string_calls.h"
#include "scancode.h"
#include "xrdp_constants.h"

/* zkde_screencast_unstable_v1 pointer mode: the cursor is sent as
 * buffer metadata, not painted into the frames */
#define POINTER_METADATA 4

/* Time allowed for KWin to announce the PipeWire node of the stream */
#define STREAM_TIMEOUT_MS 5000

/* Time allowed for a resize. xrdp waits for it, so give up after this
 * and let the picture be clipped */
#define RESIZE_TIMEOUT_USEC (5 * 1000 * 1000)

/* Modes of the output kept track of */
#define MAX_MODES 64

/* Refresh rate of the custom modes, in mHz */
#define CUSTOM_REFRESH 60000

/* Scroll distance of one wheel notch: 15 degrees, as for KRdp */
#define WHEEL_STEP 15

/* Versions bound. kde_output_device_v2 objects come from the registry,
 * which only exists from version 21. Custom modes come with
 * kde_output_management_v2 18, but KWin 6.8 takes them only from clients
 * of version 22 (add_cvt's) or later: the newest version both KWin and
 * these protocol files know is bound, from MANAGEMENT_VERSION on */
#define SCREENCAST_VERSION 6
#define FAKE_INPUT_VERSION 4
#define DEVICE_REGISTRY_VERSION 21
#define MANAGEMENT_VERSION 18

struct kwin_mode
{
    struct kde_output_device_mode_v2 *mode;
    int width;
    int height;
};

struct wlup_kwin
{
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_seat *seat;
    struct wl_output *output; /* the output cast, the first one */

    /* frames */
    struct zkde_screencast_unstable_v1 *screencast;
    struct zkde_screencast_stream_unstable_v1 *stream;
    uint32_t node_id;
    int have_node;
    int stream_failed;
    int closed;
    struct wlup_pwc *pwc;

    struct org_kde_kwin_fake_input *fake_input;
    struct wlup_wlr_clip *clip;

    /* resize */
    struct kde_output_device_registry_v2 *device_registry;
    struct kde_output_device_v2 *device; /* the first enabled device */
    int device_enabled;                  /* of the device being described */
    struct kde_output_management_v2 *management;
    struct kwin_mode modes[MAX_MODES];
    int num_modes;
    struct kde_output_device_mode_v2 *current_mode;
    struct kde_mode_list_v2 *mode_list; /* sent, kept until applied */
    int want_width;   /* size of a custom mode not yet announced, or 0 */
    int want_height;
    uint64_t resize_start;

    /* Versions KWin offers, 0 for a global it lacks, for the log */
    uint32_t screencast_offered;
    uint32_t fake_input_offered;
    uint32_t device_registry_offered;
    uint32_t management_offered;
    int data_control_offered;
};

#define KWIN(v) ((struct wlup_kwin *)(v)->backend_data)

/******************************************************************************/
static uint64_t
monotonic_usec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/******************************************************************************/
/* Screen cast stream                                                         */
/******************************************************************************/
static void
stream_closed(void *data, struct zkde_screencast_stream_unstable_v1 *stream)
{
    struct wlup_kwin *k = (struct wlup_kwin *)data;

    LOG(LOG_LEVEL_INFO, "wlup/kwin: KWin closed the screen cast stream");
    k->closed = 1;
}

/******************************************************************************/
static void
stream_created(void *data, struct zkde_screencast_stream_unstable_v1 *stream,
               uint32_t node)
{
    struct wlup_kwin *k = (struct wlup_kwin *)data;

    k->node_id = node;
    k->have_node = 1;
    LOG(LOG_LEVEL_INFO, "wlup/kwin: PipeWire node %u", node);
}

/******************************************************************************/
static void
stream_failed(void *data, struct zkde_screencast_stream_unstable_v1 *stream,
              const char *error)
{
    struct wlup_kwin *k = (struct wlup_kwin *)data;

    LOG(LOG_LEVEL_ERROR, "wlup/kwin: KWin cannot cast the output: %s%s",
        error, strstr(error, "compositing") != NULL ?
        " (KWin composites with OpenGL only when it can open a DRM render "
        "node, or from KWin 6.8 /dev/udmabuf for software rendering; "
        "without a GPU, KWin before 6.8 needs the vgem module; the user "
        "needs the device's group, as a session without a seat gets no "
        "access from logind)" : "");
    k->stream_failed = 1;
}

/******************************************************************************/
static void
stream_serial(void *data, struct zkde_screencast_stream_unstable_v1 *stream,
              uint32_t serial_hi, uint32_t serial_low)
{
    /* The node id of the created event is what PipeWire connects to */
}

static const struct zkde_screencast_stream_unstable_v1_listener
    stream_listener =
{
    .closed = stream_closed,
    .created = stream_created,
    .failed = stream_failed,
    .serial = stream_serial,
};

/******************************************************************************/
/* Output modes                                                               */
/******************************************************************************/
static struct kwin_mode *
find_mode(struct wlup_kwin *k, int width, int height)
{
    int i;

    for (i = 0; i < k->num_modes; ++i)
    {
        if (k->modes[i].width == width && k->modes[i].height == height)
        {
            return &k->modes[i];
        }
    }
    return NULL;
}

/******************************************************************************/
static void
mode_size(void *data, struct kde_output_device_mode_v2 *mode,
          int32_t width, int32_t height)
{
    struct wlup_kwin *k = (struct wlup_kwin *)data;
    int i;

    for (i = 0; i < k->num_modes; ++i)
    {
        if (k->modes[i].mode == mode)
        {
            k->modes[i].width = width;
            k->modes[i].height = height;
        }
    }
}

/******************************************************************************/
static void
mode_refresh(void *data, struct kde_output_device_mode_v2 *mode,
             int32_t refresh)
{
}

/******************************************************************************/
static void
mode_preferred(void *data, struct kde_output_device_mode_v2 *mode)
{
}

/******************************************************************************/
static void
mode_removed(void *data, struct kde_output_device_mode_v2 *mode)
{
    struct wlup_kwin *k = (struct wlup_kwin *)data;
    int i;

    for (i = 0; i < k->num_modes; ++i)
    {
        if (k->modes[i].mode == mode)
        {
            k->modes[i] = k->modes[--k->num_modes];
            break;
        }
    }
    if (k->current_mode == mode)
    {
        k->current_mode = NULL;
    }
    kde_output_device_mode_v2_destroy(mode);
}

/******************************************************************************/
static void
mode_flags(void *data, struct kde_output_device_mode_v2 *mode, uint32_t flags)
{
}

static const struct kde_output_device_mode_v2_listener mode_listener =
{
    .size = mode_size,
    .refresh = mode_refresh,
    .preferred = mode_preferred,
    .removed = mode_removed,
    .flags = mode_flags,
};

/******************************************************************************/
/* Output configurations                                                      */
/******************************************************************************/
static void
config_applied(void *data, struct kde_output_configuration_v2 *config)
{
    kde_output_configuration_v2_destroy(config);
}

/******************************************************************************/
static void
config_failed(void *data, struct kde_output_configuration_v2 *config)
{
    struct wlup *v = (struct wlup *)data;
    struct wlup_kwin *k = KWIN(v);

    LOG(LOG_LEVEL_WARNING, "wlup/kwin: KWin did not apply an output "
        "configuration, the output keeps its size");
    kde_output_configuration_v2_destroy(config);
    k->want_width = 0;
    k->want_height = 0;
    if (v->resize_pending)
    {
        wlup_resize_finished(v);
    }
}

/******************************************************************************/
static void
config_failure_reason(void *data, struct kde_output_configuration_v2 *config,
                      const char *reason)
{
    LOG(LOG_LEVEL_WARNING, "wlup/kwin: output configuration failed: %s",
        reason);
}

static const struct kde_output_configuration_v2_listener config_listener =
{
    .applied = config_applied,
    .failed = config_failed,
    .failure_reason = config_failure_reason,
};

/******************************************************************************/
/* Switches the output to one of its modes */
static void
apply_mode(struct wlup *v, struct kwin_mode *mode)
{
    struct wlup_kwin *k = KWIN(v);
    struct kde_output_configuration_v2 *config;

    LOG(LOG_LEVEL_INFO, "wlup/kwin: switching the output to %dx%d",
        mode->width, mode->height);
    config = kde_output_management_v2_create_configuration(k->management);
    kde_output_configuration_v2_add_listener(config, &config_listener, v);
    kde_output_configuration_v2_mode(config, k->device, mode->mode);
    kde_output_configuration_v2_apply(config);
}

/******************************************************************************/
/* Asks for an output of width x height: KWin's virtual outputs only have
 * the mode they started with, so a size they lack becomes a custom mode,
 * applied once KWin announces it (device_done()).
 * return 1 if under way */
static int
request_output_size(struct wlup *v, int width, int height)
{
    struct wlup_kwin *k = KWIN(v);
    struct kde_output_configuration_v2 *config;
    struct kwin_mode *mode;
    int i;

    if (k->device == NULL || k->management == NULL)
    {
        LOG(LOG_LEVEL_WARNING, "wlup/kwin: no kde_output_management_v2 "
            "version %d, the output cannot be resized", MANAGEMENT_VERSION);
        return 0;
    }
    if ((mode = find_mode(k, width, height)) != NULL)
    {
        apply_mode(v, mode);
        return 1;
    }

    /* The custom modes are replaced as a whole: keep the current one */
    if (k->mode_list != NULL)
    {
        kde_mode_list_v2_destroy(k->mode_list);
    }
    k->mode_list = kde_output_management_v2_create_mode_list(k->management);
    for (i = 0; i < k->num_modes; ++i)
    {
        if (k->modes[i].mode == k->current_mode)
        {
            kde_mode_list_v2_set_resolution(k->mode_list, k->modes[i].width,
                                            k->modes[i].height);
            kde_mode_list_v2_set_refresh_rate(k->mode_list, CUSTOM_REFRESH);
            kde_mode_list_v2_add_mode(k->mode_list);
        }
    }
    kde_mode_list_v2_set_resolution(k->mode_list, width, height);
    kde_mode_list_v2_set_refresh_rate(k->mode_list, CUSTOM_REFRESH);
    kde_mode_list_v2_add_mode(k->mode_list);

    LOG(LOG_LEVEL_INFO, "wlup/kwin: adding a %dx%d mode to the output",
        width, height);
    config = kde_output_management_v2_create_configuration(k->management);
    kde_output_configuration_v2_add_listener(config, &config_listener, v);
    kde_output_configuration_v2_set_custom_modes(config, k->device,
            k->mode_list);
    kde_output_configuration_v2_apply(config);
    k->want_width = width;
    k->want_height = height;
    return 1;
}

/******************************************************************************/
/* Output devices                                                             */
/******************************************************************************/
/* KWin describes everything about an output; only its modes and whether
 * it is enabled matter here */
#define IGNORE_EVENT(name, ...) \
    static void \
    device_##name(void *data, struct kde_output_device_v2 *device, \
                  __VA_ARGS__) \
    { \
    }

IGNORE_EVENT(geometry, int32_t x, int32_t y, int32_t physical_width,
             int32_t physical_height, int32_t subpixel, const char *make,
             const char *model, int32_t transform)
IGNORE_EVENT(scale, wl_fixed_t factor)
IGNORE_EVENT(edid, const char *raw)
IGNORE_EVENT(uuid, const char *uuid)
IGNORE_EVENT(serial_number, const char *serial_number)
IGNORE_EVENT(eisa_id, const char *eisa_id)
IGNORE_EVENT(capabilities, uint32_t flags)
IGNORE_EVENT(overscan, uint32_t overscan)
IGNORE_EVENT(vrr_policy, uint32_t vrr_policy)
IGNORE_EVENT(rgb_range, uint32_t rgb_range)
IGNORE_EVENT(name, const char *name)
IGNORE_EVENT(high_dynamic_range, uint32_t hdr_enabled)
IGNORE_EVENT(sdr_brightness, uint32_t sdr_brightness)
IGNORE_EVENT(wide_color_gamut, uint32_t wcg_enabled)
IGNORE_EVENT(auto_rotate_policy, uint32_t policy)
IGNORE_EVENT(icc_profile_path, const char *profile_path)
IGNORE_EVENT(brightness_metadata, uint32_t max_peak_brightness,
             uint32_t max_frame_average_brightness, uint32_t min_brightness)
IGNORE_EVENT(brightness_overrides, int32_t max_peak_brightness,
             int32_t max_average_brightness, int32_t min_brightness)
IGNORE_EVENT(sdr_gamut_wideness, uint32_t gamut_wideness)
IGNORE_EVENT(color_profile_source, uint32_t source)
IGNORE_EVENT(brightness, uint32_t brightness)
IGNORE_EVENT(color_power_tradeoff, uint32_t preference)
IGNORE_EVENT(dimming, uint32_t multiplier)
IGNORE_EVENT(replication_source, const char *source)
IGNORE_EVENT(ddc_ci_allowed, uint32_t allowed)
IGNORE_EVENT(max_bits_per_color, uint32_t max_bpc)
IGNORE_EVENT(max_bits_per_color_range, uint32_t min_value,
             uint32_t max_value)
IGNORE_EVENT(automatic_max_bits_per_color_limit, uint32_t max_bpc_limit)
IGNORE_EVENT(edr_policy, uint32_t policy)
IGNORE_EVENT(sharpness, uint32_t sharpness)
IGNORE_EVENT(priority, uint32_t priority)
IGNORE_EVENT(auto_brightness, uint32_t enabled)

/******************************************************************************/
static void
device_current_mode(void *data, struct kde_output_device_v2 *device,
                    struct kde_output_device_mode_v2 *mode)
{
    struct wlup *v = (struct wlup *)data;
    struct wlup_kwin *k = KWIN(v);

    if (device == k->device || k->device == NULL)
    {
        k->current_mode = mode;
    }
}

/******************************************************************************/
static void
device_mode(void *data, struct kde_output_device_v2 *device,
            struct kde_output_device_mode_v2 *mode)
{
    struct wlup *v = (struct wlup *)data;
    struct wlup_kwin *k = KWIN(v);

    kde_output_device_mode_v2_add_listener(mode, &mode_listener, k);
    if (k->num_modes < MAX_MODES && (device == k->device || k->device == NULL))
    {
        k->modes[k->num_modes].mode = mode;
        k->modes[k->num_modes].width = 0;
        k->modes[k->num_modes].height = 0;
        ++k->num_modes;
    }
}

/******************************************************************************/
static void
device_enabled(void *data, struct kde_output_device_v2 *device,
               int32_t enabled)
{
    struct wlup *v = (struct wlup *)data;
    struct wlup_kwin *k = KWIN(v);

    k->device_enabled = enabled;
}

/******************************************************************************/
static void
device_done(void *data, struct kde_output_device_v2 *device)
{
    struct wlup *v = (struct wlup *)data;
    struct wlup_kwin *k = KWIN(v);
    struct kwin_mode *mode;

    if (k->device == NULL)
    {
        if (!k->device_enabled)
        {
            /* Its modes are not ours to keep */
            k->num_modes = 0;
            k->current_mode = NULL;
            return;
        }
        k->device = device;
    }
    if (device != k->device || k->want_width == 0)
    {
        return;
    }
    /* A custom mode asked for has arrived: switch to it */
    if ((mode = find_mode(k, k->want_width, k->want_height)) != NULL)
    {
        k->want_width = 0;
        k->want_height = 0;
        if (k->mode_list != NULL)
        {
            kde_mode_list_v2_destroy(k->mode_list);
            k->mode_list = NULL;
        }
        apply_mode(v, mode);
    }
}

/******************************************************************************/
static void
device_removed(void *data, struct kde_output_device_v2 *device)
{
    struct wlup *v = (struct wlup *)data;
    struct wlup_kwin *k = KWIN(v);

    if (device == k->device)
    {
        LOG(LOG_LEVEL_WARNING, "wlup/kwin: the output device was removed");
        k->device = NULL;
        k->num_modes = 0;
        k->current_mode = NULL;
    }
    kde_output_device_v2_destroy(device);
}

static const struct kde_output_device_v2_listener device_listener =
{
    .geometry = device_geometry,
    .current_mode = device_current_mode,
    .mode = device_mode,
    .done = device_done,
    .scale = device_scale,
    .edid = device_edid,
    .enabled = device_enabled,
    .uuid = device_uuid,
    .serial_number = device_serial_number,
    .eisa_id = device_eisa_id,
    .capabilities = device_capabilities,
    .overscan = device_overscan,
    .vrr_policy = device_vrr_policy,
    .rgb_range = device_rgb_range,
    .name = device_name,
    .high_dynamic_range = device_high_dynamic_range,
    .sdr_brightness = device_sdr_brightness,
    .wide_color_gamut = device_wide_color_gamut,
    .auto_rotate_policy = device_auto_rotate_policy,
    .icc_profile_path = device_icc_profile_path,
    .brightness_metadata = device_brightness_metadata,
    .brightness_overrides = device_brightness_overrides,
    .sdr_gamut_wideness = device_sdr_gamut_wideness,
    .color_profile_source = device_color_profile_source,
    .brightness = device_brightness,
    .color_power_tradeoff = device_color_power_tradeoff,
    .dimming = device_dimming,
    .replication_source = device_replication_source,
    .ddc_ci_allowed = device_ddc_ci_allowed,
    .max_bits_per_color = device_max_bits_per_color,
    .max_bits_per_color_range = device_max_bits_per_color_range,
    .automatic_max_bits_per_color_limit =
        device_automatic_max_bits_per_color_limit,
    .edr_policy = device_edr_policy,
    .sharpness = device_sharpness,
    .priority = device_priority,
    .auto_brightness = device_auto_brightness,
    .removed = device_removed,
};

/******************************************************************************/
static void
registry_device(void *data, struct kde_output_device_registry_v2 *registry,
                struct kde_output_device_v2 *device)
{
    struct wlup *v = (struct wlup *)data;
    struct wlup_kwin *k = KWIN(v);

    /* Each device sends its state, then done(); the first one enabled
     * becomes ours */
    k->device_enabled = 0;
    kde_output_device_v2_add_listener(device, &device_listener, v);
}

/******************************************************************************/
static void
registry_finished(void *data, struct kde_output_device_registry_v2 *registry)
{
    struct wlup *v = (struct wlup *)data;
    struct wlup_kwin *k = KWIN(v);

    kde_output_device_registry_v2_destroy(registry);
    k->device_registry = NULL;
}

static const struct kde_output_device_registry_v2_listener
    device_registry_listener =
{
    .finished = registry_finished,
    .output = registry_device,
};

/******************************************************************************/
/* Wayland globals                                                            */
/******************************************************************************/
static void
registry_global(void *data, struct wl_registry *registry, uint32_t name,
                const char *interface, uint32_t version)
{
    struct wlup *v = (struct wlup *)data;
    struct wlup_kwin *k = KWIN(v);

    /* KWin's protocols change between releases: what is bound depends
     * on what is offered, and the versions go to the log */
    if (strcmp(interface, zkde_screencast_unstable_v1_interface.name) == 0)
    {
        k->screencast_offered = version;
    }
    else if (strcmp(interface, org_kde_kwin_fake_input_interface.name) == 0)
    {
        k->fake_input_offered = version;
    }
    else if (strcmp(interface,
                    kde_output_device_registry_v2_interface.name) == 0)
    {
        k->device_registry_offered = version;
    }
    else if (strcmp(interface, kde_output_management_v2_interface.name) == 0)
    {
        k->management_offered = version;
    }
    else if (strcmp(interface, "ext_data_control_manager_v1") == 0)
    {
        k->data_control_offered = 1;
    }

    if (strcmp(interface, wl_seat_interface.name) == 0 && k->seat == NULL)
    {
        k->seat = (struct wl_seat *)
                  wl_registry_bind(registry, name, &wl_seat_interface, 1);
    }
    else if (strcmp(interface, wl_output_interface.name) == 0 &&
             k->output == NULL)
    {
        k->output = (struct wl_output *)
                    wl_registry_bind(registry, name, &wl_output_interface, 1);
    }
    else if (strcmp(interface,
                    zkde_screencast_unstable_v1_interface.name) == 0)
    {
        k->screencast = (struct zkde_screencast_unstable_v1 *)
                        wl_registry_bind(registry, name,
                                         &zkde_screencast_unstable_v1_interface,
                                         MIN(version, SCREENCAST_VERSION));
    }
    else if (strcmp(interface, org_kde_kwin_fake_input_interface.name) == 0 &&
             version >= FAKE_INPUT_VERSION)
    {
        k->fake_input = (struct org_kde_kwin_fake_input *)
                        wl_registry_bind(registry, name,
                                         &org_kde_kwin_fake_input_interface,
                                         FAKE_INPUT_VERSION);
    }
    else if (strcmp(interface,
                    kde_output_device_registry_v2_interface.name) == 0 &&
             version >= DEVICE_REGISTRY_VERSION)
    {
        k->device_registry = (struct kde_output_device_registry_v2 *)
                             wl_registry_bind(registry, name,
                                              &kde_output_device_registry_v2_interface,
                                              DEVICE_REGISTRY_VERSION);
        kde_output_device_registry_v2_add_listener(k->device_registry,
                &device_registry_listener, v);
    }
    else if (strcmp(interface, kde_output_management_v2_interface.name) == 0 &&
             version >= MANAGEMENT_VERSION)
    {
        k->management = (struct kde_output_management_v2 *)
                        wl_registry_bind(registry, name,
                                         &kde_output_management_v2_interface,
                                         MIN(version, (uint32_t)
                                             kde_output_management_v2_interface.version));
    }
    else if (k->clip == NULL)
    {
        k->clip = wlup_wlr_clip_bind(v, k->display, registry, name,
                                     interface);
    }
}

/******************************************************************************/
static void
registry_global_remove(void *data, struct wl_registry *registry,
                       uint32_t name)
{
}

static const struct wl_registry_listener registry_listener =
{
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/******************************************************************************/
/* Input                                                                      */
/******************************************************************************/
static void
kwin_key(struct wlup *v, int evdev_code, int down)
{
    struct wlup_kwin *k = KWIN(v);

    if (k == NULL || k->fake_input == NULL)
    {
        return;
    }
    /* KWin tracks the modifier state itself, as for a real keyboard */
    org_kde_kwin_fake_input_keyboard_key(k->fake_input, evdev_code, down);
}

/******************************************************************************/
static void
kwin_mouse(struct wlup *v, int msg, int x, int y)
{
    struct wlup_kwin *k = KWIN(v);
    uint32_t button = 0;
    int scroll = 0;
    int down = 0;

    if (k == NULL || k->fake_input == NULL)
    {
        return;
    }

    switch (msg)
    {
        case WM_LBUTTONDOWN:
            down = 1; /* fall through */
        case WM_LBUTTONUP:
            button = BTN_LEFT;
            break;
        case WM_RBUTTONDOWN:
            down = 1; /* fall through */
        case WM_RBUTTONUP:
            button = BTN_RIGHT;
            break;
        case WM_BUTTON3DOWN:
            down = 1; /* fall through */
        case WM_BUTTON3UP:
            button = BTN_MIDDLE;
            break;
        case WM_BUTTON8DOWN:
            down = 1; /* fall through */
        case WM_BUTTON8UP:
            button = BTN_SIDE;
            break;
        case WM_BUTTON9DOWN:
            down = 1; /* fall through */
        case WM_BUTTON9UP:
            button = BTN_EXTRA;
            break;
        case WM_BUTTON4DOWN:
            scroll = -1;
            break;
        case WM_BUTTON5DOWN:
            scroll = 1;
            break;
        default:
            break;
    }

    /* Scroll goes where the pointer is; see the note in wlup.c */
    if (scroll != 0)
    {
        org_kde_kwin_fake_input_axis(k->fake_input,
                                     WL_POINTER_AXIS_VERTICAL_SCROLL,
                                     wl_fixed_from_int(scroll * WHEEL_STEP));
        return;
    }
    if (msg >= WM_BUTTON4UP && msg <= WM_BUTTON7DOWN)
    {
        return;
    }

    /* The output is the whole of KWin's space, at scale 1 */
    org_kde_kwin_fake_input_pointer_motion_absolute(k->fake_input,
            wl_fixed_from_int(x), wl_fixed_from_int(y));
    if (button != 0)
    {
        org_kde_kwin_fake_input_button(k->fake_input, button,
                                       down ? WL_POINTER_BUTTON_STATE_PRESSED :
                                       WL_POINTER_BUTTON_STATE_RELEASED);
    }
}

/******************************************************************************/
static void
kwin_flush(struct wlup *v)
{
    struct wlup_kwin *k = KWIN(v);

    if (k != NULL && k->display != NULL)
    {
        wl_display_flush(k->display);
    }
}

/******************************************************************************/
/* Module side                                                                */
/******************************************************************************/
/* Logs what KWin offers and which features that gives */
static void
log_capabilities(struct wlup_kwin *k)
{
    LOG(LOG_LEVEL_INFO, "wlup/kwin: KWin offers zkde_screencast_unstable_v1 "
        "v%u, org_kde_kwin_fake_input v%u, kde_output_device_registry_v2 "
        "v%u, kde_output_management_v2 v%u, ext_data_control_manager_v1 %s "
        "(0: not offered to xrdp)", k->screencast_offered,
        k->fake_input_offered, k->device_registry_offered,
        k->management_offered, k->data_control_offered ? "yes" : "no");
    LOG(LOG_LEVEL_INFO, "wlup/kwin: capabilities: screen cast %s, input %s, "
        "clipboard %s, resize %s",
        k->screencast != NULL && k->output != NULL ? "yes" : "no",
        k->fake_input != NULL ? "yes" : "no (needs fake_input v4)",
        k->clip != NULL && k->seat != NULL ? "yes" : "no",
        k->device != NULL && k->management != NULL ? "yes" :
        "no (needs kde_output_device_registry_v2 v21 and "
        "kde_output_management_v2 v18)");
}

/* Waits for the stream's PipeWire node. return error */
static int
wait_for_node(struct wlup_kwin *k)
{
    unsigned int start = g_get_elapsed_ms();

    while (!k->have_node && !k->stream_failed && !k->closed)
    {
        struct pollfd pfd;

        if (g_get_elapsed_ms() - start > STREAM_TIMEOUT_MS)
        {
            LOG(LOG_LEVEL_ERROR, "wlup/kwin: KWin did not announce the "
                "PipeWire node of the screen cast");
            return 1;
        }
        wl_display_flush(k->display);
        pfd.fd = wl_display_get_fd(k->display);
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, 100) > 0 && wl_display_dispatch(k->display) < 0)
        {
            return 1;
        }
    }
    return !k->have_node;
}

/******************************************************************************/
static int
kwin_connect(struct wlup *v, int fd)
{
    struct wlup_kwin *k;
    char text[512];
    const char *name = v->display_name[0] != '\0' ? v->display_name : NULL;

    k = g_new0(struct wlup_kwin, 1);
    if (k == NULL)
    {
        return 1;
    }
    v->backend_data = k;
    if (fd >= 0)
    {
        /* sesman started KWin and connected to it for us */
        name = "(connection from sesman)";
        k->display = wl_display_connect_to_fd(fd);
    }
    else
    {
        k->display = wl_display_connect(name);
    }
    if (k->display == NULL)
    {
        g_snprintf(text, sizeof(text), "wlup error - cannot connect to KWin "
                   "at '%s'", name != NULL ? name : "$WAYLAND_DISPLAY");
        v->server_msg(v, text, 0);
        return 1;
    }

    k->registry = wl_display_get_registry(k->display);
    wl_registry_add_listener(k->registry, &registry_listener, v);
    wl_display_roundtrip(k->display);
    /* Events of the globals just bound: output devices and their modes */
    wl_display_roundtrip(k->display);
    log_capabilities(k);

    if (k->output == NULL || k->screencast == NULL)
    {
        /* server_msg() only reaches the login window: say it in the log
         * too */
        LOG(LOG_LEVEL_ERROR, "wlup/kwin: KWin offers%s%s to xrdp. Before "
            "KWin 6.8, set KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 for KWin",
            k->output == NULL ? " no wl_output" : "",
            k->screencast == NULL ? " no zkde_screencast_unstable_v1" : "");
        v->server_msg(v, "wlup error - KWin does not let xrdp cast its "
                      "output", 0);
        return 1;
    }
    if (k->fake_input != NULL)
    {
        org_kde_kwin_fake_input_authenticate(k->fake_input, "xrdp",
                                             "remote desktop");
        scancode_set_keycode_set("evdev");
    }
    else
    {
        LOG(LOG_LEVEL_WARNING, "wlup/kwin: no org_kde_kwin_fake_input "
            "version %d, no input", FAKE_INPUT_VERSION);
    }
    wlup_wlr_clip_start(k->clip, k->seat);

    k->stream = zkde_screencast_unstable_v1_stream_output(k->screencast,
                k->output, POINTER_METADATA);
    zkde_screencast_stream_unstable_v1_add_listener(k->stream,
            &stream_listener, k);
    if (wait_for_node(k) != 0)
    {
        v->server_msg(v, "wlup error - KWin cannot cast its output", 0);
        return 1;
    }
    k->pwc = wlup_pwc_connect(v, "kwin", k->node_id, v->server_width,
                              v->server_height);
    if (k->pwc == NULL)
    {
        v->server_msg(v, "wlup error - cannot receive the PipeWire stream",
                      0);
        return 1;
    }

    /* Match the output to the size the RDP client asked for */
    if (k->device != NULL && k->current_mode != NULL)
    {
        struct kwin_mode *mode = NULL;
        int i;

        for (i = 0; i < k->num_modes; ++i)
        {
            if (k->modes[i].mode == k->current_mode)
            {
                mode = &k->modes[i];
            }
        }
        if (mode != NULL && (mode->width != v->server_width ||
                             mode->height != v->server_height))
        {
            request_output_size(v, v->server_width, v->server_height);
        }
    }
    wl_display_flush(k->display);

    g_snprintf(text, sizeof(text), "wlup: connected to KWin, PipeWire node "
               "%u%s", k->node_id, k->fake_input != NULL ? ", fake input" : "");
    v->server_msg(v, text, 0);
    LOG(LOG_LEVEL_INFO, "%s", text);
    return 0;
}

/******************************************************************************/
static void
kwin_disconnect(struct wlup *v)
{
    struct wlup_kwin *k = KWIN(v);
    int i;

    if (k == NULL)
    {
        return;
    }
    wlup_pwc_destroy(k->pwc);
    wlup_wlr_clip_destroy(k->clip);
    if (k->stream != NULL)
    {
        zkde_screencast_stream_unstable_v1_close(k->stream);
    }
    if (k->screencast != NULL)
    {
        zkde_screencast_unstable_v1_destroy(k->screencast);
    }
    if (k->fake_input != NULL)
    {
        org_kde_kwin_fake_input_destroy(k->fake_input);
    }
    if (k->mode_list != NULL)
    {
        kde_mode_list_v2_destroy(k->mode_list);
    }
    for (i = 0; i < k->num_modes; ++i)
    {
        kde_output_device_mode_v2_destroy(k->modes[i].mode);
    }
    if (k->device != NULL)
    {
        kde_output_device_v2_destroy(k->device);
    }
    if (k->device_registry != NULL)
    {
        kde_output_device_registry_v2_destroy(k->device_registry);
    }
    if (k->management != NULL)
    {
        kde_output_management_v2_destroy(k->management);
    }
    if (k->output != NULL)
    {
        wl_output_destroy(k->output);
    }
    if (k->seat != NULL)
    {
        wl_seat_destroy(k->seat);
    }
    if (k->registry != NULL)
    {
        wl_registry_destroy(k->registry);
    }
    if (k->display != NULL)
    {
        wl_display_flush(k->display);
        wl_display_disconnect(k->display);
    }
    g_free(k);
    v->backend_data = NULL;
}

/******************************************************************************/
static void
kwin_get_wait_objs(struct wlup *v, tbus *read_objs, int *rcount,
                   tbus *write_objs, int *wcount, int *timeout)
{
    struct wlup_kwin *k = KWIN(v);

    if (k == NULL)
    {
        return;
    }
    if (k->display != NULL)
    {
        wl_display_flush(k->display);
        read_objs[(*rcount)++] = wl_display_get_fd(k->display);
    }
    if (k->pwc != NULL)
    {
        read_objs[(*rcount)++] = wlup_pwc_get_fd(k->pwc);
    }
    if (v->resize_pending && (*timeout < 0 || *timeout > 200))
    {
        /* Wake up to check the resize timeout */
        *timeout = 200;
    }
}

/******************************************************************************/
static int
kwin_check_wait_objs(struct wlup *v)
{
    struct wlup_kwin *k = KWIN(v);
    struct pollfd pfd;

    if (k == NULL || k->display == NULL)
    {
        return 0;
    }

    /* Standard libwayland pattern for an external event loop */
    while (wl_display_prepare_read(k->display) != 0)
    {
        if (wl_display_dispatch_pending(k->display) < 0)
        {
            break;
        }
    }
    wl_display_flush(k->display);
    pfd.fd = wl_display_get_fd(k->display);
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN))
    {
        wl_display_read_events(k->display);
    }
    else
    {
        wl_display_cancel_read(k->display);
    }
    wl_display_dispatch_pending(k->display);
    /* A new selection is read once all events are dispatched */
    wlup_wlr_clip_process(k->clip);
    wl_display_flush(k->display);
    if (wl_display_get_error(k->display) != 0)
    {
        LOG(LOG_LEVEL_ERROR, "wlup/kwin: Wayland connection error %d",
            wl_display_get_error(k->display));
        return 1;
    }

    if (k->pwc != NULL)
    {
        wlup_pwc_process(k->pwc);
    }
    if (v->resize_pending &&
            monotonic_usec() - k->resize_start > RESIZE_TIMEOUT_USEC)
    {
        LOG(LOG_LEVEL_WARNING, "wlup/kwin: the output was not resized, "
            "the picture will be clipped or padded");
        wlup_resize_finished(v);
    }
    return k->closed;
}

/******************************************************************************/
static int
kwin_resize(struct wlup *v, int width, int height)
{
    struct wlup_kwin *k = KWIN(v);

    if (k == NULL || !request_output_size(v, width, height))
    {
        return 0;
    }
    k->resize_start = monotonic_usec();
    wl_display_flush(k->display);
    return 1;
}

/******************************************************************************/
static void
kwin_frames_wanted(struct wlup *v)
{
    struct wlup_kwin *k = KWIN(v);

    if (k != NULL && k->pwc != NULL)
    {
        wlup_pwc_frames_wanted(k->pwc);
    }
}

/******************************************************************************/
static void
kwin_set_clipboard_text(struct wlup *v, const char *text, int len)
{
    struct wlup_kwin *k = KWIN(v);

    if (k != NULL)
    {
        wlup_wlr_clip_set_text(k->clip, text, len);
    }
}

/******************************************************************************/
/* Frames arrive from PipeWire unasked: frames_wanted() paints what was
 * held back meanwhile. The desktop image is wlup's own: no pixels_busy() */
static const struct wlup_backend_ops kwin_ops =
{
    .name = "kwin",
    .connect = kwin_connect,
    .disconnect = kwin_disconnect,
    .get_wait_objs = kwin_get_wait_objs,
    .check_wait_objs = kwin_check_wait_objs,
    .key = kwin_key,
    .mouse = kwin_mouse,
    .resize = kwin_resize,
    .frames_wanted = kwin_frames_wanted,
    .flush = kwin_flush,
    .set_clipboard_text = kwin_set_clipboard_text
};

/******************************************************************************/
const struct wlup_backend_ops *
wlup_backend_kwin(void)
{
    return &kwin_ops;
}
