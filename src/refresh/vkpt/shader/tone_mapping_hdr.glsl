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

// ==========================================================================
//
// HDR OUTPUT (vid_hdr with tm_hdr_mode 1).
//
// The 2021 HDR path (tm_hdr_mode 0) tone-mapped exactly as SDR does and then
// multiplied the whole frame by tm_hdr_peak_nits / 80, so "peak brightness"
// was really overall brightness: a wall got 4.5x brighter going from 200 to
// 900 nits along with every lamp. That is not what a peak setting means on any
// modern display chain (HGIG, Windows' HDR guidance, BT.2408), where two
// numbers do two different jobs:
//
//   tm_hdr_paper_white  how bright ordinary scenery is: SDR white lands here
//                       (203 nits is BT.2408's HDR reference white)
//   tm_hdr_peak_nits    the screen's own limit; only highlights go near it
//
// So the HDR picture is built in three steps:
//
// 1. BELOW THE KNEE IT IS THE SDR PICTURE. The same adaptive curve blended
//    with the same Reinhard curve as SDR (same tm_white_point, no HDR-only
//    white point), up to tm_hdr_knee of SDR white. Changing the peak cannot
//    move anything down here.
//
// 2. ABOVE THE KNEE HIGHLIGHTS CARRY ON. SDR has to squeeze everything
//    brighter into the last few percent below white. HDR does not: past the
//    knee the curve keeps the SDR curve's own log-log slope there, easing over
//    one stop toward the slope tm_hdr_highlight_contrast asks for (0 = the SDR
//    image's contrast, 1 = the scene's real contrast). It never drops below
//    the SDR curve. tone_mapping_curve.comp finds the knee once per frame.
//
// 3. THE SCREEN'S LIMIT IS A ROLL-OFF, NOT A CLIP. ITU-R BT.2390's EETF, in PQ
//    (ST.2084) - perceptual - units: identity up to its knee start, then a
//    Hermite spline that lands 10000 nits exactly on tm_hdr_peak_nits with zero
//    slope. Applied to luminance, then to the brightest channel by
//    desaturating toward white just enough to keep it under the curve too -
//    the "path to white" a saturated highlight takes on film or a real screen,
//    instead of a per-channel clip that shifts its hue.
//
// Also: tm_hdr_sdr_gamma shows the range below SDR white as an SDR monitor
// does (the sRGB encode the swapchain applies, decoded with a 2.2 power), so
// shadows are not lifted against the SDR look. In linear scRGB they otherwise
// come out brighter than SDR ever showed them - the usual "washed out HDR".
//
// tm_hdr_calibrate draws test patterns over the frame while the HDR menu page
// is open (the CPU zeroes it otherwise; hdr_display.c).
//
// ==========================================================================

#ifndef TONE_MAPPING_HDR_GLSL
#define TONE_MAPPING_HDR_GLSL

// Where a luminance falls on the tone curve's bins, held inside the array.
// tone_mapping_apply.comp's own lookup is the same except that it lets the
// top bin read one past the end.
float tm_curve_position(float lum)
{
    const float b = (log2(lum) * log_luminance_scale + log_luminance_bias) * HISTOGRAM_BINS;
    return clamp(b, 0.0, float(HISTOGRAM_BINS - 1));
}

// The display luminance (1 = SDR white) the SDR tone mapper gives a grey pixel
// of scene luminance `lum`, where `curve_log` is the adaptive curve there: the
// adaptive curve blended with the Reinhard curve, as tone_mapping_apply.comp
// does it (without the SDR-only per-channel knee).
float tm_sdr_display_luminance(float curve_log, float lum, float adapted)
{
    const float adaptive = exp2(curve_log + global_ubo.tm_exposure_bias);
    const float x = exp2(global_ubo.tm_exposure_bias - 2) * lum / max(adapted, 1e-30);
    const float w2 = global_ubo.tm_white_point * global_ubo.tm_white_point;
    const float reinhard = (x * (1 + x / w2)) / (1 + x);
    return mix(adaptive, reinhard, global_ubo.tm_reinhard);
}

// The adaptive curve at `lum`, from tonemap_buffer (the apply pass's view of it)
float tm_curve_log(float lum)
{
    const float pos = tm_curve_position(lum);
    const int l = int(pos);
    const int r = min(l + 1, HISTOGRAM_BINS - 1);
    return mix(tonemap_buffer.curve[l], tonemap_buffer.curve[r], pos - float(l));
}

