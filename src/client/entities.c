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
// cl_ents.c -- entity parsing and management

#include "client.h"
#include "refresh/models.h"
#include "baseq2/g_local.h"

extern qhandle_t cl_mod_powerscreen;
extern qhandle_t cl_mod_tracker_shell;
extern qhandle_t cl_mod_laser;
extern qhandle_t cl_mod_dmspot;
extern qhandle_t cl_sfx_footsteps[4];

/*
=========================================================================

FRAME PARSING

=========================================================================
*/

static inline bool entity_is_optimized(const entity_state_t *state)
{
    if (cls.serverProtocol != PROTOCOL_VERSION_Q2PRO)
        return false;

    if (state->number != cl.frame.clientNum + 1)
        return false;

    if (cl.frame.ps.pmove.pm_type >= PM_DEAD)
        return false;

    return true;
}

static inline void
entity_update_new(centity_t *ent, const entity_state_t *state, const vec_t *origin)
{
    static int entity_ctr;
    ent->id = ++entity_ctr;
    ent->trailcount = 1024;     // for diminishing rocket / grenade trails

    // duplicate the current state so lerping doesn't hurt anything
    ent->prev = *state;
    ent->prev_frame = state->frame;
#if USE_FPS
    ent->event_frame = cl.frame.number;
#endif

    if (state->event == EV_PLAYER_TELEPORT ||
        state->event == EV_OTHER_TELEPORT ||
        (state->renderfx & (RF_FRAMELERP | RF_BEAM))) {
        // no lerping if teleported
        VectorCopy(origin, ent->lerp_origin);
        return;
    }

    // old_origin is valid for new entities,
    // so use it as starting point for interpolating between
    VectorCopy(state->old_origin, ent->prev.origin);
    VectorCopy(state->old_origin, ent->lerp_origin);
}

static inline void
entity_update_old(centity_t *ent, const entity_state_t *state, const vec_t *origin)
{
    int event = state->event;

#if USE_FPS
    // check for new event
    if (state->event != ent->current.event)
        ent->event_frame = cl.frame.number; // new
    else if (cl.frame.number - ent->event_frame >= cl.framediv)
        ent->event_frame = cl.frame.number; // refreshed
    else
        event = 0; // duplicated
#endif

    if (state->modelindex != ent->current.modelindex
        || state->modelindex2 != ent->current.modelindex2
        || state->modelindex3 != ent->current.modelindex3
        || state->modelindex4 != ent->current.modelindex4
        || event == EV_PLAYER_TELEPORT
        || event == EV_OTHER_TELEPORT
        || fabsf(origin[0] - ent->current.origin[0]) > 512
        || fabsf(origin[1] - ent->current.origin[1]) > 512
        || fabsf(origin[2] - ent->current.origin[2]) > 512
        || cl_nolerp->integer == 1) {
        // some data changes will force no lerping
        ent->trailcount = 1024;     // for diminishing rocket / grenade trails

        // duplicate the current state so lerping doesn't hurt anything
        ent->prev = *state;
        ent->prev_frame = state->frame;
        // no lerping if teleported or morphed
        VectorCopy(origin, ent->lerp_origin);
        return;
    }

    // start alias model animation
    if (state->frame != ent->current.frame) {
        ent->prev_frame = ent->current.frame;
        ent->anim_start = cl.servertime - cl.frametime;
    }

    // shuffle the last state to previous
    ent->prev = ent->current;
}

static inline bool entity_is_new(const centity_t *ent)
{
    if (!cl.oldframe.valid)
        return true;    // last received frame was invalid

    if (ent->serverframe != cl.oldframe.number)
        return true;    // wasn't in last received frame

    if (cl_nolerp->integer == 2)
        return true;    // developer option, always new

    if (cl_nolerp->integer == 3)
        return false;   // developer option, lerp from last received frame

    if (cl.oldframe.number != cl.frame.number - 1)
        return true;    // previous server frame was dropped

    return false;
}

static void parse_entity_update(const entity_state_t *state)
{
    centity_t *ent = &cl_entities[state->number];
    const vec_t *origin;
    vec3_t origin_v;

    // if entity is solid, decode mins/maxs and add to the list
    if (state->solid && state->number != cl.frame.clientNum + 1
        && cl.numSolidEntities < MAX_PACKET_ENTITIES) {
        cl.solidEntities[cl.numSolidEntities++] = ent;
        if (state->solid != PACKED_BSP) {
            // encoded bbox
            if (cl.esFlags & MSG_ES_LONGSOLID) {
                MSG_UnpackSolid32(state->solid, ent->mins, ent->maxs);
            } else {
                MSG_UnpackSolid16(state->solid, ent->mins, ent->maxs);
            }
        }
    }

    // work around Q2PRO server bandwidth optimization
    if (entity_is_optimized(state)) {
        VectorScale(cl.frame.ps.pmove.origin, 0.125f, origin_v);
        origin = origin_v;
    } else {
        origin = state->origin;
    }

    if (entity_is_new(ent)) {
        // wasn't in last update, so initialize some things
        entity_update_new(ent, state, origin);
    } else {
        entity_update_old(ent, state, origin);
    }

    ent->serverframe = cl.frame.number;
    ent->current = *state;

    // work around Q2PRO server bandwidth optimization
    if (entity_is_optimized(state)) {
        Com_PlayerToEntityState(&cl.frame.ps, &ent->current);
    }
}

// an entity has just been parsed that has an event value
static void parse_entity_event(int number)
{
    centity_t *cent = &cl_entities[number];

    // EF_TELEPORTER acts like an event, but is not cleared each frame
    if ((cent->current.effects & EF_TELEPORTER) && CL_FRAMESYNC) {
        CL_TeleporterParticles(cent->current.origin);
    }

#if USE_FPS
    if (cent->event_frame != cl.frame.number)
        return;
#endif
    switch (cent->current.event) {
    case EV_ITEM_RESPAWN:
        S_StartSound(NULL, number, CHAN_WEAPON, S_RegisterSound("items/respawn1.wav"), 1, ATTN_IDLE, 0);
        CL_ItemRespawnParticles(cent->current.origin);
        break;
    case EV_PLAYER_TELEPORT:
        S_StartSound(NULL, number, CHAN_WEAPON, S_RegisterSound("misc/tele1.wav"), 1, ATTN_IDLE, 0);
        CL_TeleportParticles(cent->current.origin);
        break;
    case EV_FOOTSTEP:
        if (cl_footsteps->integer) {
            // what you are standing on picks the sound; 0 means we could not
            // tell, and the generic set is used
            qhandle_t sfx = CL_FootstepSound(cent->current.origin);

            if (!sfx)
                sfx = cl_sfx_footsteps[Q_rand() & 3];
            S_StartSound(NULL, number, CHAN_BODY, sfx, 1, ATTN_NORM, 0);
        }
        break;
    case EV_FALLSHORT:
        S_StartSound(NULL, number, CHAN_AUTO, S_RegisterSound("player/land1.wav"), 1, ATTN_NORM, 0);
        break;
    case EV_FALL:
        S_StartSound(NULL, number, CHAN_AUTO, S_RegisterSound("*fall2.wav"), 1, ATTN_NORM, 0);
        break;
    case EV_FALLFAR:
        S_StartSound(NULL, number, CHAN_AUTO, S_RegisterSound("*fall1.wav"), 1, ATTN_NORM, 0);
        break;
    }
}

static void set_active_state(void)
{
    cls.state = ca_active;

    cl.serverdelta = Q_align(cl.frame.number, CL_FRAMEDIV);
    cl.time = cl.servertime = 0; // set time, needed for demos
#if USE_FPS
    cl.keytime = cl.keyservertime = 0;
    cl.keyframe = cl.frame; // initialize keyframe to make sure it's valid
#endif

    // initialize oldframe so lerping doesn't hurt anything
    cl.oldframe.valid = false;
    cl.oldframe.ps = cl.frame.ps;
#if USE_FPS
    cl.oldkeyframe.valid = false;
    cl.oldkeyframe.ps = cl.keyframe.ps;
#endif

    cl.frameflags = 0;

    if (cls.netchan) {
        cl.initialSeq = cls.netchan->outgoing_sequence;
    }

    if (cls.demo.playback) {
        // init some demo things
        CL_FirstDemoFrame();
    } else {
        // set initial cl.predicted_origin and cl.predicted_angles
        VectorScale(cl.frame.ps.pmove.origin, 0.125f, cl.predicted_origin);
        VectorScale(cl.frame.ps.pmove.velocity, 0.125f, cl.predicted_velocity);
        if (cl.frame.ps.pmove.pm_type < PM_DEAD &&
            cls.serverProtocol > PROTOCOL_VERSION_DEFAULT) {
            // enhanced servers don't send viewangles
            CL_PredictAngles();
        } else {
            // just use what server provided
            VectorCopy(cl.frame.ps.viewangles, cl.predicted_angles);
        }
    }

    SCR_EndLoadingPlaque();     // get rid of loading plaque
    SCR_LagClear();
    Con_Close(false);           // get rid of connection screen

    CL_CheckForPause();

    CL_UpdateFrameTimes();

    if (!cls.demo.playback) {
        EXEC_TRIGGER(cl_beginmapcmd);
        Cmd_ExecTrigger("#cl_enterlevel");
    }
}

static void
check_player_lerp(server_frame_t *oldframe, server_frame_t *frame, int framediv)
{
    player_state_t *ps, *ops;
    centity_t *ent;
    int oldnum;

    // find states to interpolate between
    ps = &frame->ps;
    ops = &oldframe->ps;

    // no lerping if previous frame was dropped or invalid
    if (!oldframe->valid)
        goto dup;

    oldnum = frame->number - framediv;
    if (oldframe->number != oldnum)
        goto dup;

    // no lerping if player entity was teleported (origin check)
    if (abs(ops->pmove.origin[0] - ps->pmove.origin[0]) > 256 * 8 ||
        abs(ops->pmove.origin[1] - ps->pmove.origin[1]) > 256 * 8 ||
        abs(ops->pmove.origin[2] - ps->pmove.origin[2]) > 256 * 8) {
        goto dup;
    }

    // no lerping if player entity was teleported (event check)
    ent = &cl_entities[frame->clientNum + 1];
    if (ent->serverframe > oldnum &&
        ent->serverframe <= frame->number &&
#if USE_FPS
        ent->event_frame > oldnum &&
        ent->event_frame <= frame->number &&
#endif
        (ent->current.event == EV_PLAYER_TELEPORT
         || ent->current.event == EV_OTHER_TELEPORT)) {
        goto dup;
    }

    // no lerping if teleport bit was flipped
    if ((ops->pmove.pm_flags ^ ps->pmove.pm_flags) & PMF_TELEPORT_BIT)
        goto dup;

    // no lerping if POV number changed
    if (oldframe->clientNum != frame->clientNum)
        goto dup;

    // developer option
    if (cl_nolerp->integer == 1)
        goto dup;

    return;

dup:
    // duplicate the current state so lerping doesn't hurt anything
    *ops = *ps;
}

