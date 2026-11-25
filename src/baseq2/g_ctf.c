/*
Copyright (C) 1997-2001 Id Software, Inc.
Copyright (c) ZeniMax Media Inc.

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
CAPTURE THE FLAG

`ctf 1` in deathmatch: id's Threewave CTF as the rerelease plays it, carried
over from its source (src/rerelease/ctf/g_ctf.cpp) - the flags, captures and
returns and their bonuses, the team spawn points, the four techs, the
grappling hook, and the HUD. Its match mode, voting, admin menus and ghost
codes are left out; the teams are the team play ones (g_bot.c), red as
team 1 and blue as team 2, so the bots, the `team` command and the team
skins all come with them.

The maps and media are the rerelease's own (q2ctf1-5, q2kctf1-2, ndctf0,
the flags, techs, hook and icons in its pak0), so it is a rerelease game
mode; the classic game has none of it without id's CTF add-on.
*/

#include "g_local.h"

cvar_t  *ctf;
static cvar_t   *capturelimit;

#define CTF_CAPTURE_BONUS               15  // what you get for capture
#define CTF_TEAM_BONUS                  10  // what your team gets for capture
#define CTF_RECOVERY_BONUS              1   // what you get for recovery
#define CTF_FRAG_CARRIER_BONUS          2   // what you get for fragging enemy flag carrier
#define CTF_CARRIER_DANGER_PROTECT_BONUS 2  // fragging someone who recently hurt your carrier
#define CTF_CARRIER_PROTECT_BONUS       1   // fragging someone near your flag carrier
#define CTF_FLAG_DEFENSE_BONUS          1   // fragging someone near your flag
#define CTF_RETURN_FLAG_ASSIST_BONUS    1   // returning a flag shortly before a capture
#define CTF_FRAG_CARRIER_ASSIST_BONUS   2   // fragging a carrier shortly before a capture

#define CTF_TARGET_PROTECT_RADIUS       400
#define CTF_ATTACKER_PROTECT_RADIUS     400

#define CTF_CARRIER_DANGER_PROTECT_TIMEOUT  8
#define CTF_FRAG_CARRIER_ASSIST_TIMEOUT     10
#define CTF_RETURN_FLAG_ASSIST_TIMEOUT      10
#define CTF_AUTO_FLAG_RETURN_TIMEOUT        30  // a dropped flag goes home
#define CTF_TECH_TIMEOUT                    60  // an untouched tech moves

#define CTF_GRAPPLE_SPEED       650     // the hook in flight
#define CTF_GRAPPLE_PULL_SPEED  650     // the player on the line
#define CTF_GRAPPLE_DAMAGE      10

/* QUAKE III'S HOOK (g_q3weap.c, weapon_q3_grapple): this grapple's machinery -
   the client's ctf_grapple, the pull each command, the resets on death and at
   teleporters and jump pads - with Quake III's rules: g_missile.c fire_grapple
   and G_MissileImpact (no damage; onto a player it follows them, anything else
   holds the point), g_weapon.c Weapon_HookThink, bg_pmove.c PM_GrappleMove.
   It looks like Quake III's: its missile is the rocket model, it has no
   sounds of its own but the hit, and its line is the lightning bolt the
   client draws (TE_Q3_GRAPPLE, q3weapons.c). */
#define GRAPPLE_Q3              1       // the hook's spawnflags: Quake III's
#define Q3_GRAPPLE_SPEED        800
#define Q3_GRAPPLE_PULL_SPEED   800
#define Q3_GRAPPLE_FLY_TIME     10      // seconds in the air before it's freed

static bool CTF_HookQ3(const edict_t *hook)
{
    return (hook->spawnflags & GRAPPLE_Q3) != 0;
}

static struct {
    int     caps[3];                // by team
    float   last_capture_time;
    int     last_capture_team;
} ctfgame;

static gitem_t  *flag_items[3];     // [TEAM_RED] item_flag_team1, [TEAM_BLUE] item_flag_team2
static gitem_t  *tech_items[4];     // resistance, strength, haste, regeneration
static gitem_t  *grapple_item;

static int  pic_ctf1, pic_ctf2, pic_ctf1d, pic_ctf2d, pic_ctf1t, pic_ctf2t, pic_ctfj;
static int  model_flag1, model_flag2;

edict_t *SelectRandomDeathmatchSpawnPoint(void);
void SV_AddGravity(edict_t *ent);
void EndDMLevel(void);
void Weapon_Generic(edict_t *ent, int FRAME_ACTIVATE_LAST, int FRAME_FIRE_LAST, int FRAME_IDLE_LAST,
                    int FRAME_DEACTIVATE_LAST, int *pause_frames, int *fire_frames, void (*fire)(edict_t *ent));

bool G_CTF(void)
{
    return deathmatch->value && ctf && ctf->value;
}

static int CTF_OtherTeam(int team)
{
    return team == TEAM_RED ? TEAM_BLUE : team == TEAM_BLUE ? TEAM_RED : TEAM_NONE;
}

static int CTF_FlagTeam(const gitem_t *item)
{
    if (item && item == flag_items[TEAM_RED])
        return TEAM_RED;
    if (item && item == flag_items[TEAM_BLUE])
        return TEAM_BLUE;
    return TEAM_NONE;
}

static int CTF_FlagIndex(int team)
{
    return flag_items[team] ? ITEM_INDEX(flag_items[team]) : 0;
}

static const char *CTF_FlagName(int team)
{
    return team == TEAM_RED ? "RED" : "BLUE";
}

int CTF_Caps(int team)
{
    return (team == TEAM_RED || team == TEAM_BLUE) ? ctfgame.caps[team] : 0;
}

// does this player carry a flag - the other team's, by the rules
bool CTF_HasFlag(const edict_t *ent)
{
    const gclient_t *cl = ent->client;

    return cl && G_CTF() &&
           ((CTF_FlagIndex(TEAM_RED) && cl->pers.inventory[CTF_FlagIndex(TEAM_RED)]) ||
            (CTF_FlagIndex(TEAM_BLUE) && cl->pers.inventory[CTF_FlagIndex(TEAM_BLUE)]));
}

/*
=================
CTF_Init

InitGame, once the items are set up.
=================
*/
void CTF_Init(void)
{
    ctf = gi.cvar("ctf", "0", CVAR_SERVERINFO | CVAR_LATCH);
    capturelimit = gi.cvar("capturelimit", "0", CVAR_SERVERINFO);

    flag_items[TEAM_RED] = FindItemByClassname("item_flag_team1");
    flag_items[TEAM_BLUE] = FindItemByClassname("item_flag_team2");
    tech_items[0] = FindItemByClassname("item_tech1");
    tech_items[1] = FindItemByClassname("item_tech2");
    tech_items[2] = FindItemByClassname("item_tech3");
    tech_items[3] = FindItemByClassname("item_tech4");
    grapple_item = FindItemByClassname("weapon_grapple");
}

