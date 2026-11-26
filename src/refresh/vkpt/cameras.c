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
	char    model[16];          // a brush entity's "*N" (Quake 1's teleporters)
	vec3_t  origin;
	bool    has_origin;         // Quake 1: what a teleporter can send you to
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
			e->has_origin = sscanf(tok, "%f %f %f", &e->origin[0], &e->origin[1], &e->origin[2]) == 3;
		else if (!strcmp(key, "angles") || !strcmp(key, "mangle"))
			sscanf(tok, "%f %f %f", &e->angles[0], &e->angles[1], &e->angles[2]);
		else if (!strcmp(key, "model"))
			Q_strlcpy(e->model, tok, sizeof(e->model));
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

/*
==============================================================================

QUAKE 1 TELEPORTERS

A Quake 1 teleporter is a trigger_teleport brush, targeting where it puts you
(an info_teleport_destination, nearly always - Quake's find() takes any
entity of that name), with a gate of *teleport faces (mg1's: *tele01) in or
at its box. The gate is the world's or a drawn brush entity's - Scourge of
Armagon and Dissolution of Eternity build most of theirs as func_walls, a few
maps as doors or func_bobs - never the trigger's own faces, which Quake does
not draw. Walk in and the game puts you at the destination facing its
"angle" (g_q1.c). So the gate is drawn as a window onto there: a portal, as
Quake III's are, with no texture of its own and no glow, and a waver over it
like Quake's water.

Which trigger a gate face is (q1_gate_owner): a trigger's box is often much
smaller than its gate - id's prefab puts a 14-unit box at the foot of a
96-unit gate, e2m7 a small box in front of a 144-unit window - so each
standing gate face goes to the trigger within Q1_GATE_REACH of it that scores
least on
  the gap between the face's box and the trigger's,
  + a tenth of a teleporter's diagonal (end's t63, a 2300-unit box fired once
    to clear the level, holds the back of Shub's gate inside it),
  + Q1_GATE_BEHIND when the trigger stands behind the face (gates back to
    back, ctf2: each side is the trigger on that side),
  + Q1_GATE_INHIBITED for a trigger Quake does not spawn in this mode and
    skill: one that is spawned wins, but hipdm1's gates, all deathmatch-only,
    still show where they go in single player.
trigger_changelevel competes on its gap alone, so a slipgate to another map
keeps its texture - there is nothing here to show - even with a teleporter
beside or inside it (e2m3, e3m4: monster catchers landing a few units away;
the player gets the level change). Only a teleporter that some face goes to
takes one of the MAX_CAMERAS slots (the material holds it in 4 bits): handed
out in entity order, hip2m2's 18 invisible monster-spawn teleporters took all
16 before its two gates. No map needs more than 15 this way (all 122 Quake 1
maps with gates, single player and deathmatch).

The turn is the shader's, from the face the ray meets (reflect_refract.rgen):
"into the gate" becomes the way the destination faces, whichever side of the
gate the eye is on - the game turns you that way from either side too. The
gate's floor (its trigger box's bottom) is the destination's: Quake puts the
player's origin 27 above the marker and the feet 24 below the origin, so the
floor there is the marker's height plus 3. A point on the gate is the same
point across the destination's view.

pt_q1_teleport_portals 0 (map load) keeps the textures; pt_q1_teleport_warp is
the waver, 0 none, 1 Quake's; pt_q1_teleport_clearance (map load, 8) raises a
gate whose bottom edge would look out from under the destination's floor
(q1_gate_shapes).

==============================================================================
*/

static cvar_t *cvar_pt_q1_teleport_portals;
static cvar_t *cvar_pt_q1_teleport_warp;
static cvar_t *cvar_pt_q1_teleport_clearance;

#define Q1_GATE_REACH       72.0f       // the farthest a gate face stands from its trigger
#define Q1_GATE_BEHIND      4.0f        // the trigger is behind the face: the other side's
#define Q1_GATE_INHIBITED   1000.0f     // a trigger Quake does not spawn in this mode and skill

/* The camera slot each face of the map is a teleporter's gate for, -1 none:
   decided once per map by vkpt_cameras_load_q1, read through
   vkpt_cameras_q1_face. */
static signed char      *q1_face_gate;
static const mface_t    *q1_faces;
static int              q1_num_faces;

static void q1_gates_clear(void)
{
	Z_Free(q1_face_gate);
	q1_face_gate = NULL;
	q1_faces = NULL;
	q1_num_faces = 0;
}

