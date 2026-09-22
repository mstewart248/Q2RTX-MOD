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

/*
CAMERA PROJECTIONS.

Every mode is a matched pair: a FORWARD map from a view-space position to a
screen position in [0,1]^2, and a REVERSE map from a screen position back to a
view-space point at a given radial distance. primary_rays.rgen and
reflect_refract.rgen trace through the reverse map; the forward map is what the
motion vectors and every temporal reprojection are built from, so the two have
to be exact inverses of each other or the picture crawls.

Only PROJECTION_RECTILINEAR uses the projection MATRIX. Cylindrical uses P only
for its vertical scale; the four curved modes ignore P and invP entirely and are
scaled instead by global_ubo.projection_fov_scale, which prepare_ubo computes on
the CPU from the current FOV. That is also why `previous` threads all the way
down: a FOV change has to reproject through LAST frame's scale.

Note that the curved modes cover more than a hemisphere at wide FOV, so
`view_pos.z > 0` is not a visibility test there and the forward maps return true
unconditionally - a point behind the camera still has a well-defined screen
position. The rectilinear map keeps its frustum test.
*/

void view_to_lonlat(vec3 view, out float lon, out float lat)
{
	lon = atan(view.x, view.z);
	lat = atan(-view.y, sqrt(view.x * view.x + view.z * view.z));
}

void lonlat_to_view(float lon, float lat, out vec3 view)
{
	view.x = sin(lon) * cos(lat);
	view.y = -sin(lat);
	view.z = cos(lon) * cos(lat);
}

vec2 get_projection_fov_scale(bool previous)
{
	return previous ? global_ubo.projection_fov_scale_prev : global_ubo.projection_fov_scale;
}

/* True when the camera has a real projection matrix behind it. The DLSS linear-Z
   path keys off this: NGX is fed view-space Z only when the motion vectors were
   built from P, and radial distance everywhere else. See primary_rays.rgen. */
bool projection_is_rectilinear()
{
	return global_ubo.pt_projection == PROJECTION_RECTILINEAR;
}

bool rectilinear_forward(vec3 view_pos, out vec2 screen_pos, out float distance, bool previous)
{
	vec4 clip_pos;
	if (previous)
		clip_pos = global_ubo.P_prev * vec4(view_pos, 1);
	else
		clip_pos = global_ubo.P * vec4(view_pos, 1);

	vec3 normalized = clip_pos.xyz / clip_pos.w;
	screen_pos.xy = normalized.xy * 0.5 + vec2(0.5);
	distance = length(view_pos);

	return screen_pos.y > 0 && screen_pos.y < 1 && screen_pos.x > 0 && screen_pos.x < 1 && view_pos.z > 0;
}

vec3 rectilinear_reverse(vec2 screen_pos, float distance, bool previous)
{
	vec4 clip_pos = vec4(screen_pos.xy * 2.0 - vec2(1.0), 1, 1);
	vec3 view_dir;
	if (previous)
		view_dir = normalize((global_ubo.invP_prev * clip_pos).xyz);
	else
		view_dir = normalize((global_ubo.invP * clip_pos).xyz);

	return view_dir * distance;
}

bool cylindrical_forward(vec3 view_pos, out vec2 screen_pos, out float distance, bool previous)
{
	float cylindrical_hfov = previous ? global_ubo.cylindrical_hfov_prev : global_ubo.cylindrical_hfov;

	float y = view_pos.y / length(view_pos.xz);
	if (previous)
		y *= global_ubo.P_prev[1][1];
	else
		y *= global_ubo.P[1][1];
	screen_pos.y = y * 0.5 + 0.5;

	float angle = atan(view_pos.x, view_pos.z);
	screen_pos.x = (angle / cylindrical_hfov) + 0.5;

	distance = length(view_pos);

	return screen_pos.y > 0 && screen_pos.y < 1 && screen_pos.x > 0 && screen_pos.x < 1;
}

vec3 cylindrical_reverse(vec2 screen_pos, float distance, bool previous)
{
	float cylindrical_hfov = previous ? global_ubo.cylindrical_hfov_prev : global_ubo.cylindrical_hfov;

	vec4 clip_pos = vec4(0, screen_pos.y * 2.0 - 1.0, 1, 1);
	vec3 view_dir;
	if (previous)
		view_dir = (global_ubo.invP_prev * clip_pos).xyz;
	else
		view_dir = (global_ubo.invP * clip_pos).xyz;

	float xangle = (screen_pos.x - 0.5) * cylindrical_hfov;
	view_dir.x = sin(xangle);
	view_dir.z = cos(xangle);

	view_dir = normalize(view_dir);

	return view_dir * distance;
}

