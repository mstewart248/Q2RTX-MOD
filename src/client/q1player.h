/*
Copyright (C) 2026 Q2RTX rerelease port

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
// q1player.h -- Quake's own player as a player model (q1player.c)

#ifndef Q1PLAYER_H
#define Q1PLAYER_H

// "quake1/ranger": Quake's Ranger from the Quake paks (id1/id1/pak0.pak). Not
// "ranger": that is Quake III's own Ranger, in baseq3.
#define Q1P_MODEL_NAME  "quake1"
#define Q1P_SKIN_NAME   "ranger"
#define Q1P_MODEL_PATH  "id1/id1/progs/player.mdl"
#define Q1P_ICON_PATH   "/id1/id1/gfx/face1.lmp"   // the status bar's face

// what the Ranger holds, which picks his attack frames (and the axe's set of
// every other)
typedef enum {
    Q1P_GUN_SHOTGUN,
    Q1P_GUN_NAILS,
    Q1P_GUN_ROCKET,
    Q1P_GUN_LIGHTNING,
    Q1P_GUN_AXE
} q1pgun_t;

bool        Q1P_Available(void);
bool        Q1P_ModelExists(const char *model);
int         Q1P_Frame(int q2frame, q1pgun_t gun);
q1pgun_t    Q1P_GunOfVWep(const char *vwep);
bool        Q1P_SexedSound(const char *model, const char *base, char *out, size_t size);

#endif // Q1PLAYER_H