typedef struct {
	const q3ent_t   *ent;
	const q3ent_t   *dest;          // where a teleporter puts you; NULL a changelevel, or nowhere
	vec3_t          mins, maxs;     // its box (a unit wider all round, bsp_q1.c)
	float           cost;           // what q1_gate_owner adds for the trigger itself
	int             model;
	int             slot;           // its camera, -1 none yet
} q1_trigger_t;

// Quake's spawn filter (ED_LoadFromFile; g_q1vm.c QC_LoadEntities)
static bool q1_inhibited(int spawnflags)
{
	if (Cvar_VariableValue("deathmatch"))
		return (spawnflags & 2048) != 0;
	int skill = Cvar_VariableInteger("skill");
	clamp(skill, 0, 3);
	return (spawnflags & (skill == 0 ? 256 : skill == 1 ? 512 : 1024)) != 0;
}

/* Where a teleporter puts you: Quake's find(), the first entity of that name,
   though one spawned in this mode first, then an info_teleport_destination.
   Not an info_null, which removes itself on spawn - e4m8's window targets one
   and never worked, so it keeps its texture. */
static const q3ent_t *q1_dest(const q3ent_t *ents, int count, const char *target)
{
	const q3ent_t *best = NULL;
	int best_rank = 4;

	if (!target[0])
		return NULL;
	for (int i = 0; i < count; i++) {
		if (!ents[i].has_origin || strcmp(ents[i].targetname, target) || !strcmp(ents[i].classname, "info_null"))
			continue;
		int rank = (q1_inhibited(ents[i].spawnflags) ? 2 : 0) + (strcmp(ents[i].classname, "info_teleport_destination") ? 1 : 0);
		if (rank < best_rank) {
			best = &ents[i];
			best_rank = rank;
		}
	}
	return best;
}

// a gate's face: a standing #teleport or #tele01 (a floor or ceiling of it has
// no way through to show)
static bool q1_gate_texture(const mface_t *surf)
{
	if (!surf->texinfo || !surf->plane || surf->numsurfedges < 3)
		return false;
	const char *base = strrchr(surf->texinfo->name, '/');
	base = base ? base + 1 : surf->texinfo->name;
	if (base[0] != '#' || !Q_strHas(base, "tele"))
		return false;
	return fabsf(surf->plane->normal[2]) <= 0.7f;
}

// the trigger a gate face is (the comment above), -1 none within reach
static int q1_gate_owner(const mface_t *surf, const q1_trigger_t *triggers, int num_triggers)
{
	vec3_t  mins, maxs, center = { 0, 0, 0 }, normal;
	int     best = -1;
	float   best_score = 0;

	ClearBounds(mins, maxs);
	for (int i = 0; i < surf->numsurfedges; i++) {
		const float *v = surf->firstsurfedge[i].edge->v[surf->firstsurfedge[i].vert]->point;
		AddPointToBounds(v, mins, maxs);
		VectorAdd(center, v, center);
	}
	VectorScale(center, 1.0f / surf->numsurfedges, center);
	VectorCopy(surf->plane->normal, normal);
	if (surf->drawflags & DSURF_PLANEBACK)
		VectorInverse(normal);

	for (int t = 0; t < num_triggers; t++) {
		const q1_trigger_t *tr = &triggers[t];
		vec3_t gap, to_mid;
		for (int k = 0; k < 3; k++) {
			gap[k] = max(0.f, max(tr->mins[k] - maxs[k], mins[k] - tr->maxs[k]));
			to_mid[k] = 0.5f * (tr->mins[k] + tr->maxs[k]) - center[k];
		}
		const float sep = VectorLength(gap);
		if (sep > Q1_GATE_REACH)
			continue;
		const float score = sep + tr->cost + (DotProduct(normal, to_mid) < 0 ? Q1_GATE_BEHIND : 0);
		if (best < 0 || score < best_score) {
			best = t;
			best_score = score;
		}
	}
	return best;
}

/* The floor under the destination: the top of the first solid going down from
   the player's origin there (the marker + 27, dest + 24) to `lowest`. False
   when there is none that high, or the origin is itself in the solid. */
static bool q1_dest_floor(const bsp_t* bsp, const vec3_t dest, float lowest, float* floor_z)
{
	vec3_t p = { dest[0], dest[1], dest[2] + 24.0f };
	mleaf_t *leaf = BSP_PointLeaf(bsp->nodes, p);

	if (!leaf || (leaf->contents & CONTENTS_SOLID))
		return false;

	for (int step = 0; step < 512 && p[2] >= lowest; step++) {
		p[2] -= 1.0f;
		leaf = BSP_PointLeaf(bsp->nodes, p);
		if (!leaf || (leaf->contents & CONTENTS_SOLID)) {
			*floor_z = p[2] + 1.0f;
			return true;
		}
	}
	return false;
}

