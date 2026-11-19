/*
Copyright (C) 2026

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

/*
==============================================================================

QUAKE III'S BOT ENGINE, HOSTED BY THE QUAKE II GAME

In Quake III the bot code is split three ways: botlib in the engine, the bot
AI in the game module, and the engine's server in between, which hands the
AI's trap_ calls to botlib, gives botlib its traces and files, and turns a
bot's usercmd into a client think. This file is that server, and the bits of
Quake III's game the AI reaches into:

  - g_entities[], level: a Quake III view of the game, rebuilt every frame
    from the Quake II side (q2b_snapshot, g_q3bot.c). The AI reads players
    out of it (BotAI_GetClientState), and BotAIStartFrame feeds every
    entity in it to botlib.

  - trap_*: botlib's exports, called directly; the rest through q2b_*.

  - botlib_import_t: Quake II's traces, point contents, PVS, files, cvars.

  - event entities: obituaries and the like, which Quake III sends as
    temporary entities and the AI reads from each bot's snapshot.

  - the bots' input: Quake III ran a bot's think every 100 ms and sent its
    input every 50 ms server frame. Here a server frame is 100 ms, sent as
    four 25 ms commands; BotQ2_UserCommands turns the view and sends each.

Compiled in Quake III's world (q3bot/game/g_local.h); the Quake II side is
reached only through q3bot_iface.h.

==============================================================================
*/

#include "g_local.h"
#include "../botlib/botlib.h"
#include "../botlib/be_aas.h"
#include "../botlib/be_ea.h"
#include "../botlib/be_ai_char.h"
#include "../botlib/be_ai_chat.h"
#include "../botlib/be_ai_gen.h"
#include "../botlib/be_ai_goal.h"
#include "../botlib/be_ai_move.h"
#include "../botlib/be_ai_weap.h"
#include "ai_main.h"
#include "../q3bot_iface.h"

#include <stdlib.h>

// ai_main.c
extern bot_state_t	*botstates[MAX_CLIENTS];
void BotUpdateInput(bot_state_t *bs, int time, int elapsed_time);

// the interface's numbers are Quake III's
#define Q3B_CHECK2(a, b, line) typedef char q3b_check_##line[((a) == (b)) ? 1 : -1]
#define Q3B_CHECK1(a, b, line) Q3B_CHECK2(a, b, line)
#define Q3B_CHECK(a, b) Q3B_CHECK1(a, b, __LINE__)

Q3B_CHECK(Q3B_MAX_CLIENTS, MAX_CLIENTS);
Q3B_CHECK(Q3B_MAX_ENTITIES, MAX_GENTITIES);
Q3B_CHECK(Q3B_ENTITYNUM_NONE, ENTITYNUM_NONE);
Q3B_CHECK(Q3B_ENTITYNUM_WORLD, ENTITYNUM_WORLD);
Q3B_CHECK(Q3B_ET_PLAYER, ET_PLAYER);
Q3B_CHECK(Q3B_ET_ITEM, ET_ITEM);
Q3B_CHECK(Q3B_ET_MISSILE, ET_MISSILE);
Q3B_CHECK(Q3B_ET_MOVER, ET_MOVER);
Q3B_CHECK(Q3B_ET_PUSH_TRIGGER, ET_PUSH_TRIGGER);
Q3B_CHECK(Q3B_ET_TELEPORT_TRIGGER, ET_TELEPORT_TRIGGER);
Q3B_CHECK(Q3B_ET_INVISIBLE, ET_INVISIBLE);
Q3B_CHECK(Q3B_ET_EVENTS, ET_EVENTS);
Q3B_CHECK(Q3B_EF_DEAD, EF_DEAD);
Q3B_CHECK(Q3B_EF_TELEPORT_BIT, EF_TELEPORT_BIT);
Q3B_CHECK(Q3B_EF_NODRAW, EF_NODRAW);
Q3B_CHECK(Q3B_EF_FIRING, EF_FIRING);
Q3B_CHECK(Q3B_EF_TALK, EF_TALK);
Q3B_CHECK(Q3B_SVF_NOCLIENT, SVF_NOCLIENT);
Q3B_CHECK(Q3B_SVF_BOT, SVF_BOT);
Q3B_CHECK(Q3B_SVF_BROADCAST, SVF_BROADCAST);
Q3B_CHECK(Q3B_FL_NOTARGET, FL_NOTARGET);
Q3B_CHECK(Q3B_WP_GAUNTLET, WP_GAUNTLET);
Q3B_CHECK(Q3B_WP_MACHINEGUN, WP_MACHINEGUN);
Q3B_CHECK(Q3B_WP_SHOTGUN, WP_SHOTGUN);
Q3B_CHECK(Q3B_WP_GRENADE_LAUNCHER, WP_GRENADE_LAUNCHER);
Q3B_CHECK(Q3B_WP_ROCKET_LAUNCHER, WP_ROCKET_LAUNCHER);
Q3B_CHECK(Q3B_WP_LIGHTNING, WP_LIGHTNING);
Q3B_CHECK(Q3B_WP_RAILGUN, WP_RAILGUN);
Q3B_CHECK(Q3B_WP_PLASMAGUN, WP_PLASMAGUN);
Q3B_CHECK(Q3B_WP_BFG, WP_BFG);
Q3B_CHECK(Q3B_WP_GRAPPLING_HOOK, WP_GRAPPLING_HOOK);
Q3B_CHECK(Q3B_WP_NUM_WEAPONS, WP_NUM_WEAPONS);
Q3B_CHECK(Q3B_PW_QUAD, PW_QUAD);
Q3B_CHECK(Q3B_PW_BATTLESUIT, PW_BATTLESUIT);
Q3B_CHECK(Q3B_PW_HASTE, PW_HASTE);
Q3B_CHECK(Q3B_PW_INVIS, PW_INVIS);
Q3B_CHECK(Q3B_PW_REGEN, PW_REGEN);
Q3B_CHECK(Q3B_PW_FLIGHT, PW_FLIGHT);
Q3B_CHECK(Q3B_PW_REDFLAG, PW_REDFLAG);
Q3B_CHECK(Q3B_PW_BLUEFLAG, PW_BLUEFLAG);
Q3B_CHECK(Q3B_STAT_HEALTH, STAT_HEALTH);
Q3B_CHECK(Q3B_STAT_HOLDABLE_ITEM, STAT_HOLDABLE_ITEM);
Q3B_CHECK(Q3B_STAT_WEAPONS, STAT_WEAPONS);
Q3B_CHECK(Q3B_STAT_ARMOR, STAT_ARMOR);
Q3B_CHECK(Q3B_STAT_MAX_HEALTH, STAT_MAX_HEALTH);
Q3B_CHECK(Q3B_PERS_SCORE, PERS_SCORE);
Q3B_CHECK(Q3B_PERS_HITS, PERS_HITS);
Q3B_CHECK(Q3B_PERS_RANK, PERS_RANK);
Q3B_CHECK(Q3B_PERS_TEAM, PERS_TEAM);
Q3B_CHECK(Q3B_PERS_SPAWN_COUNT, PERS_SPAWN_COUNT);
Q3B_CHECK(Q3B_PERS_ATTACKER, PERS_ATTACKER);
Q3B_CHECK(Q3B_PERS_KILLED, PERS_KILLED);
Q3B_CHECK(Q3B_PM_NORMAL, PM_NORMAL);
Q3B_CHECK(Q3B_PM_SPECTATOR, PM_SPECTATOR);
Q3B_CHECK(Q3B_PM_DEAD, PM_DEAD);
Q3B_CHECK(Q3B_PM_FREEZE, PM_FREEZE);
Q3B_CHECK(Q3B_PM_INTERMISSION, PM_INTERMISSION);
Q3B_CHECK(Q3B_PMF_DUCKED, PMF_DUCKED);
Q3B_CHECK(Q3B_PMF_JUMP_HELD, PMF_JUMP_HELD);
Q3B_CHECK(Q3B_WEAPON_READY, WEAPON_READY);
Q3B_CHECK(Q3B_WEAPON_FIRING, WEAPON_FIRING);
Q3B_CHECK(Q3B_TEAM_FREE, TEAM_FREE);
Q3B_CHECK(Q3B_TEAM_RED, TEAM_RED);
Q3B_CHECK(Q3B_TEAM_BLUE, TEAM_BLUE);
Q3B_CHECK(Q3B_TEAM_SPECTATOR, TEAM_SPECTATOR);
Q3B_CHECK(Q3B_GT_FFA, GT_FFA);
Q3B_CHECK(Q3B_GT_TOURNAMENT, GT_TOURNAMENT);
Q3B_CHECK(Q3B_GT_TEAM, GT_TEAM);
Q3B_CHECK(Q3B_GT_CTF, GT_CTF);
Q3B_CHECK(Q3B_CON_CONNECTED, CON_CONNECTED);
Q3B_CHECK(Q3B_BUTTON_ATTACK, BUTTON_ATTACK);
Q3B_CHECK(Q3B_BUTTON_TALK, BUTTON_TALK);
Q3B_CHECK(Q3B_BUTTON_USE_HOLDABLE, BUTTON_USE_HOLDABLE);
Q3B_CHECK(Q3B_BUTTON_GESTURE, BUTTON_GESTURE);
Q3B_CHECK(Q3B_BUTTON_WALKING, BUTTON_WALKING);
Q3B_CHECK(Q3B_MOD_SHOTGUN, MOD_SHOTGUN);
Q3B_CHECK(Q3B_MOD_RAILGUN, MOD_RAILGUN);
Q3B_CHECK(Q3B_MOD_TRIGGER_HURT, MOD_TRIGGER_HURT);
Q3B_CHECK(Q3B_MOD_GRAPPLE, MOD_GRAPPLE);
Q3B_CHECK(Q3B_GTS_RED_CAPTURE, GTS_RED_CAPTURE);
Q3B_CHECK(Q3B_GTS_BLUE_TAKEN, GTS_BLUE_TAKEN);
Q3B_CHECK(Q3B_CONTENTS_SOLID, CONTENTS_SOLID);
Q3B_CHECK(Q3B_CONTENTS_LAVA, CONTENTS_LAVA);
Q3B_CHECK(Q3B_CONTENTS_SLIME, CONTENTS_SLIME);
Q3B_CHECK(Q3B_CONTENTS_WATER, CONTENTS_WATER);
Q3B_CHECK(Q3B_CONTENTS_FOG, CONTENTS_FOG);
Q3B_CHECK(Q3B_CONTENTS_PLAYERCLIP, CONTENTS_PLAYERCLIP);
Q3B_CHECK(Q3B_CONTENTS_MONSTERCLIP, CONTENTS_MONSTERCLIP);
Q3B_CHECK(Q3B_CONTENTS_TELEPORTER, CONTENTS_TELEPORTER);
Q3B_CHECK(Q3B_CONTENTS_JUMPPAD, CONTENTS_JUMPPAD);
Q3B_CHECK(Q3B_CONTENTS_DONOTENTER, CONTENTS_DONOTENTER);
Q3B_CHECK(Q3B_CONTENTS_MOVER, CONTENTS_MOVER);
Q3B_CHECK(Q3B_CONTENTS_BODY, CONTENTS_BODY);
Q3B_CHECK(Q3B_CONTENTS_CORPSE, CONTENTS_CORPSE);
Q3B_CHECK(Q3B_CONTENTS_DETAIL, CONTENTS_DETAIL);
Q3B_CHECK(Q3B_CONTENTS_TRANSLUCENT, CONTENTS_TRANSLUCENT);
Q3B_CHECK(Q3B_CONTENTS_TRIGGER, CONTENTS_TRIGGER);

