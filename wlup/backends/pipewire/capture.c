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
 * wlup: frames from a PipeWire screen cast stream (see capture.h)
 *
 * Buffers are shared memory (no dma-buf is offered), with the damage
 * and the cursor as metadata. The PipeWire loop's fd goes into xrdp's
 * poll loop, and pw_loop_iterate(loop, 0) runs when it is readable.
 */

#if defined(HAVE_CONFIG_H)
#include <config_ac.h>
#endif

#include <errno.h>
#include <string.h>
#include <sys/mman.h>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/buffers.h>
#include <spa/buffer/meta.h>

#include "wlup.h"
#include "capture.h"
#include "log.h"
#include "os_calls.h"
#include "xrdp_constants.h"
#include "ms-rdpbcgr.h"

/* Cursor metadata of a buffer: position, hotspot and, when the shape
 * changes, a bitmap. Mutter offers bitmaps of up to 384x384 */
#define CURSOR_META_SIZE(width, height) \
    (sizeof(struct spa_meta_cursor) + sizeof(struct spa_meta_bitmap) + \
     (width) * (height) * 4)
#define CURSOR_META_MAX 384

/* Largest pointer xrdp can send (struct xrdp_pointer_item), and the
 * largest every RDP client takes */
#define POINTER_MAX 96
#define POINTER_MAX_SMALL 32

/* Damage rectangles kept per PipeWire buffer */
#define MAX_DAMAGE 16

struct wlup_pwc
{
    struct wlup *v;
    const char *who; /* the backend, for the log */
    struct pw_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct spa_hook core_listener;
    struct pw_stream *stream;
    struct spa_hook stream_listener;
    int width;
    int height;
    int info_logged; /* the daemon's version */
    /* Placed by the backend (wlup_pwc_place()): frames go to the desktop
     * image at x, y, and the backend sizes the image */
    int x;
    int y;
    void (*sized)(void *data);
    void *sized_data;
    int full;   /* placed: copy and paint the next frame whole */
    int shared; /* loop, context and core belong to another stream */
    /* Damage copied while the core wanted no frames (wlup_wants_frames()),
     * painted once it does again: x, y, cx, cy per rect, or one
     * bounding box once they don't fit */
    short held[WLUP_MAX_DAMAGE * 4];
    int num_held;
    int held_box;
};

static int pw_initialised;

/******************************************************************************/
/* Copies one rectangle of a PipeWire buffer into our desktop copy */
/* Copies x, y, cx, cy of a buffer (stream coordinates) to the desktop
 * image, at the stream's place in it */
static void
copy_region(struct wlup_pwc *c, const uint8_t *src, int src_stride,
            int x, int y, int cx, int cy)
{
    struct wlup *v = c->v;
    int row;
    int dst_stride = v->buffer_width * 4;

    x = MAX(x, 0);
    y = MAX(y, 0);
    cx = MIN(cx, MIN(c->width, v->buffer_width - c->x) - x);
    cy = MIN(cy, MIN(c->height, v->buffer_height - c->y) - y);
    for (row = y; row < y + cy; ++row)
    {
        memcpy(v->pixels + (size_t)(c->y + row) * dst_stride + (c->x + x) * 4,
               src + (size_t)row * src_stride + x * 4, (size_t)cx * 4);
    }
}

/******************************************************************************/
#define DETECT_TILE 64

/* Adds a rect to a damage list; returns 1 if the list is full */
static int
add_rect(short *rects, int *num_rects, int x, int y, int cx, int cy)
{
    if (*num_rects >= WLUP_MAX_DAMAGE)
    {
        return 1;
    }
    rects[*num_rects * 4] = x;
    rects[*num_rects * 4 + 1] = y;
    rects[*num_rects * 4 + 2] = cx;
    rects[*num_rects * 4 + 3] = cy;
    ++*num_rects;
    return 0;
}

