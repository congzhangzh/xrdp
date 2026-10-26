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
 * wlup: Mutter (GNOME) backend
 *
 * Setup, all over the session bus:
 *   1. RemoteDesktop.CreateSession            -> remote desktop session
 *   2. ScreenCast.CreateSession, with the remote desktop session id
 *   3. ScreenCast.Session.RecordVirtual       -> stream; Mutter adds a
 *      virtual monitor whose size is the size we ask PipeWire for
 *   4. RemoteDesktop.Session.Start            -> PipeWireStreamAdded(node)
 *   5. RemoteDesktop.Session.ConnectToEIS     -> fd for libei input
 *
 * Three file descriptors then go into xrdp's poll loop, so no extra
 * threads are needed:
 *   sd-bus connection   session Closed signal
 *   PipeWire loop fd    frames (pw_loop_iterate(loop, 0) when readable)
 *   libei fd            input devices appearing, pausing, resuming
 *
 * sd-bus was chosen for D-Bus because it exposes exactly what a poll
 * loop needs (sd_bus_get_fd/get_events/process). Mutter needs systemd
 * and logind anyway, so depending on libsystemd costs nothing here.
 */

#if defined(HAVE_CONFIG_H)
#include <config_ac.h>
#endif

#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <linux/input-event-codes.h>
#include <systemd/sd-bus.h>
#include <libei.h>

#include "wlup.h"
#include "mutter.h"
#include "../pipewire/capture.h"
#include "wlup_clip.h"
#include "log.h"
#include "string_calls.h"
#include "xrdp_constants.h"
#include "ms-rdpbcgr.h"
#include "scancode.h"

#define RD_NAME "org.gnome.Mutter.RemoteDesktop"
#define RD_PATH "/org/gnome/Mutter/RemoteDesktop"
#define RD_SESSION_IFACE "org.gnome.Mutter.RemoteDesktop.Session"
#define SC_NAME "org.gnome.Mutter.ScreenCast"
#define SC_PATH "/org/gnome/Mutter/ScreenCast"
#define SC_SESSION_IFACE "org.gnome.Mutter.ScreenCast.Session"
#define SC_STREAM_IFACE "org.gnome.Mutter.ScreenCast.Stream"
#define DC_NAME "org.gnome.Mutter.DisplayConfig"
#define DC_PATH "/org/gnome/Mutter/DisplayConfig"

/* DisplayConfig.ApplyMonitorsConfig: for this session, not saved */
#define DC_METHOD_TEMPORARY 1

/* ScreenCast cursor mode: the cursor is sent as buffer metadata, not
 * painted into the frames, so the RDP client can draw it locally */
#define CURSOR_MODE_METADATA 2

/* Mutter's D-Bus APIs are private and change between GNOME releases;
 * each has a Version property. RecordVirtual takes "cursor-mode" from
 * ScreenCast API version 2 and "is-platform" from version 3 (Mutter's
 * data/dbus-interfaces/org.gnome.Mutter.ScreenCast.xml). The
 * RemoteDesktop methods have no version notes: the session object is
 * introspected for them instead */
#define SC_VERSION_CURSOR_MODE 2
#define SC_VERSION_IS_PLATFORM 3

/* RemoteDesktop SupportedDeviceTypes */
#define DEVICE_TYPE_KEYBOARD 1
#define DEVICE_TYPE_POINTER 2

/* Clipboard transfers to the session queued between two polls */
#define MAX_TRANSFERS 16

/* Time allowed for Mutter to announce the PipeWire stream */
#define STREAM_TIMEOUT_USEC (5 * 1000 * 1000)

/* Time allowed for GNOME Shell to come up in a session sesman has just
 * started, and offer its remote desktop API */
#define MUTTER_TIMEOUT_USEC (60 * 1000 * 1000)

/* Time allowed for Mutter to resize the virtual monitor. xrdp waits for
 * the resize, so give up after this and let the picture be clipped */
#define RESIZE_TIMEOUT_USEC (5 * 1000 * 1000)

/* One virtual monitor of GNOME's: a ScreenCast stream, and its frames */
struct mutter_output
{
    struct wlup *v;
    char *stream_path;
    uint32_t node_id;
    int have_node;
    char *mapping_id;     /* the stream's, = its EIS region's; or NULL */
    struct wlup_pwc *pwc; /* frames (pipewire/capture.c) */
    /* the RDP monitor it shows: a rect of the desktop image */
    int x;
    int y;
    int width;
    int height;
    int primary; /* the client's primary monitor: GNOME's top bar */
};

struct wlup_mutter
{
    /* D-Bus */
    sd_bus *bus;
    sd_bus_slot *closed_slot;
    sd_bus_slot *stream_added_slot; /* any stream's PipeWireStreamAdded */
    int started; /* RemoteDesktop.Session.Start done */
    char *rd_session_path;
    char *sc_session_path;
    int closed;

    /* A virtual monitor per RDP monitor, left to right. out[0]'s pwc owns
     * the PipeWire connection; with one output it sizes the desktop image
     * itself, with more the backend does (wlup_pwc_place()) */
    struct mutter_output out[WLUP_MAX_MONITORS];
    int num_out;
    int box_width;  /* the desktop image the outputs are placed in */
    int box_height;
    int layout_applied; /* GNOME has the outputs' positions and primary */

    /* libei */
    struct ei *ei;
    struct ei_device *keyboard;
    struct ei_device *pointer; /* absolute pointer, also buttons/scroll */
    uint32_t sequence;
    int pointer_placed; /* pointer moved away from the hot corner */

    uint64_t resize_start; /* when the pending resize was requested */

    /* What this GNOME offers (probe_api(), probe_session()) */
    int sc_version;         /* ScreenCast API version */
    int rd_version;         /* RemoteDesktop API version */
    uint32_t device_types;  /* RemoteDesktop SupportedDeviceTypes */
    char shell_version[32]; /* GNOME Shell's, or empty */
    int has_eis;            /* the session has ConnectToEIS */
    int has_clipboard;      /* the session has EnableClipboard */

    /* clipboard */
    sd_bus_slot *owner_changed_slot;
    sd_bus_slot *transfer_slot;
    int clipboard_enabled;
    const char *read_mime;  /* session selection to read, or NULL */
    uint32_t transfers[MAX_TRANSFERS]; /* SelectionTransfer serials */
    int num_transfers;
    char *client_text;      /* the RDP client's clipboard text */
    int client_text_len;
};

/* MIME types accepted and offered for text, best first */
static const char *const g_text_mime_types[] =
{
    "text/plain;charset=utf-8",
    "text/plain",
    "UTF8_STRING",
    "STRING",
    "TEXT",
    NULL
};

/******************************************************************************/
static uint64_t
monotonic_usec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/******************************************************************************/
int
wlup_mutter_resize(struct wlup *v, int width, int height)
{
    struct wlup_mutter *m = (struct wlup_mutter *)v->backend_data;

    if (m == NULL || m->out[0].pwc == NULL)
    {
        return 0;
    }
    /* Offering a new preferred size renegotiates the stream format, and
     * Mutter resizes the virtual monitor to match */
    LOG(LOG_LEVEL_INFO, "wlup/mutter: requesting virtual monitor size %dx%d",
        width, height);
    if (wlup_pwc_set_size(m->out[0].pwc, width, height) != 0)
    {
        return 0;
    }
    m->resize_start = monotonic_usec();
    return 1;
}

/******************************************************************************/
/* Whether every output streams at its monitor's size */
static int
outputs_sized(struct wlup_mutter *m)
{
    int i;

    for (i = 0; i < m->num_out; ++i)
    {
        int w;
        int h;

        if (m->out[i].pwc == NULL)
        {
            return 0;
        }
        wlup_pwc_get_size(m->out[i].pwc, &w, &h);
        if (w != m->out[i].width || h != m->out[i].height)
        {
            return 0;
        }
    }
    return 1;
}

/******************************************************************************/
/* Places the outputs in the desktop image: from two on, the backend
 * owns the image (box_width x box_height) and each stream copies to its
 * monitor's rect of it */
static void on_output_sized(void *data);

