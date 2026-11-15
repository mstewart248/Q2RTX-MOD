/*
Copyright (C) 2026 Matt Stewart

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

// cl_blood_skin: blood on monsters and items, masks in their texture space
// (blood_skin.c). The API-free half, shared by vkpt and the D3D12 backend; the
// mask size BLOOD_SKIN_RES is shader/vertex_buffer.h's.

#ifndef BLOOD_SKIN_H
#define BLOOD_SKIN_H

#include "shared/shared.h"
#include "refresh/models.h"

#define BLOOD_SKIN_SLOTS        128
#define BLOOD_SKIN_SLOT_BYTES   (BLOOD_SKIN_RES * BLOOD_SKIN_RES)
#define BLOOD_SKIN_POOL_BYTES   ((size_t)BLOOD_SKIN_SLOTS * BLOOD_SKIN_SLOT_BYTES)
#define BLOOD_SKIN_UPLOAD_MAX   (2u << 20)  // bytes sent a frame at most; the rest wait

typedef struct {
	uint32_t offset, size;  // the same offset in the pool and in the device buffer
} blood_skin_copy_t;

typedef struct {
	const byte              *texels;    // the pool
	const blood_skin_copy_t *copies;
	int                     num_copies;
	uint32_t                bytes;      // their sum
} blood_skin_upload_t;

void vkpt_blood_skin_splat(const mod_blood_splat_t *splat);     // MOD_BloodSkinSplat
// fill_model_instance: the mask (slot + 1) for an entity's mesh, 0 for none
uint32_t vkpt_blood_skin_lookup(int key, qhandle_t model, int mesh, float color[3]);
// once a frame, by the backend: the rows to send - counted as sent
void vkpt_blood_skin_collect(blood_skin_upload_t *out);
// the device buffer is new (or its copy failed): every slot is sent again
void vkpt_blood_skin_device_reset(void);

#endif
