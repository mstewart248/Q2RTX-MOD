/*
Copyright (C) 2019, NVIDIA CORPORATION. All rights reserved.

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

#include "physical_sky.h"
#include "precomputed_sky.h"
#include "system/system.h"
#include "../../client/client.h"
#include <time.h>

#include <SDL_joystick.h>
#include <SDL_gamecontroller.h>

static VkImage          img_envmap = 0;
static VkImageView      imv_envmap = 0;
static VkDeviceMemory   mem_envmap = 0;

static VkPipeline       pipeline_physical_sky;
static VkPipeline       pipeline_physical_sky_space;
static VkPipelineLayout pipeline_layout_physical_sky;
static VkPipeline       pipeline_resolve;
static VkPipelineLayout pipeline_layout_resolve;

static int width  = 1024, 
           height = 1024;

static int skyNeedsUpdate = VK_TRUE;

cvar_t *sun_color[3];
cvar_t *sun_elevation;
cvar_t *sun_azimuth;
cvar_t *sun_angle;
cvar_t *sun_brightness;
cvar_t *sun_bounce;
cvar_t *sun_animate;
cvar_t *sun_gamepad;

cvar_t *sun_preset;
cvar_t *sun_latitude;

cvar_t *physical_sky;
cvar_t *physical_sky_draw_clouds;
cvar_t *physical_sky_space;
cvar_t *physical_sky_brightness;

// How much light the MAP'S OWN skybox casts on the world, as a multiple of the
// look it has always had. The procedural sky keeps its own EV control in
// physical_sky_brightness; an authored cube map has no sun position or
// atmosphere for an EV stop to mean anything against, which is why the two are
// separate. See vkpt_physical_sky_update_ubo.
//
// Only C reads it, so it deliberately gets no UBO slot of its own - it is
// folded into pt_env_scale, which the shaders already read.
static cvar_t *pt_sky_light_scale;

// Read-only, published for the menu: 1 while the procedural sky is what gets
// rendered, 0 while the map's own skybox is. See publish_sky_state.
static cvar_t *sky_procedural_active;

// The menu's single "sky type" control. The renderer's real inputs are
// physical_sky - which procedural preset backs a map with no skybox of its own
// - and sky_use_map_skybox, whether a map that ships one gets to use it. Those
// two are not a menu's worth of choices on their own: "rerelease" is really a
// statement about the second, and the other three are about the first. So the
// menu drives this, and this drives those.
static cvar_t *sky_type;

#define SKY_TYPE_RERELEASE  0
#define SKY_TYPE_ENVMAP     1
#define SKY_TYPE_EARTH      2
#define SKY_TYPE_STROGGOS   3

// index into skyPresets, i.e. a physical_sky value - not a SKY_TYPE_
#define SKY_STROGGOS_PRESET 2

// [rerelease] Maps that render the procedural sky even in "rerelease" sky mode,
// where a map that ships a skybox normally wins.
//
// mgu1m1 is why this exists, and it is the only entry. Its worldspawn names no
// sky at all, so CS_SKY falls back to the stock unit1_ cube map and the map
// looks as though it had authored one - and unit1_ is a blue Earth sky over a
// map whose own _sunlight_color and heightfog are orange. The Stroggos
// atmosphere with a fixed sun is what it wants.
//
// Doing this from maps/mgu1m1.cfg by pointing the "sky" command at a basename
// that does not exist could not hold: the target_sky its drop pod fires on
// landing re-issues CS_SKY*, CL_SetSky runs again, and the real skybox comes
// straight back.
//
// Listed maps get the map's own skybox declined and the Stroggos preset
// regardless of what physical_sky says. Nothing else changes - the sun still
// follows whatever preset the player selected, see apply_map_sun.
//
// Whitespace- or comma-separated SHORT map names, the same form cl.mapname is in
// - "mgu1m1", not "maps/mgu1m1.bsp". Matches cl_dynamic_lights_exclude.
static cvar_t *sky_procedural_maps;

// The sun a listed map STARTS with. Copied into sun_elevation / sun_azimuth
// ONCE, on map load, and only while the sun preset is Custom - see apply_map_sun.
//
// Once only, because the player has to be able to move the sun afterwards and
// have it stay moved; a per-frame override would fight the menu. Custom only,
// because any other preset means the player asked for a time of day, and a map
// is not entitled to overrule that.
//
// These are per-map, set from maps/<name>.cfg - which is exec'd at BSP load,
// well before the first frame apply_map_sun can run on - and reset back to the
// defaults below by maps/default.cfg. So a second override is two steps: add the
// map to sky_procedural_maps, and give it a cfg with these two lines. A listed
// map with no cfg of its own just gets the defaults.
//
// The defaults are mgu1m1's, set by eye with the sky held still. They are NOT
// the map's own _sun_mangle "30 -70 0" - that is what its lightmap compiler was
// aimed with, and it is not where this sun looks right.
static cvar_t *sky_map_sun_elevation;
static cvar_t *sky_map_sun_azimuth;

#define SKY_MAP_LIST_SEPARATORS " \t\r\n,;"

// Whether cl.mapname appears in a whitespace/comma separated list cvar.
static bool sky_map_in_list(const cvar_t *list)
{
	const char *p;
	size_t      maplen;

	if (!list || !list->string[0] || !cl.mapname[0])
		return false;

	maplen = strlen(cl.mapname);

	for (p = list->string; *p; ) {
		size_t len;

		p += strspn(p, SKY_MAP_LIST_SEPARATORS);
		if (!*p)
			break;

		len = strcspn(p, SKY_MAP_LIST_SEPARATORS);
		if (len == maplen && !Q_strncasecmp(p, cl.mapname, len))
			return true;

		p += len;
	}

	return false;
}

// Only meaningful in "rerelease" sky mode. An explicit sky_type pick already
// declines every map's skybox and already says which atmosphere to render, so
// overriding it here would make the menu lie.
static bool sky_map_forces_procedural(void)
{
	return sky_type && sky_type->integer == SKY_TYPE_RERELEASE
		&& sky_map_in_list(sky_procedural_maps);
}

// The preset that actually gets rendered, which is not always the one
// physical_sky names. Every read of physical_sky->integer goes through this.
static int effective_physical_sky(void)
{
	if (sky_map_forces_procedural())
		return SKY_STROGGOS_PRESET;

	return physical_sky->integer;
}

cvar_t *sky_scattering;
cvar_t *sky_transmittance;
cvar_t *sky_phase_g;
cvar_t *sky_amb_phase_g;

cvar_t *sky_use_map_skybox;

// Scope of the skybox declared by the currently loaded map, as classified by
// R_SetSky_RTX: 0 = the map has no usable skybox, 1 = rerelease map, 2 = any
// other game directory. Compared against sky_use_map_skybox on every query so
// that toggling the cvar takes effect without reloading the map.
static int map_skybox_scope = 0;

static uint32_t physical_sky_planet_albedo_map = 0;
static uint32_t physical_sky_planet_normal_map = 0;

static time_t latched_local_time;

static int current_preset = 0;

static bool sdl_initialized = false;
static SDL_GameController* game_controller = 0;

void vkpt_physical_sky_latch_local_time()
{
	time(&latched_local_time);
}

typedef enum
{
	SUN_PRESET_NONE       = 0,
	SUN_PRESET_CURRENT_TIME = 1,
	SUN_PRESET_FAST_TIME  = 2,
	SUN_PRESET_NIGHT      = 3,
	SUN_PRESET_DAWN       = 4,
	SUN_PRESET_MORNING    = 5,
	SUN_PRESET_NOON       = 6,
	SUN_PRESET_EVENING    = 7,
	SUN_PRESET_DUSK       = 8,
	SUN_PRESET_COUNT
} sun_preset_t;

static int active_sun_preset(void)
{
	bool multiplayer = cl.maxclients > 1;

	if (multiplayer)
	{
		int preset = sun_preset->integer;
		if (preset == SUN_PRESET_CURRENT_TIME)
			return SUN_PRESET_FAST_TIME;
		return preset;
	}

	return sun_preset->integer;
}

static void change_image_layouts(VkImage image, const VkImageSubresourceRange* subresource_range);
void physical_sky_cvar_changed(cvar_t *self);

static void
destroyEnvTexture(void)
{
    if (imv_envmap != VK_NULL_HANDLE) {
        vkDestroyImageView(qvk.device, imv_envmap, NULL);
        imv_envmap = NULL;
    }
    if (img_envmap != VK_NULL_HANDLE) {
        vkDestroyImage(qvk.device, img_envmap, NULL);
        img_envmap = NULL;
    }
	if (mem_envmap != VK_NULL_HANDLE) {
		vkFreeMemory(qvk.device, mem_envmap, NULL);
		mem_envmap = VK_NULL_HANDLE;
	}
}

static VkResult
initializeEnvTexture(int width, int height)
{
    vkpt_device_wait_idle();
    destroyEnvTexture();

    const int num_images = 6;

    // cube image

    VkImageCreateInfo img_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .extent = {
            .width = width,
            .height = height,
            .depth = 1,
        },
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .mipLevels = 1,
        .arrayLayers = num_images,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT
                               | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                               | VK_IMAGE_USAGE_SAMPLED_BIT,
        .flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = qvk.queue_idx_graphics,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    _VK(vkCreateImage(qvk.device, &img_info, NULL, &img_envmap));
    ATTACH_LABEL_VARIABLE(img_envmap, IMAGE);

    VkMemoryRequirements mem_req;
    vkGetImageMemoryRequirements(qvk.device, img_envmap, &mem_req);
    //assert(mem_req.size >= (img_size * num_images));

	_VK(allocate_gpu_memory(mem_req, &mem_envmap));

    _VK(vkBindImageMemory(qvk.device, img_envmap, mem_envmap, 0));

    const VkImageSubresourceRange subresource_range = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = num_images,
    };

    change_image_layouts(img_envmap, &subresource_range);

    // image view

    VkImageViewCreateInfo img_view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .viewType = VK_IMAGE_VIEW_TYPE_CUBE,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .image = img_envmap,
        .subresourceRange = subresource_range,
        .components = {
            VK_COMPONENT_SWIZZLE_R,
            VK_COMPONENT_SWIZZLE_G,
            VK_COMPONENT_SWIZZLE_B,
            VK_COMPONENT_SWIZZLE_A,
        },
    };
    _VK(vkCreateImageView(qvk.device, &img_view_info, NULL, &imv_envmap));
    ATTACH_LABEL_VARIABLE(imv_envmap, IMAGE_VIEW);

    // cube descriptor layout
    {
        VkDescriptorImageInfo desc_img_info = {
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .imageView = imv_envmap,
            .sampler = qvk.tex_sampler,
        };

        VkWriteDescriptorSet s = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = qvk.desc_set_textures_even,
            .dstBinding = BINDING_OFFSET_PHYSICAL_SKY,
            .dstArrayElement = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .pImageInfo = &desc_img_info,
        };

        vkUpdateDescriptorSets(qvk.device, 1, &s, 0, NULL);

        s.dstSet = qvk.desc_set_textures_odd;
        vkUpdateDescriptorSets(qvk.device, 1, &s, 0, NULL);
    }

    // image descriptor
    {
        VkDescriptorImageInfo desc_img_info = {
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .imageView = imv_envmap,
        };

        VkWriteDescriptorSet s = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = qvk.desc_set_textures_even,
            .dstBinding = BINDING_OFFSET_PHYSICAL_SKY_IMG,
            .dstArrayElement = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .pImageInfo = &desc_img_info,
        };

        vkUpdateDescriptorSets(qvk.device, 1, &s, 0, NULL);

        s.dstSet = qvk.desc_set_textures_odd;
        vkUpdateDescriptorSets(qvk.device, 1, &s, 0, NULL);
    }
    return VK_SUCCESS;
}

VkResult
vkpt_physical_sky_initialize()
{
	current_preset = 0;

	SkyInitializeDataGPU();

	InitializeShadowmapResources();

	{
		VkDescriptorSetLayout desc_set_layouts[] = {
			qvk.desc_set_layout_ubo,
			qvk.desc_set_layout_textures,
			qvk.desc_set_layout_vertex_buffer,
			*SkyGetDescriptorLayout()
		};

		VkPushConstantRange push_constant_range = {
			.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			.offset = 0,
			.size = sizeof(float) * 16,
		};

		CREATE_PIPELINE_LAYOUT(qvk.device, &pipeline_layout_physical_sky,
			.setLayoutCount = LENGTH(desc_set_layouts),
			.pSetLayouts = desc_set_layouts,
			.pushConstantRangeCount = 1,
			.pPushConstantRanges = &push_constant_range,
		);

		ATTACH_LABEL_VARIABLE(pipeline_layout_physical_sky, PIPELINE_LAYOUT);
	}

	{
		VkDescriptorSetLayout desc_set_layouts[] = {
			qvk.desc_set_layout_ubo,
			qvk.desc_set_layout_vertex_buffer
		};

		CREATE_PIPELINE_LAYOUT(qvk.device, &pipeline_layout_resolve,
			.setLayoutCount = LENGTH(desc_set_layouts),
			.pSetLayouts = desc_set_layouts,
			);
		ATTACH_LABEL_VARIABLE(pipeline_layout_resolve, PIPELINE_LAYOUT);
	}

	if (!sdl_initialized)
	{
		SDL_InitSubSystem(SDL_INIT_JOYSTICK);
		sdl_initialized = true;
	}

	for (int i = 0; i < SDL_NumJoysticks(); i++)
	{
		if (SDL_IsGameController(i))
		{
			game_controller = SDL_GameControllerOpen(i);
			break;
		}
	}



    return initializeEnvTexture(width, height);
}

VkResult
vkpt_physical_sky_destroy()
{
	current_preset = 0;

	ReleaseShadowmapResources();
	SkyReleaseDataGPU();
	
	vkDestroyPipelineLayout(qvk.device, pipeline_layout_physical_sky, NULL);
	pipeline_layout_physical_sky = VK_NULL_HANDLE;
	vkDestroyPipelineLayout(qvk.device, pipeline_layout_resolve, NULL);
	pipeline_layout_resolve = VK_NULL_HANDLE;
	destroyEnvTexture();

	if (game_controller)
	{
		SDL_GameControllerClose(game_controller);
		game_controller = 0;
	}

	return VK_SUCCESS;
}

VkResult 
vkpt_physical_sky_beginRegistration() 
{
    physical_sky_planet_albedo_map = 0;
    physical_sky_planet_normal_map = 0;
    return VK_SUCCESS;
}

VkResult 
vkpt_physical_sky_endRegistration()
{
    if (physical_sky_space->integer > 0)
    {
        image_t const * albedo_map = IMG_Find("env/planet_albedo.tga", IT_SKIN, IF_SRGB);
        if (albedo_map != R_NOTEXTURE) {
            physical_sky_planet_albedo_map = albedo_map - r_images;
        }

        image_t const * normal_map = IMG_Find("env/planet_normal.tga", IT_SKIN, IF_SRGB);
        if (normal_map != R_NOTEXTURE) {
            physical_sky_planet_normal_map = normal_map - r_images;
        }
    }
    return VK_SUCCESS;
}

VkResult
vkpt_physical_sky_create_pipelines()
{
	{
		VkComputePipelineCreateInfo pipeline_info = {
			.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = SHADER_STAGE(QVK_MOD_PHYSICAL_SKY_COMP, VK_SHADER_STAGE_COMPUTE_BIT),
			.layout = pipeline_layout_physical_sky
		};

		_VK(vkCreateComputePipelines(qvk.device, 0, 1, &pipeline_info, 0, &pipeline_physical_sky));
	}

    {
        VkComputePipelineCreateInfo pipeline_info = {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = SHADER_STAGE(QVK_MOD_PHYSICAL_SKY_SPACE_COMP, VK_SHADER_STAGE_COMPUTE_BIT),
            .layout = pipeline_layout_physical_sky
        };

        _VK(vkCreateComputePipelines(qvk.device, 0, 1, &pipeline_info, 0, &pipeline_physical_sky_space));
    }

	{
		VkComputePipelineCreateInfo pipeline_info = {
			.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = SHADER_STAGE(QVK_MOD_SKY_BUFFER_RESOLVE_COMP, VK_SHADER_STAGE_COMPUTE_BIT),
			.layout = pipeline_layout_resolve
		};

		_VK(vkCreateComputePipelines(qvk.device, 0, 1, &pipeline_info, 0, &pipeline_resolve));
	}

	skyNeedsUpdate = VK_TRUE;
	
	return VK_SUCCESS;
}

VkResult
vkpt_physical_sky_destroy_pipelines()
{
	vkDestroyPipeline(qvk.device, pipeline_physical_sky, NULL);
	pipeline_physical_sky = VK_NULL_HANDLE;

    vkDestroyPipeline(qvk.device, pipeline_physical_sky_space, NULL);
    pipeline_physical_sky_space = VK_NULL_HANDLE;

	vkDestroyPipeline(qvk.device, pipeline_resolve, NULL);
	pipeline_resolve = VK_NULL_HANDLE;

	return VK_SUCCESS;
}

#define BARRIER_COMPUTE(cmd_buf, img) \
	do { \
		VkImageSubresourceRange subresource_range = { \
			.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT, \
			.baseMipLevel   = 0, \
			.levelCount     = 1, \
			.baseArrayLayer = 0, \
			.layerCount     = 1 \
		}; \
		IMAGE_BARRIER(cmd_buf, \
				.image            = img, \
				.subresourceRange = subresource_range, \
				.srcAccessMask    = VK_ACCESS_SHADER_WRITE_BIT, \
				.dstAccessMask    = VK_ACCESS_SHADER_READ_BIT, \
				.oldLayout        = VK_IMAGE_LAYOUT_GENERAL, \
				.newLayout        = VK_IMAGE_LAYOUT_GENERAL, \
		); \
	} while(0)


static void
reset_sun_color_buffer(VkCommandBuffer cmd_buf)
{
	BUFFER_BARRIER(cmd_buf,
		.buffer = qvk.buf_sun_color.buffer,
		.offset = 0,
		.size = VK_WHOLE_SIZE,
		.srcAccessMask = VK_ACCESS_UNIFORM_READ_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT
	);

	vkCmdFillBuffer(cmd_buf, qvk.buf_sun_color.buffer,
		0, VK_WHOLE_SIZE, 0);

	BUFFER_BARRIER(cmd_buf,
		.buffer = qvk.buf_sun_color.buffer,
		.offset = 0,
		.size = VK_WHOLE_SIZE,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT
	);
}

bool vkpt_physical_sky_needs_update()
{
	return skyNeedsUpdate;
}

extern float terrain_shadowmap_viewproj[16];

VkResult
vkpt_physical_sky_record_cmd_buffer(VkCommandBuffer cmd_buf)
{
	if (!skyNeedsUpdate)
		return VK_SUCCESS;


	RecordCommandBufferShadowmap(cmd_buf);
	

	reset_sun_color_buffer(cmd_buf);


    {
        VkDescriptorSet desc_sets[] = {
            qvk.desc_set_ubo,
            qvk_get_current_desc_set_textures(),
            qvk.desc_set_vertex_buffer,
            SkyGetDescriptorSet()
        };

        if (physical_sky_space->integer > 0) {
            vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_physical_sky_space);
        } else {
            vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_physical_sky);
        }

		vkCmdPushConstants(cmd_buf, pipeline_layout_physical_sky, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(float) * 16, terrain_shadowmap_viewproj);

		vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE,
			pipeline_layout_physical_sky, 0, LENGTH(desc_sets), desc_sets, 0, 0);
		const int group_size = 16;
		vkCmdDispatch(cmd_buf, width / group_size, height / group_size, 6);
	}

	BARRIER_COMPUTE(cmd_buf, img_envmap);

	BUFFER_BARRIER(cmd_buf,
		.buffer = qvk.buf_sun_color.buffer,
		.offset = 0,
		.size = VK_WHOLE_SIZE,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT
	);

	{
		VkDescriptorSet desc_sets[] = {
			qvk.desc_set_ubo,
			qvk.desc_set_vertex_buffer,
		};

		vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_resolve);
		vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE,
			pipeline_layout_resolve, 0, LENGTH(desc_sets), desc_sets, 0, 0);
		
		vkCmdDispatch(cmd_buf, 1, 1, 1);
	}

	BUFFER_BARRIER(cmd_buf,
		.buffer = qvk.buf_sun_color.buffer,
		.offset = 0,
		.size = VK_WHOLE_SIZE,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT
	);

    skyNeedsUpdate = VK_FALSE;
	
    return VK_SUCCESS;
}

static void change_image_layouts(VkImage image, const VkImageSubresourceRange* subresource_range)
{
	VkCommandBuffer cmd_buf = vkpt_begin_command_buffer(&qvk.cmd_buffers_graphics);

	IMAGE_BARRIER(cmd_buf,
		.image = img_envmap,
		.subresourceRange = *subresource_range,
		.srcAccessMask = 0,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL,
	);

	vkpt_submit_command_buffer_simple(cmd_buf, qvk.queue_graphics, true);
	vkpt_wait_idle(qvk.queue_graphics, &qvk.cmd_buffers_graphics);
}

static void
process_gamepad_input(void)
{
	static uint32_t prev_milliseconds = 0;
	uint32_t curr_milliseconds = Sys_Milliseconds();
	if (!prev_milliseconds)
	{
		prev_milliseconds = curr_milliseconds;
		return;
	}

	uint32_t frame_time = curr_milliseconds - prev_milliseconds;
	prev_milliseconds = curr_milliseconds;

	if (frame_time > 1000)
		return;

	if (game_controller && sun_gamepad->integer && !((active_sun_preset() > SUN_PRESET_NONE) && (active_sun_preset() < SUN_PRESET_COUNT)) && !sun_animate->integer)
	{
		float dx = SDL_GameControllerGetAxis(game_controller, sun_gamepad->integer == 1 ? SDL_CONTROLLER_AXIS_LEFTX : SDL_CONTROLLER_AXIS_RIGHTX);
		float dy = SDL_GameControllerGetAxis(game_controller, sun_gamepad->integer == 1 ? SDL_CONTROLLER_AXIS_LEFTY : SDL_CONTROLLER_AXIS_RIGHTY);

		const float deadzone = 8000.f;
		const float limit = 32768.f - deadzone;
		dx = powf(max(fabsf(dx) - deadzone, 0.f) / limit, 2.f) * (dx < 0 ? -1.f : 1.f);
		dy = powf(max(fabsf(dy) - deadzone, 0.f) / limit, 2.f) * (dy < 0 ? -1.f : 1.f);

		dx *= (float)frame_time * 0.05f;
		dy *= (float)frame_time * 0.05f;

		if (dx != 0.f)
		{
			float azimuth = sun_azimuth->value;
			azimuth -= dx;
			while (azimuth > 360.f) azimuth -= 360.f;
			while (azimuth < 0.f) azimuth += 360.f;
			sun_azimuth->value = azimuth;
			sun_azimuth->changed(sun_azimuth);
		}

		if (dy != 0.f)
		{
			float elevation = sun_elevation->value;
			elevation -= dy;
			if (elevation > 90.f) elevation = 90.f;
			if (elevation < -90.f) elevation = -90.f;
			sun_elevation->value = elevation;
			sun_elevation->changed(sun_elevation);
		}
	}
}

void vkpt_next_sun_preset()
{
	int preset;

	if (sun_preset->integer < SUN_PRESET_NIGHT || sun_preset->integer > SUN_PRESET_DUSK)
		preset = SUN_PRESET_MORNING;
	else
	{
		preset = sun_preset->integer + 1;
		if (preset > SUN_PRESET_DUSK)
			preset = SUN_PRESET_NIGHT;
	}

	Cvar_SetByVar(sun_preset, va("%d", preset), FROM_CONSOLE);
}

void vkpt_physical_sky_set_map_skybox(int scope)
{
	if (map_skybox_scope != scope)
	{
		map_skybox_scope = scope;
		physical_sky_cvar_changed(NULL);
	}
}

// True when the environment cube map set by R_SetSky should be shown as-is,
// instead of the procedural sky. Either the active physical_sky preset asks for
// it, or the map ships its own skybox and sky_use_map_skybox allows using it.
// The space sky is excluded because physical_sky_space.comp already composites
// the environment map with the procedurally rendered planet.
bool vkpt_physical_sky_uses_skybox(void)
{
	// A map on sky_procedural_maps declines its own skybox, and
	// effective_physical_sky has already replaced the preset with Stroggos, so
	// neither test below can put the cube map back.
	if (map_skybox_scope > 0 && sky_use_map_skybox->integer >= map_skybox_scope
		&& !physical_sky_space->integer && !sky_map_forces_procedural())
		return true;

	return (GetSkyPreset(effective_physical_sky())->flags & PHYSICAL_SKY_FLAG_USE_SKYBOX) != 0;
}

/*
=================
sky_type_changed

Expands the menu's one choice into the two cvars the renderer actually reads.

boss2 needs no special case here, which is the whole reason this works: it
ships maps/boss2.cfg setting physical_sky_space, and vkpt_physical_sky_uses_skybox
already declines a map's own skybox whenever the space sky is what gets
composited - physical_sky_space.comp draws the environment map into the space
sky itself, so the two would fight. So "rerelease" gets the map skybox
everywhere it exists, and boss2 keeps the Q2RTX space sky.
=================
*/
static void sky_type_changed(cvar_t *self)
{
	switch (self->integer) {
	case SKY_TYPE_RERELEASE:
		// every map that ships a skybox uses it; physical_sky is left alone
		// and still backs the maps that do not. The space sky does not depend
		// on it - see the Stroggos override in vkpt_evaluate_sun_light.
		Cvar_SetInteger(sky_use_map_skybox, 1, FROM_CODE);
		break;

	case SKY_TYPE_ENVMAP:
	case SKY_TYPE_EARTH:
	case SKY_TYPE_STROGGOS:
		// an explicit pick wins everywhere, so a map's own skybox must not
		// override it
		Cvar_SetInteger(sky_use_map_skybox, 0, FROM_CODE);
		Cvar_SetInteger(physical_sky, self->integer - SKY_TYPE_ENVMAP, FROM_CODE);
		break;

	default:
		Com_WPrintf("sky_type %d is out of range, expected 0..3\n", self->integer);
		break;
	}
}

