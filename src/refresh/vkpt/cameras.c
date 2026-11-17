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

#include "cameras.h"
#include "vkpt.h"
#include "common/math.h"    // bytedirs, DirToByte, SetupRotationMatrix

#include <string.h>

void
vkpt_cameras_load(bsp_mesh_t* wm, const char* map_name)
{
	wm->num_cameras = 0;

	char* filebuf = NULL;

    char filename[MAX_QPATH];
    Q_snprintf(filename, sizeof(filename), "maps/cameras/%s.txt", map_name);
	FS_LoadFile(filename, (void**)&filebuf);

	if (!filebuf)
	{
		Com_DPrintf("Couldn't read %s\n", filename);
		return;
	}

	char const * ptr = (char const *)filebuf;
	char linebuf[1024];

	while (sgets(linebuf, sizeof(linebuf), &ptr))
	{
		{ char* t = strchr(linebuf, '#'); if (t) *t = 0; }   // remove comments
		{ char* t = strchr(linebuf, '\n'); if (t) *t = 0; }  // remove newline

		vec3_t pos, dir;

		if (sscanf(linebuf, "(%f, %f, %f) (%f, %f, %f)", &pos[0], &pos[1], &pos[2], &dir[0], &dir[1], &dir[2]) == 6)
		{
			if (wm->num_cameras < MAX_CAMERAS)
			{
				VectorCopy(pos, wm->cameras[wm->num_cameras].pos);
				VectorCopy(dir, wm->cameras[wm->num_cameras].dir);
				wm->num_cameras++;
			}
			else
			{
				Com_WPrintf("Map has too many cameras (max: %i)\n", MAX_CAMERAS);
				break;
			}
		}
	}

	Z_Free(filebuf);
}

/*
==============================================================================

QUAKE III PORTALS

A Quake III misc_portal_surface sits within 64 units of a surface drawn with
a "portal" shader. With no target it makes that surface a mirror - material.c
already makes those shaders mirrors, and so does vkpt_cameras_q3_face for a
portal face no camera owns. With a target it names a misc_portal_camera, and
the surface is a window onto what that camera sees, aimed at the camera's own
target or along its angles, with a roll, and either turning, swinging a few
degrees, or still.

The path tracer has no second view to render, so the portal is a transform
instead: a ray that meets the surface goes on from the camera, turned as the
portal turns it. Quake III builds that transform in R_GetPortalOrientations
and applies it in R_MirrorPoint / R_MirrorVector, and the code below follows
it step by step, down to the 162 quantized directions its game sent the aim
in, so a portal looks where it did there.

==============================================================================
*/

typedef struct {
	char    classname[32];
	char    target[64];
	char    targetname[64];
	vec3_t  origin;
	vec3_t  angles;
	float   roll;
	int     spawnflags;
} q3ent_t;

// Quake III's PerpendicularVector (q_math.c): the unit vector in the plane of
// src nearest to the axis src has least of
static void q3_perpendicular(vec3_t dst, const vec3_t src)
{
	int     pos = 0;
	float   minelem = 1.0f;
	vec3_t  temp = { 0, 0, 0 };

	for (int i = 0; i < 3; i++) {
		if (fabsf(src[i]) < minelem) {
			pos = i;
			minelem = fabsf(src[i]);
		}
	}
	temp[pos] = 1.0f;

	float d = DotProduct(src, temp) / DotProduct(src, src);
	VectorMA(temp, -d, src, dst);
	VectorNormalize(dst);
}

// G_SetMovedir: the two magic "up" and "down" angles, else the forward vector
static void q3_movedir(const vec3_t angles, vec3_t dir)
{
	if (angles[0] == 0 && angles[1] == -1 && angles[2] == 0)
		VectorSet(dir, 0, 0, 1);
	else if (angles[0] == 0 && angles[1] == -2 && angles[2] == 0)
		VectorSet(dir, 0, 0, -1);
	else
		AngleVectors(angles, dir, NULL, NULL);
}