// ==========================================================================
// QUAKE III'S GAME, AS THE AI SEES IT

level_locals_t      level;
gentity_t           g_entities[MAX_GENTITIES];
static gclient_t    g_clients[MAX_CLIENTS];
vmCvar_t            g_gametype;

static q3b_entity_t q3b_ents[Q3B_MAX_ENTITIES];
static q3b_client_t q3b_clients[Q3B_MAX_CLIENTS];

static botlib_export_t  *botlib_export;
static qboolean     botlib_setup;       // BotAISetup has run, BotAIShutdown not yet
static qboolean     aas_loaded;         // the map's AAS is in
static qboolean     suppress_commands;  // shutting down: no goodbye chats
static char         q3b_mapname[MAX_QPATH];
static int          q3b_checksum;
static int          q3b_time;           // level ms of the frame being run

// the bots' own userinfo, for what the AI keeps there (sex, teamtask)
static char         q3b_userinfo[MAX_CLIENTS][MAX_INFO_STRING];
static qboolean     q3b_running[MAX_CLIENTS];

// ==========================================================================
// MEMORY - botlib's zone memory is the C heap; its hunk memory, which it never
// frees itself (Quake III cleared the hunk under it), is kept on a list and
// freed when the library shuts down

typedef struct hunkblock_s {
	struct hunkblock_s	*next;
} hunkblock_t;

static hunkblock_t	*hunk_blocks;

static void *Q3B_GetMemory(int size) {
	return malloc(size);
}

static void Q3B_FreeMemory(void *ptr) {
	free(ptr);
}

static int Q3B_AvailableMemory(void) {
	return 256 * 1024 * 1024;
}

static void *Q3B_HunkAlloc(int size) {
	hunkblock_t *b = malloc(sizeof(hunkblock_t) + 16 + size);

	if (!b) {
		q2b_error("Quake III bots: out of memory");
		return NULL;
	}
	b->next = hunk_blocks;
	hunk_blocks = b;
	memset(b + 1, 0, 16 + size);
	return (byte *)(b + 1) + 16;
}

static void Q3B_FreeHunk(void) {
	while (hunk_blocks) {
		hunkblock_t *next = hunk_blocks->next;
		free(hunk_blocks);
		hunk_blocks = next;
	}
}

void *G_Alloc(int size) {
	void *p = calloc(1, size);
	if (!p)
		q2b_error("Quake III bots: out of memory");
	return p;
}

// ==========================================================================
// FILES - read whole through the Quake II filesystem. A path is tried as it
// is (a game directory's own copy, or a Quake II map's own AAS) and then as
// Quake III's ("baseq3/..."). Nothing is written.

#define MAX_Q3B_FILES	64

typedef struct {
	byte	*data;
	int		length;
	int		pos;
} q3bfile_t;

static q3bfile_t	q3b_files[MAX_Q3B_FILES];

static int Q3B_FS_FOpenFile(const char *qpath, fileHandle_t *f, fsMode_t mode) {
	void	*buf = NULL;
	int		len, i;

	*f = 0;
	if (mode != FS_READ)
		return -1;

	for (i = 1; i < MAX_Q3B_FILES; i++)
		if (!q3b_files[i].data)
			break;
	if (i == MAX_Q3B_FILES) {
		q2b_print(2, "Quake III bots: too many open files\n");
		return -1;
	}

	len = q2b_load_file(qpath, &buf);
	if (len < 0 || !buf)
		len = q2b_load_file(va("baseq3/%s", qpath), &buf);
	if (len < 0 || !buf)
		return -1;

	q3b_files[i].data = buf;
	q3b_files[i].length = len;
	q3b_files[i].pos = 0;
	*f = i;
	return len;
}

static int Q3B_FS_Read(void *buffer, int len, fileHandle_t f) {
	q3bfile_t *file;

	if (f <= 0 || f >= MAX_Q3B_FILES || !q3b_files[f].data)
		return 0;
	file = &q3b_files[f];
	if (len > file->length - file->pos)
		len = file->length - file->pos;
	if (len <= 0)
		return 0;
	memcpy(buffer, file->data + file->pos, len);
	file->pos += len;
	return len;
}

static int Q3B_FS_Write(const void *buffer, int len, fileHandle_t f) {
	return 0;
}

static void Q3B_FS_FCloseFile(fileHandle_t f) {
	if (f <= 0 || f >= MAX_Q3B_FILES || !q3b_files[f].data)
		return;
	q2b_free_file(q3b_files[f].data);
	memset(&q3b_files[f], 0, sizeof(q3b_files[f]));
}

static int Q3B_FS_Seek(fileHandle_t f, long offset, int origin) {
	q3bfile_t	*file;
	long		pos;

	if (f <= 0 || f >= MAX_Q3B_FILES || !q3b_files[f].data)
		return -1;
	file = &q3b_files[f];
	switch (origin) {
	case FS_SEEK_CUR: pos = file->pos + offset; break;
	case FS_SEEK_END: pos = file->length + offset; break;
	default: pos = offset; break;
	}
	if (pos < 0 || pos > file->length)
		return -1;
	file->pos = pos;
	return 0;
}

// ==========================================================================
// BOTLIB'S IMPORTS

static void QDECL Q3B_Print(int type, char *fmt, ...) {
	char	str[2048];
	va_list	ap;

	va_start(ap, fmt);
	Q_vsnprintf(str, sizeof(str), fmt, ap);
	va_end(ap);

	switch (type) {
	case PRT_MESSAGE:	q2b_print(0, str); break;
	case PRT_WARNING:	q2b_print(1, va("Warning: %s", str)); break;
	case PRT_ERROR:		q2b_print(2, va("Error: %s", str)); break;
	case PRT_FATAL:		q2b_print(2, va("Fatal: %s", str)); break;
	case PRT_EXIT:		q2b_print(2, va("Exit: %s", str)); break;
	default:			q2b_print(0, str); break;
	}
}

static void Q3B_CopyTrace(bsp_trace_t *bsptrace, const q3b_trace_t *t) {
	bsptrace->allsolid = t->allsolid;
	bsptrace->startsolid = t->startsolid;
	bsptrace->fraction = t->fraction;
	VectorCopy(t->endpos, bsptrace->endpos);
	bsptrace->plane.dist = t->dist;
	VectorCopy(t->normal, bsptrace->plane.normal);
	SetPlaneSignbits(&bsptrace->plane);
	bsptrace->plane.type = PlaneTypeForNormal(bsptrace->plane.normal);
	bsptrace->surface.value = 0;
	bsptrace->surface.flags = t->surfaceflags;
	bsptrace->ent = t->entityNum;
	bsptrace->exp_dist = 0;
	bsptrace->sidenum = 0;
	bsptrace->contents = 0;
}

static void Q3B_Trace(bsp_trace_t *bsptrace, vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end, int passent, int contentmask) {
	q3b_trace_t t;

	q2b_trace(&t, start, mins, maxs, end, passent, contentmask);
	Q3B_CopyTrace(bsptrace, &t);
}

