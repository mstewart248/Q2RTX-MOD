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
// cpu_trace.c -- cpu_bench: the scene path traced on the CPU, as a benchmark
//
// `cpu_bench` freezes the frame on screen and renders it again with no GPU at
// all, Cinebench style: the screen is cut into tiles, every logical processor
// takes the next tile as it comes free, and a bracket in the core's colour
// marks the tiles being worked on. When the last tile lands it prints the time,
// samples and rays per second. `cpu_bench` again (or `cpu_bench 0`, or a map
// change) ends it; `cpu_bench 1` starts a fresh one. Threads: cpu_bench_threads.
// With cpu_bench_infinite 1 it never lands: each finished round of
// cpu_bench_spp starts another on top, and the time per round is printed.
//
// What it renders is read straight out of what the GPU renderer already keeps
// in CPU memory: the world triangles bsp_mesh.c built (VboPrimitive, the same
// struct the shaders read), the brush entities and alias models posed for this
// frame, the textures' mip 0 (image_t.pix_data stays resident after upload),
// the light polygons and their per-cluster lists, the sun and the skybox. The
// physical sky only exists on the GPU, so its cube is copied back when the
// command runs. The shading follows get_material() and brdf.glsl: base /
// normal / roughness / metallic / emissive maps, Lambert + GGX, the 0.04
// dielectric F0.
//
// It is a reference renderer, not a copy of the GPU one: no ReSTIR, no SHaRC,
// no denoiser - one sun ray and one polygon light (picked by RIS over the
// cluster's light list) per vertex, then a BSDF bounce. The fog is the GPU's
// medium and calibration (see FOG below). Left out: dynamic and model lights,
// particles, sprites, beams, explosions, the player model, shells, material
// animation and scrolling.
//
// Emission is counted once: lights in the light lists are reached by the
// light sample, so a bounce that hits one counts its emission only when the
// bounce came off a near-mirror (roughness < CT_SMOOTH), and then the light
// sample leaves out the specular lobe - the same split as GPU's analytic light
// rule in indirect_lighting.rgen.

#include "vkpt.h"
#include "material.h"
#include "conversion.h"
#include "physical_sky.h"
#include "system/system.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#define CT_MAX_THREADS      256
#define CT_MAX_LAYERS       8       // glass / water surfaces one path may pass
#define CT_RIS_CANDIDATES   8       // light candidates per light sample
#define CT_SMOOTH           0.15f   // below this roughness specular is BSDF-sampled only
#define CT_EPS              0.02f   // ray offset off a surface, in world units
#define CT_FAR              65536.f
#define CT_STACK            96

// fog: the density is tabulated every CT_FOG_STEP units along a segment, at
// most CT_FOG_MAX_STEPS times; constants as fog_medium.glsl / global_ubo.h
#define CT_FOG_STEP         64.f
#define CT_FOG_MAX_STEPS    64
#define CT_FOG_HF_REF       0.025f  // FOG_HEIGHTFOG_REFERENCE
#define CT_FOG_DIST_REF     2.f     // FOG_DISTANCEFOG_REFERENCE
#define CT_FOG_ISO          0.0796f // FOG_ISOTROPIC_PHASE
#define CT_FOG_LOCAL        (0.005f * 0.05f)  // the local-light scale times FOG_LOCAL_DENSITY

// per-triangle flags, in ct_tri_t
#define CT_MASKED           1       // alpha tested against the material's mask
#define CT_TRANSPARENT      2       // glass, water: passed or reflected, never shaded
#define CT_SKY              4       // returns the environment
#define CT_PRIMARY_ONLY     8       // the view weapon: camera rays only

enum { CT_RAY_CAMERA, CT_RAY_BOUNCE, CT_RAY_SHADOW, CT_RAY_SKY };
enum { CT_ENV_NONE, CT_ENV_CUBE, CT_ENV_PHYSICAL, CT_ENV_FLAT };
enum { CT_FOG_SKY_NONE, CT_FOG_SKY_FADE, CT_FOG_SKY_UP, CT_FOG_SKY_EXPOSURE };
enum { CT_IDLE, CT_PENDING, CT_RUNNING, CT_DONE };

typedef struct {
	float bmin[3];
	uint32_t first;     // leaf: first triangle; interior: left child (right is +1)
	float bmax[3];
	uint32_t count;     // 0 = interior
} ct_node_t;

typedef struct {
	float v0[3], e1[3], e2[3];
	uint32_t flags;
} ct_tri_t;

typedef struct {
	float p0[3], e1[3], e2[3];
	float n[3];         // emits along -n... toward where -dot(n, L) > 0, as light_lists.h
	float area;
	float radiance[3];  // colour * emissive factor * style
	float vol_scale;    // how much it lights the fog (resolve_volumetric_scale)
	bool sky;           // a sky portal: radiance is the environment behind it
} ct_light_t;

typedef struct {
	float o[3], d[3], inv[3];
	float tmax;
} ct_ray_t;

typedef struct {
	float t, u, v;
	uint32_t prim;
} ct_hit_t;

typedef struct {
	float p[3];
	float ng[3];        // geometric normal, facing the incoming ray
	float n[3];         // shading normal, same side as ng
	float v[3];         // toward the viewer
	float base[3], albedo[3], f0[3];
	float roughness, metallic, specular_factor;
	float emissive[3];
	uint32_t material_id;
	int cluster;
	bool backface;      // the ray hit the side the triangle's normals point away from
} ct_surface_t;

typedef struct {
	uint64_t rays;
	uint64_t samples;
	uint64_t samples_by_class[2];
	int jobs;
	volatile int current_tile;  // -1 = between tiles; read by the display
	volatile int current_class;
	SDL_Thread *thread;
	char pad[64];               // keep neighbours' counters off this cache line
} ct_worker_t;

typedef struct { uint64_t state; } ct_rng_t;

static cvar_t *cvar_cpu_bench_spp;
static cvar_t *cvar_cpu_bench_scale;
static cvar_t *cvar_cpu_bench_bounces;
static cvar_t *cvar_cpu_bench_threads;
static cvar_t *cvar_cpu_bench_tile;
static cvar_t *cvar_cpu_bench_progressive;
static cvar_t *cvar_cpu_bench_clamp;
static cvar_t *cvar_cpu_bench_fog;
static cvar_t *cvar_cpu_bench_infinite;

// The scene, frozen at the frame the bench started on. Read-only while the
// workers run.
static struct {
	VboPrimitive *prims;    // shading data, in BVH order
	ct_tri_t *tris;         // intersection data, same order
	uint32_t num_prims;
	ct_node_t *nodes;
	uint32_t num_nodes;

	ct_light_t *lights;
	int num_lights;
	int num_fog_lights;     // after num_lights; reached only by the fog
	const int *cluster_light_offsets;
	const int *cluster_lights;
	int num_clusters;
	const byte *sky_visibility;

	float light_styles[MAX_LIGHTSTYLES];

	vec3_t origin, forward, right, up;
	float tan_half_x, tan_half_y;
	int width, height;

	bool sun;
	vec3_t sun_dir, sun_tangent, sun_bitangent, sun_irradiance;
	float sun_tan_half, sun_cos_half, sun_solid_angle;

	int env_type;
	vec3_t env_flat;
	float env_scale;
	vec3_t sky_matrix[3];
	vec3_t sun_dir_envmap;  // the sun in the rotated (pre-swizzle) cube space

	float bump_scale;
	float adapted_luminance;
	float exposure_bias, white_point;
	float max_indirect;     // luminance cap on a bounce's contribution, 0 = none

	int spp, max_bounces;

	// the fog, from this frame's UBO
	struct {
		bool on;
		bool map_fog;           // fog_enable: the map's medium; else the sun-only god rays
		bool white;             // getFogColor is 1 under the physical sky
		float density, hf_density, hf_falloff, hf_start_z, hf_end_z, density_max;
		vec3_t color, hf_top, hf_bottom;
		float hf_color_start_z, hf_color_end_z;
		bool color_layers;
		vec3_t world_center, world_half, world_half_inv;
		float sun_scale;        // fog_sun_scale() * 0.0001
		float g_sun, g_light, ambient;
		float sigma;            // extinction of the in-scatter on its way out
		float extinction, opacity;  // what dims the background, when > 0
		int *lights;            // the camera cluster's list, then its fog-only list
		int num_lights;
		int light_samples;
		float firefly;
		vec3_t sky_term;        // sky radiance * pi * pt_fog_scale_skybox * isotropic phase
		int sky_mode;
		float sky_fade, sky_soften, exposure_curve;
	} fog;
} scene;

static struct {
	int state;
	int num_threads;
	ct_worker_t workers[CT_MAX_THREADS];

	int tile_size, tiles_x, tiles_y, num_tiles;
	int spp_per_job, passes, num_jobs;
	bool infinite;              // passes go on past scene.spp, a round at a time
	int rounds_done;            // rounds every tile has finished (infinite mode)
	int *tile_order;            // centre-out
	SDL_atomic_t *tile_passes;  // passes finished per tile (progressive mode)
	SDL_atomic_t next_job;
	SDL_atomic_t jobs_done;
	SDL_atomic_t threads_done;
	volatile int abort;

	float *accum;               // per pixel: rgb sum, sample count

	uint64_t start_ticks, end_ticks;
	double build_seconds;

	// display
	qhandle_t pic_shown, pic_pending;
	int pic_seq;
	uint64_t last_update_ticks;
	qhandle_t font;
	char result[3][160];
} run;

// The map's skybox, kept from R_SetSky_RTX - the GPU copy is all the renderer
// keeps otherwise.
static byte *envmap_data;
static int envmap_width, envmap_height;

// The physical sky (Earth, Stroggos, clouds, terrain) only exists as a cube the
// GPU renders, so `cpu_bench` copies it back when it starts: RGBA half floats.
static uint16_t *physky_data;
static int physky_width, physky_height;
static float half_to_float[65536];

// efficiency class per logical processor, normalised so 1 = performance core
static signed char cpu_class[CT_MAX_THREADS * 4];
static bool cpu_hybrid;
static int cpu_cores_by_class[2], cpu_threads_by_class[2];

static float srgb_to_linear[256];
static byte linear_to_srgb[4096];

/*
=============================================================================

SMALL MATH

=============================================================================
*/

static inline float ct_lum(const float c[3])
{
	return c[0] * 0.2126f + c[1] * 0.7152f + c[2] * 0.0722f;
}

static inline float ct_clampf(float x, float lo, float hi)
{
	return x < lo ? lo : (x > hi ? hi : x);
}

static inline float ct_sq(float x)
{
	return x * x;
}

static inline float ct_pow5(float x)
{
	const float x2 = x * x;
	return x2 * x2 * x;
}

static inline void ct_normalize(float v[3])
{
	float l = DotProduct(v, v);
	if (l > 0.f) {
		l = 1.f / sqrtf(l);
		VectorScale(v, l, v);
	}
}

static inline void ct_decode_normal(uint32_t enc, float n[3])
{
	float px = (float)(enc & 0xffffu) / 65535.f * 2.f - 1.f;
	float py = (float)(enc >> 16) / 65535.f * 2.f - 1.f;
	n[0] = px;
	n[1] = py;
	n[2] = 1.f - fabsf(px) - fabsf(py);
	float t = max(0.f, -n[2]);
	n[0] += (n[0] >= 0.f) ? -t : t;
	n[1] += (n[1] >= 0.f) ? -t : t;
	ct_normalize(n);
}

static inline uint32_t ct_rand_u32(ct_rng_t *r)
{
	uint64_t old = r->state;
	r->state = old * 6364136223846793005ULL + 1442695040888963407ULL;
	uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
	uint32_t rot = (uint32_t)(old >> 59u);
	return (xorshifted >> rot) | (xorshifted << ((0u - rot) & 31u));
}

static inline float ct_rand(ct_rng_t *r)
{
	return (float)(ct_rand_u32(r) >> 8) * (1.f / 16777216.f);
}

// Seeded by pixel and sample alone, so the picture is the same whichever
// thread rendered which tile, and from one run to the next.
static inline void ct_rng_seed(ct_rng_t *r, uint32_t pixel, uint32_t sample)
{
	uint64_t z = ((uint64_t)pixel << 32 | sample) + 0x9E3779B97F4A7C15ULL;
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	r->state = z ^ (z >> 31);
	ct_rand_u32(r);
}

static void ct_basis(const float n[3], float t[3], float b[3])
{
	if (fabsf(n[2]) < 0.999f) {
		VectorSet(t, -n[1], n[0], 0.f);
	} else {
		VectorSet(t, 0.f, -n[2], n[1]);
	}
	ct_normalize(t);
	CrossProduct(n, t, b);
}

static inline void ct_ray_init(ct_ray_t *r, const float o[3], const float d[3], float tmax)
{
	VectorCopy(o, r->o);
	VectorCopy(d, r->d);
	for (int i = 0; i < 3; i++) {
		float c = d[i];
		if (fabsf(c) < 1e-20f)
			c = (c < 0.f) ? -1e-20f : 1e-20f;
		r->inv[i] = 1.f / c;
	}
	r->tmax = tmax;
}

static inline void ct_offset(const float p[3], const float n[3], float sign, float out[3])
{
	// grows with distance from the origin, where float spacing does
	float e = CT_EPS + 2e-6f * max(fabsf(p[0]), max(fabsf(p[1]), fabsf(p[2])));
	VectorMA(p, sign * e, n, out);
}

/*
=============================================================================

TEXTURES

=============================================================================
*/

static void ct_init_tables(void)
{
	for (int i = 0; i < 65536; i++) {
		const float f = halfToFloat((uint16_t)i);
		half_to_float[i] = isfinite(f) ? f : 0.f;
	}
	for (int i = 0; i < 256; i++) {
		float c = (float)i / 255.f;
		srgb_to_linear[i] = (c <= 0.04045f) ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
	}
	for (int i = 0; i < 4096; i++) {
		float c = (float)i / 4095.f;
		c = (c <= 0.0031308f) ? c * 12.92f : 1.055f * powf(c, 1.f / 2.4f) - 0.055f;
		linear_to_srgb[i] = (byte)ct_clampf(c * 255.f + 0.5f, 0.f, 255.f);
	}
}

// Bilinear, wrapping, from mip 0. sRGB images come back linear, alpha never
// converted - the same as sampling the VK_FORMAT_*_SRGB image on the GPU.
static void ct_sample(const image_t *img, float u, float v, float out[4])
{
	if (!img || !img->pix_data || img->pixel_format != PF_R8G8B8A8_UNORM
		|| img->upload_width <= 0 || img->upload_height <= 0 || !isfinite(u) || !isfinite(v)) {
		out[0] = out[1] = out[2] = out[3] = 1.f;
		return;
	}

	const int w = img->upload_width, h = img->upload_height;
	u -= floorf(u);
	v -= floorf(v);
	float x = u * (float)w - 0.5f, y = v * (float)h - 0.5f;
	float fx = floorf(x), fy = floorf(y);
	float ax = x - fx, ay = y - fy;
	int x0 = (int)fx, y0 = (int)fy;
	if (x0 < 0) x0 += w;
	if (y0 < 0) y0 += h;
	if (x0 >= w) x0 -= w;
	if (y0 >= h) y0 -= h;
	int x1 = (x0 + 1 == w) ? 0 : x0 + 1;
	int y1 = (y0 + 1 == h) ? 0 : y0 + 1;

	const byte *pix = img->pix_data;
	const byte *t00 = pix + ((size_t)y0 * w + x0) * 4;
	const byte *t10 = pix + ((size_t)y0 * w + x1) * 4;
	const byte *t01 = pix + ((size_t)y1 * w + x0) * 4;
	const byte *t11 = pix + ((size_t)y1 * w + x1) * 4;

	const float w00 = (1.f - ax) * (1.f - ay), w10 = ax * (1.f - ay);
	const float w01 = (1.f - ax) * ay, w11 = ax * ay;

	if (img->is_srgb) {
		for (int c = 0; c < 3; c++)
			out[c] = srgb_to_linear[t00[c]] * w00 + srgb_to_linear[t10[c]] * w10
			       + srgb_to_linear[t01[c]] * w01 + srgb_to_linear[t11[c]] * w11;
	} else {
		for (int c = 0; c < 3; c++)
			out[c] = ((float)t00[c] * w00 + (float)t10[c] * w10
			        + (float)t01[c] * w01 + (float)t11[c] * w11) * (1.f / 255.f);
	}
	out[3] = ((float)t00[3] * w00 + (float)t10[3] * w10
	        + (float)t01[3] * w01 + (float)t11[3] * w11) * (1.f / 255.f);
}