/*
=================
CTF_Precache

SP_worldspawn: the HUD's icons, the flags worn on the back, the techs and the
hook, none of which a map places.
=================
*/
void CTF_Precache(void)
{
    int i;

    pic_ctf1 = gi.imageindex("i_ctf1");
    pic_ctf2 = gi.imageindex("i_ctf2");
    pic_ctf1d = gi.imageindex("i_ctf1d");
    pic_ctf2d = gi.imageindex("i_ctf2d");
    pic_ctf1t = gi.imageindex("i_ctf1t");
    pic_ctf2t = gi.imageindex("i_ctf2t");
    pic_ctfj = gi.imageindex("i_ctfj");
    model_flag1 = gi.modelindex("players/male/flag1.md2");
    model_flag2 = gi.modelindex("players/male/flag2.md2");

    gi.soundindex("ctf/flagcap.wav");
    gi.soundindex("ctf/flagret.wav");

    for (i = 0; i < 4; i++)
        if (tech_items[i])
            PrecacheItem(tech_items[i]);
    if (grapple_item)
        PrecacheItem(grapple_item);
    gi.modelindex("models/weapons/grapple/hook/tris.md2");

    // the hook on the player's model: WEAP_GRAPPLE, the last of the vwep list
    // SP_worldspawn registers, so it can only be added after the rest
    gi.modelindex("#w_grapple.md2");
}

// the HUD: team icons and captures on the right, the carried flag at the top,
// the tech above the teams - in the stats g_local.h gives CTF
#define CTF_STATUSBAR \
    "yb -102 " \
    "if 24 xr -26 pic 24 endif " \
    "xr -62 num 2 25 " \
    "if 29 yb -104 xr -28 pic 29 endif " \
    "yb -75 " \
    "if 26 xr -26 pic 26 endif " \
    "xr -62 num 2 27 " \
    "if 30 yb -77 xr -28 pic 30 endif " \
    "if 28 yt 26 xr -24 pic 28 endif " \
    "yb -129 " \
    "if 31 xr -26 pic 31 endif "

const char *CTF_StatusBar(const char *dm_statusbar)
{
    static char sb[1536];

    Q_snprintf(sb, sizeof(sb), "%s%s", dm_statusbar, CTF_STATUSBAR);
    return sb;
}

// ==========================================================================
// SPAWNING

/*QUAKED info_player_team1 (1 0 0) (-16 -16 -24) (16 16 32)
potential team1 spawning position for ctf games
*/
void SP_info_player_team1(edict_t *self)
{
}

/*QUAKED info_player_team2 (0 0 1) (-16 -16 -24) (16 16 32)
potential team2 spawning position for ctf games
*/
void SP_info_player_team2(edict_t *self)
{
}

static bool CTF_SpotClear(edict_t *spot)
{
    vec3_t  mins, maxs;
    edict_t *touch[8];
    int     i, n;

    VectorAdd(spot->s.origin, tv(-16, -16, -24), mins);
    VectorAdd(spot->s.origin, tv(16, 16, 32), maxs);
    n = gi.BoxEdicts(mins, maxs, touch, 8, AREA_SOLID);
    for (i = 0; i < n; i++)
        if (touch[i]->client && touch[i]->health > 0)
            return false;
    return true;
}

// a team spawn point for this player's team, or NULL for a deathmatch one
edict_t *CTF_SpawnPoint(edict_t *ent)
{
    edict_t     *spots[64], *spot = NULL;
    const char  *cname;
    int         n = 0, i, start;

    if (!G_CTF() || !ent->client)
        return NULL;

    if (ent->client->resp.team == TEAM_RED)
        cname = "info_player_team1";
    else if (ent->client->resp.team == TEAM_BLUE)
        cname = "info_player_team2";
    else
        return NULL;

    while ((spot = G_Find(spot, FOFS(classname), (char *)cname)) != NULL && n < 64)
        spots[n++] = spot;
    if (!n)
        return NULL;

    start = Q_rand() % n;
    for (i = 0; i < n; i++)
        if (CTF_SpotClear(spots[(start + i) % n]))
            return spots[(start + i) % n];

    return spots[start];
}

// ==========================================================================
// FLAGS

static void CTF_FlagThink(edict_t *ent)
{
    if (ent->solid != SOLID_NOT)
        ent->s.frame = 173 + (((ent->s.frame - 173) + 1) % 16);
    ent->nextthink = level.framenum + 1;
}

// the flag at its stand, settled onto the floor - SpawnItem's think for one
void CTF_FlagSetup(edict_t *ent)
{
    trace_t tr;
    vec3_t  dest;

    VectorSet(ent->mins, -15, -15, -15);
    VectorSet(ent->maxs, 15, 15, 15);

    if (ent->model)
        gi.setmodel(ent, ent->model);
    else
        gi.setmodel(ent, ent->item->world_model);
    ent->solid = SOLID_TRIGGER;
    ent->movetype = MOVETYPE_TOSS;
    ent->touch = Touch_Item;
    ent->s.frame = 173;

    VectorCopy(ent->s.origin, dest);
    dest[2] -= 128;
    // a Quake 1 map traces this box as a whole player (droptofloor says why):
    // drop the flag's centre as a point, and stand it on what that finds
    if (level.q1map) {
        tr = gi.trace(ent->s.origin, vec3_origin, vec3_origin, dest, ent, MASK_SOLID);
        if (!tr.startsolid && tr.fraction < 1) {
            tr.endpos[2] -= ent->mins[2];
            goto placed;
        }
    }
    tr = gi.trace(ent->s.origin, ent->mins, ent->maxs, dest, ent, MASK_SOLID);
    if (tr.startsolid) {
        gi.dprintf("CTF_FlagSetup: %s startsolid at %s\n", ent->classname, vtos(ent->s.origin));
        G_FreeEdict(ent);
        return;
    }

placed:
    VectorCopy(tr.endpos, ent->s.origin);
    gi.linkentity(ent);

    ent->nextthink = level.framenum + 1;
    ent->think = CTF_FlagThink;
}

/*
Every flag of a team back on its stand. A dropped one goes - but not at once:
this is called from inside its own touch (a return), and Touch_Item still
writes to it afterwards, so it hides now and is freed next frame.
*/
static void CTF_ResetFlag(int team)
{
    edict_t *ent = NULL;

    if (!flag_items[team])
        return;

    while ((ent = G_Find(ent, FOFS(classname), flag_items[team]->classname)) != NULL) {
        if (ent->spawnflags & DROPPED_ITEM) {
            ent->svflags |= SVF_NOCLIENT;
            ent->solid = SOLID_NOT;
            ent->touch = NULL;
            ent->think = G_FreeEdict;
            ent->nextthink = level.framenum + 1;
            gi.linkentity(ent);
        } else {
            ent->svflags &= ~SVF_NOCLIENT;
            ent->solid = SOLID_TRIGGER;
            gi.linkentity(ent);
            ent->s.event = EV_ITEM_RESPAWN;
        }
    }
}

