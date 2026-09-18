/*
Copyright (C) 1997-2001 Id Software, Inc.

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
// kexdemo.c -- play back .dm2 files recorded by the Quake II 2023 remaster
//
// The remaster (KEX engine) writes demos in a protocol of its own: 2022 in
// every shipped .dm2, 2023 on the wire. It is recognisably Quake II - same
// message framing, same delta compression scheme, mostly the same server
// command numbers - but every structure it carries has been widened, and the
// tables those structures index into have moved. Rather than turn the whole
// client into a second engine, this file translates a KEX message stream into
// the state the existing client already knows how to render, once, at parse
// time.  Nothing here is ever written: the client still records and serves
// protocol 34/35/36.
//
// What is translated, and what is necessarily lost:
//
//   configstrings  KEX raised MAX_MODELS to 8192 and inserted a shadow light
//                  table, so every section sits at a different index. They are
//                  remapped into ours (CL_KexDemo_RemapConfigstring). Sections
//                  we have no room or no equivalent for - shadow lights, the
//                  weapon wheel, anything past our capacity - are dropped.
//
//   entity states  Origins are floats (or, in protocol 2022, sometimes 16-bit
//                  fixed point - see below), angles are always floats, effects
//                  are 64 bits wide, and there are per-entity alpha, scale,
//                  owner, old_frame and split-screen instance fields. We keep
//                  everything entity_state_t has a home for, including the
//                  alpha and scale this fork already added, and drop the rest.
//
//   player state   Origin and velocity are floats where ours are 12.3 shorts,
//                  pm_flags is 16 bits, pm_type gained two values, and there
//                  are 64 stats rather than 32. Converted; stats above 32,
//                  damage blend, team id and gun skin are dropped.
//
//   temp entities  Same numbering as ours through TE_FLECHETTE, then both
//                  sides appended their own. Translated by table.
//
//   frame rate     The rerelease game runs at 40 Hz, not 10. The serverdata
//                  carries the rate and CL_SetServerFrameTime applies it, or
//                  the demo would play back at a quarter speed.
//
// The wire format was cross-checked against src/rerelease/game.h (id's own
// published game headers) and against q2proto (GPLv2,
// https://github.com/res2k/q2proto), whose KEX reader documents the quirks the
// headers do not, then verified by parsing every .dm2 id ships with the
// remaster end to end with no byte left over.

#include "client.h"
#include "common/protocol_kex.h"

#if USE_ZLIB
#include <zlib.h>
#endif

/*
=====================================================================

  STATE

=====================================================================
*/

// Protocol 2022 picks the precision of an entity's origin fields from its
// `solid`: non-zero solid means 32-bit floats, zero solid means 16-bit fixed
// point. `solid` is itself delta compressed, so a frame that does not resend it
// still needs to know what it was - which means tracking it per entity for the
// whole demo, not just within one delta.
//
// The baseline copy exists because U_REMOVE returns an entity to its baseline:
// the next delta that resurrects it is encoded against the baseline's solid,
// not against whatever the entity had when it was removed.
static byte kex_solid_bits[(MAX_EDICTS + 7) / 8];
static byte kex_baseline_solid_bits[(MAX_EDICTS + 7) / 8];

// One-shot warnings, so a demo that overflows one of our tables says so once
// instead of once per frame.
static bool kex_warned_modelindex;
static bool kex_warned_configstring;
static bool kex_warned_muzzleflash;

#if USE_ZLIB
// svc_configblast and svc_spawnbaselineblast are zlib streams (header and all,
// unlike svc_zpacket's raw deflate), so they need their own inflate state.
static z_stream kex_z;
static bool     kex_z_inited;
#endif

static inline bool kex_get_solid(const byte *bits, int num)
{
    return (bits[num >> 3] & (1 << (num & 7))) != 0;
}

static inline void kex_set_solid(byte *bits, int num, bool value)
{
    if (value)
        bits[num >> 3] |= 1 << (num & 7);
    else
        bits[num >> 3] &= ~(1 << (num & 7));
}

/*
=====================================================================

  PRIMITIVES

=====================================================================
*/

// The engine has no float reader: everything it speaks quantises coordinates.
// KEX does not, so one is needed here.
static float MSG_ReadFloat(void)
{
    union {
        uint32_t u;
        float    f;
    } dat;

    dat.u = (uint32_t)MSG_ReadLong();
    return dat.f;
}

static void kex_read_pos_float(vec3_t pos)
{
    pos[0] = MSG_ReadFloat();
    pos[1] = MSG_ReadFloat();
    pos[2] = MSG_ReadFloat();
}

static void kex_read_pos_short(vec3_t pos)
{
    pos[0] = SHORT2COORD(MSG_ReadShort());
    pos[1] = SHORT2COORD(MSG_ReadShort());
    pos[2] = SHORT2COORD(MSG_ReadShort());
}

// Position encoding for the *game* messages - temp entities, sound origins.
// Protocol 2022 keeps id's original 12.3 shorts here even though it uses floats
// in entity and player state; 2023 went float everywhere.
static void kex_read_game_pos(vec3_t pos)
{
    if (cl.kex_protocol == PROTOCOL_VERSION_KEX_DEMOS)
        kex_read_pos_short(pos);
    else
        kex_read_pos_float(pos);
}

/*
=====================================================================

  CONFIGSTRINGS

=====================================================================
*/

// Translate a KEX configstring index into ours, or return -1 for something we
// have no room or no equivalent for. The sections are in the same order in both
// layouts, so this is a per-section base swap plus a capacity check.
int CL_KexDemo_RemapConfigstring(int index)
{
    int i;

    if (index < 0 || index >= KEX_MAX_CONFIGSTRINGS)
        return -1;

    // The fixed strings at the bottom - level name, cd track, sky, sky axis,
    // sky rotation - sit at the same indices in both layouts.
    if (index < KEX_CS_STATUSBAR)
        return index;

    // The status bar is one long program that overflows into the slots above
    // it, and both engines read it back as a single string starting at
    // CS_STATUSBAR. KEX broadcasts it as the whole string at CS_STATUSBAR AND
    // again as a 96 byte window per following slot (so its index 6 is the same
    // program from byte 96 on, index 7 from byte 192, and so on). Our slots are
    // MAX_QPATH wide, so writing those windows at the matching index would
    // overwrite the middle of the program we already stored correctly. Take the
    // whole string at CS_STATUSBAR and drop every continuation slot.
    //
    // A program longer than CS_SIZE(CS_STATUSBAR) - 1536 bytes, against KEX's
    // 5184 - still gets truncated, with the warning CL_ParseConfigstring prints.
    if (index < KEX_CS_AIRACCEL)
        return index == KEX_CS_STATUSBAR ? CS_STATUSBAR : -1;

    if (index == KEX_CS_AIRACCEL)
        return CS_AIRACCEL;
    if (index == KEX_CS_MAXCLIENTS)
        return CS_MAXCLIENTS;
    if (index == KEX_CS_MAPCHECKSUM)
        return CS_MAPCHECKSUM;

    if (index < KEX_CS_SOUNDS) {
        i = index - KEX_CS_MODELS;
        return i < MAX_MODELS ? CS_MODELS + i : -1;
    }
    if (index < KEX_CS_IMAGES) {
        i = index - KEX_CS_SOUNDS;
        return i < MAX_SOUNDS ? CS_SOUNDS + i : -1;
    }
    if (index < KEX_CS_LIGHTS) {
        i = index - KEX_CS_IMAGES;
        return i < MAX_IMAGES ? CS_IMAGES + i : -1;
    }
    if (index < KEX_CS_SHADOWLIGHTS) {
        i = index - KEX_CS_LIGHTS;
        return i < MAX_LIGHTSTYLES ? CS_LIGHTS + i : -1;
    }
    // Shadow lights are a rerelease renderer feature with no counterpart here,
    // and the path tracer lights the scene from the map anyway.
    if (index < KEX_CS_ITEMS)
        return -1;
    if (index < KEX_CS_PLAYERSKINS) {
        i = index - KEX_CS_ITEMS;
        return i < MAX_ITEMS ? CS_ITEMS + i : -1;
    }
    if (index < KEX_CS_GENERAL) {
        i = index - KEX_CS_PLAYERSKINS;
        return i < MAX_CLIENTS ? CS_PLAYERSKINS + i : -1;
    }
    if (index < KEX_CS_WHEEL_WEAPONS) {
        i = index - KEX_CS_GENERAL;
        return i < MAX_GENERAL ? CS_GENERAL + i : -1;
    }

    // Weapon wheel tables, CD loop count and game style are all rerelease HUD
    // features we do not implement.
    return -1;
}

