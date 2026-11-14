/*
D3D12 swapchain: a flip-discard swapchain on the SDL window, the frame target
every frame is composed into, presentation and screenshots.

FRAME GENERATION (pt_dlss_fg) changes where presents happen. A present is a queue
operation: its flip waits for everything queued ahead of it on its queue. On the
main queue the next frame's rendering is queued ahead of every generated frame's
present, so a frame's presents could only flip together once that rendering was done
- vkpt measured exactly that before it moved its presents to a queue of their own
(fg_present.c). So with frame generation the swapchain is made on a present queue
of its own (d3d12_fg_mode), and fg_present.c's thread paces the presents onto it:
each one copies a PRESENT SOURCE into the current backbuffer there, then presents.
The sources are the frame's composed images: [set][0] the real frame (a copy of the
frame target), [set][1..] the generated ones, which frontend.c draws. Two sets, used
alternately, so one frame's presents can still be going out while the next renders;
the main queue waits for a set's last copy before writing it again (d3d12_fg_wait_set).
Without frame generation nothing here changes: the main queue copies the frame
target to the backbuffer and R_EndFrame presents.

HDR (vid_hdr), as vkpt's: the swapchain is scRGB - R16G16B16A16_FLOAT in
DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, linear, 1.0 = 80 nits - and so are the frame
target and the present sources. The HDR pipelines draw into them (tone_mapping_apply's
and stretch_pic's spec_tone_mapping_hdr 1, the blit's clamp). Switching vid_hdr
remakes the buffers in the other format, as a resize does; a swapchain that cannot
show scRGB puts vid_hdr back to 0, as vkpt does without the surface format.
*/

#include "d3d12pt.h"
#include "../vkpt/conversion.h"     // halfToFloat: the HDR screenshots

#include <SDL.h>
#include <SDL_syswm.h>

extern SDL_Window *sdl_window;

static cvar_t *vid_vsync;
static cvar_t *vid_hdr;
static cvar_t *d3d12_frame_latency;     // how many presents DXGI may queue (1 = lowest latency)

#define FG_LISTS    8                   // the present thread's command lists in flight

static struct {
    bool                        active;         // the swapchain is on `queue`
    ID3D12CommandQueue          *queue;         // the present queue (direct, high priority)
    // main queue: a frame's work is done (d3d12_fg_signal_ready)
    ID3D12Fence                 *ready;
    uint64_t                    ready_value;
    HANDLE                      ready_event;    // the present thread's waits
    // present queue: its copies are done
    ID3D12Fence                 *done;
    uint64_t                    done_value;
    HANDLE                      done_event;
    // the present thread's
    ID3D12CommandAllocator      *allocators[FG_LISTS];
    uint64_t                    allocator_done[FG_LISTS];
    ID3D12GraphicsCommandList   *list;
    unsigned int                next_list;
    // the present sources, at the window's size, in COPY_SOURCE between uses
    ID3D12Resource              *sources[D3D12_FG_SETS][1 + D3D12_FG_MAX_GENERATED];
    int                         num_generated;  // each set has sources for this many generated frames
    uint64_t                    set_read[D3D12_FG_SETS];    // `done` reaches it: the set's last copy ran
} fg;

static UINT swapchain_flags(void)
{
    UINT flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (d3d.tearing_enabled)
        flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    return flags;
}

static HWND window_hwnd(void)
{
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!sdl_window || !SDL_GetWindowWMInfo(sdl_window, &info) || info.subsystem != SDL_SYSWM_WINDOWS)
        return NULL;
    return info.info.win.window;
}

// the client area in pixels: SDL's window size can be in points on a scaled desktop
static void window_size(int *width, int *height)
{
    RECT rc = { 0 };
    if (!d3d.hwnd || !GetClientRect(d3d.hwnd, &rc)) {
        *width = r_config.width;
        *height = r_config.height;
        return;
    }
    *width = rc.right - rc.left;
    *height = rc.bottom - rc.top;
}

/* ------------------------------------------------------------------------- */
/* FRAME TARGET                                                               */
/* ------------------------------------------------------------------------- */