/* Finds what changed by comparing a PipeWire buffer with our desktop copy
 * in tiles, and copies the tiles that differ. Mutter's damage metadata
 * can't be trusted: now and then a buffer carries a change outside its
 * damage, and nothing later repaints it. Each run of changed tiles in a
 * tile row adds one rect. Returns 1 when the rects did not fit (all tiles
 * are still copied) */
static int
detect_damage(struct wlup_pwc *c, const uint8_t *src, int src_stride,
              int width, int height, short *rects, int *num_rects)
{
    struct wlup *v = c->v;
    const char *dst = v->pixels + (size_t)c->y * v->buffer_width * 4 +
                      c->x * 4;
    int dst_stride = v->buffer_width * 4;
    int overflow = 0;
    int tx;
    int ty;
    int row;

    width = MIN(width, v->buffer_width - c->x);
    height = MIN(height, v->buffer_height - c->y);
    for (ty = 0; ty < height; ty += DETECT_TILE)
    {
        int th = MIN(DETECT_TILE, height - ty);
        int run_x = -1;

        for (tx = 0; tx < width; tx += DETECT_TILE)
        {
            int tw = MIN(DETECT_TILE, width - tx);
            int dirty = 0;

            for (row = ty; row < ty + th && !dirty; ++row)
            {
                dirty = memcmp(dst + (size_t)row * dst_stride + tx * 4,
                               src + (size_t)row * src_stride + tx * 4,
                               (size_t)tw * 4) != 0;
            }
            if (dirty)
            {
                copy_region(c, src, src_stride, tx, ty, tw, th);
                if (run_x < 0)
                {
                    run_x = tx;
                }
            }
            else if (run_x >= 0)
            {
                overflow |= add_rect(rects, num_rects, run_x, ty,
                                     tx - run_x, th);
                run_x = -1;
            }
        }
        if (run_x >= 0)
        {
            /* The run reaches the right edge */
            overflow |= add_rect(rects, num_rects, run_x, ty,
                                 width - run_x, th);
        }
    }
    return overflow;
}

/******************************************************************************/
/* Sends the pointer shape in a buffer's cursor metadata to the client,
 * which then draws the pointer itself, so it follows the mouse at once
 * rather than a round trip later in the frames */
