/*
==============================================================================

SURFACE-DEPENDENT FOOTSTEPS

The rerelease plays a different footstep sound depending on what you are
standing on - metal grating clanks, wood creaks, grass crunches. The mapping is
pure data and ships with the remaster:

    textures/<texture name>.mat   contains ONE WORD, e.g. "clank"
    sound/player/steps/<word><N>.wav   are the sounds for it, N starting at 1

3909 of those .mat files exist, using 15 distinct words: clank (by far the most
common), glass, boot, mech, wood, step, splash, grass, tile, carpet, energy,
snow, junk, meat, flesh. A sixteenth sound set, "ladder", exists but is not
named by any texture - it is for climbing, which this does not do.

The game DLL is not involved. It sets EV_FOOTSTEP exactly as before; all this
does is choose which sound that event plays, so nothing here affects gameplay,
prediction, or demos recorded with the old behaviour.

LIMITATION: the trace is against the WORLD bsp only, so standing on a moving
brush model (a lift, a door) falls back to the default footsteps rather than
picking up that surface's material. Walking on world geometry - which is nearly
everything - resolves correctly.

`cl_footstep_materials 0` restores the old four-sound behaviour.

==============================================================================
*/

#include "client.h"

#define FS_MAX_MATERIALS        24
#define FS_MAX_SOUNDS           8       // most sets have 4 or 5
#define FS_MAX_CACHE            512     // distinct textures seen this map

typedef struct {
    char        word[16];
    qhandle_t   sounds[FS_MAX_SOUNDS];
    int         count;
} footstep_material_t;

typedef struct {
    char    texture[MAX_QPATH];
    int     material;                   // index into fs_materials, or -1
} footstep_cache_t;

static footstep_material_t  fs_materials[FS_MAX_MATERIALS];
static int                  fs_num_materials;

static footstep_cache_t     fs_cache[FS_MAX_CACHE];
static int                  fs_num_cached;

cvar_t *cl_footstep_materials;

/*
=================
CL_ClearFootstepCache

The cache holds texture names and sound handles, both of which belong to the
map that is going away, so it is dropped on every map load.
=================
*/
void CL_ClearFootstepCache(void)
{
    fs_num_materials = 0;
    fs_num_cached = 0;
}

/*
=================
CL_FindFootstepMaterial

Find or create the sound set for one material word. Returns -1 if no sounds
exist for it, so an unknown or misspelled word falls back to the default.
=================
*/
static int CL_FindFootstepMaterial(const char *word)
{
    footstep_material_t *mat;
    char    path[MAX_QPATH];
    int     i;

    for (i = 0; i < fs_num_materials; i++) {
        if (!Q_stricmp(fs_materials[i].word, word))
            return i;
    }

    if (fs_num_materials == FS_MAX_MATERIALS)
        return -1;

    mat = &fs_materials[fs_num_materials];
    memset(mat, 0, sizeof(*mat));
    Q_strlcpy(mat->word, word, sizeof(mat->word));

    // the sets are numbered from 1 and are not all the same length, so probe
    // until one is missing rather than assuming a count
    for (i = 0; i < FS_MAX_SOUNDS; i++) {
        Q_snprintf(path, sizeof(path), "sound/player/steps/%s%d.wav", word, i + 1);
        if (!FS_FileExists(path))
            break;

        Q_snprintf(path, sizeof(path), "player/steps/%s%d.wav", word, i + 1);
        mat->sounds[i] = S_RegisterSound(path);
        if (!mat->sounds[i])
            break;
    }

    mat->count = i;

    if (!mat->count)
        return -1;

    fs_num_materials++;
    return fs_num_materials - 1;
}

/*
=================
CL_FootstepMaterialForTexture

Texture name -> material index, cached. A miss is cached too (as -1), so a
texture with no .mat file costs one failed file lookup per map, not one per
step.
=================
*/
static int CL_FootstepMaterialForTexture(const char *texture)
{
    char    path[MAX_QPATH];
    char    word[16];
    byte    *data;
    int     len, i, n;

    for (i = 0; i < fs_num_cached; i++) {
        if (!Q_stricmp(fs_cache[i].texture, texture))
            return fs_cache[i].material;
    }

    n = -1;

    Q_snprintf(path, sizeof(path), "textures/%s.mat", texture);
    len = FS_LoadFile(path, (void **)&data);
    if (data) {
        // the whole file is one bare word, but do not trust that - stop at the
        // first whitespace and ignore anything after it
        for (i = 0; i < len && i < sizeof(word) - 1; i++) {
            if (data[i] <= ' ')
                break;
            word[i] = data[i];
        }
        word[i] = 0;

        if (i)
            n = CL_FindFootstepMaterial(word);

        FS_FreeFile(data);
    }

    if (fs_num_cached < FS_MAX_CACHE) {
        Q_strlcpy(fs_cache[fs_num_cached].texture, texture, MAX_QPATH);
        fs_cache[fs_num_cached].material = n;
        fs_num_cached++;
    }

    return n;
}

/*
=================
CL_FootstepSound

The sound to play for a footstep by the entity at `origin`, or 0 to let the
caller use the default set.
=================
*/
qhandle_t CL_FootstepSound(const vec3_t origin)
{
    footstep_material_t *mat;
    trace_t trace;
    vec3_t  end;
    int     n;

    if (!cl_footstep_materials->integer)
        return 0;

    if (!cl.bsp || !cl.bsp->nodes)
        return 0;

    // straight down from the entity origin. 64 units clears the 24 to the feet
    // with room for the step the player is part way through.
    VectorCopy(origin, end);
    end[2] -= 64;

    CM_BoxTrace(&trace, origin, end, vec3_origin, vec3_origin, cl.bsp->nodes, MASK_SOLID);

    if (trace.fraction == 1.0f || !trace.surface || !trace.surface->name[0])
        return 0;

    n = CL_FootstepMaterialForTexture(trace.surface->name);
    if (n < 0)
        return 0;

    mat = &fs_materials[n];
    return mat->sounds[Q_rand() % mat->count];
}