/*
=================
apply_map_sun

Seeds sun_elevation / sun_azimuth from sky_map_sun_* when a map on
sky_procedural_maps is loaded.

Fires once per map, on the first frame the new name is visible, and only while
the sun preset is Custom. So it sets a STARTING POINT, not an override: move the
sun afterwards and it stays where you put it, and pick any time-of-day preset
and this does not run at all - that preset is honoured exactly as on any other
map.

Keyed on the map name rather than on a load hook because that is the same thing
sky_map_in_list already tests, so the two cannot disagree about which map is
loaded.
=================
*/
static void apply_map_sun(void)
{
	static char last_map[MAX_QPATH];

	if (!strcmp(last_map, cl.mapname))
		return;

	Q_strlcpy(last_map, cl.mapname, sizeof(last_map));

	if (!cl.mapname[0] || !sky_map_forces_procedural())
		return;

	// SUN_PRESET_NONE is the menu's "Custom". Read the cvar directly rather than
	// active_sun_preset(), which substitutes a different preset in multiplayer.
	if (sun_preset->integer != 0)
		return;

	Cvar_SetValue(sun_elevation, sky_map_sun_elevation->value, FROM_CODE);
	Cvar_SetValue(sun_azimuth, sky_map_sun_azimuth->value, FROM_CODE);
}

