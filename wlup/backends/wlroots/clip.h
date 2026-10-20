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
 * wlup: session clipboard (clip.c), through ext-data-control-v1, for
 * the backends that speak Wayland to the compositor
 */

#ifndef WLUP_WLR_CLIP_H
#define WLUP_WLR_CLIP_H

struct wlup;
struct wl_display;
struct wl_registry;
struct wl_seat;
struct wlup_wlr_clip;

/* Binds ext-data-control-v1 if interface is it.
 * return the clipboard state, or NULL if interface is another one */
struct wlup_wlr_clip *
wlup_wlr_clip_bind(struct wlup *v, struct wl_display *display,
                   struct wl_registry *registry, unsigned int name,
                   const char *interface);

/* Starts following the selection, once the seat is known. c may be
 * NULL: the compositor has no ext-data-control-v1 */
void
wlup_wlr_clip_start(struct wlup_wlr_clip *c, struct wl_seat *seat);

/* Reads a new session selection; call after dispatching events */
void
wlup_wlr_clip_process(struct wlup_wlr_clip *c);

void
wlup_wlr_clip_destroy(struct wlup_wlr_clip *c);

/* The RDP client's clipboard now holds this UTF-8 text: make the
 * session's clipboard offer it */
void
wlup_wlr_clip_set_text(struct wlup_wlr_clip *c, const char *text, int len);

#endif
