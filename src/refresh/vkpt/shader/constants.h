/*
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

#ifndef  _CONSTANTS_H_
#define  _CONSTANTS_H_

#define GRAD_DWN (3)

#define SHADOWMAP_SIZE 4096

#define HISTOGRAM_BINS 128

#define EMISSIVE_TRANSFORM_BIAS -0.001

#define MAX_MIRROR_ROUGHNESS 0.02

#define NUM_GLOBAL_TEXTUES 8192

#define NUM_BLUE_NOISE_TEX (128 * 4)
#define BLUE_NOISE_RES     (256)

#define NUM_LIGHT_STATS_BUFFERS 3

#define PRIMARY_RAY_T_MAX 10000

#define MAX_CAMERAS 8

#define MAX_FOG_VOLUMES 8

#define AA_MODE_OFF 0
#define AA_MODE_TAA 1
#define AA_MODE_UPSCALE 2
#define AA_MODE_DLSS 3

// Scaling factors for lighting components when they are stored in textures.
// FP16 and RGBE textures have very limited range, and these factors help bring the signal within that range.
#define STORAGE_SCALE_LF 1024
#define STORAGE_SCALE_HF 32
#define STORAGE_SCALE_SPEC 32
#define STORAGE_SCALE_HDR 128

#define MATERIAL_KIND_MASK           0xf0000000
#define MATERIAL_KIND_INVALID        0x00000000
#define MATERIAL_KIND_REGULAR        0x10000000
#define MATERIAL_KIND_CHROME         0x20000000
#define MATERIAL_KIND_WATER          0x30000000
#define MATERIAL_KIND_LAVA           0x40000000
#define MATERIAL_KIND_SLIME          0x50000000
#define MATERIAL_KIND_GLASS          0x60000000
#define MATERIAL_KIND_SKY            0x70000000
#define MATERIAL_KIND_INVISIBLE      0x80000000
#define MATERIAL_KIND_EXPLOSION      0x90000000
#define MATERIAL_KIND_TRANSPARENT    0xa0000000 // Transparent walls. Have a distortion effect applied.
#define MATERIAL_KIND_SCREEN         0xb0000000
#define MATERIAL_KIND_CAMERA         0xc0000000
#define MATERIAL_KIND_CHROME_MODEL   0xd0000000
#define MATERIAL_KIND_TRANSP_MODEL   0xe0000000 // Transparent models. No distortion, just "see through".
// Blood droplets drawn as sphere geometry (cl_blood_spheres). The LAST free
// value in the 4-bit kind field. Deliberately NOT in IS_REFLECT_REFRACT_KIND:
// blood is a glossy DIELECTRIC, not a mirror, so it wants the ordinary shading
// path at a low roughness, not a second traced ray. get_material() intercepts it
// before any texture lookup - these primitives carry no albedo texture and no
// meaningful UVs, only a per-droplet colour packed into the UV slots.
#define MATERIAL_KIND_BLOOD          0xf0000000

#define MATERIAL_FLAG_LIGHT          0x08000000
// Marks a WATER/SLIME material as a CLOSED SHAPE rather than a flat brush face,
// and sends all of it down the force-field path in primary_rays.rgen and
// reflect_refract.rgen instead of only the part facing sideways.
//
// Those shaders treat a near-vertical water surface as a force field and render
// it as GLASS.  That matters for more than naming: the glass refraction branch
// does `throughput *= primary_base_color`, so a force field SHOWS ITS OWN
// TEXTURE tinting what is behind it, while plain water only refracts and never
// multiplies by the albedo.
//
// The normal test is meaningless on a sphere - it passes through every
// orientation - so without this only the narrow band where |n.z| < 0.1 came out
// glass, and the model wore a hard stripe round its equator where the textured
// band met the untextured caps.  Set from a .mat with `curved_water 1`.
#define MATERIAL_FLAG_CURVED_WATER   0x04000000
// Marks rogue's plasma / heat beam (models/proj/beam, MCLASS_PLAYER_BEAM).
//
// CL_AddPlayerBeams starts the beam AT the muzzle, so its first 32-unit segment
// begins ~7 units from the eye and physically intersects the view weapon. A path
// tracer keeps the nearest hit, so the beam painted a bright cone across the gun.
// The GL renderer never had this problem - it draws RF_DEPTHHACK weapons with a
// crunched depth range, so the gun always wins, which is what the rerelease
// shows. primary_rays.rgen uses this flag to reproduce that for the beam alone.
//
// RECLAIMED 2026-09-10 from MATERIAL_FLAG_HANDEDNESS, whose note here already
// named it "the next bit to reclaim": nothing had read handedness since
// path_tracer_rgen.h started deriving it per triangle from the UV winding, and
// its three dead CPU write sites (bsp_mesh.c x2, main.c) went with this change.
// A flag bit was the only room left - the 4-bit kind field is full, since
// MATERIAL_KIND_BLOOD took its last value.
#define MATERIAL_FLAG_PLAYER_BEAM    0x02000000

// [Q2RTX] IN THE EFFECTS TLAS ONLY, these two bits mean something else.
//
// Explosions and muzzle flashes live in the effects TLAS and never in the
// geometry TLAS; beams and models live in the geometry TLAS and never in the
// effects one. The two sets can therefore never collide, so the bits above are
// reused here rather than spending the last of the material word:
//
//   MATERIAL_FLAG_PLAYER_BEAM  -> this effect is FIRST PERSON ONLY
//   MATERIAL_FLAG_WEAPON       -> this effect is REFLECTION ONLY
//
// pt_logic_explosion drops whichever does not belong to the ray being traced.
#define MATERIAL_FLAG_FX_FIRST_PERSON   MATERIAL_FLAG_PLAYER_BEAM
#define MATERIAL_FLAG_FX_REFLECTION     MATERIAL_FLAG_WEAPON
#define MATERIAL_FLAG_WEAPON         0x01000000
#define MATERIAL_FLAG_WARP           0x00800000
#define MATERIAL_FLAG_FLOWING        0x00400000
#define MATERIAL_FLAG_DOUBLE_SIDED   0x00200000
#define MATERIAL_FLAG_SHELL_RED      0x00100000
#define MATERIAL_FLAG_SHELL_GREEN    0x00080000
#define MATERIAL_FLAG_SHELL_BLUE     0x00040000

// Per-primitive texture flags, stored in VboPrimitive::texture_flags.
//
// These mirror the rerelease's SURF_N64_SCROLL_* surface flags.  They cannot
// live in material_id the way MATERIAL_FLAG_FLOWING does, for two reasons:
// they are a property of the BSP face rather than of the texture (the same
// material scrolls on one face and sits still on another), and material_id has
// exactly one spare bit left while these need three.
#define TEXTURE_FLAG_SCROLL_X        0x00000001
#define TEXTURE_FLAG_SCROLL_Y        0x00000002
#define TEXTURE_FLAG_SCROLL_FLIP     0x00000004

// Doubles the SURF_FLOWING scroll rate on one face. Set only on the fire over
// the MGU drop pod windows (see bsp_mesh.c), which reads as a crawl at 1.6.
#define TEXTURE_FLAG_FAST_FLOW       0x00000008

// Scroll rate in texture widths per second, for the N64 scroll flags above.
// Matches GL_ScrollSpeed() in the rerelease-derived q2repro renderer, which
// uses 1.6 for plain SURF_FLOWING, 0.5 for SURF_FLOWING on a warped surface,
// and this value whenever either N64 scroll axis is set.
#define TEXTURE_N64_SCROLL_SPEED     0.78125

#define MATERIAL_LIGHT_STYLE_MASK    0x0003f000
#define MATERIAL_LIGHT_STYLE_SHIFT   12
#define MATERIAL_INDEX_MASK          0x00000fff

#define CHECKERBOARD_FLAG_PRIMARY    1
#define CHECKERBOARD_FLAG_REFLECTION 2
#define CHECKERBOARD_FLAG_REFRACTION 4

// Combines the PRIMARY, REFLECTION, REFRACTION fields
#define CHECKERBOARD_FLAG_FIELD_MASK 7 
// Not really a checkerboard flag, but it's stored in the same channel.
// Signals that the surface is a first-person weapon.
#define CHECKERBOARD_FLAG_WEAPON     8
// Also not a checkerboard flag. Signals that reflect_refract.rgen replaced this
// pixel's shading surface with the reflected one but deliberately kept the primary
// surface's motion vector and depth (MATERIAL_KIND_CHROME_MODEL). The DLSS-RR guide
// buffers must then describe the mirror itself, not what it reflects.
#define CHECKERBOARD_FLAG_MIRROR_MODEL 16
// Also not a checkerboard flag. The split at this pixel was GLASS, not water or slime.
// pt_dlss_guide_field needs to tell them apart, and the material kind cannot be
// recovered downstream - reflect_refract.rgen has replaced the shading surface by then.
#define CHECKERBOARD_FLAG_GLASS      32
// Also not a checkerboard flag. The path from the camera to this pixel's surface has
// already been folded by something other than a straight-through refraction - a mirror
// reflection, a chrome model, a security camera, total internal reflection. Once that
// has happened, the "apparent position" motion vector in reflect_refract.rgen is no
// longer meaningful: it is a FIRST-ORDER construction that projects the hit position
// straight to the screen, and after a fold the pixel does not see that point there.
// Set at the bounce that folds the path, carried through every later bounce.
#define CHECKERBOARD_FLAG_MIRRORED   64

// Also not a checkerboard flag. A PER-MATERIAL override of pt_dlss_guide_field,
// carried from reflect_refract.rgen (which is the last place the material is
// known) to the combine pass (which is where the guide buffers are chosen).
//
// pt_dlss_guide_field is a single global answer to a question that is really
// per-surface: does this pane of glass read as a MIRROR, whose reflection is
// the thing you look at, or as a WINDOW, which you look through? Mode 3 answers
// "mirror" for all glass, which is right for most of it and wrong for a clear
// window - RR is then told the pixel is the reflected surface while the colour
// is mostly the room behind, and it filters the view through the glass into
// mush. Nothing downstream can tell the two apart, so the material says which.
//
// GUIDE_SET means the material stated a value; the three bits above it hold it,
// using the same 0..5 numbering as the cvar so both feed one branch.
//
// These flags travel in the ALPHA OF AN rgba16f IMAGE (IMG_PT_VIEW_DIRECTION, then
// IMG_ASVGF_COLOR), and half floats represent integers exactly only up to 2048. Every
// bit below now sums to 2047, so the word is full: a further flag needs a different
// home, not bit 11.
#define CHECKERBOARD_FLAG_GUIDE_SET   128
#define CHECKERBOARD_FLAG_GUIDE_SHIFT 8
#define CHECKERBOARD_FLAG_GUIDE_MASK  1792  /* bits 8-10 */