// The display's numbers. The CPU resolves tm_hdr_peak_nits 0 ("what the screen
// reports") before it gets here.
float tm_hdr_peak()  { return max(global_ubo.tm_hdr_peak_nits, 80.0); }
float tm_hdr_paper() { return max(global_ubo.tm_hdr_paper_white, 1.0); }

// Step 2: past the knee, the curve keeps going instead of flattening into SDR
// white. `y` is the SDR curve's value; the result is never below it.
float tm_hdr_extend(float y, float log_lum)
{
    const float y_knee = tonemap_buffer.hdr_knee_value;
    const float u = log_lum - tonemap_buffer.hdr_knee_log_lum;   // stops above the knee
    if (u <= 0 || y_knee <= 0)
        return y;

    const float g0 = tonemap_buffer.hdr_knee_slope;
    const float g1 = mix(g0, 1.0, clamp(global_ubo.tm_hdr_highlight_contrast, 0.0, 1.0));
    // the slope eases linearly from g0 to g1 over the first stop: integrated, that
    // is a parabola, then a straight line - the curve stays smooth at the knee
    const float ease = 1.0;
    const float e = (u < ease) ? g0 * u + (g1 - g0) * u * u / (2.0 * ease)
                               : g0 * u + (g1 - g0) * (u - 0.5 * ease);
    return max(y, y_knee * exp2(min(e, 64.0)));
}

// SMPTE ST.2084 (PQ), on nits
const float TM_PQ_M1 = 0.1593017578125;
const float TM_PQ_M2 = 78.84375;
const float TM_PQ_C1 = 0.8359375;
const float TM_PQ_C2 = 18.8515625;
const float TM_PQ_C3 = 18.6875;

float tm_pq_from_nits(float nits)
{
    const float y = pow(clamp(nits / 10000.0, 0.0, 1.0), TM_PQ_M1);
    return pow((TM_PQ_C1 + TM_PQ_C2 * y) / (1.0 + TM_PQ_C3 * y), TM_PQ_M2);
}

float tm_nits_from_pq(float e)
{
    const float p = pow(clamp(e, 0.0, 1.0), 1.0 / TM_PQ_M2);
    return 10000.0 * pow(max(p - TM_PQ_C1, 0.0) / (TM_PQ_C2 - TM_PQ_C3 * p), 1.0 / TM_PQ_M1);
}

// Step 3: ITU-R BT.2390 EETF from a 10000-nit source to `peak`. The knee start
// 1.5 * PQ(peak) - 0.5 is the highest that keeps the spline monotonic; at a
// 1000-nit peak it is about 300 nits, at 600 about 160. Below it, nothing moves.
float tm_eetf(float nits, float peak)
{
    const float m = tm_pq_from_nits(peak);
    const float ks = 1.5 * m - 0.5;
    const float e = tm_pq_from_nits(nits);
    if (e <= ks)
        return nits;
    const float t = (e - ks) / (1.0 - ks);
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float p = (2.0 * t3 - 3.0 * t2 + 1.0) * ks
                  + (t3 - 2.0 * t2 + t) * (1.0 - ks)
                  + (-2.0 * t3 + 3.0 * t2) * m;
    return tm_nits_from_pq(p);
}

// Nits in, nits shown. With the pause menu up (tonemap_hdr_clamp_strength,
// ramped by bloom.c) highlights are also eased under the UI's brightness, so
// menu text stays readable over an explosion behind it. Only what is brighter
// than 3/4 of that moves - unlike the 2021 path, which dimmed the whole frame
// toward ui_hdr_nits. The HDR page itself (`style --clear-view`) gets neither
// this nor the pause blur: it is set by eye against the world as it plays.
float tm_hdr_display_map(float nits, bool menu_cap)
{
    const float peak = tm_hdr_peak();
    float n = tm_eetf(nits, peak);

    const float s = menu_cap ? global_ubo.tonemap_hdr_clamp_strength : 0.0;
    const float cap = max(global_ubo.ui_hdr_nits, tm_hdr_paper());
    if (s > 0 && cap < peak)
    {
        const float k = 0.75 * cap;
        if (n > k)
        {
            const float t = (n - k) / (cap - k);
            n = mix(n, k + (cap - k) * t / (1.0 + t), s);
        }
    }
    return n;
}

