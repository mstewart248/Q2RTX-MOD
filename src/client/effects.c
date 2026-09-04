/*
Copyright (C) 1997-2001 Id Software, Inc.
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
// cl_fx.c -- entity effects parsing and management

#include "client.h"

static void CL_LogoutEffect(const vec3_t org, int type);

static vec3_t avelocities[NUMVERTEXNORMALS];

/*
==============================================================

LIGHT STYLE MANAGEMENT

==============================================================
*/

typedef struct {
    int     length;
    float   map[MAX_QPATH - 1];
} clightstyle_t;

static clightstyle_t    cl_lightstyles[MAX_LIGHTSTYLES];

static void CL_ClearLightStyles(void)
{
    memset(cl_lightstyles, 0, sizeof(cl_lightstyles));
}

/*
================
CL_SetLightStyle
================
*/
void CL_SetLightStyle(int index, const char *s)
{
    int     i;
    clightstyle_t   *ls;

    ls = &cl_lightstyles[index];
    ls->length = strlen(s);
    if (ls->length > MAX_QPATH) {
        Com_Error(ERR_DROP, "%s: oversize style", __func__);
    }

    for (i = 0; i < ls->length; i++)
        ls->map[i] = (float)(s[i] - 'a') / (float)('m' - 'a');
}

/*
================
CL_LightStyleValue

The current value of one light style, for callers that scale their own
contribution by it rather than going through the renderer's table.
================
*/
float CL_LightStyleValue(int style)
{
    const clightstyle_t *ls;

    if (style < 0 || style >= MAX_LIGHTSTYLES)
        return 1.0f;

    ls = &cl_lightstyles[style];
    return ls->length ? ls->map[(cl.time / 100) % ls->length] : 1.0f;
}

/*
================
CL_AddLightStyles
================
*/
void CL_AddLightStyles(void)
{
    int     i, ofs = cl.time / 100;
    clightstyle_t   *ls;

    for (i = 0, ls = cl_lightstyles; i < MAX_LIGHTSTYLES; i++, ls++) {
        float value = ls->length ? ls->map[ofs % ls->length] : 1.0f;
        V_AddLightStyle(i, value);
    }
}

/*
==============================================================

DLIGHT MANAGEMENT

==============================================================
*/

static cdlight_t       cl_dlights[MAX_DLIGHTS];

static void CL_ClearDlights(void)
{
    memset(cl_dlights, 0, sizeof(cl_dlights));
}

/*
===============
CL_AllocDlight
===============
*/
cdlight_t *CL_AllocDlight(int key)
{
    int     i;
    cdlight_t   *dl;

// first look for an exact key match
    if (key) {
        dl = cl_dlights;
        for (i = 0; i < MAX_DLIGHTS; i++, dl++) {
            if (dl->key == key) {
                memset(dl, 0, sizeof(*dl));
                dl->key = key;
                return dl;
            }
        }
    }

// then look for anything else
    dl = cl_dlights;
    for (i = 0; i < MAX_DLIGHTS; i++, dl++) {
        if (dl->die < cl.time) {
            memset(dl, 0, sizeof(*dl));
            dl->key = key;
            return dl;
        }
    }

    dl = &cl_dlights[0];
    memset(dl, 0, sizeof(*dl));
    dl->key = key;
    return dl;
}

/*
===============
CL_AddDLights
===============
*/
void CL_AddDLights(void)
{
    int         i;
    cdlight_t   *dl;

    dl = cl_dlights;
    for (i = 0; i < MAX_DLIGHTS; i++, dl++) {
        if (dl->die < cl.time)
            continue;
        V_AddLight(dl->origin, dl->radius,
                   dl->color[0], dl->color[1], dl->color[2]);
    }
}

// ==============================================================

/*
==============
CL_MuzzleFlash
==============
*/
void CL_MuzzleFlash(void)
{
    vec3_t      fv, rv;
    cdlight_t   *dl;
    centity_t   *pl;
    float       volume;
    char        soundname[MAX_QPATH];

#if USE_DEBUG
    if (developer->integer)
        CL_CheckEntityPresent(mz.entity, "muzzleflash");
#endif

    pl = &cl_entities[mz.entity];

    dl = CL_AllocDlight(mz.entity);
    VectorCopy(pl->current.origin,  dl->origin);
    AngleVectors(pl->current.angles, fv, rv, NULL);
    VectorMA(dl->origin, 18, fv, dl->origin);
    VectorMA(dl->origin, 16, rv, dl->origin);
    dl->radius = 100 * (2 - mz.silenced) + (Q_rand() & 31);
    dl->die = cl.time + 33;

    // Rerelease: the flash model, at the same place the dlight was just put.
    // Our own gun in first person is handled separately - CL_AddViewWeapon is
    // the only place that knows where the view model actually ended up.
    if (mz.entity == cl.frame.clientNum + 1 && !cl.thirdPersonView)
        CL_ViewMuzzleFlash();
    else
        CL_MuzzleFlashModel(dl->origin, pl->current.angles, 1.0f);

    volume = 1.0f - 0.8f * mz.silenced;

    switch (mz.weapon) {
    case MZ_BLASTER:        
        VectorSet(dl->color, 1, 1, 0);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("weapons/blastf1a.wav"), volume, ATTN_NORM, 0);
        break;
    case MZ_BLUEHYPERBLASTER:
        VectorSet(dl->color, 0, 0, 1);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("weapons/hyprbf1a.wav"), volume, ATTN_NORM, 0);
        break;
    case MZ_HYPERBLASTER:
        VectorSet(dl->color, 1, 1, 0);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("weapons/hyprbf1a.wav"), volume, ATTN_NORM, 0);
        break;
    case MZ_MACHINEGUN:
        //DL_COLOR(1, .5, 0);
        VectorSet(dl->color, 1, .7, .2);
        dl->radius = 100;
        Q_snprintf(soundname, sizeof(soundname), "weapons/machgf%ib.wav", (Q_rand() % 5) + 1);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound(soundname), volume, ATTN_NORM, 0);
        break;
    case MZ_SHOTGUN:
        VectorSet(dl->color, 1, 1, 0);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("weapons/shotgf1b.wav"), volume, ATTN_NORM, 0);
        S_StartSound(NULL, mz.entity, CHAN_AUTO,   S_RegisterSound("weapons/shotgr1b.wav"), volume, ATTN_NORM, 0.1f);
        break;
    case MZ_SSHOTGUN:
        VectorSet(dl->color, 1, 1, 0);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("weapons/sshotf1b.wav"), volume, ATTN_NORM, 0);
        break;
    case MZ_CHAINGUN1:
        dl->radius = 200 + (Q_rand() & 31);
        VectorSet(dl->color, 1, 0.25f, 0);
        Q_snprintf(soundname, sizeof(soundname), "weapons/machgf%ib.wav", (Q_rand() % 5) + 1);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound(soundname), volume, ATTN_NORM, 0);
        break;
    case MZ_CHAINGUN2:
        dl->radius = 225 + (Q_rand() & 31);
        VectorSet(dl->color, 1, 0.5f, 0);
        Q_snprintf(soundname, sizeof(soundname), "weapons/machgf%ib.wav", (Q_rand() % 5) + 1);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound(soundname), volume, ATTN_NORM, 0);
        Q_snprintf(soundname, sizeof(soundname), "weapons/machgf%ib.wav", (Q_rand() % 5) + 1);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound(soundname), volume, ATTN_NORM, 0.05f);
        break;
    case MZ_CHAINGUN3:
        dl->radius = 250 + (Q_rand() & 31);
        VectorSet(dl->color, 1, 1, 0);
        Q_snprintf(soundname, sizeof(soundname), "weapons/machgf%ib.wav", (Q_rand() % 5) + 1);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound(soundname), volume, ATTN_NORM, 0);
        Q_snprintf(soundname, sizeof(soundname), "weapons/machgf%ib.wav", (Q_rand() % 5) + 1);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound(soundname), volume, ATTN_NORM, 0.033f);
        Q_snprintf(soundname, sizeof(soundname), "weapons/machgf%ib.wav", (Q_rand() % 5) + 1);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound(soundname), volume, ATTN_NORM, 0.066f);
        break;
    case MZ_RAILGUN:
        VectorSet(dl->color, 0.5f, 0.5f, 1.0f);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("weapons/railgf1a.wav"), volume, ATTN_NORM, 0);
        break;
    case MZ_ROCKET:
        VectorSet(dl->color, 1, 0.5f, 0.2f);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("weapons/rocklf1a.wav"), volume, ATTN_NORM, 0);
        S_StartSound(NULL, mz.entity, CHAN_AUTO,   S_RegisterSound("weapons/rocklr1b.wav"), volume, ATTN_NORM, 0.1f);
        break;
    case MZ_GRENADE:
        VectorSet(dl->color, 1, 0.5f, 0);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("weapons/grenlf1a.wav"), volume, ATTN_NORM, 0);
        S_StartSound(NULL, mz.entity, CHAN_AUTO,   S_RegisterSound("weapons/grenlr1b.wav"), volume, ATTN_NORM, 0.1f);
        break;
    case MZ_BFG:
        VectorSet(dl->color, 0, 1, 0);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("weapons/bfg__f1y.wav"), volume, ATTN_NORM, 0);
        break;
    case MZ_LOGIN:
        VectorSet(dl->color, 0, 1, 0);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("weapons/grenlf1a.wav"), 1, ATTN_NORM, 0);
        CL_LogoutEffect(pl->current.origin, mz.weapon);
        break;
    case MZ_LOGOUT:
        VectorSet(dl->color, 1, 0, 0);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("weapons/grenlf1a.wav"), 1, ATTN_NORM, 0);
        CL_LogoutEffect(pl->current.origin, mz.weapon);
        break;
    case MZ_RESPAWN:
        VectorSet(dl->color, 1, 1, 0);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("weapons/grenlf1a.wav"), 1, ATTN_NORM, 0);
        CL_LogoutEffect(pl->current.origin, mz.weapon);
        break;
    case MZ_PHALANX:
        VectorSet(dl->color, 1, 0.5f, 0.5f);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("weapons/plasshot.wav"), volume, ATTN_NORM, 0);
        break;
    case MZ_IONRIPPER:
        VectorSet(dl->color, 1, 0.5f, 0.5f);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("weapons/rippfire.wav"), volume, ATTN_NORM, 0);
        break;

    case MZ_ETF_RIFLE:
        VectorSet(dl->color, 0.9f, 0.7f, 0);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("weapons/nail1.wav"), volume, ATTN_NORM, 0);
        break;
    case MZ_SHOTGUN2:
        VectorSet(dl->color, 1, 1, 0);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("weapons/shotg2.wav"), volume, ATTN_NORM, 0);
        break;
    case MZ_HEATBEAM:
        VectorSet(dl->color, 1, 1, 0);
        dl->die = cl.time + 100;
//      S_StartSound (NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("weapons/bfg__l1a.wav"), volume, ATTN_NORM, 0);
        break;
    case MZ_BLASTER2:
        VectorSet(dl->color, 0, 1, 0);
        // FIXME - different sound for blaster2 ??
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("weapons/blastf1a.wav"), volume, ATTN_NORM, 0);
        break;
    case MZ_TRACKER:
        // negative flashes handled the same in gl/soft until CL_AddDLights
        VectorSet(dl->color, -1, -1, -1);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("weapons/disint2.wav"), volume, ATTN_NORM, 0);
        break;
    case MZ_NUKE1:
        VectorSet(dl->color, 1, 0, 0);
        dl->die = cl.time + 100;
        break;
    case MZ_NUKE2:
        VectorSet(dl->color, 1, 1, 0);
        dl->die = cl.time + 100;
        break;
    case MZ_NUKE4:
        VectorSet(dl->color, 0, 0, 1);
        dl->die = cl.time + 100;
        break;
    case MZ_NUKE8:
        VectorSet(dl->color, 0, 1, 1);
        dl->die = cl.time + 100;
        break;

	// Q2RTX
	case MZ_FLARE:
		dl->radius = 0;
		S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("weapons/flaregun.wav"), volume, ATTN_NORM, 0);
		break;

	// A bare red light, no particles and no sound. Unlike a real muzzle flash
	// this sits on the entity rather than at the end of a gun barrel, so the
	// forward/right offset applied above is undone - the MGU drop pod is only
	// 32 units across and the offset would put the light outside its walls.
	case MZ_PODLIGHT:
		VectorCopy(pl->current.origin, dl->origin);
		dl->origin[2] += 16;
		VectorSet(dl->color, 1.0f, 0.0f, 0.0f);              // pure red
		dl->radius = 192 + (Q_rand() & 63);                   // 80 .. 143
		// Shorter than the 100ms think interval so each pulse has a real
		// off-phase, and jittered so the strobe never settles into a rhythm.
		dl->die = cl.time + 35 + (Q_rand() & 31);            // 35 .. 66ms
		break;
	// Q2RTX
    }

	//if (vid_rtx->integer)
	//{
	//	// don't add muzzle flashes in RTX mode
	//	DL_RADIUS(0.f);
	//}

    if (cl_dlight_hacks->integer & DLHACK_NO_MUZZLEFLASH) {
        switch (mz.weapon) {
        case MZ_MACHINEGUN:
        case MZ_CHAINGUN1:
        case MZ_CHAINGUN2:
        case MZ_CHAINGUN3:
            memset(dl, 0, sizeof(*dl));
            break;
        }
    }

}


/*
==============
CL_MuzzleFlash2
==============
*/
// Shifts every monster muzzle flash along its own forward axis.  These offsets
// are calibrated by eye against the model, never derived, so this is the knob
// for doing that calibration in game rather than through a rebuild each time.
// 0 is id's own value.
static cvar_t *cl_monster_flash_nudge;

