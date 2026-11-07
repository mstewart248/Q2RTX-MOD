/*
D3D12 textures: the bindless image table, its four samplers and the
byte-per-image sampler table (shader/global_textures.h, VKPT_D3D12).

As on Vulkan, IMG_Load only keeps the pixels (image->pix_data, which the shared
material and image code reads too) and the GPU texture is made at the start of
the next frame or at the end of registration, for every registered image that
has pixels and no texture yet. A qhandle_t is its own descriptor index.

Every texture gets vkpt's mip chain (upload_levels below), built on the CPU at
upload (mipgen.c) - a map load's on every logical processor (make_pending_textures).
*/

#include "d3d12pt.h"
#include "mipgen.h"
#include "system/system.h"
#include <SDL.h>
#include "../vkpt/shader/constants.h"
#include "../vkpt/texture_mips.h"
#include "../stb/stb_image.h"

static ID3D12Resource   *tex[MAX_RIMAGES];
static ID3D12Resource   *blue_noise;
static bool             tex_pending;        // some image is waiting for its texture

/* THE BINDLESS TABLE, ONE COPY PER FRAME PARITY. A frame reads the copy of its
   parity (d3d12_bindless_base: the world's textures sets and the 2D layer). Every
   write - a texture made, an image unloaded - goes into a CPU-only staging heap,
   from which a copy is brought up to date only in a frame of its parity, after
   R_BeginFrame's wait for the last frame that read it. Written straight into one
   shader-visible table, a slot changed under the frame still on the GPU: a
   cinematic unloads its picture and registers the next into the same slot every
   video frame (cin.c), and that frame drew the slot null, or the next picture
   before its upload had run - the video flickered. vkpt keeps a textures
   descriptor set per frame in flight for the same reason. */
typedef char bindless_copies_check[D3D12_BINDLESS_COPIES == D3D12_FRAMES_IN_FLIGHT ? 1 : -1];
static ID3D12DescriptorHeap *bindless_staging;     // CPU only, MAX_RIMAGES
static uint32_t         bindless_dirty[D3D12_BINDLESS_COPIES][MAX_RIMAGES / 32];

static uint8_t          sampler_kind[MAX_RIMAGES];
static bool             sampler_table_dirty;
static ID3D12Resource   *sampler_table;     // MAX_RIMAGES bytes, a UAV (storage buffers all are)

static cvar_t *pt_nearest, *pt_bilerp_chars, *pt_bilerp_pics;
static cvar_t *d3d12_texture_threads;

static void sampler_settings_changed(cvar_t *self)
{
    (void)self;
    sampler_table_dirty = true;
}

/* The four samplers, in VKPT_SAMPLER_* order, at the start of the sampler heap
   (d3d12_global_sampler_desc has the settings and how they differ from vkpt's).
   Set instances that bind them (the textures set's binding 900) copy them. */
static void create_samplers(void)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(d3d.sampler_heap, &h);
    for (int i = 0; i < D3D12_NUM_GLOBAL_SAMPLERS; i++) {
        D3D12_SAMPLER_DESC s;
        d3d12_global_sampler_desc(i, &s);
        ID3D12Device5_CreateSampler(d3d.device, &s, h);
        h.ptr += d3d.sampler_increment;
    }
}

UINT d3d12_bindless_base(int odd)
{
    return D3D12_HEAP_BINDLESS_BASE + (UINT)(odd & 1) * MAX_RIMAGES;
}

// image `index`'s descriptor in the staging heap
static D3D12_CPU_DESCRIPTOR_HANDLE staging_cpu(int index)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(bindless_staging, &h);
    h.ptr += (SIZE_T)index * d3d.srv_increment;
    return h;
}

// image `index`'s staging descriptor was rewritten: every copy is behind
static void bindless_changed(int index)
{
    for (int c = 0; c < D3D12_BINDLESS_COPIES; c++)
        bindless_dirty[c][index >> 5] |= 1u << (index & 31);
}