// Read one KEX configstring off the wire and feed it to the client under our
// own index. Dropped indices still have to be READ - the string is in the
// message either way.
static void CL_KexDemo_ParseConfigstring(void)
{
    int  kex_index = MSG_ReadWord();
    int  index = CL_KexDemo_RemapConfigstring(kex_index);
    char string[KEX_CS_MAX_STRING_LENGTH];

    if (index < 0) {
        MSG_ReadString(string, sizeof(string));
        if (!kex_warned_configstring) {
            kex_warned_configstring = true;
            Com_DPrintf("%s: dropping configstring %d (no equivalent)\n",
                        __func__, kex_index);
        }
        return;
    }

    // CL_ParseConfigstring reads the string itself, straight out of msg_read
    // into cl.configstrings[index], and does all the "apply this" work.
    CL_ParseConfigstring(index);
}

/*
=====================================================================

  ENTITY STATES

=====================================================================
*/

// KEX added a fifth bits byte, so unlike MSG_ParseEntityBits the mask does not
// fit in an int.
int CL_KexDemo_ParseEntityBits(uint64_t *bits)
{
    uint64_t total;
    int      number;

    total = (uint64_t)MSG_ReadByte();
    if (total & UK_MOREBITS1)
        total |= (uint64_t)MSG_ReadByte() << 8;
    if (total & UK_MOREBITS2)
        total |= (uint64_t)MSG_ReadByte() << 16;
    if (total & UK_MOREBITS3)
        total |= (uint64_t)MSG_ReadByte() << 24;
    if (total & UK_MOREBITS4)
        total |= (uint64_t)MSG_ReadByte() << 32;

    if (total & UK_NUMBER16)
        number = MSG_ReadWord();
    else
        number = MSG_ReadByte();

    *bits = total;
    return number;
}

// KEX model indices run to 8191; ours to MAX_MODELS. An index we cannot store
// would run off the end of cl.model_draw, so clamp it to "no model" and say so
// once. In practice no demo id ships comes close.
static int kex_clamp_modelindex(int index)
{
    if (index < 0 || index >= MAX_MODELS) {
        if (!kex_warned_modelindex) {
            kex_warned_modelindex = true;
            Com_WPrintf("Demo uses model index %d, beyond this client's "
                        "limit of %d. Some models will be missing.\n",
                        index, MAX_MODELS - 1);
        }
        return 0;
    }
    return index;
}

// See the clash table in protocol_kex.h. Everything vanilla defined lines up;
// only the bits each side invented for itself have to be moved.
static uint32_t kex_translate_effects(uint64_t effects)
{
    uint32_t out = (uint32_t)effects & ~(uint32_t)EFK_BOB;

    // We have no "bob in place" flag, so bonus items simply sit still - which
    // is a great deal better than detonating, which is what bit 2 means here.
    if (effects & EFK_BARREL_EXPLODING)
        out |= EF_BARREL_EXPLODING;

    return out;
}

static int kex_translate_renderfx(uint32_t renderfx)
{
    uint32_t out = renderfx & ~(RFK_SHELL_LITE_GREEN | RFK_OLD_FRAME_LERP | RFK_UNSUPPORTED);

    // The pale green shell is close enough to ours to be worth keeping; the
    // bit it arrives on would otherwise mean "draw in primary rays only".
    if (renderfx & RFK_SHELL_LITE_GREEN)
        out |= RF_SHELL_GREEN;

    return out;
}

void CL_KexDemo_ParseDeltaEntity(const entity_state_t *from, entity_state_t *to,
                                 int number, uint64_t bits)
{
    bool     model16, high_precision;
    uint32_t low_effects = 0, high_effects = 0;

    if (!to)
        Com_Error(ERR_DROP, "%s: NULL", __func__);

    if (number < 1 || number >= MAX_EDICTS)
        Com_Error(ERR_DROP, "%s: bad entity number: %d", __func__, number);

    // set everything to the state we are delta'ing from
    if (!from)
        memset(to, 0, sizeof(*to));
    else if (to != from)
        memcpy(to, from, sizeof(*to));

    to->number = number;
    to->event = 0;

    if (!bits)
        return;

    model16 = (bits & UK_MODEL16) != 0;

    if (bits & UK_MODEL)
        to->modelindex = kex_clamp_modelindex(model16 ? MSG_ReadWord() : MSG_ReadByte());
    if (bits & UK_MODEL2)
        to->modelindex2 = kex_clamp_modelindex(model16 ? MSG_ReadWord() : MSG_ReadByte());
    if (bits & UK_MODEL3)
        to->modelindex3 = kex_clamp_modelindex(model16 ? MSG_ReadWord() : MSG_ReadByte());
    if (bits & UK_MODEL4)
        to->modelindex4 = kex_clamp_modelindex(model16 ? MSG_ReadWord() : MSG_ReadByte());

    if (bits & UK_FRAME8)
        to->frame = MSG_ReadByte();
    else if (bits & UK_FRAME16)
        to->frame = MSG_ReadWord();

    if ((bits & UK_SKIN32) == UK_SKIN32)     // used for laser colors
        to->skinnum = MSG_ReadLong();
    else if (bits & UK_SKIN16)
        to->skinnum = MSG_ReadWord();
    else if (bits & UK_SKIN8)
        to->skinnum = MSG_ReadByte();

    // KEX widened effects to 64 bits and sends the low half first when it needs
    // both. Our field is 32 bits wide, but the high half still has to be read
    // and looked at: EF_BARREL_EXPLODING lives up there and has a home here.
    if (bits & UK_EFFECTS64)
        low_effects = (uint32_t)MSG_ReadLong();

    if ((bits & UK_EFFECTS32) == UK_EFFECTS32)
        high_effects = (uint32_t)MSG_ReadLong();
    else if (bits & UK_EFFECTS16)
        high_effects = MSG_ReadWord();
    else if (bits & UK_EFFECTS8)
        high_effects = MSG_ReadByte();

    if (bits & (UK_EFFECTS64 | UK_EFFECTS32)) {
        uint64_t effects;

        if (bits & UK_EFFECTS64)
            effects = ((uint64_t)high_effects << 32) | low_effects;
        else
            effects = high_effects;     // only the low half was sent

        to->effects = kex_translate_effects(effects);
    }

    if ((bits & UK_RENDERFX32) == UK_RENDERFX32)
        to->renderfx = kex_translate_renderfx((uint32_t)MSG_ReadLong());
    else if (bits & UK_RENDERFX16)
        to->renderfx = kex_translate_renderfx(MSG_ReadWord());
    else if (bits & UK_RENDERFX8)
        to->renderfx = kex_translate_renderfx(MSG_ReadByte());

    // Must come before the origins: in protocol 2022 it decides their encoding.
    if (bits & UK_SOLID) {
        to->solid = MSG_ReadLong();
        kex_set_solid(kex_solid_bits, number, to->solid != 0);
    }

    high_precision = cl.kex_protocol != PROTOCOL_VERSION_KEX_DEMOS ||
                     kex_get_solid(kex_solid_bits, number);

    if (high_precision) {
        if (bits & UK_ORIGIN1)
            to->origin[0] = MSG_ReadFloat();
        if (bits & UK_ORIGIN2)
            to->origin[1] = MSG_ReadFloat();
        if (bits & UK_ORIGIN3)
            to->origin[2] = MSG_ReadFloat();
        if (bits & UK_OLDORIGIN)
            kex_read_pos_float(to->old_origin);
    } else {
        if (bits & UK_ORIGIN1)
            to->origin[0] = SHORT2COORD(MSG_ReadShort());
        if (bits & UK_ORIGIN2)
            to->origin[1] = SHORT2COORD(MSG_ReadShort());
        if (bits & UK_ORIGIN3)
            to->origin[2] = SHORT2COORD(MSG_ReadShort());
        if (bits & UK_OLDORIGIN)
            kex_read_pos_short(to->old_origin);
    }

    // KEX never packs angles: UK_ANGLE16 is unused and all three are floats.
    if (bits & UK_ANGLE1)
        to->angles[0] = MSG_ReadFloat();
    if (bits & UK_ANGLE2)
        to->angles[1] = MSG_ReadFloat();
    if (bits & UK_ANGLE3)
        to->angles[2] = MSG_ReadFloat();

    if (bits & UK_SOUND) {
        int word = MSG_ReadWord();

        to->sound = word & UK_SOUND_INDEX_MASK;
        if (to->sound >= MAX_SOUNDS)
            to->sound = 0;
        // Per-entity looping volume and attenuation. Our looping sounds are
        // always full volume at the default attenuation, so these are read to
        // keep the stream aligned and then dropped.
        if (word & UK_SOUND_VOLUME)
            MSG_ReadByte();
        if (word & UK_SOUND_ATTENUATION)
            MSG_ReadByte();
    }

    if (bits & UK_EVENT)
        to->event = MSG_ReadByte();

    if (bits & UK_ALPHA)
        to->alpha = MSG_ReadByte() * (1.0f / 255);

    if (bits & UK_SCALE)
        to->scale = MSG_ReadByte() * (1.0f / 16);

    // Split screen instance mask, owner (used by the rerelease for its own
    // prediction) and old_frame (explicit animation source frame). None of
    // these have anywhere to go here.
    if (bits & UK_INSTANCE)
        MSG_ReadByte();
    if (bits & UK_OWNER)
        MSG_ReadWord();
    if (bits & UK_OLDFRAME)
        MSG_ReadWord();
}