/*
==================
CL_DeltaFrame

A valid frame has been parsed.
==================
*/
void CL_DeltaFrame(void)
{
    centity_t           *ent;
    entity_state_t      *state;
    int                 i, j;
    int                 framenum;
    int                 prevstate = cls.state;

    // getting a valid frame message ends the connection process
    if (cls.state == ca_precached)
        set_active_state();

    // Start of a new view weapon animation frame. Same reasoning as the alias
    // model animation below: the frame number steps at 10 Hz whatever the
    // server tick is, so the interpolation has to run on the animation's clock.
    if (cl.frame.ps.gunframe != cl.oldframe.ps.gunframe) {
        cl.gun_prev_frame = cl.oldframe.ps.gunframe;
        cl.gun_anim_start = cl.servertime;
    }

    // set server time
    framenum = cl.frame.number - cl.serverdelta;
    cl.servertime = framenum * CL_FRAMETIME;
#if USE_FPS
    cl.keyservertime = (framenum / cl.framediv) * BASE_FRAMETIME;
#endif

    // rebuild the list of solid entities for this frame
    cl.numSolidEntities = 0;

    // initialize position of the player's own entity from playerstate.
    // this is needed in situations when player entity is invisible, but
    // server sends an effect referencing it's origin (such as MZ_LOGIN, etc)
    ent = &cl_entities[cl.frame.clientNum + 1];
    Com_PlayerToEntityState(&cl.frame.ps, &ent->current);

    for (i = 0; i < cl.frame.numEntities; i++) {
        j = (cl.frame.firstEntity + i) & PARSE_ENTITIES_MASK;
        state = &cl.entityStates[j];

        // set current and prev
        parse_entity_update(state);

        // fire events
        parse_entity_event(state->number);
    }

    if (cls.demo.recording && !cls.demo.paused && !cls.demo.seeking && CL_FRAMESYNC) {
        CL_EmitDemoFrame();
    }

    if (prevstate == ca_precached)
        CL_GTV_Resume();
    else
        CL_GTV_EmitFrame();

    if (cls.demo.playback) {
        // this delta has nothing to do with local viewangles,
        // clear it to avoid interfering with demo freelook hack
        VectorClear(cl.frame.ps.pmove.delta_angles);
    }

    if (cl.oldframe.ps.pmove.pm_type != cl.frame.ps.pmove.pm_type) {
        IN_Activate();
    }

    check_player_lerp(&cl.oldframe, &cl.frame, 1);

#if USE_FPS
    if (CL_FRAMESYNC)
        check_player_lerp(&cl.oldkeyframe, &cl.keyframe, cl.framediv);
#endif

    CL_CheckPredictionError();

    SCR_SetCrosshairColor();
}

#if USE_DEBUG
// for debugging problems when out-of-date entity origin is referenced
void CL_CheckEntityPresent(int entnum, const char *what)
{
    centity_t *e;

    if (entnum == cl.frame.clientNum + 1) {
        return; // player entity = current
    }

    e = &cl_entities[entnum];
    if (e->serverframe == cl.frame.number) {
        return; // current
    }

    if (e->serverframe) {
        Com_LPrintf(PRINT_DEVELOPER,
                    "SERVER BUG: %s on entity %d last seen %d frames ago\n",
                    what, entnum, cl.frame.number - e->serverframe);
    } else {
        Com_LPrintf(PRINT_DEVELOPER,
                    "SERVER BUG: %s on entity %d never seen before\n",
                    what, entnum);
    }
}
#endif


/*
==========================================================================

INTERPOLATE BETWEEN FRAMES TO GET RENDERING PARMS

==========================================================================
*/

// Use a static entity ID on some things because the renderer relies on eid to match between meshes
// on the current and previous frames.
#define RESERVED_ENTITIY_GUN 1
#define RESERVED_ENTITIY_TESTMODEL 2
#define RESERVED_ENTITIY_COUNT 3
// A client-side effect entity that shadows a packet entity needs an id of
// its own, well clear of the packet ids (which are cent->id +
// RESERVED_ENTITIY_COUNT, counting up from 1 as entities are created).
#define RESERVED_ENTITIY_TRACKER_SHELL 0x20000000
// Half-extent of models/items/spawngro frame 2 (grow03), measured off the
// md2: x[-35.6,36.0] y[-35.7,35.6] z[-34.2,33.6].
#define TRACKER_SHELL_MODEL_RADIUS 36.03f

static int adjust_shell_fx(int renderfx)
{
	// PMM - at this point, all of the shells have been handled
	// if we're in the rogue pack, set up the custom mixing, otherwise just
	// keep going
	if (!strcmp(fs_game->string, "rogue")) {
		// all of the solo colors are fine.  we need to catch any of the combinations that look bad
		// (double & half) and turn them into the appropriate color, and make double/quad something special
		if (renderfx & RF_SHELL_HALF_DAM) {
			// ditch the half damage shell if any of red, blue, or double are on
			if (renderfx & (RF_SHELL_RED | RF_SHELL_BLUE | RF_SHELL_DOUBLE))
				renderfx &= ~RF_SHELL_HALF_DAM;
		}

		if (renderfx & RF_SHELL_DOUBLE) {
			// lose the yellow shell if we have a red, blue, or green shell
			if (renderfx & (RF_SHELL_RED | RF_SHELL_BLUE | RF_SHELL_GREEN))
				renderfx &= ~RF_SHELL_DOUBLE;
			// if we have a red shell, turn it to purple by adding blue
			if (renderfx & RF_SHELL_RED)
				renderfx |= RF_SHELL_BLUE;
			// if we have a blue shell (and not a red shell), turn it to cyan by adding green
			else if (renderfx & RF_SHELL_BLUE) {
				// go to green if it's on already, otherwise do cyan (flash green)
				if (renderfx & RF_SHELL_GREEN)
					renderfx &= ~RF_SHELL_BLUE;
				else
					renderfx |= RF_SHELL_GREEN;
			}
		}
	}

	return renderfx;
}