// Publishes the answer above so the menu can grey out the controls that do not
// apply to the sky currently in use. It depends on map_skybox_scope, which is a
// property of the loaded map, so no combination of cvar values tells the UI
// this on its own. Written only on a change - this is called every frame.
static void publish_sky_state(void)
{
	int procedural = vkpt_physical_sky_uses_skybox() ? 0 : 1;

	if (sky_procedural_active->integer != procedural)
		Cvar_SetInteger(sky_procedural_active, procedural, FROM_CODE);
}

void
vkpt_evaluate_sun_light(sun_light_t* light, const vec3_t sky_matrix[3], float time)
{
	// Before anything reads sun_elevation / sun_azimuth, so a map that seeds them
	// is already seeded on its first rendered frame.
	apply_map_sun();

	static uint16_t skyIndex = -1;
	if (effective_physical_sky() != skyIndex)
	{   // update cvars with presets if the user changed the sky
		UpdatePhysicalSkyCVars();
		skyIndex = effective_physical_sky();
	}

	PhysicalSkyDesc_t const * skyDesc = GetSkyPreset(skyIndex);

	if (vkpt_physical_sky_uses_skybox())
	{
		// physical sky is disabled - no direct sun light in this mode
		memset(light, 0, sizeof(*light));
		return;
	}

	if (skyIndex != current_preset)
	{
		vkpt_queue_wait_idle(qvk.queue_graphics);
		SkyLoadScatterParameters(skyDesc->preset);
		current_preset = skyIndex;
	}

	process_gamepad_input();

	double azimuth, elevation;

	static float start_time = 0.0f, sun_animate_changed = 0.0f, sun_last_update = 0.0f, old_elevation = 0.0f, old_azimuth = 0.0f, static_time = 0.0f, last_time = 0.0f;
	static qboolean bLevelChangeTwice = qfalse;
	static qboolean bStaticTimeKicked = qfalse;

	if (sun_animate->value > 0) {
		if (last_time > time) {
			if (bLevelChangeTwice) {
				static_time += last_time;
			}
			sun_last_update = 0.0f;
			bLevelChangeTwice = qtrue;
		}

		last_time = time;

		if (!bStaticTimeKicked) {
			if (static_time > time) {
				time += static_time;
				bStaticTimeKicked = qtrue;
			}
			else {
				static_time = time;
			}
		}
		else {
			time += static_time;
		}
	}

	if (sun_animate->value != sun_animate_changed)
	{
		start_time = time;
		sun_animate_changed = sun_animate->value;
		sun_last_update = 0.0f;
	}

	const int preset = active_sun_preset();

	if ((preset == SUN_PRESET_CURRENT_TIME) || (preset == SUN_PRESET_FAST_TIME))
	{
		bool fast_time = (preset == SUN_PRESET_FAST_TIME);

		struct tm* local_time;

		if (fast_time)
			local_time = gmtime(&latched_local_time);
		else
			local_time = localtime(&latched_local_time);

		float time_of_day = local_time->tm_hour + local_time->tm_min / 60.f;
		if (fast_time)
			time_of_day *= 12.f;
		else if (local_time->tm_isdst)
			time_of_day -= 1.f;

		CalculateDirectionToSun(local_time->tm_yday, time_of_day, sun_latitude->value, light->direction_envmap);
	}
	else
	{
		switch (preset)
		{
		case SUN_PRESET_NIGHT:
			elevation = -90.f;
			azimuth = 0.f;
			break;

		case SUN_PRESET_DAWN:
			elevation = -3.f;
			azimuth = 0.f;
			break;

		case SUN_PRESET_MORNING:
			elevation = 25.f;
			azimuth = -15.f;
			break;

		case SUN_PRESET_NOON:
			elevation = 80.f;
			azimuth = -75.f;
			break;

		case SUN_PRESET_EVENING:
			elevation = 15.f;
			azimuth = 190.f;
			break;

		case SUN_PRESET_DUSK:
			elevation = -6.f;
			azimuth = 205.f;
			break;

		default:
			if (sun_animate->value > 0.0f)
			{
				float elapsed = (time - start_time) * 250.f * sun_animate->value;
				float usedElapsed;

				
				if((elapsed - sun_last_update > (1000 * sun_animate->value)) || sun_last_update == 0) {					
					azimuth = fmod(sun_azimuth->value + elapsed / (24.f * 60.f * 60.f), 360.0f);
					sun_last_update = elapsed;


					double e = fmod(sun_elevation->value + elapsed / (60.f * 60.f), 360.0f);
					if (e > 270.f)
						elevation = -(360.f - e);
					else if (e > 180.0f)
					{
						elevation = -(e - 180.0f);
						azimuth = fmod(azimuth + 180.f, 360.f);
					}
					else if (e > 90.0f)
					{
						elevation = 180.0f - e;
						azimuth = fmod(azimuth + 180.f, 360.f);
					}
					else
						elevation = e;
					elevation = max(-90, min(90, elevation));

					old_elevation = elevation;
					old_azimuth = azimuth;
					skyNeedsUpdate = VK_TRUE;
				}
				else {
					skyNeedsUpdate = VK_FALSE;
					elevation = old_elevation;
					azimuth = old_azimuth;
				}
			}
			else
			{
				azimuth = sun_azimuth->value;
				elevation = sun_elevation->value;
			}
			break;
		}

		float elevation_rad = elevation * M_PI / 180.0f; //max(-20.f, min(90.f, elevation)) * M_PI / 180.f;
		float azimuth_rad = azimuth * M_PI / 180.f;
		light->direction_envmap[0] = cosf(azimuth_rad) * cosf(elevation_rad);
		light->direction_envmap[1] = sinf(azimuth_rad) * cosf(elevation_rad);
		light->direction_envmap[2] = sinf(elevation_rad);
	}

	light->angular_size_rad = max(1.f, min(10.f, sun_angle->value)) * M_PI / 180.f;

	light->use_physical_sky = true;

	// color before occlusion
	vec3_t sunColor = { sun_color[0]->value, sun_color[1]->value, sun_color[2]->value };

	// The space sky is always lit by Stroggos' star. physical_sky picks which
	// procedural ATMOSPHERE gets rendered, and the space sky renders none - it
	// composites the environment map with a planet instead - so the preset's
	// sun would otherwise leak in and light the map with, say, Earth's near
	// white sun. boss2 is the map this shows up on, and it is authored around
	// the yellow Stroggos one.
	if (physical_sky_space->integer)
	{
		const PhysicalSkyDesc_t *space = GetSkyPreset(SKY_STROGGOS_PRESET);

		VectorCopy(space->sunColor, sunColor);
		light->angular_size_rad = max(1.f, min(10.f, space->sunAngularDiameter)) * M_PI / 180.f;
	}

	VectorScale(sunColor, sun_brightness->value, light->color);

	// potentially visible - can be overridden if readback data says it's occluded
	if (physical_sky_space->integer)
		light->visible = true;
	else
		light->visible = (light->direction_envmap[2] >= -sinf(light->angular_size_rad * 0.5f));

	vec3_t sun_direction_world = { 0.f };
	sun_direction_world[0] = light->direction_envmap[0] * sky_matrix[0][0] + light->direction_envmap[1] * sky_matrix[0][1] + light->direction_envmap[2] * sky_matrix[0][2];
	sun_direction_world[1] = light->direction_envmap[0] * sky_matrix[1][0] + light->direction_envmap[1] * sky_matrix[1][1] + light->direction_envmap[2] * sky_matrix[1][2];
	sun_direction_world[2] = light->direction_envmap[0] * sky_matrix[2][0] + light->direction_envmap[1] * sky_matrix[2][1] + light->direction_envmap[2] * sky_matrix[2][2];
	VectorCopy(sun_direction_world, light->direction);
}

