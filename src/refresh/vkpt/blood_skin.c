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

/*
BLOOD ON BODIES - cl_blood_skin (Matt 11-15: "it would be cool if it splattered on
monsters and items"; not on gibs).

A droplet that hits a monster, a corpse or an item still runs off it as before
(CL_BloodRunOffModel), and now also paints where it touched into a MASK in that
model's texture space: one byte of "how much blood" per texel, BLOOD_SKIN_RES
square, kept per (entity, mesh). The material shader blends it over the skin
(get_material, path_tracer_rgen.h), so the stain follows the animation for nothing
- no geometry, no re-posing. A droplet running down a body re-hits it every step,
so it leaves a streak.

Why not the world's film: that is geometry laid on flat surfaces. On a body every
tile would have to follow the mesh each frame - the same resend-what-moved cost
that took the blood dips a day to remove.

THE POOL. BLOOD_SKIN_SLOTS masks of BLOOD_SKIN_RES^2 bytes (128 x 64 KB = 8 MB),
the same layout on the CPU (here) and on the GPU (BLOOD_SKIN_BUFFER, binding 14 of
the vertex set). A slot is (key, model, mesh): key = entity number + 1
(entity_t.blood_key, set by the client on a packet entity's main model). The
entity number, not entity_t.id: the id is new each time an entity re-enters the
PVS, and a corpse should still be bloody when you come back to it. A different
model under a key (a monster gibbed into its head, a new entity in a reused slot
with another model) frees the key's slots. Full, the slot painted longest ago is
taken. A map load (registration_sequence) clears the pool.

THE UPLOAD. Painting records the rows it touched; once a frame the backend sends
those rows (vkpt_blood_skin_collect), at most BLOOD_SKIN_UPLOAD_MAX bytes - the
rest wait a frame. A slot is named to the shader (vkpt_blood_skin_lookup) only
once it has been sent whole, so a new slot never shows the device buffer's old
bytes.

Known limits, told to Matt: models whose two sides share one half of the skin
get blood on both sides at once; a splat is cut where the texture's islands end;
it is a stain on the skin, not a raised film.
*/

#include "vkpt.h"
#include "system/system.h"	// Sys_Milliseconds (implicit on gcc 14+ is an error)

extern void cast_u32_to_f32_color(int color_index, const color_t* pcolor, float* color_f32, float hdr_factor);

typedef struct {
	int         key;            // 0 = free
	qhandle_t   model;
	int         mesh;
	float       color[3];       // linear
	unsigned    stamp;          // last painted (eviction)
	int         row0, row1;     // rows still to send; row0 > row1 = none
	bool        live;           // sent whole at least once
	int16_t     next;           // the key's next slot, -1
} skin_slot_t;

static struct {
	byte               *texels;     // BLOOD_SKIN_POOL_BYTES
	skin_slot_t         slots[BLOOD_SKIN_SLOTS];
	int16_t             head[MAX_EDICTS + 1];   // key -> its first slot, -1
	unsigned            stamp;
	int                 registration;
	blood_skin_copy_t   copies[BLOOD_SKIN_SLOTS];
	// pt_blood_skin_stats
	unsigned            splats, sent_bytes, evictions;
	unsigned            stats_ms;
} skin;

static cvar_t *cvar_pt_blood_skin_stats;

static void skin_clear(void)
{
	for (int i = 0; i < BLOOD_SKIN_SLOTS; i++) {
		skin.slots[i].key = 0;
		skin.slots[i].next = -1;
		skin.slots[i].live = false;
		skin.slots[i].row0 = 1;
		skin.slots[i].row1 = 0;
	}
	for (int k = 0; k <= MAX_EDICTS; k++)
		skin.head[k] = -1;
	skin.registration = registration_sequence;
}

static bool skin_ready(void)
{
	if (!skin.texels) {
		skin.texels = Z_Malloc(BLOOD_SKIN_POOL_BYTES);
		if (!skin.texels)
			return false;
		skin_clear();
		cvar_pt_blood_skin_stats = Cvar_Get("pt_blood_skin_stats", "0", 0);
	}
	if (skin.registration != registration_sequence)
		skin_clear();   // a new map: its entity numbers and models are another map's
	return true;
}

