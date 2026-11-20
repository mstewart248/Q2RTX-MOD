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
colour), a round or square brush sized in TEXELS of the map, and the map is
painted straight onto the surfaces with a mouse cursor while the free camera
flies; or onto a flat view of the map, like 'mat print's. A change restarts the
accumulation, as a camera move does. His answers to the four design questions:
hold the RIGHT button to look (WASD/QE fly), the left one paints; the brush is in
texels and the texels it is about to change are outlined; the normal's direction
is SCREEN-RELATIVE (relative to the image in the map view); painting stays in
memory, with undo, until Save - which overwrites a loose PNG/TGA in place, keeping
a .bak, and writes anything else (a pak's, a JPG, a map that did not exist) as a
new PNG under materialDev/, pointing the material's .mat entry at it.

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
- Undo: one entry per stroke - the rectangle it changed, before and after.
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
  along the map's u (across the map view) and its WIDTH along v (down it), each
  in texels, set apart - slider, -/+ a texel, the wheel over either a texel at
  a time, or 'mat author rect <length> <width>'. R turns it (swaps the two).
  B goes round -> square -> rectangle.
- THE LINE (L, or the panel's "line"): the button goes down where the line
  starts and comes up where it ends; until then the brush is shown swept along
  it, and on release it paints that, once, as one stroke with every tool but
  fill. The swept brush is exact - sweep_row is the texels of a row it covers,
  primary_rays.rgen's author_swept the same test texel by texel.
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
*/

#include "vkpt.h"
#include "material.h"
#include "q3stages.h"
#include "../../client/client.h"
#include "../stb/stb_image_write.h"

#include <stdio.h>
#include <limits.h>

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

// the stage-capable slots come first: a Quake III stage paints SLOT_NORMAL and below
enum { SLOT_ROUGH, SLOT_METAL, SLOT_NORMAL, SLOT_EMISSIVE, SLOT_HEIGHT, NUM_SLOTS };

static const char *const slot_label[NUM_SLOTS] = { "rough", "metal", "normal", "emissive", "height" };
static const char *const slot_word[NUM_SLOTS] = { "roughness", "metallic", "normals", "emissive", "height" };
static const char *const slot_attribute[NUM_SLOTS] = { "texture_roughness", "texture_metallic", "texture_normals", "texture_emissive", "texture_height" };
static const char *const slot_suffix[NUM_SLOTS] = { "_rough", "_metal", "_n", "_light", "_height" };

// one grey value per texel: roughness, metallic, height
static inline bool slot_is_grey(int slot)
{
	return slot == SLOT_ROUGH || slot == SLOT_METAL || slot == SLOT_HEIGHT;
}

enum { TOOL_BRUSH, TOOL_SPRAY, TOOL_HATCH, TOOL_NOISE, TOOL_BLUR, TOOL_FILL, NUM_TOOLS };

static const char *const tool_label[NUM_TOOLS] = { "brush", "spray", "hatch", "noise", "blur", "fill" };

enum { SHAPE_ROUND, SHAPE_SQUARE, SHAPE_RECT, NUM_SHAPES };

static const char *const shape_label[NUM_SHAPES] = { "round", "square", "rect" };

#define AUTHOR_MAX_STAGES   8           // Q3P_MAX_STAGES: the panel's stage buttons

#define LOCK_FACE_DEG       12.f        // a flat face: triangles within this of the one picked
#define LOCK_WELD           (1.f / 64.f) // corners this close are one (model or world units)
#define LOCK_BLEED          2           // texels a lock reaches past a seam, into texels no triangle uses

#define AUTHOR_MAX_BRUSH     512
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
	W_SLOT0, W_SLOT1, W_SLOT2, W_SLOT3, W_SLOT4,
	W_VALUE,
	W_DIAL,
	W_INTENSITY,
	W_RED, W_GREEN, W_BLUE, W_STRENGTH,
	W_ROUND, W_SQUARE,
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
	W_RECT,
	W_RECT_W, W_RECT_H,             // the rectangle's length and width sliders
	W_RECT_W_DEC, W_RECT_W_INC,
	W_RECT_H_DEC, W_RECT_H_INC,
	W_STROKE_FREE, W_STROKE_LINE,
	W_STAGE_MAT,                    // the material's own maps
	W_STAGE0,                       // ... W_STAGE0 + N: stage N's
	W_LAST = W_STAGE0 + AUTHOR_MAX_STAGES
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

	float value[NUM_SLOTS];     // roughness, metallic, height: 0..1 (the others' entries unused)
	float normal_angle;         // the tilt's direction on screen, radians, y down (-pi/2 = up)
	float normal_intensity;     // 0..1 of AUTHOR_MAX_TILT_DEG
	float emissive_rgb[3];      // 0..1, as stored
	float emissive_strength;    // 0..1
	int brush_size;             // texels
	int brush_shape;            // SHAPE_*
	int rect_w, rect_h;         // SHAPE_RECT: texels along the map's u (its length) and v (its width)
	bool line_mode;             // L: a stroke is a straight line, from where the button goes down to where it comes up
	bool line_active;           // ... the button is down on one
	bool line_have;             // ... and its start is set
	bool line_commit;           // ... released: painted at the next tick
	author_hit_t line_hit;      // ... what was under its start (a normal's tilt is turned by its tangent frame)
	float line_tu0, line_tv0;   // ... its start, texels (unwrapped on a surface)
	float line_tu1, line_tv1;   // ... its end so far

	int stage;                  // -1: the material's own maps; else Quake III stage N's own (stage<N>_texture_*)
	float stage_tc[6];          // ... its tcMods as the last frame drew them (vkpt_q3_stage_paint_info)
	bool stage_clamp;           // ... clampMap: the map does not tile
	int tool;                   // TOOL_*
	int prev_tool;              // G goes back to it
	float falloff;              // 0 hard .. 1 soft from the centre out
	float tool_opt[NUM_TOOLS];  // spray density, hatch spacing (0..1), noise amount, blur strength, fill tolerance
	bool fill_by_colour;        // fill floods by the base texture's colour, not the map's values

	bool show_2d;
	bool show_base;             // the map view: the base texture under the map
	float layer_alpha;          // ... and the map over it this opaque
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

static author_undo_t undo_stack[AUTHOR_UNDO_MAX];
static int undo_count;          // entries held
static int undo_pos;            // entries applied: undo takes undo_pos - 1, redo undo_pos
static size_t undo_bytes;

// painted and not saved: the slot it was painted as, + 1; and the Quake III
// stage whose own map it is, + 1 (0 = the material's)
static uint8_t image_dirty[MAX_RIMAGES];
static uint8_t image_stage[MAX_RIMAGES];

static widget_t widgets[96];
static int num_widgets;

static cvar_t *mat_author_cursor_speed;

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

	qhandle_t image;            // the mask as a texture (global_ubo.author_lock_image)
	char image_name[32];
	int image_seq;
	bool upload_failed;
} lk;

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
	default:            return NULL;
	}
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
	return slot_image(au.mat, au.slot);
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