VkResult
vkpt_physical_sky_update_ubo(QVKUniformBuffer_t * ubo, const sun_light_t* light, bool render_world)
{
    PhysicalSkyDesc_t const * skyDesc = GetSkyPreset(effective_physical_sky());

	publish_sky_state();

	if(physical_sky_space->integer)
		ubo->pt_env_scale = 0.3f;
	else if(vkpt_physical_sky_uses_skybox())
	{
		// The map ships its own skybox, so the procedural sky - and the EV
		// stops of physical_sky_brightness with it - is not being rendered at
		// all. Drive this from the video menu's own control instead.
		//
		// The base is what an authored skybox has always been given here:
		// physical_sky_brightness defaults to 0, so this branch used to
		// evaluate to exp2f(-2). Keeping that as the 1.0 point means the
		// default look does not move.
		ubo->pt_env_scale = 0.25f * max(0.f, pt_sky_light_scale->value);
	}
	else
	{
		const float min_brightness = -10.f;
		const float max_brightness = 2.f;

		float brightness = max(min_brightness, min(max_brightness, physical_sky_brightness->value));
		ubo->pt_env_scale = exp2f(brightness - 2.f);
	}

    // sun

    ubo->sun_bounce_scale = sun_bounce->value;
	ubo->sun_tan_half_angle = tanf(light->angular_size_rad * 0.5f);
	ubo->sun_cos_half_angle = cosf(light->angular_size_rad * 0.5f);
	ubo->sun_solid_angle = 2 * M_PI * (float)(1.0 - cos(light->angular_size_rad * 0.5)); // use double for precision
	//ubo->sun_solid_angle = max(ubo->sun_solid_angle, 1e-3f);

	VectorCopy(light->color, ubo->sun_color);
	VectorCopy(light->direction_envmap, ubo->sun_direction_envmap);
	VectorCopy(light->direction, ubo->sun_direction);

    if (light->direction[2] >= 0.99f)
    {
        VectorSet(ubo->sun_tangent, 1.f, 0.f, 0.f);
        VectorSet(ubo->sun_bitangent, 0.f, 1.f, 0.f);
    }
    else
    {
        vec3_t up;
        VectorSet(up, 0.f, 0.f, 1.f);
        CrossProduct(light->direction, up, ubo->sun_tangent);
        VectorNormalize(ubo->sun_tangent);
        CrossProduct(light->direction, ubo->sun_tangent, ubo->sun_bitangent);
        VectorNormalize(ubo->sun_bitangent);
    }
	// clouds

	ubo->sky_transmittance = sky_transmittance->value;
	ubo->sky_phase_g = sky_phase_g->value;
	ubo->sky_amb_phase_g = sky_amb_phase_g->value;
	ubo->sky_scattering = sky_scattering->value;

    // atmosphere

	if (!render_world)
		ubo->environment_type = ENVIRONMENT_NONE;
	else if (light->use_physical_sky)
		ubo->environment_type = ENVIRONMENT_DYNAMIC;
	else
		ubo->environment_type = ENVIRONMENT_STATIC;

    if (light->use_physical_sky)
    {
        uint32_t flags = skyDesc->flags;
        // adjust flags from cvars here

        if (physical_sky_draw_clouds->value > 0.0f)
            flags = flags | PHYSICAL_SKY_FLAG_DRAW_CLOUDS;
        else
            flags = flags & (~PHYSICAL_SKY_FLAG_DRAW_CLOUDS);

        ubo->physical_sky_flags = flags;

        // compute approximation of reflected radiance from ground
        vec3_t ground_radiance;
        VectorCopy(skyDesc->groundAlbedo, ground_radiance);
        VectorScale(ground_radiance, max(0.f, light->direction_envmap[2]), ground_radiance); // N.L
        VectorVectorScale(ground_radiance, light->color, ground_radiance);

		VectorCopy(ground_radiance, ubo->physical_sky_ground_radiance);
    }
	else
		skyNeedsUpdate = VK_FALSE;

    // planet

    ubo->planet_albedo_map = physical_sky_planet_albedo_map;
    ubo->planet_normal_map = physical_sky_planet_normal_map;

	ubo->sun_visible = light->visible;

	if (render_world && !vkpt_physical_sky_uses_skybox())
	{

		vec3_t forward;
		VectorScale(light->direction_envmap, -1.0f, forward);
		UpdateTerrainShadowMapView(forward);
		
	}

    return VK_SUCCESS;
}

