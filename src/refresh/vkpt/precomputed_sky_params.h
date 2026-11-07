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

/* The sky constant buffer's layout (precomputed_sky.glsl's AtmosphereParameters)
   and the two planets' values. No engine or API header: the D3D12 renderer's
   headless test (d3d12/tests/d3d12_world_test.c) renders the sky with them. */

#ifndef  __PRECOMPUTED_SKY_PARAMS_H__
#define  __PRECOMPUTED_SKY_PARAMS_H__

/*-----------------------------------------------------------------------------
	Structures for sky constant buffer
-----------------------------------------------------------------------------*/

struct AtmosphereParameters
{
	float solar_irradiance[3];
	float sun_angular_radius;

	float rayleigh_scattering[3];
	float bottom_radius;

	float mie_scattering[3];
	float top_radius;

	float mie_phase_function_g;
	float SqDistanceToHorizontalBoundary;
	float AtmosphereHeight;
	float reserved;
};

#define EARTH_SURFACE_RADIUS (6360.0f)
#define EARTH_ATMOSPHERE_RADIUS (6420.f)
#define STROGGOS_SURFACE_RADIUS (6360.0f)
#define STROGGOS_ATMOSPHERE_RADIUS (6520.f)
#define DIST_TO_HORIZON(LOW,HIGH) ( HIGH*HIGH - LOW*LOW )

static const struct AtmosphereParameters Params_Earth = {
	.solar_irradiance = { 1.47399998f, 1.85039997f, 1.91198003f },
	.sun_angular_radius = 0.00467499997f,
	.rayleigh_scattering = { 0.00580233941f, 0.0135577619f, 0.0331000052f },
	.bottom_radius = EARTH_SURFACE_RADIUS,
	.mie_scattering = { 0.0014985f, 0.0014985f, 0.0014985f },
	.top_radius = EARTH_ATMOSPHERE_RADIUS,
	.mie_phase_function_g = 0.8f,
	.SqDistanceToHorizontalBoundary = DIST_TO_HORIZON(EARTH_SURFACE_RADIUS, EARTH_ATMOSPHERE_RADIUS),
	.AtmosphereHeight = EARTH_ATMOSPHERE_RADIUS - EARTH_SURFACE_RADIUS,
};

static const struct AtmosphereParameters Params_Stroggos = {
	.solar_irradiance = { 2.47399998, 1.85039997, 1.01198006 },
	.sun_angular_radius = 0.00934999995f,
	.rayleigh_scattering = { 0.0270983186, 0.0414223559, 0.0647224262 },
	.bottom_radius = STROGGOS_SURFACE_RADIUS,
	.mie_scattering = { 0.00342514296, 0.00342514296, 0.00342514296 },
	.top_radius = STROGGOS_ATMOSPHERE_RADIUS,
	.mie_phase_function_g = 0.9f,
	.SqDistanceToHorizontalBoundary = DIST_TO_HORIZON(STROGGOS_SURFACE_RADIUS, STROGGOS_ATMOSPHERE_RADIUS),
	.AtmosphereHeight = STROGGOS_ATMOSPHERE_RADIUS - STROGGOS_SURFACE_RADIUS,
};

#endif  /*__PRECOMPUTED_SKY_PARAMS_H__*/