static void
place_outputs(struct wlup *v)
{
    struct wlup_mutter *m = (struct wlup_mutter *)v->backend_data;
    int i;

    for (i = 0; i < m->num_out; ++i)
    {
        if (m->out[i].pwc == NULL)
        {
            continue;
        }
        if (m->num_out > 1)
        {
            wlup_pwc_place(m->out[i].pwc, m->out[i].x, m->out[i].y,
                           on_output_sized, &m->out[i]);
        }
        else
        {
            wlup_pwc_place(m->out[i].pwc, 0, 0, NULL, NULL);
        }
    }
}

/******************************************************************************/
/* GNOME lines up virtual monitors in the order they were made and makes
 * the first one primary. Give it the client's layout instead: each
 * output's position, and its primary monitor (the top bar), with
 * DisplayConfig, as GNOME Settings does. Mutter names its virtual
 * monitors Meta-<n>, counting up as they are made, and outputs are made
 * in their order (added and removed at the end only): sorted by <n>, the
 * connectors are the outputs. Nothing is applied when that does not
 * hold (a count or a size that differs). return error */
static int
apply_layout(struct wlup *v)
{
    struct wlup_mutter *m = (struct wlup_mutter *)v->backend_data;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    sd_bus_message *msg = NULL;
    struct
    {
        int n;
        char connector[32];
        char mode[64];
        int width;
        int height;
    } mon[WLUP_MAX_MONITORS], t;
    int count = 0;
    uint32_t serial;
    int primary = 0;
    int i;
    int j;
    int r;

    r = sd_bus_call_method(m->bus, DC_NAME, DC_PATH, DC_NAME,
                           "GetCurrentState", &err, &reply, "");
    if (r < 0 || sd_bus_message_read(reply, "u", &serial) < 0)
    {
        LOG(LOG_LEVEL_WARNING, "wlup/mutter: no DisplayConfig state: %s",
            err.message != NULL ? err.message : "bad reply");
        sd_bus_error_free(&err);
        sd_bus_message_unref(reply);
        return 1;
    }
    /* monitors: ((connector, vendor, product, serial), modes, properties) */
    sd_bus_message_enter_container(reply, 'a', "((ssss)a(siiddada{sv})a{sv})");
    while (sd_bus_message_enter_container(reply, 'r',
                                          "(ssss)a(siiddada{sv})a{sv}") > 0)
    {
        const char *connector = "";
        const char *s1;
        const char *s2;
        const char *s3;
        const char *mode_id;
        const char *key;
        int32_t w;
        int32_t h;
        double d1;
        double d2;
        int current;
        int is_ours;

        sd_bus_message_read(reply, "(ssss)", &connector, &s1, &s2, &s3);
        is_ours = strncmp(connector, "Meta-", 5) == 0 &&
                  count < WLUP_MAX_MONITORS;
        if (is_ours)
        {
            mon[count].n = atoi(connector + 5);
            g_strncpy(mon[count].connector, connector,
                      sizeof(mon[count].connector) - 1);
            mon[count].mode[0] = '\0';
        }
        /* modes: (id, width, height, refresh, scale, scales, properties) */
        sd_bus_message_enter_container(reply, 'a', "(siiddada{sv})");
        while (sd_bus_message_enter_container(reply, 'r', "siiddada{sv}") > 0)
        {
            sd_bus_message_read(reply, "siidd", &mode_id, &w, &h, &d1, &d2);
            sd_bus_message_skip(reply, "ad");
            current = 0;
            sd_bus_message_enter_container(reply, 'a', "{sv}");
            while (sd_bus_message_enter_container(reply, 'e', "sv") > 0)
            {
                sd_bus_message_read(reply, "s", &key);
                if (strcmp(key, "is-current") == 0)
                {
                    sd_bus_message_read(reply, "v", "b", &current);
                }
                else
                {
                    sd_bus_message_skip(reply, "v");
                }
                sd_bus_message_exit_container(reply);
            }
            sd_bus_message_exit_container(reply);
            if (is_ours && current)
            {
                g_strncpy(mon[count].mode, mode_id,
                          sizeof(mon[count].mode) - 1);
                mon[count].width = w;
                mon[count].height = h;
            }
            sd_bus_message_exit_container(reply);
        }
        sd_bus_message_exit_container(reply);
        sd_bus_message_skip(reply, "a{sv}");
        sd_bus_message_exit_container(reply);
        if (is_ours && mon[count].mode[0] != '\0')
        {
            ++count;
        }
    }
    sd_bus_message_unref(reply);

    /* creation order */
    for (i = 1; i < count; ++i)
    {
        t = mon[i];
        for (j = i; j > 0 && mon[j - 1].n > t.n; --j)
        {
            mon[j] = mon[j - 1];
        }
        mon[j] = t;
    }
    if (count != m->num_out)
    {
        LOG(LOG_LEVEL_WARNING, "wlup/mutter: GNOME has %d virtual monitors, "
            "not %d: their layout is GNOME's", count, m->num_out);
        return 1;
    }
    for (i = 0; i < count; ++i)
    {
        if (mon[i].width != m->out[i].width ||
                mon[i].height != m->out[i].height)
        {
            LOG(LOG_LEVEL_WARNING, "wlup/mutter: %s is %dx%d, not %dx%d: "
                "the layout is GNOME's", mon[i].connector, mon[i].width,
                mon[i].height, m->out[i].width, m->out[i].height);
            return 1;
        }
        if (m->out[i].primary)
        {
            primary = i;
        }
    }

    sd_bus_message_new_method_call(m->bus, &msg, DC_NAME, DC_PATH, DC_NAME,
                                   "ApplyMonitorsConfig");
    sd_bus_message_append(msg, "uu", serial, (uint32_t)DC_METHOD_TEMPORARY);
    sd_bus_message_open_container(msg, 'a', "(iiduba(ssa{sv}))");
    for (i = 0; i < count; ++i)
    {
        sd_bus_message_open_container(msg, 'r', "iiduba(ssa{sv})");
        sd_bus_message_append(msg, "iidub", (int32_t)m->out[i].x,
                              (int32_t)m->out[i].y, 1.0, (uint32_t)0,
                              i == primary);
        sd_bus_message_open_container(msg, 'a', "(ssa{sv})");
        sd_bus_message_append(msg, "(ssa{sv})", mon[i].connector,
                              mon[i].mode, 0);
        sd_bus_message_close_container(msg);
        sd_bus_message_close_container(msg);
    }
    sd_bus_message_close_container(msg);
    sd_bus_message_append(msg, "a{sv}", 0);
    r = sd_bus_call(m->bus, msg, 0, &err, NULL);
    sd_bus_message_unref(msg);
    if (r < 0)
    {
        LOG(LOG_LEVEL_WARNING, "wlup/mutter: ApplyMonitorsConfig failed: %s",
            err.message != NULL ? err.message : strerror(-r));
        sd_bus_error_free(&err);
        return 1;
    }
    LOG(LOG_LEVEL_INFO, "wlup/mutter: GNOME has the client's layout, "
        "primary monitor %d (%s)", primary, mon[primary].connector);
    return 0;
}

/******************************************************************************/
/* With several outputs: once each streams at its size, the desktop image
 * gets the new layout and xrdp's resize is finished */
static void
check_outputs(struct wlup *v)
{
    struct wlup_mutter *m = (struct wlup_mutter *)v->backend_data;
    int i;

    if (m->num_out < 2 || !outputs_sized(m))
    {
        return;
    }
    if (v->pixels == NULL || v->buffer_width != m->box_width ||
            v->buffer_height != m->box_height)
    {
        if (wlup_pwc_alloc_pixels(v, m->box_width, m->box_height) != 0)
        {
            LOG(LOG_LEVEL_ERROR, "wlup/mutter: out of memory");
            return;
        }
    }
    for (i = 0; i < m->num_out; ++i)
    {
        wlup_pwc_repaint(m->out[i].pwc);
    }
    if (!m->layout_applied)
    {
        /* once per layout: a failure leaves GNOME's own */
        m->layout_applied = 1;
        apply_layout(v);
    }
    if (v->resize_pending)
    {
        LOG(LOG_LEVEL_INFO, "wlup/mutter: %d virtual monitors resized, "
            "desktop %dx%d", m->num_out, m->box_width, m->box_height);
        wlup_resize_finished(v);
    }
}