// Baselines are entity deltas from nothing. The solid they establish is what a
// later U_REMOVE reverts to, so it is snapshotted here.
static void CL_KexDemo_ParseBaseline(void)
{
    uint64_t bits;
    int      index = CL_KexDemo_ParseEntityBits(&bits);

    CL_ParseBaseline(index, bits);

    if (index > 0 && index < MAX_EDICTS)
        kex_set_solid(kex_baseline_solid_bits, index,
                      kex_get_solid(kex_solid_bits, index));
}

// Called by CL_ParsePacketEntities when a delta removes an entity.
void CL_KexDemo_EntityRemoved(int number)
{
    if (number > 0 && number < MAX_EDICTS)
        kex_set_solid(kex_solid_bits, number,
                      kex_get_solid(kex_baseline_solid_bits, number));
}

/*
=====================================================================

  PLAYER STATE

=====================================================================
*/

// KEX inserted PM_GRAPPLE and PM_NOCLIP after PM_NORMAL, so everything from
// PM_SPECTATOR down shifted by two. Getting this wrong makes a dead player
// render as a spectator (no view kick, no gib bbox), so it is worth a table.
static pmtype_t kex_pm_type(int pm_type)
{
    switch (pm_type) {
    case 0: return PM_NORMAL;       // PM_NORMAL
    case 1: return PM_NORMAL;       // PM_GRAPPLE - moves oddly, but is alive
    case 2: return PM_SPECTATOR;    // PM_NOCLIP
    case 3: return PM_SPECTATOR;    // PM_SPECTATOR
    case 4: return PM_DEAD;         // PM_DEAD
    case 5: return PM_GIB;          // PM_GIB
    case 6: return PM_FREEZE;       // PM_FREEZE
    default: return PM_NORMAL;
    }
}