// a window-sized image of the backbuffer's family, in COPY_SOURCE, with the sRGB
// render target view in RTV slot `rtv` (-1: none); with HDR, scRGB as the swapchain
static bool create_target(ID3D12Resource **out, int rtv_slot, const char *name)
{
    D3D12_HEAP_PROPERTIES hp = { 0 };
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    // typeless, so it can have the sRGB render target view; same family as the
    // backbuffer, so CopyResource takes it
    D3D12_RESOURCE_DESC rd = { 0 };
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = (UINT64)d3d.width;
    rd.Height = (UINT)d3d.height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = d3d.hdr ? D3D12_HDR_FORMAT : DXGI_FORMAT_B8G8R8A8_TYPELESS;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear = { 0 };
    clear.Format = d3d12_frame_rtv_format();
    clear.Color[3] = 1.f;

    if (!D3D_CHECK(ID3D12Device5_CreateCommittedResource(d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                         D3D12_RESOURCE_STATE_COPY_SOURCE, &clear,
                                                         &IID_ID3D12Resource, (void **)out)))
        return false;
    d3d12_set_name((ID3D12Object *)*out, name);

    if (rtv_slot >= 0) {
        D3D12_RENDER_TARGET_VIEW_DESC rtv = { 0 };
        rtv.Format = d3d12_frame_rtv_format();
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        ID3D12Device5_CreateRenderTargetView(d3d.device, *out, &rtv, d3d12_rtv(rtv_slot));
    }
    return true;
}

static int source_rtv_slot(int set, int index)
{
    return index ? D3D12_RTV_FG_SOURCES + set * D3D12_FG_MAX_GENERATED + index - 1 : -1;
}

// the present sources for `generated` frames a set, those missing made
static bool create_sources(int generated)
{
    for (int s = 0; s < D3D12_FG_SETS; s++)
        for (int i = 0; i <= generated; i++)
            if (!fg.sources[s][i] &&
                !create_target(&fg.sources[s][i], source_rtv_slot(s, i), va("present source %d.%d", s, i)))
                return false;
    return true;
}

static void release_sources(void)
{
    for (int s = 0; s < D3D12_FG_SETS; s++)
        for (int i = 0; i <= D3D12_FG_MAX_GENERATED; i++)
            D3D12_RELEASE(fg.sources[s][i]);
}

static bool get_buffers(void)
{
    for (int i = 0; i < d3d.swapchain_images; i++) {
        if (!D3D_CHECK(IDXGISwapChain3_GetBuffer(d3d.swapchain, i, &IID_ID3D12Resource, (void **)&d3d.backbuffers[i])))
            return false;
        d3d12_set_name((ID3D12Object *)d3d.backbuffers[i], va("backbuffer %d", i));
    }
    if (!create_target(&d3d.frame_target, D3D12_RTV_FRAME_TARGET, "frame target"))
        return false;
    // at the new size, as many as there were
    return !fg.active || create_sources(fg.num_generated);
}

static void release_buffers(void)
{
    // with the GPU idle: ResizeBuffers wants every reference gone
    for (int i = 0; i < D3D12_FG_SWAPCHAIN_IMAGES; i++)
        D3D12_RELEASE(d3d.backbuffers[i]);
    D3D12_RELEASE(d3d.frame_target);
    release_sources();
}

/* ------------------------------------------------------------------------- */

static DXGI_FORMAT swapchain_format(void)
{
    return d3d.hdr ? D3D12_HDR_FORMAT : D3D12_SWAPCHAIN_FORMAT;
}

