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
// cl_tent.c -- client side temporary entities

#include "client.h"
#include "refresh/models.h"

qhandle_t   cl_sfx_ric1;
qhandle_t   cl_sfx_ric2;
qhandle_t   cl_sfx_ric3;
qhandle_t   cl_sfx_lashit;
qhandle_t   cl_sfx_flare;
qhandle_t   cl_sfx_spark5;
qhandle_t   cl_sfx_spark6;
qhandle_t   cl_sfx_spark7;
qhandle_t   cl_sfx_railg;
qhandle_t   cl_sfx_rockexp;
qhandle_t   cl_sfx_grenexp;
qhandle_t   cl_sfx_watrexp;
qhandle_t   cl_sfx_footsteps[4];

qhandle_t   cl_sfx_lightning;
qhandle_t   cl_sfx_disrexp;

qhandle_t   cl_mod_explode;
// The disruptor's shell around a tracked victim. rogue drew that as a
// cloud of black particles; this is the same sphere the monster-spawn
// effect uses, scaled to the victim and rendered through the spawngro
// material. See CL_AddPacketEntities' EF_TRACKERTRAIL branch.
qhandle_t   cl_mod_tracker_shell;
qhandle_t   cl_mod_smoke;
qhandle_t   cl_mod_flash;
qhandle_t   cl_mod_muzzleflash;
qhandle_t   cl_mod_parasite_segment;
qhandle_t   cl_mod_grapple_cable;
qhandle_t   cl_mod_explo4;
qhandle_t   cl_mod_bfg_explo;
qhandle_t   cl_mod_powerscreen;
qhandle_t   cl_mod_laser;
qhandle_t   cl_mod_newlaser;
qhandle_t   cl_mod_dmspot;
qhandle_t   cl_mod_explosions[4];

qhandle_t   cl_mod_lightning;
qhandle_t   cl_mod_heatbeam;
qhandle_t   cl_mod_explo4_big;

extern cvar_t* cvar_pt_particle_emissive;

// [Q2RTX] Draw YOUR OWN plasma beam from the world muzzle instead of deriving
// it from the view. DEFAULT 0 - this was tried and it REGRESSES FIRST PERSON in
// two separate ways, both of which Matt spotted immediately:
//
//  1. THE BEAM MISSES THE CROSSHAIR. The other-player branch computes `dist`
//     from the UNCORRECTED start and only then slides `org` down and forward.
//     The beam is drawn from the moved origin along the old direction, so it
//     runs parallel to the real shot and passes about 7 units under the impact.
//
//  2. THE SPARKLE PARTICLES COLLAPSE ONTO THE BEAM. CL_Heatbeam builds its
//     rings in the VIEW basis (cl.v_right / cl.v_up) at a radius of only 1.5
//     units. The view path forces the beam direction to be cl.v_forward, so
//     those rings are exactly perpendicular to the beam and read as circles
//     around it. Point the beam anywhere else and the rings tilt, flatten
//     against the beam and stop looking like a spiral.
//
// Both are properties of the VIEW path being view-aligned by construction, so
// no amount of tuning the world-muzzle path fixes them - see the beam section
// of [[q2rtx-compass-and-beam-view]] for what a real fix needs.
//
// 1 forces the world-muzzle chain to be used for the first-person view too.
// Kept for experimenting; the two-chain path below means you should not need
// it - your own beam now gets BOTH, each restricted to where it belongs.
static cvar_t   *cl_beam_thirdperson;

// [Q2RTX] How far to drop the WORLD beam below the server's muzzle, in units.
//
// The server fires from eye level - P_ProjectSource with offset
// {7, 2, viewheight - 3}, so about 19 units above the player origin - but the
// third-person model carries its gun down at chest height. Without this the
// beam leaves the model's head.
//
// This replaces the old hardcoded correction, which built its basis from
// `angles[YAW] + 180` and therefore moved the start FORWARD 7 instead of back -
// on top of the server start already being 7 forward, which is why the beam
// floated a foot or so in front of everyone's chest.
static cvar_t   *cl_beam_muzzle_drop;


/*
=================
CL_RegisterTEntSounds
=================
*/
void CL_RegisterTEntSounds(void)
{
    int     i;
    char    name[MAX_QPATH];

    // mk1-00332 and mk1-00333 are the impacts; mk1-00334 is the pooling sound.
    // Splitting them is the whole of Matt's sound-design note: the wet slap of
    // blood arriving and the softer sound of it joining a puddle are different
    // events, and one of the two was not audible at all.
    for (i = 0; i < NUM_BLOOD_SFX; i++) {
        Q_snprintf(name, sizeof(name), "blood/mk1-0033%d.wav", 2 + i);
        cl_sfx_blood_splat[i] = S_RegisterSound(name);
    }

    cl_sfx_blood_pool = S_RegisterSound("blood/mk1-00334.wav");

    cl_sfx_ric1 = S_RegisterSound("world/ric1.wav");
    cl_sfx_ric2 = S_RegisterSound("world/ric2.wav");
    cl_sfx_ric3 = S_RegisterSound("world/ric3.wav");
    cl_sfx_lashit = S_RegisterSound("weapons/lashit.wav");
    cl_sfx_flare = S_RegisterSound("weapons/flare.wav");
    cl_sfx_spark5 = S_RegisterSound("world/spark5.wav");
    cl_sfx_spark6 = S_RegisterSound("world/spark6.wav");
    cl_sfx_spark7 = S_RegisterSound("world/spark7.wav");
    cl_sfx_railg = S_RegisterSound("weapons/railgf1a.wav");
    cl_sfx_rockexp = S_RegisterSound("weapons/rocklx1a.wav");
    cl_sfx_grenexp = S_RegisterSound("weapons/grenlx1a.wav");
    cl_sfx_watrexp = S_RegisterSound("weapons/xpld_wat.wav");

    S_RegisterSound("player/land1.wav");
    S_RegisterSound("player/fall2.wav");
    S_RegisterSound("player/fall1.wav");

    for (i = 0; i < 4; i++) {
        Q_snprintf(name, sizeof(name), "player/step%i.wav", i + 1);
        cl_sfx_footsteps[i] = S_RegisterSound(name);
    }

    cl_sfx_lightning = S_RegisterSound("weapons/tesla.wav");
    cl_sfx_disrexp = S_RegisterSound("weapons/disrupthit.wav");
}

/*
=================
CL_RegisterTEntModels
=================
*/
void CL_RegisterTEntModels(void)
{
    cl_mod_explode = R_RegisterModel("models/objects/explode/tris.md2");
    cl_mod_tracker_shell = R_RegisterModel("models/items/spawngro/tris.md2");
    cl_mod_smoke = R_RegisterModel("models/objects/smoke/tris.md2");
    cl_mod_flash = R_RegisterModel("models/objects/flash/tris.md2");
    // The rerelease muzzle flash is a 12-point STARBURST fan, not the ball that
    // models/objects/flash is. The geometry is identical across every weapon's
    // flash/ model (19 verts, 48 tris) - only the skin differs - so one of them
    // serves as the generic flash for monsters and other players.
    // The GENERIC flash, for monsters, other players, and the two weapons whose
    // own flash/ folder is missing from the pak. Every weapon that has one gets
    // its own graphic instead - see cl_weapon_muzzles[] in entities.c.
    cl_mod_muzzleflash = R_RegisterModel("models/weapons/v_machn/flash/tris.md2");
    CL_RegisterViewMuzzleFlashes();
    cl_mod_parasite_segment = R_RegisterModel("models/monsters/parasite/segment/tris.md2");
    cl_mod_grapple_cable = R_RegisterModel("models/ctf/segment/tris.md2");
    cl_mod_explo4 = R_RegisterModel("models/objects/r_explode/tris.md2");
	cl_mod_explosions[0] = R_RegisterModel("sprites/rocket_0.sp2");
	cl_mod_explosions[1] = R_RegisterModel("sprites/rocket_1.sp2");
	cl_mod_explosions[2] = R_RegisterModel("sprites/rocket_5.sp2");
	cl_mod_explosions[3] = R_RegisterModel("sprites/rocket_6.sp2");
    cl_mod_bfg_explo = R_RegisterModel("sprites/s_bfg2.sp2");
    cl_mod_powerscreen = R_RegisterModel("models/items/armor/effect/tris.md2");
    cl_mod_laser = R_RegisterModel("models/objects/laser/tris.md2");
    cl_mod_dmspot = R_RegisterModel("models/objects/dmspot/tris.md2");	
    cl_mod_lightning = R_RegisterModel("models/proj/lightning/tris.md2");
    cl_mod_heatbeam = R_RegisterModel("models/proj/beam/tris.md2");
    cl_mod_explo4_big = R_RegisterModel("models/objects/r_explode2/tris.md2");

	for (int i = 0; i < sizeof(cl_mod_explosions) / sizeof(*cl_mod_explosions); i++)
	{
		model_t* model = MOD_ForHandle(cl_mod_explosions[i]);

        if (model) {
    		model->sprite_vertical = true;
        }
    }
}