void CL_KexDemo_ParsePlayerstate(const player_state_t *from, player_state_t *to)
{
    uint32_t flags;
    uint32_t statbits;
    int      i, value;

    // clear to old value before delta parsing
    if (!from)
        memset(to, 0, sizeof(*to));
    else if (to != from)
        memcpy(to, from, sizeof(*to));

    flags = MSG_ReadWord();
    if (flags & PSK_MOREBITS)
        flags |= (uint32_t)MSG_ReadWord() << 16;

    //
    // pmove_state_t
    //
    if (flags & PS_M_TYPE)
        to->pmove.pm_type = kex_pm_type(MSG_ReadByte());

    if (flags & PS_M_ORIGIN) {
        // Ours is 12.3 fixed point, so anything past +-4096 units clips. Real
        // Quake II maps live well inside that.
        to->pmove.origin[0] = COORD2SHORT(MSG_ReadFloat());
        to->pmove.origin[1] = COORD2SHORT(MSG_ReadFloat());
        to->pmove.origin[2] = COORD2SHORT(MSG_ReadFloat());
    }

    if (flags & PS_M_VELOCITY) {
        to->pmove.velocity[0] = COORD2SHORT(MSG_ReadFloat());
        to->pmove.velocity[1] = COORD2SHORT(MSG_ReadFloat());
        to->pmove.velocity[2] = COORD2SHORT(MSG_ReadFloat());
    }

    if (flags & PS_M_TIME) {
        // KEX counts milliseconds; ours counts eight-millisecond units.
        value = MSG_ReadWord() >> 3;
        to->pmove.pm_time = value > 255 ? 255 : value;
    }

    if (flags & PS_M_FLAGS) {
        // Bits 0-6 line up with ours. Bit 7 does NOT: KEX uses it for
        // PMF_ON_LADDER where we use PMF_TELEPORT_BIT, and letting a ladder
        // masquerade as a teleport would break view interpolation every time
        // the player climbed anything. Bits 8-10 are rerelease-only.
        to->pmove.pm_flags = MSG_ReadWord() & 0x7f;
    }

    if (flags & PS_M_GRAVITY)
        to->pmove.gravity = MSG_ReadShort();

    if (flags & PS_M_DELTA_ANGLES) {
        to->pmove.delta_angles[0] = ANGLE2SHORT(MSG_ReadFloat());
        to->pmove.delta_angles[1] = ANGLE2SHORT(MSG_ReadFloat());
        to->pmove.delta_angles[2] = ANGLE2SHORT(MSG_ReadFloat());
    }

    //
    // the rest of player_state_t
    //
    if (flags & PS_VIEWOFFSET) {
        to->viewoffset[0] = MSG_ReadShort() * PSK_VIEWOFFSET_SCALE;
        to->viewoffset[1] = MSG_ReadShort() * PSK_VIEWOFFSET_SCALE;
        to->viewoffset[2] = MSG_ReadShort() * PSK_VIEWOFFSET_SCALE;

        // pm_viewheight. The rerelease keeps eye height in its own field -
        // "added to origin[2] + viewoffset[2], for crouching" (game.h) - where
        // ours is folded into viewoffset, and the client builds the view as
        // origin + viewoffset (CL_CalcViewValues). So it has to be added in
        // rather than dropped: without it the camera sits at the player's feet
        // and the whole demo looks like it was recorded crouching.
        //
        // Safe to fold at parse time because KEX sends both under this one
        // delta bit - a frame that resends either resends both.
        to->viewoffset[2] += MSG_ReadChar();
    }

    if (flags & PS_VIEWANGLES) {
        to->viewangles[0] = MSG_ReadFloat();
        to->viewangles[1] = MSG_ReadFloat();
        to->viewangles[2] = MSG_ReadFloat();
    }

    if (flags & PS_KICKANGLES) {
        to->kick_angles[0] = MSG_ReadShort() * PSK_KICKANGLE_SCALE;
        to->kick_angles[1] = MSG_ReadShort() * PSK_KICKANGLE_SCALE;
        to->kick_angles[2] = MSG_ReadShort() * PSK_KICKANGLE_SCALE;
    }

    if (flags & PS_WEAPONINDEX) {
        // Low 13 bits index, high 3 bits gun skin (which we have no use for).
        value = MSG_ReadWord();
        to->gunindex = kex_clamp_modelindex(value & PSK_GUNINDEX_MASK);
    }

    if (flags & PS_WEAPONFRAME) {
        // Low 9 bits gun frame, then a bitmask saying which gun offset and
        // angle components - and the rerelease's gun animation rate - follow.
        value = MSG_ReadWord();
        to->gunframe = value & PSK_GUNFRAME_MASK;
        value >>= PSK_GUNFRAME_BITS;

        if (value & PSK_GUNBIT_OFFSET_X)
            to->gunoffset[0] = MSG_ReadFloat();
        if (value & PSK_GUNBIT_OFFSET_Y)
            to->gunoffset[1] = MSG_ReadFloat();
        if (value & PSK_GUNBIT_OFFSET_Z)
            to->gunoffset[2] = MSG_ReadFloat();
        if (value & PSK_GUNBIT_ANGLES_X)
            to->gunangles[0] = MSG_ReadFloat();
        if (value & PSK_GUNBIT_ANGLES_Y)
            to->gunangles[1] = MSG_ReadFloat();
        if (value & PSK_GUNBIT_ANGLES_Z)
            to->gunangles[2] = MSG_ReadFloat();
        // Gun animation rate: 0 means "frozen", 1 means normal speed. We always
        // play view weapon animations at normal speed.
        if (value & PSK_GUNBIT_GUNRATE)
            MSG_ReadByte();
    }

    if (flags & PS_BLEND) {
        to->blend[0] = MSG_ReadByte() / 255.0f;
        to->blend[1] = MSG_ReadByte() / 255.0f;
        to->blend[2] = MSG_ReadByte() / 255.0f;
        to->blend[3] = MSG_ReadByte() / 255.0f;
    }

    if (flags & PS_FOV)
        to->fov = MSG_ReadByte();

    if (flags & PS_RDFLAGS)
        to->rdflags = MSG_ReadByte();

    // 64 stats, as two 32-bit bitmaps each followed by its shorts. Ours stop at
    // MAX_STATS; the rest drive rerelease HUD features (the weapon wheel, the
    // team compass) that we do not draw, but they still have to be read.
    statbits = (uint32_t)MSG_ReadLong();
    for (i = 0; i < 32; i++) {
        if (statbits & (1u << i)) {
            value = MSG_ReadShort();
            // STAT_PICKUP_STRING is the one stat that carries an absolute
            // configstring index rather than a table-relative one, so it has
            // to go through the same remap the configstrings did.
            if (i == STAT_PICKUP_STRING && value > 0)
                value = CL_KexDemo_RemapConfigstring(value);
            if (i < MAX_STATS)
                to->stats[i] = value > 0 ? value : 0;
        }
    }
    statbits = (uint32_t)MSG_ReadLong();
    for (i = 0; i < 32; i++) {
        if (statbits & (1u << i)) {
            value = MSG_ReadShort();
            if (32 + i < MAX_STATS)
                to->stats[32 + i] = value;
        }
    }

    // A second screen blend the rerelease layers under the first for damage
    // feedback, and the team id used by its scoreboard.
    if (flags & PSK_DAMAGE_BLEND) {
        MSG_ReadByte();
        MSG_ReadByte();
        MSG_ReadByte();
        MSG_ReadByte();
    }
    if (flags & PSK_TEAM_ID)
        MSG_ReadByte();
}

/*
=====================================================================

  GAME MESSAGES

=====================================================================
*/

// KEX temp entity id -> ours. Identical through TE_FLECHETTE (55); above that
// both sides appended their own effects, so the tail has to be translated. -1
// means "we have nothing that draws this"; the payload is still read.
static int kex_translate_tent(int type)
{
    switch (type) {
    // Rogue's railgun trail variant, which this client never grew a case for.
    // Draw it as an ordinary rail trail rather than dropping it.
    //
    // TE_FLAME (32) is deliberately absent: nothing documents its payload - id's
    // own demos never contain one and neither q2proto nor this client can read
    // it - so it falls through to the "bad type" error above rather than
    // desynchronising the rest of the message on a guess.
    case 31: return TE_RAILTRAIL;       // TE_RAILTRAIL2

    case TEK_BLUEHYPERBLASTER_2: return TE_HYPERBLASTER;
    case TEK_BFG_ZAP:            return TE_BFG_LASER;
    case TEK_BERSERK_SLAM:       return TE_BERSERK_SLAM;
    case TEK_GRAPPLE_CABLE_2:    return TE_GRAPPLE_CABLE;
    case TEK_POWER_SPLASH:       return -1;
    case TEK_LIGHTNING_BEAM:     return TE_LIGHTNING_BEAM;
    case TEK_EXPLOSION1_NL:      return TE_EXPLOSION1;
    case TEK_EXPLOSION2_NL:      return TE_EXPLOSION2;

    default:
        // Everything below the rerelease additions is our own numbering.
        return type < TEK_BLUEHYPERBLASTER_2 ? type : -1;
    }
}