/******************************************************************************/
static void
on_output_sized(void *data)
{
    struct mutter_output *o = (struct mutter_output *)data;

    check_outputs(o->v);
}

/******************************************************************************/
/* Sorts monitors left to right, then top to bottom: Mutter lines up its
 * virtual monitors in the order they are made */
static void
sort_monitors(struct monitor_info *mons, int count)
{
    int i;
    int j;

    for (i = 1; i < count; ++i)
    {
        struct monitor_info t = mons[i];

        for (j = i; j > 0 && (mons[j - 1].left > t.left ||
                              (mons[j - 1].left == t.left &&
                               mons[j - 1].top > t.top)); --j)
        {
            mons[j] = mons[j - 1];
        }
        mons[j] = t;
    }
}

/******************************************************************************/
/* Sets the outputs' rects from monitors (non-negative, right and bottom
 * inclusive), sorted; count is clamped to what wlup takes */
static int
set_layout(struct wlup_mutter *m, int width, int height, int count,
           const struct monitor_info *monitors,
           struct monitor_info *sorted)
{
    int i;

    count = MIN(count, WLUP_MAX_MONITORS);
    for (i = 0; i < count; ++i)
    {
        sorted[i] = monitors[i];
    }
    sort_monitors(sorted, count);
    m->box_width = width;
    m->box_height = height;
    return count;
}

/******************************************************************************/
static int record_virtual(struct wlup *v, struct mutter_output *o);
static void stop_output(struct wlup_mutter *m, struct mutter_output *o);

int
wlup_mutter_resize_monitors(struct wlup *v, int width, int height,
                            int num_monitors,
                            const struct monitor_info *monitors)
{
    struct wlup_mutter *m = (struct wlup_mutter *)v->backend_data;
    struct monitor_info mons[WLUP_MAX_MONITORS];
    char text[512];
    int len = 0;
    int pending = 0;
    int i;

    if (m == NULL || m->out[0].pwc == NULL)
    {
        return 0;
    }
    num_monitors = set_layout(m, width, height, num_monitors, monitors, mons);
    if (num_monitors <= 1 && m->num_out <= 1)
    {
        /* One monitor, as before: its stream sizes the image */
        m->out[0].width = width;
        m->out[0].height = height;
        if (width == v->buffer_width && height == v->buffer_height)
        {
            return 0;
        }
        return wlup_mutter_resize(v, width, height);
    }

    /* Fewer monitors: the last virtual monitors go */
    while (m->num_out > num_monitors)
    {
        stop_output(m, &m->out[--m->num_out]);
    }
    text[0] = '\0';
    for (i = 0; i < num_monitors; ++i)
    {
        struct mutter_output *o = &m->out[i];
        int w = mons[i].right - mons[i].left + 1;
        int h = mons[i].bottom - mons[i].top + 1;

        o->v = v;
        o->x = mons[i].left;
        o->y = mons[i].top;
        o->primary = mons[i].is_primary;
        len += g_snprintf(text + len, sizeof(text) - len, "%s%dx%d+%d+%d",
                          i > 0 ? ", " : "", w, h, o->x, o->y);
        len = MIN(len, (int)sizeof(text) - 1);
        if (i >= m->num_out)
        {
            /* More monitors: a new virtual monitor; its stream comes up
             * in check_wait_objs() */
            o->width = w;
            o->height = h;
            if (record_virtual(v, o) != 0)
            {
                num_monitors = i;
                break;
            }
            pending = 1;
            continue;
        }
        if (o->pwc != NULL)
        {
            int cw;
            int ch;

            wlup_pwc_get_size(o->pwc, &cw, &ch);
            if ((cw != w || ch != h) && wlup_pwc_set_size(o->pwc, w, h) == 0)
            {
                pending = 1;
            }
        }
        o->width = w;
        o->height = h;
    }
    m->num_out = MAX(num_monitors, 1);
    m->layout_applied = 0;
    LOG(LOG_LEVEL_INFO, "wlup/mutter: %d virtual monitor%s in %dx%d: %s",
        m->num_out, m->num_out > 1 ? "s" : "", width, height, text);
    place_outputs(v);
    if (m->num_out == 1)
    {
        /* Back to one: out[0]'s stream sizes the image again */
        if (wlup_pwc_alloc_pixels(v, width, height) == 0)
        {
            wlup_pwc_repaint(m->out[0].pwc);
        }
        if (!pending)
        {
            return 0;
        }
        m->resize_start = monotonic_usec();
        return 1;
    }
    if (!pending && outputs_sized(m))
    {
        /* Only the layout changed: done now. xrdp's resize is not pending
         * yet (we return 0), so check_outputs() does not finish it */
        check_outputs(v);
        return 0;
    }
    if (wlup_pwc_alloc_pixels(v, width, height) != 0)
    {
        return 0;
    }
    m->resize_start = monotonic_usec();
    return 1;
}

/******************************************************************************/
/* libei                                                                      */
/******************************************************************************/
static void
process_ei_events(struct wlup *v)
{
    struct wlup_mutter *m = (struct wlup_mutter *)v->backend_data;
    struct ei_event *e;

    ei_dispatch(m->ei);
    while ((e = ei_get_event(m->ei)) != NULL)
    {
        struct ei_device *dev = ei_event_get_device(e);

        switch (ei_event_get_type(e))
        {
            case EI_EVENT_SEAT_ADDED:
                ei_seat_bind_capabilities(ei_event_get_seat(e),
                                          EI_DEVICE_CAP_POINTER_ABSOLUTE,
                                          EI_DEVICE_CAP_KEYBOARD,
                                          EI_DEVICE_CAP_BUTTON,
                                          EI_DEVICE_CAP_SCROLL,
                                          NULL);
                break;
            case EI_EVENT_DEVICE_ADDED:
                if (ei_device_has_capability(dev, EI_DEVICE_CAP_KEYBOARD) &&
                        m->keyboard == NULL)
                {
                    m->keyboard = ei_device_ref(dev);
                    LOG(LOG_LEVEL_INFO, "wlup/mutter: EIS keyboard '%s'",
                        ei_device_get_name(dev));
                }
                if (ei_device_has_capability(dev,
                                             EI_DEVICE_CAP_POINTER_ABSOLUTE) &&
                        m->pointer == NULL)
                {
                    m->pointer = ei_device_ref(dev);
                    LOG(LOG_LEVEL_INFO, "wlup/mutter: EIS pointer '%s'",
                        ei_device_get_name(dev));
                }
                break;
            case EI_EVENT_DEVICE_RESUMED:
                ei_device_start_emulating(dev, ++m->sequence);
                if (dev == m->pointer)
                {
                    struct ei_region *r;
                    size_t i;

                    for (i = 0; (r = ei_device_get_region(dev, i)) != NULL; ++i)
                    {
                        const char *id = ei_region_get_mapping_id(r);

                        LOG(LOG_LEVEL_INFO, "wlup/mutter: EIS region %d: "
                            "%ux%u+%u+%u, mapping id %s", (int)i,
                            ei_region_get_width(r), ei_region_get_height(r),
                            ei_region_get_x(r), ei_region_get_y(r),
                            id != NULL ? id : "none");
                    }
                }
                if (dev == m->pointer && !m->pointer_placed)
                {
                    /* GNOME's pointer starts at (0,0), its hot corner,
                     * where the first motion opens the Activities
                     * overview. Start in the middle of the screen */
                    struct ei_region *r = ei_device_get_region(dev, 0);
                    if (r != NULL)
                    {
                        ei_device_pointer_motion_absolute(dev,
                                                          ei_region_get_x(r) + ei_region_get_width(r) / 2.0,
                                                          ei_region_get_y(r) + ei_region_get_height(r) / 2.0);
                        ei_device_frame(dev, ei_now(m->ei));
                        m->pointer_placed = 1;
                    }
                }
                break;
            case EI_EVENT_DEVICE_REMOVED:
                if (dev == m->keyboard)
                {
                    ei_device_unref(m->keyboard);
                    m->keyboard = NULL;
                }
                if (dev == m->pointer)
                {
                    ei_device_unref(m->pointer);
                    m->pointer = NULL;
                }
                break;
            case EI_EVENT_DISCONNECT:
                LOG(LOG_LEVEL_WARNING, "wlup/mutter: EIS disconnected, "
                    "input stops");
                break;
            default:
                break;
        }
        ei_event_unref(e);
    }
}