/*
===============
CL_AddPacketEntities

===============
*/
static void CL_AddPacketEntities(void)
{
    entity_t            ent;
    entity_state_t      *s1;
    float               autorotate;
    int                 i;
    int                 pnum;
    centity_t           *cent;
    int                 autoanim;
    clientinfo_t        *ci;
    unsigned int        effects, renderfx;

	
    // bonus items rotate at a fixed rate
    autorotate = anglemod(cl.time * 0.1f);

    // brush models can auto animate their frames
    autoanim = 2 * cl.time / 1000;
	
    memset(&ent, 0, sizeof(ent));

    for (pnum = 0; pnum < cl.frame.numEntities; pnum++) {
        i = (cl.frame.firstEntity + pnum) & PARSE_ENTITIES_MASK;
        s1 = &cl.entityStates[i];

        cent = &cl_entities[s1->number];
        ent.id = cent->id + RESERVED_ENTITIY_COUNT;

        // (ent) is memset once before this loop, not per entity, so anything
        // only some entity kinds assign leaks into every later entity of the
        // same frame. misc_flare sets ent.scale and brush models never do,
        // which let a flare's scale reach a func_door - create_entity_matrix()
        // then scaled the door about the world origin and threw it out of its
        // doorway, so it read as flickering out of existence.
        //
        // The rerelease's per-entity scale arrives in s1->scale (U_SCALE), so
        // this doubles as the assignment for it: 0 means unscaled, and
        // create_entity_matrix() already treats 0 as 1.0.
        ent.scale = s1->scale;
		
        effects = s1->effects;
        renderfx = s1->renderfx;	


	
		
		
		//if (effects == EF_GIBSCALE) {
		//	if (ent.scale == 0) {
		//		ent.scale = .25; //crandom();
		//	}
		//}
		
		
        // set frame
        if (effects & EF_ANIM01)
            ent.frame = autoanim & 1;
        else if (effects & EF_ANIM23)
            ent.frame = 2 + (autoanim & 1);
        else if (effects & EF_ANIM_ALL)
            ent.frame = autoanim;
        else if (effects & EF_ANIM_ALLFAST)
            ent.frame = cl.time / 100;
        else
            ent.frame = s1->frame;

        // quad and pent can do different things on client
        if (effects & EF_PENT) {
            effects &= ~EF_PENT;
            effects |= EF_COLOR_SHELL;
            renderfx |= RF_SHELL_RED;
        }

        if (effects & EF_QUAD) {
            effects &= ~EF_QUAD;
            effects |= EF_COLOR_SHELL;
            renderfx |= RF_SHELL_BLUE;
        }

        if (effects & EF_DOUBLE) {
            effects &= ~EF_DOUBLE;
            effects |= EF_COLOR_SHELL;
            renderfx |= RF_SHELL_DOUBLE;
        }

        if (effects & EF_HALF_DAMAGE) {
            effects &= ~EF_HALF_DAMAGE;
            effects |= EF_COLOR_SHELL;
            renderfx |= RF_SHELL_HALF_DAM;
        }

        // optionally remove the glowing effect
        if (cl_noglow->integer)
            renderfx &= ~RF_GLOW;

        ent.oldframe = cent->prev.frame;
        ent.backlerp = 1.0f - cl.lerpfrac;

        if (renderfx & RF_FRAMELERP) {
            // step origin discretely, because the frames
            // do the animation properly
            VectorCopy(cent->current.origin, ent.origin);
            VectorCopy(cent->current.old_origin, ent.oldorigin);  // FIXME
        } else if (renderfx & RF_BEAM) {
            // interpolate start and end points for beams
            LerpVector(cent->prev.origin, cent->current.origin,
                       cl.lerpfrac, ent.origin);
            LerpVector(cent->prev.old_origin, cent->current.old_origin,
                       cl.lerpfrac, ent.oldorigin);
        } else {
            if (s1->number == cl.frame.clientNum + 1) {
                // use predicted origin
                VectorCopy(cl.playerEntityOrigin, ent.origin);
                VectorCopy(cl.playerEntityOrigin, ent.oldorigin);
            } else {
                // interpolate origin
                LerpVector(cent->prev.origin, cent->current.origin,
                           cl.lerpfrac, ent.origin);
                VectorCopy(ent.origin, ent.oldorigin);
            }
            // Run alias model animation over its own interval rather than the
            // server's. They are the same thing at 10 Hz, which is why this is
            // skipped there and the plain cl.lerpfrac above stands. A rerelease
            // demo runs the server at 40 Hz while still stepping animation at
            // 10 Hz, so lerping across the 25 ms server tick would hold each
            // pose for three ticks and then snap through the fourth - the whole
            // scene animating in steps no matter how high the frame rate is.
            if (cl.frametime != BASE_FRAMETIME && cent->prev_frame != s1->frame) {
                int delta = cl.time - cent->anim_start;
                float frac;

                if (delta > BASE_FRAMETIME) {
                    cent->prev_frame = s1->frame;
                    frac = 1;
                } else if (delta > 0) {
                    frac = delta * BASE_1_FRAMETIME;
                } else {
                    frac = 0;
                }

                ent.oldframe = cent->prev_frame;
                ent.backlerp = 1.0f - frac;
            }
        }

        // rerelease target_light - a switchable dynamic light with no model.
        // s.frame is the radius, s.skinnum the packed RGBA. The map turns it
        // off by setting SVF_NOCLIENT, so simply being here means it is on.
        if (renderfx & RF_CUSTOM_LIGHT) {
            float r = ((s1->skinnum >> 24) & 0xff) / 255.0f;
            float g = ((s1->skinnum >> 16) & 0xff) / 255.0f;
            float b = ((s1->skinnum >>  8) & 0xff) / 255.0f;

            V_AddLight(ent.origin, s1->frame, r, g, b);
            goto skip;
        }

        // rerelease misc_flare - a camera-facing corona quad, drawn by the
        // path tracer's sprite pass. See SP_misc_flare for the state layout.
        if (renderfx & RF_FLARE) {
            vec3_t  delta;
            float   dist, fade_start, fade_end, alpha = 1.0f;

            // only the vkpt back end knows how to draw one
            if (cls.ref_type != REF_TYPE_VKPT || !cl_flares->integer)
                goto skip;

            VectorSubtract(ent.origin, cl.refdef.vieworg, delta);
            dist = VectorLength(delta);

            fade_start = s1->modelindex2 * FLARE_FADE_UNIT;
            fade_end = s1->modelindex3 * FLARE_FADE_UNIT;

            // The flare fades IN with distance. Every flare in every map has
            // fade_end_dist above fade_start_dist, and 55 of them set a start
            // with no end at all - which only makes sense this way round: the
            // maps use it to keep a big corona off the screen when the player
            // is standing right at the lamp.
            if (fade_end > fade_start)
                alpha = (dist - fade_start) / (fade_end - fade_start);
            else if (dist < fade_start)
                alpha = 0.0f;

            clamp(alpha, 0.0f, 1.0f);

            if (alpha <= 0.0f)
                goto skip;

            ent.model = 0;
            ent.skinnum = 0;
            ent.skin = 0;
            ent.frame = cl.image_precache[s1->frame & (MAX_IMAGES - 1)];
            ent.scale = s1->modelindex4 * FLARE_SCALE_UNIT;
            // the game packs "rgba" as R:G:B:A from the top byte down, and
            // the sprite shader unpacks it the same way, so pass it straight
            // through with the alpha byte forced opaque - the fade lives in
            // ent.alpha instead.
            ent.rgba.u32 = (s1->skinnum & 0xffffff00u) | 0xffu;
            ent.alpha = alpha * (((s1->skinnum) & 0xff) / 255.0f);
            ent.flags = renderfx;

            if (!ent.frame || ent.alpha <= 0.0f)
                goto skip;

            V_AddEntity(&ent);
            goto skip;
        }

        if ((effects & EF_GIB) && !cl_gibs->integer) {
            goto skip;
        }

        // create a new entity

        // tweak the color of beams
        if (renderfx & RF_BEAM) {
            // the four beam colors are encoded in 32 bits of skinnum (hack)
            ent.alpha = 0.30f;
            ent.skinnum = (s1->skinnum >> ((CL_FrameRand(s1->number) % 4) * 8)) & 0xff;
            ent.model = 0;
        } else {
            // set skin
            if (s1->modelindex == 255) {
                // use custom player skin
                ent.skinnum = 0;
                ci = &cl.clientinfo[s1->skinnum & 0xff];
                ent.skin = ci->skin;
                ent.model = ci->model;
                if (!ent.skin || !ent.model) {
                    ent.skin = cl.baseclientinfo.skin;
                    ent.model = cl.baseclientinfo.model;
                    ci = &cl.baseclientinfo;
                }
                if (renderfx & RF_USE_DISGUISE) {
                    char buffer[MAX_QPATH];

                    Q_concat(buffer, sizeof(buffer), "players/", ci->model_name, "/disguise.pcx");
                    ent.skin = R_RegisterSkin(buffer);
                }
            } else {
                ent.skinnum = s1->skinnum;
                ent.skin = 0;
				
                ent.model = cl.model_draw[s1->modelindex];
                if (ent.model == cl_mod_laser || ent.model == cl_mod_dmspot)
                    renderfx |= RF_NOSHADOW;
            }
        }

        // only used for black hole model right now, FIXME: do better
        if ((renderfx & RF_TRANSLUCENT) && !(renderfx & RF_BEAM))
            ent.alpha = 0.70f;

        // render effects (fullbright, translucent, etc)
        if ((effects & EF_COLOR_SHELL))
            ent.flags = renderfx & RF_FRAMELERP;    // renderfx go on color shell entity
        else
            ent.flags = renderfx;

        // calculate angles
        if (effects & EF_ROTATE) {  // some bonus items auto-rotate
            ent.angles[0] = 0;
            ent.angles[1] = autorotate;
            ent.angles[2] = 0;
        } else if (effects & EF_SPINNINGLIGHTS) {
            vec3_t forward;
            vec3_t start;

            ent.angles[0] = 0;
            ent.angles[1] = anglemod(cl.time / 2) + s1->angles[1];
            ent.angles[2] = 180;

            AngleVectors(ent.angles, forward, NULL, NULL);
            VectorMA(ent.origin, 64, forward, start);
            V_AddLight(start, 100, 1, 0, 0);
        } else if (s1->number == cl.frame.clientNum + 1) {
            VectorCopy(cl.playerEntityAngles, ent.angles);      // use predicted angles
        } else { // interpolate angles
            LerpAngles(cent->prev.angles, cent->current.angles,
                       cl.lerpfrac, ent.angles);

            // mimic original ref_gl "leaning" bug (uuugly!)
            if (s1->modelindex == 255 && cl_rollhack->integer) {
                ent.angles[ROLL] = -ent.angles[ROLL];
            }
        }

        int base_entity_flags = 0;

        if (s1->number == cl.frame.clientNum + 1) {
            if (effects & EF_FLAG1)
                V_AddLight(ent.origin, 225, 1.0f, 0.1f, 0.1f);
            else if (effects & EF_FLAG2)
                V_AddLight(ent.origin, 225, 0.1f, 0.1f, 1.0f);
            else if (effects & EF_TAGTRAIL)
                V_AddLight(ent.origin, 225, 1.0f, 1.0f, 0.0f);
            // [Q2RTX] no negative light here either - see the note further
            // down at the EF_TRACKERTRAIL trail branch. A light with negative
            // radiance removes energy from the whole room in a path tracer.
            else if (effects & EF_TRACKERTRAIL)
                (void)0;

			if (!cl.thirdPersonView)
			{
				if(cls.ref_type == REF_TYPE_VKPT)
					base_entity_flags |= RF_VIEWERMODEL;    // only draw from mirrors
				else
                goto skip;
            }

			// don't tilt the model - looks weird
			ent.angles[0] = 0.f;

			// offset the model back a bit to make the view point located in front of the head
			vec3_t angles = { 0.f, ent.angles[1], 0.f };
			vec3_t forward;
			AngleVectors(angles, forward, NULL, NULL);

			float offset = -15.f;
			VectorMA(ent.origin, offset, forward, ent.origin);
			VectorMA(ent.oldorigin, offset, forward, ent.oldorigin);
        }

        // if set to invisible, skip
        if (!s1->modelindex) {
            goto skip;
        }

        if (effects & EF_BFG) {
            ent.flags |= RF_TRANSLUCENT;
            ent.alpha = 0.30f;
        }

        if (effects & EF_PLASMA) {
            ent.flags |= RF_TRANSLUCENT;
            ent.alpha = 0.6f;
        }

        if (effects & EF_SPHERETRANS) {
            // [Q2RTX] The monster-spawn sphere and friends. A MODEL has no
            // BSP surface flags, so unlike the map's force fields - which are
            // transparent because the brush carries SURF_TRANS66 - the only
            // opacity lever here is this entity alpha, exposed as a cvar so it
            // can be tuned by eye at runtime.
            //
            // This only does anything if the model's material is a kind that
            // honours alpha, and the spawngro materials are NOT: they are
            // `kind WATER` + `curved_water`, which the renderer sends down the
            // GLASS path so the shell refracts and shows its own texture.
            // Alpha on a model is broken in its own right too - the split
            // rewrites the kind to TRANSP_MODEL, which is_transparent() does
            // not match, so the see-through field is never traced and the
            // model only darkens. Tune the shell with base_factor in
            // rerelease/materials/rerelease.mat instead.
            //
            // There used to be a x2 here for EF_TRACKERTRAIL (the disruptor
            // shell). With the clamp below that made every value from 0.5 up
            // fully opaque, i.e. half the slider did nothing - tuning noise
            // from when alpha was not being honoured at all. The cvar now means
            // what it says on both effects.
            ent.flags |= RF_TRANSLUCENT;
            ent.alpha = cl_spheretrans_alpha->value;
            if (ent.alpha < 0.02f)
                ent.alpha = 0.02f;
            else if (ent.alpha > 1.0f)
                ent.alpha = 1.0f;
        }

        // [rerelease] per-entity alpha (U_ALPHA). 0 is "unset", so an entity
        // that never touches it stays opaque; 1 is opaque too, which is what
        // the rerelease's fades reset to. Only the range between the two needs
        // RF_TRANSLUCENT, and it is set last on purpose so an explicit alpha
        // wins over the effects-driven values above. mgu5m1's red_field and
        // mgu5m3's twelve final_laser func_doors are the map-driven users.
        if (s1->alpha > 0.f && s1->alpha < 1.f) {
            ent.flags |= RF_TRANSLUCENT;
            ent.alpha = s1->alpha;
        }

        ent.flags |= base_entity_flags;

		// in rtx mode, the base entity has the renderfx for shells
		if ((effects & EF_COLOR_SHELL) && cls.ref_type == REF_TYPE_VKPT) {
			renderfx = adjust_shell_fx(renderfx);
			ent.flags |= renderfx;
		}

        // add to refresh list
        V_AddEntity(&ent);

		// add dlights for flares
		model_t* model;
		if (ent.model && !(ent.model & 0x80000000) &&
			(model = MOD_ForHandle(ent.model)))
		{
			if (model->model_class == MCLASS_FLARE)
			{
				float phase = (float)cl.time * 0.03f + (float)ent.id;
				float anim = sinf(phase);

				float offset = anim * 1.5f + 5.f;
				float brightness = anim * 0.2f + 0.8f;

				vec3_t origin;
				VectorCopy(ent.origin, origin);
				origin[2] += offset;

				V_AddSphereLight(origin, 500.f, 1.6f * brightness, 1.0f * brightness, 0.2f * brightness, 5.f);
                    }
                }

        // color shells generate a separate entity for the main model
        if ((effects & EF_COLOR_SHELL) && cls.ref_type != REF_TYPE_VKPT) {
			renderfx = adjust_shell_fx(renderfx);
            ent.flags = renderfx | RF_TRANSLUCENT | base_entity_flags;
            ent.alpha = 0.30f;
            V_AddEntity(&ent);
        }

        ent.skin = 0;       // never use a custom skin on others
        ent.skinnum = 0;
        ent.flags = base_entity_flags;
        ent.alpha = 0;

        // duplicate for linked models
        if (s1->modelindex2) {
            if (s1->modelindex2 == 255) {
                // custom weapon
                ci = &cl.clientinfo[s1->skinnum & 0xff];
                i = (s1->skinnum >> 8); // 0 is default weapon model
                if (i < 0 || i > cl.numWeaponModels - 1)
                    i = 0;
                ent.model = ci->weaponmodel[i];
                if (!ent.model) {
                    if (i != 0)
                        ent.model = ci->weaponmodel[0];
                    if (!ent.model)
                        ent.model = cl.baseclientinfo.weaponmodel[0];
                }
            } else
                ent.model = cl.model_draw[s1->modelindex2];

            // PMM - check for the defender sphere shell .. make it translucent
            if (!Q_strcasecmp(cl.configstrings[CS_MODELS + (s1->modelindex2)], "models/items/shell/tris.md2")) {
                ent.alpha = 0.32f;
                ent.flags = RF_TRANSLUCENT;
            }

			if ((effects & EF_COLOR_SHELL) && cls.ref_type == REF_TYPE_VKPT) {
				ent.flags |= renderfx;
			}

            V_AddEntity(&ent);

            //PGM - make sure these get reset.
            ent.flags = base_entity_flags;
            ent.alpha = 0;
        }

        if (s1->modelindex3) {
            ent.model = cl.model_draw[s1->modelindex3];
            V_AddEntity(&ent);
        }

        if (s1->modelindex4) {
            ent.model = cl.model_draw[s1->modelindex4];
            V_AddEntity(&ent);
        }

        if (effects & EF_POWERSCREEN) {
            ent.model = cl_mod_powerscreen;
            ent.oldframe = 0;
            ent.frame = 0;
            ent.flags |= (RF_TRANSLUCENT | RF_SHELL_GREEN);
            ent.alpha = 0.30f;
            V_AddEntity(&ent);
        }

        // rerelease: a barrel burning down its fuse. Handled here rather than in
        // the else-if chain below, which is for movement trails - this is a
        // stationary emitter and must not compete with them.
        if (effects & EF_BARREL_EXPLODING) {
            CL_BarrelBurnEffect(cent, ent.origin);
            V_AddLight(ent.origin, 100 + (CL_FrameRand(s1->number + 0x10000) % 60), 1.0f, 0.6f, 0.15f);
        }

        // add automatic particle trails
        if (effects & ~EF_ROTATE) {
            if (effects & EF_ROCKET) {
                if (!(cl_disable_particles->integer & NOPART_ROCKET_TRAIL)) {
                    CL_RocketTrail(cent->lerp_origin, ent.origin, cent);
                }
                if (cl_dlight_hacks->integer & DLHACK_ROCKET_COLOR)
                    V_AddLight(ent.origin, 200, 1, 0.23f, 0);
                else
                    V_AddLight(ent.origin, 200, 0.6f, 0.4f, 0.12f);
            } else if (effects & EF_BLASTER) {
                if (effects & EF_TRACKER) {
                    CL_BlasterTrail2(cent->lerp_origin, ent.origin);
                    V_AddLight(ent.origin, 200, 0.1f, 0.4f, 0.12f);
                } else {
                    CL_BlasterTrail(cent->lerp_origin, ent.origin);
                    if (cl_blaster_color->integer)
                        V_AddLight(ent.origin, 200, 0.9f, 0.7f, 0.9f);
                    else
                        V_AddLight(ent.origin, 200, 0.6f, 0.4f, 0.12f);
                }
            } else if (effects & EF_HYPERBLASTER) {
				if (effects & EF_TRACKER) {
					CL_BlasterTrail2(cent->lerp_origin, ent.origin);
					V_AddLight(ent.origin, 200, 0.1f, 0.4f, 0.12f);
				}
				else {
                    CL_HyoerBlasterTrail(cent->lerp_origin, ent.origin);

					V_AddLight(ent.origin, 200, 0.6f, 0.4f, 0.12f);
				}
            } else if (cl_ludicrous_gibs->integer && (effects == EF_GIB || effects == EF_GIBSCALE)) {
                // heavy blood trail: many more particles, and they linger
                CL_NonDiminishingTrail(cent->lerp_origin, ent.origin, cent, effects);
            } else if (effects & EF_GIB) {
                CL_DiminishingTrail(cent->lerp_origin, ent.origin, cent, effects);
            } else if (effects & EF_GRENADE) {
                if (!(cl_disable_particles->integer & NOPART_GRENADE_TRAIL)) {
                    CL_DiminishingTrail(cent->lerp_origin, ent.origin, cent, effects);
                }
            } else if (effects & EF_FLIES) {
                CL_FlyEffect(cent, ent.origin);
            } else if (effects & EF_BFG) {
                if (effects & EF_ANIM_ALLFAST) {
                    CL_BfgParticles(&ent);
                    i = 100;
                } else {
                    static const int bfg_lightramp[6] = {300, 400, 600, 300, 150, 75};
                    i = s1->frame;
                    clamp(i, 0, 5);
                    i = bfg_lightramp[i];
                }
				const vec3_t nvgreen = { 0.2716f, 0.5795f, 0.04615f };
				V_AddSphereLight(ent.origin, i, nvgreen[0], nvgreen[1], nvgreen[2], 20.f);
            } else if (effects & EF_TRAP) {
                ent.origin[2] += 32;
                CL_TrapParticles(cent, ent.origin);
                i = (CL_FrameRand(s1->number + 0x20000) % 100) + 100;
                V_AddLight(ent.origin, i, 1, 0.8f, 0.1f);
            } else if (effects & EF_FLAG1) {
                CL_FlagTrail(cent->lerp_origin, ent.origin, 242);
                V_AddLight(ent.origin, 225, 1, 0.1f, 0.1f);
            } else if (effects & EF_FLAG2) {
                CL_FlagTrail(cent->lerp_origin, ent.origin, 115);
                V_AddLight(ent.origin, 225, 0.1f, 0.1f, 1);
            } else if (effects & EF_TAGTRAIL) {
                CL_TagTrail(cent->lerp_origin, ent.origin, 220);
                V_AddLight(ent.origin, 225, 1.0f, 1.0f, 0.0f);
            // [Q2RTX] THE DISRUPTOR'S THREE NEGATIVE LIGHTS ARE GONE.
            //
            // rogue asked for V_AddLight(..., -1, -1, -1) here - a light with
            // NEGATIVE radiance. Under the software renderer that just
            // subtracted from a lightmap and darkened a small patch. A path
            // tracer has no such notion: the negative light is fed to
            // V_AddSphereLight as a real emitter and removes energy from
            // everything it reaches, so firing the disruptor blacked out half
            // the room. That is what made the effect look broken.
            //
            // The darkening is supposed to come from the SHELL PARTICLES, and
            // those already work: CL_Tracker_Shell spawns colour 0 (black) and
            // pt_logic_particle blends premultiplied, so a black particle
            // correctly darkens what is behind it.
            } else if (effects & EF_TRACKERTRAIL) {
                if (!(effects & EF_TRACKER)) {
                    // Wrap the shell around the BODY, not the origin. A
                    // monster's origin is at its feet, so the old fixed
                    // 40-unit sphere centred there sat half in the floor.
                    // cent->mins/maxs come from the packed solid, so this
                    // fits a flyer and a gladiator alike; if the box is
                    // empty (never sent), fall back to rogue's constant.
                    vec3_t  centre, size;
                    float   radius = 40.0f;

                    VectorCopy(cent->lerp_origin, centre);

                    VectorSubtract(cent->maxs, cent->mins, size);
                    if (size[0] > 0 || size[1] > 0 || size[2] > 0) {
                        centre[0] += (cent->mins[0] + cent->maxs[0]) * 0.5f;
                        centre[1] += (cent->mins[1] + cent->maxs[1]) * 0.5f;
                        centre[2] += (cent->mins[2] + cent->maxs[2]) * 0.5f;
                        radius = 0.5f * max(size[0], max(size[1], size[2]));
                    }

                    if (cl_tracker_bubble->integer && cl_mod_tracker_shell) {
                        // The spawngro sphere instead of rogue's particle
                        // cloud. Its material is the force-field recipe
                        // (kind WATER + curved_water), so the shell refracts
                        // the room and shows its own skin over it.
                        //
                        // A SEPARATE entity_t, zeroed here: `ent` is memset
                        // once before the packet loop, so writing a model or
                        // a scale into it would leak onto later entities in
                        // the same frame - the misc_flare bug noted above.
                        entity_t shell;

                        memset(&shell, 0, sizeof(shell));
                        shell.model = cl_mod_tracker_shell;
                        VectorCopy(centre, shell.origin);
                        VectorCopy(centre, shell.oldorigin);
                        // frame 2 is grow03, the fully inflated sphere. The
                        // grow animation belongs to the spawn effect, not here.
                        shell.frame = shell.oldframe = 2;
                        shell.backlerp = 0.0f;
                        shell.scale = (radius * cl_tracker_bubble_scale->value)
                                    / TRACKER_SHELL_MODEL_RADIUS;
                        // Its own id, or it would fight the victim it is
                        // wrapped around for a slot in the renderer's temporal
                        // history and ghost.
                        shell.id = RESERVED_ENTITIY_TRACKER_SHELL + cent->id;

                        V_AddEntity(&shell);
                    } else {
                        CL_Tracker_Shell(centre, radius);
                    }
                }
            } else if (effects & EF_TRACKER) {
                CL_TrackerTrail(cent->lerp_origin, ent.origin, 0);
            } else if (effects & EF_GREENGIB) {
                CL_DiminishingTrail(cent->lerp_origin, ent.origin, cent, effects);
            } else if (effects & EF_IONRIPPER) {
                CL_IonripperTrail(cent->lerp_origin, ent.origin);
                V_AddLight(ent.origin, 100, 1, 0.5f, 0.5f);
            } else if (effects & EF_BLUEHYPERBLASTER) {
                // See BLUEBLASTER_LIGHT_R in client.h - this was (0, 0, 1).
                V_AddLight(ent.origin, 200, BLUEBLASTER_LIGHT_R,
                           BLUEBLASTER_LIGHT_G, BLUEBLASTER_LIGHT_B);
            } else if (effects & EF_PLASMA) {
                if (effects & EF_ANIM_ALLFAST) {
                    CL_BlasterTrail(cent->lerp_origin, ent.origin);
                }
                V_AddLight(ent.origin, 130, 1, 0.5f, 0.5f);
            }
        }

skip:
        VectorCopy(ent.origin, cent->lerp_origin);
    }
}