static void CTF_ResetFlags(void)
{
    CTF_ResetFlag(TEAM_RED);
    CTF_ResetFlag(TEAM_BLUE);
}

bool CTFPickup_Flag(edict_t *ent, edict_t *other)
{
    int     team = CTF_FlagTeam(ent->item);
    int     mine = other->client->resp.team;
    int     enemy_index, i;
    edict_t *player;

    if (!G_CTF() || team == TEAM_NONE || (mine != TEAM_RED && mine != TEAM_BLUE))
        return false;

    enemy_index = CTF_FlagIndex(CTF_OtherTeam(team));

    if (team == mine) {
        if (!(ent->spawnflags & DROPPED_ITEM)) {
            // our flag on its stand: bringing theirs here is a capture
            if (!other->client->pers.inventory[enemy_index])
                return false;

            gi.bprintf(PRINT_HIGH, "%s captured the %s flag!\n",
                       other->client->pers.netname, CTF_FlagName(CTF_OtherTeam(team)));
            other->client->pers.inventory[enemy_index] = 0;

            ctfgame.last_capture_time = level.time;
            ctfgame.last_capture_team = team;
            ctfgame.caps[team]++;
            BotEngine_FlagCaptured(team);   // Quake III's bots: both flags home

            gi.sound(ent, CHAN_RELIABLE | CHAN_NO_PHS_ADD | CHAN_ITEM, gi.soundindex("ctf/flagcap.wav"), 1, ATTN_NONE, 0);

            other->client->resp.score += CTF_CAPTURE_BONUS;

            for (i = 1; i <= game.maxclients; i++) {
                player = g_edicts + i;
                if (!player->inuse || !player->client)
                    continue;

                if (player->client->resp.team != mine) {
                    player->client->resp.ctf_lasthurtcarrier = -5;
                } else {
                    if (player != other)
                        player->client->resp.score += CTF_TEAM_BONUS;
                    // extra for the help
                    if (player->client->resp.ctf_lastreturnedflag &&
                        player->client->resp.ctf_lastreturnedflag + CTF_RETURN_FLAG_ASSIST_TIMEOUT > level.time) {
                        gi.bprintf(PRINT_HIGH, "%s gets an assist for returning the flag!\n", player->client->pers.netname);
                        player->client->resp.score += CTF_RETURN_FLAG_ASSIST_BONUS;
                    }
                    if (player->client->resp.ctf_lastfraggedcarrier &&
                        player->client->resp.ctf_lastfraggedcarrier + CTF_FRAG_CARRIER_ASSIST_TIMEOUT > level.time) {
                        gi.bprintf(PRINT_HIGH, "%s gets an assist for fragging the flag carrier!\n", player->client->pers.netname);
                        player->client->resp.score += CTF_FRAG_CARRIER_ASSIST_BONUS;
                    }
                }
            }

            CTF_ResetFlags();
            return false;
        }

        // ours, lying somewhere: send it home
        gi.bprintf(PRINT_HIGH, "%s returned the %s flag!\n", other->client->pers.netname, CTF_FlagName(team));
        other->client->resp.score += CTF_RECOVERY_BONUS;
        other->client->resp.ctf_lastreturnedflag = level.time;
        gi.sound(ent, CHAN_RELIABLE | CHAN_NO_PHS_ADD | CHAN_ITEM, gi.soundindex("ctf/flagret.wav"), 1, ATTN_NONE, 0);
        CTF_ResetFlag(team);
        BotEngine_FlagReturned(team);
        return false;
    }

    // theirs: take it
    gi.bprintf(PRINT_HIGH, "%s got the %s flag!\n", other->client->pers.netname, CTF_FlagName(team));
    BotEngine_FlagTaken(team);

    other->client->pers.inventory[CTF_FlagIndex(team)] = 1;
    other->client->resp.ctf_flagsince = level.time;

    // from its stand it only hides, to come back on a capture or a return;
    // a dropped one is freed by Touch_Item
    if (!(ent->spawnflags & DROPPED_ITEM)) {
        ent->flags |= FL_RESPAWN;
        ent->svflags |= SVF_NOCLIENT;
        ent->solid = SOLID_NOT;
        gi.linkentity(ent);     // out of the trigger list, or it is taken again
    }
    return true;
}

static void CTF_DropFlagThink(edict_t *ent)
{
    int team = CTF_FlagTeam(ent->item);

    // back to its stand on its own; this frees the dropped one
    CTF_ResetFlag(team);
    gi.bprintf(PRINT_HIGH, "The %s flag has returned!\n", CTF_FlagName(team));
    BotEngine_FlagReturned(team);
}

static void CTF_DropFlagTouch(edict_t *ent, edict_t *other, cplane_t *plane, csurface_t *surf)
{
    // who dropped it cannot take it straight back
    if (other == ent->owner && level.framenum < ent->count)
        return;

    Touch_Item(ent, other, plane, surf);
}

static void CTF_DeadDropFlag(edict_t *self)
{
    edict_t *dropped = NULL;
    int     team;

    for (team = TEAM_RED; team <= TEAM_BLUE; team++) {
        int index = CTF_FlagIndex(team);

        if (!index || !self->client->pers.inventory[index])
            continue;

        dropped = Drop_Item(self, flag_items[team]);
        self->client->pers.inventory[index] = 0;
        gi.bprintf(PRINT_HIGH, "%s lost the %s flag!\n", self->client->pers.netname, CTF_FlagName(team));

        if (dropped) {
            dropped->think = CTF_DropFlagThink;
            dropped->nextthink = level.framenum + CTF_AUTO_FLAG_RETURN_TIMEOUT * BASE_FRAMERATE;
            dropped->touch = CTF_DropFlagTouch;
            dropped->count = level.framenum + 2 * BASE_FRAMERATE;
        }
    }
}

void CTFDrop_Flag(edict_t *ent, gitem_t *item)
{
    if (Q_rand() & 1)
        gi.cprintf(ent, PRINT_HIGH, "Only lusers drop flags.\n");
    else
        gi.cprintf(ent, PRINT_HIGH, "Winners don't drop flags.\n");
}

// for the bots: the flag on its stand (there or not), the one lying
// somewhere, and who is carrying it
edict_t *CTF_BaseFlag(int team)
{
    edict_t *ent = NULL;

    if ((team != TEAM_RED && team != TEAM_BLUE) || !flag_items[team])
        return NULL;
    while ((ent = G_Find(ent, FOFS(classname), flag_items[team]->classname)) != NULL)
        if (!(ent->spawnflags & DROPPED_ITEM))
            return ent;
    return NULL;
}

