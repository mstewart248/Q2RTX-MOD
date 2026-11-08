/*
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
HDR OUTPUT: THE SCREEN, AND THE FRAME'S HDR VALUES. Both path tracers (vid_rtx 1
and 3) use this; the mapping itself is shader/tone_mapping_hdr.glsl.

1. WHAT THE SCREEN REPORTS. The screen the window is on is asked through DXGI
   (IDXGIOutput6::GetDesc1: peak, full-frame peak, black level, whether Windows HDR
   is on for it) and Windows' DisplayConfig (the "SDR content brightness" slider).
   Published read-only as vid_hdr_display_peak / _full_frame / _black / _sdr_white /
   _on, printed when they change, asked again at every new swapchain (resize, HDR
   switch, another monitor) and by `hdr_detect`. The figures are the screen's EDID,
   or the Windows HDR Calibration app's once that has been run - which is why the
   peak test pattern exists as well.

2. THE PEAK IN USE. tm_hdr_peak_nits 0 means "what the screen reports" (1000 if it
   reports nothing); the UBO gets the resolved number, so no shader sees the 0.

3. THE TEST PATTERNS (tm_hdr_calibrate) are drawn only while the HDR menu page is
   in front of the world, and the cvar goes back to 0 as soon as it is not: a
   pattern cannot be left on in play. While one is up DLSS 5 stands aside (vkpt_hdr_calibrating)
   - its LDR model would repaint the pattern.

All the HDR settings are archived here: UBO cvars are registered without
CVAR_ARCHIVE, and these are per-screen choices.
*/

#ifdef _WIN32
// DisplayConfig is Windows 7+ and its SDR white level Windows 10; the build
// otherwise targets Vista (_WIN32_WINNT 0x0600)
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#undef WINVER
#define WINVER 0x0A00
#endif

#include "shared/shared.h"
#include "common/common.h"
#include "common/cvar.h"
#include "common/cmd.h"
#include "vkpt.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi1_6.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// main.c's UBO cvars (UBO_CVAR_LIST)
extern cvar_t *cvar_tm_hdr_mode;
extern cvar_t *cvar_tm_hdr_peak_nits;
extern cvar_t *cvar_tm_hdr_paper_white;
extern cvar_t *cvar_tm_hdr_knee;
extern cvar_t *cvar_tm_hdr_highlight_contrast;
extern cvar_t *cvar_tm_hdr_sdr_gamma;
extern cvar_t *cvar_tm_hdr_saturation_scale;
extern cvar_t *cvar_tm_hdr_calibrate;
extern cvar_t *cvar_ui_hdr_nits;

#define HDR_FALLBACK_PEAK   1000.f  // a screen that reports nothing: a common HDR1000 panel

static struct {
	bool  asked;        // a detection has run
	bool  known;        // ...and the screen answered
	bool  hdr_on;       // Windows HDR is on for it
	float peak;         // nits
	float full_frame;   // nits, the whole screen white at once
	float black;        // nits
	float sdr_white;    // nits, Windows' SDR content brightness; 0 if unknown
	char  report[256];  // the last line printed, so a resize does not repeat it
} screen;

static bool calibrating;

static void publish(void)
{
	Cvar_FullSet("vid_hdr_display_peak", va("%.0f", screen.peak), CVAR_ROM, FROM_CODE);
	Cvar_FullSet("vid_hdr_display_full_frame", va("%.0f", screen.full_frame), CVAR_ROM, FROM_CODE);
	Cvar_FullSet("vid_hdr_display_black", va("%.4f", screen.black), CVAR_ROM, FROM_CODE);
	Cvar_FullSet("vid_hdr_display_sdr_white", va("%.0f", screen.sdr_white), CVAR_ROM, FROM_CODE);
	Cvar_FullSet("vid_hdr_display_on", screen.hdr_on ? "1" : "0", CVAR_ROM, FROM_CODE);
}

#ifdef _WIN32
/* A GDI device name ("\\.\DISPLAY1") against the wide one DXGI and DisplayConfig
   give (both 32 characters at most). */
static bool same_device(const WCHAR *wide, const char *gdi)
{
	for (int i = 0; i < 32; i++) {
		if (wide[i] != (WCHAR)(unsigned char)gdi[i])
			return false;
		if (!gdi[i])
			return true;
	}
	return false;
}

// dxgi.h / dxgi1_6.h's, here so nothing has to link dxguid.lib
static const GUID hdr_iid_factory1 = { 0x770aae78, 0xf26f, 0x4dba, { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } };
static const GUID hdr_iid_output6 = { 0x068346e8, 0xaaec, 0x4b84, { 0xad, 0xd7, 0x13, 0x7f, 0x51, 0x3f, 0x77, 0xa1 } };

/* DXGI's description of the output on `gdi`. dxgi.dll is loaded at run time, as the
   D3D12 renderer does, and the factory is made fresh each time: an old factory keeps
   reporting the HDR state it was made in. */