/*
==============================================================

EXPLOSION MANAGEMENT

==============================================================
*/

explosion_t  cl_explosions[MAX_EXPLOSIONS];

static void CL_ClearExplosions(void)
{
    memset(cl_explosions, 0, sizeof(cl_explosions));
}

static explosion_t *CL_AllocExplosion(void)
{
    explosion_t *e, *oldest;
    int     i;
    int     time;

    for (i = 0, e = cl_explosions; i < MAX_EXPLOSIONS; i++, e++) {
        if (e->type == ex_free) {
            memset(e, 0, sizeof(*e));
            return e;
        }
    }
// find the oldest explosion
    time = cl.time;
    oldest = cl_explosions;

    for (i = 0, e = cl_explosions; i < MAX_EXPLOSIONS; i++, e++) {
        if (e->start < time) {
            time = e->start;
            oldest = e;
        }
    }
    memset(oldest, 0, sizeof(*oldest));
    return oldest;
}

static explosion_t *CL_PlainExplosion(bool big)
{
    explosion_t *ex;

    ex = CL_AllocExplosion();
    VectorCopy(te.pos1, ex->ent.origin);
    ex->type = ex_poly;
    ex->ent.flags = RF_FULLBRIGHT;
    ex->start = cl.servertime - CL_FRAMETIME;
    ex->light = 350;
    VectorSet(ex->lightcolor, 1.0f, 0.5f, 0.5f);
    ex->ent.angles[1] = Q_rand() % 360;

	int model_idx = Q_rand() % (sizeof(cl_mod_explosions) / sizeof(*cl_mod_explosions));
	model_t* sprite_model = MOD_ForHandle(cl_mod_explosions[model_idx]);

	if (cl_explosion_sprites->integer && !big && sprite_model)
	{
		ex->ent.model = cl_mod_explosions[model_idx];
		ex->frames = sprite_model->numframes;
		ex->frametime = cl_explosion_frametime->integer;
	}
	else
	{
		ex->ent.model = big ? cl_mod_explo4_big : cl_mod_explo4;
    ex->baseframe = 15 * (Q_rand() & 1);
    ex->frames = 15;
	}

    return ex;
}

/*
=================
CL_SmokeAndFlash
=================
*/
void CL_SmokeAndFlash(const vec3_t origin)
{
    explosion_t *ex;

    ex = CL_AllocExplosion();
    VectorCopy(origin, ex->ent.origin);
    ex->type = ex_misc;
    ex->frames = 4;
    ex->ent.flags = RF_TRANSLUCENT | RF_NOSHADOW;
    ex->start = cl.servertime - CL_FRAMETIME;
    ex->ent.model = cl_mod_smoke;

    ex = CL_AllocExplosion();
    VectorCopy(origin, ex->ent.origin);
    ex->type = ex_flash;
    ex->ent.flags = RF_FULLBRIGHT;
    ex->frames = 2;
    ex->start = cl.servertime - CL_FRAMETIME;
    ex->ent.model = cl_mod_flash;
}

/*
=================
CL_ImpactSmokeAndFlash

The same puff, but for a bullet striking a surface rather than leaving a muzzle.

The plain version drops the smoke exactly on the impact point with no
orientation and no motion, so it reads as a decal painted on the wall rather
than as something the wall threw off.  This one starts it just proud of the
surface and drifts it back along the impact normal, which is the directionality
the rerelease's impacts have.  `dir` is already unit length - MSG_ReadDir
decodes it out of the byte direction table.
=================
*/
void CL_ImpactSmokeAndFlash(const vec3_t origin, const vec3_t dir)
{
    explosion_t *ex;
    vec3_t      org;

    // Lift it clear of the wall so the sprite is not half buried in it.
    VectorMA(origin, 4.0f, dir, org);

    ex = CL_AllocExplosion();
    VectorCopy(org, ex->ent.origin);
    VectorCopy(org, ex->spawn_origin);
    // The puff only lives about 300 ms, so at the 18 units/sec this started
    // out at it travelled barely five units over its whole life - well under
    // its own size, which is why it looked identical to the undirected puff it
    // replaced. 60 units/sec moves it about one model width off the surface,
    // which is what actually reads as the wall throwing it off.
    VectorScale(dir, 60.0f, ex->vel);
    ex->type = ex_misc;
    ex->frames = 4;
    ex->ent.flags = RF_TRANSLUCENT | RF_NOSHADOW;
    ex->start = cl.servertime - CL_FRAMETIME;
    ex->ent.model = cl_mod_smoke;

    // the flash is the strike itself, so it stays where it happened
    ex = CL_AllocExplosion();
    VectorCopy(org, ex->ent.origin);
    ex->type = ex_flash;
    ex->ent.flags = RF_FULLBRIGHT;
    ex->frames = 2;
    ex->start = cl.servertime - CL_FRAMETIME;
    ex->ent.model = cl_mod_flash;
}

/*
=================
CL_SetupMuzzleFlashEntity

Everything about how a muzzle flash LOOKS, in one place, so the two ways one
reaches the screen cannot drift apart: the world flash spawned as an explosion
(monsters, other players, and your own gun's reflection copy), and the
first-person flash, which is rebuilt from the live gun transform every frame by
CL_AddViewWeaponFlash instead of being spawned and left behind.

`roll` is the spin about the barrel axis. It is passed in rather than rolled
here because the view flash has to keep the SAME roll for its whole life - the
model is a starburst fan facing +X, so a fresh random roll every frame would
make it strobe instead of sit still.
=================
*/
void CL_SetupMuzzleFlashEntity(entity_t *ent, const vec3_t origin,
                               const vec3_t angles, float roll,
                               int view_fx, qhandle_t model)
{
    vec3_t  forward, muzzle;
    bool    first_person = (view_fx == RF_FIRST_PERSON_FX);
    cvar_t  *scale_cvar  = first_person ? cl_muzzleflash_view_size
                                        : cl_muzzleflash_scale;
    cvar_t  *bright_cvar = first_person ? cl_muzzleflash_view_brightness
                                        : cl_muzzleflash_brightness;

    memset(ent, 0, sizeof(*ent));

    // Lift the flash off the muzzle POINT. The model's middle vertex sits at its
    // own origin, so centring it exactly on the muzzle buries that middle inside
    // the barrel and the gun occludes it - which is the dark diamond that shows
    // in the centre of the star. A couple of units along the barrel clears it.
    AngleVectors(angles, forward, NULL, NULL);
    VectorMA(origin, 3.0f, forward, muzzle);
    VectorCopy(muzzle, ent->origin);
    VectorCopy(muzzle, ent->oldorigin);

    VectorCopy(angles, ent->angles);
    ent->angles[ROLL] = roll;

    ent->model = model;
    ent->flags = RF_FULLBRIGHT | RF_NOSHADOW | RF_TRANSLUCENT | view_fx;
    ent->alpha = Cvar_ClampValue(bright_cvar, 0.001f, 1.0f);
    ent->scale = Cvar_ClampValue(scale_cvar, 0.1f, 20.0f);

    // the flash models are a single frame; there is nothing to interpolate
    ent->frame = 0;
    ent->oldframe = 0;
    ent->backlerp = 0.0f;
}

/*
=================
CL_MuzzleFlashModel

Rerelease muzzle flashes: a short-lived starburst model at the muzzle, instead of
(well, as well as) the bare dynamic light the classic game shows.

`origin` is the muzzle, `angles` the firer's orientation - the flash model is
authored facing +X about its own origin, so it just takes the shooter's angles.
ex_mflash was already declared in the explosion enum and allocated by nothing;
it holds for its frames and frees without the alpha fade the other types apply,
which is exactly the behaviour a muzzle flash wants.

`first_person` picks WHICH PAIR OF CVARS to use, and it has to exist. A flash
on a monster across the room and the flash on the gun in your hands are the
same model on the same code path, but they are not the same picture:

  - Size. This is a world-space model, so a shared scale means the view flash
    is drawn at 1/50th the distance of the monster one. Matt's tuned value of
    10 for monsters put a 22-unit disc on the end of his own barrel.
  - Brightness. The effects shader multiplies emission by
    `prev_adapted_luminance * 500`, so entity alpha near 1.0 is far past white.
    On a monster that clips to a hot spark a few pixels across and reads fine.
    Filling half the screen it clips to a FLAT ORANGE BLOB - measured at 68% of
    its pixels pinned at R=255 - and clipping is what destroys the texture's
    alpha taper. The taper is the whole look, so up close the flash has to sit
    UNDER the clipping point, which means an alpha around 0.06, not 1.0.

One knob cannot satisfy both, and trying to make it was the bug: see
cl_muzzleflash_view_size / cl_muzzleflash_view_brightness in main.c.

Gated on cl_muzzleflash_models so it can be turned off.
=================
*/
void CL_MuzzleFlashModel(const vec3_t origin, const vec3_t angles, int view_fx)
{
    CL_MuzzleFlashModel2(origin, angles, view_fx, 0);
}