static void Q3B_EntityTrace(bsp_trace_t *bsptrace, vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end, int entnum, int contentmask) {
	q3b_trace_t t;

	q2b_entity_trace(&t, start, mins, maxs, end, entnum, contentmask);
	Q3B_CopyTrace(bsptrace, &t);
}

static int Q3B_PointContents(vec3_t point) {
	return q2b_point_contents(point);
}

static int Q3B_inPVS(vec3_t p1, vec3_t p2) {
	return q2b_in_pvs(p1, p2);
}

static char *Q3B_BSPEntityData(void) {
	return (char *)q2b_entity_string();
}

static void Q3B_BSPModelMinsMaxsOrigin(int modelnum, vec3_t angles, vec3_t outmins, vec3_t outmaxs, vec3_t origin) {
	vec3_t	mins, maxs;
	float	max;
	int		i;

	if (!q2b_model_bounds(modelnum, mins, maxs)) {
		VectorClear(mins);
		VectorClear(maxs);
	}
	// a rotated model: what it sweeps
	if (angles && (angles[0] || angles[1] || angles[2])) {
		max = RadiusFromBounds(mins, maxs);
		for (i = 0; i < 3; i++) {
			mins[i] = -max;
			maxs[i] = max;
		}
	}
	if (outmins) VectorCopy(mins, outmins);
	if (outmaxs) VectorCopy(maxs, outmaxs);
	if (origin) VectorClear(origin);
}

static void Q3B_BotClientCommand(int client, char *command) {
	if (suppress_commands)
		return;
	q2b_client_command(client, command);
}

static int Q3B_DebugLineCreate(void) { return 0; }
static void Q3B_DebugLineDelete(int line) { }
static void Q3B_DebugLineShow(int line, vec3_t start, vec3_t end, int color) { }
static int Q3B_DebugPolygonCreate(int color, int numPoints, vec3_t *points) { return 0; }
static void Q3B_DebugPolygonDelete(int id) { }

static void Q3B_InitBotLib(void) {
	botlib_import_t imp;

	if (botlib_export)
		return;

	memset(&imp, 0, sizeof(imp));
	imp.Print = Q3B_Print;
	imp.Trace = Q3B_Trace;
	imp.EntityTrace = Q3B_EntityTrace;
	imp.PointContents = Q3B_PointContents;
	imp.inPVS = Q3B_inPVS;
	imp.BSPEntityData = Q3B_BSPEntityData;
	imp.BSPModelMinsMaxsOrigin = Q3B_BSPModelMinsMaxsOrigin;
	imp.BotClientCommand = Q3B_BotClientCommand;
	imp.GetMemory = Q3B_GetMemory;
	imp.FreeMemory = Q3B_FreeMemory;
	imp.AvailableMemory = Q3B_AvailableMemory;
	imp.HunkAlloc = Q3B_HunkAlloc;
	imp.FS_FOpenFile = Q3B_FS_FOpenFile;
	imp.FS_Read = Q3B_FS_Read;
	imp.FS_Write = Q3B_FS_Write;
	imp.FS_FCloseFile = Q3B_FS_FCloseFile;
	imp.FS_Seek = Q3B_FS_Seek;
	imp.DebugLineCreate = Q3B_DebugLineCreate;
	imp.DebugLineDelete = Q3B_DebugLineDelete;
	imp.DebugLineShow = Q3B_DebugLineShow;
	imp.DebugPolygonCreate = Q3B_DebugPolygonCreate;
	imp.DebugPolygonDelete = Q3B_DebugPolygonDelete;

	botlib_export = GetBotLibAPI(BOTLIB_API_VERSION, &imp);
}

// ==========================================================================
// THE GAME FUNCTIONS THE AI CALLS

void QDECL G_Printf(const char *fmt, ...) {
	char	str[2048];
	va_list	ap;

	va_start(ap, fmt);
	Q_vsnprintf(str, sizeof(str), fmt, ap);
	va_end(ap);
	q2b_print(0, str);
}

void QDECL G_Error(const char *fmt, ...) {
	char	str[2048];
	va_list	ap;

	va_start(ap, fmt);
	Q_vsnprintf(str, sizeof(str), fmt, ap);
	va_end(ap);
	q2b_error(str);
	exit(1);	// q2b_error does not return
}

// q_shared.c / q_math.c's (Q3_Com_Error, Q3_Com_Printf: see q3_rename.h)
void QDECL Com_Error(int level, const char *fmt, ...) {
	char	str[2048];
	va_list	ap;

	va_start(ap, fmt);
	Q_vsnprintf(str, sizeof(str), fmt, ap);
	va_end(ap);
	q2b_error(str);
	exit(1);
}

void QDECL Com_Printf(const char *fmt, ...) {
	char	str[2048];
	va_list	ap;

	va_start(ap, fmt);
	Q_vsnprintf(str, sizeof(str), fmt, ap);
	va_end(ap);
	q2b_print(0, str);
}

void G_CheckBotSpawn(void) { }	// bots join through g_bot.c
void ExitLevel(void) { }		// only interbreeding ends a level
void ClientUserinfoChanged(int clientNum) { }
int G_ModelIndex(char *name) { return 0; }

// ==========================================================================
// EVENTS - Quake III's temporary event entities, for what the AI watches for

typedef struct {
	int		born;		// level ms; 0 = free
	int		stamp;		// its gentity_t eventTime
	int		event;
	int		eventParm;
	int		other, other2;
	vec3_t	origin;
} q3bevent_t;

static q3bevent_t	q3b_events[Q3B_EVENT_COUNT];
static int			q3b_next_event;
static int			q3b_event_serial;

// Quake III's EVENT_VALID_MSEC: three bot thinks, each of which sees it once
#define EVENT_VALID_MSEC_Q2		300

static void Q3B_AddEvent(int event, int parm, int other, int other2, const vec3_t origin) {
	q3bevent_t *ev = &q3b_events[q3b_next_event];

	q3b_next_event = (q3b_next_event + 1) % Q3B_EVENT_COUNT;
	memset(ev, 0, sizeof(*ev));
	ev->born = q2b_level_ms();
	if (!ev->born)
		ev->born = 1;
	// BotCheckEvents tells a new event from one it has seen by its eventTime:
	// never 0, and never the same twice in a slot
	ev->stamp = ++q3b_event_serial;
	ev->event = event;
	ev->eventParm = parm;
	ev->other = other;
	ev->other2 = other2;
	if (origin)
		VectorCopy(origin, ev->origin);
}

void Q3Bot_Obituary(int target, int attacker, int q3mod) {
	if (aas_loaded)
		Q3B_AddEvent(EV_OBITUARY, q3mod, target, attacker, NULL);
}

void Q3Bot_TeamSound(int gts) {
	if (aas_loaded)
		Q3B_AddEvent(EV_GLOBAL_TEAM_SOUND, gts, 0, 0, NULL);
}

void Q3Bot_Teleported(const float *origin) {
	if (aas_loaded)
		Q3B_AddEvent(EV_PLAYER_TELEPORT_IN, 0, 0, 0, origin);
}

void Q3Bot_PowerupRespawned(void) {
	if (aas_loaded)
		Q3B_AddEvent(EV_GLOBAL_SOUND, 1, 0, 0, NULL);	// q3b_sounds[1]
}

// ==========================================================================
// SERVER COMMANDS - chat and prints addressed to a bot, which it reads
// through trap_BotGetServerCommand

#define MAX_BOT_COMMANDS	32

typedef struct {
	char	text[MAX_BOT_COMMANDS][MAX_STRING_CHARS];
	int		head, tail;
} botcommands_t;

static botcommands_t	q3b_commands[MAX_CLIENTS];

void Q3Bot_ServerCommand(int client, const char *text) {
	botcommands_t *q;

	if (client < 0 || client >= MAX_CLIENTS || !q3b_running[client])
		return;
	q = &q3b_commands[client];
	Q_strncpyz(q->text[q->head % MAX_BOT_COMMANDS], text, MAX_STRING_CHARS);
	q->head++;
	if (q->head - q->tail > MAX_BOT_COMMANDS)
		q->tail = q->head - MAX_BOT_COMMANDS;
}

int trap_BotGetServerCommand(int clientNum, char *message, int size) {
	botcommands_t *q;

	if (clientNum < 0 || clientNum >= MAX_CLIENTS)
		return qfalse;
	q = &q3b_commands[clientNum];
	if (q->tail >= q->head)
		return qfalse;
	Q_strncpyz(message, q->text[q->tail % MAX_BOT_COMMANDS], size);
	q->tail++;
	return qtrue;
}

// ==========================================================================
// THE SNAPSHOT - the Quake II game, rebuilt as Quake III's entities