// Whether the flash is angled along the muzzle rather than along the monster's
// body.  The two differ whenever a gun arm animates independently of the torso;
// the medic's hyperblaster is the worst case, because its barrel sweeps.
static cvar_t *cl_monster_flash_aim;

void CL_MuzzleFlash2(void)
{
    centity_t   *ent;
    vec3_t      origin;
    const vec_t *ofs;
    cdlight_t   *dl;
    vec3_t      forward, right;
    float       fwd_ofs;
    char        soundname[MAX_QPATH];

    // locate the origin
    ent = &cl_entities[mz.entity];
    AngleVectors(ent->current.angles, forward, right, NULL);
    ofs = monster_flash_offset[mz.weapon];

    // The medic's hyperblaster muzzle sweeps with the spinning barrel, so the
    // flash has to be placed from the firing FRAME rather than from a single
    // offset - otherwise it hangs in the air while the gun swings under it.
    if (mz.weapon == MZ2_MEDIC_HYPERBLASTER) {
        int i = ent->current.frame - MEDIC_FRAME_ATTACK19;

        if (i >= 0 && i < MEDIC_HYPERBLASTER_SHOTS)
            ofs = medic_hyperblaster_offset[i];
    }
    // cl_monster_flash_nudge slides the flash along the muzzle's own forward
    // axis, for calibrating these offsets in game rather than by rebuilding.
    fwd_ofs = ofs[0] + cl_monster_flash_nudge->value;

    origin[0] = ent->current.origin[0] + forward[0] * fwd_ofs + right[0] * ofs[1];
    origin[1] = ent->current.origin[1] + forward[1] * fwd_ofs + right[1] * ofs[1];
    origin[2] = ent->current.origin[2] + forward[2] * fwd_ofs + right[2] * ofs[1] + ofs[2];

    dl = CL_AllocDlight(mz.entity);
    VectorCopy(origin,  dl->origin);
    dl->radius = 200 + (Q_rand() & 31);
    dl->die = cl.time + 16;

    // Rerelease: a starburst model at the muzzle. Monsters are the easy half -
    // monster_flash_offset[] already gives the exact muzzle, which is what
    // `origin` above is.
    // The flash model is a fan facing +X, so it is only right when it points
    // down the barrel.  ent->current.angles is the monster's BODY, which is a
    // different thing the moment a gun arm animates independently of the torso
    // - the medic's hyperblaster swings about 20 degrees across its burst, and
    // any monster shooting up or down at you is aiming with its arm, not its
    // feet.
    //
    // svc_muzzleflash3 carries the real fire direction, so use it. The body
    // angles remain the fallback for the old message and for the player.
    {
        vec3_t flash_angles;

        if (mz.has_dir && cl_monster_flash_aim->integer) {
            vectoangles2(mz.dir, flash_angles);
        } else {
            VectorCopy(ent->current.angles, flash_angles);
        }

        CL_MuzzleFlashModel(origin, flash_angles, 1.0f);
    }

    switch (mz.weapon) {
    case MZ2_INFANTRY_MACHINEGUN_1:
    case MZ2_INFANTRY_MACHINEGUN_2:
    case MZ2_INFANTRY_MACHINEGUN_3:
    case MZ2_INFANTRY_MACHINEGUN_4:
    case MZ2_INFANTRY_MACHINEGUN_5:
    case MZ2_INFANTRY_MACHINEGUN_6:
    case MZ2_INFANTRY_MACHINEGUN_7:
    case MZ2_INFANTRY_MACHINEGUN_8:
    case MZ2_INFANTRY_MACHINEGUN_9:
    case MZ2_INFANTRY_MACHINEGUN_10:
    case MZ2_INFANTRY_MACHINEGUN_11:
    case MZ2_INFANTRY_MACHINEGUN_12:
    case MZ2_INFANTRY_MACHINEGUN_13:
    // rerelease: 14-21 are the run-and-gun frames, 22 is the attak416 shot
    case MZ2_INFANTRY_MACHINEGUN_14:
    case MZ2_INFANTRY_MACHINEGUN_15:
    case MZ2_INFANTRY_MACHINEGUN_16:
    case MZ2_INFANTRY_MACHINEGUN_17:
    case MZ2_INFANTRY_MACHINEGUN_18:
    case MZ2_INFANTRY_MACHINEGUN_19:
    case MZ2_INFANTRY_MACHINEGUN_20:
    case MZ2_INFANTRY_MACHINEGUN_21:
    case MZ2_INFANTRY_MACHINEGUN_22:
        VectorSet(dl->color, 1, 1, 0);
        CL_ParticleEffect(origin, forward, 0, 40);
        CL_SmokeAndFlash(origin);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("infantry/infatck1.wav"), 1, ATTN_NORM, 0);
        break;

    case MZ2_SOLDIER_MACHINEGUN_1:
    case MZ2_SOLDIER_MACHINEGUN_2:
    case MZ2_SOLDIER_MACHINEGUN_3:
    case MZ2_SOLDIER_MACHINEGUN_4:
    case MZ2_SOLDIER_MACHINEGUN_5:
    case MZ2_SOLDIER_MACHINEGUN_6:
    case MZ2_SOLDIER_MACHINEGUN_7:
    case MZ2_SOLDIER_MACHINEGUN_8:
    case MZ2_SOLDIER_MACHINEGUN_9:   // rerelease prone shot
        VectorSet(dl->color, 1, 1, 0);
        CL_ParticleEffect(origin, forward, 0, 40);
        CL_SmokeAndFlash(origin);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("soldier/solatck3.wav"), 1, ATTN_NORM, 0);
        break;

    case MZ2_GUNNER_MACHINEGUN_1:
    case MZ2_GUNNER_MACHINEGUN_2:
    case MZ2_GUNNER_MACHINEGUN_3:
    case MZ2_GUNNER_MACHINEGUN_4:
    case MZ2_GUNNER_MACHINEGUN_5:
    case MZ2_GUNNER_MACHINEGUN_6:
    case MZ2_GUNNER_MACHINEGUN_7:
    case MZ2_GUNNER_MACHINEGUN_8:
        VectorSet(dl->color, 1, 1, 0);
        CL_ParticleEffect(origin, forward, 0, 40);
        CL_SmokeAndFlash(origin);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("gunner/gunatck2.wav"), 1, ATTN_NORM, 0);
        break;

    // rerelease: the gun commander's flechette chaingun. Its own sound, and no
    // bullet puff - it fires darts, not bullets.
    case MZ2_GUNCMDR_CHAINGUN_1:
    case MZ2_GUNCMDR_CHAINGUN_2:
        VectorSet(dl->color, 1, 1, 0);
        CL_SmokeAndFlash(origin);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("guncmdr/gcdratck2.wav"), 1, ATTN_NORM, 0);
        break;

    case MZ2_ACTOR_MACHINEGUN_1:
    case MZ2_SUPERTANK_MACHINEGUN_1:
    case MZ2_SUPERTANK_MACHINEGUN_2:
    case MZ2_SUPERTANK_MACHINEGUN_3:
    case MZ2_SUPERTANK_MACHINEGUN_4:
    case MZ2_SUPERTANK_MACHINEGUN_5:
    case MZ2_SUPERTANK_MACHINEGUN_6:
    case MZ2_TURRET_MACHINEGUN:
        VectorSet(dl->color, 1, 1, 0);
        CL_ParticleEffect(origin, forward, 0, 40);
        CL_SmokeAndFlash(origin);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("infantry/infatck1.wav"), 1, ATTN_NORM, 0);
        break;

    case MZ2_BOSS2_MACHINEGUN_L1:
    case MZ2_BOSS2_MACHINEGUN_L2:
    case MZ2_BOSS2_MACHINEGUN_L3:
    case MZ2_BOSS2_MACHINEGUN_L4:
    case MZ2_BOSS2_MACHINEGUN_L5:
    case MZ2_CARRIER_MACHINEGUN_L1:
    case MZ2_CARRIER_MACHINEGUN_L2:
        VectorSet(dl->color, 1, 1, 0);
        CL_ParticleEffect(origin, forward, 0, 40);
        CL_SmokeAndFlash(origin);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("infantry/infatck1.wav"), 1, ATTN_NONE, 0);
        break;

    case MZ2_SOLDIER_BLASTER_1:
    case MZ2_SOLDIER_BLASTER_2:
    case MZ2_SOLDIER_BLASTER_3:
    case MZ2_SOLDIER_BLASTER_4:
    case MZ2_SOLDIER_BLASTER_5:
    case MZ2_SOLDIER_BLASTER_6:
    case MZ2_SOLDIER_BLASTER_7:
    case MZ2_SOLDIER_BLASTER_8:
    case MZ2_SOLDIER_BLASTER_9:   // rerelease prone shot
    case MZ2_TURRET_BLASTER:
        VectorSet(dl->color, 1, 1, 0);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("soldier/solatck2.wav"), 1, ATTN_NORM, 0);
        break;

    case MZ2_FLYER_BLASTER_1:
    case MZ2_FLYER_BLASTER_2:
        VectorSet(dl->color, 1, 1, 0);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("flyer/flyatck3.wav"), 1, ATTN_NORM, 0);
        break;

    case MZ2_MEDIC_BLASTER_1:
    case MZ2_MEDIC_HYPERBLASTER:
        VectorSet(dl->color, 1, 1, 0);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("medic/medatck1.wav"), 1, ATTN_NORM, 0);
        break;

    case MZ2_HOVER_BLASTER_1:
        VectorSet(dl->color, 1, 1, 0);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("hover/hovatck1.wav"), 1, ATTN_NORM, 0);
        break;

    case MZ2_FLOAT_BLASTER_1:
        VectorSet(dl->color, 1, 1, 0);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("floater/fltatck1.wav"), 1, ATTN_NORM, 0);
        break;

    case MZ2_SOLDIER_SHOTGUN_1:
    case MZ2_SOLDIER_SHOTGUN_2:
    case MZ2_SOLDIER_SHOTGUN_3:
    case MZ2_SOLDIER_SHOTGUN_4:
    case MZ2_SOLDIER_SHOTGUN_5:
    case MZ2_SOLDIER_SHOTGUN_6:
    case MZ2_SOLDIER_SHOTGUN_7:
    case MZ2_SOLDIER_SHOTGUN_8:
    case MZ2_SOLDIER_SHOTGUN_9:   // rerelease prone shot
        VectorSet(dl->color, 1, 1, 0);
        CL_SmokeAndFlash(origin);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("soldier/solatck1.wav"), 1, ATTN_NORM, 0);
        break;

    case MZ2_TANK_BLASTER_1:
    case MZ2_TANK_BLASTER_2:
    case MZ2_TANK_BLASTER_3:
        VectorSet(dl->color, 1, 1, 0);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("tank/tnkatck3.wav"), 1, ATTN_NORM, 0);
        break;

    case MZ2_TANK_MACHINEGUN_1:
    case MZ2_TANK_MACHINEGUN_2:
    case MZ2_TANK_MACHINEGUN_3:
    case MZ2_TANK_MACHINEGUN_4:
    case MZ2_TANK_MACHINEGUN_5:
    case MZ2_TANK_MACHINEGUN_6:
    case MZ2_TANK_MACHINEGUN_7:
    case MZ2_TANK_MACHINEGUN_8:
    case MZ2_TANK_MACHINEGUN_9:
    case MZ2_TANK_MACHINEGUN_10:
    case MZ2_TANK_MACHINEGUN_11:
    case MZ2_TANK_MACHINEGUN_12:
    case MZ2_TANK_MACHINEGUN_13:
    case MZ2_TANK_MACHINEGUN_14:
    case MZ2_TANK_MACHINEGUN_15:
    case MZ2_TANK_MACHINEGUN_16:
    case MZ2_TANK_MACHINEGUN_17:
    case MZ2_TANK_MACHINEGUN_18:
    case MZ2_TANK_MACHINEGUN_19:
        VectorSet(dl->color, 1, 1, 0);
        CL_ParticleEffect(origin, forward, 0, 40);
        CL_SmokeAndFlash(origin);
        Q_snprintf(soundname, sizeof(soundname), "tank/tnkatk2%c.wav", 'a' + Q_rand() % 5);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound(soundname), 1, ATTN_NORM, 0);
        break;

    case MZ2_CHICK_ROCKET_1:
    case MZ2_TURRET_ROCKET:
        VectorSet(dl->color, 1, 0.5f, 0.2f);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("chick/chkatck2.wav"), 1, ATTN_NORM, 0);
        break;

    case MZ2_TANK_ROCKET_1:
    case MZ2_TANK_ROCKET_2:
    case MZ2_TANK_ROCKET_3:
        VectorSet(dl->color, 1, 0.5f, 0.2f);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("tank/tnkatck1.wav"), 1, ATTN_NORM, 0);
        break;

    case MZ2_SUPERTANK_ROCKET_1:
    case MZ2_SUPERTANK_ROCKET_2:
    case MZ2_SUPERTANK_ROCKET_3:
    case MZ2_BOSS2_ROCKET_1:
    case MZ2_BOSS2_ROCKET_2:
    case MZ2_BOSS2_ROCKET_3:
    case MZ2_BOSS2_ROCKET_4:
    case MZ2_CARRIER_ROCKET_1:
//  case MZ2_CARRIER_ROCKET_2:
//  case MZ2_CARRIER_ROCKET_3:
//  case MZ2_CARRIER_ROCKET_4:
        VectorSet(dl->color, 1, 0.5f, 0.2f);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("tank/rocket.wav"), 1, ATTN_NORM, 0);
        break;

    case MZ2_GUNNER_GRENADE_1:
    case MZ2_GUNNER_GRENADE_2:
    case MZ2_GUNNER_GRENADE_3:
    case MZ2_GUNNER_GRENADE_4:
    case MZ2_GUNNER_GRENADE2_1:
    case MZ2_GUNNER_GRENADE2_2:
    case MZ2_GUNNER_GRENADE2_3:
    case MZ2_GUNNER_GRENADE2_4:
        VectorSet(dl->color, 1, 0.5f, 0);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("gunner/gunatck3.wav"), 1, ATTN_NORM, 0);
        break;

    // rerelease: the supertank's third attack, a two-shoulder grenade launcher.
    // It keeps the supertank's own weapon voice rather than the gunner's.
    case MZ2_SUPERTANK_GRENADE_1:
    case MZ2_SUPERTANK_GRENADE_2:
        VectorSet(dl->color, 1, 0.5f, 0);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("tank/rocket.wav"), 1, ATTN_NORM, 0);
        break;

    // rerelease: the gun commander's three grenade throws - mortar (lobbed
    // high), front (straight ahead) and crouch (fired from a duck).
    case MZ2_GUNCMDR_GRENADE_MORTAR_1:
    case MZ2_GUNCMDR_GRENADE_MORTAR_2:
    case MZ2_GUNCMDR_GRENADE_MORTAR_3:
    case MZ2_GUNCMDR_GRENADE_FRONT_1:
    case MZ2_GUNCMDR_GRENADE_FRONT_2:
    case MZ2_GUNCMDR_GRENADE_FRONT_3:
    case MZ2_GUNCMDR_GRENADE_CROUCH_1:
    case MZ2_GUNCMDR_GRENADE_CROUCH_2:
    case MZ2_GUNCMDR_GRENADE_CROUCH_3:
        VectorSet(dl->color, 1, 0.5f, 0);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("guncmdr/gcdratck3.wav"), 1, ATTN_NORM, 0);
        break;

    case MZ2_GLADIATOR_RAILGUN_1:
    case MZ2_CARRIER_RAILGUN:
    case MZ2_WIDOW_RAIL:
    // rerelease: the arachnid's railgun, same blue-white as the others
    case MZ2_ARACHNID_RAIL1:
    case MZ2_ARACHNID_RAIL2:
    case MZ2_ARACHNID_RAIL_UP1:
    case MZ2_ARACHNID_RAIL_UP2:
        VectorSet(dl->color, 0.5f, 0.5f, 1.0f);
        break;

    case MZ2_MAKRON_BFG:
        VectorSet(dl->color, 0.5f, 1, 0.5f);
        //S_StartSound (NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("makron/bfg_fire.wav"), 1, ATTN_NORM, 0);
        break;

    case MZ2_MAKRON_BLASTER_1:
    case MZ2_MAKRON_BLASTER_2:
    case MZ2_MAKRON_BLASTER_3:
    case MZ2_MAKRON_BLASTER_4:
    case MZ2_MAKRON_BLASTER_5:
    case MZ2_MAKRON_BLASTER_6:
    case MZ2_MAKRON_BLASTER_7:
    case MZ2_MAKRON_BLASTER_8:
    case MZ2_MAKRON_BLASTER_9:
    case MZ2_MAKRON_BLASTER_10:
    case MZ2_MAKRON_BLASTER_11:
    case MZ2_MAKRON_BLASTER_12:
    case MZ2_MAKRON_BLASTER_13:
    case MZ2_MAKRON_BLASTER_14:
    case MZ2_MAKRON_BLASTER_15:
    case MZ2_MAKRON_BLASTER_16:
    case MZ2_MAKRON_BLASTER_17:
        VectorSet(dl->color, 1, 1, 0);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("makron/blaster.wav"), 1, ATTN_NORM, 0);
        break;

    case MZ2_JORG_MACHINEGUN_L1:
    case MZ2_JORG_MACHINEGUN_L2:
    case MZ2_JORG_MACHINEGUN_L3:
    case MZ2_JORG_MACHINEGUN_L4:
    case MZ2_JORG_MACHINEGUN_L5:
    case MZ2_JORG_MACHINEGUN_L6:
        VectorSet(dl->color, 1, 1, 0);
        CL_ParticleEffect(origin, forward, 0, 40);
        CL_SmokeAndFlash(origin);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("boss3/xfire.wav"), 1, ATTN_NORM, 0);
        break;

    case MZ2_JORG_MACHINEGUN_R1:
    case MZ2_JORG_MACHINEGUN_R2:
    case MZ2_JORG_MACHINEGUN_R3:
    case MZ2_JORG_MACHINEGUN_R4:
    case MZ2_JORG_MACHINEGUN_R5:
    case MZ2_JORG_MACHINEGUN_R6:
        VectorSet(dl->color, 1, 1, 0);
        CL_ParticleEffect(origin, forward, 0, 40);
        CL_SmokeAndFlash(origin);
        break;

    case MZ2_JORG_BFG_1:
        VectorSet(dl->color, 0.5f, 1, 0.5f);
        break;

    case MZ2_BOSS2_MACHINEGUN_R1:
    case MZ2_BOSS2_MACHINEGUN_R2:
    case MZ2_BOSS2_MACHINEGUN_R3:
    case MZ2_BOSS2_MACHINEGUN_R4:
    case MZ2_BOSS2_MACHINEGUN_R5:
    case MZ2_CARRIER_MACHINEGUN_R1:
    case MZ2_CARRIER_MACHINEGUN_R2:
        VectorSet(dl->color, 1, 1, 0);
        CL_ParticleEffect(origin, forward, 0, 40);
        CL_SmokeAndFlash(origin);
        break;

    case MZ2_STALKER_BLASTER:
    case MZ2_DAEDALUS_BLASTER:
    case MZ2_MEDIC_BLASTER_2:
    case MZ2_WIDOW_BLASTER:
    case MZ2_WIDOW_BLASTER_SWEEP1:
    case MZ2_WIDOW_BLASTER_SWEEP2:
    case MZ2_WIDOW_BLASTER_SWEEP3:
    case MZ2_WIDOW_BLASTER_SWEEP4:
    case MZ2_WIDOW_BLASTER_SWEEP5:
    case MZ2_WIDOW_BLASTER_SWEEP6:
    case MZ2_WIDOW_BLASTER_SWEEP7:
    case MZ2_WIDOW_BLASTER_SWEEP8:
    case MZ2_WIDOW_BLASTER_SWEEP9:
    case MZ2_WIDOW_BLASTER_100:
    case MZ2_WIDOW_BLASTER_90:
    case MZ2_WIDOW_BLASTER_80:
    case MZ2_WIDOW_BLASTER_70:
    case MZ2_WIDOW_BLASTER_60:
    case MZ2_WIDOW_BLASTER_50:
    case MZ2_WIDOW_BLASTER_40:
    case MZ2_WIDOW_BLASTER_30:
    case MZ2_WIDOW_BLASTER_20:
    case MZ2_WIDOW_BLASTER_10:
    case MZ2_WIDOW_BLASTER_0:
    case MZ2_WIDOW_BLASTER_10L:
    case MZ2_WIDOW_BLASTER_20L:
    case MZ2_WIDOW_BLASTER_30L:
    case MZ2_WIDOW_BLASTER_40L:
    case MZ2_WIDOW_BLASTER_50L:
    case MZ2_WIDOW_BLASTER_60L:
    case MZ2_WIDOW_BLASTER_70L:
    case MZ2_WIDOW_RUN_1:
    case MZ2_WIDOW_RUN_2:
    case MZ2_WIDOW_RUN_3:
    case MZ2_WIDOW_RUN_4:
    case MZ2_WIDOW_RUN_5:
    case MZ2_WIDOW_RUN_6:
    case MZ2_WIDOW_RUN_7:
    case MZ2_WIDOW_RUN_8:
        VectorSet(dl->color, 0, 1, 0);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("tank/tnkatck3.wav"), 1, ATTN_NORM, 0);
        break;

    case MZ2_WIDOW_DISRUPTOR:
        VectorSet(dl->color, -1, -1, -1);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("weapons/disint2.wav"), 1, ATTN_NORM, 0);
        break;

    case MZ2_WIDOW_PLASMABEAM:
    case MZ2_WIDOW2_BEAMER_1:
    case MZ2_WIDOW2_BEAMER_2:
    case MZ2_WIDOW2_BEAMER_3:
    case MZ2_WIDOW2_BEAMER_4:
    case MZ2_WIDOW2_BEAMER_5:
    case MZ2_WIDOW2_BEAM_SWEEP_1:
    case MZ2_WIDOW2_BEAM_SWEEP_2:
    case MZ2_WIDOW2_BEAM_SWEEP_3:
    case MZ2_WIDOW2_BEAM_SWEEP_4:
    case MZ2_WIDOW2_BEAM_SWEEP_5:
    case MZ2_WIDOW2_BEAM_SWEEP_6:
    case MZ2_WIDOW2_BEAM_SWEEP_7:
    case MZ2_WIDOW2_BEAM_SWEEP_8:
    case MZ2_WIDOW2_BEAM_SWEEP_9:
    case MZ2_WIDOW2_BEAM_SWEEP_10:
    case MZ2_WIDOW2_BEAM_SWEEP_11:
        dl->radius = 300 + (Q_rand() & 100);
        VectorSet(dl->color, 1, 1, 0);
        dl->die = cl.time + 200;
        break;
    }
}

/*
==============================================================

PARTICLE MANAGEMENT

==============================================================
*/

static cparticle_t  *active_particles, *free_particles;

static cparticle_t  particles[MAX_PARTICLES];

extern uint32_t d_8to24table[256];

cvar_t* cvar_pt_particle_emissive = NULL;
static cvar_t* cl_particle_num_factor = NULL;

// Blood droplet collision - see CL_SimulateBloodSphere further down.
static cvar_t *cl_blood_collision = NULL;
static cvar_t *cl_blood_splat_life = NULL;
static cvar_t *cl_blood_slide = NULL;
static cvar_t *cl_blood_flatten = NULL;
static cvar_t *cl_blood_splat_size = NULL;
static cvar_t *cl_blood_air_life = NULL;
static cvar_t *cl_blood_pool = NULL;
static cvar_t *cl_blood_pool_max = NULL;
static cvar_t *cl_blood_pool_dist = NULL;
static cvar_t *cl_blood_stretch = NULL;
static cvar_t *cl_blood_splash = NULL;
static cvar_t *cl_blood_max = NULL;
static cvar_t *cl_blood_gravity = NULL;
static cvar_t *cl_blood_speed = NULL;
static cvar_t *cl_blood_sound = NULL;
static cvar_t *cl_blood_sound_volume = NULL;
static cvar_t *cl_blood_sound_gap = NULL;
static cvar_t *cl_blood_sound_dist = NULL;

// The wet-impact sounds, registered once per level by CL_RegisterTEntSounds.
// NUM_BLOOD_SFX is declared alongside the extern in client.h.
qhandle_t cl_sfx_blood_splat[NUM_BLOOD_SFX];

// Blood droplets alive at the end of the last CL_AddParticles, and the budget
// CL_MakeBloodSphere checks against. See the comment there.
static int num_blood_live;

/*
Ownership of the renderer's geometry slots.

A droplet keeps one slot from the moment it becomes a blood sphere until it dies,
so its vertices stay at one fixed place in the renderer's buffer and it can be
skipped entirely on frames where nothing about it changed.

Slots are handed out LOWEST FIRST so the occupied range stays tight - the
renderer builds its acceleration structure over slot 0 up to the highest one in
use, and a sparse high slot would drag that range out behind it.
*/
static bool blood_slot_used[MAX_BLOOD_SPHERES];
static bool blood_slot_seen[MAX_BLOOD_SPHERES];

static int CL_AllocBloodSlot(void)
{
    for (int i = 0; i < MAX_BLOOD_SPHERES; i++) {
        if (!blood_slot_used[i]) {
            blood_slot_used[i] = true;
            return i;
        }
    }
    return -1;
}

// Traces issued by the droplet simulation this frame - the suspected cost.
static int blood_traces;
static cvar_t *cl_blood_stats = NULL;