void CL_MuzzleFlashModel2(const vec3_t origin, const vec3_t angles,
                          int view_fx, qhandle_t model)
{
    explosion_t *ex;
    if (!cl_muzzleflash_models->integer)
        return;

    // `model` is this weapon's own flash graphic; 0 means the caller has none
    // and wants the generic star (monsters, and the two weapons whose flash/
    // folder is missing from the pak).
    if (!model)
        model = cl_mod_muzzleflash;

    if (!model)
        return;

    ex = CL_AllocExplosion();

    CL_SetupMuzzleFlashEntity(&ex->ent, origin, angles, frand() * 360.0f,
                              view_fx, model);
    ex->type = ex_mflash;
    /* HOW LONG THE FLASH LASTS, and why it needs saying.

       The explosion clock runs on BASE_FRAMETIME, i.e. one `frame` is 100 ms,
       and the flash was `frames = 2` with `start` already backdated by one
       frame - so it lived almost exactly 100 ms. The machinegun fires every
       100 ms. The flash was therefore being replaced at the very moment it
       expired and read as PERMANENTLY ON rather than as a flicker, which is
       the opposite of what the rerelease looks like.

       So the lifetime is set explicitly in milliseconds via ex->frametime
       (the same field cl_explosion_frametime uses) and `start` is now the
       real current time rather than a backdated one. With frames = 2 the
       explosion is freed once `cl.time - start >= frametime`, so the lifetime
       IS cl_muzzleflash_time, and anything under the 100 ms fire interval
       gives a visible gap between shots. */
    ex->frames = 2;
    ex->frametime = Cvar_ClampValue(cl_muzzleflash_time, 10, 200);
    ex->start = cl.time;
    ex->baseframe = 0;
    ex->light = 0;      // the callers already add their own dlight
}

#define LENGTH(a) ((sizeof (a)) / (sizeof(*(a))))

typedef struct light_curve_s {
	vec3_t color;
	float radius;
	float offset;
} light_curve_t;

static light_curve_t ex_poly_light[] = {
    { { 0.4f,       0.2f,       0.02f     }, 12.5f, 20.00f },
    { { 0.351563f,  0.175781f,  0.017578f }, 15.0f, 23.27f },
    { { 0.30625f,   0.153125f,  0.015312f }, 20.0f, 24.95f },
    { { 0.264062f,  0.132031f,  0.013203f }, 22.5f, 25.01f },
    { { 0.225f,     0.1125f,    0.01125f  }, 25.0f, 27.53f },
    { { 0.189063f,  0.094531f,  0.009453f }, 27.5f, 28.55f },
    { { 0.15625f,   0.078125f,  0.007813f }, 30.0f, 30.80f },
    { { 0.126563f,  0.063281f,  0.006328f }, 27.5f, 40.43f },
    { { 0.1f,       0.05f,      0.005f    }, 25.0f, 49.02f },
    { { 0.076563f,  0.038281f,  0.003828f }, 22.5f, 58.15f },
    { { 0.05625f,   0.028125f,  0.002812f }, 20.0f, 61.03f },
    { { 0.039063f,  0.019531f,  0.001953f }, 17.5f, 63.59f },
    { { 0.025f,     0.0125f,    0.00125f  }, 15.0f, 66.47f },
    { { 0.014063f,  0.007031f,  0.000703f }, 12.5f, 71.34f },
    { { 0.f,        0.f,        0.f       }, 10.0f, 72.00f }
};

static light_curve_t ex_hyper_blaster_light[] = {
	{ { 0.04f,      0.02f,      0.0f      },  5.f, 15.00f },
	{ { 0.2f,       0.15f,      0.01f     }, 15.f, 15.00f },
	{ { 0.04f,      0.02f,      0.0f      },  5.f, 15.00f },
};


static light_curve_t ex_blaster_light[] = {
	{ { 0.9f,      0.7f,      0.9f      },  5.f, 15.00f },
	{ { 0.8f,       0.65f,      0.8f		   }, 15.f, 15.00f },
	{ { 0.04f,      0.03f,      0.04f      },  5.f, 15.00f },
};

// Stock Quake II RTX blaster impact light, used when cl_blaster_color is 0.
static light_curve_t ex_blaster_light_original[] = {
	{ { 0.04f,      0.02f,      0.0f      },  5.f, 15.00f },
	{ { 0.2f,       0.15f,      0.01f     }, 15.f, 15.00f },
	{ { 0.04f,      0.02f,      0.0f      },  5.f, 15.00f },
};

static light_curve_t ex_flare_light[] = {
	{ { 1.2f,       0.75f,      0.15f     }, 10.f,  5.00f },
	{ { 1.6f,       1.0f,       0.2f      }, 10.f, 10.00f },
	{ { 1.2f,       0.75f,      0.15f     }, 10.f,  5.00f },
};

// [Q2RTX] The ETF rifle's flechette impact. It shares the ex_blaster explosion
// type, so without this it borrowed the blaster's warm white light and lit the
// wall yellow - which, next to the pale blue shards and the blue skin2 impact
// puff, looked like a different weapon had fired. Same shape and brightness as
// ex_blaster_light, just carrying the darts' colour.
static light_curve_t ex_flechette_light[] = {
	{ { 0.35f,      0.62f,      1.00f     },  5.f, 15.00f },
	{ { 0.30f,      0.55f,      0.90f     }, 15.f, 15.00f },
	{ { 0.02f,      0.03f,      0.04f     },  5.f, 15.00f },
};

static void CL_AddExplosionLight(explosion_t *ex, float phase)
{
	int curve_size;
	light_curve_t* curve;

	switch (ex->type)
	{
	case ex_hyperblaster:
		curve = ex_hyper_blaster_light;
		curve_size = LENGTH(ex_hyper_blaster_light);
		break;
	case ex_poly:
		curve = ex_poly_light;
		curve_size = LENGTH(ex_poly_light);
		break;
	case ex_blaster:
		// TE_FLECHETTE and TE_FLARE both ride ex_blaster; tent_type is what
		// tells them apart once the explosion is allocated.
		if (ex->ent.tent_type == TE_FLECHETTE) {
			curve = ex_flechette_light;
			curve_size = LENGTH(ex_flechette_light);
		} else if (cl_blaster_color->integer) {
			curve = ex_blaster_light;
			curve_size = LENGTH(ex_blaster_light);
		} else {
			curve = ex_blaster_light_original;
			curve_size = LENGTH(ex_blaster_light_original);
		}
		break;
	default:
		return;
	}

	float timeAlpha = ((float)(curve_size - 1)) * phase;
	int baseSample = (int)floorf(timeAlpha);
	baseSample = max(0, min(curve_size - 2, baseSample));

	float w1 = timeAlpha - (float)(baseSample);
	float w0 = 1.f - w1;

	light_curve_t* s0 = curve + baseSample;
	light_curve_t* s1 = curve + baseSample + 1;
	
	float offset = w0 * s0->offset + w1 * s1->offset;
	float radius = w0 * s0->radius + w1 * s1->radius;

	vec3_t origin;
	vec3_t up;
	AngleVectors(ex->ent.angles, NULL, NULL, up);
	VectorMA(ex->ent.origin, offset, up, origin);

	vec3_t color;
	VectorClear(color);
	VectorMA(color, w0, s0->color, color);
	VectorMA(color, w1, s1->color, color);

	V_AddSphereLight(origin, 500.f, color[0], color[1], color[2], radius);
}

