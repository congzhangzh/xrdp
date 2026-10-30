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
 */

#ifndef _XRDP_ENCODER_AV2_H
#define _XRDP_ENCODER_AV2_H

#include "arch.h"

/* The same calls as the H.264 encoders (xrdp_encoder_h264_*_proc) */
void *
xrdp_encoder_av2_create(void);
int
xrdp_encoder_av2_delete(void *handle);
int
xrdp_encoder_av2_encode(void *handle, int session, int left, int top,
                        int width, int height, int twidth, int theight,
                        int format, const char *data,
                        short *crects, int num_crects,
                        char *cdata, int *cdata_bytes, int connection_type,
                        int *flags_ptr);

#endif