static void
update_pointer(struct wlup_pwc *c, struct spa_buffer *buf)
{
    struct wlup *v = c->v;
    struct spa_meta *meta;
    struct spa_meta_cursor *mc;
    struct spa_meta_bitmap *mb;
    const uint8_t *src;
    char data[POINTER_MAX * POINTER_MAX * 4];
    char mask[POINTER_MAX * POINTER_MAX / 8];
    int width;
    int height;
    int stride;
    int side;   /* longer side of the compositor's bitmap */
    int scaled; /* that side once it fits the client */
    int size;   /* the RDP pointer is size x size */
    int hot_x;
    int hot_y;
    int x;
    int y;

    meta = spa_buffer_find_meta(buf, SPA_META_Cursor);
    if (meta == NULL || meta->size < sizeof(*mc))
    {
        return;
    }
    mc = (struct spa_meta_cursor *)meta->data;
    if (mc->id == 0 || mc->bitmap_offset == 0)
    {
        /* No cursor, or only its position changed */
        return;
    }
    if ((size_t)mc->bitmap_offset + sizeof(*mb) > meta->size)
    {
        return;
    }
    mb = SPA_PTROFF(mc, mc->bitmap_offset, struct spa_meta_bitmap);
    if (mb->format == 0 || mb->offset == 0 ||
            mb->size.width == 0 || mb->size.height == 0)
    {
        /* Mutter sends an empty bitmap for a hidden pointer */
        v->server_set_pointer_system(v, SYSPTR_NULL);
        return;
    }
    width = mb->size.width;
    height = mb->size.height;
    stride = mb->stride != 0 ? mb->stride : width * 4;
    if (mb->format != SPA_VIDEO_FORMAT_RGBA ||
            width > CURSOR_META_MAX || height > CURSOR_META_MAX ||
            stride < width * 4 ||
            (size_t)mc->bitmap_offset + mb->offset +
            (size_t)stride * height > meta->size)
    {
        LOG(LOG_LEVEL_WARNING, "wlup/%s: unusable pointer bitmap "
            "(format %u, %dx%d)", c->who, mb->format, width, height);
        return;
    }
    src = SPA_PTROFF(mb, mb->offset, const uint8_t);

    /* Shrink a pointer larger than the client takes. The size stays a
     * multiple of 16, as the rows of the AND mask are 16-bit aligned */
    side = MAX(width, height);
    scaled = MIN(side, (v->large_pointer_flags & LARGE_POINTER_FLAG_96x96) ?
                 POINTER_MAX : POINTER_MAX_SMALL);
    size = (scaled + 15) & ~15;
    hot_x = MIN(MAX(mc->hotspot.x, 0) * scaled / side, size - 1);
    hot_y = MIN(MAX(mc->hotspot.y, 0) * scaled / side, size - 1);

    /* RDP pointers are bottom-up BGRA. The compositor's bitmap is
     * premultiplied RGBA, which RDP clients take as it is. The AND mask
     * stays empty, so the alpha channel alone decides what shows through */
    for (y = 0; y < size; ++y)
    {
        char *dst = data + (size_t)(size - 1 - y) * size * 4;
        int sy = y * side / scaled;

        for (x = 0; x < size; ++x, dst += 4)
        {
            int sx = x * side / scaled;

            if (sx < width && sy < height)
            {
                const uint8_t *p = src + (size_t)sy * stride + sx * 4;

                dst[0] = p[2];
                dst[1] = p[1];
                dst[2] = p[0];
                dst[3] = p[3];
            }
            else
            {
                memset(dst, 0, 4);
            }
        }
    }
    memset(mask, 0, sizeof(mask));
    LOG(LOG_LEVEL_DEBUG, "wlup/%s: pointer %dx%d sent as %dx%d, "
        "hotspot %d,%d", c->who, width, height, size, size, hot_x, hot_y);
    v->server_set_pointer_large(v, hot_x, hot_y, data, mask, 32, size, size);
}

/******************************************************************************/
static void
hold_damage(struct wlup_pwc *c, int x, int y, int cx, int cy)
{
    short *h = c->held;
    int i;

    for (i = 0; i < c->num_held; ++i)
    {
        if (x >= h[i * 4] && y >= h[i * 4 + 1] &&
                x + cx <= h[i * 4] + h[i * 4 + 2] &&
                y + cy <= h[i * 4 + 1] + h[i * 4 + 3])
        {
            /* Already held: what changes again and again (an animation,
             * a busy window) stays one rect */
            return;
        }
    }
    if (!c->held_box && c->num_held < WLUP_MAX_DAMAGE)
    {
        h[c->num_held * 4] = x;
        h[c->num_held * 4 + 1] = y;
        h[c->num_held * 4 + 2] = cx;
        h[c->num_held * 4 + 3] = cy;
        ++c->num_held;
        return;
    }
    /* Too many: a bounding box of them all, in h[0..3] */
    for (i = 1; i < c->num_held; ++i)
    {
        int x2 = MAX(h[0] + h[2], h[i * 4] + h[i * 4 + 2]);
        int y2 = MAX(h[1] + h[3], h[i * 4 + 1] + h[i * 4 + 3]);

        h[0] = MIN(h[0], h[i * 4]);
        h[1] = MIN(h[1], h[i * 4 + 1]);
        h[2] = x2 - h[0];
        h[3] = y2 - h[1];
    }
    {
        int x2 = MAX(h[0] + h[2], x + cx);
        int y2 = MAX(h[1] + h[3], y + cy);

        h[0] = MIN(h[0], x);
        h[1] = MIN(h[1], y);
        h[2] = x2 - h[0];
        h[3] = y2 - h[1];
    }
    c->num_held = 1;
    c->held_box = 1;
}

/******************************************************************************/
/* Sorts the held rects top to bottom, then left to right: painted in the
 * order they were held, a change that came last waits behind everything
 * held before it, however far down the screen that is */