// Where a cube-space direction lands: face (layer) and 0..1 face coordinates,
// by the Vulkan cube map rules.
static bool ct_cube_coords(const float c[3], int *face, float *s, float *t)
{
	const float x = c[0], y = c[1], z = c[2];
	const float ax = fabsf(x), ay = fabsf(y), az = fabsf(z);
	float sc, tc, ma;
	if (ax >= ay && ax >= az) {
		*face = (x >= 0.f) ? 0 : 1;
		sc = (x >= 0.f) ? -z : z;
		tc = -y;
		ma = ax;
	} else if (ay >= az) {
		*face = (y >= 0.f) ? 2 : 3;
		sc = x;
		tc = (y >= 0.f) ? z : -z;
		ma = ay;
	} else {
		*face = (z >= 0.f) ? 4 : 5;
		sc = (z >= 0.f) ? x : -x;
		tc = -y;
		ma = az;
	}
	if (!(ma > 0.f))
		return false;
	*s = sc / ma * 0.5f + 0.5f;
	*t = tc / ma * 0.5f + 0.5f;
	return true;
}

// The inverse: the cube-space direction (unnormalised) through face
// coordinates u, v in -1..1.
static void ct_cube_dir(int face, float u, float v, float c[3])
{
	switch (face) {
	case 0: VectorSet(c, 1.f, -v, -u); break;
	case 1: VectorSet(c, -1.f, -v, u); break;
	case 2: VectorSet(c, u, 1.f, v); break;
	case 3: VectorSet(c, u, -1.f, -v); break;
	case 4: VectorSet(c, u, -v, 1.f); break;
	default: VectorSet(c, -u, -v, -1.f); break;
	}
}

// Bilinear within one face of the physical sky, edges clamped.
static void ct_physky_sample(int face, float s, float t, float out[3])
{
	const int w = physky_width, h = physky_height;
	const float x = s * (float)w - 0.5f, y = t * (float)h - 0.5f;
	const float fx = floorf(x), fy = floorf(y);
	const float ax = x - fx, ay = y - fy;
	int x0 = (int)fx, y0 = (int)fy;
	int x1 = x0 + 1, y1 = y0 + 1;
	x0 = x0 < 0 ? 0 : (x0 >= w ? w - 1 : x0);
	x1 = x1 < 0 ? 0 : (x1 >= w ? w - 1 : x1);
	y0 = y0 < 0 ? 0 : (y0 >= h ? h - 1 : y0);
	y1 = y1 < 0 ? 0 : (y1 >= h ? h - 1 : y1);

	const uint16_t *base = physky_data + (size_t)face * w * h * 4;
	const uint16_t *t00 = base + ((size_t)y0 * w + x0) * 4;
	const uint16_t *t10 = base + ((size_t)y0 * w + x1) * 4;
	const uint16_t *t01 = base + ((size_t)y1 * w + x0) * 4;
	const uint16_t *t11 = base + ((size_t)y1 * w + x1) * 4;
	const float w00 = (1.f - ax) * (1.f - ay), w10 = ax * (1.f - ay);
	const float w01 = (1.f - ax) * ay, w11 = ax * ay;
	for (int c = 0; c < 3; c++)
		out[c] = half_to_float[t00[c]] * w00 + half_to_float[t10[c]] * w10
		       + half_to_float[t01[c]] * w01 + half_to_float[t11[c]] * w11;
}

// env_map() in path_tracer_rgen.h, times pt_env_scale as every caller there
// applies it: the direction rotated by the sky matrix, then swizzled .xzy for
// the cube. The physical sky is the GPU's own cube; remove_sun clips the sun
// disk out of it where the sun is sampled separately. A map skybox has faces
// ft bk up dn rt lf = +X -X +Y -Y +Z -Z, UNORM, desaturated 10% and halved as
// DESATURATE_ENVIRONMENT_MAP does.
static void ct_env(const float dir[3], bool remove_sun, float out[3])
{
	if (scene.env_type == CT_ENV_FLAT) {
		VectorCopy(scene.env_flat, out);
		return;
	}

	VectorClear(out);
	const bool physical = (scene.env_type == CT_ENV_PHYSICAL && physky_data);
	if (!physical && (scene.env_type != CT_ENV_CUBE || !envmap_data))
		return;

	float r[3];
	for (int i = 0; i < 3; i++)
		r[i] = scene.sky_matrix[0][i] * dir[0] + scene.sky_matrix[1][i] * dir[1] + scene.sky_matrix[2][i] * dir[2];
	const float c[3] = { r[0], r[2], r[1] };

	int face;
	float s, t;
	if (!ct_cube_coords(c, &face, &s, &t))
		return;

	if (physical) {
		ct_physky_sample(face, s, t, out);
		if (remove_sun) {
			const float limit = (1.f - DotProduct(r, scene.sun_dir_envmap)) * 200.f;
			for (int k = 0; k < 3; k++)
				out[k] = min(out[k], limit);
		}
		VectorScale(out, scene.env_scale, out);
		return;
	}

	int si = (int)(s * (float)envmap_width);
	int ti = (int)(t * (float)envmap_height);
	si = si < 0 ? 0 : (si >= envmap_width ? envmap_width - 1 : si);
	ti = ti < 0 ? 0 : (ti >= envmap_height ? envmap_height - 1 : ti);

	const byte *texel = envmap_data + (((size_t)face * envmap_height + ti) * envmap_width + si) * 4;
	float v[3] = { (float)texel[0] / 255.f, (float)texel[1] / 255.f, (float)texel[2] / 255.f };
	const float avg = (v[0] + v[1] + v[2]) * (1.f / 3.f);
	for (int k = 0; k < 3; k++)
		out[k] = (v[k] + (avg - v[k]) * 0.1f) * 0.5f * scene.env_scale;
}

void vkpt_cpu_trace_set_envmap(int width, int height, const byte *data)
{
	// a running bench reads the old one
	if (run.state == CT_RUNNING)
		vkpt_cpu_trace_stop();

	Z_Free(envmap_data);
	envmap_data = NULL;
	envmap_width = envmap_height = 0;

	if (!data || width <= 0 || height <= 0)
		return;

	size_t size = (size_t)width * height * 4 * 6;
	envmap_data = Z_Malloc(size);
	memcpy(envmap_data, data, size);
	envmap_width = width;
	envmap_height = height;
}

/*
=============================================================================

INTERSECTION

=============================================================================
*/

static inline const pbr_material_t *ct_material(uint32_t material_id)
{
	return r_materials + (material_id & MATERIAL_INDEX_MASK);
}

static bool ct_alpha_test(uint32_t prim, float u, float v)
{
	const VboPrimitive *p = scene.prims + prim;
	const pbr_material_t *mat = ct_material(p->material_id);
	if (!mat->image_mask)
		return true;

	const float w = 1.f - u - v;
	float m[4];
	ct_sample(mat->image_mask,
		p->uv0[0] * w + p->uv1[0] * u + p->uv2[0] * v,
		p->uv0[1] * w + p->uv1[1] * u + p->uv2[1] * v, m);
	return m[0] >= 0.5f;
}

static inline bool ct_box(const ct_node_t *n, const ct_ray_t *r, float tmax, float *tnear)
{
	float t0 = (n->bmin[0] - r->o[0]) * r->inv[0];
	float t1 = (n->bmax[0] - r->o[0]) * r->inv[0];
	float lo = min(t0, t1), hi = max(t0, t1);

	t0 = (n->bmin[1] - r->o[1]) * r->inv[1];
	t1 = (n->bmax[1] - r->o[1]) * r->inv[1];
	lo = max(lo, min(t0, t1));
	hi = min(hi, max(t0, t1));

	t0 = (n->bmin[2] - r->o[2]) * r->inv[2];
	t1 = (n->bmax[2] - r->o[2]) * r->inv[2];
	lo = max(lo, min(t0, t1));
	hi = min(hi, max(t0, t1));

	*tnear = lo;
	return hi >= max(lo, 0.f) && lo < tmax;
}

static inline bool ct_triangle(const ct_tri_t *t, const ct_ray_t *r, float tmax, float *ot, float *ou, float *ov)
{
	float pvec[3], tvec[3], qvec[3];
	CrossProduct(r->d, t->e2, pvec);
	const float det = DotProduct(t->e1, pvec);
	if (fabsf(det) < 1e-12f)
		return false;
	const float inv = 1.f / det;

	VectorSubtract(r->o, t->v0, tvec);
	const float u = DotProduct(tvec, pvec) * inv;
	if (u < 0.f || u > 1.f)
		return false;

	CrossProduct(tvec, t->e1, qvec);
	const float v = DotProduct(r->d, qvec) * inv;
	if (v < 0.f || u + v > 1.f)
		return false;

	const float tt = DotProduct(t->e2, qvec) * inv;
	if (tt <= 0.f || tt >= tmax)
		return false;

	*ot = tt;
	*ou = u;
	*ov = v;
	return true;
}

// Closest hit, or for shadow rays any hit. Shadow rays see neither glass nor
// sky - the GPU's shadow cull mask leaves both out. Sky rays (the fog's sky
// visibility) look through glass for the sky.
static bool ct_trace(const ct_ray_t *r, ct_hit_t *hit, int mode, ct_worker_t *w)
{
	const uint32_t skip = (mode == CT_RAY_CAMERA) ? 0
		: (mode == CT_RAY_BOUNCE) ? CT_PRIMARY_ONLY
		: (mode == CT_RAY_SKY) ? (CT_PRIMARY_ONLY | CT_TRANSPARENT)
		: (CT_PRIMARY_ONLY | CT_TRANSPARENT | CT_SKY);
	const bool any = (mode == CT_RAY_SHADOW);

	w->rays++;

	float tmax = r->tmax;
	float tnear;
	bool found = false;

	if (!ct_box(&scene.nodes[0], r, tmax, &tnear))
		return false;

	uint32_t stack_node[CT_STACK];
	float stack_near[CT_STACK];
	int sp = 0;
	uint32_t ni = 0;

	for (;;) {
		const ct_node_t *n = &scene.nodes[ni];

		if (n->count) {
			const uint32_t end = n->first + n->count;
			for (uint32_t i = n->first; i < end; i++) {
				const ct_tri_t *t = &scene.tris[i];
				if (t->flags & skip)
					continue;

				float tt, u, v;
				if (!ct_triangle(t, r, tmax, &tt, &u, &v))
					continue;
				if ((t->flags & CT_MASKED) && !ct_alpha_test(i, u, v))
					continue;

				tmax = tt;
				found = true;
				if (hit) {
					hit->t = tt;
					hit->u = u;
					hit->v = v;
					hit->prim = i;
				}
				if (any)
					return true;
			}
		} else {
			uint32_t a = n->first, b = a + 1;
			float ta, tb;
			const bool ha = ct_box(&scene.nodes[a], r, tmax, &ta);
			const bool hb = ct_box(&scene.nodes[b], r, tmax, &tb);

			if (ha && hb) {
				if (tb < ta) {
					uint32_t s = a; a = b; b = s;
					float f = ta; ta = tb; tb = f;
				}
				if (sp < CT_STACK) {
					stack_node[sp] = b;
					stack_near[sp] = tb;
					sp++;
				}
				ni = a;
				continue;
			}
			if (ha) { ni = a; continue; }
			if (hb) { ni = b; continue; }
		}

		// pop, skipping what a nearer hit has since made unreachable
		for (;;) {
			if (sp == 0)
				return found;
			sp--;
			if (stack_near[sp] < tmax)
				break;
		}
		ni = stack_node[sp];
	}
}

/*
=============================================================================

BVH BUILD

Binned SAH over the triangle centroids, 12 bins on each axis, leaves of up to
4 triangles (8 when no split is cheaper). The triangles are then stored in
leaf order so a leaf is one contiguous run.

=============================================================================
*/

#define CT_BINS 12

typedef struct {
	float bmin[3], bmax[3];
} ct_aabb_t;

static inline void ct_aabb_clear(ct_aabb_t *b)
{
	VectorSet(b->bmin, 1e30f, 1e30f, 1e30f);
	VectorSet(b->bmax, -1e30f, -1e30f, -1e30f);
}

static inline void ct_aabb_grow(ct_aabb_t *b, const ct_aabb_t *o)
{
	for (int i = 0; i < 3; i++) {
		b->bmin[i] = min(b->bmin[i], o->bmin[i]);
		b->bmax[i] = max(b->bmax[i], o->bmax[i]);
	}
}

static inline float ct_aabb_area(const ct_aabb_t *b)
{
	float dx = b->bmax[0] - b->bmin[0], dy = b->bmax[1] - b->bmin[1], dz = b->bmax[2] - b->bmin[2];
	if (dx < 0.f || dy < 0.f || dz < 0.f)
		return 0.f;
	return 2.f * (dx * dy + dy * dz + dz * dx);
}

