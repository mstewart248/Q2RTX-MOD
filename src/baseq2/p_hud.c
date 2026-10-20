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
#include "g_local.h"


/*
===============
G_RereleaseHud

[rerelease] Whether this player has cl_rerelease_hud on: the hit marker,
directional damage indicators, held key icons, the low ammo warning and the
soonest-to-run-out powerup timer. The client keeps it in its userinfo.
===============
*/
bool G_RereleaseHud(edict_t *ent)
{
    return M_RereleaseGame() && ent->client &&
           atoi(Info_ValueForKey(ent->client->pers.userinfo, "cl_rerelease_hud")) > 0;
}


/*
======================================================================

END OF UNIT

[rerelease] The table of levels, kills, secrets and times shown over the
intermission that ends a unit. game.level_entries collects a row per level
across the unit: G_SetLevelEntry claims one when a player first walks into a
level, G_UpdateLevelEntry copies the level's counts into it whenever we leave
or save, and G_RunFrame adds up the time.

======================================================================
*/

void G_UpdateLevelEntry(void)
{
    if (!level.entry)
        return;

    level.entry->found_secrets = level.found_secrets;
    level.entry->total_secrets = level.total_secrets;
    level.entry->killed_monsters = level.killed_monsters;
    level.entry->total_monsters = level.total_monsters;
}

static level_entry_t *G_FindLevelEntry(const char *map, size_t len)
{
    int i;

    for (i = 0; i < MAX_LEVELS_PER_UNIT; i++) {
        level_entry_t *entry = &game.level_entries[i];

        if (!entry->map_name[0] ||
            (strlen(entry->map_name) == len && !strncmp(entry->map_name, map, len)))
            return entry;
    }

    gi.dprintf("WARNING: more than %d maps in unit, can't track the rest\n", MAX_LEVELS_PER_UNIT);
    return NULL;
}

// Called from ClientBegin, so the clock only starts once a player is in.
void G_SetLevelEntry(void)
{
    level_entry_t   *entry;
    edict_t         *changelevel = NULL;
    int             i, highest_order = 0;

    if (deathmatch->value || !M_RereleaseGame())
        return;

    // a hub is no level of any unit; the next map picks up as the start
    if (level.hub_map)
        return;

    for (i = 0; i < MAX_LEVELS_PER_UNIT; i++)
        highest_order = max(highest_order, game.level_entries[i].visit_order);

    entry = G_FindLevelEntry(level.mapname, strlen(level.mapname));
    if (!entry)
        return;

    level.entry = entry;
    Q_strlcpy(entry->map_name, level.mapname, sizeof(entry->map_name));

    // first visit: it goes after everything seen so far
    if (!entry->pretty_name[0]) {
        Q_strlcpy(entry->pretty_name, level.level_name, sizeof(entry->pretty_name));
        entry->visit_order = highest_order + 1;
    }

    // Every level this one leads to gets a row now, so a secret level that is
    // never found still shows as "???". A next unit's map does not count, nor
    // does a cinematic or end picture.
    while ((changelevel = G_Find(changelevel, FOFS(classname), "target_changelevel"))) {
        const char  *map = changelevel->map, *p;
        size_t      len;

        if (!map || !*map || strchr(map, '*'))
            continue;

        p = strchr(map, '+');
        if (p)
            map = p + 1;

        if (strstr(map, ".cin") || strstr(map, ".pcx"))
            continue;

        p = strchr(map, '$');
        len = p ? (size_t)(p - map) : strlen(map);
        if (!len || len >= MAX_QPATH)
            continue;

        entry = G_FindLevelEntry(map, len);
        if (!entry)
            return;

        if (!entry->map_name[0])
            Q_strlcpy(entry->map_name, map, len + 1);
    }
}

static int G_LevelEntryOrder(const level_entry_t *e)
{
    if (e->visit_order)
        return e->visit_order;
    return e->map_name[0] ? MAX_LEVELS_PER_UNIT + 1 : MAX_LEVELS_PER_UNIT + 2;
}

static int G_LevelEntryCompare(const void *a, const void *b)
{
    return G_LevelEntryOrder(a) - G_LevelEntryOrder(b);
}

