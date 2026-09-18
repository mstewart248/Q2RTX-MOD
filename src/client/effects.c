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
#include "refresh/models.h"

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

    // MZ_LOGIN, MZ_LOGOUT and MZ_RESPAWN are not gunfire. The game sends them
    // through svc_muzzleflash purely to get a coloured light and a particle
    // burst at a player who just appeared or left - p_client.c calls it "add a
    // teleportation effect" - and the switch below draws them that way. Putting
    // a muzzle flash MODEL on them lit up your own barrel the instant you
    // spawned, before you had fired anything.
    if (mz.weapon != MZ_LOGIN && mz.weapon != MZ_LOGOUT && mz.weapon != MZ_RESPAWN) {

    // Rerelease: the flash model, at the same place the dlight was just put.
    // Our own gun in first person is handled separately - CL_AddViewWeapon is
    // the only place that knows where the view model actually ended up.
    if (mz.entity == cl.frame.clientNum + 1 && !cl.thirdPersonView) {
        // Your own gun needs BOTH: the view flash for what you see down the
        // barrel, and a world-space one at the player model's gun that only
        // mirrors will draw. Same split as the plasma beam's two chains.
        //
        // The dlight position above is NOT the right place for that second one.
        // It is origin + forward*18 + right*16 with no vertical term at all,
        // which suits a monster - whose origin sits mid-body - but for YOUR
        // model it lands a long way out in front at hip height. Worse,
        // CL_AddPacketEntities slides your own model BACK 15 units so the view
        // point ends up in front of its head (entities.c, "offset the model
        // back a bit"), so the flash was about 33 units clear of the body it is
        // supposed to be attached to.
        //
        // So place this one relative to the model as it is actually drawn, and
        // make the three offsets cvars - where a player model holds its gun is
        // a thing to look at and nudge, not to derive.
        vec3_t  world_muzzle, yaw_only, f, r, u;

        // YAW COMES FROM THE VIEW, NOT FROM THE ENTITY.
        //
        // pl->current.angles is the server's copy of your own player entity,
        // which arrives at the server tick rate and - for your own client - is
        // the one thing the client already knows better than the server does.
        // Feeding it to a model that grows entirely along +X (the flash is
        // v_machn/flash, vertices 0.0 .. 0.4 in X, so it flares FORWARD from
        // its origin) is what had the flare pointing back at the player.
        //
        // cl.refdef.viewangles is the predicted, current facing, and it is
        // exactly what the first-person flash uses - so the two halves of the
        // pair now agree by construction.
        VectorSet(yaw_only, 0, cl.refdef.viewangles[YAW], 0);
        AngleVectors(yaw_only, f, r, u);

        VectorCopy(pl->current.origin, world_muzzle);
        VectorMA(world_muzzle, -15.0f + cl_muzzleflash_world_fwd->value, f, world_muzzle);
        VectorMA(world_muzzle, cl_muzzleflash_world_right->value, r, world_muzzle);
        VectorMA(world_muzzle, cl_muzzleflash_world_up->value, u, world_muzzle);

        CL_ViewMuzzleFlash();
        CL_MuzzleFlashModel(world_muzzle, yaw_only, RF_REFLECTION_FX);
    } else {
        CL_MuzzleFlashModel(dl->origin, pl->current.angles, 0);
    }

    }   // not a spawn/despawn ping

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
        // [Q2RTX] rogue lit this flash YELLOW (0.9, 0.7, 0), which is what it
        // still is in the original. The ETF fires pale blue flechettes and its
        // impact puff is blue (explode skin2), so a yellow muzzle flash ten
        // times a second read as a different weapon firing. Matched to the
        // darts instead.
        VectorSet(dl->color, 0.35f, 0.62f, 1.0f);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("weapons/nail1.wav"), volume, ATTN_NORM, 0);
        break;
    case MZ_PROX:
        VectorSet(dl->color, 1, 1, 0);
        S_StartSound(NULL, mz.entity, CHAN_WEAPON, S_RegisterSound("weapons/proxlr1a.wav"), volume, ATTN_NORM, 0);
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

        CL_MuzzleFlashModel(origin, flash_angles, 0);
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
static cvar_t *cl_blood_color = NULL;
static cvar_t *cl_blood_air_life = NULL;
static cvar_t *cl_blood_pool = NULL;
static cvar_t *cl_blood_pool_max = NULL;
static cvar_t *cl_blood_pool_dist = NULL;
static cvar_t *cl_blood_pool_gain = NULL;
static cvar_t *cl_blood_streak_len = NULL;
static cvar_t *cl_blood_streak_wall = NULL;
static cvar_t *cl_blood_stretch = NULL;
static cvar_t *cl_blood_splash = NULL;
static cvar_t *cl_blood_max = NULL;
static cvar_t *cl_blood_air_reserve = NULL;
static cvar_t *cl_blood_gravity = NULL;
static cvar_t *cl_blood_speed = NULL;
static cvar_t *cl_blood_sound = NULL;
static cvar_t *cl_blood_sound_volume = NULL;
static cvar_t *cl_blood_sound_gap = NULL;
static cvar_t *cl_blood_sound_pool_gap = NULL;
static cvar_t *cl_blood_sound_any_gap = NULL;
static cvar_t *cl_blood_sound_skip = NULL;
static cvar_t *cl_blood_sound_pool_skip = NULL;
static cvar_t *cl_blood_sound_dist = NULL;
static cvar_t *cl_blood_sound_attn = NULL;
static cvar_t *cl_blood_permanent = NULL;
static cvar_t *cl_blood_model_collision = NULL;
static cvar_t *cl_blood_flesh_damp = NULL;
static cvar_t *cl_blood_flesh_run = NULL;
static cvar_t *cl_blood_flesh_cling = NULL;
// Splats that overhang an edge - see CL_BloodProbeRim and CL_BloodSlideOffEdge.
static cvar_t *cl_blood_edge = NULL;
static cvar_t *cl_blood_edge_budget = NULL;
static cvar_t *cl_blood_edge_slide = NULL;
static cvar_t *cl_blood_edge_push = NULL;
static cvar_t *cl_blood_edge_tries = NULL;
// Reshaping a pool that is on the move - see CL_BloodReshapeSlide.
static cvar_t *cl_blood_slide_turn = NULL;
static cvar_t *cl_blood_slide_stretch = NULL;
static cvar_t *cl_blood_slide_len = NULL;
static cvar_t *cl_blood_slide_narrow = NULL;
static cvar_t *cl_blood_slide_relax = NULL;
static cvar_t *cl_blood_slide_speed = NULL;
static cvar_t *cl_blood_wall_spread = NULL;
static cvar_t *cl_blood_drain = NULL;
static cvar_t *cl_blood_cling_size = NULL;
static cvar_t *cl_blood_trail = NULL;

/*
===============
CL_BloodClearSlide

Back to "has not run anywhere yet".

FOUR callers reset this state - a fresh particle, a landing, losing the surface
it was riding, and running off the end of one - and they all have to agree.  A
splat that carries a live settle timer into a life where it means something else
gathers a mark that was never drawn out.
===============
*/
static inline void CL_BloodClearSlide(cparticle_t *p)
{
    p->blood_slide_dist = 0.f;
    p->blood_settling = false;
    p->blood_settle = 0.f;
    p->blood_stretch_run = 1.f;
    p->blood_cross_rest = 1.f;
    p->blood_cross_run = p->blood_cross;
    p->blood_narrow_dist = 0.f;
    p->blood_rim_dist = 0.f;
}

// The wet-impact sounds, registered once per level by CL_RegisterTEntSounds.
// NUM_BLOOD_SFX is declared alongside the extern in client.h.
qhandle_t cl_sfx_blood_splat[NUM_BLOOD_SFX];
qhandle_t cl_sfx_blood_pool;

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

/*
The splats currently on a surface, rebuilt every frame in CL_AddParticles.

Pooling needs to ask "is there already a splat where this droplet just landed?",
and CL_RetireBlood needs to ask "which splat has been here longest?" -
neither of which the particle list itself can answer during the update, because
CL_AddParticles relinks that list as it walks it and only the part already
processed is reachable. A flat array of the stuck ones, rebuilt each frame, is
both correct and faster to scan.
*/
static cparticle_t *blood_splats[MAX_PARTICLES];
static int          num_blood_splats;

/*
The droplets still in the AIR, rebuilt the same way and for one reason: they have
to be retired BEFORE any splat is.

Both count against cl_blood_max, but they are not worth the same. A splat is the
thing the player is looking at and the whole point of g_no_janitor; a droplet in
flight is on screen for under a second and nobody can tell which one went. Retiring
splats to make room for droplets - which is what this did at first - spends the
permanent blood to buy the temporary kind, so a fight quietly eats the floor it
just painted. That is "even with no janitor on, the splats disappear fast".
*/
static cparticle_t *blood_airborne[MAX_PARTICLES];
static int          num_blood_airborne;

/*
===============
CL_RetireBlood

Frees whichever piece of blood is worth least, so a new droplet can take its
place. That is the oldest splat when the floor is the greedy one, and the
NEWEST airborne droplet when the spray is - see the block comment in the body,
which is where the reasoning lives.

This is what keeps cl_blood_permanent bounded. Without it, permanent blood simply
stops appearing once the budget is full - which reads as the effect breaking, and
is what the old over-budget path did. Recycling instead means the floor always
shows the most recent fighting.

Uses the splat list that CL_AddParticles rebuilds each frame. Its entries stay
valid until the next pass, and particles are only ever freed inside that pass, so
there is nothing stale to trip over here.
===============
*/
static cparticle_t *CL_PickBlood(cparticle_t *const *list, int count, bool newest, int *live)
{
    cparticle_t *best = NULL;
    int n = 0;

    for (int i = 0; i < count; i++) {
        cparticle_t *p = list[i];

        // Already retired this frame. The lists are rebuilt once per frame in
        // CL_AddParticles, but a single burst spawns sixty droplets between two
        // frames, so a list is walked many times before it is refreshed and the
        // entries retired by earlier calls are still in it.
        if (!p->is_blood_sphere)
            continue;

        n++;

        if (!best || (newest ? (p->time > best->time) : (p->time < best->time)))
            best = p;
    }

    // Counted in the same pass rather than trusting num_blood_airborne, which is
    // last frame's number and does not fall as this function retires droplets -
    // over one burst that would keep the caller reading a spray that is already
    // back inside its share.
    if (live)
        *live = n;

    return best;
}

// WHERE BLOOD GOES WHEN IT STOPS EXISTING, which the line could not say before.
// A splat leaves by exactly three routes other than merging (already counted as
// merges/s): the budget recycles it, it runs off the end of a surface and is
// airborne again, or the surface it was riding disappears. "It just vanished"
// is answerable from these three numbers and is guesswork without them.
static int blood_retired;
static int blood_detached;
// Runs that reached the foot of a surface and finished emptying into a pool
// there - see CL_BloodDrainToPool. These are the ones that used to snap into a
// floor splat in one frame, so this and detached/s are what tell the two apart.
static int blood_drains;
// Of the stuck splats sitting on something they COULD run down, how many are
// held there by CL_BloodMayRun. This is the number that says whether the cling
// threshold is doing anything at all, or everything, and there is no way to
// pick it without seeing the split - 1.5 pinned 100% of them on the first try.
static int blood_pinned;
static int blood_runnable;

static void CL_RetireBlood(void)
{
    /*
    WHICH DROPLET IS WORTH LEAST, and the two ways of getting it wrong.

    THE FIRST, which this got right: never spend a splat to buy a droplet in
    flight. A splat is the floor the player is looking at; a droplet in the air
    is gone in under a second. See the note on blood_airborne.

    THE SECOND, which it did not: WITHIN the spray, the oldest droplet is the
    one that has been flying longest, which is the one about to LAND. Retiring
    oldest-first therefore kills droplets in precisely the order they would have
    become splats, and once the budget is full that is a treadmill the spray can
    never get off - the landing rate falls to nearly nothing however much blood
    is thrown. Matt found it with the machine gun: sixty droplets per hit at ten
    hits a second is ~600/s against 240 free slots, so the budget stood full and
    "barely any of the blood splats make it to the ground, they seem to
    disappear in mid air". Every other weapon fires in discrete shots with gaps,
    the pool drains between them, and nothing is ever retired.

    So the value ordering inside the spray is the reverse of the floor's: the
    YOUNGEST droplet is the expendable one. It was created this instant and has
    fifty-nine identical siblings from the same wound, and dropping it is
    invisible - where dropping its oldest sibling deletes a splat that was about
    to exist.

    AND THE SPRAY NEEDS A SHARE OF ITS OWN. Splats keep their slot when they
    land, so with permanence on the floor grows into the whole budget and leaves
    the spray with only what is spare. cl_blood_air_reserve fixes a share for
    it; over that share the spray is the greedy one and sheds its newest, under
    it the floor is, and the floor turns over oldest-first as it always did.
    That is self-balancing: retirement only ever runs at saturation, and at
    saturation exactly one of the two is over its share.
    */
    const int budget = max(1, min(cl_blood_max->integer, MAX_BLOOD_SPHERES));

    int reserve = (int)(budget * cl_blood_air_reserve->value);
    reserve = max(0, min(reserve, budget));

    int air_live = 0;
    cparticle_t *youngest_air = CL_PickBlood(blood_airborne, num_blood_airborne, true, &air_live);

    cparticle_t *oldest = NULL;

    if (air_live > reserve)
        oldest = youngest_air;

    if (!oldest)
        oldest = CL_PickBlood(blood_splats, num_blood_splats, false, NULL);

    // No floor to recycle - the spray owns the whole budget, so it pays.
    if (!oldest)
        oldest = youngest_air;

    if (!oldest)
        return;

    if (oldest->blood_slot >= 0 && oldest->blood_slot < MAX_BLOOD_SPHERES) {
        blood_slot_used[oldest->blood_slot] = false;
        oldest->blood_slot = -1;
    }

    // Drop it from the effect and let the ordinary faded-out branch collect the
    // particle - that runs above the relink, which is the only safe place.
    blood_retired++;
    oldest->is_blood_sphere = false;
    oldest->alpha = 0.0f;
    oldest->alphavel = 0.0f;

    if (num_blood_live > 0)
        num_blood_live--;
}

static int CL_AllocBloodSlot(void)
{
    // cl_blood_max, NOT MAX_BLOOD_SPHERES. That is only a ceiling now; the
    // renderer sizes its buffers from cl_blood_max (vkpt_blood_slot_capacity in
    // blood.c computes exactly this), and a slot past what they cover fails the
    // bounds check at the write, so the droplet allocates, generates geometry and
    // then silently never draws. The two expressions must stay identical.
    const int capacity = max(1, min(cl_blood_max->integer, MAX_BLOOD_SPHERES));

    for (int i = 0; i < capacity; i++) {
        if (!blood_slot_used[i]) {
            blood_slot_used[i] = true;
            return i;
        }
    }
    return -1;
}

// Traces issued by the droplet simulation this frame - the suspected cost.
static int blood_traces;
// Rim probes issued this frame, counted apart from the per-step traces above
// because they are budgeted separately and a spike in one says nothing about
// the other. See cl_blood_edge_budget.
static int blood_edge_traces;
static int blood_edge_probes;
// Pools shoved off an edge this frame - see CL_BloodSlideOffEdge.
static int blood_edge_slides;
// Pools that found a wall in the way and grew along it - CL_BloodSpreadAlongWall.
static int blood_wall_spreads;
// Runs that finished settling this frame, and how many of those found blood to
// merge into when they asked - see CL_BloodSettleAtRest.
static int blood_settles;
static int blood_rest_pools;
// EVERY successful merge this frame, by any of the three routes. This is also
// exactly the rate the pooling sound plays at once cl_blood_sound_pool_gap is 0,
// which is what makes it the number to tune that setting against.
static int blood_pools;
// Merges REFUSED only because the target was already at cl_blood_pool_max -
// i.e. pairs that were touching, on the same surface, and would otherwise have
// joined. Counted because "why did those not pool together" has two possible
// answers and this is the one that separates them: too far apart reads as a
// zero here, a saturated pool does not.
static int blood_pool_full;
static cvar_t *cl_blood_stats = NULL;

static void CL_BloodTrace_f(void);

