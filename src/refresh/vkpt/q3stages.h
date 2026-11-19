/*
Copyright (C) 2026 Matt Stewart

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

#ifndef VKPT_Q3STAGES_H
#define VKPT_Q3STAGES_H

#include "vkpt.h"
#include "material.h"
#include "common/q3shader.h"

/* QUAKE III SHADER STAGES, DRAWN.

   material.c reads a Quake III shader for the facts a material needs: one
   colour image, one glow image, a kind. That is all a still, single-image
   surface has. A shader that ANIMATES - scrolls, rotates, warps, flips frames,
   pulses - or that builds its look from several images (a pulse scrolling
   along a trim over a glow over the base) loses all of that to one picture.

   So such a shader is also given a PROGRAM: its stages, with their images,
   blend functions, colour generators and texture-coordinate modifiers. The
   time-dependent parts are evaluated here every frame (vkpt_q3_upload) and the
   path tracer composites the stages per hit (shader/q3_stages.glsl) the way
   Quake III's renderer blended them into the framebuffer - three times over,
   with the light at the surface and what is behind it standing in as 0 and 1,
   which pulls the result apart into what the surface REFLECTS (albedo), what it
   EMITS, and what it lets THROUGH.

   The material's ordinary images stay what they were, so anything that does
   not run the program - bounce rays, the light list, other renderers - still
   sees the surface's colour and glow. */

// Builds (or rebuilds) `mat`'s program when its shader wants one, or clears it.
// own_base / own_emissive: the .mat entry names that image itself, and the
// stage that supplied it draws the entry's image instead.
void vkpt_q3_build_program(pbr_material_t *mat, const q3shader_t *q3, imagetype_t type, imageflags_t flags,
                           bool own_base, bool own_emissive);

// True if `q3` gets a program (vkpt_q3_build_program), for apply_q3_shader's
// choice of material kind.
bool vkpt_q3_wants_program(const q3shader_t *q3);

// True if `mat`'s program has a reflection map that pt_q3_env_shine turns into
// gloss (and *metal if the shader reads as metal), for the material's defaults.
bool vkpt_q3_program_shines(const pbr_material_t *mat, bool *metal);

// Keeps the program's images registered with the material.
void vkpt_q3_touch_program(const pbr_material_t *mat);

// Evaluates every live program at `time` (seconds) into `dst` (LightBuffer
// q3_stages). Call before the material table is filled. `time_prev` is last
// frame's (the UBO's): a moving picture's motion vectors (q3stages.c MOTION).
void vkpt_q3_upload(vec4 *dst, float time, float time_prev);

// Where `mat`'s program landed in this frame's upload: its first vec4 + 1, or
// 0 when it has none - for word 9 of the material table.
uint32_t vkpt_q3_program_offset(const pbr_material_t *mat);

// 'mat print': the program stage by stage - image or NO IMAGE, blend, colour, flags.
void vkpt_q3_print_program(const pbr_material_t *mat);

/* A PROGRAM'S GLOW AS A LIGHT (light_poly_t q3_glow). bsp_mesh.c cuts the light
   polys and records each one's texture coordinates; copy_light (vertex_buffer.c)
   asks for its colour every frame. */

// What `mat`'s program emits - its stages blended with no light at the surface,
// as q3_stages.glsl draws it - averaged over the triangle with texture
// coordinates `uv` (three u,v pairs, 1 = one repeat) on an n x n grid of
// `samples`, at the shader clock `time` (seconds), into `out`; and what its
// stage<N>_emissive_factor stages glow, which the material's emissive_factor
// does not scale, into `out_stages`. 0 while the program's images are not in
// memory yet (or there is no program), 1 when the answer changes with time, 2
// when it never does.
int vkpt_q3_light_glow(const pbr_material_t *mat, const float uv[6], int samples, float time, vec3_t out,
                       vec3_t out_stages);

// Changes whenever `mat`'s program is rebuilt or what it emits could change
// for another reason (pt_q3_env_shine); 0 for no program. A still glow is
// worked out once per key.
uint32_t vkpt_q3_program_key(const pbr_material_t *mat);

// True if `mat`'s program glow moves over the surface (scrolls, spins, flips
// frames) or is more than one glow: one image's bright rectangle does not say
// where the light is, so bsp_mesh.c makes the whole face the light.
bool vkpt_q3_program_glow_moves(const pbr_material_t *mat);