static int shell_effect_hack(void)
{
    centity_t   *ent;
    int         flags = 0;

    if (cl.frame.clientNum == CLIENTNUM_NONE)
        return 0;

    ent = &cl_entities[cl.frame.clientNum + 1];
    if (ent->serverframe != cl.frame.number)
        return 0;

    if (!ent->current.modelindex)
        return 0;

    if (ent->current.effects & EF_PENT)
        flags |= RF_SHELL_RED;
    if (ent->current.effects & EF_QUAD)
        flags |= RF_SHELL_BLUE;
    if (ent->current.effects & EF_DOUBLE)
        flags |= RF_SHELL_DOUBLE;
    if (ent->current.effects & EF_HALF_DAMAGE)
        flags |= RF_SHELL_HALF_DAM;

    return flags;
}

void CL_AdjustGunPosition(vec3_t viewangles, vec3_t *gun_origin)
{
    vec3_t view_dir, right_dir, up_dir;
    vec3_t gun_real_pos, gun_tip;
    const float gun_length = 28.f;
    const float gun_right = 10.f;
    const float gun_up = -5.f;
    trace_t trace;
    static vec3_t mins = { -4, -4, -4 }, maxs = { 4, 4, 4 };

    AngleVectors(viewangles, view_dir, right_dir, up_dir);
    VectorMA(*gun_origin, gun_right, right_dir, gun_real_pos);
    VectorMA(gun_real_pos, gun_up, up_dir, gun_real_pos);
    VectorMA(gun_real_pos, gun_length, view_dir, gun_tip);

    CM_BoxTrace(&trace, gun_real_pos, gun_tip, mins, maxs, cl.bsp->nodes, MASK_SOLID);

    if (trace.fraction != 1.0f)
    {
        VectorMA(trace.endpos, -gun_length, view_dir, *gun_origin);
        VectorMA(*gun_origin, -gun_right, right_dir, *gun_origin);
        VectorMA(*gun_origin, -gun_up, up_dir, *gun_origin);
    }
}