// The range below SDR white as an SDR monitor shows it: encoded with the
// piecewise sRGB curve (what an sRGB swapchain does), decoded with a plain
// power (what the monitor does). Only the deep shadows really move: at 2.2,
// 1% grey comes out at 0.63%, 0.2% at 0.03%; above about 20% the two agree
// within 2%. Identity from SDR white up. 0 = off.
vec3 tm_hdr_sdr_gamma(vec3 c)
{
    const float g = global_ubo.tm_hdr_sdr_gamma;
    if (g <= 0)
        return c;
    const vec3 sdr = pow(linear_to_srgb(clamp(c, vec3(0), vec3(1))), vec3(g));
    return mix(sdr, c, step(vec3(1), c));
}

// The whole HDR mapping, scene-linear in, scRGB (1.0 = 80 nits) out.
vec3 hdr_tone_map(vec3 input_color, float lum, bool menu_cap)
{
    // 1 and 2: the SDR curve, continued past the knee
    float y = tm_sdr_display_luminance(tm_curve_log(lum), lum, tonemap_buffer.adapted_luminance);
    y = tm_hdr_extend(y, log2(lum));

    // hue and saturation kept, as the SDR mapping does: 1.0 = SDR white
    vec3 c = input_color * (y / lum);
    c = tm_hdr_sdr_gamma(c);
    c = apply_saturation_scale(c, global_ubo.tm_hdr_saturation_scale * 0.01);
    c *= tm_hdr_paper();

    // 3: the roll-off, on luminance, then the brightest channel toward white
    const float Y = luminance(c);
    if (Y > 0)
    {
        const float Yd = tm_hdr_display_map(Y, menu_cap);
        c *= Yd / Y;
        const float m = max(c.r, max(c.g, c.b));
        if (m > Yd)
        {
            const float md = tm_hdr_display_map(m, menu_cap);
            c = Yd + (c - Yd) * clamp((md - Yd) / (m - Yd), 0.0, 1.0);
        }
    }

    c /= 80.0;   // scRGB
    // tm_hdr_clip: what is left over - DLSS-RR ringing below zero, rounding above the peak
    if (global_ubo.tm_hdr_clip != 0)
        c = clamp(c, vec3(0), vec3(tm_hdr_peak() / 80.0));
    return c;
}

// tm_hdr_calibrate, drawn over the frame while a menu is open:
//
// 1  PEAK. Two squares, each a field at 10000 nits around a centre at
//    tm_hdr_peak_nits, in a black frame. The screen clips both at its real
//    peak, so: raise peak brightness until the centres just vanish. That value
//    is the screen's peak (the method of the Windows HDR Calibration app).
//    Works in either tm_hdr_mode.
//
// 2  STEPS. 24 grey patches along the top, one photographic stop apart, from
//    16 stops below the knee to 7 above, through the real mapping (no menu
//    cap). Patches that look the same at the dark end are crushed shadows
//    (tm_hdr_sdr_gamma, game brightness); at the bright end they show where
//    highlights roll into the peak. tm_hdr_mode 1 only.
vec3 hdr_test_pattern(vec3 c, ivec2 ipos, ivec2 size)
{
    const int pattern = int(global_ubo.tm_hdr_calibrate + 0.5);
    const vec2 p = vec2(ipos) + 0.5;
    const float w = float(size.x);
    const float h = float(size.y);

    // Both patterns stay out of the middle of the screen: the menu's band is
    // drawn there, over the world, and its backdrop would dim them.
    if (pattern == 1)
    {
        const float half_size = 0.08 * h;
        for (int i = 0; i < 2; i++)
        {
            const vec2 centre = vec2(i == 0 ? 0.11 * w : 0.89 * w, 0.2 * h);
            const vec2 d = abs(p - centre);
            const float r = max(d.x, d.y);
            if (r < half_size * 1.15)
            {
                if (r >= half_size)
                    return vec3(0);
                return vec3(r < half_size * 0.4 ? tm_hdr_peak() : 10000.0) / 80.0;
            }
        }
    }
    else if (pattern == 2 && global_ubo.tm_hdr_mode != 0)
    {
        const vec2 tl = vec2(0.04 * w, 0.04 * h);
        const vec2 br = vec2(0.96 * w, 0.11 * h);
        if (all(greaterThanEqual(p, tl)) && all(lessThan(p, br)))
        {
            const int steps = 24;
            const int i = min(int((p.x - tl.x) / (br.x - tl.x) * float(steps)), steps - 1);
            const float lum = exp2(tonemap_buffer.hdr_knee_log_lum + float(i - 16));
            return hdr_tone_map(vec3(lum), lum, false);
        }
    }
    return c;
}

#endif // TONE_MAPPING_HDR_GLSL