/******************************************************************************/
void
wlup_mutter_key(struct wlup *v, int evdev_code, int down)
{
    struct wlup_mutter *m = (struct wlup_mutter *)v->backend_data;

    if (m == NULL || m->keyboard == NULL)
    {
        return;
    }
    /* Mutter tracks the modifier state itself, as for a real keyboard */
    ei_device_keyboard_key(m->keyboard, evdev_code, down);
    ei_device_frame(m->keyboard, ei_now(m->ei));
}

/******************************************************************************/
/* The output whose monitor holds x, y, or the nearest one: a pointer in
 * a gap of the layout (monitors of different heights) stays on a monitor */
static const struct mutter_output *
output_at(const struct wlup_mutter *m, int x, int y)
{
    const struct mutter_output *best = &m->out[0];
    long best_d = -1;
    int i;

    for (i = 0; i < m->num_out; ++i)
    {
        const struct mutter_output *o = &m->out[i];
        long dx = x < o->x ? o->x - x : x >= o->x + o->width ?
                  x - (o->x + o->width - 1) : 0;
        long dy = y < o->y ? o->y - y : y >= o->y + o->height ?
                  y - (o->y + o->height - 1) : 0;
        long d = dx * dx + dy * dy;

        if (best_d < 0 || d < best_d)
        {
            best = o;
            best_d = d;
        }
    }
    return best;
}

/******************************************************************************/
/* The EIS region of an output's virtual monitor: the one with its
 * stream's mapping id, else (no mapping ids) the region in the order the
 * monitors were made */
static struct ei_region *
output_region(const struct wlup_mutter *m, struct ei_device *dev,
              const struct mutter_output *o)
{
    struct ei_region *r;
    size_t i;

    if (o->mapping_id != NULL)
    {
        for (i = 0; (r = ei_device_get_region(dev, i)) != NULL; ++i)
        {
            const char *id = ei_region_get_mapping_id(r);

            if (id != NULL && strcmp(id, o->mapping_id) == 0)
            {
                return r;
            }
        }
    }
    return ei_device_get_region(dev, (size_t)(o - m->out));
}

/******************************************************************************/
void
wlup_mutter_mouse(struct wlup *v, int msg, int x, int y)
{
    struct wlup_mutter *m = (struct wlup_mutter *)v->backend_data;
    struct ei_device *dev;
    struct ei_region *region;
    uint32_t button = 0;
    int scroll = 0;
    int down = 0;

    if (m == NULL || m->pointer == NULL)
    {
        return;
    }
    dev = m->pointer;

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
        if (ei_device_has_capability(dev, EI_DEVICE_CAP_SCROLL))
        {
            ei_device_scroll_discrete(dev, 0, scroll * 120);
            ei_device_frame(dev, ei_now(m->ei));
        }
        return;
    }
    if (msg >= WM_BUTTON4UP && msg <= WM_BUTTON7DOWN)
    {
        return;
    }

    /* Absolute coordinates are in the region of the virtual monitor
     * under the pointer */
    if (m->num_out > 1)
    {
        const struct mutter_output *o = output_at(m, x, y);

        region = output_region(m, dev, o);
        if (region != NULL && o->width > 0 && o->height > 0)
        {
            ei_device_pointer_motion_absolute(dev,
                                              ei_region_get_x(region) +
                                              (double)MAX(0, MIN(x - o->x, o->width - 1)) *
                                              ei_region_get_width(region) / o->width,
                                              ei_region_get_y(region) +
                                              (double)MAX(0, MIN(y - o->y, o->height - 1)) *
                                              ei_region_get_height(region) / o->height);
        }
    }
    else if ((region = ei_device_get_region(dev, 0)) != NULL &&
             v->buffer_width > 0 && v->buffer_height > 0)
    {
        ei_device_pointer_motion_absolute(dev,
                                          ei_region_get_x(region) +
                                          (double)x * ei_region_get_width(region) / v->buffer_width,
                                          ei_region_get_y(region) +
                                          (double)y * ei_region_get_height(region) / v->buffer_height);
    }
    else
    {
        ei_device_pointer_motion_absolute(dev, x, y);
    }
    ei_device_frame(dev, ei_now(m->ei));

    if (button != 0 && ei_device_has_capability(dev, EI_DEVICE_CAP_BUTTON))
    {
        ei_device_button_button(dev, button, down);
        ei_device_frame(dev, ei_now(m->ei));
    }
}

/******************************************************************************/
/* D-Bus                                                                      */
/******************************************************************************/
/* PipeWireStreamAdded of any stream: matched before RecordVirtual is
 * called, as in a started session Mutter starts the stream and announces
 * it at once; the output is found by the stream's path */
static int
on_stream_added(sd_bus_message *msg, void *data, sd_bus_error *err)
{
    struct wlup *v = (struct wlup *)data;
    struct wlup_mutter *m = (struct wlup_mutter *)v->backend_data;
    const char *path = sd_bus_message_get_path(msg);
    int i;

    for (i = 0; i < m->num_out; ++i)
    {
        struct mutter_output *o = &m->out[i];

        if (path != NULL && o->stream_path != NULL &&
                strcmp(path, o->stream_path) == 0 &&
                sd_bus_message_read(msg, "u", &o->node_id) >= 0)
        {
            o->have_node = 1;
            LOG(LOG_LEVEL_INFO, "wlup/mutter: PipeWire node %u",
                o->node_id);
        }
    }
    return 0;
}

/******************************************************************************/
/* Reads the stream's "mapping-id" (its Parameters property, ScreenCast
 * API version 4 on): the EIS region of its virtual monitor has the same
 * one, which is how input finds the monitor */
static void
read_mapping_id(struct wlup_mutter *m, struct mutter_output *o)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    const char *key;
    const char *id;

    if (sd_bus_get_property(m->bus, SC_NAME, o->stream_path, SC_STREAM_IFACE,
                            "Parameters", &err, &reply, "a{sv}") < 0)
    {
        LOG(LOG_LEVEL_DEBUG, "wlup/mutter: no stream Parameters: %s",
            err.message != NULL ? err.message : "?");
        sd_bus_error_free(&err);
        return;
    }
    if (sd_bus_message_enter_container(reply, 'a', "{sv}") > 0)
    {
        while (sd_bus_message_enter_container(reply, 'e', "sv") > 0)
        {
            if (sd_bus_message_read(reply, "s", &key) < 0)
            {
                break;
            }
            if (strcmp(key, "mapping-id") == 0 &&
                    sd_bus_message_read(reply, "v", "s", &id) >= 0)
            {
                g_free(o->mapping_id);
                o->mapping_id = g_strdup(id);
            }
            else
            {
                sd_bus_message_skip(reply, "v");
            }
            sd_bus_message_exit_container(reply);
        }
    }
    sd_bus_message_unref(reply);
}

/******************************************************************************/
static int
on_closed(sd_bus_message *msg, void *data, sd_bus_error *err)
{
    struct wlup *v = (struct wlup *)data;
    struct wlup_mutter *m = (struct wlup_mutter *)v->backend_data;

    LOG(LOG_LEVEL_INFO, "wlup/mutter: remote desktop session closed");
    m->closed = 1;
    return 0;
}

/******************************************************************************/
/* Calls a method returning one object path; *path receives a copy.
 * return error */
static int
call_for_path(struct wlup_mutter *m, const char *dest, const char *path,
              const char *iface, const char *method, char **out,
              sd_bus_message *args)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    const char *result;
    int r;

    r = sd_bus_call(m->bus, args, 0, &err, &reply);
    if (r < 0)
    {
        LOG(LOG_LEVEL_ERROR, "wlup/mutter: %s.%s failed: %s", iface, method,
            err.message != NULL ? err.message : strerror(-r));
        sd_bus_error_free(&err);
        return 1;
    }
    r = sd_bus_message_read(reply, "o", &result);
    if (r >= 0)
    {
        *out = g_strdup(result);
    }
    sd_bus_message_unref(reply);
    return r < 0;
}