// Frees the images the lights read (map change).
void vkpt_q3_light_cache_clear(void);

// 'mat reload': re-reads every image a stage draws from (programs, sky cloud
// layers, the quad shell) whose file was saved since it was loaded. Returns
// how many were.
int vkpt_q3_refresh_stage_images(void);

/* STAGE SETTINGS: .mat "stage<N>_<setting> <value>", and the console's
   'mat stage<N> <setting> <value>'. One stage of a Quake III shader set apart
   from the rest of it:
   - stage<N>_map <image>: the image it draws, in place of the script's;
     stage<N>_map off: the stage is not drawn at all (none / 0 only take the
     entry's image away, and the script's comes back);
   - its own surface where it shows (Matt, 2026-11-16: q3dm2's
     q1metal7_99spot is bricks over a pool of grue, and the whole floor took
     the grue's slippery metal) - stage<N>_roughness_override, _metalness_factor,
     _specular_factor, _bump_scale, _texture_roughness, _texture_metallic,
     _texture_normals. The maps are read through the stage's own texture
     coordinates (its tcMods), the values mean what the material's do, and
     where the stage covers the surface they replace the material's
     (q3_stages.glsl STAGE SETTINGS);
   - stage<N>_emissive_factor: what shows of the stage, fully lit, glows that
     much (Matt, 2026-11-18: medal_impressive's medal alone at 5, the
     material's emissive_factor 0). ADDED to the material's own glow, and not
     scaled by its emissive_factor - only by default_radiance (or the bsp
     radiance), light_factor for the light it casts, and its light style.
     With any stage stating one, the entry's glow stage (texture_emissive
     naming a stage's picture) glows only by a factor of its own.
   Kept apart from pbr_material_t (eight stages of paths would be a lot of
   memory for 8192 materials), keyed by material name, in three scopes: the
   global .mat files, the current map's, and what was typed in the console
   since the last 'mat reload' - which wins, then the map's. An instance
   (name#hash) takes what it does not state from the material it is one of. */
enum {
    // the images (paths)
    Q3SS_MAP,
    Q3SS_TEXTURE_ROUGHNESS,
    Q3SS_TEXTURE_METALLIC,
    Q3SS_TEXTURE_NORMALS,
    Q3SS_NUM_PATHS,
    // the numbers
    Q3SS_ROUGHNESS_OVERRIDE = Q3SS_NUM_PATHS,
    Q3SS_METALNESS_FACTOR,
    Q3SS_SPECULAR_FACTOR,
    Q3SS_BUMP_SCALE,
    Q3SS_EMISSIVE_FACTOR,
    Q3SS_COUNT
};

enum { Q3SS_SCOPE_GLOBAL, Q3SS_SCOPE_MAP, Q3SS_SCOPE_CONSOLE };

// What one stage of one material has: the Q3SS_* bits it states, and their values.
typedef struct {
    uint32_t    set;
    const char  *paths[Q3SS_NUM_PATHS];
    float       values[Q3SS_COUNT - Q3SS_NUM_PATHS];
} q3stagesettings_t;

void vkpt_q3_stage_override_clear(int scope);

// Sets `field` of stage `stage` of `material` in `scope`. "none" (or "-", or
// "0" for an image, "-1" for roughness_override) states that there is none,
// over what a lower scope says. False, with the reason in `err`, for a value
// that is not one.
bool vkpt_q3_stage_setting_add(int scope, const char *material, int stage, int field,
                               const char *value, char *err, size_t err_size);

// What stage `stage` of `material` has, all scopes and its instance base
// considered. False when it states nothing.
bool vkpt_q3_stage_settings(const char *material, int stage, q3stagesettings_t *out);

// The image stage `stage` of `material` draws in place of its script's, or NULL
// (NULL for "off" too: see vkpt_q3_stage_off).
const char *vkpt_q3_stage_override(const char *material, int stage);

// "stage<N>_map off": the value that drops a stage from the drawing
#define Q3SS_MAP_OFF    "off"

// True if `material`'s .mat says stage<N>_map off: the stage is left out of the
// program, and gives the material neither its base nor its glow.
bool vkpt_q3_stage_off(const char *material, int stage);