static void
sort_held(struct wlup_pwc *c)
{
    short *h = c->held;
    int i;
    int j;

    for (i = 1; i < c->num_held; ++i)
    {
        short r[4];

        memcpy(r, &h[i * 4], sizeof(r));
        for (j = i; j > 0 && (h[(j - 1) * 4 + 1] > r[1] ||
                              (h[(j - 1) * 4 + 1] == r[1] &&
                               h[(j - 1) * 4] > r[0])); --j)
        {
            memcpy(&h[j * 4], &h[(j - 1) * 4], sizeof(r));
        }
        memcpy(&h[j * 4], r, sizeof(r));
    }
}

/******************************************************************************/
/* The full paint the stream was asked for is done (or held) */
static void
painted_full(struct wlup_pwc *c)
{
    if (c->sized != NULL)
    {
        c->full = 0;
    }
    else
    {
        c->v->need_full_paint = 0;
    }
}

/******************************************************************************/
static void
on_process(void *data)
{
    struct wlup_pwc *c = (struct wlup_pwc *)data;
    struct wlup *v = c->v;
    struct pw_buffer *pb;
    short rects[WLUP_MAX_DAMAGE * 4];
    int num_rects = 0;
    /* A placed stream is told by the backend (wlup_pwc_repaint()): the
     * core's need_full_paint is one flag for all the streams */
    int full = c->sized != NULL ? c->full : v->need_full_paint;
    int full_cx = MAX(0, MIN(c->width, v->buffer_width - c->x));
    int full_cy = MAX(0, MIN(c->height, v->buffer_height - c->y));
    int first;
    int i;

    /* Take every queued buffer, so no damage is lost when frames
     * arrive faster than xrdp polls */
    while ((pb = pw_stream_dequeue_buffer(c->stream)) != NULL)
    {
        struct spa_buffer *buf = pb->buffer;
        struct spa_data *d = &buf->datas[0];
        struct spa_meta_header *h;
        struct spa_meta *dm;
        const uint8_t *src;
        int stride;

        update_pointer(c, buf);
        h = (struct spa_meta_header *)
            spa_buffer_find_meta_data(buf, SPA_META_Header, sizeof(*h));
        if ((h != NULL && (h->flags & SPA_META_HEADER_FLAG_CORRUPTED)) ||
                d->data == NULL || d->chunk->size == 0 || v->pixels == NULL)
        {
            /* No picture in this buffer (e.g. only the cursor changed) */
            pw_stream_queue_buffer(c->stream, pb);
            continue;
        }
        src = (const uint8_t *)d->data + d->chunk->offset;
        stride = d->chunk->stride != 0 ? d->chunk->stride : c->width * 4;

        dm = spa_buffer_find_meta(buf, SPA_META_VideoDamage);
        if (dm == NULL || full)
        {
            copy_region(c, src, stride, 0, 0, c->width, c->height);
            full = 1;
        }
        else
        {
            struct spa_meta_region *r;
            int mx1 = c->width;
            int my1 = c->height;
            int mx2 = 0;
            int my2 = 0;

            /* The compositor's damage, only to log it */
            spa_meta_for_each(r, dm)
            {
                if (!spa_meta_region_is_valid(r))
                {
                    break;
                }
                mx1 = MIN(mx1, r->region.position.x);
                my1 = MIN(my1, r->region.position.y);
                mx2 = MAX(mx2, r->region.position.x +
                          (int)r->region.size.width);
                my2 = MAX(my2, r->region.position.y +
                          (int)r->region.size.height);
            }
            if (mx2 > mx1)
            {
                LOG(LOG_LEVEL_DEBUG, "wlup: %s damage in %d,%d-%d,%d",
                    c->who, mx1, my1, mx2, my2);
            }
            first = num_rects;
            if (detect_damage(c, src, stride, c->width, c->height,
                              rects, &num_rects))
            {
                full = 1;
            }
            for (i = first; i < num_rects; ++i)
            {
                if (rects[i * 4] + rects[i * 4 + 2] <= mx1 ||
                        rects[i * 4] >= mx2 ||
                        rects[i * 4 + 1] + rects[i * 4 + 3] <= my1 ||
                        rects[i * 4 + 1] >= my2)
                {
                    LOG(LOG_LEVEL_DEBUG, "wlup: changed %d,%d %dx%d is "
                        "outside %s's damage", rects[i * 4],
                        rects[i * 4 + 1], rects[i * 4 + 2], rects[i * 4 + 3],
                        c->who);
                }
            }
        }
        pw_stream_queue_buffer(c->stream, pb);
    }
    /* From here on, rects are in the desktop image */
    for (i = 0; i < num_rects; ++i)
    {
        rects[i * 4] += c->x;
        rects[i * 4 + 1] += c->y;
    }

    if (full)
    {
        LOG(LOG_LEVEL_DEBUG, "wlup: damage: all");
    }
    else if (num_rects > 0)
    {
        int x1 = rects[0];
        int y1 = rects[1];
        int x2 = rects[0] + rects[2];
        int y2 = rects[1] + rects[3];

        for (i = 1; i < num_rects; ++i)
        {
            x1 = MIN(x1, rects[i * 4]);
            y1 = MIN(y1, rects[i * 4 + 1]);
            x2 = MAX(x2, rects[i * 4] + rects[i * 4 + 2]);
            y2 = MAX(y2, rects[i * 4 + 1] + rects[i * 4 + 3]);
        }
        LOG(LOG_LEVEL_DEBUG, "wlup: detected damage: %d rects in "
            "%d,%d-%d,%d", num_rects, x1, y1, x2, y2);
    }
    if (full && (full_cx == 0 || full_cy == 0))
    {
        /* The image does not reach this stream (yet): nothing to paint */
        painted_full(c);
        return;
    }
    if (v->suppress_output || (!full && num_rects == 0))
    {
        return;
    }
    if (!wlup_wants_frames(v))
    {
        /* Much of what was painted still waits for the client. Frames
         * keep coming and are copied, as the damage is found against
         * our copy, but not painted: wlup_pwc_frames_wanted() paints
         * what changed once xrdp has sent most of it */
        if (full)
        {
            hold_damage(c, c->x, c->y, full_cx, full_cy);
            painted_full(c);
        }
        for (i = 0; i < num_rects && !full; ++i)
        {
            hold_damage(c, rects[i * 4], rects[i * 4 + 1],
                        rects[i * 4 + 2], rects[i * 4 + 3]);
        }
        return;
    }
    wlup_begin_update(v);
    if (full)
    {
        wlup_paint_region(v, c->x, c->y, full_cx, full_cy);
        painted_full(c);
    }
    else
    {
        for (i = 0; i < num_rects; ++i)
        {
            wlup_paint_region(v, rects[i * 4], rects[i * 4 + 1],
                              rects[i * 4 + 2], rects[i * 4 + 3]);
        }
    }
    wlup_end_update(v);
}

