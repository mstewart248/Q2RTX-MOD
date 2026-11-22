/*
Copyright (C) 2026

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
'MAT AUTHOR': PAINTING A MATERIAL'S MAPS IN PHOTO MODE.

Matt, 2026-11-19: crosshair on a texture, `mat author`, the game pauses (and
accumulates if that is on), a small panel picks the roughness, metallic, normal
or emissive map, a value (normal: a direction and an intensity; emissive: a
brightness over the texture - see CHANGED 11-20 NIGHT), a round or square brush
sized in TEXELS of the map, and the map is
painted straight onto the surfaces with a mouse cursor while the free camera
flies; or onto a flat view of the map, like 'mat print's. A change restarts the
accumulation, as a camera move does. His answers to the four design questions:
hold the RIGHT button to look (WASD/QE fly), the left one paints; the brush is in
texels and the texels it is about to change are outlined; the normal's direction
is SCREEN-RELATIVE (relative to the image in the map view); painting stays in
memory, with undo, until Save - which overwrites a loose PNG/TGA in place, keeping
a .bak, and writes anything else (a pak's, a JPG, a map that did not exist) as a
new PNG under materialDev/, pointing the material's .mat entry at it. That
materialDev/ is the game directory's (rerelease/ for the rerelease's Quake II
textures), or baseq3/'s for a Quake III material (MAT_IsQuake3; Matt, 11-21).

HOW THE PIECES FIT
- `mat author [map]` (material.c hands it here with the crosshair's material):
  photo mode - cl_paused 2, as the pause key does - the console closed.
- The mouse: freecam.c gives it to MAT_AuthorCursorMove unless the right button
  is held (look; Ctrl + right = zoom). Keys and buttons come through
  R_InterceptKey_RTX to MAT_AuthorKey first; Escape leaves (keys.c).
- What is under the cursor: the UBO carries the cursor's render pixel
  (MAT_AuthorFillUBO, author_seq), primary_rays.rgen reads back the UV, the
  material and the tangent frame there (author_readback), and
  MAT_AuthorReadback keeps it as au.hit.
- The outline: primary_rays.rgen marks AUTHOR_MASK where the primary surface's
  texel is inside the brush; tone_mapping_apply.comp draws it over the finished
  picture - after the accumulation, so moving the brush never restarts it.
- Painting changes textures, so it runs BETWEEN frames, where a typed command
  would: MAT_AuthorReadback queues 'mat author _tick' every frame. A dab writes
  image->pix_data (the CPU copy both renderers keep) and IMG_PixelsEdited remakes
  the GPU texture from it, at most every AUTHOR_UPLOAD_MS while painting.
- Undo: one entry per stroke - the rectangle it changed, before and after - kept
  per map: undo and redo only ever touch the map being painted.
- The brush test is the shader's exactly (author_mask_bits): doubled texel
  coordinates, so an even size is centred on a texel corner.

ADDED 2026-11-20 (Matt, after a first go: "this looks and works great")
- LOCKS. F locks painting to the flat face under the cursor, P to the piece -
  "that one metal join in the character's arm". Shift + F/P adds or removes one,
  F/P over the lock (or U) unlocks. primary_rays.rgen reads back the triangle
  under the cursor (its UVs and corners); the CPU finds it in the world
  (bsp_mesh primitives) or the models (alias meshes, frame 0) by those, and grows
  it across shared edges whose normals stay within LOCK_FACE_DEG of it (a face)
  or across any shared corner (a piece). A lock is TEXELS: its triangles
  rasterised into a mask the map's size - a texel whose centre they cover, plus
  two texels of bleed into texels no triangle of the mesh uses, so a seam has no
  unpainted rim. It is uploaded as a texture (author_lock_image): the shader
  darkens what is outside it and edges it in cyan, and the brush there paints
  nothing.
- TOOLS: brush, spray (random texels, faster with density, while held), hatch
  (crossed diagonals every N texels, fixed to the texel grid so strokes line up),
  noise (the value +- an amount, fixed per texel for the stroke), blur (towards a
  box average of the map as the stroke found it), fill (the lock, or a flood of
  neighbouring texels within a tolerance of the clicked one - by the map's
  values or by the base texture's colour).
- FALLOFF: the brush's edge softens over the outer falloff% of its radius. Each
  texel gets the stroke's highest weight w and becomes mix(before, paint, w),
  where before is the texel as the stroke found it: a soft stroke blends into
  what is there, never wipes it, and a second stroke builds on the first.
- THE MAP VIEW shows the base texture under the map, the map as a layer of
  adjustable opacity over it (O, or the panel), and the lock darkened round.
- UNDER THE BRUSH: the panel shows the average of the texels the brush covers
  (inside the lock), e.g. 0% and 100% under it read 50%.

ADDED 2026-11-20 AFTERNOON (Matt: "different brush types such as lines and
rectangles (on the rectangles we should be able to specify length texels and
width texels)", and "for the Quake3 textures with multiple stages can we have a
way to select which stage to edit")
- THE RECTANGLE: a third brush shape beside round and square, its LENGTH
  along the map's u and its WIDTH along v, each in texels. (Replaced 11-21 by
  the rectangle stroke, dragged corner to corner - see CHANGED 2026-11-21.)
- THE LINE (L, or the panel's "line"): the button goes down where the line
  starts and comes up where it ends; until then the brush is shown swept along
  it, and on release it paints that, once, as one stroke with every tool but
  fill. The swept brush is exact - sweep_row is the texels of a row it covers,
  primary_rays.rgen's author_mask_bits the same test texel by texel.
- STAGES: a Quake III shader drawn stage by stage (q3stages.c) gives each stage
  a surface of its own where it shows - stage<N>_texture_roughness, _metallic,
  _normals. The panel's "maps" row picks the material's own maps or one
  stage's; a stage's map is read through that stage's tcMods (scale, scroll,
  rotate - a turb left out), so the brush is too, on the surface and in the
  lock. A stage has no emissive map (its glow is stage<N>_emissive_factor), and
  the $lightmap and reflection maps have no surface to paint. A new stage map
  is the size of the stage's picture, named after it, and set as the console's
  'mat stage<N> texture_roughness' would - Save writes it into the .mat.

ADDED 2026-11-20 EVENING (Matt: "Can we add a height map painter to the texture
authoring tool too please")
- HEIGHT, the fifth map (5, 'mat author height'): the POM map, texture_height.
  Painted like roughness, one grey value 0..100%: white the top, black
  displace_in below it (material.h), so with displace_out 0 - the default -
  white is the surface and painting darker carves into it. A new map starts
  flat at the surface (height_surface_level), named <base>_height.png. The
  image is flagged IF_HEIGHT_MAP (material.c, for a texture_height set from the
  console too), so every upload rebuilds the max-filtered mips pt_pom 1 walks.
  The brush reads back the flat surface's UV, before POM moves it, so painting
  does not chase the relief it makes. No Quake III stage has one of its own.

CHANGED 2026-11-20 NIGHT (Matt: "it looks like it's selecting everything", and
"The emissive editor should just be the albedo texture and you set brightness
values ... paint on 0 - 200. 100 being as bright as the albedo. We should have
sliders for the rgb channels on the albedo map")
- STAND-INS ARE NOT PAINTED (map_is_placeholder). id1/materials/quake1.mat gives
  259 Quake 1 materials one metallic map, materialDev/remix/solid*M1.wal (made in
  memory), and a few share 1x1 solid-colour emissive PNGs. Painting such a map
  marked every material using it, a 1x1 map is inside any brush - the whole
  screen orange - and Save would have pointed all of them at the painted file.
  A map made in memory ('*' in its name), one under 8 texels across under a
  bigger base, or the base texture itself now counts as no map: the first stroke
  makes the material its own, the base's size, starting from the stand-in's
  values. A real shared map is still painted as one: colour variants share PBR
  maps on purpose.
- EMISSIVE IS THE TEXTURE, LIT. The emissive map paints one value, a
  BRIGHTNESS 0..200% (100% = the texture's own colours, as bright as it is),
  kept in the map's alpha; its colours are the albedo's (linear) times the
  brightness times a TINT - the panel's red, green and blue, how much of each
  of the texture's channels glows, one for the whole map - stored at HALF so 200%
  of white fits a byte (emit_bake). The shader and the light measurement read
  only the colours, so nothing else changed; the material's emissive_factor is
  doubled once, when the map is made, to make up the half. Such a map is named
  <base>_emit.png (emit_painted). Any other glow - none, a _light, one made from
  the base's bright texels, a shared solid colour - is converted by the first
  stroke: brightness = its luminance over the albedo's, texel by texel (none:
  0%), so what glowed still glows as bright, in the texture's colours. The tint
  is read back from a painted map's colours, and changing it makes them again.
  Fill floods by the texture's colour on this map by default (a 0% map is all
  alike by value): click the bulb.

CHANGED 2026-11-21 (Matt: "undo redo on the selected texture and not go back to
previous selected textures", "instead of the square and rectangle brushes can I
just drag a rectangle like a line", "a border radius on the rectangles", "can I
directly key in the numbers ... or make the sliders less sensitive")
- UNDO IS PER MAP. Each step remembers the map it changed and whether it is
  applied; Ctrl+Z takes back the last applied step of the map being painted
  (author_image), Ctrl+Y puts back its first undone one, and a new stroke drops
  only that map's undone steps. Other maps' history waits for them.
- THE BRUSH IS ROUND. The square and rectangle brush shapes are gone; a
  rectangle is a STROKE now, beside free and line (R, or the panel): press at
  one corner, let go at the other, and the texels between are painted once,
  every tool but fill, its corners rounded by a radius in texels (the panel, the
  wheel while in that stroke, 'mat author rect [radius]'). The falloff softens
  the outer part of its half-width. primary_rays.rgen's author_rect is the same
  integer test as rect_inside.
- NUMBERS. Every slider has a box with its number: click it, type, Enter (Esc
  leaves it as it was; a click elsewhere takes it). Values are whole units
  (percent, texels, degrees); a slider snaps to them, and while one is dragged
  the cursor moves at mat_author_slider_speed (0.25) of its speed - Shift a
  quarter of that again. The wheel or the arrow keys over a slider step it by
  one (Shift: ten).
- THE EMISSIVE MAP VIEW (Matt: "I can't see what has been marked as emissive")
  is the texture as bright as it glows - 50% half as bright, 200% twice - with
  a purple veil over what glows less, the panel's slider strong at 0% (with
  "base under"; O shows the map alone). Before the first stroke it shows the
  glow the material already has, read as that stroke will convert it. The
  caption gives the brightness under the cursor (glow_view, DRAW_GLOW_*).

ADDED 2026-11-21 EVENING (Matt: "can we add selections, for example rectangle
select, ellipse select and lasso select. For the selection I should be able to
type in a bottom and top range of values and have it set the roughness and metal
map for that range of values based on the pixel brightness of the albedo ...
select an area on a texture and say this selection should be 30 to 50%
metallic"; and "the default roughness should be 1 and the default metalness 0")
- SELECTIONS (M cycles rect, ellipse, lasso, painting; or the panel's 'select'
  row): the left button drags one out on the surface or the map view instead
  of painting - Shift + drag adds to it, Ctrl + drag takes away, a click that
  drags nothing drops it. Ctrl+A all, Ctrl+D none, Ctrl+Shift+D the last one
  dropped back, Ctrl+I invert. A selection is the steps that made it (sl), in
  the map's own coordinates, so one lies on every map of the material whatever
  its size - each map's texels are made from it as it is painted
  (sel_rasterize): a rectangle the texels corner to corner inclusive (as the
  rectangle stroke), an ellipse those whose centres are inside the one
  inscribed in that box, a lasso those it encloses. Another material or stage
  drops it.
- THE REGION. Painting, every tool, is held inside the lock AND the selection,
  of those that are on (region_allows), and they are one texture for the shader
  and the map view (rg - what was the lock's alone): the shader is unchanged, so
  a selection shows as the lock did, darkened outside with a cyan edge, and a
  shape being dragged out grows on the surface a few times a second. The map
  view adds marching ants along the shapes. Fill fills the whole region.
- THE BRIGHTNESS RANGE (the panel's dark / bright and apply, or 'mat author
  range <dark%> <bright%>'): the map being painted set over the region (with
  none, the whole map) from the base texture's brightness - its luminance,
  sRGB-encoded, box-filtered under a map texel - stretched over the region's own
  spread, its 1st to 99th percentile: the darkest gets 'dark', the brightest
  'bright', a straight line between. 30 / 50 on the metallic map is "30 to 50%
  metallic"; bright below dark gives brighter texels less. Roughness, metallic,
  height, or emissive's brightness (0..200%); not normals. One undo step.
- DEFAULTS: the value painted starts at roughness 100%, metallic 0% - a
  texture with no maps - not 50% and 100%.
*/

#include "vkpt.h"
#include "material.h"
#include "q3stages.h"
#include "../../client/client.h"
#include "../stb/stb_image_write.h"

#include <stdio.h>
#include <limits.h>
#include <float.h>

// stb_image_write's own deflate (stb.c builds it): for the 16-bit PNG writer below
extern unsigned char *stbi_zlib_compress(unsigned char *data, int data_len, int *out_len, int quality);

static inline int Q_clip(int a, int lo, int hi)
{
	return a < lo ? lo : a > hi ? hi : a;
}

static inline float Q_clipf(float a, float lo, float hi)
{
	return a < lo ? lo : a > hi ? hi : a;
}

// the stage-capable slots come first: a Quake III stage paints SLOT_NORMAL and below.
// SLOT_GLASS, last, is stained glass's mask (texture_stained_mask): which texels
// are the glass, 1 or 0 - offered only for a stained_glass material (slot_offered).
enum { SLOT_ROUGH, SLOT_METAL, SLOT_NORMAL, SLOT_EMISSIVE, SLOT_HEIGHT, SLOT_GLASS, NUM_SLOTS };

static const char *const slot_label[NUM_SLOTS] = { "rough", "metal", "normal", "emissive", "height", "glass" };
static const char *const slot_word[NUM_SLOTS] = { "roughness", "metallic", "normals", "emissive", "height", "glass" };
static const char *const slot_attribute[NUM_SLOTS] = { "texture_roughness", "texture_metallic", "texture_normals", "texture_emissive", "texture_height", "texture_stained_mask" };
static const char *const slot_suffix[NUM_SLOTS] = { "_rough", "_metal", "_n", "_emit", "_height", "_glassmask" };

// one grey value per texel: roughness, metallic, height, the glass mask
static inline bool slot_is_grey(int slot)
{
	return slot == SLOT_ROUGH || slot == SLOT_METAL || slot == SLOT_HEIGHT || slot == SLOT_GLASS;
}

// one value painted per texel: the grey maps, and emissive's brightness (its alpha)
static inline bool slot_is_scalar(int slot)
{
	return slot_is_grey(slot) || slot == SLOT_EMISSIVE;
}

enum { TOOL_BRUSH, TOOL_SPRAY, TOOL_HATCH, TOOL_NOISE, TOOL_BLUR, TOOL_FILL, NUM_TOOLS };

static const char *const tool_label[NUM_TOOLS] = { "brush", "spray", "hatch", "noise", "blur", "fill" };

// a stroke: wherever the cursor goes; a line from press to release; a rectangle with that line its diagonal
enum { STROKE_FREE, STROKE_LINE, STROKE_RECT, NUM_STROKES };

static const char *const stroke_label[NUM_STROKES] = { "free", "line", "rect" };

// a selection's shape (M, the panel's 'select' row): what the left button drags out instead of painting
enum { SEL_OFF, SEL_RECT, SEL_ELLIPSE, SEL_LASSO, NUM_SEL };

static const char *const sel_label[NUM_SEL] = { "off", "rect", "ellipse", "lasso" };

// a selection is the steps that made it, replayed onto each map's texels (sel_rasterize)
enum { SELSTEP_ADD, SELSTEP_SUB, SELSTEP_ALL, SELSTEP_INVERT };

#define SEL_MAX_POINTS      32768       // a lasso's
#define SEL_MAX_TILES       8           // a shape on a tiled surface: no wider than this many maps (as a line)
#define SEL_PREVIEW_MS      50          // a shape being dragged out is shown this often

#define AUTHOR_MAX_STAGES   8           // Q3P_MAX_STAGES: the panel's stage buttons

#define LOCK_FACE_DEG       12.f        // a flat face: triangles within this of the one picked
#define LOCK_WELD           (1.f / 64.f) // corners this close are one (model or world units)
#define LOCK_BLEED          2           // texels a lock reaches past a seam, into texels no triangle uses

#define AUTHOR_MAX_BRUSH     512
#define AUTHOR_MAX_RADIUS    256        // a rectangle's corners, texels (doubled, its square fits an int)
#define AUTHOR_UPLOAD_MS     33
#define AUTHOR_MAX_TILT_DEG  70.f
#define AUTHOR_UNDO_MAX      128
#define AUTHOR_UNDO_BYTES    ((size_t)512 << 20)
#define AUTHOR_SEQ_RING      16
#define AUTHOR_MAX_MAP       4096
#define AUTHOR_PANEL_W       300

// the panel's controls
enum {
	W_NONE,
	W_PANEL,
	W_SLOT0, W_SLOT1, W_SLOT2, W_SLOT3, W_SLOT4, W_SLOT5,
	W_VALUE,
	W_DIAL,
	W_INTENSITY,
	W_RED, W_GREEN, W_BLUE,     // emissive's tint: consecutive (drag_update, MAT_AuthorKey)
	W_SIZE,
	W_SHOW2D,
	W_UNDO, W_REDO, W_SAVE, W_EXIT,
	W_FIX,
	W_VIEW2D,
	W_TOOL0, W_TOOL1, W_TOOL2, W_TOOL3, W_TOOL4, W_TOOL5,
	W_FALLOFF,
	W_TOOLOPT,
	W_FILL_MAP, W_FILL_COLOUR,
	W_UNLOCK,
	W_SHOWBASE,
	W_LAYER,
	W_RADIUS,                       // the rectangle's corner radius
	W_RANGE_LO, W_RANGE_HI,         // the brightness range: the darkest albedo's value, the brightest's (consecutive)
	W_RANGE_APPLY,
	W_SEL_ALL, W_SEL_INVERT, W_SEL_NONE,
	W_CAM_HOME,                     // the camera back where it started (H)
	W_LIGHT_ON,                     // the flashlight at the camera, on or off (C)
	W_LIGHT,                        // ... and its brightness
	W_GLASS_PAINT, W_WALL_PAINT,    // the glass mask: paint glass, or the wall round it (X swaps)
	W_SEL0,                        // ... W_SEL0 + SEL_*: the selection's shapes
	W_STROKE0 = W_SEL0 + 4,         // ... W_STROKE0 + STROKE_*
	W_STAGE_MAT = W_STROKE0 + NUM_STROKES,  // the material's own maps
	W_STAGE0,                       // ... W_STAGE0 + N: stage N's
	W_LAST = W_STAGE0 + AUTHOR_MAX_STAGES,
	W_NUM0 = W_LAST,                // ... W_NUM0 + a slider's id: the box with its number (field_*)
	W_NUM_END = W_NUM0 + W_LAST
};

typedef struct {
	int id;
	float x, y, w, h;
} widget_t;

typedef struct {
	bool valid;         // the cursor was on a surface, not the sky or off the view
	int material;       // its material index
	float u, v;         // the mesh UV, unwrapped
	float tx[2], ty[2]; // one render pixel right / down, in its tangent frame (x tangent, y bitangent)
	float duv[4];       // du/dx, dv/dx, du/dy, dv/dy per render pixel
	float tri_uv[6];    // the triangle's UVs, as its mesh holds them
	float tri_pos[9];   // ... and its corners where they are drawn
	uint32_t seq;
} author_hit_t;

typedef struct {
	int image;          // r_images index
	char name[MAX_IMAGE_PATH];
	int slot;           // the map it was painted as
	int stage;          // ... of this Quake III stage, -1 the material's own
	int width, height, bpp;
	int x, y, w, h;
	byte *before;
	byte *after;
	size_t bytes;       // of before and after together
	bool applied;       // the map holds (after); undone, it holds (before) - per map, a run of applied then undone
} author_undo_t;

/* What a dab paints, in the map's own terms (texel_get / texel_put): roughness or
   metallic in v[0] (linear 0..1, whatever the file stores); a normal's x, y
   (-1..1) and z; an emission's r, g, b. */
typedef struct {
	float v[3];
} author_paint_t;

// a dab under way
typedef struct {
	image_t *img;
	int slot;
	bool wrap;          // on a surface: the map tiles
	author_paint_t paint;
	float spray_p;      // the chance a texel gets sprayed by this dab
	int blur_r;         // the blur's reach, texels; 0 = from the brush's size
	bool changed;
} dab_ctx_t;

static struct {
	bool defaults_set;
	bool active;
	bool paused_by_us;
	int pause_frames;

	pbr_material_t *mat;
	char mat_name[MAX_QPATH];
	int slot;

	float value[NUM_SLOTS];     // roughness, metallic, height: 0..1; emissive: brightness 0..1 = 0..200%
	float normal_angle;         // the tilt's direction on screen, radians, y down (-pi/2 = up)
	float normal_intensity;     // 0..1 of AUTHOR_MAX_TILT_DEG
	float emit_tint[3];         // emissive: the albedo's red, green, blue as it glows, 0..1 - the whole map's
	float emit_baked[3];        // ... what the map's colours were last made with (a change re-makes them)
	int emit_mat, emit_image;   // ... whose painted map emit_tint was read back from (-1: none)
	int brush_size;             // texels across (round)
	int stroke;                 // STROKE_*: L a line, R a rectangle - from where the button goes down to where it comes up
	int rect_radius;            // STROKE_RECT: its corners' radius, texels
	bool line_active;           // a line or rectangle: the button is down on one
	bool line_have;             // ... and its start is set
	bool line_commit;           // ... released: painted at the next tick
	author_hit_t line_hit;      // ... what was under its start (a normal's tilt is turned by its tangent frame)
	float line_tu0, line_tv0;   // ... its start (a rectangle's first corner), texels (unwrapped on a surface)
	float line_tu1, line_tv1;   // ... its end so far

	int edit_id;                // the number box being typed into (a slider's id), W_NONE none
	char edit_buf[16];          // ... what has been typed
	bool edit_fresh;            // ... still the value it opened with: a key replaces it

	int stage;                  // -1: the material's own maps; else Quake III stage N's own (stage<N>_texture_*)
	float stage_tc[6];          // ... its tcMods as the last frame drew them (vkpt_q3_stage_paint_info)
	bool stage_clamp;           // ... clampMap: the map does not tile
	int tool;                   // TOOL_*
	int prev_tool;              // G goes back to it
	float falloff;              // 0 hard .. 1 soft from the centre out
	float tool_opt[NUM_TOOLS];  // spray density, hatch spacing (0..1), noise amount, blur strength, fill tolerance
	bool fill_by_colour[NUM_SLOTS]; // fill floods by the base texture's colour, not the map's values (per map)

	int select;                 // SEL_*: the shape the left button drags out instead of painting, SEL_OFF paints
	float range[NUM_SLOTS][2];  // the brightness range: the value the darkest albedo gets, the brightest's (value[]'s units)
	bool range_request;         // the panel's 'apply': done at the next tick

	bool show_2d;
	bool show_base;             // the map view: the base texture under the map
	float layer_alpha;          // ... and the map over it this opaque
	float glow_veil;            // ... the emissive map's instead (glow_view): the purple over what doesn't glow
	float cx, cy;               // the cursor, real screen pixels

	// the map view: virtual px per texel, the texel at its centre, what it was fitted to
	float v2_ppt;
	float v2_cu, v2_cv;
	int v2_fit_image;
	int v2_fit_w, v2_fit_h;

	int drag;                   // the panel control the left button is holding
	bool right_pan;             // the right button went down on the map view: it pans
	bool painting;              // the left button is down painting
	bool paint_2d;              // ... in the map view
	bool create_tried;          // this stroke already asked for a new map

	// the stroke under way
	int stroke_image;           // r_images index, -1 none
	int stroke_slot;
	int stroke_stage;
	int stroke_w, stroke_h, stroke_bpp;
	byte *stroke_before;        // the whole map as it was
	byte *stroke_weight;        // per texel, the most this stroke has painted it (0..255)
	uint32_t stroke_seed;       // the noise tool's pattern for this stroke
	int bb_x0, bb_y0, bb_x1, bb_y1;
	bool have_last;
	float last_tu, last_tv;     // the last dab's centre in texels (unwrapped on a surface)
	bool fill_done;             // this click's fill is done
	unsigned spray_ms;          // the last spray, for its rate
	uint32_t rng;

	int lock_request;           // 'f' / 'p' pressed: made at the next tick
	bool lock_toggle;           // ... with Shift: add it or take it away

	// the readback
	uint32_t seq;
	bool seq_on_view[AUTHOR_SEQ_RING];
	author_hit_t hit;

	bool tick_queued;
	int upload_image;           // r_images index waiting for IMG_PixelsEdited, -1 none
	unsigned last_upload_ms;
	bool attr_changed;          // a map was made or repointed: its .mat entry wants saving
	bool tangent_warned;

	char status[256];
	unsigned status_ms;
} au;

// every map's steps, oldest first; undo and redo pick the map being painted's own (undo_match)
static author_undo_t undo_stack[AUTHOR_UNDO_MAX];
static int undo_count;          // entries held
static size_t undo_bytes;

// painted and not saved: the slot it was painted as, + 1; and the Quake III
// stage whose own map it is, + 1 (0 = the material's)
static uint8_t image_dirty[MAX_RIMAGES];
static uint8_t image_stage[MAX_RIMAGES];

static widget_t widgets[128];
static int num_widgets;

static cvar_t *mat_author_cursor_speed;
static cvar_t *mat_author_slider_speed;

// the flashlight at the camera (MAT_AuthorCameraLight); registered on first use (light_cvars)
static cvar_t *mat_author_light;            // 1 = on: the panel's "flashlight", C, 'mat author light'
static cvar_t *mat_author_light_scale;      // its brightness, 1 = the game's own flashlight
static cvar_t *mat_author_light_cone;       // its cone, in degrees across

static void light_cvars(void)
{
	if (mat_author_light)
		return;
	mat_author_light = Cvar_Get("mat_author_light", "0", CVAR_ARCHIVE);
	mat_author_light_scale = Cvar_Get("mat_author_light_scale", "1", CVAR_ARCHIVE);
	mat_author_light_cone = Cvar_Get("mat_author_light_cone", "90", CVAR_ARCHIVE);
}

// a locked triangle, by its UVs (normalised; the map's tiling wraps them)
typedef struct {
	float uv[6];
} lock_tri_t;

/* THE LOCK (F, P): the triangles it was made of, and what they cover of the
   map being painted, a texel at a time - remade when the map's size changes. */
static struct {
	bool on;
	int kind;                   // 'f' face, 'p' piece: the last one added
	int material;               // the material index it was made on
	lock_tri_t *tris;
	int num_tris, cap_tris;
	lock_tri_t *cover;          // every triangle of the model meshes it came from: where their texels are used
	int num_cover, cap_cover;
	bool dirty;                 // the triangles changed: rasterise again

	byte *mask;                 // 1 = inside, mask_w x mask_h
	int mask_w, mask_h;
	int texels;

	const void *cover_meshes[64]; // the model meshes in cover
	int num_cover_meshes;
} lk;

/* THE SELECTION (M; Matt, 11-21): rectangles, ellipses and lassos dragged out on
   the surface or the map view, added (Shift) or taken away (Ctrl), kept as the
   steps that made it in the map's own coordinates - repeats of it, unwrapped on
   a tiled surface - so the same selection lies on a map of any size: each one's
   texels are made from it when it is painted (sel_rasterize). */
typedef struct {
	int step;                   // SELSTEP_*
	int shape;                  // ... SEL_RECT, _ELLIPSE, _LASSO for ADD and SUB
	int first, count;           // ... its points in sl.pts (a rectangle or ellipse: two opposite corners)
	bool clip;                  // drawn in the map view: clipped to the map, never wrapped round it
} sel_step_t;

static struct {
	sel_step_t *steps;
	int num_steps, cap_steps;
	float *pts;                 // u, v pairs: 1 = the map across, unwrapped
	int num_pts, cap_pts;
	int material;               // the material index it was made on
	int stage;                  // ... and its stage: another stage's map lies differently

	// the last one dropped (Ctrl+D, a click, a new one), for Ctrl+Shift+D
	sel_step_t *prev_steps;
	int prev_num_steps;
	float *prev_pts;
	int prev_num_pts;
	int prev_material, prev_stage;

	// the shape being dragged out
	bool dragging;
	bool drag_2d;               // ... on the map view
	bool drag_commit;           // ... let go of: made at the next tick
	bool drag_replace;          // ... neither Shift nor Ctrl: it replaces the selection
	bool drag_sub;              // ... Ctrl: taken away
	int drag_shape;             // ... SEL_*
	float *dpts;                // ... its points, as pts
	int num_dpts, cap_dpts;

	bool dirty;                 // the steps changed: rasterise again
	byte *mask;                 // 1 = selected, mask_w x mask_h
	int mask_w, mask_h;
	int texels;
	unsigned raster_ms;         // the last time a shape being dragged out was rasterised

	// the panel's brightness spread under it (range_stats), and what that was measured for
	bool stats_have;
	float stats_lo, stats_hi;
	int stats_n;                // texels read; -1 the region isn't ready, -2 no base colours in memory
	int stats_version, stats_w, stats_h, stats_base;
} sl;

/* THE REGION painting is held to: inside the lock and inside the selection, of
   those that are on. One texture the map's size, for the shader (dark outside
   it, a cyan edge) and the map view: inside = red 1 and clear, outside = black
   at 150 alpha. */
static struct {
	qhandle_t image;            // global_ubo.author_lock_image
	char image_name[32];
	int image_seq;
	bool upload_failed;
	int w, h;                   // the size it was made for
	int texels;
	bool dirty;                 // a lock or a selection came or went: upload it again
	int version;                // counts its uploads: what was measured from it is stale when this moves
} rg;

static bool sel_on(void)
{
	return sl.num_steps > 0 || sl.dragging;
}

static bool region_on(void)
{
	return lk.on || sel_on();
}

/* ------------------------------------------------------------------------- */
/* SMALL THINGS                                                              */
/* ------------------------------------------------------------------------- */

static void author_status(const char *fmt, ...) q_printf(1, 2);

static void author_status(const char *fmt, ...)
{
	va_list argptr;
	va_start(argptr, fmt);
	Q_vsnprintf(au.status, sizeof(au.status), fmt, argptr);
	va_end(argptr);
	au.status_ms = Sys_Milliseconds();
	Com_Printf("mat author: %s\n", au.status);
}

static image_t *slot_image(const pbr_material_t *mat, int slot)
{
	if (!mat)
		return NULL;
	switch (slot)
	{
	case SLOT_ROUGH:    return mat->image_roughness;
	case SLOT_METAL:    return mat->image_metallic;
	case SLOT_NORMAL:   return mat->image_normals;
	case SLOT_EMISSIVE: return mat->image_emissive;
	case SLOT_HEIGHT:   return mat->image_height;
	case SLOT_GLASS:    return mat->image_stained_mask;
	default:            return NULL;
	}
}

/* Is (slot) one (mat) can be painted in? The glass mask only for stained glass
   (Matt, 11-22: "This mask option should only show up if the texture is marked
   as stained glass") - and never a Quake III stage's, which has no mask. */
static bool slot_offered(const pbr_material_t *mat, int slot)
{
	if (slot != SLOT_GLASS)
		return true;
	return mat && mat->stained_glass && au.stage < 0;
}

static int image_index(const image_t *img)
{
	return img ? (int)(img - r_images) : 0;
}

static bool material_ok(void)
{
	return au.mat && au.mat->registration_sequence && !strcmp(au.mat->name, au.mat_name);
}

/* The Quake III stage being painted (au.stage), as the last frame drew it: its
   tcMods into au.stage_tc / au.stage_clamp, its picture into (picture). False,
   with (why), when it is not one whose maps can be painted. */
static bool stage_info(const image_t **picture, const char **why)
{
	const char *dummy;
	if (!why)
		why = &dummy;
	if (au.stage < 0 || !material_ok())
	{
		*why = "no stage";
		return false;
	}
	return vkpt_q3_stage_paint_info(au.mat, au.stage, picture, &au.stage_clamp, au.stage_tc, why);
}

/* An emissive map this tool painted: <base>_emit[_N].png, its alpha the
   brightness, its colours the albedo's times it (texel_put). Any other emissive
   map - none, a _light, one made from the base's bright texels, a solid colour
   several materials share - is not painted on: the first stroke makes the
   material its own, the brightness read from the old one (create_emit_map). */
static bool emit_painted(const image_t *img)
{
	if (!img)
		return false;
	char stem[MAX_QPATH];
	COM_StripExtension(stem, COM_SkipPath(img->filepath[0] ? img->filepath : img->name), sizeof(stem));
	for (const char *p = strstr(stem, "_emit"); p; p = strstr(p + 1, "_emit"))
	{
		const char *rest = p + 5;
		if (!*rest)
			return true;
		if (*rest == '_' && rest[1])
		{
			const char *d = rest + 1;
			while (*d >= '0' && *d <= '9')
				d++;
			if (!*d)
				return true;
		}
	}
	return false;
}

/* A map that stands in for one rather than being one: made in memory (a '*' name -
   a solid like materialDev/remix/solid*M1.wal, which 259 Quake 1 materials share),
   a few texels across under a bigger base (a 1x1 solid-colour PNG), or the base
   texture itself. Painted, it would mark - and on Save repoint - every material
   using it, and a 1x1 one is all inside any brush: "it's selecting everything"
   (Matt, 11-20). So it counts as no map: the first stroke makes the material its
   own, the base's size, starting from the stand-in's values (create_map). */
static bool map_is_placeholder(const pbr_material_t *mat, const image_t *img)
{
	if (!img || !mat)
		return false;
	if (img == mat->image_base || strchr(img->name, '*'))
		return true;
	const image_t *base = mat->image_base;
	return (img->upload_width < 8 || img->upload_height < 8)
		&& base && base->upload_width >= 8 && base->upload_height >= 8;
}

static image_t *author_image(void)
{
	if (!material_ok())
		return NULL;
	if (au.stage >= 0)
	{
		const char *file;
		if (au.slot > SLOT_NORMAL || !stage_info(NULL, NULL))
			return NULL;
		return (image_t *)vkpt_q3_stage_image(au.mat, au.stage, au.slot + 1, &file);
	}
	image_t *img = slot_image(au.mat, au.slot);
	if (au.slot == SLOT_EMISSIVE)
		return emit_painted(img) ? img : NULL;
	return map_is_placeholder(au.mat, img) ? NULL : img;
}

// the picture the map lies over: a stage's own, or the material's base texture
static const image_t *author_base(void)
{
	if (!material_ok())
		return NULL;
	if (au.stage >= 0)
	{
		const image_t *pic = NULL;
		if (stage_info(&pic, NULL) && pic)
			return pic;
	}
	return au.mat->image_base;
}

/* The emissive map's view with the base under it (draw_view2d): the texture as
   bright as it glows - 50% half as bright, 200% twice - and a purple veil over
   what glows less, au.glow_veil strong at 0% (Matt, 11-21: "I can't see what has
   been marked as emissive"; the map itself is the albedo times the brightness at
   half, near black for most of a Quake texture). */
static bool glow_view(void)
{
	return au.slot == SLOT_EMISSIVE && au.stage < 0 && au.show_base;
}

// the map tiles over the surface: not a clampMap stage's
static bool map_wraps(void)
{
	return !(au.stage >= 0 && au.stage_clamp);
}

// surface coordinates (s, t) on the map being painted: a stage's through its tcMods
static void map_uv(float s, float t, float *u, float *v)
{
	if (au.stage < 0)
	{
		*u = s;
		*v = t;
		return;
	}
	const float *m = au.stage_tc;
	*u = m[0] * s + m[1] * t + m[4];
	*v = m[2] * s + m[3] * t + m[5];
	if (au.stage_clamp)
	{
		*u = Q_clipf(*u, 0.f, 0.99999f);
		*v = Q_clipf(*v, 0.f, 0.99999f);
	}
}

// where (hit) is on the map, in repeats of it (1 = one), unwrapped
static void hit_map_uv(const author_hit_t *hit, float *u, float *v)
{
	map_uv(hit->u, hit->v, u, v);
}

// ... in texels of a W x H map: unwrapped, or within the one copy
static void hit_texel(const author_hit_t *hit, int W, int H, bool wrapped, float *tu, float *tv)
{
	float u, v;
	hit_map_uv(hit, &u, &v);
	if (wrapped)
	{
		u -= floorf(u);
		v -= floorf(v);
	}
	*tu = u * (float)W;
	*tv = v * (float)H;
}

static int image_bpp(const image_t *img)
{
	return img->pixel_format == PF_R16_UNORM ? 2 : 4;
}