edict_t *CTF_DroppedFlag(int team)
{
    edict_t *ent = NULL;

    if ((team != TEAM_RED && team != TEAM_BLUE) || !flag_items[team])
        return NULL;
    while ((ent = G_Find(ent, FOFS(classname), flag_items[team]->classname)) != NULL)
        if ((ent->spawnflags & DROPPED_ITEM) && ent->solid == SOLID_TRIGGER)
            return ent;
    return NULL;
}

edict_t *CTF_Carrier(int team)
{
    int i, index = (team == TEAM_RED || team == TEAM_BLUE) ? CTF_FlagIndex(team) : 0;

    if (!index)
        return NULL;
    for (i = 1; i <= game.maxclients; i++) {
        edict_t *e = g_edicts + i;

        if (e->inuse && e->client && e->health > 0 && e->client->pers.inventory[index])
            return e;
    }
    return NULL;
}

// ==========================================================================
// FRAGS

void CTF_CheckHurtCarrier(edict_t *targ, edict_t *attacker)
{
    int index;

    if (!G_CTF() || !targ->client || !attacker || !attacker->client)
        return;

    index = CTF_FlagIndex(CTF_OtherTeam(targ->client->resp.team));
    if (index && targ->client->pers.inventory[index] &&
        targ->client->resp.team != attacker->client->resp.team)
        attacker->client->resp.ctf_lasthurtcarrier = level.time;
}

static bool CTF_CanSee(edict_t *a, edict_t *b)
{
    trace_t tr = gi.trace(a->s.origin, vec3_origin, vec3_origin, b->s.origin, a, MASK_SOLID);

    return tr.fraction == 1.0f || tr.ent == b;
}

/*
The bonuses for defending your flag and your carrier, for fragging theirs.
Not cumulative: you get one, in this order of importance.
*/
static void CTF_FragBonuses(edict_t *targ, edict_t *attacker)
{
    edict_t *flag, *carrier = NULL;
    vec3_t  v1, v2;
    int     i, otherteam, flag_index, enemy_flag_index;

    if (!targ->client || !attacker->client || targ == attacker)
        return;

    otherteam = CTF_OtherTeam(targ->client->resp.team);
    if (otherteam == TEAM_NONE)
        return;

    flag_index = CTF_FlagIndex(targ->client->resp.team);
    enemy_flag_index = CTF_FlagIndex(otherteam);

    // the carrier of the attacker's flag
    if (targ->client->pers.inventory[enemy_flag_index]) {
        attacker->client->resp.ctf_lastfraggedcarrier = level.time;
        attacker->client->resp.score += CTF_FRAG_CARRIER_BONUS;
        gi.cprintf(attacker, PRINT_MEDIUM, "BONUS: %d points for fragging enemy flag carrier.\n", CTF_FRAG_CARRIER_BONUS);

        // the other team's carrier-hurting times are spent
        for (i = 1; i <= game.maxclients; i++) {
            edict_t *e = g_edicts + i;

            if (e->inuse && e->client && e->client->resp.team == otherteam)
                e->client->resp.ctf_lasthurtcarrier = 0;
        }
        return;
    }

    if (targ->client->resp.ctf_lasthurtcarrier &&
        level.time - targ->client->resp.ctf_lasthurtcarrier < CTF_CARRIER_DANGER_PROTECT_TIMEOUT &&
        !attacker->client->pers.inventory[flag_index]) {
        // the target had hurt the attacker's carrier
        attacker->client->resp.score += CTF_CARRIER_DANGER_PROTECT_BONUS;
        gi.bprintf(PRINT_MEDIUM, "%s defends %s's flag carrier against an aggressive enemy\n",
                   attacker->client->pers.netname, Team_Name(attacker->client->resp.team));
        return;
    }

    // near the attacker's flag
    flag = CTF_BaseFlag(attacker->client->resp.team);
    if (flag && flag->solid != SOLID_NOT) {
        VectorSubtract(targ->s.origin, flag->s.origin, v1);
        VectorSubtract(attacker->s.origin, flag->s.origin, v2);

        if ((VectorLength(v1) < CTF_TARGET_PROTECT_RADIUS ||
             VectorLength(v2) < CTF_TARGET_PROTECT_RADIUS ||
             CTF_CanSee(flag, targ) || CTF_CanSee(flag, attacker)) &&
            attacker->client->resp.team != targ->client->resp.team) {
            attacker->client->resp.score += CTF_FLAG_DEFENSE_BONUS;
            gi.bprintf(PRINT_MEDIUM, "%s defends the %s base.\n",
                       attacker->client->pers.netname, Team_Name(attacker->client->resp.team));
            return;
        }
    }

    // near the attacker's carrier
    for (i = 1; i <= game.maxclients; i++) {
        edict_t *e = g_edicts + i;

        if (e->inuse && e->client && e->client->pers.inventory[CTF_FlagIndex(targ->client->resp.team)]) {
            carrier = e;
            break;
        }
    }
    if (carrier && carrier != attacker) {
        VectorSubtract(targ->s.origin, carrier->s.origin, v1);
        VectorSubtract(attacker->s.origin, carrier->s.origin, v2);

        if (VectorLength(v1) < CTF_ATTACKER_PROTECT_RADIUS ||
            VectorLength(v2) < CTF_ATTACKER_PROTECT_RADIUS ||
            CTF_CanSee(carrier, targ) || CTF_CanSee(carrier, attacker)) {
            attacker->client->resp.score += CTF_CARRIER_PROTECT_BONUS;
            gi.bprintf(PRINT_MEDIUM, "%s defends the %s's flag carrier.\n",
                       attacker->client->pers.netname, Team_Name(attacker->client->resp.team));
        }
    }
}

// ==========================================================================
// TECHS

static gitem_t *CTF_WhatTech(edict_t *ent)
{
    int i;

    for (i = 0; i < 4; i++)
        if (tech_items[i] && ent->client->pers.inventory[ITEM_INDEX(tech_items[i])])
            return tech_items[i];
    return NULL;
}

bool CTF_HasTech(const edict_t *ent)
{
    return G_CTF() && ent->client && CTF_WhatTech((edict_t *)ent) != NULL;
}

bool CTFPickup_Tech(edict_t *ent, edict_t *other)
{
    if (CTF_WhatTech(other)) {
        // one at a time
        if (level.time - other->client->ctf_lasttechmsg > 2) {
            gi.centerprintf(other, "You already have a TECH powerup.");
            other->client->ctf_lasttechmsg = level.time;
        }
        return false;
    }

    other->client->pers.inventory[ITEM_INDEX(ent->item)]++;
    other->client->ctf_regentime = level.time;
    return true;
}