// The value a material stores for the above, as packed into the material table.
// 0 means the material said nothing and pt_dlss_guide_field decides; otherwise
// the guide field is this minus one. Three bits, so it fits beside next_frame.
#define MATERIAL_GUIDE_FIELD_UNSET    0

// pt_fullres_fields - reflection/refraction field layout. See the FIELD LAYOUT note
// in global_ubo.h.
#define PT_FIELDS_CHECKERBOARD    0
#define PT_FIELDS_FULLRES         1
#define PT_FIELDS_HALFRES         2

// In PT_FIELDS_HALFRES, the reflection and refraction layers are traced only on the
// rows whose parity matches this frame, alternating every frame; the combine pass reads
// the untraced rows from their neighbour. Only reflect/refract materials are affected -
// the opaque frame stays at full render resolution.
#define PT_FIELD_ROW_TRACED(ubo, y) \
	((ubo).pt_fullres_fields != PT_FIELDS_HALFRES \
	 || (((y) & 1) == ((ubo).current_frame_idx & 1)))

// The materials reflect_refract.rgen will trace a second path for. Kept in step with
// the early-out at the top of that shader; the combine pass needs the same test and
// cannot include path_tracer_rgen.h.
#define IS_REFLECT_REFRACT_KIND(m) ( \
	   ((m) & MATERIAL_KIND_MASK) == MATERIAL_KIND_WATER \
	|| ((m) & MATERIAL_KIND_MASK) == MATERIAL_KIND_SLIME \
	|| ((m) & MATERIAL_KIND_MASK) == MATERIAL_KIND_GLASS \
	|| ((m) & MATERIAL_KIND_MASK) == MATERIAL_KIND_CHROME \
	|| ((m) & MATERIAL_KIND_MASK) == MATERIAL_KIND_CHROME_MODEL \
	|| ((m) & MATERIAL_KIND_MASK) == MATERIAL_KIND_SCREEN \
	|| ((m) & MATERIAL_KIND_MASK) == MATERIAL_KIND_CAMERA \
	|| ((m) & MATERIAL_KIND_MASK) == MATERIAL_KIND_TRANSPARENT)