void FX_Init(void)
{
    cvar_pt_particle_emissive = Cvar_Get("pt_particle_emissive", "10.0", 0);
	cl_particle_num_factor = Cvar_Get("cl_particle_num_factor", "1", 0);

    // Blood droplet collision. Its own cvar, separate from cl_blood_spheres, so
    // the simulation can be A/B'd against plain ballistic droplets without also
    // switching the whole effect back to flat particle sprites.
    cl_blood_collision = Cvar_Get("cl_blood_collision", "1", CVAR_ARCHIVE);
    cl_blood_splat_life = Cvar_Get("cl_blood_splat_life", "8", CVAR_ARCHIVE);
    cl_blood_slide = Cvar_Get("cl_blood_slide", "2.5", CVAR_ARCHIVE);
    // Thickness of a splat along the surface normal. Note the coupling: the
    // renderer spreads a splat by 1/sqrt(flatten) to conserve its volume, so a
    // lower value is both thinner AND wider - which is what "more smooshed"
    // actually means.
    // Height of a landed puddle, as a fraction of its width. Independent of
    // cl_blood_splat_size now: flatness sets how raised it looks, size sets how
    // far it spreads, and neither drags the other with it.
    cl_blood_flatten = Cvar_Get("cl_blood_flatten", "0.16", CVAR_ARCHIVE);

    // Width of a landed puddle, as a multiple of the droplet's own radius.
    cl_blood_splat_size = Cvar_Get("cl_blood_splat_size", "1.5", CVAR_ARCHIVE);

    // How long a droplet may stay in the air before giving up. NOT a fade: a
    // droplet in flight keeps full size and simply waits until it hits
    // something. This exists only so that blood thrown into a void, or out of a
    // window, is eventually reclaimed instead of orbiting forever.
    cl_blood_air_life = Cvar_Get("cl_blood_air_life", "15", CVAR_ARCHIVE);

    // Pooling: a droplet landing on an existing splat merges into it instead of
    // adding another overlapping disc.
    cl_blood_pool = Cvar_Get("cl_blood_pool", "1", CVAR_ARCHIVE);
    cl_blood_pool_max = Cvar_Get("cl_blood_pool_max", "4", CVAR_ARCHIVE);
    // In DROPLET widths (see the reach calculation - it is not scaled by the
    // splat spread). This is the knob that decides whether a floor reads as many
    // separate marks or a few large pools, and it interacts with gravity: real
    // gravity lands droplets closer together, so the same reach merges far more
    // of them. 2.0 keeps individual spatter visible under the new fall rate.
    // Below about 1.0 nothing pools at all - a measured burst lands its droplets
    // ~8 units apart against a ~4 unit reach, and 120 splats became 118.
    cl_blood_pool_dist = Cvar_Get("cl_blood_pool_dist", "2.0", CVAR_ARCHIVE);

    // Elongation of a splat along the direction the droplet was travelling.
    cl_blood_stretch = Cvar_Get("cl_blood_stretch", "2.0", CVAR_ARCHIVE);

    // How much a HEAD-ON impact spreads on landing. A glancing hit smears along
    // its travel direction (cl_blood_stretch); one that arrives square has no
    // direction to smear along, and spreads outward in every direction instead -
    // so it gets a wider, still-round splat rather than a stretched one.
    cl_blood_splash = Cvar_Get("cl_blood_splash", "1.4", CVAR_ARCHIVE);

    // Ceiling on how many droplets may be alive at once.
    //
    // THIS IS THE COST CONTROL FOR THE WHOLE FEATURE, and it exists because
    // making droplets persist until they land removed the thing that used to
    // bound them. They previously faded in well under a second; now a gib trail
    // that emits every step keeps every one of those droplets alive until it
    // reaches a surface, and a measured firefight had 1617 in the air at once.
    //
    // Over budget, blood stays an ordinary particle instead - so the spray still
    // looks right, it just stops adding geometry.
    // 512 matches the renderer's own MAX_BLOOD_SPHERES, so this is a SAFETY
    // VALVE rather than a limiter - a settled firefight sits around 250. It is
    // here because persistence removed what used to bound droplet count (they
    // faded in under a second; now they live until they land), and a measured
    // burst had 1617 in the air mid-gib. Lower it if a fight ever needs bounding.
    cl_blood_max = Cvar_Get("cl_blood_max", "512", CVAR_ARCHIVE);

    // Gravity on a simulated droplet, in units/sec^2.
    //
    // NOT PARTICLE_GRAVITY, which is 120 and is not gravity at all - it is a
    // drift constant for particles that only ever have to look vaguely downward
    // over half a second, and the analytic path even applies it as accel*t^2
    // rather than the physical accel*t^2/2, so its effective value is arbitrary.
    // Blood that now arcs across a room and lands on things has to fall at a
    // rate the eye can check against everything else in the scene, and that is
    // the player's own gravity: 800.
    cl_blood_gravity = Cvar_Get("cl_blood_gravity", "800", CVAR_ARCHIVE);

    // Spray speed multiplier, applied only when the droplets are simulated.
    //
    // The 35-110 units/sec the spray was built around was tuned against
    // PARTICLE_GRAVITY, and real gravity is nearly seven times that. Range goes
    // as v^2/g, so keeping the old speeds under the new gravity collapsed the
    // spatter into a puddle at the wound - correct physics, but it threw away
    // the spread that made it read as a spray. 2.0 restores most of the reach.
    //
    // Bounded above by the same thing it always was: the spray leaves the wound
    // TOWARDS the shooter, and a droplet that arrives within a few units of the
    // eye is drawn as a screen-filling red blob. Real gravity helps here, since
    // it pulls droplets down before they can cross the gap.
    cl_blood_speed = Cvar_Get("cl_blood_speed", "2.0", CVAR_ARCHIVE);

    // A wet impact when blood lands.
    cl_blood_sound = Cvar_Get("cl_blood_sound", "1", CVAR_ARCHIVE);
    // 1.0, because the CLIPS are now normalised to Quake II's own level rather
    // than the engine compensating for quiet assets. Measured: world/ric1.wav
    // sits at mean -16 dB / peak 0 dB, and the source mp3s arrived ~10 dB under
    // that - which is why this was inaudible however far the slider went. The
    // wavs are regenerated from the mp3s with matching gain plus a limiter.
    cl_blood_sound_volume = Cvar_Get("cl_blood_sound_volume", "1.0", CVAR_ARCHIVE);

    // Minimum milliseconds between two impact sounds.
    //
    // THIS IS THE WHOLE DESIGN. A single wound throws sixty droplets that land
    // within a few tenths of a second, so one sound per droplet would be sixty
    // overlapping copies of the same clip - a burst of noise, and enough voices
    // to starve every other sound in the scene. One impact per window turns that
    // into a single wet splat, and a sustained fight into an irregular patter,
    // which is what it should sound like.
    cl_blood_sound_gap = Cvar_Get("cl_blood_sound_gap", "90", CVAR_ARCHIVE);

    // Beyond this many units a landing droplet makes no sound at all. Distance
    // attenuation would make it inaudible anyway, but it would still take a
    // voice and still count against the gap above, silencing a nearer impact.
    cl_blood_sound_dist = Cvar_Get("cl_blood_sound_dist", "1200", CVAR_ARCHIVE);
    cl_blood_stats = Cvar_Get("cl_blood_stats", "0", 0);
}

static void CL_ClearParticles(void)
{
    int     i;

    free_particles = &particles[0];
    active_particles = NULL;

    for (i = 0; i < MAX_PARTICLES - 1; i++)
        particles[i].next = &particles[i + 1];
    particles[i].next = NULL;
}

cparticle_t *CL_AllocParticle(void)
{
    cparticle_t *p;
       

    if (!free_particles)
        return NULL;
    p = free_particles;
    p->particleType = PARTICLE_TYPE_NORMAL;
    p->is_blood_sphere = false;
    p->blood_slot = -1;
    p->blood_state = BLOOD_AIRBORNE;
    p->blood_flatten = 1.f;
    p->blood_stretch = 1.f;
    p->radius = 0.f;
    p->seed = 0.f;
    free_particles = p->next;
    p->next = active_particles;
    active_particles = p;

    return p;
}

/*
===============
CL_ParticleEffect

Wall impact puffs
===============
*/
/*
===============
CL_PerpendicularBasis

Two unit vectors perpendicular to `dir` and to each other.

This exists because both impact effects were building their spread basis wrong.
CL_ParticleEffect used a raw world axis as one of the two spread vectors, so it
was never perpendicular to dir (up to 0.95 parallel), and took an un-normalised
cross product for the other, which collapses to 0.31 of unit length at some
angles.  CL_BloodParticleEffect was worse: its two "perpendicular" vectors were
just permutations of dir's own components, which for some directions are FULLY
parallel to dir.

The visible result was a spray whose width and lean changed depending on which
way you happened to be facing - which is the directionality that was missing.

Seeding from whichever axis dir is least aligned with keeps the cross product
well conditioned: one component of a unit vector is always below 1/sqrt(3), so
the seed is never closer than 54 degrees to dir.
===============
*/
static void CL_PerpendicularBasis(const vec3_t dir, vec3_t ox, vec3_t oy)
{
    vec3_t seed;

    if (fabsf(dir[0]) < 0.577f)
        VectorSet(seed, 1.0f, 0.0f, 0.0f);
    else if (fabsf(dir[1]) < 0.577f)
        VectorSet(seed, 0.0f, 1.0f, 0.0f);
    else
        VectorSet(seed, 0.0f, 0.0f, 1.0f);

    CrossProduct(seed, dir, ox);
    VectorNormalize(ox);
    CrossProduct(dir, ox, oy);
    VectorNormalize(oy);
}

void CL_ParticleEffect(const vec3_t org, const vec3_t dir, int color, int count)
{
    vec3_t ox, oy;

    CL_PerpendicularBasis(dir, ox, oy);

    count *= cl_particle_num_factor->value;
    const int spark_count = count / 10;

    // Each particle's spray DIRECTION is derived from where it is spawned
    // relative to the impact point - see the VectorSubtract further down - so
    // these two numbers are the cone: how far a particle may sit sideways
    // against how hard it is pushed out along the surface normal.
    //
    // The sideways spread used to be twice the outward push, which put the
    // average particle 44 degrees off the normal and the widest ones at 70 -
    // near enough a disc lying flat against the wall rather than debris coming
    // out of it, which is why the impacts read the same whichever way the
    // normal pointed. Leading with the outward push instead narrows that to 24
    // degrees average / 48 worst case (measured over 200k samples), and as a
    // bonus spawns every particle at least 2 units clear of the surface rather
    // than 1, so fewer are born inside the wall.
    //
    // Sparks go from 27/54 to 16/35 by the same change.
    const float dirt_horizontal_spread = 1.6f;
    const float dirt_normal_push = 2.0f;
    const float dirt_normal_push_rand = 1.5f;
    const float dirt_base_velocity = 40.0f;
    const float dirt_rand_velocity = 70.0f;

    // Sparks come off tighter and faster than the dirt, so they read as
    // ricochets leaving the surface rather than as more debris.
    const float spark_horizontal_spread = 1.0f;
    const float spark_normal_push = 2.0f;
    const float spark_normal_push_rand = 1.5f;
    const float spark_base_velocity = 50.0f;
    const float spark_rand_velocity = 130.0f;

    for (int i = 0; i < count; i++) {
        cparticle_t* p = CL_AllocParticle();
        if (!p)
            return;

        p->time = cl.time;

        p->color = color + (Q_rand() & 7);
		p->brightness = 0.5f;

        vec3_t origin;
        VectorCopy(org, origin);
        VectorMA(origin, dirt_horizontal_spread * crand(), ox, origin);
        VectorMA(origin, dirt_horizontal_spread * crand(), oy, origin);
        VectorMA(origin, dirt_normal_push + dirt_normal_push_rand * frand(), dir, origin);
        VectorCopy(origin, p->org);

        vec3_t velocity;
        VectorSubtract(origin, org, velocity);
        VectorNormalize(velocity);
        VectorScale(velocity, dirt_base_velocity + frand() * dirt_rand_velocity, p->vel);

        p->accel[0] = p->accel[1] = 0;
        p->accel[2] = -PARTICLE_GRAVITY;
        p->alpha = 1.0f;

        p->alphavel = -1.0f / (0.5f + frand() * 0.3f);
    }

    for (int i = 0; i < spark_count; i++) {
        cparticle_t* p = CL_AllocParticle();
        if (!p)
            return;

        p->time = cl.time;

        p->color = 0xe0 + (Q_rand() & 7);
		p->brightness = cvar_pt_particle_emissive->value;

        vec3_t origin;
        VectorCopy(org, origin);
        VectorMA(origin, spark_horizontal_spread * crand(), ox, origin);
        VectorMA(origin, spark_horizontal_spread * crand(), oy, origin);
        VectorMA(origin, spark_normal_push + spark_normal_push_rand * frand(), dir, origin);
        VectorCopy(origin, p->org);

        vec3_t velocity;
        VectorSubtract(origin, org, velocity);
        VectorNormalize(velocity);
        VectorScale(velocity, spark_base_velocity + powf(frand(), 2.0f) * spark_rand_velocity, p->vel);

        p->accel[0] = p->accel[1] = 0;
        p->accel[2] = -PARTICLE_GRAVITY;
        p->alpha = 1.0f;

        p->alphavel = -2.0f / (0.5f + frand() * 0.3f);
    }
}

/*
===============
CL_BarrelBurnEffect

Rerelease EF_BARREL_EXPLODING - a misc_explobox counting down its fuse. Smoke
boiling off the lid plus sparks spitting out of it. The whole thing only lives
for 750 ms, so it is emitted hard and throttled by wall time rather than by
frame, which keeps it looking the same at 60 and at 240 fps.

fly_stoptime is the throttle, reused the way CL_TrapParticles reuses it. A
barrel is never a corpse, so it can never also want EF_FLIES.
===============
*/
void CL_BarrelBurnEffect(centity_t *ent, const vec3_t origin)
{
    cparticle_t *p;
    vec3_t      top;
    int         i;

    // ~20 Hz, independent of framerate
    if (cl.time - ent->fly_stoptime < 50)
        return;
    ent->fly_stoptime = cl.time;

    // the barrel's bbox is 0..40 in Z with the origin at its base
    VectorCopy(origin, top);
    top[2] += 34;

    // smoke boiling off the lid - slow, rising, fading
    for (i = 0; i < 4; i++) {
        p = CL_AllocParticle();
        if (!p)
            return;

        p->time = cl.time;
        p->color = 4 + (Q_rand() & 7);
        p->brightness = 0.5f;

        p->org[0] = top[0] + crand() * 8;
        p->org[1] = top[1] + crand() * 8;
        p->org[2] = top[2] + crand() * 4;

        p->vel[0] = crand() * 6;
        p->vel[1] = crand() * 6;
        p->vel[2] = 20 + frand() * 20;

        VectorClear(p->accel);
        p->accel[2] = 8;    // keeps the column drifting up as it fades

        p->alpha = 0.7f;
        p->alphavel = -1.0f / (0.6f + frand() * 0.4f);
    }

    // sparks spitting out of it - fast, ballistic, emissive
    for (i = 0; i < 6; i++) {
        p = CL_AllocParticle();
        if (!p)
            return;

        p->time = cl.time;
        p->color = 0xe0 + (Q_rand() & 7);
        p->brightness = cvar_pt_particle_emissive->value;

        p->org[0] = top[0] + crand() * 6;
        p->org[1] = top[1] + crand() * 6;
        p->org[2] = top[2] + crand() * 3;

        p->vel[0] = crand() * 60;
        p->vel[1] = crand() * 60;
        p->vel[2] = 40 + frand() * 90;

        p->accel[0] = p->accel[1] = 0;
        p->accel[2] = -PARTICLE_GRAVITY;

        p->alpha = 1.0f;
        p->alphavel = -2.0f / (0.4f + frand() * 0.3f);
    }
}

