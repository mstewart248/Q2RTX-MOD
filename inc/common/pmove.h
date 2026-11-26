/*
Copyright (C) 1997-2001 Id Software, Inc.

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

#ifndef PMOVE_H
#define PMOVE_H

/*
==============================================================

PLAYER MOVEMENT CODE

Common between server and client so prediction matches

==============================================================
*/

typedef struct {
    bool        qwmode;
    bool        airaccelerate;
    bool        strafehack;
    bool        flyhack;
    bool        waterhack;
    // [rerelease] the movement fixes from the rerelease's p_move.cpp - see
    // pmove.c. Must match on client and server or prediction diverges, so both
    // derive it from the game directory the same way SV_SpawnServer does.
    bool        rerelease;
    // [rerelease] Quake II 64's player physics (KEX pm_config.n64_physics) on
    // the q64/ maps: no crouching, and every landing holds off the next jump
    // for 128 ms. Same client/server rule as `rerelease` - both set it from the
    // map name, the client in CL_ParseConfigstring, the server in
    // SV_SpawnServer.
    bool        n64;
    // A Quake III Arena map (common/bsp_q3.c). Its Haste and Flight powerups
    // change how the player moves, and the only per-player movement state that
    // reaches client prediction bit-for-bit is pmove_state_t, so the game
    // carries them in s.gravity, which it keeps even on these maps: an ODD
    // gravity is Haste (x1.3 speed), bit 1 is Team Arena's Scout (x1.5, over
    // Haste), and a gravity of 0 to 3 is Flight. 3/800 of gravity is the
    // price. Ignored when false, so no other map changes.
    bool        q3;
    // Quake III Arena's player movement (bg_pmove.c) in place of Quake II's -
    // the bot match's "physics: Quake 3" (g_q3_physics). Both sides read it
    // from CS_Q3RULES, which the game sets at every map start. In this mode
    // PMF_TIME_LAND is Quake III's PMF_TIME_KNOCKBACK: no ground friction
    // while pm_time runs (the game sets it on a hit), and no landing delay.
    bool        q3move;
    float       speedmult;
    float       watermult;
    float       maxspeed;
    float       friction;
    float       waterfriction;
    float       flyfriction;
} pmoveParams_t;

void Pmove(pmove_t *pmove, pmoveParams_t *params);

void PmoveInit(pmoveParams_t *pmp);
void PmoveEnableQW(pmoveParams_t *pmp);

#endif // PMOVE_H