static void G_EndOfUnitRow(char *layout, size_t size, const level_entry_t *e)
{
    int ms = e->time * 1000 / BASE_FRAMERATE;

    // a level we never got to
    if (!e->pretty_name[0]) {
        Q_strlcat(layout, "table_row 1 ??? ", size);
        return;
    }

    Q_strlcat(layout, va("table_row 4 \"%.23s\" %d/%d %d/%d %02d:%02d:%03d ",
                         e->pretty_name, e->killed_monsters, e->total_monsters,
                         e->found_secrets, e->total_secrets,
                         ms / 60000, (ms / 1000) % 60, ms % 1000), size);
}

/*
===============
G_EndOfUnitMessage

Sends everyone the end of unit table, drawn by the client's start_table /
table_row / draw_table layout commands. The rerelease holds the "press any
button" line back with a server frame test; this tree's frame numbers are not
the client's, so the table is simply sent again with the line once the
intermission can be left (see G_RunFrame).
===============
*/
void G_EndOfUnitMessage(bool press_button)
{
    level_entry_t   entries[MAX_LEVELS_PER_UNIT], totals;
    char            layout[MAX_STRING_CHARS];
    int             i, num_rows = 0;

    G_UpdateLevelEntry();

    memcpy(entries, game.level_entries, sizeof(entries));
    qsort(entries, MAX_LEVELS_PER_UNIT, sizeof(entries[0]), G_LevelEntryCompare);

    Q_strlcpy(layout, "start_table 4 Level Kills Secrets Time ", sizeof(layout));

    memset(&totals, 0, sizeof(totals));
    for (i = 0; i < MAX_LEVELS_PER_UNIT && entries[i].map_name[0]; i++) {
        G_EndOfUnitRow(layout, sizeof(layout), &entries[i]);

        totals.found_secrets += entries[i].found_secrets;
        totals.total_secrets += entries[i].total_secrets;
        totals.killed_monsters += entries[i].killed_monsters;
        totals.total_monsters += entries[i].total_monsters;
        totals.time += entries[i].time;

        if (entries[i].visit_order)
            num_rows++;
    }

    // a blank row, then the totals under a nameless first column
    if (num_rows > 1) {
        Q_strlcat(layout, "table_row 0 ", sizeof(layout));
        Q_strlcpy(totals.pretty_name, " ", sizeof(totals.pretty_name));
        G_EndOfUnitRow(layout, sizeof(layout), &totals);
    }

    Q_strlcat(layout, "xv 160 yt 16 draw_table ", sizeof(layout));

    if (press_button)
        Q_strlcat(layout, "xv 0 yb -48 cstring2 \"Press any button to continue.\" ", sizeof(layout));

    gi.WriteByte(svc_layout);
    gi.WriteString(layout);
    gi.multicast(vec3_origin, MULTICAST_ALL_R);

    level.eou_screen = true;
    for (i = 0; i < game.maxclients; i++) {
        if (g_edicts[i + 1].inuse) {
            game.clients[i].showeou = true;
            game.clients[i].showhelp = false;
            game.clients[i].showscores = false;
        }
    }
}


/*
======================================================================

INTERMISSION

======================================================================
*/

void MoveClientToIntermission(edict_t *ent)
{
    // [rerelease] coop shows the end of unit table in place of the scores
    bool scores = deathmatch->value || (coop->value && !level.eou_screen);

    if (scores)
        ent->client->showscores = true;
    VectorCopy(level.intermission_origin, ent->s.origin);
    ent->client->ps.pmove.origin[0] = COORD2SHORT(level.intermission_origin[0]);
    ent->client->ps.pmove.origin[1] = COORD2SHORT(level.intermission_origin[1]);
    ent->client->ps.pmove.origin[2] = COORD2SHORT(level.intermission_origin[2]);
    VectorCopy(level.intermission_angle, ent->client->ps.viewangles);
    ent->client->ps.pmove.pm_type = PM_FREEZE;
    ent->client->ps.gunindex = 0;
    ent->client->ps.blend[3] = 0;
    ent->client->ps.rdflags &= ~RDF_UNDERWATER;

    // clean up powerup info
    ent->client->quad_framenum = 0;
    ent->client->double_framenum = 0;
    ent->client->quadfire_framenum = 0;
    ent->client->invincible_framenum = 0;
    ent->client->breather_framenum = 0;
    ent->client->enviro_framenum = 0;
    ent->client->grenade_blew_up = false;
    ent->client->grenade_framenum = 0;

    ent->viewheight = 0;
    ent->s.modelindex = 0;
    ent->s.modelindex2 = 0;
    ent->s.modelindex3 = 0;
    ent->s.modelindex4 = 0;
    ent->s.effects = 0;
    ent->s.sound = 0;
    ent->solid = SOLID_NOT;

    // add the layout

    if (scores) {
        DeathmatchScoreboardMessage(ent, NULL);
        gi.unicast(ent, true);
    } else if (level.eou_screen && !ent->client->showeou) {
        // someone who came in after the table went out
        G_EndOfUnitMessage(level.framenum > level.intermission_framenum + 5 * BASE_FRAMERATE);
    }

}