// the device's copy is gone (a new buffer): every slot is sent again before use
void vkpt_blood_skin_device_reset(void)
{
	if (!skin.texels)
		return;
	for (int i = 0; i < BLOOD_SKIN_SLOTS; i++) {
		skin.slots[i].live = false;
		if (skin.slots[i].key) {
			skin.slots[i].row0 = 0;
			skin.slots[i].row1 = BLOOD_SKIN_RES - 1;
		}
	}
}

static void skin_unlink(int s)
{
	skin_slot_t *slot = &skin.slots[s];
	int16_t *link = &skin.head[slot->key];
	while (*link >= 0) {
		if (*link == s) {
			*link = (int16_t)slot->next;
			break;
		}
		link = &skin.slots[*link].next;
	}
	slot->key = 0;
	slot->next = -1;
	slot->live = false;
	slot->row0 = 1;
	slot->row1 = 0;
}

static void skin_free_key(int key)
{
	while (skin.head[key] >= 0)
		skin_unlink(skin.head[key]);
}

// The key's slot for this mesh, or -1. A slot of another model frees the key.
static int skin_find(int key, qhandle_t model, int mesh)
{
	for (int s = skin.head[key]; s >= 0; s = skin.slots[s].next) {
		if (skin.slots[s].model != model) {
			skin_free_key(key);
			return -1;
		}
		if (skin.slots[s].mesh == mesh)
			return s;
	}
	return -1;
}

static int skin_alloc(int key, qhandle_t model, int mesh)
{
	int s = -1;
	unsigned oldest = UINT_MAX;
	for (int i = 0; i < BLOOD_SKIN_SLOTS; i++) {
		if (!skin.slots[i].key) {
			s = i;
			break;
		}
		if (skin.slots[i].stamp < oldest) {
			oldest = skin.slots[i].stamp;
			s = i;
		}
	}
	if (s < 0)
		return -1;
	if (skin.slots[s].key) {
		skin_unlink(s);
		skin.evictions++;
	}

	skin_slot_t *slot = &skin.slots[s];
	slot->key = key;
	slot->model = model;
	slot->mesh = mesh;
	slot->live = false;
	slot->row0 = 0;
	slot->row1 = BLOOD_SKIN_RES - 1;    // sent whole before anything names it
	slot->next = skin.head[key];
	skin.head[key] = (int16_t)s;
	memset(skin.texels + (size_t)s * BLOOD_SKIN_SLOT_BYTES, 0, BLOOD_SKIN_SLOT_BYTES);
	return s;
}

static inline float skin_hash(int x, int y, int seed)
{
	uint32_t h = (uint32_t)x * 0x8da6b343u ^ (uint32_t)y * 0xd8163841u ^ (uint32_t)seed * 0xcb1ab31fu;
	h ^= h >> 13;
	h *= 0x5bd1e995u;
	h ^= h >> 15;
	return (float)(h & 0xffff) * (1.f / 65535.f);
}