// Copy `c` brought up to date, a run of changed slots at a time. No frame that
// reads it may still be on the GPU: the caller's to know.
static void bindless_refresh(int c)
{
    uint32_t *dirty = bindless_dirty[c];
    for (int i = 0; i < MAX_RIMAGES;) {
        if (!dirty[i >> 5]) {
            i = (i | 31) + 1;   // the next word
            continue;
        }
        if (!(dirty[i >> 5] & (1u << (i & 31)))) {
            i++;
            continue;
        }
        int end = i;
        while (end < MAX_RIMAGES && (dirty[end >> 5] & (1u << (end & 31)))) {
            dirty[end >> 5] &= ~(1u << (end & 31));
            end++;
        }
        ID3D12Device5_CopyDescriptorsSimple(d3d.device, (UINT)(end - i), d3d12_srv_cpu(d3d12_bindless_base(c) + (UINT)i),
                                            staging_cpu(i), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        i = end;
    }
}

static void write_null_srv(int index)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = { 0 };
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    ID3D12Device5_CreateShaderResourceView(d3d.device, NULL, &sv, staging_cpu(index));
    bindless_changed(index);
}

bool d3d12_textures_init(void)
{
    memset(tex, 0, sizeof(tex));
    memset(sampler_kind, 0, sizeof(sampler_kind));

    // the same cvars, names and defaults as vkpt's (main.c)
    pt_nearest = Cvar_Get("pt_nearest", "0", CVAR_ARCHIVE);
    pt_bilerp_chars = Cvar_Get("pt_bilerp_chars", "0", CVAR_ARCHIVE);
    pt_bilerp_pics = Cvar_Get("pt_bilerp_pics", "0", CVAR_ARCHIVE);
    pt_nearest->changed = pt_bilerp_chars->changed = pt_bilerp_pics->changed = sampler_settings_changed;
    // the map-load mip chain workers (make_pending_textures): 0 every logical
    // processor, 1 the main thread alone, as before 11-06 - an A/B, not saved
    d3d12_texture_threads = Cvar_Get("d3d12_texture_threads", "0", 0);

    create_samplers();
    D3D12_DESCRIPTOR_HEAP_DESC hd = { 0 };
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = MAX_RIMAGES;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (!D3D_CHECK(ID3D12Device5_CreateDescriptorHeap(d3d.device, &hd, &IID_ID3D12DescriptorHeap,
                                                      (void **)&bindless_staging)))
        return false;
    d3d12_set_name((ID3D12Object *)bindless_staging, "bindless staging");
    for (int i = 0; i < MAX_RIMAGES; i++)
        write_null_srv(i);
    // nothing has been submitted yet: both copies now
    for (int c = 0; c < D3D12_BINDLESS_COPIES; c++)
        bindless_refresh(c);

    sampler_table = d3d12_create_buffer(MAX_RIMAGES, D3D12_HEAP_TYPE_DEFAULT,
                                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "sampler table");
    if (!sampler_table)
        return false;
    sampler_table_dirty = true;
    tex_pending = true;
    return true;
}

void d3d12_textures_shutdown(void)
{
    if (pt_nearest && pt_nearest->changed == sampler_settings_changed)
        pt_nearest->changed = pt_bilerp_chars->changed = pt_bilerp_pics->changed = NULL;

    // the GPU is idle here
    for (int i = 0; i < MAX_RIMAGES; i++)
        D3D12_RELEASE(tex[i]);
    D3D12_RELEASE(sampler_table);
    D3D12_RELEASE(blue_noise);
    D3D12_RELEASE(bindless_staging);
    memset(bindless_dirty, 0, sizeof(bindless_dirty));
}

/* THE BLUE NOISE the path tracer draws its random numbers from (TEX_BLUE_NOISE,
   get_rng in path_tracer_rgen.h): vkpt's load_blue_noise, the same files and
   layout. 128 RGBA 16-bit PNGs become 512 R16 layers of 256x256, layer
   4 * file + channel - 64 MB. Made on first use (the world's init), recorded
   into the upload batch; NULL if the files are missing. */