// the colour space d3d.hdr's format is shown in: scRGB, or the sRGB default; false
// if the swapchain cannot present it
static bool set_color_space(void)
{
    DXGI_COLOR_SPACE_TYPE cs = d3d.hdr ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                                       : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    UINT support = 0;
    if (FAILED(IDXGISwapChain3_CheckColorSpaceSupport(d3d.swapchain, cs, &support)) ||
        !(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
        return false;
    return SUCCEEDED(IDXGISwapChain3_SetColorSpace1(d3d.swapchain, cs));
}

// HDR10 metadata (IDXGISwapChain4): the peak the frame is tone-mapped to, as both the
// mastering display's peak and MaxCLL, so a screen that tone-maps by metadata sees
// content that already fits it and leaves the game's own roll-off alone (HGIG's
// arrangement). Windows composes scRGB itself and may not pass it on - a hint, set
// whenever the swapchain is made or remade (a peak change waits for the next one).
static void set_hdr_metadata(void)
{
    if (!d3d.hdr || !d3d.swapchain)
        return;
    IDXGISwapChain4 *sc4 = NULL;
    if (FAILED(IDXGISwapChain3_QueryInterface(d3d.swapchain, &IID_IDXGISwapChain4, (void **)&sc4)))
        return;

    float max_mastering, min_mastering, max_cll, max_fall;
    d3d12_frontend_hdr_metadata(&max_mastering, &min_mastering, &max_cll, &max_fall);

    // scRGB's primaries and white: BT.709 and D65, in units of 0.00002
    DXGI_HDR_METADATA_HDR10 md = { 0 };
    md.RedPrimary[0] = 32000;   md.RedPrimary[1] = 16500;
    md.GreenPrimary[0] = 15000; md.GreenPrimary[1] = 30000;
    md.BluePrimary[0] = 7500;   md.BluePrimary[1] = 3000;
    md.WhitePoint[0] = 15635;   md.WhitePoint[1] = 16450;
    md.MaxMasteringLuminance = (UINT)(max_mastering + 0.5f);                 // nits
    md.MinMasteringLuminance = (UINT)(min_mastering * 10000.f + 0.5f);       // 0.0001 nits
    md.MaxContentLightLevel = (UINT16)(max_cll < 65535.f ? max_cll + 0.5f : 65535.f);
    md.MaxFrameAverageLightLevel = (UINT16)(max_fall < 65535.f ? max_fall + 0.5f : 65535.f);
    if (FAILED(IDXGISwapChain4_SetHDRMetaData(sc4, DXGI_HDR_METADATA_TYPE_HDR10, sizeof(md), &md)))
        Com_DPrintf("D3D12: SetHDRMetaData failed\n");
    IDXGISwapChain4_Release(sc4);
}

// the swapchain's buffers released (release_buffers): at this size, in the format
// `hdr` asks for, or SDR when scRGB cannot be shown
static bool resize_buffers(int width, int height, bool hdr)
{
    d3d.hdr = hdr;
    if (!D3D_CHECK(IDXGISwapChain3_ResizeBuffers(d3d.swapchain, 0, (UINT)width, (UINT)height, swapchain_format(),
                                                 swapchain_flags())))
        return false;
    if (set_color_space() || !d3d.hdr)
        return true;    // SDR: a swapchain that refuses G22 shows sRGB anyway

    Com_WPrintf("HDR was requested but the swapchain cannot present scRGB.\n");
    Cvar_SetByVar(vid_hdr, "0", FROM_CODE);
    d3d.hdr = false;
    if (!D3D_CHECK(IDXGISwapChain3_ResizeBuffers(d3d.swapchain, 0, (UINT)width, (UINT)height, swapchain_format(),
                                                 swapchain_flags())))
        return false;
    set_color_space();
    return true;
}

// the DXGI swapchain on the main queue, or with frame generation on the present queue
static bool create_swapchain(void)
{
    DXGI_SWAP_CHAIN_DESC1 sd = { 0 };
    sd.Width = (UINT)d3d.width;
    sd.Height = (UINT)d3d.height;
    sd.Format = swapchain_format();
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = (UINT)d3d.swapchain_images;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = swapchain_flags();

    IDXGISwapChain1 *sc1 = NULL;
    ID3D12CommandQueue *queue = fg.active ? fg.queue : d3d.queue;
    if (!D3D_CHECK(IDXGIFactory6_CreateSwapChainForHwnd(d3d.factory, (IUnknown *)queue, d3d.hwnd, &sd, NULL, NULL,
                                                         &sc1)))
        return false;
    HRESULT hr = IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3, (void **)&d3d.swapchain);
    IDXGISwapChain1_Release(sc1);
    if (!D3D_CHECK(hr))
        return false;

    // SDL owns fullscreen; DXGI's own Alt+Enter would fight it
    IDXGIFactory6_MakeWindowAssociation(d3d.factory, d3d.hwnd, DXGI_MWA_NO_ALT_ENTER);

    // scRGB that cannot be shown: SDR buffers instead (vid_hdr back to 0)
    if (!set_color_space() && d3d.hdr && !resize_buffers(d3d.width, d3d.height, true))
        return false;

    int latency = d3d12_frame_latency->integer;
    IDXGISwapChain3_SetMaximumFrameLatency(d3d.swapchain, (UINT)(latency < 1 ? 1 : latency > 3 ? 3 : latency));
    d3d.swapchain_waitable = IDXGISwapChain3_GetFrameLatencyWaitableObject(d3d.swapchain);

    set_hdr_metadata();
    return get_buffers();
}

static void destroy_swapchain(void)
{
    release_buffers();
    if (d3d.swapchain_waitable) {
        CloseHandle(d3d.swapchain_waitable);
        d3d.swapchain_waitable = NULL;
    }
    D3D12_RELEASE(d3d.swapchain);
}

bool d3d12_swapchain_create(void)
{
    vid_vsync = Cvar_Get("vid_vsync", "0", CVAR_ARCHIVE);
    vid_vsync->changed = NULL;          // in case the GL renderer has set it
    vid_vsync->flags &= ~CVAR_REFRESH;  // read at every present: no restart
    // vkpt's (vkpt_init_frontend): checked every frame, the buffers remade in the
    // other format when it changes
    vid_hdr = Cvar_Get("vid_hdr", "0", CVAR_ARCHIVE);
    d3d12_frame_latency = Cvar_Get("d3d12_frame_latency", "2", CVAR_REFRESH | CVAR_ARCHIVE);

    d3d.hwnd = window_hwnd();
    if (!d3d.hwnd) {
        Com_EPrintf("D3D12: no Win32 window to present to: %s\n", SDL_GetError());
        return false;
    }

    window_size(&d3d.width, &d3d.height);
    d3d.width = max(d3d.width, 8);
    d3d.height = max(d3d.height, 8);
    d3d.tearing_enabled = d3d.caps.tearing;
    d3d.swapchain_images = D3D12_SWAPCHAIN_IMAGES;
    d3d.hdr = vid_hdr->integer != 0;
    memset(&fg, 0, sizeof(fg));

    if (!create_swapchain())
        return false;

    Com_Printf("D3D12: swapchain %dx%d, %d buffers, tearing %s, %s\n", d3d.width, d3d.height, d3d.swapchain_images,
               d3d.tearing_enabled ? "allowed" : "unavailable", d3d.hdr ? "HDR (scRGB)" : "SDR");
    return true;
}

/* ------------------------------------------------------------------------- */
/* FRAME GENERATION'S QUEUE                                                   */
/* ------------------------------------------------------------------------- */

static void fg_release_objects(void)
{
    D3D12_RELEASE(fg.list);
    for (int i = 0; i < FG_LISTS; i++)
        D3D12_RELEASE(fg.allocators[i]);
    D3D12_RELEASE(fg.done);
    D3D12_RELEASE(fg.ready);
    D3D12_RELEASE(fg.queue);
    if (fg.ready_event)
        CloseHandle(fg.ready_event);
    if (fg.done_event)
        CloseHandle(fg.done_event);
    fg.ready_event = fg.done_event = NULL;
}

static bool fg_create_objects(void)
{
    if (fg.queue)
        return true;

    D3D12_COMMAND_QUEUE_DESC qd = { 0 };
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    // a present must not wait its turn behind a frame's rendering
    qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    bool ok = D3D_CHECK(ID3D12Device5_CreateCommandQueue(d3d.device, &qd, &IID_ID3D12CommandQueue, (void **)&fg.queue)) &&
              D3D_CHECK(ID3D12Device5_CreateFence(d3d.device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence,
                                                  (void **)&fg.ready)) &&
              D3D_CHECK(ID3D12Device5_CreateFence(d3d.device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence,
                                                  (void **)&fg.done));
    for (int i = 0; ok && i < FG_LISTS; i++)
        ok = D3D_CHECK(ID3D12Device5_CreateCommandAllocator(d3d.device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                            &IID_ID3D12CommandAllocator, (void **)&fg.allocators[i]));
    ok = ok && D3D_CHECK(ID3D12Device5_CreateCommandList(d3d.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                         fg.allocators[0], NULL, &IID_ID3D12GraphicsCommandList,
                                                         (void **)&fg.list)) &&
              D3D_CHECK(ID3D12GraphicsCommandList_Close(fg.list));
    fg.ready_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    fg.done_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!ok || !fg.ready_event || !fg.done_event) {
        fg_release_objects();
        return false;
    }
    d3d12_set_name((ID3D12Object *)fg.queue, "present queue");
    d3d12_set_name((ID3D12Object *)fg.list, "frame generation");  // DRED: an unnamed list is Flow's
    // Reflex: the presents here are out of band (the real frame's are marked by its id)
    d3d12_reflex_out_of_band_present_queue(fg.queue);
    fg.ready_value = fg.done_value = 0;
    memset(fg.allocator_done, 0, sizeof(fg.allocator_done));
    memset(fg.set_read, 0, sizeof(fg.set_read));
    fg.next_list = 0;
    return true;
}