// Read a KEX temp entity into the shared `te` block. Returns false if there is
// nothing to draw, in which case the payload has still been consumed.
static bool CL_KexDemo_ParseTEntPacket(void)
{
    int type = MSG_ReadByte();

    memset(&te, 0, sizeof(te));

    switch (type) {
    case 0:     // TE_GUNSHOT
    case 1:     // TE_BLOOD
    case 2:     // TE_BLASTER
    case 4:     // TE_SHOTGUN
    case 9:     // TE_SPARKS
    case 12:    // TE_SCREEN_SPARKS
    case 13:    // TE_SHIELD_SPARKS
    case 14:    // TE_BULLET_SPARKS
    case 26:    // TE_GREENBLOOD
    case 30:    // TE_BLASTER2
    case 42:    // TE_MOREBLOOD
    case 43:    // TE_HEATBEAM_SPARKS
    case 44:    // TE_HEATBEAM_STEAM
    case 46:    // TE_ELECTRIC_SPARKS
    case 55:    // TE_FLECHETTE
    case TEK_BLUEHYPERBLASTER_2:
    case TEK_BERSERK_SLAM:
        kex_read_game_pos(te.pos1);
        MSG_ReadDir(te.dir);
        break;

    case 10:    // TE_SPLASH
    case 15:    // TE_LASER_SPARKS
    case 25:    // TE_WELDING_SPARKS
    case 29:    // TE_TUNNEL_SPARKS
        te.count = MSG_ReadByte();
        kex_read_game_pos(te.pos1);
        MSG_ReadDir(te.dir);
        te.color = MSG_ReadByte();
        break;

    case 3:     // TE_RAILTRAIL
    case 11:    // TE_BUBBLETRAIL
    case 23:    // TE_BFG_LASER
    case 27:    // TE_BLUEHYPERBLASTER
    case 31:    // TE_RAILTRAIL2
    case 34:    // TE_DEBUGTRAIL
    case 41:    // TE_BUBBLETRAIL2
    case TEK_BFG_ZAP:
        kex_read_game_pos(te.pos1);
        kex_read_game_pos(te.pos2);
        break;

    case 5:     // TE_EXPLOSION1
    case 6:     // TE_EXPLOSION2
    case 7:     // TE_ROCKET_EXPLOSION
    case 8:     // TE_GRENADE_EXPLOSION
    case 17:    // TE_ROCKET_EXPLOSION_WATER
    case 18:    // TE_GRENADE_EXPLOSION_WATER
    case 20:    // TE_BFG_EXPLOSION
    case 21:    // TE_BFG_BIGEXPLOSION
    case 22:    // TE_BOSSTPORT
    case 28:    // TE_PLASMA_EXPLOSION
    case 35:    // TE_PLAIN_EXPLOSION
    case 45:    // TE_CHAINFIST_SMOKE
    case 47:    // TE_TRACKER_EXPLOSION
    case 48:    // TE_TELEPORT_EFFECT
    case 49:    // TE_DBALL_GOAL
    case 51:    // TE_NUKEBLAST
    case 52:    // TE_WIDOWSPLASH
    case 53:    // TE_EXPLOSION1_BIG
    case 54:    // TE_EXPLOSION1_NP
    case TEK_EXPLOSION1_NL:
    case TEK_EXPLOSION2_NL:
        kex_read_game_pos(te.pos1);
        break;

    case 16:    // TE_PARASITE_ATTACK
    case 19:    // TE_MEDIC_CABLE_ATTACK
    case 38:    // TE_HEATBEAM
    case 39:    // TE_MONSTER_HEATBEAM
    case TEK_GRAPPLE_CABLE_2:
    case TEK_LIGHTNING_BEAM:
        te.entity1 = MSG_ReadShort();
        kex_read_game_pos(te.pos1);
        kex_read_game_pos(te.pos2);
        break;

    case 24:    // TE_GRAPPLE_CABLE
        te.entity1 = MSG_ReadShort();
        kex_read_game_pos(te.pos1);
        kex_read_game_pos(te.pos2);
        kex_read_game_pos(te.offset);
        break;

    case 33:    // TE_LIGHTNING
        te.entity1 = MSG_ReadShort();
        te.entity2 = MSG_ReadShort();
        kex_read_game_pos(te.pos1);
        kex_read_game_pos(te.pos2);
        break;

    case 36:    // TE_FLASHLIGHT
        kex_read_game_pos(te.pos1);
        te.entity1 = MSG_ReadShort();
        break;

    case 37:    // TE_FORCEWALL
        kex_read_game_pos(te.pos1);
        kex_read_game_pos(te.pos2);
        te.color = MSG_ReadByte();
        break;

    case 40:    // TE_STEAM
        te.entity1 = MSG_ReadShort();
        te.count = MSG_ReadByte();
        kex_read_game_pos(te.pos1);
        MSG_ReadDir(te.dir);
        te.color = MSG_ReadByte();
        te.entity2 = MSG_ReadShort();
        if (te.entity1 != -1)
            te.time = MSG_ReadLong();
        break;

    case 50:    // TE_WIDOWBEAMOUT
        te.entity1 = MSG_ReadShort();
        kex_read_game_pos(te.pos1);
        break;

    case TEK_POWER_SPLASH:
        te.entity1 = MSG_ReadShort();
        te.count = MSG_ReadByte();
        break;

    default:
        Com_Error(ERR_DROP, "%s: bad type %d", __func__, type);
    }

    te.type = kex_translate_tent(type);
    return te.type >= 0;
}

// svc_sound. The index is always a short (KEX has 2048 sounds), and a large
// entity flag can widen the entity/channel word to a long.
static void CL_KexDemo_ParseStartSoundPacket(void)
{
    int flags, channel, entity;

    flags = MSG_ReadByte();
    snd.index = MSG_ReadWord();
    if (snd.index >= MAX_SOUNDS)
        snd.index = 0;

    if (flags & SND_VOLUME)
        snd.volume = MSG_ReadByte() / 255.0f;
    else
        snd.volume = DEFAULT_SOUND_PACKET_VOLUME;

    if (flags & SND_ATTENUATION)
        snd.attenuation = MSG_ReadByte() / 64.0f;
    else
        snd.attenuation = DEFAULT_SOUND_PACKET_ATTENUATION;

    if (flags & SND_OFFSET)
        snd.timeofs = MSG_ReadByte() / 1000.0f;
    else
        snd.timeofs = 0;

    if (flags & SND_ENT) {
        if (flags & SNDK_LARGE_ENT)
            channel = MSG_ReadLong();
        else
            channel = MSG_ReadWord();

        entity = channel >> 3;
        if (entity < 0 || entity >= MAX_EDICTS)
            Com_Error(ERR_DROP, "%s: bad entity: %d", __func__, entity);
        snd.entity = entity;
        snd.channel = channel & 7;
    } else {
        snd.entity = 0;
        snd.channel = 0;
    }

    if (flags & SND_POS)
        kex_read_game_pos(snd.pos);

    // S_ParseStartSound only looks at SND_ENT and SND_POS, and SNDK_LARGE_ENT
    // sits in a bit it never tests, so the flags can go through as they are.
    snd.flags = flags;
}

/*
=====================================================================

  MONSTER MUZZLE FLASHES

=====================================================================
*/

// MZ2_ ids 0..210 mean the same thing in both lists. At 211 the rerelease
// starts INSERTING its own (the soldier's ripper and hypergun, a second hover
// blaster, the medic's two hyperblasters), so from there up an id names a
// different monster than it does here - and its list runs to 289 against our
// monster_flash_offset[256], so the high ones index off the end of the table
// entirely. id's own demos do use them: rdemo1 fires 256, 257, 258, 259, 264.
//
// Flashes the rerelease has and we do not are mapped to the same muzzle on the
// same monster, which is what the offset actually describes - a soldier's
// ripper comes out of the same place as his blaster. The medic's twelve
// hyperblaster ids collapse onto our single MZ2_MEDIC_HYPERBLASTER, which this
// fork already sweeps by firing frame through medic_hyperblaster_offset[].
#define MZK_FIRST_REMAPPED  211
#define MZK_COUNT           289     // MZ2_LAST in src/rerelease/game.h