void BeginIntermission(edict_t *targ)
{
    int     i, n;
    edict_t *ent, *client;

    if (level.intermission_framenum)
        return;     // already activated

    game.autosaved = false;

    // respawn any dead clients
    for (i = 0 ; i < maxclients->value ; i++) {
        client = g_edicts + 1 + i;
        if (!client->inuse)
            continue;
        if (client->health <= 0)
            respawn(client);
    }

    level.intermission_framenum = level.framenum;
    level.changemap = targ->map;
    // [rerelease] CLEAR_INVENTORY: strip the players back to defaults on the way out
    level.intermission_clear = (targ->spawnflags & SPAWNFLAG_CHANGELEVEL_CLEAR_INVENTORY) != 0;
    level.intermission_eou = false;

    // [rerelease] this level's row of the end of unit screen
    G_UpdateLevelEntry();

    if (strstr(level.changemap, "*")) {
        if (coop->value) {
            for (i = 0 ; i < maxclients->value ; i++) {
                client = g_edicts + 1 + i;
                if (!client->inuse)
                    continue;
                // strip players of all keys between units
                for (n = 0; n < game.num_items; n++) {
                    if (itemlist[n].flags & IT_KEY)
                        client->client->pers.inventory[n] = 0;
                }
            }
        }

        // [rerelease] the unit is over: show its levels, unless this exit
        // says not to - and if it says to leave at once, do
        if (M_RereleaseGame() && !deathmatch->value) {
            level.intermission_eou = true;
            if (!(targ->spawnflags & SPAWNFLAG_CHANGELEVEL_NO_END_OF_UNIT)) {
                G_EndOfUnitMessage(false);
            } else if (targ->spawnflags & SPAWNFLAG_CHANGELEVEL_IMMEDIATE_LEAVE) {
                level.exitintermission = 1;
                return;
            }
        }
    } else {
        if (!deathmatch->value) {
            level.exitintermission = 1;     // go immediately to the next level
            return;
        }
    }

    level.exitintermission = 0;

    // [rerelease] a target_camera has already aimed the intermission view down
    // its own path. Snapping it to an info_player_intermission here would throw
    // the shot away on the frame the camera fires its killtarget.
    if (level.level_intermission_set)
        return;

    // find an intermission spot
    ent = G_Find(NULL, FOFS(classname), "info_player_intermission");
    if (!ent) {
        // the map creator forgot to put in an intermission point...
        ent = G_Find(NULL, FOFS(classname), "info_player_start");
        if (!ent)
            ent = G_Find(NULL, FOFS(classname), "info_player_deathmatch");
    } else {
        // chose one of four spots
        i = Q_rand() & 3;
        while (i--) {
            ent = G_Find(ent, FOFS(classname), "info_player_intermission");
            if (!ent)   // wrap around the list
                ent = G_Find(ent, FOFS(classname), "info_player_intermission");
        }
    }

    if (ent) {
        VectorCopy(ent->s.origin, level.intermission_origin);
        VectorCopy(ent->s.angles, level.intermission_angle);
    }

    // move all clients to the intermission point
    for (i = 0 ; i < maxclients->value ; i++) {
        client = g_edicts + 1 + i;
        if (!client->inuse)
            continue;
        MoveClientToIntermission(client);
    }
}