#define MEDIUM_NONE  0
#define MEDIUM_WATER 1
#define MEDIUM_SLIME 2
#define MEDIUM_LAVA  3
#define MEDIUM_GLASS 4

#define ENVIRONMENT_NONE 0
#define ENVIRONMENT_STATIC 1
#define ENVIRONMENT_DYNAMIC 2

#define MAX_MODEL_INSTANCES      8192 // MAX_ENTITIES * (some number of geometries per model, usually 1)
#define MAX_RESERVED_INSTANCES   16   // TLAS instances reserved for skinned geometry, particles and the like
#define MAX_TLAS_INSTANCES       (MAX_MODEL_INSTANCES + MAX_RESERVED_INSTANCES)

#define SHADER_MAX_ENTITIES                  8192
#define SHADER_MAX_BSP_ENTITIES              8192
#define MAX_LIGHT_STYLES                     64
#define MAX_MODEL_LIGHTS                     16384

#define TLAS_INDEX_GEOMETRY      0
#define TLAS_INDEX_EFFECTS       1
#define TLAS_COUNT               2

// Geometry TLAS flags
#define AS_FLAG_OPAQUE          (1 << 0)
#define AS_FLAG_TRANSPARENT     (1 << 1)
#define AS_FLAG_VIEWER_MODELS   (1 << 2)
#define AS_FLAG_VIEWER_WEAPON   (1 << 3)
#define AS_FLAG_SKY             (1 << 4)
#define AS_FLAG_CUSTOM_SKY      (1 << 5)
// Blood droplets. They are opaque geometry in every way that matters - they are
// lit, they cast shadows and they show up in reflections - but they get a bit of
// their own so the FOG MARCH can leave them out.
//
// god_rays.comp steps along every pixel's ray and fires a sky-visibility ray, and
// at cl_fog 3 a shadow ray, FROM EVERY STEP. Those queries cull on AS_FLAG_OPAQUE
// (fog_medium.glsl), so with blood in that set a floorful of droplets is
// traversed by tens of thousands of rays per pixel to decide whether a 1.2-unit
// sphere shades the fog - which it does not, in any sense worth paying for.
//
// Anything that should still SEE blood adds this bit; the four ray cull masks in
// path_tracer_rgen.h are the complete list, and every other ray path in the tree
// derives from them.
#define AS_FLAG_BLOOD           (1 << 6)