/*
==============
CL_AddViewWeapon
==============
*/

/*
=================
View-weapon muzzle flash

The flash for the gun in your hands cannot simply be another entity tagged
RF_WEAPONMODEL: vkpt's entity sort tests RF_WEAPONMODEL *before* MCLASS_FLASH
(main.c ~2218), so it would be routed to the regular viewer-weapon pass and
render as a flat opaque slab instead of the soft additive flash. So it is
spawned as an ordinary world-space flash at the muzzle's world position, which
reuses the exact rendering path already tuned for monsters.

CL_MuzzleFlash only tells us that WE fired - it has no idea where the gun ended
up on screen, since the view model's position depends on bob, kick, FOV and the
wall-avoidance nudge. So it just raises a flag, and CL_AddViewWeapon consumes it
once it has the final gun transform.

The offsets below are CALIBRATED BY EYE. There is no table to copy: id's own
rerelease code has no visual muzzle data. Its P_ProjectSource offsets are
GAMEPLAY shot origins - machinegun, chaingun, shotgun and super shotgun are all
{0,0,-8}, i.e. just below the eye for hitscan accuracy - and the flash placement
itself lives in KEX's client, which is not in this repo.

An earlier attempt derived these from each gun's forward-most vertex. It agreed
to 1.5 units with the one real muzzle marker in the game (the MF placeholder in
v_machn's md3) and was still wrong - that only proved two vertices shared a
frame, not that it was the frame the view model is drawn in.

Calibrating showed gun->origin sits essentially AT THE CAMERA, so the whole
offset decides where the flash lands. Use cl_muzzleflash_offset "x y z" to dial
a weapon in live and read the numbers back. Axes are forward / LEFT / up - the
lateral term is applied against -right below, because model space here is
Y-left (AnglesToAxis inverts axis[1]).
=================
*/
typedef struct {
    const char  *gun;        // substring match against the view model's name
    vec3_t      offset;      // forward / LEFT / up, calibrated by eye
    const char  *flash_dir;  // which weapon's flash/ folder to draw; NULL = none
    qhandle_t   flash;       // resolved at precache; 0 = fall back to the star
} weapon_muzzle_t;

// EVERY WEAPON HAS ITS OWN FLASH GRAPHIC and they are not interchangeable.
// The geometry is the same disc for all of them (19 verts, 48 tris; the rocket
// launcher is the one exception with two), so the SHAPE lives entirely in the
// skin - and the skins are wildly different. Measured alpha coverage runs from
// 13.6% for the beamer to 70.8% for the rocket launcher: the machinegun's 25.6%
// is a four-point star, while the blaster's 67.7% is a soft round ball. Drawing
// one skin for all of them is visibly wrong, which is what we did until now.
//
// `flash_dir` is which weapon's flash/ folder to draw, because it is NOT always
// the weapon's own. Verified against the remaster's pak0.pak: only twelve
// weapons ship a flash/ folder, and **the chaingun and the hyperblaster are not
// among them** - so the rerelease cannot be drawing a per-weapon flash for
// those two either, and it borrows. The hyperblaster borrows the BLASTER's
// round ball (Matt's call, and it matches the rerelease exactly); the chaingun
// borrows the machinegun's star, which is the same family of weapon.
//
// The generic models/objects/flash was checked as the fallback and REJECTED:
// it is a four-frame sprite sheet with fully opaque alpha, nothing like the
// soft ball the rerelease draws.
//
// NULL would mean "no flash asset at all", falling back to the generic star.
//
// ORDER MATTERS: the lookup is a substring match, and "v_shotg" is a prefix of
// "v_shotg2" - so the super shotgun MUST be listed before the shotgun or it
// silently takes the shotgun's offset.
// Calibrated by Matt in game 2026-09-11, with the flash finally at a sane size -
// the earlier numbers were dialled against a flash so oversized it covered the
// muzzle wherever its centre sat, which hid how far off they were.
static weapon_muzzle_t cl_weapon_muzzles[] = {
    { "v_blast",   {  40.0f, -10.0f, -10.0f }, "v_blast"  },   // blaster - a soft ball
    { "v_shotg2",  {  40.0f, -13.0f, -10.0f }, "v_shotg2" },   // super shotgun
    { "v_shotg",   {  37.0f, -13.0f, -13.0f }, "v_shotg"  },   // shotgun
    { "v_machn",   {  37.0f, -13.0f, -10.0f }, "v_machn"  },   // machinegun - a star
    { "v_chain",   {  40.0f, -12.0f, -14.0f }, "v_machn"  },   // chaingun - borrows the star
    { "v_hyperb",  {  43.0f,  -9.0f, -10.0f }, "v_blast"  },   // hyperblaster - borrows the ball
};

#define MUZZLE_OFFSET_FILE  "muzzleoffsets"

/*
=================
CL_SaveMuzzleOffsets / CL_LoadMuzzleOffsets

The offsets are eye-calibrated, so they are worth real time to produce and must
survive quitting the game. They are NOT stored as cvars: that would put six more
entries into q2config.cfg, which on this project is exactly where per-map and
per-weapon values have leaked and followed people between games before. A file
of our own, parsed by hand, is the same thing the "light" command does.

The format is one weapon per line, "<key> forward left up", so it can also be
hand-edited. Comments are stripped here rather than by the tokenizer, which has
no idea what a comment is.
=================
*/
static void CL_SaveMuzzleOffsets(void)
{
    char        buffer[MAX_OSPATH];
    qhandle_t   f;
    int         i;

    f = FS_EasyOpenFile(buffer, sizeof(buffer), FS_MODE_WRITE | FS_FLAG_TEXT,
                        "", MUZZLE_OFFSET_FILE, ".cfg");
    if (!f) {
        Com_EPrintf("Couldn't write the muzzle offsets.\n");
        return;
    }

    FS_FPrintf(f, "// muzzle flash offsets, written by the \"muzzleoffset\" command\n");
    FS_FPrintf(f, "// weapon      forward     left       up\n");

    for (i = 0; i < q_countof(cl_weapon_muzzles); i++) {
        const weapon_muzzle_t *w = &cl_weapon_muzzles[i];

        FS_FPrintf(f, "%-12s %8.1f %8.1f %8.1f\n",
                   w->gun, w->offset[0], w->offset[1], w->offset[2]);
    }

    FS_CloseFile(f);
}

static void CL_LoadMuzzleOffsets(void)
{
    char    *buffer, *s, *p;
    int     ret;

    ret = FS_LoadFile(MUZZLE_OFFSET_FILE ".cfg", (void **)&buffer);
    if (!buffer) {
        // not having one yet is the normal case, not an error
        if (ret != Q_ERR(ENOENT))
            Com_EPrintf("Couldn't load %s.cfg: %s\n",
                        MUZZLE_OFFSET_FILE, Q_ErrorString(ret));
        return;
    }

    s = buffer;
    while (*s) {
        char    name[64];
        vec3_t  v;
        int     i;

        p = strchr(s, '\n');
        if (p)
            *p = 0;

        if (sscanf(s, "%63s %f %f %f", name, &v[0], &v[1], &v[2]) == 4 &&
            name[0] != '/') {
            for (i = 0; i < q_countof(cl_weapon_muzzles); i++) {
                if (!strcmp(name, cl_weapon_muzzles[i].gun)) {
                    VectorCopy(v, cl_weapon_muzzles[i].offset);
                    break;
                }
            }
        }

        if (!p)
            break;
        s = p + 1;
    }

    FS_FreeFile(buffer);
}