static const uint16_t kex_muzzleflash_map[MZK_COUNT - MZK_FIRST_REMAPPED] = {
     39,   // 211 MZ2_SOLDIER_RIPPER_1 -> MZ2_SOLDIER_BLASTER_1
     40,   // 212 MZ2_SOLDIER_RIPPER_2 -> MZ2_SOLDIER_BLASTER_2
     83,   // 213 MZ2_SOLDIER_RIPPER_3 -> MZ2_SOLDIER_BLASTER_3
     86,   // 214 MZ2_SOLDIER_RIPPER_4 -> MZ2_SOLDIER_BLASTER_4
     89,   // 215 MZ2_SOLDIER_RIPPER_5 -> MZ2_SOLDIER_BLASTER_5
     92,   // 216 MZ2_SOLDIER_RIPPER_6 -> MZ2_SOLDIER_BLASTER_6
     95,   // 217 MZ2_SOLDIER_RIPPER_7 -> MZ2_SOLDIER_BLASTER_7
     98,   // 218 MZ2_SOLDIER_RIPPER_8 -> MZ2_SOLDIER_BLASTER_8
     39,   // 219 MZ2_SOLDIER_HYPERGUN_1 -> MZ2_SOLDIER_BLASTER_1
     40,   // 220 MZ2_SOLDIER_HYPERGUN_2 -> MZ2_SOLDIER_BLASTER_2
     83,   // 221 MZ2_SOLDIER_HYPERGUN_3 -> MZ2_SOLDIER_BLASTER_3
     86,   // 222 MZ2_SOLDIER_HYPERGUN_4 -> MZ2_SOLDIER_BLASTER_4
     89,   // 223 MZ2_SOLDIER_HYPERGUN_5 -> MZ2_SOLDIER_BLASTER_5
     92,   // 224 MZ2_SOLDIER_HYPERGUN_6 -> MZ2_SOLDIER_BLASTER_6
     95,   // 225 MZ2_SOLDIER_HYPERGUN_7 -> MZ2_SOLDIER_BLASTER_7
     98,   // 226 MZ2_SOLDIER_HYPERGUN_8 -> MZ2_SOLDIER_BLASTER_8
    245,   // 227 MZ2_GUARDIAN_BLASTER
    238,   // 228 MZ2_ARACHNID_RAIL1
    239,   // 229 MZ2_ARACHNID_RAIL2
    240,   // 230 MZ2_ARACHNID_RAIL_UP1
    241,   // 231 MZ2_ARACHNID_RAIL_UP2
    214,   // 232 MZ2_INFANTRY_MACHINEGUN_14
    215,   // 233 MZ2_INFANTRY_MACHINEGUN_15
    216,   // 234 MZ2_INFANTRY_MACHINEGUN_16
    217,   // 235 MZ2_INFANTRY_MACHINEGUN_17
    218,   // 236 MZ2_INFANTRY_MACHINEGUN_18
    219,   // 237 MZ2_INFANTRY_MACHINEGUN_19
    220,   // 238 MZ2_INFANTRY_MACHINEGUN_20
    221,   // 239 MZ2_INFANTRY_MACHINEGUN_21
    227,   // 240 MZ2_GUNCMDR_CHAINGUN_1
    228,   // 241 MZ2_GUNCMDR_CHAINGUN_2
    229,   // 242 MZ2_GUNCMDR_GRENADE_MORTAR_1
    230,   // 243 MZ2_GUNCMDR_GRENADE_MORTAR_2
    231,   // 244 MZ2_GUNCMDR_GRENADE_MORTAR_3
    232,   // 245 MZ2_GUNCMDR_GRENADE_FRONT_1
    233,   // 246 MZ2_GUNCMDR_GRENADE_FRONT_2
    234,   // 247 MZ2_GUNCMDR_GRENADE_FRONT_3
    235,   // 248 MZ2_GUNCMDR_GRENADE_CROUCH_1
    236,   // 249 MZ2_GUNCMDR_GRENADE_CROUCH_2
    237,   // 250 MZ2_GUNCMDR_GRENADE_CROUCH_3
    211,   // 251 MZ2_SOLDIER_BLASTER_9
    212,   // 252 MZ2_SOLDIER_SHOTGUN_9
    213,   // 253 MZ2_SOLDIER_MACHINEGUN_9
    211,   // 254 MZ2_SOLDIER_RIPPER_9 -> MZ2_SOLDIER_BLASTER_9
    211,   // 255 MZ2_SOLDIER_HYPERGUN_9 -> MZ2_SOLDIER_BLASTER_9
    223,   // 256 MZ2_GUNNER_GRENADE2_1
    224,   // 257 MZ2_GUNNER_GRENADE2_2
    225,   // 258 MZ2_GUNNER_GRENADE2_3
    226,   // 259 MZ2_GUNNER_GRENADE2_4
    222,   // 260 MZ2_INFANTRY_MACHINEGUN_22
    243,   // 261 MZ2_SUPERTANK_GRENADE_1
    244,   // 262 MZ2_SUPERTANK_GRENADE_2
     62,   // 263 MZ2_HOVER_BLASTER_2 -> MZ2_HOVER_BLASTER_1
    145,   // 264 MZ2_DAEDALUS_BLASTER_2 -> MZ2_DAEDALUS_BLASTER
    242,   // 265 MZ2_MEDIC_HYPERBLASTER1_1 -> MZ2_MEDIC_HYPERBLASTER
    242,   // 266 MZ2_MEDIC_HYPERBLASTER1_2 -> MZ2_MEDIC_HYPERBLASTER
    242,   // 267 MZ2_MEDIC_HYPERBLASTER1_3 -> MZ2_MEDIC_HYPERBLASTER
    242,   // 268 MZ2_MEDIC_HYPERBLASTER1_4 -> MZ2_MEDIC_HYPERBLASTER
    242,   // 269 MZ2_MEDIC_HYPERBLASTER1_5 -> MZ2_MEDIC_HYPERBLASTER
    242,   // 270 MZ2_MEDIC_HYPERBLASTER1_6 -> MZ2_MEDIC_HYPERBLASTER
    242,   // 271 MZ2_MEDIC_HYPERBLASTER1_7 -> MZ2_MEDIC_HYPERBLASTER
    242,   // 272 MZ2_MEDIC_HYPERBLASTER1_8 -> MZ2_MEDIC_HYPERBLASTER
    242,   // 273 MZ2_MEDIC_HYPERBLASTER1_9 -> MZ2_MEDIC_HYPERBLASTER
    242,   // 274 MZ2_MEDIC_HYPERBLASTER1_10 -> MZ2_MEDIC_HYPERBLASTER
    242,   // 275 MZ2_MEDIC_HYPERBLASTER1_11 -> MZ2_MEDIC_HYPERBLASTER
    242,   // 276 MZ2_MEDIC_HYPERBLASTER1_12 -> MZ2_MEDIC_HYPERBLASTER
    242,   // 277 MZ2_MEDIC_HYPERBLASTER2_1 -> MZ2_MEDIC_HYPERBLASTER
    242,   // 278 MZ2_MEDIC_HYPERBLASTER2_2 -> MZ2_MEDIC_HYPERBLASTER
    242,   // 279 MZ2_MEDIC_HYPERBLASTER2_3 -> MZ2_MEDIC_HYPERBLASTER
    242,   // 280 MZ2_MEDIC_HYPERBLASTER2_4 -> MZ2_MEDIC_HYPERBLASTER
    242,   // 281 MZ2_MEDIC_HYPERBLASTER2_5 -> MZ2_MEDIC_HYPERBLASTER
    242,   // 282 MZ2_MEDIC_HYPERBLASTER2_6 -> MZ2_MEDIC_HYPERBLASTER
    242,   // 283 MZ2_MEDIC_HYPERBLASTER2_7 -> MZ2_MEDIC_HYPERBLASTER
    242,   // 284 MZ2_MEDIC_HYPERBLASTER2_8 -> MZ2_MEDIC_HYPERBLASTER
    242,   // 285 MZ2_MEDIC_HYPERBLASTER2_9 -> MZ2_MEDIC_HYPERBLASTER
    242,   // 286 MZ2_MEDIC_HYPERBLASTER2_10 -> MZ2_MEDIC_HYPERBLASTER
    242,   // 287 MZ2_MEDIC_HYPERBLASTER2_11 -> MZ2_MEDIC_HYPERBLASTER
    242,   // 288 MZ2_MEDIC_HYPERBLASTER2_12 -> MZ2_MEDIC_HYPERBLASTER
};

// Translate a rerelease monster muzzle flash id into ours. Anything we cannot
// place at all becomes MZ2_NONE rather than an out of range read.
static int kex_translate_muzzleflash(int id)
{
    if (id < 0)
        return 0;
    if (id < MZK_FIRST_REMAPPED)
        return id;
    if (id < MZK_COUNT)
        return kex_muzzleflash_map[id - MZK_FIRST_REMAPPED];

    if (!kex_warned_muzzleflash) {
        kex_warned_muzzleflash = true;
        Com_DPrintf("%s: unknown rerelease muzzle flash id %d\n", __func__, id);
    }
    return 0;
}

// svc_muzzleflash3: like svc_muzzleflash2 but with a 16 bit flash id, because
// the rerelease ran out of monster muzzle flashes. This fork has its own
// svc_muzzleflash3 which additionally carries a direction; KEX's does not, so
// mz.has_dir stays clear and CL_MuzzleFlash2 falls back to body angles.
static void CL_KexDemo_ParseMuzzleFlash3Packet(void)
{
    mz.entity = MSG_ReadShort();
    mz.weapon = kex_translate_muzzleflash(MSG_ReadWord());
    mz.silenced = false;
    mz.has_dir = false;
}