/******************************************************************************/
/* Reads the API versions of the running GNOME. return error if it is too
 * old for wlup */
static int
probe_api(struct wlup_mutter *m)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    char *shell_version = NULL;
    int32_t version;

    if (sd_bus_get_property_trivial(m->bus, SC_NAME, SC_PATH, SC_NAME,
                                    "Version", &err, 'i', &version) < 0)
    {
        LOG(LOG_LEVEL_ERROR, "wlup/mutter: no ScreenCast API version: %s",
            err.message);
        sd_bus_error_free(&err);
        return 1;
    }
    m->sc_version = version;
    if (sd_bus_get_property_trivial(m->bus, RD_NAME, RD_PATH, RD_NAME,
                                    "Version", &err, 'i', &version) < 0)
    {
        LOG(LOG_LEVEL_ERROR, "wlup/mutter: no RemoteDesktop API version: %s",
            err.message);
        sd_bus_error_free(&err);
        return 1;
    }
    m->rd_version = version;
    if (sd_bus_get_property_trivial(m->bus, RD_NAME, RD_PATH, RD_NAME,
                                    "SupportedDeviceTypes", &err, 'u',
                                    &m->device_types) < 0)
    {
        sd_bus_error_free(&err);
        m->device_types = 0;
    }
    /* Only for the log: GNOME Shell may not be the compositor */
    if (sd_bus_get_property_string(m->bus, "org.gnome.Shell",
                                   "/org/gnome/Shell", "org.gnome.Shell",
                                   "ShellVersion", &err, &shell_version) >= 0)
    {
        g_strncpy(m->shell_version, shell_version,
                  sizeof(m->shell_version) - 1);
        free(shell_version);
    }
    sd_bus_error_free(&err);

    LOG(LOG_LEVEL_INFO, "wlup/mutter: GNOME Shell %s, ScreenCast API "
        "version %d, RemoteDesktop API version %d, device types 0x%x",
        m->shell_version[0] != '\0' ? m->shell_version : "(unknown)",
        m->sc_version, m->rd_version, m->device_types);
    if (m->sc_version < SC_VERSION_CURSOR_MODE)
    {
        LOG(LOG_LEVEL_ERROR, "wlup/mutter: wlup needs ScreenCast API "
            "version %d or later", SC_VERSION_CURSOR_MODE);
        return 1;
    }
    if (m->device_types != 0 &&
            (m->device_types & (DEVICE_TYPE_KEYBOARD | DEVICE_TYPE_POINTER)) !=
            (DEVICE_TYPE_KEYBOARD | DEVICE_TYPE_POINTER))
    {
        LOG(LOG_LEVEL_ERROR, "wlup/mutter: Mutter offers no remote keyboard "
            "and pointer (device types 0x%x)", m->device_types);
        return 1;
    }
    return 0;
}

/******************************************************************************/
/* Finds which RemoteDesktop session methods this Mutter has. return error
 * if one wlup needs is missing */
static int
probe_session(struct wlup_mutter *m)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    const char *xml;

    if (sd_bus_call_method(m->bus, RD_NAME, m->rd_session_path,
                           "org.freedesktop.DBus.Introspectable", "Introspect",
                           &err, &reply, "") < 0 ||
            sd_bus_message_read(reply, "s", &xml) < 0)
    {
        /* Not knowing is no reason to fail: the calls will tell */
        LOG(LOG_LEVEL_WARNING, "wlup/mutter: cannot introspect the remote "
            "desktop session: %s", err.message != NULL ? err.message : "");
        sd_bus_error_free(&err);
        sd_bus_message_unref(reply);
        m->has_eis = 1;
        m->has_clipboard = 1;
        return 0;
    }
    m->has_eis = strstr(xml, "<method name=\"ConnectToEIS\">") != NULL;
    m->has_clipboard = strstr(xml, "<method name=\"EnableClipboard\">") != NULL;
    sd_bus_message_unref(reply);
    if (!m->has_eis)
    {
        LOG(LOG_LEVEL_ERROR, "wlup/mutter: the remote desktop session has no "
            "ConnectToEIS: input needs libei (GNOME 46 or later)");
        return 1;
    }
    return 0;
}

/******************************************************************************/
/* Asks Mutter for a virtual monitor for o; its PipeWire node is announced
 * by PipeWireStreamAdded (on_stream_added()). The cursor comes as
 * metadata, not painted, and the monitor is a platform one (not a shared
 * screen) where the API version has it. return error */
static int
record_virtual(struct wlup *v, struct mutter_output *o)
{
    struct wlup_mutter *m = (struct wlup_mutter *)v->backend_data;
    sd_bus_message *msg = NULL;
    int r;

    o->v = v;
    sd_bus_message_new_method_call(m->bus, &msg, SC_NAME, m->sc_session_path,
                                   SC_SESSION_IFACE, "RecordVirtual");
    if (m->sc_version >= SC_VERSION_IS_PLATFORM)
    {
        sd_bus_message_append(msg, "a{sv}", 2,
                              "cursor-mode", "u", (uint32_t)CURSOR_MODE_METADATA,
                              "is-platform", "b", 1);
    }
    else
    {
        sd_bus_message_append(msg, "a{sv}", 1,
                              "cursor-mode", "u", (uint32_t)CURSOR_MODE_METADATA);
    }
    r = call_for_path(m, SC_NAME, m->sc_session_path, SC_SESSION_IFACE,
                      "RecordVirtual", &o->stream_path, msg);
    sd_bus_message_unref(msg);
    if (r != 0)
    {
        return 1;
    }
    if (m->started)
    {
        /* The session's Start started the streams made before it; one
         * made later waits for its own (as gnome-remote-desktop does) */
        sd_bus_error err = SD_BUS_ERROR_NULL;

        if (sd_bus_call_method(m->bus, SC_NAME, o->stream_path,
                               SC_STREAM_IFACE, "Start", &err, NULL, "") < 0)
        {
            LOG(LOG_LEVEL_ERROR, "wlup/mutter: cannot start the virtual "
                "monitor %s: %s", o->stream_path,
                err.message != NULL ? err.message : "?");
            sd_bus_error_free(&err);
            return 1;
        }
    }
    return 0;
}

/******************************************************************************/
/* Removes o's virtual monitor from GNOME, and forgets it */
static void
stop_output(struct wlup_mutter *m, struct mutter_output *o)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;

    wlup_pwc_destroy(o->pwc);
    if (o->stream_path != NULL && !m->closed &&
            sd_bus_call_method(m->bus, SC_NAME, o->stream_path,
                               SC_STREAM_IFACE, "Stop", &err, NULL, "") < 0)
    {
        LOG(LOG_LEVEL_WARNING, "wlup/mutter: cannot stop the virtual "
            "monitor %s: %s", o->stream_path,
            err.message != NULL ? err.message : "?");
        sd_bus_error_free(&err);
    }
    g_free(o->stream_path);
    g_free(o->mapping_id);
    g_memset(o, 0, sizeof(*o));
}