/*
==================
DeathmatchScoreboardMessage

==================
*/
void DeathmatchScoreboardMessage(edict_t *ent, edict_t *killer)
{
    char    entry[1024];
    char    string[1400];
    int     stringlength;
    int     i, j, k;
    int     sorted[MAX_CLIENTS];
    int     sortedscores[MAX_CLIENTS];
    int     score, total;
    int     x, y;
    gclient_t   *cl;
    edict_t     *cl_ent;
    char    *tag;

    // sort the clients by score
    total = 0;
    for (i = 0 ; i < game.maxclients ; i++) {
        cl_ent = g_edicts + 1 + i;
        if (!cl_ent->inuse || game.clients[i].resp.spectator)
            continue;
        score = game.clients[i].resp.score;
        for (j = 0 ; j < total ; j++) {
            if (score > sortedscores[j])
                break;
        }
        for (k = total ; k > j ; k--) {
            sorted[k] = sorted[k - 1];
            sortedscores[k] = sortedscores[k - 1];
        }
        sorted[j] = i;
        sortedscores[j] = score;
        total++;
    }

    // print level name and exit rules
    string[0] = 0;

    stringlength = strlen(string);

    // add the clients in sorted order
    if (total > 12)
        total = 12;

    for (i = 0 ; i < total ; i++) {
        cl = &game.clients[sorted[i]];
        cl_ent = g_edicts + 1 + sorted[i];

        x = (i >= 6) ? 160 : 0;
        y = 32 + 32 * (i % 6);

        // add a dogtag
        if (cl_ent == ent)
            tag = "tag1";
        else if (cl_ent == killer)
            tag = "tag2";
        else
            tag = NULL;
        if (tag) {
            Q_snprintf(entry, sizeof(entry),
                       "xv %i yv %i picn %s ", x + 32, y, tag);
            j = strlen(entry);
            if (stringlength + j > 1024)
                break;
            strcpy(string + stringlength, entry);
            stringlength += j;
        }

        // send the layout
        Q_snprintf(entry, sizeof(entry),
                   "client %i %i %i %i %i %i ",
                   x, y, sorted[i], cl->resp.score, cl->ping, (level.framenum - cl->resp.enterframe) / 600);
        j = strlen(entry);
        if (stringlength + j > 1024)
            break;
        strcpy(string + stringlength, entry);
        stringlength += j;
    }

    gi.WriteByte(svc_layout);
    gi.WriteString(string);
}


/*
==================
DeathmatchScoreboard

Draw instead of help message.
Note that it isn't that hard to overflow the 1400 byte message limit!
==================
*/
void DeathmatchScoreboard(edict_t *ent)
{
    DeathmatchScoreboardMessage(ent, ent->enemy);
    gi.unicast(ent, true);
}


/*
==================
Cmd_Score_f

Display the scoreboard
==================
*/
void Cmd_Score_f(edict_t *ent)
{
    ent->client->showinventory = false;
    ent->client->showhelp = false;

    if (!deathmatch->value && !coop->value)
        return;

    if (ent->client->showscores) {
        ent->client->showscores = false;
        return;
    }

    ent->client->showscores = true;
    DeathmatchScoreboard(ent);
}


/*
==================
HelpComputer

Draw help computer.
==================
*/
void HelpComputer(edict_t *ent)
{
    char    string[1024];
    char    *sk;

    if (skill->value == 0)
        sk = "easy";
    else if (skill->value == 1)
        sk = "medium";
    else if (skill->value == 2)
        sk = "hard";
    else
        sk = "hard+";

    // send the layout
    Q_snprintf(string, sizeof(string),
               "xv 32 yv 8 picn help "         // background
               "xv 202 yv 12 string2 \"%s\" "      // skill
               "xv 0 yv 24 cstring2 \"%s\" "       // level name
               "xv 0 yv 54 cstring2 \"%s\" "       // help 1
               "xv 0 yv 110 cstring2 \"%s\" "      // help 2
               "xv 50 yv 164 string2 \" kills     goals    secrets\" "
               "xv 50 yv 172 string2 \"%3i/%3i     %i/%i       %i/%i\" ",
               sk,
               level.level_name,
               game.helpmessage1,
               game.helpmessage2,
               level.killed_monsters, level.total_monsters,
               level.found_goals, level.total_goals,
               level.found_secrets, level.total_secrets);

    gi.WriteByte(svc_layout);
    gi.WriteString(string);
    gi.unicast(ent, true);
}


/*
==================
Cmd_Help_f

Display the current help message
==================
*/
void Cmd_Help_f(edict_t *ent)
{
    // this is for backwards compatability
    if (deathmatch->value) {
        Cmd_Score_f(ent);
        return;
    }

    // [rerelease] the help computer would replace the end of unit table and,
    // closed again, leave its own page up in the table's place
    if (level.eou_screen)
        return;

    ent->client->showinventory = false;
    ent->client->showscores = false;

    if (ent->client->showhelp && (ent->client->pers.game_helpchanged == game.helpchanged)) {
        ent->client->showhelp = false;
        return;
    }

    ent->client->showhelp = true;
    ent->client->pers.helpchanged = 0;
    HelpComputer(ent);
}