static void CL_AddExplosions(void)
{
    entity_t    *ent;
    int         i;
    explosion_t *ex;
    float       frac;
    int         f;

    for (i = 0, ex = cl_explosions; i < MAX_EXPLOSIONS; i++, ex++) {
        if (ex->type == ex_free)
            continue;
		float inv_frametime = ex->frametime ? 1.f / (float)ex->frametime : BASE_1_FRAMETIME;
        frac = (cl.time - ex->start) * inv_frametime;
        f = floor(frac);

        ent = &ex->ent;

        // impact smoke drifts off the surface it came from
        if (ex->vel[0] || ex->vel[1] || ex->vel[2])
            VectorMA(ex->spawn_origin, (cl.time - ex->start) * 0.001f,
                     ex->vel, ent->origin);

        switch (ex->type) {
        case ex_mflash:
            if (f >= ex->frames - 1)
                ex->type = ex_free;
            break;
		case ex_misc:
		case ex_hyperblaster:
		case ex_blaster:
		case ex_flare:
        case ex_light:
            if (f >= ex->frames - 1) {
                ex->type = ex_free;
                break;
            }
            ent->alpha = 1.0f - frac / (ex->frames - 1);
            break;
        case ex_flash:
            if (f >= 1) {
                ex->type = ex_free;
                break;
            }
            ent->alpha = 1.0f;
            break;
        case ex_poly:
            if (f >= ex->frames - 1) {
                ex->type = ex_free;
                break;
            }

            ent->alpha = ((float)ex->frames - (float)f) / (float)ex->frames;
			ent->alpha = max(0.f, min(1.f, ent->alpha));
			ent->alpha = ent->alpha * ent->alpha * (3.f - 2.f * ent->alpha); // smoothstep

            if (f < 10) {
                ent->skinnum = (f >> 1);
                if (ent->skinnum < 0)
                    ent->skinnum = 0;
            } else {
                ent->flags |= RF_TRANSLUCENT;
                if (f < 13)
                    ent->skinnum = 5;
                else
                    ent->skinnum = 6;
            }
            break;
        case ex_poly2:
            if (f >= ex->frames - 1) {
                ex->type = ex_free;
                break;
            }

            ent->alpha = (5.0f - (float)f) / 5.0f;
            ent->skinnum = 0;
            ent->flags |= RF_TRANSLUCENT;
            break;
        default:
            break;
        }

        if (ex->type == ex_free)
            continue;

		if (cls.ref_type == REF_TYPE_VKPT)
			CL_AddExplosionLight(ex, frac / (ex->frames - 1));
		else
		{
        if (ex->light)
            V_AddLight(ent->origin, ex->light * ent->alpha,
                       ex->lightcolor[0], ex->lightcolor[1], ex->lightcolor[2]);
		}

        if (ex->type == ex_mflash) {
            VectorCopy(ent->origin, ent->oldorigin);

            // The flash models are a single frame. The generic advance below
            // would ask for frame baseframe+f+1 and oldframe baseframe+f, and
            // any attempt to keep the former in range drives the latter
            // NEGATIVE - which reads garbage vertices and speckles the screen
            // with coloured blocks. There is nothing to interpolate here.
            ent->frame = 0;
            ent->oldframe = 0;
            ent->backlerp = 0.0f;

            V_AddEntity(ent);
        } else if (ex->type != ex_light) {
            VectorCopy(ent->origin, ent->oldorigin);

            if (f < 0)
                f = 0;
            ent->frame = ex->baseframe + f + 1;
            ent->oldframe = ex->baseframe + f;
            ent->backlerp = 1.0f - (frac - f);

            V_AddEntity(ent);
        }
    }
}

/*
==============================================================

LASER MANAGEMENT

==============================================================
*/

#define MAX_LASERS  32

typedef struct {
    vec3_t      start;
    vec3_t      end;
    int         color;
    color_t     rgba;
    int         width;
    int         lifetime, starttime;
} laser_t;

static laser_t  cl_lasers[MAX_LASERS];

static void CL_ClearLasers(void)
{
    memset(cl_lasers, 0, sizeof(cl_lasers));
}

static laser_t *CL_AllocLaser(void)
{
    laser_t *l;
    int i;

    for (i = 0, l = cl_lasers; i < MAX_LASERS; i++, l++) {
        if (cl.time - l->starttime >= l->lifetime) {
            memset(l, 0, sizeof(*l));
            l->starttime = cl.time;
            return l;
        }
    }

    return NULL;
}

static void CL_AddLasers(void)
{
    laser_t     *l;
    entity_t    ent;
    int         i;
    int         time;

    memset(&ent, 0, sizeof(ent));

    for (i = 0, l = cl_lasers; i < MAX_LASERS; i++, l++) {
        time = l->lifetime - (cl.time - l->starttime);
        if (time < 0) {
            continue;
        }

        if (l->color == -1) {
            ent.rgba = l->rgba;
            ent.alpha = (float)time / (float)l->lifetime;
        } else {
            ent.alpha = 0.30f;
        }

        ent.skinnum = l->color;
        ent.flags = RF_TRANSLUCENT | RF_BEAM;
        VectorCopy(l->start, ent.origin);
        VectorCopy(l->end, ent.oldorigin);
        ent.frame = l->width;

        V_AddEntity(&ent);
    }
}

static void CL_ParseLaser(int colors)
{
    laser_t *l;

    l = CL_AllocLaser();
    if (!l)
        return;

    VectorCopy(te.pos1, l->start);
    VectorCopy(te.pos2, l->end);
    l->lifetime = 100;
    l->color = (colors >> ((Q_rand() % 4) * 8)) & 0xff;
    l->width = 4;
}

/*
==============================================================

BEAM MANAGEMENT

==============================================================
*/

#define MAX_BEAMS   32

typedef struct {
    int         entity;
    int         dest_entity;
    qhandle_t   model;
    int         endtime;
    vec3_t      offset;
    vec3_t      start, end;
} beam_t;

static beam_t   cl_beams[MAX_BEAMS];
static beam_t   cl_playerbeams[MAX_BEAMS];

static void CL_ClearBeams(void)
{
    memset(cl_beams, 0, sizeof(cl_beams));
    memset(cl_playerbeams, 0, sizeof(cl_playerbeams));
}

static void CL_ParseBeam(qhandle_t model)
{
    beam_t  *b;
    int     i;

// override any beam with the same source AND destination entities
    for (i = 0, b = cl_beams; i < MAX_BEAMS; i++, b++)
        if (b->entity == te.entity1 && b->dest_entity == te.entity2)
            goto override;

// find a free beam
    for (i = 0, b = cl_beams; i < MAX_BEAMS; i++, b++) {
        if (!b->model || b->endtime < cl.time) {
override:
            b->entity = te.entity1;
            b->dest_entity = te.entity2;
            b->model = model;
            b->endtime = cl.time + 200;
            VectorCopy(te.pos1, b->start);
            VectorCopy(te.pos2, b->end);
            VectorCopy(te.offset, b->offset);
            return;
        }
    }
}

static void CL_ParsePlayerBeam(qhandle_t model)
{
    beam_t  *b;
    int     i;

// override any beam with the same entity
    for (i = 0, b = cl_playerbeams; i < MAX_BEAMS; i++, b++) {
        if (b->entity == te.entity1) {
            b->entity = te.entity1;
            b->model = model;
            b->endtime = cl.time + 200;
            VectorCopy(te.pos1, b->start);
            VectorCopy(te.pos2, b->end);
            VectorCopy(te.offset, b->offset);
            return;
        }
    }

// find a free beam
    for (i = 0, b = cl_playerbeams; i < MAX_BEAMS; i++, b++) {
        if (!b->model || b->endtime < cl.time) {
            b->entity = te.entity1;
            b->model = model;
            b->endtime = cl.time + 100;     // PMM - this needs to be 100 to prevent multiple heatbeams
            VectorCopy(te.pos1, b->start);
            VectorCopy(te.pos2, b->end);
            VectorCopy(te.offset, b->offset);
            return;
        }
    }

}

/*
=================
CL_AddBeams
=================
*/
static void CL_AddBeams(void)
{
    int         i, j;
    beam_t      *b;
    vec3_t      dist, org;
    float       d;
    entity_t    ent;
    vec3_t      angles;
    float       len, steps;
    float       model_length;

// update beams
    for (i = 0, b = cl_beams; i < MAX_BEAMS; i++, b++) {
        if (!b->model || b->endtime < cl.time)
            continue;

        // if coming from the player, update the start position
        if (b->entity == cl.frame.clientNum + 1)
            VectorAdd(cl.playerEntityOrigin, b->offset, org);
        else
            VectorAdd(b->start, b->offset, org);

        // calculate pitch and yaw
        VectorSubtract(b->end, org, dist);
        vectoangles2(dist, angles);

        // add new entities for the beams
        d = VectorNormalize(dist);
        if (b->model == cl_mod_lightning) {
            model_length = 35.0f;
            d -= 20.0f; // correction so it doesn't end in middle of tesla
        } else {
            model_length = 30.0f;
        }
        steps = ceil(d / model_length);
        len = (d - model_length) / (steps - 1);

        memset(&ent, 0, sizeof(ent));
        ent.model = b->model;

        // PMM - special case for lightning model .. if the real length is shorter than the model,
        // flip it around & draw it from the end to the start.  This prevents the model from going
        // through the tesla mine (instead it goes through the target)
        if ((b->model == cl_mod_lightning) && (d <= model_length)) {
            VectorCopy(b->end, ent.origin);
            ent.flags = RF_FULLBRIGHT;
            ent.angles[0] = angles[0];
            ent.angles[1] = angles[1];
            ent.angles[2] = Q_rand() % 360;
            V_AddEntity(&ent);
            return;
        }

        while (d > 0) {
            VectorCopy(org, ent.origin);
            if (b->model == cl_mod_lightning) {
                ent.flags = RF_FULLBRIGHT;
                ent.angles[0] = -angles[0];
                ent.angles[1] = angles[1] + 180.0f;
                ent.angles[2] = Q_rand() % 360;
            } else {
                ent.angles[0] = angles[0];
                ent.angles[1] = angles[1];
                ent.angles[2] = Q_rand() % 360;
            }

            V_AddEntity(&ent);

            for (j = 0; j < 3; j++)
                org[j] += dist[j] * len;
            d -= model_length;
        }
    }
}