static void CL_KexDemo_ParseMuzzleFlashPacket(int mask)
{
    int entity = MSG_ReadShort();
    int weapon;

    if (entity < 1 || entity >= MAX_EDICTS)
        Com_Error(ERR_DROP, "%s: bad entity: %d", __func__, entity);

    weapon = MSG_ReadByte();
    mz.silenced = (weapon & mask) != 0;
    mz.weapon = kex_translate_muzzleflash(weapon & ~mask);
    mz.entity = entity;
    mz.has_dir = false;
}

// svc_damage: the rerelease's directional damage indicators. We have no HUD
// element for them, so this only consumes the payload.
static void CL_KexDemo_ParseDamage(void)
{
    int i, count = MSG_ReadByte();
    vec3_t dir;

    for (i = 0; i < count; i++) {
        MSG_ReadByte();     // packed damage amount and health/armor/shield bits
        MSG_ReadDir(dir);
    }
}

// svc_fog. The rerelease drives global and height fog from the game; this fork
// has its own fog system driven from the map, so the values are read and
// dropped rather than fought over.
static void CL_KexDemo_ParseFog(void)
{
    int bits = MSG_ReadByte();

    if (bits & FOGK_MORE_BITS)
        bits |= MSG_ReadByte() << 8;

    if (bits & FOGK_DENSITY) {
        MSG_ReadLong();     // density, float
        MSG_ReadByte();     // sky factor
    }
    if (bits & FOGK_R)
        MSG_ReadByte();
    if (bits & FOGK_G)
        MSG_ReadByte();
    if (bits & FOGK_B)
        MSG_ReadByte();
    if (bits & FOGK_TIME)
        MSG_ReadWord();

    if (bits & FOGK_HEIGHTFOG_FALLOFF)
        MSG_ReadLong();     // float
    if (bits & FOGK_HEIGHTFOG_DENSITY)
        MSG_ReadLong();     // float
    if (bits & FOGK_HEIGHTFOG_START_R)
        MSG_ReadByte();
    if (bits & FOGK_HEIGHTFOG_START_G)
        MSG_ReadByte();
    if (bits & FOGK_HEIGHTFOG_START_B)
        MSG_ReadByte();
    if (bits & FOGK_HEIGHTFOG_START_DIST)
        MSG_ReadLong();
    if (bits & FOGK_HEIGHTFOG_END_R)
        MSG_ReadByte();
    if (bits & FOGK_HEIGHTFOG_END_G)
        MSG_ReadByte();
    if (bits & FOGK_HEIGHTFOG_END_B)
        MSG_ReadByte();
    if (bits & FOGK_HEIGHTFOG_END_DIST)
        MSG_ReadLong();
}

// svc_poi: the compass objective marker. This fork already draws one of its own
// (TE_POI), and takes the same three things - where, which icon, how long for -
// so the rerelease message translates straight into it.
//
// Both this and svc_help_path below write their position with the game's
// WritePosition, the same call temp entities use, so in protocol 2022 it is 12.3
// shorts rather than floats. (q2proto reads these two as floats; no demo id
// ships contains either message, so that path was never exercised there. id's
// own g_cmds.cpp and g_items.cpp settle it.)
static bool CL_KexDemo_ParsePoi(void)
{
    int time_ms, image;

    memset(&te, 0, sizeof(te));

    MSG_ReadWord();                 // key: which marker this replaces
    time_ms = MSG_ReadWord();       // lifetime, milliseconds
    kex_read_game_pos(te.pos1);
    image = MSG_ReadWord();         // CS_IMAGES index, already remapped to ours
    MSG_ReadByte();                 // colour: we draw the icon as authored
    MSG_ReadByte();                 // flags

    te.type = TE_POI;
    te.count = image;
    te.time = time_ms / 100;        // ours counts tenths of a second
    return true;
}

// svc_help_path: the breadcrumb trail to the current objective. KEX sends one
// point per message with a flag on the first, where TE_POI_PATH wants the whole
// trail at once, so the points are accumulated here and the trail so far is
// handed over each time - which also makes it grow on screen the way it does in
// the remaster.
static vec3_t kex_help_path[MAX_POI_PATH];
static int    kex_help_path_count;

static bool CL_KexDemo_ParseHelpPath(void)
{
    vec3_t pos, dir;
    bool   first;
    int    i;

    first = MSG_ReadByte() != 0;
    kex_read_game_pos(pos);
    MSG_ReadDir(dir);               // direction to the next point; we join dots

    if (first)
        kex_help_path_count = 0;
    if (kex_help_path_count < MAX_POI_PATH)
        VectorCopy(pos, kex_help_path[kex_help_path_count++]);

    memset(&te, 0, sizeof(te));
    te.type = TE_POI_PATH;
    te.count = kex_help_path_count;
    for (i = 0; i < kex_help_path_count; i++)
        VectorCopy(kex_help_path[i], te.path[i]);
    return true;
}

// svc_locprint: a localization key plus arguments, which the rerelease resolves
// against its string tables. We have no such tables, so the key is printed as
// is - better than nothing when a demo narrates an objective.
static void CL_KexDemo_ParseLocPrint(void)
{
    char base[MAX_STRING_CHARS];
    char arg[MAX_STRING_CHARS];
    int  i, count;

    MSG_ReadByte();         // flags
    MSG_ReadString(base, sizeof(base));
    count = MSG_ReadByte();
    for (i = 0; i < count; i++)
        MSG_ReadString(arg, sizeof(arg));

    Com_DPrintf("locprint: %s (%d args)\n", base, count);
}

/*
=====================================================================

  COMPRESSED GAME STATE

=====================================================================
*/

#if USE_ZLIB

// svc_configblast and svc_spawnbaselineblast carry a run of configstrings or
// baselines deflated into one message. Both are zlib streams (with the two byte
// header), unlike svc_zpacket's raw deflate, so they get their own inflate
// state rather than reusing cls.z.
//
// The inflated data is parsed by pointing msg_read at it and running the normal
// readers, exactly as CL_ParseZPacket does.
static void CL_KexDemo_ParseBlast(bool baselines)
{
    sizebuf_t temp;
    byte      buffer[MAX_MSGLEN];
    int       ret, inlen, outlen;

    if (msg_read.data != msg_read_buffer)
        Com_Error(ERR_DROP, "%s: recursively entered", __func__);

    inlen = MSG_ReadWord();
    outlen = MSG_ReadWord();

    if (inlen < 0 || outlen < 0 || msg_read.readcount + inlen > msg_read.cursize)
        Com_Error(ERR_DROP, "%s: read past end of message", __func__);
    if (outlen > MAX_MSGLEN)
        Com_Error(ERR_DROP, "%s: invalid output length", __func__);

    if (!kex_z_inited) {
        if (inflateInit2(&kex_z, MAX_WBITS) != Z_OK)
            Com_Error(ERR_DROP, "%s: inflateInit2() failed", __func__);
        kex_z_inited = true;
    }
    inflateReset(&kex_z);

    kex_z.next_in = msg_read.data + msg_read.readcount;
    kex_z.avail_in = (uInt)inlen;
    kex_z.next_out = buffer;
    kex_z.avail_out = (uInt)outlen;
    ret = inflate(&kex_z, Z_FINISH);
    if (ret != Z_STREAM_END)
        Com_Error(ERR_DROP, "%s: inflate() failed with error %d", __func__, ret);

    msg_read.readcount += inlen;

    // The uncompressed length in the header is advisory; trust what inflate
    // actually produced.
    outlen -= kex_z.avail_out;

    temp = msg_read;
    SZ_Init(&msg_read, buffer, outlen);
    msg_read.cursize = outlen;

    // The blast ends when the buffer does - there is no terminator.
    while (msg_read.readcount < msg_read.cursize) {
        if (baselines)
            CL_KexDemo_ParseBaseline();
        else
            CL_KexDemo_ParseConfigstring();
    }

    msg_read = temp;
}