//=======================================================================

/*
===============
G_SetPowerupTimer

[rerelease] cl_rerelease_hud: the powerup timer shows whichever powerup runs
out first, rather than the first held in a fixed order, and covers the ones
that order left out - IR goggles, the cloak, the silencer's shots and a
power sphere, which outranks them all.
===============
*/
static void G_SetPowerupTimer(edict_t *ent)
{
    static const struct {
        const char  *icon;
        size_t      ofs;
    } timed[] = {
        { "p_quad",             CLOFS(quad_framenum) },
        { "p_quadfire",         CLOFS(quadfire_framenum) },
        { "p_double",           CLOFS(double_framenum) },
        { "p_invulnerability",  CLOFS(invincible_framenum) },
        { "p_cloaker",          CLOFS(invisible_framenum) },
        { "p_envirosuit",       CLOFS(enviro_framenum) },
        { "p_rebreather",       CLOFS(breather_framenum) },
        { "p_ir",               CLOFS(ir_framenum) },
    };
    gclient_t   *client = ent->client;
    edict_t     *sphere = client->owned_sphere;
    const char  *icon = NULL;
    int         i, best = 0, value = 0;

    if (sphere && sphere->inuse) {
        if (sphere->spawnflags & SPHERE_DEFENDER)
            icon = "p_defender";
        else if (sphere->spawnflags & SPHERE_HUNTER)
            icon = "p_hunter";
        else if (sphere->spawnflags & SPHERE_VENGEANCE)
            icon = "p_vengeance";
        else
            icon = "i_fixme";
        value = (sphere->wait - level.framenum + BASE_FRAMERATE - 1) / BASE_FRAMERATE;
    } else {
        for (i = 0; i < q_countof(timed); i++) {
            int end = *(int *)((byte *)client + timed[i].ofs);

            if (end > level.framenum && (!icon || end < best)) {
                icon = timed[i].icon;
                best = end;
            }
        }

        if (icon) {
            value = (best - level.framenum + BASE_FRAMERATE - 1) / BASE_FRAMERATE;
        } else if (client->silencer_shots) {
            icon = "p_silencer";
            value = client->silencer_shots;
        }
    }

    client->ps.stats[STAT_TIMER_ICON] = icon ? gi.imageindex(icon) : 0;
    client->ps.stats[STAT_TIMER] = icon ? value : 0;
}

/*
===============
G_SetStats
===============
*/
/*
===============
G_PlayerNotifyGoal

The rerelease announces an objective on screen the moment target_help changes
it, rather than waiting for the player to open the help computer. The banner
text comes from the localization table ("Primary Objective:\n{}"), so it reads
correctly once loc_english.txt is present and falls back to a plain prefix if
it is not.
===============
*/
void G_PlayerNotifyGoal(edict_t *player)
{
    char    buf[1024];

    if (!player->client)
        return;

    /* Several maps fire the primary and secondary objectives from one
       targetname in the same frame, and each centerprint replaces the last.
       Announcing the secondary first leaves the primary on screen, which is
       what the rerelease shows. */
    if (player->client->pers.game_help2changed != game.help2changed) {
        player->client->pers.game_help2changed = game.help2changed;
        player->client->pers.helpchanged = 1;

        if (*game.helpmessage2) {
            L10N_Format(buf, sizeof(buf), "$g_secondary_mission_objective",
                        "Secondary Objective:\n%s", game.helpmessage2);
            gi.centerprintf(player, "%s", buf);
            gi.sound(player, CHAN_AUTO, gi.soundindex("misc/talk.wav"), 1, ATTN_NONE, 0);
        }
    }

    if (player->client->pers.game_help1changed != game.help1changed) {
        player->client->pers.game_help1changed = game.help1changed;
        player->client->pers.helpchanged = 1;

        if (*game.helpmessage1) {
            L10N_Format(buf, sizeof(buf), "$g_primary_mission_objective",
                        "Primary Objective:\n%s", game.helpmessage1);
            gi.centerprintf(player, "%s", buf);
            gi.sound(player, CHAN_AUTO, gi.soundindex("misc/talk.wav"), 1, ATTN_NONE, 0);
        }
    }
}