//
// Sun & Sky presets
//

void physical_sky_cvar_changed(cvar_t *self)
{   // cvar callback to trigger a re-render of skybox
    skyNeedsUpdate = VK_TRUE;
    vkpt_reset_accumulation();
}

void InitialiseSkyCVars()
{
    static char _rgb[3] = {'r', 'g', 'b'};

    // sun
    for (int i = 0; i < 3; ++i)
    {
        char buff[32]; 
        snprintf(buff, 32, "sun_color_%c", _rgb[i]);
        sun_color[i] = Cvar_Get(buff, "1.0", 0);
        sun_color[i]->changed = physical_sky_cvar_changed;
    }

    sun_elevation = Cvar_Get("sun_elevation", "45", 0);
    sun_elevation->changed = physical_sky_cvar_changed;

    sun_azimuth = Cvar_Get("sun_azimuth", "345", 0); 
    sun_azimuth->changed = physical_sky_cvar_changed;

    sun_angle = Cvar_Get("sun_angle", "1.0", 0); 
    sun_angle->changed = physical_sky_cvar_changed;
	
    sun_brightness = Cvar_Get("sun_brightness", "10", 0); 
    sun_brightness->changed = physical_sky_cvar_changed;

	sky_scattering = Cvar_Get("sky_scattering", "5.0", 0);
	sky_scattering->changed = physical_sky_cvar_changed;

	sky_transmittance = Cvar_Get("sky_transmittance", "10.0", 0);
	sky_transmittance->changed = physical_sky_cvar_changed;

	sky_phase_g = Cvar_Get("sky_phase_g", "0.9", 0);
	sky_phase_g->changed = physical_sky_cvar_changed;

	sky_amb_phase_g = Cvar_Get("sky_amb_phase_g", "0.3", 0);
	sky_amb_phase_g->changed = physical_sky_cvar_changed;

    sun_bounce = Cvar_Get("sun_bounce", "1.0", 0); 
    sun_bounce->changed = physical_sky_cvar_changed;

    sun_animate = Cvar_Get("sun_animate", "0", 0); 
    sun_animate->changed = physical_sky_cvar_changed;

	sun_preset = Cvar_Get("sun_preset", va("%d", SUN_PRESET_MORNING), CVAR_ARCHIVE);
	sun_preset->changed = physical_sky_cvar_changed;

	sun_latitude = Cvar_Get("sun_latitude", "32.9", CVAR_ARCHIVE); // latitude of former HQ of id Software in Richardson, TX
	sun_latitude->changed = physical_sky_cvar_changed;

	sun_gamepad = Cvar_Get("sun_gamepad", "0", 0);

    // sky

    physical_sky = Cvar_Get("physical_sky", "2", 0);
    physical_sky->changed = physical_sky_cvar_changed;

    physical_sky_draw_clouds = Cvar_Get("physical_sky_draw_clouds", "1", 0);
    physical_sky_draw_clouds->changed = physical_sky_cvar_changed;

    physical_sky_space = Cvar_Get("physical_sky_space", "0", 0);
	physical_sky_space->changed = physical_sky_cvar_changed;

	physical_sky_brightness = Cvar_Get("physical_sky_brightness", "0", 0);
	physical_sky_brightness->changed = physical_sky_cvar_changed;

	// applied per frame in vkpt_physical_sky_update_ubo, so it needs no
	// physical_sky_cvar_changed - nothing has to be re-rendered for it
	pt_sky_light_scale = Cvar_Get("pt_sky_light_scale", "1", CVAR_ARCHIVE);

	// engine state, not a setting - CVAR_ROM keeps a config from writing it
	sky_procedural_active = Cvar_Get("sky_procedural_active", "1", CVAR_ROM);

	// 0 = always use the procedural sky, 1 = show the map's own skybox on
	// rerelease maps, 2 = show it in every game directory
	sky_use_map_skybox = Cvar_Get("sky_use_map_skybox", "1", CVAR_ARCHIVE);
	sky_use_map_skybox->changed = physical_sky_cvar_changed;

	// Registered after the two it drives, then applied once, so the three are
	// consistent no matter what order the config happened to set them in.
	sky_type = Cvar_Get("sky_type", "0", CVAR_ARCHIVE);
	sky_type->changed = sky_type_changed;
	sky_type_changed(sky_type);

	// Maps that get the procedural sky even in "rerelease" sky mode, and the sun
	// they start with. sky_procedural_maps needs physical_sky_cvar_changed
	// because it decides which atmosphere is rendered; the two angles do not,
	// since they only ever write sun_elevation / sun_azimuth, which carry their
	// own callbacks.
	sky_procedural_maps = Cvar_Get("sky_procedural_maps", "mgu1m1;mgu2m1", CVAR_ARCHIVE);
	sky_procedural_maps->changed = physical_sky_cvar_changed;

	sky_map_sun_elevation = Cvar_Get("sky_map_sun_elevation", "55", CVAR_ARCHIVE);
	sky_map_sun_azimuth = Cvar_Get("sky_map_sun_azimuth", "180", CVAR_ARCHIVE);
}