/* THE SHAPE OF EACH GATE, for the light through it (pt_portal_light): direct
   light is sampled through a gate from the point mirrored to its far side
   (direct_lighting.rgen, add_portal_light), which needs to know how big each
   gate looks from a surface, to pick one, and which cluster's lights to sample.
   The faces bsp_mesh.c draws as this gate (vkpt_cameras_q1_face), the world's
   and a func_wall's alike: the way
   they stand is the area-weighted main axis of their normals - so a gate's
   narrow side faces do not tip it, and either way round counts the same - their
   reach along that axis from the trigger box's middle is the gate's half
   thickness (where the faces the eye meets stand, portal_carry_ray), across it
   the half width, and their top over the box's floor the height. */
static void q1_gate_shapes(bsp_mesh_t* wm, const bsp_t* bsp)
{
	for (int g = 0; g < wm->num_cameras; g++) {
		float sxx = 0, sxy = 0, syy = 0;
		float cx, cy, nx, ny;

		if (!wm->cameras[g].q1_gate)
			continue;
		wm->cameras[g].gate_height = 0;

		for (int i = 0; i < bsp->numfaces; i++) {
			const mface_t *surf = bsp->faces + i;
			vec3_t area = { 0, 0, 0 };

			if (vkpt_cameras_q1_face(wm, surf) != g)
				continue;
			const float *v0 = surf->firstsurfedge[0].edge->v[surf->firstsurfedge[0].vert]->point;
			for (int k = 1; k + 1 < surf->numsurfedges; k++) {
				const float *v1 = surf->firstsurfedge[k].edge->v[surf->firstsurfedge[k].vert]->point;
				const float *v2 = surf->firstsurfedge[k + 1].edge->v[surf->firstsurfedge[k + 1].vert]->point;
				vec3_t e1, e2, c;
				VectorSubtract(v1, v0, e1);
				VectorSubtract(v2, v0, e2);
				CrossProduct(e1, e2, c);
				VectorAdd(area, c, area);
			}
			float a = 0.5f * VectorLength(area);
			const float *pn = surf->plane->normal;
			sxx += a * pn[0] * pn[0];
			sxy += a * pn[0] * pn[1];
			syy += a * pn[1] * pn[1];
		}
		if (sxx + syy <= 0)
			continue;     // no face: the shader sends no light through it

		float angle = 0.5f * atan2f(2.0f * sxy, sxx - syy);
		nx = cosf(angle);
		ny = sinf(angle);
		cx = 0.5f * (wm->cameras[g].gate_mins[0] + wm->cameras[g].gate_maxs[0]);
		cy = 0.5f * (wm->cameras[g].gate_mins[1] + wm->cameras[g].gate_maxs[1]);

		float thick = 0, width = 0, height = 0, bottom = wm->cameras[g].gate_mins[2];
		bool any = false;
		for (int i = 0; i < bsp->numfaces; i++) {
			const mface_t *surf = bsp->faces + i;

			if (vkpt_cameras_q1_face(wm, surf) != g)
				continue;
			for (int k = 0; k < surf->numsurfedges; k++) {
				const float *v = surf->firstsurfedge[k].edge->v[surf->firstsurfedge[k].vert]->point;
				float dx = v[0] - cx, dy = v[1] - cy;
				thick = max(thick, fabsf(dx * nx + dy * ny));
				width = max(width, fabsf(dy * nx - dx * ny));
				height = max(height, v[2] - wm->cameras[g].gate_mins[2]);
				bottom = any ? min(bottom, v[2]) : v[2];
				any = true;
			}
		}

		/* A gate that would look out from under the destination's floor (Matt
		   11-24, e2m2: "the view going into the floor"). The TRIGGER's bottom is
		   what lands on marker + 3, and on 10 of the 299 gates in id1, mg1 and
		   mg3 it floats above the gate's own bottom edge - e2m2's box is 38 tall
		   and 25 up - or the marker sits low over its floor, so the bottom of the
		   gate showed the floor from inside it. Those are raised until the gate's
		   bottom edge lands pt_q1_teleport_clearance over the floor there. The
		   rest already clear it by 1 to 64+ units and keep their view (Quake's
		   markers float 8 or more over the floor, and a mid-air destination,
		   mgdm2's, has no floor near it at all). */
		const float clearance = cvar_pt_q1_teleport_clearance ? max(0.f, cvar_pt_q1_teleport_clearance->value) : 8.0f;
		const float bottom_there = wm->cameras[g].dest[2] + (bottom - wm->cameras[g].gate_mins[2]);
		float floor_z;
		if (any && q1_dest_floor(bsp, wm->cameras[g].dest, bottom_there - clearance, &floor_z) &&
		    floor_z + clearance > bottom_there) {
			Com_DPrintf("Teleporter %d: view raised %.0f units off the floor at its destination\n",
			            g, floor_z + clearance - bottom_there);
			wm->cameras[g].dest[2] += floor_z + clearance - bottom_there;
		}

		// the cluster at the player's origin there (dest is the floor, which a
		// point could sit on and so test as solid): Quake's marker + 27
		vec3_t player_origin;
		VectorCopy(wm->cameras[g].dest, player_origin);
		player_origin[2] += 24.0f;
		mleaf_t *leaf = BSP_PointLeaf(bsp->nodes, player_origin);
		wm->cameras[g].dest_cluster = leaf ? leaf->cluster : -1;
		if (wm->cameras[g].dest_cluster < 0)
			continue;     // a destination in the solid: nothing to light it with

		wm->cameras[g].gate_normal[0] = nx;
		wm->cameras[g].gate_normal[1] = ny;
		wm->cameras[g].gate_half_thick = thick;
		wm->cameras[g].gate_half_width = width;
		wm->cameras[g].gate_height = height;
	}
}

