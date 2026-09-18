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

#ifndef PROTOCOL_KEX_H
#define PROTOCOL_KEX_H

// This tree predates q2pro's BIT() helpers, and the KEX bit tables below are
// far easier to check against id's headers written as bit numbers than as hex.
#ifndef BIT
#define BIT(n)      (1u << (n))
#endif
#ifndef BIT_ULL
#define BIT_ULL(n)  (1ull << (n))
#endif

//
// protocol_kex.h -- wire format of the Quake II 2023 remaster (KEX engine).
//
// This describes protocol 2022 (what every .dm2 the remaster ships or records
// contains) and 2023 (its live network version). It exists ONLY so the client
// can read those demos back; nothing here is ever written. Everything is
// namespaced away from the engine's own protocol.h constants, because several
// of them collide - KEX's U_SCALE is bit 32 where ours is bit 28, its
// svc_muzzleflash3 is 32 where ours is 25, and its configstring sections sit at
// completely different indices.
//
// Cross-checked against src/rerelease/game.h (id's own published game headers,
// which define the KEX svc enum, configstring layout and fog message) and
// against q2proto (GPLv2, https://github.com/res2k/q2proto), whose KEX reader
// documents the quirks that the headers do not - notably the low-precision
// origin encoding described at UK_SOLID below.
//

//
// server to client, KEX numbering.
//
// Identical to ours up to svc_frame (20); everything above that diverges, which
// is why demo playback needs its own dispatch loop rather than extra cases in
// CL_ParseServerMessage.
//
typedef enum {
    svck_bad,

    svck_muzzleflash,
    svck_muzzleflash2,
    svck_temp_entity,
    svck_layout,
    svck_inventory,

    svck_nop,
    svck_disconnect,
    svck_reconnect,
    svck_sound,
    svck_print,
    svck_stufftext,
    svck_serverdata,
    svck_configstring,
    svck_spawnbaseline,
    svck_centerprint,
    svck_download,
    svck_playerinfo,
    svck_packetentities,
    svck_deltapacketentities,
    svck_frame,

    svck_splitclient,           // [Kex] split screen player index
    svck_configblast,           // [Kex] deflated run of configstrings
    svck_spawnbaselineblast,    // [Kex] deflated run of baselines
    svck_level_restart,         // [Paril-KEX] level was soft-rebooted
    svck_damage,                // [Paril-KEX] damage indicators
    svck_locprint,              // [Kex] localized + libfmt version of print
    svck_fog,                   // [Paril-KEX] change current fog values
    svck_waitingforplayers,     // [Kex-Edward]
    svck_bot_chat,              // [Kex]
    svck_poi,                   // [Paril-KEX] point of interest
    svck_help_path,             // [Paril-KEX]
    svck_muzzleflash3,          // [Paril-KEX] muzzleflashes, but ushort id
    svck_achievement,           // [Paril-KEX]

    svck_num_types
} svck_ops_t;

//
// entity_state_t communication.
//
// Bits 0-27 match ours exactly. KEX then adds a fifth bits byte, so the whole
// mask has to be carried in a uint64_t rather than the int our own parser uses.
//
#define UK_ORIGIN1      BIT_ULL(0)
#define UK_ORIGIN2      BIT_ULL(1)
#define UK_ANGLE2       BIT_ULL(2)
#define UK_ANGLE3       BIT_ULL(3)
#define UK_FRAME8       BIT_ULL(4)
#define UK_EVENT        BIT_ULL(5)
#define UK_REMOVE       BIT_ULL(6)
#define UK_MOREBITS1    BIT_ULL(7)

#define UK_NUMBER16     BIT_ULL(8)
#define UK_ORIGIN3      BIT_ULL(9)
#define UK_ANGLE1       BIT_ULL(10)
#define UK_MODEL        BIT_ULL(11)
#define UK_RENDERFX8    BIT_ULL(12)
#define UK_ANGLE16      BIT_ULL(13)     // unused by KEX: angles are always floats
#define UK_EFFECTS8     BIT_ULL(14)
#define UK_MOREBITS2    BIT_ULL(15)

#define UK_SKIN8        BIT_ULL(16)
#define UK_FRAME16      BIT_ULL(17)
#define UK_RENDERFX16   BIT_ULL(18)
#define UK_EFFECTS16    BIT_ULL(19)
#define UK_MODEL2       BIT_ULL(20)
#define UK_MODEL3       BIT_ULL(21)
#define UK_MODEL4       BIT_ULL(22)
#define UK_MOREBITS3    BIT_ULL(23)

#define UK_OLDORIGIN    BIT_ULL(24)
#define UK_SKIN16       BIT_ULL(25)
#define UK_SOUND        BIT_ULL(26)
// KEX always writes solid as a full 32 bits. In protocol 2022 ONLY, its value
// also picks the precision of every origin field on this entity and on later
// deltas of it: a non-zero solid means the origins are 32-bit floats, a zero
// solid means they are 16-bit fixed point. So `solid` has to be tracked per
// entity across a whole demo, not just within one delta - see kexdemo.c.
#define UK_SOLID        BIT_ULL(27)
#define UK_MODEL16      BIT_ULL(28)     // model indices are shorts, not bytes
#define UK_EFFECTS64    BIT_ULL(29)     // low 32 bits of a 64-bit effects field
#define UK_ALPHA        BIT_ULL(30)
#define UK_MOREBITS4    BIT_ULL(31)