void UpdatePhysicalSkyCVars()
{
	PhysicalSkyDesc_t const * sky = GetSkyPreset(effective_physical_sky());

	// sun
	for (int i = 0; i < 3; ++i)
		Cvar_SetValue(sun_color[i], sky->sunColor[i], FROM_CODE);

	Cvar_SetValue(sun_angle, sky->sunAngularDiameter, FROM_CODE);

	skyNeedsUpdate = VK_TRUE;
}


//
// Sky presets
//

static PhysicalSkyDesc_t skyPresets[3] = {
    { 
		.flags = PHYSICAL_SKY_FLAG_USE_SKYBOX,
		.preset = SKY_NONE,
    },
    { 
		.sunColor = {1.45f, 1.29f, 1.27f},  // earth : G2 sun, red scatter, 30% ground albedo
		.sunAngularDiameter = 1.f,
		.groundAlbedo = {0.3f, 0.15f, 0.14f},
		.flags = PHYSICAL_SKY_FLAG_DRAW_MOUNTAINS,
		.preset = SKY_EARTH,
	},
    { 
		.sunColor = {0.315f, 0.137f, 0.033f},  // red sun
		.sunAngularDiameter = 5.f,
		.groundAlbedo = {0.133, 0.101, 0.047},
		.flags = PHYSICAL_SKY_FLAG_DRAW_MOUNTAINS,
		.preset = SKY_STROGGOS,
	},
};