static void ct_build_bvh(const VboPrimitive *prims, const byte *flags, uint32_t n)
{
	ct_aabb_t *boxes = Z_Malloc(sizeof(ct_aabb_t) * n);
	float (*cent)[3] = Z_Malloc(sizeof(float) * 3 * n);
	uint32_t *idx = Z_Malloc(sizeof(uint32_t) * n);

	for (uint32_t i = 0; i < n; i++) {
		const VboPrimitive *p = prims + i;
		for (int k = 0; k < 3; k++) {
			boxes[i].bmin[k] = min(p->pos0[k], min(p->pos1[k], p->pos2[k]));
			boxes[i].bmax[k] = max(p->pos0[k], max(p->pos1[k], p->pos2[k]));
			cent[i][k] = (boxes[i].bmin[k] + boxes[i].bmax[k]) * 0.5f;
		}
		idx[i] = i;
	}

	scene.nodes = Z_Malloc(sizeof(ct_node_t) * (2 * n + 1));
	scene.num_nodes = 1;

	struct { uint32_t node, start, count; } stack[128];
	int sp = 0;
	stack[sp].node = 0; stack[sp].start = 0; stack[sp].count = n; sp++;

	while (sp > 0) {
		sp--;
		const uint32_t ni = stack[sp].node, start = stack[sp].start, count = stack[sp].count;
		ct_node_t *node = &scene.nodes[ni];

		ct_aabb_t bounds, cb;
		ct_aabb_clear(&bounds);
		ct_aabb_clear(&cb);
		for (uint32_t i = start; i < start + count; i++) {
			ct_aabb_grow(&bounds, &boxes[idx[i]]);
			for (int k = 0; k < 3; k++) {
				cb.bmin[k] = min(cb.bmin[k], cent[idx[i]][k]);
				cb.bmax[k] = max(cb.bmax[k], cent[idx[i]][k]);
			}
		}
		VectorCopy(bounds.bmin, node->bmin);
		VectorCopy(bounds.bmax, node->bmax);

		if (count <= 2 || sp + 2 > (int)(sizeof(stack) / sizeof(stack[0]))) {
			node->first = start;
			node->count = count;
			continue;
		}

		const float node_area = max(ct_aabb_area(&bounds), 1e-12f);
		float best_cost = 1e30f;
		int best_axis = -1, best_split = 0;

		for (int axis = 0; axis < 3; axis++) {
			const float extent = cb.bmax[axis] - cb.bmin[axis];
			if (extent < 1e-6f)
				continue;

			ct_aabb_t bin_box[CT_BINS];
			uint32_t bin_count[CT_BINS] = { 0 };
			for (int b = 0; b < CT_BINS; b++)
				ct_aabb_clear(&bin_box[b]);

			const float k = (float)CT_BINS / extent;
			for (uint32_t i = start; i < start + count; i++) {
				int b = (int)((cent[idx[i]][axis] - cb.bmin[axis]) * k);
				b = b < 0 ? 0 : (b >= CT_BINS ? CT_BINS - 1 : b);
				bin_count[b]++;
				ct_aabb_grow(&bin_box[b], &boxes[idx[i]]);
			}

			float right_area[CT_BINS];
			uint32_t right_count[CT_BINS];
			ct_aabb_t acc;
			ct_aabb_clear(&acc);
			uint32_t c = 0;
			for (int b = CT_BINS - 1; b > 0; b--) {
				ct_aabb_grow(&acc, &bin_box[b]);
				c += bin_count[b];
				right_area[b] = ct_aabb_area(&acc);
				right_count[b] = c;
			}

			ct_aabb_clear(&acc);
			c = 0;
			for (int b = 0; b < CT_BINS - 1; b++) {
				ct_aabb_grow(&acc, &bin_box[b]);
				c += bin_count[b];
				if (!c || !right_count[b + 1])
					continue;
				const float cost = 1.f + ((float)c * ct_aabb_area(&acc) + (float)right_count[b + 1] * right_area[b + 1]) / node_area;
				if (cost < best_cost) {
					best_cost = cost;
					best_axis = axis;
					best_split = b;
				}
			}
		}

		uint32_t mid;
		if (best_axis < 0) {
			// every centroid in one place: a leaf if it is small, halves if not
			if (count <= 8) {
				node->first = start;
				node->count = count;
				continue;
			}
			mid = start + count / 2;
		} else {
			if (best_cost >= (float)count && count <= 8) {
				node->first = start;
				node->count = count;
				continue;
			}

			const float extent = cb.bmax[best_axis] - cb.bmin[best_axis];
			const float k = (float)CT_BINS / extent;
			uint32_t i = start, j = start + count;
			while (i < j) {
				int b = (int)((cent[idx[i]][best_axis] - cb.bmin[best_axis]) * k);
				b = b < 0 ? 0 : (b >= CT_BINS ? CT_BINS - 1 : b);
				if (b <= best_split) {
					i++;
				} else {
					j--;
					uint32_t s = idx[i]; idx[i] = idx[j]; idx[j] = s;
				}
			}
			mid = i;
			if (mid == start || mid == start + count)
				mid = start + count / 2;
		}

		const uint32_t left = scene.num_nodes;
		scene.num_nodes += 2;
		node->first = left;
		node->count = 0;

		stack[sp].node = left + 1; stack[sp].start = mid; stack[sp].count = start + count - mid; sp++;
		stack[sp].node = left; stack[sp].start = start; stack[sp].count = mid - start; sp++;
	}

	// store the triangles in leaf order
	scene.prims = Z_Malloc(sizeof(VboPrimitive) * n);
	scene.tris = Z_Malloc(sizeof(ct_tri_t) * n);
	scene.num_prims = n;
	for (uint32_t i = 0; i < n; i++) {
		const VboPrimitive *p = prims + idx[i];
		ct_tri_t *t = scene.tris + i;
		scene.prims[i] = *p;
		VectorCopy(p->pos0, t->v0);
		VectorSubtract(p->pos1, p->pos0, t->e1);
		VectorSubtract(p->pos2, p->pos0, t->e2);
		t->flags = flags[idx[i]];
	}

	Z_Free(boxes);
	Z_Free(cent);
	Z_Free(idx);
}

/*
=============================================================================

SCENE CAPTURE

=============================================================================
*/

static VboPrimitive *gather_prims;
static byte *gather_flags;
static uint32_t gather_count, gather_alloc;

static VboPrimitive *ct_gather(uint32_t count, byte flags)
{
	if (gather_count + count > gather_alloc) {
		gather_alloc = max(gather_alloc * 2, gather_count + count + 65536);
		gather_prims = Z_Realloc(gather_prims, sizeof(VboPrimitive) * gather_alloc);
		gather_flags = Z_Realloc(gather_flags, gather_alloc);
	}
	memset(gather_flags + gather_count, flags, count);
	VboPrimitive *p = gather_prims + gather_count;
	gather_count += count;
	return p;
}

static byte ct_flags_for_material(uint32_t material_id)
{
	if ((material_id & MATERIAL_KIND_MASK) == MATERIAL_KIND_SKY)
		return CT_SKY;
	if (MAT_IsTransparent(material_id))
		return CT_TRANSPARENT;
	if (MAT_IsMasked(material_id))
		return CT_MASKED;
	return 0;
}

static void ct_gather_world(const bsp_mesh_t *wm, const model_geometry_t *g, byte flags)
{
	for (uint32_t i = 0; i < g->num_geometries; i++) {
		if (!g->prim_counts[i])
			continue;
		VboPrimitive *dst = ct_gather(g->prim_counts[i], flags);
		memcpy(dst, wm->primitives + g->prim_offsets[i], sizeof(VboPrimitive) * g->prim_counts[i]);
	}
}

static inline void ct_xform_point(const float m[16], const float in[3], float out[3])
{
	out[0] = m[0] * in[0] + m[4] * in[1] + m[8] * in[2] + m[12];
	out[1] = m[1] * in[0] + m[5] * in[1] + m[9] * in[2] + m[13];
	out[2] = m[2] * in[0] + m[6] * in[1] + m[10] * in[2] + m[14];
}

static inline void ct_xform_dir(const float m[16], const float in[3], float out[3])
{
	out[0] = m[0] * in[0] + m[4] * in[1] + m[8] * in[2];
	out[1] = m[1] * in[0] + m[5] * in[1] + m[9] * in[2];
	out[2] = m[2] * in[0] + m[6] * in[1] + m[10] * in[2];
	ct_normalize(out);
}

static inline uint32_t ct_xform_encoded(const float m[16], uint32_t enc)
{
	float n[3], o[3];
	ct_decode_normal(enc, n);
	ct_xform_dir(m, n, o);
	return encode_normal(o);
}

static int ct_point_cluster(bsp_t *bsp, const float p[3])
{
	if (!bsp)
		return -1;
	return BSP_PointLeaf(bsp->nodes, p)->cluster;
}

// doors, lifts, platforms: their slice of the world mesh, moved to where the
// entity is this frame - process_bsp_entity in main.c
static void ct_gather_brush_entity(const bsp_mesh_t *wm, bsp_t *bsp, const entity_t *e)
{
	const int index = ~e->model;
	if (index < 0 || index >= wm->num_models)
		return;

	const bsp_model_t *model = wm->models + index;
	const model_geometry_t *g = &model->geometry;

	float m[16], center[3];
	create_entity_matrix(m, (entity_t *)e, false);
	ct_xform_point(m, model->center, center);
	const int cluster = ct_point_cluster(bsp, center);

	for (uint32_t gi = 0; gi < g->num_geometries; gi++) {
		const VboPrimitive *src = wm->primitives + g->prim_offsets[gi];
		for (uint32_t i = 0; i < g->prim_counts[gi]; i++) {
			VboPrimitive *p = ct_gather(1, ct_flags_for_material(src[i].material_id));
			*p = src[i];
			ct_xform_point(m, src[i].pos0, p->pos0);
			ct_xform_point(m, src[i].pos1, p->pos1);
			ct_xform_point(m, src[i].pos2, p->pos2);
			for (int k = 0; k < 3; k++) {
				p->normals[k] = ct_xform_encoded(m, src[i].normals[k]);
				p->tangents[k] = ct_xform_encoded(m, src[i].tangents[k]);
			}
			p->cluster = cluster;
		}
	}
}

// One vertex of an alias model, posed: blended between two frames, or
// skinned by the bone matrices - instance_geometry.comp.
static void ct_pose_vertex(const model_t *model, const maliasmesh_t *mesh, int idx,
	int frame, int oldframe, float backlerp, const float *bones,
	float pos[3], float nrm[3], float tan[3])
{
	const int a = frame * mesh->numverts + idx;
	const int b = oldframe * mesh->numverts + idx;

	if (!bones) {
		for (int k = 0; k < 3; k++) {
			pos[k] = mesh->positions[a][k] * (1.f - backlerp) + mesh->positions[b][k] * backlerp;
			nrm[k] = mesh->normals[a][k] * (1.f - backlerp) + mesh->normals[b][k] * backlerp;
			tan[k] = mesh->tangents ? mesh->tangents[a][k] * (1.f - backlerp) + mesh->tangents[b][k] * backlerp : 0.f;
		}
		return;
	}

	VectorCopy(mesh->positions[idx], pos);
	VectorCopy(mesh->normals[idx], nrm);
	if (mesh->tangents)
		VectorCopy(mesh->tangents[idx], tan);
	else
		VectorClear(tan);

	if (!mesh->blend_indices || !mesh->blend_weights)
		return;

	const uint32_t bi = mesh->blend_indices[idx], bw = mesh->blend_weights[idx];
	float m[12] = { 0 };
	float sum = 0.f;
	for (int k = 0; k < 4; k++) {
		const uint32_t bone = (bi >> (k * 8)) & 0xff;
		const float weight = (float)((bw >> (k * 8)) & 0xff);
		if (weight <= 0.f || bone >= (uint32_t)model->iqmData->num_poses)
			continue;
		for (int j = 0; j < 12; j++)
			m[j] += bones[bone * 12 + j] * weight;
		sum += weight;
	}
	if (sum <= 0.f)
		return;

	const float rs = 1.f / sum;
	float p[3] = { pos[0], pos[1], pos[2] }, n[3] = { nrm[0], nrm[1], nrm[2] }, t[3] = { tan[0], tan[1], tan[2] };
	for (int r = 0; r < 3; r++) {
		pos[r] = (m[r * 4 + 0] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3]) * rs;
		nrm[r] = m[r * 4 + 0] * n[0] + m[r * 4 + 1] * n[1] + m[r * 4 + 2] * n[2];
		tan[r] = m[r * 4 + 0] * t[0] + m[r * 4 + 1] * t[1] + m[r * 4 + 2] * t[2];
	}
	ct_normalize(nrm);
	ct_normalize(tan);
}

static void ct_gather_alias_entity(bsp_t *bsp, const entity_t *e)
{
	if (e->flags & (RF_VIEWERMODEL | RF_BEAM | RF_FIRST_PERSON_FX | RF_REFLECTION_FX))
		return;

	const model_t *model = MOD_ForHandle(e->model);
	if (!model || !model->meshes)
		return;
	if (model->model_class == MCLASS_EXPLOSION || model->model_class == MCLASS_FLASH
		|| model->model_class == MCLASS_SMOKE || model->model_class == MCLASS_FLARE
		|| model->model_class == MCLASS_PLAYER_BEAM)
		return;

	const bool weapon = (e->flags & RF_WEAPONMODEL) != 0;

	float m[16];
	create_entity_matrix(m, (entity_t *)e, weapon);
	const int cluster = ct_point_cluster(bsp, e->origin);

	float *bones = NULL;
	if (model->iqmData && model->iqmData->num_poses) {
		bones = Z_Malloc(sizeof(float) * 12 * model->iqmData->num_poses);
		if (!R_ComputeIQMTransforms(model->iqmData, e, bones)) {
			Z_Free(bones);
			return;
		}
	}

	int frame = e->frame, oldframe = e->oldframe;
	if (frame < 0 || frame >= model->numframes) frame = 0;
	if (oldframe < 0 || oldframe >= model->numframes) oldframe = 0;
	if (bones)
		frame = oldframe = 0;

	for (int mi = 0; mi < model->nummeshes; mi++) {
		const maliasmesh_t *mesh = model->meshes + mi;
		if (!mesh->numtris || !mesh->positions || !mesh->normals || !mesh->tex_coords)
			continue;

		const uint32_t material_id = vkpt_mesh_material_id(e, model, mesh, weapon);
		if (!material_id)
			continue;

		const byte flags = ct_flags_for_material(material_id) | (weapon ? CT_PRIMARY_ONLY : 0);
		VboPrimitive *dst = ct_gather(mesh->numtris, flags);

		for (int tri = 0; tri < mesh->numtris; tri++) {
			VboPrimitive *p = dst + tri;
			memset(p, 0, sizeof(*p));
			float *positions[3] = { p->pos0, p->pos1, p->pos2 };
			float *uvs[3] = { p->uv0, p->uv1, p->uv2 };

			for (int k = 0; k < 3; k++) {
				const int idx = mesh->indices[tri * 3 + k];
				float pos[3], nrm[3], tan[3], tmp[3];
				ct_pose_vertex(model, mesh, idx, frame, oldframe, e->backlerp, bones, pos, nrm, tan);

				ct_xform_point(m, pos, positions[k]);
				ct_xform_dir(m, nrm, tmp);
				p->normals[k] = encode_normal(tmp);
				if (DotProduct(tan, tan) > 0.f) {
					ct_xform_dir(m, tan, tmp);
					p->tangents[k] = encode_normal(tmp);
				}

				const int uv = frame * mesh->numverts + idx;
				uvs[k][0] = mesh->tex_coords[uv][0];
				uvs[k][1] = mesh->tex_coords[uv][1];
			}

			p->material_id = material_id;
			p->cluster = cluster;
			p->emissive_and_alpha = 0x3c003c00; // (1.0, 1.0), as stage_mesh_primitives
		}
	}

	Z_Free(bones);
}

static void ct_capture_light(const light_poly_t *src, ct_light_t *l)
{
	VectorCopy(src->positions + 0, l->p0);
	VectorSubtract(src->positions + 3, src->positions + 0, l->e1);
	VectorSubtract(src->positions + 6, src->positions + 0, l->e2);
	CrossProduct(l->e1, l->e2, l->n);
	l->area = 0.5f * sqrtf(DotProduct(l->n, l->n));
	ct_normalize(l->n);

	// copy_light in vertex_buffer.c: a negative colour marks a sky portal,
	// which the fog leaves to its own sky term
	if (src->color[0] < 0.f) {
		l->sky = true;
		return;
	}

	float style = 1.f;
	if (src->style > 0 && src->style < MAX_LIGHTSTYLES)
		style = scene.light_styles[src->style];
	const float mat_scale = src->material ? src->material->emissive_factor : 1.f;
	VectorScale(src->color, mat_scale * style, l->radiance);
	l->vol_scale = vkpt_light_volumetric_scale(src);
}

static void ct_capture_lights(const bsp_mesh_t *wm)
{
	scene.num_lights = min(wm->num_light_polys, MAX_LIGHT_POLYS);

	// The fog-only lights (lava) go after the rest. The GPU keeps them at the
	// top of its light buffer, and goes without them when the regular lights
	// have grown into that space.
	scene.num_fog_lights = 0;
	if (wm->num_fog_light_polys > 0 && scene.num_lights <= MAX_LIGHT_POLYS - wm->num_fog_light_polys)
		scene.num_fog_lights = wm->num_fog_light_polys;

	scene.lights = Z_Mallocz(sizeof(ct_light_t) * max(1, scene.num_lights + scene.num_fog_lights));
	for (int i = 0; i < scene.num_lights; i++)
		ct_capture_light(wm->light_polys + i, scene.lights + i);
	for (int i = 0; i < scene.num_fog_lights; i++)
		ct_capture_light(wm->fog_light_polys + i, scene.lights + scene.num_lights + i);

	scene.cluster_light_offsets = wm->cluster_light_offsets;
	scene.cluster_lights = wm->cluster_lights;
	scene.num_clusters = wm->num_clusters;
	scene.sky_visibility = wm->sky_visibility;
}