static bool q3_parse_entity(const char **data, q3ent_t *e)
{
	const char *tok;
	char key[64];

	memset(e, 0, sizeof(*e));

	tok = COM_Parse(data);
	if (!*data || strcmp(tok, "{"))
		return false;

	while (1) {
		tok = COM_Parse(data);
		if (!*data)
			return false;
		if (!strcmp(tok, "}"))
			return true;
		Q_strlcpy(key, tok, sizeof(key));
		tok = COM_Parse(data);
		if (!*data)
			return false;

		if (!strcmp(key, "classname"))
			Q_strlcpy(e->classname, tok, sizeof(e->classname));
		else if (!strcmp(key, "target"))
			Q_strlcpy(e->target, tok, sizeof(e->target));
		else if (!strcmp(key, "targetname"))
			Q_strlcpy(e->targetname, tok, sizeof(e->targetname));
		else if (!strcmp(key, "origin"))
			sscanf(tok, "%f %f %f", &e->origin[0], &e->origin[1], &e->origin[2]);
		else if (!strcmp(key, "angles"))
			sscanf(tok, "%f %f %f", &e->angles[0], &e->angles[1], &e->angles[2]);
		else if (!strcmp(key, "angle"))
			e->angles[1] = (float)atof(tok);
		else if (!strcmp(key, "roll"))
			e->roll = (float)atof(tok);
		else if (!strcmp(key, "spawnflags"))
			e->spawnflags = atoi(tok);
	}
}

static const q3ent_t *q3_find(const q3ent_t *ents, int count, const char *targetname, const char *classname)
{
	if (!targetname[0])
		return NULL;
	for (int i = 0; i < count; i++) {
		if (strcmp(ents[i].targetname, targetname))
			continue;
		if (classname && strcmp(ents[i].classname, classname))
			continue;
		return &ents[i];
	}
	return NULL;
}

void vkpt_cameras_load_q3(bsp_mesh_t* wm, const bsp_t* bsp)
{
	const char  *data;
	q3ent_t     e, *ents;
	int         count = 0, max = 0;

	if (!bsp->q3 || !bsp->entitystring)
		return;

	for (data = bsp->entitystring; *data; data++)
		if (*data == '{')
			max++;
	if (!max)
		return;

	ents = Z_Malloc(sizeof(q3ent_t) * max);
	data = bsp->entitystring;
	while (count < max && q3_parse_entity(&data, &e))
		ents[count++] = e;

	for (int i = 0; i < count; i++) {
		const q3ent_t *surf = &ents[i], *cam, *aim;
		vec3_t dir;

		if (strcmp(surf->classname, "misc_portal_surface") || !surf->target[0])
			continue;     // a mirror

		cam = q3_find(ents, count, surf->target, "misc_portal_camera");
		if (!cam) {
			Com_WPrintf("misc_portal_surface at %s: no misc_portal_camera \"%s\"\n",
				vtos(surf->origin), surf->target);
			continue;
		}
		if (wm->num_cameras >= MAX_CAMERAS) {
			Com_WPrintf("Map has too many cameras and portals (max: %i)\n", MAX_CAMERAS);
			break;
		}

		// locateCamera (g_misc.c): at the camera's target, or along its angles
		aim = q3_find(ents, count, cam->target, NULL);
		if (aim) {
			VectorSubtract(aim->origin, cam->origin, dir);
			VectorNormalize(dir);
		} else {
			q3_movedir(cam->angles, dir);
		}
		// sent as a byte (DirToByte), so one of the 162 directions
		VectorCopy(bytedirs[DirToByte(dir)], dir);

		int n = wm->num_cameras++;
		memset(&wm->cameras[n], 0, sizeof(wm->cameras[n]));
		wm->cameras[n].portal = true;
		VectorCopy(surf->origin, wm->cameras[n].ent_origin);
		VectorCopy(cam->origin, wm->cameras[n].cam_origin);
		VectorCopy(cam->origin, wm->cameras[n].pos);
		VectorCopy(dir, wm->cameras[n].dir);

		// CG_Portal's axis, then R_GetPortalOrientations' flips
		vec3_t *axis = wm->cameras[n].cam_axis;
		VectorCopy(dir, axis[0]);
		q3_perpendicular(axis[1], axis[0]);
		VectorInverse(axis[1]);
		CrossProduct(axis[0], axis[1], axis[2]);
		VectorInverse(axis[0]);
		VectorInverse(axis[1]);

		// the roll goes as a byte, 256 to the turn; spawnflags 1 and 2 turn it
		// slowly or quickly, 4 holds it still, else it swings round the roll
		wm->cameras[n].roll = (float)(int)(cam->roll / 360.0f * 256.0f) / 256.0f * 360.0f;
		wm->cameras[n].rotate_speed = (cam->spawnflags & 1) ? 25.0f : (cam->spawnflags & 2) ? 75.0f : 0.0f;
		wm->cameras[n].swing = !(cam->spawnflags & 4);
	}

	Z_Free(ents);
}