#define UK_SCALE        BIT_ULL(32)
#define UK_INSTANCE     BIT_ULL(33)     // split screen instance mask
#define UK_OWNER        BIT_ULL(34)
#define UK_OLDFRAME     BIT_ULL(35)

#define UK_SKIN32       (UK_SKIN8 | UK_SKIN16)          // used for laser colors
#define UK_EFFECTS32    (UK_EFFECTS8 | UK_EFFECTS16)
#define UK_RENDERFX32   (UK_RENDERFX8 | UK_RENDERFX16)

// entity_state_t.sound is a packed word in KEX: 14 bits of index plus two
// flags that add a looping volume and attenuation byte.
#define UK_SOUND_INDEX_MASK     0x3fff
#define UK_SOUND_VOLUME         BIT(14)
#define UK_SOUND_ATTENUATION    BIT(15)

//
// player_state_t communication.
//
// Bits 0-14 match ours. Bit 15 is a "read another word of flags" escape rather
// than our PS_RESERVED.
//
#define PSK_MOREBITS        BIT(15)
#define PSK_DAMAGE_BLEND    BIT(16)
#define PSK_TEAM_ID         BIT(17)

// PS_WEAPONINDEX carries a 13 bit gun model index plus 3 bits of gun skin.
#define PSK_GUNINDEX_BITS   13
#define PSK_GUNINDEX_MASK   (BIT(PSK_GUNINDEX_BITS) - 1)

// PS_WEAPONFRAME carries a 9 bit gun frame plus 7 bits saying which of the
// gun offset/angle components (and the gun animation rate) follow it.
#define PSK_GUNFRAME_BITS   9
#define PSK_GUNFRAME_MASK   (BIT(PSK_GUNFRAME_BITS) - 1)
#define PSK_GUNBIT_OFFSET_X BIT(0)
#define PSK_GUNBIT_OFFSET_Y BIT(1)
#define PSK_GUNBIT_OFFSET_Z BIT(2)
#define PSK_GUNBIT_ANGLES_X BIT(3)
#define PSK_GUNBIT_ANGLES_Y BIT(4)
#define PSK_GUNBIT_ANGLES_Z BIT(5)
#define PSK_GUNBIT_GUNRATE  BIT(6)

// KEX sends 64 stats as two 32-bit bitmaps, each followed by its shorts.
#define PSK_MAX_STATS       64

// Fixed point divisors for the few player state fields KEX does not send as
// plain floats.
#define PSK_VIEWOFFSET_SCALE    (1.0f / 16)
#define PSK_KICKANGLE_SCALE     (1.0f / 1024)

//
// svc_sound. Same flag bits as ours plus one, and the sound index is always a
// short rather than a byte.
//
#define SNDK_LARGE_ENT  BIT(6)  // entity/channel word is a long, not a short

//
// svc_fog bit flags (src/rerelease/game.h, svc_fog_data_t::bits_t).
//
#define FOGK_DENSITY                BIT(0)  // float density + byte skyfactor
#define FOGK_R                      BIT(1)
#define FOGK_G                      BIT(2)
#define FOGK_B                      BIT(3)
#define FOGK_TIME                   BIT(4)  // short, transition milliseconds
#define FOGK_HEIGHTFOG_FALLOFF      BIT(5)
#define FOGK_HEIGHTFOG_DENSITY      BIT(6)
#define FOGK_MORE_BITS              BIT(7)  // read a second flags byte
#define FOGK_HEIGHTFOG_START_R      BIT(8)
#define FOGK_HEIGHTFOG_START_G      BIT(9)
#define FOGK_HEIGHTFOG_START_B      BIT(10)
#define FOGK_HEIGHTFOG_START_DIST   BIT(11)
#define FOGK_HEIGHTFOG_END_R        BIT(12)
#define FOGK_HEIGHTFOG_END_G        BIT(13)
#define FOGK_HEIGHTFOG_END_B        BIT(14)
#define FOGK_HEIGHTFOG_END_DIST     BIT(15)

//
// Configstring layout.
//
// KEX moved every section, because it raised MAX_MODELS to 8192 and inserted a
// shadow light table that we have no equivalent for. Demo playback remaps each
// index into our own layout; see CL_KexDemo_RemapConfigstring.
//
#define KEX_MAX_CLIENTS         256
#define KEX_MAX_LIGHTSTYLES     256
#define KEX_MAX_MODELS          8192
#define KEX_MAX_SOUNDS          2048
#define KEX_MAX_IMAGES          512
#define KEX_MAX_ITEMS           256
#define KEX_MAX_GENERAL         (KEX_MAX_CLIENTS * 2)
#define KEX_MAX_SHADOW_LIGHTS   256
#define KEX_MAX_WHEEL_ITEMS     32