/*
=================
CL_MuzzleOffset_f

Dial the per-weapon muzzle offsets live, because they CANNOT be derived. Three
separate attempts to compute them from the models failed - there is no muzzle
data in the pak, the kpf, or id's source, and the proof it is underivable is
that the machinegun and super shotgun want the SAME offset while their model
muzzles are 12 units apart. They are eye-calibrated numbers and nothing else.

    muzzleoffset                 list every weapon, ready to paste back
    muzzleoffset <x> <y> <z>     set the weapon currently in your hands

Axes are forward / LEFT / up, and changes take effect on the next shot. Every
set is written straight to muzzleoffsets.cfg and read back at startup, so a
tuning session survives quitting the game. The listing is still the way to get
the numbers into cl_weapon_muzzles[] permanently - the file is for keeping work
in progress, the table is for shipping it.
=================
*/
/*
=================
CL_GunModel

ps->gunindex comes off the wire as a 16-bit word - see the PS_WEAPONINDEX branch
of MSG_ParseDeltaPlayerstate, which reads it with MSG_ReadWord and clamps
nothing - but cl.model_draw has MAX_MODELS (512) entries. Any index above that
reads PAST the array and returns whatever field of client_state_t follows it,
reinterpreted as a qhandle_t. MOD_ForHandle then dies on a handle that
corresponds to no model anywhere in the map, which is why the failure names the
model system and points nowhere near the actual bug.

Rejected rather than clamped on purpose: an out-of-range gunindex means the
client and server disagree about the model list, and quietly drawing model 511
instead would turn a desync into a rendering oddity nobody traces back.
=================
*/
static qhandle_t CL_GunModel(int gunindex)
{
    if (gunindex < 0 || gunindex >= MAX_MODELS) {
        Com_WPrintf("%s: gunindex %d out of range [0, %d)\n",
                    __func__, gunindex, MAX_MODELS);
        return 0;
    }

    return cl.model_draw[gunindex];
}

void CL_MuzzleOffset_f(void)
{
    const model_t   *model;
    player_state_t  *ps;
    int             i, argc = Cmd_Argc();

    if (argc != 1 && argc != 4) {
        Com_Printf("Usage: muzzleoffset [x y z]   (forward / left / up)\n");
        return;
    }

    if (argc == 1) {
        if (cl_muzzleflash_offset->string[0])
            Com_Printf("WARNING: cl_muzzleflash_offset is set to \"%s\", which "
                       "overrides EVERY weapon below. Clear it to see these.\n",
                       cl_muzzleflash_offset->string);
        Com_Printf("muzzle offsets, in cl_weapon_muzzles[] order:\n");
        for (i = 0; i < q_countof(cl_weapon_muzzles); i++) {
            const weapon_muzzle_t *w = &cl_weapon_muzzles[i];
            Com_Printf("    { \"%s\",%*s{ %6.1ff, %6.1ff, %6.1ff }, \"%s\" },\n",
                       w->gun, (int)(9 - strlen(w->gun)), "",
                       w->offset[0], w->offset[1], w->offset[2],
                       w->flash_dir ? w->flash_dir : "");
        }
        return;
    }

    // which weapon is in hand right now
    ps = CL_KEYPS;
    model = MOD_ForHandle(CL_GunModel(ps->gunindex));
    if (!model) {
        Com_Printf("No view weapon to set an offset for.\n");
        return;
    }

    for (i = 0; i < q_countof(cl_weapon_muzzles); i++) {
        if (strstr(model->name, cl_weapon_muzzles[i].gun)) {
            cl_weapon_muzzles[i].offset[0] = atof(Cmd_Argv(1));
            cl_weapon_muzzles[i].offset[1] = atof(Cmd_Argv(2));
            cl_weapon_muzzles[i].offset[2] = atof(Cmd_Argv(3));
            // The old global dev cvar would override what we just set, for
            // this and every other weapon, which reads as the command being
            // broken. It is superseded by this command, so drop it.
            if (cl_muzzleflash_offset->string[0]) {
                Cvar_Set("cl_muzzleflash_offset", "");
                Com_Printf("(cleared cl_muzzleflash_offset, which was "
                           "overriding every weapon)\n");
            }

            CL_SaveMuzzleOffsets();
            Com_Printf("%s muzzle offset now %.1f %.1f %.1f - saved. "
                       "Type muzzleoffset with no arguments to list them all.\n",
                       cl_weapon_muzzles[i].gun,
                       cl_weapon_muzzles[i].offset[0],
                       cl_weapon_muzzles[i].offset[1],
                       cl_weapon_muzzles[i].offset[2]);
            return;
        }
    }

    Com_Printf("'%s' has no muzzle flash entry.\n", model->name);
}

/*
=================
CL_RegisterViewMuzzleFlashes

Precache the per-weapon flash models, from CL_RegisterTEntModels. They have to
be registered there rather than looked up when the shot is fired: handles are
invalidated by R_EndRegistration at every map load, and registering mid-game
would stall on the first shot with each weapon while the model loads.
=================
*/
void CL_RegisterViewMuzzleFlashes(void)
{
    static bool loaded;
    int i;

    // Once per run, not once per map: this must not stomp offsets that are
    // being dialled in during the session.
    if (!loaded) {
        loaded = true;
        CL_LoadMuzzleOffsets();
    }

    for (i = 0; i < q_countof(cl_weapon_muzzles); i++) {
        char path[MAX_QPATH];

        cl_weapon_muzzles[i].flash = 0;
        if (!cl_weapon_muzzles[i].flash_dir)
            continue;

        Q_snprintf(path, sizeof(path), "models/weapons/%s/flash/tris.md2",
                   cl_weapon_muzzles[i].flash_dir);
        cl_weapon_muzzles[i].flash = R_RegisterModel(path);
    }
}

// Weapons deliberately absent from that table get NO muzzle flash, which is
// the lookup's natural behaviour. The railgun and BFG are excluded on purpose -
// neither should have one. The rocket and grenade launchers are out simply
// because they have not been calibrated; add a row once one is measured.

// A TIME, not a bool. CL_AddViewWeapon has several early returns - the gun
// hidden (hand 2), the player model disabled, no gun model yet - and on any of
// them CL_AddViewWeaponFlash is never reached, so a plain flag would sit set
// until the gun came back and then pop a flash in mid-air. Stamping the time
// lets a flash nobody could draw expire on its own. 0 = nothing pending.
static int  cl_view_flash_time;
static float cl_view_flash_roll;

#define VIEW_FLASH_WINDOW   100     // ms; a flash older than this is stale

// called from CL_MuzzleFlash when the flash belongs to us, in first person
void CL_ViewMuzzleFlash(void)
{
    cl_view_flash_time = cl.time;
    // one roll for the whole life of this flash - re-rolling every frame would
    // make the starburst spin instead of sit on the barrel
    cl_view_flash_roll = frand() * 360.0f;
}

// The flash on your own PLAYER MODEL's gun: the only one in third person, and
// the mirror-only twin of the view flash in first person. Stamped the same way
// as the view flash, and for the same reason - it is rebuilt every frame from
// the predicted player transform instead of being spawned into the world once.
// A spawned one sat where you WERE: your own entity's pl->current origin is
// the lagging playerstate, and you kept moving for the flash's whole life.
static int  cl_player_flash_time;
static float cl_player_flash_roll;

// called from CL_MuzzleFlash when the flash belongs to us, in either view
void CL_PlayerModelMuzzleFlash(void)
{
    cl_player_flash_time = cl.time;
    cl_player_flash_roll = frand() * 360.0f;
}

// Runs after CL_AddPacketEntities, so cl.playerEntityOrigin/Angles and
// cl.thirdPersonView all describe the frame being drawn.
void CL_AddPlayerModelFlash(void)
{
    vec3_t  muzzle, yaw_only, f, r, u;
    entity_t ent;

    // same staleness rules as the view flash, including cl.time going backwards
    if (!cl_player_flash_time || cl.time < cl_player_flash_time ||
        cl.time - cl_player_flash_time >= Cvar_ClampValue(cl_muzzleflash_time, 10, 200)) {
        cl_player_flash_time = 0;
        return;
    }

    if (!cl_muzzleflash_models->integer || !cl_mod_muzzleflash)
        return;

    // Placed relative to the model AS DRAWN: CL_AddPacketEntities draws your
    // own model at the predicted origin, yaw only, slid back 15 units so the
    // view point sits in front of its head. The three cvars are where the
    // player model holds its gun relative to that - a thing to look at and
    // nudge, not to derive.
    //
    // Yaw only, and from the prediction: the flash model grows along +X, and
    // the server's copy of your angles is what once had it pointing back at
    // you.
    VectorSet(yaw_only, 0, cl.playerEntityAngles[YAW], 0);
    AngleVectors(yaw_only, f, r, u);

    VectorCopy(cl.playerEntityOrigin, muzzle);
    VectorMA(muzzle, -15.0f + cl_muzzleflash_world_fwd->value, f, muzzle);
    VectorMA(muzzle, cl_muzzleflash_world_right->value, r, muzzle);
    VectorMA(muzzle, cl_muzzleflash_world_up->value, u, muzzle);

    // In third person the model is in plain view, so the flash is too. In
    // first person the model is mirror-only and so is its flash - the view
    // flash covers the camera.
    CL_SetupMuzzleFlashEntity(&ent, muzzle, yaw_only, cl_player_flash_roll,
                              cl.thirdPersonView ? 0 : RF_REFLECTION_FX,
                              cl_mod_muzzleflash);
    V_AddEntity(&ent);
}