/*
=============================================================================

SHADING

=============================================================================
*/

static void ct_fetch_surface(const ct_ray_t *ray, const ct_hit_t *hit, ct_surface_t *s)
{
	const VboPrimitive *p = scene.prims + hit->prim;
	const ct_tri_t *t = scene.tris + hit->prim;
	const float u = hit->u, v = hit->v, w = 1.f - u - v;

	VectorMA(ray->o, hit->t, ray->d, s->p);
	VectorNegate(ray->d, s->v);

	float ng[3];
	CrossProduct(t->e1, t->e2, ng);
	ct_normalize(ng);

	// the front: the side the vertex normals point to
	float n0[3], n1[3], n2[3], n[3];
	ct_decode_normal(p->normals[0], n0);
	ct_decode_normal(p->normals[1], n1);
	ct_decode_normal(p->normals[2], n2);
	for (int k = 0; k < 3; k++)
		n[k] = n0[k] * w + n1[k] * u + n2[k] * v;
	if (DotProduct(n, n) < 1e-8f)
		VectorCopy(ng, n);
	ct_normalize(n);
	if (DotProduct(n, ng) < 0.f)
		VectorNegate(ng, ng);

	const float tu = p->uv0[0] * w + p->uv1[0] * u + p->uv2[0] * v;
	const float tv = p->uv0[1] * w + p->uv1[1] * u + p->uv2[1] * v;

	s->material_id = p->material_id;
	s->cluster = p->cluster;
	const pbr_material_t *mat = ct_material(p->material_id);

	float base[4] = { 1.f, 1.f, 1.f, 1.f };
	if (mat->image_base)
		ct_sample(mat->image_base, tu, tv, base);
	for (int k = 0; k < 3; k++)
		s->base[k] = ct_clampf(base[k] * mat->base_factor, 0.f, 1.f);

	// get_material() in path_tracer_rgen.h, less POM and Toksvig
	float roughness = 1.f, metallic = 0.f;
	const bool have_rough = mat->image_roughness != NULL;
	const bool have_metal = mat->image_metallic != NULL;
	float tmp[4];

	if (have_rough) {
		ct_sample(mat->image_roughness, tu, tv, tmp);
		roughness = tmp[0];
	}
	if (have_metal) {
		ct_sample(mat->image_metallic, tu, tv, tmp);
		metallic = ct_clampf(tmp[0] * mat->metalness_factor, 0.f, 1.f);
	}

	if (mat->image_normals) {
		float nm[4];
		ct_sample(mat->image_normals, tu, tv, nm);
		float local[3] = { nm[0] * 2.f - 1.f, nm[1] * 2.f - 1.f, nm[2] * 2.f - 1.f };
		ct_normalize(local);

		float t0[3], t1[3], t2[3], tangent[3];
		ct_decode_normal(p->tangents[0], t0);
		ct_decode_normal(p->tangents[1], t1);
		ct_decode_normal(p->tangents[2], t2);
		for (int k = 0; k < 3; k++)
			tangent[k] = t0[k] * w + t1[k] * u + t2[k] * v;

		float bump = scene.bump_scale * mat->bump_scale;
		if ((p->material_id & MATERIAL_KIND_MASK) == MATERIAL_KIND_GLASS)
			bump *= 0.2f;

		if (p->tangents[0] && DotProduct(tangent, tangent) > 1e-8f && bump > 0.f && isfinite(tangent[0])) {
			ct_normalize(tangent);

			// the bitangent's sign comes from this triangle's UV winding
			float bitangent[3], cr[3];
			const float duv0x = p->uv1[0] - p->uv0[0], duv0y = p->uv1[1] - p->uv0[1];
			const float duv1x = p->uv2[0] - p->uv0[0], duv1y = p->uv2[1] - p->uv0[1];
			const float uv_winding = duv0x * duv1y - duv1x * duv0y;
			CrossProduct(t->e1, t->e2, cr);
			const float tri_facing = DotProduct(cr, n);
			CrossProduct(n, tangent, bitangent);
			if (uv_winding * tri_facing < 0.f)
				VectorNegate(bitangent, bitangent);

			float mapped[3];
			for (int k = 0; k < 3; k++)
				mapped[k] = tangent[k] * local[0] + bitangent[k] * local[1] + n[k] * local[2];
			for (int k = 0; k < 3; k++)
				n[k] = n[k] + (mapped[k] - n[k]) * bump;
			ct_normalize(n);
		}

		if (!have_metal)
			metallic = ct_clampf(nm[3] * mat->metalness_factor, 0.f, 1.f);
		if (!have_rough)
			roughness = base[3];
	} else if (have_metal && !have_rough) {
		roughness = base[3];
	}

	if (mat->roughness_override >= 0.f)
		roughness = max(roughness, mat->roughness_override);
	s->roughness = ct_clampf(roughness, 0.f, 1.f);
	s->metallic = metallic;
	s->specular_factor = mat->specular_factor + (1.f - mat->specular_factor) * metallic;

	VectorClear(s->emissive);
	const float prim_emissive = halfToFloat((uint16_t)(p->emissive_and_alpha & 0xffff));
	if (prim_emissive > 0.f && mat->image_emissive) {
		float e[4];
		ct_sample(mat->image_emissive, tu, tv, e);
		float scale = mat->emissive_factor * prim_emissive;
		if ((p->material_id & MATERIAL_KIND_MASK) != MATERIAL_KIND_CAMERA) {
			const uint32_t style = (p->material_id & MATERIAL_LIGHT_STYLE_MASK) >> MATERIAL_LIGHT_STYLE_SHIFT;
			if (style)
				scale *= scene.light_styles[style];
		}
		for (int k = 0; k < 3; k++)
			s->emissive[k] = max(0.f, e[k] - 0.001f) * scale;
	}

	for (int k = 0; k < 3; k++) {
		s->albedo[k] = s->base[k] * (1.f - 0.04f) * (1.f - metallic);
		s->f0[k] = 0.04f + (s->base[k] - 0.04f) * metallic;
	}

	// turn everything to face the ray
	s->backface = DotProduct(ng, ray->d) > 0.f;
	if (s->backface) {
		VectorNegate(ng, ng);
		VectorNegate(n, n);
	}
	VectorCopy(ng, s->ng);

	// a bumped normal facing away from the viewer is pulled back to the horizon
	float nv = DotProduct(n, s->v);
	if (nv < 0.01f) {
		VectorMA(n, 0.01f - nv, s->v, n);
		ct_normalize(n);
	}
	VectorCopy(n, s->n);

	if (s->cluster < 0)
		s->cluster = -1;
}

static inline float ct_g1(float roughness, float NoX)
{
	const float a2 = ct_sq(ct_sq(roughness));
	return 2.f * NoX / (NoX + sqrtf(a2 + (1.f - a2) * NoX * NoX));
}

// brdf * cos(theta_l), as direct lighting adds it up: the diffuse
// (1 - F) * NdotL / pi times the albedo, plus GGX_times_NdotL.
static void ct_brdf_cos(const ct_surface_t *s, const float L[3], bool specular, float out[3])
{
	const float NoL = DotProduct(s->n, L);
	if (NoL <= 0.f) {
		VectorClear(out);
		return;
	}

	float H[3];
	VectorAdd(L, s->v, H);
	ct_normalize(H);
	const float VoH = max(0.f, DotProduct(s->v, H));
	const float NoV = max(1e-4f, DotProduct(s->n, s->v));
	const float NoH = ct_clampf(DotProduct(s->n, H), 0.f, 1.f);

	const float fw = ct_pow5(1.f - VoH);
	float F[3];
	for (int k = 0; k < 3; k++)
		F[k] = ct_clampf((s->f0[k] + (1.f - s->f0[k]) * fw) * s->specular_factor, 0.f, 1.f);

	float spec = 0.f;
	if (specular && VoH > 0.f) {
		const float alpha = ct_sq(max(s->roughness, 0.02f));
		const float a2 = alpha * alpha;
		const float D = a2 / (M_PI * ct_sq(NoH * NoH * a2 + (1.f - NoH * NoH)));
		const float ag = ct_sq(ct_sq(s->roughness));
		const float g1 = NoL / (NoL + sqrtf(ag + (1.f - ag) * NoL * NoL));
		const float g2 = 1.f / (NoV + sqrtf(ag + (1.f - ag) * NoV * NoV));
		spec = D * g1 * g2;
	}

	for (int k = 0; k < 3; k++)
		out[k] = s->albedo[k] * (1.f - F[k]) * NoL * (1.f / M_PI) + F[k] * spec;
}

static inline bool ct_sky_visible(int cluster)
{
	if (cluster < 0 || !scene.sky_visibility)
		return true;
	return (scene.sky_visibility[cluster >> 3] & (1 << (cluster & 7))) != 0;
}

static bool ct_unoccluded(ct_worker_t *w, const ct_surface_t *s, const float L[3], float dist)
{
	ct_ray_t r;
	float o[3];
	ct_offset(s->p, s->ng, 1.f, o);
	ct_ray_init(&r, o, L, dist);
	return !ct_trace(&r, NULL, CT_RAY_SHADOW, w);
}

// One sun sample and one polygon light sample.
static void ct_direct(ct_worker_t *w, ct_rng_t *rng, const ct_surface_t *s, float out[3])
{
	const bool specular = s->roughness >= CT_SMOOTH;
	float f[3];

	VectorClear(out);

	if (scene.sun && ct_sky_visible(s->cluster)) {
		const float r = sqrtf(ct_rand(rng)) * scene.sun_tan_half;
		const float phi = 2.f * M_PI * ct_rand(rng);
		float L[3];
		for (int k = 0; k < 3; k++)
			L[k] = scene.sun_dir[k] + scene.sun_tangent[k] * (r * cosf(phi)) + scene.sun_bitangent[k] * (r * sinf(phi));
		ct_normalize(L);

		if (DotProduct(L, s->n) > 0.f && DotProduct(L, s->ng) > 0.f && ct_unoccluded(w, s, L, CT_FAR)) {
			ct_brdf_cos(s, L, specular, f);
			for (int k = 0; k < 3; k++)
				out[k] += f[k] * scene.sun_irradiance[k];
		}
	}

	if (s->cluster < 0 || s->cluster >= scene.num_clusters || !scene.cluster_light_offsets)
		return;

	const int begin = scene.cluster_light_offsets[s->cluster];
	const int count = scene.cluster_light_offsets[s->cluster + 1] - begin;
	if (count <= 0)
		return;

	// RIS over (light, point): candidates uniform over the list and over each
	// triangle's area, kept in proportion to their unshadowed contribution.
	float wsum = 0.f, sel_phat = 0.f, sel_dist = 0.f;
	float sel_f[3] = { 0.f, 0.f, 0.f }, sel_L[3] = { 0.f, 0.f, 0.f };

	for (int c = 0; c < CT_RIS_CANDIDATES; c++) {
		int pick = (int)(ct_rand(rng) * (float)count);
		if (pick >= count)
			pick = count - 1;
		const int li = scene.cluster_lights[begin + pick];
		const float u1 = ct_rand(rng), u2 = ct_rand(rng);
		if (li < 0 || li >= scene.num_lights)
			continue;
		const ct_light_t *l = scene.lights + li;
		if (l->area <= 0.f)
			continue;

		const float su = sqrtf(u1);
		const float b1 = su * (1.f - u2), b2 = su * u2;
		float q[3], d[3];
		for (int k = 0; k < 3; k++)
			q[k] = l->p0[k] + l->e1[k] * b1 + l->e2[k] * b2;
		VectorSubtract(q, s->p, d);
		const float dist2 = DotProduct(d, d);
		if (dist2 < 1e-4f)
			continue;
		const float dist = sqrtf(dist2);
		VectorScale(d, 1.f / dist, d);

		if (DotProduct(d, s->n) <= 0.f || DotProduct(d, s->ng) <= 0.f)
			continue;

		float cos_l = -DotProduct(l->n, d);
		float Le[3];
		if (l->sky) {
			cos_l = fabsf(cos_l);
			ct_env(d, true, Le);
		} else {
			if (cos_l <= 0.f)
				continue;
			// light_lists.h: emission falls off as sqrt(cos) from the normal
			VectorScale(l->radiance, sqrtf(cos_l), Le);
		}

		ct_brdf_cos(s, d, specular, f);
		const float g = cos_l / dist2;
		for (int k = 0; k < 3; k++)
			f[k] *= Le[k] * g;

		const float phat = ct_lum(f);
		if (!(phat > 0.f))
			continue;
		const float weight = phat * (float)count * l->area;
		wsum += weight;
		if (ct_rand(rng) * wsum < weight) {
			sel_phat = phat;
			sel_dist = dist;
			VectorCopy(f, sel_f);
			VectorCopy(d, sel_L);
		}
	}

	if (wsum <= 0.f || sel_phat <= 0.f)
		return;

	// stop short of the light's own surface, which sits on the light polygon
	if (!ct_unoccluded(w, s, sel_L, max(0.f, sel_dist - 0.05f)))
		return;

	const float scale = wsum / ((float)CT_RIS_CANDIDATES * sel_phat);
	for (int k = 0; k < 3; k++)
		out[k] += sel_f[k] * scale;
}

// The next direction off an opaque surface: GGX (VNDF) or cosine diffuse,
// picked by their share of the reflectance. False ends the path.
static bool ct_bounce(const ct_surface_t *s, ct_rng_t *rng, ct_ray_t *ray, float T[3], bool *count_emission)
{
	const float NoV = max(1e-4f, DotProduct(s->n, s->v));
	const float fw = ct_pow5(1.f - NoV);
	float Fv[3], diff[3];
	for (int k = 0; k < 3; k++) {
		Fv[k] = ct_clampf((s->f0[k] + (1.f - s->f0[k]) * fw) * s->specular_factor, 0.f, 1.f);
		diff[k] = s->albedo[k] * (1.f - Fv[k]);
	}
	const float ws = ct_lum(Fv), wd = ct_lum(diff);
	if (ws + wd <= 0.f)
		return false;
	const float p_spec = ws / (ws + wd);

	float t[3], b[3], L[3];
	ct_basis(s->n, t, b);

	if (ct_rand(rng) < p_spec) {
		const float alpha = ct_sq(s->roughness);
		const float u1 = ct_rand(rng), u2 = ct_rand(rng);
		float H[3];

		if (alpha < 1e-4f) {
			VectorCopy(s->n, H);
		} else {
			// Heitz 2018, as ImportanceSampleGGX_VNDF in brdf.glsl
			float Ve[3] = { DotProduct(s->v, t), DotProduct(s->v, b), DotProduct(s->v, s->n) };
			float Vh[3] = { alpha * Ve[0], alpha * Ve[1], Ve[2] };
			ct_normalize(Vh);
			const float lensq = Vh[0] * Vh[0] + Vh[1] * Vh[1];
			float T1[3], T2[3];
			if (lensq > 0.f) {
				const float il = 1.f / sqrtf(lensq);
				VectorSet(T1, -Vh[1] * il, Vh[0] * il, 0.f);
			} else {
				VectorSet(T1, 1.f, 0.f, 0.f);
			}
			CrossProduct(Vh, T1, T2);
			const float r = sqrtf(u1), phi = 2.f * M_PI * u2;
			const float t1 = r * cosf(phi);
			float t2 = r * sinf(phi);
			const float sh = 0.5f * (1.f + Vh[2]);
			t2 = (1.f - sh) * sqrtf(max(0.f, 1.f - t1 * t1)) + sh * t2;
			const float tz = sqrtf(max(0.f, 1.f - t1 * t1 - t2 * t2));
			float Nh[3];
			for (int k = 0; k < 3; k++)
				Nh[k] = t1 * T1[k] + t2 * T2[k] + tz * Vh[k];
			float Ne[3] = { alpha * Nh[0], alpha * Nh[1], max(0.f, Nh[2]) };
			ct_normalize(Ne);
			for (int k = 0; k < 3; k++)
				H[k] = t[k] * Ne[0] + b[k] * Ne[1] + s->n[k] * Ne[2];
		}

		const float VoH = DotProduct(s->v, H);
		if (VoH <= 0.f)
			return false;
		for (int k = 0; k < 3; k++)
			L[k] = 2.f * VoH * H[k] - s->v[k];

		const float NoL = DotProduct(s->n, L);
		if (NoL <= 0.f || DotProduct(s->ng, L) <= 0.f)
			return false;

		const float hw = ct_pow5(1.f - VoH);
		const float g1 = ct_g1(s->roughness, NoL);
		for (int k = 0; k < 3; k++) {
			const float F = ct_clampf((s->f0[k] + (1.f - s->f0[k]) * hw) * s->specular_factor, 0.f, 1.f);
			T[k] *= F * g1 / p_spec;
		}
		*count_emission = s->roughness < CT_SMOOTH;
	} else {
		const float u1 = ct_rand(rng), u2 = ct_rand(rng);
		const float r = sqrtf(u1), phi = 2.f * M_PI * u2;
		const float lx = r * cosf(phi), ly = r * sinf(phi), lz = sqrtf(max(0.f, 1.f - u1));
		for (int k = 0; k < 3; k++)
			L[k] = t[k] * lx + b[k] * ly + s->n[k] * lz;
		if (DotProduct(s->ng, L) <= 0.f)
			return false;
		for (int k = 0; k < 3; k++)
			T[k] *= diff[k] / (1.f - p_spec);
		*count_emission = false;
	}

	float o[3];
	ct_offset(s->p, s->ng, 1.f, o);
	ct_ray_init(ray, o, L, CT_FAR);
	return true;
}