static bool image_paintable(const image_t *img, const char **why)
{
	if (!img->pix_data || img->upload_width <= 0 || img->upload_height <= 0)
	{
		*why = "its pixels are not in memory";
		return false;
	}
	if (img->pixel_format != PF_R8G8B8A8_UNORM && img->pixel_format != PF_R16_UNORM)
	{
		*why = "its pixel format can't be painted";
		return false;
	}
	if (img->pixel_format == PF_R16_UNORM && (au.slot == SLOT_NORMAL || au.slot == SLOT_EMISSIVE))
	{
		*why = "it is a one-channel 16-bit image";
		return false;
	}
	return true;
}

// the painted map's size, or the size a new one would get (the base texture's)
static void map_size(int *w, int *h)
{
	const image_t *img = author_image();
	if (!img && material_ok())
		img = author_base();
	if (img && img->upload_width > 0 && img->upload_height > 0)
	{
		*w = min(img->upload_width, AUTHOR_MAX_MAP);
		*h = min(img->upload_height, AUTHOR_MAX_MAP);
		if (author_image())
		{
			*w = img->upload_width;
			*h = img->upload_height;
		}
		return;
	}
	*w = *h = 256;
}

static float ui_scale(void)
{
	// a virtual screen about 720 lines tall, as 'mat print's preview uses
	return 1.f / (float)max(1, (int)((float)r_config.height / 720.f + 0.5f));
}

static void cursor_virtual(float *x, float *y)
{
	const float s = ui_scale();
	*x = au.cx * s;
	*y = au.cy * s;
}

static int count_dirty(void)
{
	int n = 0;
	for (int i = 0; i < MAX_RIMAGES; i++)
		if (image_dirty[i])
			n++;
	return n;
}

static void queue_tick(void)
{
	if (au.tick_queued)
		return;
	au.tick_queued = true;
	Cbuf_AddText(&cmd_buffer, "mat author _tick\n");
}

static float linear_to_srgb(float v)
{
	v = Q_clipf(v, 0.f, 1.f);
	return v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.f / 2.4f) - 0.055f;
}

static byte to_byte(float v)
{
	return (byte)Q_clip((int)(Q_clipf(v, 0.f, 1.f) * 255.f + 0.5f), 0, 255);
}

static float srgb_to_linear(float v)
{
	v = Q_clipf(v, 0.f, 1.f);
	return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
}