/*
=================
CL_EmitBeamChain

Lay one chain of beam segments from `org` along `dist` (which it normalises).
`extra_flags` is how the two chains of your own beam are kept apart - see
CL_AddPlayerBeams.
=================
*/
static void CL_EmitBeamChain(qhandle_t model, const vec3_t org_in,
                             const vec3_t dist_in, const vec3_t angles,
                             int framenum, int extra_flags)
{
    entity_t    ent;
    vec3_t      org, dist;
    float       d, len, steps, model_length;
    int         j;

    VectorCopy(org_in, org);
    VectorCopy(dist_in, dist);

    d = VectorNormalize(dist);
    model_length = 32.0f;
    steps = ceilf(d / model_length);
    if (steps < 2)
        steps = 2;
    len = (d - model_length) / (steps - 1);

    memset(&ent, 0, sizeof(ent));
    ent.model = model;
    ent.frame = framenum;
    ent.flags = RF_FULLBRIGHT | extra_flags;
    ent.angles[0] = -angles[0];
    ent.angles[1] = angles[1] + 180.0f;
    ent.angles[2] = cl.time % 360;

    while (d > 0) {
        VectorCopy(org, ent.origin);

        V_AddEntity(&ent);

        for (j = 0; j < 3; j++)
            org[j] += dist[j] * len;
        d -= model_length;
    }
}

/*
=================
CL_AddPlayerBeams

Draw player locked beams. Currently only used by the plasma beam.
=================
*/
static void CL_AddPlayerBeams(void)
{
    int         i, j;
    beam_t      *b;
    vec3_t      dist, org;
    float       d;
    entity_t    ent;
    vec3_t      angles;
    float       len, steps;
    int         framenum;
    float       hand_multiplier;
    bool        view_chain;
    player_state_t  *ps, *ops;

    if (info_hand->integer == 2)
        hand_multiplier = 0;
    else if (info_hand->integer == 1)
        hand_multiplier = -1;
    else
        hand_multiplier = 1;

// update beams
    for (i = 0, b = cl_playerbeams; i < MAX_BEAMS; i++, b++) {
        if (!b->model || b->endtime < cl.time)
            continue;

        // Your own beam in first person needs TWO chains. The view-derived one
        // is the only thing that looks right down the barrel - it is aligned to
        // v_forward, which is what makes the sparkle rings circle it - but it
        // sits at eye level, so in a mirror it comes out of your face. The
        // world chain is the opposite. Each is restricted to where it belongs
        // by its render flag; see the emit calls below.
        //
        // In third person (chase cam) there is no view weapon to match, so the
        // world chain is used for everything.
        view_chain = (b->entity == cl.frame.clientNum + 1) &&
                     !cl.thirdPersonView && !cl_beam_thirdperson->integer;

        if (view_chain) {
            // set up gun position
            ps = CL_KEYPS;
            ops = CL_OLDKEYPS;

            for (j = 0; j < 3; j++)
                b->start[j] = cl.refdef.vieworg[j] + ops->gunoffset[j] +
                    CL_KEYLERPFRAC * (ps->gunoffset[j] - ops->gunoffset[j]);

            VectorMA(b->start, (hand_multiplier * b->offset[0]), cl.v_right, org);
            VectorMA(org, b->offset[1], cl.v_forward, org);
            VectorMA(org, b->offset[2], cl.v_up, org);
            if (info_hand->integer == 2)
                VectorMA(org, -1, cl.v_up, org);

            // calculate pitch and yaw
            VectorSubtract(b->end, org, dist);

            // FIXME: don't add offset twice?
            d = VectorLength(dist);
            VectorScale(cl.v_forward, d, dist);
            VectorMA(dist, (hand_multiplier * b->offset[0]), cl.v_right, dist);
            VectorMA(dist, b->offset[1], cl.v_forward, dist);
            VectorMA(dist, b->offset[2], cl.v_up, dist);
            if (info_hand->integer == 2)
                VectorMA(org, -1, cl.v_up, org);

            // The SPARKLE keeps the view-aligned direction above, and it has
            // to: CL_Heatbeam builds its rings in the view basis (cl.v_right /
            // cl.v_up) at a radius of only 1.5 units, so they read as circles
            // around the beam only while the beam runs along v_forward. Point
            // it anywhere else and they tilt and flatten onto it.
            CL_Heatbeam(org, dist);

            // [Q2RTX] ...but the BEAM ITSELF aims at the real impact point.
            //
            // The direction built above is v_forward plus the muzzle offset,
            // which is NOT a line from the muzzle to where the shot actually
            // landed - the offset tilts it down ~3 units and right ~2, so the
            // drawn beam ended just under the impact sparks and read as
            // shooting low. The server's endpoint is the truth: it traced from
            // the eye along v_forward, so b->end IS the crosshair.
            //
            // Decoupling the two is the whole fix - the particles stay
            // view-aligned and the beam connects muzzle to impact.
            VectorSubtract(b->end, org, dist);

            vectoangles2(dist, angles);

            framenum = 1;
        } else {
            VectorCopy(b->start, org);

            // calculate pitch and yaw
            VectorSubtract(b->end, org, dist);
            vectoangles2(dist, angles);

            // A PLAYER's beam - anyone's, including your own reflection - leaves
            // the gun the third-person model is holding, which is well below
            // the eye the server fired from. Drop it to gun height and re-aim.
            //
            // The old correction here did this with a basis built from
            // angles[YAW] + 180 and ended up moving the start FORWARD, which is
            // what put the beam out in front of everybody's chest.
            if (!VectorEmpty(b->offset)) {
                org[2] -= cl_beam_muzzle_drop->value;
                VectorSubtract(b->end, org, dist);
                vectoangles2(dist, angles);
            } else {
                // if it's a monster, do the particle effect
                CL_MonsterPlasma_Shell(b->start);
            }

            framenum = 2;
        }

        // [Q2RTX] Do NOT slide the start of a beam forward to avoid the gun.
        // The first 32-unit segment sits right against the near plane and is
        // drawn hugely magnified, which looks like a bright blob over the gun -
        // but that magnified segment IS what makes the beam read as coming out
        // of the barrel, and the original draws it too. A forward nudge was
        // tried (cl_beam_offset, 12 units) and it moved the visible start off
        // the muzzle into mid-air; that is much worse. The gun is kept on top
        // by MATERIAL_FLAG_PLAYER_BEAM in primary_rays.rgen instead.
        //
        // RF_WEAPONMODEL on the view chain is what keeps it OUT OF MIRRORS:
        // reflect_refract.rgen includes AS_FLAG_VIEWER_MODELS and excludes
        // AS_FLAG_VIEWER_WEAPON whenever cl_player_model is FIRST_PERSON, which
        // is the default - the same rule that already hides your gun, and shows
        // your body, in a mirror.
        CL_EmitBeamChain(b->model, org, dist, angles, framenum,
                         view_chain ? RF_WEAPONMODEL : 0);

        // ...and the matching world chain, which only a mirror (or another
        // player) ever sees. RF_VIEWERMODEL is the exact complement: primary
        // rays drop it, reflections keep it.
        if (view_chain) {
            vec3_t  world_org, world_dist, world_angles;

            VectorCopy(b->start, world_org);
            world_org[2] -= cl_beam_muzzle_drop->value;
            VectorSubtract(b->end, world_org, world_dist);
            vectoangles2(world_dist, world_angles);

            CL_EmitBeamChain(b->model, world_org, world_dist, world_angles,
                             2, RF_VIEWERMODEL);
        }
    }
}