static void fg_wait(ID3D12Fence *fence, HANDLE event, uint64_t value)
{
    // two seconds at most: a GPU that never gets there must not hang the game
    if (ID3D12Fence_GetCompletedValue(fence) >= value)
        return;
    if (SUCCEEDED(ID3D12Fence_SetEventOnCompletion(fence, value, event)))
        WaitForSingleObject(event, 2000);
}

// the present queue idle; the present thread must be drained (frontend.c)
static void fg_queue_idle(void)
{
    if (!fg.queue)
        return;
    if (SUCCEEDED(ID3D12CommandQueue_Signal(fg.queue, fg.done, ++fg.done_value)))
        fg_wait(fg.done, fg.done_event, fg.done_value);
}

bool d3d12_fg_mode(bool on)
{
    if (on == fg.active)
        return true;
    if (on && !fg_create_objects())
        return false;

    // every queue idle, the present thread drained (by the caller): the swapchain
    // moves to the other queue
    d3d12_wait_idle();
    fg_queue_idle();
    destroy_swapchain();

    fg.active = on;
    fg.num_generated = 0;
    memset(fg.set_read, 0, sizeof(fg.set_read));
    d3d.swapchain_images = on ? D3D12_FG_SWAPCHAIN_IMAGES : D3D12_SWAPCHAIN_IMAGES;
    if (create_swapchain()) {
        Com_Printf("D3D12: swapchain on the %s queue, %d buffers\n", on ? "present" : "main", d3d.swapchain_images);
        return true;
    }

    // back as it was without frame generation
    Com_EPrintf("D3D12: the swapchain could not move to the %s queue\n", on ? "present" : "main");
    destroy_swapchain();
    fg.active = false;
    d3d.swapchain_images = D3D12_SWAPCHAIN_IMAGES;
    if (!create_swapchain())
        Com_EPrintf("D3D12: no swapchain\n");
    return false;
}