void CL_ParticleEffectWaterSplash(const vec3_t org, const vec3_t dir, int color, int count)
{
    vec3_t oy;
    VectorSet(oy, 0.0f, 1.0f, 0.0f);
    if (fabsf(DotProduct(oy, dir)) > 0.95f)
        VectorSet(oy, 1.0f, 0.0f, 0.0f);

    vec3_t ox;
    CrossProduct(oy, dir, ox);

    count *= cl_particle_num_factor->value;

    const float water_horizontal_spread = 0.25f;
    const float water_vertical_spread = 1.0f;
    const float water_base_velocity = 80.0f;
    const float water_rand_velocity = 150.0f;

    for (int i = 0; i < count; i++) {
        cparticle_t* p = CL_AllocParticle();
        if (!p)
            return;

        p->time = cl.time;

        p->color = color + (Q_rand() & 7);
		p->brightness = 1.0f;

        vec3_t origin;
        VectorCopy(org, origin);
        VectorMA(origin, water_horizontal_spread * crand(), ox, origin);
        VectorMA(origin, water_horizontal_spread * crand(), oy, origin);
        VectorMA(origin, water_vertical_spread * frand() + 1.0f, dir, origin);
        VectorCopy(origin, p->org);

        vec3_t velocity;
        VectorSubtract(origin, org, velocity);
        VectorNormalize(velocity);
        VectorScale(velocity, water_base_velocity + frand() * water_rand_velocity, p->vel);

        p->accel[0] = p->accel[1] = 0;
        p->accel[2] = -PARTICLE_GRAVITY;
        p->alpha = 1.0f;

        p->alphavel = -1.0f / (0.5f + frand() * 0.3f);
    }
}

/*
===============
Blood droplet collision simulation (cl_blood_collision)

Blood spheres are the only particles here that are not analytic.  Every other
particle computes its position from its spawn state as org + vel*t + accel*t^2,
which is cheap and stateless - and has no way to express "stopped when it hit a
wall", because there is no per-frame state to write the stop into.  So these
integrate step by step instead, and org/vel become live state.

A droplet is in one of two states:

  AIRBORNE  ballistic, traced against the world each step.  On a hit it sticks,
            squashing into an ellipsoid oriented to the surface normal.

  STUCK     gravity is projected into the surface plane, so it runs down a wall
            and sits still on a floor.  Each step re-traces into the surface to
            stay attached, which is also what detects running off an edge - if
            the surface is no longer there, the droplet goes airborne again and
            falls.

The trace is CL_TracePoint, which includes solid bmodel entities.  A world-only
trace would send every droplet straight through doors, lifts and platforms.
===============
*/
// How far off the surface a stuck droplet's anchor sits.
//
// This is 0.01 and not 0.15, and the difference is the whole "puddles float just
// above the floor" problem. 0.15 was correct while a splat was a squashed SPHERE
// - the anchor was its centre, so it had to clear the surface. The puddle mesh
// that replaced it has its base at z = 0 and builds upward, so the same offset
// lifts the entire puddle off the ground instead: +0.15 up, less the renderer's
// sink of 0.25 * height (about 0.05), left it hovering about a tenth of a unit
// clear of the floor with a visible gap under the rim.
//
// Now the anchor is the contact point itself, give or take enough to keep a
// trace from starting exactly in a plane, and pt_blood_puddle_sink alone decides
// how far the base is buried.
#define BLOOD_SURFACE_OFFSET    0.01f

// Below this speed a sliding droplet is considered parked.  Without it, droplets
// on a floor jitter forever against the projected-gravity term.
#define BLOOD_SLIDE_EPSILON     1.5f

/*
The splats that are currently on a surface, rebuilt every frame in
CL_AddParticles.

Pooling needs to ask "is there already a splat where this droplet just landed?",
and the particle list itself cannot answer it during the update: CL_AddParticles
relinks that list as it walks it, so from inside the loop only the part already
processed is still reachable. A flat array of the stuck ones, rebuilt each frame,
is both correct and faster to scan - it holds tens of entries where the particle
list holds hundreds, and every entry is a candidate.

A splat that sticks this frame is registered immediately, so a burst that lands
together still merges within the same frame.
*/
static cparticle_t *blood_splats[MAX_PARTICLES];
static int          num_blood_splats;

/*
===============
CL_BloodPoolInto

Merges a landing droplet into a splat already on the same surface, if there is
one close enough, and returns true when it did.

Areas add rather than radii: two equal droplets merging give a disc sqrt(2) times
wider, not twice, which is what "the same blood, spread out" actually looks like.
Adding radii instead makes a couple of hits balloon into a pond.

The normal test keeps a splat on the floor from swallowing one on the wall it
meets at the skirting.
===============
*/
static bool CL_BloodPoolInto(cparticle_t *p, const vec3_t point, const vec3_t normal)
{
    if (!cl_blood_pool->integer)
        return false;

    const float max_radius = cl_blood_sphere_radius->value * max(1.f, cl_blood_pool_max->value);

    for (int i = 0; i < num_blood_splats; i++) {
        cparticle_t *other = blood_splats[i];

        if (other->radius >= max_radius)
            continue;

        // Same surface, roughly: within about 25 degrees.
        if (DotProduct(other->blood_normal, normal) < 0.9f)
            continue;

        vec3_t delta;
        VectorSubtract(point, other->org, delta);

        // Touching, measured against the spread of the discs rather than their
        // droplet radii - a flattened splat covers much more ground than its
        // radius suggests, and that spread is what has to overlap for a merge to
        // look like a merge.
        //
        // BOTH spreads count. Testing only the existing splat's was the reason
        // pooling almost never fired: a fresh droplet landing just outside an
        // existing disc is visually touching it, but was measured as a miss.
        // Deliberately NOT scaled by the splat spread (1/sqrt(flatten)).
        //
        // It was, and that quietly coupled two unrelated knobs: flattening the
        // splats from 0.3 to 0.18 widened every merge radius by 29% as a side
        // effect, so asking for smooshier blood silently produced FEWER, bigger
        // pools. A reach in droplet widths is the thing that stays meaningful
        // when the splat shape changes, so cl_blood_pool_dist now means the same
        // thing at every flatness.
        const float reach = (other->radius + p->radius) * cl_blood_pool_dist->value;

        if (DotProduct(delta, delta) > reach * reach)
            continue;

        other->radius = sqrtf(other->radius * other->radius + p->radius * p->radius);
        if (other->radius > max_radius)
            other->radius = max_radius;

        // A pool that is still being fed should not age out mid-fight.
        other->time = cl.time;
        other->alpha = 1.0f;

        return true;
    }

    return false;
}

/*
===============
CL_BloodImpactSound

One wet splat for a droplet that has just landed, rate limited hard - see
cl_blood_sound_gap for why that matters more than anything else here.
===============
*/
static void CL_BloodImpactSound(const vec3_t point)
{
    static int last_time;

    if (!cl_blood_sound->integer || !cl_sfx_blood_splat[0])
        return;

    // cl.time can jump backwards on a map change or a demo seek, which would
    // otherwise latch the gap shut until the clock caught up again.
    if (cl.time < last_time)
        last_time = 0;

    if (cl.time - last_time < cl_blood_sound_gap->integer)
        return;

    vec3_t delta;
    VectorSubtract(point, listener_origin, delta);

    const float max_dist = cl_blood_sound_dist->value;
    if (max_dist > 0.f && VectorLength(delta) > max_dist)
        return;

    last_time = cl.time;

    const int i = Q_rand() % NUM_BLOOD_SFX;

    // entnum 0 with a world origin: a positioned sound that belongs to no
    // entity, so it will not cut off a sound another entity is playing.
    S_StartSound(point, 0, CHAN_AUTO, cl_sfx_blood_splat[i],
        cl_blood_sound_volume->value, ATTN_NORM, 0);
}

static void CL_BloodStick(cparticle_t *p, const vec3_t point, const vec3_t normal)
{
    CL_BloodImpactSound(point);

    // The direction it was travelling, flattened into the surface. Captured
    // BEFORE the velocity is cut down below, because that is what the splat's
    // elongation has to point along.
    vec3_t along;
    float into = DotProduct(p->vel, normal);
    VectorMA(p->vel, -into, normal, along);

    const float along_len = VectorNormalize(along);
    const float speed = VectorLength(p->vel);

    // Only a glancing hit leaves a directional mark. A droplet arriving straight
    // on has nothing to smear along, and stretching it would invent a direction
    // the impact never had.
    const float glance = (speed > 1.f) ? min(1.f, along_len / speed) : 0.f;

    if (glance > 0.25f && cl_blood_stretch->value > 1.f) {
        VectorCopy(along, p->blood_tangent);
        // Ramp with how glancing the hit was, so the mark lengthens as the angle
        // shallows instead of snapping between round and stretched.
        p->blood_stretch = 1.f + (cl_blood_stretch->value - 1.f) * glance;
    } else {
        VectorClear(p->blood_tangent);
        p->blood_stretch = 1.f;
    }

    // A square hit spreads outward instead of along. Scaled by how head-on it
    // was, so this and the stretch above hand over to each other rather than
    // both applying to the same impact.
    if (cl_blood_splash->value > 1.f)
        p->radius *= 1.f + (cl_blood_splash->value - 1.f) * (1.f - glance);

    p->blood_state = BLOOD_STUCK;
    VectorCopy(normal, p->blood_normal);
    VectorMA(point, BLOOD_SURFACE_OFFSET, normal, p->org);

    // THE SPLAT DOES NOT SKID. It used to keep a quarter of its tangential
    // velocity, so a droplet arriving at an angle slid across the floor for a
    // while before friction stopped it, and that was wrong twice over.
    //
    // Wrong to look at: blood hitting a floor does not travel as an intact blob,
    // it spreads. The direction it was going is already expressed - the splat is
    // elongated along it, in place, by the stretch computed above - so sliding
    // said the same thing a second time and said it badly.
    //
    // And expensive: the renderer caches a splat's generated geometry and reuses
    // it while the droplet is unchanged. A sliding splat moves every frame, so it
    // missed that cache every frame and paid the full per-vertex rebuild for the
    // whole skid - which is exactly the slowdown just after a burst lands.
    //
    // Zeroing it does NOT stop blood running down walls: on a vertical surface
    // gravity still projects into the plane below and accelerates the droplet
    // from rest. It only removes the sideways skid it should never have had.
    VectorClear(p->vel);

    p->blood_flatten = max(0.05f, min(cl_blood_flatten->value, 1.0f));

    // A splat should outlast the spray that made it.  Restart the fade so the
    // remaining life is measured from the impact rather than from the shot.
    p->time = cl.time;
    p->alpha = 1.0f;
    p->alphavel = -1.0f / max(0.1f, cl_blood_splat_life->value);
}

// Returns true when the droplet merged into an existing pool and should be
// retired - the blood it carried is now part of that splat.
static bool CL_SimulateBloodSphere(cparticle_t *p, float dt)
{
    trace_t tr;
    vec3_t  end;

    if (p->blood_state == BLOOD_STUCK) {
        // Gravity, projected into the plane of the surface.  On a floor this
        // cancels to nothing; on a wall it is a downward run; on a slope it is
        // the component that makes a droplet track downhill.
        vec3_t accel = { 0.f, 0.f, -cl_blood_gravity->value };
        float into = DotProduct(accel, p->blood_normal);
        VectorMA(accel, -into, p->blood_normal, accel);

        VectorMA(p->vel, dt, accel, p->vel);
        VectorScale(p->vel, max(0.f, 1.f - cl_blood_slide->value * dt), p->vel);

        if (VectorLength(p->vel) < BLOOD_SLIDE_EPSILON) {
            VectorClear(p->vel);
            return false;   // parked - nothing to trace
        }

        VectorMA(p->org, dt, p->vel, end);

        // Re-attach: trace from just off the surface into it.  A hit keeps the
        // droplet on the wall and picks up the new normal, which is what carries
        // it around a corner.  A miss means it has run off an edge.
        vec3_t probe_start, probe_end;
        VectorMA(end, 1.0f, p->blood_normal, probe_start);
        VectorMA(end, -2.0f, p->blood_normal, probe_end);

        blood_traces++;
        tr = CL_TracePoint(probe_start, probe_end, MASK_SOLID);

        if (tr.fraction < 1.0f && !tr.allsolid) {
            VectorCopy(tr.plane.normal, p->blood_normal);
            VectorMA(tr.endpos, BLOOD_SURFACE_OFFSET, p->blood_normal, p->org);
        } else {
            // Ran off the end of the surface - fall again.
            p->blood_state = BLOOD_AIRBORNE;
            p->blood_flatten = 1.0f;
            p->blood_stretch = 1.0f;
            VectorClear(p->blood_tangent);
            VectorCopy(end, p->org);

            // Back in the air, so it should once again persist until it lands
            // rather than run out the splat's clock.
            p->time = cl.time;
            p->alpha = 1.0f;
            p->alphavel = -1.0f / max(0.1f, cl_blood_air_life->value);
        }

        return false;
    }

    // Airborne: integrate, then trace the step we just took.
    p->vel[2] -= cl_blood_gravity->value * dt;
    VectorMA(p->org, dt, p->vel, end);

    blood_traces++;
    tr = CL_TracePoint(p->org, end, MASK_SOLID);

    if (tr.allsolid || tr.startsolid) {
        // Spawned inside geometry - a wound right against a wall. Leave it where
        // it is rather than teleporting it to a surface it never touched.
        VectorCopy(end, p->org);
        return false;
    }

    if (tr.fraction < 1.0f) {
        if (CL_BloodPoolInto(p, tr.endpos, tr.plane.normal))
            return true;

        CL_BloodStick(p, tr.endpos, tr.plane.normal);
    } else {
        VectorCopy(end, p->org);
    }

    return false;
}