/******************************************************************************/
/* Our desktop copy, an anonymous mapping so wlup's free_buffer() can
 * release it like a Wayland shm buffer. return error */
int
wlup_pwc_alloc_pixels(struct wlup *v, int width, int height)
{
    size_t size = (size_t)width * height * 4;

    if (v->pixels != NULL)
    {
        munmap(v->pixels, v->pixels_size);
    }
    v->pixels = (char *)mmap(NULL, size, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (v->pixels == MAP_FAILED)
    {
        v->pixels = NULL;
        return 1;
    }
    v->pixels_size = size;
    v->buffer_width = width;
    v->buffer_height = height;
    v->need_full_paint = 1;
    return 0;
}

/******************************************************************************/
static void
on_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
    struct wlup_pwc *c = (struct wlup_pwc *)data;
    struct wlup *v = c->v;
    struct spa_video_info_raw info;
    uint8_t pod_buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(pod_buf, sizeof(pod_buf));
    const struct spa_pod *params[4];
    int stride;

    if (param == NULL || id != SPA_PARAM_Format)
    {
        return;
    }
    if (spa_format_video_raw_parse(param, &info) < 0)
    {
        LOG(LOG_LEVEL_ERROR, "wlup/%s: cannot parse the stream format",
            c->who);
        return;
    }
    c->width = info.size.width;
    c->height = info.size.height;
    stride = c->width * 4;
    LOG(LOG_LEVEL_INFO, "wlup/%s: stream format %s %dx%d", c->who,
        info.format == SPA_VIDEO_FORMAT_BGRx ? "BGRx" :
        info.format == SPA_VIDEO_FORMAT_BGRA ? "BGRA" : "other",
        c->width, c->height);
    if (c->sized != NULL)
    {
        /* The backend owns the image and finishes resizes */
        c->full = 1;
        c->sized(c->sized_data);
    }
    else if (wlup_pwc_alloc_pixels(v, c->width, c->height) != 0)
    {
        LOG(LOG_LEVEL_ERROR, "wlup/%s: out of memory", c->who);
        return;
    }
    else if (v->resize_pending && c->width == v->resize_width &&
             c->height == v->resize_height)
    {
        LOG(LOG_LEVEL_INFO, "wlup/%s: virtual monitor resized to %dx%d",
            c->who, c->width, c->height);
        wlup_resize_finished(v);
    }

    /* Shared memory only (no dma-buf), plus damage regions and cursor */
    params[0] = (const struct spa_pod *)spa_pod_builder_add_object(&b,
                SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
                SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 16),
                SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
                SPA_PARAM_BUFFERS_size, SPA_POD_Int(stride * c->height),
                SPA_PARAM_BUFFERS_stride, SPA_POD_Int(stride),
                SPA_PARAM_BUFFERS_dataType,
                SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) |
                                         (1 << SPA_DATA_MemPtr)));
    params[1] = (const struct spa_pod *)spa_pod_builder_add_object(&b,
                SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
                SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
                SPA_PARAM_META_size,
                SPA_POD_Int(sizeof(struct spa_meta_header)));
    params[2] = (const struct spa_pod *)spa_pod_builder_add_object(&b,
                SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
                SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoDamage),
                SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int(
                    sizeof(struct spa_meta_region) * MAX_DAMAGE,
                    sizeof(struct spa_meta_region),
                    sizeof(struct spa_meta_region) * MAX_DAMAGE));
    params[3] = (const struct spa_pod *)spa_pod_builder_add_object(&b,
                SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
                SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
                SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int(
                    CURSOR_META_SIZE(64, 64),
                    CURSOR_META_SIZE(1, 1),
                    CURSOR_META_SIZE(CURSOR_META_MAX,
                                     CURSOR_META_MAX)));
    pw_stream_update_params(c->stream, params, 4);
}