// the brush as it paints: the fill tool works from the one texel clicked
static int cur_shape(void)
{
	return au.tool == TOOL_FILL ? SHAPE_SQUARE : au.brush_shape;
}

// its texels across (along the map's u) and down (v)
static int cur_w(void)
{
	if (au.tool == TOOL_FILL)
		return 1;
	return au.brush_shape == SHAPE_RECT ? au.rect_w : au.brush_size;
}

static int cur_h(void)
{
	if (au.tool == TOOL_FILL)
		return 1;
	return au.brush_shape == SHAPE_RECT ? au.rect_h : au.brush_size;
}

// the larger of the two
static int cur_size(void)
{
	return max(cur_w(), cur_h());
}

// a line is being drawn and has its start: the brush is swept along it
static bool line_preview(void)
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
   square |2t+1-c2| <= size on both axes (a rectangle: its width across, its
   height down), round dx^2 + dy^2 <= size^2. */
static bool brush_row(int c2x, int c2y, int ty, int *x0, int *x1)
{
	const int s = cur_w();
	const int dy = 2 * ty + 1 - c2y;
	int k;
	if (cur_shape() != SHAPE_ROUND)
	{
		if (abs(dy) > cur_h())
			return false;
		k = s;
	}
	else
	{
		const int r2 = s * s - dy * dy;
		if (r2 < 0)
			return false;
		k = (int)sqrtf((float)r2);
		while ((k + 1) * (k + 1) <= r2)
			k++;
		while (k > 0 && k * k > r2)
			k--;
	}
	*x0 = ceil_div2(c2x - k - 1);
	*x1 = floor_div2(c2x + k - 1);
	return *x0 <= *x1;
}

static void brush_rows(int c2y, int *ty0, int *ty1)
{
	*ty0 = ceil_div2(c2y - cur_h() - 1);
	*ty1 = floor_div2(c2y + cur_h() - 1);
}

/* THE BRUSH SWEPT ALONG A LINE, from a centre (c2x, c2y) on by (dx, dy), all in
   doubled texels - what the line tool paints. q is a texel's offset from the
   first centre (2t + 1 - c2). A box (square, rectangle) covers q when some
   centre t (dx, dy), 0 <= t <= 1, is within its half-sizes of q on both axes;
   round when q is within the radius of the segment. primary_rays.rgen's
   author_swept is this arithmetic, for the outline. */
static bool sweep_inside(float qx, float qy, float dx, float dy)
{
	const float hx = (float)cur_w(), hy = (float)cur_h();
	if (cur_shape() == SHAPE_ROUND)
	{
		const float dd = dx * dx + dy * dy;
		const float t = dd > 0.f ? Q_clipf((qx * dx + qy * dy) / dd, 0.f, 1.f) : 0.f;
		const float ex = qx - dx * t, ey = qy - dy * t;
		return ex * ex + ey * ey <= hx * hx;
	}
	float t0 = 0.f, t1 = 1.f;
	if (dy == 0.f)
	{
		if (fabsf(qy) > hy)
			return false;
	}
	else
	{
		const float ta = (qy - hy) / dy, tb = (qy + hy) / dy;
		t0 = max(t0, min(ta, tb));
		t1 = min(t1, max(ta, tb));
		if (t0 > t1)
			return false;
	}
	const float lo = min(t0 * dx, t1 * dx) - hx, hi = max(t0 * dx, t1 * dx) + hx;
	return qx >= lo && qx <= hi;
}

// the rows the swept brush covers
static void sweep_rows(int c2y, int dy, int *ty0, int *ty1)
{
	*ty0 = ceil_div2(c2y + min(0, dy) - cur_h() - 1);
	*ty1 = floor_div2(c2y + max(0, dy) + cur_h() - 1);
}

/* Row (ty)'s texels the swept brush covers: [x0, x1], or false. For a box it
   is worked out (the centres within reach of the row, then their reach
   across); round it is that, narrowed from both ends to the texels
   sweep_inside takes - the shape is convex, so what is left is one run. */
static bool sweep_row(int c2x, int c2y, int dx, int dy, int ty, int *x0, int *x1)
{
	const float qy = (float)(2 * ty + 1 - c2y);
	const float hx = (float)cur_w(), hy = (float)cur_h();
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
	// round: narrowed to the capsule; a box: to its own test, to the last float - the shader's
	while (*x0 <= *x1 && !sweep_inside((float)(2 * *x0 + 1 - c2x), qy, (float)dx, (float)dy))
		(*x0)++;
	while (*x1 >= *x0 && !sweep_inside((float)(2 * *x1 + 1 - c2x), qy, (float)dx, (float)dy))
		(*x1)--;
	return *x0 <= *x1;
}

/* How far into the swept brush q is, 0 on the line to 1 at the brush's edge
   plus a texel - as dab's r, for the falloff. Round: the distance to the
   segment. A box: the least over t of max(|qx - t dx| / (hx + 1), |qy - t dy| /
   (hy + 1)), which is convex and piecewise linear in t, so it is least at an
   end or where a term turns or the two cross. */
static float sweep_r(float qx, float qy, float dx, float dy)
{
	const float ax = (float)cur_w() + 1.f, ay = (float)cur_h() + 1.f;
	if (cur_shape() == SHAPE_ROUND)
	{
		const float dd = dx * dx + dy * dy;
		const float t = dd > 0.f ? Q_clipf((qx * dx + qy * dy) / dd, 0.f, 1.f) : 0.f;
		const float ex = qx - dx * t, ey = qy - dy * t;
		return sqrtf(ex * ex + ey * ey) / ax;
	}
	float cand[8];
	int n = 0;
	cand[n++] = 0.f;
	cand[n++] = 1.f;
	if (dx != 0.f)
		cand[n++] = qx / dx;
	if (dy != 0.f)
		cand[n++] = qy / dy;
	// (qx - t dx) / ax = +-(qy - t dy) / ay
	const float p = dx / ax - dy / ay, m = dx / ax + dy / ay;
	if (p != 0.f)
		cand[n++] = (qx / ax - qy / ay) / p;
	if (m != 0.f)
		cand[n++] = (qx / ax + qy / ay) / m;
	float best = 1e30f;
	for (int i = 0; i < n; i++)
	{
		const float t = Q_clipf(cand[i], 0.f, 1.f);
		const float r = max(fabsf(qx - t * dx) / ax, fabsf(qy - t * dy) / ay);
		best = min(best, r);
	}
	return best;
}

/* ------------------------------------------------------------------------- */
/* THE LOCK, AS PAINTING SEES IT                                             */
/* ------------------------------------------------------------------------- */

// (x, y) of a W x H map may be painted: no lock, or inside it
static bool lock_allows(int x, int y, int W, int H)
{
	if (!lk.on)
		return true;
	// not rasterised for this map yet (lock_sync, each tick): paint nothing
	if (!lk.mask || lk.mask_w != W || lk.mask_h != H)
		return false;
	return lk.mask[(size_t)y * W + x] != 0;
}