static void Q3B_BuildEntity(int num, const q3b_entity_t *e) {
	gentity_t		*ent = &g_entities[num];
	entityState_t	*s = &ent->s;
	entityShared_t	*r = &ent->r;

	memset(s, 0, sizeof(*s));
	memset(r, 0, sizeof(*r));
	ent->inuse = e->inuse;
	ent->flags = e->flags;
	ent->eventTime = e->eventTime;
	ent->activator = NULL;
	if (!e->inuse)
		return;

	s->number = num;
	s->eType = e->eType;
	s->eFlags = e->eFlags;
	s->pos.trType = TR_STATIONARY;
	VectorCopy(e->origin, s->pos.trBase);
	s->apos.trType = TR_STATIONARY;
	VectorCopy(e->angles, s->apos.trBase);
	VectorCopy(e->origin, s->origin);
	VectorCopy(e->old_origin, s->origin2);
	VectorCopy(e->angles, s->angles);
	s->otherEntityNum = e->otherEntityNum;
	s->otherEntityNum2 = e->otherEntityNum2;
	s->groundEntityNum = e->groundEntityNum;
	s->modelindex = e->modelindex;
	s->modelindex2 = e->modelindex2;
	s->clientNum = e->clientNum;
	s->frame = e->frame;
	s->solid = e->solid;
	s->event = e->event;
	s->eventParm = e->eventParm;
	s->powerups = e->powerups;
	s->weapon = e->weapon;
	s->legsAnim = e->legsAnim;
	s->torsoAnim = e->torsoAnim;

	r->linked = e->linked;
	r->svFlags = e->svflags;
	r->bmodel = e->bmodel;
	r->contents = e->contents;
	VectorCopy(e->mins, r->mins);
	VectorCopy(e->maxs, r->maxs);
	VectorCopy(e->absmin, r->absmin);
	VectorCopy(e->absmax, r->absmax);
	VectorCopy(e->origin, r->currentOrigin);
	VectorCopy(e->angles, r->currentAngles);
	r->ownerNum = ENTITYNUM_NONE;
}

static void Q3B_BuildClient(int num, const q3b_client_t *c) {
	gclient_t		*cl = &g_clients[num];
	playerState_t	*ps = &cl->ps;
	int				i;

	memset(cl, 0, sizeof(*cl));
	cl->pers.connected = c->connected;
	Q_strncpyz(cl->pers.netname, c->netname, sizeof(cl->pers.netname));
	cl->sess.sessionTeam = c->team;
	cl->lasthurt_client = c->lasthurt_client;
	cl->lasthurt_mod = c->lasthurt_mod;

	ps->commandTime = q3b_time;
	ps->pm_type = c->pm_type;
	ps->pm_flags = c->pm_flags;
	ps->pm_time = c->pm_time;
	VectorCopy(c->origin, ps->origin);
	VectorCopy(c->velocity, ps->velocity);
	VectorCopy(c->viewangles, ps->viewangles);
	for (i = 0; i < 3; i++)
		ps->delta_angles[i] = c->delta_angles[i];
	ps->viewheight = c->viewheight;
	ps->groundEntityNum = c->groundEntityNum;
	ps->weapon = c->weapon;
	ps->weaponstate = c->weaponstate;
	ps->eFlags = c->eFlags;
	ps->clientNum = num;
	ps->gravity = 800;
	ps->speed = 320;
	for (i = 0; i < MAX_STATS; i++)
		ps->stats[i] = c->stats[i];
	for (i = 0; i < MAX_PERSISTANT; i++)
		ps->persistant[i] = c->persistant[i];
	for (i = 0; i < MAX_POWERUPS; i++)
		ps->powerups[i] = c->powerups[i];
	for (i = 0; i < MAX_WEAPONS; i++)
		ps->ammo[i] = c->ammo[i];

	g_entities[num].client = cl;
}

static void Q3B_BuildEventEntity(int slot) {
	q3bevent_t		*ev = &q3b_events[slot];
	gentity_t		*ent = &g_entities[Q3B_EVENT_BASE + slot];

	memset(ent, 0, sizeof(*ent));
	if (!ev->born || q3b_time - ev->born > EVENT_VALID_MSEC_Q2)
		return;

	ent->inuse = qtrue;
	ent->eventTime = ev->stamp;
	ent->s.number = Q3B_EVENT_BASE + slot;
	ent->s.eType = ET_EVENTS + ev->event;
	ent->s.eventParm = ev->eventParm;
	ent->s.otherEntityNum = ev->other;
	ent->s.otherEntityNum2 = ev->other2;
	VectorCopy(ev->origin, ent->s.origin);
	VectorCopy(ev->origin, ent->s.pos.trBase);
	VectorCopy(ev->origin, ent->r.currentOrigin);
	ent->r.linked = qtrue;
	ent->r.svFlags = SVF_BROADCAST;
}

static void Q3B_Snapshot(void) {
	int i;

	memset(q3b_ents, 0, sizeof(q3b_ents));
	memset(q3b_clients, 0, sizeof(q3b_clients));
	q2b_snapshot(q3b_ents, q3b_clients);

	// Quake III's game registered this one itself
	g_gametype.integer = q2b_gametype();
	g_gametype.value = g_gametype.integer;
	Com_sprintf(g_gametype.string, sizeof(g_gametype.string), "%d", g_gametype.integer);

	level.time = q3b_time;
	level.maxclients = q2b_maxclients();
	if (level.maxclients > MAX_CLIENTS)
		level.maxclients = MAX_CLIENTS;
	level.clients = g_clients;
	level.num_entities = Q3B_EVENT_BASE + Q3B_EVENT_COUNT;
	level.intermissiontime = q2b_intermission() ? q3b_time : 0;

	for (i = 0; i < Q3B_EVENT_BASE; i++)
		Q3B_BuildEntity(i, &q3b_ents[i]);
	for (i = 0; i < Q3B_EVENT_COUNT; i++)
		Q3B_BuildEventEntity(i);
	for (i = Q3B_EVENT_BASE + Q3B_EVENT_COUNT; i < MAX_GENTITIES; i++)
		memset(&g_entities[i], 0, sizeof(g_entities[i]));

	for (i = 0; i < MAX_CLIENTS; i++) {
		if (i < level.maxclients) {
			Q3B_BuildClient(i, &q3b_clients[i]);
		} else {
			memset(&g_clients[i], 0, sizeof(g_clients[i]));
			g_entities[i].client = &g_clients[i];
		}
	}
}

// what each bot "receives": broadcast entities and those in its PVS, built
// the first time the AI asks in a frame

static int		snap_list[MAX_CLIENTS][MAX_GENTITIES];
static int		snap_count[MAX_CLIENTS];
static int		snap_time[MAX_CLIENTS];

static void Q3B_BuildSnapshotList(int client) {
	vec3_t	eye;
	int		i, n = 0;

	VectorCopy(g_clients[client].ps.origin, eye);
	eye[2] += g_clients[client].ps.viewheight;

	for (i = 0; i < MAX_GENTITIES; i++) {
		gentity_t *ent = &g_entities[i];

		if (i == client || !ent->inuse || !ent->r.linked)
			continue;
		if (ent->r.svFlags & SVF_NOCLIENT)
			continue;
		if (!(ent->r.svFlags & SVF_BROADCAST) && !q2b_in_pvs(eye, ent->r.currentOrigin))
			continue;
		snap_list[client][n++] = i;
	}
	snap_count[client] = n;
	snap_time[client] = q3b_time;
}

int trap_BotGetSnapshotEntity(int clientNum, int sequence) {
	if (clientNum < 0 || clientNum >= MAX_CLIENTS)
		return -1;
	if (sequence == 0 || snap_time[clientNum] != q3b_time)
		Q3B_BuildSnapshotList(clientNum);
	if (sequence < 0 || sequence >= snap_count[clientNum])
		return -1;
	return snap_list[clientNum][sequence];
}

// ==========================================================================
// ENTRY POINTS FROM THE QUAKE II GAME

static void Q3B_ShutdownClients(void) {
	int i;

	suppress_commands = qtrue;
	for (i = 0; i < MAX_CLIENTS; i++) {
		if (q3b_running[i]) {
			BotAIShutdownClient(i, qfalse);
			q3b_running[i] = qfalse;
		}
		// Quake III dropped its game's memory with the map; here the bot
		// states go back to the heap, and BotAISetupClient makes new ones
		if (botstates[i]) {
			free(botstates[i]);
			botstates[i] = NULL;
		}
	}
	suppress_commands = qfalse;
}

static void Q3B_CloseFiles(void) {
	int i;

	for (i = 1; i < MAX_Q3B_FILES; i++)
		Q3B_FS_FCloseFile(i);
}

void Q3Bot_Shutdown(void) {
	Q3B_ShutdownClients();
	if (botlib_setup) {
		BotAIShutdown(0);
		botlib_setup = qfalse;
	}
	Q3B_CloseFiles();
	Q3B_FreeHunk();
	aas_loaded = qfalse;
}

/*
=================
Q3Bot_LevelStart

The end of SpawnEntities: a new level, so a new library with this map's
AAS. Quake III restarted its whole game module on a map change, and this
does the same for its bots. 1 = the map has an AAS and bots can play it.
=================
*/
int Q3Bot_LevelStart(const char *mapname, int checksum) {
	int i;

	Q3Bot_Shutdown();

	memset(q3b_events, 0, sizeof(q3b_events));
	memset(q3b_commands, 0, sizeof(q3b_commands));
	memset(snap_time, 0, sizeof(snap_time));
	Q_strncpyz(q3b_mapname, mapname, sizeof(q3b_mapname));
	q3b_checksum = checksum;
	q3b_time = q2b_level_ms();

	Q3B_InitBotLib();
	if (!botlib_export)
		return qfalse;

	BotQ2_ResetFrame(q3b_time);

	// the AI reads the game before it sets up: give it one
	Q3B_Snapshot();

	if (!BotAISetup(0)) {
		q2b_print(1, "Quake III bots: the bot library would not start\n");
		Q3B_FreeHunk();
		return qfalse;
	}
	botlib_setup = qtrue;

	BotAILoadMap(0);
	// botlib finishes loading the AAS in its first frame
	trap_BotLibStartFrame((float)q3b_time / 1000);
	aas_loaded = trap_AAS_Initialized();
	if (!aas_loaded) {
		q2b_print(0, va("Quake III bots: no AAS for %s, so its bots play by Quake II's rules\n", mapname));
		return qfalse;
	}

	for (i = 0; i < MAX_CLIENTS; i++)
		q3b_userinfo[i][0] = 0;
	return qtrue;
}