/* Plate carree: longitude and latitude map straight to x and y. Constant angular
   resolution everywhere, which is what makes it the useful one for 360 captures. */
bool equirectangular_forward(vec3 view_pos, out vec2 screen_pos, out float distance, bool previous)
{
	float lat, lon;
	distance = length(view_pos);
	view_pos = normalize(view_pos);
	view_to_lonlat(view_pos, lon, lat);
	screen_pos.x = lon;
	screen_pos.y = lat;
	screen_pos = screen_pos / get_projection_fov_scale(previous) * 0.5 + 0.5;
	return true;
}

vec3 equirectangular_reverse(vec2 screen_pos, float distance, bool previous)
{
	vec3 view_dir;
	screen_pos = (screen_pos * 2.0 - 1.0) * get_projection_fov_scale(previous);
	lonlat_to_view(screen_pos.x, screen_pos.y, view_dir);
	return view_dir * distance;
}

/* Mercator: conformal, so local shapes survive, at the cost of a vertical scale
   that runs away towards the poles. */
bool mercator_forward(vec3 view_pos, out vec2 screen_pos, out float distance, bool previous)
{
	float lat, lon;
	distance = length(view_pos);
	view_pos = normalize(view_pos);
	view_to_lonlat(view_pos, lon, lat);

	/* log(tan(pi/4 + lat/2)) is +-infinity at the poles, and a point straight up
	   or straight down is an ordinary thing for the motion vectors to ask about.
	   Clamp just short of vertical: the value is already far off screen there, and
	   an Inf in PT_MOTION poisons DLSS for the whole frame. */
	const float lat_limit = float(M_PI) * 0.5 - 1e-3;
	lat = clamp(lat, -lat_limit, lat_limit);

	screen_pos.x = lon;
	screen_pos.y = log(tan(float(M_PI) * 0.25 + lat * 0.5));
	screen_pos = screen_pos / get_projection_fov_scale(previous) * 0.5 + 0.5;
	return true;
}

vec3 mercator_reverse(vec2 screen_pos, float distance, bool previous)
{
	vec3 view_dir;
	screen_pos = (screen_pos * 2.0 - 1.0) * get_projection_fov_scale(previous);
	float lon = screen_pos.x;
	float lat = atan(sinh(screen_pos.y));
	lonlat_to_view(lon, lat, view_dir);
	return view_dir * distance;
}

/* Stereographic ("little planet") fisheye. STEREOGRAPHIC_ANGLE is the fraction of
   the polar angle carried to the plane; 0.5 is the true stereographic map. */
bool stereographic_forward(vec3 view_pos, out vec2 screen_pos, out float distance, bool previous)
{
	distance = length(view_pos);
	view_pos = normalize(view_pos);
	float x = view_pos.x;
	float y = -view_pos.y;
	float z = view_pos.z;
	float theta = acos(clamp(z, -1.0, 1.0));
	float rxy = sqrt(x * x + y * y);
	if (theta == 0.0 || rxy == 0.0)
	{
		/* Dead centre (or the exact antipode): the direction carries no azimuth,
		   so there is nothing to scale. Upstream tests only theta and still
		   divides by rxy. */
		screen_pos = vec2(0.0, 0.0);
	}
	else
	{
		float r = tan(theta * STEREOGRAPHIC_ANGLE);
		float c = r / rxy;
		screen_pos.x = x * c;
		screen_pos.y = y * c;
	}
	screen_pos = screen_pos / get_projection_fov_scale(previous) * 0.5 + 0.5;
	return true;
}

vec3 stereographic_reverse(vec2 screen_pos, float distance, bool previous)
{
	vec3 view_dir;
	screen_pos = (screen_pos * 2.0 - 1.0) * get_projection_fov_scale(previous);
	float x = screen_pos.x;
	float y = screen_pos.y;
	float r = sqrt(x * x + y * y);

	/* The centre pixel has r == 0 and x/r would be NaN. It is on screen, and with
	   an unjittered pass it is hit exactly, so this is not a theoretical case. */
	if (r < 1e-6)
		return vec3(0, 0, 1) * distance;

	float theta = atan(r) / STEREOGRAPHIC_ANGLE;
	float s = sin(theta);
	view_dir.x = x / r * s;
	view_dir.y = -y / r * s;
	view_dir.z = cos(theta);
	return view_dir * distance;
}