static bool lock_image_ok(void)
{
	if (!lk.image || lk.image >= MAX_RIMAGES)
		return false;
	const image_t *img = r_images + lk.image;
	return img->registration_sequence && img->pix_data && !strcmp(img->name, lk.image_name)
		&& img->upload_width == lk.mask_w && img->upload_height == lk.mask_h;
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
		p->v[0] = p->v[1] = p->v[2] = Q_clipf(au.value[au.slot], 0.f, 1.f);
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
	case SLOT_EMISSIVE:
		for (int c = 0; c < 3; c++)
			p->v[c] = Q_clipf(au.emissive_rgb[c] * au.emissive_strength, 0.f, 1.f);
		break;
	}
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
	default:
		for (int c = 0; c < 3; c++)
			v[c] = (float)px[c] / 255.f;
		break;
	}
}

// texel (x, y) of (img) becomes (v): true if it changed
static bool texel_put(image_t *img, int x, int y, int slot, const float v[3])
{
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
		byte rgb[3];
		switch (slot)
		{
		case SLOT_ROUGH:
		case SLOT_METAL:
		case SLOT_HEIGHT:
			rgb[0] = rgb[1] = rgb[2] = to_byte(img->is_srgb ? linear_to_srgb(v[0]) : v[0]);
			break;
		case SLOT_NORMAL:
			rgb[0] = to_byte(v[0] * 0.5f + 0.5f);
			rgb[1] = to_byte(v[1] * 0.5f + 0.5f);
			rgb[2] = to_byte(v[2]);
			break;
		default:
			for (int c = 0; c < 3; c++)
				rgb[c] = to_byte(v[c]);
			break;
		}
		byte *px = img->pix_data + at * 4;
		if (px[0] == rgb[0] && px[1] == rgb[1] && px[2] == rgb[2])
			return false;
		// the alpha is kept: a legacy map hides metallic or roughness in it
		px[0] = rgb[0];
		px[1] = rgb[1];
		px[2] = rgb[2];
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
	const int r = Q_clip(min(cur_w(), cur_h()) / 6, 1, 6);
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
	if (!lock_allows(x, y, img->upload_width, img->upload_height))
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
	const bool round_brush = cur_shape() == SHAPE_ROUND;
	const float rx = 1.f / (float)(cur_w() + 1), ry = 1.f / (float)(cur_h() + 1);
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
			const float r = round_brush ? sqrtf((float)(dx * dx + dy * dy)) * rx
				: max((float)abs(dx) * rx, (float)abs(dy) * ry);
			dab_texel(c, x, y, r);
		}
	}
	dab_done(c);
}

// the brush's centre, doubled (centre2), for a point (tu, tv) in texels
static int centre2_x(float tu)
{
	return centre2(tu, cur_w());
}

static int centre2_y(float tv)
{
	return centre2(tv, cur_h());
}