static edict_t *CTF_FindTechSpawn(void)
{
    return SelectRandomDeathmatchSpawnPoint();
}

static void CTF_SpawnTech(gitem_t *item, edict_t *spot);

// an untouched tech moves somewhere else
static void CTF_TechThink(edict_t *tech)
{
    edict_t *spot = CTF_FindTechSpawn();

    if (spot) {
        CTF_SpawnTech(tech->item, spot);
        G_FreeEdict(tech);
    } else {
        tech->nextthink = level.framenum + CTF_TECH_TIMEOUT * BASE_FRAMERATE;
        tech->think = CTF_TechThink;
    }
}

static void CTF_SpawnTech(gitem_t *item, edict_t *spot)
{
    edict_t *ent = G_Spawn();
    vec3_t  forward, right, angles;

    ent->classname = item->classname;
    ent->item = item;
    ent->spawnflags = DROPPED_ITEM;
    ent->s.effects = item->world_model_flags;
    ent->s.renderfx = RF_GLOW;
    VectorSet(ent->mins, -15, -15, -15);
    VectorSet(ent->maxs, 15, 15, 15);
    gi.setmodel(ent, ent->item->world_model);
    ent->solid = SOLID_TRIGGER;
    ent->movetype = MOVETYPE_TOSS;
    ent->touch = Touch_Item;
    ent->owner = ent;

    VectorSet(angles, 0, (float)(Q_rand() % 360), 0);
    AngleVectors(angles, forward, right, NULL);
    VectorCopy(spot->s.origin, ent->s.origin);
    ent->s.origin[2] += 16;
    VectorScale(forward, 100, ent->velocity);
    ent->velocity[2] = 300;

    ent->nextthink = level.framenum + CTF_TECH_TIMEOUT * BASE_FRAMERATE;
    ent->think = CTF_TechThink;

    gi.linkentity(ent);
}

static void CTF_SpawnTechs(edict_t *ent)
{
    int i;

    for (i = 0; i < 4; i++) {
        edict_t *spot = CTF_FindTechSpawn();

        if (tech_items[i] && spot)
            CTF_SpawnTech(tech_items[i], spot);
    }
    if (ent)
        G_FreeEdict(ent);
}

void CTFDrop_Tech(edict_t *ent, gitem_t *item)
{
    edict_t *tech = Drop_Item(ent, item);

    tech->nextthink = level.framenum + CTF_TECH_TIMEOUT * BASE_FRAMERATE;
    tech->think = CTF_TechThink;
    ent->client->pers.inventory[ITEM_INDEX(item)] = 0;
}

static void CTF_DeadDropTech(edict_t *ent)
{
    int i;

    for (i = 0; i < 4; i++) {
        edict_t *dropped;

        if (!tech_items[i] || !ent->client->pers.inventory[ITEM_INDEX(tech_items[i])])
            continue;

        dropped = Drop_Item(ent, tech_items[i]);
        // hurl away
        dropped->velocity[0] = crandom() * 300;
        dropped->velocity[1] = crandom() * 300;
        dropped->nextthink = level.framenum + CTF_TECH_TIMEOUT * BASE_FRAMERATE;
        dropped->think = CTF_TechThink;
        dropped->owner = NULL;
        dropped->touch = Touch_Item;
        ent->client->pers.inventory[ITEM_INDEX(tech_items[i])] = 0;
    }
}

static bool CTF_Has(edict_t *ent, int tech)
{
    return G_CTF() && ent && ent->client && tech_items[tech] &&
           ent->client->pers.inventory[ITEM_INDEX(tech_items[tech])];
}

static void CTF_TechSound(edict_t *ent, const char *sound)
{
    if (ent->client->ctf_techsndtime < level.time) {
        ent->client->ctf_techsndtime = level.time + 1;
        gi.sound(ent, CHAN_ITEM, gi.soundindex(sound), ent->client->silencer_shots ? 0.2f : 1.0f, ATTN_NORM, 0);
    }
}

// the Disruptor Shield: half the damage
int CTF_ApplyResistance(edict_t *ent, int dmg)
{
    if (dmg && CTF_Has(ent, 0)) {
        CTF_TechSound(ent, "ctf/tech1.wav");
        return dmg / 2;
    }
    return dmg;
}

// the Power Amplifier: twice the damage
int CTF_ApplyStrength(edict_t *ent, int dmg)
{
    if (dmg && CTF_Has(ent, 1)) {
        CTF_TechSound(ent, ent->client->quad_framenum > level.framenum ? "ctf/tech2x.wav" : "ctf/tech2.wav");
        return dmg * 2;
    }
    return dmg;
}

// the Time Accel: the weapon thinks twice a frame (Think_Weapon)
bool CTF_HasHaste(edict_t *ent)
{
    if (!CTF_Has(ent, 2))
        return false;
    if (ent->client->buttons & BUTTON_ATTACK)
        CTF_TechSound(ent, "ctf/tech3.wav");
    return true;
}

// the AutoDoc: health and armour back up to 150, 5 at a time
static void CTF_ApplyRegeneration(edict_t *ent)
{
    gclient_t   *client = ent->client;
    bool        noise = false;
    int         index;

    if (!CTF_Has(ent, 3) || ent->health <= 0)
        return;

    if (client->ctf_regentime < level.time) {
        client->ctf_regentime = level.time;
        if (ent->health < 150) {
            ent->health = min(ent->health + 5, 150);
            client->ctf_regentime += 0.5f;
            noise = true;
        }
        index = ArmorIndex(ent);
        if (index && client->pers.inventory[index] < 150) {
            client->pers.inventory[index] = min(client->pers.inventory[index] + 5, 150);
            client->ctf_regentime += 0.5f;
            noise = true;
        }
    }
    if (noise)
        CTF_TechSound(ent, "ctf/tech4.wav");
}

// ==========================================================================
// GRAPPLE

void CTF_ResetGrapple(edict_t *hook)
{
    gclient_t *cl;

    if (!hook || !hook->owner || !hook->owner->client || !hook->owner->client->ctf_grapple)
        return;

    cl = hook->owner->client;
    if (!CTF_HookQ3(hook))
        gi.sound(hook->owner, CHAN_WEAPON, gi.soundindex("weapons/grapple/grreset.wav"), cl->silencer_shots ? 0.2f : 1.0f, ATTN_NORM, 0);
    cl->ctf_grapple = NULL;
    cl->ctf_grapplereleasetime = level.time;
    cl->ctf_grapplestate = CTF_GRAPPLE_STATE_FLY;
    G_FreeEdict(hook);
}

