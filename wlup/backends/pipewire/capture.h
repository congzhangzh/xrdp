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
 * wlup: frames from a PipeWire screen cast stream
 *
 * Compositors that cast their outputs to PipeWire (Mutter through its
 * ScreenCast D-Bus API, KWin through zkde_screencast_unstable_v1) hand
 * out a PipeWire node. This receives that node's frames in shared
 * memory, copies what changed into the desktop image, sends the pointer
 * from the cursor metadata, and paints. The stream is driven from
 * xrdp's poll loop: no thread.
 */

#ifndef WLUP_PIPEWIRE_CAPTURE_H
#define WLUP_PIPEWIRE_CAPTURE_H

#include <stdint.h>

struct wlup;
struct wlup_pwc;

/* Receives the frames of a PipeWire node. The connection is
 * v->pipewire_fd if sesman made one (it is then ours), or the session's
 * PipeWire daemon found through XDG_RUNTIME_DIR. width x height is the
 * preferred size: Mutter sizes a virtual monitor after it.
 * who: the backend, for the log.
 * return NULL on error */
struct wlup_pwc *
wlup_pwc_connect(struct wlup *v, const char *who, uint32_t node_id,
                 int width, int height);

void
wlup_pwc_destroy(struct wlup_pwc *c);

/* The fd to poll for reading */
int
wlup_pwc_get_fd(struct wlup_pwc *c);

/* Handles what is ready: new frames are copied and painted */
void
wlup_pwc_process(struct wlup_pwc *c);

/* Offers width x height as the preferred size, which renegotiates the
 * stream format. return error */
int
wlup_pwc_set_size(struct wlup_pwc *c, int width, int height);

/* Paints the damage held back while the core wanted no frames
 * (wlup_wants_frames()); for the backend's frames_wanted() */
void
wlup_pwc_frames_wanted(struct wlup_pwc *c);

#endif