PhysicalSkyDesc_t const * GetSkyPreset(uint16_t index)
{
    if (index >= 0 && index < q_countof(skyPresets))
        return &skyPresets[index];

    return &skyPresets[0];
}

static void rotationQuat(vec3_t const axis, float radians, vec4_t result)
{
    // Note: assumes axis is normalized
    float sinHalfTheta = sinf(0.5f * radians);
    float cosHalfTheta = cosf(0.5f * radians);
    result[0] = cosHalfTheta;
    result[1] = axis[0] * sinHalfTheta;
    result[2] = axis[1] * sinHalfTheta;
    result[3] = axis[2] * sinHalfTheta;
}

static float dotQuat(vec4_t const q0, vec4_t const q1)
{
    return q0[0]*q1[0] + q0[1]*q1[1] +  q0[2]*q1[2] +  q0[3]*q1[3];
}

static void normalizeQuat(vec4_t q)
{
    float l = sqrtf(dotQuat(q, q));
    for (int i = 0; i < 4; ++i)
        q[i] /= l;
}

static void conjugateQuat(vec4_t q)
{
    q[0] =  q[0];
    q[1] = -q[1];
    q[2] = -q[2];
    q[3] = -q[3];
}

static void inverseQuat(vec4_t q)
{
    float l2 = dotQuat(q, q);
    conjugateQuat(q);
    for (int i = 0; i < 4; ++i)
        q[i] /= l2;
}