void G_SetStats(edict_t *ent)
{
    gitem_t     *item;
    int         index, cells;
    int         power_armor_type;

    //
    // health
    //
    ent->client->ps.stats[STAT_HEALTH_ICON] = level.pic_health;
    ent->client->ps.stats[STAT_HEALTH] = ent->health;

    //
    // ammo
    //
    if (!ent->client->ammo_index /* || !ent->client->pers.inventory[ent->client->ammo_index] */) {
        ent->client->ps.stats[STAT_AMMO_ICON] = 0;
        ent->client->ps.stats[STAT_AMMO] = 0;
    } else {
        item = &itemlist[ent->client->ammo_index];
        ent->client->ps.stats[STAT_AMMO_ICON] = gi.imageindex(item->icon);
        ent->client->ps.stats[STAT_AMMO] = ent->client->pers.inventory[ent->client->ammo_index];
    }

    //
    // armor
    //
    power_armor_type = PowerArmorType(ent);
    if (power_armor_type) {
        cells = ent->client->pers.inventory[ITEM_INDEX(FindItem("cells"))];
        if (cells == 0) {
            // ran out of cells for power armor
            ent->flags &= ~FL_POWER_ARMOR;
            gi.sound(ent, CHAN_ITEM, gi.soundindex("misc/power2.wav"), 1, ATTN_NORM, 0);
            power_armor_type = 0;
        }
    }

    index = ArmorIndex(ent);
    if (power_armor_type && (!index || (level.framenum & 8))) {
        // flash between power armor and other armor icon
        if (power_armor_type == POWER_ARMOR_SHIELD)
            ent->client->ps.stats[STAT_ARMOR_ICON] = gi.imageindex("i_powershield");
        else
            ent->client->ps.stats[STAT_ARMOR_ICON] = gi.imageindex("i_powerscreen");
        ent->client->ps.stats[STAT_ARMOR] = cells;
    } else if (index) {
        item = GetItemByIndex(index);
        ent->client->ps.stats[STAT_ARMOR_ICON] = gi.imageindex(item->icon);
        ent->client->ps.stats[STAT_ARMOR] = ent->client->pers.inventory[index];
    } else {
        ent->client->ps.stats[STAT_ARMOR_ICON] = 0;
        ent->client->ps.stats[STAT_ARMOR] = 0;
    }

    //
    // pickup message
    //
    if (level.framenum > ent->client->pickup_msg_framenum) {
        ent->client->ps.stats[STAT_PICKUP_ICON] = 0;
        ent->client->ps.stats[STAT_PICKUP_STRING] = 0;
    }

    //
    // timers
    //
    if (G_RereleaseHud(ent)) {
        G_SetPowerupTimer(ent);
    } else if (ent->client->quad_framenum > level.framenum) {
        ent->client->ps.stats[STAT_TIMER_ICON] = gi.imageindex("p_quad");
        ent->client->ps.stats[STAT_TIMER] = (ent->client->quad_framenum - level.framenum) / 10;
    } else if (ent->client->double_framenum > level.framenum) {
        ent->client->ps.stats[STAT_TIMER_ICON] = gi.imageindex("p_double");
        ent->client->ps.stats[STAT_TIMER] = (ent->client->double_framenum - level.framenum) / 10;
    } else if (ent->client->quadfire_framenum > level.framenum) {
        ent->client->ps.stats[STAT_TIMER_ICON] = gi.imageindex("p_quadfire");
        ent->client->ps.stats[STAT_TIMER] = (ent->client->quadfire_framenum - level.framenum) / 10;
    } else if (ent->client->invincible_framenum > level.framenum) {
        ent->client->ps.stats[STAT_TIMER_ICON] = gi.imageindex("p_invulnerability");
        ent->client->ps.stats[STAT_TIMER] = (ent->client->invincible_framenum - level.framenum) / 10;
    } else if (ent->client->enviro_framenum > level.framenum) {
        ent->client->ps.stats[STAT_TIMER_ICON] = gi.imageindex("p_envirosuit");
        ent->client->ps.stats[STAT_TIMER] = (ent->client->enviro_framenum - level.framenum) / 10;
    } else if (ent->client->breather_framenum > level.framenum) {
        ent->client->ps.stats[STAT_TIMER_ICON] = gi.imageindex("p_rebreather");
        ent->client->ps.stats[STAT_TIMER] = (ent->client->breather_framenum - level.framenum) / 10;
    } else {
        ent->client->ps.stats[STAT_TIMER_ICON] = 0;
        ent->client->ps.stats[STAT_TIMER] = 0;
    }

    //
    // selected item
    //
    if (ent->client->pers.selected_item == -1)
        ent->client->ps.stats[STAT_SELECTED_ICON] = 0;
    else
        ent->client->ps.stats[STAT_SELECTED_ICON] = gi.imageindex(itemlist[ent->client->pers.selected_item].icon);

    ent->client->ps.stats[STAT_SELECTED_ITEM] = ent->client->pers.selected_item;

    //
    // layouts
    //
    ent->client->ps.stats[STAT_LAYOUTS] = 0;

    if (deathmatch->value) {
        if (ent->client->pers.health <= 0 || level.intermission_framenum
            || ent->client->showscores)
            ent->client->ps.stats[STAT_LAYOUTS] |= 1;
        if (ent->client->showinventory && ent->client->pers.health > 0)
            ent->client->ps.stats[STAT_LAYOUTS] |= 2;
    } else {
        if (ent->client->showscores || ent->client->showhelp || ent->client->showeou)
            ent->client->ps.stats[STAT_LAYOUTS] |= 1;
        if (ent->client->showinventory && ent->client->pers.health > 0)
            ent->client->ps.stats[STAT_LAYOUTS] |= 2;
    }

    //
    // frags
    //
    ent->client->ps.stats[STAT_FRAGS] = ent->client->resp.score;

    //
    // help icon / current weapon if not shown
    //
    if (ent->client->pers.helpchanged && (level.framenum & 8))
        ent->client->ps.stats[STAT_HELPICON] = gi.imageindex("i_help");
    else if ((ent->client->pers.hand == CENTER_HANDED || ent->client->ps.fov > 91)
             && ent->client->pers.weapon)
        ent->client->ps.stats[STAT_HELPICON] = gi.imageindex(ent->client->pers.weapon->icon);
    else
        ent->client->ps.stats[STAT_HELPICON] = 0;

    ent->client->ps.stats[STAT_SPECTATOR] = 0;

    //
    // rerelease flashlight - the client owns the light itself, the game only
    // owns the on/off state that trigger_flashlight drives
    //
    ent->client->ps.stats[STAT_FLASHLIGHT] = (ent->flags & FL_FLASHLIGHT) ? STAT_FLASHLIGHT_ON : 0;

    //
    // [rerelease] target_healthbar. Two bars packed a byte each into one stat:
    // bit 7 says the bar is up, bits 0-6 are health remaining out of 127. The
    // client draws them (SCR_DrawHealthBars) and can turn them off; the game
    // publishes the numbers either way.
    //
    {
        int stat = 0;
        int i;

        for (i = 0; i < MAX_HEALTH_BARS; i++) {
            edict_t *bar = level.health_bar_entities[i];
            int      value = 0;

            if (!bar) {
                // nothing here
            } else if (bar->timestamp) {
                // the monster is dead and the bar is lingering for "delay"
                if (bar->timestamp < level.framenum)
                    level.health_bar_entities[i] = NULL;
                else
                    value = 0x80;
            } else if (!bar->enemy || !bar->enemy->inuse || bar->enemy->health <= 0) {
                // [rerelease] hack for Makron: Jorg dying does not end the
                // fight, so hold the bar rather than dropping it - Makron is
                // about to take it over.
                if (bar->enemy && (bar->enemy->monsterinfo.aiflags & AI_DOUBLE_TROUBLE)) {
                    value = 0x80;
                } else if (bar->delay > 0) {
                    bar->timestamp = level.framenum + bar->delay * BASE_FRAMERATE;
                    value = 0x80;
                } else {
                    level.health_bar_entities[i] = NULL;
                }
            } else if ((bar->spawnflags & 1) &&
                       !gi.inPVS(ent->s.origin, bar->enemy->s.origin)) {
                // PVS_ONLY: only while the player can see it
            } else {
                float remaining = (float)bar->enemy->health / bar->enemy->max_health;

                if (remaining < 0)
                    remaining = 0;
                else if (remaining > 1)
                    remaining = 1;

                value = (int)(remaining * 0x7f) | 0x80;
            }

            stat |= (value & 0xff) << (i * 8);
        }

        ent->client->ps.stats[STAT_HEALTH_BARS] = (short)stat;
    }

    //
    // [rerelease] cl_rerelease_hud: up to three held keys, taking turns every
    // five seconds when there are more, and the damage this player dealt
    // since the last frame went out, for the hit marker
    //
    ent->client->ps.stats[STAT_KEY_A] = 0;
    ent->client->ps.stats[STAT_KEY_B] = 0;
    ent->client->ps.stats[STAT_KEY_C] = 0;
    ent->client->ps.stats[STAT_HIT_MARKER] = 0;

    if (G_RereleaseHud(ent)) {
        int keys[MAX_ITEMS], num_keys = 0, offset = 0, i;

        if (!deathmatch->value) {
            for (i = 0; i < game.num_items; i++)
                if ((itemlist[i].flags & IT_KEY) && ent->client->pers.inventory[i])
                    keys[num_keys++] = i;

            if (num_keys > 3)
                offset = level.framenum / (5 * BASE_FRAMERATE);

            for (i = 0; i < min(num_keys, 3); i++)
                ent->client->ps.stats[STAT_KEY_A + i] =
                    gi.imageindex(itemlist[keys[(i + offset) % num_keys]].icon);
        }

        ent->client->ps.stats[STAT_HIT_MARKER] = min(ent->client->hit_marker_damage, 0x7fff);
    }
    ent->client->hit_marker_damage = 0;
}