void CTF_PlayerResetGrapple(edict_t *ent)
{
    edict_t *hook;

    if (!ent->client || !(hook = ent->client->ctf_grapple))
        return;

    // the client outlives the level's edicts: after a map change this
    // points at whatever took the slot, so only a live hook of ours is freed
    if (hook->inuse && hook->owner == ent && hook->classname && !strcmp(hook->classname, "grapple"))
        CTF_ResetGrapple(hook);
    else
        ent->client->ctf_grapple = NULL;
    ent->client->ctf_grapplestate = CTF_GRAPPLE_STATE_FLY;
}

static void CTF_GrappleTouch(edict_t *self, edict_t *other, cplane_t *plane, csurface_t *surf)
{
    gclient_t *cl = self->owner->client;

    if (other == self->owner)
        return;
    if (cl->ctf_grapplestate != CTF_GRAPPLE_STATE_FLY)
        return;

    if (surf && (surf->flags & SURF_SKY)) {
        CTF_ResetGrapple(self);
        return;
    }

    VectorClear(self->velocity);
    PlayerNoise(self->owner, self->s.origin, PNOISE_IMPACT);

    if (CTF_HookQ3(self)) {
        // G_MissileImpact's hook: onto a player, else held where it hit; the
        // Lightning Gun's hit sound (CG_MissileHitWall's default)
        static const char *const hits[4] = {
            "weapons/lightning/lg_hit2.wav", "weapons/lightning/lg_hit2.wav",
            "weapons/lightning/lg_hit.wav", "weapons/lightning/lg_hit3.wav",
        };

        cl->ctf_grapplestate = CTF_GRAPPLE_STATE_PULL;
        self->enemy = (other->takedamage && other->client) ? other : NULL;
        self->solid = SOLID_NOT;
        self->movetype = MOVETYPE_NONE;
        self->nextthink = 0;    // no longer flying
        gi.linkentity(self);
        gi.sound(self, CHAN_WEAPON, Q3_SoundIndex(hits[Q_rand() & 3]), 1, ATTN_NORM, 0);
        return;
    }

    if (other->takedamage) {
        if (self->dmg)
            T_Damage(other, self, self->owner, self->velocity, self->s.origin,
                     plane ? plane->normal : vec3_origin, self->dmg, 1, 0, MOD_GRAPPLE);
        CTF_ResetGrapple(self);
        return;
    }

    cl->ctf_grapplestate = CTF_GRAPPLE_STATE_PULL;  // on the line
    self->enemy = other;
    self->solid = SOLID_NOT;

    gi.sound(self, CHAN_WEAPON, gi.soundindex("weapons/grapple/grhit.wav"), cl->silencer_shots ? 0.2f : 1.0f, ATTN_NORM, 0);
    self->s.sound = gi.soundindex("weapons/grapple/grpull.wav");

    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_SPARKS);
    gi.WritePosition(self->s.origin);
    gi.WriteDir(plane ? plane->normal : vec3_origin);
    gi.multicast(self->s.origin, MULTICAST_PVS);
}

// the line from the gun to the hook
static void CTF_GrappleDrawCable(edict_t *self)
{
    gclient_t   *cl = self->owner->client;
    vec3_t      forward, right, offset, start, end, dir;
    float       distance;

    AngleVectors(cl->v_angle, forward, right, NULL);
    VectorSet(offset, 16, cl->pers.hand == LEFT_HANDED ? -16 : cl->pers.hand == CENTER_HANDED ? 0 : 16,
              self->owner->viewheight - 8);
    G_ProjectSource(self->owner->s.origin, offset, forward, right, start);

    VectorSubtract(start, self->owner->s.origin, offset);
    VectorSubtract(start, self->s.origin, dir);
    distance = VectorLength(dir);
    // don't draw the cable when hanging close to the hook
    if (cl->ctf_grapplestate == CTF_GRAPPLE_STATE_HANG && distance < 64)
        return;

    VectorCopy(self->s.origin, end);

    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_GRAPPLE_CABLE);
    gi.WriteShort(self->owner - g_edicts);
    gi.WritePosition(self->owner->s.origin);
    gi.WritePosition(end);
    gi.WritePosition(offset);
    gi.multicast(self->s.origin, MULTICAST_PVS);
}

// Quake III's line: the client draws it from the owner to the hook, once a
// server frame (the pull runs on every command)
static void CTF_Q3GrappleLine(edict_t *self)
{
    if (self->count == level.framenum)
        return;
    self->count = level.framenum;

    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_Q3_GRAPPLE);
    gi.WriteShort(self->owner - g_edicts);
    gi.WriteShort(self - g_edicts);
    gi.WritePosition(self->s.origin);
    gi.multicast(self->s.origin, MULTICAST_PVS);
}

// Weapon_HookThink and PM_GrappleMove, on each command while the hook is out
static void CTF_Q3GrapplePull(edict_t *self)
{
    edict_t     *owner = self->owner;
    gclient_t   *cl = owner->client;
    gitem_t     *weapon = cl->pers.weapon;
    vec3_t      forward, v, vel;
    float       vlen;

    // only while the hook is the gun in hand (Q3W_Think lets go with the
    // trigger)
    if (!weapon || !(weapon->flags & IT_Q3) || weapon->tag != Q3W_GRAPPLE ||
        owner->health <= 0 || cl->resp.spectator) {
        CTF_ResetGrapple(self);
        return;
    }

    // on a player: with them, at the middle of their box
    if (self->enemy) {
        if (!self->enemy->inuse || self->enemy->solid == SOLID_NOT ||
            (self->enemy->takedamage && self->enemy->deadflag)) {
            CTF_ResetGrapple(self);
            return;
        }
        VectorAdd(self->enemy->mins, self->enemy->maxs, v);
        VectorMA(self->enemy->s.origin, 0.5f, v, self->s.origin);
        gi.linkentity(self);
    }

    CTF_Q3GrappleLine(self);

    if (cl->ctf_grapplestate == CTF_GRAPPLE_STATE_FLY)
        return;

    // toward 16 short of the hook along the view: 800 a second, slowing
    // over the last 100 units to hang there. The move after still has
    // gravity and air control ("we can wiggle a bit").
    AngleVectors(cl->v_angle, forward, NULL, NULL);
    VectorMA(self->s.origin, -16, forward, v);
    VectorSubtract(v, owner->s.origin, vel);
    vlen = VectorNormalize(vel);
    VectorScale(vel, vlen <= 100 ? 10 * vlen : Q3_GRAPPLE_PULL_SPEED, vel);
    VectorCopy(vel, owner->velocity);
    owner->groundentity = NULL;
}