ID3D12Resource *d3d12_blue_noise(void)
{
    if (blue_noise)
        return blue_noise;

    const int res = BLUE_NOISE_RES;
    const size_t layer_pixels = (size_t)res * res;

    D3D12_HEAP_PROPERTIES hp = { 0 };
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = { 0 };
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = (UINT64)res;
    rd.Height = (UINT)res;
    rd.DepthOrArraySize = NUM_BLUE_NOISE_TEX;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R16_UNORM;
    rd.SampleDesc.Count = 1;
    ID3D12Resource *res_tex = NULL;
    if (!D3D_CHECK(ID3D12Device5_CreateCommittedResource(d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,
                                                         (void **)&res_tex)))
        return NULL;
    d3d12_set_name((ID3D12Object *)res_tex, "blue noise");

    uint16_t *layer = Z_Malloc(layer_pixels * sizeof(uint16_t));
    bool ok = true;
    for (int i = 0; i < NUM_BLUE_NOISE_TEX / 4 && ok; i++) {
        char name[MAX_QPATH];
        Q_snprintf(name, sizeof(name), "blue_noise/%d_%d/HDR_RGBA_%04d.png", res, res, i);
        byte *file = NULL;
        int len = FS_LoadFile(name, (void **)&file);
        int w = 0, h = 0, n = 0;
        uint16_t *data = NULL;
        if (file) {
            data = stbi_load_16_from_memory(file, len, &w, &h, &n, 4);
            FS_FreeFile(file);
        }
        if (!data || w != res || h != res) {
            Com_EPrintf("D3D12: error loading blue noise tex %s\n", name);
            ok = false;
        } else {
            for (int c = 0; c < 4 && ok; c++) {
                for (size_t p = 0; p < layer_pixels; p++)
                    layer[p] = data[p * 4 + c];
                ok = d3d12_upload_subresource(res_tex, (uint32_t)(i * 4 + c), layer, res, res, sizeof(uint16_t));
            }
        }
        if (data)
            stbi_image_free(data);
    }
    Z_Free(layer);

    ID3D12GraphicsCommandList *cmd = ok ? d3d12_upload_list() : NULL;
    if (!cmd) {
        // the upload batch may already hold copies into it: run it first
        d3d12_upload_flush();
        d3d12_wait_idle();
        D3D12_RELEASE(res_tex);
        return NULL;
    }
    D3D12_RESOURCE_BARRIER b = { 0 };
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res_tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(cmd, 1, &b);

    blue_noise = res_tex;
    return blue_noise;
}

D3D12_GPU_VIRTUAL_ADDRESS d3d12_sampler_index_buffer(void)
{
    return sampler_table ? ID3D12Resource_GetGPUVirtualAddress(sampler_table) : 0;
}

ID3D12Resource *d3d12_sampler_index_resource(void)
{
    return sampler_table;
}

static int sampler_kind_of(const image_t *image)
{
    return d3d12_frontend_sampler_kind(image, pt_nearest->integer, pt_bilerp_chars->integer,
                                       pt_bilerp_pics->integer);
}

/* ------------------------------------------------------------------------- */