/******************************************************************************/
/* return error */
static int
setup_sessions(struct wlup *v)
{
    struct wlup_mutter *m = (struct wlup_mutter *)v->backend_data;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *msg = NULL;
    sd_bus_message *reply = NULL;
    char *session_id = NULL;
    uint64_t start;
    int fd;
    int i;
    int r;

    /* 1. remote desktop session */
    sd_bus_message_new_method_call(m->bus, &msg, RD_NAME, RD_PATH, RD_NAME,
                                   "CreateSession");
    r = call_for_path(m, RD_NAME, RD_PATH, RD_NAME, "CreateSession",
                      &m->rd_session_path, msg);
    msg = sd_bus_message_unref(msg);
    if (r != 0)
    {
        return 1;
    }
    if (probe_session(m) != 0)
    {
        return 1;
    }
    r = sd_bus_get_property_string(m->bus, RD_NAME, m->rd_session_path,
                                   RD_SESSION_IFACE, "SessionId", &err,
                                   &session_id);
    if (r < 0)
    {
        LOG(LOG_LEVEL_ERROR, "wlup/mutter: no SessionId: %s", err.message);
        sd_bus_error_free(&err);
        return 1;
    }

    /* 2. screen cast session tied to it */
    sd_bus_message_new_method_call(m->bus, &msg, SC_NAME, SC_PATH, SC_NAME,
                                   "CreateSession");
    sd_bus_message_append(msg, "a{sv}", 1,
                          "remote-desktop-session-id", "s", session_id);
    free(session_id);
    r = call_for_path(m, SC_NAME, SC_PATH, SC_NAME, "CreateSession",
                      &m->sc_session_path, msg);
    msg = sd_bus_message_unref(msg);
    if (r != 0)
    {
        return 1;
    }

    /* 3. a virtual monitor per RDP monitor */
    sd_bus_match_signal(m->bus, &m->stream_added_slot, SC_NAME, NULL,
                        SC_STREAM_IFACE, "PipeWireStreamAdded",
                        on_stream_added, v);
    for (i = 0; i < m->num_out; ++i)
    {
        if (record_virtual(v, &m->out[i]) != 0)
        {
            return 1;
        }
    }
    sd_bus_match_signal(m->bus, &m->closed_slot, RD_NAME,
                        m->rd_session_path, RD_SESSION_IFACE, "Closed",
                        on_closed, v);

    /* 4. start, then wait for the PipeWire nodes */
    r = sd_bus_call_method(m->bus, RD_NAME, m->rd_session_path,
                           RD_SESSION_IFACE, "Start", &err, NULL, "");
    if (r < 0)
    {
        LOG(LOG_LEVEL_ERROR, "wlup/mutter: Start failed: %s", err.message);
        sd_bus_error_free(&err);
        return 1;
    }
    m->started = 1;
    start = monotonic_usec();
    for (i = 0; i < m->num_out; ++i)
    {
        while (!m->out[i].have_node)
        {
            if (sd_bus_process(m->bus, NULL) > 0)
            {
                continue;
            }
            if (monotonic_usec() - start > STREAM_TIMEOUT_USEC)
            {
                LOG(LOG_LEVEL_ERROR, "wlup/mutter: no PipeWireStreamAdded");
                return 1;
            }
            sd_bus_wait(m->bus, 100 * 1000);
        }
        read_mapping_id(m, &m->out[i]);
    }

    /* 5. input. A remote desktop without it is not one: fail rather
     * than show the desktop read-only */
    r = sd_bus_call_method(m->bus, RD_NAME, m->rd_session_path,
                           RD_SESSION_IFACE, "ConnectToEIS", &err, &reply,
                           "a{sv}", 0);
    if (r < 0 || sd_bus_message_read(reply, "h", &fd) < 0)
    {
        LOG(LOG_LEVEL_ERROR, "wlup/mutter: ConnectToEIS failed: %s",
            err.message != NULL ? err.message : "bad reply");
        sd_bus_error_free(&err);
        sd_bus_message_unref(reply);
        return 1;
    }
    else
    {
        /* The reply owns the fd */
        fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
        m->ei = ei_new_sender(v);
        ei_configure_name(m->ei, "xrdp");
        if (ei_setup_backend_fd(m->ei, fd) != 0)
        {
            LOG(LOG_LEVEL_ERROR, "wlup/mutter: libei setup failed");
            m->ei = ei_unref(m->ei);
            sd_bus_message_unref(reply);
            return 1;
        }
    }
    sd_bus_message_unref(reply);
    return 0;
}

/******************************************************************************/
/* Clipboard                                                                  */
/******************************************************************************/
static int
on_selection_owner_changed(sd_bus_message *msg, void *data, sd_bus_error *err)
{
    struct wlup *v = (struct wlup *)data;
    struct wlup_mutter *m = (struct wlup_mutter *)v->backend_data;
    const char *key;
    const char *mime;
    const char *contents;
    const char *best = NULL;
    int best_index = -1;
    int session_is_owner = 0;
    int i;

    if (sd_bus_message_enter_container(msg, 'a', "{sv}") <= 0)
    {
        return 0;
    }
    while (sd_bus_message_enter_container(msg, 'e', "sv") > 0)
    {
        if (sd_bus_message_read(msg, "s", &key) < 0)
        {
            break;
        }
        if (strcmp(key, "mime-types") == 0 &&
                sd_bus_message_peek_type(msg, NULL, &contents) > 0 &&
                (strcmp(contents, "as") == 0 || strcmp(contents, "(as)") == 0))
        {
            /* Documented as "as"; GNOME Shell 50 sends "(as)" */
            int in_struct = (contents[0] == '(');
            sd_bus_message_enter_container(msg, 'v', contents);
            if (in_struct)
            {
                sd_bus_message_enter_container(msg, 'r', "as");
            }
            sd_bus_message_enter_container(msg, 'a', "s");
            while (sd_bus_message_read(msg, "s", &mime) > 0)
            {
                for (i = 0; g_text_mime_types[i] != NULL; ++i)
                {
                    if (strcmp(mime, g_text_mime_types[i]) == 0 &&
                            (best_index < 0 || i < best_index))
                    {
                        best_index = i;
                        best = g_text_mime_types[i];
                    }
                }
            }
            sd_bus_message_exit_container(msg);
            if (in_struct)
            {
                sd_bus_message_exit_container(msg);
            }
            sd_bus_message_exit_container(msg);
        }
        else if (strcmp(key, "session-is-owner") == 0)
        {
            sd_bus_message_read(msg, "v", "b", &session_is_owner);
        }
        else
        {
            sd_bus_message_skip(msg, "v");
        }
        sd_bus_message_exit_container(msg);
    }

    LOG(LOG_LEVEL_DEBUG, "wlup/mutter: selection owner changed, text %s, "
        "session is owner %d", best != NULL ? best : "none", session_is_owner);
    /* When we are the owner, the selection holds the client's text */
    m->read_mime = session_is_owner ? NULL : best;
    return 0;
}

/******************************************************************************/
static int
on_selection_transfer(sd_bus_message *msg, void *data, sd_bus_error *err)
{
    struct wlup *v = (struct wlup *)data;
    struct wlup_mutter *m = (struct wlup_mutter *)v->backend_data;
    const char *mime;
    uint32_t serial;

    if (sd_bus_message_read(msg, "su", &mime, &serial) >= 0 &&
            m->num_transfers < MAX_TRANSFERS)
    {
        m->transfers[m->num_transfers++] = serial;
    }
    return 0;
}

/******************************************************************************/
/* Calls a method returning an fd; returns our own copy or -1 */
static int
call_for_fd(struct wlup_mutter *m, const char *method, const char *types,
            ...)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *msg = NULL;
    sd_bus_message *reply = NULL;
    int fd = -1;
    va_list ap;

    sd_bus_message_new_method_call(m->bus, &msg, RD_NAME, m->rd_session_path,
                                   RD_SESSION_IFACE, method);
    va_start(ap, types);
    sd_bus_message_appendv(msg, types, ap);
    va_end(ap);
    if (sd_bus_call(m->bus, msg, 0, &err, &reply) >= 0 &&
            sd_bus_message_read(reply, "h", &fd) >= 0)
    {
        /* The reply owns the fd */
        fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    }
    else
    {
        LOG(LOG_LEVEL_WARNING, "wlup/mutter: %s failed: %s", method,
            err.message != NULL ? err.message : "bad reply");
        fd = -1;
    }
    sd_bus_error_free(&err);
    sd_bus_message_unref(reply);
    sd_bus_message_unref(msg);
    return fd;
}