// each think of the player while the hook is out: drag them along the line
void CTF_GrapplePull(edict_t *self)
{
    gclient_t   *cl = self->owner->client;
    vec3_t      hookdir, v;
    float       vlen;

    if (CTF_HookQ3(self)) {
        CTF_Q3GrapplePull(self);
        return;
    }

    if (cl->pers.weapon == grapple_item && !cl->newweapon &&
        cl->weaponstate != WEAPON_FIRING && cl->weaponstate != WEAPON_ACTIVATING) {
        CTF_ResetGrapple(self);
        return;
    }

    if (self->enemy) {
        if (self->enemy->solid == SOLID_NOT) {
            CTF_ResetGrapple(self);
            return;
        }
        if (self->enemy->solid == SOLID_BBOX) {
            VectorScale(self->enemy->size, 0.5f, v);
            VectorAdd(v, self->enemy->s.origin, v);
            VectorAdd(v, self->enemy->mins, self->s.origin);
            gi.linkentity(self);
        } else {
            VectorCopy(self->enemy->velocity, self->velocity);
        }
        if (self->enemy->takedamage && self->enemy->deadflag) {
            CTF_ResetGrapple(self);
            return;
        }
    }

    CTF_GrappleDrawCable(self);

    if (cl->ctf_grapplestate > CTF_GRAPPLE_STATE_FLY) {
        VectorCopy(self->owner->s.origin, v);
        v[2] += self->owner->viewheight;
        VectorSubtract(self->s.origin, v, hookdir);

        vlen = VectorLength(hookdir);

        if (cl->ctf_grapplestate == CTF_GRAPPLE_STATE_PULL && vlen < 64) {
            cl->ctf_grapplestate = CTF_GRAPPLE_STATE_HANG;
            self->s.sound = gi.soundindex("weapons/grapple/grhang.wav");
        }

        VectorNormalize(hookdir);
        VectorScale(hookdir, CTF_GRAPPLE_PULL_SPEED, hookdir);
        VectorCopy(hookdir, self->owner->velocity);
        SV_AddGravity(self->owner);
    }
}

static void CTF_GrappleDie(edict_t *self, edict_t *inflictor, edict_t *attacker, int damage, vec3_t point)
{
    CTF_ResetGrapple(self);
}

// a Quake III hook that has flown its time without hitting anything
static void CTF_GrappleExpire(edict_t *self)
{
    CTF_ResetGrapple(self);
}

static bool CTF_FireGrapple(edict_t *self, vec3_t start, vec3_t dir, int damage, int speed, bool q3)
{
    edict_t *grapple;
    trace_t tr;

    VectorNormalize(dir);

    grapple = G_Spawn();
    VectorCopy(start, grapple->s.origin);
    VectorCopy(start, grapple->s.old_origin);
    vectoangles(dir, grapple->s.angles);
    VectorScale(dir, speed, grapple->velocity);
    grapple->movetype = MOVETYPE_FLYMISSILE;
    grapple->clipmask = MASK_SHOT;
    grapple->solid = SOLID_BBOX;
    VectorClear(grapple->mins);
    VectorClear(grapple->maxs);
    grapple->s.modelindex = gi.modelindex(q3 ? "models/ammo/rocket/rocket.md3" : "models/weapons/grapple/hook/tris.md2");
    if (q3) {
        grapple->spawnflags = GRAPPLE_Q3;
        grapple->count = -1;    // its line not sent this frame (CTF_Q3GrappleLine)
        grapple->think = CTF_GrappleExpire;
        grapple->nextthink = level.framenum + Q3_GRAPPLE_FLY_TIME * BASE_FRAMERATE;
    }
    grapple->owner = self;
    grapple->touch = CTF_GrappleTouch;
    grapple->dmg = damage;
    grapple->die = CTF_GrappleDie;
    grapple->classname = "grapple";
    self->client->ctf_grapple = grapple;
    self->client->ctf_grapplestate = CTF_GRAPPLE_STATE_FLY;
    gi.linkentity(grapple);

    tr = gi.trace(self->s.origin, NULL, NULL, grapple->s.origin, grapple, MASK_SHOT);
    if (tr.fraction < 1.0f) {
        VectorMA(tr.endpos, 1, tr.plane.normal, grapple->s.origin);
        grapple->touch(grapple, tr.ent, &tr.plane, tr.surface);
        return false;
    }

    if (!q3)
        grapple->s.sound = gi.soundindex("weapons/grapple/grfly.wav");
    return true;
}

// g_q3weap.c's grappling hook: Quake III's fire_grapple
bool CTF_FireQ3Grapple(edict_t *ent, vec3_t start, vec3_t dir)
{
    vec3_t d;

    VectorCopy(dir, d);
    return CTF_FireGrapple(ent, start, d, 0, Q3_GRAPPLE_SPEED, true);
}

static void CTF_GrappleFire(edict_t *ent)
{
    gclient_t   *cl = ent->client;
    vec3_t      forward, right, offset, start;

    // it's already out. The held trigger keeps the gun on its fire frame,
    // so this runs every frame the hook is out - in flight too, and twice a
    // frame with the Time Accel - and must not throw a second one
    if (cl->ctf_grapple || cl->ctf_grapplestate > CTF_GRAPPLE_STATE_FLY)
        return;

    AngleVectors(cl->v_angle, forward, right, NULL);
    VectorSet(offset, 24, cl->pers.hand == LEFT_HANDED ? -8 : cl->pers.hand == CENTER_HANDED ? 0 : 8,
              ent->viewheight - 8 + 2);
    G_ProjectSource(ent->s.origin, offset, forward, right, start);

    VectorScale(cl->kick_origin, 0, cl->kick_origin);
    cl->kick_angles[0] = -1;

    if (CTF_FireGrapple(ent, start, forward, CTF_GRAPPLE_DAMAGE, CTF_GRAPPLE_SPEED, false))
        gi.sound(ent, CHAN_WEAPON, gi.soundindex("weapons/grapple/grfire.wav"), cl->silencer_shots ? 0.2f : 1.0f, ATTN_NORM, 0);

    PlayerNoise(ent, start, PNOISE_WEAPON);
}