/*
==============================================================

SUSTAIN MANAGEMENT

==============================================================
*/

#define MAX_SUSTAINS    32

static cl_sustain_t     cl_sustains[MAX_SUSTAINS];

static void CL_ClearSustains(void)
{
    memset(cl_sustains, 0, sizeof(cl_sustains));
}

static cl_sustain_t *CL_AllocSustain(void)
{
    cl_sustain_t    *s;
    int             i;

    for (i = 0, s = cl_sustains; i < MAX_SUSTAINS; i++, s++) {
        if (s->id == 0)
            return s;
    }

    return NULL;
}

static void CL_ProcessSustain(void)
{
    cl_sustain_t    *s;
    int             i;

    for (i = 0, s = cl_sustains; i < MAX_SUSTAINS; i++, s++) {
        if (s->id) {
            if ((s->endtime >= cl.time) && (cl.time >= s->nextthink))
                s->think(s);
            else if (s->endtime < cl.time)
                s->id = 0;
        }
    }
}

static void CL_ParseSteam(void)
{
    cl_sustain_t    *s;

    if (te.entity1 == -1) {
        CL_ParticleSteamEffect(te.pos1, te.dir, te.color & 0xff, te.count, te.entity2);
        return;
    }

    s = CL_AllocSustain();
    if (!s)
        return;

    s->id = te.entity1;
    s->count = te.count;
    VectorCopy(te.pos1, s->org);
    VectorCopy(te.dir, s->dir);
    s->color = te.color & 0xff;
    s->magnitude = te.entity2;
    s->endtime = cl.time + te.time;
    s->think = CL_ParticleSteamEffect2;
    s->nextthink = cl.time;
}

static void CL_ParseWidow(void)
{
    cl_sustain_t    *s;

    s = CL_AllocSustain();
    if (!s)
        return;

    s->id = te.entity1;
    VectorCopy(te.pos1, s->org);
    s->endtime = cl.time + 2100;
    s->think = CL_Widowbeamout;
    s->nextthink = cl.time;
}

static void CL_ParseNuke(void)
{
    cl_sustain_t    *s;

    s = CL_AllocSustain();
    if (!s)
        return;

    s->id = 21000;
    VectorCopy(te.pos1, s->org);
    s->endtime = cl.time + 1000;
    s->think = CL_Nukeblast;
    s->nextthink = cl.time;
}

//==============================================================

static color_t  railcore_color;
static color_t  railspiral_color;

static cvar_t *cl_railtrail_type;
static cvar_t *cl_railtrail_time;
static cvar_t *cl_railcore_color;
static cvar_t *cl_railcore_width;
static cvar_t *cl_railspiral_color;
static cvar_t *cl_railspiral_radius;

static void cl_railcore_color_changed(cvar_t *self)
{
    if (!SCR_ParseColor(self->string, &railcore_color)) {
        Com_WPrintf("Invalid value '%s' for '%s'\n", self->string, self->name);
        Cvar_Reset(self);
        railcore_color.u32 = U32_RED;
    }
}

static void cl_railspiral_color_changed(cvar_t *self)
{
    if (!SCR_ParseColor(self->string, &railspiral_color)) {
        Com_WPrintf("Invalid value '%s' for '%s'\n", self->string, self->name);
        Cvar_Reset(self);
        railspiral_color.u32 = U32_BLUE;
    }
}

static void CL_RailCore(void)
{
    laser_t *l;

    l = CL_AllocLaser();
    if (!l)
        return;

    VectorCopy(te.pos1, l->start);
    VectorCopy(te.pos2, l->end);
    l->color = -1;
    l->lifetime = cl_railtrail_time->integer;
    l->width = cl_railcore_width->integer;
    l->rgba.u32 = railcore_color.u32;
}

static void CL_RailSpiral(void)
{
    vec3_t      move;
    vec3_t      vec;
    float       len;
    int         j;
    cparticle_t *p;
    vec3_t      right, up;
    int         i;
    float       d, c, s;
    vec3_t      dir;

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

        d = i * 0.1f;
        c = cos(d);
        s = sin(d);

        VectorScale(right, c, dir);
        VectorMA(dir, s, up, dir);

        p->alpha = 1.0f;
        p->alphavel = -1.0f / (cl_railtrail_time->value + frand() * 0.2f);
        p->color = -1;
        p->rgba.u32 = railspiral_color.u32;
		p->brightness = cvar_pt_particle_emissive->value;
        for (j = 0; j < 3; j++) {
            p->org[j] = move[j] + dir[j] * cl_railspiral_radius->value;
            p->vel[j] = dir[j] * 6;
        }

        VectorAdd(move, vec, move);
    }
}

static void CL_RailLights(color_t color)
{
	vec3_t fcolor;
	fcolor[0] = (float)color.u8[0] / 255.f;
	fcolor[1] = (float)color.u8[1] / 255.f;
	fcolor[2] = (float)color.u8[2] / 255.f;

	vec3_t      move;
	vec3_t      vec;
	float       len;

	VectorCopy(te.pos1, move);
	VectorSubtract(te.pos2, te.pos1, vec);
	len = VectorNormalize(vec);

	float num_segments = ceilf(len / 100.f);
	float segment_size = len / num_segments;

	for (float segment = 0; segment < num_segments; segment++)
	{
		float offset = (segment + 0.25f) * segment_size;
		vec3_t pos;
		VectorMA(move, offset, vec, pos);

		cdlight_t* dl = CL_AllocDlight(0);
		VectorScale(fcolor, 0.25f, dl->color);
		VectorCopy(pos, dl->origin);
		dl->radius = 400;
		dl->decay = 400;
		dl->die = cl.time + 1000;
		VectorScale(vec, segment_size * 0.5f, dl->velosity);
	}
}

extern uint32_t d_8to24table[256];
extern cvar_t* cvar_pt_beam_lights;

static void CL_RailTrail(void)
{
	color_t rail_color;
	
	if (!cl_railtrail_type->integer) 
	{
		rail_color.u32 = d_8to24table[0x74];

        CL_OldRailTrail();
	}
	else 
	{
		rail_color = railcore_color;

        if (cl_railcore_width->integer > 0) {
            CL_RailCore();
        }
        if (cl_railtrail_type->integer > 1) {
            CL_RailSpiral();
        }
    }

    if (!cl_railtrail_type->integer || cvar_pt_beam_lights->value <= 0)
    {
        CL_RailLights(rail_color);
    }
}

static void dirtoangles(vec3_t angles)
{
    angles[0] = RAD2DEG(acos(te.dir[2]));
    if (te.dir[0])
        angles[1] = RAD2DEG(atan2(te.dir[1], te.dir[0]));
    else if (te.dir[1] > 0)
        angles[1] = 90;
    else if (te.dir[1] < 0)
        angles[1] = 270;
    else
        angles[1] = 0;
}

/*
=================
CL_ParseTEnt
=================
*/
static const byte splash_color[] = {0x00, 0xe0, 0xb0, 0x50, 0xd0, 0xe0, 0xe8};