int vkpt_cameras_q3_face(bsp_mesh_t* wm, const mface_t* surf)
{
	vec3_t  normal;
	float   dist, best = 65.0f;
	int     found = -1;

	if (!surf->plane)
		return -1;

	VectorCopy(surf->plane->normal, normal);
	dist = surf->plane->dist;
	if (surf->drawflags & DSURF_PLANEBACK) {
		VectorInverse(normal);
		dist = -dist;
	}

	for (int i = 0; i < wm->num_cameras; i++) {
		if (!wm->cameras[i].portal)
			continue;
		float d = fabsf(DotProduct(wm->cameras[i].ent_origin, normal) - dist);
		if (d <= 64.0f && d < best) {
			best = d;
			found = i;
		}
	}
	if (found < 0)
		return -1;

	// the surface's frame, from the first of its faces
	if (!wm->cameras[found].placed) {
		vec3_t *axis = wm->cameras[found].surf_axis;
		float d = DotProduct(wm->cameras[found].ent_origin, normal) - dist;

		VectorCopy(normal, axis[0]);
		q3_perpendicular(axis[1], axis[0]);
		CrossProduct(axis[0], axis[1], axis[2]);
		VectorMA(wm->cameras[found].ent_origin, -d, axis[0], wm->cameras[found].surf_origin);
		wm->cameras[found].placed = true;
	}

	return found;
}

void vkpt_cameras_q3_transform(const bsp_mesh_t* wm, int index, float time, float data[16])
{
	vec3_t  cam[3], rotated, m[3];
	float   r[3][3], t[3];
	float   d = 0;
	bool    turn = false;

	memcpy(cam, wm->cameras[index].cam_axis, sizeof(cam));

	// R_GetPortalOrientations' optional rotation about the view axis
	if (wm->cameras[index].swing) {
		if (wm->cameras[index].rotate_speed)
			d = time * wm->cameras[index].rotate_speed;
		else
			d = wm->cameras[index].roll + sinf(time * 1000.0f * 0.003f) * 4.0f;
		turn = true;
	} else if (wm->cameras[index].roll) {
		d = wm->cameras[index].roll;
		turn = true;
	}
	if (turn) {
		SetupRotationMatrix(m, cam[0], d);
		rotated[0] = DotProduct(m[0], cam[1]);
		rotated[1] = DotProduct(m[1], cam[1]);
		rotated[2] = DotProduct(m[2], cam[1]);
		VectorCopy(rotated, cam[1]);
		CrossProduct(cam[0], cam[1], cam[2]);
	}

	// out = sum_i dot(in - surf_origin, surf_axis[i]) * cam[i] + cam_origin
	const vec3_t *s = wm->cameras[index].surf_axis;
	for (int row = 0; row < 3; row++)
		for (int col = 0; col < 3; col++)
			r[row][col] = cam[0][row] * s[0][col] + cam[1][row] * s[1][col] + cam[2][row] * s[2][col];
	for (int row = 0; row < 3; row++)
		t[row] = wm->cameras[index].cam_origin[row] - DotProduct(r[row], wm->cameras[index].surf_origin);

	// path_tracer_rgen.h, is_portal_camera: rows in columns 1..3, -1 in column 0's w
	memset(data, 0, sizeof(float) * 16);
	VectorCopy(wm->cameras[index].cam_origin, data);
	data[3] = -1.0f;
	for (int row = 0; row < 3; row++) {
		VectorCopy(r[row], data + 4 * (row + 1));
		data[4 * (row + 1) + 3] = t[row];
	}
}

static void Camera_Cmd_f(void)
{
	vec3_t forward;
	AngleVectors(vkpt_refdef.fd->viewangles, forward, NULL, NULL);

	Com_Printf("(%f, %f, %f) (%f, %f, %f)\n", vkpt_refdef.fd->vieworg[0], vkpt_refdef.fd->vieworg[1], vkpt_refdef.fd->vieworg[2], forward[0], forward[1], forward[2]);
}

static const cmdreg_t cmds[] = {
	{ "camera", &Camera_Cmd_f, NULL },
	{ NULL, NULL, NULL }
};

void vkpt_cameras_init(void)
{
	Cmd_Register(cmds);
}

void vkpt_cameras_shutdown(void)
{
	Cmd_RemoveCommand("camera");
}