/******************************************************************************/
static void
on_state_changed(void *data, enum pw_stream_state old,
                 enum pw_stream_state state, const char *error)
{
    struct wlup_pwc *c = (struct wlup_pwc *)data;

    LOG(LOG_LEVEL_INFO, "wlup/%s: stream %s%s%s", c->who,
        pw_stream_state_as_string(state),
        error != NULL ? ": " : "", error != NULL ? error : "");
}

static const struct pw_stream_events stream_events =
{
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state_changed,
    .param_changed = on_param_changed,
    .process = on_process,
};

/******************************************************************************/
static void
on_core_info(void *data, const struct pw_core_info *info)
{
    struct wlup_pwc *c = (struct wlup_pwc *)data;

    if (!c->info_logged)
    {
        c->info_logged = 1;
        LOG(LOG_LEVEL_INFO, "wlup/%s: PipeWire daemon %s, library %s",
            c->who, info->version != NULL ? info->version : "(unknown)",
            pw_get_library_version());
    }
}

/******************************************************************************/
static void
on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
    struct wlup_pwc *c = (struct wlup_pwc *)data;

    LOG(LOG_LEVEL_ERROR, "wlup/%s: PipeWire error on %u: %s", c->who, id,
        message);
}

static const struct pw_core_events core_events =
{
    .version = PW_VERSION_CORE_EVENTS,
    .info = on_core_info,
    .error = on_core_error,
};