// Glass and the like are thin: a Fresnel share is mirrored, the rest goes
// straight on, tinted by the texture. Water and slime bend the ray.
static void ct_transmit(const ct_surface_t *s, ct_rng_t *rng, ct_ray_t *ray, float T[3], bool *count_emission)
{
	const uint32_t kind = s->material_id & MATERIAL_KIND_MASK;
	const bool liquid = (kind == MATERIAL_KIND_WATER || kind == MATERIAL_KIND_SLIME);

	float n[3];
	VectorCopy(s->n, n);
	float cos_i = DotProduct(s->v, n);
	if (cos_i <= 0.f) {
		VectorCopy(s->ng, n);
		cos_i = max(1e-4f, DotProduct(s->v, n));
	}

	float dir[3] = { ray->d[0], ray->d[1], ray->d[2] };
	bool reflect = false;

	if (liquid) {
		// the water surface's normals point out of the water
		const float eta = s->backface ? 1.33f : 1.f / 1.33f;
		const float sin2_t = eta * eta * (1.f - cos_i * cos_i);
		if (sin2_t >= 1.f) {
			reflect = true;
		} else {
			const float cos_t = sqrtf(1.f - sin2_t);
			const float c = s->backface ? cos_t : cos_i;
			const float F = 0.02f + 0.98f * ct_pow5(1.f - c);
			if (ct_rand(rng) < F) {
				reflect = true;
			} else {
				for (int k = 0; k < 3; k++)
					dir[k] = -s->v[k] * eta + n[k] * (eta * cos_i - cos_t);
				ct_normalize(dir);
				if (kind == MATERIAL_KIND_SLIME)
					for (int k = 0; k < 3; k++)
						T[k] *= s->base[k];
			}
		}
	} else {
		const float F = 0.04f + 0.96f * ct_pow5(1.f - cos_i);
		if (ct_rand(rng) < F) {
			reflect = true;
		} else {
			VectorCopy(ray->d, dir);
			for (int k = 0; k < 3; k++)
				T[k] *= s->base[k];
		}
	}

	float o[3];
	if (reflect) {
		for (int k = 0; k < 3; k++)
			dir[k] = 2.f * cos_i * n[k] - s->v[k];
		if (DotProduct(dir, s->ng) <= 0.f)
			for (int k = 0; k < 3; k++)
				dir[k] = 2.f * DotProduct(s->v, s->ng) * s->ng[k] - s->v[k];
		ct_offset(s->p, s->ng, 1.f, o);
		// a mirror: there was no light sample here to count the lights for us
		*count_emission = true;
	} else {
		ct_offset(s->p, s->ng, -1.f, o);
	}
	ct_ray_init(ray, o, dir, CT_FAR);
}

static inline void ct_add(float L[3], const float T[3], const float c[3], int bounce)
{
	float v[3] = { T[0] * c[0], T[1] * c[1], T[2] * c[2] };
	if (bounce > 0 && scene.max_indirect > 0.f) {
		const float l = ct_lum(v);
		if (l > scene.max_indirect)
			VectorScale(v, scene.max_indirect / l, v);
	}
	if (isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]))
		VectorAdd(L, v, L);
}

/*
=============================================================================

FOG

The medium photo mode marches in god_rays.comp (fog_medium.glsl): the same
density, colour and phase function, and the same three terms - the sun
through a shadow ray, one light picked by RIS from the camera cluster's list,
and the sky through rays looking for it - at the same calibration. As on the
GPU it lies along the camera ray and what glass and mirrors pass on, not along
diffuse bounces, and it stays additive unless pt_fog_extinction is set. Where
the GPU steps every few units, this takes one scatter point per segment,
drawn in proportion to the density.

=============================================================================
*/

// ScatterPhase_HenyeyGreenstein as god_rays.comp builds it
// (NORMALIZE_PHASE_FUNCTIONS): not the textbook HG, and not normalised
static inline float ct_fog_phase(float cosa, float g)
{
	const float g2 = g * g;
	const float frac = (1.f - fabsf(g)) / sqrtf(max(1.f - 2.f * g * cosa + g2, 1e-8f));
	return (g2 + (1.f - g2) / (4.f * M_PI)) * frac * frac * frac;
}

// getHeightFogDensity
static float ct_fog_height_density(const float p[3])
{
	if (scene.fog.hf_density <= 0.f)
		return 0.f;

	const float above = p[2] - scene.fog.hf_end_z;
	const float band = scene.fog.hf_start_z - scene.fog.hf_end_z;
	if (above <= 0.f || band <= 0.f)
		return scene.fog.hf_density / CT_FOG_HF_REF;

	const float t = above / band;
	const float decay = ct_clampf(scene.fog.hf_falloff * band, 0.75f, 1.5f);
	const float d = scene.fog.hf_density * expf(-t * decay) * ct_clampf(2.f - t, 0.f, 1.f);
	return d / CT_FOG_HF_REF;
}

// getDensity: the map's medium, faded out past the world's bounds
static float ct_fog_density(const float p[3])
{
	float box = 1.f;
	for (int k = 0; k < 3; k++)
		box *= ct_clampf(3.f - 2.f * fabsf((p[k] - scene.fog.world_center[k]) * scene.fog.world_half_inv[k]), 0.f, 1.f);
	if (box <= 0.f || !scene.fog.map_fog)
		return box;

	const float d = ct_fog_height_density(p) + scene.fog.density / CT_FOG_DIST_REF;
	return box * ct_clampf(d, 0.f, scene.fog.density_max);
}

// getFogColor
static void ct_fog_color(const float p[3], float out[3])
{
	if (!scene.fog.map_fog || scene.fog.white) {
		VectorSet(out, 1.f, 1.f, 1.f);
		return;
	}
	if (scene.fog.hf_density <= 0.f) {
		VectorCopy(scene.fog.color, out);
		return;
	}

	const float *top = scene.fog.hf_top, *bottom = scene.fog.hf_bottom;
	const float ramp = scene.fog.hf_color_end_z - scene.fog.hf_color_start_z;
	const float t = (ramp != 0.f) ? ct_clampf((p[2] - scene.fog.hf_color_start_z) / ramp, 0.f, 1.f) : 0.f;
	for (int k = 0; k < 3; k++)
		out[k] = top[k] + (bottom[k] - top[k]) * t;
	if (out[0] + out[1] + out[2] < 0.01f)
		VectorCopy((top[0] + top[1] + top[2] > bottom[0] + bottom[1] + bottom[2]) ? top : bottom, out);

	if (!scene.fog.color_layers)
		return;

	const float *dist_color = scene.fog.color;
	const float dist_d = scene.fog.density / CT_FOG_DIST_REF;
	const float hf_d = ct_fog_height_density(p);
	if (dist_color[0] + dist_color[1] + dist_color[2] < 0.01f || hf_d + dist_d <= 0.f)
		return;
	for (int k = 0; k < 3; k++)
		out[k] = (out[k] * hf_d + dist_color[k] * dist_d) / (hf_d + dist_d);
}

// The first thing along d is the sky, glass aside. *t_block gets the distance
// to what is in the way when it is not.
static bool ct_fog_sees_sky(ct_worker_t *w, const float p[3], const float d[3], float *t_block)
{
	ct_ray_t r;
	ct_hit_t hit;
	ct_ray_init(&r, p, d, CT_FAR);
	if (!ct_trace(&r, &hit, CT_RAY_SKY, w)) {
		*t_block = CT_FAR;
		return false;   // out of the map, which is not the sky
	}
	*t_block = hit.t;
	return (scene.tris[hit.prim].flags & CT_SKY) != 0;
}

// getSkyVisibility
static float ct_fog_sky_visibility(ct_worker_t *w, ct_rng_t *rng, const float p[3])
{
	float t_block;

	switch (scene.fog.sky_mode) {
	case CT_FOG_SKY_FADE:
		return scene.fog.sky_fade;

	case CT_FOG_SKY_UP: {
		static const float up[3] = { 0.f, 0.f, 1.f };
		if (ct_fog_sees_sky(w, p, up, &t_block))
			return 1.f;
		return (scene.fog.sky_soften > 0.f) ? ct_clampf(t_block / scene.fog.sky_soften, 0.f, 1.f) : 0.f;
	}

	case CT_FOG_SKY_EXPOSURE: {
		// The GPU bakes e, the share of the upper hemisphere that sees sky, and
		// uses 1 - (1 - e)^curve. With `curve` uniform rays that is exactly the
		// chance that any of them sees sky.
		int rays = (int)scene.fog.exposure_curve;
		if (ct_rand(rng) < scene.fog.exposure_curve - (float)rays)
			rays++;
		rays = max(1, rays);
		for (int i = 0; i < rays; i++) {
			const float z = ct_rand(rng), phi = 2.f * M_PI * ct_rand(rng);
			const float r = sqrtf(max(0.f, 1.f - z * z));
			const float d[3] = { r * cosf(phi), r * sinf(phi), z };
			if (ct_fog_sees_sky(w, p, d, &t_block))
				return 1.f;
		}
		return 0.f;
	}
	}

	return 0.f;
}

// volume_light_estimate: the RIS target, from the light's centre
static float ct_fog_light_estimate(const ct_light_t *l, const float p[3], const float view[3])
{
	if (l->sky || l->vol_scale <= 0.f || l->area <= 0.f)
		return 0.f;

	float d[3];
	for (int k = 0; k < 3; k++)
		d[k] = l->p0[k] + (l->e1[k] + l->e2[k]) * (1.f / 3.f) - p[k];
	const float len2 = DotProduct(d, d);
	if (len2 <= 0.f)
		return 0.f;
	const float inv = 1.f / sqrtf(len2);
	const float cos_emit = -DotProduct(l->n, d) * inv;
	if (cos_emit <= 0.f)
		return 0.f;

	const float phase = ct_fog_phase(DotProduct(view, d) * inv, scene.fog.g_light);
	return ct_lum(l->radiance) * l->area * sqrtf(cos_emit) * phase * l->vol_scale / max(len2, 64.f);
}

// fog_ris_initial + fog_shade_chosen: candidates uniform over the list, the
// winner sampled on its area with one shadow ray
static void ct_fog_lights(ct_worker_t *w, ct_rng_t *rng, const float p[3], const float view[3], float out[3])
{
	VectorClear(out);

	const int count = scene.fog.num_lights;
	if (count <= 0)
		return;

	float wsum = 0.f, sel_phat = 0.f;
	const ct_light_t *sel = NULL;
	for (int c = 0; c < scene.fog.light_samples; c++) {
		int pick = (int)(ct_rand(rng) * (float)count);
		if (pick >= count)
			pick = count - 1;
		const int li = scene.fog.lights[pick];
		if (li < 0)
			continue;
		const ct_light_t *l = scene.lights + li;
		const float estimate = ct_fog_light_estimate(l, p, view);
		if (!(estimate > 0.f))
			continue;
		const float weight = estimate * (float)count;
		wsum += weight;
		if (ct_rand(rng) * wsum <= weight) {
			sel = l;
			sel_phat = estimate;
		}
	}
	if (!sel || wsum <= 0.f)
		return;
	const float W = wsum / ((float)scene.fog.light_samples * sel_phat);

	const float su = sqrtf(ct_rand(rng)), u2 = ct_rand(rng);
	const float b1 = su * (1.f - u2), b2 = su * u2;
	float d[3];
	for (int k = 0; k < 3; k++)
		d[k] = sel->p0[k] + sel->e1[k] * b1 + sel->e2[k] * b2 - p[k];
	const float dist2 = DotProduct(d, d);
	if (dist2 < 1e-8f)
		return;
	const float dist = sqrtf(dist2);
	VectorScale(d, 1.f / dist, d);

	const float cos_l = -DotProduct(sel->n, d);
	if (cos_l <= 0.f)
		return;

	ct_ray_t r;
	ct_ray_init(&r, p, d, max(dist - 1.f, 0.01f));
	if (ct_trace(&r, NULL, CT_RAY_SHADOW, w))
		return;

	// radiance * sqrt(cos) emission profile * phase / pdf, the pdf over solid angle
	const float inv_pdf = sel->area * cos_l / dist2;
	const float k = sqrtf(cos_l) * ct_fog_phase(DotProduct(view, d), scene.fog.g_light) * sel->vol_scale * W * inv_pdf;
	VectorScale(sel->radiance, k, out);

	if (scene.fog.firefly > 0.f) {
		const float lum = ct_lum(out);
		if (lum > scene.fog.firefly)
			VectorScale(out, scene.fog.firefly / lum, out);
	}
}

// The light the medium sends back along -d at p, per unit density.
static void ct_fog_inscatter(ct_worker_t *w, ct_rng_t *rng, const float p[3], const float d[3], float out[3])
{
	VectorClear(out);

	if (scene.sun && scene.fog.sun_scale > 0.f) {
		const float r = sqrtf(ct_rand(rng)) * scene.sun_tan_half;
		const float phi = 2.f * M_PI * ct_rand(rng);
		float L[3];
		for (int k = 0; k < 3; k++)
			L[k] = scene.sun_dir[k] + scene.sun_tangent[k] * (r * cosf(phi)) + scene.sun_bitangent[k] * (r * sinf(phi));
		ct_normalize(L);

		ct_ray_t ray;
		ct_ray_init(&ray, p, L, CT_FAR);
		const float lit = ct_trace(&ray, NULL, CT_RAY_SHADOW, w) ? 0.f : 1.f;
		const float scatter = lit * ct_fog_phase(DotProduct(d, scene.sun_dir), scene.fog.g_sun)
			+ (1.f - lit) * scene.fog.ambient * CT_FOG_ISO;
		VectorScale(scene.sun_irradiance, scatter * scene.fog.sun_scale, out);
	}

	if (!scene.fog.map_fog)
		return;

	float local[3];
	ct_fog_lights(w, rng, p, d, local);
	if (scene.fog.sky_mode != CT_FOG_SKY_NONE) {
		const float vis = ct_fog_sky_visibility(w, rng, p);
		VectorMA(local, vis, scene.fog.sky_term, local);
	}
	VectorMA(out, CT_FOG_LOCAL, local, out);
}

