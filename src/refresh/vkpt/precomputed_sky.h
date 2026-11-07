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

#ifndef  __PRECOMPUTED_SKY_H__
#define  __PRECOMPUTED_SKY_H__

static const uint32_t	ShadowmapSize = 2048;		// Heightmap dimension vertical and horizontal
static const uint32_t	ShadowmapHeightmapSize = 4096;		// Heightmap dimension vertical and horizontal
static const uint32_t	ShadowmapGridSize = 1024;	// Size of the polygonal grid, in squares. Squares made of two triangles
static const float		ShadowmapWorldSize = 30.0f;	// World size of the grid
static const float		ShadowmapWorldElevation = 3.0f;	// Heightmap world height coefficient

typedef enum
{
	SKY_NONE,
	SKY_EARTH,
	SKY_STROGGOS
} SkyPreset;

// struct AtmosphereParameters, Params_Earth and Params_Stroggos
#include "precomputed_sky_params.h"


VkResult SkyInitializeDataGPU(void);
void SkyReleaseDataGPU(void);
void UpdateTerrainShadowMapView(vec3_t forward);
VkResult SkyLoadScatterParameters(SkyPreset preset);

// API-free halves, shared with the D3D12 renderer (precomputed_sky.c says more)
const struct AtmosphereParameters *SkySelectPreset(SkyPreset preset, const char **planet);
uint32_t SkyBuildTerrainGrid(const char* FileName, unsigned int SideSize, float size_km,
	float** out_vertices, uint32_t** out_indices);
extern float terrain_shadowmap_viewproj[16];

VkDescriptorSetLayout* SkyGetDescriptorLayout(void);
VkDescriptorSet SkyGetDescriptorSet(void);

void RecordCommandBufferShadowmap(VkCommandBuffer cmd_buf);

void InitializeShadowmapResources(void);
void ReleaseShadowmapResources(void);
#endif  /*__PRECOMPUTED_SKY_H__*/