void CL_ParseTEnt(void)
{
    explosion_t *ex;
    int r;
    int i;

    switch (te.type) {
    case TE_BLOOD:          // bullet hitting flesh
        if (!(cl_disable_particles->integer & NOPART_BLOOD))
        {
            // 60 is the count the original game used for TE_BLOOD. This tree
            // had it at 40, which mattered less while most of the spray was
            // being flung out of sight - see CL_BloodParticleEffect.
            CL_BloodParticleEffect(te.pos1, te.dir, 0xe8, 60);
        }
        break;

    case TE_GUNSHOT:            // bullet hitting wall
    case TE_SPARKS:
    case TE_BULLET_SPARKS:
        if (te.type == TE_GUNSHOT)
            CL_ParticleEffect(te.pos1, te.dir, 0, 40);
        else
            CL_ParticleEffect(te.pos1, te.dir, 0xe0, 6);

        if (te.type != TE_SPARKS) {
            CL_ImpactSmokeAndFlash(te.pos1, te.dir);

            // impact sound
            r = Q_rand() & 15;
            if (r == 1)
                S_StartSound(te.pos1, 0, 0, cl_sfx_ric1, 1, ATTN_NORM, 0);
            else if (r == 2)
                S_StartSound(te.pos1, 0, 0, cl_sfx_ric2, 1, ATTN_NORM, 0);
            else if (r == 3)
                S_StartSound(te.pos1, 0, 0, cl_sfx_ric3, 1, ATTN_NORM, 0);
        }
        break;

    case TE_SCREEN_SPARKS:
    case TE_SHIELD_SPARKS:
        if (te.type == TE_SCREEN_SPARKS)
            CL_ParticleEffect(te.pos1, te.dir, 0xd0, 40);
        else
            CL_ParticleEffect(te.pos1, te.dir, 0xb0, 40);
        //FIXME : replace or remove this sound
        S_StartSound(te.pos1, 0, 257, cl_sfx_lashit, 1, ATTN_NORM, 0);
        break;

    case TE_SHOTGUN:            // bullet hitting wall
        CL_ParticleEffect(te.pos1, te.dir, 0, 20);
        CL_ImpactSmokeAndFlash(te.pos1, te.dir);
        break;

    case TE_SPLASH:         // bullet hitting water
        if (te.color < 0 || te.color > 6)
            r = 0x00;
        else
            r = splash_color[te.color];
        CL_ParticleEffectWaterSplash(te.pos1, te.dir, r, te.count);

        if (te.color == SPLASH_SPARKS) {
            r = Q_rand() & 3;
            if (r == 0)
                S_StartSound(te.pos1, 0, 0, cl_sfx_spark5, 1, ATTN_STATIC, 0);
            else if (r == 1)
                S_StartSound(te.pos1, 0, 0, cl_sfx_spark6, 1, ATTN_STATIC, 0);
            else
                S_StartSound(te.pos1, 0, 0, cl_sfx_spark7, 1, ATTN_STATIC, 0);
        }
        break;

    case TE_BERSERK_SLAM: {
        // Rerelease: the berserk's ground slam.  No dedicated model here, so
        // it is built from stock pieces - a wide low fan of debris thrown up
        // off the floor, the usual impact smoke, and a brief warm flash to
        // light the burst.  te.dir is the ground normal (straight up).
        cdlight_t *dl;

        CL_ParticleEffect(te.pos1, te.dir, 0xe0, 120);
        CL_ImpactSmokeAndFlash(te.pos1, te.dir);

        dl = CL_AllocDlight(0);
        VectorCopy(te.pos1, dl->origin);
        dl->radius = 200;
        dl->die = cl.time + 200;
        dl->decay = 400;
        VectorSet(dl->color, 1.0f, 0.7f, 0.35f);
        break;
    }

    case TE_LASER_SPARKS:
        CL_ParticleEffect2(te.pos1, te.dir, te.color, te.count);
        break;

    case TE_BLUEHYPERBLASTER:
        CL_BlasterParticles(te.pos1, te.dir);
        break;
	case TE_HYPERBLASTER:
    case TE_BLASTER:            // blaster hitting wall
    case TE_BLASTER2:           // green blaster hitting wall
    case TE_FLECHETTE:          // flechette
	case TE_FLARE:              // flare
        ex = CL_AllocExplosion();
        VectorCopy(te.pos1, ex->ent.origin);
        dirtoangles(ex->ent.angles);
        ex->type = ex_blaster;
        ex->ent.flags = RF_FULLBRIGHT | RF_TRANSLUCENT;
		ex->ent.tent_type = te.type;
        switch (te.type) {
		case TE_HYPERBLASTER:
			CL_BlasterParticles(te.pos1, te.dir);
			ex->lightcolor[0] = 1;
			ex->lightcolor[1] = 1;
			ex->type = ex_hyperblaster;
			break;
        case TE_BLASTER:
            CL_BlasterParticles(te.pos1, te.dir);
            ex->lightcolor[0] = 0.5;
            ex->lightcolor[1] = 0.3;
			ex->lightcolor[2] = 0.5;
            break;
        case TE_BLASTER2:
            CL_BlasterParticles2(te.pos1, te.dir, 0xd0);
            ex->ent.skinnum = 1;
            ex->lightcolor[1] = 1;
            break;
        case TE_FLECHETTE:
            CL_BlasterParticles2(te.pos1, te.dir, 0x6f);  // 75
            ex->ent.skinnum = 2;
            ex->lightcolor[0] = 0.19f;
            ex->lightcolor[1] = 0.41f;
            ex->lightcolor[2] = 0.75f;
            break;
		case TE_FLARE:
			CL_BlasterParticles2(te.pos1, te.dir, 0xd0);
			ex->lightcolor[0] = 1;
			ex->lightcolor[1] = 1;
			ex->type = ex_flare;
			break;
        }
        ex->start = cl.servertime - CL_FRAMETIME;
        ex->light = 150;
        ex->ent.model = cl_mod_explode;
        ex->frames = 4;

		if (te.type != TE_FLARE)
		{
            S_StartSound(te.pos1,  0, 0, cl_sfx_lashit, 1, ATTN_NORM, 0);
        }
        else
        {
            // te.count is set to 1 on the first tick of the flare, 0 afterwards.
            // Play it on CHAN_VOICE rather than CHAN_AUTO so that a repeat for
            // the same flare replaces the old one instead of eating another of
            // the 32 mix channels.
            if (te.count!=0)
                S_StartSound(NULL, te.entity1, CHAN_VOICE, cl_sfx_flare, 0.5, ATTN_NORM, 0);
        }
        break;

    case TE_RAILTRAIL:          // railgun effect
        CL_RailTrail();
        S_StartSound(te.pos2, 0, 0, cl_sfx_railg, 1, ATTN_NORM, 0);
        break;

    case TE_GRENADE_EXPLOSION:
    case TE_GRENADE_EXPLOSION_WATER:
		ex = CL_PlainExplosion(false);
		if (!cl_explosion_sprites->integer)
		{
            ex->frames = 19;
            ex->baseframe = 30;
		}
        if (cl_disable_explosions->integer & NOEXP_GRENADE)
            ex->type = ex_light;

        if (!(cl_disable_particles->integer & NOPART_GRENADE_EXPLOSION))
            CL_ExplosionParticles(te.pos1);

        if (cl_dlight_hacks->integer & DLHACK_SMALLER_EXPLOSION)
            ex->light = 200;

        if (te.type == TE_GRENADE_EXPLOSION_WATER)
            S_StartSound(te.pos1, 0, 0, cl_sfx_watrexp, 1, ATTN_NORM, 0);
        else
            S_StartSound(te.pos1, 0, 0, cl_sfx_grenexp, 1, ATTN_NORM, 0);
        break;

    case TE_EXPLOSION2:
        ex = CL_PlainExplosion(false);
		if (!cl_explosion_sprites->integer)
		{
            ex->frames = 19;
            ex->baseframe = 30;
		}
        CL_ExplosionParticles(te.pos1);
        S_StartSound(te.pos1, 0, 0, cl_sfx_grenexp, 1, ATTN_NORM, 0);
        break;

    case TE_PLASMA_EXPLOSION:
        CL_PlainExplosion(false);
        CL_ExplosionParticles(te.pos1);
        S_StartSound(te.pos1, 0, 0, cl_sfx_rockexp, 1, ATTN_NORM, 0);
        break;

    case TE_ROCKET_EXPLOSION:
    case TE_ROCKET_EXPLOSION_WATER:
        ex = CL_PlainExplosion(false);
        if (cl_disable_explosions->integer & NOEXP_ROCKET)
            ex->type = ex_light;

        if (!(cl_disable_particles->integer & NOPART_ROCKET_EXPLOSION))
            CL_ExplosionParticles(te.pos1);

        if (cl_dlight_hacks->integer & DLHACK_SMALLER_EXPLOSION)
            ex->light = 200;

        if (te.type == TE_ROCKET_EXPLOSION_WATER)
            S_StartSound(te.pos1, 0, 0, cl_sfx_watrexp, 1, ATTN_NORM, 0);
        else
            S_StartSound(te.pos1, 0, 0, cl_sfx_rockexp, 1, ATTN_NORM, 0);
        break;

    case TE_EXPLOSION1:
        CL_PlainExplosion(false);
        CL_ExplosionParticles(te.pos1);
        S_StartSound(te.pos1, 0, 0, cl_sfx_rockexp, 1, ATTN_NORM, 0);
        break;

    case TE_EXPLOSION1_NP:
        CL_PlainExplosion(false);
        S_StartSound(te.pos1, 0, 0, cl_sfx_rockexp, 1, ATTN_NORM, 0);
        break;

    case TE_EXPLOSION1_BIG:
        ex = CL_PlainExplosion(true);
        S_StartSound(te.pos1, 0, 0, cl_sfx_rockexp, 1, ATTN_NORM, 0);
        break;

    case TE_BFG_EXPLOSION:
        ex = CL_AllocExplosion();
        VectorCopy(te.pos1, ex->ent.origin);
        ex->type = ex_poly;
        ex->ent.flags = RF_FULLBRIGHT;
        ex->start = cl.servertime - CL_FRAMETIME;
        ex->light = 350;
        ex->lightcolor[0] = 0.0f;
        ex->lightcolor[1] = 1.0f;
        ex->lightcolor[2] = 0.0f;
        ex->ent.model = cl_mod_bfg_explo;
        ex->ent.flags |= RF_TRANSLUCENT;
        ex->ent.alpha = 0.80;
        ex->frames = 4;
        break;

    case TE_BFG_BIGEXPLOSION:
        CL_BFGExplosionParticles(te.pos1);
        break;

    case TE_BFG_LASER:
        CL_ParseLaser(0xd0d1d2d3);
        break;

    case TE_BUBBLETRAIL:
        CL_BubbleTrail(te.pos1, te.pos2);
        break;

    case TE_PARASITE_ATTACK:
    case TE_MEDIC_CABLE_ATTACK:
        VectorClear(te.offset);
        te.entity2 = 0;
        CL_ParseBeam(cl_mod_parasite_segment);
        break;

    // The lightning model with no sound - see the enum. Held beams re-send
    // every frame, and TE_LIGHTNING's sound would then machine-gun.
    case TE_LIGHTNING_BEAM:
        VectorClear(te.offset);
        te.entity2 = 0;
        CL_ParseBeam(cl_mod_lightning);
        break;

    case TE_BOSSTPORT:          // boss teleporting to station
        CL_BigTeleportParticles(te.pos1);
        S_StartSound(te.pos1, 0, 0, S_RegisterSound("misc/bigtele.wav"), 1, ATTN_NONE, 0);
        break;

    case TE_GRAPPLE_CABLE:
        te.entity2 = 0;
        CL_ParseBeam(cl_mod_grapple_cable);
        break;

    case TE_WELDING_SPARKS:
        CL_ParticleEffect2(te.pos1, te.dir, te.color, te.count);

        ex = CL_AllocExplosion();
        VectorCopy(te.pos1, ex->ent.origin);
        ex->type = ex_flash;
        // note to self
        // we need a better no draw flag
        ex->ent.flags = RF_BEAM;
        ex->start = cl.servertime - CL_FRAMETIME;
        ex->light = 100 + (Q_rand() % 75);
        ex->lightcolor[0] = 1.0f;
        ex->lightcolor[1] = 1.0f;
        ex->lightcolor[2] = 0.3f;
        ex->ent.model = cl_mod_flash;
        ex->frames = 2;
        break;

    case TE_GREENBLOOD:
        CL_ParticleEffect2(te.pos1, te.dir, 0xdf, 30);
        break;

    case TE_TUNNEL_SPARKS:
        CL_ParticleEffect3(te.pos1, te.dir, te.color, te.count);
        break;

    case TE_LIGHTNING:
        S_StartSound(NULL, te.entity1, CHAN_WEAPON, cl_sfx_lightning, 1, ATTN_NORM, 0);
        VectorClear(te.offset);
        CL_ParseBeam(cl_mod_lightning);
        break;

    case TE_DEBUGTRAIL:
        CL_DebugTrail(te.pos1, te.pos2);
        break;

    case TE_PLAIN_EXPLOSION:
        CL_PlainExplosion(false);
        break;

    case TE_FLASHLIGHT:
        CL_Flashlight(te.entity1, te.pos1);
        break;

    case TE_POI_PATH:
        cl.poi_path_count = te.count;
        for (i = 0; i < te.count && i < MAX_POI_PATH; i++)
            VectorCopy(te.path[i], cl.poi_path[i]);
        break;

    case TE_POI:
        VectorCopy(te.pos1, cl.poi_origin);
        // a fresh objective invalidates any trail we were still drawing; the
        // TE_POI_PATH that belongs to it arrives in the same message
        cl.poi_path_count = 0;
        // the image is a CS_IMAGES index, so it is already a registered pic
        if (te.count > 0 && te.count < MAX_IMAGES)
            cl.poi_pic = cl.image_precache[te.count];
        else
            cl.poi_pic = 0;
        cl.poi_time = cl.time + te.time * 100;
        break;

    case TE_FORCEWALL:
        CL_ForceWall(te.pos1, te.pos2, te.color);
        break;

    case TE_HEATBEAM:
        VectorSet(te.offset, 2, 7, -3);
        CL_ParsePlayerBeam(cl_mod_heatbeam);
        break;

    case TE_MONSTER_HEATBEAM:
        VectorClear(te.offset);
        CL_ParsePlayerBeam(cl_mod_heatbeam);
        break;

    case TE_HEATBEAM_SPARKS:
        CL_ParticleSteamEffect(te.pos1, te.dir, 0x8, 50, 60);
        S_StartSound(te.pos1,  0, 0, cl_sfx_lashit, 1, ATTN_NORM, 0);
        break;

    case TE_HEATBEAM_STEAM:
        CL_ParticleSteamEffect(te.pos1, te.dir, 0xE0, 20, 60);
        S_StartSound(te.pos1,  0, 0, cl_sfx_lashit, 1, ATTN_NORM, 0);
        break;

    case TE_STEAM:
        CL_ParseSteam();
        break;

    case TE_BUBBLETRAIL2:
        CL_BubbleTrail2(te.pos1, te.pos2, 8);
        S_StartSound(te.pos1,  0, 0, cl_sfx_lashit, 1, ATTN_NORM, 0);
        break;

    case TE_MOREBLOOD:
        CL_ParticleEffect(te.pos1, te.dir, 0xe8, 250);
        break;

    case TE_CHAINFIST_SMOKE:
        VectorSet(te.dir, 0, 0, 1);
        CL_ParticleSmokeEffect(te.pos1, te.dir, 0, 20, 20);
        break;

    case TE_ELECTRIC_SPARKS:
        CL_ParticleEffect(te.pos1, te.dir, 0x75, 40);
        //FIXME : replace or remove this sound
        S_StartSound(te.pos1, 0, 0, cl_sfx_lashit, 1, ATTN_NORM, 0);
        break;

    case TE_TRACKER_EXPLOSION:
        // [Q2RTX] rogue flashed a NEGATIVE dlight here (-1,-1,-1, radius 150,
        // 100 ms). The disruptor fires about ten times a second, so under the
        // path tracer that was a near-continuous energy-removing light sitting
        // on the victim - it blacked out half the room. The dark look comes
        // from CL_ColorExplosionParticles and the tracker shell instead.
        CL_ColorExplosionParticles(te.pos1, 0, 1);
        S_StartSound(te.pos1, 0, 0, cl_sfx_disrexp, 1, ATTN_NORM, 0);
        break;

    case TE_TELEPORT_EFFECT:
    case TE_DBALL_GOAL:
        CL_TeleportParticles(te.pos1);
        break;

    case TE_WIDOWBEAMOUT:
        CL_ParseWidow();
        break;

    case TE_NUKEBLAST:
        CL_ParseNuke();
        break;

    case TE_WIDOWSPLASH:
        CL_WidowSplash();
        break;

    default:
        Com_Error(ERR_DROP, "%s: bad type", __func__);
    }
}