bool d3d12_fg_mode_active(void)
{
    return fg.active;
}

bool d3d12_fg_sources(int generated)
{
    if (!fg.active)
        return false;
    generated = min(max(generated, 0), D3D12_FG_MAX_GENERATED);
    if (!create_sources(generated))
        return false;
    fg.num_generated = max(fg.num_generated, generated);
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE d3d12_fg_source_rtv(int set, int index)
{
    return d3d12_rtv(source_rtv_slot(set, index));
}

static void transition(ID3D12GraphicsCommandList4 *cmd, ID3D12Resource *res, D3D12_RESOURCE_STATES from,
                       D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b = { 0 };
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    ID3D12GraphicsCommandList4_ResourceBarrier(cmd, 1, &b);
}

void d3d12_fg_source_begin(ID3D12GraphicsCommandList4 *cmd, int set, int index)
{
    transition(cmd, fg.sources[set][index], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
}

void d3d12_fg_source_end(ID3D12GraphicsCommandList4 *cmd, int set, int index)
{
    transition(cmd, fg.sources[set][index], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
}

void d3d12_fg_record_real(ID3D12GraphicsCommandList4 *cmd, int set)
{
    ID3D12Resource *src = fg.sources[set][0];
    transition(cmd, d3d.frame_target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (!src)
        return;     // could not be made: the frame is not presented (d3d12_fg_present)
    transition(cmd, src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList4_CopyResource(cmd, src, d3d.frame_target);
    transition(cmd, src, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
}

void d3d12_fg_wait_set(int set)
{
    if (fg.set_read[set])
        ID3D12CommandQueue_Wait(d3d.queue, fg.done, fg.set_read[set]);
}

uint64_t d3d12_fg_signal_ready(void)
{
    if (FAILED(ID3D12CommandQueue_Signal(d3d.queue, fg.ready, fg.ready_value + 1)))
        return 0;
    return ++fg.ready_value;
}

bool d3d12_fg_ready_wait(uint64_t value)
{
    if (!value || !fg.ready)
        return false;
    fg_wait(fg.ready, fg.ready_event, value);
    return ID3D12Fence_GetCompletedValue(fg.ready) >= value;
}

bool d3d12_fg_ready_passed(uint64_t value)
{
    return value && fg.ready && ID3D12Fence_GetCompletedValue(fg.ready) > value;
}

bool d3d12_fg_present(int set, int index, uint64_t ready_value)
{
    ID3D12Resource *src = fg.sources[set][index];
    if (!fg.active || !src || !d3d.swapchain)
        return false;

    // the frame's work - the thread has normally seen it done on the host already
    if (ready_value)
        ID3D12CommandQueue_Wait(fg.queue, fg.ready, ready_value);

    // DXGI's frame latency, which the main thread leaves to this thread with frame
    // generation (d3d12_swapchain_wait): at most d3d12_frame_latency presents ahead
    // of the display. A tenth of a second at most - a late present beats none.
    if (d3d.swapchain_waitable)
        WaitForSingleObjectEx(d3d.swapchain_waitable, 100, TRUE);

    const unsigned int l = fg.next_list++ % FG_LISTS;
    fg_wait(fg.done, fg.done_event, fg.allocator_done[l]);
    if (!D3D_CHECK(ID3D12CommandAllocator_Reset(fg.allocators[l])) ||
        !D3D_CHECK(ID3D12GraphicsCommandList_Reset(fg.list, fg.allocators[l], NULL)))
        return false;

    UINT index_bb = IDXGISwapChain3_GetCurrentBackBufferIndex(d3d.swapchain);
    ID3D12Resource *backbuffer = d3d.backbuffers[index_bb % (UINT)d3d.swapchain_images];
    ID3D12GraphicsCommandList4 *cmd = (ID3D12GraphicsCommandList4 *)fg.list;
    transition(cmd, backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList_CopyResource(fg.list, backbuffer, src);
    transition(cmd, backbuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    if (!D3D_CHECK(ID3D12GraphicsCommandList_Close(fg.list)))
        return false;
    ID3D12CommandList *lists[1] = { (ID3D12CommandList *)fg.list };
    ID3D12CommandQueue_ExecuteCommandLists(fg.queue, 1, lists);

    UINT interval = vid_vsync && vid_vsync->integer ? 1 : 0;
    UINT flags = (!interval && d3d.tearing_enabled) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    HRESULT hr = IDXGISwapChain3_Present(d3d.swapchain, interval, flags);

    if (SUCCEEDED(ID3D12CommandQueue_Signal(fg.queue, fg.done, fg.done_value + 1))) {
        fg.done_value++;
        fg.allocator_done[l] = fg.done_value;
        fg.set_read[set] = fg.done_value;
    }
    return hr == DXGI_STATUS_OCCLUDED || D3D_CHECK(hr);
}

/* ------------------------------------------------------------------------- */

void d3d12_swapchain_destroy(void)
{
    d3d12_frontend_fg_drain();
    d3d12_wait_idle();
    fg_queue_idle();
    destroy_swapchain();
    fg_release_objects();
    memset(&fg, 0, sizeof(fg));
    d3d.hwnd = NULL;
}

bool d3d12_swapchain_update(void)
{
    if (!d3d.swapchain)
        return false;

    int width, height;
    window_size(&width, &height);
    if (width <= 0 || height <= 0)
        return false;   // minimized: no frame

    // vid_hdr off -> on, or back: the buffers in the other format
    const bool hdr = vid_hdr->integer != 0;
    if (!d3d.resize_pending && width == d3d.width && height == d3d.height && hdr == d3d.hdr)
        return true;
    d3d.resize_pending = false;
    if (width == d3d.width && height == d3d.height && hdr == d3d.hdr)
        return true;

    // the presents still queued refer to these buffers
    d3d12_frontend_fg_drain();
    d3d12_wait_idle();
    fg_queue_idle();
    d3d12_collect_garbage(false);
    release_buffers();

    const bool was_hdr = d3d.hdr;
    if (!resize_buffers(width, height, hdr))
        return false;
    d3d.width = width;
    d3d.height = height;
    if (!get_buffers())
        return false;

    // the window may be on another monitor now: Reflex's VRR answer for it (and the
    // screen's HDR numbers, which the metadata below is made from)
    d3d12_frontend_detect_vrr();
    set_hdr_metadata();

    if (d3d.hdr != was_hdr)
        Com_Printf("D3D12: swapchain %s\n", d3d.hdr ? "HDR (scRGB)" : "SDR");
    Com_DPrintf("D3D12: swapchain resized to %dx%d\n", width, height);
    return true;
}

void d3d12_swapchain_wait(void)
{
    // with frame generation the present thread presents, and the frontend waits for it
    if (fg.active)
        return;
    // a second at most: a swapchain that never signals must not hang the game
    if (d3d.swapchain_waitable)
        WaitForSingleObjectEx(d3d.swapchain_waitable, 1000, TRUE);
}

void d3d12_swapchain_record_copy(ID3D12GraphicsCommandList4 *cmd)
{
    UINT index = IDXGISwapChain3_GetCurrentBackBufferIndex(d3d.swapchain);
    ID3D12Resource *backbuffer = d3d.backbuffers[index % (UINT)d3d.swapchain_images];

    D3D12_RESOURCE_BARRIER b[2] = { 0 };
    b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition.pResource = d3d.frame_target;
    b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[1].Transition.pResource = backbuffer;
    b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    ID3D12GraphicsCommandList4_ResourceBarrier(cmd, 2, b);

    ID3D12GraphicsCommandList4_CopyResource(cmd, backbuffer, d3d.frame_target);

    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    ID3D12GraphicsCommandList4_ResourceBarrier(cmd, 1, &b[1]);
}

void d3d12_present(void)
{
    UINT interval = vid_vsync && vid_vsync->integer ? 1 : 0;
    // tearing is what makes vsync off actually off on a flip-model swapchain
    UINT flags = (!interval && d3d.tearing_enabled) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    HRESULT hr = IDXGISwapChain3_Present(d3d.swapchain, interval, flags);
    if (hr == DXGI_STATUS_OCCLUDED)
        return;
    D3D_CHECK(hr);
}

/* ------------------------------------------------------------------------- */
/* SCREENSHOTS                                                                */
/* ------------------------------------------------------------------------- */

// the last composed frame copied into a mapped readback buffer (release it with
// read_done); NULL if there is none
static ID3D12Resource *read_back(D3D12_PLACED_SUBRESOURCE_FOOTPRINT *fp, UINT *rows, DXGI_FORMAT *format,
                                 const byte **map)
{
    if (!d3d.frame_target)
        return NULL;

    D3D12_RESOURCE_DESC desc;
    ID3D12Resource_GetDesc(d3d.frame_target, &desc);
    *format = desc.Format;
    UINT64 row_size, total;
    ID3D12Device5_GetCopyableFootprints(d3d.device, &desc, 0, 1, 0, fp, rows, &row_size, &total);

    ID3D12Resource *readback = d3d12_create_buffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                                                   "screenshot readback");
    ID3D12GraphicsCommandList *cmd = readback ? d3d12_upload_list() : NULL;
    if (!cmd) {
        D3D12_RELEASE(readback);
        return NULL;
    }

    // Between frames the target holds the last composed frame, in COPY_SOURCE.
    // Mid-frame the upload batch still runs before the frame's own list, so it
    // reads the same thing.
    D3D12_TEXTURE_COPY_LOCATION to = { 0 }, from = { 0 };
    to.pResource = readback;
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint = *fp;
    from.pResource = d3d.frame_target;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.SubresourceIndex = 0;
    ID3D12GraphicsCommandList_CopyTextureRegion(cmd, &to, 0, 0, 0, &from, NULL);

    d3d12_upload_flush();
    d3d12_wait_idle();

    void *m = NULL;
    D3D12_RANGE range = { 0, (SIZE_T)total };
    if (!D3D_CHECK(ID3D12Resource_Map(readback, 0, &range, &m))) {
        D3D12_RELEASE(readback);
        return NULL;
    }
    *map = (const byte *)m + fp->Offset;
    return readback;
}

static void read_done(ID3D12Resource *readback)
{
    D3D12_RANGE none = { 0, 0 };
    ID3D12Resource_Unmap(readback, 0, &none);
    ID3D12Resource_Release(readback);
}

// linear (1.0 = white) to an sRGB byte
static byte srgb_byte(float v)
{
    if (!(v > 0.f))
        return 0;   // and NaN
    if (v >= 1.f)
        return 255;
    v = v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.f / 2.4f) - 0.055f;
    return (byte)(v * 255.f + 0.5f);
}

byte *d3d12_read_frame_target(int *width, int *height, int *rowbytes)
{
    // the savers write whatever comes back, so never return NULL
    int w = d3d.frame_target ? d3d.width : 1;
    int h = d3d.frame_target ? d3d.height : 1;
    int pitch = w * 3;
    byte *pixels = FS_AllocTempMem((size_t)pitch * h);
    memset(pixels, 0, (size_t)pitch * h);
    *width = w;
    *height = h;
    *rowbytes = pitch;

    if (!d3d.frame_target) {
        Com_WPrintf("D3D12: nothing drawn yet; the screenshot is black\n");
        return pixels;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    DXGI_FORMAT format;
    const byte *map;
    ID3D12Resource *readback = read_back(&fp, &rows, &format, &map);
    if (!readback)
        return pixels;

    // scRGB (the client asks HDR screens for .hdr; this is for anything else):
    // ui_hdr_nits is white, as the UI is drawn
    float white = 1.f;
    if (format == D3D12_HDR_FORMAT) {
        float nits, saturation, video_gamma;
        d3d12_frontend_ui_hdr_params(&nits, &saturation, &video_gamma);
        white = 80.f / max(nits, 1.f);
    }
    for (int y = 0; y < h && y < (int)rows; y++) {
        const byte *src = map + (size_t)y * fp.Footprint.RowPitch;
        byte *dst = pixels + (size_t)pitch * (h - y - 1);     // bottom-up, as vkpt's
        if (format == D3D12_HDR_FORMAT) {
            const uint16_t *half = (const uint16_t *)src;
            for (int x = 0; x < w; x++, half += 4, dst += 3)
                for (int c = 0; c < 3; c++)
                    dst[c] = srgb_byte(halfToFloat(half[c]) * white);
        } else {
            for (int x = 0; x < w; x++, src += 4, dst += 3) {
                dst[0] = src[2];    // BGRA -> RGB
                dst[1] = src[1];
                dst[2] = src[0];
            }
        }
    }
    read_done(readback);
    return pixels;
}

float *d3d12_read_frame_target_hdr(int *width, int *height)
{
    // IMG_SaveHDR writes whatever comes back, so never return NULL
    int w = d3d.frame_target ? d3d.width : 1;
    int h = d3d.frame_target ? d3d.height : 1;
    float *pixels = FS_AllocTempMem((size_t)w * h * 3 * sizeof(float));
    memset(pixels, 0, (size_t)w * h * 3 * sizeof(float));
    *width = w;
    *height = h;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    const byte *map;
    ID3D12Resource *readback = read_back(&fp, &rows, &format, &map);
    if (readback && format != D3D12_HDR_FORMAT) {
        read_done(readback);
        readback = NULL;
    }
    if (!readback) {
        Com_EPrintf("IMG_ReadPixelsHDR: no HDR frame to read\n");
        return pixels;
    }

    // vkpt's: the scRGB values as they are, bottom-up RGB
    for (int y = 0; y < h && y < (int)rows; y++) {
        const uint16_t *src = (const uint16_t *)(map + (size_t)y * fp.Footprint.RowPitch);
        float *dst = pixels + (size_t)w * 3 * (h - y - 1);
        for (int x = 0; x < w; x++, src += 4, dst += 3) {
            dst[0] = halfToFloat(src[0]);
            dst[1] = halfToFloat(src[1]);
            dst[2] = halfToFloat(src[2]);
        }
    }
    read_done(readback);
    return pixels;
}