static uint32_t hash32(uint32_t x)
{
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

// -1..1, the same for (x, y, seed) every time
static float hash_signed(int x, int y, uint32_t seed)
{
	const uint32_t h = hash32((uint32_t)x * 0x9e3779b1u ^ hash32((uint32_t)y + seed));
	return (float)(h & 0xffffff) / 8388607.5f - 1.f;
}

static float rand01(void)
{
	au.rng = au.rng * 1664525u + 1013904223u;
	return (float)(hash32(au.rng) & 0xffffff) / 16777216.f;
}

static int hatch_spacing(void)
{
	return 2 + (int)(au.tool_opt[TOOL_HATCH] * 30.f + 0.5f);
}

/* The brush, texels across: round. The fill tool works from the one texel
   clicked, a rectangle from the one its corner goes down on, and a selection
   from the one under the cursor. */
static int cur_size(void)
{
	if (au.tool == TOOL_FILL || au.stroke == STROKE_RECT || au.select != SEL_OFF)
		return 1;
	return au.brush_size;
}

// a line or rectangle is being drawn and has its start: it is shown as it would paint
static bool drag_preview(void)
{
	return au.line_active && au.line_have && au.tool != TOOL_FILL;
}

/* ------------------------------------------------------------------------- */
/* THE BRUSH                                                                 */
/* ------------------------------------------------------------------------- */

static int floor_div2(int a)
{
	return a >= 0 ? a / 2 : -((-a + 1) / 2);
}

static int ceil_div2(int a)
{
	return -floor_div2(-a);
}

// the brush's centre on one axis, doubled: an odd size sits on the texel the point
// is in, an even one on the texel corner nearest it
static int centre2(float t, int size)
{
	return (size & 1) ? 2 * (int)floorf(t) + 1 : 2 * (int)floorf(t + 0.5f);
}

/* Row (ty)'s texels inside the brush centred at (c2x, c2y): [x0, x1], or false.
   primary_rays.rgen's author_mask_bits asks the same thing texel by texel:
   dx^2 + dy^2 <= size^2. */
static bool brush_row(int c2x, int c2y, int ty, int *x0, int *x1)
{
	const int s = cur_size();
	const int dy = 2 * ty + 1 - c2y;
	const int r2 = s * s - dy * dy;
	if (r2 < 0)
		return false;
	int k = (int)sqrtf((float)r2);
	while ((k + 1) * (k + 1) <= r2)
		k++;
	while (k > 0 && k * k > r2)
		k--;
	*x0 = ceil_div2(c2x - k - 1);
	*x1 = floor_div2(c2x + k - 1);
	return *x0 <= *x1;
}

static void brush_rows(int c2y, int *ty0, int *ty1)
{
	*ty0 = ceil_div2(c2y - cur_size() - 1);
	*ty1 = floor_div2(c2y + cur_size() - 1);
}

/* THE BRUSH SWEPT ALONG A LINE, from a centre (c2x, c2y) on by (dx, dy), all in
   doubled texels - what the line tool paints. q is a texel's offset from the
   first centre (2t + 1 - c2), covered when it is within the radius of the
   segment. primary_rays.rgen's author_mask_bits (author_line 1) is this
   arithmetic, for the outline - times the inverse of the length squared, as it. */
static bool sweep_inside(float qx, float qy, float dx, float dy)
{
	const float h = (float)cur_size();
	const float dd = dx * dx + dy * dy;
	const float inv_dd = dd > 0.f ? 1.f / dd : 0.f;
	const float t = Q_clipf((qx * dx + qy * dy) * inv_dd, 0.f, 1.f);
	const float ex = qx - dx * t, ey = qy - dy * t;
	return ex * ex + ey * ey <= h * h;
}

// the rows the swept brush covers
static void sweep_rows(int c2y, int dy, int *ty0, int *ty1)
{
	*ty0 = ceil_div2(c2y + min(0, dy) - cur_size() - 1);
	*ty1 = floor_div2(c2y + max(0, dy) + cur_size() - 1);
}

/* Row (ty)'s texels the swept brush covers: [x0, x1], or false. The box round
   the centres within reach of the row, then narrowed from both ends to the
   texels sweep_inside takes - the shape is convex, so what is left is one run. */
static bool sweep_row(int c2x, int c2y, int dx, int dy, int ty, int *x0, int *x1)
{
	const float qy = (float)(2 * ty + 1 - c2y);
	const float hx = (float)cur_size(), hy = hx;
	float t0 = 0.f, t1 = 1.f;
	if (dy == 0)
	{
		if (fabsf(qy) > hy)
			return false;
	}
	else
	{
		const float ta = (qy - hy) / (float)dy, tb = (qy + hy) / (float)dy;
		t0 = max(t0, min(ta, tb));
		t1 = min(t1, max(ta, tb));
		if (t0 > t1)
			return false;
	}
	const float lo = min(t0 * dx, t1 * dx) - hx, hi = max(t0 * dx, t1 * dx) + hx;
	// 2 tx + 1 - c2x within [lo, hi]
	*x0 = (int)ceilf(((float)c2x + lo - 1.f) * 0.5f);
	*x1 = (int)floorf(((float)c2x + hi - 1.f) * 0.5f);
	// narrowed to the capsule, to the last float - the shader's
	while (*x0 <= *x1 && !sweep_inside((float)(2 * *x0 + 1 - c2x), qy, (float)dx, (float)dy))
		(*x0)++;
	while (*x1 >= *x0 && !sweep_inside((float)(2 * *x1 + 1 - c2x), qy, (float)dx, (float)dy))
		(*x1)--;
	return *x0 <= *x1;
}

/* How far into the swept brush q is, 0 on the line to 1 at the brush's edge
   plus a texel - as dab's r, for the falloff: the distance to the segment. */
static float sweep_r(float qx, float qy, float dx, float dy)
{
	const float a = (float)cur_size() + 1.f;
	const float dd = dx * dx + dy * dy;
	const float t = dd > 0.f ? Q_clipf((qx * dx + qy * dy) / dd, 0.f, 1.f) : 0.f;
	const float ex = qx - dx * t, ey = qy - dy * t;
	return sqrtf(ex * ex + ey * ey) / a;
}

/* THE RECTANGLE STROKE, in doubled texels: centred at (c2x, c2y) - the sum of its
   first and last texel, plus one - with half sides (bx, by), which are its
   texels across and down, its corners rounded by r2 (at most the shorter half
   side). (qx, qy) is a texel's offset from the centre, 2t + 1 - c2. All integer,
   so primary_rays.rgen's author_rect agrees texel for texel. (depth): how far
   inside it the texel's centre is, for the falloff. */
static bool rect_inside(int qx, int qy, int bx, int by, int r2, float *depth)
{
	qx = abs(qx);
	qy = abs(qy);
	if (qx > bx || qy > by)
		return false;
	const int ex = qx - bx + r2, ey = qy - by + r2;
	if (ex > 0 && ey > 0)
	{
		const int e2 = ex * ex + ey * ey;
		if (e2 > r2 * r2)
			return false;
		if (depth)
			*depth = (float)r2 - sqrtf((float)e2);
	}
	else if (depth)
		*depth = (float)min(bx - qx, by - qy);
	return true;
}

// a rectangle's texels on one axis: [t0, t1]
static void rect_span(int c2, int b, int *t0, int *t1)
{
	*t0 = ceil_div2(c2 - b - 1);
	*t1 = floor_div2(c2 + b - 1);
}

/* Row (ty)'s texels inside the rectangle: [x0, x1], or false. Away from the
   corners, its whole width; across them, |qx| <= bx - r2 + the largest m with
   m^2 + ey^2 <= r2^2. */
static bool rect_row(int c2x, int c2y, int bx, int by, int r2, int ty, int *x0, int *x1)
{
	const int qy = abs(2 * ty + 1 - c2y);
	if (qy > by)
		return false;
	int k = bx;
	const int ey = qy - by + r2;
	if (ey > 0)
	{
		const int m2 = r2 * r2 - ey * ey;
		int m = (int)sqrtf((float)max(m2, 0));
		while ((m + 1) * (m + 1) <= m2)
			m++;
		while (m > 0 && m * m > m2)
			m--;
		k = bx - r2 + m;
	}
	*x0 = ceil_div2(c2x - k - 1);
	*x1 = floor_div2(c2x + k - 1);
	return *x0 <= *x1;
}

// an offset on a tiled axis of n texels: the copy nearest the centre, in [-n, n) - the shader's wrap
static int rect_q(int q, int n, bool wrap)
{
	if (!wrap)
		return q;
	const int p = 2 * n, s = q + n;
	const int k = s >= 0 ? s / p : -((-s + p - 1) / p);
	return q - k * p;
}

/* ------------------------------------------------------------------------- */
/* THE LOCK, AS PAINTING SEES IT                                             */
/* ------------------------------------------------------------------------- */

/* (x, y) of a W x H map may be painted: inside the lock and inside the
   selection, those that are on. One not rasterised for this map yet
   (region_sync, each tick) holds everything back. */
static bool region_allows(int x, int y, int W, int H)
{
	const size_t at = (size_t)y * W + x;
	if (lk.on && (!lk.mask || lk.mask_w != W || lk.mask_h != H || !lk.mask[at]))
		return false;
	if (sel_on() && (!sl.mask || sl.mask_w != W || sl.mask_h != H || !sl.mask[at]))
		return false;
	return true;
}

// the lock and the selection, those that are on, are rasterised for a W x H map
static bool region_ready(int W, int H)
{
	if (lk.on && (!lk.mask || lk.mask_w != W || lk.mask_h != H))
		return false;
	if (sel_on() && (!sl.mask || sl.mask_w != W || sl.mask_h != H))
		return false;
	return true;
}

// what painting is held to, for the status line and the panel
static const char *region_name(void)
{
	if (lk.on && sel_on())
		return "lock inside the selection";
	return lk.on ? "lock" : "selection";
}

static bool region_image_ok(void)
{
	if (!rg.image || rg.image >= MAX_RIMAGES)
		return false;
	const image_t *img = r_images + rg.image;
	return img->registration_sequence && img->pix_data && !strcmp(img->name, rg.image_name)
		&& img->upload_width == rg.w && img->upload_height == rg.h;
}

/* ------------------------------------------------------------------------- */
/* PAINT, STROKES AND UNDO                                                   */
/* ------------------------------------------------------------------------- */

// what a dab paints, from the panel: (hit) turns a normal's screen direction into its tangent frame
static void make_paint(const author_hit_t *hit, author_paint_t *p)
{
	memset(p, 0, sizeof(*p));
	switch (au.slot)
	{
	case SLOT_ROUGH:
	case SLOT_METAL:
	case SLOT_HEIGHT:
	case SLOT_EMISSIVE:     // the brightness, 0..1 = 0..200% of the albedo
		p->v[0] = p->v[1] = p->v[2] = Q_clipf(au.value[au.slot], 0.f, 1.f);
		break;
	case SLOT_GLASS:        // glass 1, or the wall round it 0
		p->v[0] = p->v[1] = p->v[2] = au.value[SLOT_GLASS] >= 0.5f ? 1.f : 0.f;
		break;
	case SLOT_NORMAL:
	{
		// the direction on screen (y down), as the tangent frame's x/y
		const float sx = cosf(au.normal_angle), sy = sinf(au.normal_angle);
		float ts[2] = { sx, sy };   // the map view: the image's own axes, x = +u, y = +v (down)
		if (hit)
		{
			ts[0] = hit->tx[0] * sx + hit->ty[0] * sy;
			ts[1] = hit->tx[1] * sx + hit->ty[1] * sy;
			if (fabsf(ts[0]) + fabsf(ts[1]) < 1e-12f)
			{
				// no tangents there: along the texture's axes as they lie on screen
				ts[0] = hit->duv[0] * sx + hit->duv[2] * sy;
				ts[1] = hit->duv[1] * sx + hit->duv[3] * sy;
				if (!au.tangent_warned)
				{
					au.tangent_warned = true;
					author_status("this surface has no tangents: the tilt follows its texture axes");
				}
			}
			if (au.stage >= 0)
			{
				/* A stage's normal map leans along the stage's own axes, which
				   q3_stages.glsl turns back onto the surface's by transpose(lin)
				   (the turn, not the scale): so the lean wanted on the surface is
				   turned the other way, by the inverse of that. */
				const float *m = au.stage_tc;
				const float det = m[0] * m[3] - m[1] * m[2];
				if (fabsf(det) > 1e-8f)
				{
					const float sx2 = (m[3] * ts[0] - m[2] * ts[1]) / det;
					const float sy2 = (-m[1] * ts[0] + m[0] * ts[1]) / det;
					ts[0] = sx2;
					ts[1] = sy2;
				}
			}
		}
		const float len = sqrtf(ts[0] * ts[0] + ts[1] * ts[1]);
		if (len > 0.f)
		{
			ts[0] /= len;
			ts[1] /= len;
		}
		const float theta = au.normal_intensity * AUTHOR_MAX_TILT_DEG * (float)M_PI / 180.f;
		const float s = sinf(theta);
		// path_tracer_rgen.h rgbToNormal: x, y = rgb * 2 - 1; z = b as stored
		p->v[0] = ts[0] * s;
		p->v[1] = ts[1] * s;
		p->v[2] = cosf(theta);
		break;
	}
	}
}

static bool base_rgb(int x, int y, int W, int H, byte rgb[3]);

static float luminance(const float c[3])
{
	return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}

/* A painted emissive texel's colour (emit_painted): the albedo there, linear, times
   the map's tint, times its brightness (alpha / 255 = 0..200%) - stored at HALF,
   so 200% of a white texel still fits a byte, and the material's emissive_factor
   doubled once to make up for it (create_emit_map): 100% glows as bright as the
   texture, times the factor it had. */
static void emit_colour(const byte albedo[3], byte alpha, bool srgb, byte out[3])
{
	const float b = (float)alpha / 255.f;
	for (int c = 0; c < 3; c++)
	{
		const float lin = srgb_to_linear((float)albedo[c] / 255.f) * Q_clipf(au.emit_tint[c], 0.f, 1.f) * b;
		out[c] = to_byte(srgb ? linear_to_srgb(lin) : lin);
	}
}

static void emit_bake(const image_t *img, int x, int y, byte alpha, byte out[3])
{
	byte albedo[3] = { 255, 255, 255 };     // no albedo in memory: white
	base_rgb(x, y, img->upload_width, img->upload_height, albedo);
	emit_colour(albedo, alpha, img->is_srgb, out);
}

// texel (at) of (pix) - pixels in (img)'s format - painted as (slot), in paint terms
static void texel_get(const image_t *img, const byte *pix, size_t at, int slot, float v[3])
{
	if (img->pixel_format == PF_R16_UNORM)
	{
		v[0] = v[1] = v[2] = (float)((const uint16_t *)pix)[at] / 65535.f;
		return;
	}
	const byte *px = pix + at * 4;
	switch (slot)
	{
	case SLOT_ROUGH:
	case SLOT_METAL:
	case SLOT_HEIGHT:
	case SLOT_GLASS:
		v[0] = (float)px[0] / 255.f;
		if (img->is_srgb)
			v[0] = srgb_to_linear(v[0]);
		v[1] = v[2] = v[0];
		break;
	case SLOT_NORMAL:
		v[0] = (float)px[0] / 255.f * 2.f - 1.f;
		v[1] = (float)px[1] / 255.f * 2.f - 1.f;
		v[2] = (float)px[2] / 255.f;
		break;
	case SLOT_EMISSIVE:
		// a painted map's brightness is its alpha (emit_bake)
		v[0] = v[1] = v[2] = (float)px[3] / 255.f;
		break;
	default:
		for (int c = 0; c < 3; c++)
			v[c] = (float)px[c] / 255.f;
		break;
	}
}

// texel (x, y) of (img) becomes (v): true if it changed
static bool texel_put(image_t *img, int x, int y, int slot, const float v[3])
{
	/* The glass mask is 1 or 0, whatever made the value: every tool's blend
	   lands here, so a soft brush edge, the blur, the noise and the brightness
	   range all cut at half way. A stroke's weight over a half flips a texel. */
	float cut[3];
	if (slot == SLOT_GLASS)
	{
		cut[0] = cut[1] = cut[2] = v[0] >= 0.5f ? 1.f : 0.f;
		v = cut;
	}
	const size_t at = (size_t)y * img->upload_width + x;
	if (img->pixel_format == PF_R16_UNORM)
	{
		uint16_t *px = (uint16_t *)img->pix_data + at;
		const uint16_t g = (uint16_t)Q_clip((int)(Q_clipf(v[0], 0.f, 1.f) * 65535.f + 0.5f), 0, 65535);
		if (*px == g)
			return false;
		*px = g;
	}
	else
	{
		byte *px = img->pix_data + at * 4;
		byte rgb[3];
		// the alpha is kept: a legacy map hides metallic or roughness in it - but a
		// painted emissive map's alpha is its brightness, the colours made from it
		byte alpha = px[3];
		switch (slot)
		{
		case SLOT_ROUGH:
		case SLOT_METAL:
		case SLOT_HEIGHT:
		case SLOT_GLASS:
			rgb[0] = rgb[1] = rgb[2] = to_byte(img->is_srgb ? linear_to_srgb(v[0]) : v[0]);
			break;
		case SLOT_NORMAL:
			rgb[0] = to_byte(v[0] * 0.5f + 0.5f);
			rgb[1] = to_byte(v[1] * 0.5f + 0.5f);
			rgb[2] = to_byte(v[2]);
			break;
		case SLOT_EMISSIVE:
			alpha = to_byte(v[0]);
			emit_bake(img, x, y, alpha, rgb);
			break;
		default:
			for (int c = 0; c < 3; c++)
				rgb[c] = to_byte(v[c]);
			break;
		}
		if (px[0] == rgb[0] && px[1] == rgb[1] && px[2] == rgb[2] && px[3] == alpha)
			return false;
		px[0] = rgb[0];
		px[1] = rgb[1];
		px[2] = rgb[2];
		px[3] = alpha;
	}
	au.bb_x0 = min(au.bb_x0, x);
	au.bb_y0 = min(au.bb_y0, y);
	au.bb_x1 = max(au.bb_x1, x);
	au.bb_y1 = max(au.bb_y1, y);
	return true;
}

// (a) towards (b) by (w); a normal stays a unit vector
static void paint_mix(int slot, const float a[3], const float b[3], float w, float out[3])
{
	for (int c = 0; c < 3; c++)
		out[c] = a[c] + (b[c] - a[c]) * w;
	if (slot == SLOT_NORMAL)
	{
		const float len = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
		if (len > 1e-6f)
		{
			for (int c = 0; c < 3; c++)
				out[c] /= len;
		}
		else
		{
			out[0] = out[1] = 0.f;
			out[2] = 1.f;
		}
	}
}

// the brush's weight at (r): 0 its centre, 1 its edge; falloff 0 = hard all the way
static float falloff_weight(float r)
{
	const float f = au.falloff;
	if (f <= 0.f)
		return 1.f;
	const float inner = 1.f - f;
	if (r <= inner)
		return 1.f;
	const float t = Q_clipf((r - inner) / f, 0.f, 1.f);
	return 1.f - t * t * (3.f - 2.f * t);
}

// the noise tool: the paint +- the amount, the same per texel all stroke long
static void noise_target(const dab_ctx_t *c, int x, int y, float out[3])
{
	const float a = au.tool_opt[TOOL_NOISE];
	const float h = hash_signed(x, y, au.stroke_seed);
	memcpy(out, c->paint.v, sizeof(c->paint.v));
	switch (c->slot)
	{
	case SLOT_ROUGH:
	case SLOT_METAL:
	case SLOT_HEIGHT:
	case SLOT_EMISSIVE:
	case SLOT_GLASS:
		out[0] = out[1] = out[2] = Q_clipf(out[0] + h * a, 0.f, 1.f);
		break;
	case SLOT_NORMAL:
	{
		// a random lean on top of the painted one, up to the full tilt at 100%
		const float h2 = hash_signed(y + 7919, x - 104729, au.stroke_seed ^ 0x5bd1e995u);
		const float k = a * sinf(AUTHOR_MAX_TILT_DEG * (float)M_PI / 180.f);
		out[0] += h * k;
		out[1] += h2 * k;
		float xy2 = out[0] * out[0] + out[1] * out[1];
		if (xy2 > 0.98f)
		{
			const float s = sqrtf(0.98f / xy2);
			out[0] *= s;
			out[1] *= s;
			xy2 = 0.98f;
		}
		out[2] = sqrtf(1.f - xy2);
		break;
	}
	default:
		for (int i = 0; i < 3; i++)
			out[i] = Q_clipf(out[i] * (1.f + h * a), 0.f, 1.f);
		break;
	}
}

// the blur tool: a box average of the map as the stroke found it
static void blur_target(const dab_ctx_t *c, int x, int y, float out[3])
{
	const image_t *img = c->img;
	const int W = img->upload_width, H = img->upload_height;
	const int r = c->blur_r > 0 ? c->blur_r : Q_clip(cur_size() / 6, 1, 6);
	float sum[3] = { 0.f, 0.f, 0.f };
	int n = 0;
	for (int dy = -r; dy <= r; dy++)
	{
		int sy = y + dy;
		if (c->wrap)
			sy = ((sy % H) + H) % H;
		else if (sy < 0 || sy >= H)
			continue;
		for (int dx = -r; dx <= r; dx++)
		{
			int sx = x + dx;
			if (c->wrap)
				sx = ((sx % W) + W) % W;
			else if (sx < 0 || sx >= W)
				continue;
			float v[3];
			texel_get(img, au.stroke_before, (size_t)sy * W + sx, c->slot, v);
			for (int i = 0; i < 3; i++)
				sum[i] += v[i];
			n++;
		}
	}
	for (int i = 0; i < 3; i++)
		out[i] = n ? sum[i] / (float)n : 0.f;
}

/* Texel (x, y) - on the map, wrapped - painted with weight (w) by this stroke.
   It keeps the highest weight the stroke gives it, as mix(before, target, w):
   a soft edge blends into what was there, and only the next stroke builds on it. */
static void paint_texel(dab_ctx_t *c, int x, int y, float w)
{
	image_t *img = c->img;
	const size_t at = (size_t)y * img->upload_width + x;
	const byte wb = (byte)Q_clip((int)(w * 255.f + 0.5f), 0, 255);
	if (wb == 0 || wb <= au.stroke_weight[at])
		return;
	au.stroke_weight[at] = wb;

	float target[3], before[3], out[3];
	if (au.tool == TOOL_NOISE)
		noise_target(c, x, y, target);
	else if (au.tool == TOOL_BLUR)
		blur_target(c, x, y, target);
	else
		memcpy(target, c->paint.v, sizeof(target));
	texel_get(img, au.stroke_before, at, c->slot, before);
	paint_mix(c->slot, before, target, (float)wb / 255.f, out);
	if (texel_put(img, x, y, c->slot, out))
		c->changed = true;
}

// texel (x, y) at (r) from the brush's centre (0 .. 1 at its edge), by the tool
static void dab_texel(dab_ctx_t *c, int x, int y, float r)
{
	const image_t *img = c->img;
	if (!region_allows(x, y, img->upload_width, img->upload_height))
		return;
	float w = falloff_weight(r);
	switch (au.tool)
	{
	case TOOL_SPRAY:
		// dots: a texel is sprayed whole or not at all, more often near the centre
		if (rand01() >= c->spray_p * w)
			return;
		w = 1.f;
		break;
	case TOOL_HATCH:
	{
		const int sp = hatch_spacing();
		if (((x + y) % sp + sp) % sp != 0 && ((x - y) % sp + sp) % sp != 0)
			return;
		break;
	}
	case TOOL_BLUR:
		w *= au.tool_opt[TOOL_BLUR];
		break;
	default:
		break;
	}
	paint_texel(c, x, y, w);
}

static void dab_done(dab_ctx_t *c)
{
	if (!c->changed)
		return;
	au.upload_image = image_index(c->img);
	image_dirty[au.upload_image] = (uint8_t)(c->slot + 1);
	image_stage[au.upload_image] = (uint8_t)(au.stage + 1);
}

static void dab(dab_ctx_t *c, int c2x, int c2y)
{
	image_t *img = c->img;
	const int W = img->upload_width, H = img->upload_height;
	const float rs = 1.f / (float)(cur_size() + 1);
	int ty0, ty1;
	brush_rows(c2y, &ty0, &ty1);
	for (int ty = ty0; ty <= ty1; ty++)
	{
		int x0, x1;
		if (!brush_row(c2x, c2y, ty, &x0, &x1))
			continue;
		int y = ty;
		if (c->wrap)
			y = ((y % H) + H) % H;
		else if (y < 0 || y >= H)
			continue;
		const int dy = 2 * ty + 1 - c2y;
		for (int tx = x0; tx <= x1; tx++)
		{
			int x = tx;
			if (c->wrap)
				x = ((x % W) + W) % W;
			else if (x < 0 || x >= W)
				continue;
			const int dx = 2 * tx + 1 - c2x;
			dab_texel(c, x, y, sqrtf((float)(dx * dx + dy * dy)) * rs);
		}
	}
	dab_done(c);
}

// the brush's centre, doubled (centre2), for a point (tu, tv) in texels
static int centre2_x(float tu)
{
	return centre2(tu, cur_size());
}

static int centre2_y(float tv)
{
	return centre2(tv, cur_size());
}

static void dab_line(dab_ctx_t *c, float tu0, float tv0, float tu1, float tv1)
{
	const float dx = tu1 - tu0, dy = tv1 - tv0;
	const float dist = sqrtf(dx * dx + dy * dy);
	const float step = max(1.f, (float)cur_size() * 0.25f);
	int n = (int)ceilf(dist / step);
	n = Q_clip(n, 1, 8192);
	// the spray's chance this tick, shared out over the dabs
	const float spray_total = c->spray_p;
	c->spray_p = 1.f - powf(1.f - Q_clipf(spray_total, 0.f, 0.999f), 1.f / (float)n);
	for (int i = 1; i <= n; i++)
	{
		const float t = (float)i / (float)n;
		dab(c, centre2_x(tu0 + dx * t), centre2_y(tv0 + dy * t));
	}
	c->spray_p = spray_total;
}

/* The line tool: the brush swept from centre (c2x, c2y) on by (dx, dy), doubled
   texels - every texel it covers once, its falloff from the line itself. */
static void dab_sweep(dab_ctx_t *c, int c2x, int c2y, int dx, int dy)
{
	image_t *img = c->img;
	const int W = img->upload_width, H = img->upload_height;
	int ty0, ty1;
	sweep_rows(c2y, dy, &ty0, &ty1);
	for (int ty = ty0; ty <= ty1; ty++)
	{
		int x0, x1;
		if (!sweep_row(c2x, c2y, dx, dy, ty, &x0, &x1))
			continue;
		int y = ty;
		if (c->wrap)
			y = ((y % H) + H) % H;
		else if (y < 0 || y >= H)
			continue;
		const float qy = (float)(2 * ty + 1 - c2y);
		for (int tx = x0; tx <= x1; tx++)
		{
			int x = tx;
			if (c->wrap)
				x = ((x % W) + W) % W;
			else if (x < 0 || x >= W)
				continue;
			dab_texel(c, x, y, sweep_r((float)(2 * tx + 1 - c2x), qy, (float)dx, (float)dy));
		}
	}
	dab_done(c);
}

/* The rectangle stroke (rect_inside), painted once: its falloff over the outer
   part of its half-width. On a tiled map each map texel is taken once, at the
   copy of it nearest the centre - an axis longer than the map is gone over a
   map's width, not the rectangle's - as the shader tests it. */
static void dab_rect(dab_ctx_t *c, int c2x, int c2y, int bx, int by, int r2)
{
	image_t *img = c->img;
	const int W = img->upload_width, H = img->upload_height;
	const float m = (float)max(1, min(bx, by));
	int tx0, tx1, ty0, ty1;
	rect_span(c2x, bx, &tx0, &tx1);
	rect_span(c2y, by, &ty0, &ty1);
	if (c->wrap && tx1 - tx0 + 1 > W)
	{
		tx0 = 0;
		tx1 = W - 1;
	}
	if (c->wrap && ty1 - ty0 + 1 > H)
	{
		ty0 = 0;
		ty1 = H - 1;
	}
	for (int ty = ty0; ty <= ty1; ty++)
	{
		int y = ty;
		if (c->wrap)
			y = ((y % H) + H) % H;
		else if (y < 0 || y >= H)
			continue;
		const int qy = rect_q(2 * ty + 1 - c2y, H, c->wrap);
		for (int tx = tx0; tx <= tx1; tx++)
		{
			int x = tx;
			if (c->wrap)
				x = ((x % W) + W) % W;
			else if (x < 0 || x >= W)
				continue;
			float depth;
			if (!rect_inside(rect_q(2 * tx + 1 - c2x, W, c->wrap), qy, bx, by, r2, &depth))
				continue;
			dab_texel(c, x, y, Q_clipf(1.f - depth / m, 0.f, 1.f));
		}
	}
	dab_done(c);
}

// how far apart two texels' values are, 0..1, for the fill's tolerance
static float value_distance(int slot, const float a[3], const float b[3])
{
	float d = 0.f;
	const int n = slot_is_scalar(slot) ? 1 : 3;
	for (int i = 0; i < n; i++)
		d = max(d, fabsf(a[i] - b[i]) * (slot == SLOT_NORMAL && i < 2 ? 0.5f : 1.f));
	return d;
}

// the base texture's colour at texel (x, y) of a W x H map, nearest; false if it can't say
static bool base_rgb(int x, int y, int W, int H, byte rgb[3])
{
	const image_t *b = author_base();
	if (!b || !b->pix_data || b->pixel_format != PF_R8G8B8A8_UNORM || b->upload_width <= 0 || b->upload_height <= 0)
		return false;
	const int sx = (int)((int64_t)x * b->upload_width / W), sy = (int)((int64_t)y * b->upload_height / H);
	const byte *p = b->pix_data + ((size_t)sy * b->upload_width + sx) * 4;
	rgb[0] = p[0];
	rgb[1] = p[1];
	rgb[2] = p[2];
	return true;
}

/* The fill tool, clicked on texel (sx, sy): the whole lock and/or selection if
   one is on; else the texels reached from it through neighbours (4-way, the
   map's edges stop it) whose value - or base texture colour - is within the
   tolerance of its. */
static void fill_at(dab_ctx_t *c, int sx, int sy)
{
	image_t *img = c->img;
	const int W = img->upload_width, H = img->upload_height;
	int filled = 0;

	if (region_on())
	{
		if (!region_ready(W, H))
		{
			author_status("the %s isn't ready for this map yet - click again", region_name());
			return;
		}
		for (int y = 0; y < H; y++)
		{
			for (int x = 0; x < W; x++)
			{
				if (region_allows(x, y, W, H))
				{
					paint_texel(c, x, y, 1.f);
					filled++;
				}
			}
		}
		dab_done(c);
		author_status("filled the %s: %d texels", region_name(), filled);
		return;
	}

	if (sx < 0 || sy < 0 || sx >= W || sy >= H)
		return;

	bool by_colour = au.fill_by_colour[c->slot];
	byte ref_rgb[3];
	if (by_colour && !base_rgb(sx, sy, W, H, ref_rgb))
	{
		by_colour = false;
		author_status("the base texture's colours aren't in memory: filling by the map's values");
	}
	float ref[3];
	texel_get(img, au.stroke_before, (size_t)sy * W + sx, c->slot, ref);
	const float tol = au.tool_opt[TOOL_FILL] + 1e-4f;
	const int tol255 = (int)(au.tool_opt[TOOL_FILL] * 255.f + 0.5f);

	byte *seen = Z_Mallocz((size_t)W * H);
	int cap = 4096, top = 0;
	int *stack = Z_Malloc(sizeof(int) * cap * 2);
	stack[0] = sx;
	stack[1] = sy;
	top = 1;
	seen[(size_t)sy * W + sx] = 1;
	static const int nx[4] = { 1, -1, 0, 0 }, ny[4] = { 0, 0, 1, -1 };
	while (top > 0)
	{
		top--;
		const int x = stack[top * 2], y = stack[top * 2 + 1];
		paint_texel(c, x, y, 1.f);
		filled++;
		for (int k = 0; k < 4; k++)
		{
			const int x2 = x + nx[k], y2 = y + ny[k];
			if (x2 < 0 || y2 < 0 || x2 >= W || y2 >= H)
				continue;
			const size_t at = (size_t)y2 * W + x2;
			if (seen[at])
				continue;
			seen[at] = 1;
			bool same;
			if (by_colour)
			{
				byte rgb[3];
				same = base_rgb(x2, y2, W, H, rgb)
					&& abs(rgb[0] - ref_rgb[0]) <= tol255 && abs(rgb[1] - ref_rgb[1]) <= tol255
					&& abs(rgb[2] - ref_rgb[2]) <= tol255;
			}
			else
			{
				// against the map as the click found it: what was there decides
				float v[3];
				texel_get(img, au.stroke_before, at, c->slot, v);
				same = value_distance(c->slot, v, ref) <= tol;
			}
			if (!same)
				continue;
			if (top == cap)
			{
				cap *= 2;
				stack = Z_Realloc(stack, sizeof(int) * cap * 2);
			}
			stack[top * 2] = x2;
			stack[top * 2 + 1] = y2;
			top++;
		}
	}
	Z_Free(stack);
	Z_Free(seen);
	dab_done(c);
	author_status("filled %d texels (%s within %d%%)", filled, by_colour ? "colour" : "value",
		(int)(au.tool_opt[TOOL_FILL] * 100.f + 0.5f));
}

static void upload_now(void)
{
	if (au.upload_image < 0)
		return;
	image_t *img = r_images + au.upload_image;
	au.upload_image = -1;
	if (!img->registration_sequence || !img->pix_data)
		return;
	IMG_PixelsEdited(img);
	vkpt_reset_accumulation();
	au.last_upload_ms = Sys_Milliseconds();
}

// an emissive map's light colour and extent are measured from its pixels
static void after_emissive_change(image_t *img)
{
	img->processing_complete = false;
	MAT_ImagesReloaded();
}

static void undo_free(author_undo_t *e)
{
	Z_Free(e->before);
	Z_Free(e->after);
	undo_bytes -= e->bytes;
	memset(e, 0, sizeof(*e));
}

static void undo_drop_oldest(void)
{
	if (!undo_count)
		return;
	undo_free(&undo_stack[0]);
	memmove(undo_stack, undo_stack + 1, sizeof(undo_stack[0]) * (undo_count - 1));
	memset(&undo_stack[undo_count - 1], 0, sizeof(undo_stack[0]));
	undo_count--;
}

// step (e) was painted on (img): the map undo and redo work on
static bool undo_match(const author_undo_t *e, const image_t *img)
{
	return img && e->image == image_index(img) && !strcmp(e->name, img->name);
}

// a new stroke on (img): its undone steps can't be redone any more - other maps' still can
static void undo_drop_redo(const image_t *img)
{
	int n = 0;
	for (int i = 0; i < undo_count; i++)
	{
		if (!undo_stack[i].applied && undo_match(&undo_stack[i], img))
			undo_free(&undo_stack[i]);
		else
			undo_stack[n++] = undo_stack[i];
	}
	for (int i = n; i < undo_count; i++)
		memset(&undo_stack[i], 0, sizeof(undo_stack[i]));
	undo_count = n;
}

// (img)'s steps that can be undone and redone
static void undo_counts(const image_t *img, int *undo, int *redo)
{
	*undo = *redo = 0;
	for (int i = 0; i < undo_count; i++)
	{
		if (undo_match(&undo_stack[i], img))
			(*(undo_stack[i].applied ? undo : redo))++;
	}
}

static void copy_rect(byte *dst, int dst_stride, const byte *src, int src_stride, int row_bytes, int rows)
{
	for (int r = 0; r < rows; r++)
		memcpy(dst + (size_t)r * dst_stride, src + (size_t)r * src_stride, row_bytes);
}

// the stroke on (img) has begun: keep the whole map as it was
static void stroke_end(void);

static void stroke_touch(image_t *img)
{
	const int idx = image_index(img);
	if (au.stroke_before && au.stroke_image == idx)
		return;
	stroke_end();

	const size_t bytes = (size_t)img->upload_width * img->upload_height * image_bpp(img);
	au.stroke_before = Z_Malloc(bytes);
	memcpy(au.stroke_before, img->pix_data, bytes);
	au.stroke_weight = Z_Mallocz((size_t)img->upload_width * img->upload_height);
	au.stroke_seed = hash32(Sys_Milliseconds() ^ (au.rng += 0x9e3779b9u));
	au.stroke_image = idx;
	au.stroke_slot = au.slot;
	au.stroke_stage = au.stage;
	au.stroke_w = img->upload_width;
	au.stroke_h = img->upload_height;
	au.stroke_bpp = image_bpp(img);
	au.bb_x0 = au.bb_y0 = INT_MAX;
	au.bb_x1 = au.bb_y1 = INT_MIN;
}

// the stroke is over: its rectangle, before and after, onto the undo stack
static void stroke_end(void)
{
	if (!au.stroke_before)
		return;

	image_t *img = r_images + au.stroke_image;
	const bool same = img->registration_sequence && img->pix_data && img->upload_width == au.stroke_w
		&& img->upload_height == au.stroke_h && image_bpp(img) == au.stroke_bpp;

	if (same && au.bb_x0 <= au.bb_x1 && au.bb_y0 <= au.bb_y1)
	{
		// a new stroke drops what could have been redone on this map
		undo_drop_redo(img);

		author_undo_t e;
		memset(&e, 0, sizeof(e));
		e.image = au.stroke_image;
		Q_strlcpy(e.name, img->name, sizeof(e.name));
		e.slot = au.stroke_slot;
		e.stage = au.stroke_stage;
		e.width = au.stroke_w;
		e.height = au.stroke_h;
		e.bpp = au.stroke_bpp;
		e.x = au.bb_x0;
		e.y = au.bb_y0;
		e.w = au.bb_x1 - au.bb_x0 + 1;
		e.h = au.bb_y1 - au.bb_y0 + 1;
		const int row = e.w * e.bpp, stride = e.width * e.bpp;
		const size_t rect = (size_t)row * e.h;
		const size_t first = ((size_t)e.y * e.width + e.x) * e.bpp;
		e.before = Z_Malloc(rect);
		e.after = Z_Malloc(rect);
		e.bytes = rect * 2;
		copy_rect(e.before, row, au.stroke_before + first, stride, row, e.h);
		copy_rect(e.after, row, img->pix_data + first, stride, row, e.h);
		e.applied = true;

		while (undo_count && (undo_count == AUTHOR_UNDO_MAX || undo_bytes + e.bytes > AUTHOR_UNDO_BYTES))
			undo_drop_oldest();
		undo_stack[undo_count++] = e;
		undo_bytes += e.bytes;

		if (e.slot == SLOT_EMISSIVE)
			after_emissive_change(img);
	}

	Z_Free(au.stroke_before);
	au.stroke_before = NULL;
	Z_Free(au.stroke_weight);
	au.stroke_weight = NULL;
	au.stroke_image = -1;
	upload_now();
}

/* Undo: the last step still applied on the map being painted; redo: its first
   undone one. Per map, the steps are a run applied then a run undone, so these
   two stay at the boundary between them - the steps of other maps between
   them are left alone, for when those maps are painted again. */
static void undo_apply(bool redo)
{
	stroke_end();
	const image_t *cur = author_image();
	int pick = -1, others = 0;
	for (int i = 0; i < undo_count; i++)
	{
		const author_undo_t *e = &undo_stack[i];
		if (!undo_match(e, cur))
		{
			others += e->applied != redo;
			continue;
		}
		if (redo && !e->applied && pick < 0)
			pick = i;
		else if (!redo && e->applied)
			pick = i;
	}
	if (pick < 0)
	{
		const char *what = redo ? "redo" : "undo";
		if (!cur)
			author_status("nothing to %s: this %s map isn't made yet", what, slot_word[au.slot]);
		else if (others)
			author_status("nothing to %s on this %s map (%d step%s on other maps: paint those to %s them)",
				what, slot_word[au.slot], others, others == 1 ? "" : "s", what);
		else
			author_status("nothing to %s on this %s map", what, slot_word[au.slot]);
		return;
	}

	author_undo_t *e = &undo_stack[pick];
	image_t *img = r_images + e->image;
	if (!img->registration_sequence || !img->pix_data || strcmp(img->name, e->name)
		|| img->upload_width != e->width || img->upload_height != e->height || image_bpp(img) != e->bpp)
	{
		author_status("can't %s: %s is no longer loaded as it was", redo ? "redo" : "undo", e->name);
		return;
	}

	const int row = e->w * e->bpp, stride = e->width * e->bpp;
	copy_rect(img->pix_data + ((size_t)e->y * e->width + e->x) * e->bpp, stride,
		redo ? e->after : e->before, row, row, e->h);
	e->applied = redo;

	if (!image_dirty[e->image])
	{
		image_dirty[e->image] = (uint8_t)(e->slot + 1);
		image_stage[e->image] = (uint8_t)(e->stage + 1);
	}
	if (e->slot == SLOT_EMISSIVE)
		after_emissive_change(img);
	IMG_PixelsEdited(img);
	vkpt_reset_accumulation();
	int n_undo, n_redo;
	undo_counts(img, &n_undo, &n_redo);
	author_status("%s on the %s map (%d of %d)", redo ? "redone" : "undone", slot_word[e->slot], n_undo, n_undo + n_redo);
}

// a saved map was loaded again under its new name: the history follows it
static void undo_remap(int from, const image_t *to)
{
	for (int i = 0; i < undo_count; i++)
	{
		if (undo_stack[i].image == from)
		{
			undo_stack[i].image = image_index(to);
			Q_strlcpy(undo_stack[i].name, to->name, sizeof(undo_stack[i].name));
		}
	}
}

/* ------------------------------------------------------------------------- */
/* FILES                                                                     */
/* ------------------------------------------------------------------------- */

typedef struct {
	byte *data;
	size_t len, cap;
} membuf_t;

static void membuf_put(membuf_t *m, const void *data, size_t size)
{
	if (m->len + size > m->cap)
	{
		size_t cap = max(m->cap * 2, m->len + size + 4096);
		m->data = Z_Realloc(m->data, cap);
		m->cap = cap;
	}
	memcpy(m->data + m->len, data, size);
	m->len += size;
}

static void membuf_func(void *context, void *data, int size)
{
	membuf_put((membuf_t *)context, data, (size_t)size);
}

static void put_be32(byte *p, uint32_t v)
{
	p[0] = (byte)(v >> 24);
	p[1] = (byte)(v >> 16);
	p[2] = (byte)(v >> 8);
	p[3] = (byte)v;
}

static uint32_t crc_table[256];

static uint32_t crc32_update(uint32_t c, const byte *p, size_t n)
{
	if (!crc_table[1])
	{
		for (uint32_t i = 0; i < 256; i++)
		{
			uint32_t k = i;
			for (int b = 0; b < 8; b++)
				k = (k & 1) ? 0xedb88320u ^ (k >> 1) : k >> 1;
			crc_table[i] = k;
		}
	}
	for (size_t i = 0; i < n; i++)
		c = crc_table[(c ^ p[i]) & 0xff] ^ (c >> 8);
	return c;
}

static void png_chunk(membuf_t *m, const char *type, const byte *data, uint32_t len)
{
	byte b[4];
	put_be32(b, len);
	membuf_put(m, b, 4);
	membuf_put(m, type, 4);
	if (len)
		membuf_put(m, data, len);
	uint32_t c = crc32_update(0xffffffffu, (const byte *)type, 4);
	c = crc32_update(c, data, len) ^ 0xffffffffu;
	put_be32(b, c);
	membuf_put(m, b, 4);
}

// a one-channel 16-bit PNG, which stb_image_write cannot make
static bool encode_png16(const uint16_t *px, int w, int h, membuf_t *out)
{
	const size_t row = 1 + (size_t)w * 2;
	byte *raw = Z_Malloc(row * h);
	for (int y = 0; y < h; y++)
	{
		byte *r = raw + row * y;
		r[0] = 0;   // no filter
		for (int x = 0; x < w; x++)
		{
			const uint16_t v = px[(size_t)y * w + x];
			r[1 + 2 * x] = (byte)(v >> 8);
			r[2 + 2 * x] = (byte)v;
		}
	}
	int zlen = 0;
	unsigned char *z = stbi_zlib_compress(raw, (int)(row * h), &zlen, 8);
	Z_Free(raw);
	if (!z)
		return false;

	static const byte sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
	membuf_put(out, sig, 8);
	byte ihdr[13];
	put_be32(ihdr, (uint32_t)w);
	put_be32(ihdr + 4, (uint32_t)h);
	ihdr[8] = 16;   // bits
	ihdr[9] = 0;    // grey
	ihdr[10] = ihdr[11] = ihdr[12] = 0;
	png_chunk(out, "IHDR", ihdr, 13);
	png_chunk(out, "IDAT", z, (uint32_t)zlen);
	png_chunk(out, "IEND", NULL, 0);
	free(z);   // stb_image_write's own malloc
	return true;
}

// the fewest channels that keep every pixel: 1 grey, 3 opaque, 4
static int rgba_channels(const byte *rgba, size_t n)
{
	bool grey = true, opaque = true;
	for (size_t i = 0; i < n && (grey || opaque); i++)
	{
		const byte *p = rgba + i * 4;
		if (p[0] != p[1] || p[1] != p[2])
			grey = false;
		if (p[3] != 255)
			opaque = false;
	}
	return opaque ? (grey ? 1 : 3) : 4;
}

static bool encode_rgba(const byte *rgba, int w, int h, bool tga, membuf_t *out)
{
	const size_t n = (size_t)w * h;
	const int comp = rgba_channels(rgba, n);
	byte *packed = NULL;
	const byte *data = rgba;
	if (comp != 4)
	{
		packed = Z_Malloc(n * comp);
		for (size_t i = 0; i < n; i++)
			for (int c = 0; c < comp; c++)
				packed[i * comp + c] = rgba[i * 4 + c];
		data = packed;
	}
	const int ok = tga
		? stbi_write_tga_to_func(membuf_func, out, w, h, comp, data)
		: stbi_write_png_to_func(membuf_func, out, w, h, comp, data, w * comp);
	Z_Free(packed);
	return ok != 0;
}

static bool encode_image(const image_t *img, bool tga, membuf_t *out)
{
	if (img->pixel_format == PF_R16_UNORM)
		return !tga && encode_png16((const uint16_t *)img->pix_data, img->upload_width, img->upload_height, out);
	return encode_rgba(img->pix_data, img->upload_width, img->upload_height, tga, out);
}

static bool file_exists(const char *ospath)
{
	FILE *f = fopen(ospath, "rb");
	if (!f)
		return false;
	fclose(f);
	return true;
}

// (ospath) gets (m), through a .tmp; an old one is kept as .bak
static bool write_with_backup(const char *ospath, const membuf_t *m, bool *backed_up)
{
	char tmp[MAX_OSPATH + 8], bak[MAX_OSPATH + 8];
	Q_concat(tmp, sizeof(tmp), ospath, ".tmp");
	Q_concat(bak, sizeof(bak), ospath, ".bak");
	*backed_up = false;

	FILE *f = fopen(tmp, "wb");
	if (!f)
		return false;
	bool ok = fwrite(m->data, 1, m->len, f) == m->len;
	ok = (fclose(f) == 0) && ok;
	if (!ok)
	{
		remove(tmp);
		return false;
	}

	if (file_exists(ospath))
	{
		remove(bak);
		if (rename(ospath, bak) != 0)
		{
			remove(tmp);
			return false;
		}
		*backed_up = true;
	}
	if (rename(tmp, ospath) != 0)
		return false;
	return true;
}

// materialDev/<path without materialDev/ or extension><suffix>.png
static void dev_png_path(const char *from, const char *suffix, char *out, size_t size)
{
	char stem[MAX_OSPATH];
	const char *p = from;
	if (!Q_strncasecmp(p, "materialDev/", 12))
		p += 12;
	while (*p == '/')
		p++;
	COM_StripExtension(stem, p, sizeof(stem));
	for (char *c = stem; *c; c++)
		if (*c == '*' || *c == ':' || *c == '?' || *c == '"' || *c == '<' || *c == '>' || *c == '|')
			*c = '_';
	Q_snprintf(out, size, "materialDev/%s%s.png", stem, suffix);
}

/* The OS path a new file at (game_path) is written to: the game directory -
   rerelease/ for the rerelease's Quake II textures - or, for a Quake III
   material's, Quake III's own baseq3/, beside quake3.mat and the rest of its
   materialDev (Matt, 2026-11-21). baseq3 is searched after every Quake II
   directory (files.c add_quake3_paths), so a game-path name still finds it. */
static bool game_path_to_os(const char *game_path, bool quake3, char *out, size_t size)
{
	const size_t n = quake3
		? Q_snprintf(out, size, "%s/baseq3/%s", Cvar_VariableString("basedir"), game_path)
		: Q_snprintf(out, size, "%s/%s", fs_gamedir, game_path);
	if (n >= size)
		return false;
	char dirs[MAX_OSPATH];
	Q_strlcpy(dirs, out, sizeof(dirs));
	FS_CreatePath(dirs);
	return true;
}

/* ------------------------------------------------------------------------- */
/* A NEW MAP                                                                 */
/* ------------------------------------------------------------------------- */

// texel (x, y) of a W x H map, sampled from (img)'s alpha, nearest
static byte alpha_at(const image_t *img, int x, int y, int W, int H)
{
	if (!img || !img->pix_data || img->pixel_format != PF_R8G8B8A8_UNORM || img->upload_width <= 0)
		return 255;
	const int sx = x * img->upload_width / W, sy = y * img->upload_height / H;
	return img->pix_data[((size_t)sy * img->upload_width + sx) * 4 + 3];
}

/* The material has no map in this slot: make one that looks as the surface does
   now - roughness as the shader works it out without a map (the base texture's
   alpha under a normal map, else 1, or the roughness_override that was the whole
   of it), metallic from the normal map's alpha, a flat normal map (alpha 0, so no
   metal comes out of it), black emission - write it as a PNG under materialDev/
   and point the material at it, as 'mat texture_roughness <file>' would. Runs
   between frames ('mat author _tick'): that can reload the map. */
// the average of (img)'s texels as (slot) reads them, on a grid of at most 64 x 64; (def) without one
static float image_average(const image_t *img, int slot, float def)
{
	if (!img || !img->pix_data || img->upload_width <= 0 || img->upload_height <= 0
		|| (img->pixel_format != PF_R8G8B8A8_UNORM && img->pixel_format != PF_R16_UNORM))
		return def;
	const int W = img->upload_width, H = img->upload_height;
	const int sx = max(1, W / 64), sy = max(1, H / 64);
	double sum = 0.0;
	int n = 0;
	for (int y = sy / 2; y < H; y += sy)
	{
		for (int x = sx / 2; x < W; x += sx)
		{
			float v[3];
			texel_get(img, img->pix_data, (size_t)y * W + x, slot, v);
			sum += v[0];
			n++;
		}
	}
	return n ? (float)(sum / n) : def;
}

/* A stage's new roughness or metallic map starts as one value - it is read
   through the stage's own coordinates, so the material's map can't be copied
   in: what the stage states (its roughness_override, the floor under a map;
   its metalness_factor, which then scales a map of 1s), else the material's
   own - its roughness_override when that is the whole of it, else its map's
   average. Linear 0..1. */
static float stage_start_value(void)
{
	const pbr_material_t *mat = au.mat;
	q3stagesettings_t s;
	const bool have = vkpt_q3_stage_settings(mat->name, au.stage, &s);
	if (au.slot == SLOT_ROUGH)
	{
		const float r = s.values[Q3SS_ROUGHNESS_OVERRIDE - Q3SS_NUM_PATHS];
		if (have && (s.set & (1u << Q3SS_ROUGHNESS_OVERRIDE)) && r >= 0.f)
			return Q_clipf(r, 0.f, 1.f);
		if (MAT_RoughnessIsWhole(mat))
			return Q_clipf(mat->roughness_override, 0.f, 1.f);
		return image_average(mat->image_roughness, SLOT_ROUGH, 1.f);
	}
	if (have && (s.set & (1u << Q3SS_METALNESS_FACTOR)))
		return 1.f;
	return Q_clipf(image_average(mat->image_metallic, SLOT_METAL, 0.f) * mat->metalness_factor, 0.f, 1.f);
}

/* Where the surface itself lies in (mat)'s height map, 0..1: a height h lands at
   h * (in + out) - in (material.h), so at h = in / (in + out) - 1, white, while
   nothing rises above it (displace_out 0, the default). */
static float height_surface_level(const pbr_material_t *mat)
{
	const float in = max(mat->displace_in, 0.f), out = max(mat->displace_out, 0.f);
	return in + out > 0.f ? in / (in + out) : 1.f;
}

/* (rgba), W x H, as a new PNG under materialDev/ named after (from) + (suffix) -
   never over an older file of that name: it may be work the material forgot. Its
   game path in (path). Always the material being authored's (au.mat): a Quake
   III one's goes into baseq3/. */
static bool write_new_map(const byte *rgba, int W, int H, const char *from, const char *suffix, char *path, size_t size)
{
	const bool quake3 = MAT_IsQuake3(au.mat);
	dev_png_path(from, suffix, path, size);
	for (int n = 2; n < 100; n++)
	{
		char found[MAX_OSPATH];
		if (FS_RealPath(path, 0, found, sizeof(found)) == FS_REALPATH_NONE)
			break;
		char numbered[32];
		Q_snprintf(numbered, sizeof(numbered), "%s_%d", suffix, n);
		dev_png_path(from, numbered, path, size);
	}

	char ospath[MAX_OSPATH];
	membuf_t m = { 0 };
	bool backed_up;
	const bool ok = encode_rgba(rgba, W, H, false, &m) && game_path_to_os(path, quake3, ospath, sizeof(ospath))
		&& write_with_backup(ospath, &m, &backed_up);
	Z_Free(m.data);
	return ok;
}

/* The emissive slot's first stroke: the material's own painted map (emit_painted),
   the base texture's size. Its brightness starts from the glow the material had -
   none: 0%, the texture shown dark; a _light, a glow made from the base's bright
   texels, a shared solid colour: that glow's luminance over the albedo's, texel by
   texel, up to 200% - and its colours are the albedo's times that and the tint. The
   map holds the brightness at half (emit_bake), so emissive_factor doubles, once:
   what glowed before glows as bright, in the texture's own colours. */
static image_t *create_emit_map(void)
{
	pbr_material_t *mat = au.mat;
	const image_t *base = author_base();
	if (!base || !base->pix_data || base->pixel_format != PF_R8G8B8A8_UNORM
		|| base->upload_width <= 0 || base->upload_height <= 0)
	{
		author_status("%s's texture isn't in memory: the emissive map is painted over it", mat->name);
		return NULL;
	}
	int W, H;
	map_size(&W, &H);

	const float factor = mat->emissive_factor;
	const image_t *old = mat->image_emissive;
	const bool from_old = old && old->pix_data && old->pixel_format == PF_R8G8B8A8_UNORM
		&& old->upload_width > 0 && old->upload_height > 0 && factor > 0.f;
	char was[MAX_QPATH];
	Q_strlcpy(was, old ? (old->filepath[0] ? old->filepath : old->name) : "", sizeof(was));

	byte *rgba = Z_Malloc((size_t)W * H * 4);
	int lit = 0;
	for (int y = 0; y < H; y++)
	{
		for (int x = 0; x < W; x++)
		{
			byte *p = rgba + ((size_t)y * W + x) * 4;
			byte albedo[3] = { 255, 255, 255 };
			base_rgb(x, y, W, H, albedo);
			float b = 0.f;
			if (from_old)
			{
				const int ox = (int)((int64_t)x * old->upload_width / W), oy = (int)((int64_t)y * old->upload_height / H);
				const byte *q = old->pix_data + ((size_t)oy * old->upload_width + ox) * 4;
				float e[3], a[3];
				for (int c = 0; c < 3; c++)
				{
					e[c] = old->is_srgb ? srgb_to_linear((float)q[c] / 255.f) : (float)q[c] / 255.f;
					a[c] = srgb_to_linear((float)albedo[c] / 255.f);
				}
				const float la = luminance(a);
				b = la > 1e-4f ? luminance(e) / la : 0.f;
			}
			p[3] = to_byte(Q_clipf(b * 0.5f, 0.f, 1.f));
			emit_colour(albedo, p[3], true, p);     // loaded as sRGB, as every emissive map is
			lit += p[3] != 0;
		}
	}

	char path[MAX_OSPATH];
	const bool ok = write_new_map(rgba, W, H, base->name, slot_suffix[SLOT_EMISSIVE], path, sizeof(path));
	Z_Free(rgba);
	if (!ok)
	{
		author_status("could not write %s", path);
		return NULL;
	}

	const float new_factor = 2.f * (factor > 0.f ? factor : 1.f);
	MAT_CommandFor(mat, va("%s \"%s\"", slot_attribute[SLOT_EMISSIVE], path));
	MAT_CommandFor(mat, va("emissive_factor %g", new_factor));
	image_t *img = author_image();
	if (!img || !img->pix_data)
	{
		author_status("wrote %s, but %s did not load it as a painted emissive map", path, mat->name);
		return NULL;
	}
	au.attr_changed = true;
	au.emit_mat = MAT_IndexOf(mat);
	au.emit_image = image_index(img);
	memcpy(au.emit_baked, au.emit_tint, sizeof(au.emit_baked));
	if (from_old)
		author_status("made %s (%dx%d), brightness read from %s (%d texels glow); emissive_factor %g -> %g "
			"(the map holds half) - Save writes both into the .mat", path, W, H, was, lit, factor, new_factor);
	else
		author_status("made %s (%dx%d), the texture at 0%%; emissive_factor %g -> %g (the map holds half) - "
			"Save writes both into the .mat", path, W, H, factor, new_factor);
	return img;
}

// a stand-in map's texel (map_is_placeholder) as the new map's, W x H: its values, nearest
static void placeholder_texel(const image_t *was, int slot, int x, int y, int W, int H, byte p[4])
{
	const int sx = (int)((int64_t)x * was->upload_width / W), sy = (int)((int64_t)y * was->upload_height / H);
	const size_t at = (size_t)sy * was->upload_width + sx;
	if (was->pixel_format == PF_R16_UNORM)
	{
		p[0] = p[1] = p[2] = to_byte((float)((const uint16_t *)was->pix_data)[at] / 65535.f);
		p[3] = 255;
		return;
	}
	const byte *q = was->pix_data + at * 4;
	if (slot_is_grey(slot))
	{
		// the new map loads linear, as every grey one does
		float v = (float)q[0] / 255.f;
		if (was->is_srgb)
			v = srgb_to_linear(v);
		p[0] = p[1] = p[2] = to_byte(v);
		p[3] = 255;
		return;
	}
	memcpy(p, q, 4);
}

static image_t *create_map(void)
{
	pbr_material_t *mat = au.mat;
	if (au.slot == SLOT_EMISSIVE && au.stage < 0)
		return create_emit_map();

	const image_t *base = author_base();
	int W, H;
	map_size(&W, &H);

	// a stand-in the material had: this map starts from its values, the material's own
	const image_t *was = au.stage < 0 ? slot_image(mat, au.slot) : NULL;
	if (was && (!was->pix_data || was->upload_width <= 0 || was->upload_height <= 0
		|| (was->pixel_format != PF_R8G8B8A8_UNORM && was->pixel_format != PF_R16_UNORM)))
		was = NULL;
	char was_name[MAX_QPATH];
	Q_strlcpy(was_name, was ? (was->filepath[0] ? was->filepath : was->name) : "", sizeof(was_name));

	byte *rgba = Z_Malloc((size_t)W * H * 4);
	const bool whole = MAT_RoughnessIsWhole(mat);
	const byte rough_whole = to_byte(mat->roughness_override);
	// a stage's: one value all over (stored as the new PNG's linear bytes)
	const byte stage_value = (au.stage >= 0 && au.slot <= SLOT_METAL) ? to_byte(stage_start_value()) : 0;
	// a height map starts flat at the surface: white, or in / (in + out) when white rises above it
	const byte surface = to_byte(height_surface_level(mat));
	for (int y = 0; y < H; y++)
	{
		for (int x = 0; x < W; x++)
		{
			byte *p = rgba + ((size_t)y * W + x) * 4;
			if (was)
			{
				placeholder_texel(was, au.slot, x, y, W, H, p);
				continue;
			}
			switch (au.slot)
			{
			case SLOT_ROUGH:
				if (au.stage >= 0)
					p[0] = stage_value;
				else
					p[0] = whole ? rough_whole : (mat->image_normals ? alpha_at(base, x, y, W, H) : 255);
				p[1] = p[2] = p[0];
				p[3] = 255;
				break;
			case SLOT_METAL:
				if (au.stage >= 0)
					p[0] = stage_value;
				else
					p[0] = mat->image_normals ? alpha_at(mat->image_normals, x, y, W, H) : 0;
				p[1] = p[2] = p[0];
				p[3] = 255;
				break;
			case SLOT_NORMAL:
				p[0] = 128;
				p[1] = 128;
				p[2] = 255;
				p[3] = 0;
				break;
			case SLOT_HEIGHT:
				p[0] = p[1] = p[2] = surface;
				p[3] = 255;
				break;
			case SLOT_GLASS:
				// all glass: what no mask means, so making it changes nothing
				p[0] = p[1] = p[2] = 255;
				p[3] = 255;
				break;
			default:
				p[0] = p[1] = p[2] = 0;
				p[3] = 255;
				break;
			}
		}
	}

	char path[MAX_OSPATH];
	const bool ok = write_new_map(rgba, W, H, base ? base->name : mat->name, slot_suffix[au.slot], path, sizeof(path));
	Z_Free(rgba);
	if (!ok)
	{
		author_status("could not write %s", path);
		return NULL;
	}

	if (au.stage >= 0)
		MAT_CommandFor(mat, va("stage%d %s \"%s\"", au.stage, slot_attribute[au.slot], path));
	else
		MAT_CommandFor(mat, va("%s \"%s\"", slot_attribute[au.slot], path));
	image_t *img = author_image();
	if (!img || !img->pix_data)
	{
		author_status("wrote %s, but %s did not load it", path, mat->name);
		return NULL;
	}
	au.attr_changed = true;
	if (au.stage >= 0)
		author_status("%s stage%d had no %s map: made %s (%dx%d) - Save writes stage%d_%s into the .mat",
			mat->name, au.stage, slot_word[au.slot], path, img->upload_width, img->upload_height, au.stage,
			slot_attribute[au.slot]);
	else if (was_name[0])
		author_status("%s's %s map was a stand-in (%s): made its own, %s (%dx%d), from it - Save writes it into "
			"the .mat", mat->name, slot_word[au.slot], was_name, path, img->upload_width, img->upload_height);
	else
		author_status("%s had no %s map: made %s (%dx%d) - Save writes it into the .mat", mat->name,
			slot_word[au.slot], path, img->upload_width, img->upload_height);
	return img;
}

/* ------------------------------------------------------------------------- */
/* SAVE                                                                      */
/* ------------------------------------------------------------------------- */

// (mat) draws (img) as its (slot) map - in its Quake III stage (stage) when that is >= 0
static bool material_draws(const pbr_material_t *mat, const image_t *img, int slot, int stage)
{
	if (stage >= 0)
	{
		const char *file;
		return slot <= SLOT_NORMAL && vkpt_q3_stage_image(mat, stage, slot + 1, &file) == img;
	}
	return slot_image(mat, slot) == img;
}

// A material that draws (img) is a Quake III one: its new PNG goes into baseq3/
static bool image_for_quake3(const image_t *img, int slot, int stage)
{
	for (int k = 0; k < MAX_PBR_MATERIALS; k++)
	{
		const pbr_material_t *m2 = MAT_ForIndex(k);
		if (m2 && m2->registration_sequence && material_draws(m2, img, slot, stage) && MAT_IsQuake3(m2))
			return true;
	}
	return false;
}

static void author_save(void)
{
	stroke_end();
	upload_now();

	int saved = 0, failed = 0, repointed = 0;
	for (int i = 0; i < MAX_RIMAGES; i++)
	{
		if (!image_dirty[i])
			continue;
		const int slot = image_dirty[i] - 1;
		image_t *img = r_images + i;
		if (!img->registration_sequence || !img->pix_data)
		{
			image_dirty[i] = 0;
			continue;
		}

		char game_path[MAX_IMAGE_PATH], ospath[MAX_OSPATH];
		Q_strlcpy(game_path, img->filepath[0] ? img->filepath : img->name, sizeof(game_path));
		const char *ext = COM_FileExtension(game_path);
		const bool png = !Q_stricmp(ext, ".png"), tga = !Q_stricmp(ext, ".tga");
		const bool loose = FS_RealPath(game_path, FS_TYPE_REAL, ospath, sizeof(ospath)) == FS_REALPATH_DISK;

		membuf_t m = { 0 };
		bool backed_up = false;

		if (loose && (png || (tga && img->pixel_format != PF_R16_UNORM)))
		{
			// in place, the old file kept as .bak
			const bool ok = encode_image(img, tga, &m) && write_with_backup(ospath, &m, &backed_up);
			Z_Free(m.data);
			if (!ok)
			{
				Com_WPrintf("mat author: could not write %s\n", ospath);
				failed++;
				continue;
			}
			FS_LastModified(game_path, &img->last_modified);
			image_dirty[i] = 0;
			saved++;
			Com_Printf("mat author: saved %s%s\n", ospath, backed_up ? " (the old one is .bak)" : "");
			continue;
		}

		// a pak's, a JPG, a picture made in memory: a new PNG, and the materials pointed at it
		const int stage = image_stage[i] - 1;
		char new_path[MAX_OSPATH];
		dev_png_path(game_path, "", new_path, sizeof(new_path));
		const bool ok = encode_image(img, false, &m)
			&& game_path_to_os(new_path, image_for_quake3(img, slot, stage), ospath, sizeof(ospath))
			&& write_with_backup(ospath, &m, &backed_up);
		Z_Free(m.data);
		if (!ok)
		{
			Com_WPrintf("mat author: could not write %s\n", new_path);
			failed++;
			continue;
		}
		Com_Printf("mat author: %s is %s - saved as %s\n", game_path,
			loose ? "not a PNG or TGA" : "in a pack", ospath);
		{
			// baseq3 is searched last: an older copy of that name higher up (one
			// saved into rerelease/ before 11-21) is what the material would load
			char found[MAX_OSPATH];
			if (FS_RealPath(new_path, 0, found, sizeof(found)) == FS_REALPATH_DISK && Q_stricmp(found, ospath))
				Com_WPrintf("mat author: %s is hidden by %s - move or delete that one\n", ospath, found);
		}

		const image_t *loaded = NULL;
		for (int k = 0; k < MAX_PBR_MATERIALS; k++)
		{
			pbr_material_t *m2 = MAT_ForIndex(k);
			if (!m2 || !m2->registration_sequence)
				continue;
			if (stage >= 0)
			{
				// a Quake III stage's own map: every material whose stage N draws it
				const char *file;
				if (!material_draws(m2, img, slot, stage))
					continue;
				MAT_CommandFor(m2, va("stage%d %s \"%s\"", stage, slot_attribute[slot], new_path));
				const image_t *now = vkpt_q3_stage_image(m2, stage, slot + 1, &file);
				if (now && now != img)
				{
					loaded = now;
					repointed++;
				}
				continue;
			}
			if (slot_image(m2, slot) != img)
				continue;
			MAT_CommandFor(m2, va("%s \"%s\"", slot_attribute[slot], new_path));
			if (slot_image(m2, slot) && slot_image(m2, slot) != img)
			{
				loaded = slot_image(m2, slot);
				repointed++;
			}
		}
		image_dirty[i] = 0;
		image_stage[i] = 0;
		if (loaded)
		{
			undo_remap(i, loaded);
			au.attr_changed = true;
		}
		saved++;
	}

	if (au.attr_changed)
	{
		// the .mat entries: the maps made or moved (everything 'mat save' would write)
		MAT_SaveToSourceFiles();
		au.attr_changed = false;
	}

	if (failed)
		author_status("saved %d map(s), %d FAILED - see the console", saved, failed);
	else if (saved)
		author_status("saved %d map(s)%s", saved, repointed ? ", materials pointed at the new files" : "");
	else
		author_status("nothing painted to save");
}

/* ------------------------------------------------------------------------- */
/* THE LOCK: F A FACE, P A PIECE                                             */
/* ------------------------------------------------------------------------- */

static bool hit_on_map(const author_hit_t *hit, const image_t *img);

/* Triangles to find the one under the cursor in, and to grow a lock across:
   the world's with the material, and the model meshes that hold that triangle. */
typedef struct {
	int n, cap;
	float *pos;                     // 9 a triangle
	float *uv;                      // 6 a triangle
	int *group;                     // corners weld only within a group: 0 the world, else a model mesh
	bool *owned;                    // the world, or a mesh whose own skins include the material
	const maliasmesh_t **meshes;    // group g's mesh at g - 1
	int num_meshes, cap_meshes;
} soup_t;

static void soup_add(soup_t *s, const float *p0, const float *p1, const float *p2,
	const float *t0, const float *t1, const float *t2, int group, bool owned)
{
	if (s->n == s->cap)
	{
		s->cap = max(1024, s->cap * 2);
		s->pos = Z_Realloc(s->pos, sizeof(float) * 9 * s->cap);
		s->uv = Z_Realloc(s->uv, sizeof(float) * 6 * s->cap);
		s->group = Z_Realloc(s->group, sizeof(int) * s->cap);
		s->owned = Z_Realloc(s->owned, sizeof(bool) * s->cap);
	}
	float *p = s->pos + 9 * s->n, *t = s->uv + 6 * s->n;
	VectorCopy(p0, p);
	VectorCopy(p1, p + 3);
	VectorCopy(p2, p + 6);
	t[0] = t0[0];
	t[1] = t0[1];
	t[2] = t1[0];
	t[3] = t1[1];
	t[4] = t2[0];
	t[5] = t2[1];
	s->group[s->n] = group;
	s->owned[s->n] = owned;
	s->n++;
}

static void soup_free(soup_t *s)
{
	Z_Free(s->pos);
	Z_Free(s->uv);
	Z_Free(s->group);
	Z_Free(s->owned);
	Z_Free((void *)s->meshes);
	memset(s, 0, sizeof(*s));
}

static bool near_equal(float a, float b, float eps)
{
	return fabsf(a - b) <= eps * max(1.f, fabsf(a));
}

// triangles (a) and (b), (dim) floats a corner, the same: b's corners may start anywhere or run backwards
static bool tri_match(const float *a, const float *b, int dim, float eps)
{
	for (int flip = 0; flip < 2; flip++)
	{
		for (int rot = 0; rot < 3; rot++)
		{
			bool ok = true;
			for (int k = 0; k < 3 && ok; k++)
			{
				const int j = flip ? (rot + 3 - k) % 3 : (rot + k) % 3;
				for (int d = 0; d < dim && ok; d++)
					ok = near_equal(a[k * dim + d], b[j * dim + d], eps);
			}
			if (ok)
				return true;
		}
	}
	return false;
}

// (u, v) inside UV triangle (t)
static bool uv_in_tri(const float *t, float u, float v)
{
	const float e0 = (t[2] - t[0]) * (v - t[1]) - (t[3] - t[1]) * (u - t[0]);
	const float e1 = (t[4] - t[2]) * (v - t[3]) - (t[5] - t[3]) * (u - t[2]);
	const float e2 = (t[0] - t[4]) * (v - t[5]) - (t[1] - t[5]) * (u - t[4]);
	return (e0 >= 0.f && e1 >= 0.f && e2 >= 0.f) || (e0 <= 0.f && e1 <= 0.f && e2 <= 0.f);
}

static void soup_add_mesh(soup_t *s, const maliasmesh_t *mesh, bool owned)
{
	if (s->num_meshes == s->cap_meshes)
	{
		s->cap_meshes = max(16, s->cap_meshes * 2);
		s->meshes = Z_Realloc((void *)s->meshes, sizeof(*s->meshes) * s->cap_meshes);
	}
	s->meshes[s->num_meshes++] = mesh;
	const int group = s->num_meshes;
	for (int t = 0; t < mesh->numtris; t++)
	{
		const int i0 = mesh->indices[t * 3], i1 = mesh->indices[t * 3 + 1], i2 = mesh->indices[t * 3 + 2];
		if (i0 < 0 || i1 < 0 || i2 < 0 || i0 >= mesh->numverts || i1 >= mesh->numverts || i2 >= mesh->numverts)
			continue;
		soup_add(s, mesh->positions[i0], mesh->positions[i1], mesh->positions[i2],
			mesh->tex_coords[i0], mesh->tex_coords[i1], mesh->tex_coords[i2], group, owned);
	}
}

static bool mesh_usable(const maliasmesh_t *mesh)
{
	return mesh->indices && mesh->positions && mesh->tex_coords && mesh->numverts > 0 && mesh->numtris > 0;
}

static bool mesh_owns(const maliasmesh_t *mesh, const pbr_material_t *mat)
{
	for (int sk = 0; sk < MAX_ALIAS_SKINS; sk++)
		if (mesh->materials[sk] == mat)
			return true;
	return false;
}

/* The world's triangles with the hit's material (bsp_mesh: brush models too, as
   built), and every model mesh (frame 0, or the bind pose) holding a triangle
   with the hit triangle's UVs: a player's skin is not one of its mesh's own
   materials, so any mesh may be the one. If none does, the meshes that own the
   material and have a triangle round the hit's UV. */
static void soup_gather(const author_hit_t *hit, soup_t *s)
{
	const pbr_material_t *mat = MAT_ForIndex(hit->material);

	if (vkpt_refdef.bsp_mesh_world_loaded && vkpt_refdef.bsp_mesh_world.primitives)
	{
		const bsp_mesh_t *wm = &vkpt_refdef.bsp_mesh_world;
		for (uint32_t i = 0; i < wm->num_primitives; i++)
		{
			const VboPrimitive *p = wm->primitives + i;
			if ((int)(p->material_id & MATERIAL_INDEX_MASK) != hit->material)
				continue;
			soup_add(s, p->pos0, p->pos1, p->pos2, p->uv0, p->uv1, p->uv2, 0, true);
		}
	}

	for (int pass = 0; pass < 2; pass++)
	{
		bool any = false;
		for (int m = 0; m < r_numModels; m++)
		{
			const model_t *model = &r_models[m];
			if (model->type != MOD_ALIAS || !model->registration_sequence || !model->meshes)
				continue;
			for (int k = 0; k < model->nummeshes; k++)
			{
				const maliasmesh_t *mesh = &model->meshes[k];
				if (!mesh_usable(mesh))
					continue;
				const bool owned = mesh_owns(mesh, mat);
				if (pass == 1 && !owned)
					continue;
				bool found = false;
				for (int t = 0; t < mesh->numtris && !found; t++)
				{
					const int i0 = mesh->indices[t * 3], i1 = mesh->indices[t * 3 + 1], i2 = mesh->indices[t * 3 + 2];
					if (i0 < 0 || i1 < 0 || i2 < 0 || i0 >= mesh->numverts || i1 >= mesh->numverts || i2 >= mesh->numverts)
						continue;
					const float uv[6] = {
						mesh->tex_coords[i0][0], mesh->tex_coords[i0][1],
						mesh->tex_coords[i1][0], mesh->tex_coords[i1][1],
						mesh->tex_coords[i2][0], mesh->tex_coords[i2][1],
					};
					found = pass == 0 ? tri_match(uv, hit->tri_uv, 2, 1e-4f) : uv_in_tri(uv, hit->u, hit->v);
				}
				if (found)
				{
					soup_add_mesh(s, mesh, owned);
					any = true;
				}
			}
		}
		if (any)
			break;
	}
}

/* The triangles the cursor is on: matching the hit triangle's UVs and corners
   (the world, where it was built), else its UVs (a model, posed elsewhere) - in a
   mesh that owns the material if one does - else round the hit's UV. Mirrored
   halves share UVs: both are found, and both share their texels anyway. */
static int find_seeds(const soup_t *s, const author_hit_t *hit, byte *seed)
{
	byte *score = Z_Mallocz(max(s->n, 1));
	int best = 0;
	for (int t = 0; t < s->n; t++)
	{
		int sc = 0;
		if (tri_match(s->uv + t * 6, hit->tri_uv, 2, 1e-4f))
			sc |= 2;
		if (s->group[t] == 0 && tri_match(s->pos + t * 9, hit->tri_pos, 3, 1e-3f))
			sc |= 4;
		if (sc && s->owned[t])
			sc |= 1;
		score[t] = (byte)sc;
		best = max(best, sc);
	}
	if (!best)
	{
		for (int t = 0; t < s->n; t++)
		{
			score[t] = uv_in_tri(s->uv + t * 6, hit->u, hit->v) ? (byte)(1 + s->owned[t]) : 0;
			best = max(best, (int)score[t]);
		}
	}
	int n = 0;
	for (int t = 0; t < s->n; t++)
	{
		seed[t] = best && score[t] == best;
		n += seed[t];
	}
	Z_Free(score);
	return n;
}

/* one id per welded corner: (data) holds (dim) floats a corner - its position
   (weld_corners) or its UV (a line's island) - and corners within (quantum) of
   each other, in the same group, share one */
static int *weld_by(const soup_t *s, const float *data, int dim, float quantum)
{
	const int nc = s->n * 3;
	int *id = Z_Malloc(sizeof(int) * max(nc, 1));
	int *q = Z_Malloc(sizeof(int) * 3 * max(nc, 1));
	int hsize = 16;
	while (hsize < nc * 2)
		hsize <<= 1;
	int *table = Z_Mallocz(sizeof(int) * hsize);   // corner + 1; 0 = empty
	int next = 0;
	for (int c = 0; c < nc; c++)
	{
		const float *p = data + c * dim;
		int *qc = q + c * 3;
		for (int k = 0; k < 3; k++)
			qc[k] = k < dim ? (int)floorf(p[k] / quantum + 0.5f) : 0;
		const int g = s->group[c / 3];
		uint32_t slot = hash32((uint32_t)qc[0] * 73856093u ^ (uint32_t)qc[1] * 19349663u
			^ (uint32_t)qc[2] * 83492791u ^ (uint32_t)g * 2654435761u) & (uint32_t)(hsize - 1);
		for (;;)
		{
			const int o = table[slot];
			if (!o)
			{
				table[slot] = c + 1;
				id[c] = next++;
				break;
			}
			const int oc = o - 1;
			const int *qo = q + oc * 3;
			if (qo[0] == qc[0] && qo[1] == qc[1] && qo[2] == qc[2] && s->group[oc / 3] == g)
			{
				id[c] = id[oc];
				break;
			}
			slot = (slot + 1) & (uint32_t)(hsize - 1);
		}
	}
	Z_Free(table);
	Z_Free(q);
	return id;
}

// one id per welded corner: within LOCK_WELD of each other, in the same group
static int *weld_corners(const soup_t *s)
{
	return weld_by(s, s->pos, 3, LOCK_WELD);
}

typedef struct {
	uint64_t key;
	int tri;
} keyed_t;

static int keyed_cmp(const void *a, const void *b)
{
	const uint64_t ka = ((const keyed_t *)a)->key, kb = ((const keyed_t *)b)->key;
	return ka < kb ? -1 : ka > kb;
}

// the first entry with (key), or n
static int keyed_find(const keyed_t *k, int n, uint64_t key)
{
	int lo = 0, hi = n;
	while (lo < hi)
	{
		const int mid = (lo + hi) / 2;
		if (k[mid].key < key)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static uint64_t edge_key(int a, int b)
{
	return a < b ? ((uint64_t)(uint32_t)a << 32) | (uint32_t)b : ((uint64_t)(uint32_t)b << 32) | (uint32_t)a;
}

/* (sel) grows from its seeds: a face across shared edges, to triangles facing
   within LOCK_FACE_DEG of the seed they grew from - so a curve stops after a
   few degrees, not wherever its facets add up; a piece across any shared corner;
   'u', a UV island, across any shared edge (with (id) welding corners by UV). */
static void grow(const soup_t *s, const int *id, int kind, byte *sel)
{
	const int n = s->n;
	float *nrm = Z_Malloc(sizeof(float) * 3 * max(n, 1));
	for (int t = 0; t < n; t++)
	{
		const float *p = s->pos + t * 9;
		vec3_t e1, e2, c;
		VectorSubtract(p + 3, p, e1);
		VectorSubtract(p + 6, p, e2);
		CrossProduct(e1, e2, c);
		const float len = VectorLength(c);
		if (len > 1e-12f)
			VectorScale(c, 1.f / len, c);
		VectorCopy(c, nrm + t * 3);
	}

	const int nk = n * 3;
	keyed_t *keys = Z_Malloc(sizeof(keyed_t) * max(nk, 1));
	for (int t = 0; t < n; t++)
	{
		for (int e = 0; e < 3; e++)
		{
			const int a = id[t * 3 + e], b = id[t * 3 + (e + 1) % 3];
			keys[t * 3 + e].key = kind != 'p' ? edge_key(a, b) : (uint64_t)(uint32_t)a;
			keys[t * 3 + e].tri = t;
		}
	}
	qsort(keys, nk, sizeof(keyed_t), keyed_cmp);

	int *queue = Z_Malloc(sizeof(int) * max(n, 1));
	int *ref = Z_Malloc(sizeof(int) * max(n, 1));
	int qh = 0, qt = 0;
	for (int t = 0; t < n; t++)
	{
		if (sel[t])
		{
			queue[qt++] = t;
			ref[t] = t;
		}
	}
	const float cos_max = cosf(LOCK_FACE_DEG * (float)M_PI / 180.f);
	while (qh < qt)
	{
		const int t = queue[qh++];
		for (int e = 0; e < 3; e++)
		{
			const int a = id[t * 3 + e], b = id[t * 3 + (e + 1) % 3];
			const uint64_t key = kind != 'p' ? edge_key(a, b) : (uint64_t)(uint32_t)a;
			for (int i = keyed_find(keys, nk, key); i < nk && keys[i].key == key; i++)
			{
				const int u = keys[i].tri;
				if (sel[u])
					continue;
				if (kind == 'f' && DotProduct(nrm + u * 3, nrm + ref[t] * 3) < cos_max)
					continue;
				sel[u] = 1;
				ref[u] = ref[t];
				queue[qt++] = u;
			}
		}
	}
	Z_Free(queue);
	Z_Free(ref);
	Z_Free(keys);
	Z_Free(nrm);
}

static void lock_push(lock_tri_t **arr, int *n, int *cap, const float *uv)
{
	if (*n == *cap)
	{
		*cap = max(256, *cap * 2);
		*arr = Z_Realloc(*arr, sizeof(lock_tri_t) * *cap);
	}
	memcpy((*arr)[*n].uv, uv, sizeof((*arr)[*n].uv));
	(*n)++;
}

static int lock_find(const float *uv)
{
	for (int i = 0; i < lk.num_tris; i++)
		if (!memcmp(lk.tris[i].uv, uv, sizeof(lk.tris[i].uv)))
			return i;
	return -1;
}

static void lock_clear(const char *why)
{
	const bool was = lk.on;
	Z_Free(lk.tris);
	Z_Free(lk.cover);
	Z_Free(lk.mask);
	lk.tris = lk.cover = NULL;
	lk.mask = NULL;
	lk.num_tris = lk.cap_tris = lk.num_cover = lk.cap_cover = 0;
	lk.num_cover_meshes = 0;
	lk.mask_w = lk.mask_h = lk.texels = 0;
	lk.on = false;
	lk.dirty = false;
	rg.dirty = true;    // the selection, if one is on, alone now
	if (was && why)
		author_status("%s", why);
}

static int wrap_i(int a, int n)
{
	return ((a % n) + n) % n;
}

/* (uv)'s triangle into (mask), W x H, wrapped: the texels whose centres it
   covers, and always the one its centroid is in, so a sliver locks a texel. */
static void raster_tri(const float *uv, int W, int H, byte *mask)
{
	float x[3], y[3];
	for (int k = 0; k < 3; k++)
	{
		x[k] = uv[k * 2] * (float)W;
		y[k] = uv[k * 2 + 1] * (float)H;
	}
	mask[(size_t)wrap_i((int)floorf((y[0] + y[1] + y[2]) / 3.f), H) * W
		+ wrap_i((int)floorf((x[0] + x[1] + x[2]) / 3.f), W)] = 1;

	const float area2 = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
	if (fabsf(area2) < 1e-9f)
		return;
	// tiled over the whole map (a big world face): all of it
	if (fabsf(area2) * 0.5f >= 2.f * (float)W * (float)H)
	{
		memset(mask, 1, (size_t)W * H);
		return;
	}
	const int x0 = (int)floorf(min(x[0], min(x[1], x[2]))), x1 = (int)ceilf(max(x[0], max(x[1], x[2])));
	const int y0 = (int)floorf(min(y[0], min(y[1], y[2]))), y1 = (int)ceilf(max(y[0], max(y[1], y[2])));
	if ((int64_t)(x1 - x0) * (y1 - y0) > (int64_t)64 * W * H)
	{
		memset(mask, 1, (size_t)W * H);
		return;
	}
	for (int ty = y0; ty < y1; ty++)
	{
		const float py = (float)ty + 0.5f;
		byte *row = mask + (size_t)wrap_i(ty, H) * W;
		for (int tx = x0; tx < x1; tx++)
		{
			const float px = (float)tx + 0.5f;
			const float e0 = (x[1] - x[0]) * (py - y[0]) - (y[1] - y[0]) * (px - x[0]);
			const float e1 = (x[2] - x[1]) * (py - y[1]) - (y[2] - y[1]) * (px - x[1]);
			const float e2 = (x[0] - x[2]) * (py - y[2]) - (y[0] - y[2]) * (px - x[2]);
			if ((e0 >= 0.f && e1 >= 0.f && e2 >= 0.f) || (e0 <= 0.f && e1 <= 0.f && e2 <= 0.f))
				row[wrap_i(tx, W)] = 1;
		}
	}
}

/* The lock for a W x H map. Texels whose centres its triangles cover; then, on a
   model, LOCK_BLEED texels further into texels NO triangle of the mesh uses - the
   space round a UV island - so filtering at a seam finds paint, not the old value,
   while a neighbouring face's texels inside the island are left alone. */
// (uv)'s triangle onto the map being painted: a stage's through its tcMods
static void raster_map_tri(const float *uv, int W, int H, byte *mask)
{
	if (au.stage < 0)
	{
		raster_tri(uv, W, H, mask);
		return;
	}
	float st[6];
	for (int k = 0; k < 3; k++)
		map_uv(uv[k * 2], uv[k * 2 + 1], &st[k * 2], &st[k * 2 + 1]);
	raster_tri(st, W, H, mask);
}

// (tris)' texels on a W x H map, bled past their edge into texels none of (cover) uses (see above)
static byte *tris_rasterize(const lock_tri_t *tris, int num_tris, const lock_tri_t *cover_tris, int num_cover,
	int W, int H, int *texels_out)
{
	const size_t n = (size_t)W * H;
	byte *mask = Z_Mallocz(n);
	stage_info(NULL, NULL);     // the stage's tcMods as they are now
	for (int i = 0; i < num_tris; i++)
		raster_map_tri(tris[i].uv, W, H, mask);

	if (num_cover)
	{
		byte *cover = Z_Mallocz(n);
		for (int i = 0; i < num_cover; i++)
			raster_map_tri(cover_tris[i].uv, W, H, cover);
		byte *prev = Z_Malloc(n);
		for (int pass = 0; pass < LOCK_BLEED; pass++)
		{
			memcpy(prev, mask, n);
			for (int y = 0; y < H; y++)
			{
				for (int x = 0; x < W; x++)
				{
					const size_t at = (size_t)y * W + x;
					if (mask[at] || cover[at])
						continue;
					if (prev[(size_t)y * W + wrap_i(x + 1, W)] || prev[(size_t)y * W + wrap_i(x - 1, W)]
						|| prev[(size_t)wrap_i(y + 1, H) * W + x] || prev[(size_t)wrap_i(y - 1, H) * W + x])
						mask[at] = 1;
				}
			}
		}
		Z_Free(prev);
		Z_Free(cover);
	}

	int texels = 0;
	for (size_t i = 0; i < n; i++)
		texels += mask[i];
	*texels_out = texels;
	return mask;
}

static void lock_rasterize(int W, int H)
{
	int texels;
	byte *mask = tris_rasterize(lk.tris, lk.num_tris, lk.cover, lk.num_cover, W, H, &texels);
	Z_Free(lk.mask);
	lk.mask = mask;
	lk.mask_w = W;
	lk.mask_h = H;
	lk.texels = texels;
}

/* The region (the lock and the selection) as a texture, for the shader and the
   map view (see rg). Remade in place while its size holds; a new size is a new
   image. */
static void region_upload(int W, int H)
{
	if (W <= 0 || H <= 0)
		return;
	rg.version++;
	image_t *img = region_image_ok() && rg.w == W && rg.h == H ? r_images + rg.image : NULL;
	byte *px = img ? img->pix_data : IMG_AllocPixels((size_t)W * H * 4);
	int texels = 0;
	for (int y = 0; y < H; y++)
	{
		for (int x = 0; x < W; x++)
		{
			byte *p = px + ((size_t)y * W + x) * 4;
			const bool in = region_allows(x, y, W, H);
			p[0] = in ? 255 : 0;
			p[1] = p[2] = 0;
			p[3] = in ? 0 : 150;
			texels += in;
		}
	}
	rg.texels = texels;
	if (img)
	{
		IMG_PixelsEdited(img);
		return;
	}

	if (rg.image > 0 && rg.image < MAX_RIMAGES && r_images[rg.image].registration_sequence
		&& !strcmp(r_images[rg.image].name, rg.image_name))
		R_UnregisterImage(rg.image);
	Q_snprintf(rg.image_name, sizeof(rg.image_name), "**mat_author_lock_%d**", ++rg.image_seq);
	rg.image = R_RegisterRawImage(rg.image_name, W, H, px, IT_SPRITE, IF_NEAREST | IF_NO_MIPMAPS);
	rg.w = W;
	rg.h = H;
	if (!rg.image)
	{
		IMG_FreePixels(px);
		rg.image_name[0] = 0;
		rg.upload_failed = true;
		author_status("couldn't make the %s's texture: it won't show, but it still holds", region_name());
	}
}

static void sel_rasterize(int W, int H);

/* The lock and the selection, for the map being painted now: each tick, before
   any painting. A selection being dragged out is rasterised a few times a
   second (SEL_PREVIEW_MS), so the surface shows it as it grows. */
static void region_sync(void)
{
	if (!region_on())
	{
		if (rg.dirty)
		{
			rg.dirty = false;
			rg.version++;
		}
		return;
	}
	int W, H;
	map_size(&W, &H);
	bool remade = rg.dirty;
	if (lk.on && (lk.dirty || !lk.mask || lk.mask_w != W || lk.mask_h != H))
	{
		lock_rasterize(W, H);
		lk.dirty = false;
		remade = true;
	}
	if (sel_on() && (sl.dirty || !sl.mask || sl.mask_w != W || sl.mask_h != H))
	{
		const unsigned now = Sys_Milliseconds();
		const bool wait = sl.dragging && !sl.drag_commit && sl.mask && sl.mask_w == W && sl.mask_h == H
			&& now - sl.raster_ms < SEL_PREVIEW_MS;
		if (!wait)
		{
			sel_rasterize(W, H);
			sl.dirty = false;
			sl.raster_ms = now;
			remade = true;
		}
	}
	if (remade)
	{
		rg.dirty = false;
		rg.upload_failed = false;
		region_upload(W, H);
	}
	else if (!region_image_ok() && !rg.upload_failed)
		region_upload(W, H);   // its texture went (a vid_restart)
}

/* F or P (kind), Shift (toggle): lock to the face or piece under the cursor; F/P
   on the lock unlocks; with Shift it is added to the lock, or taken out of it if
   it is all in it already. Between frames (author_tick). */
static void lock_command(int kind, bool toggle)
{
	const char *what = kind == 'f' ? "face" : "piece";
	if (!au.hit.valid)
	{
		if (!toggle && lk.on)
			lock_clear("unlocked");
		else
			author_status("nothing under the cursor to lock");
		return;
	}
	if (!hit_on_map(&au.hit, author_image()))
	{
		author_status("the cursor isn't on %s's %s map", au.mat->name, slot_word[au.slot]);
		return;
	}

	int W, H;
	map_size(&W, &H);
	if (!toggle && lk.on && lk.mask && lk.mask_w == W && lk.mask_h == H)
	{
		float hu, hv;
		hit_texel(&au.hit, W, H, true, &hu, &hv);
		const int x = Q_clip((int)floorf(hu), 0, W - 1);
		const int y = Q_clip((int)floorf(hv), 0, H - 1);
		if (lk.mask[(size_t)y * W + x])
		{
			lock_clear("unlocked");
			return;
		}
	}

	soup_t s;
	memset(&s, 0, sizeof(s));
	soup_gather(&au.hit, &s);
	byte *sel = Z_Mallocz(max(s.n, 1));
	if (!s.n || !find_seeds(&s, &au.hit, sel))
	{
		author_status("couldn't find the triangle under the cursor in the world or a model");
		Z_Free(sel);
		soup_free(&s);
		return;
	}
	int *id = weld_corners(&s);
	grow(&s, id, kind, sel);
	Z_Free(id);

	if (!toggle || !lk.on || lk.material != au.hit.material)
		lock_clear(NULL);

	int picked = 0, already = 0;
	for (int t = 0; t < s.n; t++)
	{
		if (!sel[t])
			continue;
		picked++;
		if (lock_find(s.uv + t * 6) >= 0)
			already++;
	}

	const bool removing = toggle && lk.on && picked && already == picked;
	if (removing)
	{
		for (int t = 0; t < s.n; t++)
		{
			const int i = sel[t] ? lock_find(s.uv + t * 6) : -1;
			if (i >= 0)
				lk.tris[i] = lk.tris[--lk.num_tris];
		}
	}
	else
	{
		for (int t = 0; t < s.n; t++)
			if (sel[t] && lock_find(s.uv + t * 6) < 0)
				lock_push(&lk.tris, &lk.num_tris, &lk.cap_tris, s.uv + t * 6);

		// the meshes it came from: their texels, for the bleed past a seam
		for (int t = 0; t < s.n; t++)
		{
			if (!sel[t] || s.group[t] == 0)
				continue;
			const maliasmesh_t *mesh = s.meshes[s.group[t] - 1];
			bool have = false;
			for (int m = 0; m < lk.num_cover_meshes && !have; m++)
				have = lk.cover_meshes[m] == mesh;
			if (have || lk.num_cover_meshes >= (int)q_countof(lk.cover_meshes))
				continue;
			lk.cover_meshes[lk.num_cover_meshes++] = mesh;
			for (int u = 0; u < s.n; u++)
				if (s.group[u] == s.group[t])
					lock_push(&lk.cover, &lk.num_cover, &lk.cap_cover, s.uv + u * 6);
		}
	}

	Z_Free(sel);
	soup_free(&s);

	lk.on = lk.num_tris > 0;
	lk.kind = kind;
	lk.material = au.hit.material;
	lk.dirty = true;
	if (!lk.on)
	{
		lock_clear(NULL);
		author_status("unlocked: that was all of the lock");
		return;
	}
	region_sync();
	if (removing)
		author_status("took a %s out of the lock: %d triangles, %d texels left", what, lk.num_tris, lk.texels);
	else if (toggle && already < picked && lk.num_tris > picked - already)
		author_status("added a %s (%d triangles): %d texels locked - Shift+%c again takes it out",
			what, picked - already, lk.texels, kind == 'f' ? 'F' : 'P');
	else
		author_status("locked to a %s: %d triangle%s, %d texels - %c on it again unlocks", what, picked,
			picked == 1 ? "" : "s", lk.texels, kind == 'f' ? 'F' : 'P');
}

/* ------------------------------------------------------------------------- */
/* A LINE STAYS ON ITS PART OF THE MODEL                                     */
/* ------------------------------------------------------------------------- */

/* Matt, 2026-11-22, drawing lines on the Enforcer's helmet: "if the line will
   jump to a different part of the texture, instead of splitting the line on
   the model can we just make it so the line can't be any longer?"

   A line is straight on the MAP, and a model's map is cut into islands - the
   helmet's here, a shoulder's beside it - so a line from a point on one to a
   point on another crosses whatever lies between them on the map, and its
   pieces land all over the model. So when a line starts on a model, the island
   under its start is found: the triangles joined to the one under the cursor
   by edges whose two corners have the same UVs (grow 'u'; mirrored halves that
   share UVs come along, as they share texels anyway), rasterised with the
   lock's bleed past the island's edge (tris_rasterize). While it is drawn, the
   line's end goes no further along it than the island does; with the cursor
   off the island altogether, the end stays where it last was on it.

   Lines only, and only on models: the world tiles its maps (a line on it is
   left as it was), the map view is the map itself, and a rectangle is a
   rectangle on the map whatever it covers. */
#define LINE_UV_WELD        (1.f / 65536.f) // UVs this close are one corner, for the island

static struct {
	bool on;
	bool held;                  // the end is held short of where the cursor is
	byte *mask;                 // 1 = the island's texels, mask_w x mask_h (wrapped)
	int mask_w, mask_h;
} ln;

static void island_clear(void)
{
	Z_Free(ln.mask);
	memset(&ln, 0, sizeof(ln));
}

static bool island_has(float tu, float tv)
{
	const int x = wrap_i((int)floorf(tu), ln.mask_w), y = wrap_i((int)floorf(tv), ln.mask_h);
	return ln.mask[(size_t)y * ln.mask_w + x] != 0;
}

// the island under (hit), the line's start at (tu, tv) on a W x H map; ln.on = found
static void island_build(const author_hit_t *hit, int W, int H, float tu, float tv)
{
	island_clear();

	soup_t s;
	memset(&s, 0, sizeof(s));
	soup_gather(hit, &s);
	byte *sel = Z_Mallocz(max(s.n, 1));
	int seeds = s.n ? find_seeds(&s, hit, sel) : 0;
	for (int t = 0; t < s.n; t++)
	{
		if (sel[t] && s.group[t] == 0)
		{
			sel[t] = 0;     // the world's: its maps tile, and a line on it is left as it was
			seeds--;
		}
	}

	if (seeds > 0)
	{
		int *id = weld_by(&s, s.uv, 2, LINE_UV_WELD);
		grow(&s, id, 'u', sel);
		Z_Free(id);

		// the island's triangles, and every triangle of the meshes it is in (for the bleed)
		lock_tri_t *tris = NULL, *cover = NULL;
		int num_tris = 0, cap_tris = 0, num_cover = 0, cap_cover = 0;
		byte *in_group = Z_Mallocz(s.num_meshes + 1);
		for (int t = 0; t < s.n; t++)
		{
			if (!sel[t])
				continue;
			lock_push(&tris, &num_tris, &cap_tris, s.uv + t * 6);
			in_group[s.group[t]] = 1;
		}
		for (int t = 0; t < s.n; t++)
			if (in_group[s.group[t]])
				lock_push(&cover, &num_cover, &cap_cover, s.uv + t * 6);

		int texels;
		ln.mask = tris_rasterize(tris, num_tris, cover, num_cover, W, H, &texels);
		ln.mask_w = W;
		ln.mask_h = H;
		// the texel the line starts in is on it, whatever the rasteriser made of a sliver there
		ln.mask[(size_t)wrap_i((int)floorf(tv), H) * W + wrap_i((int)floorf(tu), W)] = 1;
		ln.on = true;

		Z_Free(in_group);
		Z_Free(tris);
		Z_Free(cover);
	}

	Z_Free(sel);
	soup_free(&s);
}

/* The line's end for the cursor at (*tu, *tv), texels unwrapped: no further
   along the line from its start (tu0, tv0) than the island goes, a quarter
   texel at a time. False: the cursor is off the island, and the end stays. */
static bool island_clamp(float tu0, float tv0, float *tu, float *tv, bool *held)
{
	*held = false;
	if (!island_has(*tu, *tv))
		return false;

	const float dx = *tu - tu0, dy = *tv - tv0;
	const int steps = (int)ceilf(sqrtf(dx * dx + dy * dy) * 4.f);
	float last_u = tu0, last_v = tv0;
	for (int i = 1; i <= steps; i++)
	{
		const float k = (float)i / (float)steps;
		const float u = tu0 + dx * k, v = tv0 + dy * k;
		if (!island_has(u, v))
		{
			*tu = last_u;
			*tv = last_v;
			*held = true;
			return true;
		}
		last_u = u;
		last_v = v;
	}
	return true;
}

/* ------------------------------------------------------------------------- */
/* THE SELECTION                                                             */
/* ------------------------------------------------------------------------- */

static void sel_free_prev(void)
{
	Z_Free(sl.prev_steps);
	Z_Free(sl.prev_pts);
	sl.prev_steps = NULL;
	sl.prev_pts = NULL;
	sl.prev_num_steps = sl.prev_num_pts = 0;
}

/* The selection is dropped - (why) on the status line, if there was one. Its
   steps are kept for Ctrl+Shift+D (sel_reselect) until the next one goes. */
static void sel_clear(const char *why)
{
	const bool was = sl.num_steps > 0;
	if (was)
	{
		sel_free_prev();
		sl.prev_steps = sl.steps;
		sl.prev_num_steps = sl.num_steps;
		sl.prev_pts = sl.pts;
		sl.prev_num_pts = sl.num_pts;
		sl.prev_material = sl.material;
		sl.prev_stage = sl.stage;
	}
	else
	{
		Z_Free(sl.steps);
		Z_Free(sl.pts);
	}
	sl.steps = NULL;
	sl.pts = NULL;
	sl.num_steps = sl.cap_steps = sl.num_pts = sl.cap_pts = 0;
	sl.dragging = sl.drag_commit = false;
	Z_Free(sl.mask);
	sl.mask = NULL;
	sl.mask_w = sl.mask_h = sl.texels = 0;
	sl.dirty = false;
	sl.stats_have = false;
	rg.dirty = true;
	if (was && why)
		author_status("%s", why);
}

// a step onto the selection: a shape added or taken away (its points, map repeats), all, or invert
static void sel_push(int step, int shape, const float *pts, int count)
{
	if (!sl.num_steps)
	{
		sl.material = MAT_IndexOf(au.mat);
		sl.stage = au.stage;
	}
	if (sl.num_steps == sl.cap_steps)
	{
		sl.cap_steps = max(16, sl.cap_steps * 2);
		sl.steps = Z_Realloc(sl.steps, sizeof(sel_step_t) * sl.cap_steps);
	}
	if (count > 0 && sl.num_pts + count > sl.cap_pts)
	{
		sl.cap_pts = max(sl.num_pts + count, max(256, sl.cap_pts * 2));
		sl.pts = Z_Realloc(sl.pts, sizeof(float) * 2 * sl.cap_pts);
	}
	sel_step_t *s = &sl.steps[sl.num_steps++];
	s->step = step;
	s->shape = shape;
	s->first = sl.num_pts;
	s->count = max(count, 0);
	s->clip = false;
	if (count > 0)
	{
		memcpy(sl.pts + 2 * sl.num_pts, pts, sizeof(float) * 2 * count);
		sl.num_pts += count;
	}
	sl.dirty = true;
	sl.stats_have = false;
	queue_tick();
}

// row (ty) of a W x H mask, texels [x0, x1], set to (v): wrapped on a tiled map, else clipped to it
static void sel_span(byte *mask, int W, int H, bool wrap, int ty, int x0, int x1, byte v)
{
	if (x0 > x1)
		return;
	if (wrap)
		ty = wrap_i(ty, H);
	else
	{
		if (ty < 0 || ty >= H)
			return;
		x0 = max(x0, 0);
		x1 = min(x1, W - 1);
		if (x0 > x1)
			return;
	}
	byte *row = mask + (size_t)ty * W;
	if (x1 - x0 + 1 >= W)
	{
		memset(row, v, W);
		return;
	}
	const int a = wrap ? wrap_i(x0, W) : x0, n = x1 - x0 + 1;
	const int run = min(n, W - a);
	memset(row + a, v, run);
	if (n > run)
		memset(row, v, n - run);
}

static int sel_float_cmp(const void *a, const void *b)
{
	const float x = *(const float *)a, y = *(const float *)b;
	return (x > y) - (x < y);
}

/* A lasso's texels: those whose centres it encloses (even-odd), row by row. Each
   edge's crossings with the rows it spans go in first - a row's centre on
   [low, high) of the edge, so a corner counts once - then each row's are sorted
   and filled between in pairs. The last point joins the first. */
static void sel_lasso(byte *mask, int W, int H, bool wrap, const float *p, int count, byte v)
{
	if (count < 3)
		return;
	float ymin = FLT_MAX, ymax = -FLT_MAX;
	for (int i = 0; i < count; i++)
	{
		const float y = p[i * 2 + 1] * (float)H;
		ymin = min(ymin, y);
		ymax = max(ymax, y);
	}
	const int ty0 = (int)floorf(ymin), ty1 = (int)ceilf(ymax);
	const int rows = ty1 - ty0 + 1;
	if (rows <= 0 || rows > SEL_MAX_TILES * H + 2)
		return;

	int *start = Z_Mallocz(sizeof(int) * (rows + 1));
	int *fill = NULL;
	float *xs = NULL;
	for (int pass = 0; pass < 2; pass++)
	{
		for (int i = 0; i < count; i++)
		{
			const int j = i + 1 < count ? i + 1 : 0;
			const float ax = p[i * 2] * (float)W, ay = p[i * 2 + 1] * (float)H;
			const float bx = p[j * 2] * (float)W, by = p[j * 2 + 1] * (float)H;
			if (ay == by)
				continue;
			const float lo = min(ay, by), hi = max(ay, by);
			const int r0 = (int)ceilf(lo - 0.5f), r1 = (int)ceilf(hi - 0.5f) - 1;
			for (int ty = r0; ty <= r1; ty++)
			{
				const int r = ty - ty0;
				if (r < 0 || r >= rows)
					continue;
				if (pass == 0)
					start[r + 1]++;
				else
					xs[fill[r]++] = ax + ((float)ty + 0.5f - ay) * (bx - ax) / (by - ay);
			}
		}
		if (pass == 0)
		{
			for (int r = 0; r < rows; r++)
				start[r + 1] += start[r];
			if (!start[rows])
				break;
			xs = Z_Malloc(sizeof(float) * start[rows]);
			fill = Z_Malloc(sizeof(int) * rows);
			memcpy(fill, start, sizeof(int) * rows);
		}
	}
	if (xs)
	{
		for (int r = 0; r < rows; r++)
		{
			float *row = xs + start[r];
			const int n = start[r + 1] - start[r];
			if (n < 2)
				continue;
			qsort(row, n, sizeof(float), sel_float_cmp);
			// texel centres tx + 0.5 on [row[k], row[k + 1])
			for (int k = 0; k + 1 < n; k += 2)
				sel_span(mask, W, H, wrap, ty0 + r, (int)ceilf(row[k] - 0.5f), (int)ceilf(row[k + 1] - 0.5f) - 1, v);
		}
	}
	Z_Free(xs);
	Z_Free(fill);
	Z_Free(start);
}

/* One shape's texels into a W x H mask, set to (v). A rectangle: the texels from
   the one its first corner is in to the one its second is in, inclusive (as the
   rectangle stroke). An ellipse: those whose centres lie inside the ellipse
   inscribed in that same box. A lasso: sel_lasso. */
static void sel_shape(byte *mask, int W, int H, bool wrap, int shape, const float *p, int count, byte v)
{
	if (shape == SEL_LASSO)
	{
		sel_lasso(mask, W, H, wrap, p, count, v);
		return;
	}
	if (count < 2)
		return;
	const int x0 = (int)floorf(min(p[0], p[2]) * (float)W), x1 = (int)floorf(max(p[0], p[2]) * (float)W);
	const int y0 = (int)floorf(min(p[1], p[3]) * (float)H), y1 = (int)floorf(max(p[1], p[3]) * (float)H);
	if (x1 - x0 > SEL_MAX_TILES * W || y1 - y0 > SEL_MAX_TILES * H)
		return;
	const float cx = (float)(x0 + x1 + 1) * 0.5f, cy = (float)(y0 + y1 + 1) * 0.5f;
	const float rx = (float)(x1 - x0 + 1) * 0.5f, ry = (float)(y1 - y0 + 1) * 0.5f;
	for (int ty = y0; ty <= y1; ty++)
	{
		int a = x0, b = x1;
		if (shape == SEL_ELLIPSE)
		{
			const float dy = ((float)ty + 0.5f - cy) / ry, k = 1.f - dy * dy;
			if (k < 0.f)
				continue;
			const float half = rx * sqrtf(k);
			a = (int)ceilf(cx - half - 0.5f);
			b = (int)floorf(cx + half - 0.5f);
		}
		sel_span(mask, W, H, wrap, ty, a, b, v);
	}
}

static void sel_replay(const sel_step_t *s, byte *mask, int W, int H, bool wrap)
{
	switch (s->step)
	{
	case SELSTEP_ADD:
	case SELSTEP_SUB:
		sel_shape(mask, W, H, wrap && !s->clip, s->shape, sl.pts + 2 * s->first, s->count, s->step == SELSTEP_ADD ? 1 : 0);
		break;
	case SELSTEP_ALL:
		memset(mask, 1, (size_t)W * H);
		break;
	case SELSTEP_INVERT:
		for (size_t i = 0; i < (size_t)W * H; i++)
			mask[i] ^= 1;
		break;
	}
}

/* The selection's texels on a W x H map: its steps replayed in order, then the
   shape being dragged out - alone, while it is to replace the selection. */
static void sel_rasterize(int W, int H)
{
	const size_t n = (size_t)W * H;
	byte *mask = Z_Mallocz(n);
	if (au.stage >= 0)
		stage_info(NULL, NULL);     // a clampMap stage's map doesn't tile
	const bool wrap = map_wraps();
	if (!(sl.dragging && sl.drag_replace))
		for (int i = 0; i < sl.num_steps; i++)
			sel_replay(&sl.steps[i], mask, W, H, wrap);
	if (sl.dragging && sl.num_dpts)
		sel_shape(mask, W, H, wrap && !sl.drag_2d, sl.drag_shape, sl.dpts, sl.num_dpts, sl.drag_sub ? 0 : 1);
	int texels = 0;
	for (size_t i = 0; i < n; i++)
		texels += mask[i];
	Z_Free(sl.mask);
	sl.mask = mask;
	sl.mask_w = W;
	sl.mask_h = H;
	sl.texels = texels;
}

// Ctrl+A: all of the map
static void sel_all(void)
{
	sel_clear(NULL);
	sel_push(SELSTEP_ALL, SEL_OFF, NULL, 0);
	author_status("selected all of the map - Ctrl+D drops it");
}

// Ctrl+I: what is selected becomes what isn't
static void sel_invert(void)
{
	if (!sl.num_steps)
	{
		author_status("nothing selected to invert - Ctrl+A selects all");
		return;
	}
	sel_push(SELSTEP_INVERT, SEL_OFF, NULL, 0);
	author_status("selection inverted");
}

// Ctrl+Shift+D: the selection last dropped comes back, and swaps places with the one held, if any
static void sel_reselect(void)
{
	if (!sl.prev_num_steps)
	{
		author_status("no dropped selection to bring back");
		return;
	}
	if (sl.prev_material != MAT_IndexOf(au.mat) || sl.prev_stage != au.stage)
	{
		author_status("the last selection dropped was on another material or stage");
		return;
	}
	sel_step_t *steps = sl.steps;
	float *pts = sl.pts;
	const int num_steps = sl.num_steps, num_pts = sl.num_pts, material = sl.material, stage = sl.stage;
	sl.steps = sl.prev_steps;
	sl.pts = sl.prev_pts;
	sl.num_steps = sl.cap_steps = sl.prev_num_steps;
	sl.num_pts = sl.cap_pts = sl.prev_num_pts;
	sl.material = sl.prev_material;
	sl.stage = sl.prev_stage;
	sl.prev_steps = steps;
	sl.prev_pts = pts;
	sl.prev_num_steps = num_steps;
	sl.prev_num_pts = num_pts;
	sl.prev_material = material;
	sl.prev_stage = stage;
	sl.dragging = sl.drag_commit = false;
	sl.dirty = true;
	sl.stats_have = false;
	rg.dirty = true;
	queue_tick();
	author_status("the dropped selection is back");
}

/* What the left button does: drag out a selection of this shape, or (SEL_OFF)
   paint - the selection still holding painting to it. */
static void set_select(int mode)
{
	if (mode < SEL_OFF || mode >= NUM_SEL)
		return;
	if (sl.dragging)
	{
		sl.dragging = sl.drag_commit = false;
		sl.dirty = true;
		rg.dirty = true;
	}
	au.select = mode;
	au.line_active = au.line_have = au.line_commit = false;
	if (mode != SEL_OFF)
		author_status("%s select: drag on the surface or the map view - Shift adds, Ctrl takes away, a click drops it",
			sel_label[mode]);
	else if (sel_on())
		author_status("painting - only inside the selection (Ctrl+D drops it)");
	else
		author_status("painting");
}

/* ------------------------------------------------------------------------- */
/* THE BRIGHTNESS RANGE                                                      */
/* ------------------------------------------------------------------------- */

static float lum_lut[256];      // an sRGB byte, linear
static bool lum_lut_ready;

/* The base texture's brightness over texel (x, y) of a W x H map, 0..1: its
   luminance, sRGB-encoded - as bright as it looks. Where the base has more
   texels than the map and (box), the mean over those under this one (at most
   8 x 8 of them read); else the nearest. False if its colours aren't in memory. */
static bool base_brightness(int x, int y, int W, int H, bool box, float *out)
{
	const image_t *b = author_base();
	if (!b || !b->pix_data || b->pixel_format != PF_R8G8B8A8_UNORM || b->upload_width <= 0 || b->upload_height <= 0)
		return false;
	if (!lum_lut_ready)
	{
		for (int i = 0; i < 256; i++)
			lum_lut[i] = srgb_to_linear((float)i / 255.f);
		lum_lut_ready = true;
	}
	const int bw = b->upload_width, bh = b->upload_height;
	const int sx0 = (int)((int64_t)x * bw / W), sy0 = (int)((int64_t)y * bh / H);
	int sx1 = sx0, sy1 = sy0;
	if (box)
	{
		sx1 = max(sx0, (int)((int64_t)(x + 1) * bw / W) - 1);
		sy1 = max(sy0, (int)((int64_t)(y + 1) * bh / H) - 1);
	}
	const int stx = max(1, (sx1 - sx0 + 8) / 8), sty = max(1, (sy1 - sy0 + 8) / 8);
	float sum = 0.f;
	int n = 0;
	for (int sy = sy0; sy <= sy1; sy += sty)
	{
		const byte *row = b->pix_data + (size_t)sy * bw * 4;
		for (int sx = sx0; sx <= sx1; sx += stx)
		{
			const byte *p = row + (size_t)sx * 4;
			if (b->is_srgb)
				sum += 0.2126f * lum_lut[p[0]] + 0.7152f * lum_lut[p[1]] + 0.0722f * lum_lut[p[2]];
			else
				sum += (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) / 255.f;
			n++;
		}
	}
	*out = linear_to_srgb(n ? sum / (float)n : 0.f);
	return true;
}

/* The base texture's brightness over what the range sets on a W x H map - the
   region (the lock and the selection), or all of it with neither: its spread
   (lo, hi), the 1st and 99th percentiles so a few odd texels don't decide it
   (the very least and most under 100 texels), and the texels read (n). Exact
   reads every texel, box-filtered; else a grid of at most 256 x 256, nearest -
   the panel's. False when the base's colours aren't in memory. The caller
   checks region_ready. */
static bool range_stats(int W, int H, bool exact, float *lo, float *hi, int *n)
{
	*lo = 0.f;
	*hi = 1.f;
	*n = 0;
	float probe;
	if (W <= 0 || H <= 0 || !base_brightness(0, 0, W, H, false, &probe))
		return false;
	const bool held = region_on();
	const int sx = exact ? 1 : max(1, W / 256), sy = exact ? 1 : max(1, H / 256);
	static int hist[1024];
	memset(hist, 0, sizeof(hist));
	float mn = 1.f, mx = 0.f;
	int count = 0;
	for (int y = 0; y < H; y += sy)
	{
		for (int x = 0; x < W; x += sx)
		{
			if (held && !region_allows(x, y, W, H))
				continue;
			float l = 0.f;
			base_brightness(x, y, W, H, exact, &l);
			hist[Q_clip((int)(l * 1023.f + 0.5f), 0, 1023)]++;
			mn = min(mn, l);
			mx = max(mx, l);
			count++;
		}
	}
	*n = count;
	if (!count)
		return true;
	if (count < 100)
	{
		*lo = mn;
		*hi = mx;
		return true;
	}
	const int cut = count / 100;
	int acc = 0, b = 0;
	while (b < 1023 && acc + hist[b] <= cut)
		acc += hist[b++];
	*lo = (float)b / 1023.f;
	acc = 0;
	b = 1023;
	while (b > 0 && acc + hist[b] <= cut)
		acc += hist[b--];
	*hi = max((float)b / 1023.f, *lo);
	return true;
}

/* THE RANGE APPLIED (Matt, 11-21: "select an area on a texture and say this
   selection should be 30 to 50% metallic"): every texel of the region - the
   selection, the lock, both; the whole map with neither - set on the map being
   painted from the base texture's brightness there: the darkest (the spread's
   1st percentile) gets 'dark', the brightest (99th) 'bright', in a straight
   line between. Roughness, metallic, height or emissive's brightness; a stroke
   of its own, one undo step. A region all one brightness gets the middle of the
   range; dark = bright is a plain fill and needs no base texture. Between
   frames: it can make the map. */
static void range_apply(void)
{
	if (!slot_is_scalar(au.slot))
	{
		author_status("the brightness range sets a roughness, metallic, height or emissive map - not normals");
		return;
	}
	if (au.stage >= 0)
	{
		const char *why;
		if (!stage_info(NULL, &why))
		{
			author_status("can't set stage%d's map: %s", au.stage, why);
			return;
		}
	}
	stroke_end();
	image_t *img = author_image();
	if (!img && !(img = create_map()))
		return;
	const char *why;
	if (!image_paintable(img, &why))
	{
		author_status("can't set %s: %s", img->name, why);
		return;
	}

	region_sync();
	const int W = img->upload_width, H = img->upload_height;
	const bool held = region_on();
	if (held && !region_ready(W, H))
	{
		author_status("the %s isn't ready for this map yet - apply again", region_name());
		return;
	}
	const int slot = au.slot;
	const float dark = Q_clipf(au.range[slot][0], 0.f, 1.f), bright = Q_clipf(au.range[slot][1], 0.f, 1.f);
	const bool flat = fabsf(bright - dark) < 0.5f / 255.f;
	float lo = 0.f, hi = 1.f;
	int n = 0;
	if (!flat && !range_stats(W, H, true, &lo, &hi, &n))
	{
		author_status("the base texture's colours aren't in memory: only a plain fill (dark = bright) works");
		return;
	}
	const float span = hi - lo;
	const bool even = !flat && span < 1.f / 255.f;

	stroke_touch(img);
	dab_ctx_t c;
	memset(&c, 0, sizeof(c));
	c.img = img;
	c.slot = slot;
	int set = 0;
	for (int y = 0; y < H; y++)
	{
		for (int x = 0; x < W; x++)
		{
			if (held && !region_allows(x, y, W, H))
				continue;
			float t = 0.5f;
			if (!flat && !even)
			{
				float l = lo;
				base_brightness(x, y, W, H, true, &l);
				t = Q_clipf((l - lo) / span, 0.f, 1.f);
			}
			const float v = dark + (bright - dark) * t;
			const float vv[3] = { v, v, v };
			if (texel_put(img, x, y, slot, vv))
				c.changed = true;
			set++;
		}
	}
	dab_done(&c);
	stroke_end();

	const float scale = slot == SLOT_EMISSIVE ? 200.f : 100.f;
	const char *what = slot == SLOT_EMISSIVE ? "glow" : slot_word[slot];
	const char *where = held ? region_name() : "whole map (nothing selected)";
	const int pd = (int)(dark * scale + 0.5f), pb = (int)(bright * scale + 0.5f);
	if (!set)
		author_status("nothing to set: the %s has no texels on this map", where);
	else if (flat)
		author_status("%s %d%% over the %s: %d texels", what, pd, where, set);
	else if (even)
		author_status("the %s is all one brightness (%d%%): %d texels set to %s %d%%, the middle", where,
			(int)(lo * 100.f + 0.5f), set, what, (int)((dark + bright) * 0.5f * scale + 0.5f));
	else
		author_status("%s %d%% (darkest) to %d%% (brightest) by the base's brightness %d-%d%%, over the %s: %d texels",
			what, pd, pb, (int)(lo * 100.f + 0.5f), (int)(hi * 100.f + 0.5f), where, set);
}

/* The panel's line under the range: the base's brightness spread over what it
   would set, measured on a grid when that changes (rg.version, the map's size,
   the base). */
static void range_spread_text(char *buf, size_t size)
{
	if (sl.dragging)
	{
		Q_strlcpy(buf, "base ...", size);
		return;
	}
	int W, H;
	map_size(&W, &H);
	const int base = image_index(author_base());
	if (!sl.stats_have || sl.stats_version != rg.version || sl.stats_w != W || sl.stats_h != H || sl.stats_base != base)
	{
		sl.stats_have = true;
		sl.stats_version = rg.version;
		sl.stats_w = W;
		sl.stats_h = H;
		sl.stats_base = base;
		float lo, hi;
		int n;
		if (region_on() && !region_ready(W, H))
			sl.stats_n = -1;
		else if (!range_stats(W, H, false, &lo, &hi, &n))
			sl.stats_n = -2;
		else
		{
			sl.stats_lo = lo;
			sl.stats_hi = hi;
			sl.stats_n = n;
		}
	}
	if (sl.stats_n == -1)
		Q_strlcpy(buf, "base ...", size);
	else if (sl.stats_n == -2)
		Q_strlcpy(buf, "base not in memory", size);
	else if (sl.stats_n == 0)
		Q_strlcpy(buf, "nothing selected here", size);
	else
		Q_snprintf(buf, size, "base %d-%d%%%s", (int)(sl.stats_lo * 100.f + 0.5f), (int)(sl.stats_hi * 100.f + 0.5f),
			region_on() ? "" : " (all)");
}

/* ------------------------------------------------------------------------- */
/* START, STOP                                                               */
/* ------------------------------------------------------------------------- */

static void author_stop(const char *why)
{
	stroke_end();
	upload_now();
	island_clear();
	au.active = false;
	au.painting = false;
	au.drag = W_NONE;
	au.edit_id = W_NONE;
	au.right_pan = false;
	au.hit.valid = false;
	au.range_request = false;
	if (sl.dragging)
	{
		// a shape half dragged out is let go of; the selection stays
		sl.dragging = sl.drag_commit = false;
		sl.dirty = true;
		rg.dirty = true;
	}

	if (au.paused_by_us && cl_paused->integer == 2)
	{
		Cvar_Set("cl_paused", "0");
		CL_CheckForPause();
	}
	au.paused_by_us = false;

	Com_Printf("mat author: off%s\n", why);
	const int dirty = count_dirty();
	if (dirty)
		Com_Printf("mat author: %d painted map(s) NOT SAVED - 'mat author save' writes them, "
			"'mat author' carries on (the undo history is kept)\n", dirty);
}

static void author_fit_view(void);

static void author_start(pbr_material_t *mat, int slot)
{
	if (cls.state != ca_active)
	{
		Com_Printf("mat author: needs a level running\n");
		return;
	}
	if (!mat)
	{
		Com_Printf("mat author: nothing under the crosshair - aim at the surface to paint\n");
		return;
	}

	if (!mat_author_cursor_speed)
		mat_author_cursor_speed = Cvar_Get("mat_author_cursor_speed", "1", CVAR_ARCHIVE);
	if (!mat_author_slider_speed)
		mat_author_slider_speed = Cvar_Get("mat_author_slider_speed", "0.25", CVAR_ARCHIVE);

	if (!au.defaults_set)
	{
		au.defaults_set = true;
		// fully rough, not metal: what a texture is with no maps (Matt, 11-21)
		au.value[SLOT_ROUGH] = 1.f;
		au.value[SLOT_METAL] = 0.f;
		au.value[SLOT_HEIGHT] = 0.5f;
		au.value[SLOT_EMISSIVE] = 0.5f;     // 100%: as bright as the texture
		au.value[SLOT_GLASS] = 0.f;         // the mask starts all glass: what's painted first is wall
		au.normal_angle = -(float)M_PI / 2.f;
		au.normal_intensity = 0.5f;
		au.emit_tint[0] = au.emit_tint[1] = au.emit_tint[2] = 1.f;
		memcpy(au.emit_baked, au.emit_tint, sizeof(au.emit_baked));
		au.emit_mat = au.emit_image = -1;
		// an emissive map starts all 0%: a flood by value would take all of it, so
		// the bulb is picked by its colour in the texture
		au.fill_by_colour[SLOT_EMISSIVE] = true;
		// the brightness range: the darkest of the base to 0%, the brightest to 100% (emissive: 100% = as bright as the texture)
		for (int s = 0; s < NUM_SLOTS; s++)
		{
			au.range[s][0] = 0.f;
			au.range[s][1] = s == SLOT_EMISSIVE ? 0.5f : 1.f;
		}
		au.select = SEL_OFF;
		au.brush_size = 8;
		au.stroke = STROKE_FREE;
		au.rect_radius = 0;
		au.edit_id = W_NONE;
		au.stage = -1;
		au.tool = au.prev_tool = TOOL_BRUSH;
		au.falloff = 0.f;
		au.tool_opt[TOOL_SPRAY] = 0.25f;
		au.tool_opt[TOOL_HATCH] = 4.f / 30.f;   // every 6 texels
		au.tool_opt[TOOL_NOISE] = 0.3f;
		au.tool_opt[TOOL_BLUR] = 0.5f;
		au.tool_opt[TOOL_FILL] = 0.1f;
		au.show_base = true;
		au.layer_alpha = 0.6f;
		au.glow_veil = 0.6f;
		au.stroke_image = -1;
		au.upload_image = -1;
		au.rng = Sys_Milliseconds() | 1u;
	}

	if (au.mat != mat)
	{
		stroke_end();
		lock_clear(NULL);
		sel_clear(NULL);
		au.stage = -1;
	}
	au.mat = mat;
	Q_strlcpy(au.mat_name, mat->name, sizeof(au.mat_name));
	if (slot >= 0 && !(au.stage >= 0 && slot > SLOT_NORMAL) && slot_offered(mat, slot))
		au.slot = slot;
	else if (slot == SLOT_GLASS)
		Com_Printf("mat author: %s isn't stained glass - the glass map is its mask "
			"('mat stained_glass 1' or 'kind STAINEDGLASS' first)\n", mat->name);
	if (!slot_offered(mat, au.slot))
		au.slot = SLOT_ROUGH;
	au.v2_fit_image = -2;
	au.line_active = au.line_have = au.line_commit = false;

	if (!au.active)
	{
		au.active = true;
		au.cx = r_config.width * 0.5f;
		au.cy = r_config.height * 0.5f;
		au.pause_frames = 0;
		au.painting = false;
		au.drag = W_NONE;
		au.hit.valid = false;
		au.tick_queued = false;
		if (cl_paused->integer != 2)
		{
			Cvar_Set("cl_paused", "2");
			CL_CheckForPause();
			au.paused_by_us = true;
		}
		Con_Close(true);
		Com_Printf("mat author: hold the RIGHT mouse button to look (WASD/QE fly, Shift fast, Ctrl slow,\n"
			"  Ctrl + right button zooms); H = camera back where it started; C = flashlight (a light\n"
			"  at the camera; mat_author_light_scale, mat_author_light_cone);\n"
			"  LEFT paints; Alt + left picks the material under the brush;\n"
			"  wheel or [ ] = brush size (on the map view the wheel zooms, right-drag pans);\n"
			"  Shift + wheel = value; Ctrl + wheel = falloff; 1-5 = map (6 = glass, stained glass's\n"
			"  mask: glass or wall, X swaps);\n"
			"  L = line (press where it starts, let go where it ends); R = rectangle (press at one\n"
			"  corner, let go at the other; the wheel sets its corner radius);\n"
			"  T = next tool (brush spray hatch noise blur fill), G = fill; Tab = map view, O = base under it;\n"
			"  F = lock to the face under the brush, P = to the piece, Shift adds/removes, U unlocks;\n"
			"  M = select (rect, ellipse, lasso): drag it out, Shift + drag adds, Ctrl + drag takes away,\n"
			"  a click drops it; Ctrl+A all, Ctrl+D none (Ctrl+Shift+D back), Ctrl+I invert; painting\n"
			"  stays inside it, and the panel's dark/bright + apply set the map over it by how bright\n"
			"  the base texture is (also 'mat author range <dark%%> <bright%%>');\n"
			"  a Quake III shader drawn in stages: the panel's 'maps' row paints one stage's own maps;\n"
			"  a slider: click its number to type one (Enter), the wheel or arrows step it by 1,\n"
			"  Shift while dragging is finer (mat_author_slider_speed);\n"
			"  Ctrl+Z / Ctrl+Y = undo / redo on the map being painted; Ctrl+S = save; Esc = leave\n");
	}

	image_t *img = author_image();
	char whose[MAX_QPATH + 16];
	if (au.stage >= 0)
		Q_snprintf(whose, sizeof(whose), "%s stage%d", mat->name, au.stage);
	else
		Q_strlcpy(whose, mat->name, sizeof(whose));
	if (img)
		author_status("painting %s's %s map, %s (%dx%d)", whose, slot_word[au.slot],
			img->filepath[0] ? img->filepath : img->name, img->upload_width, img->upload_height);
	else
		author_status("painting %s's %s map - it has none yet: the first stroke makes one", whose,
			slot_word[au.slot]);
}

// the glass mask's paint: glass (true) or the wall round it
static void glass_paint_set(bool glass)
{
	au.value[SLOT_GLASS] = glass ? 1.f : 0.f;
	author_status(glass ? "painting glass: the light comes through here, and it glows"
		: "painting wall: an ordinary surface here - no light through, no glow");
}

static void set_slot(int slot)
{
	if (slot == au.slot || slot < 0 || slot >= NUM_SLOTS)
		return;
	if (!slot_offered(au.mat, slot))
	{
		if (au.stage >= 0)
			author_status("a stage has no glass mask: 'material' in the maps row paints the material's");
		else
			author_status("the glass map is a stained glass mask: %s isn't stained glass "
				"('mat stained_glass 1' or 'kind STAINEDGLASS' first)", au.mat ? au.mat->name : "this");
		return;
	}
	if (au.stage >= 0 && slot > SLOT_NORMAL)
	{
		if (slot == SLOT_EMISSIVE)
			author_status("a stage has no emissive map of its own: its glow is stage%d_emissive_factor "
				"('mat stage%d emissive_factor <n>')", au.stage, au.stage);
		else
			author_status("a stage has no %s map of its own: 'material' in the maps row paints the "
				"material's", slot_word[slot]);
		return;
	}
	stroke_end();
	au.slot = slot;
	au.v2_fit_image = -2;
	image_t *img = author_image();
	if (img)
		author_status("%s map: %s (%dx%d)", slot_word[slot], img->filepath[0] ? img->filepath : img->name,
			img->upload_width, img->upload_height);
	else if (au.stage >= 0)
		author_status("%s stage%d has no %s map of its own yet: the first stroke makes one", au.mat->name,
			au.stage, slot_word[slot]);
	else
		author_status("%s has no %s map yet: the first stroke makes one", au.mat->name, slot_word[slot]);
}

/* Paint the material's own maps (-1) or Quake III stage N's own: its
   stage<N>_texture_roughness, _metallic, _normals, read through its tcMods. */
static void set_stage(int stage)
{
	if (stage == au.stage)
		return;
	if (stage >= 0)
	{
		const image_t *pic;
		const char *why;
		bool clamp;
		float tc[6];
		if (!vkpt_q3_stage_paint_info(au.mat, stage, &pic, &clamp, tc, &why))
		{
			author_status("stage%d's maps can't be painted: %s", stage, why);
			return;
		}
	}
	stroke_end();
	au.stage = stage;
	au.line_active = au.line_have = au.line_commit = false;
	if (stage >= 0 && au.slot > SLOT_NORMAL)
		au.slot = SLOT_ROUGH;
	au.v2_fit_image = -2;
	if (lk.on)
		lk.dirty = true;    // the same triangles, laid on another map
	// a selection is in its map's coordinates: another stage's map lies differently
	sel_clear("the selection was on the other map: dropped");
	if (stage < 0)
	{
		author_status("painting %s's own maps", au.mat->name);
		return;
	}
	const image_t *pic = NULL;
	stage_info(&pic, NULL);
	image_t *img = author_image();
	if (img)
		author_status("stage%d (%s): its %s map %s (%dx%d)", stage, pic ? COM_SkipPath(pic->name) : "$whiteimage",
			slot_word[au.slot], img->filepath[0] ? img->filepath : img->name, img->upload_width, img->upload_height);
	else
		author_status("stage%d (%s) has no %s map of its own yet: the first stroke makes one", stage,
			pic ? COM_SkipPath(pic->name) : "$whiteimage", slot_word[au.slot]);
}

static void set_stroke(int stroke)
{
	if (stroke < 0 || stroke >= NUM_STROKES)
		return;
	// a painting stroke picked: the left button paints again (the selection still holds)
	au.select = SEL_OFF;
	au.stroke = stroke;
	au.line_active = au.line_have = au.line_commit = false;
	switch (stroke)
	{
	case STROKE_LINE:
		author_status("line: press where it starts, let go where it ends");
		break;
	case STROKE_RECT:
		author_status("rectangle: press at one corner, let go at the other - corner radius %d (the wheel)",
			au.rect_radius);
		break;
	default:
		author_status("free strokes");
		break;
	}
}

static void set_tool(int tool)
{
	if (tool < 0 || tool >= NUM_TOOLS)
		return;
	// a painting tool picked: the left button paints again (the selection still holds)
	if (au.select != SEL_OFF)
	{
		set_select(SEL_OFF);
		if (tool == au.tool)
			return;
	}
	if (tool == au.tool)
		return;
	stroke_end();
	if (tool == TOOL_FILL)
		au.prev_tool = au.tool;
	au.tool = tool;
	static const char *const what[NUM_TOOLS] = {
		"brush",
		"spray: holds paint on while the button is down - density sets how fast",
		"hatch: crossed lines on the texel grid",
		"noise: the value give or take the amount, per texel",
		"blur: smooths what is there",
		"fill: click fills the lock, or neighbouring texels within the tolerance",
	};
	author_status("%s", what[tool]);
}

static void pick_material(void)
{
	if (!au.hit.valid)
	{
		author_status("nothing under the brush to pick");
		return;
	}
	pbr_material_t *mat = MAT_ForIndex(au.hit.material);
	if (!mat)
		return;
	stroke_end();
	if (mat != au.mat)
	{
		lock_clear(NULL);
		sel_clear(NULL);
		au.stage = -1;
	}
	au.mat = mat;
	Q_strlcpy(au.mat_name, mat->name, sizeof(au.mat_name));
	au.v2_fit_image = -2;
	if (!slot_offered(mat, au.slot))
	{
		au.slot = SLOT_ROUGH;
		author_status("now painting %s - not stained glass, so its roughness map", mat->name);
		return;
	}
	author_status("now painting %s", mat->name);
}

/* ------------------------------------------------------------------------- */
/* THE MAP VIEW                                                              */
/* ------------------------------------------------------------------------- */

static void view2d_rect(float *x, float *y, float *size)
{
	const float s = ui_scale();
	const float vw = r_config.width * s, vh = r_config.height * s;
	*size = floorf(min(vh - 40.f, vw * 0.45f));
	*x = vw - 8.f - *size;
	*y = 8.f;
}

static void author_fit_view(void)
{
	int W, H;
	float x, y, size;
	map_size(&W, &H);
	view2d_rect(&x, &y, &size);
	au.v2_ppt = size / (float)max(W, H);
	au.v2_cu = W * 0.5f;
	au.v2_cv = H * 0.5f;
	au.v2_fit_image = image_index(author_image());
	au.v2_fit_w = W;
	au.v2_fit_h = H;
}

static void view2d_check_fit(void)
{
	int W, H;
	map_size(&W, &H);
	if (au.v2_fit_image != image_index(author_image()) || au.v2_fit_w != W || au.v2_fit_h != H)
		author_fit_view();
}

static bool cursor_in_view2d(void)
{
	if (!au.show_2d)
		return false;
	float x, y, size, mx, my;
	view2d_rect(&x, &y, &size);
	cursor_virtual(&mx, &my);
	return mx >= x && mx < x + size && my >= y && my < y + size;
}

// the cursor in the map view, in texels; false if it is not over the map
static bool view2d_texel(float *tu, float *tv)
{
	if (!cursor_in_view2d())
		return false;
	float x, y, size, mx, my;
	int W, H;
	view2d_rect(&x, &y, &size);
	cursor_virtual(&mx, &my);
	map_size(&W, &H);
	*tu = au.v2_cu + (mx - (x + size * 0.5f)) / au.v2_ppt;
	*tv = au.v2_cv + (my - (y + size * 0.5f)) / au.v2_ppt;
	return *tu >= 0.f && *tu < (float)W && *tv >= 0.f && *tv < (float)H;
}

static void view2d_zoom(bool in)
{
	float x, y, size, mx, my;
	int W, H;
	view2d_rect(&x, &y, &size);
	cursor_virtual(&mx, &my);
	map_size(&W, &H);
	const float cx = x + size * 0.5f, cy = y + size * 0.5f;
	const float tu = au.v2_cu + (mx - cx) / au.v2_ppt, tv = au.v2_cv + (my - cy) / au.v2_ppt;
	const float fit = size / (float)max(W, H);
	au.v2_ppt = Q_clipf(au.v2_ppt * (in ? 1.25f : 0.8f), fit * 0.25f, 64.f);
	// the texel under the cursor stays there
	au.v2_cu = tu - (mx - cx) / au.v2_ppt;
	au.v2_cv = tv - (my - cy) / au.v2_ppt;
}

/* ------------------------------------------------------------------------- */
/* THE PANEL                                                                 */
/* ------------------------------------------------------------------------- */

// laying the panel out to measure it: nothing drawn, no controls
static bool ui_measuring;

static void add_widget(int id, float x, float y, float w, float h)
{
	if (ui_measuring || num_widgets >= (int)q_countof(widgets))
		return;
	widget_t *wd = &widgets[num_widgets++];
	wd->id = id;
	wd->x = x;
	wd->y = y;
	wd->w = w;
	wd->h = h;
}

static const widget_t *widget_find(int id)
{
	for (int i = num_widgets - 1; i >= 0; i--)
		if (widgets[i].id == id)
			return &widgets[i];
	return NULL;
}

static int widget_at(float mx, float my)
{
	for (int i = num_widgets - 1; i >= 0; i--)
	{
		const widget_t *w = &widgets[i];
		if (mx >= w->x && mx < w->x + w->w && my >= w->y && my < w->y + w->h)
			return w->id;
	}
	return W_NONE;
}

// over the panel or the map view: not the scene
static bool cursor_on_ui(void)
{
	float mx, my;
	cursor_virtual(&mx, &my);
	return widget_at(mx, my) != W_NONE;
}

bool MAT_AuthorMouseOverPanel(void)
{
	return au.active && au.right_pan;
}

/* THE PANEL'S NUMBERS. Every slider - and the normal's direction dial - is a
   number in whole units: percent, texels, degrees. Beside each is a box with
   it, to click and type into (edit_*); the slider snaps to it, and the wheel
   or the arrow keys over either step it by one. field_get reads one as shown,
   field_set takes one (rounded, kept in range) back into au. */
typedef struct {
	float v, lo, hi;        // shown, and its range
	const char *name;       // for the status line
	const char *unit;
	bool squared;           // the slider is the square root of the range: small values get room (sizes)
	bool wraps;             // degrees: round the circle, not stopped at its ends
} field_t;

static bool field_get(int id, field_t *f)
{
	static const char *const tint_names[3] = { "red", "green", "blue" };
	memset(f, 0, sizeof(*f));
	f->unit = "%";
	f->hi = 100.f;
	switch (id)
	{
	case W_VALUE:
		// the glass mask paints glass or wall, no value between (W_GLASS_PAINT)
		if (!slot_is_scalar(au.slot) || au.slot == SLOT_GLASS)
			return false;
		// emissive's brightness: 0..1 = 0..200%
		f->name = au.slot == SLOT_EMISSIVE ? "brightness" : slot_word[au.slot];
		f->hi = au.slot == SLOT_EMISSIVE ? 200.f : 100.f;
		f->v = au.value[au.slot] * f->hi;
		return true;
	case W_INTENSITY:
		f->name = "intensity";
		f->v = au.normal_intensity * 100.f;
		return true;
	case W_RED: case W_GREEN: case W_BLUE:
		f->name = tint_names[id - W_RED];
		f->v = au.emit_tint[id - W_RED] * 100.f;
		return true;
	case W_SIZE:
		f->name = "brush";
		f->unit = " texels";
		f->lo = 1.f;
		f->hi = AUTHOR_MAX_BRUSH;
		f->v = (float)au.brush_size;
		f->squared = true;
		return true;
	case W_RADIUS:
		f->name = "corner radius";
		f->unit = " texels";
		f->hi = AUTHOR_MAX_RADIUS;
		f->v = (float)au.rect_radius;
		f->squared = true;
		return true;
	case W_FALLOFF:
		f->name = "falloff";
		f->v = au.falloff * 100.f;
		return true;
	case W_RANGE_LO:
	case W_RANGE_HI:
		// the brightness range, in the value's own units (emissive: 0..200% of the texture)
		if (!slot_is_scalar(au.slot))
			return false;
		f->name = id == W_RANGE_LO ? "darkest gets" : "brightest gets";
		f->hi = au.slot == SLOT_EMISSIVE ? 200.f : 100.f;
		f->v = au.range[au.slot][id - W_RANGE_LO] * f->hi;
		return true;
	case W_TOOLOPT:
		switch (au.tool)
		{
		case TOOL_SPRAY: f->name = "density"; break;
		case TOOL_NOISE: f->name = "noise"; break;
		case TOOL_BLUR:  f->name = "blur strength"; break;
		case TOOL_FILL:  f->name = "tolerance"; break;
		case TOOL_HATCH:
			// lines every 2..32 texels (hatch_spacing)
			f->name = "hatch spacing";
			f->unit = " texels";
			f->lo = 2.f;
			f->hi = 32.f;
			f->v = (float)hatch_spacing();
			return true;
		default:
			return false;
		}
		f->v = au.tool_opt[au.tool] * 100.f;
		return true;
	case W_LAYER:
		f->name = glow_view() ? "purple over what doesn't glow" : "map over the base";
		f->v = (glow_view() ? au.glow_veil : au.layer_alpha) * 100.f;
		return true;
	case W_LIGHT:
		// the flashlight's brightness: 100% = the game's own flashlight
		light_cvars();
		f->name = "flashlight";
		f->hi = 1000.f;
		f->squared = true;
		f->v = max(0.f, mat_author_light_scale->value) * 100.f;
		return true;
	case W_DIAL:
		// on screen, y down: 0 right, 90 down, 270 up
		f->name = "leans";
		f->unit = " deg";
		f->hi = 360.f;
		f->wraps = true;
		f->v = (float)((((int)floorf(au.normal_angle * 180.f / (float)M_PI + 0.5f)) % 360 + 360) % 360);
		return true;
	default:
		return false;
	}
}

static int field_shown(const field_t *f)
{
	return (int)floorf(f->v + 0.5f);
}

static void field_set(int id, float v)
{
	field_t f;
	if (!field_get(id, &f))
		return;
	v = floorf(v + 0.5f);
	if (f.wraps)
		v = fmodf(fmodf(v, f.hi) + f.hi, f.hi);
	else
		v = Q_clipf(v, f.lo, f.hi);
	switch (id)
	{
	case W_VALUE:     au.value[au.slot] = v / f.hi; break;
	case W_INTENSITY: au.normal_intensity = v / 100.f; break;
	case W_RED: case W_GREEN: case W_BLUE:
		// the albedo's channel the glow keeps: the map's colours are made again (emit_retint)
		au.emit_tint[id - W_RED] = v / 100.f;
		queue_tick();
		break;
	case W_SIZE:      au.brush_size = (int)v; break;
	case W_RADIUS:    au.rect_radius = (int)v; break;
	case W_FALLOFF:   au.falloff = v / 100.f; break;
	case W_RANGE_LO: case W_RANGE_HI: au.range[au.slot][id - W_RANGE_LO] = v / f.hi; break;
	case W_TOOLOPT:   au.tool_opt[au.tool] = au.tool == TOOL_HATCH ? (v - 2.f) / 30.f : v / 100.f; break;
	case W_LAYER:     *(glow_view() ? &au.glow_veil : &au.layer_alpha) = v / 100.f; break;
	case W_LIGHT:     Cvar_SetValue(mat_author_light_scale, v / 100.f, FROM_CONSOLE); break;
	case W_DIAL:      au.normal_angle = v * (float)M_PI / 180.f; break;
	default:          break;
	}
}

// one up or down (by): the wheel or the arrow keys over a slider, Shift ten
static bool field_step(int id, int by)
{
	field_t f;
	if (!field_get(id, &f))
		return false;
	field_set(id, (float)(field_shown(&f) + by));
	return true;
}

// the slider's position, 0..1, and the number at one
static float field_to_t(const field_t *f)
{
	const float t = Q_clipf((f->v - f->lo) / max(f->hi - f->lo, 1e-6f), 0.f, 1.f);
	return f->squared ? sqrtf(t) : t;
}

static float field_at_t(const field_t *f, float t)
{
	t = Q_clipf(t, 0.f, 1.f);
	if (f->squared)
		t *= t;
	return f->lo + t * (f->hi - f->lo);
}

// the number the cursor is over: a slider, the dial or a number box; W_NONE none
static int field_under(float mx, float my)
{
	int id = widget_at(mx, my);
	if (id >= W_NUM0 && id < W_NUM_END)
		id -= W_NUM0;
	field_t f;
	return field_get(id, &f) ? id : W_NONE;
}

// a number box clicked: typing replaces what it shows, Enter takes it
static void edit_begin(int id)
{
	field_t f;
	if (!field_get(id, &f))
		return;
	au.edit_id = id;
	Q_snprintf(au.edit_buf, sizeof(au.edit_buf), "%d", field_shown(&f));
	au.edit_fresh = true;
}

static void edit_end(bool apply)
{
	const int id = au.edit_id;
	au.edit_id = W_NONE;
	if (!apply || id == W_NONE || !au.edit_buf[0])
		return;
	char *end;
	const float v = strtof(au.edit_buf, &end);
	if (end == au.edit_buf)
	{
		author_status("'%s' isn't a number", au.edit_buf);
		return;
	}
	field_set(id, v);
	field_t f;
	if (field_get(id, &f))
		author_status("%s %d%s", f.name, field_shown(&f), f.unit);
}

/* A key while a number box is being typed into: digits, '.', '-', Backspace;
   Enter (or Tab) takes it, Escape leaves it as it was. Every other key is kept
   from the shortcuts and the camera while typing - but keys let go of pass, so
   a fly key held from before is not stuck. The mouse takes the number first. */
static bool edit_key(unsigned key, bool down)
{
	if (au.edit_id == W_NONE)
		return false;
	if (key >= K_MOUSEFIRST && key <= K_MOUSELAST)
	{
		if (down)
			edit_end(true);
		return false;
	}
	if (!down || key == K_SHIFT || key == K_CTRL || key == K_ALT)
		return false;

	static const struct { unsigned key; char ch; } keypad[] = {
		{ K_KP_INS, '0' }, { K_KP_END, '1' }, { K_KP_DOWNARROW, '2' }, { K_KP_PGDN, '3' },
		{ K_KP_LEFTARROW, '4' }, { K_KP_5, '5' }, { K_KP_RIGHTARROW, '6' }, { K_KP_HOME, '7' },
		{ K_KP_UPARROW, '8' }, { K_KP_PGUP, '9' }, { K_KP_DEL, '.' }, { K_KP_MINUS, '-' },
	};
	char ch = 0;
	if ((key >= '0' && key <= '9') || key == '.' || key == '-')
		ch = (char)key;
	for (int i = 0; i < (int)q_countof(keypad) && !ch; i++)
		if (keypad[i].key == key)
			ch = keypad[i].ch;

	size_t len = strlen(au.edit_buf);
	switch (key)
	{
	case K_ESCAPE:
		edit_end(false);
		break;
	case K_ENTER:
	case K_KP_ENTER:
	case K_TAB:
		edit_end(true);
		break;
	case K_BACKSPACE:
		if (au.edit_fresh)
			au.edit_buf[0] = 0;
		else if (len)
			au.edit_buf[len - 1] = 0;
		au.edit_fresh = false;
		break;
	default:
		if (!ch)
			break;
		if (au.edit_fresh)
		{
			au.edit_buf[0] = 0;
			au.edit_fresh = false;
			len = 0;
		}
		if (len < 6)
		{
			au.edit_buf[len] = ch;
			au.edit_buf[len + 1] = 0;
		}
		break;
	}
	return true;
}

static int size_step(int s, int dir, int lo, int hi)
{
	if (dir > 0)
		s = s < 10 ? s + 1 : (int)(s * 1.15f + 0.5f);
	else
		s = s <= 10 ? s - 1 : (int)(s / 1.15f + 0.5f);
	return Q_clip(s, lo, hi);
}

// the wheel off the panel: the brush's size - or, drawing rectangles, their corners' radius
static void brush_size_step(int dir)
{
	if (au.stroke == STROKE_RECT)
	{
		au.rect_radius = size_step(au.rect_radius, dir, 0, AUTHOR_MAX_RADIUS);
		author_status("corner radius %d texel%s", au.rect_radius, au.rect_radius == 1 ? "" : "s");
		return;
	}
	au.brush_size = size_step(au.brush_size, dir, 1, AUTHOR_MAX_BRUSH);
}

// the main value of the map being painted, by Shift + wheel: 5 at a time
static void value_step(int dir)
{
	if (au.slot == SLOT_GLASS)
	{
		glass_paint_set(dir > 0);
		return;
	}
	field_step(au.slot == SLOT_NORMAL ? W_INTENSITY : W_VALUE, dir * 5);
}

// the left button holding control (id) at the cursor
static void drag_update(int id)
{
	const widget_t *w = widget_find(id);
	if (!w)
		return;
	float mx, my;
	cursor_virtual(&mx, &my);
	if (id == W_DIAL)
	{
		const float dx = mx - (w->x + w->w * 0.5f), dy = my - (w->y + w->h * 0.5f);
		if (dx * dx + dy * dy < 1.f)
			return;
		float a = atan2f(dy, dx);
		// whole degrees; Shift, the nearest 45
		const float step = Key_IsDown(K_SHIFT) ? (float)M_PI / 4.f : (float)M_PI / 180.f;
		au.normal_angle = floorf(a / step + 0.5f) * step;
		return;
	}
	field_t f;
	if (!field_get(id, &f))
		return;
	// the track, inside the widget's 3 px margins (ui_slider)
	field_set(id, field_at_t(&f, (mx - (w->x + 3.f)) / max(w->w - 6.f, 1.f)));
}

// what the warning line's Fix button does, and the warning; NULL = none
static const char *panel_warning(const char **fix)
{
	const pbr_material_t *m = au.mat;
	*fix = NULL;
	// a stage's own maps answer to its stage<N>_* settings, not the material's
	if (au.stage >= 0)
		return NULL;
	switch (au.slot)
	{
	case SLOT_ROUGH:
		if (m->specular_factor <= 0.f)
		{
			*fix = "specular_factor 1";
			return "specular_factor 0: no shine";
		}
		if (m->roughness_override > 0.f && author_image())
		{
			// the shader's roughness = max(map, override) while a map carries one
			*fix = "roughness_override -1";
			return va("override %.2f is a floor", m->roughness_override);
		}
		break;
	case SLOT_METAL:
		if (m->metalness_factor <= 0.f)
		{
			*fix = "metalness_factor 1";
			return "metalness_factor 0: no metal";
		}
		break;
	case SLOT_NORMAL:
		if (m->bump_scale <= 0.f)
		{
			*fix = "bump_scale 1";
			return "bump_scale 0: map won't show";
		}
		break;
	case SLOT_EMISSIVE:
		if (m->emissive_factor <= 0.f)
		{
			// a painted map holds its brightness at half (emit_bake)
			*fix = emit_painted(m->image_emissive) ? "emissive_factor 2" : "emissive_factor 1";
			return "emissive_factor 0: no glow";
		}
		break;
	case SLOT_HEIGHT:
		// "set ..." is a console command, not a material one: panel_press tells them apart
		if (Cvar_VariableInteger("pt_pom") == 0)
		{
			*fix = "set pt_pom 1";
			return "pt_pom 0: no relief anywhere";
		}
		if (Cvar_VariableValue("pt_pom_scale") <= 0.f)
		{
			*fix = "set pt_pom_scale 1";
			return "pt_pom_scale 0: no relief";
		}
		if (m->displace_in <= 0.f && m->displace_out <= 0.f)
		{
			*fix = "displace_in 0.05";
			return "displace_in/out 0: no depth";
		}
		break;
	}
	return NULL;
}

/* The free camera back where it was when the tool started - the player's eye,
   unzoomed (Matt, 11-21: "an option or button to return the free camera in
   material mode back to the original position"). The panel's "home" and H. */
static void camera_home(void)
{
	vkpt_freecam_home();
	author_status("camera back where it started");
}

/* The flashlight on or off (Matt, 11-22: "a toggle for a camera light ... a
   flashlight kind of thing for the material editor camera"): the panel's
   "flashlight", C, 'mat author light'. MAT_AuthorCameraLight makes the light. */
static void light_set(bool on)
{
	light_cvars();
	Cvar_SetInteger(mat_author_light, on ? 1 : 0, FROM_CONSOLE);
	author_status(on ? "flashlight on: a light at the camera, pointing where it looks" : "flashlight off");
}

static void light_toggle(void)
{
	light_cvars();
	light_set(!mat_author_light->integer);
}

static void panel_press(int id)
{
	switch (id)
	{
	case W_SLOT0: case W_SLOT1: case W_SLOT2: case W_SLOT3: case W_SLOT4: case W_SLOT5:
		set_slot(id - W_SLOT0);
		break;
	case W_GLASS_PAINT: glass_paint_set(true); break;
	case W_WALL_PAINT:  glass_paint_set(false); break;
	case W_VALUE: case W_DIAL: case W_INTENSITY: case W_RED: case W_GREEN: case W_BLUE: case W_SIZE:
	case W_FALLOFF: case W_TOOLOPT: case W_LAYER: case W_RADIUS: case W_RANGE_LO: case W_RANGE_HI:
	case W_LIGHT:
		au.drag = id;
		drag_update(id);
		break;
	case W_RANGE_APPLY:
		// it can make the map: between frames
		au.range_request = true;
		queue_tick();
		break;
	case W_SEL_ALL:     sel_all(); break;
	case W_SEL_INVERT:  sel_invert(); break;
	case W_SEL_NONE:
		if (sl.num_steps)
			sel_clear("selection dropped - Ctrl+Shift+D brings it back");
		else
			author_status("nothing is selected");
		break;
	case W_STAGE_MAT:   set_stage(-1); break;
	case W_TOOL0: case W_TOOL1: case W_TOOL2: case W_TOOL3: case W_TOOL4: case W_TOOL5:
		set_tool(id - W_TOOL0);
		break;
	case W_FILL_MAP:    au.fill_by_colour[au.slot] = false; break;
	case W_FILL_COLOUR: au.fill_by_colour[au.slot] = true; break;
	case W_UNLOCK:      lock_clear("unlocked"); break;
	case W_SHOWBASE:    au.show_base = !au.show_base; break;
	case W_SHOW2D: au.show_2d = !au.show_2d; au.v2_fit_image = -2; break;
	case W_CAM_HOME: camera_home(); break;
	case W_LIGHT_ON: light_toggle(); break;
	case W_UNDO:  undo_apply(false); break;
	case W_REDO:   undo_apply(true); break;
	case W_SAVE:   author_save(); break;
	case W_EXIT:   author_stop(""); break;
	case W_FIX:
	{
		const char *fix;
		if (panel_warning(&fix) && fix && !strncmp(fix, "set ", 4))
		{
			// a cvar: for the session, as typed in the console
			Cbuf_AddText(&cmd_buffer, va("%s\n", fix));
			author_status("%s", fix + 4);
		}
		else if (fix)
		{
			MAT_CommandFor(au.mat, fix);
			au.attr_changed = true;
			author_status("%s: %s (Save writes it into the .mat)", au.mat->name, fix);
		}
		break;
	}
	default:
		if (id >= W_STAGE0 && id < W_STAGE0 + AUTHOR_MAX_STAGES)
			set_stage(id - W_STAGE0);
		else if (id >= W_STROKE0 && id < W_STROKE0 + NUM_STROKES)
		{
			if (au.stroke != id - W_STROKE0 || au.select != SEL_OFF)
				set_stroke(id - W_STROKE0);
		}
		else if (id > W_SEL0 && id < W_SEL0 + NUM_SEL)
			set_select(au.select == id - W_SEL0 ? SEL_OFF : id - W_SEL0);   // the one on again: back to painting
		else if (id >= W_NUM0 && id < W_NUM_END)
			edit_begin(id - W_NUM0);
		break;
	}
}

/* The panel is laid out twice a frame: once measuring (nothing drawn, no
   controls), to size its background, which has to go down first; then drawn. */
static void ui_fill(float x, float y, float w, float h, uint32_t color)
{
	if (!ui_measuring)
		R_DrawFill32_RTX((int)x, (int)y, (int)w, (int)h, color);
}

// at most (max_chars): the end cut off
static void ui_text(float x, float y, int flags, const char *s, int max_chars, qhandle_t font)
{
	if (ui_measuring || max_chars <= 0)
		return;
	const size_t len = strlen(s);
	R_DrawString_RTX((int)x, (int)y, flags, min(len, (size_t)max_chars), s, font);
}

// a path: the start cut off instead, as "...", the file name kept
static void ui_tail(float x, float y, int flags, const char *s, int max_chars, qhandle_t font)
{
	const size_t len = strlen(s);
	if (max_chars > 3 && len > (size_t)max_chars)
	{
		char buf[512];
		Q_concat(buf, sizeof(buf), "...", s + len - (max_chars - 3));
		ui_text(x, y, flags, buf, max_chars, font);
	}
	else
		ui_text(x, y, flags, s, max_chars, font);
}

// (s) over at most (max_lines) lines, broken at spaces; y moves down past it
static void ui_wrapped(float x, float *y, int flags, const char *s, int max_chars, int max_lines, float line, qhandle_t font)
{
	for (int l = 0; l < max_lines && *s; l++)
	{
		int n = (int)strlen(s);
		if (n > max_chars && l < max_lines - 1)
		{
			int cut = max_chars;
			while (cut > max_chars / 2 && s[cut] != ' ')
				cut--;
			if (s[cut] != ' ')
				cut = max_chars;
			n = cut;
		}
		ui_text(x, *y, flags, s, min(n, max_chars), font);
		*y += line;
		s += n;
		while (*s == ' ')
			s++;
	}
}

static void ui_button(int id, float x, float y, float w, const char *label, bool on, qhandle_t font)
{
	const float h = 11.f;
	ui_fill(x, y, w, h, on ? MakeColor(190, 110, 30, 240) : MakeColor(64, 64, 64, 240));
	const int chars = (int)strlen(label);
	ui_text(x + (w - chars * CHAR_WIDTH) * 0.5f, y + 2.f, 0, label, chars, font);
	add_widget(id, x, y, w, h);
}

static void ui_slider(int id, float x, float y, float w, float t)
{
	t = Q_clipf(t, 0.f, 1.f);
	ui_fill(x, y + 4, w, 3, MakeColor(40, 40, 40, 255));
	ui_fill(x, y + 4, w * t, 3, MakeColor(190, 110, 30, 255));
	ui_fill((float)((int)(x + w * t) - 1), y, 3, 11, MakeColor(255, 255, 255, 255));
	add_widget(id, x - 3.f, y - 1.f, w + 6.f, 13.f);
}

#define NUMBOX_W    (6 * CHAR_WIDTH + 4)

/* Field (id)'s number in a box: click it to type one (edit_*). Being typed
   into, it shows what is typed and a caret - lit while a key still replaces
   the whole number. */
static void ui_numbox(int id, float x, float y, qhandle_t font)
{
	field_t f;
	if (!field_get(id, &f))
		return;
	const bool editing = au.edit_id == id;
	char text[24];
	if (editing)
		Q_snprintf(text, sizeof(text), "%s_", au.edit_buf);
	else
		Q_snprintf(text, sizeof(text), "%d", field_shown(&f));
	const float h = 11.f;
	const uint32_t edge = editing ? MakeColor(255, 170, 60, 255) : MakeColor(110, 110, 110, 255);
	ui_fill(x, y, NUMBOX_W, h, editing && au.edit_fresh ? MakeColor(120, 70, 20, 255) : MakeColor(16, 16, 16, 255));
	ui_fill(x, y, NUMBOX_W, 1, edge);
	ui_fill(x, y + h - 1.f, NUMBOX_W, 1, edge);
	ui_fill(x, y, 1, h, edge);
	ui_fill(x + NUMBOX_W - 1.f, y, 1, h, edge);
	const int n = min((int)strlen(text), 6);
	ui_text(x + NUMBOX_W - 2.f - n * CHAR_WIDTH, y + 2.f, 0, text + strlen(text) - n, n, font);
	add_widget(W_NUM0 + id, x, y, NUMBOX_W, h);
}

// a slider (w wide, its number box included) for field (id)
static void ui_slider_num(int id, float x, float y, float w, qhandle_t font)
{
	field_t f;
	if (!field_get(id, &f))
		return;
	ui_slider(id, x, y, w - NUMBOX_W - 8.f, field_to_t(&f));
	ui_numbox(id, x + w - NUMBOX_W, y, font);
}

static void ui_dot(float x, float y, int size, uint32_t color)
{
	ui_fill((float)(int)(x - size * 0.5f + 0.5f), (float)(int)(y - size * 0.5f + 0.5f), (float)size, (float)size, color);
}

// the direction dial: which way the painted normal leans on screen
static void ui_dial(float cx, float cy, float r)
{
	const uint32_t ring = MakeColor(150, 150, 150, 255), hand = MakeColor(255, 170, 60, 255);
	ui_fill((float)(int)(cx - r), (float)(int)(cy - r), (float)(int)(2 * r + 1), (float)(int)(2 * r + 1), MakeColor(24, 24, 24, 255));
	for (int i = 0; i < 48; i++)
	{
		const float a = i * 2.f * (float)M_PI / 48.f;
		ui_dot(cx + cosf(a) * r, cy + sinf(a) * r, 1, ring);
	}
	const float dx = cosf(au.normal_angle), dy = sinf(au.normal_angle);
	const float len = r * (0.25f + 0.75f * au.normal_intensity);
	for (float t = 0.f; t <= len; t += 1.f)
		ui_dot(cx + dx * t, cy + dy * t, 1, hand);
	ui_dot(cx + dx * len, cy + dy * len, 3, hand);
	add_widget(W_DIAL, cx - r, cy - r, 2 * r + 1, 2 * r + 1);
}

static const char *direction_name(float a)
{
	static const char *const names[8] = { "right", "down-right", "down", "down-left", "left", "up-left", "up", "up-right" };
	int i = (int)floorf(a / ((float)M_PI / 4.f) + 0.5f);
	i = ((i % 8) + 8) % 8;
	return names[i];
}

/* ------------------------------------------------------------------------- */
/* UNDER THE BRUSH                                                           */
/* ------------------------------------------------------------------------- */

typedef struct {
	bool valid;
	float v[3];     // the average, in paint terms (texel_get)
	int texels;
} under_t;

// where the brush is on a W x H map: under the cursor in the map view, or where the cursor is on a surface
static bool brush_centre(int W, int H, int *c2x, int *c2y, bool *wrap)
{
	float tu, tv;
	if (view2d_texel(&tu, &tv))
	{
		*c2x = centre2_x(tu);
		*c2y = centre2_y(tv);
		*wrap = false;
		return true;
	}
	if (cursor_on_ui() || !hit_on_map(&au.hit, author_image()))
		return false;
	hit_texel(&au.hit, W, H, true, &tu, &tv);
	*c2x = centre2_x(tu);
	*c2y = centre2_y(tv);
	*wrap = map_wraps();
	return true;
}

/* The average of the texels the brush covers, inside the lock, as they are now:
   two texels at 0% and 100% read 50%. A big brush reads every few texels. */
static void brush_under(under_t *u)
{
	memset(u, 0, sizeof(*u));
	const image_t *img = author_image();
	const char *why;
	if (!img || !image_paintable(img, &why))
		return;
	const int W = img->upload_width, H = img->upload_height;
	int c2x, c2y;
	bool wrap;
	if (!brush_centre(W, H, &c2x, &c2y, &wrap))
		return;

	const int step = max(1, cur_size() / 96);   // the larger side: a long thin rectangle still reads every few
	double sum[3] = { 0.0, 0.0, 0.0 };
	int n = 0, ty0, ty1;
	brush_rows(c2y, &ty0, &ty1);
	for (int ty = ty0; ty <= ty1; ty += step)
	{
		int x0, x1;
		if (!brush_row(c2x, c2y, ty, &x0, &x1))
			continue;
		int y = ty;
		if (wrap)
			y = wrap_i(y, H);
		else if (y < 0 || y >= H)
			continue;
		for (int tx = x0; tx <= x1; tx += step)
		{
			int x = tx;
			if (wrap)
				x = wrap_i(x, W);
			else if (x < 0 || x >= W)
				continue;
			if (!region_allows(x, y, W, H))
				continue;
			float v[3];
			texel_get(img, img->pix_data, (size_t)y * W + x, au.slot, v);
			for (int c = 0; c < 3; c++)
				sum[c] += v[c];
			n++;
		}
	}
	if (!n)
		return;
	u->valid = true;
	u->texels = n;
	for (int c = 0; c < 3; c++)
		u->v[c] = (float)(sum[c] / n);
}

static void under_text(const under_t *u, char *buf, size_t size)
{
	if (!u->valid)
	{
		Q_strlcpy(buf, "under -", size);
		return;
	}
	switch (au.slot)
	{
	case SLOT_ROUGH:
	case SLOT_METAL:
	case SLOT_HEIGHT:
		Q_snprintf(buf, size, "under %d%%", (int)(u->v[0] * 100.f + 0.5f));
		break;
	case SLOT_GLASS:
		Q_strlcpy(buf, u->v[0] >= 0.5f ? "under glass" : "under wall", size);
		break;
	case SLOT_EMISSIVE:
		Q_snprintf(buf, size, "under %d%%", (int)(u->v[0] * 200.f + 0.5f));
		break;
	case SLOT_NORMAL:
	{
		const float xy = sqrtf(u->v[0] * u->v[0] + u->v[1] * u->v[1]);
		Q_snprintf(buf, size, "under the brush: tilt %d deg", (int)(atan2f(xy, u->v[2]) * 180.f / (float)M_PI + 0.5f));
		break;
	}
	default:
		Q_snprintf(buf, size, "under the brush: %d%% %d%% %d%%", (int)(u->v[0] * 100.f + 0.5f),
			(int)(u->v[1] * 100.f + 0.5f), (int)(u->v[2] * 100.f + 0.5f));
		break;
	}
}

/* ------------------------------------------------------------------------- */
/* THE PANEL'S ROWS                                                          */
/* ------------------------------------------------------------------------- */

// the panel from (x0, y0) down: the y it ends at
static float panel_rows(float x0, float y0, const under_t *under, qhandle_t font)
{
	const float pad = 6.f, line = 12.f, w = AUTHOR_PANEL_W;
	const float iw = w - 2 * pad;
	const int max_chars = (int)(iw / CHAR_WIDTH);
	const float x = x0 + pad;
	float y = y0 + pad;

	char under_s[64];
	under_text(under, under_s, sizeof(under_s));
	const int under_n = (int)strlen(under_s);

	ui_text(x, y, UI_ALTCOLOR, "MAT AUTHOR", max_chars, font);
	ui_tail(x + 11 * CHAR_WIDTH, y, 0, au.mat->name, max_chars - 11, font);
	y += line;

	image_t *img = author_image();
	int W, H;
	map_size(&W, &H);
	if (img)
	{
		char size[32];
		Q_snprintf(size, sizeof(size), "%dx%d ", img->upload_width, img->upload_height);
		const int n = (int)strlen(size);
		ui_text(x, y, 0, size, n, font);
		ui_tail(x + n * CHAR_WIDTH, y, 0, img->filepath[0] ? img->filepath : img->name, max_chars - n, font);
	}
	else
	{
		// a stand-in, or a glow not painted here, is copied on the first stroke (create_map)
		const image_t *was = au.stage < 0 ? slot_image(au.mat, au.slot) : NULL;
		if (au.slot == SLOT_EMISSIVE && au.stage < 0 && was)
			ui_text(x, y, 0, va("old glow: paint converts, %dx%d", W, H), max_chars, font);
		else if (was)
			ui_text(x, y, 0, va("stand-in: paint copies, %dx%d", W, H), max_chars, font);
		else
			ui_text(x, y, 0, va("no %s map: painting makes one (%dx%d)", slot_word[au.slot], W, H), max_chars, font);
	}
	y += line + 2.f;

	// a Quake III shader drawn in stages: whose maps - the material's, or one stage's own
	const int stages = min(vkpt_q3_paint_stage_count(au.mat), AUTHOR_MAX_STAGES);
	if (stages > 0)
	{
		const float lw = 5 * CHAR_WIDTH, mw = 8 * CHAR_WIDTH + 6.f;
		ui_text(x, y + 2.f, 0, "maps", 4, font);
		ui_button(W_STAGE_MAT, x + lw, y, mw, "material", au.stage < 0, font);
		const float sx = x + lw + mw + 4.f;
		const float sw = min(24.f, (x + iw - sx + 3.f) / (float)stages - 3.f);
		for (int i = 0; i < stages; i++)
		{
			const image_t *pic;
			const char *why;
			bool clamp;
			float tc[6];
			const bool ok = vkpt_q3_stage_paint_info(au.mat, i, &pic, &clamp, tc, &why);
			const float bx = sx + i * (sw + 3.f);
			char label[4];
			Q_snprintf(label, sizeof(label), "%d", i);
			if (ok || au.stage == i)
				ui_button(W_STAGE0 + i, bx, y, sw, label, au.stage == i, font);
			else
			{
				// no surface to paint: dim, and a click says why
				ui_fill(bx, y, sw, 11.f, MakeColor(32, 32, 32, 240));
				ui_text(bx + (sw - CHAR_WIDTH) * 0.5f, y + 2.f, UI_ALTCOLOR, label, 1, font);
				add_widget(W_STAGE0 + i, bx, y, sw, 11.f);
			}
		}
		y += line + 1.f;
		if (au.stage >= 0)
		{
			const image_t *pic = NULL;
			const char *why;
			if (stage_info(&pic, &why))
			{
				const char *what = va("stage%d%s over ", au.stage, au.stage_clamp ? " (clamp)" : "");
				const int n = (int)strlen(what);
				ui_text(x, y, UI_ALTCOLOR, what, n, font);
				ui_tail(x + n * CHAR_WIDTH, y, UI_ALTCOLOR, pic ? pic->name : "$whiteimage", max_chars - n, font);
			}
			else
				ui_text(x, y, UI_ALTCOLOR, va("stage%d: %s", au.stage, why), max_chars, font);
			y += line + 2.f;
		}
	}

	// a stage has no emissive or height map: their buttons are left out, and
	// the glass mask's (last) is there only for stained glass (slot_offered).
	// Each button its label's width, the room left over shared out evenly; with
	// all six, emissive's says "emit" so the row fits the panel.
	const int slots = au.stage >= 0 ? SLOT_NORMAL + 1 : slot_offered(au.mat, SLOT_GLASS) ? NUM_SLOTS : SLOT_GLASS;
	const char *labels[NUM_SLOTS];
	int label_chars = 0;
	for (int i = 0; i < slots; i++)
	{
		labels[i] = (i == SLOT_EMISSIVE && slots == NUM_SLOTS) ? "emit" : slot_label[i];
		label_chars += (int)strlen(labels[i]);
	}
	const float spare = max(0.f, (iw - (slots - 1) * 4.f - label_chars * CHAR_WIDTH) / (float)slots);
	float slot_x = x;
	for (int i = 0; i < slots; i++)
	{
		const float bw = (float)strlen(labels[i]) * CHAR_WIDTH + spare;
		ui_button(W_SLOT0 + i, slot_x, y, bw, labels[i], au.slot == i, font);
		slot_x += bw + 4.f;
	}
	y += line + 6.f;

	switch (au.slot)
	{
	case SLOT_GLASS:
	{
		// what a stroke makes the texels: glass, or the wall round it - 1 or 0, no
		// value between (texel_put cuts every blend at half way)
		const bool glass = au.value[SLOT_GLASS] >= 0.5f;
		ui_text(x, y, 0, "paint (X swaps)", max_chars - under_n - 1, font);
		ui_text(x + iw - under_n * CHAR_WIDTH, y, UI_ALTCOLOR, under_s, under_n, font);
		y += line;
		const float gbw = (iw - 4.f) * 0.5f;
		ui_button(W_GLASS_PAINT, x, y, gbw, "glass", glass, font);
		ui_button(W_WALL_PAINT, x + gbw + 4.f, y, gbw, "wall", !glass, font);
		y += line + 4.f;
		ui_text(x, y, UI_ALTCOLOR, "white glass: lit through, glows", max_chars, font);
		y += line;
		ui_text(x, y, UI_ALTCOLOR, "black wall: plain surface", max_chars, font);
		y += line;
		ui_text(x, y, UI_ALTCOLOR, "fill light + window box: on map load", max_chars, font);
		y += line + 2.f;
		break;
	}
	case SLOT_ROUGH:
	case SLOT_METAL:
	case SLOT_HEIGHT:
		ui_text(x, y, 0, va("%s %d%%", slot_word[au.slot], (int)(au.value[au.slot] * 100.f + 0.5f)), max_chars - under_n - 1, font);
		ui_text(x + iw - under_n * CHAR_WIDTH, y, UI_ALTCOLOR, under_s, under_n, font);
		y += line;
		ui_slider_num(W_VALUE, x + 3.f, y, iw - 3.f, font);
		y += line + 4.f;
		if (au.slot == SLOT_HEIGHT)
		{
			// what the grey means for this material: where its surface lies, how deep black goes
			const pbr_material_t *m = au.mat;
			if (m->displace_out > 0.f)
				ui_text(x, y, 0, va("surface at %d%% (in %.3g, out %.3g)", (int)(height_surface_level(m) * 100.f + 0.5f),
					max(m->displace_in, 0.f), m->displace_out), max_chars, font);
			else
				ui_text(x, y, 0, va("white = surface, black = %.3g deep", max(m->displace_in, 0.f)), max_chars, font);
			y += line + 2.f;
		}
		break;
	case SLOT_NORMAL:
	{
		const float r = 18.f;
		ui_dial(x + r, y + r, r);
		const float tx = x + 2 * r + 10.f;
		const int tchars = (int)((x + iw - tx) / CHAR_WIDTH);
		// the direction in degrees, in its own box: 0 right, 90 down, 270 up
		ui_text(tx, y, 0, va("leans %s, deg", direction_name(au.normal_angle)),
			tchars - (int)(NUMBOX_W / CHAR_WIDTH) - 1, font);
		ui_numbox(W_DIAL, x + iw - NUMBOX_W, y - 2.f, font);
		ui_text(tx, y + line, 0, va("intensity %d%% (tilt %d deg)", (int)(au.normal_intensity * 100.f + 0.5f),
			(int)(au.normal_intensity * AUTHOR_MAX_TILT_DEG + 0.5f)), tchars, font);
		ui_slider_num(W_INTENSITY, tx + 3.f, y + 2 * line, x + iw - tx - 3.f, font);
		y += 2 * r + 6.f;
		ui_text(x, y, 0, "screen-relative (map view: image)", max_chars, font);
		y += line;
		ui_text(x, y, UI_ALTCOLOR, under_s, max_chars, font);
		y += line + 2.f;
		break;
	}
	case SLOT_EMISSIVE:
	{
		// the brightness painted, 0..200% of the texture; the texture's channels the glow keeps
		ui_text(x, y, 0, va("brightness %d%%", (int)(au.value[SLOT_EMISSIVE] * 200.f + 0.5f)), max_chars - under_n - 1, font);
		ui_text(x + iw - under_n * CHAR_WIDTH, y, UI_ALTCOLOR, under_s, under_n, font);
		y += line;
		ui_slider_num(W_VALUE, x + 3.f, y, iw - 3.f, font);
		y += line + 2.f;
		ui_text(x, y, 0, "100% = as bright as the texture", max_chars, font);
		y += line + 4.f;

		static const char *const names[3] = { "red", "green", "blue" };
		const float sw = 30.f;
		ui_fill(x, y, sw, 3 * line - 2, MakeColor(to_byte(au.emit_tint[0]), to_byte(au.emit_tint[1]),
			to_byte(au.emit_tint[2]), 255));
		// each channel's percent is in its box
		const float sx = x + sw + 8.f, sl = 6.f * CHAR_WIDTH;
		for (int c = 0; c < 3; c++)
		{
			ui_text(sx, y + c * line + 1.f, 0, names[c], 5, font);
			ui_slider_num(W_RED + c, sx + sl + 4.f, y + c * line, x + iw - (sx + sl + 4.f), font);
		}
		y += 3 * line + 2.f;
		ui_text(x, y, 0, "texture channels (whole map)", max_chars, font);
		y += line + 2.f;
		break;
	}
	}

	// the tools
	const float tw = (iw - (NUM_TOOLS - 1) * 3.f) / NUM_TOOLS;
	for (int i = 0; i < NUM_TOOLS; i++)
		ui_button(W_TOOL0 + i, x + i * (tw + 3.f), y, tw, tool_label[i], au.tool == i, font);
	y += line + 5.f;

	if (au.tool == TOOL_FILL)
	{
		const float fbw = 6 * CHAR_WIDTH + 6.f;
		ui_text(x, y + 2.f, 0, va("tolerance %d%%", (int)(au.tool_opt[TOOL_FILL] * 100.f + 0.5f)),
			(int)((iw - 2 * fbw - 8.f) / CHAR_WIDTH), font);
		ui_button(W_FILL_MAP, x + iw - 2 * fbw - 4.f, y, fbw, "value", !au.fill_by_colour[au.slot], font);
		ui_button(W_FILL_COLOUR, x + iw - fbw, y, fbw, "colour", au.fill_by_colour[au.slot], font);
		y += line + 1.f;
		ui_slider_num(W_TOOLOPT, x + 3.f, y, iw - 3.f, font);
		y += line + 4.f;
		ui_text(x, y, UI_ALTCOLOR, region_on() ? va("click: fills the whole %s", region_name())
			: "click: floods the texels alike", max_chars, font);
		y += line + 2.f;
	}
	else
	{
		ui_text(x, y, 0, au.falloff > 0.f ? va("falloff %d%% (soft edge)", (int)(au.falloff * 100.f + 0.5f))
			: "falloff 0% (hard edge)", max_chars, font);
		y += line;
		ui_slider_num(W_FALLOFF, x + 3.f, y, iw - 3.f, font);
		y += line + 4.f;

		const char *opt = NULL;
		switch (au.tool)
		{
		case TOOL_SPRAY: opt = va("density %d%%", (int)(au.tool_opt[TOOL_SPRAY] * 100.f + 0.5f)); break;
		case TOOL_HATCH: opt = va("lines every %d texels", hatch_spacing()); break;
		case TOOL_NOISE: opt = va("noise %d%% either way", (int)(au.tool_opt[TOOL_NOISE] * 100.f + 0.5f)); break;
		case TOOL_BLUR:  opt = va("blur strength %d%%", (int)(au.tool_opt[TOOL_BLUR] * 100.f + 0.5f)); break;
		default: break;
		}
		if (opt)
		{
			ui_text(x, y, 0, opt, max_chars, font);
			y += line;
			ui_slider_num(W_TOOLOPT, x + 3.f, y, iw - 3.f, font);
			y += line + 4.f;
		}

		/* the stroke: free; a straight line from where the button goes down to
		   where it comes up; or the rectangle with that line its diagonal */
		const float lbw = 40.f;
		const float sx0 = x + iw - NUM_STROKES * lbw - (NUM_STROKES - 1) * 3.f;
		ui_text(x, y + 2.f, 0, drag_preview() ? "let go to paint" : "stroke", (int)((sx0 - x - 4.f) / CHAR_WIDTH), font);
		for (int i = 0; i < NUM_STROKES; i++)
			ui_button(W_STROKE0 + i, sx0 + i * (lbw + 3.f), y, lbw, stroke_label[i], au.stroke == i, font);
		y += line + 4.f;

		// the round brush's size; a rectangle's corners instead (it is drawn, not a brush)
		if (au.stroke == STROKE_RECT)
		{
			ui_text(x, y, 0, va("corner radius %d texel%s (wheel)", au.rect_radius, au.rect_radius == 1 ? "" : "s"),
				max_chars, font);
			y += line;
			ui_slider_num(W_RADIUS, x + 3.f, y, iw - 3.f, font);
		}
		else
		{
			ui_text(x, y, 0, va("brush %d texel%s across", au.brush_size, au.brush_size == 1 ? "" : "s"), max_chars, font);
			y += line;
			ui_slider_num(W_SIZE, x + 3.f, y, iw - 3.f, font);
		}
		y += line + 4.f;
	}

	// the lock
	if (lk.on)
	{
		const float ubw = 6 * CHAR_WIDTH + 6.f;
		ui_text(x, y + 2.f, UI_ALTCOLOR, va("%s lock: %d tris, %d tx", lk.kind == 'f' ? "face" : "piece", lk.num_tris, lk.texels),
			(int)((iw - ubw - 4.f) / CHAR_WIDTH), font);
		ui_button(W_UNLOCK, x + iw - ubw, y, ubw, "unlock", false, font);
	}
	else
		ui_text(x, y + 2.f, 0, "no lock: F face, P piece", max_chars, font);
	y += line + 4.f;

	/* the selection (M): the shape the left button drags out instead of painting -
	   the one lit on again goes back to painting, which stays inside it */
	{
		const float lw = 7 * CHAR_WIDTH;
		const float bw = (iw - lw - 2 * 3.f) / (float)(NUM_SEL - 1);
		ui_text(x, y + 2.f, sl.dragging ? UI_ALTCOLOR : 0, sl.dragging ? "let go" : "select", 6, font);
		for (int i = SEL_RECT; i < NUM_SEL; i++)
			ui_button(W_SEL0 + i, x + lw + (i - SEL_RECT) * (bw + 3.f), y, bw, sel_label[i], au.select == i, font);
		y += line + 4.f;
	}
	if (au.select != SEL_OFF || sl.num_steps)
	{
		const float sbw = 6 * CHAR_WIDTH + 6.f;
		const int tchars = (int)((iw - 3 * sbw - 12.f) / CHAR_WIDTH);
		if (sl.num_steps)
			ui_text(x, y + 2.f, UI_ALTCOLOR, va("%d tx", sl.texels), tchars, font);
		else
			ui_text(x, y + 2.f, 0, "Shift+ Ctrl-", tchars, font);
		ui_button(W_SEL_ALL, x + iw - 3 * sbw - 8.f, y, sbw, "all", false, font);
		ui_button(W_SEL_INVERT, x + iw - 2 * sbw - 4.f, y, sbw, "invert", false, font);
		ui_button(W_SEL_NONE, x + iw - sbw, y, sbw, "none", false, font);
		y += line + 4.f;

		// the brightness range: the darkest of the base under the selection gets 'dark', the brightest 'bright'
		if (slot_is_scalar(au.slot))
		{
			const float lw = 7 * CHAR_WIDTH;
			ui_text(x, y + 1.f, 0, "dark", 6, font);
			ui_slider_num(W_RANGE_LO, x + lw + 3.f, y, iw - lw - 3.f, font);
			y += line + 2.f;
			ui_text(x, y + 1.f, 0, "bright", 6, font);
			ui_slider_num(W_RANGE_HI, x + lw + 3.f, y, iw - lw - 3.f, font);
			y += line + 4.f;
			const float abw = 6 * CHAR_WIDTH + 6.f;
			char spread[48];
			range_spread_text(spread, sizeof(spread));
			ui_text(x, y + 2.f, UI_ALTCOLOR, spread, (int)((iw - abw - 4.f) / CHAR_WIDTH), font);
			ui_button(W_RANGE_APPLY, x + iw - abw, y, abw, "apply", false, font);
			y += line + 4.f;
		}
		else
		{
			ui_text(x, y, UI_ALTCOLOR, "brightness range: not on normals", max_chars, font);
			y += line + 2.f;
		}
	}

	// the map view
	const float vbw = 14 * CHAR_WIDTH + 8.f;
	ui_button(W_SHOW2D, x, y, vbw, au.show_2d ? "[x] map view" : "[ ] map view", au.show_2d, font);
	ui_button(W_SHOWBASE, x + vbw + 4.f, y, vbw, au.show_base ? "[x] base under" : "[ ] base under", au.show_base, font);
	// the rest of the row (5 characters): the camera back where it started
	ui_button(W_CAM_HOME, x + 2.f * (vbw + 4.f), y, iw - 2.f * (vbw + 4.f), "home", false, font);
	y += line + 4.f;
	if (au.show_2d && au.show_base)
	{
		if (glow_view())
			ui_text(x, y, 0, va("purple over what doesn't glow: %d%%", (int)(au.glow_veil * 100.f + 0.5f)), max_chars, font);
		else
			ui_text(x, y, 0, va("map over the base: %d%%", (int)(au.layer_alpha * 100.f + 0.5f)), max_chars, font);
		y += line;
		ui_slider_num(W_LAYER, x + 3.f, y, iw - 3.f, font);
		y += line + 4.f;
	}

	// the flashlight at the camera, and while it is on, how bright
	light_cvars();
	const bool lit = mat_author_light->integer != 0;
	ui_button(W_LIGHT_ON, x, y, vbw, lit ? "[x] flashlight" : "[ ] flashlight", lit, font);
	if (lit)
		ui_slider_num(W_LIGHT, x + vbw + 7.f, y, iw - vbw - 7.f, font);
	y += line + 4.f;

	// undo and redo: this map's own steps
	const float b4 = (iw - 3 * 4.f) / 4.f;
	int n_undo, n_redo;
	undo_counts(img, &n_undo, &n_redo);
	ui_button(W_UNDO, x, y, b4, n_undo ? va("undo %d", n_undo) : "undo", false, font);
	ui_button(W_REDO, x + (b4 + 4.f), y, b4, n_redo ? va("redo %d", n_redo) : "redo", false, font);
	const int dirty = count_dirty();
	char save_label[16];
	Q_snprintf(save_label, sizeof(save_label), dirty ? "save %d" : "save", dirty);
	ui_button(W_SAVE, x + 2 * (b4 + 4.f), y, b4, save_label, dirty > 0, font);
	ui_button(W_EXIT, x + 3 * (b4 + 4.f), y, b4, "exit", false, font);
	y += line + 4.f;

	const char *fix;
	const char *warning = panel_warning(&fix);
	if (warning)
	{
		const float fw = 4 * CHAR_WIDTH + 6.f;
		ui_text(x, y + 2.f, UI_ALTCOLOR, warning, (int)((iw - fw - 4.f) / CHAR_WIDTH), font);
		if (fix)
			ui_button(W_FIX, x + iw - fw, y, fw, "fix", false, font);
		y += line;
	}

	const unsigned now = Sys_Milliseconds();
	if (au.status[0] && now - au.status_ms < 6000)
		ui_wrapped(x, &y, 0, au.status, max_chars, 2, line, font);
	else if (dirty)
	{
		ui_text(x, y, 0, va("%d painted map%s not saved", dirty, dirty == 1 ? "" : "s"), max_chars, font);
		y += line;
	}
	else
		y += line;

	// 36 characters a line at most
	ui_text(x, y, UI_ALTCOLOR, "RMB look  WASD/QE  H home  C light", max_chars, font);
	y += line - 2.f;
	ui_text(x, y, UI_ALTCOLOR, "Alt+click pick  wheel size/radius", max_chars, font);
	y += line - 2.f;
	ui_text(x, y, UI_ALTCOLOR, "Shift+wheel value  Ctrl+wheel soft", max_chars, font);
	y += line - 2.f;
	ui_text(x, y, UI_ALTCOLOR, slot_offered(au.mat, SLOT_GLASS) ? "1-6 map  X glass/wall  T tool  G fill"
		: "1-5 map  T tool  G fill  Tab 2D  O", max_chars, font);
	y += line - 2.f;
	ui_text(x, y, UI_ALTCOLOR, "L line  R rectangle  F face  P piece", max_chars, font);
	y += line - 2.f;
	ui_text(x, y, UI_ALTCOLOR, "Shift+F/P add  U unlock  Ctrl+S save", max_chars, font);
	y += line - 2.f;
	ui_text(x, y, UI_ALTCOLOR, "M select: Shift+drag add, Ctrl cut", max_chars, font);
	y += line - 2.f;
	ui_text(x, y, UI_ALTCOLOR, "Ctrl+A all  Ctrl+D none  Ctrl+I inv", max_chars, font);
	y += line - 2.f;
	ui_text(x, y, UI_ALTCOLOR, "LMB paint  click a number, Enter", max_chars, font);
	y += line - 2.f;
	ui_text(x, y, UI_ALTCOLOR, "slider: Shift finer, wheel/arrows 1", max_chars, font);
	y += line - 2.f;
	ui_text(x, y, UI_ALTCOLOR, "Ctrl+Z/Y undo/redo (this map)  Esc", max_chars, font);
	y += line;
	return y;
}

static void draw_panel(qhandle_t font)
{
	const float x0 = 8.f, y0 = 8.f;
	under_t under;
	brush_under(&under);

	// measured first: the background goes down before what is on it, and takes the clicks that miss a control
	ui_measuring = true;
	const float bottom = panel_rows(x0, y0, &under, font);
	ui_measuring = false;
	const float h = bottom - y0 + 2.f;
	R_DrawFill32_RTX((int)x0, (int)y0, AUTHOR_PANEL_W, (int)h, MakeColor(0, 0, 0, 200));
	add_widget(W_PANEL, x0, y0, AUTHOR_PANEL_W, h);
	panel_rows(x0, y0, &under, font);
}

/* What an outline is of, all in doubled texels: the brush at a centre; the
   brush swept from it by (dx, dy); or a rectangle centred there, (dx, dy) its
   half sides and r2 its corners (rect_inside). */
enum { OUT_BRUSH, OUT_LINE, OUT_RECT };

typedef struct {
	int kind;
	int c2x, c2y;
	int dx, dy;
	int r2;
} outline_t;

static bool outline_row(const outline_t *o, int ty, int *x0, int *x1)
{
	switch (o->kind)
	{
	case OUT_LINE: return sweep_row(o->c2x, o->c2y, o->dx, o->dy, ty, x0, x1);
	case OUT_RECT: return rect_row(o->c2x, o->c2y, o->dx, o->dy, o->r2, ty, x0, x1);
	default:       return brush_row(o->c2x, o->c2y, ty, x0, x1);
	}
}

static void outline_rows(const outline_t *o, int *ty0, int *ty1)
{
	switch (o->kind)
	{
	case OUT_LINE: sweep_rows(o->c2y, o->dy, ty0, ty1); break;
	case OUT_RECT: rect_span(o->c2y, o->dy, ty0, ty1); break;
	default:       brush_rows(o->c2y, ty0, ty1); break;
	}
}

/* The line or rectangle being drawn, in doubled texels of a W x H map. A line:
   its first centre and how far the last is from it. A rectangle: the texels
   from the one its first corner went down on to the one under the cursor,
   inclusive - its centre, half sides, and corners (no rounder than its shorter
   half side allows). On a tiled surface the centre is moved into the map's
   one copy - the shader wraps what it tests, and the map view shows that copy -
   the shape keeping its size. */
static void drag_outline(int W, int H, outline_t *o)
{
	memset(o, 0, sizeof(*o));
	if (au.stroke == STROKE_RECT)
	{
		const int x0 = (int)floorf(min(au.line_tu0, au.line_tu1)), x1 = (int)floorf(max(au.line_tu0, au.line_tu1));
		const int y0 = (int)floorf(min(au.line_tv0, au.line_tv1)), y1 = (int)floorf(max(au.line_tv0, au.line_tv1));
		o->kind = OUT_RECT;
		o->c2x = x0 + x1 + 1;
		o->c2y = y0 + y1 + 1;
		o->dx = x1 - x0 + 1;
		o->dy = y1 - y0 + 1;
		o->r2 = min(2 * Q_clip(au.rect_radius, 0, AUTHOR_MAX_RADIUS), min(o->dx, o->dy));
	}
	else
	{
		o->kind = OUT_LINE;
		o->c2x = centre2_x(au.line_tu0);
		o->c2y = centre2_y(au.line_tv0);
		o->dx = centre2_x(au.line_tu1) - o->c2x;
		o->dy = centre2_y(au.line_tv1) - o->c2y;
	}
	if (!au.paint_2d && map_wraps())
	{
		o->c2x -= 2 * W * (int)floorf((float)o->c2x / (float)(2 * W));
		o->c2y -= 2 * H * (int)floorf((float)o->c2y / (float)(2 * H));
	}
}

// the brush's texels as an outline on the map view, edge by edge
static void outline_2d(float vx, float vy, float vs, const outline_t *o, uint32_t color)
{
	const float cx = vx + vs * 0.5f, cy = vy + vs * 0.5f, ppt = au.v2_ppt;
	#define SX(t) (cx + ((float)(t) - au.v2_cu) * ppt)
	#define SY(t) (cy + ((float)(t) - au.v2_cv) * ppt)

	int ty0, ty1;
	outline_rows(o, &ty0, &ty1);

	if ((o->kind == OUT_BRUSH && cur_size() * ppt < 3.f) || ty1 - ty0 > 8192)
	{
		// smaller than a few pixels on screen (or a shape beyond reason): a marker at each end
		ui_dot(SX(o->c2x * 0.5f), SY(o->c2y * 0.5f), 3, color);
		if (o->kind == OUT_LINE)
			ui_dot(SX((o->c2x + o->dx) * 0.5f), SY((o->c2y + o->dy) * 0.5f), 3, color);
		return;
	}

	int px0 = 0, px1 = -1;   // the row above: none
	for (int ty = ty0; ty <= ty1 + 1; ty++)
	{
		int x0 = 0, x1 = -1;
		if (ty <= ty1 && !outline_row(o, ty, &x0, &x1))
			x1 = x0 - 1;

		// the horizontal edge between this row and the one above: where exactly one has texels
		for (int t = min(x0, px0); t <= max(x1, px1); t++)
		{
			const bool here = t >= x0 && t <= x1, above = t >= px0 && t <= px1;
			if (here == above)
				continue;
			int e = t;
			while (e + 1 <= max(x1, px1) && ((e + 1 >= x0 && e + 1 <= x1) == here) && ((e + 1 >= px0 && e + 1 <= px1) == above))
				e++;
			R_DrawFill32_RTX((int)SX(t), (int)SY(ty), max(1, (int)(SX(e + 1) - SX(t))), 1, color);
			t = e;
		}
		// this row's two sides
		if (ty <= ty1 && x0 <= x1)
		{
			const int h = max(1, (int)(SY(ty + 1) - SY(ty)));
			R_DrawFill32_RTX((int)SX(x0), (int)SY(ty), 1, h, color);
			R_DrawFill32_RTX((int)SX(x1 + 1), (int)SY(ty), 1, h, color);
		}
		px0 = x0;
		px1 = x1;
	}
	#undef SX
	#undef SY
}

/* A selection's shapes on the map view: marching ants - white and black dashes
   that crawl - along each one's outline, clipped to the view. A shape made on a
   tiled surface is moved into the map's one copy by its first point. */
typedef struct {
	float clip[4];          // the view: x0, y0, x1, y1
	float cx, cy;           // its centre on screen
	float ox, oy;           // the shape's shift into the map's copy, texels
	float s;                // the length gone along the outline, for the dashes
	float step;             // virtual px between dots
	int phase;
} ants_t;

static void ants_point(const ants_t *a, float tu, float tv, float *x, float *y)
{
	*x = a->cx + (tu + a->ox - au.v2_cu) * au.v2_ppt;
	*y = a->cy + (tv + a->oy - au.v2_cv) * au.v2_ppt;
}

static void ants_line(ants_t *a, float x0, float y0, float x1, float y1)
{
	const float dx = x1 - x0, dy = y1 - y0;
	const float len = sqrtf(dx * dx + dy * dy);
	const int n = max(1, (int)ceilf(len / a->step));
	for (int i = 0; i < n; i++)
	{
		const float t = (float)i / (float)n;
		const float px = x0 + dx * t, py = y0 + dy * t;
		if (px < a->clip[0] || py < a->clip[1] || px >= a->clip[2] || py >= a->clip[3])
			continue;
		const bool white = (((int)(a->s + len * t) + a->phase) / 4 & 1) == 0;
		R_DrawFill32_RTX((int)px, (int)py, 1, 1, white ? MakeColor(255, 255, 255, 255) : MakeColor(0, 0, 0, 255));
	}
	a->s += len;
}

// one shape's outline (its points in map repeats) on a W x H map
static void ants_shape(ants_t *a, int shape, const float *p, int count, int W, int H)
{
	if (count < 2)
		return;
	a->ox = -(float)W * floorf(p[0]);
	a->oy = -(float)H * floorf(p[1]);
	float pts[2 * 64];
	float *poly = pts;
	int n = 0;
	if (shape == SEL_LASSO)
	{
		poly = Z_Malloc(sizeof(float) * 2 * count);
		for (int i = 0; i < count; i++)
			ants_point(a, p[i * 2] * W, p[i * 2 + 1] * H, &poly[i * 2], &poly[i * 2 + 1]);
		n = count;
	}
	else
	{
		// the texels' box: from the first one's corner to past the last one
		const float x0 = floorf(min(p[0], p[2]) * W), x1 = floorf(max(p[0], p[2]) * W) + 1.f;
		const float y0 = floorf(min(p[1], p[3]) * H), y1 = floorf(max(p[1], p[3]) * H) + 1.f;
		if (shape == SEL_RECT)
		{
			const float c[8] = { x0, y0, x1, y0, x1, y1, x0, y1 };
			for (int i = 0; i < 4; i++)
				ants_point(a, c[i * 2], c[i * 2 + 1], &pts[i * 2], &pts[i * 2 + 1]);
			n = 4;
		}
		else
		{
			for (int i = 0; i < 64; i++)
			{
				const float t = (float)i * 2.f * (float)M_PI / 64.f;
				ants_point(a, (x0 + x1) * 0.5f + cosf(t) * (x1 - x0) * 0.5f, (y0 + y1) * 0.5f + sinf(t) * (y1 - y0) * 0.5f,
					&pts[i * 2], &pts[i * 2 + 1]);
			}
			n = 64;
		}
	}
	// a long outline is dotted more sparsely: at most a few thousand dots
	float len = 0.f;
	for (int i = 0; i < n; i++)
	{
		const int j = (i + 1) % n;
		len += sqrtf((poly[j * 2] - poly[i * 2]) * (poly[j * 2] - poly[i * 2]) + (poly[j * 2 + 1] - poly[i * 2 + 1]) * (poly[j * 2 + 1] - poly[i * 2 + 1]));
	}
	a->step = max(1.f, len / 4000.f);
	for (int i = 0; i < n; i++)
	{
		const int j = (i + 1) % n;
		ants_line(a, poly[i * 2], poly[i * 2 + 1], poly[j * 2], poly[j * 2 + 1]);
	}
	if (poly != pts)
		Z_Free(poly);
}

static void sel_outline_2d(float vx, float vy, float vs, int W, int H)
{
	if (!sl.num_steps && !sl.dragging)
		return;
	ants_t a;
	memset(&a, 0, sizeof(a));
	a.clip[0] = vx;
	a.clip[1] = vy;
	a.clip[2] = vx + vs;
	a.clip[3] = vy + vs;
	a.cx = vx + vs * 0.5f;
	a.cy = vy + vs * 0.5f;
	a.phase = (int)(Sys_Milliseconds() / 80u % 8u);
	if (!(sl.dragging && sl.drag_replace))
	{
		for (int i = 0; i < sl.num_steps; i++)
		{
			const sel_step_t *s = &sl.steps[i];
			if (s->step == SELSTEP_ADD || s->step == SELSTEP_SUB)
				ants_shape(&a, s->shape, sl.pts + 2 * s->first, s->count, W, H);
		}
	}
	if (sl.dragging)
		ants_shape(&a, sl.drag_shape, sl.dpts, sl.num_dpts, W, H);
}

static bool hit_on_map(const author_hit_t *hit, const image_t *img);

static void draw_view2d(qhandle_t font)
{
	float vx, vy, vs;
	view2d_rect(&vx, &vy, &vs);
	view2d_check_fit();

	image_t *img = author_image();
	const image_t *shown = img ? img : author_base();
	int W, H;
	map_size(&W, &H);

	R_DrawFill32_RTX((int)vx - 2, (int)vy - 2, (int)vs + 4, (int)vs + 4 + 24, MakeColor(0, 0, 0, 220));
	R_DrawFill32_RTX((int)vx, (int)vy, (int)vs, (int)vs, MakeColor(40, 0, 40, 255));
	add_widget(W_VIEW2D, vx, vy, vs, vs);

	const float half = vs * 0.5f / au.v2_ppt;
	const float u0 = max(0.f, au.v2_cu - half), u1 = min((float)W, au.v2_cu + half);
	const float v0 = max(0.f, au.v2_cv - half), v1 = min((float)H, au.v2_cv + half);
	if (shown && u1 > u0 && v1 > v0)
	{
		const float cx = vx + vs * 0.5f, cy = vy + vs * 0.5f;
		const float sx0 = cx + (u0 - au.v2_cu) * au.v2_ppt, sx1 = cx + (u1 - au.v2_cu) * au.v2_ppt;
		const float sy0 = cy + (v0 - au.v2_cv) * au.v2_ppt, sy1 = cy + (v1 - au.v2_cv) * au.v2_ppt;
		const bool nearest = au.v2_ppt / ui_scale() >= 2.f;
		const bool red_grey = img && slot_is_grey(au.slot);
		const image_t *base = author_base();
		const image_t *glow = NULL;
		int glow_kind = DRAW_GLOW_OFF;
		if (glow_view() && base)
		{
			// the painted map's brightness; before the first stroke, the glow the material
			// has, read the way that stroke will convert it (create_emit_map)
			const image_t *old = au.mat->image_emissive;
			if (img)
				glow = img, glow_kind = DRAW_GLOW_ALPHA;
			else if (old && au.mat->emissive_factor > 0.f)
				glow = old, glow_kind = DRAW_GLOW_LUMINANCE;
			else
				glow = base, glow_kind = DRAW_GLOW_NONE;
		}
		if (glow)
			vkpt_draw_stretch_subpic_layer(sx0, sy0, sx1 - sx0, sy1 - sy0, u0 / W, v0 / H, u1 / W, v1 / H,
				(qhandle_t)image_index(glow), (qhandle_t)image_index(base), au.glow_veil, nearest, false, glow_kind);
		else if (img && au.show_base && base && base != img)
		{
			// the base texture, and the map over it as a layer (O, or the panel's slider) - mixed
			// by the shader as an image editor does, not blended as light (see the function)
			vkpt_draw_stretch_subpic_layer(sx0, sy0, sx1 - sx0, sy1 - sy0, u0 / W, v0 / H, u1 / W, v1 / H,
				(qhandle_t)image_index(img), (qhandle_t)image_index(base), au.layer_alpha, nearest, red_grey,
				DRAW_GLOW_OFF);
		}
		else
			vkpt_draw_stretch_subpic_flags(sx0, sy0, sx1 - sx0, sy1 - sy0, u0 / W, v0 / H, u1 / W, v1 / H,
				(qhandle_t)image_index(shown), nearest, red_grey, true);

		// the lock and the selection: what is outside them, darker (their texture's alpha)
		if (region_on() && rg.w == W && rg.h == H && region_image_ok())
			vkpt_draw_stretch_subpic_flags(sx0, sy0, sx1 - sx0, sy1 - sy0, u0 / W, v0 / H, u1 / W, v1 / H,
				rg.image, true, false, false);

		if (!img && glow)
		{
			// the texture shows what glows already: the note on a band of its own
			const char *note = glow_kind == DRAW_GLOW_LUMINANCE
				? "not painted yet: its glow as it is now - painting makes a map"
				: "nothing glows yet - painting makes an emissive map";
			R_DrawFill32_RTX((int)vx, (int)vy, (int)vs, 20, MakeColor(0, 0, 0, 170));
			ui_text(vx + 6.f, vy + 6.f, UI_ALTCOLOR, note, (int)((vs - 12.f) / CHAR_WIDTH), font);
		}
		else if (!img)
		{
			R_DrawFill32_RTX((int)sx0, (int)sy0, (int)(sx1 - sx0), (int)(sy1 - sy0), MakeColor(0, 0, 0, 150));
			ui_text(vx + 6.f, vy + 6.f, UI_ALTCOLOR, va("no %s map yet - painting makes one", slot_word[au.slot]),
				(int)((vs - 12.f) / CHAR_WIDTH), font);
		}
	}

	// the brush where it would paint: under the cursor here, or where the cursor is on a surface;
	// a line being drawn, swept from its start to where the cursor is
	float tu, tv;
	outline_t o;
	memset(&o, 0, sizeof(o));
	if (drag_preview())
	{
		drag_outline(W, H, &o);
		outline_2d(vx, vy, vs, &o, au.paint_2d ? MakeColor(255, 255, 255, 255) : MakeColor(255, 170, 60, 255));
	}
	else if (view2d_texel(&tu, &tv))
	{
		o.c2x = centre2_x(tu);
		o.c2y = centre2_y(tv);
		outline_2d(vx, vy, vs, &o, MakeColor(255, 255, 255, 255));
	}
	else if (au.hit.valid && hit_on_map(&au.hit, img))
	{
		hit_texel(&au.hit, W, H, true, &tu, &tv);
		o.c2x = centre2_x(tu);
		o.c2y = centre2_y(tv);
		outline_2d(vx, vy, vs, &o, MakeColor(255, 170, 60, 255));
	}

	// the selection's shapes, and one being dragged out (here or on a surface)
	sel_outline_2d(vx, vy, vs, W, H);

	char caption[160], what[48];
	if (au.stage >= 0)
		Q_snprintf(what, sizeof(what), "stage%d %s", au.stage, slot_word[au.slot]);
	else
		Q_strlcpy(what, slot_word[au.slot], sizeof(what));
	if (view2d_texel(&tu, &tv))
	{
		// a painted emissive map: the brightness under the cursor too
		char glow_at[24] = "";
		if (au.slot == SLOT_EMISSIVE && img && img->pix_data && img->pixel_format == PF_R8G8B8A8_UNORM
			&& img->upload_width > 0 && img->upload_height > 0)
		{
			const int gx = Q_clip((int)tu, 0, img->upload_width - 1), gy = Q_clip((int)tv, 0, img->upload_height - 1);
			const byte a = img->pix_data[((size_t)gy * img->upload_width + gx) * 4 + 3];
			Q_snprintf(glow_at, sizeof(glow_at), "  glows %d%%", (int)(a * 200.f / 255.f + 0.5f));
		}
		Q_snprintf(caption, sizeof(caption), "%s %dx%d  zoom %d%%  texel %d,%d%s", what, W, H,
			(int)(au.v2_ppt / ui_scale() * 100.f + 0.5f), (int)tu, (int)tv, glow_at);
	}
	else
		Q_snprintf(caption, sizeof(caption), "%s %dx%d  zoom %d%%  wheel zooms, right-drag pans, O base", what,
			W, H, (int)(au.v2_ppt / ui_scale() * 100.f + 0.5f));
	ui_text(vx, vy + vs + 6.f, 0, caption, (int)(vs / CHAR_WIDTH), font);
}

static void draw_cursor(void)
{
	float mx, my;
	cursor_virtual(&mx, &my);
	const uint32_t black = MakeColor(0, 0, 0, 255), white = MakeColor(255, 255, 255, 255);
	R_DrawFill32_RTX((int)mx - 5, (int)my - 1, 11, 3, black);
	R_DrawFill32_RTX((int)mx - 1, (int)my - 5, 3, 11, black);
	R_DrawFill32_RTX((int)mx - 4, (int)my, 9, 1, white);
	R_DrawFill32_RTX((int)mx, (int)my - 4, 1, 9, white);

	// selecting: the shape's mark beside the cross
	const int sx = (int)mx + 6, sy = (int)my + 6;
	switch (au.select)
	{
	case SEL_RECT:
		R_DrawFill32_RTX(sx, sy, 7, 1, white);
		R_DrawFill32_RTX(sx, sy + 5, 7, 1, white);
		R_DrawFill32_RTX(sx, sy, 1, 6, white);
		R_DrawFill32_RTX(sx + 6, sy, 1, 6, white);
		break;
	case SEL_ELLIPSE:
	case SEL_LASSO:
		for (int i = 0; i < 16; i++)
		{
			const float t = (float)i * 2.f * (float)M_PI / 16.f;
			R_DrawFill32_RTX(sx + 3 + (int)floorf(cosf(t) * 3.5f + 0.5f), sy + 3 + (int)floorf(sinf(t) * 2.5f + 0.5f), 1, 1, white);
		}
		if (au.select == SEL_LASSO)
			R_DrawFill32_RTX(sx + 1, sy + 6, 1, 3, white);    // the rope's tail
		break;
	default:
		break;
	}
}

void MAT_AuthorDraw(void)
{
	num_widgets = 0;
	if (!au.active || !material_ok())
		return;

	const qhandle_t font = R_RegisterFont("conchars");
	vkpt_draw_push_state(ui_scale());
	if (au.show_2d)
		draw_view2d(font);
	draw_panel(font);
	draw_cursor();
	vkpt_draw_pop_state();
}

/* ------------------------------------------------------------------------- */
/* PAINTING, BETWEEN FRAMES                                                  */
/* ------------------------------------------------------------------------- */

// (hit) is on a surface showing (img) - or, while the map does not exist, the material
static bool hit_on_map(const author_hit_t *hit, const image_t *img)
{
	if (!hit->valid)
		return false;
	const pbr_material_t *m = MAT_ForIndex(hit->material);
	if (!m)
		return false;
	// a stage's own map: the material's surfaces (the shader's test too)
	if (img && au.stage < 0)
		return slot_image(m, au.slot) == img;
	return m == au.mat;
}

/* The line and rectangle strokes, each tick while the button is down: where it
   starts (the first place the cursor is on the map) and where it ends so far;
   once the button has come up, the brush swept along it - or the rectangle -
   painted once as a stroke of its own. */
static void line_step(image_t *img, dab_ctx_t *c)
{
	const int W = img->upload_width, H = img->upload_height;
	float tu, tv;
	bool have;
	if (au.paint_2d && au.line_have)
	{
		/* Once started, the end follows the cursor wherever it goes - off the
		   map, past the view's edge - held to the map's edge (Matt, 2026-11-21:
		   it stopped dead as soon as the cursor left the texture). */
		float x, y, size, mx, my;
		view2d_rect(&x, &y, &size);
		cursor_virtual(&mx, &my);
		tu = Q_clipf(au.v2_cu + (mx - (x + size * 0.5f)) / au.v2_ppt, 0.f, (float)W - 0.001f);
		tv = Q_clipf(au.v2_cv + (my - (y + size * 0.5f)) / au.v2_ppt, 0.f, (float)H - 0.001f);
		have = true;
	}
	else if (au.paint_2d)
		have = view2d_texel(&tu, &tv);
	else
	{
		have = hit_on_map(&au.hit, img);
		if (have)
			hit_texel(&au.hit, W, H, false, &tu, &tv);     // unwrapped: the line can cross a tile's edge
	}
	if (have)
	{
		if (!au.line_have)
		{
			au.line_have = true;
			au.line_tu0 = tu;
			au.line_tv0 = tv;
			au.line_hit = au.hit;
			// a line on a model: the island of the map it starts on, to keep it there
			if (!au.paint_2d && au.stroke == STROKE_LINE)
				island_build(&au.hit, W, H, tu, tv);
			else
				island_clear();
		}
		bool take = true;
		if (ln.on && !au.paint_2d && ln.mask_w == W && ln.mask_h == H)
		{
			bool held;
			take = island_clamp(au.line_tu0, au.line_tv0, &tu, &tv, &held);
			held = held || !take;
			if (held && !ln.held)
			{
				// on the panel only: this comes and goes as the cursor moves
				Q_strlcpy(au.status, "the line stops where this part of the model ends on the map", sizeof(au.status));
				au.status_ms = Sys_Milliseconds();
			}
			ln.held = held;
		}
		if (take)
		{
			au.line_tu1 = tu;
			au.line_tv1 = tv;
		}
	}
	if (!au.line_commit)
		return;

	au.line_commit = au.line_active = false;
	au.painting = false;
	if (!au.line_have)
		return;
	au.line_have = false;
	const bool held = ln.on && ln.held;
	island_clear();

	// ends on two parts of a model whose UVs lie far apart make no line on it
	const bool rect = au.stroke == STROKE_RECT;
	if (!au.paint_2d && (fabsf(au.line_tu1 - au.line_tu0) > 8.f * W || fabsf(au.line_tv1 - au.line_tv0) > 8.f * H))
	{
		author_status("the %s's ends are too far apart on the map (two parts of it far apart?): not painted",
			rect ? "rectangle" : "line");
		return;
	}

	outline_t o;
	drag_outline(W, H, &o);
	make_paint(au.paint_2d ? NULL : &au.line_hit, &c->paint);
	c->wrap = !au.paint_2d && map_wraps();
	// the spray, once: density is the share of the texels it dots
	c->spray_p = au.tool_opt[TOOL_SPRAY];
	stroke_touch(img);
	if (rect)
	{
		// the blur reaches as a brush the rectangle's shorter side across would
		c->blur_r = Q_clip(min(o.dx, o.dy) / 6, 1, 6);
		dab_rect(c, o.c2x, o.c2y, o.dx, o.dy, o.r2);
	}
	else
		dab_sweep(c, o.c2x, o.c2y, o.dx, o.dy);
	stroke_end();
	if (rect)
		author_status("rectangle %d x %d texels, corners %d", o.dx, o.dy, o.r2 / 2);
	else
	{
		const float len = sqrtf((au.line_tu1 - au.line_tu0) * (au.line_tu1 - au.line_tu0)
			+ (au.line_tv1 - au.line_tv0) * (au.line_tv1 - au.line_tv0));
		author_status("line, %d texels long%s", (int)(len + 0.5f),
			held ? " - stopped where its part of the model ends on the map" : "");
	}
}

/* THE SELECTION BEING DRAGGED OUT (M). The button goes down (select_begin); each
   tick while it is down, a rectangle or ellipse runs from where the cursor
   first was on the map to where it is, a lasso along where it has been - the
   surface showing it as it grows (region_sync); let go, it joins the
   selection: replacing it, added (Shift) or taken away (Ctrl). A click that
   drags nothing out drops the selection, as an image editor's does. */
static void select_begin(bool in_2d, bool add, bool sub)
{
	sl.dragging = true;
	sl.drag_2d = in_2d;
	sl.drag_commit = false;
	sl.drag_shape = au.select;
	sl.drag_replace = !add && !sub;
	sl.drag_sub = sub && !add;
	sl.num_dpts = 0;
	sl.dirty = true;
	sl.raster_ms = 0;
	queue_tick();
}

static void sel_drag_point(float u, float v)
{
	if (sl.num_dpts == sl.cap_dpts)
	{
		sl.cap_dpts = max(256, sl.cap_dpts * 2);
		sl.dpts = Z_Realloc(sl.dpts, sizeof(float) * 2 * sl.cap_dpts);
	}
	sl.dpts[sl.num_dpts * 2] = u;
	sl.dpts[sl.num_dpts * 2 + 1] = v;
	sl.num_dpts++;
	sl.dirty = true;
}

static void select_commit(int W, int H)
{
	const bool replace = sl.drag_replace, sub = sl.drag_sub, in_2d = sl.drag_2d;
	const int shape = sl.drag_shape, count = sl.num_dpts;
	sl.dragging = sl.drag_commit = false;
	sl.dirty = true;
	rg.dirty = true;

	float x0 = FLT_MAX, y0 = FLT_MAX, x1 = -FLT_MAX, y1 = -FLT_MAX;
	for (int i = 0; i < count; i++)
	{
		x0 = min(x0, sl.dpts[i * 2] * W);
		x1 = max(x1, sl.dpts[i * 2] * W);
		y0 = min(y0, sl.dpts[i * 2 + 1] * H);
		y1 = max(y1, sl.dpts[i * 2 + 1] * H);
	}
	bool click;
	if (shape == SEL_LASSO)
		click = count < 3 || (x1 - x0 < 1.f && y1 - y0 < 1.f);
	else
		click = count < 2 || ((int)floorf(sl.dpts[0] * W) == (int)floorf(sl.dpts[2] * W)
			&& (int)floorf(sl.dpts[1] * H) == (int)floorf(sl.dpts[3] * H));
	if (click)
	{
		sl.num_dpts = 0;
		if (replace && sl.num_steps)
			sel_clear("selection dropped - Ctrl+Shift+D brings it back");
		return;
	}
	// ends on two parts of a model whose UVs lie far apart make no shape on it
	if (!in_2d && (x1 - x0 > SEL_MAX_TILES * W || y1 - y0 > SEL_MAX_TILES * H))
	{
		sl.num_dpts = 0;
		author_status("the %s spans too much of the map (two parts of it far apart?): not selected", sel_label[shape]);
		return;
	}

	if (replace)
		sel_clear(NULL);    // kept for Ctrl+Shift+D
	sel_push(sub ? SELSTEP_SUB : SELSTEP_ADD, shape, sl.dpts, count);
	sl.steps[sl.num_steps - 1].clip = in_2d;
	sl.num_dpts = 0;
	region_sync();
	if (!sl.texels)
	{
		sel_clear(NULL);
		author_status("nothing is selected now");
		return;
	}
	char size[40] = "";
	if (in_2d)
	{
		// what of it lies on the map
		x0 = Q_clipf(x0, 0.f, (float)W - 0.001f);
		x1 = Q_clipf(x1, 0.f, (float)W - 0.001f);
		y0 = Q_clipf(y0, 0.f, (float)H - 0.001f);
		y1 = Q_clipf(y1, 0.f, (float)H - 0.001f);
	}
	if (shape != SEL_LASSO)
		Q_snprintf(size, sizeof(size), " %d x %d", (int)floorf(x1) - (int)floorf(x0) + 1, (int)floorf(y1) - (int)floorf(y0) + 1);
	author_status("%s%s %s: %d texels selected - painting stays inside, Ctrl+D drops it", sel_label[shape], size,
		sub ? "taken away" : replace ? "selected" : "added", sl.texels);
}

static void select_step(void)
{
	int W, H;
	map_size(&W, &H);
	float tu = 0.f, tv = 0.f;
	bool have = false;
	if (sl.drag_2d)
	{
		/* BEGUN OFF THE MAP. A shape in the map view starts anywhere in the view and
		   follows the cursor anywhere, off the map as well - so one begun or carried
		   past an edge takes every texel along it (Matt, 2026-11-21: "let me start
		   selecting outside of the image so I can get all pixels on the edges").
		   It used to wait for the cursor to reach the map, its first corner landing
		   a texel or more inside. Its points are kept where they are, up to a map's
		   width past each side; the shape is CLIPPED to the map (sel_step_t.clip),
		   never wrapped round to the far side as a shape on a tiled surface is, and
		   an ellipse keeps its true outline rather than one squeezed onto the map. */
		float x, y, size, mx, my;
		view2d_rect(&x, &y, &size);
		cursor_virtual(&mx, &my);
		if (sl.num_dpts || cursor_in_view2d())
		{
			tu = Q_clipf(au.v2_cu + (mx - (x + size * 0.5f)) / au.v2_ppt, -(float)W, 2.f * (float)W);
			tv = Q_clipf(au.v2_cv + (my - (y + size * 0.5f)) / au.v2_ppt, -(float)H, 2.f * (float)H);
			have = true;
		}
	}
	else if (hit_on_map(&au.hit, author_image()))
	{
		hit_texel(&au.hit, W, H, false, &tu, &tv);     // unwrapped: a shape can cross a tile's edge
		have = true;
	}
	if (have)
	{
		const float u = tu / (float)W, v = tv / (float)H;
		if (sl.drag_shape == SEL_LASSO)
		{
			bool moved = true;
			if (sl.num_dpts)
			{
				const float dx = (u - sl.dpts[sl.num_dpts * 2 - 2]) * W, dy = (v - sl.dpts[sl.num_dpts * 2 - 1]) * H;
				moved = dx * dx + dy * dy >= 0.25f;     // half a texel on
			}
			if (moved && sl.num_dpts < SEL_MAX_POINTS)
				sel_drag_point(u, v);
		}
		else if (!sl.num_dpts)
		{
			// a rectangle or ellipse: two opposite corners, the second following the cursor
			sel_drag_point(u, v);
			sel_drag_point(u, v);
		}
		else if (sl.dpts[2] != u || sl.dpts[3] != v)
		{
			sl.dpts[2] = u;
			sl.dpts[3] = v;
			sl.dirty = true;
		}
	}
	if (sl.drag_commit)
		select_commit(W, H);
}

static void paint_step(void)
{
	// a Quake III stage's maps: only while the stage still has a surface to paint
	if (au.stage >= 0)
	{
		const char *why;
		if (!stage_info(NULL, &why))
		{
			author_status("can't paint stage%d: %s", au.stage, why);
			au.painting = au.line_active = au.line_commit = false;
			return;
		}
	}

	image_t *img = author_image();
	if (!img)
	{
		const bool on = au.paint_2d ? cursor_in_view2d() : hit_on_map(&au.hit, NULL);
		if (!on || au.create_tried)
		{
			// a line let go of before it ever touched the map
			if (au.line_commit)
				au.painting = au.line_active = au.line_commit = au.line_have = false;
			return;
		}
		au.create_tried = true;
		img = create_map();
		if (!img)
		{
			au.painting = au.line_active = au.line_commit = au.line_have = false;
			return;
		}
	}

	const char *why;
	if (!image_paintable(img, &why))
	{
		author_status("can't paint %s: %s", img->name, why);
		au.painting = au.line_active = au.line_commit = au.line_have = false;
		return;
	}

	// the lock and the selection, for this map (a new map is the base texture's size, as they were made)
	region_sync();

	const int W = img->upload_width, H = img->upload_height;
	dab_ctx_t c;
	memset(&c, 0, sizeof(c));
	c.img = img;
	c.slot = au.slot;

	// the spray's chance a texel is hit this tick: faster with density, steady whatever the frame rate
	if (au.tool == TOOL_SPRAY)
	{
		const unsigned now = Sys_Milliseconds();
		const float dt = au.spray_ms ? Q_clipf((float)(now - au.spray_ms) * 0.001f, 0.f, 0.1f) : 1.f / 60.f;
		au.spray_ms = now;
		const float rate = 0.3f + au.tool_opt[TOOL_SPRAY] * 8.f;   // per second
		c.spray_p = 1.f - expf(-rate * dt);
	}

	if (au.line_active)
	{
		line_step(img, &c);
		return;
	}

	if (au.paint_2d)
	{
		float tu, tv;
		if (!view2d_texel(&tu, &tv))
		{
			au.have_last = false;
			return;
		}
		make_paint(NULL, &c.paint);
		stroke_touch(img);
		c.wrap = false;
		if (au.tool == TOOL_FILL)
		{
			if (!au.fill_done)
				fill_at(&c, (int)floorf(tu), (int)floorf(tv));
			au.fill_done = true;
			return;
		}
		if (au.have_last)
			dab_line(&c, au.last_tu, au.last_tv, tu, tv);
		else
			dab(&c, centre2_x(tu), centre2_y(tv));
		au.last_tu = tu;
		au.last_tv = tv;
		au.have_last = true;
		return;
	}

	if (!hit_on_map(&au.hit, img))
	{
		au.have_last = false;
		return;
	}
	float tu, tv;
	hit_texel(&au.hit, W, H, false, &tu, &tv);   // unwrapped: a line across a tile edge stays short
	make_paint(&au.hit, &c.paint);
	stroke_touch(img);
	c.wrap = map_wraps();
	if (au.tool == TOOL_FILL)
	{
		if (!au.fill_done)
			fill_at(&c, Q_clip(wrap_i((int)floorf(tu), W), 0, W - 1), Q_clip(wrap_i((int)floorf(tv), H), 0, H - 1));
		au.fill_done = true;
		return;
	}
	const float reach = max(32.f, 4.f * cur_size());
	if (au.have_last && fabsf(tu - au.last_tu) < reach && fabsf(tv - au.last_tv) < reach)
		dab_line(&c, au.last_tu, au.last_tv, tu, tv);
	else
		dab(&c, centre2_x(tu), centre2_y(tv));
	au.last_tu = tu;
	au.last_tv = tv;
	au.have_last = true;
}

/* EMISSIVE'S TINT (the panel's red, green, blue): how much of each of the
   texture's channels the glow keeps, one for the whole map, held in its colours
   (emit_bake). Read back when a material's painted map is first shown - its
   colours over the albedo times its brightness, channel by channel, which never
   clip as the map holds half - and a change makes every texel's colour again from
   its brightness: one undo step a drag. */
static void emit_sync(void)
{
	if (au.slot != SLOT_EMISSIVE || au.stage >= 0)
		return;
	const int mi = MAT_IndexOf(au.mat);
	image_t *img = author_image();
	const int ii = img ? image_index(img) : -1;
	if (mi == au.emit_mat && ii == au.emit_image)
		return;
	if (mi != au.emit_mat)
		au.emit_tint[0] = au.emit_tint[1] = au.emit_tint[2] = 1.f;
	au.emit_mat = mi;
	au.emit_image = ii;

	byte probe[3];
	if (img && img->pix_data && img->pixel_format == PF_R8G8B8A8_UNORM
		&& base_rgb(0, 0, img->upload_width, img->upload_height, probe))
	{
		const int W = img->upload_width, H = img->upload_height;
		double num[3] = { 0.0, 0.0, 0.0 }, den[3] = { 0.0, 0.0, 0.0 };
		for (int y = 0; y < H; y++)
		{
			for (int x = 0; x < W; x++)
			{
				const byte *px = img->pix_data + ((size_t)y * W + x) * 4;
				byte albedo[3];
				if (!px[3] || !base_rgb(x, y, W, H, albedo))
					continue;
				const float b = (float)px[3] / 255.f;
				for (int c = 0; c < 3; c++)
				{
					const float k = srgb_to_linear((float)albedo[c] / 255.f) * b;
					if (k < 0.02f)
						continue;
					num[c] += img->is_srgb ? srgb_to_linear((float)px[c] / 255.f) : (float)px[c] / 255.f;
					den[c] += k;
				}
			}
		}
		for (int c = 0; c < 3; c++)
			if (den[c] > 0.0)
				au.emit_tint[c] = floorf(Q_clipf((float)(num[c] / den[c]), 0.f, 1.f) * 100.f + 0.5f) / 100.f;
	}
	memcpy(au.emit_baked, au.emit_tint, sizeof(au.emit_baked));
}

static void emit_retint(void)
{
	if (au.slot != SLOT_EMISSIVE || au.stage >= 0 || !memcmp(au.emit_tint, au.emit_baked, sizeof(au.emit_tint)))
		return;
	memcpy(au.emit_baked, au.emit_tint, sizeof(au.emit_baked));
	image_t *img = author_image();
	const char *why;
	if (!img || !image_paintable(img, &why))
		return;     // no painted map yet: the first stroke's takes the tint as it is

	stroke_touch(img);
	const int W = img->upload_width, H = img->upload_height;
	bool changed = false;
	for (int y = 0; y < H; y++)
	{
		for (int x = 0; x < W; x++)
		{
			float v[3];
			texel_get(img, img->pix_data, (size_t)y * W + x, SLOT_EMISSIVE, v);
			changed |= texel_put(img, x, y, SLOT_EMISSIVE, v);
		}
	}
	if (changed)
	{
		au.upload_image = image_index(img);
		image_dirty[au.upload_image] = (uint8_t)(SLOT_EMISSIVE + 1);
		image_stage[au.upload_image] = 0;
	}
	// a drag keeps its stroke open until the button comes up (MAT_AuthorKey)
	if (au.drag < W_RED || au.drag > W_BLUE)
		stroke_end();
}

static void author_tick(void)
{
	au.tick_queued = false;
	if (!au.active)
		return;
	if (!material_ok())
	{
		author_stop(" - the material is gone (a new map?)");
		return;
	}

	// photo mode ended (the pause key), or never began
	au.pause_frames++;
	if (cl_paused->integer != 2 && au.pause_frames > 2)
	{
		author_stop(" - photo mode ended");
		return;
	}
	if (!sv_paused->integer && au.pause_frames > 60)
	{
		author_stop(" - the game would not pause (a local game only)");
		return;
	}

	if (au.lock_request)
	{
		const int kind = au.lock_request;
		au.lock_request = 0;
		lock_command(kind, au.lock_toggle);
	}
	// a selection made on another material or stage lies on nothing of this one
	if (sl.num_steps && (sl.material != MAT_IndexOf(au.mat) || sl.stage != au.stage))
		sel_clear("the selection was on another material or stage: dropped");
	if (sl.dragging)
		select_step();
	region_sync();
	emit_sync();
	emit_retint();

	if (au.range_request)
	{
		au.range_request = false;
		range_apply();
	}

	if (au.painting)
		paint_step();

	if (au.upload_image >= 0 && (!au.painting || Sys_Milliseconds() - au.last_upload_ms >= AUTHOR_UPLOAD_MS))
		upload_now();
}

/* ------------------------------------------------------------------------- */
/* THE RENDERER'S SIDE                                                       */
/* ------------------------------------------------------------------------- */

bool MAT_AuthorActive(void)
{
	return au.active;
}

/* PHOTO MODE AT HALF THE RAYS (Matt, 2026-11-21: "have the accumulation mode cast
   half the rays ... it should do half the samples per frame so it improves the
   responsiveness of the tool. And it should still complete a 4k image").

   While the tool is open, photo mode's accumulation runs as sparse rendering
   (pt_sparse, main.c sparse_rendering_rate): each frame lights a blue-noise
   mat_author_sparse of the pixels - direct light and every bounce - and weights
   them by its inverse. That is unbiased, so the plain average photo mode keeps
   converges to the same picture, at the full render resolution; each pixel just
   gets its samples every other frame. The progress count needs that many more
   frames to read 100% (evaluate_reference_mode), so "done" is the same samples
   a pixel as before. The primary rays, the fog and the compositing stay at every
   pixel, so a frame costs less than half as much, not half. It saves time only
   where pt_sparse does: with the tile lists (ray query, one GPU,
   pt_tile_compaction 1) - else the same picture at the same cost.

   0 or 1 = off, every pixel every frame. Below 1/16 is held at 1/16. Read every
   frame. */
static cvar_t *mat_author_sparse;

float MAT_AuthorSparseRate(void)
{
	if (!mat_author_sparse)
		mat_author_sparse = Cvar_Get("mat_author_sparse", "0.5", CVAR_ARCHIVE);
	if (!au.active)
		return 0.f;

	const float rate = mat_author_sparse->value;
	if (rate <= 0.f || rate >= 1.f)
		return 0.f;
	return max(rate, 1.f / 16.f);
}

/* THE FLASHLIGHT (Matt, 2026-11-22: "a toggle for a camera light. Basically a
   flashlight kind of thing for the material editor camera"). While the tool is
   open and mat_author_light is 1 - the panel's "flashlight", C, 'mat author
   light' - a spot light sits at the free camera and points where it looks.

   - Brightness: the game's own flashlight (cl_flashlight_intensity's default,
     20000) times mat_author_light_scale, the panel's slider in percent.
   - Cone: mat_author_light_cone degrees across, at full brightness over the
     middle half of its width, fading to nothing at its edge.
   - At the eye itself, not offset like the game's flashlight (view.c
     V_Flashlight, which clears the gun): close work on a wall needs the light
     right where the camera is, and a light on the view axis casts no shadow
     you can see, so nothing hides what is being painted.
   - No fog glow (volumetric scale 0): a headlamp's beam in the haze would veil
     the very surface under the brush.
   - One id, so ReSTIR keeps its reservoir as the camera flies.

   main.c adds it with the frame's dlights, after vkpt_freecam_update has moved
   fd's view to the free camera. Turning it on or off, or changing either cvar,
   starts photo mode's average again, as a camera move does. */
#define AUTHOR_LIGHT_INTENSITY  20000.f
#define AUTHOR_LIGHT_ID         0x4d41544cu     // "MATL"

bool MAT_AuthorCameraLight(const refdef_t *fd, dlight_t *light)
{
	static bool was_on;
	static float was_scale, was_cone;

	light_cvars();
	const bool on = au.active && mat_author_light->integer;
	const float scale = max(0.f, mat_author_light_scale->value);
	const float cone = Q_clipf(mat_author_light_cone->value, 1.f, 179.f);
	if (on != was_on || (on && (scale != was_scale || cone != was_cone)))
		vkpt_reset_accumulation();
	was_on = on;
	was_scale = scale;
	was_cone = cone;
	if (!on || scale <= 0.f)
		return false;

	vec3_t forward;
	AngleVectors(fd->viewangles, forward, NULL, NULL);

	memset(light, 0, sizeof(*light));
	VectorCopy(fd->vieworg, light->origin);
	VectorSet(light->color, 1.f, 1.f, 1.f);
	light->intensity = AUTHOR_LIGHT_INTENSITY * scale;
	light->radius = 1.f;
	light->volumetric_scale = 0.f;
	light->id = AUTHOR_LIGHT_ID;
	light->light_type = DLIGHT_SPOT;
	VectorCopy(forward, light->spot.direction);
	light->spot.emission_profile = DLIGHT_SPOT_EMISSION_PROFILE_FALLOFF;
	const float half = DEG2RAD(cone * 0.5f);
	light->spot.cos_total_width = cosf(half);
	light->spot.cos_falloff_start = cosf(half * 0.5f);
	return true;
}

void MAT_AuthorCursorMove(float dx, float dy)
{
	if (!au.active)
		return;
	const float speed = (float)r_config.height / 1080.f * (mat_author_cursor_speed ? mat_author_cursor_speed->value : 1.f);

	if (au.right_pan && au.show_2d)
	{
		const float s = ui_scale();
		au.v2_cu -= dx * speed * s / au.v2_ppt;
		au.v2_cv -= dy * speed * s / au.v2_ppt;
		return;
	}

	/* A control held: the cursor slows, so a slider - which follows it - moves a
	   unit at a time without a steady hand; Shift slower again (not the dial's:
	   Shift snaps it to 45 degrees). */
	float drag_speed = 1.f;
	if (au.drag != W_NONE)
	{
		drag_speed = Q_clipf(mat_author_slider_speed ? mat_author_slider_speed->value : 0.25f, 0.01f, 1.f);
		if (Key_IsDown(K_SHIFT) && au.drag != W_DIAL)
			drag_speed *= 0.25f;
	}

	au.cx = Q_clipf(au.cx + dx * speed * drag_speed, 0.f, (float)(r_config.width - 1));
	au.cy = Q_clipf(au.cy + dy * speed * drag_speed, 0.f, (float)(r_config.height - 1));

	if (au.drag != W_NONE)
		drag_update(au.drag);
	if (au.painting || sl.dragging)
		queue_tick();
}

void MAT_AuthorFillUBO(QVKUniformBuffer_t *ubo)
{
	ubo->author_active = 0;
	ubo->author_cursor_x = ubo->author_cursor_y = -1;
	ubo->author_brush_valid = 0;
	ubo->author_line = 0;
	ubo->author_stage = -1;
	if (!au.active || !material_ok())
		return;

	// the glass mask, and the material stopped being stained glass ('mat
	// stained_glass 0'): its button is gone, so is the slot
	if (!slot_offered(au.mat, au.slot))
	{
		stroke_end();
		au.slot = SLOT_ROUGH;
		au.v2_fit_image = -2;
		author_status("%s isn't stained glass any more: painting its roughness map", au.mat->name);
	}

	ubo->author_active = 1;
	au.seq++;
	ubo->author_seq = (int)au.seq;

	// what the cursor is over: the panel and the map view are not the scene
	const bool on_view = !cursor_on_ui();
	au.seq_on_view[au.seq % AUTHOR_SEQ_RING] = on_view;
	if (on_view && r_config.width > 0 && r_config.height > 0)
	{
		ubo->author_cursor_x = Q_clip((int)(au.cx * ubo->width / r_config.width), 0, ubo->width - 1);
		ubo->author_cursor_y = Q_clip((int)(au.cy * ubo->height / r_config.height), 0, ubo->height - 1);
	}

	image_t *img = author_image();
	int W, H;
	map_size(&W, &H);
	ubo->author_image = image_index(img);
	ubo->author_material = MAT_IndexOf(au.mat);
	ubo->author_slot = au.slot;
	ubo->author_map_w = W;
	ubo->author_map_h = H;
	ubo->author_brush_size = cur_size();
	ubo->author_rect_r2 = 0;
	// the lock and the selection, once rasterised for this map (region_sync, between frames)
	ubo->author_lock_image = (region_on() && rg.w == W && rg.h == H && region_image_ok()) ? (int)rg.image : 0;

	// a Quake III stage's own map: the material's surfaces, through the stage's tcMods
	if (au.stage >= 0 && stage_info(NULL, NULL))
	{
		ubo->author_stage = au.stage;
		ubo->author_stage_clamp = au.stage_clamp ? 1 : 0;
		ubo->author_stage_tc[0] = au.stage_tc[0];
		ubo->author_stage_tc[1] = au.stage_tc[1];
		ubo->author_stage_tc[2] = au.stage_tc[2];
		ubo->author_stage_tc[3] = au.stage_tc[3];
		ubo->author_stage_tc_offset[0] = au.stage_tc[4];
		ubo->author_stage_tc_offset[1] = au.stage_tc[5];
	}
	else if (au.stage >= 0)
	{
		// the stage has nothing to paint (the panel says why): no surface marked, no brush -
		// the cursor's readback still runs, for Alt + click
		ubo->author_image = 0;
		ubo->author_material = -1;
		return;
	}

	// the brush's centre: a line being drawn, swept from its start, or a rectangle; under the
	// cursor in the map view; else where it is on a surface
	float tu, tv;
	if (drag_preview())
	{
		outline_t o;
		drag_outline(W, H, &o);
		ubo->author_brush_valid = 1;
		ubo->author_line = o.kind == OUT_RECT ? 2 : 1;
		ubo->author_center2_x = o.c2x;
		ubo->author_center2_y = o.c2y;
		ubo->author_line_dx2 = o.dx;
		ubo->author_line_dy2 = o.dy;
		ubo->author_rect_r2 = o.r2;
	}
	else if (view2d_texel(&tu, &tv))
	{
		ubo->author_brush_valid = 1;
		ubo->author_center2_x = centre2_x(tu);
		ubo->author_center2_y = centre2_y(tv);
	}
	else if (on_view && hit_on_map(&au.hit, img))
	{
		hit_texel(&au.hit, W, H, true, &tu, &tv);
		ubo->author_brush_valid = 1;
		ubo->author_center2_x = centre2_x(tu);
		ubo->author_center2_y = centre2_y(tv);
	}
}

void MAT_AuthorReadback(const ReadbackBuffer *rb)
{
	if (!au.active)
		return;

	// a write from one of the last few frames, with the cursor on the view then
	const uint32_t age = au.seq - rb->author_seq;
	const bool fresh = rb->author_seq != 0 && age <= 6 && au.seq_on_view[rb->author_seq % AUTHOR_SEQ_RING];
	if (!fresh || rb->author_material == ~0u)
	{
		au.hit.valid = false;
	}
	else
	{
		au.hit.valid = true;
		au.hit.seq = rb->author_seq;
		au.hit.material = (int)rb->author_material;
		au.hit.u = rb->author_u;
		au.hit.v = rb->author_v;
		au.hit.tx[0] = rb->author_tx_x;
		au.hit.tx[1] = rb->author_tx_y;
		au.hit.ty[0] = rb->author_ty_x;
		au.hit.ty[1] = rb->author_ty_y;
		au.hit.duv[0] = rb->author_dudx;
		au.hit.duv[1] = rb->author_dvdx;
		au.hit.duv[2] = rb->author_dudy;
		au.hit.duv[3] = rb->author_dvdy;
		au.hit.tri_uv[0] = rb->author_tri_u0;
		au.hit.tri_uv[1] = rb->author_tri_v0;
		au.hit.tri_uv[2] = rb->author_tri_u1;
		au.hit.tri_uv[3] = rb->author_tri_v1;
		au.hit.tri_uv[4] = rb->author_tri_u2;
		au.hit.tri_uv[5] = rb->author_tri_v2;
		au.hit.tri_pos[0] = rb->author_tri_p0x;
		au.hit.tri_pos[1] = rb->author_tri_p0y;
		au.hit.tri_pos[2] = rb->author_tri_p0z;
		au.hit.tri_pos[3] = rb->author_tri_p1x;
		au.hit.tri_pos[4] = rb->author_tri_p1y;
		au.hit.tri_pos[5] = rb->author_tri_p1z;
		au.hit.tri_pos[6] = rb->author_tri_p2x;
		au.hit.tri_pos[7] = rb->author_tri_p2y;
		au.hit.tri_pos[8] = rb->author_tri_p2z;
	}

	// every frame: the stroke, the uploads, and whether photo mode is still on
	queue_tick();
}

/* ------------------------------------------------------------------------- */
/* KEYS                                                                      */
/* ------------------------------------------------------------------------- */

bool MAT_AuthorKey(unsigned key, bool down)
{
	if (!au.active)
		return false;

	const bool ctrl = Key_IsDown(K_CTRL), alt = Key_IsDown(K_ALT), shift = Key_IsDown(K_SHIFT);
	float mx, my;
	cursor_virtual(&mx, &my);

	// a number being typed into its box: its keys first (Escape leaves the box, not the tool);
	// a click off the panel takes the number and does nothing more
	if (au.edit_id != W_NONE)
	{
		const int w = widget_at(mx, my);
		const bool click_off = key == K_MOUSE1 && down && (w == W_NONE || w == W_VIEW2D);
		if (edit_key(key, down) || click_off)
			return true;
	}

	if (key == K_ESCAPE)
	{
		if (down)
			author_stop("");
		return true;
	}

	switch (key)
	{
	case K_MOUSE1:
		if (!down)
		{
			if (sl.dragging)
			{
				// the shape ends here: made at the next tick, between frames
				sl.drag_commit = true;
				queue_tick();
			}
			if (au.painting && au.line_active)
			{
				// the line ends here: painted at the next tick, between frames
				au.line_commit = true;
				queue_tick();
			}
			else if (au.painting)
			{
				au.painting = false;
				stroke_end();
			}
			const bool tint_drag = au.drag >= W_RED && au.drag <= W_BLUE;
			au.drag = W_NONE;
			if (tint_drag)
			{
				// the emissive tint let go of: its last change, and the drag as one undo step
				emit_retint();
				stroke_end();
			}
			return true;
		}
		if (!material_ok())
			return true;
		if (alt)
		{
			pick_material();
			return true;
		}
		{
			const int w = widget_at(mx, my);
			if ((w == W_VIEW2D || w == W_NONE) && au.select != SEL_OFF)
			{
				// a selection: Shift adds to it, Ctrl takes away, neither replaces it
				select_begin(w == W_VIEW2D, shift, ctrl);
			}
			else if (w == W_VIEW2D || w == W_NONE)
			{
				au.painting = true;
				au.paint_2d = (w == W_VIEW2D);
				au.have_last = false;
				au.create_tried = false;
				au.fill_done = false;
				au.spray_ms = 0;
				// a line or rectangle starts where the cursor first is on the map (paint_step)
				au.line_active = au.stroke != STROKE_FREE && au.tool != TOOL_FILL;
				au.line_have = au.line_commit = false;
				queue_tick();
			}
			else
				panel_press(w);
		}
		return true;

	case K_MOUSE2:
		au.right_pan = down && widget_at(mx, my) == W_VIEW2D;
		return true;

	case K_MWHEELUP:
	case K_MWHEELDOWN:
		if (!down)
			return true;
		if (au.show_2d && widget_at(mx, my) == W_VIEW2D)
			view2d_zoom(key == K_MWHEELUP);
		else if (field_step(field_under(mx, my), (key == K_MWHEELUP ? 1 : -1) * (shift ? 10 : 1)))
			;   // over a slider or its number: one at a time (Shift: ten)
		else if (shift)
			value_step(key == K_MWHEELUP ? 1 : -1);
		else if (ctrl)
			au.falloff = Q_clipf(au.falloff + (key == K_MWHEELUP ? 0.1f : -0.1f), 0.f, 1.f);
		else
			brush_size_step(key == K_MWHEELUP ? 1 : -1);
		return true;

	case 'f':
	case 'p':
		// made between frames (author_tick): it reads the world and the models
		if (down)
		{
			au.lock_request = (int)key;
			au.lock_toggle = shift;
			queue_tick();
		}
		return true;

	case 'u':
		if (down)
		{
			if (lk.on)
				lock_clear("unlocked");
			else
				author_status("no lock to undo - F locks a face, P a piece");
		}
		return true;

	case 't':
		if (down)
			set_tool((au.tool + (shift ? NUM_TOOLS - 1 : 1)) % NUM_TOOLS);
		return true;

	case 'g':
		if (down)
			set_tool(au.tool == TOOL_FILL ? au.prev_tool : TOOL_FILL);
		return true;

	case 'o':
		if (down)
		{
			au.show_base = !au.show_base;
			author_status(!au.show_base ? "map view: the map alone"
				: glow_view() ? "map view: the texture as bright as it glows, purple over what doesn't"
				: "map view: the base texture under the map");
		}
		return true;

	case '[':
	case ']':
		if (down)
			brush_size_step(key == ']' ? 1 : -1);
		return true;

	case '1': case '2': case '3': case '4': case '5': case '6':
		if (down)
			set_slot(key - '1');
		return true;

	case 'x':
		// the glass mask: glass <-> the wall round it
		if (down && au.slot == SLOT_GLASS)
			glass_paint_set(au.value[SLOT_GLASS] < 0.5f);
		return true;

	case 'l':
		if (down)
			set_stroke(au.stroke == STROKE_LINE ? STROKE_FREE : STROKE_LINE);
		return true;

	case 'r':
		if (down)
			set_stroke(au.stroke == STROKE_RECT ? STROKE_FREE : STROKE_RECT);
		return true;

	case 'b':
		// it cycled the brush shapes until 11-21
		if (down)
			author_status("the brush is round now - R drags a rectangle, corner to corner");
		return true;

	case 'm':
		// the selection's shape: rect, ellipse, lasso, then back to painting (Shift: the other way)
		if (down)
			set_select((au.select + (shift ? NUM_SEL - 1 : 1)) % NUM_SEL);
		return true;

	case 'a':
	case 'd':
	case 'i':
		/* Ctrl+A all, Ctrl+D none (Ctrl+Shift+D back), Ctrl+I invert - not while the
		   right button flies the camera, where Ctrl is slow and A/D strafe; plain
		   A and D always strafe (freecam.c). A key let go of always passes on, so a
		   strafe begun before Ctrl went down still stops. */
		if (!down || !ctrl || Key_IsDown(K_MOUSE2))
			return false;
		if (key == 'a')
			sel_all();
		else if (key == 'i')
			sel_invert();
		else if (shift)
			sel_reselect();
		else if (sl.num_steps)
			sel_clear("selection dropped - Ctrl+Shift+D brings it back");
		else
			author_status("nothing is selected");
		return true;

	case K_LEFTARROW:
	case K_RIGHTARROW:
		// over a slider or its number: one at a time (Shift: ten)
		if (field_under(mx, my) == W_NONE)
			return false;
		if (down)
			field_step(field_under(mx, my), (key == K_RIGHTARROW ? 1 : -1) * (shift ? 10 : 1));
		return true;

	case K_TAB:
		if (down)
		{
			au.show_2d = !au.show_2d;
			au.v2_fit_image = -2;
		}
		return true;

	case 'z':
		if (down && ctrl)
			undo_apply(shift);
		return true;

	case 'y':
		if (down && ctrl)
			undo_apply(true);
		return true;

	case 'h':
		// the camera back where it started - the panel's "home"
		if (down)
			camera_home();
		return true;

	case 'c':
		// the flashlight at the camera - the panel's "flashlight"
		if (down)
			light_toggle();
		return true;

	case 's':
		// Ctrl+S saves; plain S flies the camera back (freecam.c)
		if (ctrl)
		{
			if (down)
				author_save();
			return true;
		}
		return false;

	default:
		return false;
	}
}

/* ------------------------------------------------------------------------- */
/* THE COMMAND                                                               */
/* ------------------------------------------------------------------------- */

void MAT_AuthorCommand(pbr_material_t *crosshair)
{
	char word[64];
	Q_strlcpy(word, Cmd_Argc() >= 3 ? Cmd_Argv(2) : "", sizeof(word));

	if (!strcmp(word, "_tick"))
	{
		author_tick();
		return;
	}
	if (!strcmp(word, "off"))
	{
		if (au.active)
			author_stop("");
		else
			Com_Printf("mat author is not on\n");
		return;
	}
	if (!strcmp(word, "save"))
	{
		author_save();
		return;
	}

	// mat author stage <N|mat> (or stageN): paint a Quake III stage's own maps, or the material's
	const bool stage_word = !Q_stricmp(word, "stage");
	if (stage_word || (!Q_strncasecmp(word, "stage", 5) && Q_isdigit(word[5])))
	{
		const char *arg = stage_word ? (Cmd_Argc() >= 4 ? Cmd_Argv(3) : "") : word + 5;
		if (!au.active)
			author_start(crosshair, -1);
		if (!au.active)
			return;
		const int count = vkpt_q3_paint_stage_count(au.mat);
		if (!arg[0])
		{
			if (au.stage >= 0)
				Com_Printf("painting %s stage%d (of %d)\n", au.mat->name, au.stage, count);
			else
				Com_Printf("painting %s's own maps (%d Quake III stages)\n", au.mat->name, count);
			return;
		}
		if (!Q_stricmp(arg, "mat") || !Q_stricmp(arg, "material") || !strcmp(arg, "-1"))
		{
			set_stage(-1);
			return;
		}
		if (!Q_isdigit(arg[0]))
		{
			Com_Printf("usage: mat author stage <0..%d | mat>\n", max(count - 1, 0));
			return;
		}
		if (!count)
		{
			author_status("%s has no Quake III stages", au.mat->name);
			return;
		}
		set_stage(atoi(arg));
		return;
	}

	// mat author select rect|ellipse|lasso|off | all|none|invert|back: the selection
	if (!Q_stricmp(word, "select"))
	{
		if (!au.active)
			author_start(crosshair, -1);
		if (!au.active)
			return;
		const char *arg = Cmd_Argc() >= 4 ? Cmd_Argv(3) : "";
		for (int s = 0; s < NUM_SEL; s++)
		{
			if (!Q_stricmp(arg, sel_label[s]))
			{
				set_select(s);
				return;
			}
		}
		if (!Q_stricmp(arg, "all"))
			sel_all();
		else if (!Q_stricmp(arg, "none"))
		{
			if (sl.num_steps)
				sel_clear("selection dropped - 'mat author select back' brings it back");
			else
				author_status("nothing is selected");
		}
		else if (!Q_stricmp(arg, "invert"))
			sel_invert();
		else if (!Q_stricmp(arg, "back"))
			sel_reselect();
		else
			Com_Printf("usage: mat author select rect|ellipse|lasso|off | all|none|invert|back\n");
		return;
	}

	/* mat author range <dark> <bright>: the map being painted set over the selection
	   (and lock; else all of it) from the base texture's brightness - the darkest
	   to <dark>, the brightest to <bright>, in percent (emissive: 0..200) */
	if (!Q_stricmp(word, "range"))
	{
		if (!au.active)
			author_start(crosshair, -1);
		if (!au.active)
			return;
		if (Cmd_Argc() < 5)
		{
			Com_Printf("usage: mat author range <dark%%> <bright%%> - e.g. 'mat author range 30 50' sets the darkest "
				"of the base under the selection to 30%% and the brightest to 50%%\n");
			return;
		}
		const float top = au.slot == SLOT_EMISSIVE ? 200.f : 100.f;
		au.range[au.slot][0] = Q_clipf(floorf((float)atof(Cmd_Argv(3)) + 0.5f) / top, 0.f, 1.f);
		au.range[au.slot][1] = Q_clipf(floorf((float)atof(Cmd_Argv(4)) + 0.5f) / top, 0.f, 1.f);
		range_apply();
		return;
	}

	// mat author light [on|off]: the flashlight at the camera; no word toggles it
	if (!Q_stricmp(word, "light") || !Q_stricmp(word, "flashlight"))
	{
		const char *arg = Cmd_Argc() >= 4 ? Cmd_Argv(3) : "";
		light_cvars();
		if (!Q_stricmp(arg, "on") || !strcmp(arg, "1"))
			light_set(true);
		else if (!Q_stricmp(arg, "off") || !strcmp(arg, "0"))
			light_set(false);
		else if (!arg[0])
			light_toggle();
		else
			Com_Printf("usage: mat author light [on|off] - brightness: mat_author_light_scale (1 = the game's "
				"flashlight), cone: mat_author_light_cone (degrees across)\n");
		return;
	}

	// mat author free | line | rect [corner radius]: the stroke (a rectangle's radius in texels)
	for (int s = 0; s < NUM_STROKES; s++)
	{
		if (Q_stricmp(word, stroke_label[s]))
			continue;
		if (!au.active)
			author_start(crosshair, -1);
		if (!au.active)
			return;
		if (s == STROKE_RECT && Cmd_Argc() >= 4)
			au.rect_radius = Q_clip(atoi(Cmd_Argv(3)), 0, AUTHOR_MAX_RADIUS);
		set_stroke(s);
		return;
	}

	int slot = -1;
	for (int i = 0; i < NUM_SLOTS; i++)
		if (!Q_stricmp(word, slot_word[i]) || !Q_stricmp(word, slot_label[i]))
			slot = i;
	if (!Q_stricmp(word, "glassmask") || !Q_stricmp(word, "stained_mask"))
		slot = SLOT_GLASS;
	if (word[0] && slot < 0)
	{
		Com_Printf("usage: mat author [roughness|metallic|normals|emissive|height|glass] | stage <N|mat> | "
			"free | line | rect [corner radius] | select <shape|all|none|invert|back> | range <dark%%> <bright%%> | "
			"light [on|off] | save | off\n");
		return;
	}

	author_start(crosshair, slot);
}