// MOD_BloodSkinSplat
void vkpt_blood_skin_splat(const mod_blood_splat_t *sp)
{
	if (!sp || sp->key <= 0 || sp->key > MAX_EDICTS || sp->mesh < 0 || !(sp->strength > 0.f))
		return;
	if (!skin_ready())
		return;

	int s = skin_find(sp->key, sp->model, sp->mesh);
	const bool fresh = s < 0;
	if (fresh)
		s = skin_alloc(sp->key, sp->model, sp->mesh);
	if (s < 0)
		return;
	skin_slot_t *slot = &skin.slots[s];
	slot->stamp = ++skin.stamp;
	skin.splats++;

	// the blood's colour, as the droplet's own (blood.c's write_blood_geometry)
	float c[3];
	cast_u32_to_f32_color(sp->color, &sp->rgba, c, 1.0f);
	for (int i = 0; i < 3; i++)
		slot->color[i] = fresh ? c[i] : slot->color[i] + (c[i] - slot->color[i]) * 0.15f;

	// a soft disc, roughened per texel so it reads as spatter rather than a stamp
	const float res = (float)BLOOD_SKIN_RES;
	const float r = max(0.75f, min(sp->radius * res, 20.f));
	const float cx = (sp->st[0] - floorf(sp->st[0])) * res - 0.5f;
	const float cy = (sp->st[1] - floorf(sp->st[1])) * res - 0.5f;
	const int x0 = (int)floorf(cx - r), x1 = (int)ceilf(cx + r);
	const int y0 = (int)floorf(cy - r), y1 = (int)ceilf(cy + r);
	const float strength = min(sp->strength, 1.f);
	const int seed = (int)skin.stamp;
	byte *tex = skin.texels + (size_t)s * BLOOD_SKIN_SLOT_BYTES;
	bool wrapped_rows = false;
	int touched0 = BLOOD_SKIN_RES, touched1 = -1;

	for (int y = y0; y <= y1; y++) {
		const int wy = y & (BLOOD_SKIN_RES - 1);
		const float dy = ((float)y - cy) / r;
		bool row_touched = false;
		for (int x = x0; x <= x1; x++) {
			const float dx = ((float)x - cx) / r;
			const float d2 = dx * dx + dy * dy;
			if (d2 >= 1.f)
				continue;
			const float fall = (1.f - d2) * (1.f - d2);
			const float v = strength * fall * (0.55f + 0.45f * skin_hash(x, y, seed));
			byte *t = tex + wy * BLOOD_SKIN_RES + (x & (BLOOD_SKIN_RES - 1));
			const int old = *t;
			const int nv = old + (int)((float)(255 - old) * v + 0.5f);
			if (nv != old) {
				*t = (byte)min(nv, 255);
				row_touched = true;
			}
		}
		if (row_touched) {
			if (y != wy)
				wrapped_rows = true;
			touched0 = min(touched0, wy);
			touched1 = max(touched1, wy);
		}
	}

	if (touched1 < 0)
		return;
	if (wrapped_rows) {
		touched0 = 0;
		touched1 = BLOOD_SKIN_RES - 1;
	}
	if (slot->row0 > slot->row1) {
		slot->row0 = touched0;
		slot->row1 = touched1;
	} else {
		slot->row0 = min(slot->row0, touched0);
		slot->row1 = max(slot->row1, touched1);
	}
}

// fill_model_instance: the mask (slot + 1) for this entity's mesh, 0 for none
uint32_t vkpt_blood_skin_lookup(int key, qhandle_t model, int mesh, float color[3])
{
	if (key <= 0 || key > MAX_EDICTS || !skin.texels || skin.registration != registration_sequence)
		return 0;
	if (skin.head[key] < 0)
		return 0;
	const int s = skin_find(key, model, mesh);    // another model: the key is freed
	if (s < 0 || !skin.slots[s].live)
		return 0;
	VectorCopy(skin.slots[s].color, color);
	return (uint32_t)s + 1;
}

// Once a frame, by the backend: the rows to send, at most BLOOD_SKIN_UPLOAD_MAX
// bytes. They count as sent - the backend records their copy this frame.
void vkpt_blood_skin_collect(blood_skin_upload_t *out)
{
	memset(out, 0, sizeof(*out));
	if (!skin.texels || skin.registration != registration_sequence)
		return;
	out->texels = skin.texels;
	out->copies = skin.copies;

	uint32_t bytes = 0;
	for (int s = 0; s < BLOOD_SKIN_SLOTS; s++) {
		skin_slot_t *slot = &skin.slots[s];
		if (!slot->key || slot->row0 > slot->row1)
			continue;
		const uint32_t size = (uint32_t)(slot->row1 - slot->row0 + 1) * BLOOD_SKIN_RES;
		if (bytes + size > BLOOD_SKIN_UPLOAD_MAX)
			continue;   // next frame
		blood_skin_copy_t *c = &skin.copies[out->num_copies++];
		c->offset = (uint32_t)s * BLOOD_SKIN_SLOT_BYTES + (uint32_t)slot->row0 * BLOOD_SKIN_RES;
		c->size = size;
		bytes += size;
		slot->row0 = 1;
		slot->row1 = 0;
		slot->live = true;
	}
	out->bytes = bytes;
	skin.sent_bytes += bytes;

	// pt_blood_skin_stats 1: once a second, what the pool holds and what it cost
	if (cvar_pt_blood_skin_stats && cvar_pt_blood_skin_stats->integer) {
		const unsigned now = Sys_Milliseconds();
		if (now - skin.stats_ms >= 1000) {
			int used = 0;
			for (int s = 0; s < BLOOD_SKIN_SLOTS; s++)
				used += skin.slots[s].key != 0;
			Com_Printf("blood skin: %d/%d masks, %u splats, %.1f KB sent, %u evicted in the last second\n",
				used, BLOOD_SKIN_SLOTS, skin.splats, (float)skin.sent_bytes / 1024.f, skin.evictions);
			skin.splats = skin.sent_bytes = skin.evictions = 0;
			skin.stats_ms = now;
		}
	}
}