int Q3Bot_Ready(void) {
	return botlib_setup && aas_loaded && trap_AAS_Initialized();
}

int Q3Bot_IsRunning(int client) {
	return client >= 0 && client < MAX_CLIENTS && q3b_running[client];
}

int Q3Bot_AddClient(int client, const char *charfile, float skill, int team, const char *name) {
	bot_settings_t settings;

	if (client < 0 || client >= MAX_CLIENTS || !Q3Bot_Ready())
		return qfalse;
	if (q3b_running[client])
		Q3Bot_RemoveClient(client);

	// the AI reads its own player as it sets up
	q3b_time = q2b_level_ms();
	Q3B_Snapshot();

	q3b_userinfo[client][0] = 0;
	Info_SetValueForKey(q3b_userinfo[client], "name", name);
	Info_SetValueForKey(q3b_userinfo[client], "team",
		team == TEAM_RED ? "red" : team == TEAM_BLUE ? "blue" : "free");

	memset(&settings, 0, sizeof(settings));
	Q_strncpyz(settings.characterfile, charfile, sizeof(settings.characterfile));
	settings.skill = skill;

	memset(&q3b_commands[client], 0, sizeof(q3b_commands[client]));
	if (!BotAISetupClient(client, &settings, qfalse)) {
		q2b_print(1, va("Quake III bots: couldn't set up %s from %s\n", name, charfile));
		return qfalse;
	}
	q3b_running[client] = qtrue;
	return qtrue;
}

/*
=================
Q3Bot_FlagStatus

Quake III's bots keep the flags' state from what they hear - the team sounds
and the "got the flag" prints - so one that joins a game under way thinks
both flags are home. This tells it, as BotMatch_CTF would have. Set straight
into its state: the events are gone before a new bot's first look (its four
setup thinks).
=================
*/
void Q3Bot_FlagStatus(int client, int redstatus, int bluestatus, int carrier) {
	bot_state_t *bs;

	if (client < 0 || client >= MAX_CLIENTS || !q3b_running[client])
		return;
	bs = botstates[client];
	if (!bs || !bs->inuse)
		return;
	bs->redflagstatus = redstatus;
	bs->blueflagstatus = bluestatus;
	bs->flagcarrier = carrier;
	bs->flagstatuschanged = qtrue;
}

void Q3Bot_RemoveClient(int client) {
	if (client < 0 || client >= MAX_CLIENTS || !q3b_running[client])
		return;
	suppress_commands = qtrue;	// a goodbye chat would run inside ClientDisconnect
	BotAIShutdownClient(client, qfalse);
	suppress_commands = qfalse;
	q3b_running[client] = qfalse;
}

/*
=================
Q3Bot_Frame

G_RunFrame, once for all the bots on Quake III's engine: the game as Quake
III would see it, botlib's frame and the AI's thinks (BotAIStartFrame), then
each bot's commands for the frame (BotQ2_UserCommands).
=================
*/
void Q3Bot_Frame(int level_ms) {
	int i, any = 0;

	if (!Q3Bot_Ready())
		return;
	for (i = 0; i < MAX_CLIENTS; i++)
		any |= q3b_running[i];
	if (!any)
		return;

	q3b_time = level_ms;
	Q3B_Snapshot();
	BotAIStartFrame(level_ms);
}

/*
=================
BotQ2_UserCommands

The end of BotAIStartFrame, in place of Quake III's one usercmd a bot per
server frame: the frame is cut into 25 ms commands, as g_bot.c sends its
own, and before each the view turns (BotUpdateInput) by that much.
=================
*/
void BotQ2_UserCommands(int time, int elapsed_time) {
	int				i, step, steps, msec;
	q3b_usercmd_t	cmd;

	if (elapsed_time <= 0)
		elapsed_time = 100;
	steps = (elapsed_time + 24) / 25;
	if (steps < 1)
		steps = 1;
	if (steps > 8)
		steps = 8;
	msec = elapsed_time / steps;
	if (msec < 1)
		msec = 1;

	for (i = 0; i < MAX_CLIENTS; i++) {
		bot_state_t *bs = botstates[i];

		if (!q3b_running[i] || !bs || !bs->inuse)
			continue;
		if (g_entities[i].client->pers.connected != CON_CONNECTED)
			continue;

		for (step = 0; step < steps; step++) {
			int t = time - elapsed_time + (step + 1) * msec;
			int j, delta[3];

			// BotUpdateInput turns the view relative to delta_angles, which a
			// respawn or a teleporter changes between commands
			q2b_client_delta_angles(i, delta);
			for (j = 0; j < 3; j++)
				bs->cur_ps.delta_angles[j] = delta[j];

			BotUpdateInput(bs, t, msec);

			for (j = 0; j < 3; j++)
				cmd.angles[j] = bs->lastucmd.angles[j];
			cmd.buttons = bs->lastucmd.buttons;
			cmd.weapon = bs->lastucmd.weapon;
			cmd.forwardmove = bs->lastucmd.forwardmove;
			cmd.rightmove = bs->lastucmd.rightmove;
			cmd.upmove = bs->lastucmd.upmove;
			q2b_bot_usercmd(i, &cmd, msec);

			// a command can end the bot (a "kill" that disconnects, a level change)
			if (!q3b_running[i])
				break;
		}
	}
}

// ==========================================================================
// TRAPS - Quake III's game syscalls, as its server answered them

void trap_Print(const char *text) {
	q2b_print(0, text);
}

void trap_Error(const char *text) {
	q2b_error(text);
	exit(1);
}

int trap_Milliseconds(void) {
	return q2b_milliseconds();
}

int trap_RealTime(qtime_t *qtime) {
	memset(qtime, 0, sizeof(*qtime));
	return 0;
}

int trap_Argc(void) { return 0; }
void trap_Argv(int n, char *buffer, int bufferLength) { if (bufferLength > 0) buffer[0] = 0; }
void trap_Args(char *buffer, int bufferLength) { if (bufferLength > 0) buffer[0] = 0; }

int trap_FS_FOpenFile(const char *qpath, fileHandle_t *f, fsMode_t mode) { return Q3B_FS_FOpenFile(qpath, f, mode); }
void trap_FS_Read(void *buffer, int len, fileHandle_t f) { Q3B_FS_Read(buffer, len, f); }
void trap_FS_Write(const void *buffer, int len, fileHandle_t f) { }
void trap_FS_FCloseFile(fileHandle_t f) { Q3B_FS_FCloseFile(f); }
int trap_FS_GetFileList(const char *path, const char *extension, char *listbuf, int bufsize) { return 0; }
int trap_FS_Seek(fileHandle_t f, long offset, int origin) { return Q3B_FS_Seek(f, offset, origin); }

void trap_SendConsoleCommand(int exec_when, const char *text) {
	q2b_console_command(text);
}

// cvars: the few Quake III's server owned are answered here, the rest are
// Quake II cvars of the same names
static const char *Q3B_SpecialCvar(const char *name) {
	static char buf[64];

	if (!Q_stricmp(name, "g_gametype"))
		return va("%d", q2b_gametype());
	if (!Q_stricmp(name, "sv_maxclients") || !Q_stricmp(name, "g_maxclients"))
		return va("%d", q2b_maxclients());
	if (!Q_stricmp(name, "mapname"))
		return q3b_mapname;
	if (!Q_stricmp(name, "sv_mapChecksum")) {
		Com_sprintf(buf, sizeof(buf), "%d", q3b_checksum);
		return buf;
	}
	if (!Q_stricmp(name, "fs_basepath") || !Q_stricmp(name, "fs_homepath") || !Q_stricmp(name, "fs_game")
		|| !Q_stricmp(name, "fs_basegame") || !Q_stricmp(name, "logfile"))
		return "";
	return NULL;
}

// a vmCvar_t's handle is a slot here: its name, and the Quake II cvar
// (q2b_cvar_register) when it is not one of the above
#define MAX_Q3B_CVARS	256

typedef struct {
	char	name[MAX_CVAR_VALUE_STRING];
	int		q2handle;	// -1: Q3B_SpecialCvar
} q3bcvar_t;

static q3bcvar_t	q3b_cvars[MAX_Q3B_CVARS];
static int			q3b_numcvars;

static void Q3B_CvarFill(vmCvar_t *cv) {
	q3bcvar_t	*c;
	const char	*special;

	if (cv->handle < 0 || cv->handle >= q3b_numcvars)
		return;
	c = &q3b_cvars[cv->handle];
	if (c->q2handle < 0) {
		special = Q3B_SpecialCvar(c->name);
		Q_strncpyz(cv->string, special ? special : "", sizeof(cv->string));
		cv->value = atof(cv->string);
		cv->integer = atoi(cv->string);
		return;
	}
	q2b_cvar_read(c->q2handle, &cv->value, &cv->integer, cv->string, sizeof(cv->string), &cv->modificationCount);
}