/* Panini: cylindrical about the vertical axis, stereographic across it. Keeps
   vertical lines straight at wide FOV, which is what makes it the usable one for
   actually playing at 120+ degrees. PANINI_D picks the variant - see constants.h. */
bool panini_forward(vec3 view_pos, out vec2 screen_pos, out float distance, bool previous)
{
	float lat, lon;
	distance = length(view_pos);
	view_pos = normalize(view_pos);
	view_to_lonlat(view_pos, lon, lat);

	/* S blows up as cos(lon) approaches -PANINI_D, i.e. at the projection's own
	   horizon behind the camera. Anything there is far off screen already. */
	float denom = PANINI_D + cos(lon);
	float S = (PANINI_D + 1.0) / max(denom, 1e-3);

	/* tan(lat) is infinite straight up and down for the same reason mercator's log
	   is - clamp it rather than hand an Inf to the motion vectors. */
	const float lat_limit = float(M_PI) * 0.5 - 1e-3;
	lat = clamp(lat, -lat_limit, lat_limit);

	screen_pos.x = S * sin(lon);
	screen_pos.y = S * tan(lat);
	screen_pos = screen_pos / get_projection_fov_scale(previous) * 0.5 + 0.5;
	return true;
}

vec3 panini_reverse(vec2 screen_pos, float distance, bool previous)
{
	const float c_D = PANINI_D;
	vec3 view_dir;
	screen_pos = (screen_pos * 2.0 - 1.0) * get_projection_fov_scale(previous);
	float k = screen_pos.x * screen_pos.x / ((c_D + 1.0) * (c_D + 1.0));
	float dscr = k * k * c_D * c_D - (k + 1.0) * (k * c_D * c_D - 1.0);
	/* The quadratic has no real root outside the projection's valid disc; clamping
	   to zero lands on the horizon instead of returning NaN. */
	float clon = (-k * c_D + sqrt(max(dscr, 0.0))) / (k + 1.0);
	float S = (c_D + 1.0) / (c_D + clon);
	float lon = atan(screen_pos.x, S * clon);
	float lat = atan(screen_pos.y, S);
	lonlat_to_view(lon, lat, view_dir);
	return view_dir * distance;
}

bool projection_view_to_screen(vec3 view_pos, out vec2 screen_pos, out float distance, bool previous)
{
	switch (global_ubo.pt_projection)
	{
	default:
	case PROJECTION_RECTILINEAR:
		return rectilinear_forward(view_pos, screen_pos, distance, previous);
	case PROJECTION_PANINI:
		return panini_forward(view_pos, screen_pos, distance, previous);
	case PROJECTION_STEREOGRAPHIC:
		return stereographic_forward(view_pos, screen_pos, distance, previous);
	case PROJECTION_CYLINDRICAL:
		return cylindrical_forward(view_pos, screen_pos, distance, previous);
	case PROJECTION_EQUIRECTANGULAR:
		return equirectangular_forward(view_pos, screen_pos, distance, previous);
	case PROJECTION_MERCATOR:
		return mercator_forward(view_pos, screen_pos, distance, previous);
	}
}

vec3 projection_screen_to_view(vec2 screen_pos, float distance, bool previous)
{
	switch (global_ubo.pt_projection)
	{
	default:
	case PROJECTION_RECTILINEAR:
		return rectilinear_reverse(screen_pos, distance, previous);
	case PROJECTION_PANINI:
		return panini_reverse(screen_pos, distance, previous);
	case PROJECTION_STEREOGRAPHIC:
		return stereographic_reverse(screen_pos, distance, previous);
	case PROJECTION_CYLINDRICAL:
		return cylindrical_reverse(screen_pos, distance, previous);
	case PROJECTION_EQUIRECTANGULAR:
		return equirectangular_reverse(screen_pos, distance, previous);
	case PROJECTION_MERCATOR:
		return mercator_reverse(screen_pos, distance, previous);
	}
}