// Effects TLAS flags
#define AS_FLAG_EFFECTS         (1 << 0)

#define RT_PAYLOAD_GEOMETRY      0
#define RT_PAYLOAD_EFFECTS       1

#define SBT_RGEN                 0
#define SBT_RMISS_EMPTY          1

#define SBT_RCHIT_GEOMETRY       2
#define SBT_RAHIT_MASKED         3

#define SBT_RCHIT_EFFECTS        4
#define SBT_RAHIT_PARTICLE       5
#define SBT_RAHIT_EXPLOSION      6
#define SBT_RAHIT_SPRITE         7
#define SBT_RINT_BEAM            8
#define SBT_ENTRIES_PER_PIPELINE 9
// vkpt_pt_create_pipelines() relies on all 'transparency' SBT entries coming after SBT_FIRST_TRANSPARENCY
#define SBT_FIRST_TRANSPARENCY SBT_RCHIT_EFFECTS

// SBT indices for geometry and shadow rays
#define SBTO_OPAQUE     (SBT_RCHIT_GEOMETRY - SBT_RCHIT_GEOMETRY)
#define SBTO_MASKED     (SBT_RAHIT_MASKED - SBT_RCHIT_GEOMETRY)
// SBT indices for effect rays
#define SBTO_PARTICLE   (SBT_RAHIT_PARTICLE - SBT_RCHIT_EFFECTS)
#define SBTO_EXPLOSION  (SBT_RAHIT_EXPLOSION - SBT_RCHIT_EFFECTS)
#define SBTO_SPRITE     (SBT_RAHIT_SPRITE - SBT_RCHIT_EFFECTS)
#define SBTO_BEAM       (SBT_RINT_BEAM - SBT_RCHIT_EFFECTS)

#ifndef M_PI
#define M_PI 3.1415926535897932384626433832795
#endif

// Dynamic light types
#define DYNLIGHT_POLYGON        0
#define DYNLIGHT_SPHERE         1
#define DYNLIGHT_SPOT           2

//
// Spotlight styles (emission profiles)
//
// spotlight emission profile is smooth falloff between two angle values
#define DYNLIGHT_SPOT_EMISSION_PROFILE_FALLOFF              0
// spotlight emission profile given by an 1D texture, indexed by the cosine of the angle from the axis
#define DYNLIGHT_SPOT_EMISSION_PROFILE_AXIS_ANGLE_TEXTURE   1

//
// Camera projections - the value of the pt_projection cvar, mirrored into
// global_ubo.pt_projection and switched on in projection.glsl.
//
// RECTILINEAR is the only one that goes through the projection MATRIX; every
// other mode ignores P/invP entirely and maps the screen through a closed-form
// warp of the view direction, scaled by global_ubo.projection_fov_scale.
//
// These numbers are the upstream Q2RTX ones and are part of the cvar's public
// interface - do not renumber them.
//
#define PROJECTION_RECTILINEAR      0
#define PROJECTION_PANINI           1
#define PROJECTION_STEREOGRAPHIC    2
#define PROJECTION_CYLINDRICAL      3
#define PROJECTION_EQUIRECTANGULAR  4
#define PROJECTION_MERCATOR         5

// Fraction of the polar angle the stereographic projection carries to the plane.
// 0.5 is the true stereographic map (projecting from the antipode); smaller
// values pull the edges in and flatten the fisheye.
#define STEREOGRAPHIC_ANGLE         0.5

// Panini distance of the projection centre from the cylinder axis.
// 0.0 -> rectilinear, 1.0 -> cylindrical stereographic, +inf -> cylindrical orthographic.
#define PANINI_D                    1.0

#endif /*_CONSTANTS_H_*/