static void CL_AddViewWeaponFlash(const entity_t *gun)
{
    const model_t   *model;
    const vec3_t    *ofs = NULL;
    qhandle_t       flash = 0;
    vec3_t          forward, right, up, muzzle;
    static vec3_t   tuned_storage;
    int             i;

    /* THE FLASH HAS TO FOLLOW THE GUN, so this runs EVERY frame of its life
       rather than spawning something once.

       It used to allocate an ordinary world-space explosion at the muzzle and
       leave it there. But the gun is drawn relative to the camera and keeps
       moving: running forward at Quake II's 300 units/sec carries it 15 units
       in the flash's 50 ms, against a muzzle offset of only ~37-40 units. The
       barrel overtook the flash and it surfaced BEHIND the gun.

       Rebuilding it here from the gun transform we were just handed keeps the
       two in sync by construction - there is no stored position to go stale,
       and it does not matter whether CL_AddExplosions runs before or after the
       view weapon. It is also why this is not an explosion any more: the view
       flash is the one effect whose position is not a fact about the world. */
    /* cl.time restarts at zero on every map change, demo start and reconnect
       (CL_ClearState), but this stamp is a file static that survives all of
       them. A flash still pending when that happens left cl.time - stamp
       deeply NEGATIVE, which is less than any window, so the flash never went
       stale and sat welded to the barrel from the moment you spawned until
       cl.time climbed back past where the old session had got to.

       Time running backwards means the stamp belongs to a world that no longer
       exists, so treat it as stale rather than as "not expired yet". */
    if (!cl_view_flash_time || cl.time < cl_view_flash_time ||
        cl.time - cl_view_flash_time >= Cvar_ClampValue(cl_muzzleflash_time, 10, 200)) {
        cl_view_flash_time = 0;
        return;
    }

    if (!cl_muzzleflash_models->integer)
        return;

    model = MOD_ForHandle(gun->model);
    if (!model)
        return;

    for (i = 0; i < q_countof(cl_weapon_muzzles); i++) {
        if (strstr(model->name, cl_weapon_muzzles[i].gun)) {
            ofs = &cl_weapon_muzzles[i].offset;
            flash = cl_weapon_muzzles[i].flash;      // 0 = fall back to the star
            break;
        }
    }
    if (!ofs)
        return;     // an unknown weapon - better no flash than one in mid-air

    // cl_muzzleflash_offset overrides the table, for dialling a weapon in live:
    //    cl_muzzleflash_offset "22 -5 -23"
    // Set it empty again to go back to the table.
    if (cl_muzzleflash_offset->string[0]) {
        vec3_t tuned;
        if (sscanf(cl_muzzleflash_offset->string, "%f %f %f",
                   &tuned[0], &tuned[1], &tuned[2]) == 3)
            ofs = (const vec3_t *)&tuned_storage, VectorCopy(tuned, tuned_storage);
    }

    // The gun's own axes, so the offset rides along with bob and kick.
    // NOTE the minus on the lateral term: model space here is X forward,
    // Y LEFT, Z up - AnglesToAxis (shared.h) inverts axis[1] - so a model
    // coordinate must be applied against -right, not +right.
    AngleVectors(gun->angles, forward, right, up);
    VectorCopy(gun->origin, muzzle);
    VectorMA(muzzle,  (*ofs)[0], forward, muzzle);
    VectorMA(muzzle, -(*ofs)[1], right,   muzzle);
    VectorMA(muzzle,  (*ofs)[2], up,      muzzle);

    // RF_FIRST_PERSON_FX keeps this copy out of reflections - the world twin
    // spawned on the player model is the one a mirror should see. `flash` is
    // this weapon's own graphic, which is the difference between a blaster
    // ball and a machinegun star; 0 falls back to the generic star.
    if (!flash)
        flash = cl_mod_muzzleflash;
    if (flash) {
        entity_t ent;

        CL_SetupMuzzleFlashEntity(&ent, muzzle, gun->angles, cl_view_flash_roll,
                                  RF_FIRST_PERSON_FX, flash);
        V_AddEntity(&ent);
    }
}

// [Q2RTX] The view weapon's FINAL transform, republished every frame for
// anything that has to ride the gun rather than the view.
//
// Everything the gun does that the view does not is baked in here and nowhere
// else: the gunangles sway (which the game code drives off your angular
// velocity, so it swings hardest exactly when you flick), the weapon kick
// added by CL_SetupFirstPersonView, CL_AdjustGunPosition's pullback out of
// walls, and the high-fov nudge. Anything that rebuilds its own "near the gun"
// position from cl.refdef.vieworg and cl.v_forward gets none of it and comes
// apart from the model whenever one of them moves - which is what the plasma
// beam was doing.
//
// Stamped with cls.framecount because CL_AddViewWeapon has several early
// returns (gun hidden, no player model, no gun model yet); a caller must be
// able to tell "the gun is over here" from "there is no gun this frame".
static vec3_t   cl_view_gun_origin;
static vec3_t   cl_view_gun_angles;
static int      cl_view_gun_framecount = -1;

bool CL_GetViewWeaponTransform(vec3_t origin, vec3_t angles)
{
    if (cl_view_gun_framecount != cls.framecount)
        return false;

    VectorCopy(cl_view_gun_origin, origin);
    VectorCopy(cl_view_gun_angles, angles);
    return true;
}

static void CL_AddViewWeapon(void)
{
    player_state_t *ps, *ops;
    entity_t    gun;        // view model
    int         i, shell_flags;

    // allow the gun to be completely removed
    if (cl_player_model->integer == CL_PLAYER_MODEL_DISABLED) {
        return;
    }

    if (info_hand->integer == 2) {
        return;
    }

    // find states to interpolate between
    ps = CL_KEYPS;
    ops = CL_OLDKEYPS;

    memset(&gun, 0, sizeof(gun));

    if (gun_model) {
        gun.model = gun_model;  // development tool
    } else {
        gun.model = CL_GunModel(ps->gunindex);
    }
    if (!gun.model) {
        return;
    }

	gun.id = RESERVED_ENTITIY_GUN;

    // set up gun position
    for (i = 0; i < 3; i++) {
        gun.origin[i] = cl.refdef.vieworg[i] + ops->gunoffset[i] +
                        CL_KEYLERPFRAC * (ps->gunoffset[i] - ops->gunoffset[i]);
        gun.angles[i] = cl.refdef.viewangles[i] + LerpAngle(ops->gunangles[i],
                        ps->gunangles[i], CL_KEYLERPFRAC);
    }

    // adjust for high fov
    if (ps->fov > 90) {
        vec_t ofs = (90 - ps->fov) * 0.2f;
        VectorMA(gun.origin, ofs, cl.v_forward, gun.origin);
    }

    // adjust the gun origin so that the gun doesn't intersect with walls
    CL_AdjustGunPosition(cl.refdef.viewangles, &gun.origin);

    VectorCopy(gun.origin, gun.oldorigin);      // don't lerp at all

    if (gun_frame) {
        gun.frame = gun_frame;  // development tool
        gun.oldframe = gun_frame;   // development tool
    } else {
        gun.frame = ps->gunframe;
        if (gun.frame == 0) {
            gun.oldframe = 0;   // just changed weapons, don't lerp from old
        } else {
            gun.oldframe = ops->gunframe;
            gun.backlerp = 1.0f - CL_KEYLERPFRAC;

            // Interpolate over the animation interval, not the server one.
            // Identical at 10 Hz; a rerelease demo ticks at 40 Hz while still
            // stepping the gun one frame every 100 ms, so lerping across the
            // 25 ms tick leaves the weapon holding each pose for three ticks
            // and snapping through the fourth.
            if (cl.frametime != BASE_FRAMETIME) {
                int delta = cl.time - cl.gun_anim_start;
                float frac;

                if (delta >= BASE_FRAMETIME)
                    frac = 1;
                else if (delta > 0)
                    frac = delta * BASE_1_FRAMETIME;
                else
                    frac = 0;

                gun.oldframe = cl.gun_prev_frame;
                gun.backlerp = 1.0f - frac;
            }
        }
    }

    gun.flags = RF_MINLIGHT | RF_DEPTHHACK | RF_WEAPONMODEL;
    if (info_hand->integer == 1) {
        gun.flags |= RF_LEFTHAND;
    }

    if (cl_gunalpha->value != 1) {
        gun.alpha = Cvar_ClampValue(cl_gunalpha, 0.1f, 1.0f);
        gun.flags |= RF_TRANSLUCENT;
    }

	// add shell effect from player entity
	shell_flags = shell_effect_hack();

	// same entity in rtx mode
	if (cls.ref_type == REF_TYPE_VKPT) {
		gun.flags |= shell_flags;
	}

	model_t* model = MOD_ForHandle(gun.model);
	if (model && strstr(model->name, "v_flareg"))
		gun.scale = 0.3f;

    // [Q2RTX] Publish it before drawing, so the beam can hang off the barrel.
    VectorCopy(gun.origin, cl_view_gun_origin);
    VectorCopy(gun.angles, cl_view_gun_angles);
    cl_view_gun_framecount = cls.framecount;

    V_AddEntity(&gun);

    CL_AddViewWeaponFlash(&gun);

	// separate entity in non-rtx mode
    if (shell_flags && cls.ref_type != REF_TYPE_VKPT) {
        gun.alpha = 0.30f * cl_gunalpha->value;
        gun.flags |= shell_flags | RF_TRANSLUCENT;
        V_AddEntity(&gun);
    }
}

static void CL_SetupFirstPersonView(void)
{
    player_state_t *ps, *ops;
    vec3_t kickangles;
    float lerp;

    // add kick angles
    if (cl_kickangles->integer) {
        ps = CL_KEYPS;
        ops = CL_OLDKEYPS;

        lerp = CL_KEYLERPFRAC;

        LerpAngles(ops->kick_angles, ps->kick_angles, lerp, kickangles);
        VectorAdd(cl.refdef.viewangles, kickangles, cl.refdef.viewangles);
    }

    // add the weapon
    CL_AddViewWeapon();

    cl.thirdPersonView = false;
}

/*
===============
CL_SetupThirdPersionView
===============
*/
static void CL_SetupThirdPersionView(void)
{
    vec3_t focus;
    float fscale, rscale;
    float dist, angle, range;
    trace_t trace;
    static const vec3_t mins = { -4, -4, -4 }, maxs = { 4, 4, 4 };

    // if dead, set a nice view angle
    if (cl.frame.ps.stats[STAT_HEALTH] <= 0) {
        cl.refdef.viewangles[ROLL] = 0;
        cl.refdef.viewangles[PITCH] = 10;
    }

    VectorMA(cl.refdef.vieworg, 512, cl.v_forward, focus);

    cl.refdef.vieworg[2] += 8;

    cl.refdef.viewangles[PITCH] *= 0.5f;
    AngleVectors(cl.refdef.viewangles, cl.v_forward, cl.v_right, cl.v_up);

    angle = DEG2RAD(cl_thirdperson_angle->value);
    range = cl_thirdperson_range->value;
    fscale = cos(angle);
    rscale = sin(angle);
    VectorMA(cl.refdef.vieworg, -range * fscale, cl.v_forward, cl.refdef.vieworg);
    VectorMA(cl.refdef.vieworg, -range * rscale, cl.v_right, cl.refdef.vieworg);

    CM_BoxTrace(&trace, cl.playerEntityOrigin, cl.refdef.vieworg,
                mins, maxs, cl.bsp->nodes, MASK_SOLID);
    if (trace.fraction != 1.0f) {
        VectorCopy(trace.endpos, cl.refdef.vieworg);
    }

    VectorSubtract(focus, cl.refdef.vieworg, focus);
    dist = sqrtf(focus[0] * focus[0] + focus[1] * focus[1]);

    cl.refdef.viewangles[PITCH] = -RAD2DEG(atan2(focus[2], dist));
    cl.refdef.viewangles[YAW] -= cl_thirdperson_angle->value;

    cl.thirdPersonView = true;
}

