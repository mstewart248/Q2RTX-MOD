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

#include "fog.h"
#include "vkpt.h"

#include <string.h>

/*
This file implements host-side control of the local fog volumes. The volumes
are manipulated using the "fog" console command and are reset before loading
a new map. Typically, the volumes should be defined using a map-specific config file.

Up to MAX_FOG_VOLUMES volumes can be defined, and up to 2 volumes are supported
by the renderer per ray. Use volumes with caution to avoid placing more than
two in the same area.

A fog volume is an axis-aligned box filled with uniform or gradient fog.
The volume is defined using two points on any diagonal of said box, a and b.
The fog parameters: color, density and gradient, are defined independently.
See the fog command help message for the options list.


Suggested workflow for defining a fog volume without access to a map editor
is to place the camera into the diagonal points, capturing each point:

    (place the camera in the first point)
    > fog -v <index> -a here
    (move the camera)
    > fog -v <index> -b here

Then set the color and density as necessary, and use "fog -v <index> -p"
to print all the volume parameters. The output of that commnad can be copied
from the console directly into a map config file.


On the renderer side, the fog volumes are processed for the primary rays and for
the reflection, refraction, and camera rays. The implementation starts in
trace_effects_ray(...) - see path_tracer_rgen.h. That function first finds the
volumes that intersect with the ray, and selects the two closest volumes.
These two volumes are passed through the ray payload to the any-hit shaders
that deal with transparent effects such as explosions. These any-hit shaders
will accumulate the transparent effects along the ray and any fog that is
between these effects. After that's done, the fog between the ray origin and
the first effect is blended over the effects, and the fog between the last
effect and the end of the ray is blended under the effects.
*/

fog_volume_t fog_volumes[MAX_FOG_VOLUMES];


static const cmd_option_t o_fog[] = {
	{ "v:int", "volume", "volume index" },
	{ "p", "print", "print the selected volume to the console" },
	{ "r", "reset", "reset the selected volume" },
	{ "R", "reset-all", "reset all volumes" },
	{ "a:x,y,z", "", "first point on the volume diagonal, or 'here'" },
	{ "b:x,y,z", "", "second point on the volume diagonal, or 'here'" },
	{ "c:r,g,b", "color", "fog color" },
	{ "d:float", "distance", "distance at which objects in the fog are 50% visible" },
	{ "f:face", "softface", "face where the density is zero: none, xa, xb, ya, yb, za, zb" },
	{ "h", "help", "display this message" },
	{ NULL }
};

static const char* o_softface[] = {
	"none", "xa", "xb", "ya", "yb", "za", "zb"
};


static void Fog_Cmd_c(genctx_t* ctx, int argnum)
{
	Cmd_Option_c(o_fog, NULL, ctx, argnum);
}

