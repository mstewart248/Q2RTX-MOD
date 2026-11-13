/*
Copyright (C) 2026 Q2RTX contributors

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

/*
==============================================================================

NVIDIA FLOW

Fire and smoke for explosions, simulated by NVIDIA Flow 2 - the sparse-grid
gas solver in the flow/ directory of the PhysX 5 SDK - and drawn by the path
tracer. The client hands effects here through R_FlowEmit (cl_flow_explosions):
explosions from client/tent.c, the BFG's ball and lasers from there and from
client/entities.c. shader/flow.glsl marches the result along the primary and
reflection rays and lays it over the effects overlay.

nvflow.dll and nvflowext.dll are loaded at run time and come from
setup/build_flow.py, which builds them from a pinned PhysX commit with one
addition, NvFlowVulkanExternal.h: Flow's Vulkan backend running on OUR
VkDevice instead of a private one of its own. That is the whole design.

  - Flow records its compute passes into its own command buffers and submits
    them to qvk.queue_graphics from vkpt_flow_update, before anything of this
    frame is submitted. Every Flow frame starts its transitions from all
    stages and ends with an all-stages barrier, so submission order on the
    one queue is all the synchronisation there is, in both directions.
  - The result - the density atlas (RGBA16F: temperature, fuel, burn or
    shadow, smoke) and the sparse block table that says where each block of
    cells sits in it - is ACQUIRED from Flow every frame: a reference that
    keeps Flow's pool from handing it out again. Its VkImageView and VkBuffer
    go straight into the global texture set at BINDING_OFFSET_FLOW_*, with no
    copy.
  - That set alternates even/odd per frame, and a set is only rewritten once
    the frame that last used it has passed its fence. So each parity keeps
    its acquisitions until its set is rewritten, and every descriptor always
    names something alive. With Flow idle, missing or switched off, the sets
    name small dummies and FlowParams says there is nothing to draw.

The FG present thread submits to the same queue under FGPresent_SwapchainLock,
so Flow is given a vkGetInstanceProcAddr whose vkQueueSubmit and
vkQueueWaitIdle take that lock.

THE SIMULATION has a Flow layer per kind of effect (r_flow_style_t), all of
pt_flow_cell_size cells:

  - FIRE. An explosion is a sphere emitter that lives pt_flow_burst seconds
    of game time: it drives the grid inside it to hot, fuel-laden and a
    little smoky, and pushes outward (the emitter's divergence, which the
    pressure solve turns into an expanding flow round it). Then it goes, and
    Flow's combustion runs on by itself: fuel above the ignition temperature
    burns, burning makes heat and smoke, heat rises and cools, smoke fades.
    A rocket's trail (a TRAIL of fire, flow_rocket_spheres) is the same gas
    on a layer of its own, FLOW_LAYER_FIRE_TRAIL, which glows by
    pt_flow_rocket_brightness: with HDR Matt took the blasts down from 40 to
    5, and the small fireball at a flying rocket's tail wanted the 40 still
    (11-09). Its light is the fire's (seed_layers). A grenade's trail, smoke
    with too little heat to glow, stays on the fire's layer, where its own
    blast blows it about.
  - PLASMA, the BFG's: heat and smoke with no fuel, so nothing burns. It
    glows green while it cools and leaves dark green smoke behind. The ball
    in flight is a TRAIL - a sphere that follows it, drawing a glowing tail
    of cooling plasma - its lasers are BEAMS, rows of thin boxes, and its
    BLAST is some ninety small bursts in a set order (flow_emit_blast).
    What it does to each target it zaps is a small burst of its own, about
    the size of one of the blast's lumps.
  - BUBBLES, an explosion under water: no heat, only "smoke" that floats
    (Flow's buoyancy per smoke) and is drawn as pale, scattering foam, up to
    the water's surface and no further.
  - RAIL, the railgun's trail (cl_flow_railtrail): heat and smoke with no
    fuel, like the plasma, but glowing the classic trail's cyan-blue. A shot
    is one beam, built each frame it emits into a helix of small boxes round
    a thread of smoke (flow_rail_boxes). Not on pt_flow_cell_size cells:
    its own pt_flow_rail_cell_size are finer, for a spiral only a cell or so
    thick (FLOW_GRIDS in shader/flow.h). Its LEVEL OF DETAIL: the part of a
    shot further than pt_flow_rail_lod_distance from the view when it was
    fired goes on a layer of its own, FLOW_LAYER_RAIL_FAR, that is the same
    gas on coarser pt_flow_rail_far_cell_size cells - far fewer blocks, and
    a longer march step, where the fine cells would be too small to see.
  - HEATBEAM, the plasma beam (cl_flow_heatbeam): heat and a little smoke,
    glowing yellow on the fire's colour map, and in place of the beam's
    model. A beam given every frame it is on, like a trail, and built each
    frame into a hot core along it, rings of small boxes running out round
    that and a hot spot where it hits (flow_heatbeam). Its fuel never burns:
    it is how bright the glow is, part by part (FLOW_COLORMAP_WEIGHTED_FIRE).
    On a grid of its own, pt_flow_heatbeam_cell_size, finer than the main
    grid's cells.

Flow allocates blocks where there is smoke and frees them once it has faded,
so when nothing is going on the grid is empty and this module skips Flow
entirely until the next explosion.

WHAT IT COSTS is Flow's steps and flow.glsl's march, per pixel, along every
primary ray and reflection that crosses the box round the blocks. So:

  - Flow takes at most pt_flow_rate steps a second. Below that frame rate
    each frame steps by its own time; above it Flow's own stepper takes a
    step on only some frames, and the frames between draw the last one.
  - flow_occupancy.comp marks, after every step, the 4x4x4-cell bricks of
    each block that can show anything, and the march only samples those -
    measured headless, 12-21% of the steps it would take through allocated
    blocks. Reflections march pt_flow_reflect_step times coarser.
  - Fire's leftover fuel fades (pt_flow_fuel_fade), or it smoulders on just
    above Flow's ignition temperature, making smoke too thin to see that
    keeps its blocks for seconds.

Two things in the menu go further (pt_flow_lighting, pt_flow_collision):

  - LIGHT. Each fire or plasma burst takes a light slot. flow_lights.comp adds
    up the glow of every block round the slot's seed; two frames later, past
    that frame's fence, flow_lights_collect reads the sums back and puts a
    sphere light where the glow is, as wide as it is spread and as bright as
    all of it (FLOW_LIGHT_UNIT), and moves the seed there to follow it as it
    rises. The light goes into the scene like any other dlight, so it casts
    shadows and lights the walls; and the smoke scatters it (flow_lit in
    flow.glsl), shadowed by Flow's own shadow pass toward the brightest.
    With pt_flow_heatbeam_light 1 (set with cl_flow_heatbeam_fire 1) things on fire take a slot too
    (flow_burn_light), and the plasma beam is lit as the railgun's beam is,
    a cylinder light along its gas (vkpt_flow_beam_lights).
  - COLLISION. The world's opaque triangles near the gas go to Flow as a
    collision mesh (flow_collision_update): Flow stops the gas from moving in
    a band from pt_flow_collision_depth units behind each face to
    pt_flow_collision_margin in front, which holds smoke in a room.

==============================================================================
*/

#include "shared/shared.h"
#include "common/common.h"
#include "system/system.h"
#include "vkpt.h"
#include "vk_util.h"
#include "fg_present.h"
#include "shader/flow.h"

#include <float.h>

#if USE_FLOW
#define NV_FLOW_CPU 1
#include "NvFlowExt.h"
#include "NvFlowVulkanExternal.h"
#include "NvFlowD3D12External.h"
#endif

#ifdef _WIN32
#define FLOW_LIBNAME        "nvflow.dll"
#define FLOW_EXT_LIBNAME    "nvflowext.dll"
#else
#define FLOW_LIBNAME        "libnvflow.so"
#define FLOW_EXT_LIBNAME    "libnvflowext.so"
#endif

// emitters of every kind at once; the oldest goes when another comes. A BFG
// blast is about 90 by itself.
#define FLOW_MAX_EMITTERS   256

// the even and odd global texture sets, see qvk_get_current_desc_set_textures
#define FLOW_SET_COUNT      2

// A beam is cut into boxes no longer than this. Flow allocates blocks over a
// box's axis-aligned bounds, so one box along a long diagonal would take
// every block of the box round it.
#define FLOW_BEAM_SEGMENT   48.f

// Box emitters at once: the BFG's laser segments, and the railgun's trails,
// which are some 300 boxes each at their longest (flow_rail_boxes).
#define FLOW_MAX_BOXES      1024

// Boxes a turn of a railgun trail's spiral is cut into.
#define FLOW_RAIL_CHORDS    8

// Light power - radiance x radius^2, the way a sphere light's brightness
// goes - per unit of glow, colour map x cubic units. Measured headless with
// the defaults: a rocket's fireball holds about 25 million of glow for most
// of a second, which comes out at 5000, three times the classic explosion
// light's 1600 (ex_poly_light in client/tent.c at its widest).
#define FLOW_LIGHT_UNIT     2e-4f

// The most collision triangles handed to Flow at once.
#define FLOW_MAX_MESH_TRIS  65536

// The collision mesh is cut for a box round the gas in steps of this, so it
// is cut again only when the gas has moved on by about as much.
#define FLOW_MESH_STEP      128.f

// Trails pt_flow_pool_stats tells apart in its second; past that it stops counting.
#define FLOW_REFUSED_TRAILS 64

// The block pool, pt_flow_max_blocks, can be anything from FLOW_POOL_MIN to
// FLOW_POOL_MAX, and changes while the game runs (flow_pool_resize). What is
// sized by it on our side - the light sums and the occupancy - is sized for
// the most, a few hundred KB.
#define FLOW_POOL_MIN       16
#define FLOW_POOL_MAX       4096

// After a resize Flow keeps stepping, gas or none, for this many steps, so
// that its backend frees the old pool's textures: that is done in its flush,
// 60 fences after their last use (ContextD3D12.cpp / ContextVulkan.cpp).
#define FLOW_POOL_SETTLE    72

// The most puffs a rocket's trail fills one step's flight with, and how far
// its source may move in a step before that is taken for a jump - a new
// rocket in the old one's slot - rather than flight.
#define FLOW_TRAIL_PUFFS    8
#define FLOW_TRAIL_JUMP     128.f

typedef struct {
	r_flow_kind_t   kind;
	int             layer;      // an r_flow_style_t
	vec3_t          origin;
	vec3_t          end;        // a beam segment's other end
	float           scale;
	float           age;        // seconds of game time it has been emitting; below 0, yet to start
	uint64_t        luid;       // Flow's identity for the emitter
	int             key;        // a trail's source
	bool            seen;       // a trail: given again since the last frame
	vec3_t          heading;    // a trail: which way its source last moved, or 0
	vec3_t          last;       // a rocket's trail: where its source was at Flow's last step
	bool            smoke_only; // a trail of fire with no flame: a grenade's
	float           flown;      // a grenade's trail: units its source has gone
	int             light;      // a trail: its light slot, or -1
	uint32_t        light_generation;   // ... while the slot has this generation
	// A piece of a BFG blast: its own radius in units, push, emitting time and
	// outward velocity instead of the cvars'. 0 radius for any other emitter.
	float           radius;
	float           expansion;
	float           life;
	vec3_t          velocity;
	// A railgun's trail: the stretch of it drawn, in units from `origin`
	// toward `end` (pt_flow_rail_length); and the part of that on the fine
	// grid, near_from to near_to, the rest being on FLOW_LAYER_RAIL_FAR's
	// coarse one (flow_rail_stretch). A plasma beam: the stretch its core
	// is drawn on, the client's model being on the rest (flow_heatbeam_stretch).
	float           from, to;
	float           near_from, near_to;
	// A plasma beam: given again since the last frame (`seen`, as a trail),
	// and its end on something it burns; and the line its gas was last laid
	// along, which a pause can leave behind (flow_heatbeam_relay).
	bool            hit;
	vec3_t          laid_origin, laid_end;
	// Something on fire (R_FLOW_BURN): half its box's size round `origin`,
	// and how hot it burns, 0 to 1 (flow_burn_spheres). `last` is where its
	// box's middle was at Flow's last step, as a rocket trail's source.
	vec3_t          half;
	float           heat;
	float           core;       // its flames' share that is heat, 0 = pt_flow_burn_flame
} flow_emitter_t;

// A light that a fire or plasma burst casts, see flow_lights_collect.
typedef struct {
	int             layer;      // an r_flow_style_t; -1 for a free slot
	uint32_t        generation; // bumped each time the slot is taken
	vec3_t          seed;       // where flow_lights.comp gathers its glow
	float           gather;     // how far from the seed a block may be
	float           age;        // seconds of game time since it was taken
	float           strength;   // pt_flow_light or pt_flow_bfg_light
	// A trail's: the seed is where its source is now, and the light keeps to
	// the glow's place relative to it, so it does not lag the two frames the
	// sums take to come back.
	bool            follow;
	// Something on fire's (R_FLOW_BURN, flow_burn_light): never shared with a
	// burst, and the first to go to one when every slot is taken.
	bool            burn;
	bool            lit;        // the glow has come back: the light below is real
	vec3_t          origin;
	vec3_t          radiance;   // absolute, like any light's
	float           radius;
} flow_light_t;

static cvar_t *cvar_flow_available;
static cvar_t *cvar_pt_flow;
static cvar_t *cvar_pt_flow_lighting;
static cvar_t *cvar_pt_flow_collision;
static cvar_t *cvar_pt_flow_cell_size;
static cvar_t *cvar_pt_flow_max_blocks;
static cvar_t *cvar_pt_flow_prewarm;
static cvar_t *cvar_pt_flow_substeps;
static cvar_t *cvar_pt_flow_radius;
static cvar_t *cvar_pt_flow_burst;
static cvar_t *cvar_pt_flow_expansion;
static cvar_t *cvar_pt_flow_temperature;
static cvar_t *cvar_pt_flow_fuel;
static cvar_t *cvar_pt_flow_smoke;
static cvar_t *cvar_pt_flow_couple_rate;
static cvar_t *cvar_pt_flow_buoyancy;
static cvar_t *cvar_pt_flow_cooling;
static cvar_t *cvar_pt_flow_smoke_fade;
static cvar_t *cvar_pt_flow_smoke_damping;
static cvar_t *cvar_pt_flow_smoke_per_burn;
static cvar_t *cvar_pt_flow_vorticity;
static cvar_t *cvar_pt_flow_fire_brightness;
static cvar_t *cvar_pt_flow_fire_orange;
static cvar_t *cvar_pt_flow_fire_glow_start;
static cvar_t *cvar_pt_flow_smoke_brightness;
static cvar_t *cvar_pt_flow_smoke_albedo;
static cvar_t *cvar_pt_flow_smoke_scatter;
static cvar_t *cvar_pt_flow_density;
static cvar_t *cvar_pt_flow_step;
static cvar_t *cvar_pt_flow_max_steps;
static cvar_t *cvar_pt_flow_march;
static cvar_t *cvar_pt_flow_stats;
static cvar_t *cvar_pt_flow_profile;
static cvar_t *cvar_pt_flow_collision_layers;
static cvar_t *cvar_pt_flow_pool_stats;
static cvar_t *cvar_pt_flow_glow_floor;
static cvar_t *cvar_pt_flow_rate;
static cvar_t *cvar_pt_flow_reflect_step;
static cvar_t *cvar_pt_flow_fuel_fade;
static cvar_t *cvar_pt_flow_bfg_radius;
static cvar_t *cvar_pt_flow_bfg_expansion;
static cvar_t *cvar_pt_flow_bfg_burst;
static cvar_t *cvar_pt_flow_bfg_cooling;
static cvar_t *cvar_pt_flow_bfg_buoyancy;
static cvar_t *cvar_pt_flow_bfg_brightness;
static cvar_t *cvar_pt_flow_bfg_smoke;
static cvar_t *cvar_pt_flow_bfg_smoke_fade;
static cvar_t *cvar_pt_flow_bfg_smoke_damping;
static cvar_t *cvar_pt_flow_bfg_smoke_brightness;
static cvar_t *cvar_pt_flow_bfg_smoke_density;
static cvar_t *cvar_pt_flow_bfg_puffs;
static cvar_t *cvar_pt_flow_bfg_lumps;
static cvar_t *cvar_pt_flow_bfg_jets;
static cvar_t *cvar_pt_flow_bfg_jet_length;
static cvar_t *cvar_pt_flow_bfg_blast_size;
static cvar_t *cvar_pt_flow_bfg_blast_expansion;
static cvar_t *cvar_pt_flow_bfg_blast_light;
static cvar_t *cvar_pt_flow_bfg_vorticity;
static cvar_t *cvar_pt_flow_trail_radius;
static cvar_t *cvar_pt_flow_trail_expansion;
static cvar_t *cvar_pt_flow_trail_smoke;
static cvar_t *cvar_pt_flow_trail_length;
static cvar_t *cvar_pt_flow_bfg_trail_light;
static cvar_t *cvar_pt_flow_rocket_radius;
static cvar_t *cvar_pt_flow_rocket_expansion;
static cvar_t *cvar_pt_flow_rocket_smoke;
static cvar_t *cvar_pt_flow_rocket_temperature;
static cvar_t *cvar_pt_flow_rocket_glow;
static cvar_t *cvar_pt_flow_rocket_brightness;
static cvar_t *cvar_pt_flow_rocket_flame;
static cvar_t *cvar_pt_flow_rocket_lumps;
static cvar_t *cvar_pt_flow_rocket_budget;
static cvar_t *cvar_pt_flow_burn_temperature;
static cvar_t *cvar_pt_flow_burn_smoke;
static cvar_t *cvar_pt_flow_burn_flame;
static cvar_t *cvar_pt_flow_burn_size;
static cvar_t *cvar_pt_flow_burn_min_radius;
static cvar_t *cvar_pt_flow_burn_max_radius;
static cvar_t *cvar_pt_flow_burn_lumps;
static cvar_t *cvar_pt_flow_burn_burst;
static cvar_t *cvar_pt_flow_burn_expansion;
static cvar_t *cvar_pt_flow_burn_budget;
static cvar_t *cvar_pt_flow_gib_smoke;
static cvar_t *cvar_pt_flow_gib_smoke_heat;
static cvar_t *cvar_pt_flow_gib_smoke_rise;
static cvar_t *cvar_pt_flow_gib_smoke_size;
static cvar_t *cvar_pt_flow_grenade_radius;
static cvar_t *cvar_pt_flow_grenade_expansion;
static cvar_t *cvar_pt_flow_grenade_smoke;
static cvar_t *cvar_pt_flow_grenade_heat;
static cvar_t *cvar_pt_flow_grenade_thin;
static cvar_t *cvar_pt_flow_grenade_thin_length;
static cvar_t *cvar_pt_flow_grenade_lumps;
static cvar_t *cvar_pt_flow_beam_radius;
static cvar_t *cvar_pt_flow_beam_expansion;
static cvar_t *cvar_pt_flow_beam_burst;
static cvar_t *cvar_pt_flow_rail_cell_size;
static cvar_t *cvar_pt_flow_rail_far_cell_size;
static cvar_t *cvar_pt_flow_rail_lod_distance;
static cvar_t *cvar_pt_flow_rail_far_brightness;
static cvar_t *cvar_pt_flow_rail_life;
static cvar_t *cvar_pt_flow_rail_drain;
static cvar_t *cvar_pt_flow_rail_fade;
static cvar_t *cvar_pt_flow_rail_radius;
static cvar_t *cvar_pt_flow_rail_width;
static cvar_t *cvar_pt_flow_rail_turn;
static cvar_t *cvar_pt_flow_rail_temperature;
static cvar_t *cvar_pt_flow_rail_smoke;
static cvar_t *cvar_pt_flow_rail_expansion;
static cvar_t *cvar_pt_flow_rail_swirl;
static cvar_t *cvar_pt_flow_rail_core_radius;
static cvar_t *cvar_pt_flow_rail_core_temperature;
static cvar_t *cvar_pt_flow_rail_core_smoke;
static cvar_t *cvar_pt_flow_rail_core_expansion;
static cvar_t *cvar_pt_flow_rail_burst;
static cvar_t *cvar_pt_flow_rail_cooling;
static cvar_t *cvar_pt_flow_rail_buoyancy;
static cvar_t *cvar_pt_flow_rail_vorticity;
static cvar_t *cvar_pt_flow_rail_smoke_damping;
static cvar_t *cvar_pt_flow_rail_smoke_fade;
static cvar_t *cvar_pt_flow_rail_brightness;
static cvar_t *cvar_pt_flow_rail_smoke_brightness;
static cvar_t *cvar_pt_flow_rail_density;
static cvar_t *cvar_pt_flow_rail_length;
static cvar_t *cvar_pt_flow_rail_budget;
static cvar_t *cvar_pt_flow_heatbeam_ring_radius;
static cvar_t *cvar_pt_flow_heatbeam_ring_width;
static cvar_t *cvar_pt_flow_heatbeam_ring_spacing;
static cvar_t *cvar_pt_flow_heatbeam_ring_speed;
static cvar_t *cvar_pt_flow_heatbeam_ring_temperature;
static cvar_t *cvar_pt_flow_heatbeam_ring_smoke;
static cvar_t *cvar_pt_flow_heatbeam_ring_expansion;
static cvar_t *cvar_pt_flow_heatbeam_ring_brightness;
static cvar_t *cvar_pt_flow_heatbeam_ring_travel;
static cvar_t *cvar_pt_flow_heatbeam_ring_end_smoke;
static cvar_t *cvar_pt_flow_heatbeam_ring_tail;
static cvar_t *cvar_pt_flow_heatbeam_core_brightness;
static cvar_t *cvar_pt_flow_heatbeam_end_on;
static cvar_t *cvar_pt_flow_heatbeam_core_clear;
static cvar_t *cvar_pt_flow_heatbeam_near_fade;
static cvar_t *cvar_pt_flow_heatbeam_near_brightness;
static cvar_t *cvar_pt_flow_heatbeam_relay;
static cvar_t *cvar_pt_flow_heatbeam_cell_size;
static cvar_t *cvar_pt_flow_heatbeam_mirror;
static cvar_t *cvar_pt_flow_heatbeam_trails;
static cvar_t *cvar_pt_flow_heatbeam_lean;
static cvar_t *cvar_pt_flow_heatbeam_soft_fill;
static cvar_t *cvar_pt_flow_heatbeam_ring_soft;
static cvar_t *cvar_pt_flow_heatbeam_inner_radius;
static cvar_t *cvar_pt_flow_heatbeam_inner_temperature;
static cvar_t *cvar_pt_flow_heatbeam_inner_brightness;
static cvar_t *cvar_pt_flow_heatbeam_strands;
static cvar_t *cvar_pt_flow_heatbeam_strand_pitch;
static cvar_t *cvar_pt_flow_heatbeam_strand_radius;
static cvar_t *cvar_pt_flow_heatbeam_strand_width;
static cvar_t *cvar_pt_flow_heatbeam_strand_temperature;
static cvar_t *cvar_pt_flow_heatbeam_strand_brightness;
static cvar_t *cvar_pt_flow_heatbeam_strand_length;
static cvar_t *cvar_pt_flow_heatbeam_every_frame;
static cvar_t *cvar_pt_flow_heatbeam_radius;
static cvar_t *cvar_pt_flow_heatbeam_temperature;
static cvar_t *cvar_pt_flow_heatbeam_smoke;
static cvar_t *cvar_pt_flow_heatbeam_expansion;
static cvar_t *cvar_pt_flow_heatbeam_flow;
static cvar_t *cvar_pt_flow_heatbeam_impact_radius;
static cvar_t *cvar_pt_flow_heatbeam_impact_temperature;
static cvar_t *cvar_pt_flow_heatbeam_impact_smoke;
static cvar_t *cvar_pt_flow_heatbeam_impact_expansion;
static cvar_t *cvar_pt_flow_heatbeam_cooling;
static cvar_t *cvar_pt_flow_heatbeam_buoyancy;
static cvar_t *cvar_pt_flow_heatbeam_vorticity;
static cvar_t *cvar_pt_flow_heatbeam_smoke_fade;
static cvar_t *cvar_pt_flow_heatbeam_brightness;
static cvar_t *cvar_pt_flow_heatbeam_yellow;
static cvar_t *cvar_pt_flow_heatbeam_smoke_brightness;
static cvar_t *cvar_pt_flow_heatbeam_density;
static cvar_t *cvar_pt_flow_heatbeam_length;
static cvar_t *cvar_pt_flow_heatbeam_clear;
static cvar_t *cvar_pt_flow_heatbeam_budget;
static cvar_t *cvar_pt_flow_bubble_radius;
static cvar_t *cvar_pt_flow_bubble_expansion;
static cvar_t *cvar_pt_flow_bubble_amount;
static cvar_t *cvar_pt_flow_bubble_rise;
static cvar_t *cvar_pt_flow_bubble_fade;
static cvar_t *cvar_pt_flow_bubble_brightness;
static cvar_t *cvar_pt_flow_bubble_density;
static cvar_t *cvar_pt_flow_light;
static cvar_t *cvar_pt_flow_bfg_light;
static cvar_t *cvar_pt_flow_heatbeam_light;
static cvar_t *cvar_pt_flow_heatbeam_light_power;
static cvar_t *cvar_pt_flow_burn_light;
static cvar_t *cvar_pt_flow_light_size;
static cvar_t *cvar_pt_flow_light_radius;
static cvar_t *cvar_pt_flow_shadow;
static cvar_t *cvar_pt_flow_shadow_min;
static cvar_t *cvar_pt_flow_collision_depth;
static cvar_t *cvar_pt_flow_env;
static cvar_t *cvar_pt_flow_env_light;
static cvar_t *cvar_pt_flow_env_flat;
static cvar_t *cvar_pt_flow_env_spacing;
static cvar_t *cvar_pt_flow_env_history;
static cvar_t *cvar_pt_flow_env_clamp;
static cvar_t *cvar_pt_flow_env_max_probes;
static cvar_t *cvar_pt_flow_collision_margin;

static struct {
	// Always there, so the descriptors always name something.
	VkImage             dummy_image;
	VkDeviceMemory      dummy_memory;
	VkImageView         dummy_view;
	BufferResource_t    dummy_table;
	BufferResource_t    params;         // a FlowParams per set, params_stride apart
	byte                *params_mapped;
	size_t              params_stride;
	// this frame's FlowParams.block_dim_bits.w: the march has a grid to read
	bool                marching;

	// What flow_lights.comp writes: FLOW_LIGHT_SUM_VEC4S vec4s a block, a
	// set's worth light_sums_stride apart. Read on the host once the set's
	// frame has passed its fence.
	BufferResource_t    light_sums;
	byte                *light_sums_mapped;
	size_t              light_sums_stride;
	VkPipelineLayout    lights_pipeline_layout;
	VkPipeline          lights_pipeline;

	// What flow_occupancy.comp writes for the march: a uvec4 a block, for
	// occupancy_blocks blocks, then the group bitset (FLOW_COARSE_UVEC4S), the
	// block bitset (FLOW_BLOCK_UVEC4S) and the screen tiles (FLOW_SCREEN_UVEC4S);
	// a set's worth occupancy_stride apart.
	BufferResource_t    occupancy;
	size_t              occupancy_stride;
	uint32_t            occupancy_blocks;
	VkPipeline          occupancy_pipeline;

	flow_emitter_t      emitters[FLOW_MAX_EMITTERS];
	int                 num_emitters;
	uint64_t            next_luid;

	flow_light_t        lights[FLOW_MAX_LIGHTS];

	// qvk.frame_counter + 1 of the frame vkpt_flow_update last ran in, 0 for none
	uint64_t            updated_frame;

	// The world's light on the smoke (pt_flow_env, flow_ambient.comp): the
	// probes, which texel holds which probe, and the pass's own set per frame
	// in flight for the TLAS. Only with ray query; otherwise env_view is null
	// and the dummy stands in.
	VkImage             env_image;
	VkDeviceMemory      env_memory;
	VkImageView         env_view;
	VkImage             env_key_image;
	VkDeviceMemory      env_key_memory;
	VkImageView         env_key_view;
	VkDescriptorSetLayout env_set_layout;
	VkDescriptorPool    env_pool;
	VkDescriptorSet     env_sets[MAX_FRAMES_IN_FLIGHT];
	VkPipelineLayout    env_pipeline_layout;
	VkPipeline          env_pipeline;
	uint32_t            env_groups[3];
	// On another renderer (vkpt_flow_backend): its probe grid is there, and
	// what vkpt_flow_update left it to record this frame.
	bool                env_backend;
	vkpt_flow_dispatch_t dispatch;
	// given by vkpt_flow_set_env for this frame only
	const bsp_t         *env_bsp;
	int                 env_first_light;
	int                 env_num_lights;

#if USE_FLOW
	bool                libraries_loaded;
	NvFlowOpList        *op_list;
	NvFlowExtOpList     *ext_op_list;
	NvFlowGridInterface         *grid_iface;
	NvFlowGridParamsInterface   *params_iface;
	NvFlowDeviceInterface       *device_iface;
	PFN_NvFlowVulkanExternalCreateDeviceManager create_device_manager;
	PFN_NvFlowVulkanExternalCreateDevice        create_device;
	PFN_NvFlowVulkanExternalGetTexture          get_texture;
	PFN_NvFlowVulkanExternalGetBuffer           get_buffer;
	PFN_NvFlowGetDeviceInterface                get_device_iface;
	// the D3D12 backend setup/build_flow.py adds (vkpt_flow_backend); NULL in a
	// build of the libraries from before it
	PFN_NvFlowD3D12ExternalCreateDeviceManager  d3d12_create_device_manager;
	PFN_NvFlowD3D12ExternalCreateDevice         d3d12_create_device;
	PFN_NvFlowD3D12ExternalGetTexture           d3d12_get_texture;
	PFN_NvFlowD3D12ExternalGetBuffer            d3d12_get_buffer;

	NvFlowDeviceManager     *device_manager;
	NvFlowDevice            *device;
	NvFlowDeviceQueue       *queue;
	NvFlowContextInterface  *context_iface;
	NvFlowContext           *context;
	NvFlowGrid              *grid;
	NvFlowGridParamsNamed   *params_named;
	NvFlowGridParams        *grid_params;

	// What each set's descriptors name, held until that set is rewritten.
	NvFlowTexture           *set_density[FLOW_SET_COUNT];
	NvFlowBuffer            *set_table[FLOW_SET_COUNT];

	// This frame's parameters. Flow keeps the pointers, not copies, until
	// the simulation that reads them is done, so they live here.
	NvFlowGridSimulateLayerParams   simulate[FLOW_LAYERS];
	NvFlowGridSimulateLayerParams   *simulate_ptrs[FLOW_LAYERS];
	// an emitter's own, and the sphere that ends a trail's glow
	NvFlowGridEmitterSphereParams   spheres[2 * FLOW_MAX_EMITTERS];
	NvFlowGridEmitterSphereParams   *sphere_ptrs[2 * FLOW_MAX_EMITTERS];
	NvFlowGridEmitterBoxParams      boxes[FLOW_MAX_BOXES];
	NvFlowGridEmitterBoxParams      *box_ptrs[FLOW_MAX_BOXES];
	NvFlowGridEmitterMeshParams     mesh;
	NvFlowGridEmitterMeshParams     *mesh_ptr;
	NvFlowGridOffscreenLayerParams  offscreen[FLOW_LAYERS];
	NvFlowGridOffscreenLayerParams  *offscreen_ptrs[FLOW_LAYERS];
	NvFlowDatabaseTypeSnapshot      type_snapshots[5];
	NvFlowUint64            version;

	double                  sim_time;   // seconds simulated since the grid was made
	float                   last_time;  // refdef time of the last world frame
	bool                    have_last_time;
	float                   frame_dt;   // game time a frame, smoothed, for pt_flow_rate
	float                   rail_age;   // game time since the last railgun trail was emitted
	float                   rail_smoke; // the most smoke the trails can have left, followed down as Flow fades it
	float                   rail_heat;  // the most heat
	float                   rail_smoke_fade;    // this frame's fades for the rail layer, per second (flow_rail_fade)
	float                   rail_heat_fade;
	// Flow's own stepper mirrored (flow_step_mirror): the time it has yet to
	// step, whether it steps this frame and how much time it steps in all.
	float                   step_error;
	bool                    stepping;
	float                   step_time;
	uint32_t                num_blocks; // the blocks Flow had after its last simulation
	uint32_t                max_blocks; // the most it can have
	// pt_flow_pool_stats (flow_pool_stats): each layer's share of num_blocks,
	// the most in use since the last print, and the trails sent back to the
	// client's particles since then for want of blocks - by source, as the
	// client asks again every frame (flow_refused).
	uint32_t                layer_blocks[FLOW_LAYERS];
	uint32_t                peak_blocks;
	struct {
		int     key;
		bool    grenade;
		bool    cut;        // it was in Flow, then cut off in flight
	}                       refused[FLOW_REFUSED_TRAILS];
	uint32_t                num_refused;
	uint32_t                refused_rail;   // railgun shots
	uint32_t                refused_heatbeam;   // plasma beams, frames of them
	unsigned                pool_stats_ms;
	// flow_pool_resize: the second reset is still to come, and the steps
	// Flow still takes, idle or not, for the old pool to be freed
	bool                    pool_reset_again;
	int                     pool_settle;
	bool                    idle;       // nothing allocated and nothing emitting: skip Flow
	bool                    force_clear;
	bool                    shadow_pass;    // this frame's simulation is followed by Flow's shadow pass
	// pt_flow_prewarm (flow_warm_emitters): a map was loaded and its warm-up
	// step is still to come; this frame is that step; where it is put
	bool                    warm_pending;
	bool                    warm_now;
	vec3_t                  warm_origin;

	// Where the bubbles stop: the highest water surface over any underwater
	// explosion since the bubble layer was last empty.
	float                   bubble_surface;
	bool                    have_bubble_surface;

	// Where the last world frame was seen from, for the stretch of a long
	// railgun trail that is drawn; and how much nearer than it is things look
	// for its field of view - 1 at Quake's 90 degrees, more zoomed in - for
	// the part of the trail on the fine grid.
	vec3_t                  view_origin;
	float                   view_zoom;

	// Laying a plasma beam's gas again under a pause (flow_heatbeam_relay):
	// the frame of it this is - -1 none, 0 its clearing, 1 to relay_steps its
	// steps - and how long each step is; where the view was when it was last
	// laid, and the frame before this one; and whether one has been laid
	// since time last ran, to be cleared when it runs again.
	int                     relay_step;
	int                     relay_steps;
	float                   relay_dt;
	vec3_t                  relay_view;
	vec3_t                  last_view;
	bool                    relaid;
	bool                    heatbeam_clear;     // this simulation clears the plasma beam's layer
	bool                    every_frame;        // Flow steps every frame: your own plasma beam is on (flow_every_frame)

	// The corners of every block Flow had last frame, for the collision mesh.
	vec3_t                  blocks_min, blocks_max;
	bool                    have_blocks;

	// The world's opaque triangles, 9 floats each (vkpt_flow_set_world), and
	// the ones near the gas, as Flow's mesh emitter wants them.
	float                   *world_tris;
	int                     num_world_tris;
	NvFlowFloat3            *mesh_positions;
	int                     *mesh_indices;
	int                     *mesh_counts;
	int                     mesh_capacity;      // triangles
	int                     num_mesh_tris;
	NvFlowUint64            mesh_version;
	int                     mesh_region[6];     // what the mesh was cut for, in FLOW_MESH_STEPs
	bool                    mesh_region_valid;

	// Light slots as they went to the GPU for each set: which generation of
	// the slot, and its seed, which the sums are relative to; and how many
	// blocks were added up. 0 blocks is no sums to read.
	uint32_t                light_sum_blocks[FLOW_SET_COUNT];
	uint32_t                light_sum_generation[FLOW_SET_COUNT][FLOW_MAX_LIGHTS];
	vec3_t                  light_sum_seed[FLOW_SET_COUNT][FLOW_MAX_LIGHTS];
	uint64_t                lights_collected_frame;    // qvk.frame_counter + 1

	// The slots with a light, brightest first, as the scene and flow.glsl
	// get them; and per layer, the one whose shadow Flow works out, -1 none.
	dlight_t                dlights[FLOW_MAX_LIGHTS];
	int                     light_slot[FLOW_MAX_LIGHTS];
	int                     num_dlights;
	int                     layer_shadow[FLOW_LAYERS];
#endif
} flow;

const vkpt_flow_backend_t *vkpt_flow_backend;

#if USE_FLOW

/*
==============================================================================

THE QUEUE LOCK

==============================================================================
*/

static PFN_vkGetDeviceProcAddr  flow_real_get_device_proc_addr;
static PFN_vkQueueSubmit        flow_real_queue_submit;
static PFN_vkQueueWaitIdle      flow_real_queue_wait_idle;

static VKAPI_ATTR VkResult VKAPI_CALL
flow_queue_submit(VkQueue queue, uint32_t submit_count, const VkSubmitInfo *submits, VkFence fence)
{
	FGPresent_SwapchainLock();
	VkResult result = flow_real_queue_submit(queue, submit_count, submits, fence);
	FGPresent_SwapchainUnlock();
	return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL
flow_queue_wait_idle(VkQueue queue)
{
	FGPresent_SwapchainLock();
	VkResult result = flow_real_queue_wait_idle(queue);
	FGPresent_SwapchainUnlock();
	return result;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
flow_get_device_proc_addr(VkDevice device, const char *name)
{
	if (!strcmp(name, "vkQueueSubmit"))
		return (PFN_vkVoidFunction)flow_queue_submit;
	if (!strcmp(name, "vkQueueWaitIdle"))
		return (PFN_vkVoidFunction)flow_queue_wait_idle;
	return flow_real_get_device_proc_addr(device, name);
}

// Flow loads every device function through the vkGetDeviceProcAddr it gets
// from here.
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
flow_get_instance_proc_addr(VkInstance instance, const char *name)
{
	if (!strcmp(name, "vkGetDeviceProcAddr"))
		return (PFN_vkVoidFunction)flow_get_device_proc_addr;
	if (!strcmp(name, "vkQueueSubmit"))
		return (PFN_vkVoidFunction)flow_queue_submit;
	if (!strcmp(name, "vkQueueWaitIdle"))
		return (PFN_vkVoidFunction)flow_queue_wait_idle;
	return vkGetInstanceProcAddr(instance, name);
}

static void NV_FLOW_ABI flow_log(NvFlowLogLevel level, const char *format, ...)
{
	char        buffer[MAXPRINTMSG];
	va_list     args;

	va_start(args, format);
	Q_vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);

	if (level == eNvFlowLogLevel_error)
		Com_EPrintf("NVIDIA Flow: %s\n", buffer);
	else if (level == eNvFlowLogLevel_warning)
		Com_WPrintf("NVIDIA Flow: %s\n", buffer);
	else
		Com_DPrintf("NVIDIA Flow: %s\n", buffer);
}

/*
==============================================================================

SETUP

==============================================================================
*/

static void *flow_load_library(const char *name)
{
	char    path[MAX_OSPATH];
	void    *handle;

	Q_concat(path, sizeof(path), sys_libdir->string, PATH_SEP_STRING, name);
	Sys_LoadLibrary(path, NULL, &handle);
	if (!handle)
		Sys_LoadLibrary(name, NULL, &handle);
	return handle;
}

// Loaded once and never unloaded: Flow's worker threads and statics outlive
// any one renderer, and a vid_restart comes back to the same libraries.
static bool flow_load_libraries(void)
{
	static bool tried;
	void        *lib, *ext_lib;

	if (flow.libraries_loaded || tried)
		return flow.libraries_loaded;
	tried = true;

	lib = flow_load_library(FLOW_LIBNAME);
	ext_lib = lib ? flow_load_library(FLOW_EXT_LIBNAME) : NULL;
	if (!lib || !ext_lib) {
		Com_Printf("NVIDIA Flow: %s not found, explosions stay classic (setup/build_flow.py builds it).\n",
			lib ? FLOW_EXT_LIBNAME : FLOW_LIBNAME);
		return false;
	}

	PFN_NvFlowGetOpList get_op_list = (PFN_NvFlowGetOpList)Sys_GetProcAddress(lib, "NvFlowGetOpList");
	PFN_NvFlowGetExtOpList get_ext_op_list = (PFN_NvFlowGetExtOpList)Sys_GetProcAddress(ext_lib, "NvFlowGetExtOpList");
	PFN_NvFlowGetGridInterface get_grid = (PFN_NvFlowGetGridInterface)Sys_GetProcAddress(ext_lib, "NvFlowGetGridInterface");
	PFN_NvFlowGetGridParamsInterface get_grid_params = (PFN_NvFlowGetGridParamsInterface)Sys_GetProcAddress(ext_lib, "NvFlowGetGridParamsInterface");
	flow.get_device_iface = (PFN_NvFlowGetDeviceInterface)Sys_GetProcAddress(ext_lib, "NvFlowGetDeviceInterface");
	flow.create_device_manager = (PFN_NvFlowVulkanExternalCreateDeviceManager)Sys_GetProcAddress(ext_lib, "NvFlowVulkanExternalCreateDeviceManager");
	flow.create_device = (PFN_NvFlowVulkanExternalCreateDevice)Sys_GetProcAddress(ext_lib, "NvFlowVulkanExternalCreateDevice");
	flow.get_texture = (PFN_NvFlowVulkanExternalGetTexture)Sys_GetProcAddress(ext_lib, "NvFlowVulkanExternalGetTexture");
	flow.get_buffer = (PFN_NvFlowVulkanExternalGetBuffer)Sys_GetProcAddress(ext_lib, "NvFlowVulkanExternalGetBuffer");
	flow.d3d12_create_device_manager = (PFN_NvFlowD3D12ExternalCreateDeviceManager)Sys_GetProcAddress(ext_lib, "NvFlowD3D12ExternalCreateDeviceManager");
	flow.d3d12_create_device = (PFN_NvFlowD3D12ExternalCreateDevice)Sys_GetProcAddress(ext_lib, "NvFlowD3D12ExternalCreateDevice");
	flow.d3d12_get_texture = (PFN_NvFlowD3D12ExternalGetTexture)Sys_GetProcAddress(ext_lib, "NvFlowD3D12ExternalGetTexture");
	flow.d3d12_get_buffer = (PFN_NvFlowD3D12ExternalGetBuffer)Sys_GetProcAddress(ext_lib, "NvFlowD3D12ExternalGetBuffer");

	if (!get_op_list || !get_ext_op_list || !get_grid || !get_grid_params || !flow.get_device_iface ||
		!flow.create_device_manager || !flow.create_device || !flow.get_texture || !flow.get_buffer) {
		// A stock Flow build has no NvFlowVulkanExternal*: it can only make a
		// device of its own, which the renderer cannot sample.
		Com_Printf("NVIDIA Flow: %s is not the build from setup/build_flow.py.\n", FLOW_EXT_LIBNAME);
		return false;
	}

	flow.op_list = get_op_list();
	flow.ext_op_list = get_ext_op_list();
	flow.grid_iface = get_grid();
	flow.params_iface = get_grid_params();

	if (!flow.op_list || !flow.ext_op_list || !flow.grid_iface || !flow.params_iface)
		return false;

	flow.libraries_loaded = true;
	return true;
}

static void flow_destroy_simulation(void);

// Flow's device on the renderer's Vulkan device, submitting to its graphics queue
static bool flow_create_device_vulkan(void)
{
	if (qvk.device_count > 1) {
		Com_Printf("NVIDIA Flow: not with more than one GPU.\n");
		return false;
	}
	if (!qvk.supports_storage_image_without_format || !qvk.supports_fp16) {
		Com_Printf("NVIDIA Flow: this GPU lacks storage images without a format, or FP16.\n");
		return false;
	}
	flow.device_iface = flow.get_device_iface(eNvFlowContextApi_vulkan);
	if (!flow.device_iface)
		return false;

	flow_real_get_device_proc_addr = (PFN_vkGetDeviceProcAddr)vkGetInstanceProcAddr(qvk.instance, "vkGetDeviceProcAddr");
	flow_real_queue_submit = (PFN_vkQueueSubmit)vkGetDeviceProcAddr(qvk.device, "vkQueueSubmit");
	flow_real_queue_wait_idle = (PFN_vkQueueWaitIdle)vkGetDeviceProcAddr(qvk.device, "vkQueueWaitIdle");

	NvFlowVulkanExternalDesc desc = {
		.instance = qvk.instance,
		.physicalDevice = qvk.physical_device,
		.device = qvk.device,
		.queue = qvk.queue_graphics,
		.queueFamilyIndex = (NvFlowUint)qvk.queue_idx_graphics,
		.getInstanceProcAddr = (void *)flow_get_instance_proc_addr,
		.shaderStorageImageWriteWithoutFormat = NV_FLOW_TRUE,
		.logPrint = flow_log,
	};

	flow.device_manager = flow.create_device_manager(&desc);
	if (flow.device_manager)
		flow.device = flow.create_device(flow.device_manager, &desc);
	if (!flow.device)
		Com_EPrintf("NVIDIA Flow: could not start on the renderer's device.\n");
	return flow.device != NULL;
}

// ...or on the D3D12 renderer's device (vkpt_flow_backend), executing on the
// frame's queue: the backend setup/build_flow.py adds to Flow
static bool flow_create_device_d3d12(void)
{
	if (!flow.d3d12_create_device_manager || !flow.d3d12_create_device || !flow.d3d12_get_texture ||
		!flow.d3d12_get_buffer) {
		Com_Printf("NVIDIA Flow: %s has no D3D12 backend - setup/build_flow.py builds one that has.\n",
			FLOW_EXT_LIBNAME);
		return false;
	}
	flow.device_iface = flow.get_device_iface(eNvFlowContextApi_d3d12);
	if (!flow.device_iface)
		return false;

	NvFlowD3D12ExternalDesc desc = {
		.device = vkpt_flow_backend->d3d12_device,
		.queue = vkpt_flow_backend->d3d12_queue,
		.logPrint = flow_log,
	};
	flow.device_manager = flow.d3d12_create_device_manager(&desc);
	if (flow.device_manager)
		flow.device = flow.d3d12_create_device(flow.device_manager, &desc);
	if (!flow.device)
		Com_EPrintf("NVIDIA Flow: could not start on the renderer's D3D12 device.\n");
	return flow.device != NULL;
}

// The grid as pt_flow_max_blocks asks for it.
static NvFlowGridDesc flow_grid_desc(void)
{
	NvFlowGridDesc grid_desc = NvFlowGridDesc_default;
	grid_desc.maxLocations = (NvFlowUint)Cvar_ClampInteger(cvar_pt_flow_max_blocks, FLOW_POOL_MIN, FLOW_POOL_MAX);
	grid_desc.maxLocationsIsosurface = 16;  // the isosurface path is never used
	return grid_desc;
}

static void flow_create_simulation(void)
{
	if (!cvar_pt_flow->integer) {
		Com_Printf("NVIDIA Flow: off (pt_flow 0).\n");
		return;
	}
	if (!flow_load_libraries())
		return;

	if (!(vkpt_flow_backend ? flow_create_device_d3d12() : flow_create_device_vulkan())) {
		flow_destroy_simulation();
		return;
	}

	flow.queue = flow.device_iface->getDeviceQueue(flow.device);
	flow.context_iface = flow.device_iface->getContextInterface(flow.queue);
	flow.context = flow.device_iface->getContext(flow.queue);

	// The block pool is allocated whole at Flow's first step: an atlas of
	// max_blocks blocks of 34x18x18 RGBA16F texels, about 88 KB each, twice
	// over for the density and again for the velocity and the solver's
	// scratch - some 0.45 MB a block in all (measured, see pt_flow_max_blocks).
	const NvFlowGridDesc grid_desc = flow_grid_desc();

	flow.grid = flow.grid_iface->createGrid(flow.context_iface, flow.context, flow.op_list, flow.ext_op_list, &grid_desc);
	flow.params_named = flow.params_iface->createGridParamsNamed("q2rtx");
	flow.grid_params = flow.params_named ? flow.params_iface->mapGridParamsNamed(flow.params_named) : NULL;

	if (!flow.grid || !flow.grid_params) {
		Com_EPrintf("NVIDIA Flow: could not create the grid.\n");
		flow_destroy_simulation();
		return;
	}

	flow.sim_time = 0.0;
	flow.have_last_time = false;
	flow.idle = true;
	flow.force_clear = false;
	flow.warm_pending = false;      // the map's registration asks for it (vkpt_flow_reset)
	flow.warm_now = false;
	flow.step_error = 0.f;
	flow.num_blocks = 0;
	flow.max_blocks = grid_desc.maxLocations;
	flow.pool_reset_again = false;
	flow.pool_settle = 0;
	flow.relay_step = -1;
	flow.relaid = false;
	flow.heatbeam_clear = false;

	Cvar_FullSet("flow_available", "1", CVAR_ROM, FROM_CODE);
	Com_Printf("NVIDIA Flow: ready on %s, up to %u blocks.\n", vkpt_flow_backend ? "D3D12" : "Vulkan",
		grid_desc.maxLocations);
}

static void flow_release_set(int set)
{
	if (flow.set_density[set])
		flow.context_iface->destroyTexture(flow.context, flow.set_density[set]);
	if (flow.set_table[set])
		flow.context_iface->destroyBuffer(flow.context, flow.set_table[set]);
	flow.set_density[set] = NULL;
	flow.set_table[set] = NULL;
	flow.light_sum_blocks[set] = 0;
}

static void flow_destroy_simulation(void)
{
	if (flow.device) {
		NvFlowUint64 flushed_frame;

		flow.device_iface->waitIdle(flow.queue);

		for (int set = 0; set < FLOW_SET_COUNT; set++)
			flow_release_set(set);

		if (flow.grid)
			flow.grid_iface->destroyGrid(flow.context, flow.grid);
		if (flow.params_named)
			flow.params_iface->destroyGridParamsNamed(flow.params_named);

		// what destroying the grid recorded
		flow.device_iface->flush(flow.queue, &flushed_frame, NULL, NULL);
		flow.device_iface->waitIdle(flow.queue);

		flow.device_iface->destroyDevice(flow.device_manager, flow.device);
	}
	if (flow.device_manager)
		flow.device_iface->destroyDeviceManager(flow.device_manager);

	flow.device_manager = NULL;
	flow.device = NULL;
	flow.queue = NULL;
	flow.context_iface = NULL;
	flow.context = NULL;
	flow.grid = NULL;
	flow.params_named = NULL;
	flow.grid_params = NULL;

	Cvar_FullSet("flow_available", "0", CVAR_ROM, FROM_CODE);
}

/*
==============================================================================

LIGHT

==============================================================================
*/

static float flow_luminance(const vec3_t c)
{
	return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}

static void flow_free_lights(void)
{
	for (int i = 0; i < FLOW_MAX_LIGHTS; i++)
		flow.lights[i].layer = -1;
	flow.num_dlights = 0;
	for (int l = 0; l < FLOW_LAYERS; l++)
		flow.layer_shadow[l] = -1;
}

// A light slot for a burst of `layer` at `origin`, or -1 for none. A burst
// close to one only just started shares its light: a rocket's splash and the
// rocket are one fire.
static int flow_take_light(int layer, const vec3_t origin, float scale)
{
	if (!cvar_pt_flow_lighting->integer || (layer != R_FLOW_FIRE && layer != R_FLOW_PLASMA))
		return -1;

	for (int i = 0; i < FLOW_MAX_LIGHTS; i++) {
		const flow_light_t *l = &flow.lights[i];
		if (l->layer == layer && !l->burn && l->age < 0.5f && Distance(l->seed, origin) < 48.f * scale)
			return i;
	}

	int slot = -1;

	// a free slot; or something on fire's, which takes one again once one is
	// free (flow_burn_light); or the oldest
	int burning = -1;
	float oldest = -1.f;
	for (int i = 0; i < FLOW_MAX_LIGHTS; i++) {
		const flow_light_t *l = &flow.lights[i];
		if (l->layer < 0) {
			slot = i;
			break;
		}
		if (l->burn) {
			if (burning < 0)
				burning = i;
		} else if (l->age > oldest) {
			oldest = l->age;
			slot = i;
		}
	}
	if ((slot < 0 || flow.lights[slot].layer >= 0) && burning >= 0)
		slot = burning;

	flow_light_t *l = &flow.lights[slot];
	l->layer = layer;
	l->generation++;
	VectorCopy(origin, l->seed);
	l->gather = 96.f * scale;
	l->age = 0.f;
	l->strength = layer == R_FLOW_PLASMA ? cvar_pt_flow_bfg_light->value : cvar_pt_flow_light->value;
	l->follow = false;
	l->burn = false;
	l->lit = false;
	return slot;
}

// pt_flow_heatbeam_light: 1 on, 0 off. The client sets it to match
// cl_flow_heatbeam_fire (archived) at start and whenever that changes
// (cl_flow_heatbeam_fire_changed in client/tent.c) - Matt (11-12): it did not
// save, it goes with the fire, and it should read 1 then, not the -1 it was.
static bool flow_heatbeam_light_on(void)
{
	return cvar_pt_flow_heatbeam_light->integer > 0;
}

// Whether things on fire cast light (pt_flow_heatbeam_light, which gates the
// plasma beam's light too, and pt_flow_burn_light).
static bool flow_burn_lighting(void)
{
	return cvar_pt_flow_lighting->integer && flow_heatbeam_light_on() &&
		cvar_pt_flow_burn_light->value > 0.f;
}

// A light slot for something on fire at `origin`, gathering its glow from
// `gather` units round it, or -1 for none: a free slot only, and no more than
// half of them. A fire keeps its light for as long as it burns - a burning
// body for seconds, a wall the plasma beam is on for as long as it is held -
// so fires must not crowd out the explosions, which take a fire's slot
// before any other (flow_take_light).
static int flow_take_burn_light(const vec3_t origin, float gather)
{
	if (!flow_burn_lighting())
		return -1;

	int slot = -1, burning = 0;
	for (int i = 0; i < FLOW_MAX_LIGHTS; i++) {
		const flow_light_t *l = &flow.lights[i];
		if (l->layer < 0) {
			if (slot < 0)
				slot = i;
		} else if (l->burn) {
			burning++;
		}
	}
	if (slot < 0 || burning >= FLOW_MAX_LIGHTS / 2)
		return -1;

	flow_light_t *l = &flow.lights[slot];
	l->layer = R_FLOW_FIRE;
	l->generation++;
	VectorCopy(origin, l->seed);
	l->gather = gather;
	l->age = 0.f;
	l->strength = cvar_pt_flow_burn_light->value;
	l->follow = true;
	l->burn = true;
	l->lit = false;
	return slot;
}

/*
The sums flow_lights.comp wrote for `set` two frames ago, read now that the
frame has passed its fence: each slot's light goes to the glow-weighted middle
of its blocks, as wide as the glow is spread and as bright as all of it, and
its seed follows. A slot whose blocks have stopped glowing is given up. Then
the lights are listed for the scene, brightest first. Once a frame, before
the frame's own sums are dispatched into the same set.
*/
static void flow_lights_collect(int set)
{
	if (flow.lights_collected_frame == qvk.frame_counter + 1)
		return;
	flow.lights_collected_frame = qvk.frame_counter + 1;

	if (!cvar_pt_flow_lighting->integer) {
		flow_free_lights();
		flow.light_sum_blocks[set] = 0;
		return;
	}

	// glow rgb, glow-weighted position from the seed and glow, glow-weighted
	// squared distance from the seed
	double sums[FLOW_MAX_LIGHTS][8];
	bool gathered[FLOW_MAX_LIGHTS];
	memset(sums, 0, sizeof(sums));

	const uint32_t blocks = flow.light_sum_blocks[set];
	flow.light_sum_blocks[set] = 0;
	for (int i = 0; i < FLOW_MAX_LIGHTS; i++)
		gathered[i] = blocks > 0 && flow.lights[i].layer >= 0 &&
			flow.light_sum_generation[set][i] == flow.lights[i].generation;

	const vec4_t *src = (const vec4_t *)(flow.light_sums_mapped + set * flow.light_sums_stride);
	for (uint32_t b = 0; b < blocks; b++, src += FLOW_LIGHT_SUM_VEC4S) {
		const int slot = (int)src[0][3];
		if (slot < 0 || slot >= FLOW_MAX_LIGHTS || !gathered[slot])
			continue;
		double *s = sums[slot];
		s[0] += src[0][0];
		s[1] += src[0][1];
		s[2] += src[0][2];
		s[3] += src[1][0];
		s[4] += src[1][1];
		s[5] += src[1][2];
		s[6] += src[1][3];
		s[7] += src[2][0];
	}

	const float size = max(cvar_pt_flow_light_size->value, 0.f);
	const float max_radius = max(cvar_pt_flow_light_radius->value, 4.f);

	for (int i = 0; i < FLOW_MAX_LIGHTS; i++) {
		flow_light_t *l = &flow.lights[i];
		if (l->layer < 0)
			continue;

		if (l->age > 12.f) {
			l->layer = -1;
			continue;
		}
		if (!gathered[i])
			continue;

		const double *s = sums[i];
		if (s[6] <= 0.0) {
			// nothing glows near it any more - or nothing yet, for a moment
			if (l->age > 0.3f)
				l->layer = -1;
			l->lit = false;
			continue;
		}

		const double inv_w = 1.0 / s[6];
		vec3_t middle = { (float)(s[3] * inv_w), (float)(s[4] * inv_w), (float)(s[5] * inv_w) };
		const double variance = s[7] * inv_w - DotProduct(middle, middle);
		const float spread = (float)sqrt(max(variance, 0.0));

		if (l->follow) {
			VectorAdd(middle, l->seed, l->origin);
		} else {
			VectorAdd(middle, flow.light_sum_seed[set][i], l->origin);
			VectorCopy(l->origin, l->seed);
			l->gather = max(96.f, 3.f * spread);
		}
		l->radius = min(max(size * spread, 4.f), max_radius);

		const float power = FLOW_LIGHT_UNIT * max(l->strength, 0.f) / (l->radius * l->radius);
		VectorSet(l->radiance, (float)s[0] * power, (float)s[1] * power, (float)s[2] * power);
		l->lit = true;
	}

	// The lights, brightest first.
	flow.num_dlights = 0;
	for (int l = 0; l < FLOW_LAYERS; l++)
		flow.layer_shadow[l] = -1;

	for (int i = 0; i < FLOW_MAX_LIGHTS; i++) {
		const flow_light_t *l = &flow.lights[i];
		if (l->layer < 0 || !l->lit)
			continue;
		// Below this a light lights nothing visible: a classic rocket light
		// is 1600 at its widest.
		const float power = flow_luminance(l->radiance) * l->radius * l->radius;
		if (power < 4.f)
			continue;

		int at = flow.num_dlights;
		while (at > 0) {
			const flow_light_t *o = &flow.lights[flow.light_slot[at - 1]];
			if (flow_luminance(o->radiance) * o->radius * o->radius >= power)
				break;
			flow.light_slot[at] = flow.light_slot[at - 1];
			at--;
		}
		flow.light_slot[at] = i;
		flow.num_dlights++;
	}

	for (int n = 0; n < flow.num_dlights; n++) {
		const flow_light_t *l = &flow.lights[flow.light_slot[n]];
		dlight_t *dl = &flow.dlights[n];
		memset(dl, 0, sizeof(*dl));
		VectorCopy(l->origin, dl->origin);
		VectorCopy(l->radiance, dl->color);
		dl->intensity = 25.f;   // add_dlights takes color x intensity / 25
		dl->radius = l->radius;
		dl->volumetric_scale = LIGHT_VOLUMETRIC_SCALE_UNSET;
		dl->light_type = DLIGHT_SPHERE;

		if (flow.layer_shadow[l->layer] < 0)
			flow.layer_shadow[l->layer] = n;
		// a rocket's trail is the fire's gas on a layer of its own
		if (l->layer == R_FLOW_FIRE && flow.layer_shadow[FLOW_LAYER_FIRE_TRAIL] < 0)
			flow.layer_shadow[FLOW_LAYER_FIRE_TRAIL] = n;
	}
}

// The light slots and lights as flow.glsl and flow_lights.comp see them.
static void flow_lights_params(int set, FlowParams *params)
{
	params->counts[1] = (uint32_t)flow.num_dlights;
	for (int n = 0; n < flow.num_dlights; n++) {
		const dlight_t *dl = &flow.dlights[n];
		VectorCopy(dl->origin, params->lights[n]);
		params->lights[n][3] = dl->radius;
		// what smoke at distance r scatters is albedo x this / r^2: an
		// isotropic scatterer sees radiance x pi radius^2 / r^2 of a sphere
		// light, a 4 pi'th of which goes each way
		VectorScale(dl->color, 0.25f * dl->radius * dl->radius, params->light_colors[n]);
	}

	for (int i = 0; i < FLOW_MAX_LIGHTS; i++) {
		const flow_light_t *l = &flow.lights[i];
		if (l->layer < 0) {
			flow.light_sum_generation[set][i] = 0;
			continue;
		}
		VectorCopy(l->seed, params->seeds[i]);
		params->seeds[i][3] = l->gather;
		params->seed_layers[i][0] = (uint32_t)NvFlow_packLayerAndLevel(l->layer, 0);
		// a fire's light is its rocket's trail's as well, on a layer of its own
		params->seed_layers[i][1] = (uint32_t)NvFlow_packLayerAndLevel(l->layer == R_FLOW_FIRE ? FLOW_LAYER_FIRE_TRAIL : l->layer, 0);
		flow.light_sum_generation[set][i] = l->generation;
		VectorCopy(l->seed, flow.light_sum_seed[set][i]);
	}
}

// Marks the bricks of every block that the march needs to sample, for this
// frame's rays. Flow's own submission is already in the queue ahead of this.
static void flow_occupancy_dispatch(int set, uint32_t blocks)
{
	if (!flow.occupancy_pipeline || blocks == 0)
		return;
	// never past the blocks' part of the buffer, which has room for the most
	// the grid can have
	blocks = min(blocks, flow.occupancy_blocks);

	VkCommandBuffer cmd_buf = vkpt_begin_command_buffer(&qvk.cmd_buffers_graphics);

	// The group and block bitsets and the screen tiles are only ever ORed
	// into, so they start each frame clear.
	const size_t uvec4_size = 4 * sizeof(uint32_t);
	vkCmdFillBuffer(cmd_buf, flow.occupancy.buffer,
		set * flow.occupancy_stride + flow.occupancy_blocks * uvec4_size,
		FLOW_BITSET_UVEC4S * uvec4_size, 0);
	VkMemoryBarrier cleared = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
	};
	vkCmdPipelineBarrier(cmd_buf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0, 1, &cleared, 0, NULL, 0, NULL);

	VkDescriptorSet desc_sets[] = {
		qvk.desc_set_ubo,
		qvk_get_current_desc_set_textures(),
	};
	vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, flow.occupancy_pipeline);
	vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE,
		flow.lights_pipeline_layout, 0, LENGTH(desc_sets), desc_sets, 0, NULL);
	vkCmdDispatch(cmd_buf, blocks, 1, 1);

	// for the ray-gen shaders, ray query or pipeline, and anything else
	VkMemoryBarrier barrier = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	};
	vkCmdPipelineBarrier(cmd_buf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		0, 1, &barrier, 0, NULL, 0, NULL);

	vkpt_submit_command_buffer_simple(cmd_buf, qvk.queue_graphics, true);
}

// Whether flow_lights.comp has anything to add up: blocks, and a light slot in use.
static bool flow_lights_wanted(uint32_t blocks)
{
	if (blocks == 0)
		return false;
	bool any = false;
	for (int i = 0; i < FLOW_MAX_LIGHTS; i++)
		any = any || flow.lights[i].layer >= 0;
	return any;
}

// Adds up the glow round each light slot's seed, for flow_lights_collect two
// frames on. Flow's own submission is already in the queue ahead of this.
static void flow_lights_dispatch(int set, uint32_t blocks)
{
	flow.light_sum_blocks[set] = 0;
	if (!flow.lights_pipeline || !flow_lights_wanted(blocks))
		return;

	VkCommandBuffer cmd_buf = vkpt_begin_command_buffer(&qvk.cmd_buffers_graphics);

	VkDescriptorSet desc_sets[] = {
		qvk.desc_set_ubo,
		qvk_get_current_desc_set_textures(),
	};
	vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, flow.lights_pipeline);
	vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE,
		flow.lights_pipeline_layout, 0, LENGTH(desc_sets), desc_sets, 0, NULL);
	vkCmdDispatch(cmd_buf, blocks, 1, 1);

	VkMemoryBarrier barrier = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
	};
	vkCmdPipelineBarrier(cmd_buf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
		0, 1, &barrier, 0, NULL, 0, NULL);

	vkpt_submit_command_buffer_simple(cmd_buf, qvk.queue_graphics, true);
	flow.light_sum_blocks[set] = blocks;
}

/*
==============================================================================

COLLISION

==============================================================================
*/

/*
Cuts the world's triangles down to those near the gas - the blocks Flow had
last frame and round what the emitters are about to add - and makes them
Flow's collision mesh. False when there is nothing to collide with.

The faces' front, cross(pos1 - pos0, pos2 - pos0), is the side the player is
on, so Flow's signed distance is negative inside the walls. The band where the
gas is held still runs pt_flow_collision_depth units in and only
pt_flow_collision_margin out: in front of a face it leaves a gap, and the gas
crosses it only by soaking through the band. Measured headless against a
floor, a ceiling and a wall: 12 in and 3 out lets none through the floor or
the ceiling and a 0.06% trace through the wall, where +-4.5 let 40% through
the ceiling (hot gas presses up into it harder than into anything else).
*/
static bool flow_collision_update(void)
{
	if (!cvar_pt_flow_collision->integer || flow.num_world_tris == 0)
		return false;

	vec3_t mins, maxs;
	ClearBounds(mins, maxs);
	if (flow.have_blocks) {
		AddPointToBounds(flow.blocks_min, mins, maxs);
		AddPointToBounds(flow.blocks_max, mins, maxs);
	}
	for (int i = 0; i < flow.num_emitters; i++) {
		const flow_emitter_t *e = &flow.emitters[i];
		const float reach = 128.f * e->scale;
		vec3_t p;
		for (int k = 0; k < 3; k++) p[k] = e->origin[k] - reach;
		AddPointToBounds(p, mins, maxs);
		for (int k = 0; k < 3; k++) p[k] = e->origin[k] + reach;
		AddPointToBounds(p, mins, maxs);
		if (e->kind == R_FLOW_BEAM)
			AddPointToBounds(e->end, mins, maxs);
	}
	// the warm-up's emitters, for Flow to make the mesh's buffers too
	if (flow.warm_now) {
		vec3_t p;
		for (int k = 0; k < 3; k++) p[k] = flow.warm_origin[k] - 128.f;
		AddPointToBounds(p, mins, maxs);
		for (int k = 0; k < 3; k++) p[k] = flow.warm_origin[k] + 128.f;
		AddPointToBounds(p, mins, maxs);
	}
	if (mins[0] > maxs[0])
		return false;

	int region[6];
	for (int k = 0; k < 3; k++) {
		region[k] = (int)floorf((mins[k] - 96.f) / FLOW_MESH_STEP);
		region[k + 3] = (int)ceilf((maxs[k] + 96.f) / FLOW_MESH_STEP);
	}

	if (!flow.mesh_region_valid || memcmp(region, flow.mesh_region, sizeof(region))) {
		memcpy(flow.mesh_region, region, sizeof(region));
		flow.mesh_region_valid = true;

		vec3_t box_min, box_max;
		for (int k = 0; k < 3; k++) {
			box_min[k] = region[k] * FLOW_MESH_STEP;
			box_max[k] = region[k + 3] * FLOW_MESH_STEP;
		}

		int count = 0;
		for (int t = 0; t < flow.num_world_tris && count < FLOW_MAX_MESH_TRIS; t++) {
			const float *v = flow.world_tris + t * 9;
			bool outside = false;
			for (int k = 0; k < 3 && !outside; k++) {
				const float lo = min(v[k], min(v[k + 3], v[k + 6]));
				const float hi = max(v[k], max(v[k + 3], v[k + 6]));
				outside = hi < box_min[k] || lo > box_max[k];
			}
			if (outside)
				continue;

			if (count == flow.mesh_capacity) {
				flow.mesh_capacity = flow.mesh_capacity ? flow.mesh_capacity * 2 : 4096;
				flow.mesh_positions = Z_Realloc(flow.mesh_positions, sizeof(NvFlowFloat3) * 3 * flow.mesh_capacity);
				flow.mesh_indices = Z_Realloc(flow.mesh_indices, sizeof(int) * 3 * flow.mesh_capacity);
				flow.mesh_counts = Z_Realloc(flow.mesh_counts, sizeof(int) * flow.mesh_capacity);
			}
			for (int j = 0; j < 3; j++) {
				NvFlowFloat3 *p = &flow.mesh_positions[count * 3 + j];
				p->x = v[j * 3 + 0];
				p->y = v[j * 3 + 1];
				p->z = v[j * 3 + 2];
				flow.mesh_indices[count * 3 + j] = count * 3 + j;
			}
			flow.mesh_counts[count] = 3;
			count++;
		}

		flow.num_mesh_tris = count;
		flow.mesh_version++;
	}

	if (flow.num_mesh_tris == 0)
		return false;

	NvFlowGridEmitterMeshParams *mesh = &flow.mesh;
	*mesh = NvFlowEmitterMeshParams_default;
	mesh->luid = 0x300;
	mesh->minDistance = -max(cvar_pt_flow_collision_depth->value, 0.f);
	mesh->maxDistance = max(cvar_pt_flow_collision_margin->value, 0.f);
	// Never makes blocks of its own: only where there is gas already.
	mesh->allocateMask = NV_FLOW_FALSE;
	// Still air, everywhere in the band, at once; nothing else is touched,
	// which also keeps Flow from running the mesh on the density at all.
	mesh->velocityIsWorldSpace = NV_FLOW_TRUE;
	mesh->velocity.x = mesh->velocity.y = mesh->velocity.z = 0.f;
	mesh->divergence = 0.f;
	mesh->coupleRateVelocity = 1000.f;
	mesh->coupleRateDivergence = 1000.f;
	mesh->coupleRateTemperature = 0.f;
	mesh->coupleRateFuel = 0.f;
	mesh->coupleRateBurn = 0.f;
	mesh->coupleRateSmoke = 0.f;
	mesh->meshPositions = flow.mesh_positions;
	mesh->meshPositionCount = (NvFlowUint64)flow.num_mesh_tris * 3;
	mesh->meshPositionVersion = flow.mesh_version;
	mesh->meshFaceVertexIndices = flow.mesh_indices;
	mesh->meshFaceVertexIndexCount = (NvFlowUint64)flow.num_mesh_tris * 3;
	mesh->meshFaceVertexIndexVersion = flow.mesh_version;
	mesh->meshFaceVertexCounts = flow.mesh_counts;
	mesh->meshFaceVertexCountCount = (NvFlowUint64)flow.num_mesh_tris;
	mesh->meshFaceVertexCountVersion = flow.mesh_version;
	// After the pressure solve, which knows nothing of walls and would
	// otherwise put the flow straight back through them.
	mesh->applyPostPressure = NV_FLOW_TRUE;
	// Every layer with physicsCollisionEnabled, one mesh for all.
	mesh->isPhysicsCollision = NV_FLOW_TRUE;
	flow.mesh_ptr = mesh;
	return true;
}

/*
==============================================================================

THE FRAME

==============================================================================
*/

// Flow's layer for each r_flow_style_t is the style's own number; after them
// comes the far part of the railgun's trail, which only flow_rail_boxes puts
// anything in, the rocket's fire trail, and your own plasma beam's two
// first-person layers (r_flow_emit_t.view).
typedef char flow_layers_match_styles[(R_FLOW_STYLES + 4 == FLOW_LAYERS) ? 1 : -1];
typedef char flow_rail_layer_matches[(R_FLOW_RAIL == FLOW_LAYER_RAIL) ? 1 : -1];
typedef char flow_heatbeam_layer_matches[(R_FLOW_HEATBEAM == FLOW_LAYER_HEATBEAM) ? 1 : -1];
typedef char flow_rail_far_layer_after_styles[(FLOW_LAYER_RAIL_FAR == R_FLOW_STYLES) ? 1 : -1];
typedef char flow_fire_trail_layer_after_rail_far[(FLOW_LAYER_FIRE_TRAIL == R_FLOW_STYLES + 1) ? 1 : -1];
typedef char flow_heatbeam_view_layers_last[(FLOW_LAYER_HEATBEAM_VIEW == R_FLOW_STYLES + 2 && FLOW_LAYER_HEATBEAM_MIRROR == R_FLOW_STYLES + 3) ? 1 : -1];

// A layer's cell size in world units: pt_flow_cell_size, the railgun trail's
// own finer pt_flow_rail_cell_size, pt_flow_rail_far_cell_size for its far
// part, or the plasma beam's pt_flow_heatbeam_cell_size (FLOW_GRIDS in
// shader/flow.h).
static float flow_layer_cell_size(int layer)
{
	switch (FLOW_LAYER_GRID(layer)) {
	case 1:
		return max(cvar_pt_flow_rail_cell_size->value, 0.5f);
	case 2:
		return max(cvar_pt_flow_rail_far_cell_size->value, 0.5f);
	case 3:
		return max(cvar_pt_flow_heatbeam_cell_size->value, 0.5f);
	default:
		return max(cvar_pt_flow_cell_size->value, 0.5f);
	}
}

// How many times slower than as tuned a railgun trail fades: the menu's
// "railgun trail fade time", pt_flow_rail_fade seconds, over the
// pt_flow_rail_life it was tuned to (1.4). Never faster than that - the
// tuned fade is the shortest, picked for what a shot costs (Matt, 11-12:
// "make the current fade out length the default but ... users can make the
// fade out time longer"). Everything that says how long the trail lasts is
// stretched by it alike - its cooling, the smoke's damping and fade, its
// rise, and its end - so it fades as it did, only slower.
static float flow_rail_slow(void)
{
	const float life = max(cvar_pt_flow_rail_life->value, 0.1f);
	return min(max(cvar_pt_flow_rail_fade->value / life, 1.f), 10.f);
}

// The railgun layers' cooling rate and smoke damping (the share of it lost
// each second) at that pace: the share left after `slow` seconds is what was
// left after one.
static float flow_rail_cooling(float slow)
{
	return max(cvar_pt_flow_rail_cooling->value, 0.1f) / slow;
}

static float flow_rail_smoke_damping(float slow)
{
	const float damping = Cvar_ClampValue(cvar_pt_flow_rail_smoke_damping, 0.f, 1.f);
	return 1.f - powf(1.f - damping, 1.f / slow);
}

// The railgun trails' end: pt_flow_rail_life after the last shot nothing is
// left of them, so that every block is free before the railgun can fire
// again (1.5 s). Over the pt_flow_rail_drain before that, what can be left
// of the smoke and the heat - the most a shot puts in, followed down here as
// Flow takes it off - fades evenly to nothing, the faintest first. Fading
// at a rate meant for a whole puff, as at first, emptied the fifth of one
// that was left in a twentieth of a second, and the last of the glow with
// it: the trail cut off at 1.05 s rather than fading, where left alone it
// lasted to 1.5 s (Matt, 10-27). Both times, and the rates, stretched by
// flow_rail_slow: a longer fade than the tuned one holds blocks past the
// refire. Sets this frame's fades, then moves the age and what is left on by
// `dt`.
static void flow_rail_fade(float dt)
{
	const float slow = flow_rail_slow();
	const float left = max(cvar_pt_flow_rail_life->value, 0.f) * slow - flow.rail_age;

	flow.rail_smoke_fade = max(cvar_pt_flow_rail_smoke_fade->value, 0.f) / slow;
	flow.rail_heat_fade = 0.f;
	if (left <= 0.f) {
		// time is up: whatever is left goes in a step
		flow.rail_smoke_fade = 1000.f;
		flow.rail_heat_fade = 1000.f;
	}
	else if (left < cvar_pt_flow_rail_drain->value * slow) {
		flow.rail_smoke_fade = max(flow.rail_smoke_fade, flow.rail_smoke / left);
		flow.rail_heat_fade = flow.rail_heat / left;
	}

	if (dt <= 0.f)
		return;
	flow.rail_age = min(flow.rail_age + dt, 1e6f);

	// While a shot still emits, what it puts in holds.
	if (flow.rail_age <= max(cvar_pt_flow_rail_burst->value, 0.f))
		return;

	// As Flow has it (Advection.cpp, AdvectionCommon.hlsli): smoke loses the
	// larger of its damping and its fade; heat cools by coolingRate of
	// itself, then fades.
	const float damping = flow_rail_smoke_damping(slow);
	const float smoke_loss = max(flow.rail_smoke * (1.f - powf(1.f - damping, dt)), flow.rail_smoke_fade * dt);
	flow.rail_smoke = max(flow.rail_smoke - smoke_loss, 0.f);
	flow.rail_heat -= flow_rail_cooling(slow) * dt * flow.rail_heat;
	flow.rail_heat = max(flow.rail_heat - flow.rail_heat_fade * dt, 0.f);
}

/* THE PLASMA BEAM, LEAN (pt_flow_heatbeam_lean 1, the default). With no trails
   (pt_flow_heatbeam_trails 0) a step damps all of the beam's gas away before
   its emitters lay it again (flow_layer_simulate), so what is drawn is only
   what the emitters laid that step. Everything Flow does to move gas is lost
   on it: the advection, the vorticity and the pressure solve on its blocks,
   the velocity its boxes push, and the blocks Flow's summary keeps round gas
   for it to move into (each block with gas, and its neighbours, until a
   readback a few frames old says otherwise). None of it reaches a cell the
   renderer reads; and the renderer reads no velocity. So, for its layers:
   - no blocks from the summary - an emitter allocates the blocks it lays
     gas in, in the same step (EmitterBoxAllocate);
   - no core simulation (Flow's forceDisableCoreSimulation), the density
     only faded (forceFadeEnabled: AdvectionFadeDensityCS, which damps it
     away as the advection did). Flow skips the advection, vorticity and
     pressure passes when no layer with blocks wants them, so this pays only
     while the beam's gas is all there is; with a fire burning beside it
     they run for every block as before;
   - no velocity or divergence from its boxes and hot spot
     (flow_emit_no_velocity): Flow leaves an emitter that couples neither
     out of its velocity passes (EmitterBox.cpp).
   0 = Flow does it all, as before (for an A/B). */
static bool flow_heatbeam_lean(void)
{
	return !cvar_pt_flow_heatbeam_trails->integer && cvar_pt_flow_heatbeam_lean->integer;
}

static void flow_emit_no_velocity(int first_box, int num_boxes, int first_sphere, int num_spheres)
{
	for (int i = first_box; i < num_boxes; i++) {
		flow.boxes[i].coupleRateVelocity = 0.f;
		flow.boxes[i].coupleRateDivergence = 0.f;
	}
	for (int i = first_sphere; i < num_spheres; i++) {
		flow.spheres[i].coupleRateVelocity = 0.f;
		flow.spheres[i].coupleRateDivergence = 0.f;
	}
}

// How one layer's gas behaves, starting from Flow's defaults.
static void flow_layer_simulate(NvFlowGridSimulateLayerParams *simulate, int layer, bool collide)
{
	*simulate = NvFlowGridSimulateLayerParams_default;
	simulate->luid = 1 + layer;
	simulate->layer = layer;
	simulate->densityCellSize = flow_layer_cell_size(layer);
	// Below pt_flow_rate frames a second every frame steps by its own time,
	// so the gas keeps up with the game; flow_simulate caps how big a step
	// gets. Above it, Flow's own stepper steps pt_flow_rate times a second,
	// on some frames and not others - a frame with no step costs next to
	// nothing, and draws the last one.
	// Your own plasma beam on: one step a frame, of the frame's time
	// (flow_every_frame).
	const float rate = cvar_pt_flow_rate->value;
	if (flow.every_frame) {
		simulate->enableVariableTimeStep = NV_FLOW_TRUE;
		simulate->maxStepsPerSimulate = 1;
	}
	else if (rate > 0.f && flow.frame_dt > 0.f && flow.frame_dt < 1.f / rate) {
		simulate->enableVariableTimeStep = NV_FLOW_FALSE;
		simulate->stepsPerSecond = rate;
		simulate->maxStepsPerSimulate = 1;
	}
	else {
		simulate->enableVariableTimeStep = NV_FLOW_TRUE;
		simulate->maxStepsPerSimulate = (NvFlowUint)Cvar_ClampInteger(cvar_pt_flow_substeps, 1, 4);
	}
	// The warm-up (flow_warm_emitters): exactly one step, whatever the frame
	// rate and Flow's own stepper say - a frame with none would make nothing.
	if (flow.warm_now) {
		simulate->enableVariableTimeStep = NV_FLOW_TRUE;
		simulate->maxStepsPerSimulate = 1;
	}
	simulate->forceClear = flow.force_clear;
	// Laying a plasma beam again under a pause (flow_heatbeam_relay): one
	// step a frame, of flow.relay_dt; the beam's layer cleared first, then
	// stepped though there is no game time; every other layer's emitters
	// off, so that only its gas changes - Flow cannot step one layer alone
	// (its advection takes every layer by the same time), which is what
	// relay_dt is chosen for.
	if (flow.relay_step >= 0) {
		simulate->enableVariableTimeStep = NV_FLOW_TRUE;
		simulate->maxStepsPerSimulate = 1;
		if (FLOW_LAYER_IS_HEATBEAM(layer)) {
			if (flow.relay_step == 0)
				simulate->forceClear = NV_FLOW_TRUE;
			else
				simulate->simulateWhenPaused = NV_FLOW_TRUE;
		}
		else {
			simulate->forceDisableEmitters = NV_FLOW_TRUE;
		}
	}
	// ...and once time runs again, what was laid goes: the beam is back where
	// the one firing sees it, and builds again in a step
	if (flow.heatbeam_clear && FLOW_LAYER_IS_HEATBEAM(layer))
		simulate->forceClear = NV_FLOW_TRUE;
	/* A layer's forceClear does not empty it. All it does is stop Flow asking
	   for the layer's blocks (Summary.cpp), and a block nobody asks for is kept
	   for blockMinLifetime more simulations, gas and all (Sparse.cpp) - the
	   most any layer has, so every layer's 4 kept it. The re-lay is one or a
	   few simulations, then the pause holds whatever is there: the first-person
	   beam stayed where it was, the re-laid one under it - two beams (Matt,
	   11-09). Flow gives a cleared layer no lifetime only when every layer
	   clears; so, on a frame that clears the plasma beam, none has any. What
	   that drops of the others is only blocks Flow's last summary did not
	   want - under a pause, where nothing has emitted since, empty ones. */
	if ((flow.relay_step == 0 || flow.heatbeam_clear) && !flow.force_clear)
		simulate->blockMinLifetime = 0;
	simulate->vorticity.forceScale = cvar_pt_flow_vorticity->value;
	simulate->physicsCollisionEnabled = collide ? NV_FLOW_TRUE : NV_FLOW_FALSE;

	// Quake is Z-up like Flow, and Flow's gravity of -100 is about right
	// in Quake units: with buoyancy 2 a cell at temperature 1 accelerates
	// upward at 200 units/s^2.
	NvFlowAdvectionCombustionParams *advection = &simulate->advection;

	switch (layer) {
	case R_FLOW_FIRE:
	case FLOW_LAYER_FIRE_TRAIL:     // the same gas, but for how bright it glows
		advection->buoyancyPerTemp = cvar_pt_flow_buoyancy->value;
		advection->coolingRate = max(cvar_pt_flow_cooling->value, 0.f);
		advection->smokePerBurn = max(cvar_pt_flow_smoke_per_burn->value, 0.f);
		// Smoke loses the larger of `damping` of itself and `fade` each
		// second: the first thins a thick cloud quickly, the second finishes
		// off the thin wisps that would otherwise hang about.
		advection->smoke.damping = Cvar_ClampValue(cvar_pt_flow_smoke_damping, 0.f, 1.f);
		advection->smoke.fade = max(cvar_pt_flow_smoke_fade->value, 0.f);
		// Flow's fuel never goes but by burning, and what the fireball did
		// not burn smoulders on just above the ignition temperature: no glow,
		// smoke too thin to see, and blocks kept for seconds. Fading it takes
		// a quarter off the blocks a rocket has at its most (64 to 48);
		// fading it faster (0.5: 44) also cools the fireball's heart sooner.
		advection->fuel.fade = max(cvar_pt_flow_fuel_fade->value, 0.f);
		break;

	case R_FLOW_PLASMA:
		// Heat without fuel: nothing burns, it only glows as it cools, and
		// leaves its smoke hanging after - about 3.5 s at the defaults.
		advection->buoyancyPerTemp = cvar_pt_flow_bfg_buoyancy->value;
		advection->coolingRate = max(cvar_pt_flow_bfg_cooling->value, 0.1f);
		advection->smoke.damping = Cvar_ClampValue(cvar_pt_flow_bfg_smoke_damping, 0.f, 1.f);
		advection->smoke.fade = max(cvar_pt_flow_bfg_smoke_fade->value, 0.f);
		// Strong vorticity tears the blast's puffs into ragged green smoke
		// within half a second; 30% of the speed going each second keeps
		// what the puffs were thrown out with from carrying the cloud off.
		simulate->vorticity.forceScale = max(cvar_pt_flow_bfg_vorticity->value, 0.f);
		advection->velocity.damping = 0.3f;
		break;

	case R_FLOW_BUBBLES:
		// No heat and nothing to burn: the bubbles are smoke that floats.
		// Combustion stays on all the same - it is also what clears the
		// expansion an emitter leaves in the velocity, and without it that
		// stays and spreads until the layer has every block there is.
		advection->buoyancyPerSmoke = max(cvar_pt_flow_bubble_rise->value, 0.f);
		advection->buoyancyMaxSmoke = 1.f;
		// Water drags: half the speed goes each second, so the bubbles
		// settle to a steady rise instead of gathering speed to the surface.
		advection->velocity.damping = 0.5f;
		advection->smoke.damping = 0.3f;
		advection->smoke.fade = max(cvar_pt_flow_bubble_fade->value, 0.f);
		break;

	case R_FLOW_RAIL:
	case FLOW_LAYER_RAIL_FAR:
		// Heat without fuel, as the plasma: the spiral glows for as long as
		// it takes to cool - about the second the classic particles last -
		// rising a little, then leaves pale wisps that the vorticity curls.
		// Its far part is the same gas, on coarser cells. All of it slower
		// by flow_rail_slow - its rise too, as the heat that lifts it stays
		// that much longer.
		{
			const float slow = flow_rail_slow();
			advection->buoyancyPerTemp = max(cvar_pt_flow_rail_buoyancy->value, 0.f) / slow;
			advection->coolingRate = flow_rail_cooling(slow);
			advection->smoke.damping = flow_rail_smoke_damping(slow);
		}
		simulate->vorticity.forceScale = max(cvar_pt_flow_rail_vorticity->value, 0.f);
		advection->velocity.damping = 0.5f;
		// Speed alone keeps no block: Flow keeps one while its gas moves and
		// has any smoke at all, and the swirl moves it for seconds - a trace
		// of smoke far too thin to see, spread by the advection, kept its
		// blocks and their neighbours, more of them the longer it went on. The
		// end of a shot cost the most, and held the blocks the next one wanted:
		// a second shot right after fell back to the classic particles (Matt,
		// 10-27).
		simulate->summaryAllocate.speedThresholdMinSmoke = simulate->summaryAllocate.smokeThreshold;
		// And gone at pt_flow_rail_life, stretched as the rest
		// (flow_rail_fade, which slows pt_flow_rail_smoke_fade too).
		advection->smoke.fade = flow.rail_smoke_fade;
		advection->temperature.fade = flow.rail_heat_fade;
		break;

	case R_FLOW_HEATBEAM:
	case FLOW_LAYER_HEATBEAM_VIEW:      // your own, as the camera sees it...
	case FLOW_LAYER_HEATBEAM_MIRROR:    // ...and as a mirror does
		// Heat cooling fast: the rings glow only just behind where they are,
		// and what a swept beam leaves behind goes in a quarter of a second,
		// lifting off in curls as it cools. Its little smoke is what keeps a
		// block while there is glow in it, and is gone soon after - as the
		// railgun's, speed alone keeps no block. Its fuel is how bright its
		// glow is (FLOW_COLORMAP_WEIGHTED_FIRE), so it must never burn: no
		// temperature reaches the ignition, and none would burn any if it did.
		// It neither fades nor damps (Flow's defaults for fuel), and goes with
		// the gas it is in.
		advection->ignitionTemp = 1e6f;
		advection->burnPerTemp = 0.f;
		advection->buoyancyPerTemp = max(cvar_pt_flow_heatbeam_buoyancy->value, 0.f);
		advection->coolingRate = max(cvar_pt_flow_heatbeam_cooling->value, 0.1f);
		advection->smoke.damping = 0.6f;
		advection->smoke.fade = max(cvar_pt_flow_heatbeam_smoke_fade->value, 0.f);
		simulate->vorticity.forceScale = max(cvar_pt_flow_heatbeam_vorticity->value, 0.f);
		advection->velocity.damping = 0.5f;
		simulate->summaryAllocate.speedThresholdMinSmoke = simulate->summaryAllocate.smokeThreshold;
		// No trails (pt_flow_heatbeam_trails 0): nothing stays where the beam
		// was. A step fades the gas before the emitters lay the new
		// (Grid.cpp: AdvectionCombustionDensity, then the sphere and box
		// emitters), and the beam lays its cells whole in a step
		// (flow_heatbeam), so damping of 1 - all of it, every step - leaves
		// the layer only the gas the beam is laying now: no glow, heat or
		// smoke behind it as it is swept, and no smoke to keep blocks there.
		// Strafing with it left a sheet of fire across the view, and the
		// blocks and the march through them that came with it.
		if (!cvar_pt_flow_heatbeam_trails->integer) {
			advection->temperature.damping = 1.f;
			advection->fuel.damping = 1.f;
			advection->smoke.damping = 1.f;
		}
		// ...so what moves it, and where it would move to, are never seen
		// (flow_heatbeam_lean)
		if (flow_heatbeam_lean()) {
			simulate->summaryAllocate.enabled = NV_FLOW_FALSE;
			simulate->forceDisableCoreSimulation = NV_FLOW_TRUE;
			advection->forceFadeEnabled = NV_FLOW_TRUE;
		}
		break;
	}
}

// How long an emitter emits; a trail, for as long as it is given.
static float flow_emitter_life(const flow_emitter_t *e)
{
	if (e->radius > 0.f)
		return e->life;
	if (e->layer == R_FLOW_RAIL)
		return max(cvar_pt_flow_rail_burst->value, 0.f);
	if (e->kind == R_FLOW_BEAM)
		return max(cvar_pt_flow_beam_burst->value, 0.f);
	return max(cvar_pt_flow_burst->value, 0.f);
}

// A trail, a plasma beam and something on fire: given every frame, gone the
// first one it is not.
static bool flow_emitter_given(const flow_emitter_t *e)
{
	return e->kind == R_FLOW_TRAIL || e->kind == R_FLOW_BURN || FLOW_LAYER_IS_HEATBEAM(e->layer);
}

static bool flow_emitter_emitting(const flow_emitter_t *e)
{
	if (flow_emitter_given(e))
		return e->seen;
	return e->age >= 0.f && e->age < flow_emitter_life(e);
}

// What an emitter fills itself with: its radius at scale 1 (for a beam, its
// half thickness), how fast it pushes out at its edge, and its heat, fuel
// and smoke.
static void flow_emitter_gas(const flow_emitter_t *e, float *radius, float *expansion,
	float *temperature, float *fuel, float *smoke)
{
	switch (e->layer) {
	case R_FLOW_PLASMA:
		*radius = cvar_pt_flow_bfg_radius->value;
		*expansion = cvar_pt_flow_bfg_expansion->value;
		*temperature = 1.f;
		*fuel = 0.f;
		*smoke = cvar_pt_flow_bfg_smoke->value;
		break;
	case R_FLOW_BUBBLES:
		*radius = cvar_pt_flow_bubble_radius->value;
		*expansion = cvar_pt_flow_bubble_expansion->value;
		*temperature = 0.f;
		*fuel = 0.f;
		*smoke = cvar_pt_flow_bubble_amount->value;
		break;
	default:
		*radius = cvar_pt_flow_radius->value;
		*expansion = cvar_pt_flow_expansion->value;
		*temperature = cvar_pt_flow_temperature->value;
		*fuel = cvar_pt_flow_fuel->value;
		*smoke = cvar_pt_flow_smoke->value;
		break;
	}

	if (e->kind == R_FLOW_TRAIL && e->layer == R_FLOW_FIRE && e->smoke_only) {
		// a grenade's, see flow_grenade_spheres
		*radius = cvar_pt_flow_grenade_radius->value;
		*expansion = cvar_pt_flow_grenade_expansion->value;
		// well under where the fire starts to glow, whatever that is set to
		*temperature = min(max(cvar_pt_flow_grenade_heat->value, 0.f),
			0.5f * Cvar_ClampValue(cvar_pt_flow_fire_glow_start, 0.f, 0.5f));
		*fuel = 0.f;
		*smoke = cvar_pt_flow_grenade_smoke->value;
	}
	else if (e->kind == R_FLOW_TRAIL && e->layer == R_FLOW_FIRE) {
		// a rocket's, see flow_rocket_spheres
		*radius = cvar_pt_flow_rocket_radius->value;
		*expansion = cvar_pt_flow_rocket_expansion->value;
		*temperature = cvar_pt_flow_rocket_temperature->value;
		*fuel = 0.f;
		*smoke = cvar_pt_flow_rocket_smoke->value;
	}
	else if (e->kind == R_FLOW_TRAIL) {
		*radius = cvar_pt_flow_trail_radius->value;
		*expansion = cvar_pt_flow_trail_expansion->value;
		if (e->layer != R_FLOW_BUBBLES)
			*smoke = cvar_pt_flow_trail_smoke->value;
	}
	else if (e->kind == R_FLOW_BEAM) {
		*radius = cvar_pt_flow_beam_radius->value;
		*expansion = cvar_pt_flow_beam_expansion->value;
		if (e->layer != R_FLOW_BUBBLES)
			*smoke = cvar_pt_flow_trail_smoke->value;
	}

	if (e->radius > 0.f) {
		*radius = e->radius;
		*expansion = e->expansion;
	}
}

// Flow's shadow pass, per layer: toward the brightest light of the layer, if
// it has one; nothing else of the pass is used. The shadow is worked out from
// smoke alone, so the colour map is a flat opacity of 1, and the attenuation
// is the layer's extinction per unit of smoke, as flow.glsl has it. With the
// defaults' thin smoke it darkens the far side of a cloud by about 10%.
static bool flow_build_offscreen(float cell_size)
{
	static const float x_points[2] = { 0.f, 1.f };
	static const NvFlowFloat4 rgba_points[2] = { { 1.f, 1.f, 1.f, 1.f }, { 1.f, 1.f, 1.f, 1.f } };
	static const float color_scale_points[2] = { 1.f, 1.f };

	bool any = false;
	for (int layer = 0; layer < FLOW_LAYERS; layer++) {
		NvFlowGridOffscreenLayerParams *off = &flow.offscreen[layer];
		*off = NvFlowGridOffscreenLayerParams_default;
		off->luid = 0x200 + layer;
		off->layer = layer;
		off->level = 0;
		off->colormap.resolution = 2;
		off->colormap.xPoints = x_points;
		off->colormap.xPointCount = 2;
		off->colormap.rgbaPoints = rgba_points;
		off->colormap.rgbaPointCount = 2;
		off->colormap.colorScalePoints = color_scale_points;
		off->colormap.colorScalePointCount = 2;
		off->colormap.colorScale = 1.f;
		off->debugVolume.enabled = NV_FLOW_FALSE;
		off->shadow.enabled = NV_FLOW_FALSE;
		flow.offscreen_ptrs[layer] = off;

		const int n = flow.layer_shadow[layer];
		if (!cvar_pt_flow_shadow->integer || n < 0)
			continue;

		const float density = layer == R_FLOW_PLASMA ? cvar_pt_flow_bfg_smoke_density->value : cvar_pt_flow_density->value;
		off->shadow.enabled = NV_FLOW_TRUE;
		off->shadow.isPointLight = NV_FLOW_TRUE;
		off->shadow.lightPosition.x = flow.dlights[n].origin[0];
		off->shadow.lightPosition.y = flow.dlights[n].origin[1];
		off->shadow.lightPosition.z = flow.dlights[n].origin[2];
		off->shadow.attenuation = max(density, 0.f) / cell_size;
		off->shadow.minIntensity = Cvar_ClampValue(cvar_pt_flow_shadow_min, 0.f, 1.f);
		any = true;
	}
	return any;
}

/*
Whether Flow steps every frame, not pt_flow_rate times a second: while your own
plasma beam is on (pt_flow_heatbeam_every_frame). Above pt_flow_rate fps the
frames between steps draw the gas the last step laid, where the gun was then.
Turning, that is the whole beam a frame's turn behind, the same small angle
along all of it; strafing, it is the gun's sideways move, which right by the
eye - the gas starts in the barrel - is a big angle: the beam went off to the
side and snapped back to the gun on the next step (Matt, 11-12: "If I am
straifing left and right the beam gets kind of off center then snaps to the
gun"). Stepped every frame, the beam is laid where the gun is in each frame,
as its model was. The cost is a step a frame, all of Flow's, for as long as
it is firing. Your own: a beam on the first-person layer, or one that starts
within 64 units of the view - first person with no body in the mirror lays
its beam on the shared layer.
*/
static void flow_every_frame(void)
{
	flow.every_frame = false;
	if (!cvar_pt_flow_heatbeam_every_frame->integer)
		return;
	for (int i = 0; i < flow.num_emitters; i++) {
		const flow_emitter_t *e = &flow.emitters[i];
		if (!flow_emitter_emitting(e) || !FLOW_LAYER_IS_HEATBEAM(e->layer))
			continue;
		if (e->layer == FLOW_LAYER_HEATBEAM_VIEW ||
			DistanceSquared(e->origin, flow.view_origin) < 64.f * 64.f) {
			flow.every_frame = true;
			return;
		}
	}
}

// Whether Flow steps this frame, and how much time it steps in all: its own
// stepper (TimeStepper_update in Grid.cpp) mirrored, under the settings
// flow_layer_simulate gives it. A frame misjudged only overlaps a rocket
// trail's puffs, or leaves one frame's flight to the puffs' radius.
static void flow_step_mirror(float dt)
{
	const float rate = cvar_pt_flow_rate->value;
	// laying a plasma beam again under a pause: a step a frame, of relay_dt -
	// none on the frame that clears it - and the stepper's count left alone
	if (flow.relay_step >= 0) {
		flow.stepping = true;
		flow.step_time = flow.relay_step > 0 ? flow.relay_dt : 0.f;
		return;
	}
	// the warm-up: one step of its own time (flow_layer_simulate), the
	// stepper's count left alone as for a relay
	if (flow.warm_now) {
		flow.stepping = true;
		flow.step_time = dt;
		return;
	}
	if (flow.force_clear)
		flow.step_error = 0.f;
	if (!flow.every_frame && rate > 0.f && flow.frame_dt > 0.f && flow.frame_dt < 1.f / rate) {
		flow.step_error += dt;
		const int steps = min((int)floorf(flow.step_error * rate), 1);
		flow.step_error = min(flow.step_error - steps / rate, 4.f / rate);
		flow.stepping = steps > 0;
		flow.step_time = 1.f / rate;
	}
	else {
		// every frame steps, and Flow leaves its count of time as it was
		flow.stepping = dt > 0.f;
		flow.step_time = dt;
	}
}

/*
A rocket's trail (a trail of fire): a little fireball at its tail that cools
into a column of smoke, like a small explosion that keeps going. Puts its
spheres at `num_spheres` on and gives back the new count.

A rocket flies some 11 units a step at 60 steps a second, more than a puff is
wide, so each step fills the whole stretch its source flew since the last one
with a chain of puffs half a radius apart: no gaps at any speed or frame rate.
(Flow's own sub-steps for a moving emitter match it to last frame's by its
place in the list, which the explosions' coming and going shifts.) Each step's
puff is up to 40% bigger or smaller and off the line by up to 30% of its
radius, times pt_flow_rocket_lumps, so the smoke comes out lumpy, not a tube.
The heat goes into a smaller sphere at each puff's middle, pt_flow_rocket_flame
of its radius: a smaller flame on the same smoke.

pt_flow_rocket_glow behind the source a second chain, twice as wide, takes the
heat out - a cell that goes through is left under a tenth, at any speed and
however hot the flame - which leaves a ball of fire at the tail. Without it the
whole trail glowed, the fire's cooling taking over a second. The trail has no
fuel: what the quench left of a fuelled one lit again and burned back down the
trail like a fuse. All measured headless.
*/
static int flow_rocket_spheres(flow_emitter_t *e, int num_spheres, float cell_size, float couple_rate)
{
	float radius, expansion, temperature, fuel, smoke;
	flow_emitter_gas(e, &radius, &expansion, &temperature, &fuel, &smoke);
	radius = max(radius * e->scale, cell_size);

	vec3_t path;
	VectorSubtract(e->origin, e->last, path);
	float dist = VectorLength(path);
	if (dist > FLOW_TRAIL_JUMP * max(e->scale, 1.f)) {
		VectorCopy(e->origin, e->last);
		VectorClear(path);
		dist = 0.f;
	}

	const int room = ((int)LENGTH(flow.spheres) - num_spheres) / 3;
	const int puffs = min(min(max((int)ceilf(dist / (0.5f * radius)), 1), FLOW_TRAIL_PUFFS), room);
	if (puffs < 1)
		return num_spheres;

	const float lumps = max(cvar_pt_flow_rocket_lumps->value, 0.f);
	const float r = radius * max(1.f + 0.4f * lumps * crand(), 0.2f);
	vec3_t offset;
	for (int k = 0; k < 3; k++)
		offset[k] = 0.3f * lumps * radius * crand();
	const float divergence = 3.f * expansion * (2.f * cell_size) / r;
	// as hard as a blast's puff: the rocket is past in a hundredth of a second
	const float rate = 5.f * couple_rate;
	// The heat only part way each pass, and with the edge cells' coverage
	// sampled eight times: set whole, a puff under three cells across came
	// out a voxel ball, its yellow core a plus sign close up (Matt, 10-25).
	// The puffs a cell is in build up a soft round core instead, hotter than
	// the colour map's top (pt_flow_rocket_temperature) to stay yellow - the
	// smaller the flame (pt_flow_rocket_flame), the hotter it has to be: at
	// 0.65 of the puff and 1.5 it came out all orange.
	const float heat_rate = 0.75f / max(flow.step_time, 1e-3f);
	const float flame = r * Cvar_ClampValue(cvar_pt_flow_rocket_flame, 0.f, 1.f);

	for (int i = 0; i < puffs; i++) {
		const float f = (float)(i + 1) / puffs;
		NvFlowGridEmitterSphereParams *sphere = &flow.spheres[num_spheres];
		*sphere = NvFlowEmitterSphereParams_default;
		sphere->luid = e->luid | ((uint64_t)i << 40);
		sphere->layer = FLOW_LAYER_FIRE_TRAIL;   // the fire's gas, pt_flow_rocket_brightness
		sphere->position.x = e->last[0] + path[0] * f + offset[0];
		sphere->position.y = e->last[1] + path[1] * f + offset[1];
		sphere->position.z = e->last[2] + path[2] * f + offset[2];
		sphere->radius = r;
		sphere->multisample = NV_FLOW_TRUE;
		sphere->velocity.x = sphere->velocity.y = sphere->velocity.z = 0.f;
		sphere->divergence = divergence;
		sphere->temperature = temperature;
		sphere->fuel = fuel;
		sphere->burn = 0.f;
		sphere->smoke = smoke;
		sphere->coupleRateVelocity = 0.f;
		sphere->coupleRateDivergence = rate;
		sphere->coupleRateTemperature = 0.f;
		sphere->coupleRateFuel = rate;
		sphere->coupleRateBurn = 0.f;
		sphere->coupleRateSmoke = rate;
		flow.sphere_ptrs[num_spheres++] = sphere;

		// the flame, in the puff's middle (which has made room for it)
		if (flame > 0.f) {
			NvFlowGridEmitterSphereParams *heat = &flow.spheres[num_spheres];
			*heat = *sphere;
			heat->luid = e->luid | (2ull << 48) | ((uint64_t)i << 40);
			heat->radius = flame;
			heat->allocationScale = 0.f;
			heat->coupleRateDivergence = 0.f;
			heat->coupleRateTemperature = heat_rate;
			heat->coupleRateFuel = 0.f;
			heat->coupleRateSmoke = 0.f;
			flow.sphere_ptrs[num_spheres++] = heat;
		}
	}

	const float glow = cvar_pt_flow_rocket_glow->value * e->scale;
	if (glow > 0.f && !VectorEmpty(e->heading)) {
		// A cell on the line is in some 2 qr / spacing of them as they pass,
		// each taking rate x step time of what heat is left: ln (T / 0.09) in
		// all, which leaves under a tenth, black on the colour map (0.15). A
		// set share, a hotter flame left a faint glow 250 units down the trail.
		const float qr = 2.f * radius;
		const float spacing = max(dist / puffs, 1.f);
		const float keep_inv = max(temperature * (16.f / 1.5f), 2.f);
		const float quench_rate = logf(keep_inv) * spacing / (2.f * qr * max(flow.step_time, 1e-3f));
		for (int i = 0; i < puffs; i++) {
			const float f = (float)(i + 1) / puffs;
			NvFlowGridEmitterSphereParams *quench = &flow.spheres[num_spheres];
			*quench = NvFlowEmitterSphereParams_default;
			quench->luid = e->luid | (1ull << 48) | ((uint64_t)i << 40);
			quench->layer = FLOW_LAYER_FIRE_TRAIL;
			quench->position.x = e->last[0] + path[0] * f - e->heading[0] * glow;
			quench->position.y = e->last[1] + path[1] * f - e->heading[1] * glow;
			quench->position.z = e->last[2] + path[2] * f - e->heading[2] * glow;
			quench->radius = qr;
			quench->allocationScale = 0.f;
			quench->velocity.x = quench->velocity.y = quench->velocity.z = 0.f;
			quench->divergence = 0.f;
			quench->temperature = 0.f;
			quench->fuel = 0.f;
			quench->burn = 0.f;
			quench->smoke = 0.f;
			quench->coupleRateVelocity = 0.f;
			quench->coupleRateDivergence = 0.f;
			quench->coupleRateTemperature = quench_rate;
			quench->coupleRateFuel = 0.f;
			quench->coupleRateBurn = 0.f;
			quench->coupleRateSmoke = 0.f;
			flow.sphere_ptrs[num_spheres++] = quench;
		}
	}

	if (flow.stepping)
		VectorCopy(e->origin, e->last);
	return num_spheres;
}

/*
A grenade's trail (a trail of fire with smoke_only): the classic trail's smoke
as Flow gas, lit like the explosions' smoke. The puffs go along the stretch
flown since Flow's last step, half a radius apart and jittered by
pt_flow_grenade_lumps, as a rocket's do (flow_rocket_spheres), but with no
flame and nothing to quench.

The classic trail is thick for the first ~90 units of flight and then thin
(trailcount, CL_DiminishingTrail): the smoke here starts at
pt_flow_grenade_smoke and falls to pt_flow_grenade_thin of that over the first
pt_flow_grenade_thin_length units. A little heat, pt_flow_grenade_heat, lifts
it the way the classic particles drift up: 0.1 x pt_flow_buoyancy 2 is their 20
units/s^2. It stays well under where the fire starts to glow.

A grenade lying still puts nothing in, the same as the classic trail. A source
held in one place keeps pushing gas out and fills the room. A slow roll adds
up until it has gone a quarter of a radius.
*/
static int flow_grenade_spheres(flow_emitter_t *e, int num_spheres, float cell_size, float couple_rate)
{
	float radius, expansion, temperature, fuel, smoke;
	flow_emitter_gas(e, &radius, &expansion, &temperature, &fuel, &smoke);
	radius = max(radius * e->scale, cell_size);

	vec3_t path;
	VectorSubtract(e->origin, e->last, path);
	float dist = VectorLength(path);
	if (dist > FLOW_TRAIL_JUMP * max(e->scale, 1.f)) {
		VectorCopy(e->origin, e->last);
		return num_spheres;
	}
	if (dist < 0.25f * radius)
		return num_spheres;

	const int room = (int)LENGTH(flow.spheres) - num_spheres;
	const int puffs = min(min((int)ceilf(dist / (0.5f * radius)), FLOW_TRAIL_PUFFS), room);
	if (puffs < 1)
		return num_spheres;

	const float thin_length = max(cvar_pt_flow_grenade_thin_length->value, 0.f);
	const float thin = Cvar_ClampValue(cvar_pt_flow_grenade_thin, 0.f, 1.f);
	const float lumps = max(cvar_pt_flow_grenade_lumps->value, 0.f);
	const float r = radius * max(1.f + 0.4f * lumps * crand(), 0.2f);
	vec3_t offset;
	for (int k = 0; k < 3; k++)
		offset[k] = 0.3f * lumps * radius * crand();
	const float divergence = 3.f * expansion * (2.f * cell_size) / r;
	// as hard as a rocket's: a grenade flies past at some 600 units/s
	const float rate = 5.f * couple_rate;

	for (int i = 0; i < puffs; i++) {
		const float f = (float)(i + 1) / puffs;
		const float flown = e->flown + dist * f;
		const float share = thin_length > 0.f && flown < thin_length
			? 1.f - (1.f - thin) * (flown / thin_length) : thin;

		NvFlowGridEmitterSphereParams *sphere = &flow.spheres[num_spheres];
		*sphere = NvFlowEmitterSphereParams_default;
		sphere->luid = e->luid | ((uint64_t)i << 40);
		sphere->layer = e->layer;
		sphere->position.x = e->last[0] + path[0] * f + offset[0];
		sphere->position.y = e->last[1] + path[1] * f + offset[1];
		sphere->position.z = e->last[2] + path[2] * f + offset[2];
		sphere->radius = r;
		sphere->multisample = NV_FLOW_TRUE;
		sphere->velocity.x = sphere->velocity.y = sphere->velocity.z = 0.f;
		sphere->divergence = divergence;
		sphere->temperature = temperature;
		sphere->fuel = 0.f;
		sphere->burn = 0.f;
		sphere->smoke = smoke * share;
		sphere->coupleRateVelocity = 0.f;
		sphere->coupleRateDivergence = rate;
		// none when there is no heat: it would cool what it went through
		sphere->coupleRateTemperature = temperature > 0.f ? rate : 0.f;
		sphere->coupleRateFuel = 0.f;
		sphere->coupleRateBurn = 0.f;
		sphere->coupleRateSmoke = rate;
		flow.sphere_ptrs[num_spheres++] = sphere;
	}

	if (flow.stepping) {
		e->flown = min(e->flown + dist, 1e6f);
		VectorCopy(e->origin, e->last);
	}
	return num_spheres;
}

/*
Something on fire (R_FLOW_BURN), given by the client every frame it burns:
where the plasma beam hits a wall, what it hits that can be hurt - a monster,
a player, a corpse - and the gibs it blows off them (cl_flow_heatbeam_fire,
client/tent.c). Matt, 11-12: "where the plasma beam is hitting I want it to
make it look like it's bursting into flames", the thing it hits "bursting
into flames like the palm tree", and gibs with "little fires on them that go
out into smoke". On the rocket trails' layer (FLOW_LAYER_FIRE_TRAIL): the
fire's gas, its cooling and its rise, glowing at pt_flow_rocket_brightness.

Built as a rocket's fireball is (flow_rocket_spheres): puffs of smoke
pt_flow_burn_smoke, each with a smaller ball of heat in it
(the emit's `core` of its radius, or pt_flow_burn_flame without one - the
beam's own fire on a wall asks for more, 11-12: "the bright core of the fire
larger with the over fire a little larger" - at pt_flow_burn_temperature, a share of
the way each step so its edge is soft), the heat only, no fuel. The puffs go
anywhere in the box, new places every step, one per 24 units of its height
up to four, each a little bigger or smaller (pt_flow_burn_lumps): flames all
over a body that flicker and climb off it. Their radius is pt_flow_burn_size
of the box's narrower side, pt_flow_burn_min_radius to _max_radius. A box
that moved since Flow's last step - a gib in flight, the beam swept along a
wall - gets them along the way it went too, half a radius apart, up to
FLOW_TRAIL_PUFFS; a jump past FLOW_TRAIL_JUMP starts again where it is.

`heat` 1 burns; as it falls the heat falls with it and the smoke thickens to
twice as much, and once it is no longer given what is left rises and cools
into smoke on its own - fire at the colour map's top for a third of a second,
orange to the glow's start (pt_flow_fire_glow_start) a second after. For its
first pt_flow_burn_burst seconds it also swells, at pt_flow_burn_expansion,
and burns half as hot again: it bursts into flame. Not after: a source held
in one place that pushes gas out fills the room (flow_grenade_spheres).
No light of its own. Worked out, not seen.

With smoke_only - a gib the railgun blew off (cl_flow_gib_effects, client/
tent.c; Matt, 11-12: "have the meat gibs smoke a little bit") - the puffs only:
no flame, no burst, smoke pt_flow_gib_smoke and heat pt_flow_gib_smoke_heat to
lift it, under the glow. Both full until `heat` is down to a half, then
falling with it: a gib smokes in the air and on as it lies - "a smoking pile
of gibs after a rail shot gib" (Matt, 11-12; it had thinned out by the time
they landed) - and fades at the end. The heat is put in a share of the way
each step, as a flame's is. That heat alone, kept under the glow, is too little
to lift it before the layer cools it (pt_flow_cooling 1.3): the puffs, filled
again every step, held a ball of haze round the gib (Matt, 11-12: "the smoke
doesn't seem like it rises from the gibs. The smoke is kind of clouded around
the gib"). So they also push it up, at pt_flow_gib_smoke_rise units/s, and are
pt_flow_gib_smoke_size of a flame's puff (not below a cell): a thin source the
smoke climbs out of.
*/
static int flow_burn_spheres(flow_emitter_t *e, int num_spheres, float couple_rate)
{
	const float cell_size = flow_layer_cell_size(FLOW_LAYER_FIRE_TRAIL);
	const float heat = min(max(e->heat, 0.f), 1.f);
	const float smallest = max(cvar_pt_flow_burn_min_radius->value, cell_size);
	float radius = min(max(min(e->half[0], e->half[1]) * max(cvar_pt_flow_burn_size->value, 0.f), smallest),
		max(cvar_pt_flow_burn_max_radius->value, smallest));
	if (e->smoke_only)
		radius = max(radius * Cvar_ClampValue(cvar_pt_flow_gib_smoke_size, 0.f, 1.f), cell_size);
	const int flames = min(max((int)(2.f * e->half[2] / 24.f), 1), 4);

	vec3_t path;
	VectorSubtract(e->origin, e->last, path);
	float dist = VectorLength(path);
	if (dist > FLOW_TRAIL_JUMP) {
		VectorCopy(e->origin, e->last);
		VectorClear(path);
		dist = 0.f;
	}

	const int room = ((int)LENGTH(flow.spheres) - num_spheres) / 2;
	const int puffs = min(min(max((int)ceilf(dist / (0.5f * radius)), flames), FLOW_TRAIL_PUFFS), room);
	if (puffs < 1)
		return num_spheres;

	const float lumps = max(cvar_pt_flow_burn_lumps->value, 0.f);
	const bool smoking = e->smoke_only;
	const bool bursting = !smoking && e->age < max(cvar_pt_flow_burn_burst->value, 0.f);
	const float rate = 5.f * couple_rate;
	const float heat_rate = 0.75f / max(flow.step_time, 1e-3f);
	const float smouldering = min(2.f * heat, 1.f);
	const float temperature = smoking ? max(cvar_pt_flow_gib_smoke_heat->value, 0.f) * smouldering
		: max(cvar_pt_flow_burn_temperature->value, 0.f) * heat * (bursting ? 1.5f : 1.f);
	const float smoke = smoking ? max(cvar_pt_flow_gib_smoke->value, 0.f) * smouldering
		: max(cvar_pt_flow_burn_smoke->value, 0.f) * (2.f - heat);
	const float flame = smoking ? 0.f : e->core > 0.f ? min(e->core, 1.f) : Cvar_ClampValue(cvar_pt_flow_burn_flame, 0.f, 1.f);
	const float expansion = bursting ? max(cvar_pt_flow_burn_expansion->value, 0.f) : 0.f;
	const float rise = smoking ? max(cvar_pt_flow_gib_smoke_rise->value, 0.f) : 0.f;

	for (int i = 0; i < puffs; i++) {
		const float f = (float)(i + 1) / puffs;
		const float r = radius * max(1.f + 0.4f * lumps * crand(), 0.2f);
		NvFlowGridEmitterSphereParams *sphere = &flow.spheres[num_spheres];
		*sphere = NvFlowEmitterSphereParams_default;
		sphere->luid = e->luid | ((uint64_t)i << 40);
		sphere->layer = FLOW_LAYER_FIRE_TRAIL;
		sphere->position.x = e->last[0] + path[0] * f + 0.8f * e->half[0] * crand();
		sphere->position.y = e->last[1] + path[1] * f + 0.8f * e->half[1] * crand();
		sphere->position.z = e->last[2] + path[2] * f + 0.8f * e->half[2] * crand();
		sphere->radius = r;
		sphere->multisample = NV_FLOW_TRUE;
		sphere->velocity.x = sphere->velocity.y = sphere->velocity.z = 0.f;
		sphere->divergence = 3.f * expansion * (2.f * cell_size) / r;
		sphere->temperature = temperature;
		sphere->fuel = 0.f;
		sphere->burn = 0.f;
		sphere->smoke = smoke;
		sphere->velocity.z = rise;
		sphere->coupleRateVelocity = rise > 0.f ? rate : 0.f;
		sphere->coupleRateDivergence = expansion > 0.f ? rate : 0.f;
		// a fire's heat is in its flame; a smoking gib's in the puff
		sphere->coupleRateTemperature = smoking && temperature > 0.f ? heat_rate : 0.f;
		sphere->coupleRateFuel = 0.f;
		sphere->coupleRateBurn = 0.f;
		sphere->coupleRateSmoke = rate;
		flow.sphere_ptrs[num_spheres++] = sphere;

		// the flame, in the puff's middle
		if (flame > 0.f && temperature > 0.f) {
			NvFlowGridEmitterSphereParams *hot = &flow.spheres[num_spheres];
			*hot = *sphere;
			hot->luid = e->luid | (2ull << 48) | ((uint64_t)i << 40);
			hot->radius = r * flame;
			hot->allocationScale = 0.f;
			hot->coupleRateDivergence = 0.f;
			hot->coupleRateTemperature = heat_rate;
			hot->coupleRateSmoke = 0.f;
			flow.sphere_ptrs[num_spheres++] = hot;
		}
	}

	if (flow.stepping)
		VectorCopy(e->origin, e->last);
	return num_spheres;
}

// A box round the line from `a` to `b`, `half_width` each way across it and
// `pad` past either end, sampled eight times a cell at its edges (it is
// about a cell thick), its gas, velocity and rates for the caller to fill.
// Its x axis is the line, centred between the ends; Flow's matrices act on
// row vectors, so the rows are the axes and the last is the translation.
// NULL when there is no room for it.
static NvFlowGridEmitterBoxParams *flow_line_box(int *num_boxes, uint64_t luid, int layer,
	const vec3_t a, const vec3_t b, float half_width, float pad)
{
	if (*num_boxes >= (int)LENGTH(flow.boxes))
		return NULL;

	vec3_t dir, side, up;
	VectorSubtract(b, a, dir);
	const float length = VectorNormalize(dir);
	if (length < 0.01f)
		return NULL;
	// Quake's `up` here is side x dir; dir x side keeps the box a rotation
	MakeNormalVectors(dir, side, up);
	CrossProduct(dir, side, up);

	NvFlowGridEmitterBoxParams *box = &flow.boxes[*num_boxes];
	*box = NvFlowEmitterBoxParams_default;
	box->luid = luid;
	box->layer = layer;
	const NvFlowFloat4x4 to_world = {
		dir[0], dir[1], dir[2], 0.f,
		side[0], side[1], side[2], 0.f,
		up[0], up[1], up[2], 0.f,
		0.5f * (a[0] + b[0]), 0.5f * (a[1] + b[1]), 0.5f * (a[2] + b[2]), 1.f
	};
	box->localToWorld = to_world;
	box->localToWorldVelocity = to_world;
	box->velocityIsWorldSpace = NV_FLOW_TRUE;
	box->position.x = box->position.y = box->position.z = 0.f;
	box->halfSize.x = 0.5f * length + pad;
	box->halfSize.y = box->halfSize.z = half_width;
	box->velocity.x = box->velocity.y = box->velocity.z = 0.f;
	box->multisample = NV_FLOW_TRUE;
	box->coupleRateVelocity = 0.f;
	box->coupleRateDivergence = 0.f;
	box->coupleRateTemperature = 0.f;
	box->coupleRateFuel = 0.f;
	box->coupleRateBurn = 0.f;
	box->coupleRateSmoke = 0.f;
	flow.box_ptrs[(*num_boxes)++] = box;
	return box;
}

/*
A railgun's trail (a beam of the rail layer), after the classic one
(CL_OldRailTrail in client/effects.c): a particle a unit along the shot, on a
spiral 3 units round that turns 0.1 radians a unit, glowing cyan-blue and gone
in a second, round a dotted line of grey ones. Here the spiral is a helix of
glowing gas pt_flow_rail_radius round the line, a turn every pt_flow_rail_turn
units, cut into FLOW_RAIL_CHORDS boxes a turn pt_flow_rail_width thick; and
the line is a thread of smoke with a little heat in it, in FLOW_BEAM_SEGMENT
boxes like the BFG's lasers. At 3 units round the spiral would be two cells
across, too few for its twist to show, so it is twice that.

The spiral's gas is set turning round the line at pt_flow_rail_swirl units/s,
which the layer's vorticity curls into wisps as it cools. Pushed outward
instead, as the classic particles drift, it would mostly stay put: a velocity
spreading out from a line is all divergence, which the pressure solve takes
out. So both swell by their divergence (pt_flow_rail_expansion,
_core_expansion) instead.

Only the stretch from e->from to e->to units along the shot is drawn, chosen
when it came in (vkpt_flow_emit); the twist counts from the true start, so
the spiral turns where the classic one would. Of that, e->near_from to
e->near_to goes on the rail layer's fine grid and the rest, either side, on
FLOW_LAYER_RAIL_FAR's coarse one (flow_rail_stretch) - each piece by
flow_rail_piece, cut where the other starts. Returns the new box count.
*/
static int flow_rail_piece(const flow_emitter_t *e, int num_boxes, float couple_rate,
	float from, float to, int piece, int layer)
{
	vec3_t dir, right, up;
	VectorSubtract(e->end, e->origin, dir);
	if (VectorNormalize(dir) < 1.f || to - from < 1.f)
		return num_boxes;
	// the classic spiral's own axes
	MakeNormalVectors(dir, right, up);

	const float cell_size = flow_layer_cell_size(layer);
	// Each piece's boxes are Flow emitters of their own.
	const uint64_t luid = e->luid | ((uint64_t)piece << 57);

	// It emits for a few frames, so it fills itself as hard as a blast's puff.
	const float rate = 5.f * couple_rate;

	// The line: smoke and a little heat. The divergence of a cylinder
	// growing at `expansion` units/s at its surface is 2 * expansion / radius
	// per unit length, on velocity cells twice the density cells' size; a
	// line thinner than a cell grows as one a cell thick would.
	{
		const float radius = max(cvar_pt_flow_rail_core_radius->value, 0.5f);
		const float divergence = 2.f * cvar_pt_flow_rail_core_expansion->value * (2.f * cell_size) / max(radius, cell_size);
		const int segments = max((int)ceilf((to - from) / FLOW_BEAM_SEGMENT), 1);
		for (int s = 0; s < segments; s++) {
			vec3_t a, b;
			VectorMA(e->origin, from + (to - from) * s / segments, dir, a);
			VectorMA(e->origin, from + (to - from) * (s + 1) / segments, dir, b);
			NvFlowGridEmitterBoxParams *box = flow_line_box(&num_boxes, luid | ((uint64_t)(s + 1) << 32),
				layer, a, b, radius, 0.f);
			if (!box)
				return num_boxes;
			box->divergence = divergence;
			box->temperature = cvar_pt_flow_rail_core_temperature->value;
			box->fuel = 0.f;
			box->burn = 0.f;
			box->smoke = cvar_pt_flow_rail_core_smoke->value;
			box->coupleRateDivergence = rate;
			box->coupleRateTemperature = rate;
			box->coupleRateFuel = rate;
			box->coupleRateSmoke = rate;
		}
	}

	// The spiral: a box along each chord of the helix, as long again as it
	// is thick so that the next one's corner meets it.
	const float radius = max(cvar_pt_flow_rail_radius->value, 0.f);
	const float width = max(cvar_pt_flow_rail_width->value, 0.5f);
	const float turn = max(cvar_pt_flow_rail_turn->value, 8.f);
	const float step = turn / FLOW_RAIL_CHORDS;
	const float k = 2.f * M_PI / turn;   // radians a unit
	const float swirl = cvar_pt_flow_rail_swirl->value;
	const float divergence = 2.f * cvar_pt_flow_rail_expansion->value * (2.f * cell_size) / max(width, cell_size);
	if (radius <= 0.f)
		return num_boxes;

	const int first = (int)floorf(from / step);
	const int last = (int)ceilf(to / step);
	for (int i = first; i < last; i++) {
		const float s0 = max(i * step, from);
		const float s1 = min((i + 1) * step, to);
		if (s1 - s0 < 0.5f)
			continue;

		vec3_t a, b;
		VectorMA(e->origin, s0, dir, a);
		VectorMA(a, radius * cosf(k * s0), right, a);
		VectorMA(a, radius * sinf(k * s0), up, a);
		VectorMA(e->origin, s1, dir, b);
		VectorMA(b, radius * cosf(k * s1), right, b);
		VectorMA(b, radius * sinf(k * s1), up, b);

		NvFlowGridEmitterBoxParams *box = flow_line_box(&num_boxes, luid | (1ull << 56) | ((uint64_t)(i + 1) << 32),
			layer, a, b, width, 0.5f * width);
		if (!box)
			return num_boxes;

		// round the line, the way the spiral turns
		const float angle = k * 0.5f * (s0 + s1);
		vec3_t velocity;
		VectorScale(up, swirl * cosf(angle), velocity);
		VectorMA(velocity, -swirl * sinf(angle), right, velocity);
		box->velocity.x = velocity[0];
		box->velocity.y = velocity[1];
		box->velocity.z = velocity[2];
		box->coupleRateVelocity = swirl != 0.f ? rate : 0.f;

		box->divergence = divergence;
		box->temperature = cvar_pt_flow_rail_temperature->value;
		box->fuel = 0.f;
		box->burn = 0.f;
		box->smoke = cvar_pt_flow_rail_smoke->value;
		box->coupleRateDivergence = rate;
		box->coupleRateTemperature = rate;
		box->coupleRateFuel = rate;
		box->coupleRateSmoke = rate;
	}

	return num_boxes;
}

static int flow_rail_boxes(const flow_emitter_t *e, int num_boxes, float couple_rate)
{
	num_boxes = flow_rail_piece(e, num_boxes, couple_rate, e->from, e->near_from, 0, FLOW_LAYER_RAIL_FAR);
	num_boxes = flow_rail_piece(e, num_boxes, couple_rate, e->near_from, e->near_to, 1, R_FLOW_RAIL);
	return flow_rail_piece(e, num_boxes, couple_rate, e->near_to, e->to, 2, FLOW_LAYER_RAIL_FAR);
}

// `from` along the beam from `origin` in unit `dir`, moved on past the ball of
// `clear` round the view where that ball is round it - no further than `to`.
static float flow_beam_clear(const vec3_t origin, const vec3_t dir, float from, float to, float clear)
{
	vec3_t to_view;
	VectorSubtract(flow.view_origin, origin, to_view);
	// where along the beam the view is nearest it, and how far off it it is
	const float along = DotProduct(to_view, dir);
	const float across2 = max(DotProduct(to_view, to_view) - along * along, 0.f);
	if (clear * clear > across2) {
		const float reach = sqrtf(clear * clear - across2);
		if (along - reach <= from)
			from = max(from, along + reach);
	}
	return min(from, to);
}

// The stretch of plasma beam `e` its core is drawn on (e->from to e->to): all
// of it up to pt_flow_heatbeam_length units, or that much round the point of
// it nearest the view, as the railgun's trail (flow_rail_stretch) - and none
// within pt_flow_heatbeam_core_clear of the view where that is round its
// start, but for the one only a mirror sees, which is not seen from the view.
// Worked out as it is given, from the view of the frame before; the client
// draws the beam's model on the stretch past it, and both go by this.
static void flow_heatbeam_stretch(flow_emitter_t *e)
{
	vec3_t dir, to_view;
	VectorSubtract(e->end, e->origin, dir);
	const float length = VectorNormalize(dir);
	e->from = e->to = 0.f;
	if (length < 1.f)
		return;

	VectorSubtract(flow.view_origin, e->origin, to_view);
	const float keep = max(cvar_pt_flow_heatbeam_length->value, 64.f);
	e->to = length;
	if (length > keep) {
		const float nearest = min(max(DotProduct(to_view, dir), 0.f), length);
		e->from = min(max(nearest - 0.5f * keep, 0.f), length - keep);
		e->to = e->from + keep;
	}
	if (e->layer != FLOW_LAYER_HEATBEAM_MIRROR)
		e->from = flow_beam_clear(e->origin, dir, e->from, e->to,
			max(cvar_pt_flow_heatbeam_core_clear->value, 0.f));
}

/*
A plasma beam (R_FLOW_HEATBEAM), given every frame it is on, after the classic
one (the beam's model, and CL_Heatbeam in client/newfx.c): rings of yellow
particles round the beam, 32 units apart, running out along it at 96 units/s,
gone each frame. Here, on FLOW_LAYER_HEATBEAM's own grid
(pt_flow_heatbeam_cell_size):

  - the beam itself is a core of hot gas pt_flow_heatbeam_radius round, the
    yellow-orange of the model's skin at its hottest, pushed out along the
    beam at pt_flow_heatbeam_flow; what leaves it cools to a glow round it,
    which the layer's buoyancy and curl lift off in wisps;
  - the rings are glowing gas, pt_flow_heatbeam_ring_radius round, each
    FLOW_RAIL_CHORDS boxes pt_flow_heatbeam_ring_width thick, running out at
    pt_flow_heatbeam_ring_speed from where the beam came on - and pushing the
    gas they heat along with them, so what cools trails just behind, cooled
    to nothing pt_flow_heatbeam_ring_tail units back by a quench of its own.
    Each starts white-hot where it shows and cools to yellow, orange and
    smoke over pt_flow_heatbeam_ring_travel units (flow_heatbeam_ring_heat);
  - where it hits (`hit`), a hot spot pulled back off the surface by half its
    radius, which burns off a thin column of smoke.

All of it fills its cells in the step it reaches them, at five times the
coupling rate, as the railgun's spiral and a blast's puffs do. A ring runs on
a cell and more in a step, and at the plain rate it left each cell it crossed
at half its heat or less, most of a cell's only partly in it: on the fire's
colour map, which is steep at the top, a dull red a small part as bright as
the heat it was given (Matt, 11-09: "the rings need to be a lot brighter or
start brighter"). Worked out, not seen.

THE CORE SEEN END-ON. Glowing gas is as bright as the length of it looked
through. A line of it seen from near one end - by the one firing it, down the
barrel - is looked through for 1/sin of the angle it is seen at, against its
width side-on: tens of times, and more toward the crosshair, where the model
it stands in for, a surface, is as bright from any side. The sheath this was
before outshone the rings that way. So each piece of the core glows that sine
as brightly - pt_flow_heatbeam_end_on of it - by its fuel, which weights the
layer's glow (FLOW_COLORMAP_WEIGHTED_FIRE) and never burns here. The sine
changes fastest near the view, so the pieces are shorter there - a quarter
of their distance from it, down to two cells - and 16 cells long at most
further off: Flow allocates every block in a box's axis-aligned bounds, and a
FLOW_BEAM_SEGMENT of 48 units across a diagonal took a 2x3x3 run of the
grid's 48x24x24-unit blocks, where 24 takes 2x2x2 at most. It is the one
view's sine. Your own beam in first person is laid a second time, from the
gun your body holds, for mirrors only (FLOW_LAYER_HEATBEAM_MIRROR): that one
is not seen from the view, so it is neither dimmed nor kept clear of it.

NEAR THE VIEW the core starts pt_flow_heatbeam_core_clear units from it
(flow_heatbeam_stretch) and comes up from pt_flow_heatbeam_near_brightness of
its glow there to all of it pt_flow_heatbeam_near_fade units further on. Gas
a few units from the eye is cells magnified many times over, the glow lifting
off them with it: from the model's start, 8 units from the eye, a blob of
fire over the gun (Matt, 11-09: "too much fire near the camera", "it looks
like it's top of the gun"), and kept 24 units off it, a beam that began in
mid-air (11-10). So the one firing has its gas start in the barrel instead
(cl_flow_heatbeam_muzzle), where the gun hides the near end, as it hides the
model's. Nothing is drawn short of the core: keeping the model there - the
core from 32 units out - left the model as most of what the one firing sees
of the beam, the rest of it end-on near the crosshair ("I shouldn't see the
old beam").

The core is on e->from to e->to (flow_heatbeam_stretch); the client draws
the beam's model past it. The rings are not within pt_flow_heatbeam_clear of
the view either, nor the hot spot within FLOW_HEATBEAM_POINT_BLANK: the one
firing has the start of the beam on the gun, where they would fill the
screen - and seen end-on, the first rings out stack into a ball round the
crosshair. Ring radius grows from nothing over the first 10 units, as the
classic's does.

Laid again under a pause (flow_heatbeam_relay), the coupling fills a cell in
the one step whatever its length, and the rings are where they were
`ring_back` seconds before, run out over the steps it takes.

NO TRAILS (pt_flow_heatbeam_trails 0, the default since 11-12): the layer
keeps nothing from one step to the next (flow_layer_simulate) and every cell
is laid whole each step, so the beam is only ever where it is now. Swept, it
leaves no glow, curls or smoke behind - strafing with it had left a sheet of
fire across the view, and the cost of it (Matt: "This really hits performance
hard"). The rings keep no tails, so they get no quench; the hot spot burns
off no column of smoke.
*/
// No hot spot where the beam hits closer to the view than this: a wall at
// point blank, all fire. It went with the rings' pt_flow_heatbeam_clear until
// that was 80 (11-10).
#define FLOW_HEATBEAM_POINT_BLANK 32.f

// The core's pieces as laid, for its hotter middle; past this many, the far
// end has none.
#define FLOW_HEATBEAM_PIECES 256
// a soft ring's fuel against the hard fill's (pt_flow_heatbeam_ring_soft):
// a band 2 cells wide lies on 2.5 cells a quarter covered or more, on average,
// across each way of its section - 6.25 cells' worth hard for 4 soft
#define FLOW_RING_SOFT_GAIN  1.5f
typedef struct {
	vec3_t a, b;
	float fuel;
} flow_core_piece_t;

// A strand turns this many chords a turn (flow_heatbeam).
#define FLOW_HEATBEAM_STRAND_CHORDS 8

/* How bright a piece of the plasma beam's core glows, at `middle`, running
   along `dir`, as the view sees it (THE CORE SEEN END-ON and NEAR THE VIEW
   above): pt_flow_heatbeam_end_on of the sine of the angle it is seen at, and
   from pt_flow_heatbeam_near_brightness up to all of it over
   pt_flow_heatbeam_near_fade from pt_flow_heatbeam_core_clear units off the
   view. Neither for a mirror's beam, which is not seen from the view. */
static float flow_heatbeam_view_glow(const vec3_t middle, const vec3_t dir, bool mirror)
{
	const float end_on = mirror ? 0.f : Cvar_ClampValue(cvar_pt_flow_heatbeam_end_on, 0.f, 1.f);
	const float near_glow = Cvar_ClampValue(cvar_pt_flow_heatbeam_near_brightness, 0.f, 1.f);
	const float near_fade = mirror ? 0.f : max(cvar_pt_flow_heatbeam_near_fade->value, 0.f);
	const float near_start = max(cvar_pt_flow_heatbeam_core_clear->value, 0.f);
	vec3_t seen, across;
	VectorSubtract(middle, flow.view_origin, seen);
	CrossProduct(seen, dir, across);
	const float distance = VectorLength(seen);
	const float sine = distance > 0.01f ? min(VectorLength(across) / distance, 1.f) : 1.f;
	const float near = near_fade > 0.f ? min(max((distance - near_start) / near_fade, near_glow), 1.f) : 1.f;
	return near * (1.f - end_on + end_on * max(sine, 0.02f));
}

// Chord `c` of the FLOW_RAIL_CHORDS round a ring `r` units round `centre`,
// across `right` and `up`: from `a` to `b`.
static void flow_ring_chord(const vec3_t centre, const vec3_t right, const vec3_t up, float r, int c, vec3_t a, vec3_t b)
{
	const float a0 = 2.f * (float)M_PI * c / FLOW_RAIL_CHORDS;
	const float a1 = 2.f * (float)M_PI * (c + 1) / FLOW_RAIL_CHORDS;
	VectorMA(centre, r * cosf(a0), right, a);
	VectorMA(a, r * sinf(a0), up, a);
	VectorMA(centre, r * cosf(a1), right, b);
	VectorMA(b, r * sinf(a1), up, b);
}

/*
A plasma beam ring's heat and smoke `f` of the way along its run, from where
it shows to pt_flow_heatbeam_ring_travel units on (Matt, 11-12: "have ring
start out as white hot and fade to yellow to orange to smoke as they travel
down the beam"). On the layer's colour map (flow_fire_color, then
flow_white_hot above 1, pt_flow_heatbeam_yellow 1):

  f    0 - 0.1   pt_flow_heatbeam_ring_temperature, 1.25: white
       0.3       1: golden yellow
       0.55      0.85: orange
       0.75      0.6: a dull red
       0.85      0.2: dark - below pt_flow_fire_glow_start, 0.3
       0.85 - 1  smoke, which it has thickened to from 0.7 on, from
                 pt_flow_heatbeam_ring_smoke to pt_flow_heatbeam_ring_end_smoke

then nothing: the last smoke runs on and thins out on its own. In between,
straight lines. Each stop no hotter than the start, should that be turned
down. `f` below 0: no run, the start's heat and smoke all the way.
*/
static void flow_heatbeam_ring_heat(float f, float *temperature, float *smoke)
{
	const float start = cvar_pt_flow_heatbeam_ring_temperature->value;
	const float base_smoke = max(cvar_pt_flow_heatbeam_ring_smoke->value, 0.f);
	*temperature = start;
	*smoke = base_smoke;
	if (f < 0.f)
		return;

	const float at[5] = { 0.1f, 0.3f, 0.55f, 0.75f, 0.85f };
	const float heat[5] = { start, min(start, 1.f), min(start, 0.85f), min(start, 0.6f), min(start, 0.2f) };
	if (f > at[0]) {
		*temperature = heat[4];
		for (int i = 1; i < 5; i++) {
			if (f <= at[i]) {
				*temperature = heat[i - 1] + (heat[i] - heat[i - 1]) * (f - at[i - 1]) / (at[i] - at[i - 1]);
				break;
			}
		}
	}

	const float end_smoke = max(cvar_pt_flow_heatbeam_ring_end_smoke->value, 0.f);
	*smoke = base_smoke + (end_smoke - base_smoke) * min(max((f - 0.7f) / 0.15f, 0.f), 1.f);
}

/* How long each of this frame's Flow steps is, under flow_layer_simulate's
   settings: Flow's variable stepper makes maxStepsPerSimulate equal steps of the
   frame's time (Grid.cpp), its fixed one steps of 1/pt_flow_rate. */
static float flow_one_step_time(void)
{
	if (flow.relay_step >= 0 || flow.every_frame)
		return flow.step_time;
	const float rate = cvar_pt_flow_rate->value;
	if (rate > 0.f && flow.frame_dt > 0.f && flow.frame_dt < 1.f / rate)
		return flow.step_time;
	return flow.step_time / (float)Cvar_ClampInteger(cvar_pt_flow_substeps, 1, 4);
}

static void flow_heatbeam(const flow_emitter_t *e, int *num_boxes, int *num_spheres, float couple_rate)
{
	vec3_t dir, right, up;
	VectorSubtract(e->end, e->origin, dir);
	const float length = VectorNormalize(dir);
	if (length < 1.f)
		return;
	MakeNormalVectors(dir, right, up);

	const int layer = e->layer;
	const bool mirror = layer == FLOW_LAYER_HEATBEAM_MIRROR;  // not seen from the view
	const float cell_size = flow_layer_cell_size(layer);
	// laid again under a pause: full in a step, however short the step. And
	// with no trails, where the layer keeps nothing from the step before
	// (flow_layer_simulate), full in every step, or the beam would be only
	// the share of it one step fills - a frame's time is up to
	// pt_flow_substeps' 4 steps, and Flow takes a rate past a whole step as
	// a whole step.
	const bool relay = flow.relay_step > 0;
	const bool trails = cvar_pt_flow_heatbeam_trails->integer != 0;
	float rate = relay ? max(5.f * couple_rate, 1.f / max(flow.relay_dt, 1e-4f)) : 5.f * couple_rate;
	/* SOFT FILL (pt_flow_heatbeam_soft_fill 1). An emitter's cell moves
	   rate x step x coverage of the way to its value, at most all of it, where
	   coverage is how much of the cell is inside the shape (8 samples, the
	   emitters' `multisample`). At 4 / step any cell a quarter covered was
	   filled whole: the beam's edges stood on whole cells, and with no trails
	   - nothing kept from the last step to smooth them - the hot spot and the
	   rings came out terraced, a step a cell. On the fire's colour map, steep at
	   the top, each step is a line, and finer cells made more of them; a finer
	   march (pt_flow_step) showed them sharper (Matt, 11-12: "lines in the rings
	   behind the first ring" at cell size 0.75 and 0.5). At 1 / step a cell gets
	   as much as is covered, so an edge falls off across a cell wherever it is.
	   THE CORE ONLY (its hotter middle and strands with it): soft rings lost
	   the look they had - Matt, 11-12: "It looked better before even with the
	   lines" - so the rings and the hot spot keep the hard fill.

	   SOFT RING GLOW (pt_flow_heatbeam_ring_soft 1). Matt, 11-12, again: the
	   lines are in a single ring as it runs away from the view - stacked
	   level stripes down its side (one per row of cells: the ring stands near
	   upright across the grid's level rows). Why soft rings looked worse: a
	   cell's COLOUR is its temperature and its BRIGHTNESS its fuel
	   (flow_layer_add, FLOW_COLORMAP_WEIGHTED_FIRE), and soft fill gave a
	   part-covered cell part of the ring's temperature - lower on the fire's
	   colour map, so each ring had an orange-red fringe, and was dimmer. Now
	   only the fuel is soft (Matt sees no lines in smoke, only in the rings'
	   glow, so their smoke stays hard): every cell the ring is in a quarter
	   or more takes its full temperature (its colour, as with the hard fill)
	   and as much glow as it is covered, so the edge falls off smoothly instead
	   of standing on whole cells. A cell's coverage is 8 samples. x
	   FLOW_RING_SOFT_GAIN: soft, a ring 2 cells across glows over some 4 cells'
	   worth of its section where the hard fill lit some 6. */
	float core_rate = rate, ring_soft_rate = rate;
	if (!trails) {
		rate = max(rate, 4.f / max(flow.step_time, 1e-3f));
		const float soft_rate = 1.f / max(flow_one_step_time(), 1e-4f);
		core_rate = cvar_pt_flow_heatbeam_soft_fill->integer ? soft_rate : rate;
		ring_soft_rate = cvar_pt_flow_heatbeam_ring_soft->integer ? soft_rate : rate;
	}
	const float ring_gain = ring_soft_rate != rate ? FLOW_RING_SOFT_GAIN : 1.f;
	const float ring_back = relay ? (float)(flow.relay_steps - flow.relay_step) * flow.relay_dt : 0.f;
	const float from = min(max(e->from, 0.f), length);
	const float to = min(max(e->to, from), length);
	const float clear = mirror ? 0.f : max(cvar_pt_flow_heatbeam_clear->value, 0.f);

	// The core. A cylinder growing at `expansion` units/s at its surface
	// diverges by 2 * expansion / radius per unit length, on velocity cells
	// twice the density cells' size, as the railgun's line.
	const float core_radius = max(cvar_pt_flow_heatbeam_radius->value, 0.5f);
	const float brightness = max(cvar_pt_flow_heatbeam_core_brightness->value, 0.f);
	flow_core_piece_t pieces[FLOW_HEATBEAM_PIECES];  // for its hotter middle, laid last
	int num_pieces = 0;
	if (to - from >= 1.f) {
		const float radius = core_radius;
		const float divergence = 2.f * cvar_pt_flow_heatbeam_expansion->value * (2.f * cell_size) / max(radius, cell_size);
		const float speed = cvar_pt_flow_heatbeam_flow->value;
		float s = from;
		for (uint64_t piece = 1; s < to - 0.01f; piece++) {
			vec3_t a, b, middle;
			VectorMA(e->origin, s, dir, a);
			const float step = min(max(0.25f * Distance(a, flow.view_origin), 2.f * cell_size), 16.f * cell_size);
			float t = min(s + step, to);
			if (to - t < 0.5f * step)   // no sliver at the end
				t = to;
			VectorMA(e->origin, t, dir, b);
			VectorMA(e->origin, 0.5f * (s + t), dir, middle);
			s = t;

			NvFlowGridEmitterBoxParams *box = flow_line_box(num_boxes, e->luid | (piece << 32),
				layer, a, b, radius, 0.f);
			if (!box)
				return;
			box->velocity.x = dir[0] * speed;
			box->velocity.y = dir[1] * speed;
			box->velocity.z = dir[2] * speed;
			box->coupleRateVelocity = speed != 0.f ? core_rate : 0.f;
			box->divergence = divergence;
			box->temperature = cvar_pt_flow_heatbeam_temperature->value;
			box->fuel = brightness * flow_heatbeam_view_glow(middle, dir, mirror);
			box->burn = 0.f;
			box->smoke = cvar_pt_flow_heatbeam_smoke->value;
			box->coupleRateDivergence = core_rate;
			box->coupleRateTemperature = core_rate;
			box->coupleRateFuel = core_rate;
			box->coupleRateSmoke = core_rate;

			if (num_pieces < FLOW_HEATBEAM_PIECES) {
				VectorCopy(a, pieces[num_pieces].a);
				VectorCopy(b, pieces[num_pieces].b);
				pieces[num_pieces].fuel = box->fuel;
				num_pieces++;
			}
		}
	}

	// The rings: ring n is at the beam's age x speed - n x spacing along it,
	// all along the beam from its first frame, as the classic's are (n below
	// 0 for the ones further out than the first has run yet) - but only for
	// pt_flow_heatbeam_ring_travel units from where they show (ring_from),
	// cooling as they go (flow_heatbeam_ring_heat). A ring's boxes keep their
	// luids as it runs out, n's last ten bits; its quench's have bit 55.
	const float ring_from = flow_beam_clear(e->origin, dir, from, to, clear);
	const float ring_radius = max(cvar_pt_flow_heatbeam_ring_radius->value, 0.f);
	const float width = max(cvar_pt_flow_heatbeam_ring_width->value, 0.5f);
	const float spacing = max(cvar_pt_flow_heatbeam_ring_spacing->value, 4.f);
	const float ring_speed = max(cvar_pt_flow_heatbeam_ring_speed->value, 0.f);
	const float ring_brightness = max(cvar_pt_flow_heatbeam_ring_brightness->value, 0.f);
	const float travel = max(cvar_pt_flow_heatbeam_ring_travel->value, 0.f);
	const float ring_to = travel > 0.f ? min(to, ring_from + travel) : to;
	// no trails: nothing behind a ring to quench
	const float tail = trails ? max(cvar_pt_flow_heatbeam_ring_tail->value, 0.f) : 0.f;
	if (ring_radius > 0.f && ring_to - ring_from >= 1.f) {
		const float divergence = 2.f * cvar_pt_flow_heatbeam_ring_expansion->value * (2.f * cell_size) / max(width, cell_size);
		const double run = (double)max(e->age - ring_back, 0.f) * ring_speed;
		const int newest = (int)floor(run / spacing);
		// the furthest ring not past `ring_to`, then in toward the start
		int n = newest - (int)floor((ring_to - fmod(run, spacing)) / spacing);
		for (; n <= newest; n++) {
			const float s = (float)(run - (double)n * spacing);
			if (s < ring_from || s > ring_to)
				continue;
			const float r = ring_radius * min(s / 10.f, 1.f);
			if (r < 0.5f)
				continue;

			float temperature, smoke;
			flow_heatbeam_ring_heat(travel > 0.f ? (s - ring_from) / travel : -1.f, &temperature, &smoke);

			vec3_t centre;
			VectorMA(e->origin, s, dir, centre);
			const uint64_t ring_id = ((unsigned)n & 1023u) * FLOW_RAIL_CHORDS + 1;
			for (int c = 0; c < FLOW_RAIL_CHORDS; c++) {
				vec3_t a, b;
				flow_ring_chord(centre, right, up, r, c, a, b);
				NvFlowGridEmitterBoxParams *box = flow_line_box(num_boxes, e->luid | (1ull << 56) | ((ring_id + c) << 32),
					layer, a, b, width, 0.5f * width);
				if (!box)
					return;
				box->velocity.x = dir[0] * ring_speed;
				box->velocity.y = dir[1] * ring_speed;
				box->velocity.z = dir[2] * ring_speed;
				box->coupleRateVelocity = ring_speed > 0.f ? rate : 0.f;
				box->divergence = divergence;
				box->temperature = temperature;
				box->fuel = ring_brightness * ring_gain;
				box->burn = 0.f;
				box->smoke = smoke;
				box->coupleRateDivergence = rate;
				box->coupleRateTemperature = rate;            // its colour: hard
				box->coupleRateFuel = ring_soft_rate;         // its glow: as covered
				box->coupleRateSmoke = rate;                  // no lines in smoke (Matt)
			}

			// Its quench, `tail` units behind it: what the ring heated and
			// left there is cooled to nothing in a step, so each ring glows
			// as a ring with a short tail of its own colour cooling behind
			// it, apart from the next, and is seen to run. The layer's
			// cooling alone (pt_flow_heatbeam_cooling, 4) leaves the heat
			// glowing a third of a second, some 30 units of tail at 96
			// units/s, and the glow is so bright next to the room that all
			// of it shows: a glowing tube from ring to ring, 32 apart, that
			// hid their running (Matt, 11-12: the rings "should look like
			// they are traveling down the beam"). Only the heat; no blocks
			// of its own; a little wider than the ring, which swells.
			const float q = s - tail;
			const float rq = ring_radius * min(q / 10.f, 1.f);
			if (tail > 0.f && q >= ring_from && rq >= 0.5f) {
				VectorMA(e->origin, q, dir, centre);
				for (int c = 0; c < FLOW_RAIL_CHORDS; c++) {
					vec3_t a, b;
					flow_ring_chord(centre, right, up, rq, c, a, b);
					NvFlowGridEmitterBoxParams *box = flow_line_box(num_boxes, e->luid | (1ull << 55) | ((ring_id + c) << 32),
						layer, a, b, 1.25f * width, 0.5f * width);
					if (!box)
						return;
					box->allocationScale = 0.f;
					box->temperature = 0.f;
					box->coupleRateTemperature = rate;
				}
			}
		}
	}

	// Where it hits: a ball growing at `expansion` at its surface diverges by
	// 3 * expansion / radius, as an explosion's. Whether or not the core
	// reaches it - the model does - but not right in front of the view - a
	// wall at point blank. Its glow as bright as its heat makes it.
	if (e->hit && *num_spheres < (int)LENGTH(flow.spheres) &&
		(mirror || Distance(e->end, flow.view_origin) > FLOW_HEATBEAM_POINT_BLANK)) {
		const float radius = max(cvar_pt_flow_heatbeam_impact_radius->value, cell_size);
		NvFlowGridEmitterSphereParams *sphere = &flow.spheres[*num_spheres];
		*sphere = NvFlowEmitterSphereParams_default;
		sphere->luid = e->luid | (1ull << 48);
		sphere->layer = layer;
		sphere->position.x = e->end[0] - dir[0] * 0.5f * radius;
		sphere->position.y = e->end[1] - dir[1] * 0.5f * radius;
		sphere->position.z = e->end[2] - dir[2] * 0.5f * radius;
		sphere->radius = radius;
		sphere->multisample = NV_FLOW_TRUE;
		sphere->velocity.x = sphere->velocity.y = sphere->velocity.z = 0.f;
		sphere->divergence = 3.f * cvar_pt_flow_heatbeam_impact_expansion->value * (2.f * cell_size) / radius;
		sphere->temperature = cvar_pt_flow_heatbeam_impact_temperature->value;
		sphere->fuel = 1.f;
		sphere->burn = 0.f;
		sphere->smoke = cvar_pt_flow_heatbeam_impact_smoke->value;
		sphere->coupleRateVelocity = 0.f;
		sphere->coupleRateDivergence = rate;
		sphere->coupleRateTemperature = rate;
		sphere->coupleRateFuel = rate;
		sphere->coupleRateBurn = 0.f;
		sphere->coupleRateSmoke = rate;
		flow.sphere_ptrs[(*num_spheres)++] = sphere;
	}

	/* THE CORE'S FIGURE (Matt, 11-12: the core "looks a little bit monotone";
	   brighter toward the middle and fading out to what it is, or brighter lines
	   in it, as the classic beam's model has). Laid last, so that a full box
	   list loses only these, and after the core, which Flow lays in the order
	   given (EmitterBox.cpp keeps it, block by block), so that they lay over
	   it: heat and fuel only, the core's push and swell left as they are.

	   A hotter middle: each piece of the core again, pt_flow_heatbeam_inner_radius
	   of its radius round, at pt_flow_heatbeam_inner_temperature - 1 is the top
	   of the fire map, some three times as bright as the core's 0.88 and
	   yellower - and pt_flow_heatbeam_inner_brightness of the piece's glow. With
	   soft fill its edge goes over to the core's across a cell.

	   Strands: pt_flow_heatbeam_strands thin lines twisting round inside the
	   core, a turn every pt_flow_heatbeam_strand_pitch units, at
	   pt_flow_heatbeam_strand_radius of the core's radius out, running out
	   along the beam as the rings do (pt_flow_heatbeam_ring_speed) - a twist that
	   travels - over its first pt_flow_heatbeam_strand_length units. White-hot
	   at pt_flow_heatbeam_strand_temperature. Their chords sit on fixed places
	   along the beam, so they do not crawl as the start of the core moves.
	   Worked out, not seen. */
	if (num_pieces > 0) {
		const float inner = Cvar_ClampValue(cvar_pt_flow_heatbeam_inner_radius, 0.f, 1.f);
		const float inner_brightness = max(cvar_pt_flow_heatbeam_inner_brightness->value, 0.f);
		if (inner > 0.f) {
			for (int i = 0; i < num_pieces; i++) {
				NvFlowGridEmitterBoxParams *box = flow_line_box(num_boxes, e->luid | (1ull << 57) | ((uint64_t)(i + 1) << 32),
					layer, pieces[i].a, pieces[i].b, max(inner * core_radius, 0.25f), 0.f);
				if (!box)
					return;
				box->temperature = cvar_pt_flow_heatbeam_inner_temperature->value;
				box->fuel = pieces[i].fuel * inner_brightness;
				box->coupleRateTemperature = core_rate;
				box->coupleRateFuel = core_rate;
			}
		}

		const int strands = Cvar_ClampInteger(cvar_pt_flow_heatbeam_strands, 0, 4);
		const float strand_to = min(to, from + max(cvar_pt_flow_heatbeam_strand_length->value, 0.f));
		if (strands > 0 && strand_to - from >= 1.f) {
			const float pitch = max(cvar_pt_flow_heatbeam_strand_pitch->value, 8.f);
			const float r = core_radius * Cvar_ClampValue(cvar_pt_flow_heatbeam_strand_radius, 0.f, 1.f);
			const float half_width = max(cvar_pt_flow_heatbeam_strand_width->value, 0.1f);
			const float strand_brightness = max(cvar_pt_flow_heatbeam_strand_brightness->value, 0.f);
			const float chord = pitch / FLOW_HEATBEAM_STRAND_CHORDS;
			// how far the twist has run out along the beam
			const double run = (double)max(e->age - ring_back, 0.f) * max(cvar_pt_flow_heatbeam_ring_speed->value, 0.f);
			const int first = (int)floorf(from / chord);
			for (int k = 0; k < strands; k++) {
				for (int i = first; (float)i * chord < strand_to; i++) {
					const float s0 = max((float)i * chord, from);
					const float s1 = min((float)(i + 1) * chord, strand_to);
					if (s1 - s0 < 0.01f)
						continue;
					vec3_t a, b, along, middle;
					for (int end = 0; end < 2; end++) {
						const float s = end ? s1 : s0;
						const double turns = ((double)s - run) / pitch + (double)k / strands;
						const float angle = (float)(2.0 * M_PI * (turns - floor(turns)));
						vec3_t *p = end ? &b : &a;
						VectorMA(e->origin, s, dir, *p);
						VectorMA(*p, r * cosf(angle), right, *p);
						VectorMA(*p, r * sinf(angle), up, *p);
					}
					VectorSubtract(b, a, along);
					VectorNormalize(along);
					VectorAdd(a, b, middle);
					VectorScale(middle, 0.5f, middle);
					const uint64_t id = (uint64_t)(k * 4096 + (i - first) + 1);
					NvFlowGridEmitterBoxParams *box = flow_line_box(num_boxes, e->luid | (1ull << 54) | (id << 32),
						layer, a, b, half_width, half_width);
					if (!box)
						return;
					box->temperature = cvar_pt_flow_heatbeam_strand_temperature->value;
					box->fuel = brightness * strand_brightness * flow_heatbeam_view_glow(middle, along, mirror);
					box->coupleRateTemperature = core_rate;
					box->coupleRateFuel = core_rate;
				}
			}
		}
	}
}

/*
THE WARM-UP (pt_flow_prewarm). Flow makes its GPU memory the first time it
steps with blocks, not at createGrid: with no blocks its passes return before
they ask for any (Advection.cpp, Pressure.cpp, Vorticity.cpp: numLocations 0).
So the first explosion after the game starts made it all on its own frame -
13 textures, 883 MB at the 3584-block pool, each a zeroed committed resource
made on the spot (CreateCommittedResource on D3D12, a fresh allocation on
Vulkan), with the shadow pass's and the collision mesh's buffers on top - a
hitch on that explosion (Matt, 11-13). Headless at 3584 (a copy of
d3d12_flow_bench): that frame took 30 ms on an idle GPU, every later one
under 2; the game's own GPU memory, nearly full, makes it worse.

So one step after a map loads makes it, with emitters that give nothing: a
sphere on the explosions' layer and a box on the railgun's (the boxes' path
has buffers of its own), coupling nothing - the gas in their blocks stays
zero, so nothing is drawn or lit - and Flow's shadow pass on the sphere's
layer, and the collision mesh round it. Their blocks hold no gas, so Flow
drops them by itself some five steps on; no clear is needed, nor wanted (it
would take a real explosion of those frames with it). What Flow made stays:
its textures are held from one step to the next, and 87 steps of an empty
grid later the first real one made no texture (headless, 11-13; the backend
frees only what nothing holds). The first explosion then made 3.7 ms of small
buffers - sized by what is emitted, which no warm-up can guess - not 30 ms.
*/
#define FLOW_WARM_LUID      0x7f000000ull   // clear of the emitters' (0x1000 up) and the layers'/mesh's
#define FLOW_WARM_DT        (1.f / 60.f)

static void flow_warm_emitters(int *num_spheres, int *num_boxes, float cell_size)
{
	if (*num_spheres < (int)LENGTH(flow.spheres)) {
		NvFlowGridEmitterSphereParams *sphere = &flow.spheres[*num_spheres];
		*sphere = NvFlowEmitterSphereParams_default;
		sphere->luid = FLOW_WARM_LUID;
		sphere->layer = R_FLOW_FIRE;
		sphere->position.x = flow.warm_origin[0];
		sphere->position.y = flow.warm_origin[1];
		sphere->position.z = flow.warm_origin[2];
		sphere->radius = cell_size;
		sphere->velocity.x = sphere->velocity.y = sphere->velocity.z = 0.f;
		sphere->divergence = 0.f;
		sphere->temperature = 0.f;
		sphere->fuel = 0.f;
		sphere->burn = 0.f;
		sphere->smoke = 0.f;
		sphere->coupleRateVelocity = 0.f;
		sphere->coupleRateDivergence = 0.f;
		sphere->coupleRateTemperature = 0.f;
		sphere->coupleRateFuel = 0.f;
		sphere->coupleRateBurn = 0.f;
		sphere->coupleRateSmoke = 0.f;
		flow.sphere_ptrs[(*num_spheres)++] = sphere;
	}

	// flow_line_box couples nothing as it comes
	vec3_t a, b;
	VectorCopy(flow.warm_origin, a);
	VectorCopy(flow.warm_origin, b);
	a[0] += 16.f;
	b[0] += 48.f;
	flow_line_box(num_boxes, FLOW_WARM_LUID + 1, FLOW_LAYER_RAIL, a, b, flow_layer_cell_size(FLOW_LAYER_RAIL), 0.f);
}

static void flow_build_params(float dt)
{
	const float cell_size = max(cvar_pt_flow_cell_size->value, 0.5f);
	const float couple_rate = max(cvar_pt_flow_couple_rate->value, 0.f);
	bool collide = flow_collision_update();

	flow_every_frame();
	flow_step_mirror(dt);

	for (int layer = 0; layer < FLOW_LAYERS; layer++) {
		flow_layer_simulate(&flow.simulate[layer], layer, collide);
		flow.simulate_ptrs[layer] = &flow.simulate[layer];
	}

	int num_spheres = 0;
	int num_boxes = 0;
	for (int i = 0; i < flow.num_emitters; i++) {
		flow_emitter_t *e = &flow.emitters[i];
		if (!flow_emitter_emitting(e))
			continue;

		if (e->kind == R_FLOW_BURN) {
			num_spheres = flow_burn_spheres(e, num_spheres, couple_rate);
			continue;
		}
		if (e->kind == R_FLOW_TRAIL && e->layer == R_FLOW_FIRE) {
			if (e->smoke_only)
				num_spheres = flow_grenade_spheres(e, num_spheres, cell_size, couple_rate);
			else
				num_spheres = flow_rocket_spheres(e, num_spheres, cell_size, couple_rate);
			continue;
		}
		if (e->layer == R_FLOW_RAIL) {
			num_boxes = flow_rail_boxes(e, num_boxes, couple_rate);
			continue;
		}
		if (FLOW_LAYER_IS_HEATBEAM(e->layer)) {
			const int first_box = num_boxes, first_sphere = num_spheres;
			flow_heatbeam(e, &num_boxes, &num_spheres, couple_rate);
			if (flow_heatbeam_lean())
				flow_emit_no_velocity(first_box, num_boxes, first_sphere, num_spheres);
			continue;
		}

		float radius, expansion, temperature, fuel, smoke;
		flow_emitter_gas(e, &radius, &expansion, &temperature, &fuel, &smoke);
		radius = max(radius * e->scale, cell_size);
		// The divergence Flow's pressure solve keeps in each velocity cell is
		// half the difference of the velocities either side of it, in world
		// units per second (PressureDivergenceCS.hlsl). A ball growing at
		// `expansion` units/s at its surface diverges by 3 * expansion /
		// radius per unit length, and the velocity grid's cells are twice the
		// density grid's.
		const float divergence = 3.f * expansion * (2.f * cell_size) / radius;

		if (e->kind == R_FLOW_BEAM) {
			// A box along the segment: its x axis the beam, centred between
			// the ends. Flow's matrices act on row vectors, so the rows are
			// the axes and the last is the translation.
			// the railgun trails' boxes may have taken the room
			if (num_boxes >= (int)LENGTH(flow.boxes))
				continue;
			vec3_t dir, side, up;
			VectorSubtract(e->end, e->origin, dir);
			const float length = VectorNormalize(dir);
			if (length < 1.f)
				continue;
			// Quake's `up` here is side x dir; dir x side keeps the box a rotation
			MakeNormalVectors(dir, side, up);
			CrossProduct(dir, side, up);

			NvFlowGridEmitterBoxParams *box = &flow.boxes[num_boxes];
			*box = NvFlowEmitterBoxParams_default;
			box->luid = e->luid;
			box->layer = e->layer;
			const NvFlowFloat4x4 to_world = {
				dir[0], dir[1], dir[2], 0.f,
				side[0], side[1], side[2], 0.f,
				up[0], up[1], up[2], 0.f,
				0.5f * (e->origin[0] + e->end[0]), 0.5f * (e->origin[1] + e->end[1]), 0.5f * (e->origin[2] + e->end[2]), 1.f
			};
			box->localToWorld = to_world;
			box->localToWorldVelocity = to_world;
			box->position.x = box->position.y = box->position.z = 0.f;
			box->halfSize.x = 0.5f * length;
			box->halfSize.y = box->halfSize.z = radius;
			box->velocity.x = box->velocity.y = box->velocity.z = 0.f;
			box->divergence = divergence;
			box->temperature = temperature;
			box->fuel = fuel;
			box->burn = 0.f;
			box->smoke = smoke;
			box->coupleRateVelocity = 0.f;
			box->coupleRateDivergence = couple_rate;
			box->coupleRateTemperature = couple_rate;
			box->coupleRateFuel = couple_rate;
			box->coupleRateBurn = 0.f;
			box->coupleRateSmoke = couple_rate;
			flow.box_ptrs[num_boxes++] = box;
			continue;
		}

		// the rocket trails' chains may have taken the room
		if (num_spheres + 2 > (int)LENGTH(flow.spheres))
			continue;

		NvFlowGridEmitterSphereParams *sphere = &flow.spheres[num_spheres];
		*sphere = NvFlowEmitterSphereParams_default;
		sphere->luid = e->luid;
		sphere->layer = e->layer;
		sphere->position.x = e->origin[0];
		sphere->position.y = e->origin[1];
		sphere->position.z = e->origin[2];
		sphere->radius = radius;
		sphere->velocity.x = e->velocity[0];
		sphere->velocity.y = e->velocity[1];
		sphere->velocity.z = e->velocity[2];
		sphere->divergence = divergence;
		sphere->temperature = temperature;
		sphere->fuel = fuel;
		sphere->burn = 0.f;
		sphere->smoke = smoke;
		// The rates are per second: at 30 the cells inside reach the targets
		// within a frame or two, and the velocity is left to the expansion.
		// A blast's puff emits for only three frames, so it fills itself five
		// times as hard, and gives the gas its outward throw at the usual rate.
		const float rate = e->radius > 0.f ? 5.f * couple_rate : couple_rate;
		sphere->coupleRateVelocity = VectorEmpty(e->velocity) ? 0.f : couple_rate;
		sphere->coupleRateDivergence = rate;
		sphere->coupleRateTemperature = rate;
		sphere->coupleRateFuel = rate;
		sphere->coupleRateBurn = 0.f;
		sphere->coupleRateSmoke = rate;
		flow.sphere_ptrs[num_spheres++] = sphere;

		// The end of a trail's glow: a sphere as far behind its source as
		// pt_flow_trail_length, twice as wide as the trail, that pulls the
		// heat to nothing and leaves everything else - smoke, velocity - be.
		// It allocates nothing; it only acts on the blocks the trail has.
		const float length = cvar_pt_flow_trail_length->value * e->scale;
		if (e->kind == R_FLOW_TRAIL && length > 0.f && !VectorEmpty(e->heading) && e->layer != R_FLOW_BUBBLES) {
			NvFlowGridEmitterSphereParams *quench = &flow.spheres[num_spheres];
			*quench = NvFlowEmitterSphereParams_default;
			quench->luid = e->luid | (1ull << 48);   // its own, as long as the trail's
			quench->layer = e->layer;
			quench->position.x = e->origin[0] - e->heading[0] * length;
			quench->position.y = e->origin[1] - e->heading[1] * length;
			quench->position.z = e->origin[2] - e->heading[2] * length;
			quench->radius = 2.f * radius;
			quench->allocationScale = 0.f;
			quench->velocity.x = quench->velocity.y = quench->velocity.z = 0.f;
			quench->divergence = 0.f;
			quench->temperature = 0.f;
			quench->fuel = 0.f;
			quench->burn = 0.f;
			quench->smoke = 0.f;
			// gas passes through it in some 0.1 s and keeps an eighth of
			// its heat: below the colour map's glow
			quench->coupleRateVelocity = 0.f;
			quench->coupleRateDivergence = 0.f;
			quench->coupleRateTemperature = 20.f;
			quench->coupleRateFuel = 0.f;
			quench->coupleRateBurn = 0.f;
			quench->coupleRateSmoke = 0.f;
			flow.sphere_ptrs[num_spheres++] = quench;
		}
	}

	if (flow.warm_now)
		flow_warm_emitters(&num_spheres, &num_boxes, cell_size);

	/* COLLISION ONLY WHERE THERE IS GAS (pt_flow_collision_layers 1). Flow
	   builds the collision mesh's tree again for every layer that has
	   physicsCollisionEnabled, every step - its bounds, an 8-pass radix sort,
	   the tree, the closest-face search (EmitterMesh.cpp: a physics-collision
	   mesh is copied to each such layer) - and skips that only when the whole
	   grid is empty, not when a layer is. With all nine layers on, a plasma
	   beam's step spent some 1.3 ms of its 1.7 on it (pt_flow_profile, 11-12),
	   most of it for layers that held nothing. A layer with no gas has nothing
	   to keep off the walls, and a layer gets gas only from its emitters - this
	   step's, all in these lists - or from the blocks it had, last frame's
	   layer_blocks; those layers keep it. So do all of them at 0. The plasma
	   beam's lean layers move nothing at all (flow_heatbeam_lean). */
	if (collide && cvar_pt_flow_collision_layers->integer) {
		bool active[FLOW_LAYERS];
		for (int l = 0; l < FLOW_LAYERS; l++)
			active[l] = flow.layer_blocks[l] > 0;
		for (int i = 0; i < num_boxes; i++)
			if (flow.box_ptrs[i]->layer >= 0 && flow.box_ptrs[i]->layer < FLOW_LAYERS)
				active[flow.box_ptrs[i]->layer] = true;
		for (int i = 0; i < num_spheres; i++)
			if (flow.sphere_ptrs[i]->layer >= 0 && flow.sphere_ptrs[i]->layer < FLOW_LAYERS)
				active[flow.sphere_ptrs[i]->layer] = true;
		bool any = false;
		for (int l = 0; l < FLOW_LAYERS; l++) {
			if (FLOW_LAYER_IS_HEATBEAM(l) && flow_heatbeam_lean())
				active[l] = false;
			if (!active[l])
				flow.simulate[l].physicsCollisionEnabled = NV_FLOW_FALSE;
			any = any || active[l];
		}
		// With no layer to take it, Flow would run the mesh as an ordinary
		// emitter on its own layer (0) - for nothing.
		collide = any;
	}

	flow.shadow_pass = flow_build_offscreen(cell_size);

	// The warm-up's shadow pass, whose textures Flow makes the first time it
	// runs one - only where explosions would light one (pt_flow_lighting).
	// The march never reads it: the layer has no light (flow_layer_look).
	if (flow.warm_now && cvar_pt_flow_shadow->integer && cvar_pt_flow_lighting->integer) {
		NvFlowGridOffscreenLayerParams *off = &flow.offscreen[R_FLOW_FIRE];
		if (!off->shadow.enabled) {
			off->shadow.enabled = NV_FLOW_TRUE;
			off->shadow.isPointLight = NV_FLOW_TRUE;
			off->shadow.lightPosition.x = flow.warm_origin[0];
			off->shadow.lightPosition.y = flow.warm_origin[1];
			off->shadow.lightPosition.z = flow.warm_origin[2] + 32.f;
			off->shadow.attenuation = max(cvar_pt_flow_density->value, 0.f) / cell_size;
			off->shadow.minIntensity = Cvar_ClampValue(cvar_pt_flow_shadow_min, 0.f, 1.f);
		}
		flow.shadow_pass = true;
	}

	flow.version++;
	flow.type_snapshots[0] = (NvFlowDatabaseTypeSnapshot) {
		flow.version, &NvFlowGridSimulateLayerParams_NvFlowReflectDataType,
		(NvFlowUint8 **)flow.simulate_ptrs, FLOW_LAYERS
	};
	flow.type_snapshots[1] = (NvFlowDatabaseTypeSnapshot) {
		flow.version, &NvFlowGridEmitterSphereParams_NvFlowReflectDataType,
		(NvFlowUint8 **)flow.sphere_ptrs, (NvFlowUint64)num_spheres
	};
	flow.type_snapshots[2] = (NvFlowDatabaseTypeSnapshot) {
		flow.version, &NvFlowGridEmitterBoxParams_NvFlowReflectDataType,
		(NvFlowUint8 **)flow.box_ptrs, (NvFlowUint64)num_boxes
	};
	flow.type_snapshots[3] = (NvFlowDatabaseTypeSnapshot) {
		flow.version, &NvFlowGridEmitterMeshParams_NvFlowReflectDataType,
		(NvFlowUint8 **)&flow.mesh_ptr, collide ? 1u : 0u
	};
	flow.type_snapshots[4] = (NvFlowDatabaseTypeSnapshot) {
		flow.version, &NvFlowGridOffscreenLayerParams_NvFlowReflectDataType,
		(NvFlowUint8 **)flow.offscreen_ptrs, FLOW_LAYERS
	};

	flow.sim_time += dt;

	NvFlowGridParamsDescSnapshot snapshot = {
		.snapshot = { flow.version, flow.type_snapshots, LENGTH(flow.type_snapshots) },
		.absoluteSimTime = flow.sim_time,
		.deltaTime = dt,
		.globalForceClear = flow.force_clear,
	};
	flow.params_iface->commitParams(flow.grid_params, &snapshot);
}

// Whether the rocket trails may have more of Flow's blocks: while the gas
// fills less than `share` x pt_flow_rocket_budget of them. One rocket's smoke
// across a big room takes some 250 blocks, so with the old pool of 512 more
// than one left the explosions none. Past the share R_FlowEmit gives 0 and
// the client draws its particle flame instead. Explosions are never refused:
// what the trails leave is kept for them, as Flow drops new blocks, not old
// ones, once the pool is full (compactNonZeroWithLimit) - a blast into a full
// pool comes out cut off.
static bool flow_rocket_room(float share)
{
	const float budget = Cvar_ClampValue(cvar_pt_flow_rocket_budget, 0.f, 1.f);
	return (float)flow.num_blocks < share * budget * (float)flow.max_blocks;
}

// Whether something on fire (R_FLOW_BURN) may have more of Flow's blocks:
// while the gas fills less than `share` x pt_flow_burn_budget of them - one
// starts within it and keeps going to half as much again, as a rocket's trail.
static bool flow_burn_room(float share)
{
	const float budget = Cvar_ClampValue(cvar_pt_flow_burn_budget, 0.f, 1.f);
	return (float)flow.num_blocks < share * budget * (float)flow.max_blocks;
}

// Whether a railgun's trail may start: while the gas fills less than
// pt_flow_rail_budget of Flow's blocks. At the default 0.5-unit cells a block
// is 16x8x8 units and Flow keeps a ring of neighbours round the gas, so a
// shot takes something like a block or more per unit of its length - worked
// out, not measured; a long one may want more than the pool has. Past the
// budget R_FlowEmit gives 0 and the client draws the classic particles
// instead.
static bool flow_rail_room(void)
{
	const float budget = Cvar_ClampValue(cvar_pt_flow_rail_budget, 0.f, 1.f);
	return (float)flow.num_blocks < budget * (float)flow.max_blocks;
}

// Whether a plasma beam may have more of Flow's blocks: while the gas fills
// less than `share` x pt_flow_heatbeam_budget of them. A beam starts within
// its share and keeps going to half as much again, as a rocket's trail
// (flow_rocket_room), so that one held at the edge does not flick between
// the gas and the client's rings frame by frame.
static bool flow_heatbeam_room(float share)
{
	const float budget = Cvar_ClampValue(cvar_pt_flow_heatbeam_budget, 0.f, 1.f);
	return (float)flow.num_blocks < share * budget * (float)flow.max_blocks;
}

// The shortest piece of a railgun's trail put on a grid of its own: a shorter
// one goes with the piece next to it, so that no sliver is left out.
#define FLOW_RAIL_MIN_PIECE 16.f

// The stretch of the railgun's trail `e` that is drawn: all of it up to
// pt_flow_rail_length units, or that much round the point of it nearest the
// view - which for the one who fired is the near end, and for the one shot
// at the far end.
//
// And the part of that within pt_flow_rail_lod_distance of the view, which
// goes on the fine grid; the rest goes on FLOW_LAYER_RAIL_FAR's coarser one.
// The distance grows as the view zooms in (flow.view_zoom). It is settled
// here, once, from where the shot was seen when it came in: the gas stays on
// the grid it started on for the second or so it lasts, and moving the cut
// while the shot still emits would leave a ragged seam.
static void flow_rail_stretch(flow_emitter_t *e)
{
	vec3_t dir, to_view;
	VectorSubtract(e->end, e->origin, dir);
	const float length = VectorNormalize(dir);
	const float keep = max(cvar_pt_flow_rail_length->value, 64.f);
	VectorSubtract(flow.view_origin, e->origin, to_view);
	// where along the shot the view is nearest it, and how far off it it is
	const float along = DotProduct(to_view, dir);
	const float across2 = max(DotProduct(to_view, to_view) - along * along, 0.f);

	e->from = 0.f;
	e->to = length;
	if (length > keep) {
		const float nearest = min(max(along, 0.f), length);
		e->from = min(max(nearest - 0.5f * keep, 0.f), length - keep);
		e->to = e->from + keep;
	}

	e->near_from = e->from;
	e->near_to = e->to;
	const float lod = max(cvar_pt_flow_rail_lod_distance->value, 0.f) * (flow.view_zoom > 0.f ? flow.view_zoom : 1.f);
	if (lod <= 0.f)
		return;

	// The shot is within `lod` of the view from along - reach to along + reach.
	const float reach2 = lod * lod - across2;
	const float reach = reach2 > 0.f ? sqrtf(reach2) : 0.f;
	e->near_from = min(max(along - reach, e->from), e->to);
	e->near_to = min(max(along + reach, e->from), e->to);

	// no slivers, on either grid
	if (e->near_to - e->near_from < FLOW_RAIL_MIN_PIECE) {
		e->near_from = e->near_to = e->from;
		return;
	}
	if (e->near_from - e->from < FLOW_RAIL_MIN_PIECE)
		e->near_from = e->from;
	if (e->to - e->near_to < FLOW_RAIL_MIN_PIECE)
		e->near_to = e->to;
}

// A trail's light slot, if it still has it.
static flow_light_t *flow_trail_light(const flow_emitter_t *e)
{
	if (e->light < 0 || e->light >= FLOW_MAX_LIGHTS)
		return NULL;
	flow_light_t *l = &flow.lights[e->light];
	if (l->layer != e->layer || l->generation != e->light_generation)
		return NULL;
	return l;
}

/*
Something on fire's light (R_FLOW_BURN; pt_flow_heatbeam_light 1 with
pt_flow_burn_light above 0), each frame it is given: the glow of its own
flames, gathered round its box's middle, as a rocket's fireball's is - so it is
as bright as the fire it is, and grows and dies with it - at
pt_flow_burn_light times FLOW_LIGHT_UNIT. The slot keeps to the box, as the BFG
ball's does to the ball, and once the fire is no longer given its light fades
with what is left of the glow (flow_age). A smoking gib has none. A fire left
without a slot, every slot being taken, tries again each frame.
*/
static void flow_burn_light(flow_emitter_t *e)
{
	flow_light_t *l = flow_trail_light(e);

	if (e->smoke_only || !flow_burn_lighting()) {
		if (l)
			l->layer = -1;
		e->light = -1;
		return;
	}

	if (!l) {
		// as far as its flames reach above the box
		const float half = max(max(e->half[0], e->half[1]), e->half[2]);
		e->light = flow_take_burn_light(e->origin, max(64.f, 2.f * half));
		if (e->light < 0)
			return;
		l = &flow.lights[e->light];
		e->light_generation = l->generation;
	}

	VectorCopy(e->origin, l->seed);
	l->age = 0.f;
	l->follow = true;
	l->strength = cvar_pt_flow_burn_light->value;
}

// Ages the emitters and the light slots by `dt`, and lets go of the emitters
// that are done: a burst or a beam that has had its time, a trail that was not
// given again this frame. Flow drops what it knew of an emitter once its luid
// stops coming.
static void flow_age(float dt)
{
	int kept = 0;
	for (int i = 0; i < flow.num_emitters; i++) {
		flow_emitter_t *e = &flow.emitters[i];
		e->age += dt;
		if (flow_emitter_given(e)) {
			if (!e->seen) {
				// its light fades with what is left of the glow, and the
				// blast at the end of a BFG ball's flight may take it over
				flow_light_t *l = flow_trail_light(e);
				if (l)
					l->follow = false;
				continue;
			}
			e->seen = false;
		}
		else if (e->age >= flow_emitter_life(e)) {
			continue;
		}
		flow.emitters[kept++] = *e;
	}
	flow.num_emitters = kept;

	for (int i = 0; i < FLOW_MAX_LIGHTS; i++) {
		if (flow.lights[i].layer >= 0)
			flow.lights[i].age += dt;
	}
}

// pt_flow_pool_stats 1: once a second while Flow has anything, how full its
// block pool is, by layer, the most it held since the last line, and how
// many trails went to the client's particles for want of room.
static void flow_pool_stats(void)
{
	if (!cvar_pt_flow_pool_stats->integer) {
		flow.pool_stats_ms = 0;
		return;
	}

	const unsigned ms = Sys_Milliseconds();
	if (flow.pool_stats_ms && ms - flow.pool_stats_ms < 1000)
		return;

	if (flow.pool_stats_ms) {
		// rockets / grenades never started, then cut off in flight
		uint32_t counts[2][2] = { 0 };
		for (uint32_t i = 0; i < flow.num_refused; i++)
			counts[flow.refused[i].grenade][flow.refused[i].cut]++;

		if (flow.peak_blocks || flow.num_refused || flow.refused_rail || flow.refused_heatbeam) {
			Com_Printf("FLOWPOOL %u/%u blocks, peak %u | fire %u (+%u rocket trails) plasma %u bubbles %u rail %u (+%u far) plasma beam %u (+%u first person, +%u mirror) | "
				"to particles: rockets %u (+%u cut), grenades %u (+%u cut), rail shots %u, plasma beam frames %u\n",
				flow.num_blocks, flow.max_blocks, flow.peak_blocks,
				flow.layer_blocks[R_FLOW_FIRE], flow.layer_blocks[FLOW_LAYER_FIRE_TRAIL], flow.layer_blocks[R_FLOW_PLASMA],
				flow.layer_blocks[R_FLOW_BUBBLES], flow.layer_blocks[R_FLOW_RAIL],
				flow.layer_blocks[FLOW_LAYER_RAIL_FAR], flow.layer_blocks[R_FLOW_HEATBEAM],
				flow.layer_blocks[FLOW_LAYER_HEATBEAM_VIEW], flow.layer_blocks[FLOW_LAYER_HEATBEAM_MIRROR],
				counts[0][0], counts[0][1], counts[1][0], counts[1][1], flow.refused_rail, flow.refused_heatbeam);
		}
	}

	flow.pool_stats_ms = ms;
	flow.peak_blocks = flow.num_blocks;
	flow.num_refused = 0;
	flow.refused_rail = 0;
	flow.refused_heatbeam = 0;
}

// A trail of fire sent back to the client's particles, for flow_pool_stats:
// once a source, however many frames it asks again, `cut` if it had been in
// Flow before.
static void flow_refused(const r_flow_emit_t *emit, bool cut)
{
	if (!cvar_pt_flow_pool_stats->integer)
		return;

	uint32_t i = 0;
	while (i < flow.num_refused && flow.refused[i].key != emit->key)
		i++;
	if (i == flow.num_refused) {
		if (i == FLOW_REFUSED_TRAILS)
			return;
		flow.num_refused++;
		flow.refused[i].key = emit->key;
		flow.refused[i].grenade = emit->smoke_only;
		flow.refused[i].cut = false;
	}
	flow.refused[i].cut |= cut;
}

/* pt_flow_max_blocks changed - the menu's slider, or the console: Flow takes
   the new pool size at its next step (resetGrid), clearing the gas. Its
   textures of the old size stay referenced until one more reset, so a second
   follows the first step on the new pool (flow_pool_step). Measured headless
   on D3D12 (11-07): 4096 -> 1024 held 2070 MB with one reset, 470 with the
   second one step later - the same as starting at 1024. Then Flow keeps
   stepping FLOW_POOL_SETTLE times, gas or none, for its backend to free
   them. */
static void flow_pool_resize(void)
{
	NvFlowGridDesc grid_desc = flow_grid_desc();
	if (grid_desc.maxLocations == flow.max_blocks)
		return;

	flow.grid_iface->resetGrid(flow.context, flow.grid, &grid_desc);
	Com_DPrintf("NVIDIA Flow: pool %u -> %u blocks.\n", flow.max_blocks, grid_desc.maxLocations);
	flow.max_blocks = grid_desc.maxLocations;
	flow.num_blocks = 0;
	flow.pool_reset_again = true;
	flow.pool_settle = FLOW_POOL_SETTLE;
	flow.idle = false;
}

// After each step Flow has taken: the second reset of a resize, and the
// count of steps it must still take.
static void flow_pool_step(void)
{
	if (flow.pool_reset_again) {
		NvFlowGridDesc grid_desc = flow_grid_desc();
		grid_desc.maxLocations = flow.max_blocks;
		flow.grid_iface->resetGrid(flow.context, flow.grid, &grid_desc);
		flow.pool_reset_again = false;
	}
	else if (flow.pool_settle > 0) {
		flow.pool_settle--;
	}
}

/*
A plasma beam's gas laid again under a pause. Paused, Flow holds still
(flow_simulate), but the client goes on giving the beam where it is now - and
photo mode's free camera, once moved, switches to third person, which moves
your own beam from the gun you see in first person (at the eye, some 15 units
ahead of and 10 above your body's) to the barrel of the gun your body holds.
The gas stayed where first person laid it, its core dimmed for being seen
end-on from the eye: a beam ahead of the gun and above it, its core faint
(Matt, 11-09: "it needs to be right in both").

So, under a pause, when a beam's line has moved since its gas was laid - or
the view has come to rest more than 8 units from where it was then, the core's
end-on dimming and near fade being the view's - the beam's layer is cleared on
one frame and laid again over the next ones: Flow steps though there is no
game time, with every other layer's emitters off. Flow's advection takes every
layer by the same time, so it is only where no other layer has any blocks that
the beam is run for pt_flow_heatbeam_relay seconds, a step a frame - its rings
running out over them, its glow lifting off; with other gas about it is laid
in one step of a thousandth of a second, which leaves that gas as it was: the
core and the rings, without what they leave behind. Photo mode's picture
starts again on each of these frames. When time runs again what was laid
goes, and the beam builds again where the one firing sees it.
pt_flow_heatbeam_relay 0: never.

Run before flow_build_params, which reads flow.relay_step.
*/
static void flow_heatbeam_relay(float dt)
{
	flow.heatbeam_clear = false;

	bool any = false, moved = false;
	for (int i = 0; i < flow.num_emitters; i++) {
		const flow_emitter_t *e = &flow.emitters[i];
		if (!FLOW_LAYER_IS_HEATBEAM(e->layer) || !flow_emitter_emitting(e))
			continue;
		any = true;
		if (Distance(e->origin, e->laid_origin) > 1.f || Distance(e->end, e->laid_end) > 1.f)
			moved = true;
	}
	const bool still = Distance(flow.view_origin, flow.last_view) < 0.1f;
	VectorCopy(flow.view_origin, flow.last_view);

	const float relay_time = max(cvar_pt_flow_heatbeam_relay->value, 0.f);
	if (dt > 0.f || relay_time <= 0.f || !any) {
		// time runs (or none to lay): the gas is laid where the beam is given,
		// and what a pause laid goes
		flow.heatbeam_clear = flow.relaid && dt > 0.f;
		if (dt > 0.f)
			flow.relaid = false;
		flow.relay_step = -1;
		for (int i = 0; i < flow.num_emitters; i++) {
			flow_emitter_t *e = &flow.emitters[i];
			if (FLOW_LAYER_IS_HEATBEAM(e->layer)) {
				VectorCopy(e->origin, e->laid_origin);
				VectorCopy(e->end, e->laid_end);
			}
		}
		VectorCopy(flow.view_origin, flow.relay_view);
		return;
	}

	if (moved || (still && Distance(flow.view_origin, flow.relay_view) > 8.f)) {
		bool others = false;
		for (int l = 0; l < FLOW_LAYERS; l++) {
			if (!FLOW_LAYER_IS_HEATBEAM(l) && flow.layer_blocks[l] > 0)
				others = true;
		}
		const float rate = cvar_pt_flow_rate->value > 0.f ? cvar_pt_flow_rate->value : 60.f;
		flow.relay_steps = others ? 1 : max((int)ceilf(relay_time * rate), 1);
		flow.relay_dt = others ? 1e-3f : 1.f / rate;
		flow.relay_step = 0;
		flow.relaid = true;
		for (int i = 0; i < flow.num_emitters; i++) {
			flow_emitter_t *e = &flow.emitters[i];
			if (FLOW_LAYER_IS_HEATBEAM(e->layer)) {
				VectorCopy(e->origin, e->laid_origin);
				VectorCopy(e->end, e->laid_end);
			}
		}
		VectorCopy(flow.view_origin, flow.relay_view);
		vkpt_reset_accumulation();
		return;
	}

	if (flow.relay_step >= 0 && flow.relay_step < flow.relay_steps) {
		flow.relay_step++;
		vkpt_reset_accumulation();
	}
	else {
		flow.relay_step = -1;
	}
}

// Runs this frame's simulation and acquires its result for `set`, filling
// `params` with where everything is. False when there is nothing to draw.
/*
pt_flow_profile 1: Flow's own profiler (NvFlowDeviceInterface enableProfiler -
both its Vulkan backend and ours for D3D12 put a timestamp after every pass),
summed by pass and printed once a second as FLOWPROF lines: GPU time per frame
for each of Flow's passes, their total, and the CPU time flow_simulate takes
(laying the emitters, Flow's recording, its submission). The profiler's
"flow simulation" row runs from before all of that to after Flow's work on the
queue, so where the row is well over the passes' total, the queue waited for
the CPU in between.
*/
#define FLOW_PROF_LABELS 64
static struct {
	bool        on;
	const void  *context;    // the one it is on for: a new one starts without it
	unsigned    start_ms;
	int         frames, captures;
	uint64_t    cpu_us, build_us;
	int         num;
	struct {
		char    label[48];
		double  gpu;
		int     count;
	} e[FLOW_PROF_LABELS];
} flow_prof;

static void NV_FLOW_ABI flow_prof_report(void *userdata, NvFlowUint64 capture_id, NvFlowUint num, NvFlowProfilerEntry *entries)
{
	(void)userdata; (void)capture_id;
	flow_prof.captures++;
	for (NvFlowUint i = 0; i < num; i++) {
		const char *label = entries[i].label ? entries[i].label : "?";
		int k;
		for (k = 0; k < flow_prof.num; k++)
			if (!strncmp(flow_prof.e[k].label, label, sizeof(flow_prof.e[k].label) - 1))
				break;
		if (k == flow_prof.num) {
			if (k == FLOW_PROF_LABELS)
				continue;
			Q_strlcpy(flow_prof.e[k].label, label, sizeof(flow_prof.e[k].label));
			flow_prof.e[k].gpu = 0.0;
			flow_prof.e[k].count = 0;
			flow_prof.num++;
		}
		flow_prof.e[k].gpu += entries[i].gpuDeltaTime;
		flow_prof.e[k].count++;
	}
}

static void flow_prof_update(void)
{
	const bool want = cvar_pt_flow_profile->integer != 0 && flow.device_iface && flow.context;
	if (flow_prof.on && flow_prof.context != flow.context)
		memset(&flow_prof, 0, sizeof(flow_prof));
	if (want != flow_prof.on) {
		if (want)
			flow.device_iface->enableProfiler(flow.context, NULL, flow_prof_report);
		else
			flow.device_iface->disableProfiler(flow.context);
		memset(&flow_prof, 0, sizeof(flow_prof));
		flow_prof.on = want;
		flow_prof.context = flow.context;
		flow_prof.start_ms = Sys_Milliseconds();
	}
	if (!flow_prof.on)
		return;

	const unsigned ms = Sys_Milliseconds();
	if (ms - flow_prof.start_ms < 1000 || flow_prof.frames == 0)
		return;
	const double per = 1000.0 / flow_prof.frames;  // seconds summed -> ms a frame
	double total = 0.0;
	for (int k = 0; k < flow_prof.num; k++)
		total += flow_prof.e[k].gpu;
	Com_Printf("FLOWPROF %d frames, %d captures: Flow passes %.3f ms GPU a frame; flow_simulate CPU %.3f ms "
		"(emitters %.3f) a frame; %u blocks, %d collision triangles\n", flow_prof.frames, flow_prof.captures, total * per,
		flow_prof.cpu_us / 1000.0 / flow_prof.frames, flow_prof.build_us / 1000.0 / flow_prof.frames, flow.num_blocks,
		flow.mesh_ptr ? flow.num_mesh_tris : 0);
	// the passes, the costliest first, while they are worth a line
	for (int printed = 0; printed < 16; printed++) {
		int best = -1;
		for (int k = 0; k < flow_prof.num; k++)
			if (flow_prof.e[k].count >= 0 && (best < 0 || flow_prof.e[k].gpu > flow_prof.e[best].gpu))
				best = k;
		if (best < 0 || flow_prof.e[best].gpu * per < 0.005)
			break;
		Com_Printf("FLOWPROF   %-32s %.3f ms (%.1f a frame)\n", flow_prof.e[best].label,
			flow_prof.e[best].gpu * per, (double)flow_prof.e[best].count / flow_prof.frames);
		flow_prof.e[best].count = -1;
	}
	flow_prof.num = 0;
	flow_prof.frames = flow_prof.captures = 0;
	flow_prof.cpu_us = flow_prof.build_us = 0;
	flow_prof.start_ms = ms;
}

static bool flow_simulate_inner(const refdef_t *fd, int set, FlowParams *params);

static bool flow_simulate(const refdef_t *fd, int set, FlowParams *params)
{
	flow_prof_update();
	const uint64_t t0 = flow_prof.on ? Sys_Microseconds() : 0;
	const bool ret = flow_simulate_inner(fd, set, params);
	if (flow_prof.on && ret) {
		flow_prof.cpu_us += Sys_Microseconds() - t0;
		flow_prof.frames++;
	}
	return ret;
}

static bool flow_simulate_inner(const refdef_t *fd, int set, FlowParams *params)
{
	flow_pool_stats();
	flow_pool_resize();

	float dt = 0.f;
	if (flow.have_last_time && fd->time >= flow.last_time)
		dt = fd->time - flow.last_time;
	flow.last_time = fd->time;
	flow.have_last_time = true;

	// Below 20 fps the gas slows down rather than taking steps too long
	// for the solver; paused, it holds still.
	dt = min(dt, 0.05f);
	if (dt > 0.f)
		flow.frame_dt = flow.frame_dt > 0.f ? 0.9f * flow.frame_dt + 0.1f * dt : dt;
	flow_rail_fade(dt);

	if (flow.idle && flow.num_emitters == 0 && !flow.force_clear && !flow.warm_pending)
		return false;

	// A plasma beam laid again under a pause steps Flow by a time of its own.
	flow_heatbeam_relay(dt);
	const bool relaying = flow.relay_step >= 0;

	// The warm-up (flow_warm_emitters), at the view: not on the map's clearing
	// frame, whose clear would leave it no blocks to make memory for. A step
	// of its own time, paused or not.
	flow.warm_now = flow.warm_pending && !flow.force_clear && !relaying;
	if (flow.warm_now)
		VectorCopy(fd->vieworg, flow.warm_origin);
	const float sim_dt = relaying ? (flow.relay_step > 0 ? flow.relay_dt : 0.f) : flow.warm_now ? FLOW_WARM_DT : dt;

	const uint64_t build_t0 = flow_prof.on ? Sys_Microseconds() : 0;
	flow_build_params(sim_dt);
	if (flow_prof.on)
		flow_prof.build_us += Sys_Microseconds() - build_t0;

	/* Paused - no game time - the gas holds still, and Flow is not stepped at all.
	   Below pt_flow_rate fps (or at pt_flow_rate 0) its variable time step takes a
	   solver step every frame even at dt 0 (Grid.cpp: at least one), and that is not
	   quite nothing - the pressure solve, the advection resampling the atlas - so the
	   gas kept moving a little under the pause (Matt, 11-06). Without a simulate
	   getRenderData hands over what the last one left (the density, and the shadow
	   pass's copy of it). A force clear still runs, and so does a plasma beam
	   being laid again (flow_heatbeam_relay). */
	const bool hold = dt <= 0.f && flow.have_blocks && !flow.force_clear && !relaying && !flow.warm_now;

	NvFlowGridParamsSnapshot *snapshot = flow.params_iface->getParamsSnapshot(flow.grid_params, flow.sim_time, 0);
	NvFlowGridParamsDesc params_desc = { 0 };
	NvFlowTextureAcquire *density_acquire = NULL;
	NvFlowBufferAcquire *table_acquire = NULL;
	bool have_blocks = false;
	bool have_bubbles = false;

	if (flow.params_iface->mapParamsDesc(flow.grid_params, snapshot, &params_desc)) {
		NvFlowGridRenderData render_data = { 0 };

		if (!hold) {
			flow.grid_iface->simulate(flow.context, flow.grid, &params_desc, flow.force_clear);
			// The shadow goes into the density's burn channel, and what
			// getRenderData hands over after this is that copy.
			if (flow.shadow_pass)
				flow.grid_iface->offscreen(flow.context, flow.grid, &params_desc);
			// At dt 0 simulate returns before it takes a pending reset
			// (Grid.cpp canSkipSimulation); at any other, even one its
			// stepper takes no step on, it does - and so it does on a plasma
			// beam's clearing, whose forced clear keeps it from returning.
			if (sim_dt > 0.f || relaying)
				flow_pool_step();
			if (flow.warm_now)
				flow.warm_pending = false;
		}
		flow.grid_iface->getRenderData(flow.context, flow.grid, &render_data);

		const NvFlowSparseParams *sparse = &render_data.sparseParams;
		memset(flow.layer_blocks, 0, sizeof(flow.layer_blocks));
		if (render_data.densityTexture && render_data.sparseBuffer &&
			sparse->levelCount > 0 && sparse->levels[0].numLocations > 0) {
			// The density level; every layer's blocks are in its one table.
			const NvFlowSparseLevelParams *level = &sparse->levels[0];
			params->block_dim_bits[0] = level->blockDimBits.x;
			params->block_dim_bits[1] = level->blockDimBits.y;
			params->block_dim_bits[2] = level->blockDimBits.z;
			params->table_dim_less_one[0] = level->tableDimLessOne.x;
			params->table_dim_less_one[1] = level->tableDimLessOne.y;
			params->table_dim_less_one[2] = level->tableDimLessOne.z;
			params->table_dim_less_one[3] = level->tableDimBits_x;
			params->table_offsets[0] = level->tableDimBits_xy;
			params->table_offsets[1] = level->locationOffset;
			params->table_offsets[2] = level->blockLevelOffsetGlobal;
			params->dim_inv[0] = level->dimInv.x;
			params->dim_inv[1] = level->dimInv.y;
			params->dim_inv[2] = level->dimInv.z;
			params->counts[0] = level->numLocations;
			flow.num_blocks = level->numLocations;
			flow.peak_blocks = max(flow.peak_blocks, flow.num_blocks);

			vec3_t world_min = { FLT_MAX, FLT_MAX, FLT_MAX };
			vec3_t world_max = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
			bool grid_seen[FLOW_GRIDS] = { false };

			for (NvFlowUint i = 0; i < sparse->layerCount; i++) {
				const NvFlowSparseLayerParams *layer = &sparse->layers[i];
				int l = 0;
				while (l < FLOW_LAYERS && layer->layerAndLevel != NvFlow_packLayerAndLevel(l, 0))
					l++;
				if (l == FLOW_LAYERS || layer->numLocations == 0)
					continue;
				flow.layer_blocks[l] = layer->numLocations;

				FlowLayerParams *dst = &params->layers[l];
				VectorCopy(&layer->worldMin.x, dst->world_min);
				VectorCopy(&layer->worldMax.x, dst->world_max);
				dst->info[0] = 1;
				dst->info[1] = (uint32_t)layer->layerAndLevel;

				for (int k = 0; k < 3; k++) {
					world_min[k] = min(world_min[k], dst->world_min[k]);
					world_max[k] = max(world_max[k], dst->world_max[k]);
				}

				// And round each grid's layers alone, for the march to leave a
				// grid out where it has no blocks (pt_flow_march 8).
				const int g = FLOW_LAYER_GRID(l);
				for (int k = 0; k < 3; k++) {
					params->grid_min[g][k] = grid_seen[g] ? min(params->grid_min[g][k], dst->world_min[k]) : dst->world_min[k];
					params->grid_max[g][k] = grid_seen[g] ? max(params->grid_max[g][k], dst->world_max[k]) : dst->world_max[k];
				}
				grid_seen[g] = true;

				// The cells of the layer's grid; the layers of a grid all have the same.
				float *grid = params->grids[FLOW_LAYER_GRID(l)];
				grid[0] = (float)(level->blockDimLessOne.x + 1) * layer->blockSizeWorldInv.x;
				grid[1] = (float)(level->blockDimLessOne.y + 1) * layer->blockSizeWorldInv.y;
				grid[2] = (float)(level->blockDimLessOne.z + 1) * layer->blockSizeWorldInv.z;

				have_blocks = true;
				have_bubbles = have_bubbles || l == R_FLOW_BUBBLES;
			}

			if (have_blocks) {
				VectorCopy(world_min, params->world_min);
				VectorCopy(world_max, params->world_max);
				density_acquire = flow.context_iface->enqueueAcquireTexture(flow.context, render_data.densityTexture);
				table_acquire = flow.context_iface->enqueueAcquireBuffer(flow.context, render_data.sparseBuffer);
			}

			// for next frame's collision mesh
			VectorCopy(world_min, flow.blocks_min);
			VectorCopy(world_max, flow.blocks_max);
			flow.have_blocks = have_blocks;
		}
		else {
			flow.have_blocks = false;
			flow.num_blocks = 0;
		}

		flow.params_iface->unmapParamsDesc(flow.grid_params, snapshot);
	}

	flow.force_clear = false;
	flow.warm_now = false;

	// Submits to qvk.queue_graphics, through flow_queue_submit.
	NvFlowUint64 flushed_frame;
	if (flow.device_iface->flush(flow.queue, &flushed_frame, NULL, NULL)) {
		// the renderer's own next submission reports it
		return false;
	}

	if (density_acquire)
		flow.context_iface->getAcquiredTexture(flow.context, density_acquire, &flow.set_density[set]);
	if (table_acquire)
		flow.context_iface->getAcquiredBuffer(flow.context, table_acquire, &flow.set_table[set]);

	flow_age(dt);

	// Once the last bubbles are gone, the next ones may be in other water.
	if (!have_bubbles) {
		bool emitting = false;
		for (int i = 0; i < flow.num_emitters; i++)
			emitting = emitting || flow.emitters[i].layer == R_FLOW_BUBBLES;
		if (!emitting)
			flow.have_bubble_surface = false;
	}

	if (!have_blocks || !flow.set_density[set] || !flow.set_table[set]) {
		flow.idle = flow.num_emitters == 0 && !flow.pool_reset_again && flow.pool_settle == 0;
		flow_release_set(set);
		return false;
	}

	flow.idle = false;
	return true;
}

// A new emitter. When they are all taken the oldest that has started makes
// way - a BFG blast's pieces wait their turn for up to a quarter of a second,
// and lasers coming in every frame must not push them out half drawn - and
// failing that the oldest of all.
static flow_emitter_t *flow_new_emitter(r_flow_kind_t kind, int layer, const vec3_t origin, float scale)
{
	if (flow.num_emitters == FLOW_MAX_EMITTERS) {
		int oldest = 0;
		while (oldest < FLOW_MAX_EMITTERS - 1 && flow.emitters[oldest].age < 0.f)
			oldest++;
		if (flow.emitters[oldest].age < 0.f)
			oldest = 0;
		memmove(flow.emitters + oldest, flow.emitters + oldest + 1, sizeof(flow.emitters[0]) * (FLOW_MAX_EMITTERS - 1 - oldest));
		flow.num_emitters--;
	}

	flow_emitter_t *e = &flow.emitters[flow.num_emitters++];
	memset(e, 0, sizeof(*e));
	e->kind = kind;
	e->layer = layer;
	VectorCopy(origin, e->origin);
	VectorCopy(origin, e->end);
	e->scale = scale;
	e->luid = ++flow.next_luid + 0x1000;   // clear of the layers' and the mesh's luids
	flow.idle = false;
	return e;
}

// How far a blast's gas may go from its origin along `dir`, up to `distance`:
// the client's trace, or as far as it likes without one.
static float flow_blast_reach(const r_flow_emit_t *emit, const vec3_t dir, float distance)
{
	if (!emit->reach)
		return distance;
	return min(max(emit->reach(emit->origin, dir, distance), 0.f), distance);
}

// A piece of a blast: a plasma burst `distance` along `dir`, starting `start`
// seconds from now, thrown outward at 100 to 300 units/s.
static void flow_blast_puff(const vec3_t origin, const vec3_t dir, float distance,
	float radius, float expansion, float start, float life)
{
	vec3_t position;
	VectorMA(origin, distance, dir, position);

	flow_emitter_t *e = flow_new_emitter(R_FLOW_BURST, R_FLOW_PLASMA, position, 1.f);
	e->radius = radius;
	e->expansion = expansion;
	e->life = life;
	e->age = -start;
	if (distance > 1.f)
		VectorScale(dir, 200.f * (0.5f + frand()), e->velocity);
}

// A blast's piece `want` out along `dir`, or short of the wall that way by
// half its radius.
static float flow_blast_distance(const r_flow_emit_t *emit, const vec3_t dir, float want, float radius)
{
	return min(want, max(flow_blast_reach(emit, dir, want + radius) - 0.5f * radius, 0.f));
}

static void flow_random_dir(vec3_t dir)
{
	const float z = crand();
	const float r = sqrtf(max(1.f - z * z, 0.f));
	const float a = frand() * 2.f * M_PI;
	VectorSet(dir, r * cosf(a), r * sinf(a), z);
}

/*
The BFG's blast, after NVIDIA's 2019 RTX trailer (2:07 on). In the first
frames, a flower of pt_flow_bfg_puffs round puffs about the middle; then
pt_flow_bfg_lumps smaller ones further out and a little later, so that it
swells into a lumpy cloud and not a ball; and pt_flow_bfg_jets jets shot out
up to pt_flow_bfg_jet_length, strings of beads that grow as they go and end
in a bigger puff. Every piece is a burst of its own, a few frames long and
started late by a negative age, so the order is set here once and Flow
plays it out; the plasma's vorticity (pt_flow_bfg_vorticity) then tears it
all into ragged green smoke. `reach` - the client's trace - keeps each piece
short of the walls and points the jets where there is room for them.

Tuned headless against the trailer's frames: a flower at 0.05 s, the jets
out by 0.2 s, torn smoke from 0.4 s, the glow gone by 0.8 s.
*/
static void flow_emit_blast(const r_flow_emit_t *emit)
{
	const float u = max(cvar_pt_flow_bfg_blast_size->value, 0.1f) * emit->scale;
	const float push = max(cvar_pt_flow_bfg_blast_expansion->value, 0.f);
	const float puff_life = 0.05f;
	vec3_t dir;

	// the middle
	VectorClear(dir);
	flow_blast_puff(emit->origin, dir, 0.f, 12.f * u, push, 0.f, puff_life);

	// The flower: spread evenly round a sphere (a golden-angle spiral) turned
	// a random way, all but at once.
	const int lobes = Cvar_ClampInteger(cvar_pt_flow_bfg_puffs, 0, 16);
	const float turn = frand() * 2.f * M_PI;
	const float tilt = frand() * M_PI;
	for (int i = 0; i < lobes; i++) {
		const float z = 1.f - (2.f * i + 1.f) / lobes;
		const float r = sqrtf(max(1.f - z * z, 0.f));
		const float a = turn + i * 2.39996323f;
		VectorSet(dir, r * cosf(a),
			r * sinf(a) * cosf(tilt) - z * sinf(tilt),
			r * sinf(a) * sinf(tilt) + z * cosf(tilt));
		const float radius = 15.f * u;
		flow_blast_puff(emit->origin, dir, flow_blast_distance(emit, dir, 34.f * u, radius),
			radius, push, 0.005f * i, puff_life);
	}

	// The lumps: the further out, the later.
	const int lumps = Cvar_ClampInteger(cvar_pt_flow_bfg_lumps, 0, 48);
	for (int i = 0; i < lumps; i++) {
		flow_random_dir(dir);
		const float f = frand();
		const float radius = 11.f * u;
		flow_blast_puff(emit->origin, dir, flow_blast_distance(emit, dir, (35.f + 35.f * f) * u, radius),
			radius, push * (2.f / 3.f), 0.04f + 0.11f * (0.7f * f + 0.3f * frand()), puff_life);
	}

	// The jets, at 1000 units/s from a moment after the flower: each the way
	// with the most room of a few tried, at least 60 degrees from the others.
	const int jets = Cvar_ClampInteger(cvar_pt_flow_bfg_jets, 0, 8);
	const float longest = max(cvar_pt_flow_bfg_jet_length->value, 0.f) * u;
	const float end_radius = 11.f * u;
	const float first = 25.f * u;
	const float speed = 1000.f;
	vec3_t jet_dirs[8];

	for (int j = 0; j < jets; j++) {
		float room = -1.f;
		VectorClear(jet_dirs[j]);
		for (int tries = 0; tries < 16 && room < longest + end_radius; tries++) {
			flow_random_dir(dir);
			bool apart = true;
			for (int k = 0; k < j; k++)
				apart = apart && DotProduct(dir, jet_dirs[k]) < 0.5f;
			if (!apart)
				continue;
			const float r = flow_blast_reach(emit, dir, longest + end_radius);
			if (r > room) {
				room = r;
				VectorCopy(dir, jet_dirs[j]);
			}
		}

		const float length = min(longest * (0.6f + 0.4f * frand()), room - end_radius);
		if (length <= first)
			continue;

		const float delay = 0.06f * (0.6f + 0.8f * frand());
		for (float d = first; d < length; ) {
			const float radius = (5.f + 3.f * (d - first) / (length - first)) * u;
			flow_blast_puff(emit->origin, jet_dirs[j], d, radius, 0.5f * push, delay + d / speed, 0.04f);
			d += 1.7f * radius;
		}
		flow_blast_puff(emit->origin, jet_dirs[j], length, end_radius, push, delay + length / speed, 0.06f);
	}
}

static int vkpt_flow_emit(const r_flow_emit_t *emit)
{
	if (!flow.grid || !emit || emit->scale <= 0.f || (int)emit->style < 0 || emit->style >= R_FLOW_STYLES)
		return 0;

	int result = R_FLOW_DRAWN;

	switch (emit->kind) {
	case R_FLOW_BURST:
		{
			flow_emitter_t *e = flow_new_emitter(R_FLOW_BURST, emit->style, emit->origin, emit->scale);
			// A plasma burst is what the BFG does to a target it zaps: a small
			// puff that fills itself in a few frames, like a piece of its
			// blast, and pushes out gently - not a ball of its own. At the old
			// radius 24 and push 500 each swelled to some 50 blocks, measured
			// headless, and four of them took the BFG from 133 blocks to 339;
			// at 16 and 40 one takes 10 to 20.
			if (emit->style == R_FLOW_PLASMA) {
				e->radius = max(cvar_pt_flow_bfg_radius->value, 1.f);
				e->expansion = cvar_pt_flow_bfg_expansion->value;
				e->life = max(cvar_pt_flow_bfg_burst->value, 0.f);
			}
		}

		if (flow_take_light(emit->style, emit->origin, emit->scale) >= 0)
			result |= R_FLOW_LIT;

		if (emit->style == R_FLOW_BUBBLES) {
			flow.bubble_surface = flow.have_bubble_surface ? max(flow.bubble_surface, emit->surface) : emit->surface;
			flow.have_bubble_surface = true;
		}
		break;

	case R_FLOW_TRAIL:
		for (int i = 0; i < flow.num_emitters; i++) {
			flow_emitter_t *e = &flow.emitters[i];
			if (e->kind == R_FLOW_TRAIL && e->key == emit->key && e->layer == (int)emit->style &&
				e->smoke_only == emit->smoke_only) {
				// a rocket's or a grenade's lets go at half as much again as
				// its share...
				if (emit->style == R_FLOW_FIRE && !flow_rocket_room(1.5f)) {
					flow_refused(emit, true);
					return 0;
				}
				vec3_t moved;
				VectorSubtract(emit->origin, e->origin, moved);
				if (VectorLength(moved) > 0.5f)
					VectorNormalize2(moved, e->heading);
				VectorCopy(emit->origin, e->origin);
				e->scale = emit->scale;
				e->seen = true;
				flow_light_t *l = flow_trail_light(e);
				if (l) {
					VectorCopy(e->origin, l->seed);
					l->age = 0.f;
					result |= R_FLOW_LIT;
				}
				flow.idle = false;
				return result;
			}
		}
		// ... and starts only within it
		if (emit->style == R_FLOW_FIRE && !flow_rocket_room(1.f)) {
			flow_refused(emit, false);
			return 0;
		}
		{
			flow_emitter_t *e = flow_new_emitter(R_FLOW_TRAIL, emit->style, emit->origin, emit->scale);
			e->key = emit->key;
			e->seen = true;
			e->smoke_only = emit->style == R_FLOW_FIRE && emit->smoke_only;
			VectorCopy(emit->origin, e->last);
			// A light that keeps to its source: the glow behind it, gathered
			// from as far back as the trail glows. Only plasma has a trail's
			// light; flow_take_light knows no other.
			e->light = emit->style == R_FLOW_PLASMA ? flow_take_light(R_FLOW_PLASMA, emit->origin, emit->scale) : -1;
			if (e->light >= 0) {
				flow_light_t *l = &flow.lights[e->light];
				const float length = cvar_pt_flow_trail_length->value > 0.f ? cvar_pt_flow_trail_length->value : 200.f;
				e->light_generation = l->generation;
				l->follow = true;
				l->strength = cvar_pt_flow_bfg_trail_light->value;
				l->gather = (length + 32.f) * emit->scale;
				result |= R_FLOW_LIT;
			}
		}
		break;

	case R_FLOW_BEAM:
		// A plasma beam is one emitter for as long as it is given, by its
		// key, as a trail; flow_heatbeam builds it from where it is each
		// frame. No light slot: with pt_flow_heatbeam_light its light is a
		// rail's, along its gas (vkpt_flow_beam_lights). On the layer of whoever sees it: your
		// own in first person is given twice, down the barrel and for mirrors.
		if (emit->style == R_FLOW_HEATBEAM) {
			const int layer = emit->view == R_FLOW_VIEW_FIRST_PERSON ? FLOW_LAYER_HEATBEAM_VIEW
				: emit->view == R_FLOW_VIEW_MIRROR ? FLOW_LAYER_HEATBEAM_MIRROR : R_FLOW_HEATBEAM;
			// the mirror's copy off: a mirror shows the beam's model instead
			if (layer == FLOW_LAYER_HEATBEAM_MIRROR && !cvar_pt_flow_heatbeam_mirror->integer)
				return 0;
			flow_emitter_t *e = NULL;
			for (int i = 0; i < flow.num_emitters && !e; i++) {
				if (flow.emitters[i].layer == layer && flow.emitters[i].key == emit->key)
					e = &flow.emitters[i];
			}
			if (!flow_heatbeam_room(e ? 1.5f : 1.f)) {
				flow.refused_heatbeam++;
				return 0;
			}
			if (!e) {
				e = flow_new_emitter(R_FLOW_BEAM, layer, emit->origin, emit->scale);
				e->layer = layer;
				e->key = emit->key;
				e->light = -1;
			}
			VectorCopy(emit->origin, e->origin);
			VectorCopy(emit->end, e->end);
			e->scale = emit->scale;
			e->hit = emit->hit;
			e->seen = true;
			flow_heatbeam_stretch(e);
			if (emit->stretch) {
				emit->stretch[0] = e->from;
				emit->stretch[1] = e->to;
			}
			flow.idle = false;
			break;
		}
		// A railgun's trail is one emitter, made into its boxes every frame
		// it emits (flow_rail_boxes); it casts no light of its own - the
		// client's classic beam lights stay.
		if (emit->style == R_FLOW_RAIL) {
			if (!flow_rail_room()) {
				flow.refused_rail++;
				return 0;
			}
			flow_emitter_t *e = flow_new_emitter(R_FLOW_BEAM, R_FLOW_RAIL, emit->origin, emit->scale);
			VectorCopy(emit->end, e->end);
			flow_rail_stretch(e);
			flow.rail_age = 0.f;
			flow.rail_smoke = max(max(cvar_pt_flow_rail_smoke->value, cvar_pt_flow_rail_core_smoke->value), 0.f);
			flow.rail_heat = Cvar_ClampValue(cvar_pt_flow_rail_temperature, 0.f, 1.f);
			flow.rail_heat = max(flow.rail_heat, Cvar_ClampValue(cvar_pt_flow_rail_core_temperature, 0.f, 1.f));
			break;
		}
		{
			vec3_t delta;
			VectorSubtract(emit->end, emit->origin, delta);
			const float length = VectorLength(delta);
			const int segments = min(max((int)ceilf(length / FLOW_BEAM_SEGMENT), 1), 32);
			for (int s = 0; s < segments; s++) {
				vec3_t a, b;
				VectorMA(emit->origin, (float)s / segments, delta, a);
				VectorMA(emit->origin, (float)(s + 1) / segments, delta, b);
				flow_emitter_t *e = flow_new_emitter(R_FLOW_BEAM, emit->style, a, emit->scale);
				VectorCopy(b, e->end);
			}
		}
		break;

	case R_FLOW_BURN:
		// Something on fire is one emitter for as long as it is given, by its
		// key, as a trail; flow_burn_spheres builds it from where its box is
		// each frame. Its light, with pt_flow_heatbeam_light: flow_burn_light.
		{
			flow_emitter_t *e = NULL;
			for (int i = 0; i < flow.num_emitters && !e; i++) {
				if (flow.emitters[i].kind == R_FLOW_BURN && flow.emitters[i].key == emit->key)
					e = &flow.emitters[i];
			}
			if (!flow_burn_room(e ? 1.5f : 1.f))
				return 0;
			vec3_t centre, half;
			for (int k = 0; k < 3; k++) {
				centre[k] = 0.5f * (emit->origin[k] + emit->end[k]);
				half[k] = max(0.5f * fabsf(emit->end[k] - emit->origin[k]), 1.f);
			}
			if (!e) {
				e = flow_new_emitter(R_FLOW_BURN, R_FLOW_FIRE, centre, 1.f);
				e->key = emit->key;
				e->light = -1;
				VectorCopy(centre, e->last);
			}
			VectorCopy(centre, e->origin);
			// the plasma beam's fire on a wall, with no trails: where the beam
			// is, not along the way it was swept (flow_burn_spheres)
			if (emit->beam_fire && !cvar_pt_flow_heatbeam_trails->integer)
				VectorCopy(centre, e->last);
			VectorCopy(half, e->half);
			e->heat = emit->scale;
			e->core = emit->core;
			e->smoke_only = emit->smoke_only;
			e->seen = true;
			flow_burn_light(e);
			flow.idle = false;
		}
		break;

	case R_FLOW_BLAST:
		flow_emit_blast(emit);
		{
			// one light for all of it, gathered from as far as the jets go
			const int slot = flow_take_light(R_FLOW_PLASMA, emit->origin, emit->scale);
			if (slot >= 0) {
				flow_light_t *l = &flow.lights[slot];
				const float u = max(cvar_pt_flow_bfg_blast_size->value, 0.1f) * emit->scale;
				l->gather = max(l->gather, (max(cvar_pt_flow_bfg_jet_length->value, 0.f) + 24.f) * u);
				l->strength = cvar_pt_flow_bfg_blast_light->value;
				result |= R_FLOW_LIT;
			}
		}
		break;

	default:
		return 0;
	}

	return result;
}

#endif // USE_FLOW

/*
==============================================================================

DESCRIPTORS AND DUMMIES

==============================================================================
*/

static void flow_write_descriptors(int set, VkImageView density_view, VkImageLayout density_layout, VkBuffer table)
{
	VkDescriptorSet desc_set = set ? qvk.desc_set_textures_odd : qvk.desc_set_textures_even;

	VkDescriptorImageInfo density_info = {
		.sampler = qvk.tex_sampler_linear_clamp,
		.imageView = density_view,
		.imageLayout = density_layout,
	};
	VkDescriptorBufferInfo table_info = {
		.buffer = table,
		.offset = 0,
		.range = VK_WHOLE_SIZE,
	};
	VkDescriptorBufferInfo params_info = {
		.buffer = flow.params.buffer,
		.offset = set * flow.params_stride,
		.range = sizeof(FlowParams),
	};
	VkDescriptorBufferInfo lights_info = {
		.buffer = flow.light_sums.buffer,
		.offset = set * flow.light_sums_stride,
		.range = flow.light_sums_stride,
	};
	VkDescriptorBufferInfo occupancy_info = {
		.buffer = flow.occupancy.buffer,
		.offset = set * flow.occupancy_stride,
		.range = flow.occupancy_stride,
	};
	// wraps, like the probe grid
	VkDescriptorImageInfo ambient_info = {
		.sampler = qvk.tex_sampler,
		.imageView = flow.env_view ? flow.env_view : flow.dummy_view,
		.imageLayout = flow.env_view ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};

	VkWriteDescriptorSet writes[] = {
		{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = desc_set,
			.dstBinding = BINDING_OFFSET_FLOW_DENSITY,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &density_info,
		},
		{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = desc_set,
			.dstBinding = BINDING_OFFSET_FLOW_TABLE,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.pBufferInfo = &table_info,
		},
		{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = desc_set,
			.dstBinding = BINDING_OFFSET_FLOW_PARAMS,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.pBufferInfo = &params_info,
		},
		{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = desc_set,
			.dstBinding = BINDING_OFFSET_FLOW_LIGHTS,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.pBufferInfo = &lights_info,
		},
		{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = desc_set,
			.dstBinding = BINDING_OFFSET_FLOW_OCCUPANCY,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.pBufferInfo = &occupancy_info,
		},
		{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = desc_set,
			.dstBinding = BINDING_OFFSET_FLOW_AMBIENT,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &ambient_info,
		},
	};

	vkUpdateDescriptorSets(qvk.device, LENGTH(writes), writes, 0, NULL);
}

// How far apart each set's FlowParams, light sums and occupancy are, for the
// most blocks the grid can have.
static void flow_set_strides(void)
{
	// A uniform buffer (see flow_common.glsl). 256 is the most
	// minUniformBufferOffsetAlignment may be.
	flow.params_stride = align(sizeof(FlowParams), 256);

	// A block's sums for every block the grid can have, at the largest pool
	// pt_flow_max_blocks can ask for while the game runs (flow_pool_resize).
	const size_t max_blocks = FLOW_POOL_MAX;
	flow.light_sums_stride = align(max_blocks * FLOW_LIGHT_SUM_VEC4S * sizeof(vec4_t), 256);

	// A block's bricks for every block the grid can have, then the group
	// bitset and the block bitset. Written before it is read, every frame
	// there are blocks.
	flow.occupancy_blocks = (uint32_t)max_blocks;
	flow.occupancy_stride = align((max_blocks + FLOW_BITSET_UVEC4S) * 4 * sizeof(uint32_t), 256);
}

static VkResult flow_create_dummies(void)
{
	VkImageCreateInfo img_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_3D,
		.format = VK_FORMAT_R16G16B16A16_SFLOAT,
		.extent = { 1, 1, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	_VK(vkCreateImage(qvk.device, &img_info, NULL, &flow.dummy_image));
	ATTACH_LABEL_VARIABLE(flow.dummy_image, IMAGE);

	VkMemoryRequirements mem_req;
	vkGetImageMemoryRequirements(qvk.device, flow.dummy_image, &mem_req);

	VkMemoryAllocateInfo mem_alloc_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mem_req.size,
		.memoryTypeIndex = get_memory_type(mem_req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
	};
	_VK(vkAllocateMemory(qvk.device, &mem_alloc_info, NULL, &flow.dummy_memory));
	_VK(vkBindImageMemory(qvk.device, flow.dummy_image, flow.dummy_memory, 0));

	const VkImageSubresourceRange subresource_range = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.baseMipLevel = 0,
		.levelCount = 1,
		.baseArrayLayer = 0,
		.layerCount = 1,
	};

	VkImageViewCreateInfo view_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = flow.dummy_image,
		.viewType = VK_IMAGE_VIEW_TYPE_3D,
		.format = img_info.format,
		.subresourceRange = subresource_range,
	};
	_VK(vkCreateImageView(qvk.device, &view_info, NULL, &flow.dummy_view));
	ATTACH_LABEL_VARIABLE(flow.dummy_view, IMAGE_VIEW);

	VkCommandBuffer cmd_buf = vkpt_begin_command_buffer(&qvk.cmd_buffers_graphics);

	IMAGE_BARRIER(cmd_buf,
		.image = flow.dummy_image,
		.subresourceRange = subresource_range,
		.srcAccessMask = 0,
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
	);

	VkClearColorValue clear = { .float32 = { 0, 0, 0, 0 } };
	vkCmdClearColorImage(cmd_buf, flow.dummy_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &subresource_range);

	IMAGE_BARRIER(cmd_buf,
		.image = flow.dummy_image,
		.subresourceRange = subresource_range,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
	);

	vkpt_submit_command_buffer_simple(cmd_buf, qvk.queue_graphics, true);
	vkpt_wait_idle(qvk.queue_graphics, &qvk.cmd_buffers_graphics);

	// Never read: FlowParams.block_dim_bits.w is 0 whenever this is bound.
	_VK(buffer_create(&flow.dummy_table, 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));

	flow_set_strides();
	_VK(buffer_create(&flow.params, flow.params_stride * FLOW_SET_COUNT, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
	flow.params_mapped = buffer_map(&flow.params);
	memset(flow.params_mapped, 0, flow.params_stride * FLOW_SET_COUNT);

	_VK(buffer_create(&flow.light_sums, flow.light_sums_stride * FLOW_SET_COUNT, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
	flow.light_sums_mapped = buffer_map(&flow.light_sums);
	memset(flow.light_sums_mapped, 0, flow.light_sums_stride * FLOW_SET_COUNT);

	_VK(buffer_create(&flow.occupancy, flow.occupancy_stride * FLOW_SET_COUNT,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));

	return VK_SUCCESS;
}

static void flow_destroy_dummies(void)
{
	buffer_destroy(&flow.occupancy);

	if (flow.light_sums_mapped)
		buffer_unmap(&flow.light_sums);
	flow.light_sums_mapped = NULL;
	buffer_destroy(&flow.light_sums);

	if (flow.params_mapped)
		buffer_unmap(&flow.params);
	flow.params_mapped = NULL;
	buffer_destroy(&flow.params);
	buffer_destroy(&flow.dummy_table);

	if (flow.dummy_view)
		vkDestroyImageView(qvk.device, flow.dummy_view, NULL);
	if (flow.dummy_image)
		vkDestroyImage(qvk.device, flow.dummy_image, NULL);
	if (flow.dummy_memory)
		vkFreeMemory(qvk.device, flow.dummy_memory, NULL);
	flow.dummy_view = VK_NULL_HANDLE;
	flow.dummy_image = VK_NULL_HANDLE;
	flow.dummy_memory = VK_NULL_HANDLE;
}

#if USE_FLOW
// One of the probe grid's images, cleared, in the general layout for good.
static VkResult flow_env_create_image(VkFormat format, VkImage *image, VkDeviceMemory *memory, VkImageView *view)
{
	VkImageCreateInfo img_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_3D,
		.format = format,
		.extent = { FLOW_ENV_DIM_X, FLOW_ENV_DIM_Y, FLOW_ENV_DIM_Z },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	_VK(vkCreateImage(qvk.device, &img_info, NULL, image));
	ATTACH_LABEL_VARIABLE(*image, IMAGE);

	VkMemoryRequirements mem_req;
	vkGetImageMemoryRequirements(qvk.device, *image, &mem_req);

	VkMemoryAllocateInfo mem_alloc_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mem_req.size,
		.memoryTypeIndex = get_memory_type(mem_req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
	};
	_VK(vkAllocateMemory(qvk.device, &mem_alloc_info, NULL, memory));
	_VK(vkBindImageMemory(qvk.device, *image, *memory, 0));

	const VkImageSubresourceRange subresource_range = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.baseMipLevel = 0,
		.levelCount = 1,
		.baseArrayLayer = 0,
		.layerCount = 1,
	};

	VkImageViewCreateInfo view_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = *image,
		.viewType = VK_IMAGE_VIEW_TYPE_3D,
		.format = format,
		.subresourceRange = subresource_range,
	};
	_VK(vkCreateImageView(qvk.device, &view_info, NULL, view));
	ATTACH_LABEL_VARIABLE(*view, IMAGE_VIEW);

	VkCommandBuffer cmd_buf = vkpt_begin_command_buffer(&qvk.cmd_buffers_graphics);

	IMAGE_BARRIER(cmd_buf,
		.image = *image,
		.subresourceRange = subresource_range,
		.srcAccessMask = 0,
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL
	);

	VkClearColorValue clear = { .uint32 = { 0, 0, 0, 0 } };
	vkCmdClearColorImage(cmd_buf, *image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &subresource_range);

	IMAGE_BARRIER(cmd_buf,
		.image = *image,
		.subresourceRange = subresource_range,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL
	);

	vkpt_submit_command_buffer_simple(cmd_buf, qvk.queue_graphics, true);
	vkpt_wait_idle(qvk.queue_graphics, &qvk.cmd_buffers_graphics);

	return VK_SUCCESS;
}

static bool flow_env_format_ok(VkFormat format, VkFormatFeatureFlags features)
{
	VkFormatProperties props;
	vkGetPhysicalDeviceFormatProperties(qvk.physical_device, format, &props);
	return (props.optimalTilingFeatures & features) == features;
}

// The probe grid and its pass's set and layout; the pipeline comes with the
// others. Ray query only: the pass traces from a compute shader.
static void flow_env_create(void)
{
	if (!qvk.use_ray_query)
		return;

	if (!flow_env_format_ok(VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ||
		!flow_env_format_ok(VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
		Com_WPrintf("NVIDIA Flow: no filtered rgba32f storage images, so the smoke takes no light from the world\n");
		return;
	}

	if (flow_env_create_image(VK_FORMAT_R32G32B32A32_SFLOAT, &flow.env_image, &flow.env_memory, &flow.env_view) != VK_SUCCESS ||
		flow_env_create_image(VK_FORMAT_R32G32B32A32_UINT, &flow.env_key_image, &flow.env_key_memory, &flow.env_key_view) != VK_SUCCESS)
		return;

	VkDescriptorSetLayoutBinding bindings[3] = {
		{ .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
		{ .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
		{ .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
	};
	VkDescriptorSetLayoutCreateInfo layout_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = LENGTH(bindings),
		.pBindings = bindings,
	};
	_VK(vkCreateDescriptorSetLayout(qvk.device, &layout_info, NULL, &flow.env_set_layout));
	ATTACH_LABEL_VARIABLE(flow.env_set_layout, DESCRIPTOR_SET_LAYOUT);

	const VkDescriptorPoolSize pool_sizes[] = {
		{ VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, MAX_FRAMES_IN_FLIGHT },
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * MAX_FRAMES_IN_FLIGHT },
	};
	VkDescriptorPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = MAX_FRAMES_IN_FLIGHT,
		.poolSizeCount = LENGTH(pool_sizes),
		.pPoolSizes = pool_sizes,
	};
	_VK(vkCreateDescriptorPool(qvk.device, &pool_info, NULL, &flow.env_pool));
	ATTACH_LABEL_VARIABLE(flow.env_pool, DESCRIPTOR_POOL);

	VkDescriptorSetLayout set_layouts[MAX_FRAMES_IN_FLIGHT];
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
		set_layouts[i] = flow.env_set_layout;
	VkDescriptorSetAllocateInfo alloc_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = flow.env_pool,
		.descriptorSetCount = MAX_FRAMES_IN_FLIGHT,
		.pSetLayouts = set_layouts,
	};
	_VK(vkAllocateDescriptorSets(qvk.device, &alloc_info, flow.env_sets));

	// The images for good; the TLAS every frame, see vkpt_flow_ambient.
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		VkDescriptorImageInfo ambient_info = { .imageView = flow.env_view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
		VkDescriptorImageInfo key_info = { .imageView = flow.env_key_view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
		VkWriteDescriptorSet writes[] = {
			{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = flow.env_sets[i],
				.dstBinding = 1,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
				.pImageInfo = &ambient_info,
			},
			{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = flow.env_sets[i],
				.dstBinding = 2,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
				.pImageInfo = &key_info,
			},
		};
		vkUpdateDescriptorSets(qvk.device, LENGTH(writes), writes, 0, NULL);
	}

	VkDescriptorSetLayout desc_set_layouts[] = {
		flow.env_set_layout, qvk.desc_set_layout_vertex_buffer, qvk.desc_set_layout_ubo, qvk.desc_set_layout_textures,
	};
	CREATE_PIPELINE_LAYOUT(qvk.device, &flow.env_pipeline_layout,
		.setLayoutCount = LENGTH(desc_set_layouts),
		.pSetLayouts = desc_set_layouts,
	);
	ATTACH_LABEL_VARIABLE(flow.env_pipeline_layout, PIPELINE_LAYOUT);
}

static void flow_env_destroy(void)
{
	if (flow.env_pipeline_layout)
		vkDestroyPipelineLayout(qvk.device, flow.env_pipeline_layout, NULL);
	if (flow.env_pool)
		vkDestroyDescriptorPool(qvk.device, flow.env_pool, NULL);
	if (flow.env_set_layout)
		vkDestroyDescriptorSetLayout(qvk.device, flow.env_set_layout, NULL);
	if (flow.env_view)
		vkDestroyImageView(qvk.device, flow.env_view, NULL);
	if (flow.env_image)
		vkDestroyImage(qvk.device, flow.env_image, NULL);
	if (flow.env_memory)
		vkFreeMemory(qvk.device, flow.env_memory, NULL);
	if (flow.env_key_view)
		vkDestroyImageView(qvk.device, flow.env_key_view, NULL);
	if (flow.env_key_image)
		vkDestroyImage(qvk.device, flow.env_key_image, NULL);
	if (flow.env_key_memory)
		vkFreeMemory(qvk.device, flow.env_key_memory, NULL);
	flow.env_pipeline_layout = VK_NULL_HANDLE;
	flow.env_pool = VK_NULL_HANDLE;
	flow.env_set_layout = VK_NULL_HANDLE;
	flow.env_view = VK_NULL_HANDLE;
	flow.env_image = VK_NULL_HANDLE;
	flow.env_memory = VK_NULL_HANDLE;
	flow.env_key_view = VK_NULL_HANDLE;
	flow.env_key_image = VK_NULL_HANDLE;
	flow.env_key_memory = VK_NULL_HANDLE;
	memset(flow.env_sets, 0, sizeof(flow.env_sets));
}

/*
Where this frame's probes go (pt_flow_env), into params: a grid fixed in the
world, pt_flow_env_spacing apart, over the box round the gas and a probe past
it each way, so that every point in the box has its eight. Twice, four or
eight times as far apart when the box needs more probes than the texture
holds each way or than pt_flow_env_max_probes; past that the smoke keeps its
flat colour. The level is in a probe's key, so a new spacing starts every
history over. The lights are the BSP cluster's at the middle of the box, or
where the gas began when that is in a wall.
*/
// the probe grid is there to fill: the Vulkan pass's, or the backend's
static bool flow_env_ready(void)
{
	return vkpt_flow_backend ? flow.env_backend : flow.env_pipeline != VK_NULL_HANDLE;
}

static void flow_env_params(FlowParams *params)
{
	const bsp_t *bsp = flow.env_bsp;
	const int first_light = flow.env_first_light;
	const int num_lights = flow.env_num_lights;
	flow.env_bsp = NULL;
	flow.env_num_lights = 0;

	if (!flow_env_ready() || !cvar_pt_flow_env->integer || !params->block_dim_bits[3])
		return;

	static const int dim[3] = { FLOW_ENV_DIM_X, FLOW_ENV_DIM_Y, FLOW_ENV_DIM_Z };
	const int max_probes = Cvar_ClampInteger(cvar_pt_flow_env_max_probes, 64, FLOW_ENV_DIM_X * FLOW_ENV_DIM_Y * FLOW_ENV_DIM_Z);
	float spacing = max(cvar_pt_flow_env_spacing->value, 8.f);
	int first[3], count[3];
	int level = 0;
	for (;; level++, spacing *= 2.f) {
		bool fits = true;
		int total = 1;
		for (int k = 0; k < 3; k++) {
			first[k] = (int)floorf(params->world_min[k] / spacing);
			count[k] = (int)floorf(params->world_max[k] / spacing) + 2 - first[k];
			fits = fits && count[k] >= 1 && count[k] <= dim[k];
			total *= fits ? count[k] : 1;
		}
		if (fits && total <= max_probes)
			break;
		if (level == 3)
			return;
	}

	int cluster = -1;
	if (bsp && bsp->nodes) {
		vec3_t middle;
		VectorAdd(params->world_min, params->world_max, middle);
		VectorScale(middle, 0.5f, middle);
		cluster = BSP_PointLeaf(bsp->nodes, middle)->cluster;
		for (int i = 0; cluster < 0 && i < flow.num_emitters; i++)
			cluster = BSP_PointLeaf(bsp->nodes, flow.emitters[i].origin)->cluster;
	}

	for (int k = 0; k < 3; k++) {
		params->env_first[k] = first[k];
		params->env_count[k] = count[k];
		flow.env_groups[k] = (uint32_t)(count[k] + 3) / 4;
	}
	params->env_first[3] = cluster;
	params->env_count[3] = (int)(uint32_t)qvk.frame_counter;
	params->env_params[0] = spacing;
	params->env_params[1] = max(cvar_pt_flow_env_history->value, 1.f);
	params->env_params[2] = max(cvar_pt_flow_env_light->value, 0.f);
	params->env_params[3] = max(cvar_pt_flow_env_clamp->value, 0.f);
	params->env_lights[0] = first_light;
	params->env_lights[1] = num_lights;
	params->env_lights[2] = level;
}
#endif

/*
==============================================================================

ENTRY POINTS

==============================================================================
*/

VkResult vkpt_flow_initialize(void)
{
	cvar_flow_available = Cvar_Get("flow_available", "0", CVAR_ROM);

	// Whether to start Flow at all, read when the renderer starts. Starting it
	// builds all of Flow's compute pipelines on the spot, in the driver's
	// shader compiler - which is where a startup hang once came from (see
	// EmitterNanoVdb in setup/build_flow.py) - so +set pt_flow 0 is the way
	// round one without moving the DLLs.
	cvar_pt_flow = Cvar_Get("pt_flow", "1", 0);

	// The menu's two: fire and plasma light the world and the smoke, and the
	// world holds the gas in.
	cvar_pt_flow_lighting = Cvar_Get("pt_flow_lighting", "1", CVAR_ARCHIVE);
	cvar_pt_flow_collision = Cvar_Get("pt_flow_collision", "1", CVAR_ARCHIVE);

	// The grid. The block pool is the menu's slider, FLOW_POOL_MIN to
	// FLOW_POOL_MAX, and a change takes at Flow's next step (flow_pool_resize).
	// Flow's own default is 4096; 512 sent the rocket and rail trails to
	// particles in a big fight (Matt, 11-07), as their budgets are shares of
	// it. The whole pool's VRAM, used or not, is taken at Flow's first step -
	// measured on D3D12, headless (11-07): 214 MB at 512, 455 at 1024, 916 at
	// 2048, 1600 at 4096, some 0.45 MB a block. 3584 is Matt's own setting,
	// made the default (11-10). The cell sizes - this one and the railgun's
	// and plasma beam's below - are menu sliders since 11-10, so archived.
	cvar_pt_flow_cell_size = Cvar_Get("pt_flow_cell_size", "3", CVAR_ARCHIVE);
	cvar_pt_flow_max_blocks = Cvar_Get("pt_flow_max_blocks", "3584", CVAR_ARCHIVE);
	// That VRAM, and the rest of what Flow makes the first time it steps with
	// blocks, made while a map loads, not on the first explosion's frame
	// (flow_warm_emitters). 0 = made at the first explosion, as before.
	cvar_pt_flow_prewarm = Cvar_Get("pt_flow_prewarm", "1", 0);
	cvar_pt_flow_substeps = Cvar_Get("pt_flow_substeps", "1", 0);

	// An explosion: its size at scale 1 (a rocket), how long it emits, how
	// fast it pushes out, and what it fills itself with.
	cvar_pt_flow_radius = Cvar_Get("pt_flow_radius", "24", 0);
	cvar_pt_flow_burst = Cvar_Get("pt_flow_burst", "0.12", 0);
	cvar_pt_flow_expansion = Cvar_Get("pt_flow_expansion", "350", 0);
	cvar_pt_flow_temperature = Cvar_Get("pt_flow_temperature", "1", 0);
	cvar_pt_flow_fuel = Cvar_Get("pt_flow_fuel", "1", 0);
	cvar_pt_flow_smoke = Cvar_Get("pt_flow_smoke", "0.3", 0);
	cvar_pt_flow_couple_rate = Cvar_Get("pt_flow_couple_rate", "30", 0);

	// Flow's combustion, at its own defaults but for the smoke, which goes
	// sooner, and the heat, which stays a little longer (Flow's cooling 1.5:
	// the trailer's fireballs stay orange for over a second). Smoke loses
	// the larger of pt_flow_smoke_damping of itself (Flow's 0.3) and
	// pt_flow_smoke_fade (0.65) every second; burning makes
	// pt_flow_smoke_per_burn (Flow's 3) of it.
	cvar_pt_flow_buoyancy = Cvar_Get("pt_flow_buoyancy", "2", 0);
	cvar_pt_flow_cooling = Cvar_Get("pt_flow_cooling", "1.3", 0);
	cvar_pt_flow_smoke_fade = Cvar_Get("pt_flow_smoke_fade", "0.65", 0);
	cvar_pt_flow_smoke_damping = Cvar_Get("pt_flow_smoke_damping", "0.5", 0);
	cvar_pt_flow_smoke_per_burn = Cvar_Get("pt_flow_smoke_per_burn", "3", 0);
	cvar_pt_flow_vorticity = Cvar_Get("pt_flow_vorticity", "0.6", 0);

	// The look. Colours are relative to the adapted luminance, like the
	// explosion sprites' (pt_explosion_brightness 4 x 500). Fire glows by
	// its colour map times pt_flow_fire_brightness for every 32 units of it
	// looked through, however much smoke there is, and pt_flow_fire_orange
	// more orange than Flow's own map (0 to 1: up to 40% less green and 80%
	// less blue, in the glow and its light) - none of it where the gas has
	// cooled below pt_flow_fire_glow_start, 0 to 0.5 (Flow's own map: 0.15;
	// with the heat going at pt_flow_cooling a second, 0.3 ends the glow and
	// its light some 0.5 s sooner, 0.4 some 0.75 s); the smoke darkens what
	// is behind it by pt_flow_density per unit of smoke per cell, shows
	// pt_flow_smoke_albedo x pt_flow_smoke_brightness of its own, and
	// scatters pt_flow_smoke_scatter of the fire's light. The fire was 40
	// until Matt set 5 for HDR, which looked better (11-09); a rocket's trail
	// keeps the 40 (pt_flow_rocket_brightness).
	cvar_pt_flow_fire_brightness = Cvar_Get("pt_flow_fire_brightness", "5", 0);
	cvar_pt_flow_fire_orange = Cvar_Get("pt_flow_fire_orange", "0.3", 0);
	cvar_pt_flow_fire_glow_start = Cvar_Get("pt_flow_fire_glow_start", "0.3", 0);
	cvar_pt_flow_smoke_brightness = Cvar_Get("pt_flow_smoke_brightness", "20", 0);
	cvar_pt_flow_smoke_albedo = Cvar_Get("pt_flow_smoke_albedo", "0.25", 0);
	cvar_pt_flow_smoke_scatter = Cvar_Get("pt_flow_smoke_scatter", "0.8", 0);
	cvar_pt_flow_density = Cvar_Get("pt_flow_density", "0.01", 0);
	// march step, in cells, for every layer: 1 (from 0.75, Matt 11-12) - some
	// 0.3 ms less, and the plasma beam's ring lines were in the gas, not the march
	cvar_pt_flow_step = Cvar_Get("pt_flow_step", "1", 0);
	cvar_pt_flow_max_steps = Cvar_Get("pt_flow_max_steps", "256", 0);  // samples, past empty bricks
	// How the march gets through the volume, not what it reads: 1, the
	// world's light read once a step for every layer with smoke there; 2, two
	// steps of a brick read at a time. Same samples, same sums - 0 is the old
	// one-read-after-another march, kept for A/B (11-03). 4, a block with
	// nothing to see passed on a bit of the block bitset, not a search of
	// Flow's table; 8, each grid looked at only inside the box round its own
	// blocks - both for a railgun shot, whose box holds the one who fired it,
	// so that every ray on the screen walked it (11-09). Neither changes the
	// look: the steps sampled are the ones in marked bricks either way (fewer
	// stops on the way only ever land one nearer the start of a brick), less
	// what an unmarked brick of one grid would have added to a step another
	// grid samples, which flow_occupancy.comp already calls nothing.
	// 16, primary rays whose screen tile has none of the volume in front are
	// not marched (flow.glsl flow_screen_clear, 11-12). 15 = without.
	cvar_pt_flow_march = Cvar_Get("pt_flow_march", "31", 0);
	// 1: print what the primary march did, once a second (FLOWSTATS, see
	// vkpt_flow_stats) - needs FLOW_STATS compiled into flow.glsl.
	cvar_pt_flow_stats = Cvar_Get("pt_flow_stats", "0", 0);
	// FLOWPROF lines once a second: Flow's passes, timed on the GPU, and its CPU (flow_prof_update)
	cvar_pt_flow_profile = Cvar_Get("pt_flow_profile", "0", 0);
	// collision only on the layers with gas (flow_build_params); 0 = on every layer, as before
	cvar_pt_flow_collision_layers = Cvar_Get("pt_flow_collision_layers", "1", 0);
	// 1: print how full the block pool is, once a second (FLOWPOOL, see
	// flow_pool_stats), and which trails went to particles for want of room.
	cvar_pt_flow_pool_stats = Cvar_Get("pt_flow_pool_stats", "0", 0);
	// The least share of the fire's glow smoke in front of it lets through
	// (flow_step_add in flow.glsl): a rocket's own trail, seen down its length
	// from where it was fired, hid the middle of the explosion at its end
	// (11-03). Only where the smoke is thicker than that; 0 = the old march.
	cvar_pt_flow_glow_floor = Cvar_Get("pt_flow_glow_floor", "0.2", 0);

	// What it costs (see the top of this file): Flow's steps a second at
	// most, 0 for one every frame; how much coarser a reflection marches;
	// how much of fire's leftover fuel goes each second.
	cvar_pt_flow_rate = Cvar_Get("pt_flow_rate", "60", 0);
	cvar_pt_flow_reflect_step = Cvar_Get("pt_flow_reflect_step", "2", 0);
	cvar_pt_flow_fuel_fade = Cvar_Get("pt_flow_fuel_fade", "0.25", 0);

	// The BFG's plasma (R_FLOW_PLASMA): green heat and dark green smoke.
	// A burst of it is a target the BFG zaps: its radius, push and how long
	// it emits, like one of the blast's lumps. Brightness as
	// pt_flow_fire_brightness; the smoke as the fire's, its own colour green;
	// vorticity the plasma's own, strong enough to tear its blast apart (0.6,
	// the fire's, leaves the ball's trail smoother).
	cvar_pt_flow_bfg_radius = Cvar_Get("pt_flow_bfg_radius", "16", 0);
	cvar_pt_flow_bfg_expansion = Cvar_Get("pt_flow_bfg_expansion", "40", 0);
	cvar_pt_flow_bfg_burst = Cvar_Get("pt_flow_bfg_burst", "0.05", 0);
	cvar_pt_flow_bfg_cooling = Cvar_Get("pt_flow_bfg_cooling", "3", 0);
	cvar_pt_flow_bfg_buoyancy = Cvar_Get("pt_flow_bfg_buoyancy", "0.5", 0);
	cvar_pt_flow_bfg_brightness = Cvar_Get("pt_flow_bfg_brightness", "40", 0);
	cvar_pt_flow_bfg_smoke = Cvar_Get("pt_flow_bfg_smoke", "1", 0);
	cvar_pt_flow_bfg_smoke_fade = Cvar_Get("pt_flow_bfg_smoke_fade", "0.3", 0);
	cvar_pt_flow_bfg_smoke_damping = Cvar_Get("pt_flow_bfg_smoke_damping", "0.3", 0);
	cvar_pt_flow_bfg_smoke_brightness = Cvar_Get("pt_flow_bfg_smoke_brightness", "2", 0);
	cvar_pt_flow_bfg_smoke_density = Cvar_Get("pt_flow_bfg_smoke_density", "0.02", 0);
	cvar_pt_flow_bfg_vorticity = Cvar_Get("pt_flow_bfg_vorticity", "6", 0);

	// The BFG's own blast (R_FLOW_BLAST, flow_emit_blast): the flower's puffs,
	// the lumps round it, the jets and how far they go, the size of all of
	// it, how hard its pieces push out, and its light, which goes in place of
	// the classic one - about 1.6 times its peak, measured headless, and gone
	// after half a second.
	cvar_pt_flow_bfg_puffs = Cvar_Get("pt_flow_bfg_puffs", "8", 0);
	cvar_pt_flow_bfg_lumps = Cvar_Get("pt_flow_bfg_lumps", "24", 0);
	cvar_pt_flow_bfg_jets = Cvar_Get("pt_flow_bfg_jets", "3", 0);
	cvar_pt_flow_bfg_jet_length = Cvar_Get("pt_flow_bfg_jet_length", "200", 0);
	cvar_pt_flow_bfg_blast_size = Cvar_Get("pt_flow_bfg_blast_size", "1", 0);
	cvar_pt_flow_bfg_blast_expansion = Cvar_Get("pt_flow_bfg_blast_expansion", "60", 0);
	cvar_pt_flow_bfg_blast_light = Cvar_Get("pt_flow_bfg_blast_light", "3.75", 0);
	// The light of the glow behind the BFG ball in flight (pt_flow_lighting),
	// on top of the ball's own classic light. The glow of a trail is thin - a
	// hundredth of a rocket's fireball - so it takes this much to come out at
	// about the classic ball light's brightness again.
	cvar_pt_flow_bfg_trail_light = Cvar_Get("pt_flow_bfg_trail_light", "12", 0);

	// A trail (the BFG ball in flight) and a beam (its lasers): radius - a
	// beam's half thickness - push and smoke at scale 1; a beam emits for
	// pt_flow_beam_burst seconds, about the laser's own life.
	cvar_pt_flow_trail_radius = Cvar_Get("pt_flow_trail_radius", "10", 0);
	cvar_pt_flow_trail_expansion = Cvar_Get("pt_flow_trail_expansion", "60", 0);
	cvar_pt_flow_trail_smoke = Cvar_Get("pt_flow_trail_smoke", "0.5", 0);
	// How far behind its source a trail stops glowing, at scale 1: there a
	// sphere takes the heat out of the gas (and only the heat - the smoke
	// stays). On its own the plasma cools for some 0.6 s, and a BFG ball at
	// 400 units/s drew a glowing tube 220 units long - from the player's eye
	// out to the ball, the whole view down it, at the moment of firing. 96
	// glows for some 90 units, measured headless. 0 = no limit.
	cvar_pt_flow_trail_length = Cvar_Get("pt_flow_trail_length", "96", 0);
	cvar_pt_flow_beam_radius = Cvar_Get("pt_flow_beam_radius", "4", 0);
	cvar_pt_flow_beam_expansion = Cvar_Get("pt_flow_beam_expansion", "100", 0);
	cvar_pt_flow_beam_burst = Cvar_Get("pt_flow_beam_burst", "0.1", 0);

	// The railgun's trail (R_FLOW_RAIL, flow_rail_boxes). The spiral: how far
	// round the line (the classic's is 3, too thin for the grid), how thick,
	// units a turn (the classic's 0.1 radians a unit), its heat and smoke, how
	// hard it swells, and how fast it turns round the line, which the
	// vorticity tears into wisps. The line: its thickness, heat, smoke and
	// swell. All of it emits for pt_flow_rail_burst seconds; the heat goes at
	// pt_flow_rail_cooling a second (2.5: the glow is gone in about 1.2 s),
	// the smoke as the fire's does, from pt_flow_rail_smoke_damping and
	// _fade. Looks: glow per 32 units looked through, smoke colour, and
	// extinction per unit of smoke per cell - high, as the trail is a cell or
	// so thick. Only pt_flow_rail_length units of a longer shot are drawn,
	// the stretch nearest the view, and only while the gas fills less than
	// pt_flow_rail_budget of the blocks. Its gas is on a grid of its own,
	// pt_flow_rail_cell_size units a cell: on the main grid's 3 the spiral
	// came out boxy; 1 looked good and 0.5 "how it should look" (Matt,
	// 10-27). A block is then 16x8x8 units, and a long shot can want more
	// blocks than there are - hence the budget of a half, not three
	// quarters, and pt_flow_rail_life.
	cvar_pt_flow_rail_cell_size = Cvar_Get("pt_flow_rail_cell_size", "0.5", CVAR_ARCHIVE);
	// Its level of detail (Matt, 11-08: "a tight grid when it's close to the
	// camera ... more loose grid" further off): the part of a shot further
	// than pt_flow_rail_lod_distance from the view when it came in - at
	// Quake's 90 degree field of view, further zoomed in - is on cells of
	// pt_flow_rail_far_cell_size (FLOW_LAYER_RAIL_FAR). At 1.5 a block is
	// 48x24x24 units, so the far part takes a fifth or less of the blocks a
	// unit of length the fine one does - worked out, not measured.
	// At 256 a far cell looks no bigger than a fine one 85 units off. 0 puts
	// the whole shot on the fine grid, as before. Its glow is
	// pt_flow_rail_far_brightness times the fine part's. Read when a shot
	// comes in.
	cvar_pt_flow_rail_lod_distance = Cvar_Get("pt_flow_rail_lod_distance", "256", 0);
	cvar_pt_flow_rail_far_cell_size = Cvar_Get("pt_flow_rail_far_cell_size", "1.5", CVAR_ARCHIVE);
	cvar_pt_flow_rail_far_brightness = Cvar_Get("pt_flow_rail_far_brightness", "1", 0);
	cvar_pt_flow_rail_radius = Cvar_Get("pt_flow_rail_radius", "6", 0);
	cvar_pt_flow_rail_width = Cvar_Get("pt_flow_rail_width", "1.25", 0);     // 2 until Matt found it too big (10-27)
	cvar_pt_flow_rail_turn = Cvar_Get("pt_flow_rail_turn", "63", 0);
	cvar_pt_flow_rail_temperature = Cvar_Get("pt_flow_rail_temperature", "0.85", 0);
	cvar_pt_flow_rail_smoke = Cvar_Get("pt_flow_rail_smoke", "0.5", 0);
	cvar_pt_flow_rail_expansion = Cvar_Get("pt_flow_rail_expansion", "4", 0);    // 10 until then
	cvar_pt_flow_rail_swirl = Cvar_Get("pt_flow_rail_swirl", "15", 0);
	cvar_pt_flow_rail_core_radius = Cvar_Get("pt_flow_rail_core_radius", "1.5", 0);
	cvar_pt_flow_rail_core_temperature = Cvar_Get("pt_flow_rail_core_temperature", "0.3", 0);
	cvar_pt_flow_rail_core_smoke = Cvar_Get("pt_flow_rail_core_smoke", "1", 0);
	cvar_pt_flow_rail_core_expansion = Cvar_Get("pt_flow_rail_core_expansion", "30", 0);
	cvar_pt_flow_rail_burst = Cvar_Get("pt_flow_rail_burst", "0.05", 0);
	cvar_pt_flow_rail_cooling = Cvar_Get("pt_flow_rail_cooling", "2.5", 0);
	cvar_pt_flow_rail_buoyancy = Cvar_Get("pt_flow_rail_buoyancy", "0.5", 0);
	cvar_pt_flow_rail_vorticity = Cvar_Get("pt_flow_rail_vorticity", "2", 0);
	cvar_pt_flow_rail_smoke_damping = Cvar_Get("pt_flow_rail_smoke_damping", "0.8", 0);
	cvar_pt_flow_rail_smoke_fade = Cvar_Get("pt_flow_rail_smoke_fade", "0.4", 0);
	// Seconds after the last shot that nothing is left, and over how many
	// before that what is left fades out (flow_rail_fade). Left alone the
	// trail lasts 1.5 s; 1 and a fade too fast cut it off at 1.05 (Matt: "a
	// quarter of a second to half a second longer", 10-27). 1.4 leaves the
	// blocks a tenth of a second to go before the railgun's 1.5 s refire.
	cvar_pt_flow_rail_life = Cvar_Get("pt_flow_rail_life", "1.4", 0);
	cvar_pt_flow_rail_drain = Cvar_Get("pt_flow_rail_drain", "0.4", 0);
	// The menu's "railgun trail fade time": seconds the trail takes to fade
	// away, from pt_flow_rail_life (the fade as tuned, also the shortest) up.
	// Longer stretches all of it alike (flow_rail_slow): its glow and smoke
	// stay in the air longer, as they did before the fade was cut short for
	// what it cost - and hold the simulation's blocks that much longer, so a
	// second shot soon after can find the pool full and go to particles.
	// Read every frame.
	cvar_pt_flow_rail_fade = Cvar_Get("pt_flow_rail_fade", "1.4", CVAR_ARCHIVE);
	cvar_pt_flow_rail_brightness = Cvar_Get("pt_flow_rail_brightness", "40", 0);
	cvar_pt_flow_rail_smoke_brightness = Cvar_Get("pt_flow_rail_smoke_brightness", "5", 0);
	cvar_pt_flow_rail_density = Cvar_Get("pt_flow_rail_density", "0.15", 0);
	cvar_pt_flow_rail_length = Cvar_Get("pt_flow_rail_length", "2048", 0);
	cvar_pt_flow_rail_budget = Cvar_Get("pt_flow_rail_budget", "0.5", 0);

	// The plasma beam (R_FLOW_HEATBEAM, flow_heatbeam), after the classic
	// beam model and the rings of yellow particles 1.5 units round the
	// first-person beam (3.9 round the thicker third-person one,
	// cl_beam_mirror_ring_scale), 32 units apart, running out at 96 units/s.
	// Rings: how far round, half how thick, how far apart, how fast they run
	// out, their heat where they show (cooling as they run, below), smoke, swell and how
	// bright their glow is for that heat. The core, in place of the model:
	// how bright for its heat; how much it is dimmed where seen end-on (1 as
	// much as a solid beam would be, 0 not at all - a line of gas seen from
	// down the barrel is looked through for far further than its width);
	// none within pt_flow_heatbeam_core_clear of the view, and nothing else
	// there either; its glow pt_flow_heatbeam_near_brightness as bright where
	// it starts and all of it pt_flow_heatbeam_near_fade units further on.
	// Your own first-person beam's gas starts in the barrel, 17 units from the
	// eye (cl_flow_heatbeam_muzzle), and the gun hides it out to some 24: 16
	// and 8 are on full where it comes out from behind the gun. From the
	// model's start, 8 units from the eye, 4 put fire on top of the gun and
	// 24 / 32 left it floating mid-screen (Matt, 11-09 / 11-10); 32 with the
	// model there left the model most of what the one firing sees, and Matt
	// wants none of the old beam with Flow. Then half how thick, its heat
	// (0.88, the model skin's yellow-orange), smoke, swell, and how fast its
	// gas is pushed out along the beam. Where the beam hits: how big the hot
	// spot is, its heat, smoke and swell. The gas: cooling (4: the rings' glow
	// is gone in about a quarter of a second, so a beam swept round the room
	// leaves only a short wake), rise, curl and smoke fade. Looks: glow per
	// 32 units looked through on the fire's colour map, pt_flow_heatbeam_yellow
	// (0 to 1) toward the classic rings' yellow - 0 is the fire's own
	// orange-white - and the smoke's colour and extinction per unit of smoke
	// per cell. The core is only pt_flow_heatbeam_length units of the beam,
	// the stretch nearest the view, the model being the rest; the rings are
	// not within pt_flow_heatbeam_clear of the view (80: from 48, the first
	// rings, seen end-on, were a ball of fire round the crosshair - Matt,
	// 11-10: "so much fire taking up the player's field of view"), the hot
	// spot not within FLOW_HEATBEAM_POINT_BLANK; and a
	// beam starts only while the gas fills less than pt_flow_heatbeam_budget
	// of the blocks (keeps going to half as much again). On a grid of its own,
	// pt_flow_heatbeam_cell_size: it shared the railgun's far one (1.5), which
	// came out boxy, and the railgun's cells are not to get any finer for it
	// (Matt, 11-09). How long a beam is run for when a pause lays it again
	// (flow_heatbeam_relay), 0 never. Picked by reasoning, not tuned by
	// eye: the rings' heat 0.9, the sheath's 0.7 and 512 long were the first
	// guesses, rings Matt found far too dim (11-09).
	cvar_pt_flow_heatbeam_ring_radius = Cvar_Get("pt_flow_heatbeam_ring_radius", "4", 0);
	cvar_pt_flow_heatbeam_ring_width = Cvar_Get("pt_flow_heatbeam_ring_width", "1", 0);
	// 125 at 225 units/s: a ring every 0.56 s. Matt (11-12), in turns: 32 at 96
	// (every 1/3 s); twice the time between them, 64 (2/3 s); twice again and
	// a little faster, 160 at 120 (4/3 s - he saw about 2 s); "one second" and
	// a little faster, 150 at 150 (he counted "one one thousand two"); then one
	// a second at 1.5 x that speed; then spacing 125 (a ring every 0.56 s), Matt's
	// pick. The strands' twist runs at this speed too.
	cvar_pt_flow_heatbeam_ring_spacing = Cvar_Get("pt_flow_heatbeam_ring_spacing", "125", 0);
	cvar_pt_flow_heatbeam_ring_speed = Cvar_Get("pt_flow_heatbeam_ring_speed", "225", 0);
	// The rings' heat where they show: 1.25 is white (flow_white_hot), 1
	// the top of the fire map, golden yellow - all of their run until 11-12.
	cvar_pt_flow_heatbeam_ring_temperature = Cvar_Get("pt_flow_heatbeam_ring_temperature", "1.25", 0);
	cvar_pt_flow_heatbeam_ring_smoke = Cvar_Get("pt_flow_heatbeam_ring_smoke", "0.3", 0);
	cvar_pt_flow_heatbeam_ring_expansion = Cvar_Get("pt_flow_heatbeam_ring_expansion", "4", 0);
	cvar_pt_flow_heatbeam_ring_brightness = Cvar_Get("pt_flow_heatbeam_ring_brightness", "1", 0);
	// How far a ring runs from where it shows, cooling white to yellow to
	// orange to smoke on the way (flow_heatbeam_ring_heat): 384 units, 1.7 s
	// at 225 units/s, three rings at the 125 spacing (256 until Matt
	// asked for them to run further, 11-12). 0: the whole beam at the start's heat,
	// as before 11-12. The smoke it ends as. And how many units of glow each
	// ring keeps behind it before its quench (flow_heatbeam); 0 none, the
	// layer's cooling alone.
	cvar_pt_flow_heatbeam_ring_travel = Cvar_Get("pt_flow_heatbeam_ring_travel", "384", 0);
	cvar_pt_flow_heatbeam_ring_end_smoke = Cvar_Get("pt_flow_heatbeam_ring_end_smoke", "1.5", 0);
	cvar_pt_flow_heatbeam_ring_tail = Cvar_Get("pt_flow_heatbeam_ring_tail", "4", 0);
	// 1 until 11-12; Matt: "I would just like it a little bit brighter"
	cvar_pt_flow_heatbeam_core_brightness = Cvar_Get("pt_flow_heatbeam_core_brightness", "1.25", 0);
	cvar_pt_flow_heatbeam_end_on = Cvar_Get("pt_flow_heatbeam_end_on", "1", 0);
	cvar_pt_flow_heatbeam_core_clear = Cvar_Get("pt_flow_heatbeam_core_clear", "16", 0);
	cvar_pt_flow_heatbeam_near_fade = Cvar_Get("pt_flow_heatbeam_near_fade", "8", 0);
	cvar_pt_flow_heatbeam_near_brightness = Cvar_Get("pt_flow_heatbeam_near_brightness", "0", 0);
	cvar_pt_flow_heatbeam_relay = Cvar_Get("pt_flow_heatbeam_relay", "0.25", 0);
	cvar_pt_flow_heatbeam_cell_size = Cvar_Get("pt_flow_heatbeam_cell_size", "1", CVAR_ARCHIVE);
	// Your own beam in first person laid a second time, from the gun your
	// body holds, for mirrors (FLOW_LAYER_HEATBEAM_MIRROR): twice the blocks
	// for it. 0: a mirror shows the beam's model and classic rings there.
	cvar_pt_flow_heatbeam_mirror = Cvar_Get("pt_flow_heatbeam_mirror", "1", 0);
	// Whether the beam leaves its gas where it was as it is swept - glow
	// lifting off in curls, smoke - or only ever is where it is now (0,
	// flow_layer_simulate). Matt, 11-12, of strafing with it: "the beam
	// leaves behind trails. This really hits performance hard", so 0.
	cvar_pt_flow_heatbeam_trails = Cvar_Get("pt_flow_heatbeam_trails", "0", CVAR_ARCHIVE);
	// With no trails, Flow only lays the beam each step: none of its motion,
	// and none of the blocks kept for it, is simulated (flow_heatbeam_lean).
	// 0 = Flow does it all, as before.
	cvar_pt_flow_heatbeam_lean = Cvar_Get("pt_flow_heatbeam_lean", "1", 0);
	// Flow steps every frame while your own beam is on, not pt_flow_rate
	// times a second (flow_every_frame): its gas is laid where the gun is in
	// every frame drawn. 0: the frames between steps show it where it was.
	cvar_pt_flow_heatbeam_every_frame = Cvar_Get("pt_flow_heatbeam_every_frame", "1", 0);
	// With no trails, each cell gets as much of the beam's heat, fuel and smoke
	// as the shape covers of it (flow_heatbeam), so its edges fall off across a
	// cell rather than stepping from cell to cell. 0: any cell a quarter
	// covered or more is filled whole, as before 11-12.
	cvar_pt_flow_heatbeam_soft_fill = Cvar_Get("pt_flow_heatbeam_soft_fill", "1", 0);
	// the rings' glow as covered, their colour and smoke hard (flow_heatbeam's
	// SOFT RING GLOW; Matt, 11-12, lines in a single ring); 0 the hard fill
	cvar_pt_flow_heatbeam_ring_soft = Cvar_Get("pt_flow_heatbeam_ring_soft", "1", 0);
	// The core's figure (flow_heatbeam, THE CORE'S FIGURE): a hotter middle,
	// this share of the core's radius round (0 none), at this heat (1, the top
	// of the fire map: some three times as bright as the core's 0.88), this
	// share of the core's glow; and strands twisting through the core (0
	// none), a turn every so many units, this share of the core's radius out,
	// so thick (half), white-hot, this share of the core's glow, over the first
	// so many units of the core. Picked by reasoning, not by eye (11-12).
	// Then the middle 1 -> 1.06 and the strands 1.15 -> 1.19 (Matt, 11-12:
	// "the plasma beam core brightest spots be a little tiny bit closer to
	// white"): a quarter of the way and three quarters of it to white
	// (flow_white_hot, white at 1.25).
	cvar_pt_flow_heatbeam_inner_radius = Cvar_Get("pt_flow_heatbeam_inner_radius", "0.5", 0);
	cvar_pt_flow_heatbeam_inner_temperature = Cvar_Get("pt_flow_heatbeam_inner_temperature", "1.06", 0);
	cvar_pt_flow_heatbeam_inner_brightness = Cvar_Get("pt_flow_heatbeam_inner_brightness", "1", 0);
	cvar_pt_flow_heatbeam_strands = Cvar_Get("pt_flow_heatbeam_strands", "2", 0);
	cvar_pt_flow_heatbeam_strand_pitch = Cvar_Get("pt_flow_heatbeam_strand_pitch", "48", 0);
	cvar_pt_flow_heatbeam_strand_radius = Cvar_Get("pt_flow_heatbeam_strand_radius", "0.6", 0);
	cvar_pt_flow_heatbeam_strand_width = Cvar_Get("pt_flow_heatbeam_strand_width", "0.4", 0);
	cvar_pt_flow_heatbeam_strand_temperature = Cvar_Get("pt_flow_heatbeam_strand_temperature", "1.19", 0);
	cvar_pt_flow_heatbeam_strand_brightness = Cvar_Get("pt_flow_heatbeam_strand_brightness", "1", 0);
	cvar_pt_flow_heatbeam_strand_length = Cvar_Get("pt_flow_heatbeam_strand_length", "256", 0);
	cvar_pt_flow_heatbeam_radius = Cvar_Get("pt_flow_heatbeam_radius", "1.5", 0);
	cvar_pt_flow_heatbeam_temperature = Cvar_Get("pt_flow_heatbeam_temperature", "0.88", 0);
	cvar_pt_flow_heatbeam_smoke = Cvar_Get("pt_flow_heatbeam_smoke", "0.15", 0);
	cvar_pt_flow_heatbeam_expansion = Cvar_Get("pt_flow_heatbeam_expansion", "10", 0);
	cvar_pt_flow_heatbeam_flow = Cvar_Get("pt_flow_heatbeam_flow", "100", 0);
	cvar_pt_flow_heatbeam_impact_radius = Cvar_Get("pt_flow_heatbeam_impact_radius", "6", 0);
	cvar_pt_flow_heatbeam_impact_temperature = Cvar_Get("pt_flow_heatbeam_impact_temperature", "1", 0);
	cvar_pt_flow_heatbeam_impact_smoke = Cvar_Get("pt_flow_heatbeam_impact_smoke", "1.5", 0);
	cvar_pt_flow_heatbeam_impact_expansion = Cvar_Get("pt_flow_heatbeam_impact_expansion", "30", 0);
	cvar_pt_flow_heatbeam_cooling = Cvar_Get("pt_flow_heatbeam_cooling", "4", 0);
	cvar_pt_flow_heatbeam_buoyancy = Cvar_Get("pt_flow_heatbeam_buoyancy", "1", 0);
	cvar_pt_flow_heatbeam_vorticity = Cvar_Get("pt_flow_heatbeam_vorticity", "2", 0);
	cvar_pt_flow_heatbeam_smoke_fade = Cvar_Get("pt_flow_heatbeam_smoke_fade", "0.5", 0);
	cvar_pt_flow_heatbeam_brightness = Cvar_Get("pt_flow_heatbeam_brightness", "40", 0);
	cvar_pt_flow_heatbeam_yellow = Cvar_Get("pt_flow_heatbeam_yellow", "1", 0);
	cvar_pt_flow_heatbeam_smoke_brightness = Cvar_Get("pt_flow_heatbeam_smoke_brightness", "4", 0);
	cvar_pt_flow_heatbeam_density = Cvar_Get("pt_flow_heatbeam_density", "0.05", 0);
	cvar_pt_flow_heatbeam_length = Cvar_Get("pt_flow_heatbeam_length", "1024", 0);
	cvar_pt_flow_heatbeam_clear = Cvar_Get("pt_flow_heatbeam_clear", "80", 0);  // 32, then 48 (11-09), then 80 (11-10): Matt wants the rings further out
	cvar_pt_flow_heatbeam_budget = Cvar_Get("pt_flow_heatbeam_budget", "0.5", 0);

	// A rocket's trail (a trail of fire, flow_rocket_spheres): its puffs'
	// radius, push, smoke and heat at scale 1; how much of the puff the flame
	// fills (the smoke the same either way); how far behind the rocket the
	// fire is put out, which makes the ball at its tail (12: some 12-20 units
	// of glow, measured headless); how lumpy the smoke comes out (0: a smooth
	// tube); and the share of pt_flow_max_blocks the gas may fill for a new
	// trail to start - past it the client's particle flame goes instead, and
	// a trail stops at half as much again. One rocket's smoke lasts some 3 s
	// and peaks at 250 blocks over a 2000-unit flight. The flame was 1 / 1.5
	// / 16 until Matt asked for it smaller (10-25).
	cvar_pt_flow_rocket_radius = Cvar_Get("pt_flow_rocket_radius", "8", 0);
	cvar_pt_flow_rocket_expansion = Cvar_Get("pt_flow_rocket_expansion", "120", 0);
	cvar_pt_flow_rocket_smoke = Cvar_Get("pt_flow_rocket_smoke", "4", 0);
	cvar_pt_flow_rocket_temperature = Cvar_Get("pt_flow_rocket_temperature", "3", 0);
	cvar_pt_flow_rocket_flame = Cvar_Get("pt_flow_rocket_flame", "0.65", 0);
	cvar_pt_flow_rocket_glow = Cvar_Get("pt_flow_rocket_glow", "12", 0);
	cvar_pt_flow_rocket_lumps = Cvar_Get("pt_flow_rocket_lumps", "1", 0);
	cvar_pt_flow_rocket_budget = Cvar_Get("pt_flow_rocket_budget", "0.5", 0);
	// How bright the trail's fire glows, as pt_flow_fire_brightness for the
	// blasts: the trail is on a layer of its own (FLOW_LAYER_FIRE_TRAIL) so
	// that it could keep the 40 the blasts had before they went down to 5 for
	// HDR - its small ball of fire looked better at it (Matt, 11-09).
	cvar_pt_flow_rocket_brightness = Cvar_Get("pt_flow_rocket_brightness", "40", 0);

	// Something on fire (R_FLOW_BURN, flow_burn_spheres): its flames' heat
	// at full burn (1.5: white-yellow at the heart, under the colour map's
	// top for a third of a second as it rises), its smoke, the flame's share
	// of a puff, the puffs' radius - this share of the box's narrower side,
	// within min and max - and how much they vary; the burst it starts with,
	// seconds and swell; and the share of Flow's blocks a new one starts
	// within. Picked by reasoning from the rocket trail's, not tuned by eye.
	cvar_pt_flow_burn_temperature = Cvar_Get("pt_flow_burn_temperature", "1.5", 0);
	cvar_pt_flow_burn_smoke = Cvar_Get("pt_flow_burn_smoke", "1.5", 0);
	cvar_pt_flow_burn_flame = Cvar_Get("pt_flow_burn_flame", "0.65", 0);
	cvar_pt_flow_burn_size = Cvar_Get("pt_flow_burn_size", "0.6", 0);
	cvar_pt_flow_burn_min_radius = Cvar_Get("pt_flow_burn_min_radius", "5", 0);
	cvar_pt_flow_burn_max_radius = Cvar_Get("pt_flow_burn_max_radius", "14", 0);
	cvar_pt_flow_burn_lumps = Cvar_Get("pt_flow_burn_lumps", "1", 0);
	cvar_pt_flow_burn_burst = Cvar_Get("pt_flow_burn_burst", "0.15", 0);
	cvar_pt_flow_burn_expansion = Cvar_Get("pt_flow_burn_expansion", "60", 0);
	cvar_pt_flow_burn_budget = Cvar_Get("pt_flow_burn_budget", "0.5", 0);
	// A railgun's gib smoking (R_FLOW_BURN with smoke_only, cl_flow_gib_effects):
	// its smoke at full - Matt, 11-12: "the gibs smoking like the plasma beam
	// gib effect without actual fire"; a burning gib's starts at 1.5,
	// pt_flow_burn_smoke, and was 1 here until "basically the effect from the
	// plasma beam, rockets, grenades only without fire" - and the heat that
	// lifts it, as high as it goes and stays under where the fire map starts
	// to glow (pt_flow_fire_glow_start, 0.3); a grenade's trail has 0.1.
	cvar_pt_flow_gib_smoke = Cvar_Get("pt_flow_gib_smoke", "1.5", 0);
	cvar_pt_flow_gib_smoke_heat = Cvar_Get("pt_flow_gib_smoke_heat", "0.25", 0);
	// ...and the push up that carries it off the gib, units/s - a slow
	// smoulder's, some 1 m/s - and its source's size
	// against a flame's puff (pt_flow_burn_min_radius 5: 3, a cell at
	// pt_flow_cell_size 3). Picked by reasoning, not by eye (11-12).
	cvar_pt_flow_gib_smoke_rise = Cvar_Get("pt_flow_gib_smoke_rise", "40", 0);
	cvar_pt_flow_gib_smoke_size = Cvar_Get("pt_flow_gib_smoke_size", "0.6", 0);

	// A grenade's trail (a trail of fire with smoke_only, flow_grenade_spheres):
	// its puffs' radius, push and smoke at scale 1. The smoke falls to _thin of
	// that over the first _thin_length units of flight, as the classic trail
	// thins. _heat lifts it and is held well under the fire's glow. _lumps is
	// as the rocket's. A grenade's trail shares the rockets'
	// pt_flow_rocket_budget, and past it the client draws the classic
	// particles. Picked by reasoning, not tuned by eye (11-07).
	cvar_pt_flow_grenade_radius = Cvar_Get("pt_flow_grenade_radius", "6", 0);
	cvar_pt_flow_grenade_expansion = Cvar_Get("pt_flow_grenade_expansion", "60", 0);
	cvar_pt_flow_grenade_smoke = Cvar_Get("pt_flow_grenade_smoke", "2", 0);
	cvar_pt_flow_grenade_heat = Cvar_Get("pt_flow_grenade_heat", "0.1", 0);
	cvar_pt_flow_grenade_thin = Cvar_Get("pt_flow_grenade_thin", "0.35", 0);
	cvar_pt_flow_grenade_thin_length = Cvar_Get("pt_flow_grenade_thin_length", "96", 0);
	cvar_pt_flow_grenade_lumps = Cvar_Get("pt_flow_grenade_lumps", "1", 0);

	// An explosion under water (R_FLOW_BUBBLES): foam that rises to the
	// surface. Rise is Flow's buoyancy per unit of smoke; fade the amount
	// lost per second, on top of 30% of it.
	cvar_pt_flow_bubble_radius = Cvar_Get("pt_flow_bubble_radius", "24", 0);
	cvar_pt_flow_bubble_expansion = Cvar_Get("pt_flow_bubble_expansion", "250", 0);
	cvar_pt_flow_bubble_amount = Cvar_Get("pt_flow_bubble_amount", "1", 0);
	cvar_pt_flow_bubble_rise = Cvar_Get("pt_flow_bubble_rise", "2", 0);
	cvar_pt_flow_bubble_fade = Cvar_Get("pt_flow_bubble_fade", "0.4", 0);
	cvar_pt_flow_bubble_brightness = Cvar_Get("pt_flow_bubble_brightness", "6", 0);
	cvar_pt_flow_bubble_density = Cvar_Get("pt_flow_bubble_density", "0.05", 0);

	// The light (pt_flow_lighting): fire at pt_flow_light times FLOW_LIGHT_UNIT,
	// plasma at pt_flow_bfg_light (the BFG's blast at pt_flow_bfg_blast_light,
	// which peaks at some three times the classic BFG explosion light, as a
	// rocket's fireball does the classic explosion's; the ball in flight at
	// pt_flow_bfg_trail_light),
	// a sphere pt_flow_light_size times as wide
	// as the glow is spread, up to pt_flow_light_radius; the smoke's shadow
	// toward it no darker than pt_flow_shadow_min.
	cvar_pt_flow_light = Cvar_Get("pt_flow_light", "1", 0);
	cvar_pt_flow_bfg_light = Cvar_Get("pt_flow_bfg_light", "0.6", 0);
	// The plasma beam's light and the light of things on fire - with the fire:
	// the client sets 1 while cl_flow_heatbeam_fire is on and 0 while it is off
	// (Matt, 11-12, after trying it; flow_heatbeam_light_on). 1 lights the beam's gas along its core as the
	// railgun's beam is lit, at pt_flow_heatbeam_light_power per 100 units
	// (vkpt_flow_beam_lights), and anything on fire - a wall the beam is on,
	// a burning body or gib - by its own flames' glow at pt_flow_burn_light
	// times FLOW_LIGHT_UNIT, as a fireball is by its glow at pt_flow_light
	// (flow_burn_light). Both with pt_flow_lighting only. 300 is under the
	// classic rail's points (400); 20 is a guess that a burning body comes out
	// at a few hundred, as a small fire's glow is some 1/100 a fireball's -
	// not measured.
	cvar_pt_flow_heatbeam_light = Cvar_Get("pt_flow_heatbeam_light", "0", 0);
	cvar_pt_flow_heatbeam_light_power = Cvar_Get("pt_flow_heatbeam_light_power", "300", 0);
	cvar_pt_flow_burn_light = Cvar_Get("pt_flow_burn_light", "20", 0);
	cvar_pt_flow_light_size = Cvar_Get("pt_flow_light_size", "0.5", 0);
	cvar_pt_flow_light_radius = Cvar_Get("pt_flow_light_radius", "24", 0);
	cvar_pt_flow_shadow = Cvar_Get("pt_flow_shadow", "1", 0);
	cvar_pt_flow_shadow_min = Cvar_Get("pt_flow_shadow_min", "0.15", 0);

	// The world holding the gas (pt_flow_collision): units behind a face and
	// in front of it where the gas is held still.
	cvar_pt_flow_collision_depth = Cvar_Get("pt_flow_collision_depth", "12", 0);
	cvar_pt_flow_collision_margin = Cvar_Get("pt_flow_collision_margin", "3", 0);

	// The world's light on the smoke (pt_flow_env, ray query only): the sun,
	// the sky and the map's lights, from probes pt_flow_env_spacing apart,
	// pt_flow_env_light times as bright as they are (1 is right for smoke
	// that scatters pt_flow_smoke_scatter of it), with pt_flow_env_flat of
	// the flat smoke colour kept for the light the walls bounce, which the
	// probes do not see. A probe averages over pt_flow_env_history frames,
	// and one light sample brings no more than pt_flow_env_clamp times the
	// adapted luminance. Past pt_flow_env_max_probes the probes spread out.
	cvar_pt_flow_env = Cvar_Get("pt_flow_env", "1", CVAR_ARCHIVE);
	cvar_pt_flow_env_light = Cvar_Get("pt_flow_env_light", "1", 0);
	cvar_pt_flow_env_flat = Cvar_Get("pt_flow_env_flat", "0.01", 0);   // Matt's, 2026-10-25
	cvar_pt_flow_env_spacing = Cvar_Get("pt_flow_env_spacing", "32", 0);
	cvar_pt_flow_env_history = Cvar_Get("pt_flow_env_history", "16", 0);
	cvar_pt_flow_env_clamp = Cvar_Get("pt_flow_env_clamp", "64", 0);
	cvar_pt_flow_env_max_probes = Cvar_Get("pt_flow_env_max_probes", "32768", 0);
	Cvar_Get("flow_env_available", "0", CVAR_ROM);

	Cvar_FullSet("flow_available", "0", CVAR_ROM, FROM_CODE);

	if (vkpt_flow_backend) {
		// The backend's buffers at the same strides, and its dummies in the
		// sets; it makes the passes too. Without them there is no Flow.
		flow_set_strides();
		if (!vkpt_flow_backend->create(flow.params_stride, flow.light_sums_stride, flow.occupancy_stride,
				&flow.params_mapped, &flow.light_sums_mapped)) {
			flow.params_mapped = NULL;
			flow.light_sums_mapped = NULL;
			Com_WPrintf("NVIDIA Flow: the renderer could not make its buffers - no Flow.\n");
			return VK_SUCCESS;
		}
		// (the light sums are read only where the GPU has written them: light_sum_blocks)
		memset(flow.params_mapped, 0, flow.params_stride * FLOW_SET_COUNT);
		for (int set = 0; set < FLOW_SET_COUNT; set++)
			vkpt_flow_backend->bind(set, NULL, 0, NULL);
	}
	else {
		VkResult result = flow_create_dummies();
		if (result != VK_SUCCESS)
			return result;

		for (int set = 0; set < FLOW_SET_COUNT; set++)
			flow_write_descriptors(set, flow.dummy_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, flow.dummy_table.buffer);

		VkDescriptorSetLayout desc_set_layouts[] = {
			qvk.desc_set_layout_ubo, qvk.desc_set_layout_textures,
		};
		CREATE_PIPELINE_LAYOUT(qvk.device, &flow.lights_pipeline_layout,
			.setLayoutCount = LENGTH(desc_set_layouts),
			.pSetLayouts = desc_set_layouts,
		);
		ATTACH_LABEL_VARIABLE(flow.lights_pipeline_layout, PIPELINE_LAYOUT);
	}

	flow.num_emitters = 0;
	flow.updated_frame = 0;
	for (int i = 0; i < FLOW_MAX_LIGHTS; i++)
		flow.lights[i].layer = -1;

#if USE_FLOW
	flow_free_lights();
	flow_create_simulation();
	if (flow.grid) {
		if (vkpt_flow_backend) {
			// vkpt_flow_create_pipelines' word on the probes, for the menu
			flow.env_backend = vkpt_flow_backend->create_env();
			Cvar_FullSet("flow_env_available", flow.env_backend ? "1" : "0", CVAR_ROM, FROM_CODE);
		}
		else
			flow_env_create();
	}
#endif

	return VK_SUCCESS;
}

VkResult vkpt_flow_destroy(void)
{
#if USE_FLOW
	if (vkpt_flow_backend) {
		vkpt_flow_backend->destroy_env();
		flow.env_backend = false;
		Cvar_FullSet("flow_env_available", "0", CVAR_ROM, FROM_CODE);
	}
	else
		flow_env_destroy();
	flow_destroy_simulation();
	Z_Free(flow.world_tris);
	Z_Free(flow.mesh_positions);
	Z_Free(flow.mesh_indices);
	Z_Free(flow.mesh_counts);
	flow.world_tris = NULL;
	flow.num_world_tris = 0;
	flow.mesh_positions = NULL;
	flow.mesh_indices = NULL;
	flow.mesh_counts = NULL;
	flow.mesh_capacity = 0;
	flow.num_mesh_tris = 0;
	flow.mesh_region_valid = false;
#endif
	if (vkpt_flow_backend) {
		vkpt_flow_backend->destroy();
		flow.params_mapped = NULL;
		flow.light_sums_mapped = NULL;
	}
	else {
		if (flow.lights_pipeline_layout)
			vkDestroyPipelineLayout(qvk.device, flow.lights_pipeline_layout, NULL);
		flow.lights_pipeline_layout = VK_NULL_HANDLE;
		flow_destroy_dummies();
	}
	flow.num_emitters = 0;
	flow.marching = false;
	flow.updated_frame = 0;
	memset(&flow.dispatch, 0, sizeof(flow.dispatch));
	return VK_SUCCESS;
}

VkResult vkpt_flow_create_pipelines(void)
{
	VkComputePipelineCreateInfo pipeline_info = {
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = SHADER_STAGE(QVK_MOD_FLOW_LIGHTS_COMP, VK_SHADER_STAGE_COMPUTE_BIT),
		.layout = flow.lights_pipeline_layout,
	};

	_VK(vkCreateComputePipelines(qvk.device, 0, 1, &pipeline_info, 0, &flow.lights_pipeline));
	ATTACH_LABEL_VARIABLE(flow.lights_pipeline, PIPELINE);

	pipeline_info.stage = (VkPipelineShaderStageCreateInfo)SHADER_STAGE(QVK_MOD_FLOW_OCCUPANCY_COMP, VK_SHADER_STAGE_COMPUTE_BIT);
	_VK(vkCreateComputePipelines(qvk.device, 0, 1, &pipeline_info, 0, &flow.occupancy_pipeline));
	ATTACH_LABEL_VARIABLE(flow.occupancy_pipeline, PIPELINE);

	// The world's light on the smoke, where flow_env_create made its layout.
	if (flow.env_pipeline_layout) {
		pipeline_info.stage = (VkPipelineShaderStageCreateInfo)SHADER_STAGE(QVK_MOD_FLOW_AMBIENT_COMP, VK_SHADER_STAGE_COMPUTE_BIT);
		pipeline_info.layout = flow.env_pipeline_layout;
		_VK(vkCreateComputePipelines(qvk.device, 0, 1, &pipeline_info, 0, &flow.env_pipeline));
		ATTACH_LABEL_VARIABLE(flow.env_pipeline, PIPELINE);
	}
	Cvar_FullSet("flow_env_available", flow.env_pipeline ? "1" : "0", CVAR_ROM, FROM_CODE);

	return VK_SUCCESS;
}

VkResult vkpt_flow_destroy_pipelines(void)
{
	if (flow.lights_pipeline)
		vkDestroyPipeline(qvk.device, flow.lights_pipeline, NULL);
	flow.lights_pipeline = VK_NULL_HANDLE;
	if (flow.occupancy_pipeline)
		vkDestroyPipeline(qvk.device, flow.occupancy_pipeline, NULL);
	flow.occupancy_pipeline = VK_NULL_HANDLE;
	if (flow.env_pipeline)
		vkDestroyPipeline(qvk.device, flow.env_pipeline, NULL);
	flow.env_pipeline = VK_NULL_HANDLE;
	Cvar_FullSet("flow_env_available", "0", CVAR_ROM, FROM_CODE);

	return VK_SUCCESS;
}

void vkpt_flow_register(void)
{
#if USE_FLOW
	R_FlowEmit = vkpt_flow_emit;
#else
	R_FlowEmit = NULL;
#endif
}

/* d3d12_vram (Matt, 11-13: "where is the vram budget all going?"): what Flow's own
   device has made - its backend counts every allocation and free (DeviceD3D12.cpp's
   device_reportMemoryAllocate) - and the block pool it was asked for. False without
   a Flow device. The renderer's own Flow buffers (the march's) are not in it. */
bool vkpt_flow_memory(uint64_t *device_bytes, uint64_t *upload_bytes, uint64_t *readback_bytes, int *pool)
{
#if USE_FLOW
	if (flow.device && flow.device_iface && flow.device_iface->getMemoryStats) {
		NvFlowDeviceMemoryStats stats = { 0 };
		flow.device_iface->getMemoryStats(flow.device, &stats);
		*device_bytes = stats.deviceMemoryBytes;
		*upload_bytes = stats.uploadMemoryBytes;
		*readback_bytes = stats.readbackMemoryBytes;
		*pool = Cvar_ClampInteger(cvar_pt_flow_max_blocks, FLOW_POOL_MIN, FLOW_POOL_MAX);
		return true;
	}
#endif
	*device_bytes = *upload_bytes = *readback_bytes = 0;
	*pool = 0;
	return false;
}

void vkpt_flow_reset(void)
{
	flow.num_emitters = 0;
#if USE_FLOW
	flow.have_last_time = false;
	flow.have_bubble_surface = false;
	flow.have_blocks = false;
	flow.relay_step = -1;
	flow.relaid = false;
	flow_free_lights();
	for (int set = 0; set < FLOW_SET_COUNT; set++)
		flow.light_sum_blocks[set] = 0;
	if (flow.grid) {
		flow.force_clear = true;
		flow.idle = false;
		// After the clear, a step that makes Flow's memory (flow_warm_emitters).
		// Every map: once made it is kept, so later ones cost a step and no more.
		flow.warm_pending = cvar_pt_flow_prewarm && cvar_pt_flow_prewarm->integer;
	}
#endif
}

void vkpt_flow_set_world(const bsp_mesh_t *wm)
{
#if USE_FLOW
	Z_Free(flow.world_tris);
	flow.world_tris = NULL;
	flow.num_world_tris = 0;
	flow.num_mesh_tris = 0;
	flow.mesh_region_valid = false;

	if (!wm || !wm->primitives)
		return;

	// Only the static, opaque world: glass, water and grates let smoke
	// through, and brush models move.
	const model_geometry_t *geom = &wm->geom_opaque;
	int total = 0;
	for (uint32_t g = 0; g < geom->num_geometries; g++) {
		if (geom->prim_offsets[g] + geom->prim_counts[g] <= wm->num_primitives)
			total += (int)geom->prim_counts[g];
	}
	if (total == 0)
		return;

	flow.world_tris = Z_Malloc(sizeof(float) * 9 * total);
	int n = 0;
	for (uint32_t g = 0; g < geom->num_geometries; g++) {
		if (geom->prim_offsets[g] + geom->prim_counts[g] > wm->num_primitives)
			continue;
		for (uint32_t p = 0; p < geom->prim_counts[g]; p++) {
			const VboPrimitive *prim = wm->primitives + geom->prim_offsets[g] + p;
			float *dst = flow.world_tris + n * 9;
			VectorCopy(prim->pos0, dst + 0);
			VectorCopy(prim->pos1, dst + 3);
			VectorCopy(prim->pos2, dst + 6);
			n++;
		}
	}
	flow.num_world_tris = n;
#else
	(void)wm;
#endif
}

void vkpt_flow_set_env(const bsp_t *bsp, int first_flow_light, int num_flow_lights)
{
	flow.env_bsp = bsp;
	flow.env_first_light = first_flow_light;
	flow.env_num_lights = max(num_flow_lights, 0);
}

/*
The probes of the world's light on the smoke (flow_ambient.comp), for this
frame's march. Recorded right after the TLAS is built, into the same command
buffer, and before the primary rays. The pass's set takes this frame's TLAS
here every frame: a handle is not an identity - a TLAS rebuilt after a map
change may come back with the same one (see
vkpt_god_rays_update_tlas_descriptor) - and the set is per frame in flight,
so the frame fence has retired its last use.
*/
void vkpt_flow_ambient(VkCommandBuffer cmd_buf)
{
#if USE_FLOW
	if (!flow.env_pipeline || flow.updated_frame != qvk.frame_counter + 1)
		return;

	const int set = qvk.frame_counter & 1;
	FlowParams *params = (FlowParams *)(flow.params_mapped + set * flow.params_stride);
	if (params->env_count[0] <= 0)
		return;

	// the classic TLAS's handle or (pt_ptlas) the partitioned one's address
	vkpt_tlas_write_t tlas_write;
	const void *tlas_next = vkpt_pt_geometry_tlas_write(qvk.current_frame_index, &tlas_write);
	if (!tlas_next) {
		// no probes this frame: the march must not read the old ones
		params->env_count[0] = 0;
		return;
	}

	VkDescriptorSet desc_set = flow.env_sets[qvk.current_frame_index];
	VkWriteDescriptorSet write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.pNext = tlas_next,
		.dstSet = desc_set,
		.dstBinding = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
	};
	vkUpdateDescriptorSets(qvk.device, 1, &write, 0, NULL);

	// The TLAS build, the uniform buffer's copy, and the last frame's march
	// over the probes, all before this writes them.
	VkMemoryBarrier before = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT,
	};
	vkCmdPipelineBarrier(cmd_buf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0, 1, &before, 0, NULL, 0, NULL);

	vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, flow.env_pipeline);
	VkDescriptorSet desc_sets[] = {
		desc_set,
		qvk.desc_set_vertex_buffer,
		qvk.desc_set_ubo,
		qvk_get_current_desc_set_textures(),
	};
	vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, flow.env_pipeline_layout, 0,
		LENGTH(desc_sets), desc_sets, 0, NULL);
	vkCmdDispatch(cmd_buf, flow.env_groups[0], flow.env_groups[1], flow.env_groups[2]);

	// The probes for the march, in whichever stage it runs.
	VkMemoryBarrier after = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	};
	vkCmdPipelineBarrier(cmd_buf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		0, 1, &after, 0, NULL, 0, NULL);
#else
	(void)cmd_buf;
#endif
}

int vkpt_flow_lights(dlight_t *lights, int max_lights)
{
#if USE_FLOW
	if (!flow.grid || max_lights <= 0)
		return 0;
	flow_lights_collect(qvk.frame_counter & 1);
	const int count = min(flow.num_dlights, max_lights);
	memcpy(lights, flow.dlights, sizeof(dlight_t) * count);
	return count;
#else
	(void)lights;
	(void)max_lights;
	return 0;
#endif
}

#if USE_FLOW
// Whether the plasma beam in slot `key` is given down your barrel this frame.
static bool flow_heatbeam_in_view(int key)
{
	for (int i = 0; i < flow.num_emitters; i++) {
		const flow_emitter_t *e = &flow.emitters[i];
		if (e->kind == R_FLOW_BEAM && e->layer == FLOW_LAYER_HEATBEAM_VIEW && e->key == key && e->seen)
			return true;
	}
	return false;
}
#endif

/*
The plasma beams' light (pt_flow_heatbeam_light 1, with pt_flow_lighting):
the railgun's way - its core beam is a cylinder light (vkpt_build_beam_lights)
- along each beam Flow draws this frame, from where its gas shows
(flow_heatbeam_stretch's `from`) to where it ends, the client's model drawing
the rest past pt_flow_heatbeam_length. Your own is lit once, from the beam
down your barrel, not again from its mirror copy.

Not the gas's own glow: a light the path tracer can sample must be a shape
(the march only draws the gas along primary rays and reflections - bounce
rays never see it). The shape is the core, pt_flow_heatbeam_radius round, cut
into pieces of no more than FLOW_BEAM_LIGHT_PIECE units - each piece takes
its own cluster, so a beam down a corridor lights the far end too, which one
cylinder the whole length would only from the cluster round its middle. Its
brightness is pt_flow_heatbeam_light_power per 100 units of beam, as a sphere
light's power goes (radiance x radius^2: a muzzle flash some 800, the classic
explosion 1600, a rocket's Flow fireball 5000; the classic rail's point
lights 400 per 100 units), spread over the prism's three sides: the flux
of a sphere light of that power is 4 pi^2 x power, that of a prism of radius
r and length h of radiance L is pi x L x 3 sqrt(3) r h. Absolute, as the
fire's: a light that is brighter for a brighter room, as the rail's beam
lights are (pt_beam_lights x the adapted luminance), would feed itself in a
room the beam is lighting - for as long as the beam is held. Golden yellow,
the hottest of the gas at pt_flow_heatbeam_yellow 1.

Called while the frame's light list is built (vkpt_prepare_frame), after the
client has given this frame's beams and before vkpt_flow_update ages them.
*/
#define FLOW_BEAM_LIGHT_PIECE   128.f
#define FLOW_BEAM_LIGHT_PIECES  16

void vkpt_flow_beam_lights(light_poly_t *lights, int *num_lights, int max_lights, bsp_t *bsp, int *light_entity_ids)
{
#if USE_FLOW
	if (!flow.grid || !bsp || !cvar_pt_flow_lighting->integer || !flow_heatbeam_light_on())
		return;

	const float power = max(cvar_pt_flow_heatbeam_light_power->value, 0.f);
	if (power <= 0.f)
		return;

	// golden yellow, luminance 1
	vec3_t colour = { 1.f, 0.75f, 0.15f };
	VectorScale(colour, 1.f / flow_luminance(colour), colour);

	for (int i = 0; i < flow.num_emitters; i++) {
		const flow_emitter_t *e = &flow.emitters[i];
		if (e->kind != R_FLOW_BEAM || !FLOW_LAYER_IS_HEATBEAM(e->layer) || !e->seen)
			continue;
		if (e->layer == FLOW_LAYER_HEATBEAM_MIRROR && flow_heatbeam_in_view(e->key))
			continue;

		vec3_t dir;
		VectorSubtract(e->end, e->origin, dir);
		const float length = VectorNormalize(dir);
		const float from = min(max(e->from, 0.f), length);
		if (length - from < 1.f)
			continue;

		const float radius = max(cvar_pt_flow_heatbeam_radius->value, 0.5f);
		// 4 pi / (3 sqrt(3)) = 2.4184: see above
		const float radiance = 2.4184f * power / (100.f * radius);
		vec3_t color;
		VectorScale(colour, radiance, color);

		const int pieces = min(max((int)ceilf((length - from) / FLOW_BEAM_LIGHT_PIECE), 1), FLOW_BEAM_LIGHT_PIECES);
		const float piece = (length - from) / pieces;
		for (int p = 0; p < pieces; p++) {
			vec3_t begin, end;
			VectorMA(e->origin, from + p * piece, dir, begin);
			VectorMA(e->origin, from + (p + 1) * piece, dir, end);

			// The light's identity for ReSTIR's remap, steady while the beam
			// is held: its slot and the piece. 0xFC is no other light's
			// (add_dlights' are 0xFE and 0xFD, the beams' RF_BEAM).
			entity_hash_t hash;
			hash.entity = (uint32_t)(1 + (e->key & 0x1FF) * FLOW_BEAM_LIGHT_PIECES + p) & 0x3FFF;
			hash.model = 0xFC;
			hash.mesh = 0;   // set per triangle by vkpt_build_cylinder_light
			hash.bsp = 0;

			if (!vkpt_build_cylinder_light(lights, num_lights, max_lights, bsp, begin, end, color, radius, hash, light_entity_ids))
				return;
		}
	}
#else
	(void)lights;
	(void)num_lights;
	(void)max_lights;
	(void)bsp;
	(void)light_entity_ids;
#endif
}

#if USE_FLOW
// How flow.glsl draws a layer; flow_simulate has said where it is.
static void flow_layer_look(FlowLayerParams *look, int layer, float cell_size)
{
	// Fire and plasma brightness is per 32 units of it looked through.
	const float per_unit = 1.f / 32.f;
	const float scatter = max(cvar_pt_flow_smoke_scatter->value, 0.f);
	float smoke = 0.f;

	look->world_max[3] = FLT_MAX;

	// The glow's colour balance, none but the fire's and the plasma beam's.
	look->emission[1] = look->emission[2] = look->emission[3] = 1.f;

	switch (layer) {
	case R_FLOW_FIRE:
	case FLOW_LAYER_FIRE_TRAIL:
		look->info[2] = 1;
		look->emission[0] = max((layer == FLOW_LAYER_FIRE_TRAIL ? cvar_pt_flow_rocket_brightness : cvar_pt_flow_fire_brightness)->value, 0.f) * per_unit;
		{
			// Less green and less blue still: the hot heart goes from
			// yellow-white toward yellow and its edges toward orange.
			const float orange = Cvar_ClampValue(cvar_pt_flow_fire_orange, 0.f, 1.f);
			look->emission[2] = 1.f - 0.4f * orange;
			look->emission[3] = 1.f - 0.8f * orange;
		}
		// Short of the colour map's orange at 0.6.
		look->albedo[3] = Cvar_ClampValue(cvar_pt_flow_fire_glow_start, 0.f, 0.5f);
		smoke = cvar_pt_flow_smoke_albedo->value * cvar_pt_flow_smoke_brightness->value;
		VectorSet(look->smoke, smoke, smoke, smoke);
		look->smoke[3] = max(cvar_pt_flow_density->value, 0.f) / cell_size;
		VectorSet(look->albedo, scatter, scatter, scatter);
		break;

	case R_FLOW_PLASMA:
		// Green-white heat, then dark green smoke that the green light picks out.
		look->info[2] = 2;
		look->emission[0] = max(cvar_pt_flow_bfg_brightness->value, 0.f) * per_unit;
		smoke = max(cvar_pt_flow_bfg_smoke_brightness->value, 0.f);
		VectorSet(look->smoke, 0.15f * smoke, 0.45f * smoke, 0.12f * smoke);
		look->smoke[3] = max(cvar_pt_flow_bfg_smoke_density->value, 0.f) / cell_size;
		VectorSet(look->albedo, 0.7f * scatter, scatter, 0.6f * scatter);
		break;

	case R_FLOW_BUBBLES:
		// Pale blue-white foam that stops at the water's surface.
		look->info[2] = 0;
		smoke = max(cvar_pt_flow_bubble_brightness->value, 0.f);
		VectorSet(look->smoke, 0.85f * smoke, 0.95f * smoke, smoke);
		look->smoke[3] = max(cvar_pt_flow_bubble_density->value, 0.f) / cell_size;
		VectorSet(look->albedo, 0.9f * scatter, 0.95f * scatter, scatter);
		if (flow.have_bubble_surface)
			look->world_max[3] = flow.bubble_surface;
		break;

	case R_FLOW_RAIL:
	case FLOW_LAYER_RAIL_FAR:
		// Cyan-blue heat, then pale blue-grey smoke. On the coarse cells of
		// the far part the spiral comes out a little thicker - a cell only
		// partly in a box still fills up to its heat in a frame or two - so
		// its glow has a knob of its own.
		look->info[2] = 3;
		look->emission[0] = max(cvar_pt_flow_rail_brightness->value, 0.f) * per_unit;
		if (layer == FLOW_LAYER_RAIL_FAR)
			look->emission[0] *= max(cvar_pt_flow_rail_far_brightness->value, 0.f);
		smoke = max(cvar_pt_flow_rail_smoke_brightness->value, 0.f);
		VectorSet(look->smoke, 0.85f * smoke, 0.92f * smoke, smoke);
		look->smoke[3] = max(cvar_pt_flow_rail_density->value, 0.f) / cell_size;
		VectorSet(look->albedo, 0.9f * scatter, 0.95f * scatter, scatter);
		break;

	case R_FLOW_HEATBEAM:
	case FLOW_LAYER_HEATBEAM_VIEW:      // the march draws these two as this one (flow_march)
	case FLOW_LAYER_HEATBEAM_MIRROR:
		// The fire's colour map, toward the yellow of the classic rings
		// (palette 0xd8-0xdf, linear 1 : 0.5-1 : under 0.1): at
		// pt_flow_heatbeam_yellow 1 its hottest is golden yellow where the
		// fire's is orange-white, then orange and dull red as it cools, as
		// the fire's. Weighted by the fuel, how bright each part of it is
		// (flow_heatbeam). Faint warm grey smoke.
		look->info[2] = FLOW_COLORMAP_WEIGHTED_FIRE;
		look->emission[0] = max(cvar_pt_flow_heatbeam_brightness->value, 0.f) * per_unit;
		{
			const float yellow = Cvar_ClampValue(cvar_pt_flow_heatbeam_yellow, 0.f, 1.f);
			look->emission[2] = 1.f + 0.4f * yellow;
			look->emission[3] = 1.f - 0.7f * yellow;
		}
		look->albedo[3] = Cvar_ClampValue(cvar_pt_flow_fire_glow_start, 0.f, 0.5f);
		smoke = max(cvar_pt_flow_heatbeam_smoke_brightness->value, 0.f);
		VectorSet(look->smoke, smoke, 0.95f * smoke, 0.9f * smoke);
		look->smoke[3] = max(cvar_pt_flow_heatbeam_density->value, 0.f) / cell_size;
		VectorSet(look->albedo, scatter, scatter, scatter);
		break;
	}

	// The burn channel holds the shadow toward this light, numbered from 1.
	look->info[3] = (flow.shadow_pass && flow.layer_shadow[layer] >= 0) ? (uint32_t)flow.layer_shadow[layer] + 1 : 0;
}
#endif

#if USE_FLOW
// A timestamp for the profiler's Flow row, in a command buffer of its own:
// Flow submits its work itself, so there is nowhere inside it to put one. On
// another renderer, that renderer's (vkpt_flow_backend->profile).
static void flow_profile(VKPTProfilerAction action)
{
	if (vkpt_flow_backend) {
		if (vkpt_flow_backend->profile)
			vkpt_flow_backend->profile((int)action);
		return;
	}
	VkCommandBuffer cmd_buf = vkpt_begin_command_buffer(&qvk.cmd_buffers_graphics);
	vkpt_profiler_query_after(cmd_buf, PROFILER_FLOW_SIMULATION, action);
	vkpt_submit_command_buffer_simple(cmd_buf, qvk.queue_graphics, true);
}
#endif

bool vkpt_flow_profiled(void)
{
#if USE_FLOW
	return flow.grid != NULL;
#else
	return false;
#endif
}

/* Whether this frame's FlowParams has a grid for the march to read
   (block_dim_bits.w). When it does not, every flow.glsl march returns at its
   first test, so path_tracer.c binds the reflect/refract variant compiled
   without it (spec_flow 0): 167 -> 128 registers, nothing else changes. */
bool vkpt_flow_marching(void)
{
	return flow.marching;
}

bool vkpt_flow_env_profiled(void)
{
#if USE_FLOW
	return flow.grid && flow_env_ready() && cvar_pt_flow_env->integer;
#else
	return false;
#endif
}

/*
pt_flow_stats 1: what the primary-ray march did, per frame, averaged over a
second - from flow.glsl's counters in the readback buffer, which only ever
count up (a uint32 wraps, the deltas do not care). Rays that entered the box
round the gas, loop iterations, samples, block lookups; seen = samples that
added at least 1e-3 of their pixel, faint = under 1e-4 (all but nothing).
*/
void vkpt_flow_stats(const ReadbackBuffer *readback)
{
	static bool have_last;
	static uint32_t last[6];
	static uint64_t sums[6];
	static int frames;
	static unsigned start_ms;

	if (!cvar_pt_flow_stats || !cvar_pt_flow_stats->integer) {
		have_last = false;
		return;
	}

	const uint32_t now[6] = {
		readback->flow_rays, readback->flow_iterations, readback->flow_samples,
		readback->flow_blocks, readback->flow_seen, readback->flow_faint,
	};
	const unsigned ms = Sys_Milliseconds();
	if (have_last) {
		for (int i = 0; i < 6; i++)
			sums[i] += (uint32_t)(now[i] - last[i]);
		frames++;
	} else {
		memset(sums, 0, sizeof(sums));
		frames = 0;
		start_ms = ms;
	}
	memcpy(last, now, sizeof(last));
	have_last = true;

	if (ms - start_ms < 1000 || frames == 0)
		return;

	const double f = (double)frames;
	const double samples = (double)sums[2];
	Com_Printf("FLOWSTATS %d frames | per frame: rays %.0f iterations %.0f samples %.0f blocks %.0f | "
		"%.1f samples/ray, %.1f iterations/ray | seen %.1f%% faint %.1f%% of samples | %ux%u\n",
		frames, sums[0] / f, sums[1] / f, samples / f, sums[3] / f,
		sums[0] ? samples / (double)sums[0] : 0.0, sums[0] ? (double)sums[1] / (double)sums[0] : 0.0,
		samples > 0.0 ? 100.0 * (double)sums[4] / samples : 0.0,
		samples > 0.0 ? 100.0 * (double)sums[5] / samples : 0.0,
		qvk.extent_render.width, qvk.extent_render.height);

	memset(sums, 0, sizeof(sums));
	frames = 0;
	start_ms = ms;
}

void vkpt_flow_update(const refdef_t *fd, bool render_world)
{
	// Once a frame. timerefresh renders many views between one R_BeginFrame
	// and R_EndFrame, and by the second this frame's set is bound in command
	// buffers already submitted - it must not be rewritten under them.
	if (flow.updated_frame == qvk.frame_counter + 1)
		return;
	flow.updated_frame = qvk.frame_counter + 1;
	flow.marching = false;
	memset(&flow.dispatch, 0, sizeof(flow.dispatch));
	// the backend could not make its buffers (vkpt_flow_initialize said so)
	if (!flow.params_mapped)
		return;

	const int set = qvk.frame_counter & 1;
	FlowParams *params = (FlowParams *)(flow.params_mapped + set * flow.params_stride);

	VkImageView density_view = flow.dummy_view;
	VkImageLayout density_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	VkBuffer table = flow.dummy_table.buffer;
	// ...or on the backend's renderer, its native resources (NULL: its dummies)
	void *backend_density = NULL, *backend_table = NULL;
	uint32_t backend_format = 0;
	uint32_t blocks = 0;

	memset(params, 0, sizeof(*params));

#if USE_FLOW
	// This set was last read by the frame before last, whose fence
	// R_BeginFrame_RTX has waited for - so its light sums are in, too.
	if (flow.grid) {
		flow_lights_collect(set);
		flow_release_set(set);
	}

	// From before Flow's submission to after the passes that follow it - not
	// on the frames it has nothing to do, as flow_simulate tells them. (On the
	// backend's renderer those passes are in its frame list, which times them
	// apart and adds them to the row.)
	const bool profiled = flow.grid && render_world &&
		!(flow.idle && flow.num_emitters == 0 && !flow.force_clear);
	if (profiled)
		flow_profile(PROFILER_START);

	if (render_world) {
		VectorCopy(fd->vieworg, flow.view_origin);
		// 90 degrees across at 4:3 is about 73.7 up and down, whatever the
		// screen's shape (cl_adjustfov keeps the height)
		const float half_y = 0.5f * (float)DEG2RAD(fd->fov_y);
		flow.view_zoom = (half_y > 0.01f && half_y < 1.56f) ? 0.75f / tanf(half_y) : 1.f;
	}

	// The player setup menu's model view is not the world; the fire holds
	// still and stays out of it.
	if (flow.grid && render_world && flow_simulate(fd, set, params)) {
		bool have_grid;
		if (vkpt_flow_backend) {
			NvFlowD3D12ExternalTexture density;
			NvFlowD3D12ExternalBuffer sparse_table;
			flow.d3d12_get_texture(flow.set_density[set], &density);
			flow.d3d12_get_buffer(flow.set_table[set], &sparse_table);
			have_grid = density.resource && sparse_table.resource;
			if (have_grid) {
				backend_density = density.resource;
				backend_format = density.format;
				backend_table = sparse_table.resource;
			}
		}
		else {
			NvFlowVulkanExternalTexture density;
			NvFlowVulkanExternalBuffer sparse_table;
			flow.get_texture(flow.set_density[set], &density);
			flow.get_buffer(flow.set_table[set], &sparse_table);
			have_grid = density.imageView && sparse_table.buffer;
			if (have_grid) {
				density_view = (VkImageView)density.imageView;
				density_layout = (VkImageLayout)density.layout;
				table = (VkBuffer)sparse_table.buffer;
			}
		}

		if (have_grid) {
			// Smoke's extinction is per cell of the main grid on every layer,
			// so the railgun trail's finer cells change how sharp it is, not
			// how thick.
			const float cell_size = flow_layer_cell_size(R_FLOW_FIRE);

			params->block_dim_bits[3] = 1;
			flow.marching = true;
			params->limits[0] = (uint32_t)Cvar_ClampInteger(cvar_pt_flow_max_steps, 16, 4096);
			params->limits[1] = 64;
			params->limits[2] = flow.occupancy_blocks;
			const int march = Cvar_ClampInteger(cvar_pt_flow_march, 0, 31);
			params->limits[3] = (uint32_t)march;
			// The camera, for the screen tiles (pt_flow_march 16, flow.h
			// FLOW_SCREEN_TX): any frame round vieworg would do, as long as the
			// tiles and the march use the same one; the view's, so the tiles
			// cover the screen - a little past it, from the FOV.
			{
				vec3_t forward, right, up;
				AngleVectors(fd->viewangles, forward, right, up);
				VectorCopy(fd->vieworg, params->camera_pos);
				params->camera_pos[3] = (march & 16) ? 1.f : 0.f;
				VectorCopy(forward, params->camera_forward);
				VectorCopy(right, params->camera_right);
				VectorCopy(up, params->camera_up);
				const float fov_x = max(1.f, min(fd->fov_x, 170.f)), fov_y = max(1.f, min(fd->fov_y, 170.f));
				params->camera_forward[3] = 1.05f * tanf(0.5f * (float)DEG2RAD(fov_x));
				params->camera_right[3] = 1.05f * tanf(0.5f * (float)DEG2RAD(fov_y));
			}
			// 2: count only the primary rays the screen tiles pass, marched (flow.glsl flow_screen_pass)
			params->counts[2] = (uint32_t)Cvar_ClampInteger(cvar_pt_flow_stats, 0, 2);
			// Each grid in use marches pt_flow_step of its own cells a step.
			for (int g = 0; g < FLOW_GRIDS; g++) {
				if (params->grids[g][0] > 0.f)
					params->grids[g][3] = max(cvar_pt_flow_step->value, 0.1f) / params->grids[g][0];
			}
			params->world_min[3] = max(cvar_pt_flow_reflect_step->value, 1.f);
			params->world_max[3] = Cvar_ClampValue(cvar_pt_flow_glow_floor, 0.f, 1.f);
			for (int layer = 0; layer < FLOW_LAYERS; layer++)
				flow_layer_look(&params->layers[layer], layer, cell_size);
			flow_lights_params(set, params);
			flow_env_params(params);
			// Lit by the world, the smoke keeps only so much of its flat
			// colour: what stands in for the light bounced off the walls.
			if (params->env_count[0] > 0) {
				const float flat = Cvar_ClampValue(cvar_pt_flow_env_flat, 0.f, 1.f);
				for (int layer = 0; layer < FLOW_LAYERS; layer++)
					VectorScale(params->layers[layer].smoke, flat, params->layers[layer].smoke);
			}
			blocks = params->counts[0];
		}
		else {
			memset(params, 0, sizeof(*params));
		}
	}
	else {
		memset(params, 0, sizeof(*params));
	}
#else
	(void)fd;
	(void)render_world;
#endif

	if (vkpt_flow_backend)
		vkpt_flow_backend->bind(set, backend_density, backend_format, backend_table);
	else
		flow_write_descriptors(set, density_view, density_layout, table);

#if USE_FLOW
	if (vkpt_flow_backend) {
		// the same passes, recorded by the backend's renderer (vkpt_flow_dispatches)
		vkpt_flow_dispatch_t *d = &flow.dispatch;
		d->set = set;
		d->occupancy_blocks = min(blocks, flow.occupancy_blocks);
		flow.light_sum_blocks[set] = flow_lights_wanted(blocks) ? blocks : 0;
		d->light_blocks = flow.light_sum_blocks[set];
		if (flow.env_backend && params->env_count[0] > 0)
			memcpy(d->env_groups, flow.env_groups, sizeof(d->env_groups));
	}
	else {
		flow_occupancy_dispatch(set, blocks);
		flow_lights_dispatch(set, blocks);
	}
	if (profiled)
		flow_profile(PROFILER_STOP);
#else
	(void)blocks;
	(void)backend_density;
	(void)backend_table;
	(void)backend_format;
#endif
}

void vkpt_flow_dispatches(vkpt_flow_dispatch_t *out)
{
	if (flow.updated_frame == qvk.frame_counter + 1)
		*out = flow.dispatch;
	else
		memset(out, 0, sizeof(*out));
	out->set = qvk.frame_counter & 1;
}