static void CL_FinishViewValues(void)
{
    centity_t *ent;

    if (cl_player_model->integer != CL_PLAYER_MODEL_THIRD_PERSON)
        goto first;

    if (cl.frame.clientNum == CLIENTNUM_NONE)
        goto first;

    ent = &cl_entities[cl.frame.clientNum + 1];
    if (ent->serverframe != cl.frame.number)
        goto first;

    if (!ent->current.modelindex)
        goto first;

    CL_SetupThirdPersionView();
    return;

first:
    CL_SetupFirstPersonView();
}

#if USE_SMOOTH_DELTA_ANGLES
static inline float LerpShort(int a2, int a1, float frac)
{
    if (a1 - a2 > 32768)
        a1 &= 65536;
    if (a2 - a1 > 32768)
        a1 &= 65536;
    return a2 + frac * (a1 - a2);
}
#endif

static inline float lerp_client_fov(float ofov, float nfov, float lerp)
{
    if (cls.demo.playback) {
        int fov = info_fov->integer;

        if (fov < 1)
            fov = 90;
        else if (fov > 160)
            fov = 160;

        if (info_uf->integer & UF_LOCALFOV)
            return fov;

        if (!(info_uf->integer & UF_PLAYERFOV)) {
            if (ofov >= 90)
                ofov = fov;
            if (nfov >= 90)
                nfov = fov;
        }
    }

    return ofov + lerp * (nfov - ofov);
}

/*
===============
CL_CalcViewValues

Sets cl.refdef view values and sound spatialization params.
Usually called from CL_AddEntities, but may be directly called from the main
loop if rendering is disabled but sound is running.
===============
*/
void CL_CalcViewValues(void)
{
    player_state_t *ps, *ops;
    vec3_t viewoffset;
    float lerp;

    if (!cl.frame.valid) {
        return;
    }

    // find states to interpolate between
    ps = &cl.frame.ps;
    ops = &cl.oldframe.ps;

    lerp = cl.lerpfrac;

    // calculate the origin
    if (!cls.demo.playback && cl_predict->integer && !(ps->pmove.pm_flags & PMF_NO_PREDICTION)) {
        // use predicted values
        unsigned delta = cls.realtime - cl.predicted_step_time;
        float backlerp = lerp - 1.0f;

        VectorMA(cl.predicted_origin, backlerp, cl.prediction_error, cl.refdef.vieworg);

        // smooth out stair climbing
        if (cl.predicted_step < 127 * 0.125f) {
            delta <<= 1; // small steps
        }
        if (delta < 100) {
            cl.refdef.vieworg[2] -= cl.predicted_step * (100 - delta) * 0.01f;
        }
    } else {
        int i;

        // just use interpolated values
        for (i = 0; i < 3; i++) {
            cl.refdef.vieworg[i] = SHORT2COORD(ops->pmove.origin[i] +
                lerp * (ps->pmove.origin[i] - ops->pmove.origin[i]));
        }
    }

    // if not running a demo or on a locked frame, add the local angle movement
    if (cls.demo.playback) {
        LerpAngles(ops->viewangles, ps->viewangles, lerp, cl.refdef.viewangles);
    } else if (ps->pmove.pm_type < PM_DEAD) {
        // use predicted values
        VectorCopy(cl.predicted_angles, cl.refdef.viewangles);
    } else if (ops->pmove.pm_type < PM_DEAD && cls.serverProtocol > PROTOCOL_VERSION_DEFAULT) {
        // lerp from predicted angles, since enhanced servers
        // do not send viewangles each frame
        LerpAngles(cl.predicted_angles, ps->viewangles, lerp, cl.refdef.viewangles);
    } else {
        // just use interpolated values
        LerpAngles(ops->viewangles, ps->viewangles, lerp, cl.refdef.viewangles);
    }

#if USE_SMOOTH_DELTA_ANGLES
    cl.delta_angles[0] = LerpShort(ops->pmove.delta_angles[0], ps->pmove.delta_angles[0], lerp);
    cl.delta_angles[1] = LerpShort(ops->pmove.delta_angles[1], ps->pmove.delta_angles[1], lerp);
    cl.delta_angles[2] = LerpShort(ops->pmove.delta_angles[2], ps->pmove.delta_angles[2], lerp);
#endif

    // don't interpolate blend color
    Vector4Copy(ps->blend, cl.refdef.blend);

#if USE_FPS
    ps = &cl.keyframe.ps;
    ops = &cl.oldkeyframe.ps;

    lerp = cl.keylerpfrac;
#endif

    // interpolate field of view
    cl.fov_x = lerp_client_fov(ops->fov, ps->fov, lerp);
    cl.fov_y = V_CalcFov(cl.fov_x, 4, 3);

    LerpVector(ops->viewoffset, ps->viewoffset, lerp, viewoffset);

    AngleVectors(cl.refdef.viewangles, cl.v_forward, cl.v_right, cl.v_up);

    VectorCopy(cl.refdef.vieworg, cl.playerEntityOrigin);
    VectorCopy(cl.refdef.viewangles, cl.playerEntityAngles);

    if (cl.playerEntityAngles[PITCH] > 180) {
        cl.playerEntityAngles[PITCH] -= 360;
    }

    cl.playerEntityAngles[PITCH] = cl.playerEntityAngles[PITCH] / 3;

    VectorAdd(cl.refdef.vieworg, viewoffset, cl.refdef.vieworg);

    VectorCopy(cl.refdef.vieworg, listener_origin);
    VectorCopy(cl.v_forward, listener_forward);
    VectorCopy(cl.v_right, listener_right);
    VectorCopy(cl.v_up, listener_up);
}

void CL_AddTestModel(void)
{
    static float frame = 0.f;
    static int prevtime = 0;

    if (cl_testmodel_handle != -1)
    {
        model_t* model = MOD_ForHandle(cl_testmodel_handle);

        if (model != NULL && model->meshes != NULL)
        {
            entity_t entity = { 0 };
            entity.model = cl_testmodel_handle;
            entity.id = RESERVED_ENTITIY_TESTMODEL;

        	VectorCopy(cl_testmodel_position, entity.origin);
            VectorCopy(cl_testmodel_position, entity.oldorigin);

            entity.alpha = cl_testalpha->value;
            clamp(entity.alpha, 0.f, 1.f);
            if (entity.alpha < 1.f)
                entity.flags |= RF_TRANSLUCENT;

            int numframes = model->numframes;
            if (model->iqmData)
                numframes = (int)model->iqmData->num_poses;

            if (numframes > 1 && prevtime != 0)
            {
                const float millisecond = 1e-3f;

                int timediff = cl.time - prevtime;
                frame += (float)timediff * millisecond * max(cl_testfps->value, 0.f);

                if (frame >= (float)numframes || frame < 0.f)
                    frame = 0.f;

                float frac = frame - floorf(frame);

                entity.oldframe = (int)frame;
                entity.frame = entity.oldframe + 1;
                entity.backlerp = 1.f - frac;
            }

            prevtime = cl.time;

            V_AddEntity(&entity);
        }
    }
}

/*
===============
CL_FrameRand

Q_rand() for effects that re-roll EVERY RENDER FRAME rather than once per
event - lightning segment rolls, beam colour, flickering fuse and trap lights.

While the game is paused those draws kept changing although nothing else did,
so photo mode (accumulation rendering) averaged every variant together: a
lightning bolt converged to a wide smeared band, and a flickering light to its
mean. cl.time stops while sv_paused, so hashing it with a per-site salt gives a
value that is random-looking across sites and identical from frame to frame
for as long as the pause lasts. The player's own beam already did the
equivalent (CL_EmitBeamChain rolls by cl.time).

Unpaused it is plain Q_rand(), so gameplay is unchanged. Not a general
replacement: the renderer draws its photo-mode jitter from Q_rand too
(shadow_map.c), and that must keep varying.
===============
*/
uint32_t CL_FrameRand(uint32_t salt)
{
    if (!sv_paused->integer)
        return Q_rand();

    uint32_t h = (uint32_t)cl.time * 0x9E3779B1u ^ salt * 0x85EBCA77u;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

/*
===============
CL_AddEntities

Emits all entities, particles, and lights to the refresh
===============
*/
void CL_AddEntities(void)
{
    CL_CalcViewValues();
    CL_FinishViewValues();
    CL_AddPacketEntities();
    CL_AddPlayerModelFlash();
    CL_AddTEnts();
    CL_AddParticles();
    CL_AddDLights();
    CL_AddLightStyles();
	CL_AddTestModel();
    CL_AddDynamicLightsToScene();
    LE_AddLightsToScene();
    LOC_AddLocationsToScene();
}

/*
===============
CL_GetEntitySoundOrigin

Called to get the sound spatialization origin
===============
*/
void CL_GetEntitySoundOrigin(int entnum, vec3_t org)
{
    centity_t   *ent;
    mmodel_t    *cm;
    vec3_t      mid;

    if (entnum < 0 || entnum >= MAX_EDICTS) {
        Com_Error(ERR_DROP, "%s: bad entnum: %d", __func__, entnum);
    }

    if (!entnum || entnum == listener_entnum) {
        // should this ever happen?
        VectorCopy(listener_origin, org);
        return;
    }

    // interpolate origin
    // FIXME: what should be the sound origin point for RF_BEAM entities?
    ent = &cl_entities[entnum];
    LerpVector(ent->prev.origin, ent->current.origin, cl.lerpfrac, org);

    // offset the origin for BSP models
    if (ent->current.solid == PACKED_BSP) {
        cm = cl.model_clip[ent->current.modelindex];
        if (cm) {
            VectorAvg(cm->mins, cm->maxs, mid);
            VectorAdd(org, mid, org);
        }
    }
}