static void Fog_Cmd_f(void)
{
	fog_volume_t* volume = NULL;
	float x, y, z;
	int index = -1;
	int c, i;
	while ((c = Cmd_ParseOptions(o_fog)) != -1) {
		switch(c)
		{
		case 'h':
			Cmd_PrintUsage(o_fog, NULL);
			Com_Printf("Set parameters of a fog volume.\n");
			Cmd_PrintHelp(o_fog);
			return;
		case 'v':
			if (1 != sscanf(cmd_optarg, "%d", &index) || index < 0 || index >= MAX_FOG_VOLUMES) {
				Com_WPrintf("invalid volume index '%s'\n", cmd_optarg);
				return;
			}
			volume = fog_volumes + index;
			break;
		case 'p':
			if (!volume) goto no_volume;
			Com_Printf("fog -v %d ", index);
			Com_Printf("-a %.2f,%.2f,%.2f ", volume->point_a[0], volume->point_a[1], volume->point_a[2]);
			Com_Printf("-b %.2f,%.2f,%.2f ", volume->point_b[0], volume->point_b[1], volume->point_b[2]);
			Com_Printf("-c %.2f,%.2f,%.2f ", volume->color[0], volume->color[1], volume->color[2]);
			Com_Printf("-d %.0f ", volume->half_extinction_distance);
			Com_Printf("-f %s\n", o_softface[volume->softface]);
			break;
		case 'r':
			if (!volume) goto no_volume;
			memset(volume, 0, sizeof(*volume));
			break;
		case 'R':
			memset(fog_volumes, 0, sizeof(fog_volumes));
			break;
		case 'a':
			if (!volume) goto no_volume;
			if (3 != sscanf(cmd_optarg, "%f,%f,%f", &x, &y, &z)) {
				if (strcmp(cmd_optarg, "here") == 0) {
					VectorCopy(vkpt_refdef.fd->vieworg, volume->point_a);
					continue;
				}

				Com_WPrintf("invalid coordinates '%s'\n", cmd_optarg);
				return;
			}
			volume->point_a[0] = x;
			volume->point_a[1] = y;
			volume->point_a[2] = z;
			break;
		case 'b':
			if (!volume) goto no_volume;
			if (3 != sscanf(cmd_optarg, "%f,%f,%f", &x, &y, &z)) {
				if (strcmp(cmd_optarg, "here") == 0) {
					VectorCopy(vkpt_refdef.fd->vieworg, volume->point_b);
					continue;
				}

				Com_WPrintf("invalid coordinates '%s'\n", cmd_optarg);
				return;
			}
			volume->point_b[0] = x;
			volume->point_b[1] = y;
			volume->point_b[2] = z;
			break;
		case 'c':
			if (!volume) goto no_volume;
			if (3 != sscanf(cmd_optarg, "%f,%f,%f", &x, &y, &z)) {
				Com_WPrintf("invalid color '%s'\n", cmd_optarg);
				return;
			}
			volume->color[0] = x;
			volume->color[1] = y;
			volume->color[2] = z;
			break;
		case 'd':
			if (!volume) goto no_volume;
			if (1 != sscanf(cmd_optarg, "%f", &x)) {
				Com_WPrintf("invalid distance '%s'\n", cmd_optarg);
				return;
			}
			volume->half_extinction_distance = x;
			break;
		case 'f':
			if (!volume) goto no_volume;
			for (i = 0; i < (int)q_countof(o_softface); i++) {
				if (strcmp(cmd_optarg, o_softface[i]) == 0) {
					volume->softface = i;
					break;
				}
			}
			if (i >= (int)q_countof(o_softface)) {
				Com_WPrintf("invalid value for softface '%s'\n", cmd_optarg);
				return;
			}
			break;
		default:
			return;
		}
	}
	return;
	
no_volume:
	Com_WPrintf("volume not specified\n");
}

void vkpt_fog_init(void)
{
	vkpt_fog_reset();
	
	cmdreg_t cmds[] = {
		{ "fog", &Fog_Cmd_f, &Fog_Cmd_c },
		{ NULL, NULL, NULL }
	};
	Cmd_Register(cmds);
}

void vkpt_fog_shutdown(void)
{
	Cmd_RemoveCommand("fog");
}

void vkpt_fog_reset(void)
{
	memset(fog_volumes, 0, sizeof(fog_volumes));
}

// The loaded map's Quake III fog brushes - a copy, since the bsp_t can go first.
static mfogvolume_t q3_volumes[MAX_FOG_VOLUMES];
static int num_q3_volumes = 0;
static bool world_is_q3 = false;

void vkpt_fog_set_world(const bsp_t* bsp)
{
	num_q3_volumes = 0;
	world_is_q3 = bsp && bsp->q3;
	if (!bsp)
		return;
	for (int i = 0; i < bsp->numfogvolumes && num_q3_volumes < MAX_FOG_VOLUMES; i++)
		q3_volumes[num_q3_volumes++] = bsp->fogvolumes[i];
	if (bsp->numfogvolumes > MAX_FOG_VOLUMES)
		Com_WPrintf("fog: the map has %d fog volumes, only %d are drawn\n", bsp->numfogvolumes, MAX_FOG_VOLUMES);
	else if (num_q3_volumes)
		Com_DPrintf("fog: %d Quake III fog volume(s)\n", num_q3_volumes);
}

bool vkpt_fog_world_is_q3(void)
{
	return world_is_q3;
}