/*
===============
G_CheckChaseStats
===============
*/
void G_CheckChaseStats(edict_t *ent)
{
    int i;
    gclient_t *cl;

    for (i = 1; i <= maxclients->value; i++) {
        cl = g_edicts[i].client;
        if (!g_edicts[i].inuse || cl->chase_target != ent)
            continue;
        memcpy(cl->ps.stats, ent->client->ps.stats, sizeof(cl->ps.stats));
        G_SetSpectatorStats(g_edicts + i);
    }
}

/*
===============
G_SendInventory

The client's weapon bar reads cl.inventory[], and the 1997 game only ever fills
that in when the player opens the inventory screen - so every count on the bar
would be whatever it was the last time you pressed Tab.

Push it whenever it has really changed: at once when an item appears or runs
out, and otherwise no more than five times a second.  Half a second of lag on a
number you only see while the bar is up is invisible, and the current weapon's
own ammo is live in STAT_AMMO regardless.
===============
*/
#define INVENTORY_RESEND_FRAMES 5

static int      inventory_sent[MAX_CLIENTS][MAX_ITEMS];
static int      inventory_sent_frame[MAX_CLIENTS];

void G_ResetInventoryTracking(edict_t *ent)
{
    int     n = ent->client - game.clients;

    memset(inventory_sent[n], 0, sizeof(inventory_sent[n]));
    inventory_sent_frame[n] = -INVENTORY_RESEND_FRAMES;
}