void CTFWeapon_Grapple(edict_t *ent)
{
    static int  pause_frames[] = { 10, 18, 27, 0 };
    static int  fire_frames[] = { 6, 0 };
    gclient_t   *cl = ent->client;
    int         prevstate;

    // while the attack button is down, stay in the firing frame
    if ((cl->buttons & BUTTON_ATTACK) && cl->weaponstate == WEAPON_FIRING && cl->ctf_grapple)
        cl->ps.gunframe = 6;

    if (!(cl->buttons & BUTTON_ATTACK) && cl->ctf_grapple) {
        CTF_ResetGrapple(cl->ctf_grapple);
        if (cl->weaponstate == WEAPON_FIRING)
            cl->weaponstate = WEAPON_READY;
    }

    if (cl->newweapon && cl->ctf_grapplestate > CTF_GRAPPLE_STATE_FLY &&
        cl->weaponstate == WEAPON_FIRING) {
        // wants another weapon while on the line
        cl->weaponstate = WEAPON_DROPPING;
        cl->ps.gunframe = 32;
    }

    prevstate = cl->weaponstate;
    Weapon_Generic(ent, 5, 10, 31, 36, pause_frames, fire_frames, CTF_GrappleFire);

    if ((cl->buttons & BUTTON_ATTACK) && cl->weaponstate == WEAPON_FIRING && cl->ctf_grapple)
        cl->ps.gunframe = 6;

    // just switched back to the hook while on the line: straight to firing
    if (prevstate == WEAPON_ACTIVATING && cl->weaponstate == WEAPON_READY &&
        cl->ctf_grapplestate > CTF_GRAPPLE_STATE_FLY) {
        if (!(cl->buttons & BUTTON_ATTACK))
            cl->ps.gunframe = 6;
        else
            cl->ps.gunframe = 5;
        cl->weaponstate = WEAPON_FIRING;
    }
}

// InitClientPersistant: everyone has the hook in CTF
void CTF_GivePersistant(gclient_t *client)
{
    if (G_CTF() && grapple_item)
        client->pers.inventory[ITEM_INDEX(grapple_item)] = 1;
}

// ==========================================================================
// PLAYERS

// ClientThink, before the move: the hook pulls
void CTF_ClientThink(edict_t *ent)
{
    if (ent->client->ctf_grapple)
        CTF_GrapplePull(ent->client->ctf_grapple);
}

// ClientEndServerFrame: the AutoDoc
void CTF_ClientFrame(edict_t *ent)
{
    if (G_CTF())
        CTF_ApplyRegeneration(ent);
}

// player_die, after the obituary, before the inventory goes
void CTF_PlayerDied(edict_t *self, edict_t *attacker)
{
    if (!G_CTF())
        return;

    if (attacker)
        CTF_FragBonuses(self, attacker);
    CTF_DeadDropFlag(self);
    CTF_DeadDropTech(self);
    CTF_PlayerResetGrapple(self);
    self->s.modelindex3 = 0;    // the flag off the corpse's back
}

// ClientDisconnect, and a change of team: what they hold goes back in play
void CTF_PlayerLeft(edict_t *ent)
{
    if (!G_CTF() || !ent->client)
        return;

    CTF_DeadDropFlag(ent);
    CTF_DeadDropTech(ent);
    CTF_PlayerResetGrapple(ent);
}

// G_SetClientEffects: the flag's glow, and the flag on the carrier's back
void CTF_ClientEffects(edict_t *player)
{
    int red, blue;

    if (!G_CTF())
        return;

    red = CTF_FlagIndex(TEAM_RED);
    blue = CTF_FlagIndex(TEAM_BLUE);

    player->s.effects &= ~(EF_FLAG1 | EF_FLAG2);
    if (player->health > 0) {
        if (red && player->client->pers.inventory[red])
            player->s.effects |= EF_FLAG1;
        if (blue && player->client->pers.inventory[blue])
            player->s.effects |= EF_FLAG2;
    }

    if (red && player->client->pers.inventory[red])
        player->s.modelindex3 = model_flag1;
    else if (blue && player->client->pers.inventory[blue])
        player->s.modelindex3 = model_flag2;
    else
        player->s.modelindex3 = 0;
}

static int CTF_FlagPic(int team, int home, int dropped, int taken)
{
    edict_t *e = CTF_BaseFlag(team);

    if (!e)
        return home;
    if (e->solid == SOLID_NOT)
        return CTF_Carrier(team) ? taken : dropped;
    return home;
}

// G_SetStats: the CTF part of the HUD
void CTF_SetStats(edict_t *ent)
{
    gclient_t   *cl = ent->client;
    bool        blink = ((int)(level.time * 10) % 10) < 5;
    gitem_t     *tech;
    int         p1, p2;

    if (!G_CTF())
        return;

    p1 = CTF_FlagPic(TEAM_RED, pic_ctf1, pic_ctf1d, pic_ctf1t);
    p2 = CTF_FlagPic(TEAM_BLUE, pic_ctf2, pic_ctf2d, pic_ctf2t);

    // the team that just scored blinks for five seconds
    if (ctfgame.last_capture_time && level.time - ctfgame.last_capture_time < 5 && !blink) {
        if (ctfgame.last_capture_team == TEAM_RED)
            p1 = 0;
        else
            p2 = 0;
    }

    cl->ps.stats[STAT_CTF_TEAM1_PIC] = p1;
    cl->ps.stats[STAT_CTF_TEAM2_PIC] = p2;
    cl->ps.stats[STAT_CTF_TEAM1_CAPS] = ctfgame.caps[TEAM_RED];
    cl->ps.stats[STAT_CTF_TEAM2_CAPS] = ctfgame.caps[TEAM_BLUE];

    // the flag you are carrying, blinking
    cl->ps.stats[STAT_CTF_FLAG_PIC] = 0;
    if (blink) {
        if (cl->resp.team == TEAM_RED && cl->pers.inventory[CTF_FlagIndex(TEAM_BLUE)])
            cl->ps.stats[STAT_CTF_FLAG_PIC] = pic_ctf2;
        else if (cl->resp.team == TEAM_BLUE && cl->pers.inventory[CTF_FlagIndex(TEAM_RED)])
            cl->ps.stats[STAT_CTF_FLAG_PIC] = pic_ctf1;
    }

    cl->ps.stats[STAT_CTF_JOINED_TEAM1_PIC] = cl->resp.team == TEAM_RED ? pic_ctfj : 0;
    cl->ps.stats[STAT_CTF_JOINED_TEAM2_PIC] = cl->resp.team == TEAM_BLUE ? pic_ctfj : 0;

    tech = CTF_WhatTech(ent);
    cl->ps.stats[STAT_CTF_TECH] = tech ? gi.imageindex(tech->icon) : 0;
}

// ==========================================================================
// THE MATCH

// SpawnEntities: a new level, nil-nil, and the techs out in two seconds
void CTF_LevelStart(void)
{
    edict_t *ent;

    memset(&ctfgame, 0, sizeof(ctfgame));
    if (!G_CTF())
        return;

    ent = G_Spawn();
    ent->classname = "ctf_tech_spawner";
    ent->think = CTF_SpawnTechs;
    ent->nextthink = level.framenum + 2 * BASE_FRAMERATE;
}

// CheckDMRules: the capture limit
bool CTF_CheckRules(void)
{
    if (!G_CTF() || !capturelimit->value)
        return false;

    if (ctfgame.caps[TEAM_RED] >= capturelimit->value || ctfgame.caps[TEAM_BLUE] >= capturelimit->value) {
        gi.bprintf(PRINT_HIGH, "Capturelimit hit.\n");
        return true;
    }
    return false;
}