static DXGI_FORMAT image_format(const image_t *image, int *bytes_per_pixel)
{
    switch (image->pixel_format) {
    case PF_R16_UNORM:
        *bytes_per_pixel = 2;
        return DXGI_FORMAT_R16_UNORM;
    case PF_R8G8B8A8_UNORM:
    default:
        *bytes_per_pixel = 4;
        return image->is_srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

/* THE MIP CHAIN, as vkpt/textures.c builds it (its phases 3 and 4):
   - level count: vkpt_image_num_miplevels (1 + log2 of the larger side; IF_NO_MIPMAPS,
     the video frames, get none);
   - POM height maps (IF_HEIGHT_MAP): vkpt's max-filtered chain, vkpt_downsample_max;
   - a linear (not sRGB) normal map (IF_NORMAL_MAP): renormalised first, as
     normalize_normal_map.comp does to vkpt's level 0, so the chain is built from that;
     pix_data keeps what was loaded (materials read it);
   - everything else: the linear blit chain (mipgen.c), filtered in linear light for
     sRGB images.
   build_chain makes all of it but level 0 as loaded into one buffer, level after level,
   each from the one before; upload_levels puts each level into the upload batch as its
   own copy. */
typedef struct {
    int     w, h, bpp, levels;
    bool    max_mips, normalize, srgb;
} chain_t;

static bool chain_of(const image_t *image, chain_t *c)
{
    image_format(image, &c->bpp);
    c->w = image->upload_width;
    c->h = image->upload_height;
    if (c->w <= 0 || c->h <= 0)
        return false;
    c->levels = vkpt_image_num_miplevels(image);
    c->max_mips = vkpt_image_has_max_mips(image);
    c->normalize = !c->max_mips && (image->flags & IF_NORMAL_MAP) && !image->is_srgb && c->bpp == 4;
    c->srgb = image->is_srgb && c->bpp == 4;
    return true;
}

// build_chain's buffer: the renormalised level 0 of a normal map, then levels 1 and
// down, tightly packed. 0 for a texture that is level 0 as loaded and nothing else.
static size_t chain_bytes(const chain_t *c)
{
    size_t total = c->normalize ? (size_t)c->w * c->h * 4 : 0;
    int lw = c->w, lh = c->h;
    for (int mip = 1; mip < c->levels; mip++) {
        lw = lw > 1 ? lw >> 1 : lw;
        lh = lh > 1 ? lh >> 1 : lh;
        total += (size_t)lw * lh * c->bpp;
    }
    return total;
}

// Fills `out` (chain_bytes of it). Only reads the image's pixels and calls mipgen.c's
// pure functions, so a worker thread may run it (make_pending_textures).
static void build_chain(const image_t *image, const chain_t *c, uint8_t *out)
{
    const uint8_t *level = image->pix_data;
    if (c->normalize) {
        // pix_data keeps what was loaded: the materials read it
        memcpy(out, level, (size_t)c->w * c->h * 4);
        d3d12_mip_normalize_normals(out, (size_t)c->w * c->h);
        level = out;
        out += (size_t)c->w * c->h * 4;
    }
    int lw = c->w, lh = c->h;
    for (int mip = 1; mip < c->levels; mip++) {
        const int nw = lw > 1 ? lw >> 1 : lw, nh = lh > 1 ? lh >> 1 : lh;
        if (c->max_mips)
            vkpt_downsample_max(level, lw, lh, out, nw, nh, c->bpp);
        else
            d3d12_mip_downsample(level, lw, lh, out, nw, nh, c->bpp, c->srgb);
        level = out;
        out += (size_t)nw * nh * c->bpp;
        lw = nw;
        lh = nh;
    }
}

// `chain` is build_chain's buffer, already made, or NULL to make it here
static bool upload_levels(ID3D12Resource *res, const image_t *image, const chain_t *c, const uint8_t *chain)
{
    uint8_t *own = NULL;
    const size_t bytes = chain_bytes(c);
    if (bytes && !chain) {
        own = malloc(bytes);
        if (!own) {
            // level 0 as loaded beats no texture at all
            const bool ok = d3d12_upload_subresource(res, 0, image->pix_data, c->w, c->h, c->bpp);
            return ok && c->levels == 1;
        }
        build_chain(image, c, own);
        chain = own;
    }

    const uint8_t *level0 = c->normalize ? chain : image->pix_data;
    const uint8_t *next = chain ? chain + (c->normalize ? (size_t)c->w * c->h * 4 : 0) : NULL;
    bool ok = d3d12_upload_subresource(res, 0, level0, c->w, c->h, c->bpp);
    int lw = c->w, lh = c->h;
    for (int mip = 1; mip < c->levels && ok; mip++) {
        lw = lw > 1 ? lw >> 1 : lw;
        lh = lh > 1 ? lh >> 1 : lh;
        ok = d3d12_upload_subresource(res, (uint32_t)mip, next, lw, lh, c->bpp);
        next += (size_t)lw * lh * c->bpp;
    }
    free(own);
    return ok;
}

static void create_texture(int index, const uint8_t *chain)
{
    image_t *image = r_images + index;
    chain_t c;
    if (!chain_of(image, &c))
        return;
    const DXGI_FORMAT format = image_format(image, &c.bpp);
    const int w = c.w, h = c.h, levels = c.levels;

    D3D12_HEAP_PROPERTIES hp = { 0 };
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = { 0 };
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = (UINT64)w;
    rd.Height = (UINT)h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = (UINT16)levels;
    rd.Format = format;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    ID3D12Resource *res = NULL;
    if (!D3D_CHECK(ID3D12Device5_CreateCommittedResource(d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,
                                                         (void **)&res)))
        return;
    d3d12_set_name((ID3D12Object *)res, image->name);
    if (!upload_levels(res, image, &c, chain))
        Com_EPrintf("D3D12: %s: not every mip level was uploaded\n", image->name);

    // every level to shader reads, uploaded or not, so nothing samples a COPY_DEST texture
    ID3D12GraphicsCommandList *cmd = d3d12_upload_list();
    if (cmd) {
        D3D12_RESOURCE_BARRIER b = { 0 };
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        ID3D12GraphicsCommandList_ResourceBarrier(cmd, 1, &b);
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC sv = { 0 };
    sv.Format = format;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = (UINT)levels;
    ID3D12Device5_CreateShaderResourceView(d3d.device, res, &sv, staging_cpu(index));
    bindless_changed(index);

    tex[index] = res;

    uint8_t kind = (uint8_t)sampler_kind_of(image);
    if (kind != sampler_kind[index]) {
        sampler_kind[index] = kind;
        sampler_table_dirty = true;
    }
}

/* A MAP LOAD'S TEXTURES, ON EVERY CORE. The first frame after a map load makes every
   texture the map registered - hundreds on a rerelease map, many 2048 square, with
   their normal maps - and their mip chains and renormalisation are CPU work here,
   where vkpt hands them to the GPU (blits, normalize_normal_map.comp). On the main
   thread alone that kept the world black for about 5 s after the level's music had
   started (Matt, 11-06).

   So the frame first makes every texture's build_chain buffer on all the logical
   processors (the main thread works too), biggest first so a batch does not end on
   one big texture, in batches of at most PREP_BATCH_BYTES of chains held at once;
   then the main thread creates and uploads that batch's textures as before. The
   bytes are the same either way. The workers use malloc, not Z_Malloc, which is not
   thread-safe. A few textures (a HUD picture, a cinematic frame) skip all this. */
#define PREP_BATCH_BYTES    ((size_t)512 << 20)
#define PREP_MIN_BYTES      ((size_t)8 << 20)
#define PREP_MAX_THREADS    64

typedef struct {
    int         index;      // the r_images slot
    chain_t     chain;
    size_t      bytes;      // chain_bytes, 0 = nothing to make
    uint8_t     *data;      // build_chain's buffer once made; NULL = create_texture makes it
} tex_job_t;

static tex_job_t    tex_jobs[MAX_RIMAGES];

typedef struct {
    tex_job_t       *jobs;
    int             count;
    SDL_atomic_t    next;   // the next job to take
} prep_batch_t;

static void prep_run(prep_batch_t *b)
{
    int i;
    while ((i = SDL_AtomicAdd(&b->next, 1)) < b->count) {
        tex_job_t *j = &b->jobs[i];
        if (!j->bytes)
            continue;
        j->data = malloc(j->bytes);
        if (j->data)
            build_chain(r_images + j->index, &j->chain, j->data);
    }
}

static int SDLCALL prep_worker(void *arg)
{
    prep_run(arg);
    return 0;
}

static int prep_order(const void *a, const void *b)
{
    const tex_job_t *x = a, *y = b;
    if (x->bytes != y->bytes)
        return x->bytes < y->bytes ? 1 : -1;
    return x->index - y->index;
}

static void make_pending_textures(void)
{
    int count = 0, with_chain = 0;
    size_t chain_total = 0;
    for (int i = 0; i < MAX_RIMAGES; i++) {
        const image_t *image = r_images + i;
        if (tex[i] || !image->registration_sequence || !image->pix_data)
            continue;
        tex_job_t *j = &tex_jobs[count++];
        j->index = i;
        j->data = NULL;
        j->bytes = chain_of(image, &j->chain) ? chain_bytes(&j->chain) : 0;
        chain_total += j->bytes;
        with_chain += j->bytes > 0;
    }
    if (!count)
        return;

    const uint64_t start = Sys_Microseconds();
    int threads = d3d12_texture_threads->integer > 0 ? d3d12_texture_threads->integer : SDL_GetCPUCount();
    threads = threads < 1 ? 1 : threads > PREP_MAX_THREADS ? PREP_MAX_THREADS : threads;
    if (with_chain < 2 || chain_total < PREP_MIN_BYTES)
        threads = 1;

    uint64_t chains_us = 0;
    if (threads == 1) {
        // create_texture makes each chain itself, on this thread
        for (int k = 0; k < count; k++)
            create_texture(tex_jobs[k].index, NULL);
    } else {
        d3d12_mip_init();   // the sRGB tables, before any worker reads them
        qsort(tex_jobs, (size_t)count, sizeof(tex_jobs[0]), prep_order);
        for (int first = 0; first < count;) {
            int end = first;
            size_t held = 0;
            while (end < count && (end == first || held + tex_jobs[end].bytes <= PREP_BATCH_BYTES))
                held += tex_jobs[end++].bytes;

            prep_batch_t b;
            b.jobs = tex_jobs + first;
            b.count = end - first;
            SDL_AtomicSet(&b.next, 0);

            const uint64_t t0 = Sys_Microseconds();
            SDL_Thread *workers[PREP_MAX_THREADS];
            int started = 0;
            for (int t = 1; t < threads && t < b.count; t++) {
                workers[started] = SDL_CreateThread(prep_worker, "texture mips", &b);
                if (workers[started])
                    started++;
            }
            prep_run(&b);   // and whatever no worker took, if none started
            for (int t = 0; t < started; t++)
                SDL_WaitThread(workers[t], NULL);
            chains_us += Sys_Microseconds() - t0;

            for (int k = first; k < end; k++) {
                create_texture(tex_jobs[k].index, tex_jobs[k].data);
                free(tex_jobs[k].data);
                tex_jobs[k].data = NULL;
            }
            first = end;
        }
    }

    // a map load's: one line, to compare with d3d12_texture_threads 1 and with Vulkan
    if (count >= 64) {
        const int total_ms = (int)((Sys_Microseconds() - start) / 1000);
        if (threads > 1)
            Com_Printf("D3D12: %d textures made in %d ms (mip chains %d ms on %d threads)\n", count, total_ms,
                       (int)(chains_us / 1000), threads);
        else
            Com_Printf("D3D12: %d textures made in %d ms (one thread)\n", count, total_ms);
    }
}

/* Called at the start of every frame and before its 2D layer is drawn (R_EndFrame).
   Records into the upload batch, which runs before the frame's commands, and brings
   the frame's copy of the bindless table up to date. */
void d3d12_textures_begin_frame(void)
{
    if (tex_pending) {
        tex_pending = false;
        make_pending_textures();
    }

    if (sampler_table_dirty) {
        sampler_table_dirty = false;
        for (int i = 0; i < MAX_RIMAGES; i++)
            sampler_kind[i] = (uint8_t)(r_images[i].registration_sequence ? sampler_kind_of(r_images + i) : 0);
        // a buffer in COMMON is promoted to COPY_DEST by the copy and decays
        // back when the batch completes: no barriers
        d3d12_upload_buffer(sampler_table, 0, sampler_kind, sizeof(sampler_kind));
    }

    // R_BeginFrame waited for the last frame of this one's parity: its copy is free
    if (d3d.frame_open && bindless_staging)
        bindless_refresh((int)(d3d.frame_counter & 1));
}

void IMG_Load_D3D12(image_t *image, byte *pic)
{
    // kept, like vkpt's: materials and the image code read pix_data later
    image->pix_data = pic;
    tex_pending = true;

    /* A RELOAD (IMG_ReloadFrom: reload_textures) into a slot with a texture: it was
       never made again, so an edited file changed nothing on screen. Now the old one
       goes as IMG_Unload's does - it lives on until the frames in flight are done -
       and the next frame's d3d12_textures_begin_frame makes it from the new pixels. */
    const int index = (int)(image - r_images);
    if (index >= 0 && index < MAX_RIMAGES && tex[index]) {
        D3D12_DEFER_RELEASE(tex[index]);
        write_null_srv(index);
    }
}

void IMG_Unload_D3D12(image_t *image)
{
    int index = (int)(image - r_images);

    if (image->pix_data)
        Z_Free(image->pix_data);
    image->pix_data = NULL;

    if (index >= 0 && index < MAX_RIMAGES && tex[index]) {
        /* A frame still in flight may sample this slot through its own copy of
           the table, which still names this texture: it lives on until that frame
           is done (the deferred release). The slot goes null in the staging heap,
           and so in each copy at its next frame (bindless_refresh). */
        D3D12_DEFER_RELEASE(tex[index]);
        write_null_srv(index);
    }
}

byte *IMG_ReadPixels_D3D12(int *width, int *height, int *rowbytes)
{
    return d3d12_read_frame_target(width, height, rowbytes);
}
