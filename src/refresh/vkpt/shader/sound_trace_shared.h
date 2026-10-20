/*
Copyright (C) 2026 Q2RTX contributors

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

// Shared by sound_trace.c and sound_trace.comp - see sound_trace.comp.

#ifndef SOUND_TRACE_SHARED_H
#define SOUND_TRACE_SHARED_H

#define SOUND_TRACE_GROUP           64
#define SOUND_TRACE_CHANNELS        4       // first-order Ambisonics
#define SOUND_TRACE_BANDS           3
#define SOUND_TRACE_MAX_BINS        300     // 3 s of 10 ms bins: s_reflections_duration's most
#define SOUND_TRACE_MAX_SOURCES     32
#define SOUND_TRACE_MAX_MATERIALS   32

#define SOUND_TRACE_BIN_SECONDS     0.01    // Steam Audio's EnergyField::kBinDuration
#define SOUND_TRACE_SPEED_OF_SOUND  340.0   // and its PropagationMedium::kSpeedOfSound

// the ways round corners: see sound_diffract.comp
#define SOUND_DIFFRACT_GROUP        64
#define SOUND_TRACE_MAX_QUERIES     32      // a voice each (MAX_CHANNELS)
#define SOUND_DIFFRACT_FLOATS       8       // R_SOUND_DIFFRACTION_FLOATS
#define SOUND_EDGE_FLOATS           16      // R_SOUND_EDGE_FLOATS

#endif