static void quatMult(vec4_t const a, vec4_t const b, vec4_t result)
{
    result[0] = a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3];
    result[1] = a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2];
    result[2] = a[0] * b[2] + a[2] * b[0] + a[3] * b[1] - a[1] * b[3];
    result[3] = a[0] * b[3] + a[3] * b[0] + a[1] * b[2] - a[2] * b[1];
}

void CalculateDirectionToSun(float DayOfYear, float TimeOfDay, float LatitudeDegrees, vec3_t result)
{
    const float AxialTilt = 23.439f;
    const float DaysInYear = 365.25f;
    const float SpringEquinoxDay = 79.0f; // Mar 20

    float altitudeAtNoon = 90 - LatitudeDegrees + AxialTilt * sinf((DayOfYear - SpringEquinoxDay) / DaysInYear * 2.0f * M_PI);
    altitudeAtNoon = altitudeAtNoon * M_PI / 180.f;

    float altitudeOfEarthAxis = 180 - LatitudeDegrees;
    altitudeOfEarthAxis = altitudeOfEarthAxis * M_PI / 180.f;

    // X -> North
    // Y -> Zenith
    // Z -> East

    vec3_t noonVector = { cosf(altitudeAtNoon), sinf(altitudeAtNoon), 0.0f };
    vec3_t earthAxis = { cosf(altitudeOfEarthAxis), sinf(altitudeOfEarthAxis), 0.0f };

    float angleFromNoon = (TimeOfDay - 12.0f) / 24.0f * 2.f * M_PI;
 
    vec4_t dayRotation;
    rotationQuat(earthAxis, -angleFromNoon, dayRotation);
  
    vec4_t dayRotationInv = { dayRotation[0], dayRotation[1], dayRotation[2], dayRotation[3] };
    inverseQuat(dayRotationInv);


    // = normalize(dayRotationInv * makequat(0.f, noonVector) * dayRotation);
    vec4_t sunDirection = { 0.0f, 0.0f, 0.0f, 0.0f };
    vec4_t noonQuat = { 0.0f, noonVector[0], noonVector[1], noonVector[2] };
    quatMult(dayRotationInv, noonQuat, sunDirection);
    quatMult(sunDirection, dayRotation, sunDirection);
    normalizeQuat(sunDirection);

    result[0] = sunDirection[1];
    result[1] = sunDirection[3];
    result[2] = sunDirection[2];
}
