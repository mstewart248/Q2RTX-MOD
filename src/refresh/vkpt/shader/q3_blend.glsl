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

// Needs constants.h first. Shared by q3_stages.glsl and q3_sky.glsl.

#ifndef Q3_BLEND_GLSL_
#define Q3_BLEND_GLSL_

// A Quake III blendFunc factor (Q3P_BF_*), as GL applied it to src and dst.
vec4 q3_blend_factor(uint f, vec4 src, vec4 dst)
{
	switch (f)
	{
	case Q3P_BF_ZERO:                 return vec4(0);
	case Q3P_BF_DST_COLOR:            return dst;
	case Q3P_BF_ONE_MINUS_DST_COLOR:  return vec4(1) - dst;
	case Q3P_BF_SRC_ALPHA:            return vec4(src.a);
	case Q3P_BF_ONE_MINUS_SRC_ALPHA:  return vec4(1.0 - src.a);
	case Q3P_BF_DST_ALPHA:            return vec4(dst.a);
	case Q3P_BF_ONE_MINUS_DST_ALPHA:  return vec4(1.0 - dst.a);
	case Q3P_BF_SRC_COLOR:            return src;
	case Q3P_BF_ONE_MINUS_SRC_COLOR:  return vec4(1) - src;
	case Q3P_BF_SRC_ALPHA_SATURATE:   { float s = min(src.a, 1.0 - dst.a); return vec4(s, s, s, 1); }
	default:                          return vec4(1);   // Q3P_BF_ONE
	}
}

#endif // Q3_BLEND_GLSL_
