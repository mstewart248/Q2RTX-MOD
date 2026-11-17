/*
Copyright (C) 2021, NVIDIA CORPORATION. All rights reserved.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#ifndef __CAMERAS_H_
#define __CAMERAS_H_

typedef struct bsp_mesh_s bsp_mesh_t;

void vkpt_cameras_init(void);
void vkpt_cameras_shutdown(void);
void vkpt_cameras_load(bsp_mesh_t* wm, const char* map_name);

// Quake III Arena maps: their misc_portal_surfaces that look through a
// misc_portal_camera become camera slots of their own.
typedef struct bsp_s bsp_t;
typedef struct mface_s mface_t;
void vkpt_cameras_load_q3(bsp_mesh_t* wm, const bsp_t* bsp);
// The portal a face of a portal shader belongs to: the misc_portal_surface
// within 64 units of its plane, as Quake III picks it. -1 for none - then the
// face is a mirror, as in Quake III.
int vkpt_cameras_q3_face(bsp_mesh_t* wm, const mface_t* surf);
// Fills a portal's slot of the UBO's security_camera_data, every frame.
void vkpt_cameras_q3_transform(const bsp_mesh_t* wm, int index, float time, float data[16]);

#endif