void trap_Cvar_Register(vmCvar_t *cvar, const char *var_name, const char *value, int flags) {
	int i;

	for (i = 0; i < q3b_numcvars; i++)
		if (!Q_stricmp(q3b_cvars[i].name, var_name))
			break;
	if (i == q3b_numcvars) {
		if (q3b_numcvars == MAX_Q3B_CVARS) {
			q2b_print(1, va("Quake III bots: too many cvars (%s)\n", var_name));
			if (cvar)
				memset(cvar, 0, sizeof(*cvar));
			return;
		}
		Q_strncpyz(q3b_cvars[i].name, var_name, sizeof(q3b_cvars[i].name));
		q3b_cvars[i].q2handle = Q3B_SpecialCvar(var_name) ? -1
			: q2b_cvar_register(var_name, value, (flags & CVAR_ARCHIVE) ? 1 : 0);
		q3b_numcvars++;
	}

	if (!cvar)
		return;
	memset(cvar, 0, sizeof(*cvar));
	cvar->handle = i;
	Q3B_CvarFill(cvar);
}

void trap_Cvar_Update(vmCvar_t *cvar) {
	Q3B_CvarFill(cvar);
}

void trap_Cvar_Set(const char *var_name, const char *value) {
	if (Q3B_SpecialCvar(var_name))
		return;
	q2b_cvar_set(var_name, value);
}

void trap_Cvar_VariableStringBuffer(const char *var_name, char *buffer, int bufsize) {
	const char *special = Q3B_SpecialCvar(var_name);

	if (special)
		Q_strncpyz(buffer, special, bufsize);
	else
		q2b_cvar_string(var_name, buffer, bufsize);
}

int trap_Cvar_VariableIntegerValue(const char *var_name) {
	char buf[MAX_CVAR_VALUE_STRING];

	trap_Cvar_VariableStringBuffer(var_name, buf, sizeof(buf));
	return atoi(buf);
}

float trap_Cvar_VariableValue(const char *var_name) {
	char buf[MAX_CVAR_VALUE_STRING];

	trap_Cvar_VariableStringBuffer(var_name, buf, sizeof(buf));
	return atof(buf);
}

void trap_LocateGameData(gentity_t *gEnts, int numGEntities, int sizeofGEntity_t, playerState_t *gameClients, int sizeofGameClient) { }
void trap_DropClient(int clientNum, const char *reason) { }
void trap_SendServerCommand(int clientNum, const char *text) { }
void trap_SetConfigstring(int num, const char *string) { }

// a player's configstring, as Quake III's ClientUserinfoChanged wrote it
static void Q3B_PlayerConfigstring(int client, char *buffer, int size) {
	char		info[MAX_INFO_STRING];
	gclient_t	*cl;

	buffer[0] = 0;
	if (client < 0 || client >= level.maxclients)
		return;
	cl = &g_clients[client];
	if (cl->pers.connected == CON_DISCONNECTED)
		return;
	info[0] = 0;
	Info_SetValueForKey(info, "n", cl->pers.netname);
	Info_SetValueForKey(info, "t", va("%i", cl->sess.sessionTeam));
	Info_SetValueForKey(info, "model", q3b_clients[client].model);
	Info_SetValueForKey(info, "hmodel", q3b_clients[client].model);
	Info_SetValueForKey(info, "skill", q3b_clients[client].bot ? "1" : "");
	Q_strncpyz(buffer, info, size);
}

static void Q3B_ServerInfo(char *buffer, int size) {
	char info[MAX_INFO_STRING];

	info[0] = 0;
	Info_SetValueForKey(info, "mapname", q3b_mapname);
	Info_SetValueForKey(info, "g_gametype", va("%d", q2b_gametype()));
	Info_SetValueForKey(info, "sv_maxclients", va("%d", q2b_maxclients()));
	Q_strncpyz(buffer, info, size);
}

// the sounds the AI listens for, by the CS_SOUNDS index it is told
static const char *const q3b_sounds[] = {
	"",
	"sound/items/poweruprespawn.wav",
	"*falling1.wav",
};

void trap_GetConfigstring(int num, char *buffer, int bufferSize) {
	if (bufferSize <= 0)
		return;
	buffer[0] = 0;
	if (num >= CS_PLAYERS && num < CS_PLAYERS + MAX_CLIENTS)
		Q3B_PlayerConfigstring(num - CS_PLAYERS, buffer, bufferSize);
	else if (num == CS_SERVERINFO)
		Q3B_ServerInfo(buffer, bufferSize);
	else if (num >= CS_SOUNDS && num < CS_SOUNDS + (int)ARRAY_LEN(q3b_sounds))
		Q_strncpyz(buffer, q3b_sounds[num - CS_SOUNDS], bufferSize);
}

void trap_GetUserinfo(int num, char *buffer, int bufferSize) {
	if (bufferSize <= 0)
		return;
	buffer[0] = 0;
	if (num < 0 || num >= MAX_CLIENTS)
		return;
	Q_strncpyz(buffer, q3b_userinfo[num], bufferSize);
	if (!*Info_ValueForKey(buffer, "name") && num < level.maxclients) {
		Info_SetValueForKey(buffer, "name", g_clients[num].pers.netname);
		Info_SetValueForKey(buffer, "model", q3b_clients[num].model);
	}
}

void trap_SetUserinfo(int num, const char *buffer) {
	if (num >= 0 && num < MAX_CLIENTS)
		Q_strncpyz(q3b_userinfo[num], buffer, sizeof(q3b_userinfo[num]));
}

void trap_GetServerinfo(char *buffer, int bufferSize) {
	if (bufferSize > 0)
		Q3B_ServerInfo(buffer, bufferSize);
}

void trap_SetBrushModel(gentity_t *ent, const char *name) { }

void trap_Trace(trace_t *results, const vec3_t start, const vec3_t mins, const vec3_t maxs, const vec3_t end, int passEntityNum, int contentmask) {
	q3b_trace_t t;

	q2b_trace(&t, start, mins, maxs, end, passEntityNum, contentmask);
	memset(results, 0, sizeof(*results));
	results->allsolid = t.allsolid;
	results->startsolid = t.startsolid;
	results->fraction = t.fraction;
	VectorCopy(t.endpos, results->endpos);
	VectorCopy(t.normal, results->plane.normal);
	results->plane.dist = t.dist;
	SetPlaneSignbits(&results->plane);
	results->plane.type = PlaneTypeForNormal(results->plane.normal);
	results->surfaceFlags = t.surfaceflags;
	results->contents = t.contents;
	results->entityNum = t.entityNum;
}

int trap_PointContents(const vec3_t point, int passEntityNum) {
	return q2b_point_contents(point);
}

qboolean trap_InPVS(const vec3_t p1, const vec3_t p2) {
	return q2b_in_pvs(p1, p2);
}

qboolean trap_InPVSIgnorePortals(const vec3_t p1, const vec3_t p2) {
	return q2b_in_pvs(p1, p2);
}

void trap_AdjustAreaPortalState(gentity_t *ent, qboolean open) { }
qboolean trap_AreasConnected(int area1, int area2) { return qtrue; }
void trap_LinkEntity(gentity_t *ent) { }
void trap_UnlinkEntity(gentity_t *ent) { }
int trap_EntitiesInBox(const vec3_t mins, const vec3_t maxs, int *entityList, int maxcount) { return 0; }
qboolean trap_EntityContact(const vec3_t mins, const vec3_t maxs, const gentity_t *ent) { return qfalse; }
int trap_BotAllocateClient(void) { return -1; }
void trap_BotFreeClient(int clientNum) { }
void trap_GetUsercmd(int clientNum, usercmd_t *cmd) { memset(cmd, 0, sizeof(*cmd)); }
qboolean trap_GetEntityToken(char *buffer, int bufferSize) { return qfalse; }
int trap_DebugPolygonCreate(int color, int numPoints, vec3_t *points) { return 0; }
void trap_DebugPolygonDelete(int id) { }
void trap_SnapVector(float *v) { v[0] = (int)v[0]; v[1] = (int)v[1]; v[2] = (int)v[2]; }

// one command for the whole frame: only bot_pause sends this way, the AI's
// own commands go out in BotQ2_UserCommands
void trap_BotUserCommand(int client, usercmd_t *ucmd) {
	q3b_usercmd_t	cmd;
	int				j;

	if (client < 0 || client >= MAX_CLIENTS || !q3b_running[client])
		return;
	for (j = 0; j < 3; j++)
		cmd.angles[j] = ucmd->angles[j];
	cmd.buttons = ucmd->buttons;
	cmd.weapon = ucmd->weapon;
	cmd.forwardmove = ucmd->forwardmove;
	cmd.rightmove = ucmd->rightmove;
	cmd.upmove = ucmd->upmove;
	q2b_bot_usercmd(client, &cmd, 100);
}

// --------------------------------------------------------------------------
// botlib