// "stage2_roughness_override" -> 2, with *field Q3SS_ROUGHNESS_OVERRIDE. -1 when
// the key is not "stage<N>_..."; *field is -1 when the setting is not one.
int vkpt_q3_stage_parse_key(const char *key, int *field);

// "roughness_override" -> Q3SS_ROUGHNESS_OVERRIDE, else -1 ("texture_base" is
// "map"); and back.
int vkpt_q3_stage_field(const char *word);
const char *vkpt_q3_stage_field_name(int field);

// The value of a setting the way a .mat file states it.
void vkpt_q3_stage_format_value(const q3stagesettings_t *s, int field, char *out, size_t size);

/* 'mat save': the console settings of `material` not written yet, one at a
   time - index 0, 1, ... until it returns false - as a .mat key and value. Then
   vkpt_q3_stage_saved: they are in the file now (`map_scope`: the map's .mat),
   so they stay after the console's are cleared. */
bool vkpt_q3_stage_unsaved(const char *material);
bool vkpt_q3_stage_unsaved_get(const char *material, int index, char *key, size_t key_size,
                               char *value, size_t value_size);
void vkpt_q3_stage_saved(const char *material, bool map_scope);

/* 'mat view stage<N> [map]': the image stage `stage` of `mat`'s program draws
   now (`which` 0), or its roughness (1), metallic (2) or normals (3) map. NULL
   when there is none; *file then says why, else names the image. */
const image_t *vkpt_q3_stage_image(const pbr_material_t *mat, int stage, int which, const char **file);

/* QUAKE III SKY. R_SetSky_RTX hands over the map's sky shader (NULL for a map
   without one). Its cloud layers are drawn per ray, moving (shader/q3_sky.glsl),
   unless pt_q3_sky_clouds is 0; its q3map_sun is the map's sun under its own
   skybox (physical_sky.c vkpt_evaluate_sun_light). */

// True when the cloud layers will be drawn on the GPU: the cube map uploaded
// for the sky should then be the far box alone, or they would be drawn twice.
bool vkpt_q3_sky_set(const q3shader_t *sky);

// The layers' transforms and colours at `time` (seconds), into the UBO.
void vkpt_q3_sky_upload(QVKUniformBuffer_t *ubo, float time);

// The sky's q3map_sun, if it has one and pt_q3_sun is on (default on): `color` is its light
// (colour x intensity x pt_q3_sun_scale, tinted toward the sky by pt_q3_sun_sky_tint),
// `degrees` its azimuth (0 = from +x, 90 = from +y), `elevation` its height above the
// horizon.
bool vkpt_q3_sky_sun(vec3_t color, float *degrees, float *elevation);

// What the fog's sun term (pt_fog_scale_sun) is multiplied by under a Quake III sun, so the
// haze keeps the sun's brightness at pt_q3_sun_scale 0.1 while pt_q3_sun_scale dims only
// the light on surfaces (0.1 / pt_q3_sun_scale). pt_q3_sun_sky_tint colours both.
// 1 with no Q3 sun.
float vkpt_q3_sun_fog_ratio(void);

// Registers the Quake III sky cvars (pt_q3_sun, pt_q3_sun_scale, ...), so a map cfg's
// mapcvar can set them before the first frame under a Quake III sky.
void vkpt_q3_sky_register(void);

// The current Quake III sky's average colour (the baked cube map's), for the sun's tint.
// Call after vkpt_q3_sky_set; it resets the tint.
void vkpt_q3_sky_tint(const vec3_t avg);

/* QUAKE III QUAD DAMAGE (q3stages.c): the quad's look on a Quake III map, its
   powerups/quad stage glowing over the model (TEXTURE_FLAG_Q3_QUAD).
   vkpt_q3_quad_register: at every map registration - loads the stage for a
   Quake III map, forgets it for any other. vkpt_q3_quad_ready: its image is in
   memory, so a quad entity takes this look instead of the blue shell.
   vkpt_q3_quad_upload: the stage at `time` (seconds) into the UBO. */
void vkpt_q3_quad_register(bool quake3);
bool vkpt_q3_quad_ready(void);
void vkpt_q3_quad_upload(QVKUniformBuffer_t *ubo, float time);

#endif // VKPT_Q3STAGES_H