// The fog between the ray's origin and len along it: its in-scatter goes into
// L, and T is dimmed for what lies behind when the medium has real extinction.
static void ct_fog_segment(ct_worker_t *w, ct_rng_t *rng, const ct_ray_t *ray, float len, float T[3], float L[3], int bounce)
{
	// getDensity is zero 1.5 half-sizes from the world's centre
	float t0 = 0.f, t1 = min(len, CT_FAR);
	for (int k = 0; k < 3; k++) {
		const float lo = (scene.fog.world_center[k] - 1.5f * scene.fog.world_half[k] - ray->o[k]) * ray->inv[k];
		const float hi = (scene.fog.world_center[k] + 1.5f * scene.fog.world_half[k] - ray->o[k]) * ray->inv[k];
		t0 = max(t0, min(lo, hi));
		t1 = min(t1, max(lo, hi));
	}
	if (!(t1 > t0))
		return;

	// optical depth, tabulated at jittered points
	const int n = (int)ct_clampf(ceilf((t1 - t0) / CT_FOG_STEP), 1.f, (float)CT_FOG_MAX_STEPS);
	const float h = (t1 - t0) / (float)n;
	const float jitter = ct_rand(rng);
	float od[CT_FOG_MAX_STEPS + 1];
	od[0] = 0.f;
	for (int i = 0; i < n; i++) {
		float p[3];
		VectorMA(ray->o, t0 + ((float)i + jitter) * h, ray->d, p);
		od[i + 1] = od[i] + ct_fog_density(p) * h;
	}
	const float total = od[n];
	if (!(total > 0.f))
		return;

	// one scatter point, in proportion to the density
	const float target = ct_rand(rng) * total;
	int i = 0;
	while (i < n - 1 && od[i + 1] < target)
		i++;
	const float span = od[i + 1] - od[i];
	const float f = (span > 0.f) ? ct_clampf((target - od[i]) / span, 0.f, 1.f) : 0.5f;
	float p[3];
	VectorMA(ray->o, t0 + ((float)i + f) * h, ray->d, p);

	float Ls[3], color[3], c[3];
	ct_fog_inscatter(w, rng, p, ray->d, Ls);
	ct_fog_color(p, color);
	const float k = total * expf(-scene.fog.sigma * target);
	for (int j = 0; j < 3; j++)
		c[j] = Ls[j] * color[j] * k;
	ct_add(L, T, c, bounce);

	if (scene.fog.extinction > 0.f) {
		const float tr = 1.f + (expf(-scene.fog.extinction * total) - 1.f) * scene.fog.opacity;
		VectorScale(T, tr, T);
	}
}

static void ct_path(ct_worker_t *w, ct_rng_t *rng, int px, int py, float L[3])
{
	const float sx = ((float)px + ct_rand(rng)) / (float)scene.width * 2.f - 1.f;
	const float sy = ((float)py + ct_rand(rng)) / (float)scene.height * 2.f - 1.f;

	float d[3];
	for (int k = 0; k < 3; k++)
		d[k] = scene.forward[k] + scene.right[k] * (sx * scene.tan_half_x) - scene.up[k] * (sy * scene.tan_half_y);
	ct_normalize(d);

	ct_ray_t ray;
	ct_ray_init(&ray, scene.origin, d, CT_FAR);

	float T[3] = { 1.f, 1.f, 1.f };
	VectorClear(L);

	bool count_emission = true;
	bool fog = scene.fog.on;    // the camera ray and what glass and mirrors carry on
	int mode = CT_RAY_CAMERA;
	int layers = 0;
	int bounce = 0;

	for (;;) {
		ct_hit_t hit;
		float c[3];

		const bool found = ct_trace(&ray, &hit, mode, w);
		if (fog)
			ct_fog_segment(w, rng, &ray, found ? hit.t : CT_FAR, T, L, bounce);

		if (!found) {
			// out of the map
			if (count_emission) {
				ct_env(ray.d, false, c);
				ct_add(L, T, c, bounce);
			}
			break;
		}

		const VboPrimitive *p = scene.prims + hit.prim;
		const uint32_t flags = scene.tris[hit.prim].flags;

		if (flags & CT_SKY) {
			if (count_emission || !(p->material_id & MATERIAL_FLAG_LIGHT)) {
				// diffuse bounces leave the sun out, as the light sample has it
				ct_env(ray.d, !count_emission, c);
				// the physical sky draws its own sun disk
				if (count_emission && scene.sun && scene.env_type != CT_ENV_PHYSICAL
					&& DotProduct(ray.d, scene.sun_dir) >= scene.sun_cos_half && scene.sun_solid_angle > 0.f)
					VectorMA(c, 1.f / scene.sun_solid_angle, scene.sun_irradiance, c);
				ct_add(L, T, c, bounce);
			}
			break;
		}

		ct_surface_t s;
		ct_fetch_surface(&ray, &hit, &s);

		if (flags & CT_TRANSPARENT) {
			if (++layers > CT_MAX_LAYERS)
				break;
			ct_transmit(&s, rng, &ray, T, &count_emission);
			// into water the GPU marches its own water haze instead
			const uint32_t kind = s.material_id & MATERIAL_KIND_MASK;
			if ((kind == MATERIAL_KIND_WATER || kind == MATERIAL_KIND_SLIME) && !s.backface
				&& DotProduct(ray.d, s.ng) < 0.f)
				fog = false;
			continue;
		}

		if (count_emission || !(s.material_id & MATERIAL_FLAG_LIGHT))
			ct_add(L, T, s.emissive, bounce);

		if (bounce >= scene.max_bounces)
			break;

		ct_direct(w, rng, &s, c);
		ct_add(L, T, c, bounce);

		if (!ct_bounce(&s, rng, &ray, T, &count_emission))
			break;
		if (!(T[0] > 0.f || T[1] > 0.f || T[2] > 0.f))
			break;
		fog = fog && count_emission;   // a near-mirror reflection keeps it

		mode = CT_RAY_BOUNCE;
		bounce++;
	}
}

/*
=============================================================================

WORKERS

=============================================================================
*/

static void ct_detect_cpu(void)
{
	memset(cpu_class, 0, sizeof(cpu_class));
	cpu_hybrid = false;
	cpu_cores_by_class[0] = cpu_cores_by_class[1] = 0;
	cpu_threads_by_class[0] = cpu_threads_by_class[1] = 0;

#ifdef _WIN32
	ULONG len = 0;
	GetSystemCpuSetInformation(NULL, 0, &len, GetCurrentProcess(), 0);
	if (!len)
		return;

	byte *buf = Z_Malloc(len);
	if (GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)buf, len, &len, GetCurrentProcess(), 0)) {
		int max_class = 0, min_class = 255;
		for (ULONG off = 0; off < len;) {
			const SYSTEM_CPU_SET_INFORMATION *info = (const SYSTEM_CPU_SET_INFORMATION *)(buf + off);
			if (info->Type == CpuSetInformation && info->CpuSet.Group == 0) {
				max_class = max(max_class, (int)info->CpuSet.EfficiencyClass);
				min_class = min(min_class, (int)info->CpuSet.EfficiencyClass);
			}
			off += info->Size;
		}

		cpu_hybrid = max_class > min_class;

		// one bit per physical core seen, per class
		byte seen[2][256 / 8] = { { 0 } };
		for (ULONG off = 0; off < len;) {
			const SYSTEM_CPU_SET_INFORMATION *info = (const SYSTEM_CPU_SET_INFORMATION *)(buf + off);
			if (info->Type == CpuSetInformation && info->CpuSet.Group == 0) {
				const int lp = info->CpuSet.LogicalProcessorIndex;
				const int cls = (cpu_hybrid && info->CpuSet.EfficiencyClass == max_class) ? 1 : 0;
				const int core = info->CpuSet.CoreIndex & 255;
				if (lp >= 0 && lp < (int)sizeof(cpu_class))
					cpu_class[lp] = (signed char)cls;
				cpu_threads_by_class[cls]++;
				if (!(seen[cls][core >> 3] & (1 << (core & 7)))) {
					seen[cls][core >> 3] |= 1 << (core & 7);
					cpu_cores_by_class[cls]++;
				}
			}
			off += info->Size;
		}
	}
	Z_Free(buf);
#endif
}

static int ct_current_class(void)
{
#ifdef _WIN32
	if (cpu_hybrid) {
		DWORD n = GetCurrentProcessorNumber();
		if (n < sizeof(cpu_class))
			return cpu_class[n];
	}
#endif
	return 1;
}

static void ct_render_tile(ct_worker_t *w, int tile, int pass)
{
	const int ts = run.tile_size;
	const int x0 = (tile % run.tiles_x) * ts, y0 = (tile / run.tiles_x) * ts;
	const int x1 = min(x0 + ts, scene.width), y1 = min(y0 + ts, scene.height);
	const int s0 = pass * run.spp_per_job;
	const int s1 = run.infinite ? s0 + run.spp_per_job : min(s0 + run.spp_per_job, scene.spp);
	const int cls = w->current_class;

	// sample-major, so a tile refines in place while its bracket is up
	for (int s = s0; s < s1 && !run.abort; s++) {
		for (int y = y0; y < y1; y++) {
			for (int x = x0; x < x1; x++) {
				const uint32_t pixel = (uint32_t)(y * scene.width + x);
				ct_rng_t rng;
				float L[3];
				ct_rng_seed(&rng, pixel, (uint32_t)s);
				ct_path(w, &rng, x, y, L);

				float *a = run.accum + (size_t)pixel * 4;
				a[0] += L[0];
				a[1] += L[1];
				a[2] += L[2];
				a[3] += 1.f;
			}
		}
		const uint64_t n = (uint64_t)(x1 - x0) * (uint64_t)(y1 - y0);
		w->samples += n;
		w->samples_by_class[cls] += n;
	}
}

static int SDLCALL ct_worker_main(void *data)
{
	ct_worker_t *w = data;
	int job;

	// infinite mode: num_jobs is INT_MAX, and pass counts on through the rounds
	while (!run.abort && (job = SDL_AtomicAdd(&run.next_job, 1)) < run.num_jobs && job >= 0) {
		const int pass = job / run.num_tiles;
		const int tile = run.tile_order[job % run.num_tiles];

		// a slow core may still be on this tile's previous pass (progressive
		// mode, or the round before in infinite mode)
		while (SDL_AtomicGet(&run.tile_passes[tile]) < pass && !run.abort)
			SDL_Delay(1);

		w->current_class = ct_current_class();
		w->current_tile = tile;
		ct_render_tile(w, tile, pass);
		w->current_tile = -1;
		w->jobs++;

		SDL_AtomicAdd(&run.tile_passes[tile], 1);
		SDL_AtomicAdd(&run.jobs_done, 1);
	}

	SDL_AtomicAdd(&run.threads_done, 1);
	return 0;
}

static double ct_seconds(uint64_t from, uint64_t to)
{
	return (double)(to - from) / (double)SDL_GetPerformanceFrequency();
}

static void ct_free_scene(void)
{
	Z_Free(scene.prims);
	Z_Free(scene.tris);
	Z_Free(scene.nodes);
	Z_Free(scene.lights);
	Z_Free(scene.fog.lights);
	Z_Free(run.accum);
	Z_Free(run.tile_order);
	Z_Free(run.tile_passes);
	Z_Free(gather_prims);
	Z_Free(gather_flags);
	Z_Free(physky_data);
	physky_data = NULL;
	scene.prims = NULL;
	scene.tris = NULL;
	scene.nodes = NULL;
	scene.lights = NULL;
	scene.fog.lights = NULL;
	scene.fog.num_lights = 0;
	run.accum = NULL;
	run.tile_order = NULL;
	run.tile_passes = NULL;
	gather_prims = NULL;
	gather_flags = NULL;
	gather_count = gather_alloc = 0;
}

static void ct_join_workers(void)
{
	for (int i = 0; i < run.num_threads; i++) {
		if (run.workers[i].thread) {
			SDL_WaitThread(run.workers[i].thread, NULL);
			run.workers[i].thread = NULL;
		}
	}
}

void vkpt_cpu_trace_stop(void)
{
	if (run.state == CT_IDLE)
		return;

	run.abort = 1;
	ct_join_workers();
	run.abort = 0;

	if (run.pic_pending)
		R_UnregisterImage(run.pic_pending);
	if (run.pic_shown)
		R_UnregisterImage(run.pic_shown);
	run.pic_pending = run.pic_shown = 0;

	ct_free_scene();
	run.state = CT_IDLE;
}

/*
=============================================================================

RESULTS AND DISPLAY

=============================================================================
*/

// The two result lines, "<what> <secs> s: rates" and the P/E-core split,
// from the workers' counters so far.
static void ct_write_results(const char *what, double secs, uint64_t *out_rays, uint64_t *out_samples)
{
	uint64_t rays = 0, samples = 0, by_class[2] = { 0, 0 };
	for (int i = 0; i < run.num_threads; i++) {
		rays += run.workers[i].rays;
		samples += run.workers[i].samples;
		by_class[0] += run.workers[i].samples_by_class[0];
		by_class[1] += run.workers[i].samples_by_class[1];
	}

	Q_snprintf(run.result[0], sizeof(run.result[0]), "%s %.2f s:  %.2f M samples/s,  %.1f M rays/s",
		what, secs, (double)samples / secs * 1e-6, (double)rays / secs * 1e-6);

	if (cpu_hybrid && samples) {
		Q_snprintf(run.result[1], sizeof(run.result[1]), "P-cores did %.0f%% of the work, E-cores %.0f%%  (%dP + %dE cores)",
			100.0 * (double)by_class[1] / (double)samples, 100.0 * (double)by_class[0] / (double)samples,
			cpu_cores_by_class[1], cpu_cores_by_class[0]);
	} else {
		run.result[1][0] = 0;
	}

	*out_rays = rays;
	*out_samples = samples;
}

// Infinite mode: a round is done when every tile has finished it.
static void ct_check_round(void)
{
	int passes = INT_MAX;
	for (int i = 0; i < run.num_tiles; i++)
		passes = min(passes, SDL_AtomicGet(&run.tile_passes[i]));
	const int rounds = passes / run.passes;
	if (rounds <= run.rounds_done)
		return;

	run.rounds_done = rounds;
	const double secs = max(1e-6, ct_seconds(run.start_ticks, SDL_GetPerformanceCounter()));
	uint64_t rays, samples;
	ct_write_results(va("%d spp in", rounds * scene.spp), secs, &rays, &samples);
	Com_Printf("cpu_bench: %s\n", run.result[0]);
}

static void ct_finish(void)
{
	ct_join_workers();
	run.end_ticks = SDL_GetPerformanceCounter();
	run.state = CT_DONE;

	const double secs = max(1e-6, ct_seconds(run.start_ticks, run.end_ticks));
	uint64_t rays, samples;
	ct_write_results("Done in", secs, &rays, &samples);

	Com_Printf("cpu_bench: %dx%d, %d spp, %d bounces, %d threads: %.2f s\n",
		scene.width, scene.height, scene.spp, scene.max_bounces, run.num_threads, secs);
	Com_Printf("cpu_bench: %.1f M samples (%.2f M/s), %.1f M rays (%.2f M/s)\n",
		(double)samples * 1e-6, (double)samples / secs * 1e-6, (double)rays * 1e-6, (double)rays / secs * 1e-6);
	if (run.result[1][0])
		Com_Printf("cpu_bench: %s\n", run.result[1]);

	char line[1024];
	size_t len = 0;
	line[0] = 0;
	for (int i = 0; i < run.num_threads && len < sizeof(line) - 16; i++)
		len += Q_snprintf(line + len, sizeof(line) - len, " %d", run.workers[i].jobs);
	Com_Printf("cpu_bench: tiles per thread:%s\n", line);
}