int trap_BotLibSetup(void) { return botlib_export->BotLibSetup(); }
int trap_BotLibShutdown(void) { return botlib_export->BotLibShutdown(); }
int trap_BotLibVarSet(char *var_name, char *value) { return botlib_export->BotLibVarSet(var_name, value); }
int trap_BotLibVarGet(char *var_name, char *value, int size) { return botlib_export->BotLibVarGet(var_name, value, size); }
int trap_BotLibDefine(char *string) { return botlib_export->PC_AddGlobalDefine(string); }
int trap_BotLibStartFrame(float time) { return botlib_export->BotLibStartFrame(time); }
int trap_BotLibLoadMap(const char *mapname) { return botlib_export->BotLibLoadMap(mapname); }
int trap_BotLibUpdateEntity(int ent, void *bue) { return botlib_export->BotLibUpdateEntity(ent, bue); }
int trap_BotLibTest(int parm0, char *parm1, vec3_t parm2, vec3_t parm3) { return botlib_export->Test(parm0, parm1, parm2, parm3); }

int trap_AAS_BBoxAreas(vec3_t absmins, vec3_t absmaxs, int *areas, int maxareas) { return botlib_export->aas.AAS_BBoxAreas(absmins, absmaxs, areas, maxareas); }
int trap_AAS_AreaInfo(int areanum, void *info) { return botlib_export->aas.AAS_AreaInfo(areanum, info); }
void trap_AAS_EntityInfo(int entnum, void *info) { botlib_export->aas.AAS_EntityInfo(entnum, info); }
int trap_AAS_Initialized(void) { return botlib_export && botlib_setup ? botlib_export->aas.AAS_Initialized() : 0; }
void trap_AAS_PresenceTypeBoundingBox(int presencetype, vec3_t mins, vec3_t maxs) { botlib_export->aas.AAS_PresenceTypeBoundingBox(presencetype, mins, maxs); }
float trap_AAS_Time(void) { return botlib_export->aas.AAS_Time(); }
int trap_AAS_PointAreaNum(vec3_t point) { return botlib_export->aas.AAS_PointAreaNum(point); }
int trap_AAS_PointReachabilityAreaIndex(vec3_t point) { return botlib_export->aas.AAS_PointReachabilityAreaIndex(point); }
int trap_AAS_TraceAreas(vec3_t start, vec3_t end, int *areas, vec3_t *points, int maxareas) { return botlib_export->aas.AAS_TraceAreas(start, end, areas, points, maxareas); }
int trap_AAS_PointContents(vec3_t point) { return botlib_export->aas.AAS_PointContents(point); }
int trap_AAS_NextBSPEntity(int ent) { return botlib_export->aas.AAS_NextBSPEntity(ent); }
int trap_AAS_ValueForBSPEpairKey(int ent, char *key, char *value, int size) { return botlib_export->aas.AAS_ValueForBSPEpairKey(ent, key, value, size); }
int trap_AAS_VectorForBSPEpairKey(int ent, char *key, vec3_t v) { return botlib_export->aas.AAS_VectorForBSPEpairKey(ent, key, v); }
int trap_AAS_FloatForBSPEpairKey(int ent, char *key, float *value) { return botlib_export->aas.AAS_FloatForBSPEpairKey(ent, key, value); }
int trap_AAS_IntForBSPEpairKey(int ent, char *key, int *value) { return botlib_export->aas.AAS_IntForBSPEpairKey(ent, key, value); }
int trap_AAS_AreaReachability(int areanum) { return botlib_export->aas.AAS_AreaReachability(areanum); }
int trap_AAS_AreaTravelTimeToGoalArea(int areanum, vec3_t origin, int goalareanum, int travelflags) { return botlib_export->aas.AAS_AreaTravelTimeToGoalArea(areanum, origin, goalareanum, travelflags); }
int trap_AAS_EnableRoutingArea(int areanum, int enable) { return botlib_export->aas.AAS_EnableRoutingArea(areanum, enable); }
int trap_AAS_PredictRoute(void *route, int areanum, vec3_t origin, int goalareanum, int travelflags, int maxareas, int maxtime, int stopevent, int stopcontents, int stoptfl, int stopareanum) {
	return botlib_export->aas.AAS_PredictRoute(route, areanum, origin, goalareanum, travelflags, maxareas, maxtime, stopevent, stopcontents, stoptfl, stopareanum);
}
int trap_AAS_AlternativeRouteGoals(vec3_t start, int startareanum, vec3_t goal, int goalareanum, int travelflags, void *altroutegoals, int maxaltroutegoals, int type) {
	return botlib_export->aas.AAS_AlternativeRouteGoals(start, startareanum, goal, goalareanum, travelflags, altroutegoals, maxaltroutegoals, type);
}
int trap_AAS_Swimming(vec3_t origin) { return botlib_export->aas.AAS_Swimming(origin); }
int trap_AAS_PredictClientMovement(void *move, int entnum, vec3_t origin, int presencetype, int onground, vec3_t velocity, vec3_t cmdmove, int cmdframes, int maxframes, float frametime, int stopevent, int stopareanum, int visualize) {
	return botlib_export->aas.AAS_PredictClientMovement(move, entnum, origin, presencetype, onground, velocity, cmdmove, cmdframes, maxframes, frametime, stopevent, stopareanum, visualize);
}

void trap_EA_Say(int client, char *str) { botlib_export->ea.EA_Say(client, str); }
void trap_EA_SayTeam(int client, char *str) { botlib_export->ea.EA_SayTeam(client, str); }
void trap_EA_Command(int client, char *command) { botlib_export->ea.EA_Command(client, command); }
void trap_EA_Action(int client, int action) { botlib_export->ea.EA_Action(client, action); }
void trap_EA_Gesture(int client) { botlib_export->ea.EA_Gesture(client); }
void trap_EA_Talk(int client) { botlib_export->ea.EA_Talk(client); }
void trap_EA_Attack(int client) { botlib_export->ea.EA_Attack(client); }
void trap_EA_Use(int client) { botlib_export->ea.EA_Use(client); }
void trap_EA_Respawn(int client) { botlib_export->ea.EA_Respawn(client); }
void trap_EA_Crouch(int client) { botlib_export->ea.EA_Crouch(client); }
void trap_EA_MoveUp(int client) { botlib_export->ea.EA_MoveUp(client); }
void trap_EA_MoveDown(int client) { botlib_export->ea.EA_MoveDown(client); }
void trap_EA_MoveForward(int client) { botlib_export->ea.EA_MoveForward(client); }
void trap_EA_MoveBack(int client) { botlib_export->ea.EA_MoveBack(client); }
void trap_EA_MoveLeft(int client) { botlib_export->ea.EA_MoveLeft(client); }
void trap_EA_MoveRight(int client) { botlib_export->ea.EA_MoveRight(client); }
void trap_EA_SelectWeapon(int client, int weapon) { botlib_export->ea.EA_SelectWeapon(client, weapon); }
void trap_EA_Jump(int client) { botlib_export->ea.EA_Jump(client); }
void trap_EA_DelayedJump(int client) { botlib_export->ea.EA_DelayedJump(client); }
void trap_EA_Move(int client, vec3_t dir, float speed) { botlib_export->ea.EA_Move(client, dir, speed); }
void trap_EA_View(int client, vec3_t viewangles) { botlib_export->ea.EA_View(client, viewangles); }
void trap_EA_EndRegular(int client, float thinktime) { botlib_export->ea.EA_EndRegular(client, thinktime); }
void trap_EA_GetInput(int client, float thinktime, void *input) { botlib_export->ea.EA_GetInput(client, thinktime, input); }
void trap_EA_ResetInput(int client) { botlib_export->ea.EA_ResetInput(client); }

int trap_BotLoadCharacter(char *charfile, float skill) { return botlib_export->ai.BotLoadCharacter(charfile, skill); }
void trap_BotFreeCharacter(int character) { botlib_export->ai.BotFreeCharacter(character); }
float trap_Characteristic_Float(int character, int index) { return botlib_export->ai.Characteristic_Float(character, index); }
float trap_Characteristic_BFloat(int character, int index, float min, float max) { return botlib_export->ai.Characteristic_BFloat(character, index, min, max); }
int trap_Characteristic_Integer(int character, int index) { return botlib_export->ai.Characteristic_Integer(character, index); }
int trap_Characteristic_BInteger(int character, int index, int min, int max) { return botlib_export->ai.Characteristic_BInteger(character, index, min, max); }
void trap_Characteristic_String(int character, int index, char *buf, int size) { botlib_export->ai.Characteristic_String(character, index, buf, size); }