/* ---------------------------------------------------------------------------
   Vulkan: qvk.buf_blood_skin (vertex_buffer.c) and a staging buffer per frame
   in flight, made the first time there is something to send.
   --------------------------------------------------------------------------- */

static struct {
	BufferResource_t    staging[MAX_FRAMES_IN_FLIGHT];
	void               *mapped[MAX_FRAMES_IN_FLIGHT];
	bool                made, failed;
} skin_vk;

void vkpt_blood_skin_destroy_vk(void)
{
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		if (skin_vk.mapped[i]) {
			buffer_unmap(&skin_vk.staging[i]);
			skin_vk.mapped[i] = NULL;
		}
		buffer_destroy(&skin_vk.staging[i]);
	}
	skin_vk.made = skin_vk.failed = false;
}

static bool skin_vk_staging(void)
{
	if (skin_vk.made)
		return true;
	if (skin_vk.failed)
		return false;
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		if (buffer_create(&skin_vk.staging[i], BLOOD_SKIN_UPLOAD_MAX, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS
			|| !(skin_vk.mapped[i] = buffer_map(&skin_vk.staging[i])))
		{
			vkpt_blood_skin_destroy_vk();
			skin_vk.failed = true;  // no blood on bodies rather than a retry a frame
			Com_WPrintf("blood skin: no memory for its staging; blood will not stain bodies\n");
			return false;
		}
	}
	skin_vk.made = true;
	return true;
}

// The frame's rows into qvk.buf_blood_skin, before anything traces.
void vkpt_blood_skin_record(VkCommandBuffer cmd)
{
	if (!qvk.buf_blood_skin.buffer || !skin.texels)
		return;
	// nothing to send: no staging made, nothing collected
	bool any = false;
	for (int s = 0; s < BLOOD_SKIN_SLOTS && !any; s++)
		any = skin.slots[s].key && skin.slots[s].row0 <= skin.slots[s].row1;
	if (!any || !skin_vk_staging())
		return;

	blood_skin_upload_t up;
	vkpt_blood_skin_collect(&up);
	if (!up.num_copies)
		return;

	const int fi = qvk.current_frame_index % MAX_FRAMES_IN_FLIGHT;
	byte *dst = skin_vk.mapped[fi];
	VkBufferCopy regions[BLOOD_SKIN_SLOTS];
	VkDeviceSize at = 0;
	for (int i = 0; i < up.num_copies; i++) {
		memcpy(dst + at, up.texels + up.copies[i].offset, up.copies[i].size);
		regions[i].srcOffset = at;
		regions[i].dstOffset = up.copies[i].offset;
		regions[i].size = up.copies[i].size;
		at += up.copies[i].size;
	}

	// last frame's shader reads, then the copy, then this frame's reads
	const VkPipelineStageFlags shader_stages = qvk.use_ray_query
		? (VkPipelineStageFlags)VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
		: (VkPipelineStageFlags)(VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	VkBufferMemoryBarrier b = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.buffer = qvk.buf_blood_skin.buffer,
		.offset = 0,
		.size = VK_WHOLE_SIZE,
	};
	vkCmdPipelineBarrier(cmd, shader_stages, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1, &b, 0, NULL);
	vkCmdCopyBuffer(cmd, skin_vk.staging[fi].buffer, qvk.buf_blood_skin.buffer, (uint32_t)up.num_copies, regions);
	b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, shader_stages, 0, 0, NULL, 1, &b, 0, NULL);
}