/******************************************************************************/
/* Work queued by the clipboard signals, done outside sd-bus callbacks */
static void
process_clipboard(struct wlup *v)
{
    struct wlup_mutter *m = (struct wlup_mutter *)v->backend_data;
    int fd;
    int i;
    int done;
    int n;
    int ok;
    char *text;
    int len;

    if (m->read_mime != NULL)
    {
        fd = call_for_fd(m, "SelectionRead", "s", m->read_mime);
        m->read_mime = NULL;
        if (fd >= 0)
        {
            text = wlup_clip_read_fd(fd, &len);
            wlup_clip_session_text(v, text != NULL ? text : "", len);
            g_free(text);
        }
    }

    for (i = 0; i < m->num_transfers; ++i)
    {
        ok = 0;
        fd = call_for_fd(m, "SelectionWrite", "u", m->transfers[i]);
        if (fd >= 0)
        {
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
            for (done = 0; m->client_text != NULL &&
                    done < m->client_text_len; done += n)
            {
                n = write(fd, m->client_text + done, m->client_text_len - done);
                if (n <= 0 && errno != EINTR)
                {
                    break;
                }
                n = MAX(n, 0);
            }
            ok = (m->client_text != NULL && done == m->client_text_len);
            close(fd);
        }
        sd_bus_call_method(m->bus, RD_NAME, m->rd_session_path,
                           RD_SESSION_IFACE, "SelectionWriteDone", NULL, NULL,
                           "ub", m->transfers[i], ok);
    }
    m->num_transfers = 0;
}

/******************************************************************************/
void
wlup_mutter_set_clipboard_text(struct wlup *v, const char *text, int len)
{
    struct wlup_mutter *m = (struct wlup_mutter *)v->backend_data;
    sd_bus_error err = SD_BUS_ERROR_NULL;

    if (m == NULL || !m->clipboard_enabled)
    {
        return;
    }
    g_free(m->client_text);
    m->client_text = (char *)g_malloc(len + 1, 0);
    memcpy(m->client_text, text, len);
    m->client_text_len = len;

    /* Mutter asks for the data with SelectionTransfer when it is pasted */
    if (sd_bus_call_method(m->bus, RD_NAME, m->rd_session_path,
                           RD_SESSION_IFACE, "SetSelection", &err, NULL,
                           "a{sv}", 1, "mime-types", "as", 2,
                           g_text_mime_types[0], g_text_mime_types[1]) < 0)
    {
        LOG(LOG_LEVEL_WARNING, "wlup/mutter: SetSelection failed: %s",
            err.message);
    }
    sd_bus_error_free(&err);
}

/******************************************************************************/
static void
enable_clipboard(struct wlup *v)
{
    struct wlup_mutter *m = (struct wlup_mutter *)v->backend_data;
    sd_bus_error err = SD_BUS_ERROR_NULL;

    if (!m->has_clipboard)
    {
        LOG(LOG_LEVEL_WARNING, "wlup/mutter: the remote desktop session has "
            "no EnableClipboard, no clipboard");
        return;
    }
    sd_bus_match_signal(m->bus, &m->owner_changed_slot, RD_NAME,
                        m->rd_session_path, RD_SESSION_IFACE,
                        "SelectionOwnerChanged", on_selection_owner_changed, v);
    sd_bus_match_signal(m->bus, &m->transfer_slot, RD_NAME,
                        m->rd_session_path, RD_SESSION_IFACE,
                        "SelectionTransfer", on_selection_transfer, v);
    if (sd_bus_call_method(m->bus, RD_NAME, m->rd_session_path,
                           RD_SESSION_IFACE, "EnableClipboard", &err, NULL,
                           "a{sv}", 0) < 0)
    {
        LOG(LOG_LEVEL_WARNING, "wlup/mutter: EnableClipboard failed: %s",
            err.message);
    }
    else
    {
        m->clipboard_enabled = 1;
        LOG(LOG_LEVEL_INFO, "wlup/mutter: clipboard enabled");
    }
    sd_bus_error_free(&err);
}

/******************************************************************************/
/* Module side                                                                */
/******************************************************************************/
/* Waits until GNOME Shell offers one of its D-Bus APIs (name).
 * return error */
static int
wait_for_mutter(struct wlup_mutter *m, const char *name)
{
    uint64_t start = monotonic_usec();
    int logged = 0;
    int has_owner = 0;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;

    for (;;)
    {
        if (sd_bus_call_method(m->bus, "org.freedesktop.DBus",
                               "/org/freedesktop/DBus", "org.freedesktop.DBus",
                               "NameHasOwner", &err, &reply, "s",
                               name) >= 0 &&
                sd_bus_message_read(reply, "b", &has_owner) >= 0 &&
                has_owner)
        {
            sd_bus_message_unref(reply);
            return 0;
        }
        reply = sd_bus_message_unref(reply);
        sd_bus_error_free(&err);
        if (monotonic_usec() - start > MUTTER_TIMEOUT_USEC)
        {
            LOG(LOG_LEVEL_ERROR, "wlup/mutter: %s did not appear on the "
                "session bus", name);
            return 1;
        }
        if (!logged)
        {
            LOG(LOG_LEVEL_INFO, "wlup/mutter: waiting for GNOME Shell");
            logged = 1;
        }
        usleep(200 * 1000);
    }
}

/******************************************************************************/
/* The outputs for the client's monitors at connection time: one per
 * monitor of a multi-monitor client, else one of the session's size */
static void
initial_layout(struct wlup *v)
{
    struct wlup_mutter *m = (struct wlup_mutter *)v->backend_data;
    const struct display_size_description *ds =
        v->client_info != NULL ? &v->client_info->display_sizes : NULL;
    struct monitor_info mons[WLUP_MAX_MONITORS];
    int i;

    m->num_out = 1;
    m->out[0].v = v;
    m->out[0].width = v->server_width;
    m->out[0].height = v->server_height;
    m->box_width = v->server_width;
    m->box_height = v->server_height;
    if (ds == NULL || ds->monitorCount < 2)
    {
        return;
    }
    m->num_out = set_layout(m, v->server_width, v->server_height,
                            ds->monitorCount, ds->minfo_wm, mons);
    for (i = 0; i < m->num_out; ++i)
    {
        m->out[i].v = v;
        m->out[i].x = mons[i].left;
        m->out[i].y = mons[i].top;
        m->out[i].width = mons[i].right - mons[i].left + 1;
        m->out[i].height = mons[i].bottom - mons[i].top + 1;
        m->out[i].primary = mons[i].is_primary;
        LOG(LOG_LEVEL_INFO, "wlup/mutter: virtual monitor %d: %dx%d+%d+%d%s",
            i, m->out[i].width, m->out[i].height, m->out[i].x, m->out[i].y,
            m->out[i].primary ? ", primary" : "");
    }
}

/******************************************************************************/
int
wlup_mutter_connect(struct wlup *v, int bus_fd)
{
    struct wlup_mutter *m;
    char text[512];
    int i;
    int r;

    m = (struct wlup_mutter *)g_malloc(sizeof(struct wlup_mutter), 1);
    v->backend_data = m;

    if (bus_fd >= 0)
    {
        /* sesman connected to the session bus as the user. sd-bus
         * authenticates with EXTERNAL without claiming a uid, so the bus
         * takes the identity of the process that connected. sd-bus gets
         * a copy: xrdp closes bus_fd itself if we fail */
        int fd = fcntl(bus_fd, F_DUPFD_CLOEXEC, 3);
        r = sd_bus_new(&m->bus);
        if (r >= 0)
        {
            r = sd_bus_set_fd(m->bus, fd, fd);
        }
        if (r < 0)
        {
            close(fd);
        }
        if (r >= 0)
        {
            sd_bus_set_bus_client(m->bus, 1);
            r = sd_bus_start(m->bus);
        }
    }
    else if (v->dbus_address[0] != '\0')
    {
        sd_bus_new(&m->bus);
        sd_bus_set_address(m->bus, v->dbus_address);
        sd_bus_set_bus_client(m->bus, 1);
        r = sd_bus_start(m->bus);
    }
    else
    {
        r = sd_bus_open_user(&m->bus);
    }
    if (r < 0)
    {
        g_snprintf(text, sizeof(text), "wlup error - cannot connect to the "
                   "session bus: %s", strerror(-r));
        v->server_msg(v, text, 0);
        return 1;
    }

    if (wait_for_mutter(m, RD_NAME) != 0 || wait_for_mutter(m, SC_NAME) != 0)
    {
        v->server_msg(v, "wlup error - GNOME Shell does not offer its "
                      "remote desktop API", 0);
        return 1;
    }
    if (probe_api(m) != 0)
    {
        v->server_msg(v, "wlup error - this GNOME's remote desktop API is "
                      "not one wlup can use (see xrdp.log)", 0);
        return 1;
    }
    initial_layout(v);
    if (setup_sessions(v) != 0)
    {
        v->server_msg(v, "wlup error - cannot start a Mutter remote desktop "
                      "session", 0);
        return 1;
    }
    for (i = 0; i < m->num_out; ++i)
    {
        struct mutter_output *o = &m->out[i];

        o->pwc = i == 0 ?
                 wlup_pwc_connect(v, "mutter", o->node_id, o->width, o->height) :
                 wlup_pwc_connect_more(m->out[0].pwc, o->node_id, o->width,
                                       o->height);
        if (o->pwc == NULL)
        {
            v->server_msg(v, "wlup error - cannot receive the PipeWire "
                          "stream", 0);
            return 1;
        }
    }
    if (m->num_out > 1 &&
            wlup_pwc_alloc_pixels(v, m->box_width, m->box_height) != 0)
    {
        v->server_msg(v, "wlup error - out of memory", 0);
        return 1;
    }
    place_outputs(v);
    enable_clipboard(v);
    LOG(LOG_LEVEL_INFO, "wlup/mutter: capabilities: virtual monitor yes, "
        "platform monitor %s, EIS input %s, clipboard %s, resize yes",
        m->sc_version >= SC_VERSION_IS_PLATFORM ? "yes" : "no",
        m->ei != NULL ? "yes" : "no", m->clipboard_enabled ? "yes" : "no");

    /* Success: the connection from sesman is ours to close */
    if (bus_fd >= 0)
    {
        close(bus_fd);
    }
    scancode_set_keycode_set("evdev");
    g_snprintf(text, sizeof(text), "wlup: connected to Mutter, PipeWire "
               "node %u%s%s", m->out[0].node_id,
               m->num_out > 1 ? " and more, a virtual monitor per monitor" : "",
               m->ei != NULL ? ", EIS input" : "");
    v->server_msg(v, text, 0);
    LOG(LOG_LEVEL_INFO, "%s", text);
    return 0;
}