void vkpt_fog_upload(ShaderFogVolume* dst)
{
	ShaderFogVolume* first = dst;
	memset(dst, 0, sizeof(ShaderFogVolume) * MAX_FOG_VOLUMES);
	
	for (int i = 0; i < MAX_FOG_VOLUMES; i++)
	{
		const fog_volume_t* src = fog_volumes + i;
		if (src->half_extinction_distance <= 0.f || src->point_a[0] == src->point_b[0] || src->point_a[1] == src->point_b[1] || src->point_a[2] == src->point_b[2])
			continue;

		VectorCopy(src->color, dst->color);

		// Find the min and max bounds to support specifying two points on any diagonal of the volume
		for (int axis = 0; axis < 3; axis++)
		{
			dst->mins[axis] = min(src->point_a[axis], src->point_b[axis]);
			dst->maxs[axis] = max(src->point_a[axis], src->point_b[axis]);
		}

		// Convert the half-extinction distance into density
		// exp(-kx) = 0.5   =>   k = -ln(0.5) / x
		float density = 0.69315f / src->half_extinction_distance;

		if (1 <= src->softface && src->softface <= 6)
		{
			// Find the axis along which the density gradient is happening: x, y or z
			int axis = (src->softface - 1) / 2;

			// Find the positions on that axis where the density multiplier is 0 (pos0) and 1 (pos1)
			float pos0 = (src->softface & 1) ? src->point_a[axis] : src->point_b[axis];
			float pos1 = (src->softface & 1) ? src->point_b[axis] : src->point_a[axis];

			// Derive the linear function of the form (ax + b) that describes the density along the axis
			float a = density / (pos1 - pos0);
			float b = -pos0 * a;

			// Convert the 1D linear funciton into a volumetric one
			dst->density[axis] = a;
			dst->density[3] = b;
		}
		else
		{
			// No density gradient, just store the density with 0 spatial coefficinents
			Vector4Set(dst->density, 0.f, 0.f, 0.f, density);
		}
		
		dst->is_active = 1;

		++dst;
	}

	/* THEN THE QUAKE III MAP'S FOG BRUSHES, in whatever slots the command left.

	   These are not the boxes above: is_active 2 keeps them out of the path
	   tracer's tint-the-ray fog (find_fog_volumes stops at the first slot that is
	   not 1). The volumetric medium reads them instead (fog_medium.glsl,
	   q3_fog_at): real fog that the map's lights scatter in, in the colour of the
	   shader's fogparms, and that hides what is behind it.

	   fogparms gives the distance at which Quake III draws the fog fully opaque.
	   Read here as 95% gone there: extinction 3 / distance, per unit - times
	   pt_fog_q3_thickness. Quake III's own curve is sqrt(depth / distance), which
	   is thicker than any exponential close to the eye: 1.5 matches it over the
	   first sixteenth of the distance and is a little thicker past a quarter. */
	static cvar_t* cvar_pt_fog_q3_thickness = NULL;
	if (!cvar_pt_fog_q3_thickness)
		cvar_pt_fog_q3_thickness = Cvar_Get("pt_fog_q3_thickness", "1.5", CVAR_ARCHIVE);
	const float q3_thickness = max(cvar_pt_fog_q3_thickness->value, 0.f);

	/* pt_fog_q3_fade: the top of each brush fades in over this many units, from
	   none at the top face to full below - see fog_q3.glsl. 72 on q3dm4's
	   128-unit pit is full fog at a player's height off its floor (Matt,
	   2026-11-17). 0 = the old hard top. Rides in density.x, which only the
	   command's boxes (is_active 1) use for their gradient. */
	static cvar_t* cvar_pt_fog_q3_fade = NULL;
	if (!cvar_pt_fog_q3_fade)
		cvar_pt_fog_q3_fade = Cvar_Get("pt_fog_q3_fade", "72", CVAR_ARCHIVE);
	const float q3_fade = max(cvar_pt_fog_q3_fade->value, 0.f);

	for (int i = 0; i < num_q3_volumes; i++)
	{
		if (dst - first >= MAX_FOG_VOLUMES)
		{
			static bool warned = false;
			if (!warned)
				Com_WPrintf("fog: no room for %d of the map's fog volumes beside the \"fog\" command's\n", num_q3_volumes - i);
			warned = true;
			break;
		}
		const mfogvolume_t* v = &q3_volumes[i];
		VectorCopy(v->mins, dst->mins);
		VectorCopy(v->maxs, dst->maxs);
		VectorCopy(v->color, dst->color);
		Vector4Set(dst->density, q3_fade, 0.f, 0.f, q3_thickness * 3.f / max(v->distance, 1.f));
		dst->is_active = 2;
		++dst;
	}
}