void vkpt_cameras_load_q1(bsp_mesh_t* wm, const bsp_t* bsp)
{
	const char  *data;
	q3ent_t     e, *ents;
	int         count = 0, max = 0;

	q1_gates_clear();
	if (!bsp->q1 || !bsp->entitystring || !cvar_pt_q1_teleport_portals || !cvar_pt_q1_teleport_portals->integer)
		return;
	if (bsp->numfaces <= 0)
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

	/* Whose each face is: 0 the world's, 1 a drawn brush entity's, 2 another
	   model's - a trigger's (Quake draws none), or one no entity uses. A model
	   placed by its entity's "origin" is in its own space: not a gate. */
	byte *owner = Z_Mallocz(bsp->numfaces);
	for (int m = 1; m < bsp->nummodels; m++) {
		const int first = (int)(bsp->models[m].firstface - bsp->faces);
		for (int k = 0; k < bsp->models[m].numfaces; k++)
			if (first + k >= 0 && first + k < bsp->numfaces)
				owner[first + k] = 2;
	}

	q1_trigger_t *triggers = Z_Mallocz(sizeof(q1_trigger_t) * count);
	int num_triggers = 0;
	for (int i = 0; i < count; i++) {
		const q3ent_t *ent = &ents[i];
		if (ent->model[0] != '*')
			continue;
		const int model = atoi(ent->model + 1);
		if (model <= 0 || model >= bsp->nummodels)
			continue;

		const bool teleport = !strcmp(ent->classname, "trigger_teleport");
		if (teleport || !strcmp(ent->classname, "trigger_changelevel")) {
			q1_trigger_t *tr = &triggers[num_triggers++];
			tr->ent = ent;
			tr->dest = teleport ? q1_dest(ents, count, ent->target) : NULL;
			tr->model = model;
			tr->slot = -1;
			VectorCopy(bsp->models[model].mins, tr->mins);
			VectorCopy(bsp->models[model].maxs, tr->maxs);
			if (teleport) {
				vec3_t size;
				VectorSubtract(tr->maxs, tr->mins, size);
				tr->cost = 0.1f * VectorLength(size);
			}
			if (q1_inhibited(ent->spawnflags))
				tr->cost += Q1_GATE_INHIBITED;
		} else if (strncmp(ent->classname, "trigger_", 8) && !(ent->has_origin && !VectorEmpty(ent->origin))) {
			const int first = (int)(bsp->models[model].firstface - bsp->faces);
			for (int k = 0; k < bsp->models[model].numfaces; k++)
				if (first + k >= 0 && first + k < bsp->numfaces)
					owner[first + k] = 1;
		}
	}

	q1_face_gate = Z_Malloc(bsp->numfaces);
	memset(q1_face_gate, -1, bsp->numfaces);
	q1_faces = bsp->faces;
	q1_num_faces = bsp->numfaces;

	int num_gates = 0;
	bool full = false;
	for (int i = 0; i < bsp->numfaces; i++) {
		const mface_t *surf = bsp->faces + i;
		if (owner[i] == 2 || !q1_gate_texture(surf))
			continue;
		const int t = q1_gate_owner(surf, triggers, num_triggers);
		if (t < 0 || !triggers[t].dest)
			continue;     // nothing near, a slipgate to another map, or a teleporter to nowhere

		q1_trigger_t *tr = &triggers[t];
		if (tr->slot < 0) {
			if (wm->num_cameras >= MAX_CAMERAS) {
				if (!full)
					Com_WPrintf("Map has too many teleporters to see through (max: %i); the rest keep their texture\n", MAX_CAMERAS);
				full = true;
				continue;
			}

			int n = tr->slot = wm->num_cameras++;
			memset(&wm->cameras[n], 0, sizeof(wm->cameras[n]));
			wm->cameras[n].portal = true;       // so the portal code is in (main.c)
			wm->cameras[n].placed = true;
			wm->cameras[n].q1_gate = true;
			VectorCopy(tr->mins, wm->cameras[n].gate_mins);
			VectorCopy(tr->maxs, wm->cameras[n].gate_maxs);
			VectorCopy(tr->dest->origin, wm->cameras[n].dest);
			wm->cameras[n].dest[2] += 3.0f;
			wm->cameras[n].dest_yaw = tr->dest->angles[1];
			VectorCopy(tr->dest->origin, wm->cameras[n].pos);
			num_gates++;
		}
		q1_face_gate[i] = (signed char)tr->slot;
	}
	Com_DPrintf("Quake 1 teleporters: %d of %d seen through\n", num_gates, num_triggers);

	Z_Free(triggers);
	Z_Free(owner);
	Z_Free(ents);

	q1_gate_shapes(wm, bsp);
}