#define KEX_CS_NAME             0
#define KEX_CS_CDTRACK          1
#define KEX_CS_SKY              2
#define KEX_CS_SKYAXIS          3
#define KEX_CS_SKYROTATE        4
#define KEX_CS_STATUSBAR        5

#define KEX_CS_AIRACCEL         59
#define KEX_CS_MAXCLIENTS       60
#define KEX_CS_MAPCHECKSUM      61

#define KEX_CS_MODELS           62
#define KEX_CS_SOUNDS           (KEX_CS_MODELS + KEX_MAX_MODELS)
#define KEX_CS_IMAGES           (KEX_CS_SOUNDS + KEX_MAX_SOUNDS)
#define KEX_CS_LIGHTS           (KEX_CS_IMAGES + KEX_MAX_IMAGES)
#define KEX_CS_SHADOWLIGHTS     (KEX_CS_LIGHTS + KEX_MAX_LIGHTSTYLES)
#define KEX_CS_ITEMS            (KEX_CS_SHADOWLIGHTS + KEX_MAX_SHADOW_LIGHTS)
#define KEX_CS_PLAYERSKINS      (KEX_CS_ITEMS + KEX_MAX_ITEMS)
#define KEX_CS_GENERAL          (KEX_CS_PLAYERSKINS + KEX_MAX_CLIENTS)
#define KEX_CS_WHEEL_WEAPONS    (KEX_CS_GENERAL + KEX_MAX_GENERAL)
#define KEX_CS_WHEEL_AMMO       (KEX_CS_WHEEL_WEAPONS + KEX_MAX_WHEEL_ITEMS)
#define KEX_CS_WHEEL_POWERUPS   (KEX_CS_WHEEL_AMMO + KEX_MAX_WHEEL_ITEMS)
#define KEX_CS_CD_LOOP_COUNT    (KEX_CS_WHEEL_POWERUPS + KEX_MAX_WHEEL_ITEMS)
#define KEX_CS_GAME_STYLE       (KEX_CS_CD_LOOP_COUNT + 1)
#define KEX_MAX_CONFIGSTRINGS   (KEX_CS_GAME_STYLE + 1)

// KEX widened configstrings from 64 to 96 bytes. Ours are still MAX_QPATH, so
// anything longer gets truncated on the way in - in practice only the status
// bar program and a few general strings are ever that long.
#define KEX_CS_MAX_STRING_LENGTH    96

//
// entity_state_t.effects and .renderfx.
//
// Both sides agree on every bit the original game defined. The clashes are
// where each of us separately claimed a bit that vanilla left free, so an
// entity arrives flagged for an effect nobody meant:
//
//   effects bit 2   EF_BOB there (every bonus item sets it, to bob in place)
//                   against EF_BARREL_EXPLODING here - which is why items in a
//                   rerelease demo arrived trailing sparks and a light.
//   effects bit 35  EF_BARREL_EXPLODING there. Same flag, different home.
//   renderfx bit 19 RF_SHELL_LITE_GREEN there against RF_FIRST_PERSON_FX here.
//   renderfx bit 22 RF_OLD_FRAME_LERP there against RF_REFLECTION_FX here.
//
// Bit 6 looks like a clash and is not: the rerelease renamed vanilla's
// RF_FRAMELERP to RF_NO_ORIGIN_LERP, and it still means the same thing.
//
#define EFK_BOB                 BIT_ULL(2)
#define EFK_DUALFIRE            BIT_ULL(32)
#define EFK_HOLOGRAM            BIT_ULL(33)
#define EFK_FLASHLIGHT          BIT_ULL(34)
#define EFK_BARREL_EXPLODING    BIT_ULL(35)
#define EFK_TELEPORTER2         BIT_ULL(36)
#define EFK_GRENADE_LIGHT       BIT_ULL(37)

#define RFK_SHELL_LITE_GREEN    BIT(19)
#define RFK_OLD_FRAME_LERP      BIT(22)

// Rerelease-only renderfx that sit on bits this fork does not define. Harmless
// in themselves, but dropped so nothing downstream has to wonder about them.
#define RFK_UNSUPPORTED         (BIT(14) | BIT(23) | BIT(24) | BIT(25) |                                  BIT(26) | BIT(28) | BIT(29) | BIT(30) | BIT(31))

//
// temp entities.
//
// Identical numbering to ours through TE_FLECHETTE (57); after that both sides
// appended their own, so the ids have to be translated. TE_NUM_ENTITIES here is
// KEX's count, used to bound the translation table.
//
typedef enum {
    TEK_BLUEHYPERBLASTER_2 = 56,
    TEK_BFG_ZAP,
    TEK_BERSERK_SLAM,
    TEK_GRAPPLE_CABLE_2,
    TEK_POWER_SPLASH,
    TEK_LIGHTNING_BEAM,
    TEK_EXPLOSION1_NL,
    TEK_EXPLOSION2_NL,

    TEK_NUM_ENTITIES
} tek_event_t;

#endif // PROTOCOL_KEX_H