/******************************************************************************/
void
wlup_mutter_disconnect(struct wlup *v)
{
    struct wlup_mutter *m = (struct wlup_mutter *)v->backend_data;
    int i;

    if (m == NULL)
    {
        return;
    }
    /* out[0]'s stream owns the PipeWire connection: last */
    for (i = m->num_out - 1; i >= 0; --i)
    {
        wlup_pwc_destroy(m->out[i].pwc);
        m->out[i].pwc = NULL;
        g_free(m->out[i].stream_path);
        g_free(m->out[i].mapping_id);
    }
    if (m->keyboard != NULL)
    {
        ei_device_unref(m->keyboard);
    }
    if (m->pointer != NULL)
    {
        ei_device_unref(m->pointer);
    }
    if (m->ei != NULL)
    {
        ei_unref(m->ei);
    }
    if (m->bus != NULL)
    {
        if (m->rd_session_path != NULL && !m->closed)
        {
            sd_bus_call_method(m->bus, RD_NAME, m->rd_session_path,
                               RD_SESSION_IFACE, "Stop", NULL, NULL, "");
        }
        sd_bus_slot_unref(m->stream_added_slot);
        sd_bus_slot_unref(m->closed_slot);
        sd_bus_slot_unref(m->owner_changed_slot);
        sd_bus_slot_unref(m->transfer_slot);
        sd_bus_flush_close_unref(m->bus);
    }
    g_free(m->client_text);
    g_free(m->rd_session_path);
    g_free(m->sc_session_path);
    g_free(m);
    v->backend_data = NULL;
}

/******************************************************************************/
void
wlup_mutter_get_wait_objs(struct wlup *v, tbus *read_objs, int *rcount,
                          tbus *write_objs, int *wcount, int *timeout)
{
    struct wlup_mutter *m = (struct wlup_mutter *)v->backend_data;

    if (m == NULL)
    {
        return;
    }
    if (m->bus != NULL)
    {
        read_objs[(*rcount)++] = sd_bus_get_fd(m->bus);
        if (sd_bus_get_events(m->bus) & POLLOUT)
        {
            write_objs[(*wcount)++] = sd_bus_get_fd(m->bus);
        }
    }
    if (m->out[0].pwc != NULL)
    {
        /* every stream's frames come through this loop */
        read_objs[(*rcount)++] = wlup_pwc_get_fd(m->out[0].pwc);
    }
    if (m->ei != NULL)
    {
        read_objs[(*rcount)++] = ei_get_fd(m->ei);
    }
    if (v->resize_pending && (*timeout < 0 || *timeout > 200))
    {
        /* Wake up to check the resize timeout */
        *timeout = 200;
    }
}

/******************************************************************************/
/* A virtual monitor added by a resize: once Mutter has announced its
 * PipeWire node, its frames are received too */
static void
connect_new_outputs(struct wlup *v)
{
    struct wlup_mutter *m = (struct wlup_mutter *)v->backend_data;
    int i;

    for (i = 1; i < m->num_out; ++i)
    {
        struct mutter_output *o = &m->out[i];

        if (o->pwc != NULL || !o->have_node || m->out[0].pwc == NULL)
        {
            continue;
        }
        read_mapping_id(m, o);
        o->pwc = wlup_pwc_connect_more(m->out[0].pwc, o->node_id, o->width,
                                       o->height);
        if (o->pwc == NULL)
        {
            continue;
        }
        wlup_pwc_place(o->pwc, o->x, o->y, on_output_sized, o);
        LOG(LOG_LEVEL_INFO, "wlup/mutter: virtual monitor %d added, "
            "mapping id %s", i, o->mapping_id != NULL ? o->mapping_id : "none");
    }
}

/******************************************************************************/
int
wlup_mutter_check_wait_objs(struct wlup *v)
{
    struct wlup_mutter *m = (struct wlup_mutter *)v->backend_data;
    int i;

    if (m == NULL)
    {
        return 0;
    }
    if (m->bus != NULL)
    {
        while (sd_bus_process(m->bus, NULL) > 0)
        {
        }
        process_clipboard(v);
    }
    connect_new_outputs(v);
    if (m->num_out > 1 && v->need_full_paint)
    {
        /* One flag for the core; each stream paints its own part */
        for (i = 0; i < m->num_out; ++i)
        {
            if (m->out[i].pwc != NULL)
            {
                wlup_pwc_repaint(m->out[i].pwc);
            }
        }
        v->need_full_paint = 0;
    }
    if (m->out[0].pwc != NULL)
    {
        wlup_pwc_process(m->out[0].pwc);
    }
    if (m->ei != NULL)
    {
        process_ei_events(v);
    }
    if (v->resize_pending &&
            monotonic_usec() - m->resize_start > RESIZE_TIMEOUT_USEC)
    {
        LOG(LOG_LEVEL_WARNING, "wlup/mutter: the virtual monitor was not "
            "resized, the picture will be clipped or padded");
        wlup_resize_finished(v);
    }
    return m->closed;
}

/******************************************************************************/
/* Paints the damage held back while frames were not wanted */
static void
mutter_frames_wanted(struct wlup *v)
{
    struct wlup_mutter *m = (struct wlup_mutter *)v->backend_data;

    int i;

    for (i = 0; m != NULL && i < m->num_out; ++i)
    {
        if (m->out[i].pwc != NULL)
        {
            wlup_pwc_frames_wanted(m->out[i].pwc);
        }
    }
}

/******************************************************************************/
/* Frames arrive from PipeWire unasked: frames_wanted() paints what was
 * held back meanwhile. The desktop image is wlup's own: no pixels_busy() */
static const struct wlup_backend_ops mutter_ops =
{
    .name = "mutter",
    .connect = wlup_mutter_connect,
    .disconnect = wlup_mutter_disconnect,
    .get_wait_objs = wlup_mutter_get_wait_objs,
    .check_wait_objs = wlup_mutter_check_wait_objs,
    .key = wlup_mutter_key,
    .mouse = wlup_mutter_mouse,
    .resize = wlup_mutter_resize,
    .resize_monitors = wlup_mutter_resize_monitors,
    .frames_wanted = mutter_frames_wanted,
    .set_clipboard_text = wlup_mutter_set_clipboard_text
};

/******************************************************************************/
const struct wlup_backend_ops *
wlup_backend_mutter(void)
{
    return &mutter_ops;
}