static bool query_dxgi(const char *gdi)
{
	static HMODULE dxgi_dll;
	if (!dxgi_dll)
		dxgi_dll = LoadLibraryA("dxgi.dll");
	if (!dxgi_dll)
		return false;

	typedef HRESULT (WINAPI *create_factory_t)(REFIID riid, void **factory);
	create_factory_t create = (create_factory_t)(void *)GetProcAddress(dxgi_dll, "CreateDXGIFactory1");
	if (!create)
		return false;

	IDXGIFactory1 *factory = NULL;
	if (FAILED(create(&hdr_iid_factory1, (void **)&factory)))
		return false;

	bool found = false;
	IDXGIAdapter1 *adapter = NULL;
	for (UINT a = 0; !found && factory->lpVtbl->EnumAdapters1(factory, a, &adapter) == S_OK; a++) {
		IDXGIOutput *output = NULL;
		for (UINT o = 0; !found && adapter->lpVtbl->EnumOutputs(adapter, o, &output) == S_OK; o++) {
			IDXGIOutput6 *output6 = NULL;
			if (SUCCEEDED(output->lpVtbl->QueryInterface(output, &hdr_iid_output6, (void **)&output6))) {
				DXGI_OUTPUT_DESC1 desc;
				if (SUCCEEDED(output6->lpVtbl->GetDesc1(output6, &desc)) && same_device(desc.DeviceName, gdi)) {
					screen.peak = desc.MaxLuminance;
					screen.full_frame = desc.MaxFullFrameLuminance;
					screen.black = desc.MinLuminance;
					screen.hdr_on = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
					found = true;
				}
				output6->lpVtbl->Release(output6);
			}
			output->lpVtbl->Release(output);
		}
		adapter->lpVtbl->Release(adapter);
	}
	factory->lpVtbl->Release(factory);
	return found;
}

/* Windows' SDR content brightness for the screen on `gdi`, nits (0 if unknown). It is
   the white Windows gives SDR apps on an HDR desktop. */
static float query_sdr_white(const char *gdi)
{
	UINT32 num_paths = 0, num_modes = 0;
	if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &num_paths, &num_modes) != ERROR_SUCCESS || !num_paths)
		return 0.f;

	DISPLAYCONFIG_PATH_INFO *paths = calloc(num_paths, sizeof(*paths));
	DISPLAYCONFIG_MODE_INFO *modes = calloc(num_modes ? num_modes : 1, sizeof(*modes));
	float nits = 0.f;
	if (paths && modes &&
		QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &num_paths, paths, &num_modes, modes, NULL) == ERROR_SUCCESS) {
		for (UINT32 i = 0; i < num_paths; i++) {
			DISPLAYCONFIG_SOURCE_DEVICE_NAME source;
			memset(&source, 0, sizeof(source));
			source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
			source.header.size = sizeof(source);
			source.header.adapterId = paths[i].sourceInfo.adapterId;
			source.header.id = paths[i].sourceInfo.id;
			if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
				!same_device(source.viewGdiDeviceName, gdi))
				continue;

			DISPLAYCONFIG_SDR_WHITE_LEVEL white;
			memset(&white, 0, sizeof(white));
			white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
			white.header.size = sizeof(white);
			white.header.adapterId = paths[i].targetInfo.adapterId;
			white.header.id = paths[i].targetInfo.id;
			if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS)
				nits = (float)white.SDRWhiteLevel / 1000.f * 80.f;   // 1000 = 80 nits
			break;
		}
	}
	free(paths);
	free(modes);
	return nits;
}
#endif

static void report(const char *device, bool force)
{
	char line[sizeof(screen.report)];
	if (screen.known) {
		snprintf(line, sizeof(line),
			"HDR screen %s: peak %.0f nits, full frame %.0f nits, black %.4f nits; Windows HDR %s, SDR content brightness %.0f nits",
			device, screen.peak, screen.full_frame, screen.black, screen.hdr_on ? "on" : "OFF", screen.sdr_white);
	} else {
		snprintf(line, sizeof(line), "HDR screen %s: it reports no brightness figures", device);
	}
	if (!force && !strcmp(line, screen.report))
		return;
	Q_strlcpy(screen.report, line, sizeof(screen.report));
	Com_Printf("%s\n", line);

	if (screen.known && !screen.hdr_on)
		Com_WPrintf("Windows HDR is off for this screen: an HDR (vid_hdr 1) frame is shown clipped to SDR. "
			"Turn on \"Use HDR\" in Windows' display settings.\n");
}