/******************************************************************************/
/* The formats we accept. For a virtual monitor, the preferred size here
 * becomes the size of the monitor. No modifier is offered, so buffers
 * are shared memory */
static const struct spa_pod *
build_enum_format(struct spa_pod_builder *b, int width, int height)
{
    struct spa_rectangle size = SPA_RECTANGLE((uint32_t)width,
                                (uint32_t)height);
    struct spa_rectangle min_size = SPA_RECTANGLE(1, 1);
    struct spa_rectangle max_size = SPA_RECTANGLE(8192, 8192);
    struct spa_fraction rate = SPA_FRACTION(0, 1);
    struct spa_fraction max_rate = SPA_FRACTION(60, 1);
    struct spa_fraction min_rate = SPA_FRACTION(1, 1);

    /* (casts: the macro gives a void *, which C++ does not convert) */
    return (const struct spa_pod *)spa_pod_builder_add_object(b,
            SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_VIDEO_format,
            SPA_POD_CHOICE_ENUM_Id(3, SPA_VIDEO_FORMAT_BGRx,
                                   SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA),
            SPA_FORMAT_VIDEO_size,
            SPA_POD_CHOICE_RANGE_Rectangle(&size, &min_size, &max_size),
            SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&rate),
            SPA_FORMAT_VIDEO_maxFramerate,
            SPA_POD_CHOICE_RANGE_Fraction(&max_rate, &min_rate, &max_rate));
}

/******************************************************************************/
/* Creates c's stream on c->core and connects it to the node.
 * return error */
static int
connect_stream(struct wlup_pwc *c, uint32_t node_id, int width, int height)
{
    uint8_t pod_buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(pod_buf, sizeof(pod_buf));
    const struct spa_pod *params[1];

    c->stream = pw_stream_new(c->core, "xrdp",
                              pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
                                  PW_KEY_MEDIA_CATEGORY, "Capture",
                                  PW_KEY_MEDIA_ROLE, "Screen",
                                  NULL));
    pw_stream_add_listener(c->stream, &c->stream_listener, &stream_events, c);

    params[0] = build_enum_format(&b, width, height);

    if (pw_stream_connect(c->stream, PW_DIRECTION_INPUT, node_id,
                          (enum pw_stream_flags)
                          (PW_STREAM_FLAG_AUTOCONNECT |
                           PW_STREAM_FLAG_MAP_BUFFERS),
                          params, 1) < 0)
    {
        LOG(LOG_LEVEL_ERROR, "wlup/%s: pw_stream_connect failed", c->who);
        return 1;
    }
    return 0;
}

/******************************************************************************/
struct wlup_pwc *
wlup_pwc_connect(struct wlup *v, const char *who, uint32_t node_id,
                 int width, int height)
{
    struct wlup_pwc *c;

    if (!pw_initialised)
    {
        pw_init(NULL, NULL);
        pw_initialised = 1;
    }
    c = (struct wlup_pwc *)g_malloc(sizeof(struct wlup_pwc), 1);
    c->v = v;
    c->who = who;
    c->loop = pw_loop_new(NULL);
    c->context = pw_context_new(c->loop, NULL, 0);
    if (c->context == NULL)
    {
        LOG(LOG_LEVEL_ERROR, "wlup/%s: pw_context_new failed", who);
        wlup_pwc_destroy(c);
        return NULL;
    }
    if (v->pipewire_fd >= 0)
    {
        /* Connection to the session's PipeWire daemon from sesman, made
         * as the session user. PipeWire takes ownership of it */
        c->core = pw_context_connect_fd(c->context, v->pipewire_fd, NULL, 0);
        v->pipewire_fd = -1;
    }
    else
    {
        /* The session's PipeWire daemon, found through XDG_RUNTIME_DIR */
        c->core = pw_context_connect(c->context, NULL, 0);
    }
    if (c->core == NULL)
    {
        LOG(LOG_LEVEL_ERROR, "wlup/%s: cannot connect to PipeWire: %s",
            who, strerror(errno));
        wlup_pwc_destroy(c);
        return NULL;
    }
    pw_core_add_listener(c->core, &c->core_listener, &core_events, c);

    if (connect_stream(c, node_id, width, height) != 0)
    {
        wlup_pwc_destroy(c);
        return NULL;
    }
    return c;
}