int vkpt_cameras_q1_face(const bsp_mesh_t* wm, const mface_t* surf)
{
	(void)wm;
	if (!q1_face_gate || surf < q1_faces || surf >= q1_faces + q1_num_faces)
		return -1;
	return q1_face_gate[surf - q1_faces];
}

void vkpt_cameras_q1_gate(const bsp_mesh_t* wm, int index, float data[16])
{
	const vec3_t *mins = &wm->cameras[index].gate_mins, *maxs = &wm->cameras[index].gate_maxs;

	// reflect_refract.rgen: column 0 the gate's floor, -2 in its .w to say
	// "Quake 1" (less than 0: a portal, path_tracer_rgen.h); column 1 the
	// destination's floor and its yaw; column 2's .xy the waver
	memset(data, 0, sizeof(float) * 16);
	data[0] = 0.5f * ((*mins)[0] + (*maxs)[0]);
	data[1] = 0.5f * ((*mins)[1] + (*maxs)[1]);
	data[2] = (*mins)[2];
	data[3] = -2.0f;
	VectorCopy(wm->cameras[index].dest, data + 4);
	data[7] = DEG2RAD(wm->cameras[index].dest_yaw);
	data[8] = cvar_pt_q1_teleport_warp ? 0.03f * max(0.f, cvar_pt_q1_teleport_warp->value) : 0.03f;    // radians
	data[9] = 2.0f;                                                 // radians a second

	// direct_lighting.rgen, add_portal_light (q1_gate_shapes): column 2's .z the
	// destination's cluster, .w the gate's height (0: no light through it);
	// column 3 the way its faces stand, its half width and half thickness
	data[10] = (float)wm->cameras[index].dest_cluster;
	data[11] = wm->cameras[index].gate_height;
	data[12] = wm->cameras[index].gate_normal[0];
	data[13] = wm->cameras[index].gate_normal[1];
	data[14] = wm->cameras[index].gate_half_width;
	data[15] = wm->cameras[index].gate_half_thick;
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
	cvar_pt_q1_teleport_portals = Cvar_Get("pt_q1_teleport_portals", "1", CVAR_ARCHIVE);
	cvar_pt_q1_teleport_warp = Cvar_Get("pt_q1_teleport_warp", "1", CVAR_ARCHIVE);
	// map load: how far over the destination's floor a gate's bottom edge has
	// to look out from - a gate that would look out lower is raised to it
	cvar_pt_q1_teleport_clearance = Cvar_Get("pt_q1_teleport_clearance", "8", CVAR_ARCHIVE);
}

void vkpt_cameras_shutdown(void)
{
	q1_gates_clear();
	Cmd_RemoveCommand("camera");
}