void vkpt_hdr_detect_display(void)
{
	const bool was_known = screen.known;
	const float was_peak = screen.peak;

	screen.asked = true;
	screen.known = false;
	screen.hdr_on = false;
	screen.peak = screen.full_frame = screen.black = screen.sdr_white = 0.f;

	const char *device = "(unknown)";
#ifdef _WIN32
	char gdi[64];
	if (vkpt_get_window_gdi_device(gdi, sizeof(gdi))) {
		device = gdi;
		screen.known = query_dxgi(gdi) && screen.peak > 0.f;
		screen.sdr_white = query_sdr_white(gdi);
	}
#endif
	publish();
	report(device, false);

	if (cvar_tm_hdr_peak_nits && cvar_tm_hdr_peak_nits->value <= 0.f &&
		(screen.known != was_known || screen.peak != was_peak))
		Com_Printf("HDR peak brightness: %.0f nits (tm_hdr_peak_nits 0 follows the screen)\n", vkpt_hdr_peak_nits());
}

/* `hdr_detect`: ask the screen again and say what is in use. */
static void HDR_Detect_f(void)
{
	screen.report[0] = 0;   // print even if nothing changed
	vkpt_hdr_detect_display();

	const float set = cvar_tm_hdr_peak_nits ? cvar_tm_hdr_peak_nits->value : 0.f;
	Com_Printf("Peak brightness in use: %.0f nits (%s). Game brightness %.0f nits, UI %.0f nits.\n",
		vkpt_hdr_peak_nits(),
		set > 0.f ? "tm_hdr_peak_nits" : screen.known ? "the screen's report" : "nothing reported, the 1000-nit default",
		cvar_tm_hdr_paper_white ? cvar_tm_hdr_paper_white->value : 0.f,
		cvar_ui_hdr_nits ? cvar_ui_hdr_nits->value : 0.f);
	if (screen.sdr_white > 0.f)
		Com_Printf("Windows gives SDR apps %.0f nits of white; tm_hdr_paper_white %.0f matches the game to them.\n",
			screen.sdr_white, screen.sdr_white);
	Com_Printf("To measure the real peak: Video > HDR options > test pattern \"peak\" (with a map loaded).\n");
}

void vkpt_hdr_register_commands(void)
{
	Cmd_AddCommand("hdr_detect", HDR_Detect_f);
}

void vkpt_hdr_unregister_commands(void)
{
	Cmd_RemoveCommand("hdr_detect");
}

void vkpt_hdr_init_cvars(void)
{
	cvar_t *archived[] = {
		cvar_tm_hdr_mode, cvar_tm_hdr_peak_nits, cvar_tm_hdr_paper_white, cvar_tm_hdr_knee,
		cvar_tm_hdr_highlight_contrast, cvar_tm_hdr_sdr_gamma, cvar_tm_hdr_saturation_scale, cvar_ui_hdr_nits,
	};
	for (int i = 0; i < (int)LENGTH(archived); i++) {
		if (archived[i])
			archived[i]->flags |= CVAR_ARCHIVE;
	}
	// the read-only figures exist from the start, for the console and the menu
	publish();
}

float vkpt_hdr_peak_nits(void)
{
	if (!screen.asked)
		vkpt_hdr_detect_display();

	float peak = cvar_tm_hdr_peak_nits ? cvar_tm_hdr_peak_nits->value : 0.f;
	if (peak <= 0.f)
		peak = screen.known ? screen.peak : HDR_FALLBACK_PEAK;
	return max(80.f, min(10000.f, peak));
}

void vkpt_hdr_prepare_ubo(QVKUniformBuffer_t *ubo, bool hdr_page_open, bool render_world)
{
	ubo->tm_hdr_peak_nits = vkpt_hdr_peak_nits();
	ubo->tm_hdr_paper_white = max(10.f, min(10000.f, ubo->tm_hdr_paper_white));

	// The test patterns exist only while the HDR page (a `style --clear-view`
	// menu) is in front, and switch off as soon as it is not: leaving the page
	// writes the menu's value back (Menu_Pop), so without this the pattern would
	// stay up over the parent menu - and when set from the console with no menu.
	if (cvar_tm_hdr_calibrate->integer != 0 && !hdr_page_open)
		Cvar_SetByVar(cvar_tm_hdr_calibrate, "0", FROM_CODE);
	calibrating = hdr_page_open && render_world && cvar_tm_hdr_calibrate->integer != 0;
	ubo->tm_hdr_calibrate = calibrating ? (float)cvar_tm_hdr_calibrate->integer : 0.f;
}

bool vkpt_hdr_calibrating(void)
{
	return calibrating;
}

void vkpt_hdr_metadata(vkpt_hdr_metadata_t *md)
{
	const float peak = vkpt_hdr_peak_nits();
	const float paper = cvar_tm_hdr_paper_white ? cvar_tm_hdr_paper_white->value : 203.f;

	md->max_mastering_nits = peak;
	md->min_mastering_nits = screen.known ? screen.black : 0.f;
	md->max_cll = peak;
	md->max_fall = min(peak, screen.known && screen.full_frame > 0.f ? screen.full_frame : max(paper, 80.f));
}