int trap_BotAllocChatState(void) { return botlib_export->ai.BotAllocChatState(); }
void trap_BotFreeChatState(int handle) { botlib_export->ai.BotFreeChatState(handle); }
void trap_BotQueueConsoleMessage(int chatstate, int type, char *message) { botlib_export->ai.BotQueueConsoleMessage(chatstate, type, message); }
void trap_BotRemoveConsoleMessage(int chatstate, int handle) { botlib_export->ai.BotRemoveConsoleMessage(chatstate, handle); }
int trap_BotNextConsoleMessage(int chatstate, void *cm) { return botlib_export->ai.BotNextConsoleMessage(chatstate, cm); }
int trap_BotNumConsoleMessages(int chatstate) { return botlib_export->ai.BotNumConsoleMessages(chatstate); }
void trap_BotInitialChat(int chatstate, char *type, int mcontext, char *var0, char *var1, char *var2, char *var3, char *var4, char *var5, char *var6, char *var7) {
	botlib_export->ai.BotInitialChat(chatstate, type, mcontext, var0, var1, var2, var3, var4, var5, var6, var7);
}
int trap_BotNumInitialChats(int chatstate, char *type) { return botlib_export->ai.BotNumInitialChats(chatstate, type); }
int trap_BotReplyChat(int chatstate, char *message, int mcontext, int vcontext, char *var0, char *var1, char *var2, char *var3, char *var4, char *var5, char *var6, char *var7) {
	return botlib_export->ai.BotReplyChat(chatstate, message, mcontext, vcontext, var0, var1, var2, var3, var4, var5, var6, var7);
}
int trap_BotChatLength(int chatstate) { return botlib_export->ai.BotChatLength(chatstate); }
void trap_BotEnterChat(int chatstate, int client, int sendto) { botlib_export->ai.BotEnterChat(chatstate, client, sendto); }
void trap_BotGetChatMessage(int chatstate, char *buf, int size) { botlib_export->ai.BotGetChatMessage(chatstate, buf, size); }
int trap_StringContains(char *str1, char *str2, int casesensitive) { return botlib_export->ai.StringContains(str1, str2, casesensitive); }
int trap_BotFindMatch(char *str, void *match, unsigned long int context) { return botlib_export->ai.BotFindMatch(str, match, context); }
void trap_BotMatchVariable(void *match, int variable, char *buf, int size) { botlib_export->ai.BotMatchVariable(match, variable, buf, size); }
void trap_UnifyWhiteSpaces(char *string) { botlib_export->ai.UnifyWhiteSpaces(string); }
void trap_BotReplaceSynonyms(char *string, unsigned long int context) { botlib_export->ai.BotReplaceSynonyms(string, context); }
int trap_BotLoadChatFile(int chatstate, char *chatfile, char *chatname) { return botlib_export->ai.BotLoadChatFile(chatstate, chatfile, chatname); }
void trap_BotSetChatGender(int chatstate, int gender) { botlib_export->ai.BotSetChatGender(chatstate, gender); }
void trap_BotSetChatName(int chatstate, char *name, int client) { botlib_export->ai.BotSetChatName(chatstate, name, client); }

void trap_BotResetGoalState(int goalstate) { botlib_export->ai.BotResetGoalState(goalstate); }
void trap_BotRemoveFromAvoidGoals(int goalstate, int number) { botlib_export->ai.BotRemoveFromAvoidGoals(goalstate, number); }
void trap_BotResetAvoidGoals(int goalstate) { botlib_export->ai.BotResetAvoidGoals(goalstate); }
void trap_BotPushGoal(int goalstate, void *goal) { botlib_export->ai.BotPushGoal(goalstate, goal); }
void trap_BotPopGoal(int goalstate) { botlib_export->ai.BotPopGoal(goalstate); }
void trap_BotEmptyGoalStack(int goalstate) { botlib_export->ai.BotEmptyGoalStack(goalstate); }
void trap_BotDumpAvoidGoals(int goalstate) { botlib_export->ai.BotDumpAvoidGoals(goalstate); }
void trap_BotDumpGoalStack(int goalstate) { botlib_export->ai.BotDumpGoalStack(goalstate); }
void trap_BotGoalName(int number, char *name, int size) { botlib_export->ai.BotGoalName(number, name, size); }
int trap_BotGetTopGoal(int goalstate, void *goal) { return botlib_export->ai.BotGetTopGoal(goalstate, goal); }
int trap_BotGetSecondGoal(int goalstate, void *goal) { return botlib_export->ai.BotGetSecondGoal(goalstate, goal); }
int trap_BotChooseLTGItem(int goalstate, vec3_t origin, int *inventory, int travelflags) { return botlib_export->ai.BotChooseLTGItem(goalstate, origin, inventory, travelflags); }
int trap_BotChooseNBGItem(int goalstate, vec3_t origin, int *inventory, int travelflags, void *ltg, float maxtime) {
	return botlib_export->ai.BotChooseNBGItem(goalstate, origin, inventory, travelflags, ltg, maxtime);
}
int trap_BotTouchingGoal(vec3_t origin, void *goal) { return botlib_export->ai.BotTouchingGoal(origin, goal); }
int trap_BotItemGoalInVisButNotVisible(int viewer, vec3_t eye, vec3_t viewangles, void *goal) { return botlib_export->ai.BotItemGoalInVisButNotVisible(viewer, eye, viewangles, goal); }
int trap_BotGetNextCampSpotGoal(int num, void *goal) { return botlib_export->ai.BotGetNextCampSpotGoal(num, goal); }
int trap_BotGetMapLocationGoal(char *name, void *goal) { return botlib_export->ai.BotGetMapLocationGoal(name, goal); }
int trap_BotGetLevelItemGoal(int index, char *classname, void *goal) { return botlib_export->ai.BotGetLevelItemGoal(index, classname, goal); }
float trap_BotAvoidGoalTime(int goalstate, int number) { return botlib_export->ai.BotAvoidGoalTime(goalstate, number); }
void trap_BotSetAvoidGoalTime(int goalstate, int number, float avoidtime) { botlib_export->ai.BotSetAvoidGoalTime(goalstate, number, avoidtime); }
void trap_BotInitLevelItems(void) { botlib_export->ai.BotInitLevelItems(); }
void trap_BotUpdateEntityItems(void) { botlib_export->ai.BotUpdateEntityItems(); }
int trap_BotLoadItemWeights(int goalstate, char *filename) { return botlib_export->ai.BotLoadItemWeights(goalstate, filename); }
void trap_BotFreeItemWeights(int goalstate) { botlib_export->ai.BotFreeItemWeights(goalstate); }
void trap_BotInterbreedGoalFuzzyLogic(int parent1, int parent2, int child) { botlib_export->ai.BotInterbreedGoalFuzzyLogic(parent1, parent2, child); }
void trap_BotSaveGoalFuzzyLogic(int goalstate, char *filename) { botlib_export->ai.BotSaveGoalFuzzyLogic(goalstate, filename); }
void trap_BotMutateGoalFuzzyLogic(int goalstate, float range) { botlib_export->ai.BotMutateGoalFuzzyLogic(goalstate, range); }
int trap_BotAllocGoalState(int state) { return botlib_export->ai.BotAllocGoalState(state); }
void trap_BotFreeGoalState(int handle) { botlib_export->ai.BotFreeGoalState(handle); }

void trap_BotResetMoveState(int movestate) { botlib_export->ai.BotResetMoveState(movestate); }
void trap_BotMoveToGoal(void *result, int movestate, void *goal, int travelflags) { botlib_export->ai.BotMoveToGoal(result, movestate, goal, travelflags); }
int trap_BotMoveInDirection(int movestate, vec3_t dir, float speed, int type) { return botlib_export->ai.BotMoveInDirection(movestate, dir, speed, type); }
void trap_BotResetAvoidReach(int movestate) { botlib_export->ai.BotResetAvoidReach(movestate); }
void trap_BotResetLastAvoidReach(int movestate) { botlib_export->ai.BotResetLastAvoidReach(movestate); }
int trap_BotReachabilityArea(vec3_t origin, int testground) { return botlib_export->ai.BotReachabilityArea(origin, testground); }
int trap_BotMovementViewTarget(int movestate, void *goal, int travelflags, float lookahead, vec3_t target) { return botlib_export->ai.BotMovementViewTarget(movestate, goal, travelflags, lookahead, target); }
int trap_BotPredictVisiblePosition(vec3_t origin, int areanum, void *goal, int travelflags, vec3_t target) { return botlib_export->ai.BotPredictVisiblePosition(origin, areanum, goal, travelflags, target); }
int trap_BotAllocMoveState(void) { return botlib_export->ai.BotAllocMoveState(); }
void trap_BotFreeMoveState(int handle) { botlib_export->ai.BotFreeMoveState(handle); }
void trap_BotInitMoveState(int handle, void *initmove) { botlib_export->ai.BotInitMoveState(handle, initmove); }
void trap_BotAddAvoidSpot(int movestate, vec3_t origin, float radius, int type) { botlib_export->ai.BotAddAvoidSpot(movestate, origin, radius, type); }

int trap_BotChooseBestFightWeapon(int weaponstate, int *inventory) { return botlib_export->ai.BotChooseBestFightWeapon(weaponstate, inventory); }
void trap_BotGetWeaponInfo(int weaponstate, int weapon, void *weaponinfo) { botlib_export->ai.BotGetWeaponInfo(weaponstate, weapon, weaponinfo); }
int trap_BotLoadWeaponWeights(int weaponstate, char *filename) { return botlib_export->ai.BotLoadWeaponWeights(weaponstate, filename); }
int trap_BotAllocWeaponState(void) { return botlib_export->ai.BotAllocWeaponState(); }
void trap_BotFreeWeaponState(int weaponstate) { botlib_export->ai.BotFreeWeaponState(weaponstate); }
void trap_BotResetWeaponState(int weaponstate) { botlib_export->ai.BotResetWeaponState(weaponstate); }

int trap_GeneticParentsAndChildSelection(int numranks, float *ranks, int *parent1, int *parent2, int *child) {
	return botlib_export->ai.GeneticParentsAndChildSelection(numranks, ranks, parent1, parent2, child);
}