static void dab_line(dab_ctx_t *c, float tu0, float tv0, float tu1, float tv1)
{
	const float dx = tu1 - tu0, dy = tv1 - tv0;
	const float dist = sqrtf(dx * dx + dy * dy);
	const float step = max(1.f, (float)min(cur_w(), cur_h()) * 0.25f);
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

// how far apart two texels' values are, 0..1, for the fill's tolerance
static float value_distance(int slot, const float a[3], const float b[3])
{
	float d = 0.f;
	const int n = slot_is_grey(slot) ? 1 : 3;
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

/* The fill tool, clicked on texel (sx, sy): the whole lock if one is on; else
   the texels reached from it through neighbours (4-way, the map's edges stop
   it) whose value - or base texture colour - is within the tolerance of its. */
static void fill_at(dab_ctx_t *c, int sx, int sy)
{
	image_t *img = c->img;
	const int W = img->upload_width, H = img->upload_height;
	int filled = 0;

	if (lk.on)
	{
		if (!lk.mask || lk.mask_w != W || lk.mask_h != H)
		{
			author_status("the lock isn't ready for this map yet - click again");
			return;
		}
		for (int y = 0; y < H; y++)
		{
			for (int x = 0; x < W; x++)
			{
				if (lk.mask[(size_t)y * W + x])
				{
					paint_texel(c, x, y, 1.f);
					filled++;
				}
			}
		}
		dab_done(c);
		author_status("filled the lock: %d texels", filled);
		return;
	}

	if (sx < 0 || sy < 0 || sx >= W || sy >= H)
		return;

	bool by_colour = au.fill_by_colour;
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
	if (undo_pos > 0)
		undo_pos--;
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
		// a new stroke drops what could have been redone
		while (undo_count > undo_pos)
			undo_free(&undo_stack[--undo_count]);

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

		while (undo_count && (undo_count == AUTHOR_UNDO_MAX || undo_bytes + e.bytes > AUTHOR_UNDO_BYTES))
			undo_drop_oldest();
		undo_stack[undo_count++] = e;
		undo_bytes += e.bytes;
		undo_pos = undo_count;

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

static void undo_apply(bool redo)
{
	stroke_end();
	if (redo ? undo_pos >= undo_count : undo_pos <= 0)
	{
		author_status(redo ? "nothing to redo" : "nothing to undo");
		return;
	}

	author_undo_t *e = &undo_stack[redo ? undo_pos : undo_pos - 1];
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
	undo_pos += redo ? 1 : -1;

	if (!image_dirty[e->image])
	{
		image_dirty[e->image] = (uint8_t)(e->slot + 1);
		image_stage[e->image] = (uint8_t)(e->stage + 1);
	}
	if (e->slot == SLOT_EMISSIVE)
		after_emissive_change(img);
	IMG_PixelsEdited(img);
	vkpt_reset_accumulation();
	author_status("%s (%d of %d)", redo ? "redone" : "undone", undo_pos, undo_count);
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

static bool game_path_to_os(const char *game_path, char *out, size_t size)
{
	if (Q_snprintf(out, size, "%s/%s", fs_gamedir, game_path) >= size)
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

static image_t *create_map(void)
{
	pbr_material_t *mat = au.mat;
	const image_t *base = author_base();
	int W, H;
	map_size(&W, &H);

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
			default:
				p[0] = p[1] = p[2] = 0;
				p[3] = 255;
				break;
			}
		}
	}

	char path[MAX_OSPATH], ospath[MAX_OSPATH];
	const char *from = base ? base->name : mat->name;
	dev_png_path(from, slot_suffix[au.slot], path, sizeof(path));
	// never over an older file of that name: it may be work the material forgot
	for (int n = 2; n < 100; n++)
	{
		char found[MAX_OSPATH];
		if (FS_RealPath(path, 0, found, sizeof(found)) == FS_REALPATH_NONE)
			break;
		char suffix[32];
		Q_snprintf(suffix, sizeof(suffix), "%s_%d", slot_suffix[au.slot], n);
		dev_png_path(from, suffix, path, sizeof(path));
	}

	membuf_t m = { 0 };
	bool backed_up;
	const bool ok = encode_rgba(rgba, W, H, false, &m) && game_path_to_os(path, ospath, sizeof(ospath))
		&& write_with_backup(ospath, &m, &backed_up);
	Z_Free(m.data);
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
	else
		author_status("%s had no %s map: made %s (%dx%d) - Save writes it into the .mat", mat->name,
			slot_word[au.slot], path, img->upload_width, img->upload_height);
	return img;
}

/* ------------------------------------------------------------------------- */
/* SAVE                                                                      */
/* ------------------------------------------------------------------------- */

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
		char new_path[MAX_OSPATH];
		dev_png_path(game_path, "", new_path, sizeof(new_path));
		const bool ok = encode_image(img, false, &m) && game_path_to_os(new_path, ospath, sizeof(ospath))
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

		const image_t *loaded = NULL;
		const int stage = image_stage[i] - 1;
		for (int k = 0; k < MAX_PBR_MATERIALS; k++)
		{
			pbr_material_t *m2 = MAT_ForIndex(k);
			if (!m2 || !m2->registration_sequence)
				continue;
			if (stage >= 0)
			{
				// a Quake III stage's own map: every material whose stage N draws it
				const char *file;
				if (slot > SLOT_NORMAL || vkpt_q3_stage_image(m2, stage, slot + 1, &file) != img)
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

// one id per welded corner: within LOCK_WELD of each other, in the same group
static int *weld_corners(const soup_t *s)
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
		const float *p = s->pos + c * 3;
		int *qc = q + c * 3;
		for (int k = 0; k < 3; k++)
			qc[k] = (int)floorf(p[k] / LOCK_WELD + 0.5f);
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
   few degrees, not wherever its facets add up; a piece across any shared corner. */
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
			keys[t * 3 + e].key = kind == 'f' ? edge_key(a, b) : (uint64_t)(uint32_t)a;
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
			const uint64_t key = kind == 'f' ? edge_key(a, b) : (uint64_t)(uint32_t)a;
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

static void lock_rasterize(int W, int H)
{
	const size_t n = (size_t)W * H;
	byte *mask = Z_Mallocz(n);
	stage_info(NULL, NULL);     // the stage's tcMods as they are now
	for (int i = 0; i < lk.num_tris; i++)
		raster_map_tri(lk.tris[i].uv, W, H, mask);

	if (lk.num_cover)
	{
		byte *cover = Z_Mallocz(n);
		for (int i = 0; i < lk.num_cover; i++)
			raster_map_tri(lk.cover[i].uv, W, H, cover);
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
	Z_Free(lk.mask);
	lk.mask = mask;
	lk.mask_w = W;
	lk.mask_h = H;
	lk.texels = texels;
}

/* The mask as a texture, for the shader (dark outside the lock, a cyan edge) and
   the map view: inside = red 1 and clear, outside = black at 150 alpha. Remade in
   place while its size holds; a new size is a new image. */
static void lock_upload(void)
{
	const int W = lk.mask_w, H = lk.mask_h;
	if (!lk.mask || W <= 0 || H <= 0)
		return;
	image_t *img = lock_image_ok() ? r_images + lk.image : NULL;
	byte *px = img ? img->pix_data : IMG_AllocPixels((size_t)W * H * 4);
	for (size_t i = 0; i < (size_t)W * H; i++)
	{
		byte *p = px + i * 4;
		const bool in = lk.mask[i] != 0;
		p[0] = in ? 255 : 0;
		p[1] = p[2] = 0;
		p[3] = in ? 0 : 150;
	}
	if (img)
	{
		IMG_PixelsEdited(img);
		return;
	}

	if (lk.image > 0 && lk.image < MAX_RIMAGES && r_images[lk.image].registration_sequence
		&& !strcmp(r_images[lk.image].name, lk.image_name))
		R_UnregisterImage(lk.image);
	Q_snprintf(lk.image_name, sizeof(lk.image_name), "**mat_author_lock_%d**", ++lk.image_seq);
	lk.image = R_RegisterRawImage(lk.image_name, W, H, px, IT_SPRITE, IF_NEAREST | IF_NO_MIPMAPS);
	if (!lk.image)
	{
		IMG_FreePixels(px);
		lk.image_name[0] = 0;
		lk.upload_failed = true;
		author_status("couldn't make the lock's texture: it won't show, but it still holds");
	}
}

// the lock, for the map being painted now: each tick, before any painting
static void lock_sync(void)
{
	if (!lk.on)
		return;
	int W, H;
	map_size(&W, &H);
	if (lk.dirty || !lk.mask || lk.mask_w != W || lk.mask_h != H)
	{
		lock_rasterize(W, H);
		lk.dirty = false;
		lk.upload_failed = false;
		lock_upload();
	}
	else if (!lock_image_ok() && !lk.upload_failed)
		lock_upload();   // its texture went (a vid_restart)
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
	lock_sync();
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
/* START, STOP                                                               */
/* ------------------------------------------------------------------------- */

static void author_stop(const char *why)
{
	stroke_end();
	upload_now();
	au.active = false;
	au.painting = false;
	au.drag = W_NONE;
	au.right_pan = false;
	au.hit.valid = false;

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

	if (!au.defaults_set)
	{
		au.defaults_set = true;
		au.value[SLOT_ROUGH] = 0.5f;
		au.value[SLOT_METAL] = 1.f;
		au.value[SLOT_HEIGHT] = 0.5f;
		au.normal_angle = -(float)M_PI / 2.f;
		au.normal_intensity = 0.5f;
		au.emissive_rgb[0] = au.emissive_rgb[1] = au.emissive_rgb[2] = 1.f;
		au.emissive_strength = 1.f;
		au.brush_size = 8;
		au.brush_shape = SHAPE_ROUND;
		au.rect_w = 32;
		au.rect_h = 4;
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
		au.stroke_image = -1;
		au.upload_image = -1;
		au.rng = Sys_Milliseconds() | 1u;
	}

	if (au.mat != mat)
	{
		stroke_end();
		lock_clear(NULL);
		au.stage = -1;
	}
	au.mat = mat;
	Q_strlcpy(au.mat_name, mat->name, sizeof(au.mat_name));
	if (slot >= 0 && !(au.stage >= 0 && slot > SLOT_NORMAL))
		au.slot = slot;
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
			"  Ctrl + right button zooms); LEFT paints; Alt + left picks the material under the brush;\n"
			"  wheel or [ ] = brush size (on the map view the wheel zooms, right-drag pans);\n"
			"  Shift + wheel = value; Ctrl + wheel = falloff; 1-5 = map; B = round/square/rectangle;\n"
			"  R = turn the rectangle; L = line (press where it starts, let go where it ends);\n"
			"  T = next tool (brush spray hatch noise blur fill), G = fill; Tab = map view, O = base under it;\n"
			"  F = lock to the face under the brush, P = to the piece, Shift adds/removes, U unlocks;\n"
			"  a Quake III shader drawn in stages: the panel's 'maps' row paints one stage's own maps;\n"
			"  Ctrl+Z / Ctrl+Y = undo / redo; Ctrl+S = save; Esc = leave\n");
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

static void set_slot(int slot)
{
	if (slot == au.slot || slot < 0 || slot >= NUM_SLOTS)
		return;
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

static void set_shape(int shape)
{
	if (shape < 0 || shape >= NUM_SHAPES)
		return;
	au.brush_shape = shape;
	if (shape == SHAPE_RECT)
		author_status("rectangle %d x %d texels: R turns it", au.rect_w, au.rect_h);
}

static void set_line_mode(bool on)
{
	au.line_mode = on;
	au.line_active = au.line_have = au.line_commit = false;
	author_status(on ? "line: press where it starts, let go where it ends" : "free strokes");
}

static void rect_turn(void)
{
	const int t = au.rect_w;
	au.rect_w = au.rect_h;
	au.rect_h = t;
	author_status("rectangle %d x %d texels", au.rect_w, au.rect_h);
}

static void set_tool(int tool)
{
	if (tool < 0 || tool >= NUM_TOOLS || tool == au.tool)
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
		au.stage = -1;
	}
	au.mat = mat;
	Q_strlcpy(au.mat_name, mat->name, sizeof(au.mat_name));
	au.v2_fit_image = -2;
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

static float size_to_t(int size)
{
	return sqrtf((float)(size - 1) / (float)(AUTHOR_MAX_BRUSH - 1));
}

static int t_to_size(float t)
{
	t = Q_clipf(t, 0.f, 1.f);
	return Q_clip((int)(1.f + t * t * (AUTHOR_MAX_BRUSH - 1) + 0.5f), 1, AUTHOR_MAX_BRUSH);
}

static int size_step(int s, int dir)
{
	if (dir > 0)
		s = s < 10 ? s + 1 : (int)(s * 1.15f + 0.5f);
	else
		s = s <= 10 ? s - 1 : (int)(s / 1.15f + 0.5f);
	return Q_clip(s, 1, AUTHOR_MAX_BRUSH);
}

// the wheel: the brush's size - a rectangle's two sides together
static void brush_size_step(int dir)
{
	if (au.brush_shape == SHAPE_RECT)
	{
		// stop both at once at either end, so the shape keeps its proportions
		const int w = size_step(au.rect_w, dir), h = size_step(au.rect_h, dir);
		if (w != au.rect_w && h != au.rect_h)
		{
			au.rect_w = w;
			au.rect_h = h;
		}
		return;
	}
	au.brush_size = size_step(au.brush_size, dir);
}

// one of the size sliders a texel (or ten) at a time: the wheel over it, or its - / + buttons
static bool size_slider_step(int id, int by)
{
	int *v;
	switch (id)
	{
	case W_SIZE:   v = &au.brush_size; break;
	case W_RECT_W: v = &au.rect_w; break;
	case W_RECT_H: v = &au.rect_h; break;
	default:       return false;
	}
	*v = Q_clip(*v + by, 1, AUTHOR_MAX_BRUSH);
	return true;
}

// the main value of the map being painted, by Shift + wheel
static void value_step(int dir)
{
	const float d = dir * 0.05f;
	switch (au.slot)
	{
	case SLOT_ROUGH:
	case SLOT_METAL:
	case SLOT_HEIGHT:   au.value[au.slot] = Q_clipf(au.value[au.slot] + d, 0.f, 1.f); break;
	case SLOT_NORMAL:   au.normal_intensity = Q_clipf(au.normal_intensity + d, 0.f, 1.f); break;
	case SLOT_EMISSIVE: au.emissive_strength = Q_clipf(au.emissive_strength + d, 0.f, 1.f); break;
	}
}

// the left button holding control (id) at the cursor
static void drag_update(int id)
{
	const widget_t *w = widget_find(id);
	if (!w)
		return;
	float mx, my;
	cursor_virtual(&mx, &my);
	const float t = Q_clipf((mx - w->x) / max(w->w, 1.f), 0.f, 1.f);
	switch (id)
	{
	case W_VALUE:     if (slot_is_grey(au.slot)) au.value[au.slot] = t; break;
	case W_INTENSITY: au.normal_intensity = t; break;
	case W_RED:       au.emissive_rgb[0] = t; break;
	case W_GREEN:     au.emissive_rgb[1] = t; break;
	case W_BLUE:      au.emissive_rgb[2] = t; break;
	case W_STRENGTH:  au.emissive_strength = t; break;
	case W_SIZE:      au.brush_size = t_to_size(t); break;
	case W_RECT_W:    au.rect_w = t_to_size(t); break;
	case W_RECT_H:    au.rect_h = t_to_size(t); break;
	case W_FALLOFF:   au.falloff = t; break;
	case W_TOOLOPT:   au.tool_opt[au.tool] = t; break;
	case W_LAYER:     au.layer_alpha = t; break;
	case W_DIAL:
	{
		const float dx = mx - (w->x + w->w * 0.5f), dy = my - (w->y + w->h * 0.5f);
		if (dx * dx + dy * dy < 1.f)
			break;
		float a = atan2f(dy, dx);
		if (Key_IsDown(K_SHIFT))
		{
			const float step = (float)M_PI / 4.f;
			a = floorf(a / step + 0.5f) * step;
		}
		au.normal_angle = a;
		break;
	}
	default:
		break;
	}
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
			*fix = "emissive_factor 1";
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

static void panel_press(int id)
{
	switch (id)
	{
	case W_SLOT0: case W_SLOT1: case W_SLOT2: case W_SLOT3: case W_SLOT4:
		set_slot(id - W_SLOT0);
		break;
	case W_VALUE: case W_DIAL: case W_INTENSITY: case W_RED: case W_GREEN: case W_BLUE: case W_STRENGTH: case W_SIZE:
	case W_FALLOFF: case W_TOOLOPT: case W_LAYER: case W_RECT_W: case W_RECT_H:
		au.drag = id;
		drag_update(id);
		break;
	case W_RECT_W_DEC: size_slider_step(W_RECT_W, Key_IsDown(K_SHIFT) ? -10 : -1); break;
	case W_RECT_W_INC: size_slider_step(W_RECT_W, Key_IsDown(K_SHIFT) ? 10 : 1); break;
	case W_RECT_H_DEC: size_slider_step(W_RECT_H, Key_IsDown(K_SHIFT) ? -10 : -1); break;
	case W_RECT_H_INC: size_slider_step(W_RECT_H, Key_IsDown(K_SHIFT) ? 10 : 1); break;
	case W_RECT:        set_shape(SHAPE_RECT); break;
	case W_STROKE_FREE: if (au.line_mode) set_line_mode(false); break;
	case W_STROKE_LINE: if (!au.line_mode) set_line_mode(true); break;
	case W_STAGE_MAT:   set_stage(-1); break;
	case W_TOOL0: case W_TOOL1: case W_TOOL2: case W_TOOL3: case W_TOOL4: case W_TOOL5:
		set_tool(id - W_TOOL0);
		break;
	case W_FILL_MAP:    au.fill_by_colour = false; break;
	case W_FILL_COLOUR: au.fill_by_colour = true; break;
	case W_UNLOCK:      lock_clear("unlocked"); break;
	case W_SHOWBASE:    au.show_base = !au.show_base; break;
	case W_ROUND:  set_shape(SHAPE_ROUND); break;
	case W_SQUARE: set_shape(SHAPE_SQUARE); break;
	case W_SHOW2D: au.show_2d = !au.show_2d; au.v2_fit_image = -2; break;
	case W_UNDO:   undo_apply(false); break;
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
			if (!lock_allows(x, y, W, H))
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
		ui_text(x, y, 0, va("no %s map: painting makes one (%dx%d)", slot_word[au.slot], W, H), max_chars, font);
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

	// a stage has no emissive or height map: their buttons are left out. Each
	// button its label's width, the room left over shared out evenly.
	const int slots = au.stage >= 0 ? SLOT_NORMAL + 1 : NUM_SLOTS;
	int label_chars = 0;
	for (int i = 0; i < slots; i++)
		label_chars += (int)strlen(slot_label[i]);
	const float spare = max(0.f, (iw - (slots - 1) * 4.f - label_chars * CHAR_WIDTH) / (float)slots);
	float slot_x = x;
	for (int i = 0; i < slots; i++)
	{
		const float bw = (float)strlen(slot_label[i]) * CHAR_WIDTH + spare;
		ui_button(W_SLOT0 + i, slot_x, y, bw, slot_label[i], au.slot == i, font);
		slot_x += bw + 4.f;
	}
	y += line + 6.f;

	switch (au.slot)
	{
	case SLOT_ROUGH:
	case SLOT_METAL:
	case SLOT_HEIGHT:
		ui_text(x, y, 0, va("%s %d%%", slot_word[au.slot], (int)(au.value[au.slot] * 100.f + 0.5f)), max_chars - under_n - 1, font);
		ui_text(x + iw - under_n * CHAR_WIDTH, y, UI_ALTCOLOR, under_s, under_n, font);
		y += line;
		ui_slider(W_VALUE, x + 3.f, y, iw - 6.f, au.value[au.slot]);
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
		ui_text(tx, y, 0, va("leans %s (%d deg)", direction_name(au.normal_angle),
			((int)floorf(au.normal_angle * 180.f / (float)M_PI + 0.5f) + 360) % 360), tchars, font);
		ui_text(tx, y + line, 0, va("intensity %d%% (tilt %d deg)", (int)(au.normal_intensity * 100.f + 0.5f),
			(int)(au.normal_intensity * AUTHOR_MAX_TILT_DEG + 0.5f)), tchars, font);
		ui_slider(W_INTENSITY, tx + 3.f, y + 2 * line, x + iw - tx - 6.f, au.normal_intensity);
		y += 2 * r + 6.f;
		ui_text(x, y, 0, "screen-relative (map view: image)", max_chars, font);
		y += line;
		ui_text(x, y, UI_ALTCOLOR, under_s, max_chars, font);
		y += line + 2.f;
		break;
	}
	case SLOT_EMISSIVE:
	{
		static const char *const names[3] = { "red", "green", "blue" };
		const float sw = 30.f;
		ui_fill(x, y, sw, 4 * line - 2, MakeColor(
			to_byte(au.emissive_rgb[0] * au.emissive_strength), to_byte(au.emissive_rgb[1] * au.emissive_strength),
			to_byte(au.emissive_rgb[2] * au.emissive_strength), 255));
		const float sx = x + sw + 8.f, sl = 10.f * CHAR_WIDTH;
		for (int c = 0; c < 3; c++)
		{
			ui_text(sx, y + c * line + 1.f, 0, va("%s %d%%", names[c], (int)(au.emissive_rgb[c] * 100.f + 0.5f)), 10, font);
			ui_slider(W_RED + c, sx + sl + 4.f, y + c * line, x + iw - sx - sl - 8.f, au.emissive_rgb[c]);
		}
		ui_text(sx, y + 3 * line + 1.f, 0, va("glow %d%%", (int)(au.emissive_strength * 100.f + 0.5f)), 10, font);
		ui_slider(W_STRENGTH, sx + sl + 4.f, y + 3 * line, x + iw - sx - sl - 8.f, au.emissive_strength);
		y += 4 * line + 6.f;
		ui_text(x, y, 0, "black paints the glow away", max_chars, font);
		y += line;
		ui_text(x, y, UI_ALTCOLOR, under_s, max_chars, font);
		y += line;
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
		ui_button(W_FILL_MAP, x + iw - 2 * fbw - 4.f, y, fbw, "value", !au.fill_by_colour, font);
		ui_button(W_FILL_COLOUR, x + iw - fbw, y, fbw, "colour", au.fill_by_colour, font);
		y += line + 1.f;
		ui_slider(W_TOOLOPT, x + 3.f, y, iw - 6.f, au.tool_opt[TOOL_FILL]);
		y += line + 4.f;
		ui_text(x, y, UI_ALTCOLOR, lk.on ? "click: fills the whole lock" : "click: floods the texels alike", max_chars, font);
		y += line + 2.f;
	}
	else
	{
		ui_text(x, y, 0, au.falloff > 0.f ? va("falloff %d%% (soft edge)", (int)(au.falloff * 100.f + 0.5f))
			: "falloff 0% (hard edge)", max_chars, font);
		y += line;
		ui_slider(W_FALLOFF, x + 3.f, y, iw - 6.f, au.falloff);
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
			ui_slider(W_TOOLOPT, x + 3.f, y, iw - 6.f, au.tool_opt[au.tool]);
			y += line + 4.f;
		}

		// the shape: round, square or a rectangle of its own length and width
		const float sbw = 46.f;
		const float shx = x + iw - 3 * sbw - 2 * 3.f;
		if (au.brush_shape == SHAPE_RECT)
			ui_text(x, y, 0, va("rect %dx%d", au.rect_w, au.rect_h), (int)((shx - x) / CHAR_WIDTH), font);
		else
			ui_text(x, y, 0, va("brush %d texel%s", au.brush_size, au.brush_size == 1 ? "" : "s"),
				(int)((shx - x) / CHAR_WIDTH), font);
		ui_button(W_ROUND, shx, y - 1.f, sbw, shape_label[SHAPE_ROUND], au.brush_shape == SHAPE_ROUND, font);
		ui_button(W_SQUARE, shx + sbw + 3.f, y - 1.f, sbw, shape_label[SHAPE_SQUARE], au.brush_shape == SHAPE_SQUARE, font);
		ui_button(W_RECT, shx + 2 * (sbw + 3.f), y - 1.f, sbw, shape_label[SHAPE_RECT], au.brush_shape == SHAPE_RECT, font);
		y += line + 1.f;
		if (au.brush_shape == SHAPE_RECT)
		{
			// length across the map (u), width down it (v): a label, - and +, the slider
			static const char *const side[2] = { "length", "width" };
			const int ids[2][3] = { { W_RECT_W, W_RECT_W_DEC, W_RECT_W_INC }, { W_RECT_H, W_RECT_H_DEC, W_RECT_H_INC } };
			const int vals[2] = { au.rect_w, au.rect_h };
			const float lw = 10 * CHAR_WIDTH, pm = 14.f;
			for (int k = 0; k < 2; k++)
			{
				ui_text(x, y + 1.f, 0, va("%s %d", side[k], vals[k]), 10, font);
				ui_button(ids[k][1], x + lw, y, pm, "-", false, font);
				ui_button(ids[k][2], x + lw + pm + 2.f, y, pm, "+", false, font);
				const float slx = x + lw + 2 * pm + 10.f;
				ui_slider(ids[k][0], slx, y, x + iw - slx - 3.f, size_to_t(vals[k]));
				y += line + 1.f;
			}
			ui_text(x, y, UI_ALTCOLOR, "length across, width down; R turns", max_chars, font);
			y += line + 3.f;
		}
		else
		{
			ui_slider(W_SIZE, x + 3.f, y, iw - 6.f, size_to_t(au.brush_size));
			y += line + 4.f;
		}

		// the stroke: free, or a straight line from where the button goes down to where it comes up
		const float lbw = 40.f;
		ui_text(x, y + 2.f, 0, au.line_mode ? (line_preview() ? "stroke: let go to paint" : "stroke: line")
			: "stroke: free", (int)((iw - 2 * lbw - 4.f) / CHAR_WIDTH), font);
		ui_button(W_STROKE_FREE, x + iw - 2 * lbw - 3.f, y, lbw, "free", !au.line_mode, font);
		ui_button(W_STROKE_LINE, x + iw - lbw, y, lbw, "line", au.line_mode, font);
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

	// the map view
	const float vbw = 14 * CHAR_WIDTH + 8.f;
	ui_button(W_SHOW2D, x, y, vbw, au.show_2d ? "[x] map view" : "[ ] map view", au.show_2d, font);
	ui_button(W_SHOWBASE, x + vbw + 4.f, y, vbw, au.show_base ? "[x] base under" : "[ ] base under", au.show_base, font);
	y += line + 4.f;
	if (au.show_2d && au.show_base)
	{
		ui_text(x, y, 0, va("map over the base: %d%%", (int)(au.layer_alpha * 100.f + 0.5f)), max_chars, font);
		y += line;
		ui_slider(W_LAYER, x + 3.f, y, iw - 6.f, au.layer_alpha);
		y += line + 4.f;
	}

	const float b4 = (iw - 3 * 4.f) / 4.f;
	ui_button(W_UNDO, x, y, b4, "undo", false, font);
	ui_button(W_REDO, x + (b4 + 4.f), y, b4, "redo", false, font);
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
	ui_text(x, y, UI_ALTCOLOR, "RMB look  WASD/QE fly  LMB paint", max_chars, font);
	y += line - 2.f;
	ui_text(x, y, UI_ALTCOLOR, "Alt+click pick  wheel size  B shape", max_chars, font);
	y += line - 2.f;
	ui_text(x, y, UI_ALTCOLOR, "Shift+wheel value  Ctrl+wheel soft", max_chars, font);
	y += line - 2.f;
	ui_text(x, y, UI_ALTCOLOR, "1-5 map  T tool  G fill  Tab 2D  O", max_chars, font);
	y += line - 2.f;
	ui_text(x, y, UI_ALTCOLOR, "L line  R turn rect  F face  P piece", max_chars, font);
	y += line - 2.f;
	ui_text(x, y, UI_ALTCOLOR, "Shift+F/P add  U unlock  Ctrl+S save", max_chars, font);
	y += line - 2.f;
	ui_text(x, y, UI_ALTCOLOR, "Ctrl+Z/Y undo/redo  Esc leave", max_chars, font);
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

// what an outline is of: the brush at a centre, or swept from it by (dx, dy) - all doubled texels
typedef struct {
	int c2x, c2y;
	int dx, dy;
	bool sweep;
} outline_t;

static bool outline_row(const outline_t *o, int ty, int *x0, int *x1)
{
	return o->sweep ? sweep_row(o->c2x, o->c2y, o->dx, o->dy, ty, x0, x1) : brush_row(o->c2x, o->c2y, ty, x0, x1);
}

/* The line being drawn, in doubled texels of a W x H map: its first centre and
   how far the last is from it. On a tiled surface the first is moved into the
   map's one copy - the shader wraps what it tests, and the map view shows that
   copy - the line keeping its length. */
static void line_centres(int W, int H, outline_t *o)
{
	o->sweep = true;
	o->c2x = centre2_x(au.line_tu0);
	o->c2y = centre2_y(au.line_tv0);
	o->dx = centre2_x(au.line_tu1) - o->c2x;
	o->dy = centre2_y(au.line_tv1) - o->c2y;
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
	if (o->sweep)
		sweep_rows(o->c2y, o->dy, &ty0, &ty1);
	else
		brush_rows(o->c2y, &ty0, &ty1);

	if ((!o->sweep && cur_size() * ppt < 3.f) || ty1 - ty0 > 8192)
	{
		// smaller than a few pixels on screen (or a line beyond reason): a marker at each end
		ui_dot(SX(o->c2x * 0.5f), SY(o->c2y * 0.5f), 3, color);
		if (o->sweep)
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
		if (img && au.show_base && base && base != img)
		{
			// the base texture, and the map over it as a layer (O, or the panel's slider)
			vkpt_draw_stretch_subpic_flags(sx0, sy0, sx1 - sx0, sy1 - sy0, u0 / W, v0 / H, u1 / W, v1 / H,
				(qhandle_t)image_index(base), nearest, false, true);
			R_SetColor_RTX(MakeColor(255, 255, 255, to_byte(au.layer_alpha)));
			vkpt_draw_stretch_subpic_flags(sx0, sy0, sx1 - sx0, sy1 - sy0, u0 / W, v0 / H, u1 / W, v1 / H,
				(qhandle_t)image_index(img), nearest, red_grey, true);
			R_SetColor_RTX(MakeColor(255, 255, 255, 255));
		}
		else
			vkpt_draw_stretch_subpic_flags(sx0, sy0, sx1 - sx0, sy1 - sy0, u0 / W, v0 / H, u1 / W, v1 / H,
				(qhandle_t)image_index(shown), nearest, red_grey, true);

		// the lock: what is outside it, darker (its texture's alpha)
		if (lk.on && lk.mask_w == W && lk.mask_h == H && lock_image_ok())
			vkpt_draw_stretch_subpic_flags(sx0, sy0, sx1 - sx0, sy1 - sy0, u0 / W, v0 / H, u1 / W, v1 / H,
				lk.image, true, false, false);

		if (!img)
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
	if (line_preview())
	{
		line_centres(W, H, &o);
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

	char caption[160], what[48];
	if (au.stage >= 0)
		Q_snprintf(what, sizeof(what), "stage%d %s", au.stage, slot_word[au.slot]);
	else
		Q_strlcpy(what, slot_word[au.slot], sizeof(what));
	if (view2d_texel(&tu, &tv))
		Q_snprintf(caption, sizeof(caption), "%s %dx%d  zoom %d%%  texel %d,%d", what, W, H,
			(int)(au.v2_ppt / ui_scale() * 100.f + 0.5f), (int)tu, (int)tv);
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

/* The line tool, each tick while the button is down: where it starts (the first
   place the cursor is on the map) and where it ends so far; once the button
   has come up, the brush swept along it, painted once as a stroke of its own. */
static void line_step(image_t *img, dab_ctx_t *c)
{
	const int W = img->upload_width, H = img->upload_height;
	float tu, tv;
	bool have;
	if (au.paint_2d)
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
		}
		au.line_tu1 = tu;
		au.line_tv1 = tv;
	}
	if (!au.line_commit)
		return;

	au.line_commit = au.line_active = false;
	au.painting = false;
	if (!au.line_have)
		return;
	au.line_have = false;

	// ends on two parts of a model whose UVs lie far apart make no line on it
	if (!au.paint_2d && (fabsf(au.line_tu1 - au.line_tu0) > 8.f * W || fabsf(au.line_tv1 - au.line_tv0) > 8.f * H))
	{
		author_status("the line's ends are too far apart on the map (two parts of it far apart?): not painted");
		return;
	}

	outline_t o;
	line_centres(W, H, &o);
	make_paint(au.paint_2d ? NULL : &au.line_hit, &c->paint);
	c->wrap = !au.paint_2d && map_wraps();
	// the spray, once: density is the share of the texels it dots
	c->spray_p = au.tool_opt[TOOL_SPRAY];
	stroke_touch(img);
	dab_sweep(c, o.c2x, o.c2y, o.dx, o.dy);
	stroke_end();
	const float len = sqrtf((au.line_tu1 - au.line_tu0) * (au.line_tu1 - au.line_tu0)
		+ (au.line_tv1 - au.line_tv0) * (au.line_tv1 - au.line_tv0));
	author_status("line, %d texels long", (int)(len + 0.5f));
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

	// the lock, for this map (a new map is the base texture's size, as the lock was made)
	lock_sync();

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
	lock_sync();

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

	au.cx = Q_clipf(au.cx + dx * speed, 0.f, (float)(r_config.width - 1));
	au.cy = Q_clipf(au.cy + dy * speed, 0.f, (float)(r_config.height - 1));

	if (au.drag != W_NONE)
		drag_update(au.drag);
	if (au.painting)
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
	ubo->author_brush_size = cur_w();
	ubo->author_brush_h = cur_h();
	ubo->author_brush_shape = cur_shape() == SHAPE_ROUND ? 0 : 1;
	// the lock, once rasterised for this map (lock_sync, between frames)
	ubo->author_lock_image = (lk.on && lk.mask_w == W && lk.mask_h == H && lock_image_ok()) ? (int)lk.image : 0;

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

	// the brush's centre: a line being drawn, swept from its start; under the cursor in the map
	// view; else where it is on a surface
	float tu, tv;
	if (line_preview())
	{
		outline_t o;
		line_centres(W, H, &o);
		ubo->author_brush_valid = 1;
		ubo->author_line = 1;
		ubo->author_center2_x = o.c2x;
		ubo->author_center2_y = o.c2y;
		ubo->author_line_dx2 = o.dx;
		ubo->author_line_dy2 = o.dy;
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

	if (key == K_ESCAPE)
	{
		if (down)
			author_stop("");
		return true;
	}

	const bool ctrl = Key_IsDown(K_CTRL), alt = Key_IsDown(K_ALT), shift = Key_IsDown(K_SHIFT);
	float mx, my;
	cursor_virtual(&mx, &my);

	switch (key)
	{
	case K_MOUSE1:
		if (!down)
		{
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
			au.drag = W_NONE;
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
			if (w == W_VIEW2D || w == W_NONE)
			{
				au.painting = true;
				au.paint_2d = (w == W_VIEW2D);
				au.have_last = false;
				au.create_tried = false;
				au.fill_done = false;
				au.spray_ms = 0;
				// a line starts where the cursor first is on the map (paint_step)
				au.line_active = au.line_mode && au.tool != TOOL_FILL;
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
		else if (size_slider_step(widget_at(mx, my), (key == K_MWHEELUP ? 1 : -1) * (shift ? 10 : 1)))
			;   // over a size slider: a texel at a time (Shift: ten)
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
			author_status(au.show_base ? "map view: the base texture under the map" : "map view: the map alone");
		}
		return true;

	case '[':
	case ']':
		if (down)
			brush_size_step(key == ']' ? 1 : -1);
		return true;

	case '1': case '2': case '3': case '4': case '5':
		if (down)
			set_slot(key - '1');
		return true;

	case 'b':
		if (down)
			set_shape((au.brush_shape + (shift ? NUM_SHAPES - 1 : 1)) % NUM_SHAPES);
		return true;

	case 'l':
		if (down)
			set_line_mode(!au.line_mode);
		return true;

	case 'r':
		if (down)
		{
			if (au.brush_shape == SHAPE_RECT)
				rect_turn();
			else
				author_status("R turns the rectangle brush - B picks it");
		}
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

	// mat author rect <length> <width>: the rectangle brush, in texels (length across the map, width down)
	if (!Q_stricmp(word, "rect"))
	{
		if (Cmd_Argc() < 5)
		{
			Com_Printf("usage: mat author rect <length texels> <width texels>\n");
			return;
		}
		if (!au.active)
			author_start(crosshair, -1);
		if (!au.active)
			return;
		au.rect_w = Q_clip(atoi(Cmd_Argv(3)), 1, AUTHOR_MAX_BRUSH);
		au.rect_h = Q_clip(atoi(Cmd_Argv(4)), 1, AUTHOR_MAX_BRUSH);
		set_shape(SHAPE_RECT);
		return;
	}

	int slot = -1;
	for (int i = 0; i < NUM_SLOTS; i++)
		if (!Q_stricmp(word, slot_word[i]) || !Q_stricmp(word, slot_label[i]))
			slot = i;
	if (word[0] && slot < 0)
	{
		Com_Printf("usage: mat author [roughness|metallic|normals|emissive|height] | stage <N|mat> | "
			"rect <length> <width> | save | off\n");
		return;
	}

	author_start(crosshair, slot);
}
