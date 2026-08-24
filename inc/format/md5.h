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

#ifndef FORMAT_MD5_H
#define FORMAT_MD5_H

//
// md5.h -- the rerelease's skeletal model format
//
// These are plain text files that always begin with "MD5Version 10", so the
// first four bytes are what MOD_Load dispatches on.  A model is a pair:
// <dir>/md5/tris.md5mesh holds the bind pose and the skinning weights,
// <dir>/md5/tris.md5anim holds every animation frame, and the frame count
// matches the classic <dir>/tris.md2 one for one - so entity->frame indexes
// the md5anim directly and no animation remapping is needed.
//

#define MD5_IDENT       MakeLittleLong('M','D','5','V')
#define MD5_VERSION     10

// the shader/bone index that reaches the GPU in instance_geometry.comp is a
// byte, so this is a hard limit and not just a sanity check
#define MD5_MAX_JOINTS  256

// influences per vertex that survive into the vertex buffer; the format allows
// more (the rerelease goes up to 7) and the weakest ones are dropped
#define MD5_MAX_INFLUENCES 4

#endif // FORMAT_MD5_H