/*
===============
CL_MakeBloodSphere

Turn a particle that has already been given its position, velocity and life into
a shaded sphere droplet.  Only the PRESENTATION changes - every spawn site keeps
its own tuned cone, speed and lifetime, so flipping cl_blood_spheres compares
like with like.

Read at SPAWN rather than at draw time, so toggling the cvar mid-fight leaves the
droplets already in the air alone instead of making them change form in place.
===============
*/
void CL_MakeBloodSphere(cparticle_t *p, float scale)
{
    if (!cl_blood_spheres->integer)
        return;

    // Counted optimistically rather than measured, because a single burst spawns
    // sixty droplets between two frames and the measured count would not move
    // until the next one - so the budget has to tighten as the burst is created,
    // not a frame later.
    if (num_blood_live >= cl_blood_max->integer) {
        // Over budget. Retire the particle rather than leaving it as an ordinary
        // one: returning early used to hand it back to the legacy quad path, so
        // a heavy fight visibly switched between shaded droplets and flat
        // sprites mid-stream, which reads as the effect breaking. Zeroing alpha
        // makes CL_AddParticles free it on its next pass, through the existing
        // faded-out branch that runs BEFORE the relink - the only place it is
        // safe to free from.
        p->alpha = 0.0f;
        p->alphavel = 0.0f;
        return;
    }

    p->blood_slot = CL_AllocBloodSlot();
    if (p->blood_slot < 0) {
        // No geometry slot free. Same treatment as being over budget: retire it
        // rather than let it fall back to the legacy quad.
        p->alpha = 0.0f;
        p->alphavel = 0.0f;
        return;
    }

    num_blood_live++;

    p->is_blood_sphere = true;
    p->blood_state = BLOOD_AIRBORNE;
    p->blood_flatten = 1.0f;
    p->blood_stretch = 1.0f;
    VectorClear(p->blood_normal);
    VectorClear(p->blood_tangent);

    // With collision on, a droplet does not fade in flight at all - it persists
    // until it hits something, and only then starts the splat's clock (see
    // CL_BloodStick). The spray's own 0.5-0.8 s life was tuned for particles
    // that merely fade in the air, and from a wound about 40 units up free fall
    // alone takes ~0.8 s at PARTICLE_GRAVITY, so droplets were expiring at the
    // very moment they arrived: 7 of 120 ever landed.
    //
    // cl_blood_air_life is therefore a backstop rather than a fade - it reclaims
    // blood thrown somewhere it can never land, not blood still on its way down.
    if (cl_blood_collision->integer)
        p->alphavel = -1.0f / max(0.1f, cl_blood_air_life->value);
    p->radius = cl_blood_sphere_radius->value * scale * (0.7f + frand() * 0.6f);
    p->seed = frand() * 64.f;
    VectorCopy(p->org, p->prev_org);
}

void CL_BloodParticleEffect(const vec3_t org, const vec3_t dir, int color, int count)
{
    int         i;
    cparticle_t *p;

    // add decal:
    decal_t dec = {
      .pos = {org[0],org[1],org[2]},
      .dir = {dir[0],dir[1],dir[2]},
      .spread = 0.25f,
      .length = 350};
    R_AddDecal(&dec);

    // a proper frame around the impact direction - see CL_PerpendicularBasis.
    // These used to be permutations of dir's own components, which for some
    // directions pointed straight along dir, collapsing the spray.
    vec3_t a, b;
    CL_PerpendicularBasis(dir, a, b);

    count *= cl_particle_num_factor->value;

    // `dir` is the surface normal, so it points back out of the wound towards
    // whoever fired - which is the way blood should leave the body.
    //
    // This used to PLACE the particles along that vector instead of throwing
    // them along it: `d = (Q_rand() & 31) * 10.0f` strung them out over 310
    // units - ten times the 31 the original game used - and gave them a
    // velocity of only `10*dir + crand()*20`, which is no coherent motion at
    // all. Forty particles smeared down a 310 unit line puts one or two in
    // view near the wound and buries the rest in whatever is behind the
    // monster, which is exactly the "barely any particles, and they do not
    // spray" that this looked like.
    //
    // So: spawn them all AT the wound inside a cone, and give them real
    // outward speed. Same construction as CL_ParticleEffect above - the offset
    // from the impact point is what defines each particle's direction - just
    // wider and slower, because blood spatters where debris ricochets.
    //
    // Speed is deliberately modest. The spray leaves the wound towards the
    // shooter, so anything much faster than this crosses the gap and sails
    // past the camera - and a particle a few units from the eye is drawn as a
    // screen-filling red blob. 35-110 units/sec over a ~0.7 s life keeps the
    // spray on and around the body, which is where it should be.
    const float blood_spread = 2.2f;
    const float blood_push = 2.0f;
    const float blood_push_rand = 1.5f;
    const float blood_base_velocity = 35.0f;
    const float blood_rand_velocity = 75.0f;

    for (i = 0; i < count; i++) {
        p = CL_AllocParticle();
        if (!p)
            return;

        p->time = cl.time;

        p->color = color + (Q_rand() & 7);
        // ludicrous gibs make blood self-lit enough to glow in the path tracer
        p->brightness = cl_ludicrous_gibs->integer ? 10.0f : 0.5f;

        vec3_t origin;
        VectorCopy(org, origin);
        VectorMA(origin, blood_spread * crand(), a, origin);
        VectorMA(origin, blood_spread * crand(), b, origin);
        VectorMA(origin, blood_push + blood_push_rand * frand(), dir, origin);
        VectorCopy(origin, p->org);

        vec3_t velocity;
        VectorSubtract(origin, org, velocity);
        VectorNormalize(velocity);

        // Only the simulated path needs the boost - the analytic one still runs
        // under PARTICLE_GRAVITY and its original tuning holds.
        const float speed_scale = (cl_blood_spheres->integer && cl_blood_collision->integer)
            ? max(0.1f, cl_blood_speed->value) : 1.0f;

        VectorScale(velocity,
            (blood_base_velocity + frand() * blood_rand_velocity) * speed_scale, p->vel);

        p->accel[0] = p->accel[1] = 0;
        p->accel[2] = -PARTICLE_GRAVITY;
        p->alpha = 1.0f;

        p->alphavel = -1.0f / (0.5f + frand() * 0.3f);

        CL_MakeBloodSphere(p, 1.0f);
    }
}


/*
===============
CL_ParticleEffect2
===============
*/
void CL_ParticleEffect2(const vec3_t org, const vec3_t dir, int color, int count)
{
    int         i, j;
    cparticle_t *p;
    float       d;

    count *= cl_particle_num_factor->value;

    for (i = 0; i < count; i++) {
        p = CL_AllocParticle();
        if (!p)
            return;

        p->time = cl.time;

        p->color = color;
		p->brightness = 1.0f;

        d = Q_rand() & 7;
        for (j = 0; j < 3; j++) {
            p->org[j] = org[j] + ((int)(Q_rand() & 7) - 4) + d * dir[j];
            p->vel[j] = crand() * 20;
        }

        p->accel[0] = p->accel[1] = 0;
        p->accel[2] = -PARTICLE_GRAVITY;
        p->alpha = 1.0f;

        p->alphavel = -1.0f / (0.5f + frand() * 0.3f);
    }
}


/*
===============
CL_TeleporterParticles
===============
*/
void CL_TeleporterParticles(const vec3_t org)
{
    int         i, j;
    cparticle_t *p;

    const int count = 8 * cl_particle_num_factor->value;

    for (i = 0; i < count; i++) {
        p = CL_AllocParticle();
        if (!p)
            return;

        p->time = cl.time;

        p->color = 0xdb;
		p->brightness = 1.0f;

        for (j = 0; j < 2; j++) {
            p->org[j] = org[j] - 16 + (Q_rand() & 31);
            p->vel[j] = crand() * 14;
        }

        p->org[2] = org[2] - 8 + (Q_rand() & 7);
        p->vel[2] = 80 + (Q_rand() & 7);

        p->accel[0] = p->accel[1] = 0;
        p->accel[2] = -PARTICLE_GRAVITY;
        p->alpha = 1.0f;

        p->alphavel = -0.5f;
    }
}


/*
===============
CL_LogoutEffect

===============
*/
static void CL_LogoutEffect(const vec3_t org, int type)
{
    int         i, j;
    cparticle_t *p;

    for (i = 0; i < 500; i++) {
        p = CL_AllocParticle();
        if (!p)
            return;

        p->time = cl.time;

        int color;
        if (type == MZ_LOGIN)
            color = 0xd0 + (Q_rand() & 7); // green
        else if (type == MZ_LOGOUT)
            color = 0x40 + (Q_rand() & 7); // red
        else
            color = 0xe0 + (Q_rand() & 7); // yellow

        p->color = color;
		p->brightness = 1.0f;

        p->org[0] = org[0] - 16 + frand() * 32;
        p->org[1] = org[1] - 16 + frand() * 32;
        p->org[2] = org[2] - 24 + frand() * 56;

        for (j = 0; j < 3; j++)
            p->vel[j] = crand() * 20;

        p->accel[0] = p->accel[1] = 0;
        p->accel[2] = -PARTICLE_GRAVITY;
        p->alpha = 1.0f;

        p->alphavel = -1.0f / (1.0f + frand() * 0.3f);
    }
}


/*
===============
CL_ItemRespawnParticles

===============
*/
void CL_ItemRespawnParticles(const vec3_t org)
{
    int         i, j;
    cparticle_t *p;

    const int count = 64 * cl_particle_num_factor->value;

    for (i = 0; i < count; i++) {
        p = CL_AllocParticle();
        if (!p)
            return;

        p->time = cl.time;

        p->color = 0xd4 + (Q_rand() & 3); // green
		p->brightness = 1.0f;

        p->org[0] = org[0] + crand() * 8;
        p->org[1] = org[1] + crand() * 8;
        p->org[2] = org[2] + crand() * 8;

        for (j = 0; j < 3; j++)
            p->vel[j] = crand() * 8;

        p->accel[0] = p->accel[1] = 0;
        p->accel[2] = -PARTICLE_GRAVITY * 0.2f;
        p->alpha = 1.0f;

        p->alphavel = -1.0f / (1.0f + frand() * 0.3f);
    }
}


/*
===============
CL_ExplosionParticles
===============
*/
void CL_ExplosionParticles(const vec3_t org)
{
    int         i, j;
    cparticle_t *p;

    const int count = 256 * cl_particle_num_factor->value;

    for (i = 0; i < count; i++) {
        p = CL_AllocParticle();
        if (!p)
            return;

        p->time = cl.time;

        p->color = 0xe0 + (Q_rand() & 7);
		p->brightness = cvar_pt_particle_emissive->value;

        for (j = 0; j < 3; j++) {
            p->org[j] = org[j] + ((int)(Q_rand() % 32) - 16);
            p->vel[j] = (int)(Q_rand() % 384) - 192;
        }

        p->accel[0] = p->accel[1] = 0;
        p->accel[2] = -PARTICLE_GRAVITY;
        p->alpha = 1.0f;

        p->alphavel = -0.8f / (0.5f + frand() * 0.3f);
    }
}

/*
===============
CL_BigTeleportParticles
===============
*/
void CL_BigTeleportParticles(const vec3_t org)
{
    static const byte   colortable[4] = {2 * 8, 13 * 8, 21 * 8, 18 * 8};
    int         i;
    cparticle_t *p;
    float       angle, dist;

    for (i = 0; i < 4096; i++) {
        p = CL_AllocParticle();
        if (!p)
            return;

        p->time = cl.time;

        p->color = colortable[Q_rand() & 3];
		p->brightness = 1.0f;

        angle = (Q_rand() & 1023) * (M_PI * 2 / 1023);
        dist = Q_rand() & 31;
        p->org[0] = org[0] + cos(angle) * dist;
        p->vel[0] = cos(angle) * (70 + (Q_rand() & 63));
        p->accel[0] = -cos(angle) * 100;

        p->org[1] = org[1] + sin(angle) * dist;
        p->vel[1] = sin(angle) * (70 + (Q_rand() & 63));
        p->accel[1] = -sin(angle) * 100;

        p->org[2] = org[2] + 8 + (Q_rand() % 90);
        p->vel[2] = -100 + (Q_rand() & 31);
        p->accel[2] = PARTICLE_GRAVITY * 4;
        p->alpha = 1.0f;

        p->alphavel = -0.3f / (0.5f + frand() * 0.3f);
    }
}


/*
===============
CL_BlasterParticles

Wall impact puffs
===============
*/
void CL_BlasterParticles(const vec3_t org, const vec3_t dir)
{
    int         i, j;
    cparticle_t *p;
    float       d;

    const int count = 40 * cl_particle_num_factor->value;

    for (i = 0; i < count; i++) {
        p = CL_AllocParticle();
        if (!p)
            return;

        p->time = cl.time;

        p->color = 0xe0 + (Q_rand() & 7);
		p->brightness = cvar_pt_particle_emissive->value;

        d = Q_rand() & 15;
        for (j = 0; j < 3; j++) {
            p->org[j] = org[j] + ((int)(Q_rand() & 7) - 4) + d * dir[j];
            p->vel[j] = dir[j] * 30 + crand() * 40;
        }

        p->accel[0] = p->accel[1] = 0;
        p->accel[2] = -PARTICLE_GRAVITY;
        p->alpha = 1.0f;

        p->alphavel = -1.0f / (0.5f + frand() * 0.3f);
    }
}