/******************************************************************************/
struct wlup_pwc *
wlup_pwc_connect_more(struct wlup_pwc *first, uint32_t node_id,
                      int width, int height)
{
    struct wlup_pwc *c;

    c = (struct wlup_pwc *)g_malloc(sizeof(struct wlup_pwc), 1);
    c->v = first->v;
    c->who = first->who;
    c->loop = first->loop;
    c->context = first->context;
    c->core = first->core;
    c->shared = 1;
    c->info_logged = 1;
    if (connect_stream(c, node_id, width, height) != 0)
    {
        wlup_pwc_destroy(c);
        return NULL;
    }
    return c;
}

/******************************************************************************/
void
wlup_pwc_destroy(struct wlup_pwc *c)
{
    if (c == NULL)
    {
        return;
    }
    if (c->stream != NULL)
    {
        pw_stream_destroy(c->stream);
    }
    if (c->shared)
    {
        /* The connection is the first stream's */
        g_free(c);
        return;
    }
    if (c->core != NULL)
    {
        pw_core_disconnect(c->core);
    }
    if (c->context != NULL)
    {
        pw_context_destroy(c->context);
    }
    if (c->loop != NULL)
    {
        pw_loop_destroy(c->loop);
    }
    g_free(c);
}

/******************************************************************************/
int
wlup_pwc_get_fd(struct wlup_pwc *c)
{
    return pw_loop_get_fd(c->loop);
}

/******************************************************************************/
void
wlup_pwc_process(struct wlup_pwc *c)
{
    pw_loop_enter(c->loop);
    pw_loop_iterate(c->loop, 0);
    pw_loop_leave(c->loop);
}

/******************************************************************************/
int
wlup_pwc_set_size(struct wlup_pwc *c, int width, int height)
{
    uint8_t pod_buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(pod_buf, sizeof(pod_buf));
    const struct spa_pod *params[1];

    params[0] = build_enum_format(&b, width, height);
    if (pw_stream_update_params(c->stream, params, 1) < 0)
    {
        LOG(LOG_LEVEL_ERROR, "wlup/%s: cannot renegotiate the stream",
            c->who);
        return 1;
    }
    return 0;
}

/******************************************************************************/
void
wlup_pwc_frames_wanted(struct wlup_pwc *c)
{
    struct wlup *v = c->v;
    int i;

    if (c->num_held == 0 || !wlup_wants_frames(v))
    {
        return;
    }
    sort_held(c);
    wlup_begin_update(v);
    for (i = 0; i < c->num_held; ++i)
    {
        wlup_paint_region(v, c->held[i * 4], c->held[i * 4 + 1],
                          c->held[i * 4 + 2], c->held[i * 4 + 3]);
    }
    wlup_end_update(v);
    c->num_held = 0;
    c->held_box = 0;
}

/******************************************************************************/
void
wlup_pwc_place(struct wlup_pwc *c, int x, int y,
               void (*sized)(void *data), void *data)
{
    c->x = sized != NULL ? x : 0;
    c->y = sized != NULL ? y : 0;
    c->sized = sized;
    c->sized_data = data;
    c->full = 1;
    /* Held rects were in the old place */
    c->num_held = 0;
    c->held_box = 0;
}

/******************************************************************************/
void
wlup_pwc_get_size(struct wlup_pwc *c, int *width, int *height)
{
    *width = c->width;
    *height = c->height;
}

/******************************************************************************/
void
wlup_pwc_repaint(struct wlup_pwc *c)
{
    c->full = 1;
}