#else

static void CL_KexDemo_ParseBlast(bool baselines)
{
    Com_Error(ERR_DROP, "Compressed rerelease demo game state received, "
              "but no zlib support linked in.");
}

#endif // USE_ZLIB

/*
=====================================================================

  SERVERDATA

=====================================================================
*/

// Called by CL_ParseServerData once it has read a KEX protocol number, with
// servercount and attractloop already consumed. KEX's serverdata differs from
// ours by exactly one field - a server frame rate byte - so this reads that,
// sets up the compatibility state, and lets the common code carry on with the
// gamedir, client number and level name.
//
// Returns true to tell CL_ParseServerMessage that the rest of this message, and
// every message after it, uses KEX command numbering.
bool CL_KexDemo_ParseServerData(int protocol)
{
    int fps;

    if (!cls.demo.playback) {
        // 2023 is a live protocol, but we have no way to speak it - we cannot
        // send a KEX clc_move, and the rerelease game DLL is not ours to load.
        Com_Error(ERR_DROP, "Protocol %d (Quake II rerelease) is supported for "
                  "demo playback only.", protocol);
    }

    cl.kex_protocol = protocol;

    memset(kex_solid_bits, 0, sizeof(kex_solid_bits));
    memset(kex_baseline_solid_bits, 0, sizeof(kex_baseline_solid_bits));
    kex_warned_modelindex = false;
    kex_warned_configstring = false;
    kex_warned_muzzleflash = false;
    kex_help_path_count = 0;

    fps = MSG_ReadByte();
    CL_SetServerFrameTime(fps);

    Com_DPrintf("Playing back a Quake II rerelease demo "
                "(protocol %d, %d Hz)\n", protocol, fps);

    return true;
}

/*
=====================================================================

  MESSAGE DISPATCH

=====================================================================
*/

// KEX renumbered every server command above svc_frame, so this cannot be extra
// cases in CL_ParseServerMessage: svc_zpacket and svc_splitclient are both 21,
// svc_muzzleflash3 is 25 for us and 32 for them. Commands 0-20 are shared, and
// for those this dispatches into exactly the same handlers.
void CL_KexDemo_ParseMessage(void)
{
    int cmd = -1;
    size_t readcount = 0;

#if USE_DEBUG
    if (cl_shownet->integer == 1)
        Com_LPrintf(PRINT_DEVELOPER, "%zu ", msg_read.cursize);
    else if (cl_shownet->integer > 1)
        Com_LPrintf(PRINT_DEVELOPER, "------------------\n");
#endif

    while (1) {
        if (msg_read.readcount > msg_read.cursize)
            Com_Error(ERR_DROP, "%s: read past end of server message "
                      "(last svc %d began at %zu, readcount %zu, cursize %zu)",
                      __func__, cmd, readcount, msg_read.readcount, msg_read.cursize);

        readcount = msg_read.readcount;

        if ((cmd = MSG_ReadByte()) == -1) {
            SHOWNET(1, "%3zu:END OF MESSAGE\n", msg_read.readcount - 1);
            break;
        }

        switch (cmd) {
        default:
            Com_Error(ERR_DROP, "%s: illegible server message: %d "
                      "(at offset %zu)", __func__, cmd, readcount);
            break;

        case svck_nop:
            break;

        case svck_disconnect:
            Com_Error(ERR_DISCONNECT, "Server disconnected");
            break;

        case svck_reconnect:
            CL_ParseReconnect();
            return;

        case svck_print:
            CL_ParsePrint();
            break;

        case svck_centerprint:
            CL_ParseCenterPrint();
            break;

        case svck_stufftext:
            CL_ParseStuffText();
            break;

        case svck_serverdata:
            // A level change inside the demo. Normally it is KEX again and we
            // simply carry on; if it somehow is not, hand the rest of the
            // message back to the ordinary parser rather than reading it with
            // the wrong command numbers.
            if (!CL_ParseServerData()) {
                CL_ParseServerMessage();
                return;
            }
            continue;

        case svck_configstring:
            CL_KexDemo_ParseConfigstring();
            break;

        case svck_sound:
            // Every one of these still has to be READ during a seek - the
            // payload is in the message either way - but firing them would
            // dump the whole fast-forwarded stretch of effects on screen at
            // once when the seek lands, so only act outside one.
            CL_KexDemo_ParseStartSoundPacket();
            if (!cls.demo.seeking)
                S_ParseStartSound();
            break;

        case svck_spawnbaseline:
            CL_KexDemo_ParseBaseline();
            break;

        case svck_temp_entity:
            if (CL_KexDemo_ParseTEntPacket() && !cls.demo.seeking)
                CL_ParseTEnt();
            break;

        case svck_muzzleflash:
            CL_KexDemo_ParseMuzzleFlashPacket(MZ_SILENCED);
            if (!cls.demo.seeking)
                CL_MuzzleFlash();
            break;

        case svck_muzzleflash2:
            CL_KexDemo_ParseMuzzleFlashPacket(0);
            if (!cls.demo.seeking)
                CL_MuzzleFlash2();
            break;

        case svck_muzzleflash3:
            CL_KexDemo_ParseMuzzleFlash3Packet();
            if (!cls.demo.seeking)
                CL_MuzzleFlash2();
            break;

        case svck_frame:
            CL_ParseFrame(0);
            continue;

        case svck_inventory:
            CL_ParseInventory();
            break;

        case svck_layout:
            CL_ParseLayout();
            break;

        case svck_splitclient:
            // Which split screen player the following messages are for. We only
            // ever render the first, and no demo id ships is split screen.
            MSG_ReadByte();
            break;

        case svck_configblast:
            CL_KexDemo_ParseBlast(false);
            break;

        case svck_spawnbaselineblast:
            CL_KexDemo_ParseBlast(true);
            break;

        case svck_level_restart:
        case svck_waitingforplayers:
        case svck_bot_chat:
            // Three multiplayer-only commands whose payloads are not documented
            // anywhere we can check: they are written by the rerelease ENGINE
            // rather than its game DLL, so id's published headers only name
            // them, and q2proto does not read them either. None appears in any
            // demo id ships. Guessing at a length would silently misalign the
            // rest of the message, so say plainly what happened instead.
            Com_Error(ERR_DROP, "This demo contains the rerelease server "
                      "command %d, whose format is not publicly documented. "
                      "Only multiplayer demos contain it.", cmd);
            break;

        case svck_damage:
            CL_KexDemo_ParseDamage();
            break;

        case svck_locprint:
            CL_KexDemo_ParseLocPrint();
            break;

        case svck_fog:
            CL_KexDemo_ParseFog();
            break;

        case svck_poi:
            // The marker is state rather than an effect, so it is worth
            // keeping across a seek - it is what you want to see when the
            // seek lands.
            if (CL_KexDemo_ParsePoi())
                CL_ParseTEnt();
            break;

        case svck_help_path:
            if (CL_KexDemo_ParseHelpPath())
                CL_ParseTEnt();
            break;

        case svck_achievement:
            {
                char id[MAX_STRING_CHARS];
                MSG_ReadString(id, sizeof(id));
            }
            break;
        }
    }
}

// Fast forward variant used by 'seek'. CL_SeekDemoMessage wants the state
// bearing messages applied and everything with a visible or audible side effect
// skipped, which for KEX means the same dispatch with the effects suppressed.
void CL_KexDemo_SeekMessage(void)
{
    // Frames, configstrings and baselines are what seeking has to keep, and
    // those all run through handlers that already test cls.demo.seeking. The
    // effect messages are cheap enough that letting them parse and drop on the
    // floor is simpler - and safer - than a second, subtly different reader.
    CL_KexDemo_ParseMessage();
}