/*
===============
CL_BlasterTrail

===============
*/
void CL_BlasterTrail(const vec3_t start, const vec3_t end)
{
    vec3_t      move;
    vec3_t      vec;
    float       len;
    int         j;
    cparticle_t *p;
    int         dec;

    VectorCopy(start, move);
    VectorSubtract(end, start, vec);
    len = VectorNormalize(vec);

    dec = 5;
    VectorScale(vec, 5, vec);


    // FIXME: this is a really silly way to have a loop
    while (len > 0) {
        len -= dec;

        p = CL_AllocParticle();
        if (!p)
            return;
        VectorClear(p->accel);

        p->time = cl.time;

        p->alpha = 1.0;
        p->alphavel = -1.0f / (0.3f + frand() * 0.2f);

        p->color = cl_blaster_color->integer ? 108 : 0xe0;
		p->brightness = cvar_pt_particle_emissive->value;
    
		
        for (j = 0; j < 3; j++) {
            p->org[j] = move[j] + crand();
            p->vel[j] = crand() * 5;
            p->accel[j] = 0;
        }

        VectorAdd(move, vec, move);
    }
}

void CL_HyoerBlasterTrail(vec3_t start, vec3_t end)
{
	vec3_t      move;
	vec3_t      vec;
	float       len;
	int         j;
	cparticle_t *p;
	int         dec;

	VectorCopy(start, move);
	VectorSubtract(end, start, vec);
	len = VectorNormalize(vec);

	dec = 5;
	VectorScale(vec, 5, vec);

	// FIXME: this is a really silly way to have a loop
	while (len > 0) {
		len -= dec;

		p = CL_AllocParticle();
		if (!p)
			return;
		VectorClear(p->accel);

		p->time = cl.time;

		p->alpha = 1.0;
		p->alphavel = -1.0 / (0.3 + frand() * 0.2);

		p->color = 0xe0;
		p->brightness = cvar_pt_particle_emissive->value;

		for (j = 0; j < 3; j++) {
			p->org[j] = move[j] + crand();
			p->vel[j] = crand() * 5;
			p->accel[j] = 0;
		}

		VectorAdd(move, vec, move);
	}
}

/*
===============
CL_FlagTrail

===============
*/
void CL_FlagTrail(const vec3_t start, const vec3_t end, int color)
{
    vec3_t      move;
    vec3_t      vec;
    float       len;
    int         j;
    cparticle_t *p;
    int         dec;

    VectorCopy(start, move);
    VectorSubtract(end, start, vec);
    len = VectorNormalize(vec);

    dec = 5;
    VectorScale(vec, 5, vec);

    while (len > 0) {
        len -= dec;

        p = CL_AllocParticle();
        if (!p)
            return;
        VectorClear(p->accel);

        p->time = cl.time;

        p->alpha = 1.0;
        p->alphavel = -1.0f / (0.8f + frand() * 0.2f);

        p->color = color;
		p->brightness = 1.0f;

        for (j = 0; j < 3; j++) {
            p->org[j] = move[j] + crand() * 16;
            p->vel[j] = crand() * 5;
            p->accel[j] = 0;
        }

        VectorAdd(move, vec, move);
    }
}

/*
===============
CL_DiminishingTrail

===============
*/
void CL_DiminishingTrail(const vec3_t start, const vec3_t end, centity_t *old, int flags)
{
    vec3_t      move;
    vec3_t      vec;
    float       len;
    int         j;
    cparticle_t *p;
    float       dec;
    float       orgscale;
    float       velscale;

    VectorCopy(start, move);
    VectorSubtract(end, start, vec);
    len = VectorNormalize(vec);

    dec = 0.5f;
    VectorScale(vec, dec, vec);

    if (old->trailcount > 900) {
        orgscale = 4;
        velscale = 15;
    } else if (old->trailcount > 800) {
        orgscale = 2;
        velscale = 10;
    } else {
        orgscale = 1;
        velscale = 5;
    }

    while (len > 0) {
        len -= dec;

        // drop less particles as it flies
        if ((Q_rand() & 1023) < old->trailcount) {
            p = CL_AllocParticle();
            if (!p)
                return;
            VectorClear(p->accel);

            p->time = cl.time;

            if (flags & EF_GIB) {
                p->alpha = 1.0;
                p->alphavel = -1.0f / (1 + frand() * 0.4f);

                p->color = 0xe8 + (Q_rand() & 7);
				p->brightness = 1.0f;

                for (j = 0; j < 3; j++) {
                    p->org[j] = move[j] + crand() * orgscale;
                    p->vel[j] = crand() * velscale;
                    p->accel[j] = 0;
                }
                p->vel[2] -= PARTICLE_GRAVITY;

                // Gib blood is a DRIP, not a wound spray - smaller than the
                // droplets CL_BloodParticleEffect throws.
                CL_MakeBloodSphere(p, 0.6f);
            } else if (flags & EF_GREENGIB) {
                p->alpha = 1.0f;
                p->alphavel = -1.0f / (1 + frand() * 0.4f);

                p->color = 0xdb + (Q_rand() & 7);
				p->brightness = 1.0f;

                for (j = 0; j < 3; j++) {
                    p->org[j] = move[j] + crand() * orgscale;
                    p->vel[j] = crand() * velscale;
                    p->accel[j] = 0;
                }
                p->vel[2] -= PARTICLE_GRAVITY;

                CL_MakeBloodSphere(p, 0.6f);
            } else {
                p->alpha = 1.0f;
                p->alphavel = -1.0f / (1 + frand() * 0.2f);

                p->color = 4 + (Q_rand() & 7);
				p->brightness = 1.0f;

                for (j = 0; j < 3; j++) {
                    p->org[j] = move[j] + crand() * orgscale;
                    p->vel[j] = crand() * velscale;
                }
                p->accel[2] = 20;
            }
        }

        old->trailcount -= 5;
        if (old->trailcount < 100)
            old->trailcount = 100;

        VectorAdd(move, vec, move);
    }
}

void CL_NonDiminishingTrail(vec3_t start, vec3_t end, centity_t *old, int flags)
{
	vec3_t      move;
	vec3_t      vec;
	float       len;
	int         j;
	cparticle_t *p;
	float       dec;
	float       orgscale;
	float       velscale;
	float valueTest;

	VectorCopy(start, move);
	VectorSubtract(end, start, vec);
	len = VectorNormalize(vec);

	dec = 0.5;
	VectorScale(vec, dec, vec);

	if (old->trailcount > 900) {
		orgscale = 4;
		velscale = 15;
	}
	else if (old->trailcount > 800) {
		orgscale = 2;
		velscale = 10;
	}
	else {
		orgscale = 1;
		velscale = 5;
	}

		len = len * orgscale;
	while (len > 0) {
		len -= dec;

		// drop less particles as it flies
	
		p = CL_AllocParticle();
        p->particleType = PARTICLE_TYPE_SHORT_LIVED;
		if (!p)
			return;
		VectorClear(p->accel);

		p->time = cl.time;

		if (flags & EF_GIB) {
			p->alpha = 1.0;
			valueTest = (4.0 + frand() * 0.4);			
			p->alphavel = -1.0 / valueTest;
			p->color = 235 + (rand() % 5);
			p->brightness = 1.0f;

			for (j = 0; j < 3; j++) {
				p->org[j] = move[j] + crand();
				p->vel[j] = crand() * velscale;
				p->accel[j] = 0;
			}
			p->vel[2] -= PARTICLE_GRAVITY;

			CL_MakeBloodSphere(p, 0.6f);
		}
		else if (flags & EF_GREENGIB) {
			p->alpha = 1.0;
			p->alphavel = -1.0 / (1 + frand() * 0.4);

			p->color = 0xdb + (rand() & 7);
			p->brightness = 10.0f;

			for (j = 0; j < 3; j++) {
				p->org[j] = move[j] + crand() * orgscale;
				p->vel[j] = crand() * velscale;
				p->accel[j] = 0;
			}
			p->vel[2] -= PARTICLE_GRAVITY;

			CL_MakeBloodSphere(p, 0.6f);
		}
		else {
			p->alpha = 1.0;
			p->alphavel = -1.0 / (1 + frand() * 0.2);

			p->color = 4 + (rand() & 7);
			p->brightness = 1.0f;

			for (j = 0; j < 3; j++) {
				p->org[j] = move[j] + crand() * orgscale;
				p->vel[j] = crand() * velscale;
			}
			p->accel[2] = 20;
		}

        
	}

		
	
}

/*
===============
CL_RocketTrail

===============
*/
void CL_RocketTrail(const vec3_t start, const vec3_t end, centity_t *old)
{
    vec3_t      move;
    vec3_t      vec;
    float       len;
    int         j;
    cparticle_t *p;
    float       dec;

    // smoke
    CL_DiminishingTrail(start, end, old, EF_ROCKET);

    // fire
    VectorCopy(start, move);
    VectorSubtract(end, start, vec);
    len = VectorNormalize(vec);

    dec = 1;
    VectorScale(vec, dec, vec);

    while (len > 0) {
        len -= dec;

        if ((Q_rand() & 7) == 0) {
            p = CL_AllocParticle();
            if (!p)
                return;

            VectorClear(p->accel);
            p->time = cl.time;

            p->alpha = 1.0;
            p->alphavel = -1.0f / (1 + frand() * 0.2f);

            p->color = 0xdc + (Q_rand() & 3);
			p->brightness = cvar_pt_particle_emissive->value;

            for (j = 0; j < 3; j++) {
                p->org[j] = move[j] + crand() * 5;
                p->vel[j] = crand() * 20;
            }
            p->accel[2] = -PARTICLE_GRAVITY;
        }
        VectorAdd(move, vec, move);
    }
}

/*
===============
CL_RailTrail

===============
*/
void CL_OldRailTrail(void)
{
    vec3_t      move;
    vec3_t      vec;
    float       len;
    int         j;
    cparticle_t *p;
    float       dec;
    vec3_t      right, up;
    int         i;
    float       d, c, s;
    vec3_t      dir;
    byte        clr = 0x74;

    VectorCopy(te.pos1, move);
    VectorSubtract(te.pos2, te.pos1, vec);
    len = VectorNormalize(vec);

    MakeNormalVectors(vec, right, up);

    for (i = 0; i < len; i++) {
        p = CL_AllocParticle();
        if (!p)
            return;

        p->time = cl.time;
        VectorClear(p->accel);

        d = i * 0.1;
        c = cos(d);
        s = sin(d);

        VectorScale(right, c, dir);
        VectorMA(dir, s, up, dir);

        p->alpha = 1.0;
        p->alphavel = -1.0f / (1 + frand() * 0.2f);

        p->color = clr + (Q_rand() & 7);
		p->brightness = cvar_pt_particle_emissive->value;

        for (j = 0; j < 3; j++) {
            p->org[j] = move[j] + dir[j] * 3;
            p->vel[j] = dir[j] * 6;
        }

        VectorAdd(move, vec, move);
    }

    dec = 0.75f;
    VectorScale(vec, dec, vec);
    VectorCopy(te.pos1, move);

    while (len > 0) {
        len -= dec;

        p = CL_AllocParticle();
        if (!p)
            return;

        p->time = cl.time;
        VectorClear(p->accel);

        p->alpha = 1.0;
        p->alphavel = -1.0f / (0.6f + frand() * 0.2f);

        p->color = Q_rand() & 15;
		p->brightness = 1.0f;

        for (j = 0; j < 3; j++) {
            p->org[j] = move[j] + crand() * 3;
            p->vel[j] = crand() * 3;
            p->accel[j] = 0;
        }

        VectorAdd(move, vec, move);
    }
}


/*
===============
CL_BubbleTrail

===============
*/
void CL_BubbleTrail(const vec3_t start, const vec3_t end)
{
    vec3_t      move;
    vec3_t      vec;
    float       len;
    int         i, j;
    cparticle_t *p;
    float       dec;

    VectorCopy(start, move);
    VectorSubtract(end, start, vec);
    len = VectorNormalize(vec);

    dec = 32;
    VectorScale(vec, dec, vec);

    for (i = 0; i < len; i += dec) {
        p = CL_AllocParticle();
        if (!p)
            return;

        VectorClear(p->accel);
        p->time = cl.time;

        p->alpha = 1.0;
        p->alphavel = -1.0f / (1 + frand() * 0.2);

        p->color = 4 + (Q_rand() & 7);
		p->brightness = 1.0f;

        for (j = 0; j < 3; j++) {
            p->org[j] = move[j] + crand() * 2;
            p->vel[j] = crand() * 5;
        }
        p->vel[2] += 6;

        VectorAdd(move, vec, move);
    }
}


/*
===============
CL_FlyParticles
===============
*/

#define BEAMLENGTH  16

static void CL_FlyParticles(const vec3_t origin, int count)
{
    int         i;
    cparticle_t *p;
    float       angle;
    float       sp, sy, cp, cy;
    vec3_t      forward;
    float       dist;
    float       ltime;

    if (count > NUMVERTEXNORMALS)
        count = NUMVERTEXNORMALS;

    ltime = cl.time * 0.001f;
    for (i = 0; i < count; i += 2) {
        angle = ltime * avelocities[i][0];
        sy = sin(angle);
        cy = cos(angle);
        angle = ltime * avelocities[i][1];
        sp = sin(angle);
        cp = cos(angle);

        forward[0] = cp * cy;
        forward[1] = cp * sy;
        forward[2] = -sp;

        p = CL_AllocParticle();
        if (!p)
            return;

        p->time = cl.time;

        dist = sin(ltime + i) * 64;
        p->org[0] = origin[0] + bytedirs[i][0] * dist + forward[0] * BEAMLENGTH;
        p->org[1] = origin[1] + bytedirs[i][1] * dist + forward[1] * BEAMLENGTH;
        p->org[2] = origin[2] + bytedirs[i][2] * dist + forward[2] * BEAMLENGTH;

        VectorClear(p->vel);
        VectorClear(p->accel);

        p->color = 0;
		p->brightness = 1.0f;

        p->alpha = 1;
        p->alphavel = INSTANT_PARTICLE;
    }
}