// exposure = exp2(tm_exposure_bias - 2) / adapted luminance
static uint32_t ct_tonemap(const float *a, float exposure)
{
	if (a[3] <= 0.f)
		return MakeColor(10, 10, 12, 255);

	const float inv = 1.f / a[3];
	float c[3] = { a[0] * inv, a[1] * inv, a[2] * inv };
	const float lum = ct_lum(c);
	if (!(lum > 0.f))
		return MakeColor(0, 0, 0, 255);

	// the Reinhard half of tone_mapping_apply.comp
	const float scaled = exposure * lum;
	const float wp2 = scene.white_point * scene.white_point;
	const float mapped = scaled * (1.f + scaled / wp2) / (1.f + scaled);
	const float k = mapped / lum;

	byte out[3];
	for (int i = 0; i < 3; i++) {
		float v = c[i] * k;
		v = (v > 0.f) ? min(v, 1.f) : 0.f;   // NaN lands on 0
		out[i] = linear_to_srgb[(int)(v * 4095.f + 0.5f)];
	}
	return MakeColor(out[0], out[1], out[2], 255);
}

static float ct_measure_adapted(void)
{
	// the log-average over what has been rendered so far, if the GPU gave none
	double sum = 0.0;
	int n = 0;
	const int count = scene.width * scene.height;
	for (int i = 0; i < count; i += 7) {
		const float *a = run.accum + (size_t)i * 4;
		if (a[3] <= 0.f)
			continue;
		float c[3] = { a[0] / a[3], a[1] / a[3], a[2] / a[3] };
		sum += log(max(1e-6f, ct_lum(c)));
		n++;
	}
	return n ? (float)exp(sum / n) : 1.f;
}

static void ct_bracket(uint32_t *rgba, int tile, uint32_t color)
{
	const int W = scene.width, H = scene.height, ts = run.tile_size;
	const int x0 = (tile % run.tiles_x) * ts, y0 = (tile / run.tiles_x) * ts;
	const int x1 = min(x0 + ts, W) - 1, y1 = min(y0 + ts, H) - 1;
	const int len = max(4, ts / 4);
	const int th = max(1, H / 540);

	for (int t = 0; t < th; t++) {
		for (int i = 0; i < len; i++) {
			const int xs[4] = { x0 + i, x1 - i, x0 + i, x1 - i };
			const int ys[4] = { y0 + t, y0 + t, y1 - t, y1 - t };
			const int xv[4] = { x0 + t, x1 - t, x0 + t, x1 - t };
			const int yv[4] = { y0 + i, y0 + i, y1 - i, y1 - i };
			for (int c = 0; c < 4; c++) {
				if (xs[c] >= 0 && xs[c] < W && ys[c] >= 0 && ys[c] < H)
					rgba[ys[c] * W + xs[c]] = color;
				if (xv[c] >= 0 && xv[c] < W && yv[c] >= 0 && yv[c] < H)
					rgba[yv[c] * W + xv[c]] = color;
			}
		}
	}
}

static void ct_update_picture(void)
{
	const int W = scene.width, H = scene.height;
	uint32_t *rgba = Z_Malloc((size_t)W * H * 4);

	float adapted = scene.adapted_luminance;
	if (!(adapted > 0.f))
		adapted = ct_measure_adapted();

	const float exposure = exp2f(scene.exposure_bias - 2.f) / adapted;
	for (int i = 0; i < W * H; i++)
		rgba[i] = ct_tonemap(run.accum + (size_t)i * 4, exposure);

	if (run.state == CT_RUNNING) {
		for (int i = 0; i < run.num_threads; i++) {
			const int tile = run.workers[i].current_tile;
			if (tile < 0 || tile >= run.num_tiles)
				continue;
			const bool p_core = run.workers[i].current_class != 0;
			ct_bracket(rgba, tile, (cpu_hybrid && !p_core) ? MakeColor(60, 200, 255, 255) : MakeColor(255, 150, 40, 255));
		}
	}

	// R_RegisterRawImage keeps the buffer. Drawn from the next frame, once
	// R_BeginFrame has uploaded it.
	if (run.pic_pending)
		R_UnregisterImage(run.pic_pending);
	run.pic_pending = R_RegisterRawImage(va("**cpu_bench_%d**", run.pic_seq++), W, H, (byte *)rgba, IT_SPRITE, IF_SRGB | IF_NO_MIPMAPS);
}

bool vkpt_cpu_trace_active(void)
{
	return run.state == CT_RUNNING || run.state == CT_DONE;
}

bool vkpt_cpu_trace_capture_pending(void)
{
	return run.state == CT_PENDING;
}

void vkpt_cpu_trace_draw(void)
{
	if (run.state == CT_RUNNING && SDL_AtomicGet(&run.threads_done) >= run.num_threads) {
		ct_finish();
		run.last_update_ticks = 0;
	}
	if (run.state == CT_RUNNING && run.infinite)
		ct_check_round();

	// last frame's picture has been uploaded by now
	if (run.pic_pending) {
		if (run.pic_shown)
			R_UnregisterImage(run.pic_shown);
		run.pic_shown = run.pic_pending;
		run.pic_pending = 0;
	}

	const uint64_t now = SDL_GetPerformanceCounter();
	if (run.last_update_ticks == 0 || (run.state == CT_RUNNING && ct_seconds(run.last_update_ticks, now) > 0.25)) {
		ct_update_picture();
		run.last_update_ticks = now;
	}

	const float scale = 1.f / (float)max(1, (int)((float)r_config.height / 540.f + 0.5f));
	const int vw = (int)((float)r_config.width * scale + 0.5f);
	const int vh = (int)((float)r_config.height * scale + 0.5f);

	vkpt_draw_push_state(scale);

	R_DrawFill32_RTX(0, 0, vw, vh, MakeColor(0, 0, 0, 255));
	if (run.pic_shown)
		R_DrawStretchPic_RTX(0, 0, vw, vh, run.pic_shown);

	if (!run.font)
		run.font = R_RegisterFont("conchars");

	const double secs = ct_seconds(run.start_ticks, run.state == CT_DONE ? run.end_ticks : now);
	uint64_t rays = 0, samples = 0;
	for (int i = 0; i < run.num_threads; i++) {
		rays += run.workers[i].rays;
		samples += run.workers[i].samples;
	}
	const uint64_t total = (uint64_t)scene.width * scene.height * scene.spp;   // one round's worth

	// title, settings, then progress + help, or the results + help
	const bool round_result = run.state == CT_RUNNING && run.rounds_done > 0;
	int lines = 3;
	if (run.state == CT_RUNNING)
		lines += round_result ? (run.result[1][0] ? 3 : 2) : 1;
	else
		lines += run.result[1][0] ? 2 : 1;

	char line[256];
	int y = 8;
	R_DrawFill32_RTX(4, 4, 8 * 84, lines * 10 + 4, MakeColor(0, 0, 0, 160));

	R_DrawString_RTX(8, y, UI_ALTCOLOR, MAX_STRING_CHARS, "CPU PATH TRACER", run.font);
	y += 10;
	Q_snprintf(line, sizeof(line), "%dx%d  %d spp%s  %d bounces  %d threads  %u k tris  %d lights%s",
		scene.width, scene.height, scene.spp, run.infinite ? " a round" : "", scene.max_bounces, run.num_threads,
		scene.num_prims / 1000, scene.num_lights, scene.fog.on ? "  fog" : "");
	R_DrawString_RTX(8, y, 0, MAX_STRING_CHARS, line, run.font);
	y += 10;

	if (run.state == CT_RUNNING) {
		const double rate = secs > 0.0 ? (double)samples / secs * 1e-6 : 0.0;
		const double ray_rate = secs > 0.0 ? (double)rays / secs * 1e-6 : 0.0;
		if (run.infinite) {
			// how far into the round after the last finished one
			const double done = (double)total * run.rounds_done;
			const double frac = total ? ct_clampf((float)(((double)samples - done) / (double)total), 0.f, 0.999f) : 0.0;
			Q_snprintf(line, sizeof(line), "%d spp  +%4.1f%%  %6.1f s   %.2f M samples/s  %.1f M rays/s",
				run.rounds_done * scene.spp, 100.0 * frac, secs, rate, ray_rate);
		} else {
			Q_snprintf(line, sizeof(line), "%5.1f%%  %6.1f s   %.2f M samples/s  %.1f M rays/s",
				total ? 100.0 * (double)samples / (double)total : 0.0, secs, rate, ray_rate);
		}
		R_DrawString_RTX(8, y, 0, MAX_STRING_CHARS, line, run.font);
		y += 10;
		if (round_result) {
			R_DrawString_RTX(8, y, UI_ALTCOLOR, MAX_STRING_CHARS, run.result[0], run.font);
			y += 10;
			if (run.result[1][0]) {
				R_DrawString_RTX(8, y, 0, MAX_STRING_CHARS, run.result[1], run.font);
				y += 10;
			}
		}
		R_DrawString_RTX(8, y, 0, MAX_STRING_CHARS, cpu_hybrid ? "orange = P-core   blue = E-core   'cpu_bench' to stop" : "'cpu_bench' to stop", run.font);
	} else {
		R_DrawString_RTX(8, y, UI_ALTCOLOR, MAX_STRING_CHARS, run.result[0], run.font);
		y += 10;
		if (run.result[1][0]) {
			R_DrawString_RTX(8, y, 0, MAX_STRING_CHARS, run.result[1], run.font);
			y += 10;
		}
		R_DrawString_RTX(8, y, 0, MAX_STRING_CHARS, "'cpu_bench' to go back", run.font);
	}

	vkpt_draw_pop_state();

	// leave the cores to the workers - the game frame is only drawing this
	if (run.state == CT_RUNNING)
		Sys_Sleep(10);
}

/*
=============================================================================

START

=============================================================================
*/

// The sun's colour as the GPU gets it (physical_sky.comp): the sky cube summed
// over the sun disk, times each texel's solid angle. Only its hue is taken
// from here - the brightness is the GPU's own readback, which also knows when
// clouds or terrain cover the sun.
static void ct_sun_color_from_sky(float sun_luminance)
{
	const int w = physky_width, h = physky_height;
	double sum[3] = { 0.0, 0.0, 0.0 };

	for (int face = 0; face < 6; face++) {
		const uint16_t *texels = physky_data + (size_t)face * w * h * 4;
		for (int y = 0; y < h; y++) {
			const float v = ((float)y + 0.5f) / (float)h * 2.f - 1.f;
			for (int x = 0; x < w; x++) {
				const float u = ((float)x + 0.5f) / (float)w * 2.f - 1.f;
				float c[3];
				ct_cube_dir(face, u, v, c);
				const float r[3] = { c[0], c[2], c[1] };   // undo the .xzy
				const float len2 = DotProduct(r, r);
				const float d = DotProduct(r, scene.sun_dir_envmap);
				if (d <= 0.f || d * d < scene.sun_cos_half * scene.sun_cos_half * len2)
					continue;
				const float k = 1.f + u * u + v * v;
				const double solid_angle = 4.0 / ((double)w * h * k * sqrtf(k));
				const uint16_t *t = texels + ((size_t)y * w + x) * 4;
				for (int i = 0; i < 3; i++)
					sum[i] += half_to_float[t[i]] * solid_angle;
			}
		}
	}

	float color[3] = { (float)sum[0], (float)sum[1], (float)sum[2] };
	const float l = ct_lum(color);
	if (l > 0.f)
		VectorScale(color, sun_luminance / l, scene.sun_irradiance);
}

// physical_sky.comp's sky_color, which lights the fog: the plain average of
// every 32nd texel each way, the sun disk left out, times pt_env_scale
static void ct_sky_color_from_cube(float cos_half_sun, float out[3])
{
	const int w = physky_width, h = physky_height;
	double sum[3] = { 0.0, 0.0, 0.0 };
	int n = 0;

	for (int face = 0; face < 6; face++) {
		const uint16_t *texels = physky_data + (size_t)face * w * h * 4;
		for (int y = 15; y < h; y += 32) {
			const float v = ((float)y + 0.5f) / (float)h * 2.f - 1.f;
			for (int x = 15; x < w; x += 32) {
				const float u = ((float)x + 0.5f) / (float)w * 2.f - 1.f;
				float c[3];
				ct_cube_dir(face, u, v, c);
				float r[3] = { c[0], c[2], c[1] };   // undo the .xzy
				ct_normalize(r);
				if (DotProduct(r, scene.sun_dir_envmap) > cos_half_sun - 0.000875f)
					continue;
				const uint16_t *t = texels + ((size_t)y * w + x) * 4;
				for (int i = 0; i < 3; i++)
					sum[i] += half_to_float[t[i]];
				n++;
			}
		}
	}

	for (int i = 0; i < 3; i++)
		out[i] = n ? (float)(sum[i] / n) * scene.env_scale : 0.f;
}