void G_SendInventory(edict_t *ent)
{
    int     n = ent->client - game.clients;
    int     *sent = inventory_sent[n];
    int     i;
    bool    changed = false, urgent = false;

    for (i = 0; i < MAX_ITEMS; i++) {
        int have = ent->client->pers.inventory[i];

        if (sent[i] == have)
            continue;
        changed = true;
        if (!sent[i] || !have) {
            urgent = true;      // picked something up, or ran it out
            break;
        }
    }

    if (!changed)
        return;
    if (!urgent && level.framenum - inventory_sent_frame[n] < INVENTORY_RESEND_FRAMES)
        return;

    for (i = 0; i < MAX_ITEMS; i++)
        sent[i] = ent->client->pers.inventory[i];
    inventory_sent_frame[n] = level.framenum;

    gi.WriteByte(svc_inventory);
    for (i = 0; i < MAX_ITEMS; i++)
        gi.WriteShort(ent->client->pers.inventory[i]);
    gi.unicast(ent, true);
}

/*
===============
G_SetSpectatorStats
===============
*/
void G_SetSpectatorStats(edict_t *ent)
{
    gclient_t *cl = ent->client;

    if (!cl->chase_target)
        G_SetStats(ent);

    cl->ps.stats[STAT_SPECTATOR] = 1;

    // layouts are independant in spectator
    cl->ps.stats[STAT_LAYOUTS] = 0;
    if (cl->pers.health <= 0 || level.intermission_framenum || cl->showscores)
        cl->ps.stats[STAT_LAYOUTS] |= 1;
    if (cl->showinventory && cl->pers.health > 0)
        cl->ps.stats[STAT_LAYOUTS] |= 2;

    if (cl->chase_target && cl->chase_target->inuse)
        cl->ps.stats[STAT_CHASE] = CS_PLAYERSKINS +
                                   (cl->chase_target - g_edicts) - 1;
    else
        cl->ps.stats[STAT_CHASE] = 0;
}