void CL_FlyEffect(centity_t *ent, const vec3_t origin)
{
    int     n;
    int     count;
    int     starttime;

    if (ent->fly_stoptime < cl.time) {
        starttime = cl.time;
        ent->fly_stoptime = cl.time + 60000;
    } else {
        starttime = ent->fly_stoptime - 60000;
    }

    n = cl.time - starttime;
    if (n < 20000)
        count = n * NUMVERTEXNORMALS / 20000;
    else {
        n = ent->fly_stoptime - cl.time;
        if (n < 20000)
            count = n * NUMVERTEXNORMALS / 20000;
        else
            count = NUMVERTEXNORMALS;
    }

    CL_FlyParticles(origin, count);
}

/*
===============
CL_BfgParticles
===============
*/
void CL_BfgParticles(entity_t *ent)
{
    int         i;
    cparticle_t *p;
    float       angle;
    float       sp, sy, cp, cy;
    vec3_t      forward;
    float       dist;
    float       ltime;

    const int count = NUMVERTEXNORMALS * cl_particle_num_factor->value;

    ltime = cl.time * 0.001f;
    for (i = 0; i < count; i++) {
        angle = ltime * avelocities[i][0];
        sy = sin(angle);
        cy = cos(angle);
        angle = ltime * avelocities[i][1];
        sp = sin(angle);
        cp = cos(angle);

        forward[0] = cp * cy;
        forward[1] = cp * sy;
        forward[2] = -sp;

        p = CL_AllocParticle();
        if (!p)
            return;

        p->time = cl.time;

        dist = sin(ltime + i) * 64;
        p->org[0] = ent->origin[0] + bytedirs[i][0] * dist + forward[0] * BEAMLENGTH;
        p->org[1] = ent->origin[1] + bytedirs[i][1] * dist + forward[1] * BEAMLENGTH;
        p->org[2] = ent->origin[2] + bytedirs[i][2] * dist + forward[2] * BEAMLENGTH;

        VectorClear(p->vel);
        VectorClear(p->accel);

        dist = Distance(p->org, ent->origin) / 90.0f;
        p->color = floor(0xd0 + dist * 7);
		p->brightness = cvar_pt_particle_emissive->value;

        p->alpha = 1.0f - dist;
        p->alphavel = INSTANT_PARTICLE;
    }
}


/*
===============
CL_BFGExplosionParticles
===============
*/
//FIXME combined with CL_ExplosionParticles
void CL_BFGExplosionParticles(const vec3_t org)
{
    int         i, j;
    cparticle_t *p;

    const int count = 256 * cl_particle_num_factor->value;

    for (i = 0; i < count; i++) {
        p = CL_AllocParticle();
        if (!p)
            return;

        p->time = cl.time;

        p->color = 0xd0 + (Q_rand() & 7);
		p->brightness = cvar_pt_particle_emissive->value;

        for (j = 0; j < 3; j++) {
            p->org[j] = org[j] + ((int)(Q_rand() % 32) - 16);
            p->vel[j] = (int)(Q_rand() % 384) - 192;
        }

        p->accel[0] = p->accel[1] = 0;
        p->accel[2] = -PARTICLE_GRAVITY;
        p->alpha = 1.0f;

        p->alphavel = -0.8f / (0.5f + frand() * 0.3f);
    }
}


/*
===============
CL_TeleportParticles

===============
*/
void CL_TeleportParticles(const vec3_t org)
{
    int         i, j, k;
    cparticle_t *p;
    float       vel;
    vec3_t      dir;

    for (i = -16; i <= 16; i += 4)
        for (j = -16; j <= 16; j += 4)
            for (k = -16; k <= 32; k += 4) {
                p = CL_AllocParticle();
                if (!p)
                    return;

                p->time = cl.time;

                p->color = 7 + (Q_rand() & 7);
				p->brightness = 1.0f;

                p->alpha = 1.0f;
                p->alphavel = -1.0f / (0.3f + (Q_rand() & 7) * 0.02f);

                p->org[0] = org[0] + i + (Q_rand() & 3);
                p->org[1] = org[1] + j + (Q_rand() & 3);
                p->org[2] = org[2] + k + (Q_rand() & 3);

                dir[0] = j * 8;
                dir[1] = i * 8;
                dir[2] = k * 8;

                VectorNormalize(dir);
                vel = 50 + (Q_rand() & 63);
                VectorScale(dir, vel, p->vel);

                p->accel[0] = p->accel[1] = 0;
                p->accel[2] = -PARTICLE_GRAVITY;
            }
}

extern int          r_numparticles;
extern particle_t   r_particles[MAX_PARTICLES];

/*
===============
CL_AddParticles
===============
*/
void CL_AddParticles(void)
{
    cparticle_t     *p, *next;
    float           alpha;
    float           time = 0, time2;
    int             color;
    cparticle_t     *active, *tail;
    particle_t      *part;

    active = NULL;
    tail = NULL;

    // Timestep for the blood droplet simulation.  Taken from the client clock
    // rather than assumed, because this runs at whatever rate the client frame
    // does.  Clamped so that a hitch, a load or a paused console does not
    // teleport every droplet through the nearest wall in one enormous step.
    static int blood_sim_time;
    float blood_dt = (cl.time - blood_sim_time) * 0.001f;
    blood_sim_time = cl.time;
    blood_dt = max(0.f, min(blood_dt, 0.05f));

    num_blood_splats = 0;

    // Slots still in use are marked as the list is walked; whatever is left
    // unmarked at the end belonged to a droplet that has died. Reclaiming them
    // here rather than at each free site keeps it to one place, and there are
    // several ways a particle can be freed.
    memset(blood_slot_seen, 0, sizeof(blood_slot_seen));

    // Free-list health, reported by cl_blood_stats. The particle pool is shared
    // with every other effect in the game, so a leak here starves the blaster
    // and the trails long before it is obvious that blood is the cause.
    int blood_air = 0, blood_stuck = 0, num_free = 0, num_active = 0;

    // Time actually spent in the droplet simulation, and traces issued.
    //
    // This is the half timerefresh CANNOT see: it renders 128 frames without
    // running a client frame, so it measures the renderer and nothing else. The
    // per-droplet CL_TracePoint - a BSP trace plus a loop over every solid
    // entity, per droplet, per client frame - only happens in a real frame.
    static uint64_t blood_sim_usec_acc;
    static int      blood_trace_acc, blood_sim_frames;
    uint64_t blood_sim_usec = 0;
    blood_traces = 0;
    for (cparticle_t *f = free_particles; f; f = f->next)
        num_free++;

    for (p = active_particles; p; p = next) {
        next = p->next;

        if (p->alphavel != INSTANT_PARTICLE) {
            time = (cl.time - p->time) * 0.001f;
            alpha = p->alpha + time * p->alphavel;
            if (alpha <= 0) {
                // faded out
                p->next = free_particles;
                free_particles = p;
                continue;
            }

            // Blood spheres are exempt: the gib trail is SHORT_LIVED and is
            // also blood, and a splat's life is set from its impact by
            // CL_BloodStick, not from the two seconds this cull assumes.
            if (p->particleType == PARTICLE_TYPE_SHORT_LIVED && !p->is_blood_sphere
                && time > 2 /* && p->org[2] < cl.refdef.vieworg[2]*/) {
                p->next = free_particles;
                free_particles = p;
                continue;
            }
        } else {
            alpha = p->alpha;
        }

        // Blood droplets simulate BEFORE the particle is relinked below.
        //
        // THIS ORDERING IS LOAD-BEARING. A droplet that merges into a pool has
        // to be freed, and freeing one that has ALREADY been linked into the
        // rebuilt active list puts it on the active list and the free list at
        // the same time: `tail` still points at it, so the next iteration's
        // `tail->next = p` overwrites the `p->next` that was holding the head of
        // the free list. The free list is then spliced into the active list and
        // lost, and CL_AllocParticle starts handing out particles that are
        // simultaneously live. It presents as the particle pool exhausting
        // itself and as droplets behaving as though they belong to something
        // else - which is exactly what it did.
        //
        // Every other free-and-continue in this loop is above the relink for the
        // same reason. This one has to be too.
        const bool blood_sim = p->is_blood_sphere && cl_blood_collision->integer;

        uint64_t sim_t0 = blood_sim ? Sys_Microseconds() : 0;
        const bool absorbed = blood_sim && CL_SimulateBloodSphere(p, blood_dt);
        if (blood_sim)
            blood_sim_usec += Sys_Microseconds() - sim_t0;

        if (absorbed) {
            // Merged into a pool - the blood it carried is part of that splat
            // now, so retire it rather than drawing it on top of what it became.
            p->next = free_particles;
            free_particles = p;
            continue;
        }

        p->next = NULL;
        if (!tail)
            active = tail = p;
        else {
            tail->next = p;
            tail = p;
        }

        if (alpha > 1.0f)
            alpha = 1;
        color = p->color;

        time2 = time * time;

        vec3_t origin;

        num_active++;

        // Outside the blood_sim branch on purpose: a droplet owns its slot
        // whenever it is a blood sphere, not only when the collision simulation
        // is running. Marking it inside meant that with cl_blood_collision 0
        // every slot was reclaimed from under a live droplet, and two droplets
        // could then be handed the same geometry.
        if (p->is_blood_sphere && p->blood_slot >= 0 && p->blood_slot < MAX_BLOOD_SPHERES)
            blood_slot_seen[p->blood_slot] = true;

        if (blood_sim) {
            // Stepped, collided, stateful - see CL_SimulateBloodSphere. org is
            // the live position, so the analytic formula below does not apply.
            if (p->blood_state == BLOOD_STUCK) {
                blood_stuck++;
                if (num_blood_splats < MAX_PARTICLES)
                    blood_splats[num_blood_splats++] = p;
            } else {
                blood_air++;
            }

            VectorCopy(p->org, origin);
        } else {
            origin[0] = p->org[0] + p->vel[0] * time + p->accel[0] * time2;
            origin[1] = p->org[1] + p->vel[1] * time + p->accel[1] * time2;
            origin[2] = p->org[2] + p->vel[2] * time + p->accel[2] * time2;
        }

        if (p->is_blood_sphere) {
            // Sphere geometry rather than a quad. The droplet stays on the
            // active list either way, so it still ages and frees normally.
            blood_sphere_t *b = (p->blood_slot >= 0) ? V_AddBloodSphere() : NULL;
            if (b) {
                b->slot = p->blood_slot;
                VectorCopy(origin, b->origin);
                VectorCopy(p->prev_org, b->prev_origin);
                b->radius = p->radius;
                b->color = color;
                b->rgba = p->rgba;
                b->seed = p->seed;
                b->flatten = p->blood_flatten;
                VectorCopy(p->blood_normal, b->normal);
                b->stretch = p->blood_stretch;
                VectorCopy(p->blood_tangent, b->tangent);

                // Shrink away over the last of the life instead of vanishing.
                // Blood spheres do not use alpha for anything else - they are
                // opaque geometry - so without this a splat pops out of
                // existence, which is very visible when a floorful expire
                // together.
                //
                // QUANTIZED, and that is not cosmetic. The renderer caches a
                // splat's generated geometry and reuses it while the droplet is
                // unchanged; a radius that moves every frame misses that cache
                // every frame, so a continuous shrink put the whole per-vertex
                // rebuild back exactly when a floorful of splats faded together.
                // Twelve steps is under a pixel of movement per step at the size
                // these are drawn, and costs twelve rebuilds instead of hundreds.
                if (alpha < 0.25f) {
                    const float steps = 12.0f;
                    b->radius *= floorf(alpha * 4.0f * steps) / steps;
                }
            }
            VectorCopy(origin, p->prev_org);
        } else {
            if (r_numparticles >= MAX_PARTICLES)
                break;
            part = &r_particles[r_numparticles++];

            VectorCopy(origin, part->origin);

            part->rgba = p->rgba;
            part->color = color;
            part->brightness = p->brightness;
            part->alpha = alpha;
            part->radius = 0.f;
        }

        if (p->alphavel == INSTANT_PARTICLE) {
            p->alphavel = 0.0f;
            p->alpha = 0.0f;
        }
    }

    active_particles = active;

    for (int i = 0; i < MAX_BLOOD_SPHERES; i++)
        if (!blood_slot_seen[i])
            blood_slot_used[i] = false;

    num_blood_live = blood_air + blood_stuck;

    blood_sim_usec_acc += blood_sim_usec;
    blood_trace_acc += blood_traces;
    blood_sim_frames++;

    if (cl_blood_stats->integer) {
        static int last_report;
        if (cl.time - last_report > 1000 || cl.time < last_report) {
            last_report = cl.time;
            Com_Printf("blood: %d airborne, %d stuck | sim %.2f ms/frame, %d traces/frame | particles %d active, %d free\n",
                blood_air, blood_stuck,
                blood_sim_frames ? (float)blood_sim_usec_acc / blood_sim_frames / 1000.f : 0.f,
                blood_sim_frames ? blood_trace_acc / blood_sim_frames : 0,
                num_active, num_free);
            blood_sim_usec_acc = 0; blood_trace_acc = 0; blood_sim_frames = 0;
        }
    }
}


/*
==============
CL_ClearEffects

==============
*/
void CL_ClearEffects(void)
{
    CL_ClearLightStyles();
    CL_ClearParticles();
    CL_ClearDlights();
}

void CL_InitEffects(void)
{
    int i, j;

    cl_monster_flash_nudge = Cvar_Get("cl_monster_flash_nudge", "0", 0);
    cl_monster_flash_aim = Cvar_Get("cl_monster_flash_aim", "1", 0);

    for (i = 0; i < NUMVERTEXNORMALS; i++)
        for (j = 0; j < 3; j++)
            avelocities[i][j] = (Q_rand() & 255) * 0.01f;
}