/*
=================
CL_AddTEnts
=================
*/
void CL_AddTEnts(void)
{
    CL_AddBeams();
    CL_AddPlayerBeams();
    CL_AddExplosions();
    CL_ProcessSustain();
    CL_AddLasers();
}

/*
=================
CL_ClearTEnts
=================
*/
void CL_ClearTEnts(void)
{
    CL_ClearBeams();
    CL_ClearExplosions();
    CL_ClearLasers();
    CL_ClearSustains();
}

void CL_InitTEnts(void)
{
    cl_beam_thirdperson = Cvar_Get("cl_beam_thirdperson", "0", CVAR_ARCHIVE);
    cl_beam_muzzle_drop = Cvar_Get("cl_beam_muzzle_drop", "10", CVAR_ARCHIVE);
    cl_railtrail_type = Cvar_Get("cl_railtrail_type", "0", 0);
    cl_railtrail_time = Cvar_Get("cl_railtrail_time", "1.0", 0);
    cl_railtrail_time->changed = cl_timeout_changed;
    cl_railtrail_time->changed(cl_railtrail_time);
    cl_railcore_color = Cvar_Get("cl_railcore_color", "red", 0);
    cl_railcore_color->changed = cl_railcore_color_changed;
    cl_railcore_color->generator = Com_Color_g;
    cl_railcore_color_changed(cl_railcore_color);
    cl_railcore_width = Cvar_Get("cl_railcore_width", "2", 0);
    cl_railspiral_color = Cvar_Get("cl_railspiral_color", "blue", 0);
    cl_railspiral_color->changed = cl_railspiral_color_changed;
    cl_railspiral_color->generator = Com_Color_g;
    cl_railspiral_color_changed(cl_railspiral_color);
    cl_railspiral_radius = Cvar_Get("cl_railspiral_radius", "3", 0);
}