void FX_Init(void)
{
    Cmd_AddCommand("bloodtrace", CL_BloodTrace_f);
    cvar_pt_particle_emissive = Cvar_Get("pt_particle_emissive", "10.0", 0);
	cl_particle_num_factor = Cvar_Get("cl_particle_num_factor", "1", 0);

    // Blood droplet collision. Its own cvar, separate from cl_blood_spheres, so
    // the simulation can be A/B'd against plain ballistic droplets without also
    // switching the whole effect back to flat particle sprites.
    cl_blood_collision = Cvar_Get("cl_blood_collision", "1", CVAR_ARCHIVE);
    // Test droplets against monster and corpse GEOMETRY rather than against
    // the axial box the server packs for them. Off, blood passes straight
    // through bodies and lands on the floor; the one thing it must never do
    // is stop on the box, which is nowhere near the surface being drawn.
    // See CL_TracePoint and CL_BloodTraceModels.
    cl_blood_model_collision = Cvar_Get("cl_blood_model_collision", "1", CVAR_ARCHIVE);
    // How a droplet leaves a body it has hit - see CL_BloodRunOffModel.
    // damp is the fraction of the along-the-surface speed it keeps; run is
    // the minimum speed it is given downhill, which is what gets it off a
    // shoulder or the flat of a back instead of sitting there.
    cl_blood_flesh_damp = Cvar_Get("cl_blood_flesh_damp", "0.35", CVAR_ARCHIVE);
    cl_blood_flesh_run = Cvar_Get("cl_blood_flesh_run", "70", CVAR_ARCHIVE);
    // Without cling a droplet leaving a chest just falls away from it and
    // reads as dripping past the body rather than running down it. A gentle
    // pull into the surface keeps it in contact so it follows the shape.
    cl_blood_flesh_cling = Cvar_Get("cl_blood_flesh_cling", "25", CVAR_ARCHIVE);
    cl_blood_splat_life = Cvar_Get("cl_blood_splat_life", "8", CVAR_ARCHIVE);
    cl_blood_slide = Cvar_Get("cl_blood_slide", "2.5", CVAR_ARCHIVE);

    // HOW FAST BLOOD IS ALLOWED TO RUN, in units per second.
    //
    // cl_blood_slide is a linear drag, so the speed a run settles at is
    // cl_blood_gravity / cl_blood_slide - 320 u/s on a vertical wall at the
    // defaults, which crosses a room-height wall in a fifth of a second. Blood
    // does not do that. A film running under gravity is viscosity-limited, and a
    // terminal speed is the honest way to say so: the drag still brings a run to
    // a stop, this only caps how fast it can get.
    //
    // A NEW CVAR RATHER THAN A RETUNED cl_blood_slide, deliberately. Both are
    // archived, and a value already sitting in a q2config.cfg makes a changed
    // default invisible to the person who has one - so the knob that changes
    // behaviour has to be one nobody can already have set.
    cl_blood_slide_speed = Cvar_Get("cl_blood_slide_speed", "24", CVAR_ARCHIVE);

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

    // Force every blood sphere to one colour. 0 leaves whatever the effect that
    // spawned it asked for, which is red for TE_BLOOD and green for
    // TE_GREENBLOOD. See CL_ApplyBloodColor.
    cl_blood_color = Cvar_Get("cl_blood_color", "0", CVAR_ARCHIVE);

    // How long a droplet may stay in the air before giving up. NOT a fade: a
    // droplet in flight keeps full size and simply waits until it hits
    // something. This exists only so that blood thrown into a void, or out of a
    // window, is eventually reclaimed instead of orbiting forever.
    cl_blood_air_life = Cvar_Get("cl_blood_air_life", "15", CVAR_ARCHIVE);

    // Pooling: a droplet landing on an existing splat merges into it instead of
    // adding another overlapping disc.
    cl_blood_pool = Cvar_Get("cl_blood_pool", "1", CVAR_ARCHIVE);
    // MEASURED, not guessed: with this at 4 a run down base1's rocky ramp
    // reported up to 140 merges per second REFUSED for no reason other than the
    // target already being at the cap - pairs that were touching, on the same
    // surface, and would otherwise have joined (the "full/s" figure on the blood
    // stats line). That is what leaves a hillside as a row of separate marks
    // instead of one pool at the bottom, because a saturated pool absorbs
    // nothing and every later droplet has to start a splat of its own beside it.
    //
    // Areas add in quadrature, so the cap is a multiple of one droplet's WIDTH:
    // 4 saturates after 16 droplets, which one wound exceeds on its own. 8 takes
    // 64 and is a proper puddle - about 29 units across at the default radius
    // and splat size.
    cl_blood_pool_max = Cvar_Get("cl_blood_pool_max", "8", CVAR_ARCHIVE);
    // In DROPLET widths (see the reach calculation - it is not scaled by the
    // splat spread). This is the knob that decides whether a floor reads as many
    // separate marks or a few large pools, and it interacts with gravity: real
    // gravity lands droplets closer together, so the same reach merges far more
    // of them. 2.0 keeps individual spatter visible under the new fall rate.
    // Below about 1.0 nothing pools at all - a measured burst lands its droplets
    // ~8 units apart against a ~4 unit reach, and 120 splats became 118.
    cl_blood_pool_dist = Cvar_Get("cl_blood_pool_dist", "2.0", CVAR_ARCHIVE);

    // DIMINISHING RETURNS on a merge: the fraction of the absorbed droplet's
    // AREA the pool actually gains. Areas add in quadrature, so at 1.0 a pool is
    // exactly as wide as the blood in it - which is correct and, past a certain
    // size, not what anyone wants to look at. Matt, testing at pool_max 24:
    // "when the blood drops pool up maybe each additional contribution should
    // not make the puddle quite as large as it gets."
    //
    // At 0.5 a pool needs twice the droplets to reach any given width, so the
    // early droplets still visibly grow it and the hundredth one barely moves
    // it. Independent of cl_blood_pool_max, which is still the hard ceiling.
    cl_blood_pool_gain = Cvar_Get("cl_blood_pool_gain", "0.5", CVAR_ARCHIVE);

    // LONGEST a mark may be drawn ON A FLOOR, tip to tip, in world units - see
    // CL_BloodStretchLimit. 0 removes the limit and leaves only
    // cl_blood_slide_stretch, which is what this used to be.
    cl_blood_streak_len = Cvar_Get("cl_blood_streak_len", "16", CVAR_ARCHIVE);

    // How much further a mark may be drawn on a VERTICAL surface than on a flat
    // one, as a multiple. Matt: "the blood streaks should be able to grow longer
    // as they slide down a wall but once they hit the floor they should be less
    // long and more circular."
    //
    // Scales BOTH ceilings in CL_BloodStretchLimit - the length and the shape -
    // so that raising it cannot be silently defeated by whichever of the two
    // happened to bind first. Deliberately a multiplier rather than a second
    // pair of absolute values: it means a wall inherits any tuning done to the
    // floor numbers instead of drifting away from them.
    cl_blood_streak_wall = Cvar_Get("cl_blood_streak_wall", "4", CVAR_ARCHIVE);

    // A SPLAT IS A DISC AND IT LANDS WHERE ITS CENTRE LANDS, so one that lands
    // near a ledge draws its far half out over the drop. cl_blood_edge measures
    // how far the surface really reaches around the rim; a pool hanging well over
    // the side is shoved off it, and whatever stays has its outline clipped to
    // the edge. Off, both revert to the old behaviour exactly.
    cl_blood_edge = Cvar_Get("cl_blood_edge", "1", CVAR_ARCHIVE);
    // Splats measured per frame. THE COST CONTROL for the whole thing: measuring
    // is eight to twenty-four point traces and happens once in a splat's life,
    // but a single wound lands sixty droplets between two frames, so doing them
    // all at the moment they stick would put the entire bill in one frame. Spread
    // over frames instead, a burst is fully measured in well under a second and
    // no frame pays more than this. Measured at the default: a burst that put 512
    // droplets in the air peaked at 268 probes and 2144 traces per SECOND, next
    // to 265 traces per FRAME for the droplet steps that were already happening.
    cl_blood_edge_budget = Cvar_Get("cl_blood_edge_budget", "8", CVAR_ARCHIVE);
    // How much of a pool has to be over thin air before the whole thing lets go
    // and slides off, as a fraction of its rim. Around a quarter is "clearly
    // hanging off the side"; at 0 anything that overhangs at all leaves, and at 1
    // nothing ever does and every overhang is handled by clipping alone.
    cl_blood_edge_slide = Cvar_Get("cl_blood_edge_slide", "0.25", CVAR_ARCHIVE);
    // The shove it leaves with, in units/sec along the surface. It is a slide, not
    // a jump - it has to be enough to carry the pool past its own rim against
    // cl_blood_slide drag, and no more.
    cl_blood_edge_push = Cvar_Get("cl_blood_edge_push", "60", CVAR_ARCHIVE);
    cl_blood_edge_tries = Cvar_Get("cl_blood_edge_tries", "3", CVAR_ARCHIVE);

    // A MARK POINTS THE WAY THE BLOOD WENT, and blood that keeps going has a new
    // way. These reshape a pool while it is sliding: turn is how fast the long
    // axis swings round to the direction of travel (a smoothing RATE, not an
    // angular speed - it is the reciprocal of a time constant, so 4 re-aims a
    // pool over about a quarter of a second); stretch is how far a run may draw
    // one out; len is the distance over which it gets most of the way there.
    //
    // TURN IS A COST KNOB AS WELL AS A LOOK KNOB, which is not obvious. Every
    // committed step of the axis is one full per-vertex mesh rebuild (see
    // CL_BloodReshapeSlide), and how many steps a 90 degree turn takes is fixed
    // by BLOOD_SLIDE_TURN_STEP - so turn decides how many FRAMES those rebuilds
    // are spread over, not how many there are. At 12 the whole turn lands inside
    // about five frames, i.e. a rebuild per frame per splat, and a burst that
    // puts fifty splats on a wall re-aims all of them at once. 4 spreads the same
    // steps over ~15 frames. It also just looks better: a pool leaning round as
    // it begins to run instead of snapping to its new direction.
    cl_blood_slide_turn = Cvar_Get("cl_blood_slide_turn", "4", CVAR_ARCHIVE);
    cl_blood_slide_stretch = Cvar_Get("cl_blood_slide_stretch", "3.5", CVAR_ARCHIVE);
    cl_blood_slide_len = Cvar_Get("cl_blood_slide_len", "24", CVAR_ARCHIVE);

    // THE SIZE BELOW WHICH BLOOD DOES NOT RUN AT ALL, as a splat radius on a
    // vertical surface.
    //
    // THIS IS THE ADHESION THE SIMULATION HAD NO MODEL OF, and its absence is why
    // a wall ended up bare. On a vertical face the whole of gravity acts along
    // the surface, so EVERY splat ran, every time - the wall was only ever a
    // staging area on the way to the floor, and a fight against a wall finished
    // with all of its blood in a puddle and none of it on the wall. Matt, three
    // times: "it still looks like they are disappearing off the wall."
    //
    // Real blood does not behave that way and the reason is surface tension: the
    // hold scales with the contact edge while the weight scales with the volume,
    // so below a critical size a drop pins and never moves however steep the
    // surface. Above it, it runs. That is the whole of this test.
    //
    // It gives the right picture for free rather than by tuning: spatter sticks
    // where it lands and STAYS, while blood that keeps arriving in one place
    // pools until it crosses the threshold and only THEN runs, as a rivulet. Both
    // halves are what a wall actually looks like.
    //
    // Scaled by how steep the surface is, so a shallow ramp holds blood that a
    // vertical wall would not. 0 disables it - everything runs, as before.
    cl_blood_cling_size = Cvar_Get("cl_blood_cling_size", "1.0", CVAR_ARCHIVE);

    // UNITS OF RUN BETWEEN THE MARKS A RIVULET LEAVES BEHIND IT.
    //
    // A run wets the surface it passes over; it does not lick it clean. Without
    // this the mark is carried down the wall and the wall above it stays as clean
    // as if nothing had happened, which is the other half of "disappearing off
    // the wall". Each mark is paid for out of the runner, so blood is moved, not
    // invented. 0 leaves no track.
    cl_blood_trail = Cvar_Get("cl_blood_trail", "8", CVAR_ARCHIVE);

    // SECONDS A RUN TAKES TO HAND ITS BLOOD OVER once it reaches the foot of the
    // surface it was running down.
    //
    // The whole mark used to become a floor splat on the single frame the
    // droplet's centre crossed the corner. This spreads it: the pool starts as a
    // small puddle where the run arrives and grows to full size as the streak
    // above it empties into it. 0 restores the instant hand-over.
    cl_blood_drain = Cvar_Get("cl_blood_drain", "1.2", CVAR_ARCHIVE);

    // UNITS OF RUN OVER WHICH A MARK NARROWS ACROSS ITS TRAVEL, the companion to
    // cl_blood_slide_len drawing it out along it.
    //
    // A run is fed by the blood moving through it, so what it leaves behind is
    // about as wide as that stream - not as wide as the blot it drained out of.
    // A hand-width smear on a wall that starts running down does not stay a
    // hand-width wide the whole way down; it pulls into a rivulet. This is the
    // distance constant of that pull, and it is what makes a sliding mark GROW
    // vertically while it SHRINKS horizontally rather than simply pivoting.
    //
    // 0 keeps the width the mark landed with, all the way down.
    cl_blood_slide_narrow = Cvar_Get("cl_blood_slide_narrow", "20", CVAR_ARCHIVE);

    // HOW FAR A POOL MAY GROW ALONG A WALL IT HAS RUN INTO, as a multiple of the
    // width it would have had in open floor. See CL_BloodSpreadAlongWall: the
    // blood that would have gone through the wall has to go somewhere, and
    // sideways along the foot of it is where. 1 disables the spread and leaves
    // the pool simply clipped, which is still not clipping THROUGH.
    cl_blood_wall_spread = Cvar_Get("cl_blood_wall_spread", "1.6", CVAR_ARCHIVE);

    // SECONDS A POOL TAKES TO GATHER BACK UP once its run stops - see
    // CL_BloodSettleAtRest. A streak is what blood on the MOVE looks like; what
    // is left at the bottom of a slope is a pool, and nothing used to turn one
    // back into the other, so a hillside of droplets ended up as a fan of
    // identical needles lying on the flat.
    //
    // 0 keeps the old behaviour exactly: the streak the run drew is frozen where
    // it stopped.
    cl_blood_slide_relax = Cvar_Get("cl_blood_slide_relax", "1.2", CVAR_ARCHIVE);

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
    // This is the real budget AND what sizes the renderer's buffers, so raising
    // it costs memory: cl_blood_max * worst_faces * (128 + 36) bytes of shadow,
    // and twice that again in staging. worst_faces is the LARGER of the sphere
    // and the puddle at that tessellation, and since the puddle was widened to
    // fill the slot the two are equal at the top level: pt_blood_tess 2 is 320
    // faces either way, so 512 is about 81 MB and 2048 about 324 MB.
    // pt_blood_tess 1 is 128 (the puddle, against the sphere's 80) and divides
    // all of it by 2.5 rather than by the 4 it used to. MAX_BLOOD_SPHERES is
    // only the ceiling. SAFETY
    // VALVE rather than a limiter - a settled firefight sits around 250. It is
    // here because persistence removed what used to bound droplet count (they
    // faded in under a second; now they live until they land), and a measured
    // burst had 1617 in the air mid-gib. Lower it if a fight ever needs bounding.
    cl_blood_max = Cvar_Get("cl_blood_max", "512", CVAR_ARCHIVE);

    // What share of cl_blood_max the SPRAY may hold - see CL_RetireBlood.
    //
    // Without a share of its own the floor takes the whole budget as it fills,
    // and a sustained weapon is left with whatever happens to be spare. Matt's
    // machine gun ran into exactly that: ~1808 parked splats out of 2048 left
    // 240 slots for a stream needing about 390, so the budget stood permanently
    // full and every new droplet retired one already in flight.
    //
    // A fraction rather than a count, so it tracks cl_blood_max. 0 restores the
    // old behaviour of giving the spray no reserved room at all.
    cl_blood_air_reserve = Cvar_Get("cl_blood_air_reserve", "0.25", CVAR_ARCHIVE);

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

    // One LANDING sound heard in every N that clear the gap above - a THINNING
    // on top of that rate limit. Matt: "the blood splats should only play sounds
    // for every other splat for the 332 and 333 sounds". 1 restores every-splat
    // behaviour.
    //
    // A DIVIDER ON THE SOUND, NOT ON THE DROPLETS, and that distinction is the
    // whole reason this setting used to do nothing you could hear. It counted
    // droplets and ran BEFORE the gap: sixty of them land inside one window the
    // gap was only ever going to let a single sound out of, so thinning sixty to
    // eight still left that same one sound. Counted after the gap instead, the
    // two multiply - 8 here against 90 ms above is a landing sound no oftener
    // than every 720 ms.
    cl_blood_sound_skip = Cvar_Get("cl_blood_sound_skip", "2", CVAR_ARCHIVE);

    // The POOLING sound's own thinning, the companion to the one above.
    //
    // TWO SETTINGS AND NOT ONE, because the two sounds do not happen at anything
    // like the same rate and a single factor cannot thin both to taste. A
    // droplet only makes the landing sound if it hits a DRY surface; one that
    // lands on blood already there merges instead, and makes this sound. So the
    // first shots of a fight are landings and everything after them is merges -
    // which is why winding the landing skip up to 16 barely changed what a
    // firefight sounded like. This is the one with the volume in it.
    //
    // 1 by default, leaving the gaps to do the work; the range reaches 32
    // because a measured burst merged 668 times in a second, and one in thirty
    // of that is still twenty-odd sounds.
    cl_blood_sound_pool_skip = Cvar_Get("cl_blood_sound_pool_skip", "1", CVAR_ARCHIVE);

    // The POOLING sound's own gap. Matt asked for "every blood drop that starts
    // to pool up", i.e. 0, and MEASUREMENT SAYS THAT CANNOT BE ONE VOICE PER
    // EVENT: four soldiers gibbed in base1 peaked at 668 MERGES IN ONE SECOND
    // (the "merges/s" figure on the blood stats line, which is exactly this
    // sound's rate). 668 voices does not thin the blood audio, it drops the
    // gunfire and the monsters - a failure that reads as a bug somewhere else
    // entirely, which is the expensive kind.
    //
    // So 40 ms: still a fast wet crackle during a burst, every isolated droplet
    // still heard on its own, and bounded at 25/s. 0 is one command away and
    // gives the literal behaviour for anyone who wants to hear it.
    //
    // Still the OPPOSITE treatment to the impacts, which is the point: an impact
    // is a loud transient and fifty at once is mush, while the pooling sound is
    // quieter and wetter and reads as texture when it overlaps. Hence 40 here
    // against 90 plus every-other-splat there.
    cl_blood_sound_pool_gap = Cvar_Get("cl_blood_sound_pool_gap", "40", CVAR_ARCHIVE);

    // THE FLOOR UNDER BOTH SOUNDS AT ONCE, in milliseconds.
    //
    // The two gaps above are per sound and deliberately independent, so that an
    // impact and a droplet pooling up cannot silence each other inside one
    // window. The cost of that independence is that the two rates ADD: at the
    // defaults above it is 11 impacts and 25 pooling sounds a second, and 36 wet
    // voices a second is not a patter, it is a wall.
    //
    // It is also why raising cl_blood_sound_gap on its own does not quieten a
    // fight the way it looks like it should - it only ever thinned one of the
    // two streams, and the denser one was the other one. This gap counts EVERY
    // blood sound whatever kind it is, so it is the one to reach for when the
    // answer is simply "fewer".
    //
    // Checked LAST, after a sound has already earned its own window, so it only
    // removes the overlap between the two streams rather than reordering them.
    cl_blood_sound_any_gap = Cvar_Get("cl_blood_sound_any_gap", "60", CVAR_ARCHIVE);

    // Beyond this many units a landing droplet makes no sound at all. Distance
    // attenuation would make it inaudible anyway, but it would still take a
    // voice and still count against the gap above, silencing a nearer impact.
    //
    // NOT the only cull any more: CL_BloodSoundRange takes the tighter of this
    // and the distance the attenuation below actually reaches silence at, so
    // this value binding is now the exception rather than the rule.
    cl_blood_sound_dist = Cvar_Get("cl_blood_sound_dist", "1200", CVAR_ARCHIVE);

    // HOW FAST THE SPLAT FALLS OFF WITH DISTANCE, as a Quake II ATTN_ value.
    //
    // This was ATTN_NORM, and ATTN_NORM is far too long-range for a droplet.
    // Both mixers use the same LINEAR law - AL_LINEAR_DISTANCE_CLAMPED in
    // sound/al.c and the identical arithmetic by hand in S_SpatializeOrigin -
    // which is gain = 1 - dist_mult * (dist - SOUND_FULLVOLUME), reaching zero
    // at 80 + 1/dist_mult units and NOT the inverse-square curve the phrase
    // "falls off with distance" suggests. At ATTN_NORM that zero is 2080 units,
    // so across the 80-600 units where blood actually lands the gain only moves
    // 1.0 -> 0.74. A 2.6 dB spread over the entire plausible range is why the
    // sound read as coming from nowhere even though it was already positioned
    // correctly at the impact point.
    //
    // ATTN_STATIC (3) puts the zero at 413 units instead, which is a real
    // gradient over the distances involved. Lower it toward 1 for the old
    // near-flat behaviour, and note ATTN_NONE (0) means no attenuation AND no
    // stereo placement in both mixers - the "off" setting, not a quiet one.
    //
    // A float, and clamped to (0, 3] on purpose: S_IssuePlaysound tests
    // `attenuation == ATTN_STATIC` EXACTLY and only then uses the 0.001 scale
    // instead of 0.0005, so the mapping doubles discontinuously at 3 and then
    // FOLDS BACK - attn 4 attenuates LESS than attn 3. Values above 3 are a
    // footgun, so they are not offered.
    cl_blood_sound_attn = Cvar_Get("cl_blood_sound_attn", "3", CVAR_ARCHIVE);

    // ONE SWITCH for the whole "leave the mess alone" idea - blood here, and
    // gibs, heads and debris in the game library, which registers the same name.
    // Deliberately NOT g_ludicrous_gibs: that also multiplies the gib count and
    // gives every gib a non-diminishing blood trail, which with path-traced
    // droplets is one to two orders of magnitude more blood.
    //
    // Landed blood never fades.
    //
    // Worth having for two reasons. It looks better - blood does not evaporate -
    // and it is FASTER, which is the counterintuitive part. A settled splat is
    // free: its geometry is cached and its upload is skipped entirely. All the
    // cost is in landing and in fading, and fading is the worse of the two
    // because a shrinking splat changes every frame and so misses the cache
    // every frame, right when a whole floor of them expires together.
    //
    // Nothing is unbounded: once cl_blood_max splats exist, the OLDEST is
    // retired to make room, so the count is capped and the newest blood is
    // always the blood you keep.
    cl_blood_permanent = Cvar_Get("g_no_janitor", "1", CVAR_ARCHIVE);
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
    p->blood_cross = 1.f;
    p->blood_ent = -1;
    p->blood_ent_id = 0;
    p->radius = 0.f;
    p->seed = 0.f;
    p->blood_rim = BLOOD_RIM_FULL;
    p->blood_rim_dirty = false;
    p->blood_block = 0;
    p->blood_wall_spread = false;
    p->blood_arrived = false;
    p->blood_drain = 0.f;
    p->blood_edge_tries = 0;
    p->blood_stretch_base = 1.f;
    p->blood_cross_base = 1.f;
    CL_BloodClearSlide(p);
    VectorClear(p->blood_slide_axis);
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
// blood_splats / num_blood_splats are declared further up, beside the slot pool -
// CL_RetireBlood needs them and runs before this point in the file.

// Defined below, beside the impact sound it deliberately does not share a timer
// with. Declared here because the merge is the one place that knows a droplet
// joined a puddle, and there are three paths into it.
static void CL_BloodPoolSound(const vec3_t point);

/*
===============
CL_BloodStretchLimit

The longest elongation a mark of this SIZE may hold, from cl_blood_streak_len -
the length of the mark in world units, tip to tip.

A streak is thin because it is a thin film being dragged along.  A pool is not:
once enough blood has gathered in one place it spreads under its own weight and
reads as a puddle however it got there.  Matt, testing at cl_blood_pool_max 24:
"when the pools get past a certain length they should settle more circular ... it
should slide down the surface and be long but when it pools up past a certain
point it should become more circular."

That is also what was making big pools FLICKER, which he identified himself: "the
flicker looks like because the blood pool is so stretched out it's on both
surfaces."  A pool at radius 28.8 draws a 43-unit rim, and at the 3.5 stretch cap
that is a 300-unit sliver - long enough to lie across a surface junction with
half of itself buried in one plane and half hanging past it.  The rim clip is
supposed to fit a mark to the ground it settles in, and it still does; what it
cannot do is make sense of a shape that is an order of magnitude larger than the
surface it is on.

EXPRESSING THE LIMIT AS A LENGTH IN UNITS, rather than as a multiple of the
droplet, is what makes this one rule instead of two.  The drawn half-length is
spread * stretch and spread grows with the pool, so a single absolute cap lets a
lone droplet streak out to the full cl_blood_slide_stretch, forces anything that
has pooled up to sit round, and ramps smoothly between - with no second knob to
keep in step with the first.  Note that it can only ever remove ELONGATION: a
round pool's own diameter is 2 * spread, so the cap never shrinks a circular
pool, it only stops a big one from being a needle.
===============
*/
static float CL_BloodStretchLimit(const cparticle_t *p)
{
    // HOW MUCH OF GRAVITY ACTS ALONG THIS SURFACE, which is the one number the
    // whole rule needs. On a vertical wall it is all of it and the run keeps
    // being drawn out for as long as the blood keeps moving; on a floor it is
    // none of it, nothing pulls the film anywhere, and what is there spreads.
    // A ceiling is 0 for the same reason a floor is, which is correct.
    //
    // This is the same quantity that drives the slide itself - the in-plane
    // component of gravity in the stuck branch - so the shape a mark is allowed
    // to hold and the force that gave it that shape now come from one place.
    float slope = 0.f;

    if (DotProduct(p->blood_normal, p->blood_normal) > 0.5f) {
        const float nz = min(1.f, fabsf(p->blood_normal[2]));
        slope = sqrtf(max(0.f, 1.f - nz * nz));
    }

    // 1 on a floor, cl_blood_streak_wall on a vertical face.
    const float reach = 1.f + (max(1.f, cl_blood_streak_wall->value) - 1.f) * slope;

    const float cap = max(1.f, cl_blood_slide_stretch->value) * reach;
    const float len = cl_blood_streak_len->value * reach;

    if (cl_blood_streak_len->value <= 0.f)
        return cap;             // no length limit; the old behaviour exactly

    const float spread = max(0.01f, p->radius * max(0.1f, cl_blood_splat_size->value));

    float limit = min(cap, len / (2.f * spread));

    // A SMEAR CANNOT BE LONGER THAN THE RUN THAT DREW IT, and nothing used to say
    // so. This is the cause of Matt's screenshot - streaks standing off the top
    // edge of a stair rail into thin air, over surface the blood had never been
    // anywhere near.
    //
    // The renderer hangs the run's share of the length BEHIND the droplet (it
    // shifts the mesh back by spread*trail, where trail is stretch minus the
    // stretch it landed with), so the mark's trailing edge sits
    // 2*spread*(stretch - stretch_base) behind the droplet plus the landing
    // mark's own half. Three units of travel on a wall at the streak_wall-raised
    // ceiling could put that forty units back up the wall, which is why the
    // needles reached so far past everything.
    //
    // Inverting it gives the ceiling directly: the trailing edge may reach back
    // exactly as far as the droplet has actually come. It binds only on a mark
    // that is long against its own run, which is every one of the offending
    // needles and none of the settled blood on a floor.
    limit = min(limit, max(1.f, p->blood_stretch_base) +
                       p->blood_slide_dist / (2.f * spread));

    // A POOL SHAPED BY A WALL IS NOT A STREAK, and the length limit above cannot
    // tell them apart - it is expressed in absolute units, so for any pool of
    // real size it collapses to "round". Every merge into a pool that had spread
    // along a wall would therefore square it back up and push it into the wall
    // again, one droplet at a time.
    //
    // The exemption is exactly as wide as the spread is allowed to be, so it
    // cannot become a way around the flicker this limit exists to stop.
    if (p->blood_wall_spread)
        limit = max(limit, min(cap, max(1.f, cl_blood_wall_spread->value)));

    return max(1.f, limit);
}

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

ent is the entity the landing droplet hit, or -1 for the world, and it has to
match the splat being merged into. Two splats a few units apart right now are not
a few units apart once one of them rides a door away, so merging across that
boundary would put the combined blood on whichever surface won.
===============
*/
static bool CL_BloodPoolInto(cparticle_t *p, const vec3_t point, const vec3_t normal,
                            int ent, float give)
{
    if (!cl_blood_pool->integer)
        return false;

    // HOW MUCH BLOOD IS BEING HANDED OVER, as a radius. Zero means "all of p",
    // which is the whole-splat merge every original caller wants and the one
    // whose true return means "retire the donor". A draining run passes a
    // fraction instead and keeps what is left - see CL_BloodDrainToPool.
    const float donor = (give > 0.f) ? give : p->radius;

    const float max_radius = cl_blood_sphere_radius->value * max(1.f, cl_blood_pool_max->value);

    for (int i = 0; i < num_blood_splats; i++) {
        cparticle_t *other = blood_splats[i];

        // ITSELF. A landing droplet is airborne and is not on this list, but a
        // SLIDING one is - it is a stuck splat that happens to be moving - and
        // without this it merges into itself, doubles its own area and is then
        // retired for having been absorbed by the splat it still is.
        if (other == p)
            continue;

        if (other->blood_ent != ent)
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
        const float reach = (other->radius + donor) * cl_blood_pool_dist->value;

        if (DotProduct(delta, delta) > reach * reach)
            continue;

        // SATURATED. Tested last rather than first, so that this counter means
        // "these two were touching and did not join" rather than "these two
        // were never candidates" - the difference between a pooling reach that
        // is too small and a pool that is simply full. Costs one distance test
        // per already-full pool in reach.
        if (other->radius >= max_radius) {
            blood_pool_full++;
            continue;
        }

        // DIMINISHING RETURNS - see cl_blood_pool_gain. Areas add in quadrature,
        // and the gain is the fraction of the incoming droplet's area the pool
        // actually keeps, so a pool needs 1/gain times the droplets to reach any
        // given width. The early ones still grow it visibly; the hundredth one
        // barely moves it.
        const float gain = max(0.f, min(cl_blood_pool_gain->value, 1.f));

        other->radius = sqrtf(other->radius * other->radius +
                              gain * donor * donor);
        if (other->radius > max_radius)
            other->radius = max_radius;

        // IT IS A BIGGER POOL NOW, so it may be longer than that size is allowed
        // to be - see CL_BloodStretchLimit. Applied here rather than left to the
        // settle because the disc widens on this frame regardless: the mesh is
        // being rebuilt either way, and a pool that has grown round while still
        // drawn as a sliver is precisely the artifact the limit exists for.
        const float limit = CL_BloodStretchLimit(other);
        if (other->blood_stretch > limit)
            other->blood_stretch = limit;
        if (other->blood_stretch_base > limit)
            other->blood_stretch_base = limit;

        // The pool just got wider, so its rim is somewhere new and the reach
        // measured for the old one no longer describes it. This is the ONLY thing
        // that invalidates a parked splat's rim - which is what keeps the probe
        // off the per-frame path.
        other->blood_rim_dirty = true;

        // And it may have grown into a wall it previously cleared, so it is
        // allowed to spread along one again. Safe against the runaway the flag
        // exists to prevent: this re-arms only on a MERGE, and a merge is real
        // blood arriving, not the pool arguing with its own outline.
        other->blood_wall_spread = false;

        // A pool that is still being fed should not age out mid-fight.
        other->time = cl.time;
        other->alpha = 1.0f;

        // Blood joining blood. Sounded here rather than at the three call sites
        // so that every way of pooling up is covered by one line: a droplet
        // landing in a puddle, a pool sliding into one, and a run coming to rest
        // against one.
        blood_pools++;
        CL_BloodPoolSound(point);

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
// The rate limit and the distance cull, shared by both blood sounds. `last_time`
// is the CALLER'S, deliberately: an impact and a droplet joining a puddle are
// different events and must not suppress each other, but each on its own still
// obeys the one gap - see the design constraint below.
// Close enough to be worth a voice at all?
//
// TESTED BEFORE BOTH THE THINNING AND THE RATE LIMIT, deliberately. A splat
// across the level that could never be heard must not consume the one slot in
// the window or one of the every-other-splat turns, because then the near impact
// that actually matters is the one that gets dropped. That is the whole reason
// cl_blood_sound_dist exists, and both filters below have to sit behind it.
// cl_blood_sound_attn, clamped to the range where the engine's own attn ->
// dist_mult mapping is still monotonic. See the cvar for why 3 is the ceiling.
static float CL_BloodSoundAttn(void)
{
    const float attn = cl_blood_sound_attn->value;

    if (attn <= 0.f)
        return 0.f;             // ATTN_NONE - no falloff and no stereo placement

    return min(attn, (float)ATTN_STATIC);
}

// How far away a droplet can be and still be worth a voice, in units. 0 means
// unbounded.
//
// THE TIGHTER OF TWO LIMITS, and the attenuation's own is usually the tighter
// one. Quake II's falloff is linear, so a sound does not merely get quiet with
// distance, it goes EXACTLY silent at a distance S_AttenuationRange can name -
// and past that point letting a droplet through would spend a voice and a slot
// in cl_blood_sound_gap's window on nothing, which is the very failure
// cl_blood_sound_dist was added to prevent.
//
// That cull was a flat 1200 while ATTN_NORM stayed audible out to 2080, so it
// used to cut the sound off while it was still at 44% volume - an audible
// pop-out rather than a fade. Deriving the range from the attenuation means the
// cull now lands where the sound has already faded to nothing, whatever
// cl_blood_sound_attn is set to, and cl_blood_sound_dist is left as a way to ask
// for something tighter still.
static float CL_BloodSoundRange(void)
{
    float range = max(0.f, cl_blood_sound_dist->value);
    const float silent_at = S_AttenuationRange(CL_BloodSoundAttn());

    if (silent_at > 0.f && (range <= 0.f || silent_at < range))
        range = silent_at;

    return range;
}

static bool CL_BloodSoundAudible(const vec3_t point)
{
    if (!cl_blood_sound->integer)
        return false;

    const float max_dist = CL_BloodSoundRange();

    if (max_dist <= 0.f)
        return true;

    vec3_t delta;
    VectorSubtract(point, listener_origin, delta);

    return VectorLength(delta) <= max_dist;
}

// The rate limit. `last_time` is the CALLER'S, and so is the gap: an impact and a
// droplet joining a puddle are different events and must not suppress each other,
// and they no longer even want the same interval.
static bool CL_BloodSoundReady(int *last_time, int gap)
{
    // cl.time can jump backwards on a map change or a demo seek, which would
    // otherwise latch the gap shut until the clock caught up again.
    if (cl.time < *last_time)
        *last_time = 0;

    if (gap > 0 && cl.time - *last_time < gap)
        return false;

    *last_time = cl.time;
    return true;
}

// ONE IN EVERY N. Each sound passes its own factor and its own tally, because
// landings and merges do not happen at anything like the same rate - see
// cl_blood_sound_pool_skip for why one shared factor could not thin both.
//
// `heard` counts what has ALREADY cleared that sound's gap, which is what makes
// this a divider on the sound rather than on the droplets.
static bool CL_BloodSoundSkipped(unsigned *heard, int skip)
{
    skip = max(1, skip);

    return ((*heard)++ % (unsigned)skip) != 0;
}

// The gap that both blood sounds share - see cl_blood_sound_any_gap for why one
// exists on top of the two per-sound gaps. Committed only when it passes, so a
// sound that this rejects does not also push the floor forward.
static bool CL_BloodSoundFloorReady(void)
{
    static int last_time;

    return CL_BloodSoundReady(&last_time, cl_blood_sound_any_gap->integer);
}

static void CL_BloodPlaySoundAt(const vec3_t point, qhandle_t sfx)
{
    // entnum 0 with a world origin: a positioned sound that belongs to no
    // entity, so it will not cut off a sound another entity is playing.
    // cl_blood_sound_attn, not ATTN_NORM - a droplet is a small quiet event and
    // ATTN_NORM's linear falloff does not reach silence until 2080 units, which
    // is nearly flat across the distances blood lands at. See the cvar.
    S_StartSound(point, 0, CHAN_AUTO, sfx,
        cl_blood_sound_volume->value, CL_BloodSoundAttn(), 0);
}

static void CL_BloodImpactSound(const vec3_t point)
{
    static int last_time;
    static unsigned audible;

    if (!cl_sfx_blood_splat[0] || !CL_BloodSoundAudible(point))
        return;

    // EVERY OTHER SPLAT - cl_blood_sound_skip. Counted, not timed, and that is
    // the point of adding it next to a gap that already exists: the gap decides
    // how many impacts CAN be heard per second, while this decides how sparse
    // the ones that are heard feel. Matt asked for the impacts to thin out; the
    // gap stays underneath as the thing that stops sixty droplets in two frames
    // from becoming sixty voices.
    //
    // Gap first, then the thinning - see CL_BloodSoundSkipped for why that order
    // is the whole difference between this setting working and doing nothing.
    if (!CL_BloodSoundReady(&last_time, cl_blood_sound_gap->integer))
        return;

    if (CL_BloodSoundSkipped(&audible, cl_blood_sound_skip->integer))
        return;

    if (!CL_BloodSoundFloorReady())
        return;

    CL_BloodPlaySoundAt(point, cl_sfx_blood_splat[Q_rand() % NUM_BLOOD_SFX]);
}

/*
===============
CL_BloodPoolSound

Blood joining blood that is already on the surface.

THIS EVENT WAS SILENT, and it is the commonest one in the system once pooling is
on: a droplet that merges returns from CL_SimulateBloodSphere early and never
reaches CL_BloodStick, so it never reached the impact sound either. Every droplet
that landed on bare floor was heard and every one that landed in a puddle was not,
which is the opposite of what a listener expects.

Its own timer, not the impact's. Sharing one would mean whichever event happened
first in a 90 ms window silenced the other, and during a burst that is a coin
toss deciding which half of the sound design you hear. Both do answer to
cl_blood_sound_any_gap afterwards, which is a ceiling on the pair of them rather
than a timer either one can latch shut against the other - see the cvar.

A MUCH SHORTER GAP than the impacts get - the opposite treatment, on purpose.
It has a thinning of its own as well, cl_blood_sound_pool_skip, separate from the
impacts' because the two rates are nothing alike. An impact is a loud transient and fifty at once is mush; the pooling
sound is quieter and wetter and reads as texture when it overlaps. So the impacts
are thinned to every other splat at 90 ms and this is left dense at 40.

NOT 0, THOUGH, AND THE MEASUREMENT IS WHY. Matt asked for every droplet that
pools up to be heard. Four soldiers gibbed in base1 peaked at 668 MERGES IN ONE
SECOND - the "merges/s" figure on the blood stats line, which IS this sound's
rate. 668 voices does not make the blood loud, it drops the gunfire and the
monsters, and a mixer starved by blood is a fault that presents as a bug in
something else. cl_blood_sound_pool_gap 0 is there for anyone who wants the
literal version.
===============
*/
static void CL_BloodPoolSound(const vec3_t point)
{
    static int last_time;
    static unsigned heard;

    if (!cl_sfx_blood_pool || !CL_BloodSoundAudible(point))
        return;

    if (!CL_BloodSoundReady(&last_time, cl_blood_sound_pool_gap->integer))
        return;

    // ITS OWN THINNING, on its own counter and its own factor. The landing
    // skip used to be the only one there was, and it left the commonest blood
    // sound in the game entirely untouched - so turning it all the way up barely
    // changed what a fight sounded like. See cl_blood_sound_pool_skip.
    if (CL_BloodSoundSkipped(&heard, cl_blood_sound_pool_skip->integer))
        return;

    if (!CL_BloodSoundFloorReady())
        return;

    CL_BloodPlaySoundAt(point, cl_sfx_blood_pool);
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

    // The shape it landed with. Anything the slide adds on top of the length is
    // what trails behind the pool rather than growing around it, so the impact
    // smear stays centred on the point of contact where it belongs.
    //
    // An impact smear COVERS THE AREA THE ROUND SPLAT WOULD HAVE, which is where
    // the cross extent comes from: a droplet thrown sideways spreads along its
    // travel and gives that width up across it. That is right for an impact and
    // wrong for a run, which is exactly why the two extents are now separate
    // numbers instead of one and its reciprocal - see blood_sphere_t::cross.
    p->blood_stretch_base = p->blood_stretch;
    p->blood_cross = 1.f / max(1.f, p->blood_stretch);
    p->blood_cross_base = p->blood_cross;
    CL_BloodClearSlide(p);
    VectorCopy(p->blood_tangent, p->blood_slide_axis);

    // It now has an outline, and the outline may be hanging over a drop. Queued
    // rather than measured here: sixty droplets land between two frames, and
    // measuring all of them at once is the one way to make this expensive.
    //
    // The shove budget is refilled on every landing, so a pool that slid off a
    // crate gets a fresh set of tries wherever it lands. It is a bound on how
    // many times ONE resting place may be argued with, not a lifetime total.
    p->blood_rim = BLOOD_RIM_FULL;
    p->blood_rim_dirty = true;
    p->blood_block = 0;
    p->blood_wall_spread = false;
    p->blood_arrived = false;
    p->blood_drain = 0.f;
    p->blood_edge_tries = max(0, cl_blood_edge_tries->integer);

    // A splat should outlast the spray that made it.  Restart the fade so the
    // remaining life is measured from the impact rather than from the shot.
    //
    // p->time is still stamped when permanent, because it is what
    // CL_RetireBlood sorts on to decide which splat to recycle.
    p->time = cl.time;
    p->alpha = 1.0f;
    p->alphavel = cl_blood_permanent->integer
        ? 0.0f
        : -1.0f / max(0.1f, cl_blood_splat_life->value);
}

/*
===============
OVERHANGING POOLS - measuring the edge, and sliding off it

A landing droplet is a POINT to the collision trace and a DISC to the renderer,
and that difference is the whole bug: the trace answers "did it hit the crate",
which it did, while the disc it turns into reaches several units further out than
the point it hit at.  Land one near the lip of a crate and half the puddle is
drawn over thin air.

Nothing in the simulation can notice, either.  A parked splat returns early from
CL_SimulateBloodSphere - that early return is the reason a floor of settled blood
costs nothing - so it never asks another question about the world after the frame
it landed on.

CL_BloodProbeRim asks that question once.  Eight directions around the rim, each
walked inward until the surface is found under it, giving a reach per direction
that the renderer clips the outline to.  Two things make it affordable, and both
matter:

  - IT IS MEASURED ONCE.  A splat's rim cannot change unless the splat does, and
    the only thing that changes a parked splat is pooling growing its radius -
    which re-queues it.  This is not a per-frame test and must never become one.

  - IT IS AMORTISED.  One wound lands sixty droplets across two frames.  Probing
    at the moment of impact would put sixty splats' worth of traces in one of
    them, which is precisely the shape of every CPU spike this system has had.
    cl_blood_edge_budget splats are probed per frame instead, so a burst is fully
    measured in a fraction of a second and no frame carries more than the budget.

The answer is quantized to four levels, and that is not laziness either: it feeds
the renderer's geometry cache, and anything that varies continuously there
rebuilds the mesh every frame.  Same rule that the fade shrink had to learn.

And where the surface runs out is which way the pool goes over the side, which is
the other half.  A pool with enough of itself over thin air does not stay and does
not shed droplets: THE WHOLE THING LETS GO.

CL_BloodSlideOffEdge does that in three lines of actual work, because every part
of the journey already exists.  It shoves the splat along the surface, toward the
side with no floor under it, and leaves it STUCK.  From there the stuck branch of
CL_SimulateBloodSphere carries it: it steps, probes forward, and when the probe
finds nothing it detaches and falls - "ran off the end of the surface", written
long before any of this.  It then hits the face below, sticks to it, and gravity
projected into that plane runs it down to the floor, which is the same code that
already makes blood run down a wall.

There was a version of this that shed small droplets off the lip instead and left
the pool where it was.  It is not what a pool of blood on a ledge does, and Matt
said so: the pool slides down the surface and lands on the floor.  The shove is
the whole feature; everything after it was already here.
===============
*/

// How far above and below the surface a rim probe reaches. Big enough to find a
// floor that is not perfectly flat under the splat, small enough that it cannot
// find a different floor a step down - which would report the overhang as solid.
#define BLOOD_RIM_LIFT      2.0f

// How far a RUNNING splat travels between rim measurements. Same idea as
// BLOOD_SLIDE_POOL_STEP and for a sharper reason: a probe is eight rays and a
// full per-vertex mesh rebuild, so per frame is out of the question - while
// never is what draws a streak off the end of the surface it is running down.
#define BLOOD_SLIDE_RIM_STEP    4.0f

/*
The two in-plane axes a splat is drawn against, at their drawn lengths.

A DUPLICATE OF splat_basis() IN blood.c, deliberately, and it has to stay one:
the rim reach is measured in the renderer's own parameter space - the angle a
template vertex sits at - or the eight numbers would clip the wrong parts of the
outline. If either construction changes, both do. The client cannot call into the
renderer's copy; blood.c is not linked against effects.c, which is the same
reason the slot capacity is computed twice.
*/
static void CL_BloodSplatAxes(const cparticle_t *p, vec3_t out_t1, vec3_t out_t2)
{
    float stretch = p->blood_stretch;
    // See splat_basis: the two in-plane extents are independent numbers and both
    // are carried on the particle. Must match, or the eight reaches clip the
    // wrong parts of the outline.
    float cross = p->blood_cross;
    bool have_dir = DotProduct(p->blood_tangent, p->blood_tangent) > 0.5f;

    if (have_dir) {
        VectorMA(p->blood_tangent, -DotProduct(p->blood_tangent, p->blood_normal),
                 p->blood_normal, out_t1);
        have_dir = VectorNormalize(out_t1) > 0.1f;
    }

    if (!have_dir) {
        vec3_t seed;
        if (fabsf(p->blood_normal[0]) < 0.577f)      VectorSet(seed, 1.f, 0.f, 0.f);
        else if (fabsf(p->blood_normal[1]) < 0.577f) VectorSet(seed, 0.f, 1.f, 0.f);
        else                                         VectorSet(seed, 0.f, 0.f, 1.f);

        CrossProduct(seed, p->blood_normal, out_t1);
        VectorNormalize(out_t1);
        stretch = 1.f;
        cross = 1.f;
    }

    CrossProduct(p->blood_normal, out_t1, out_t2);
    VectorNormalize(out_t2);

    const float spread = p->radius * max(0.1f, cl_blood_splat_size->value);
    stretch = max(1.f, stretch);
    cross = max(0.05f, min(cross, 16.f));

    VectorScale(out_t1, spread * stretch, out_t1);
    VectorScale(out_t2, spread * cross, out_t2);
}

// How far off the surface the in-plane obstruction ray is fired. High enough to
// clear the joint between two floor brushes - a point trace grazing a shared edge
// finds material at every one of them and would chop every pool along the BSP's
// seams - and low enough to still find a real step.
#define BLOOD_WALL_LIFT     1.5f

// How far a blocking plane has to lean away from the splat's own before it counts
// as a WALL rather than as more of the same surface. cos 60 degrees: a floor
// continuing, a gentle ramp or a bevel is not something blood stops against.
#define BLOOD_WALL_DOT      0.5f

/*
===============
CL_BloodRimReach

How far out the splat may reach along one rim direction, in nibble units, and
whether what stopped it was MATERIAL or a DROP.

Two different questions, and the pool needs both answers:

  - IS THERE SURFACE UNDER THE RIM.  Walked from the outside in, so the common
    answer - supported all the way out - costs a single trace and only the rare
    answers are expensive.

  - IS THERE ANYTHING STANDING IN THE WAY.  One trace out along the surface.  A
    pool is a disc that grows by pooling, and nothing about that growth consults
    the world, so a pool at the foot of a wall simply grew INTO it and drew its
    far side out the other face.  Matt, playing: "if it hits a wall it shouldn't
    clip through the wall, it should stop at the wall".

`startsolid` on the SUPPORT probe still counts as supported, and that is not the
same case: the probe began inside the wall this floor runs into, which is material
meeting the splat rather than a hole, and treating it as a drop would carve a bite
out of every splat that landed against a skirting.  `startsolid` on the OBSTRUCTION
ray means the splat's own centre is buried, which nothing here can measure, so it
declines to answer rather than erasing the splat.

The two are kept apart in the caller because CL_BloodOverhang must not read a wall
as somewhere to fall: a pool blocked all the way round is a pool that has found
its shape, not one that needs shoving.
===============
*/
static int CL_BloodRimReach(const cparticle_t *p, const vec3_t centre,
                            const vec3_t rim, bool *blocked)
{
    static const float levels[] = { 1.5f, 1.0f, 0.5f };
    static const int   reach[]  = { 15, 10, 5 };

    int limit = 15;
    bool wall = false;

    *blocked = false;

    // MATERIAL IN THE WAY, measured first because it bounds the walk below and
    // because a fully blocked direction needs no support probe at all.
    {
        vec3_t start, end;

        VectorMA(centre, BLOOD_WALL_LIFT, p->blood_normal, start);
        VectorMA(start, levels[0], rim, end);

        blood_edge_traces++;
        trace_t tr = CL_TracePoint(start, end, MASK_SOLID, false);

        if (!tr.startsolid && !tr.allsolid && tr.fraction < 1.0f &&
            DotProduct(tr.plane.normal, p->blood_normal) < BLOOD_WALL_DOT) {
            // fraction is of the 1.5x ray, and 15 nibble units IS 1.5x the rim,
            // so the conversion is the identity. Floored onto the same four
            // levels the support walk returns: this feeds the geometry cache, and
            // a continuously varying reach rebuilds the mesh every frame.
            limit = (int)(tr.fraction * 15.f) / 5 * 5;
            wall = true;
        }
    }

    // Wall right against the splat's centre. The walk below would skip every
    // level and fall out of the bottom reporting a DROP, which is the one answer
    // that must not be given here - a pool settled against a skirting would be
    // read as hanging over a ledge and shoved into the wall it is resting on.
    if (wall && limit <= 0) {
        *blocked = true;
        return 0;
    }

    for (int i = 0; i < 3; i++) {
        vec3_t at, start, end;

        // Levels STRICTLY beyond the wall cannot change the answer: whether the
        // floor reaches past a wall the blood cannot cross is not a question the
        // outline has any use for. Levels at or inside it still have to be asked,
        // because a drop this side of the wall wins over the wall.
        if (reach[i] > limit)
            continue;

        VectorMA(centre, levels[i], rim, at);
        VectorMA(at,  BLOOD_RIM_LIFT, p->blood_normal, start);
        VectorMA(at, -BLOOD_RIM_LIFT, p->blood_normal, end);

        blood_edge_traces++;
        trace_t tr = CL_TracePoint(start, end, MASK_SOLID, false);

        if (tr.startsolid || tr.allsolid || tr.fraction < 1.0f) {
            // Blocked means THE WALL IS WHAT STOPPED IT, not that a wall was seen
            // somewhere out there. Levels above the wall were skipped, so the two
            // can only agree when the wall is the binding constraint - and if the
            // floor ran out first, this direction is an overhang and has to stay
            // readable as one, or a pool on a ledge with a wall behind it would
            // never be shoved off the ledge.
            *blocked = wall && reach[i] == limit;
            return reach[i];
        }
    }

    // Nothing underneath, at any level the wall left open. A drop this side of a
    // wall is still a drop, so this is 0 either way.
    return 0;
}

static void CL_BloodProbeRim(cparticle_t *p)
{
    vec3_t t1, t2;
    uint32_t rim = 0;
    uint32_t block = 0;

    p->blood_rim_dirty = false;
    p->blood_rim_dist = p->blood_slide_dist;

    if (!cl_blood_edge->integer) {
        p->blood_rim = BLOOD_RIM_FULL;
        p->blood_block = 0;
        return;
    }

    blood_edge_probes++;
    CL_BloodSplatAxes(p, t1, t2);

    // MEASURE IT ABOUT THE POINT THE OUTLINE IS DRAWN ABOUT, which is not the
    // droplet once the mark has a trail.
    //
    // The renderer hangs the run's share of the length BEHIND the droplet by
    // shifting the whole mesh back along t1 by spread*trail, so for a streak the
    // ellipse being clipped sits well up the wall from p->org. Probing at p->org
    // and clipping the shifted mesh with the answer cuts the outline in a place
    // that has nothing to do with where the surface actually ends. Harmless
    // while only PARKED splats were ever clipped (they have no trail worth the
    // name); a real defect the moment running ones are.
    //
    // t1 already carries the spread*stretch scale, the same vector the renderer
    // scales by trail/stretch, so this is its shift exactly.
    vec3_t centre;
    VectorCopy(p->org, centre);

    const float trail = max(0.f, p->blood_stretch - p->blood_stretch_base);
    if (trail > 0.f)
        VectorMA(centre, -trail / max(1.f, p->blood_stretch), t1, centre);

    for (int i = 0; i < BLOOD_RIM_SAMPLES; i++) {
        const float a = (float)i / BLOOD_RIM_SAMPLES * 6.2831853f;
        vec3_t dir;
        bool blocked;

        VectorScale(t1, cosf(a), dir);
        VectorMA(dir, sinf(a), t2, dir);

        rim |= (uint32_t)CL_BloodRimReach(p, centre, dir, &blocked) << (i * 4);

        if (blocked)
            block |= 1u << i;
    }

    p->blood_rim = rim;
    p->blood_block = block;
}

/*
===============
CL_BloodOverhang

How much of the pool has no floor under it, as a fraction of its rim, and which
way that is.

A COUNT OF UNSUPPORTED SAMPLES WOULD BE THE WRONG MEASURE: a direction that
reaches two thirds of the way out is barely overhanging and one that reaches
nothing at all is completely off the side, and treating those as the same thing
either shoves pools that should stay or leaves pools that should go. The reach is
already a number, so each sample contributes what it actually lost.

The direction is the sum of the unsupported sample directions weighted the same
way, which for a pool on a lip points squarely out over the drop and for one in a
corner - overhanging two opposite ways at once, if such a thing is measured -
cancels toward nothing and correctly declines to shove it anywhere.
===============
*/
static float CL_BloodOverhang(const cparticle_t *p, vec3_t out_dir)
{
    vec3_t t1, t2;
    float lost = 0.f;

    VectorClear(out_dir);

    if (p->blood_rim == BLOOD_RIM_FULL)
        return 0.f;

    CL_BloodSplatAxes(p, t1, t2);

    for (int i = 0; i < BLOOD_RIM_SAMPLES; i++) {
        const int n = (int)((p->blood_rim >> (i * 4)) & 15u);

        // 10 is the rim itself. Short of that is blood with no floor under it;
        // beyond it is only the outward lobes pt_blood_wobble adds, which are not
        // worth moving a pool over.
        if (n >= 10)
            continue;

        // A WALL IS NOT SOMEWHERE TO FALL. This sample came back short because
        // there is material in the way, not because the floor ran out, and a pool
        // that has settled against a skirting is exactly where it should be. Read
        // as overhang it would be shoved INTO the wall, which is the one direction
        // that cannot work.
        if (p->blood_block & (1u << i))
            continue;

        const float w = (10.f - (float)n) / 10.f;
        const float a = (float)i / BLOOD_RIM_SAMPLES * 6.2831853f;
        vec3_t dir;

        VectorScale(t1, cosf(a), dir);
        VectorMA(dir, sinf(a), t2, dir);
        VectorNormalize(dir);

        VectorMA(out_dir, w, dir, out_dir);
        lost += w;
    }

    return lost / (float)BLOOD_RIM_SAMPLES;
}

/*
===============
CL_BloodSlideOffEdge

Shoves an overhanging pool toward the side with no floor under it and leaves it
STUCK, which is the whole of the feature - see the block above for why everything
after the shove is code that was already here.

Left stuck rather than made airborne on purpose. Airborne would be a pool
teleporting into a droplet in mid-air over the lip, and worse, a droplet released
a hundredth of a unit above the floor it was sitting on falls into that same floor
on its first step and re-sticks where it started, over and over. Sliding is
already the motion that carries a droplet along a surface and off the end of it.
===============
*/
static void CL_BloodSlideOffEdge(cparticle_t *p, const vec3_t dir)
{
    vec3_t along;
    float into;

    // Flatten the shove into the surface. The sample directions are already in
    // the plane, but their weighted sum need not be exactly - and a component
    // into the wall would be absorbed while one out of it would launch the pool.
    into = DotProduct(dir, p->blood_normal);
    VectorMA(dir, -into, p->blood_normal, along);

    if (VectorNormalize(along) < 0.01f)
        return;                 // overhanging every way at once; nowhere to go

    VectorScale(along, max(1.f, cl_blood_edge_push->value), p->vel);

    // It is a pool that has started to move, so the reach it was clipped to no
    // longer describes where it is. Cleared, and re-measured on the first
    // distance step of the run rather than only when it next comes to rest.
    p->blood_rim = BLOOD_RIM_FULL;
    p->blood_rim_dirty = true;
    p->blood_block = 0;
    p->blood_rim_dist = p->blood_slide_dist - BLOOD_SLIDE_RIM_STEP;
    p->blood_edge_tries--;
}

/*
===============
Riding a brush model

A splat's org is a WORLD position, and nothing in the simulation re-derives it
once the droplet has parked - the stuck branch returns early the moment the
droplet stops sliding, which is the whole point of parking (a splat that moves
misses the renderer's geometry cache and pays a full per-vertex rebuild every
frame).  So a splat on a door is a world position that the door then drives out
from under, leaving the blood hanging in the air where the door used to be.

Same artifact as the bounding-box one, different cause: there the surface was
never where it was drawn, here it stops being where it was drawn.

A rigid transform - one origin, one set of angles - is the complete truth for a
brush model, so for doors, lifts and platforms this is exact.  For a body it is
an approximation: the blood rides the monster's origin and yaw but not its
animation, so a splat on a walking soldier holds still against a skin that keeps
moving under it.  Cheap and wrong by a few units beats free and wrong by ten.

THE POSE USED TO ATTACH AND THE POSE USED TO DRAW ARE NOT ALWAYS THE SAME ONE.
CL_TracePoint runs against current.origin/current.angles, the unlerped server
state, while create_entity_matrix draws the model at the pose interpolated with
cl.lerpfrac; MOD_TraceMesh below is handed the render pose because that is where
the triangles it tests are actually drawn.  So the caller passes in the pose its
contact point belongs to, and the world position is always rebuilt against the
RENDER pose - which is what puts the splat on the surface the player can see
rather than a whole server frame of door travel away from it.
===============
*/

// The pose the renderer will draw this entity at, matching the origin and angle
// interpolation in CL_AddPacketEntities.  False when the entity is not in the
// current server frame at all, i.e. it has been freed.
static bool CL_BloodEntityRenderPose(int entnum, int entid, vec3_t origin, vec3_t angles)
{
    const centity_t *cent;

    if (entnum < 0 || entnum >= MAX_EDICTS)
        return false;

    cent = &cl_entities[entnum];

    // Gone from the frame, or the slot has been recycled for a different
    // entity since the droplet landed.
    if (cent->serverframe != cl.frame.number || cent->id != entid)
        return false;

    // Only brush models are ever ridden. A body's pose is not a rigid transform -
    // it animates - so riding one is what put splats in mid-air when the death
    // animation moved out from under them. Bodies get CL_BloodRunOffModel now.
    if (cent->current.solid != PACKED_BSP)
        return false;

    LerpVector(cent->prev.origin, cent->current.origin, cl.lerpfrac, origin);
    LerpAngles(cent->prev.angles, cent->current.angles, cl.lerpfrac, angles);
    return true;
}

// Record the contact in the entity's own frame of reference.  origin/angles are
// the pose point and normal were measured against - see the note above about
// which trace produces which.
static void CL_BloodAttachToEntity(cparticle_t *p, const centity_t *cent,
                                   const vec3_t origin, const vec3_t angles,
                                   const vec3_t point, const vec3_t normal)
{
    vec3_t axis[3];

    p->blood_ent = (int)(cent - cl_entities);
    p->blood_ent_id = cent->id;

    VectorSubtract(point, origin, p->blood_local_org);
    VectorCopy(normal, p->blood_local_normal);

    if (!VectorEmpty(angles)) {
        AnglesToAxis(angles, axis);
        RotatePoint(p->blood_local_org, axis);
        RotatePoint(p->blood_local_normal, axis);
    }
}

static inline void CL_BloodDetachFromEntity(cparticle_t *p)
{
    p->blood_ent = -1;
    p->blood_ent_id = 0;
}

// Put a riding splat back where its surface has moved to.  Returns false when
// the entity is gone, which drops the droplet back into free fall rather than
// leaving it stranded in mid-air.
static bool CL_BloodRideEntity(cparticle_t *p)
{
    vec3_t origin, angles, axis[3];

    if (p->blood_ent < 0)
        return true;

    if (!CL_BloodEntityRenderPose(p->blood_ent, p->blood_ent_id, origin, angles)) {
        CL_BloodDetachFromEntity(p);
        return false;
    }

    VectorCopy(p->blood_local_org, p->org);
    VectorCopy(p->blood_local_normal, p->blood_normal);

    if (!VectorEmpty(angles)) {
        AnglesToAxis(angles, axis);
        TransposeAxis(axis);
        RotatePoint(p->org, axis);
        RotatePoint(p->blood_normal, axis);
    }

    VectorAdd(p->org, origin, p->org);
    return true;
}

// Whether a riding splat is allowed to run downhill this frame.
//
// Never on a body: what it is stuck to is model geometry, and model geometry is
// not in the collision world at all, so the slide's re-trace probe finds nothing
// and every splat on the corpse detaches at once.
//
// On a brush model, only while it is standing still.  The probe runs against the
// collision pose while the droplet sits at the render pose, and on a moving door
// those are up to a full server frame of travel apart - same missed probe, same
// mass detach.  A door only moves for a second or two at a time, so riding
// rigidly through the motion and resuming the run afterwards costs nothing
// anyone can see.
static bool CL_BloodMaySlide(const cparticle_t *p)
{
    const centity_t *cent;

    if (p->blood_ent < 0)
        return true;

    cent = &cl_entities[p->blood_ent];

    if (cent->current.solid != PACKED_BSP)
        return false;

    return VectorCompare(cent->prev.origin, cent->current.origin)
        && VectorCompare(cent->prev.angles, cent->current.angles);
}

/*
===============
CL_BloodTraceModels

The other half of the bounding-box problem.  CL_TracePoint has been told to
ignore the boxes of monsters, corpses, players and crates, so without this a
droplet flies through a body as if it were not there.  With it, the droplet is
tested against the model's ACTUAL triangles and lands on the surface that is
drawn - which is the only version of blood-on-a-body that does not float.

Costs are kept off the common path three ways: nothing happens unless the step
crosses an entity's box in the first place, MOD_TraceMesh caches the posed model
and rejects almost every triangle on a bounding box, and a hard per-frame budget
bounds the worst case a burst can produce.  Returns the closest hit that beats
`best`, which starts as the world trace's fraction so the world always wins a tie.
===============
*/

// Monsters routinely reach outside the axial box the server packs for them - an
// arm, a wing, a gladiator's gun - so the box is widened before it is used to
// decide whether the mesh is worth testing.  Too small silently loses hits at the
// edges; too large only buys mesh traces that then miss.
#define BLOOD_MODEL_BOX_MARGIN  24.0f

// Mesh traces allowed per client frame.  One wound throws sixty droplets and a
// mesh trace is thousands of times a box test, so this is the backstop that
// keeps a bad case bad rather than catastrophic. Hit only while blood is flying
// through several bodies at once, and losing a hit means a droplet passes
// through - the behaviour with the feature off, not a glitch.
#define BLOOD_MODEL_TRACE_BUDGET 384

static int blood_model_traces, blood_model_hits;

static bool CL_BloodTraceModels(const vec3_t start, const vec3_t end,
                                float *best, vec3_t out_normal, centity_t **out_ent)
{
    vec3_t seg_mins, seg_maxs;
    bool found = false;

    if (!cl_blood_model_collision->integer || !MOD_TraceMesh)
        return false;

    for (int i = 0; i < 3; i++) {
        seg_mins[i] = min(start[i], end[i]) - BLOOD_MODEL_BOX_MARGIN;
        seg_maxs[i] = max(start[i], end[i]) + BLOOD_MODEL_BOX_MARGIN;
    }

    for (int i = 0; i < cl.numSolidEntities; i++) {
        centity_t *cent = cl.solidEntities[i];
        const model_t *model;
        mod_pose_t pose;
        vec3_t normal;
        float frac;
        int j;

        if (cent->current.solid == PACKED_BSP)
            continue;   // real geometry already; CL_TracePoint has it

        for (j = 0; j < 3; j++) {
            if (cent->current.origin[j] + cent->maxs[j] < seg_mins[j])
                break;
            if (cent->current.origin[j] + cent->mins[j] > seg_maxs[j])
                break;
        }
        if (j < 3)
            continue;

        // modelindex 255 is not an index at all - it means "look the model up in
        // the clientinfo for this player's chosen skin", and cl.model_draw[255]
        // is somebody else's model entirely. Other players therefore get no mesh
        // trace and blood passes through them, which is the safe half of the
        // trade; resolving it would mean duplicating the clientinfo fallback
        // chain out of CL_AddPacketEntities.
        if (cent->current.modelindex <= 0 || cent->current.modelindex >= 255)
            continue;

        model = MOD_ForHandle(cl.model_draw[cent->current.modelindex]);
        if (!model || model->type != MOD_ALIAS)
            continue;

        if (blood_model_traces >= BLOOD_MODEL_TRACE_BUDGET)
            return found;

        blood_model_traces++;

        // The RENDER pose, not the collision one: these are the triangles the
        // player can see, so the hit has to be where they are drawn.
        LerpVector(cent->prev.origin, cent->current.origin, cl.lerpfrac, pose.origin);
        LerpAngles(cent->prev.angles, cent->current.angles, cl.lerpfrac, pose.angles);
        pose.scale = cent->current.scale;
        pose.frame = cent->current.frame;

        if (!MOD_TraceMesh(model, &pose, start, end, &frac, normal))
            continue;

        if (frac >= *best)
            continue;

        *best = frac;
        VectorCopy(normal, out_normal);
        *out_ent = cent;
        if (!found)
            blood_model_hits++;
        found = true;
    }

    return found;
}


/*
===============
CL_BloodTrace_f

Console command `bloodtrace`: fire one ray down the crosshair and print what the
box trace and the mesh trace each say about it.

This exists because the two failure modes of CL_BloodTraceModels look identical
from the outside - a mesh trace that is silently wrong and a burst of blood that
simply never crosses a body both show up as zero splats riding an entity.  Aim at
a monster and this separates them in one line: a working mesh trace reports a
fraction slightly LARGER than the box trace's (the box is bigger than the body)
and a normal pointing back at the camera.
===============
*/
static void CL_BloodTrace_f(void)
{
    if (cls.state != ca_active) {
        Com_Printf("bloodtrace: not connected" "\n");
        return;
    }

    if (!MOD_TraceMesh) {
        Com_Printf("bloodtrace: no mesh trace in this renderer" "\n");
        return;
    }

    Com_Printf("bloodtrace: %d solid entities" "\n", cl.numSolidEntities);

    for (int i = 0; i < cl.numSolidEntities; i++) {
        centity_t *cent = cl.solidEntities[i];
        const model_t *model;
        mod_pose_t pose;
        vec3_t start, end, dir, normal, point;
        float frac, len;

        if (cent->current.solid == PACKED_BSP) {
            Com_Printf("  ent %3d: brush model" "\n", (int)(cent - cl_entities));
            continue;
        }

        if (cent->current.modelindex <= 0 || cent->current.modelindex >= 255) {
            Com_Printf("  ent %3d: modelindex %d - skipped" "\n",
                (int)(cent - cl_entities), cent->current.modelindex);
            continue;
        }

        model = MOD_ForHandle(cl.model_draw[cent->current.modelindex]);
        if (!model || model->type != MOD_ALIAS) {
            Com_Printf("  ent %3d: no alias model" "\n", (int)(cent - cl_entities));
            continue;
        }

        // Straight at the entity origin and well past it, so the aim cannot be
        // what fails.
        VectorCopy(cl.refdef.vieworg, start);
        VectorSubtract(cent->current.origin, start, dir);
        len = VectorNormalize(dir);
        VectorMA(start, len + 128.0f, dir, end);

        LerpVector(cent->prev.origin, cent->current.origin, cl.lerpfrac, pose.origin);
        LerpAngles(cent->prev.angles, cent->current.angles, cl.lerpfrac, pose.angles);
        pose.scale = cent->current.scale;
        pose.frame = cent->current.frame;

        if (MOD_TraceMesh(model, &pose, start, end, &frac, normal)) {
            LerpVector(start, end, frac, point);
            Com_Printf("  ent %3d: %s frame %d dist %.1f -> HIT at %.1f units, "
                       "%.1f %.1f %.1f nrm %.2f %.2f %.2f" "\n",
                (int)(cent - cl_entities), model->name, cent->current.frame, len,
                frac * (len + 128.0f), point[0], point[1], point[2],
                normal[0], normal[1], normal[2]);
        } else {
            Com_Printf("  ent %3d: %s frame %d dist %.1f org %.0f %.0f %.0f -> MISS" "\n",
                (int)(cent - cl_entities), model->name, cent->current.frame, len,
                cent->current.origin[0], cent->current.origin[1], cent->current.origin[2]);
        }
    }
}


/*
===============
CL_BloodRunOffModel

BLOOD NEVER STICKS TO A BODY.  It arrives, loses everything it had going into the
flesh, and runs down the outside until it drops off onto the floor.

That is not a stylistic choice, it is the only thing that works.  A splat is a
static piece of geometry and a body is not a static surface: a monster's death
animation alone moves its skin several feet over about two seconds, so a splat
placed on the shoulder of a standing soldier is hanging in mid-air by the time the
corpse has finished falling.  Riding the entity's origin and angles does not fix
it either - during a death the origin barely moves and the SKIN does all the
travelling.  Following the animation properly would mean storing a triangle and
barycentric coordinates and re-posing the splat every frame, which is both a lot
of machinery and a guarantee of missing the renderer's geometry cache for as long
as the body is moving.

Running off costs none of that and is what blood does anyway.  The droplet stays
AIRBORNE throughout, so if the body animates out from under it, it simply falls -
there is no state left behind to strand.

The minimum run speed is the part that is easy to leave out and then wonder about:
gravity projected into a surface is ZERO on anything facing straight up, so a
droplet landing squarely on a shoulder or the flat of a back has nothing to move
it and hovers there until its air life expires - the same artifact by another
route.  Steepest descent gives it a direction, and on a genuinely flat face the
direction it arrived from does.
===============
*/

// How far off the surface a running droplet is held. Enough that the next step's
// trace does not start inside the triangle it just hit, small enough that the
// droplet still reads as touching the body.
#define BLOOD_FLESH_CLEARANCE   0.5f

static void CL_BloodRunOffModel(cparticle_t *p, const vec3_t point, const vec3_t normal)
{
    vec3_t vel, dir;
    float into, speed, len;
    bool have_dir = true;

    VectorMA(point, BLOOD_FLESH_CLEARANCE, normal, p->org);

    // Everything heading into the body is absorbed; what is left is damped,
    // because flesh is not a bouncy surface.
    into = DotProduct(p->vel, normal);
    VectorMA(p->vel, -into, normal, vel);
    VectorScale(vel, max(0.f, cl_blood_flesh_damp->value), vel);

    // Downhill along this surface. Zero length means the face points straight up.
    VectorSet(dir, 0.f, 0.f, -1.f);
    into = DotProduct(dir, normal);
    VectorMA(dir, -into, normal, dir);
    len = VectorNormalize(dir);

    if (len < 0.1f) {
        // Flat. Carry on the way it was already going, in the surface plane.
        VectorCopy(vel, dir);
        if (VectorNormalize(dir) < 0.01f) {
            // Arrived dead square with nothing to inherit. Any direction in the
            // plane will do; the seed keeps a burst from leaving in lockstep.
            vec3_t any = { cosf(p->seed * 6.283f), sinf(p->seed * 6.283f), 0.f };
            into = DotProduct(any, normal);
            VectorMA(any, -into, normal, dir);
            if (VectorNormalize(dir) < 0.01f)
                have_dir = false;   // degenerate normal; let gravity do it
        }
    }

    if (have_dir) {
        speed = DotProduct(vel, dir);
        if (speed < cl_blood_flesh_run->value)
            VectorMA(vel, cl_blood_flesh_run->value - speed, dir, vel);
    }

    // Hold it against the surface, so it follows the body's shape instead of
    // separating at the first vertical face and falling past it. It is re-traced
    // every step, so being pulled a little into the body is corrected rather than
    // accumulated - and in the worst case the droplet ends up inside, where the
    // front-face-only test sends it straight out the far side.
    VectorMA(vel, -cl_blood_flesh_cling->value, normal, vel);

    VectorCopy(vel, p->vel);

    // Still in flight, and DELIBERATELY NOT re-clocked.
    //
    // Restarting time/alpha here - which is what this did at first - makes a
    // droplet in continuous contact with a body immortal: it re-hits every frame,
    // resets its own age every frame, and holds a slot out of cl_blood_max
    // forever while never being old enough to retire. A handful of those loitering
    // on a corpse is enough to squeeze the real splats out of the budget.
    //
    // Nothing needs the reset anyway. cl_blood_air_life is measured from spawn and
    // defaults to 15 seconds, while running down even a berserk takes well under
    // one; and CL_BloodStick restarts the clock properly when it finally lands,
    // which is where the splat's own life should be measured from.
    p->blood_state = BLOOD_AIRBORNE;
}

/*
===============
CL_BloodReshapeSlide

Turns a moving pool's long axis toward the direction it is actually travelling,
and draws it out as it goes.

The bug this fixes: CL_BloodStick captured blood_tangent from the impact and
NOTHING EVER TOUCHED IT AGAIN.  The stuck branch updates org and blood_normal as a
droplet runs downhill - it will carry a splat around a corner onto a whole
different plane - but the elongation stayed pointing wherever the droplet happened
to be flying when it first landed.  Matt saw it directly: pools landing smeared
along X, then sliding away along Y still smeared along X.

A directional mark means "the blood went that way".  That is true of an impact and
it is true of a run, so the same field has to answer for both.

TWO SEPARATE QUANTIZERS, AND THEY ARE THE REASON THIS IS AFFORDABLE.  Everything
here writes into blood_sphere_t, which the renderer caches on by memcmp of the
whole struct.  A sliding splat already misses the exact-match tier because its
origin moves, but it still hits the translate-only tier - "same mesh, moved" -
which rewrites two per-droplet values and leaves every vertex alone.  Changing the
tangent or the stretch every frame drops it out of that tier too and pays the full
per-vertex rebuild (splat_basis, an inverse-scale normal transform, a normalize and
an encode_normal PER VERTEX) for the whole slide.  That is the same mistake the
fade shrink made, and the general rule this system keeps re-learning: anything
feeding a content-keyed cache must change in STEPS, not continuously.

  - The AXIS is turned smoothly in blood_slide_axis, which is not in the struct,
    and committed to blood_tangent only when the two have diverged by more than
    BLOOD_SLIDE_TURN_STEP.  Thresholding the smoothed value against itself would
    not work - a small per-frame step never exceeds its own threshold and the
    axis would never move at all.
  - The STRETCH is floored to BLOOD_SLIDE_STRETCH_STEP steps and only ever
    raised, so it cannot chatter across a boundary.

A long slide therefore costs on the order of a dozen rebuilds instead of one per
frame.
===============
*/

// ~13 degrees of divergence before a new long axis is committed, and eighths of
// a unit of stretch. Both are cache granularity, not looks - deliberately NOT
// cvars, because an archived tuning knob silently overrode the menu once already
// (see pt_blood_splat_tess).
#define BLOOD_SLIDE_TURN_STEP       0.974f      // cos(13 degrees)
#define BLOOD_SLIDE_STRETCH_STEP    8.0f

/*
===============
CL_BloodReshapeOnAxis

Puts the mark's long axis somewhere new, and re-expresses the shape it already
has in that new frame rather than carrying it across.

THIS IS THE DIFFERENCE BETWEEN A MARK THAT DEFORMS AND ONE THAT PIVOTS, and it is
what Matt asked for: "if the blood splats on the wall horizontally the shape
should become larger vertically as it slides down the wall and kind of shrink the
horizontal size as it slides down."

The old code assigned the new axis and left the extents alone, so a mark two
droplets long and half a droplet wide, turned ninety degrees, came out two
droplets long and half a droplet wide the OTHER way - a needle swinging round like
a compass. What it should become is a mark half a droplet tall and two droplets
wide, which then grows tall and narrows as the run draws it out.

For an ellipse, the half-width along a direction at angle t from its long axis is
sqrt(A*A*cos^2 t + B*B*sin^2 t), and across it the same with cos and sin swapped.
That is the honest re-expression and it is the whole of the arithmetic here.

IT HAS TO BE RENORMALISED, and that is the non-obvious part.  Those two widths
preserve A*A + B*B, not A*B, so each turn drags the extents toward each other and
INFLATES the area they bound - a mark would grow simply for having changed
direction, and over a long curving run it would keep growing.  Scaling both back
to the area the mark had makes the turn a pure reshape.

The landing shape (blood_stretch_base / blood_cross_base) goes through the same
rotation, because the settle relaxes back to it and relaxing onto an axis the mark
no longer has would swing it back round again at the end of every run.

Quantized to the same eighths the growth uses: these two feed a memcmp-keyed
geometry cache.  A committed turn is a full per-vertex rebuild either way, but
without the quantization every frame AFTER it would be one too.
===============
*/
static void CL_BloodReshapeOnAxis(cparticle_t *p, const vec3_t axis)
{
    vec3_t old;

    if (DotProduct(p->blood_tangent, p->blood_tangent) < 0.5f) {
        VectorCopy(axis, p->blood_tangent);
        return;                 // it was round; there is no shape to re-express
    }

    VectorMA(p->blood_tangent, -DotProduct(p->blood_tangent, p->blood_normal),
             p->blood_normal, old);

    if (VectorNormalize(old) < 0.01f) {
        VectorCopy(axis, p->blood_tangent);
        return;
    }

    const float c = max(-1.f, min(1.f, DotProduct(old, axis)));

    // sin^2 of the turn: 0 when the new axis is the one it already has, 1 when
    // the mark is being asked to run square across itself.
    const float ss = 1.f - c * c;

    const float a = max(1.f, p->blood_stretch);
    const float b = max(0.05f, p->blood_cross);
    const float ba = max(1.f, p->blood_stretch_base);
    const float bb = max(0.05f, p->blood_cross_base);

    // GEOMETRIC INTERPOLATION BETWEEN (a,b) AND (b,a), weighted by that. Three
    // properties, and all three are load-bearing:
    //
    //   - EXACTLY area-preserving at every angle (a'*b' == a*b identically), so
    //     a mark can never grow or shrink merely by changing direction.
    //   - The IDENTITY for a small turn, so a run continuing in very nearly its
    //     own direction is left completely alone.
    //   - A clean SWAP at ninety degrees, which is the whole point of the
    //     function: a bar that starts running across itself becomes a mark as
    //     wide as the bar was long, rather than the same bar facing a new way.
    //
    // THE FIRST VERSION OF THIS ATE STREAKS ALIVE and it is worth recording why.
    // It took the ellipse's BOUNDING half-widths in the new frame and rescaled
    // those back to the old area. That is reasonable for a fat ellipse and
    // catastrophic for a thin one: the bounding box of a slightly tilted 16:1
    // streak is several times the streak's own area, so the rescale HALVED the
    // length on every thirteen-degree commit. A run whose direction wandered at
    // all deleted itself in a handful of steps.
    const float na = powf(a, 1.f - ss) * powf(b, ss);
    const float nb = powf(b, 1.f - ss) * powf(a, ss);
    const float nba = powf(ba, 1.f - ss) * powf(bb, ss);
    const float nbb = powf(bb, 1.f - ss) * powf(ba, ss);

    p->blood_stretch = max(1.f, floorf(na * BLOOD_SLIDE_STRETCH_STEP) /
                                BLOOD_SLIDE_STRETCH_STEP);
    p->blood_cross = max(0.125f, floorf(nb * BLOOD_SLIDE_STRETCH_STEP) /
                                 BLOOD_SLIDE_STRETCH_STEP);

    p->blood_stretch_base = max(1.f, nba);
    p->blood_cross_base = max(0.125f, nbb);

    // The narrowing below decays from wherever the mark is NOW. Without this a
    // turn that widened it would immediately be undone by a decay still aimed at
    // the width it had three steps ago.
    p->blood_cross_run = p->blood_cross;
    p->blood_narrow_dist = 0.f;

    VectorCopy(axis, p->blood_tangent);
}

/*
===============
CL_BloodSpreadAlongWall

A pool that has run into a wall grows ALONG the foot of it instead of through it.

Clipping the outline (CL_BloodRimReach's obstruction ray) stops the pool drawing
out the far face of the wall, which is the bug Matt reported.  On its own, though,
it only deletes the blood: a pool that grew to twice the width of the alcove it is
in is simply drawn as a smaller pool.  He asked for the other half - "it should
stop at the wall and start to spread out more adjacent to the wall" - and that is
conservation.  The blood that cannot go through the wall goes sideways along it.

The shape that wants is one the mark can already express: an ellipse with its long
axis parallel to the wall.  So this points the long axis along the blocked
direction's perpendicular, through the same area-preserving re-expression the
slide uses, and then grows it by roughly the area the clip took away.

ONCE PER RESTING PLACE, and the flag is load-bearing rather than a tuning choice.
Growing along the wall moves the outline, which re-queues the rim probe, which
finds the new blocked directions of the now-longer pool and would grow it again,
and again, for the rest of the level.  Cleared by CL_BloodStick, so a pool that
slides off and lands somewhere new gets to do it once more there.

Returns true when it changed the shape, which is the caller's cue to re-measure.
===============
*/
static bool CL_BloodSpreadAlongWall(cparticle_t *p)
{
    vec3_t t1, t2, into, along;
    float lost = 0.f;

    if (!p->blood_block || p->blood_wall_spread)
        return false;

    const float gain = cl_blood_wall_spread->value;

    if (gain <= 1.f)
        return false;

    CL_BloodSplatAxes(p, t1, t2);
    VectorClear(into);

    // Weighted by how much each blocked direction lost, exactly as
    // CL_BloodOverhang weights the unsupported ones - a direction stopped just
    // short of the rim has barely been clipped, and counting it the same as one
    // stopped at the centre would aim the growth at the wrong wall. A pool in a
    // corner, blocked two opposite ways, cancels toward zero and declines.
    for (int i = 0; i < BLOOD_RIM_SAMPLES; i++) {
        const int n = (int)((p->blood_rim >> (i * 4)) & 15u);

        if (!(p->blood_block & (1u << i)) || n >= 10)
            continue;

        const float w = (10.f - (float)n) / 10.f;
        const float a = (float)i / BLOOD_RIM_SAMPLES * 6.2831853f;
        vec3_t dir;

        VectorScale(t1, cosf(a), dir);
        VectorMA(dir, sinf(a), t2, dir);
        VectorNormalize(dir);

        VectorMA(into, w, dir, into);
        lost += w;
    }

    lost /= (float)BLOOD_RIM_SAMPLES;

    if (lost < 0.05f)
        return false;           // a sliver off one side; not worth reshaping for

    VectorMA(into, -DotProduct(into, p->blood_normal), p->blood_normal, into);

    if (VectorNormalize(into) < 0.01f)
        return false;           // walled in symmetrically; there is no "along"

    CrossProduct(p->blood_normal, into, along);

    if (VectorNormalize(along) < 0.01f)
        return false;

    // The mark keeps its area through the turn, so all that is left is to add
    // back what the clip takes off. Bounded by cl_blood_wall_spread, or a pool
    // wedged into a corner would draw itself into a hairline across the room.
    const float want = min(gain, 1.f / max(0.2f, 1.f - lost));

    CL_BloodReshapeOnAxis(p, along);

    const float q = floorf(p->blood_stretch * want * BLOOD_SLIDE_STRETCH_STEP) /
                    BLOOD_SLIDE_STRETCH_STEP;

    if (q > p->blood_stretch) {
        p->blood_stretch = q;
        // The spread is a property of where the pool is LYING, not of a run, so
        // it belongs in the landing shape. Left out of it, the settle would hand
        // the length straight back and the pool would crawl into the wall again.
        p->blood_stretch_base = q;
    }

    VectorCopy(along, p->blood_slide_axis);
    p->blood_wall_spread = true;
    return true;
}

static void CL_BloodReshapeSlide(cparticle_t *p, float dt, float speed)
{
    vec3_t dir;
    float into;

    // The direction of travel, in the surface plane. vel is already in the plane
    // when gravity put it there, but a droplet that has just turned a corner is
    // carrying a velocity measured against the plane it came from.
    VectorCopy(p->vel, dir);
    into = DotProduct(dir, p->blood_normal);
    VectorMA(dir, -into, p->blood_normal, dir);

    if (VectorNormalize(dir) < 0.01f)
        return;

    if (DotProduct(p->blood_slide_axis, p->blood_slide_axis) < 0.5f) {
        // Landed square, so it has no axis yet - it was a round dot. The
        // direction it is leaving in is the only one it has ever had.
        VectorCopy(dir, p->blood_slide_axis);
    } else if (DotProduct(p->blood_slide_axis, dir) < -0.9f) {
        // Reversing. The ellipse is symmetric, so at 180 degrees the shape is
        // identical either way and only the trail flips - but interpolating
        // through it passes through zero length and a degenerate basis, so snap.
        // Reached by a pool shoved back the way it came off an edge.
        VectorCopy(dir, p->blood_slide_axis);
    } else {
        const float f = min(1.f, max(0.f, cl_blood_slide_turn->value) * dt);
        vec3_t want;

        VectorScale(p->blood_slide_axis, 1.f - f, want);
        VectorMA(want, f, dir, want);

        if (VectorNormalize(want) > 0.01f)
            VectorCopy(want, p->blood_slide_axis);
    }

    // Commit the axis only once it has actually gone somewhere - and re-express
    // the shape onto it rather than carrying it across. See CL_BloodReshapeOnAxis.
    if (DotProduct(p->blood_tangent, p->blood_tangent) < 0.5f ||
        DotProduct(p->blood_tangent, p->blood_slide_axis) < BLOOD_SLIDE_TURN_STEP)
        CL_BloodReshapeOnAxis(p, p->blood_slide_axis);

    // Draw it out with distance run, not with time: a pool creeping to a halt
    // should stop lengthening as it stops moving, and an exponential approach on
    // distance gives that for free while never quite reaching the cap.
    p->blood_slide_dist += speed * dt;
    p->blood_narrow_dist += speed * dt;

    // AND NARROW ACROSS IT over the same run, which is the other half of the
    // deformation. A run is fed by the blood moving through it, so what it draws
    // out behind is about as wide as that stream - not as wide as the mark it is
    // draining. Decayed toward one droplet's width and LOWERED ONLY, the mirror
    // of the length's raise-only growth and for the same chatter reason.
    if (cl_blood_slide_narrow->value > 0.f && p->blood_cross > 1.f) {
        const float nlen = max(1.f, cl_blood_slide_narrow->value);
        const float want = 1.f + (max(1.f, p->blood_cross_run) - 1.f) *
            expf(-p->blood_narrow_dist / nlen);
        const float q = ceilf(want * BLOOD_SLIDE_STRETCH_STEP) / BLOOD_SLIDE_STRETCH_STEP;

        if (q < p->blood_cross)
            p->blood_cross = max(0.125f, q);
    }

    // NOT cl_blood_slide_stretch directly: how long this mark may be drawn
    // depends on how big it is AND on how steep the surface under it is. See
    // CL_BloodStretchLimit.
    const float cap = CL_BloodStretchLimit(p);

    // A RUN THAT HAS REACHED THE FLOOR HAS TO GIVE THE LENGTH BACK ON ARRIVAL.
    //
    // The growth below is raise-only, which is right while the ceiling is fixed
    // and wrong the moment a droplet slides off a wall onto a floor: the ceiling
    // collapses by cl_blood_streak_wall and nothing here would lower the mark,
    // so a 50-unit wall streak would lie on the floor at full length until it
    // parked and the settle got to it a second later. Matt asked for it to
    // shorten when it hits the floor, and this is where it hits the floor - the
    // stuck branch re-attaches to the new plane, so blood_normal is already the
    // floor's by the time this runs.
    //
    // CEILINGED to the same eighths the growth is FLOORED to, so it steps rather
    // than sliding: this feeds a memcmp-keyed geometry cache, and a surface that
    // flattens out gradually would otherwise pay a full per-vertex rebuild every
    // frame the whole way down. Monotone downward, so it terminates.
    if (p->blood_stretch > cap) {
        const float q = max(1.f, ceilf(cap * BLOOD_SLIDE_STRETCH_STEP) /
                                 BLOOD_SLIDE_STRETCH_STEP);

        if (q < p->blood_stretch)
            p->blood_stretch = q;

        // The impact smear came from a surface it may no longer be on, and the
        // settle relaxes TO this value - leaving it above the ceiling would have
        // the settle refuse to finish the job the clamp just started.
        if (p->blood_stretch_base > q)
            p->blood_stretch_base = q;
    }

    if (cap > p->blood_stretch_base) {
        const float len = max(1.f, cl_blood_slide_len->value);
        const float target = p->blood_stretch_base +
            (cap - p->blood_stretch_base) * (1.f - expf(-p->blood_slide_dist / len));
        const float q = floorf(target * BLOOD_SLIDE_STRETCH_STEP) / BLOOD_SLIDE_STRETCH_STEP;

        // Raised only. A stretch that could also fall would chatter back and
        // forth across a step boundary and rebuild the mesh on every crossing.
        if (q > p->blood_stretch)
            p->blood_stretch = q;
    }
}

// How far a sliding pool travels between checks for another pool to merge into.
// The check scans every stuck splat and cl_blood_max can be 2048, so running it
// per sliding droplet per frame is real work for an answer that cannot change in
// a fraction of a unit.
#define BLOOD_SLIDE_POOL_STEP   2.0f



// Defined below, with the settle it is shared with.
static bool CL_BloodRelaxShape(cparticle_t *p, float dt);

// How far below the foot of a run to look for ground to pool on. A run ends at
// the inside of a corner, so the floor is a unit or two away; far more than that
// and the mark really has gone over a lip and should fall.
#define BLOOD_ARRIVE_REACH  24.0f

// How many deliveries a draining mark makes. Each one is a scan of every stuck
// splat, and a sixth of the mark is already a visible step up in the pool -
// dribbling a sixtieth per frame would cost ten times the scans to look the same.
#define BLOOD_DRAIN_STEPS   6.0f

/*
===============
CL_BloodSpawnMark

Puts a new stuck splat on a surface: the puddle a draining run is feeding when
there is nothing there yet, or one of the marks a rivulet leaves behind it.

Deliberately NOT CL_BloodStick: that plays the wet impact sound and reads the
velocity to decide how glancing the hit was, and this is neither an impact nor a
new droplet - it is blood arriving from the mark above, and it should arrive
silently and round. The pooling sound belongs to the MERGES that follow, where
CL_BloodPoolInto already plays it under its own spacing rule.

WORLD SURFACES ONLY - see CL_BloodArriveAtFloor for why.
===============
*/
static void CL_BloodSpawnMark(const cparticle_t *from, const vec3_t org,
                              const vec3_t normal, float radius,
                              const vec3_t tangent, float stretch)
{
    cparticle_t *seed = CL_AllocParticle();

    if (!seed)
        return;

    seed->particleType = from->particleType;
    VectorCopy(org, seed->org);
    VectorCopy(seed->org, seed->prev_org);
    VectorClear(seed->vel);
    VectorClear(seed->accel);
    seed->color = from->color;
    seed->rgba = from->rgba;
    seed->alpha = 1.0f;
    seed->alphavel = 0.0f;
    seed->time = cl.time;

    CL_MakeBloodSphere(seed, 1.0f);

    if (!seed->is_blood_sphere)
        return;             // over budget; CL_MakeBloodSphere has retired it

    // Sized by what was handed over rather than by CL_MakeBloodSphere's own
    // random droplet radius. This IS that blood, not another droplet.
    seed->radius = max(0.05f, radius);

    seed->blood_state = BLOOD_STUCK;
    VectorCopy(normal, seed->blood_normal);
    seed->blood_flatten = max(0.05f, min(cl_blood_flatten->value, 1.0f));

    // A track mark is drawn along the run that left it; a seeded pool is round.
    // stretch_base carries the same value as stretch so the settle has nothing to
    // hand back - this shape is not a run's streak, it IS the mark.
    if (tangent && stretch > 1.f) {
        VectorCopy(tangent, seed->blood_tangent);
        VectorCopy(tangent, seed->blood_slide_axis);
        seed->blood_stretch = stretch;
        seed->blood_stretch_base = stretch;
        seed->blood_cross = 1.0f;
        seed->blood_cross_base = 1.0f;
    } else {
        VectorClear(seed->blood_tangent);
        VectorClear(seed->blood_slide_axis);
        seed->blood_stretch = 1.0f;
        seed->blood_stretch_base = 1.0f;
        seed->blood_cross = 1.0f;
        seed->blood_cross_base = 1.0f;
    }

    CL_BloodClearSlide(seed);

    seed->blood_ent = -1;
    seed->blood_ent_id = 0;
    seed->blood_rim = BLOOD_RIM_FULL;
    seed->blood_rim_dirty = true;
    seed->blood_block = 0;
    seed->blood_wall_spread = false;
    seed->blood_arrived = false;
    seed->blood_edge_tries = max(0, cl_blood_edge_tries->integer);

    seed->alphavel = cl_blood_permanent->integer
        ? 0.0f
        : -1.0f / max(0.1f, cl_blood_splat_life->value);
}


/*
===============
CL_BloodMayRun

Whether this splat is heavy enough to overcome its own grip on the surface.

SURFACE TENSION HOLDS ALONG THE CONTACT EDGE AND GRAVITY PULLS ON THE VOLUME, so
there is a size below which blood on a vertical wall simply does not move. The
simulation had no model of that at all: on a vertical face the whole of gravity
acts along the surface, so every splat ran, always, and a wall was never anything
but a staging area on the way to the floor.

Tested only on blood AT REST. Once a run is moving it keeps moving until the drag
stops it, which is static-versus-kinetic friction and is also what keeps a splat
sitting exactly on the threshold from chattering in and out of motion every frame
- that would miss the geometry cache every frame for as long as it sat there.
===============
*/
static bool CL_BloodMayRun(const cparticle_t *p)
{
    if (cl_blood_cling_size->value <= 0.f)
        return true;

    // How much of gravity acts ALONG this surface: 1 on a vertical wall, 0 on a
    // floor or a ceiling. The same quantity CL_BloodStretchLimit is built on.
    float slope = 0.f;

    if (DotProduct(p->blood_normal, p->blood_normal) > 0.5f) {
        const float nz = min(1.f, fabsf(p->blood_normal[2]));
        slope = sqrtf(max(0.f, 1.f - nz * nz));
    }

    // IN DROPLETS, not in units. The thing this is really asking is "has enough
    // blood gathered here to overcome its grip", and the natural unit for that is
    // the size of the drops arriving - so the answer does not silently change
    // meaning the moment cl_blood_sphere_radius is touched.
    const float hold = cl_blood_cling_size->value *
                       max(0.01f, cl_blood_sphere_radius->value);

    return p->radius * slope > hold;
}

/*
===============
CL_BloodShedMark

Leaves part of a running mark on the surface it is passing over.

A rivulet WETS a wall; it does not carry all of its blood down and leave the wall
as clean as if nothing had happened. That is the half of "disappearing off the
wall" the drain at the foot does not address: the pool at the bottom was right
and the track above it did not exist.

Paid for out of the runner, so blood is moved rather than invented, and floored at
the cling size - a mark that is already down to a rivulet has nothing spare and
stops shedding instead of erasing itself.

The marks are drawn along the run and sized so consecutive ones about meet, which
makes a track rather than a row of dots. They are below the cling size by
construction, so they pin where they are laid and never run themselves.
===============
*/
static void CL_BloodShedMark(cparticle_t *p)
{
    if (cl_blood_trail->value <= 0.f)
        return;

    // Riding a brush model: the mark would need carrying in the model's own
    // frame, which is more than a track is worth. Same restriction as the drain.
    if (p->blood_ent >= 0)
        return;

    const float keep = max(0.2f, cl_blood_cling_size->value *
                                 max(0.01f, cl_blood_sphere_radius->value));
    const float area = p->radius * p->radius;
    const float floor_area = keep * keep;

    if (area <= floor_area * 1.2f)
        return;                 // already only a rivulet; nothing to spare

    const float give_area = min((area - floor_area) * 0.35f, area * 0.25f);
    const float give = sqrtf(give_area);

    if (give < 0.15f)
        return;

    if (!CL_BloodPoolInto(p, p->org, p->blood_normal, p->blood_ent, give)) {
        // Long enough that consecutive marks about meet, so the track reads as a
        // line rather than as beads - but bounded, because a thin mark longer
        // than the gap between them is the needle this system keeps relearning.
        const float step = max(1.f, cl_blood_trail->value);
        const float spread = max(0.01f, give * max(0.1f, cl_blood_splat_size->value));
        const float stretch = max(1.f, min(step / (2.f * spread), 5.f));

        CL_BloodSpawnMark(p, p->org, p->blood_normal, give,
                          p->blood_tangent, stretch);
    }

    p->radius = sqrtf(max(floor_area, area - give_area));
    p->blood_rim_dirty = true;
}

/*
===============
CL_BloodArriveAtFloor

A run that has reached the foot of the surface it was running down stops and
hands its blood over, instead of collapsing into a droplet and falling.

WHY THIS EXISTS.  The probe that keeps a running splat attached fires
perpendicular to the surface, and at the inside of a wall/floor corner it goes
solid - so a run arriving at the bottom of a wall took the "ran off the end of
the surface" branch.  That branch resets the stretch, the cross extent and the
tangent: it COLLAPSES THE WHOLE STREAK TO A SPHERE in a single frame and re-lands
it as one round floor splat.  Matt, playing: "as soon as a part of the splat hits
the floor the whole thing disappears to a splat on the floor ... it should be a
tiny floor puddle and expand to the normal size it would be if it hit the floor
as the rest of the splat makes it to the floor."

Running off a LEDGE takes the same branch and genuinely should fall, so the two
are told apart by looking: if there is ground within BLOOD_ARRIVE_REACH below the
foot, the run has arrived; if there is not, it really has run out of surface and
the old path is correct.

WORLD SURFACES ONLY.  Pooling onto a brush model would need the contact carried
in the model's own frame and re-derived every frame as the door moves, which is
what blood_local_org does for a splat but is more than a feed site is worth.  A
run reaching the foot of a moving brush model declines and falls, exactly as it
did before any of this.
===============
*/
static bool CL_BloodArriveAtFloor(cparticle_t *p)
{
    static const vec3_t down = { 0.f, 0.f, -1.f };
    vec3_t start, end;
    trace_t tr;

    if (cl_blood_drain->value <= 0.f)
        return false;

    if (p->blood_slide_dist <= 0.f)
        return false;           // it never ran; there is no streak to hand over

    // Started a little way OUT from the surface it has been running down, so the
    // pool forms at the foot of the wall rather than buried in it.
    const float out = max(0.5f, p->radius * max(0.1f, cl_blood_splat_size->value));

    VectorMA(p->org, out, p->blood_normal, start);
    VectorMA(start, BLOOD_ARRIVE_REACH, down, end);

    blood_traces++;
    tr = CL_TracePoint(start, end, MASK_SOLID, false);

    if (tr.startsolid || tr.allsolid || tr.fraction >= 1.0f)
        return false;           // nothing under it - it has run off a ledge

    if (tr.plane.normal[2] < 0.7f)
        return false;           // not ground a pool would stay on

    if (CL_TraceHitEntity(&tr))
        return false;           // a brush model; see the note above

    VectorMA(tr.endpos, BLOOD_SURFACE_OFFSET, tr.plane.normal, p->blood_feed_org);
    VectorCopy(tr.plane.normal, p->blood_feed_normal);

    p->blood_arrived = true;
    p->blood_drain = max(0.05f, cl_blood_drain->value);
    p->blood_drain_total = p->radius * p->radius;
    p->blood_drain_done = 0.f;
    VectorClear(p->vel);

    return true;
}

/*
===============
CL_BloodDrainToPool

Delivers a stopped run's blood to the pool at its foot, a share at a time.

The mark keeps its place on the wall and SHRINKS as it empties; the pool starts
as a puddle the size of one delivery and grows with each one.  Between them the
blood is conserved - areas subtract here exactly the way CL_BloodPoolInto adds
them - so the pool ends up the size the whole mark would have made, reached over
cl_blood_drain seconds instead of in one frame.

The streak also relaxes while it drains, and that half is free: the renderer
anchors a run's trail at its LEADING edge, so shortening it contracts the mark
toward the foot of the run - into the pool it is feeding.

Returns true when the mark is empty and should be retired; its blood is on the
floor by then.
===============
*/
static bool CL_BloodDrainToPool(cparticle_t *p, float dt)
{
    const float total = max(1e-4f, p->blood_drain_total);
    const float span = max(0.05f, cl_blood_drain->value);

    // WHAT STAYS ON THE WALL. Draining a mark to nothing puts the pool at the
    // foot exactly right and leaves the wall above it bare - the opposite end of
    // the bug, and identical to look at. A run ends against the floor; it does
    // not evaporate off the surface it ran down. So it hands over everything
    // ABOVE a mark's worth and keeps the rest, pinned where it stopped.
    const float keep = cl_blood_cling_size->value *
                       max(0.01f, cl_blood_sphere_radius->value);
    const float keep_area = min(keep * keep, total * 0.5f);
    const float deliver = max(0.f, total - max(0.f, keep_area));

    // How much SHOULD have arrived by now. Linear in AREA rather than in radius,
    // so the pool widens quickly at first and creeps at the end, which is how a
    // spreading puddle actually reads.
    const float want = deliver * (1.f - p->blood_drain / span);
    const float owed = max(0.f, want - p->blood_drain_done);
    const float quantum = max(1e-4f, deliver / BLOOD_DRAIN_STEPS);

    if (owed > 0.f && (owed >= quantum || p->blood_drain <= 0.f)) {
        const float give = sqrtf(owed);

        if (!CL_BloodPoolInto(p, p->blood_feed_org, p->blood_feed_normal, -1, give))
            CL_BloodSpawnMark(p, p->blood_feed_org, p->blood_feed_normal,
                              give, NULL, 1.f);

        p->blood_drain_done += owed;

        // Take it out of the mark. A splat's blood IS its area, which is the
        // quantity the merge adds in quadrature, so this is that sum run
        // backwards and the two stay in step by construction.
        p->radius = sqrtf(max(max(0.f, keep_area), total - p->blood_drain_done));
        p->blood_rim_dirty = true;
    }

    CL_BloodRelaxShape(p, dt);

    if (p->blood_drain > 0.f)
        return false;

    blood_drains++;

    // Done. What is left is the stain the run left on the wall - below the cling
    // size by construction, so CL_BloodMayRun pins it there and it never moves
    // again. Only a mark too small to see is worth retiring.
    p->blood_arrived = false;
    p->blood_drain = 0.f;

    if (p->radius >= 0.2f) {
        p->blood_rim_dirty = true;
        return false;
    }

    return true;
}

/*
===============
CL_BloodSettleAtRest

A pool that has stopped running gathers back up, and then asks once whether it
has come to rest against blood that is already lying there.

THE BUG THIS FIXES.  Matt, screenshot: droplets ran down a rocky ramp onto the
floor and stayed on it as a fan of eight identical needles - "they should have
all pooled together at the bottom of the hill and stopped looking so stretched
out."  Two separate omissions, both of them "nothing happens when the motion
ends":

  - CL_BloodReshapeSlide draws the mark out with distance run and RAISES ONLY.
    That is right while the blood is moving: a run leaves a streak.  It is wrong
    the instant it stops, and nothing ever handed the length back, so every
    splat froze at whatever it had reached - the cap, for anything that ran more
    than a couple of body lengths.
  - The merge is gated on DISTANCE TRAVELLED (BLOOD_SLIDE_POOL_STEP), so a splat
    that comes to rest a unit away from another one never asks again.  Arriving
    beside blood and arriving on top of it are the same event to a viewer, and
    only the second one pooled.

The length is given back over cl_blood_slide_relax seconds rather than dropped,
because a pool snapping from a streak to a disc in one frame reads as a bug of
its own, and because the geometry rebuild wants spreading over frames anyway.

WHY THE CONTINUOUS STATE IS A SEPARATE FIELD.  blood_stretch is quantized and
feeds a memcmp-keyed cache, so decaying it in place cannot work: subtracting a
frame's worth and re-quantizing lands back on the value it started from, so it
would never move at all.  That is the same trap blood_slide_axis has a comment
about, and this is the same answer - decay blood_stretch_run, which nothing
caches, and commit to blood_stretch in steps.

Reached ONLY from the parked branch, and only while blood_slide_dist is nonzero,
which is true just between the end of a run and the end of the settle.  A floor
of splats that never moved costs the one compare it always did.
===============
*/

// Eighths of a unit of stretch, ceilinged so the value only ever falls - the
// mirror of BLOOD_SLIDE_STRETCH_STEP, and a #define for the same reason.
#define BLOOD_SETTLE_STEP   8.0f

/*
===============
CL_BloodRelaxShape

The streak a run drew is given back over cl_blood_slide_relax seconds.

SHARED BY THE TWO WAYS A RUN CAN END, which is why it is its own function: it
either comes to a halt on the surface it is on (CL_BloodSettleAtRest) or it
reaches the foot of that surface and empties into a pool (CL_BloodDrainToPool).
The mark does the same thing in both cases and it should not be written twice.

Returns true once the shape has finished arriving.
===============
*/
static bool CL_BloodRelaxShape(cparticle_t *p, float dt)
{
    const float base = max(1.f, p->blood_stretch_base);
    const float relax = max(0.f, cl_blood_slide_relax->value);

    const float cbase = max(0.125f, p->blood_cross_base);

    if (!p->blood_settling) {
        p->blood_settling = true;
        p->blood_settle = relax;
        p->blood_stretch_run = p->blood_stretch;
        p->blood_cross_rest = p->blood_cross;
    }

    p->blood_settle = max(0.f, p->blood_settle - dt);

    if (p->blood_settle > 0.f) {
        // Linear in time, not exponential: this has to actually ARRIVE, and an
        // asymptote leaves a permanent sliver of the streak behind.
        const float f = p->blood_settle / max(0.001f, relax);

        const float want = base + (p->blood_stretch_run - base) * f;
        const float q = ceilf(want * BLOOD_SETTLE_STEP) / BLOOD_SETTLE_STEP;

        // Lowered only, for the same reason the growth is raised only.
        if (q < p->blood_stretch)
            p->blood_stretch = q;

        // The width comes back WIDER as the length comes back shorter, because
        // the blood in the rivulet has to end up somewhere: a run that stops
        // gathers into a pool, it does not evaporate. Raised only here, the
        // mirror of the length - and floored, the mirror of the length's ceil.
        const float cwant = cbase + (p->blood_cross_rest - cbase) * f;
        const float cq = floorf(cwant * BLOOD_SETTLE_STEP) / BLOOD_SETTLE_STEP;

        if (cq > p->blood_cross)
            p->blood_cross = cq;

        return false;
    }

    // Settled. Back to the shape it landed with - the impact smear is a
    // permanent feature of the mark, the run's share of the length is not.
    p->blood_stretch = base;
    p->blood_cross = cbase;
    CL_BloodClearSlide(p);
    return true;
}

static bool CL_BloodSettleAtRest(cparticle_t *p, float dt)
{
    if (!CL_BloodRelaxShape(p, dt))
        return false;

    // Its OUTLINE just changed, and the rim reach is measured in that outline's
    // own parameter space (CL_BloodSplatAxes scales by the stretch), so whatever
    // was probed while it was still a streak describes a shape it no longer has.
    // Re-measure. Costs nothing extra in the common case: the slide had already
    // invalidated the rim, so this replaces a queued probe rather than adding
    // one, and it is now queued for the shape that will actually be drawn.
    p->blood_rim = BLOOD_RIM_FULL;
    p->blood_rim_dirty = true;

    blood_settles++;

    // AND NOW ASK, ONCE. This is the merge the distance gate cannot reach: a
    // splat that ran to a halt beside blood already on the floor. Its shape has
    // finished changing, so this is also the only moment at which the answer is
    // about the pool it has actually become.
    if (!CL_BloodPoolInto(p, p->org, p->blood_normal, p->blood_ent, 0.f))
        return false;

    blood_rest_pools++;
    return true;
}

// Returns true when the droplet merged into an existing pool and should be
// retired - the blood it carried is now part of that splat.
static bool CL_SimulateBloodSphere(cparticle_t *p, float dt)
{
    trace_t tr;
    vec3_t  end;

    if (p->blood_state == BLOOD_STUCK) {
        // Follow the surface first, so everything below works from where the
        // droplet actually is this frame.
        if (p->blood_ent >= 0) {
            if (!CL_BloodRideEntity(p)) {
                // The door it was on has gone. Fall.
                p->blood_state = BLOOD_AIRBORNE;
                p->blood_flatten = 1.0f;
                p->blood_stretch = 1.0f;
                p->blood_stretch_base = 1.0f;
                p->blood_cross = 1.0f;
                p->blood_cross_base = 1.0f;
                CL_BloodClearSlide(p);
                VectorClear(p->blood_slide_axis);
                VectorClear(p->blood_tangent);
                VectorClear(p->vel);
                p->time = cl.time;
                p->alpha = 1.0f;
                p->alphavel = -1.0f / max(0.1f, cl_blood_air_life->value);
                return false;
            }

            if (!CL_BloodMaySlide(p)) {
                VectorClear(p->vel);
                return false;
            }
        }

        // ARRIVED AT THE FOOT AND HANDING ITS BLOOD OVER. Tested before gravity
        // rather than in the parked branch below, because gravity on a wall would
        // simply start the run again - the mark has stopped because it has run
        // out of surface, not because it ran out of speed.
        if (p->blood_arrived) {
            VectorClear(p->vel);
            return CL_BloodDrainToPool(p, dt);
        }

        // PINNED? Blood below the cling size does not run however steep the
        // surface is - see CL_BloodMayRun. Asked only of blood AT REST, so a run
        // already in motion is never stopped dead by it, and asked BEFORE gravity
        // so a pinned splat takes the same early-out a settled floor splat does
        // and costs nothing.
        if (DotProduct(p->vel, p->vel) < BLOOD_SLIDE_EPSILON * BLOOD_SLIDE_EPSILON &&
            !CL_BloodMayRun(p)) {
            VectorClear(p->vel);

            if (p->blood_slide_dist > 0.f)
                return CL_BloodSettleAtRest(p, dt);

            return false;
        }

        // Gravity, projected into the plane of the surface.  On a floor this
        // cancels to nothing; on a wall it is a downward run; on a slope it is
        // the component that makes a droplet track downhill.
        vec3_t accel = { 0.f, 0.f, -cl_blood_gravity->value };
        float into = DotProduct(accel, p->blood_normal);
        VectorMA(accel, -into, p->blood_normal, accel);

        VectorMA(p->vel, dt, accel, p->vel);
        VectorScale(p->vel, max(0.f, 1.f - cl_blood_slide->value * dt), p->vel);

        float speed = VectorLength(p->vel);

        // TERMINAL SPEED. Drag alone settles a run at cl_blood_gravity divided by
        // cl_blood_slide, which at the defaults is 320 u/s down a vertical wall -
        // a room-height wall crossed in a fifth of a second, and blood does not do
        // that. A film running under gravity is limited by its own viscosity, and
        // capping the speed is the cheap honest way to say so: the drag still
        // brings the run to rest, this only stops it getting away.
        //
        // Clamped rather than folded into the drag so the SHAPE is untouched. The
        // streak is drawn out by distance run (cl_blood_slide_len), never by time,
        // so a slower run reaches the same mark - it just takes longer about it.
        if (cl_blood_slide_speed->value > 0.f && speed > cl_blood_slide_speed->value) {
            VectorScale(p->vel, cl_blood_slide_speed->value / speed, p->vel);
            speed = cl_blood_slide_speed->value;
        }

        if (speed < BLOOD_SLIDE_EPSILON) {
            VectorClear(p->vel);

            // Parked - nothing to trace. But a pool that has just STOPPED
            // running still has the streak its run drew and a neighbour it may
            // have come to rest against; see CL_BloodSettleAtRest.
            //
            // blood_slide_dist is nonzero only between the end of the run and
            // the end of the settle, and CL_BloodClearSlide zeroes it there, so
            // a splat that never moved - which is nearly all of them - takes
            // exactly the early return it always took.
            if (p->blood_slide_dist > 0.f)
                return CL_BloodSettleAtRest(p, dt);

            return false;
        }

        // A MARK THAT IS STILL BEING DRAWN IS NOT AN OLD MARK.
        //
        // p->time is stamped by CL_BloodStick and is what CL_RetireBlood sorts
        // the floor on, oldest first. A splat that keeps running on the same
        // plane never re-sticks, so its clock ran from the moment of FIRST
        // contact for the whole journey - and once cl_blood_max is saturated,
        // which is what a fight against a wall does, the longest-running mark on
        // the wall is the first thing the budget takes.
        //
        // That was survivable while a run down a wall took a fifth of a second.
        // With a run capped at cl_blood_slide_speed it takes ten or more, so a
        // streak now spends its whole life as the oldest thing on the floor and
        // is deleted in the middle of being drawn - Matt, playing: "sometimes
        // when the blood is sliding down the wall it just disappears".
        //
        // Exactly the argument CL_BloodStick already makes for restarting the
        // clock on impact ("a splat should outlast the spray that made it"),
        // carried to its conclusion: a splat should also outlast its own run.
        // It also stops a non-permanent splat fading out mid-slide.
        p->time = cl.time;

        // It is moving, so its mark points somewhere new - and gets longer.
        const float slide_dist_before = p->blood_slide_dist;
        CL_BloodReshapeSlide(p, dt, speed);

        VectorMA(p->org, dt, p->vel, end);

        // Re-attach: trace from just off the surface into it.  A hit keeps the
        // droplet on the wall and picks up the new normal, which is what carries
        // it around a corner.  A miss means it has run off an edge.
        vec3_t probe_start, probe_end;
        VectorMA(end, 1.0f, p->blood_normal, probe_start);
        VectorMA(end, -2.0f, p->blood_normal, probe_end);

        blood_traces++;
        tr = CL_TracePoint(probe_start, probe_end, MASK_SOLID, false);

        if (tr.fraction < 1.0f && !tr.allsolid) {
            centity_t *hit = CL_TraceHitEntity(&tr);
            const bool same_plane =
                DotProduct(tr.plane.normal, p->blood_normal) > 0.98f;

            VectorCopy(tr.plane.normal, p->blood_normal);
            VectorMA(tr.endpos, BLOOD_SURFACE_OFFSET, p->blood_normal, p->org);

            // KEEP THE REACH IT HAS while it is only travelling along the same
            // plane. Blanking it every frame is what let a running streak draw
            // straight off the end of the rail it was running down - it is the
            // marks that MOVE that reach furthest past a surface, and those were
            // exactly the ones never measured. A measurement a few units back
            // still describes very nearly the same surface, and the probe site
            // re-measures on a distance step.
            //
            // A CHANGE OF PLANE is a different matter and does blank it: the
            // reach is stored in the splat's own in-plane frame, so carried onto
            // a new normal it clips the wrong parts of the outline.
            if (!same_plane) {
                p->blood_rim = BLOOD_RIM_FULL;
                p->blood_block = 0;
            }
            p->blood_rim_dirty = true;

            // It may have slid from the world onto a door, or the other way.
            // Only a brush model can be reached from here - CL_TracePoint has
            // the boxes turned off - so its collision pose is the right frame to
            // measure the local coordinates in.
            if (hit)
                CL_BloodAttachToEntity(p, hit, hit->current.origin, hit->current.angles,
                                       p->org, p->blood_normal);
            else
                CL_BloodDetachFromEntity(p);

            // ARRIVED AT ANOTHER POOL? Merge into it. Blood running down a wall
            // that reaches blood already lying there does not slide over it, and
            // this is the same CL_BloodPoolInto that a landing droplet uses - the
            // areas add, the target grows, and this droplet is retired.
            //
            // Gated on DISTANCE TRAVELLED rather than run every frame: the scan
            // is over every stuck splat and cl_blood_max can be 2048, so a frame
            // with fifty sliding droplets would be a hundred thousand distance
            // tests for an answer that cannot change in a fraction of a unit.
            // Crossing a step boundary is free to detect - the distance is being
            // accumulated for the stretch anyway.
            if (cl_blood_pool->integer &&
                floorf(slide_dist_before / BLOOD_SLIDE_POOL_STEP) !=
                floorf(p->blood_slide_dist / BLOOD_SLIDE_POOL_STEP) &&
                CL_BloodPoolInto(p, tr.endpos, p->blood_normal, p->blood_ent, 0.f))
                return true;

            // WET THE SURFACE IT IS PASSING OVER. Metered on distance for the
            // same reason the merge above is: it spawns geometry, and per frame
            // it would spawn a mark every inch of a slow run.
            {
                const float step = max(1.f, cl_blood_trail->value);

                if (cl_blood_trail->value > 0.f &&
                    floorf(slide_dist_before / step) !=
                    floorf(p->blood_slide_dist / step))
                    CL_BloodShedMark(p);
            }
        } else {
            // Ran off the end of the surface.
            //
            // TWO DIFFERENT EVENTS REACH HERE and only one of them is a fall. A
            // run that has reached the foot of a wall gets here because the
            // re-attach probe goes solid at the inside of the corner, and
            // everything below - sphere, stretch 1, cleared tangent - destroys
            // the streak in one frame and re-lands it as a single round floor
            // splat. That was the snap Matt reported. A run that has gone over a
            // LEDGE reaches the same line and genuinely should fall.
            //
            // CL_BloodArriveAtFloor looks below and answers which one it is.
            if (CL_BloodArriveAtFloor(p))
                return false;

            blood_detached++;
            CL_BloodDetachFromEntity(p);
            p->blood_state = BLOOD_AIRBORNE;
            p->blood_flatten = 1.0f;
            p->blood_stretch = 1.0f;
            p->blood_stretch_base = 1.0f;
            p->blood_cross = 1.0f;
            p->blood_cross_base = 1.0f;
            CL_BloodClearSlide(p);
            VectorClear(p->blood_slide_axis);
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

    // Bounding boxes are deliberately NOT in this trace - see CL_TracePoint. The
    // world and the brush models have real geometry and are handled here; bodies
    // have only a box, and get their real triangles from CL_BloodTraceModels
    // below instead.
    blood_traces++;
    tr = CL_TracePoint(p->org, end, MASK_SOLID, false);

    if (tr.allsolid || tr.startsolid) {
        // Spawned inside geometry - a wound right against a wall. Leave it where
        // it is rather than teleporting it to a surface it never touched.
        VectorCopy(end, p->org);
        return false;
    }

    centity_t *hit = CL_TraceHitEntity(&tr);
    vec3_t hit_point, hit_normal, pose_origin, pose_angles;
    float frac = tr.fraction;

    VectorCopy(tr.plane.normal, hit_normal);
    VectorClear(pose_origin);
    VectorClear(pose_angles);

    // A brush model hit: CL_TracePoint ran against the unlerped server state, so
    // that is the frame this contact point has to be measured in.
    if (hit) {
        VectorCopy(hit->current.origin, pose_origin);
        VectorCopy(hit->current.angles, pose_angles);
    }

    // A body's real triangles, if any are closer than what the world trace found.
    // Closer is the whole test: a monster standing against a wall still catches
    // its own blood, and a droplet that has already stopped at the wall behind it
    // is not dragged back onto the body.
    {
        centity_t *model_ent = NULL;

        if (CL_BloodTraceModels(p->org, end, &frac, hit_normal, &model_ent)) {
            // A BODY IS NOT A SURFACE TO PARK ON. Run down it and keep falling -
            // see CL_BloodRunOffModel. Nothing is attached and nothing is stuck,
            // so there is no state to strand when the animation moves on.
            LerpVector(p->org, end, frac, hit_point);
            CL_BloodRunOffModel(p, hit_point, hit_normal);
            return false;
        }
    }

    if (frac < 1.0f) {
        LerpVector(p->org, end, frac, hit_point);

        if (CL_BloodPoolInto(p, hit_point, hit_normal,
                             hit ? (int)(hit - cl_entities) : -1, 0.f))
            return true;

        CL_BloodStick(p, hit_point, hit_normal);

        // Only a brush model can be reached here - a door, lift or platform,
        // whose pose IS a rigid transform, so riding it is exact.
        if (hit) {
            CL_BloodAttachToEntity(p, hit, pose_origin, pose_angles, hit_point, hit_normal);
            // Snap straight to the render pose, so its first drawn frame is
            // already on the surface rather than a server frame behind it.
            CL_BloodRideEntity(p);
            VectorMA(p->org, BLOOD_SURFACE_OFFSET, p->blood_normal, p->org);
        } else {
            CL_BloodDetachFromEntity(p);
        }
    } else {
        VectorCopy(end, p->org);
    }

    return false;
}

/*
===============
CL_ApplyBloodColor

cl_blood_color: 0 leaves the colour the effect asked for - red for TE_BLOOD,
green for TE_GREENBLOOD - and anything else forces one.

APPLIED AT THE SINGLE POINT WHERE A PARTICLE BECOMES A BLOOD SPHERE, which is
why it is one function and not a change at every spawn site. The spray, the
diminishing gib trail and the non-diminishing one all pass through there and all
pick different palette ranges of their own; recolouring at the source would mean
finding each of them, and would risk catching an ordinary particle that happens
to share a palette index with blood.

THE JITTER IS NOT DECORATION. Every blood spawn site picks its colour as a
palette BASE plus a few random low bits - `0xe8 + (Q_rand() & 7)` - so a burst
carries a spread of shades. Forcing a single RGB throws that away and sixty
droplets become sixty copies of one object, which is the same mistake the wobble
made when it used one seed for a whole splat. The spread here is in the same
direction for all three channels, so it reads as light and dark droplets rather
than as a rainbow.

Per-droplet colour reaches the renderer in the otherwise dead uv slots (see
write_blood_geometry in blood.c), so this costs nothing beyond the particle it
is written to - no material, no texture, no extra geometry.
===============
*/
static void CL_ApplyBloodColor(cparticle_t *p)
{
    // Straight RGB rather than palette indices. Quake II's palette has a run of
    // reds and a run of greens and nothing usable for the rest, so picking
    // indices for blue or purple would mean guessing at ranges that are not
    // there. color -1 means "use rgba", which cast_u32_to_f32_color already
    // honours on the renderer side.
    static const byte blood_tint[][3] = {
        { 232,  36,  28 },   // 1 red - matches the 0xe8 range TE_BLOOD uses
        {  46, 200,  54 },   // 2 green
        {  48,  96, 236 },   // 3 blue
        { 236, 206,  44 },   // 4 yellow
        { 158,  52, 220 },   // 5 purple
        {  30,  28,  26 },   // 6 black - oil, for anything mechanical
    };
    const int num_tints = (int)(sizeof(blood_tint) / sizeof(blood_tint[0]));

    const int mode = cl_blood_color->integer;

    if (mode <= 0)
        return;

    const byte *tint = blood_tint[min(mode, num_tints) - 1];

    const int jitter = (int)(Q_rand() & 31) - 12;

    for (int i = 0; i < 3; i++)
        p->rgba.u8[i] = (byte)max(0, min(255, (int)tint[i] + jitter));

    p->rgba.u8[3] = 255;
    p->color = -1;
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
    // Full? Make room by retiring the least valuable blood rather than refusing,
    // so permanent blood keeps turning over instead of simply stopping. Which
    // one that is depends on whether the floor or the spray is over its share -
    // CL_RetireBlood decides.
    if (num_blood_live >= cl_blood_max->integer)
        CL_RetireBlood();

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
        CL_RetireBlood();
        p->blood_slot = CL_AllocBloodSlot();
    }

    if (p->blood_slot < 0) {
        // No geometry slot free. Same treatment as being over budget: retire it
        // rather than let it fall back to the legacy quad.
        p->alpha = 0.0f;
        p->alphavel = 0.0f;
        return;
    }

    num_blood_live++;

    p->is_blood_sphere = true;
    CL_ApplyBloodColor(p);
    p->blood_state = BLOOD_AIRBORNE;
    p->blood_flatten = 1.0f;
    p->blood_stretch = 1.0f;
    p->blood_ent = -1;
    p->blood_ent_id = 0;
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
    num_blood_airborne = 0;

    // Slots still in use are marked as the list is walked; whatever is left
    // unmarked at the end belonged to a droplet that has died. Reclaiming them
    // here rather than at each free site keeps it to one place, and there are
    // several ways a particle can be freed.
    memset(blood_slot_seen, 0, sizeof(blood_slot_seen));

    // Free-list health, reported by cl_blood_stats. The particle pool is shared
    // with every other effect in the game, so a leak here starves the blaster
    // and the trails long before it is obvious that blood is the cause.
    int blood_air = 0, blood_stuck = 0, blood_riding = 0, num_free = 0, num_active = 0;

    // Rim measurements left in this frame's budget. Bounded per frame because a
    // burst lands sixty droplets at once; a pool can only be shoved off an edge
    // on a frame it was measured on, so this bounds both halves at once.
    int blood_edge_left = max(0, cl_blood_edge_budget->integer);

    // And a SEPARATE, much smaller slice for splats that are still running.
    //
    // Those used to be refused outright - a droplet measured where it is not
    // going to stay is re-queued the next frame and eats a slice of the budget
    // for the whole run, starving the ones that have actually settled. The refusal
    // is why a sliding mark draws unclipped, and it is half of why Matt's streaks
    // stand off the edge of the rail.
    //
    // Giving them their own slice keeps that starvation impossible by
    // construction while still letting a long run notice the wall it is about to
    // be drawn through. The other half of the gate is DISTANCE: a running splat
    // asks again only after BLOOD_SLIDE_RIM_STEP units, never per frame.
    int blood_edge_move_left = max(0, cl_blood_edge_budget->integer) / 4;

    // Time actually spent in the droplet simulation, and traces issued.
    //
    // This is the half timerefresh CANNOT see: it renders 128 frames without
    // running a client frame, so it measures the renderer and nothing else. The
    // per-droplet CL_TracePoint - a BSP trace plus a loop over every solid
    // entity, per droplet, per client frame - only happens in a real frame.
    static uint64_t blood_sim_usec_acc;
    static int      blood_trace_acc, blood_model_trace_acc, blood_model_hit_acc, blood_sim_frames;
    static int      blood_edge_trace_acc, blood_edge_probe_acc, blood_edge_slide_acc;
    static int      blood_wall_spread_acc;
    static int      blood_retired_acc, blood_detached_acc, blood_drain_acc;
    static int      blood_settle_acc, blood_rest_pool_acc, blood_pool_full_acc;
    static int      blood_pool_acc;
    uint64_t blood_sim_usec = 0;
    blood_traces = 0;
    blood_model_traces = 0;
    blood_model_hits = 0;
    blood_edge_traces = 0;
    blood_edge_probes = 0;
    blood_edge_slides = 0;
    blood_wall_spreads = 0;
    blood_retired = 0;
    blood_detached = 0;
    blood_drains = 0;
    blood_pinned = 0;
    blood_runnable = 0;
    blood_settles = 0;
    blood_rest_pools = 0;
    blood_pool_full = 0;
    blood_pools = 0;
    // ONLY when someone is looking. MAX_PARTICLES is 1000000, and on a quiet
    // map essentially the whole pool is on this list, so this is a million-node
    // pointer chase with no cache locality - MEASURED AT 19 ms PER FRAME, which
    // was the entire frame budget: host_speeds showed rf 20 / everything else 0,
    // CL_AddParticles was 19 ms of it, and R_RenderFrame only 1 ms.
    //
    // num_free feeds the cl_blood_stats line and nothing else, so gating it
    // costs that report nothing and gives the frame back.
    if (cl_blood_stats->integer)
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

                // On a surface steep enough to run down at all?
                if (DotProduct(p->blood_normal, p->blood_normal) > 0.5f) {
                    const float nz = min(1.f, fabsf(p->blood_normal[2]));

                    if (sqrtf(max(0.f, 1.f - nz * nz)) > 0.3f) {
                        blood_runnable++;
                        if (!CL_BloodMayRun(p))
                            blood_pinned++;
                    }
                }

                if (p->blood_ent >= 0)
                    blood_riding++;    // on a body or a brush model
                if (num_blood_splats < MAX_PARTICLES)
                    blood_splats[num_blood_splats++] = p;

                // Measure how far the surface reaches under this splat, at most
                // cl_blood_edge_budget of them per frame. The budget is the whole
                // reason this is safe: a wound lands sixty droplets between two
                // frames and measuring them all where they stick would put every
                // one of those traces in a single frame.
                //
                // A PARKED splat is measured as soon as the budget reaches it; a
                // RUNNING one only after it has covered BLOOD_SLIDE_RIM_STEP units
                // since its last measurement, and out of a separate small slice.
                // Per frame is not an option for a running splat - it would be
                // re-queued every frame and eat the whole budget for the length of
                // the run - but never measuring one at all is what leaves a streak
                // drawn straight off the end of the surface it is running down.
                const bool parked = DotProduct(p->vel, p->vel) < 1e-4f;
                const bool moved = !parked &&
                    p->blood_slide_dist - p->blood_rim_dist >= BLOOD_SLIDE_RIM_STEP;

                if (p->blood_rim_dirty &&
                    ((parked && blood_edge_left > 0) ||
                     (moved && blood_edge_move_left > 0))) {
                    CL_BloodProbeRim(p);

                    if (parked)
                        blood_edge_left--;
                    else
                        blood_edge_move_left--;

                    // RUN INTO A WALL? Spread along the foot of it. Done before
                    // the overhang test because it re-queues the probe, and the
                    // pool the shove has to judge is the one it has become.
                    if (parked && CL_BloodSpreadAlongWall(p)) {
                        p->blood_rim = BLOOD_RIM_FULL;
                        p->blood_rim_dirty = true;
                        blood_wall_spreads++;
                    }
                    // Enough of it hanging over thin air? The WHOLE POOL lets go.
                    // The shove is all that happens here; the slide, the fall off
                    // the lip, the run down the face below and the splat at the
                    // bottom are all paths a droplet already took before this
                    // existed. See CL_BloodSlideOffEdge.
                    else if (parked && p->blood_rim != BLOOD_RIM_FULL &&
                             p->blood_edge_tries > 0) {
                        vec3_t off;
                        if (CL_BloodOverhang(p, off) > cl_blood_edge_slide->value) {
                            CL_BloodSlideOffEdge(p, off);
                            blood_edge_slides++;
                        }
                    }
                }
            } else {
                blood_air++;
                if (num_blood_airborne < MAX_PARTICLES)
                    blood_airborne[num_blood_airborne++] = p;
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
                b->cross = p->blood_cross;
                VectorCopy(p->blood_tangent, b->tangent);
                b->rim_support = p->blood_rim;

                // Only the length the SLIDE added trails. Clamped at zero because
                // the detach branches reset the stretch to 1 without knowing what
                // the base was, and a negative trail would shift the mesh forward
                // of its own contact point.
                b->stretch_trail = max(0.f, p->blood_stretch - p->blood_stretch_base);

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
    blood_model_trace_acc += blood_model_traces;
    blood_model_hit_acc += blood_model_hits;
    blood_edge_trace_acc += blood_edge_traces;
    blood_edge_probe_acc += blood_edge_probes;
    blood_edge_slide_acc += blood_edge_slides;
    blood_wall_spread_acc += blood_wall_spreads;
    blood_retired_acc += blood_retired;
    blood_detached_acc += blood_detached;
    blood_drain_acc += blood_drains;
    blood_settle_acc += blood_settles;
    blood_rest_pool_acc += blood_rest_pools;
    blood_pool_full_acc += blood_pool_full;
    blood_pool_acc += blood_pools;
    blood_sim_frames++;

    if (cl_blood_stats->integer) {
        static int last_report;
        if (cl.time - last_report > 1000 || cl.time < last_report) {
            last_report = cl.time;
            // Edge numbers are per SECOND, not per frame: measurements are
            // amortised across frames on purpose, so a per-frame figure would
            // round to zero and say nothing about what the sweep is costing.
            Com_Printf("blood: %d airborne, %d stuck (%d riding, %d/%d pinned) | sim %.2f ms/frame, %d traces/frame, %d mesh/frame (%d hits/s) | edge %d probes/s, %d traces/s, %d slid off/s, %d walled/s | rest %d settled/s, %d pooled/s, %d full/s | %d merges/s, %d retired/s, %d detached/s, %d drained/s | particles %d active, %d free\n",
                blood_air, blood_stuck, blood_riding, blood_pinned, blood_runnable,
                blood_sim_frames ? (float)blood_sim_usec_acc / blood_sim_frames / 1000.f : 0.f,
                blood_sim_frames ? blood_trace_acc / blood_sim_frames : 0,
                blood_sim_frames ? blood_model_trace_acc / blood_sim_frames : 0,
                blood_model_hit_acc,
                blood_edge_probe_acc, blood_edge_trace_acc, blood_edge_slide_acc,
                blood_wall_spread_acc,
                blood_settle_acc, blood_rest_pool_acc, blood_pool_full_acc,
                blood_pool_acc, blood_retired_acc, blood_detached_acc, blood_drain_acc,
                num_active, num_free);
            blood_sim_usec_acc = 0; blood_trace_acc = 0; blood_model_trace_acc = 0; blood_model_hit_acc = 0; blood_sim_frames = 0;
            blood_edge_trace_acc = 0; blood_edge_probe_acc = 0; blood_edge_slide_acc = 0;
            blood_wall_spread_acc = 0;
            blood_retired_acc = 0; blood_detached_acc = 0; blood_drain_acc = 0;
            blood_settle_acc = 0; blood_rest_pool_acc = 0; blood_pool_full_acc = 0;
            blood_pool_acc = 0;
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