// The fog as this frame's UBO has it; see FOG.
static void ct_capture_fog(const bsp_mesh_t *wm, const sun_light_t *sun, const QVKUniformBuffer_t *ubo)
{
	memset(&scene.fog, 0, sizeof(scene.fog));

	// whether R_RenderFrame_RTX runs the march at all. Under water the GPU
	// marches its water haze instead, which this leaves out.
	if (!cvar_cpu_bench_fog->integer || !vkpt_god_rays_enabled(sun) || ubo->medium != MEDIUM_NONE)
		return;

	const bool map_fog = ubo->fog_enable != 0;
	scene.fog.on = true;
	scene.fog.map_fog = map_fog;
	scene.fog.white = ubo->environment_type == ENVIRONMENT_DYNAMIC;
	scene.fog.density = ubo->fog_density;
	scene.fog.hf_density = ubo->fog_hf_density;
	scene.fog.hf_falloff = ubo->fog_hf_falloff;
	scene.fog.hf_start_z = ubo->fog_hf_start_z;
	scene.fog.hf_end_z = ubo->fog_hf_end_z;
	scene.fog.density_max = max(0.f, ubo->pt_fog_density_max);
	VectorSet(scene.fog.color, ubo->fog_color_r, ubo->fog_color_g, ubo->fog_color_b);
	VectorSet(scene.fog.hf_top, ubo->fog_hf_start_r, ubo->fog_hf_start_g, ubo->fog_hf_start_b);
	VectorSet(scene.fog.hf_bottom, ubo->fog_hf_end_r, ubo->fog_hf_end_g, ubo->fog_hf_end_b);
	scene.fog.hf_color_start_z = ubo->fog_hf_color_start_z;
	scene.fog.hf_color_end_z = ubo->fog_hf_color_end_z;
	scene.fog.color_layers = ubo->fog_color_layers != 0;
	for (int k = 0; k < 3; k++) {
		scene.fog.world_center[k] = ubo->world_center[k];
		scene.fog.world_half[k] = ubo->world_size[k] * 0.5f;
		scene.fog.world_half_inv[k] = ubo->world_half_size_inv[k];
	}

	// god_rays.comp's sun term and eccentricity; fog_eccentricity() for the lights
	scene.fog.sun_scale = (map_fog ? ubo->pt_fog_scale_sun : ubo->god_rays_intensity) * 0.0001f;
	float g = (map_fog && ubo->pt_fog_eccentricity >= 0.f) ? ubo->pt_fog_eccentricity : ubo->god_rays_eccentricity;
	scene.fog.g_sun = ct_clampf(g, -0.95f, 0.95f);
	g = (ubo->pt_fog_eccentricity >= 0.f) ? ubo->pt_fog_eccentricity : ubo->god_rays_eccentricity;
	scene.fog.g_light = ct_clampf(g, -0.95f, 0.95f);
	scene.fog.ambient = max(0.f, ubo->pt_fog_ambient);

	// pt_fog_extinction 0 is the legacy additive fog: a token extinction of
	// the in-scatter, and nothing behind it dimmed
	const bool medium = map_fog && ubo->pt_fog_extinction > 0.f;
	scene.fog.sigma = medium ? ubo->pt_fog_extinction : 0.0001f;
	scene.fog.extinction = medium ? ubo->pt_fog_extinction : 0.f;
	scene.fog.opacity = ct_clampf(ubo->pt_fog_opacity, 0.f, 1.f);

	// the local lights and the sky are cl_fog's alone
	if (!map_fog)
		return;

	// every fog sample draws from the CAMERA's cluster: its light list, then
	// its fog-only list, whose entries index the top of the GPU's light buffer
	const int cluster = ubo->fog_camera_cluster;
	if (cluster >= 0 && cluster < wm->num_clusters && wm->cluster_light_offsets) {
		const int reg_begin = wm->cluster_light_offsets[cluster];
		const int reg_count = wm->cluster_light_offsets[cluster + 1] - reg_begin;
		int fog_begin = 0, fog_count = 0;
		if (scene.num_fog_lights > 0 && wm->fog_cluster_light_offsets) {
			fog_begin = wm->fog_cluster_light_offsets[cluster];
			fog_count = wm->fog_cluster_light_offsets[cluster + 1] - fog_begin;
		}

		scene.fog.lights = Z_Malloc(sizeof(int) * max(1, reg_count + fog_count));
		const int fog_base = MAX_LIGHT_POLYS - scene.num_fog_lights;
		for (int i = 0; i < reg_count; i++) {
			const int li = wm->cluster_lights[reg_begin + i];
			scene.fog.lights[scene.fog.num_lights++] = (li >= 0 && li < scene.num_lights) ? li : -1;
		}
		for (int i = 0; i < fog_count; i++) {
			const int li = wm->fog_cluster_lights[fog_begin + i] - fog_base;
			scene.fog.lights[scene.fog.num_lights++] = (li >= 0 && li < scene.num_fog_lights) ? scene.num_lights + li : -1;
		}
	}
	scene.fog.light_samples = (int)ct_clampf(ubo->pt_fog_light_samples, 1.f, 32.f);
	scene.fog.firefly = ubo->pt_fog_firefly;

	// fog_sky_inscatter: none under the physical sky while its fog is the sun alone
	vec3_t sky = { 0.f, 0.f, 0.f };
	if (ubo->environment_type == ENVIRONMENT_DYNAMIC && ubo->pt_fog_sky_sun_only == 0.f) {
		if (physky_data)
			ct_sky_color_from_cube(cosf(sun->angular_size_rad * 0.5f), sky);
		else
			VectorCopy(scene.env_flat, sky);
	} else if (ubo->environment_type == ENVIRONMENT_STATIC) {
		VectorSet(sky, ubo->fog_sky_r, ubo->fog_sky_g, ubo->fog_sky_b);
	}
	VectorScale(sky, M_PI * ubo->pt_fog_scale_skybox * CT_FOG_ISO, scene.fog.sky_term);

	if (ct_lum(scene.fog.sky_term) <= 0.f) {
		scene.fog.sky_mode = CT_FOG_SKY_NONE;
	} else if (ubo->fog_sky_trace == 0) {
		scene.fog.sky_mode = CT_FOG_SKY_FADE;
		scene.fog.sky_fade = ubo->fog_sky_fade;
	} else if (ubo->sky_exposure_ready && ubo->pt_fog_sky_exposure > 0.f) {
		scene.fog.sky_mode = CT_FOG_SKY_EXPOSURE;
		scene.fog.exposure_curve = ct_clampf(ubo->pt_fog_sky_exposure_curve, 1.f, 8.f);
	} else {
		scene.fog.sky_mode = CT_FOG_SKY_UP;
		scene.fog.sky_soften = ubo->pt_fog_sky_soften;
	}
}

void vkpt_cpu_trace_capture(const refdef_t *fd, bsp_t *bsp, const sun_light_t *sun, const vec3_t sky_matrix[3],
	float env_scale, float adapted_luminance, float sun_luminance, float sky_luminance, const QVKUniformBuffer_t *ubo)
{
	if (run.state != CT_PENDING)
		return;

	const bsp_mesh_t *wm = &vkpt_refdef.bsp_mesh_world;
	if (!vkpt_refdef.bsp_mesh_world_loaded || !wm->primitives) {
		Com_Printf("cpu_bench: no map loaded\n");
		run.state = CT_IDLE;
		return;
	}

	const uint64_t t0 = SDL_GetPerformanceCounter();

	for (int i = 0; i < MAX_LIGHTSTYLES; i++)
		scene.light_styles[i] = fd->lightstyles ? ct_clampf(fd->lightstyles[i].white, 0.f, 2.f) : 1.f;

	// geometry
	gather_count = 0;
	ct_gather_world(wm, &wm->geom_opaque, 0);
	ct_gather_world(wm, &wm->geom_masked, CT_MASKED);
	ct_gather_world(wm, &wm->geom_transparent, CT_TRANSPARENT);
	ct_gather_world(wm, &wm->geom_sky, CT_SKY);
	ct_gather_world(wm, &wm->geom_custom_sky, CT_SKY);
	for (int i = 0; i < fd->num_entities; i++) {
		const entity_t *e = fd->entities + i;
		if (e->model & 0x80000000)
			ct_gather_brush_entity(wm, bsp, e);
		else
			ct_gather_alias_entity(bsp, e);
	}

	if (!gather_count) {
		Com_Printf("cpu_bench: nothing to render\n");
		ct_free_scene();
		run.state = CT_IDLE;
		return;
	}

	ct_build_bvh(gather_prims, gather_flags, gather_count);
	Z_Free(gather_prims);
	Z_Free(gather_flags);
	gather_prims = NULL;
	gather_flags = NULL;
	gather_count = gather_alloc = 0;

	ct_capture_lights(wm);

	// camera
	VectorCopy(fd->vieworg, scene.origin);
	AngleVectors(fd->viewangles, scene.forward, scene.right, scene.up);
	scene.tan_half_x = tanf(DEG2RAD(fd->fov_x) * 0.5f);
	scene.tan_half_y = tanf(DEG2RAD(fd->fov_y) * 0.5f);

	const float res_scale = ct_clampf(cvar_cpu_bench_scale->value, 0.05f, 2.f);
	scene.width = max(16, (int)((float)r_config.width * res_scale));
	scene.height = max(16, (int)((float)r_config.height * res_scale));

	// sun: the GPU's own colour is integrated from the sky on the GPU and only
	// its luminance comes back, so the CPU sun takes that luminance in the
	// hue of the preset's sun colour
	scene.sun = sun->visible && sun_luminance > 0.f;
	if (scene.sun) {
		VectorCopy(sun->direction, scene.sun_dir);
		ct_normalize(scene.sun_dir);
		if (scene.sun_dir[2] >= 0.99f) {
			VectorSet(scene.sun_tangent, 1.f, 0.f, 0.f);
			VectorSet(scene.sun_bitangent, 0.f, 1.f, 0.f);
		} else {
			const vec3_t up = { 0.f, 0.f, 1.f };
			CrossProduct(scene.sun_dir, up, scene.sun_tangent);
			ct_normalize(scene.sun_tangent);
			CrossProduct(scene.sun_dir, scene.sun_tangent, scene.sun_bitangent);
			ct_normalize(scene.sun_bitangent);
		}
		scene.sun_tan_half = tanf(sun->angular_size_rad * 0.5f);
		scene.sun_cos_half = cosf(sun->angular_size_rad * 0.5f);
		scene.sun_solid_angle = 2.f * M_PI * (float)(1.0 - cos(sun->angular_size_rad * 0.5));
		const float hue = ct_lum(sun->color);
		if (hue > 0.f)
			VectorScale(sun->color, sun_luminance / hue, scene.sun_irradiance);
		else
			VectorSet(scene.sun_irradiance, sun_luminance, sun_luminance, sun_luminance);
	}

	// environment
	scene.env_scale = env_scale;
	for (int i = 0; i < 3; i++)
		VectorCopy(sky_matrix[i], scene.sky_matrix[i]);
	VectorCopy(sun->direction_envmap, scene.sun_dir_envmap);
	if (sun->use_physical_sky && physky_data) {
		scene.env_type = CT_ENV_PHYSICAL;
		if (scene.sun)
			ct_sun_color_from_sky(sun_luminance);
	} else if (sun->use_physical_sky) {
		// no copy of the GPU's sky: one flat colour at its measured luminance
		static const vec3_t sky_hue = { 0.55f, 0.72f, 1.f };
		const float l = ct_lum(sky_hue);
		scene.env_type = CT_ENV_FLAT;
		VectorScale(sky_hue, sky_luminance / l, scene.env_flat);
	} else {
		scene.env_type = envmap_data ? CT_ENV_CUBE : CT_ENV_NONE;
	}

	ct_capture_fog(wm, sun, ubo);

	scene.bump_scale = Cvar_VariableValue("pt_bump_scale");
	scene.adapted_luminance = adapted_luminance;
	scene.exposure_bias = Cvar_VariableValue("tm_exposure_bias");
	scene.white_point = max(0.1f, Cvar_VariableValue("tm_white_point"));
	scene.max_indirect = (adapted_luminance > 0.f && cvar_cpu_bench_clamp->value > 0.f)
		? cvar_cpu_bench_clamp->value * adapted_luminance : 0.f;

	scene.spp = max(1, cvar_cpu_bench_spp->integer);
	scene.max_bounces = max(0, min(8, cvar_cpu_bench_bounces->integer));

	// tiles, handed out from the centre of the screen outwards
	run.tile_size = max(8, min(512, cvar_cpu_bench_tile->integer));
	run.tiles_x = (scene.width + run.tile_size - 1) / run.tile_size;
	run.tiles_y = (scene.height + run.tile_size - 1) / run.tile_size;
	run.num_tiles = run.tiles_x * run.tiles_y;
	run.spp_per_job = cvar_cpu_bench_progressive->integer ? 1 : scene.spp;
	run.passes = (scene.spp + run.spp_per_job - 1) / run.spp_per_job;
	run.infinite = cvar_cpu_bench_infinite->integer != 0;
	run.num_jobs = run.infinite ? INT_MAX : run.num_tiles * run.passes;
	run.rounds_done = 0;

	run.tile_order = Z_Malloc(sizeof(int) * run.num_tiles);
	float *dist = Z_Malloc(sizeof(float) * run.num_tiles);
	for (int i = 0; i < run.num_tiles; i++) {
		const float dx = ((float)(i % run.tiles_x) + 0.5f) * run.tile_size - scene.width * 0.5f;
		const float dy = ((float)(i / run.tiles_x) + 0.5f) * run.tile_size - scene.height * 0.5f;
		dist[i] = dx * dx + dy * dy;
		run.tile_order[i] = i;
	}
	for (int i = 1; i < run.num_tiles; i++) {
		const int t = run.tile_order[i];
		int j = i;
		while (j > 0 && dist[run.tile_order[j - 1]] > dist[t]) {
			run.tile_order[j] = run.tile_order[j - 1];
			j--;
		}
		run.tile_order[j] = t;
	}
	Z_Free(dist);

	run.tile_passes = Z_Mallocz(sizeof(SDL_atomic_t) * run.num_tiles);
	run.accum = Z_Mallocz(sizeof(float) * 4 * (size_t)scene.width * scene.height);

	const uint64_t t1 = SDL_GetPerformanceCounter();
	run.build_seconds = ct_seconds(t0, t1);

	ct_detect_cpu();

	int threads = cvar_cpu_bench_threads->integer;
	if (threads <= 0)
		threads = SDL_GetCPUCount();
	run.num_threads = max(1, min(CT_MAX_THREADS, threads));

	Com_Printf("cpu_bench: %u triangles, %u BVH nodes, %d lights, built in %.2f s\n",
		scene.num_prims, scene.num_nodes, scene.num_lights, run.build_seconds);
	Com_Printf("cpu_bench: %dx%d, %d spp%s, %d bounces, %d tiles of %d px, %d threads%s\n",
		scene.width, scene.height, scene.spp, run.infinite ? " a round, no end" : "", scene.max_bounces,
		run.num_tiles, run.tile_size, run.num_threads,
		cpu_hybrid ? va(" (%dP + %dE cores)", cpu_cores_by_class[1], cpu_cores_by_class[0]) : "");
	if (scene.fog.on) {
		static const char *const sky_modes[] = { "none", "fade", "up ray", "exposure rays" };
		Com_Printf("cpu_bench: fog: %s, sun x%g, %d lights from cluster %d, sky %s\n",
			scene.fog.map_fog ? "map medium" : "god rays only", scene.fog.sun_scale * 10000.f,
			scene.fog.num_lights, ubo->fog_camera_cluster, sky_modes[scene.fog.sky_mode]);
	}

	memset(run.workers, 0, sizeof(run.workers));
	SDL_AtomicSet(&run.next_job, 0);
	SDL_AtomicSet(&run.jobs_done, 0);
	SDL_AtomicSet(&run.threads_done, 0);
	run.abort = 0;
	run.result[0][0] = run.result[1][0] = 0;
	run.last_update_ticks = 0;
	run.state = CT_RUNNING;
	run.start_ticks = SDL_GetPerformanceCounter();

	for (int i = 0; i < run.num_threads; i++) {
		ct_worker_t *w = run.workers + i;
		w->current_tile = -1;
		w->thread = SDL_CreateThread(ct_worker_main, va("cpu_bench %d", i), w);
		if (!w->thread) {
			// the ones that did start take all the jobs
			Com_WPrintf("cpu_bench: couldn't start thread %d: %s\n", i, SDL_GetError());
			run.num_threads = i;
			break;
		}
	}

	if (!run.num_threads) {
		vkpt_cpu_trace_stop();
		return;
	}
}

// `cpu_bench` toggles, `cpu_bench 1` starts (again, if one is up), `cpu_bench 0`
// stops. The thread count is cpu_bench_threads only.
static void ct_command(void)
{
	const bool busy = vkpt_cpu_trace_active() || run.state == CT_PENDING;
	const bool start = (Cmd_Argc() > 1) ? atoi(Cmd_Argv(1)) != 0 : !busy;

	vkpt_cpu_trace_stop();
	if (!start)
		return;

	// Commands run between frames, so the GPU's sky cube can be copied now;
	// the capture decides whether this map uses it.
	Z_Free(physky_data);
	physky_data = vkpt_physical_sky_read_envmap(&physky_width, &physky_height);

	run.state = CT_PENDING;   // R_RenderFrame_RTX takes the scene on its next world frame
	run.pic_shown = run.pic_pending = 0;
}

void vkpt_cpu_trace_init(void)
{
	cvar_cpu_bench_spp = Cvar_Get("cpu_bench_spp", "64", CVAR_ARCHIVE);
	cvar_cpu_bench_scale = Cvar_Get("cpu_bench_scale", "0.5", CVAR_ARCHIVE);
	cvar_cpu_bench_bounces = Cvar_Get("cpu_bench_bounces", "2", CVAR_ARCHIVE);
	cvar_cpu_bench_threads = Cvar_Get("cpu_bench_threads", "0", CVAR_ARCHIVE);
	cvar_cpu_bench_tile = Cvar_Get("cpu_bench_tile", "64", CVAR_ARCHIVE);
	cvar_cpu_bench_progressive = Cvar_Get("cpu_bench_progressive", "0", CVAR_ARCHIVE);
	cvar_cpu_bench_clamp = Cvar_Get("cpu_bench_clamp", "64", CVAR_ARCHIVE);
	cvar_cpu_bench_fog = Cvar_Get("cpu_bench_fog", "1", CVAR_ARCHIVE);
	cvar_cpu_bench_infinite = Cvar_Get("cpu_bench_infinite", "0", CVAR_ARCHIVE);

	ct_init_tables();
	Cmd_AddCommand("cpu_bench", ct_command);
}

void vkpt_cpu_trace_shutdown(void)
{
	vkpt_cpu_trace_stop();
	Cmd_RemoveCommand("cpu_bench");
	Z_Free(envmap_data);
	envmap_data = NULL;
	envmap_width = envmap_height = 0;
	Z_Free(physky_data);
	physky_data = NULL;
	run.font = 0;
}
