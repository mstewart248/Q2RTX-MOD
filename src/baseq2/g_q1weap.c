/*
Copyright (C) 1996-1997 Id Software, Inc.

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
==============================================================================

QUAKE'S WEAPONS

The bot match's weapons row has a "quake" choice: g_q3_weapons 3, read into
level.q1_weapons by Q3_LevelRules at each map start (deathmatch, with Quake's
data there) and sent to the clients as a '3' in CS_Q3RULES. Everyone then
plays with Quake's eight guns, on any map set:

  axe, shotgun, super shotgun, nailgun, super nailgun, grenade launcher,
  rocket launcher, thunderbolt

They are Quake's in what they do - weapons.qc's damage, spread, speeds,
splash and attack times, and player.qc's frames, which ran at 10 Hz as
Quake II's weapon think does, so the guns run there too - and in how they
sound and look in the hand (Quake's view models). The rockets and grenades
fly with Quake II's trails and go off in Quake II's fire and Flow
(TE_Q1_EXPLOSION, the client's), as the Quake III guns' do.

The ammo is Quake II's items, shared: shells, bullets for the nails, rockets
for both launchers, cells for the Thunderbolt, in Quake's amounts. Quake II's
own guns and their other ammo turn into the Quake gun or ammo most like them
as the map spawns (q1w_swap), on Quake II's, Quake III's and Quake's own maps
(g_q1.c and g_q3.c rename those to Quake II's first).

==============================================================================
*/

#include "g_local.h"
#include "m_player.h"

#define Q1_PATH(x)      "id1/id1/" x

// one Quake gun, by gitem_t->tag (Q1W_*)
typedef struct {
    const char  *classname;
    int         attack_ticks;   // weapons.qc's attack_finished, in 10 Hz thinks
    int         pickup_ammo;    // items.qc weapon_touch: ammo with the gun
    int         rank;           // items.qc RankForWeapon: lower is better
} q1w_info_t;

static const q1w_info_t q1w_info[Q1W_NUM] = {
    [Q1W_AXE]           = { "weapon_q1_axe",               5,  0, 7 },
    [Q1W_SHOTGUN]       = { "weapon_q1_shotgun",           5,  0, 7 },
    [Q1W_SUPERSHOTGUN]  = { "weapon_q1_supershotgun",      7,  5, 5 },
    [Q1W_NAILGUN]       = { "weapon_q1_nailgun",           1, 30, 6 },
    [Q1W_SUPERNAILGUN]  = { "weapon_q1_supernailgun",      1, 30, 3 },
    [Q1W_GRENADE]       = { "weapon_q1_grenadelauncher",   6,  5, 4 },
    [Q1W_ROCKET]        = { "weapon_q1_rocketlauncher",    8,  5, 2 },
    [Q1W_LIGHTNING]     = { "weapon_q1_lightning",         1, 15, 1 },
};

// Quake's ammo limits (items.qc bound_other_ammo)
#define Q1_MAX_SHELLS   100
#define Q1_MAX_NAILS    200
#define Q1_MAX_ROCKETS  100
#define Q1_MAX_CELLS    100

#define Q1_WEAPON_RESPAWN   30
#define Q1_AMMO_RESPAWN     30

// Quake II's guns and ammo as Quake's, the mission packs' among them (the
// rerelease's deathmatch maps place them). Quake's own maps come here through
// g_q1.c's renames, so its nailgun is a machinegun by now, and so on.
static const struct {
    const char  *q2, *q1;
} q1w_swap[] = {
    { "weapon_shotgun",         "weapon_q1_supershotgun" },
    { "weapon_supershotgun",    "weapon_q1_supershotgun" },
    { "weapon_chainfist",       "weapon_q1_supershotgun" },
    { "weapon_machinegun",      "weapon_q1_nailgun" },
    { "weapon_etf_rifle",       "weapon_q1_nailgun" },
    { "weapon_chaingun",        "weapon_q1_supernailgun" },
    { "weapon_hyperblaster",    "weapon_q1_supernailgun" },
    { "weapon_boomer",          "weapon_q1_supernailgun" },
    { "weapon_grenadelauncher", "weapon_q1_grenadelauncher" },
    { "weapon_proxlauncher",    "weapon_q1_grenadelauncher" },
    { "weapon_rocketlauncher",  "weapon_q1_rocketlauncher" },
    { "weapon_phalanx",         "weapon_q1_rocketlauncher" },
    { "weapon_railgun",         "weapon_q1_lightning" },
    // in a game of Quake's guns; the Ranger by player model keeps it (q1w_keep)
    { "weapon_bfg",             "weapon_q1_lightning" },
    { "weapon_plasmabeam",      "weapon_q1_lightning" },
    { "weapon_disintegrator",   "weapon_q1_lightning" },
    // the ammo no Quake gun takes, as the ammo of the gun standing in
    { "ammo_grenades",          "ammo_rockets" },
    { "ammo_prox",              "ammo_rockets" },
    { "ammo_slugs",             "ammo_cells" },
    { "ammo_magslug",           "ammo_cells" },
    { "ammo_disruptor",         "ammo_cells" },
    { "ammo_flechettes",        "ammo_bullets" },
    { "ammo_tesla",             "ammo_cells" },
    { "ammo_trap",              "ammo_rockets" },
};

/*
=================
Q1W_Available

Q3_LevelRules: Quake's guns need Quake's own episode (id1/id1/pak0.pak), for
the models, sounds and icons. Without it the menu hides the choice, and a
value left in a config falls back to Quake II's guns.
=================
*/
bool Q1W_Available(void)
{
    return gi.cvar("fs_quake1_id1", "0", 0)->integer == 1;
}

static gitem_t *Q1W_Item(const char *classname)
{
    return FindItemByClassname((char *)classname);
}

static gitem_t *Q1W_Gun(int tag)
{
    return tag > Q1W_NONE && tag < Q1W_NUM ? Q1W_Item(q1w_info[tag].classname) : NULL;
}

static bool Q1W_IsGun(const gitem_t *it)
{
    return it && (it->flags & IT_Q1) && (it->flags & IT_WEAPON) &&
           it->tag > Q1W_NONE && it->tag < Q1W_NUM;
}

/*
=================
Q1W_ClientQ1 / Q1W_PlayerQ1

Whether this player plays with Quake's guns this level: everyone, in a game
of them (g_q3_weapons 3); in a match by player model (g_q3_weapons 2), anyone
who is Quake's Ranger - "quake1/<skin>", the client's q1player.c - as a
Quake III body gets Quake III's guns (Q3W_ClientQ3). Matt, 2026-11-22: "Quake
1 ranger should use quake 1 weapons when playing with mixed weapons".
=================
*/
#define Q1W_RANGER_MODEL    "quake1"    // the client's Q1P_MODEL_NAME

bool Q1W_ClientQ1(const gclient_t *client)
{
    const char  *skin;
    size_t      len = strlen(Q1W_RANGER_MODEL);

    if (!client)
        return false;
    if (level.q1_weapons)
        return true;
    if (!level.q1_by_model)
        return false;
    skin = Info_ValueForKey((char *)client->pers.userinfo, "skin");
    return !Q_strncasecmp(skin, Q1W_RANGER_MODEL, len) && (skin[len] == '/' || skin[len] == '\\');
}

bool Q1W_PlayerQ1(const edict_t *ent)
{
    return ent && ent->client && Q1W_ClientQ1(ent->client);
}

// Quake III's guns as Quake's, for the Ranger in a match by player model
static const struct {
    const char  *q3, *q1;
} q1w_from_q3[] = {
    { "weapon_q3_gauntlet",         "weapon_q1_axe" },
    { "weapon_q3_machinegun",       "weapon_q1_nailgun" },
    { "weapon_q3_shotgun",          "weapon_q1_supershotgun" },
    { "weapon_q3_grenadelauncher",  "weapon_q1_grenadelauncher" },
    { "weapon_q3_rocketlauncher",   "weapon_q1_rocketlauncher" },
    { "weapon_q3_lightning",        "weapon_q1_lightning" },
    { "weapon_q3_railgun",          "weapon_q1_lightning" },
    { "weapon_q3_plasmagun",        "weapon_q1_supernailgun" },
    // the Lightning Gun's own ammo: the cells of the Thunderbolt standing in
    { "ammo_q3_lightning",          "ammo_cells" },
};

/*
Guns Quake has nothing like: the Ranger takes them as they are, with their own
ammo - Quake II's on Quake II's and Quake's maps, Quake III's on Quake III's
(Q1W_RangerItem). Matt, 2026-11-24: "If there is no one to one quake 1 weapon
with the quake3 weapon (bfg for example) maybe just use/pickup the
quake2/quake3 (depending on map)". Checked before the tables above, so a
game of Quake's guns (g_q3_weapons 3) still turns the BFG10K into the
Thunderbolt.
*/
static const char *const q1w_keep[] = {
    "weapon_bfg",
    "weapon_q3_bfg",
    "ammo_q3_bfg",      // the BFG10K's; Quake II's takes cells
};

static bool Q1W_Keeps(const gitem_t *it)
{
    int i;

    if (!it || !it->classname)
        return false;
    for (i = 0; i < q_countof(q1w_keep); i++)
        if (!strcmp(it->classname, q1w_keep[i]))
            return true;
    return false;
}

// and the other way, for everyone else and a gun a Ranger dropped: Quake II's
// gun, and Quake III's for a Quake III body
static const struct {
    const char  *q2, *q3;
} q1w_to[Q1W_NUM] = {
    [Q1W_AXE]           = { "weapon_chainfist",         "weapon_q3_gauntlet" },
    [Q1W_SHOTGUN]       = { "weapon_shotgun",           "weapon_q3_shotgun" },
    [Q1W_SUPERSHOTGUN]  = { "weapon_supershotgun",      "weapon_q3_shotgun" },
    [Q1W_NAILGUN]       = { "weapon_machinegun",        "weapon_q3_machinegun" },
    [Q1W_SUPERNAILGUN]  = { "weapon_chaingun",          "weapon_q3_plasmagun" },
    [Q1W_GRENADE]       = { "weapon_grenadelauncher",   "weapon_q3_grenadelauncher" },
    [Q1W_ROCKET]        = { "weapon_rocketlauncher",    "weapon_q3_rocketlauncher" },
    [Q1W_LIGHTNING]     = { "weapon_railgun",           "weapon_q3_lightning" },
};

/*
=================
Q1W_RangerItem

What the Ranger takes from a pickup that is (item), with (alt) the other
game's gun or ammo riding along (edict_t q3_alt_item, NULL for none): Quake's
gun most like the item, and Quake's ammo for the ammo no Quake gun uses. A
gun Quake has nothing like (q1w_keep) is the map's game's own: Quake III's on
a Quake III map, Quake II's anywhere else. Anything else is itself.
=================
*/
static gitem_t *Q1W_RangerItem(gitem_t *item, gitem_t *alt)
{
    gitem_t *other;
    int     i;

    if (!item || !item->classname || (item->flags & IT_Q1))
        return item;

    if (Q1W_Keeps(item)) {
        if (alt && Q1W_Keeps(alt) && !!(alt->flags & IT_Q3) == level.q3map)
            return alt;
        return item;
    }

    if (item->flags & IT_Q3) {
        for (i = 0; i < q_countof(q1w_from_q3); i++)
            if (!strcmp(item->classname, q1w_from_q3[i].q3) && (other = Q1W_Item(q1w_from_q3[i].q1)))
                return other;
        return item;
    }

    for (i = 0; i < q_countof(q1w_swap); i++)
        if (!strcmp(item->classname, q1w_swap[i].q2) && (other = Q1W_Item(q1w_swap[i].q1)))
            return other;
    return item;
}

/*
=================
Q1W_ItemFor

Q3W_ItemFor, in a match by player model with Quake's Ranger in it: what
(player) takes from (item_ent) on the ground. The Ranger takes
Q1W_RangerItem's; anyone else takes their own game's gun for a Quake gun a
Ranger dropped. Anything else is itself.
=================
*/
gitem_t *Q1W_ItemFor(const edict_t *item_ent, const edict_t *player)
{
    gitem_t     *item = item_ent->item;
    gitem_t     *other;
    const char  *name;

    if (!item || !item->classname || !level.q1_by_model)
        return item;

    if (!Q1W_PlayerQ1(player)) {
        if (!Q1W_IsGun(item))
            return item;
        name = Q3W_PlayerQ3(player) ? q1w_to[item->tag].q3 : q1w_to[item->tag].q2;
        other = name ? Q1W_Item(name) : NULL;
        return other ? other : item;
    }

    return Q1W_RangerItem(item, item_ent->q3_alt_item);
}

/*
=================
Q1W_FindItem

Q3W_FindItem, with Quake's guns: Quake's gun of a name before Quake II's or
Quake III's ("Shotgun", "Rocket Launcher"...); a gun the Ranger keeps
(q1w_keep - Quake II's and Quake III's BFG10K share the name) as the one he
carries; and for the name of a Quake II gun that is not Quake's, the Quake gun
standing in for it - so the stock "use Chaingun" and "use Railgun" binds raise
the super nailgun and the Thunderbolt.
=================
*/
gitem_t *Q1W_FindItem(edict_t *ent, const char *name)
{
    gitem_t *it, *found;
    int     i, j;

    for (i = 0, it = itemlist; i < game.num_items; i++, it++)
        if ((it->flags & IT_Q1) && it->pickup_name && !Q_stricmp(it->pickup_name, name))
            return it;

    if (ent && ent->client)
        for (i = 0, it = itemlist; i < game.num_items; i++, it++)
            if (Q1W_Keeps(it) && it->pickup_name && !Q_stricmp(it->pickup_name, name) &&
                ent->client->pers.inventory[i])
                return it;

    found = FindItem((char *)name);
    if (level.q1_by_model && Q1W_Keeps(found))
        return found;   // not carried: "Out of item"
    if (found && (found->flags & IT_WEAPON) && found->classname)
        for (j = 0; j < q_countof(q1w_swap); j++)
            if (!strcmp(found->classname, q1w_swap[j].q2))
                return Q1W_Item(q1w_swap[j].q1);
    return found;
}

/*
==============================================================================

ITEMS ON THE MAP

==============================================================================
*/

/*
=================
Q1W_SpawnClassname

ED_CallSpawn, for every entity of every map: which item a Quake II gun or
ammo spawns as.
=================
*/
const char *Q1W_SpawnClassname(const char *classname)
{
    int     i;

    if (!level.q1_weapons || !classname)
        return classname;

    for (i = 0; i < q_countof(q1w_swap); i++)
        if (!strcmp(classname, q1w_swap[i].q2))
            return q1w_swap[i].q1;
    return classname;
}

/*
=================
Q1W_ItemModel

droptofloor and Drop_Item, after Q3W_ItemModel.

In a game of Quake's guns: Quake's gun models stand on their origin, where
Quake II's are centred on it, so the box goes from the origin up and the drop
puts the origin on the floor.

In a match by player model, the Ranger SEES what he takes (Matt, 2026-11-24:
"the weapon pickups should be quake1 weapons"). A pickup carrying both games'
guns (RF_Q3_ITEM: Quake II's model in modelindex, Quake III's in modelindex2)
also carries the model of the Ranger's in modelindex3, which the client draws
for him alone: Quake's gun, or for a gun he keeps the Quake II or Quake III
model already there (the client tells them apart by index). A Quake gun a
Ranger dropped becomes such a pickup too, with the guns the others take from
it. The box stays Quake II's, centred on the origin; the client lowers Quake's
model by its half height to stand it where Quake would.
=================
*/
void Q1W_ItemModel(edict_t *ent)
{
    gitem_t *item = ent->item, *q2, *q3, *mine;

    if (!item)
        return;

    if (level.q1_weapons) {
        if (item->flags & IT_Q1) {
            ent->mins[2] = 0;
            ent->maxs[2] = 30;
        }
        return;
    }

    if (!level.q1_by_model)
        return;

    if (Q1W_IsGun(item)) {
        q2 = Q1W_Item(q1w_to[item->tag].q2);
        q3 = Q1W_Item(q1w_to[item->tag].q3);
        if (!q2 || !q3 || !q2->world_model || !q3->world_model || !item->world_model)
            return;     // Quake's model for everyone
        ent->s.modelindex = gi.modelindex(q2->world_model);
        ent->s.modelindex2 = gi.modelindex(q3->world_model);
        ent->s.modelindex3 = gi.modelindex(item->world_model);
        ent->s.renderfx |= RF_Q3_ITEM;
        return;
    }

    if (!(ent->s.renderfx & RF_Q3_ITEM) || !ent->s.modelindex2)
        return;
    mine = Q1W_RangerItem(item, ent->q3_alt_item);
    // the axe from a gauntlet: its model is the one in the hand, so he sees
    // the gauntlet
    if (!mine || !mine->world_model || (Q1W_IsGun(mine) && mine->tag == Q1W_AXE))
        return;
    ent->s.modelindex3 = gi.modelindex(mine->world_model);
}

/*
=================
Q1W_Rank

items.qc RankForWeapon, lower is better. A gun the Ranger keeps (q1w_keep,
the BFG10K) is better than all of Quake's, so a pickup of Quake's never takes
it out of his hands, and he raises it as he would a better gun.
=================
*/
static int Q1W_Rank(const gitem_t *it)
{
    if (Q1W_IsGun(it))
        return q1w_info[it->tag].rank;
    if (it && (it->flags & IT_WEAPON) && Q1W_Keeps(it))
        return 0;
    return 7;
}

/*
=================
Q1W_RaiseOnPickup

Pickup_Weapon, for a Quake II gun the Ranger keeps (Quake III's switch to any
gun new to the player, Pickup_Q3Weapon): raise it if it is better than the one
in hand, as Quake does (items.qc weapon_touch).
=================
*/
bool Q1W_RaiseOnPickup(edict_t *player, const gitem_t *item)
{
    gclient_t   *client = player->client;
    gitem_t     *current;

    if (!level.q1_by_model || !Q1W_PlayerQ1(player))
        return false;
    current = client->newweapon ? client->newweapon : client->pers.weapon;
    return Q1W_Rank(item) < Q1W_Rank(current);
}

static int Q1W_Ammo(edict_t *ent, const gitem_t *gun)
{
    gitem_t *ammo;

    if ((int)dmflags->value & DF_INFINITE_AMMO)
        return 999;
    if (!gun || !gun->ammo || !(ammo = FindItem(gun->ammo)))
        return 999;     // the axe
    return ent->client->pers.inventory[ITEM_INDEX(ammo)];
}

static bool Q1W_Has(edict_t *ent, int tag)
{
    gitem_t *it = Q1W_Gun(tag);

    return it && ent->client->pers.inventory[ITEM_INDEX(it)] > 0;
}

/*
=================
Q1W_BestWeapon

weapons.qc W_BestWeapon: the Thunderbolt out of water, then the nailguns and
shotguns with ammo for them - never a launcher, which could kill the one
holding it - and the axe.
=================
*/
static gitem_t *Q1W_BestWeapon(edict_t *ent)
{
    static const int order[] = { Q1W_SUPERNAILGUN, Q1W_SUPERSHOTGUN, Q1W_NAILGUN, Q1W_SHOTGUN };
    int     i, need;

    if (ent->waterlevel <= 1 && Q1W_Has(ent, Q1W_LIGHTNING) && Q1W_Ammo(ent, Q1W_Gun(Q1W_LIGHTNING)) >= 1)
        return Q1W_Gun(Q1W_LIGHTNING);
    for (i = 0; i < q_countof(order); i++) {
        need = (order[i] == Q1W_SUPERNAILGUN || order[i] == Q1W_SUPERSHOTGUN) ? 2 : 1;
        if (Q1W_Has(ent, order[i]) && Q1W_Ammo(ent, Q1W_Gun(order[i])) >= need)
            return Q1W_Gun(order[i]);
    }
    return Q1W_Gun(Q1W_AXE);
}

/*
=================
Q1W_NoAmmoChange

NoAmmoWeaponChange and Quake III's (Q3W_NoAmmoChange), for the Ranger out of
ammo for a gun he keeps (the BFG10K): Quake's best gun, as Quake's own guns
do. Their searches look only at their own game's guns, which he has none of.
False for anyone else.
=================
*/
bool Q1W_NoAmmoChange(edict_t *ent)
{
    gitem_t *best;

    if (!level.q1_by_model || !Q1W_PlayerQ1(ent))
        return false;
    best = Q1W_BestWeapon(ent);
    if (best && best != ent->client->pers.weapon)
        ent->client->newweapon = best;
    return true;
}

/*
=================
Pickup_Q1Weapon

items.qc weapon_touch: the gun's ammo, and in hand if it ranks above the one
there (Deathmatch_Weapon). Back in 30 seconds, or left for others to take
with weapons stay (Quake's deathmatch 2).
=================
*/
bool Pickup_Q1Weapon(edict_t *ent, edict_t *other)
{
    gclient_t   *client = other->client;
    gitem_t     *item = ent->item;
    gitem_t     *ammo = item->ammo ? FindItem(item->ammo) : NULL;
    gitem_t     *current;
    int         index = ITEM_INDEX(item);
    bool        dropped = (ent->spawnflags & (DROPPED_ITEM | DROPPED_PLAYER_ITEM)) != 0;
    bool        stay = ((int)dmflags->value & DF_WEAPONS_STAY) && !dropped;
    int         quantity;

    if (stay && client->pers.inventory[index])
        return false;

    client->pers.inventory[index] = 1;

    if (ammo) {
        quantity = ent->count ? ent->count : q1w_info[item->tag].pickup_ammo;
        if ((int)dmflags->value & DF_INFINITE_AMMO)
            quantity = 1000;
        Add_Ammo(other, ammo, quantity);
    }

    if (!dropped && deathmatch->value) {
        if (stay)
            ent->flags |= FL_RESPAWN;
        else
            SetRespawn(ent, Q1_WEAPON_RESPAWN);
    }

    current = client->newweapon ? client->newweapon : client->pers.weapon;
    if (Q1W_Rank(item) < Q1W_Rank(current))
        client->newweapon = item;

    return true;
}

/*
=================
Q1W_PickupAmmo

Pickup_Ammo, in a game of Quake's guns: items.qc's boxes - 20 shells, 25
nails, 5 rockets, 6 cells, twice that in a big box (g_q1.c sets its count) -
and, if the gun in hand was the best one, the best one after (ammo_touch).
False to leave the box.
=================
*/
bool Q1W_PickupAmmo(edict_t *ent, edict_t *other)
{
    gclient_t   *client = other->client;
    gitem_t     *item = ent->item;
    gitem_t     *best_before, *best_after;
    int         count;

    if (ent->count)
        count = ent->count;
    else if (item->tag == AMMO_SHELLS)
        count = 20;
    else if (item->tag == AMMO_BULLETS)
        count = 25;
    else if (item->tag == AMMO_ROCKETS)
        count = 5;
    else if (item->tag == AMMO_CELLS)
        count = 6;
    else
        count = item->quantity;

    best_before = Q1W_BestWeapon(other);
    if (!Add_Ammo(other, item, count))
        return false;

    best_after = Q1W_BestWeapon(other);
    if (client->pers.weapon == best_before && best_after != best_before && !client->newweapon)
        client->newweapon = best_after;

    if (!(ent->spawnflags & (DROPPED_ITEM | DROPPED_PLAYER_ITEM)) && deathmatch->value)
        SetRespawn(ent, Q1_AMMO_RESPAWN);
    return true;
}

/*
=================
Q1W_GiveSpawnKit

InitClientPersistant: Quake's spawn (client.qc SetNewParms) - the axe and the
shotgun with 25 shells, no blaster - and Quake's ammo limits.
=================
*/
void Q1W_GiveSpawnKit(gclient_t *client)
{
    gitem_t *axe = Q1W_Gun(Q1W_AXE);
    gitem_t *shotgun = Q1W_Gun(Q1W_SHOTGUN);
    gitem_t *shells = Q1W_Item("ammo_shells");
    gitem_t *blaster = Q1W_Item("weapon_blaster");

    if (!axe || !shotgun || !shells)
        return;

    if (blaster)
        client->pers.inventory[ITEM_INDEX(blaster)] = 0;
    client->pers.inventory[ITEM_INDEX(axe)] = 1;
    client->pers.inventory[ITEM_INDEX(shotgun)] = 1;
    client->pers.inventory[ITEM_INDEX(shells)] = 25;
    client->pers.weapon = shotgun;
    client->pers.selected_item = ITEM_INDEX(shotgun);

    client->pers.max_shells = Q1_MAX_SHELLS;
    client->pers.max_bullets = Q1_MAX_NAILS;
    client->pers.max_rockets = Q1_MAX_ROCKETS;
    client->pers.max_cells = Q1_MAX_CELLS;
}

/*
=================
Q1W_Precache

SP_worldspawn, in a game of Quake's guns: every gun and what it shoots, as a
spawn kit or a dropped gun needs them without one on the map.
=================
*/
void Q1W_Precache(void)
{
    static const char *const sounds[] = {
        "weapons/ax1.wav", "player/axhit1.wav", "player/axhit2.wav",
        "weapons/guncock.wav", "weapons/shotgn2.wav",
        "weapons/rocket1i.wav", "weapons/spike2.wav",
        "weapons/grenade.wav", "weapons/bounce.wav", "weapons/sgun1.wav",
        "weapons/lstart.wav", "weapons/lhit.wav",
        "weapons/tink1.wav", "weapons/ric1.wav", "weapons/ric2.wav", "weapons/ric3.wav",
        "weapons/r_exp3.wav", "weapons/pkup.wav",
    };
    static const char *const models[] = {
        Q1_PATH("progs/spike.mdl"), Q1_PATH("progs/s_spike.mdl"),
        Q1_PATH("progs/grenade.mdl"), Q1_PATH("progs/missile.mdl"),
        Q1_PATH("progs/bolt2.mdl"),
    };
    int     i;

    if (!level.q1_weapons && !level.q1_by_model)
        return;

    for (i = 1; i < Q1W_NUM; i++)
        PrecacheItem(Q1W_Gun(i));
    for (i = 0; i < q_countof(sounds); i++)
        Q1_SoundIndex(sounds[i]);
    for (i = 0; i < q_countof(models); i++)
        gi.modelindex((char *)models[i]);
    for (i = 0; i < q_countof(q1w_swap); i++)
        if (!strncmp(q1w_swap[i].q1, "ammo_", 5))
            PrecacheItem(Q1W_Item(q1w_swap[i].q1));
    // a Quake gun a Ranger drops shows the others theirs (Q1W_ItemModel):
    // their pickup models now, not a disk load at the drop
    if (level.q1_by_model) {
        for (i = 1; i < Q1W_NUM; i++) {
            gitem_t *q2 = q1w_to[i].q2 ? Q1W_Item(q1w_to[i].q2) : NULL;
            gitem_t *q3 = q1w_to[i].q3 ? Q1W_Item(q1w_to[i].q3) : NULL;

            if (q2 && q2->world_model)
                gi.modelindex(q2->world_model);
            if (q3 && q3->world_model)
                gi.modelindex(q3->world_model);
        }
    }
    gi.imageindex("/id1/id1/gfx/sb_shells.lmp");
    gi.imageindex("/id1/id1/gfx/sb_nails.lmp");
    gi.imageindex("/id1/id1/gfx/sb_rocket.lmp");
    gi.imageindex("/id1/id1/gfx/sb_cells.lmp");
}

/*
=================
Q1W_AmmoIcon

G_SetStats: the ammo counter wears Quake's status bar icons while a Quake
gun is in hand.
=================
*/
const char *Q1W_AmmoIcon(const gitem_t *weapon)
{
    gitem_t *ammo;

    if (!weapon || !(weapon->flags & IT_Q1) || !weapon->ammo || !(ammo = FindItem(weapon->ammo)))
        return NULL;
    switch (ammo->tag) {
    case AMMO_SHELLS:   return "/id1/id1/gfx/sb_shells.lmp";
    case AMMO_BULLETS:  return "/id1/id1/gfx/sb_nails.lmp";
    case AMMO_ROCKETS:  return "/id1/id1/gfx/sb_rocket.lmp";
    case AMMO_CELLS:    return "/id1/id1/gfx/sb_cells.lmp";
    default:            return NULL;
    }
}

/*
==============================================================================

FIRING

weapons.qc. Quake's damage pushes a player by 8 units a second for each
point (T_Damage); Quake II's T_Damage gives 500 / mass for each point of
knockback against someone else and 1600 / mass against yourself, mass 200,
so the knockback asked of it is 16/5 the damage for others and the damage
itself for you (Q1W_Knock).

==============================================================================
*/

static int Q1W_DamageScale(edict_t *ent)
{
    int scale = 1;

    if (ent->client->quad_framenum > level.framenum)
        scale *= 4;
    if (ent->client->double_framenum > level.framenum)
        scale *= 2;
    return scale;
}

static int Q1W_Knock(const edict_t *targ, const edict_t *attacker, int damage)
{
    return targ == attacker ? damage : damage * 16 / 5;
}

static void Q1W_Damage(edict_t *targ, edict_t *inflictor, edict_t *attacker, const vec3_t dir,
                       const vec3_t point, const vec3_t normal, int damage, int dflags, int mod)
{
    vec3_t  push;

    // Quake pushes away from the middle of the inflictor
    VectorAdd(inflictor->absmin, inflictor->absmax, push);
    VectorScale(push, 0.5f, push);
    VectorSubtract(targ->s.origin, push, push);
    if (VectorLength(push) < 0.01f)
        VectorCopy(dir, push);

    T_Damage(targ, inflictor, attacker, push, (vec_t *)point, (vec_t *)normal,
             damage, Q1W_Knock(targ, attacker, damage), dflags, mod);
}

/*
=================
Q1W_RadiusDamage

combat.qc T_RadiusDamage: damage less half the distance to the middle of the
target's box, your own share halved, out to the damage plus 40 units.
=================
*/
static void Q1W_RadiusDamage(edict_t *inflictor, edict_t *attacker, float damage, edict_t *ignore, int mod)
{
    edict_t *ent = NULL;
    vec3_t  v;
    float   points;

    while ((ent = findradius(ent, inflictor->s.origin, damage + 40)) != NULL) {
        if (ent == ignore || !ent->takedamage)
            continue;

        VectorAdd(ent->mins, ent->maxs, v);
        VectorMA(ent->s.origin, 0.5f, v, v);
        VectorSubtract(inflictor->s.origin, v, v);
        points = damage - 0.5f * VectorLength(v);
        if (ent == attacker)
            points *= 0.5f;
        if (points <= 0 || !CanDamage(ent, inflictor))
            continue;

        Q1W_Damage(ent, inflictor, attacker, vec3_origin, inflictor->s.origin, vec3_origin,
                   (int)points, DAMAGE_RADIUS, mod);
    }
}

static void Q1W_UseAmmo(edict_t *ent, int count)
{
    gclient_t *client = ent->client;

    if (!client->ammo_index || ((int)dmflags->value & DF_INFINITE_AMMO))
        return;
    client->pers.inventory[client->ammo_index] -= count;
    if (client->pers.inventory[client->ammo_index] < 0)
        client->pers.inventory[client->ammo_index] = 0;
}

static void Q1W_TempEntity(int kind, const vec3_t pos, multicast_t to)
{
    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_Q1);
    gi.WriteByte(kind);
    gi.WritePosition((vec_t *)pos);
    gi.multicast((vec_t *)pos, to);
}

static void Q1W_MuzzleFlash(edict_t *ent)
{
    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_Q1);
    gi.WriteByte(TE_Q1_MUZZLEFLASH);
    gi.WriteShort(ent - g_edicts);
    gi.multicast(ent->s.origin, MULTICAST_PVS);
}

static void Q1W_Sound(edict_t *ent, int channel, const char *name)
{
    gi.sound(ent, channel, Q1_SoundIndex(name), 1, ATTN_NORM, 0);
}

// Quake shot from 16 above the origin, 6 below its eyes; a crouching
// Quake II player's eyes are lower, and so is the gun
static void Q1W_Source(edict_t *ent, vec3_t out)
{
    VectorCopy(ent->s.origin, out);
    out[2] += ent->viewheight - 6;
}

static bool Q1W_HitSky(const trace_t *tr)
{
    return tr->surface && (tr->surface->flags & SURF_SKY);
}

/*
=================
Q1W_FireAxe

weapons.qc W_FireAxe: 64 units, 20 damage.
=================
*/
static void Q1W_FireAxe(edict_t *ent)
{
    vec3_t  forward, src, end, org;
    trace_t tr;
    int     damage;

    AngleVectors(ent->client->v_angle, forward, NULL, NULL);
    Q1W_Source(ent, src);
    VectorMA(src, 64, forward, end);
    tr = gi.trace(src, NULL, NULL, end, ent, MASK_SHOT);
    if (tr.fraction == 1.0f || Q1W_HitSky(&tr))
        return;

    VectorMA(tr.endpos, -4, forward, org);
    if (tr.ent && tr.ent->takedamage) {
        damage = 20 * Q1W_DamageScale(ent);
        // player.qc PainSound: the axe's own thud on the one it hits
        if (tr.ent->client)
            Q1W_Sound(tr.ent, CHAN_AUTO, "player/axhit1.wav");
        Q1W_Damage(tr.ent, ent, ent, forward, org, tr.plane.normal, damage, 0, MOD_Q1_AXE);
    } else {
        Q1W_Sound(ent, CHAN_WEAPON, "player/axhit2.wav");
        Q1W_TempEntity(TE_Q1_GUNSHOT, org, MULTICAST_PVS);
    }
}

/*
=================
Q1W_FireBullets

weapons.qc FireBullets: from 7/10 of the way up the shooter's box, 10 units
out, each pellet spread by its own crandom on each axis (unnormalised, as
Quake's), 2048 units, 4 damage.
=================
*/
static void Q1W_FireBullets(edict_t *ent, int shots, float spread_x, float spread_y, int mod)
{
    vec3_t  forward, right, up, src, dir, end, org;
    trace_t tr;
    int     damage = 4 * Q1W_DamageScale(ent);

    AngleVectors(ent->client->v_angle, forward, right, up);
    VectorMA(ent->s.origin, 10, forward, src);
    src[2] = ent->absmin[2] + ent->size[2] * 0.7f;

    while (shots-- > 0) {
        VectorMA(forward, crandom() * spread_x, right, dir);
        VectorMA(dir, crandom() * spread_y, up, dir);
        VectorMA(src, 2048, dir, end);
        tr = gi.trace(src, NULL, NULL, end, ent, MASK_SHOT);
        if (tr.fraction == 1.0f || Q1W_HitSky(&tr))
            continue;

        VectorMA(tr.endpos, -4, dir, org);
        if (tr.ent && tr.ent->takedamage)
            Q1W_Damage(tr.ent, ent, ent, dir, org, tr.plane.normal, damage, DAMAGE_BULLET, mod);
        else
            Q1W_TempEntity(TE_Q1_GUNSHOT, org, MULTICAST_PVS);
    }
}

/*
=================
spikes

weapons.qc launch_spike, spike_touch and superspike_touch: 1000 units a
second, 9 damage or a super nail's 18, gone after 6 seconds.
=================
*/
static void q1_spike_touch(edict_t *self, edict_t *other, cplane_t *plane, csurface_t *surf)
{
    if (other == self->owner)
        return;

    if (surf && (surf->flags & SURF_SKY)) {
        G_FreeEdict(self);
        return;
    }

    if (other->takedamage) {
        Q1W_Damage(other, self, self->owner, self->velocity, self->s.origin,
                   plane ? plane->normal : vec3_origin, self->dmg, DAMAGE_BULLET,
                   self->style ? MOD_Q1_SNAIL : MOD_Q1_NAIL);
    } else {
        Q1W_TempEntity(self->style ? TE_Q1_SUPERSPIKE : TE_Q1_SPIKE, self->s.origin, MULTICAST_PVS);
    }
    G_FreeEdict(self);
}

static void Q1W_LaunchSpike(edict_t *ent, const vec3_t org, const vec3_t dir, bool super)
{
    edict_t *m = G_Spawn();

    m->classname = "spike";
    m->owner = ent;
    m->style = super;
    m->movetype = MOVETYPE_FLYMISSILE;
    m->clipmask = MASK_SHOT;
    m->solid = SOLID_BBOX;
    VectorClear(m->mins);
    VectorClear(m->maxs);
    m->dmg = (super ? 18 : 9) * Q1W_DamageScale(ent);
    m->touch = q1_spike_touch;
    m->think = G_FreeEdict;
    m->nextthink = level.framenum + 6 * BASE_FRAMERATE;
    m->s.modelindex = gi.modelindex(super ? Q1_PATH("progs/s_spike.mdl") : Q1_PATH("progs/spike.mdl"));
    VectorScale(dir, 1000, m->velocity);
    vectoangles((vec_t *)dir, m->s.angles);
    VectorCopy(org, m->s.origin);
    VectorCopy(org, m->s.old_origin);
    gi.linkentity(m);
}

/*
=================
grenades

weapons.qc W_FireGrenade: thrown at 600 forward and 200 up with a little
jitter, then 200 up whatever the aim; bounces off anything that cannot be
aimed at, goes off on anyone who can or after 2.5 seconds; 120 splash.
=================
*/
static void q1_grenade_explode(edict_t *self)
{
    Q1W_RadiusDamage(self, self->owner, self->dmg, NULL, MOD_Q1_G_SPLASH);
    Q1W_TempEntity(TE_Q1_EXPLOSION, self->s.origin, MULTICAST_PHS);
    G_FreeEdict(self);
}

static void q1_grenade_touch(edict_t *self, edict_t *other, cplane_t *plane, csurface_t *surf)
{
    if (other == self->owner)
        return;

    if (surf && (surf->flags & SURF_SKY)) {
        G_FreeEdict(self);
        return;
    }

    if (other->takedamage == DAMAGE_AIM) {
        q1_grenade_explode(self);
        return;
    }

    Q1W_Sound(self, CHAN_VOICE, "weapons/bounce.wav");
    if (VectorCompare(self->velocity, vec3_origin))
        VectorClear(self->avelocity);
}

static void Q1W_FireGrenade(edict_t *ent)
{
    vec3_t  forward, right, up;
    edict_t *m = G_Spawn();

    AngleVectors(ent->client->v_angle, forward, right, up);

    m->classname = "grenade";
    m->owner = ent;
    m->movetype = MOVETYPE_BOUNCE;
    m->clipmask = MASK_SHOT;
    m->solid = SOLID_BBOX;
    VectorClear(m->mins);
    VectorClear(m->maxs);
    m->dmg = 120 * Q1W_DamageScale(ent);
    m->touch = q1_grenade_touch;
    m->think = q1_grenade_explode;
    m->nextthink = level.framenum + 25 * BASE_FRAMERATE / 10;
    m->s.modelindex = gi.modelindex(Q1_PATH("progs/grenade.mdl"));
    m->s.effects = EF_GRENADE;

    VectorScale(forward, 600, m->velocity);
    VectorMA(m->velocity, 200, up, m->velocity);
    VectorMA(m->velocity, crandom() * 10, right, m->velocity);
    VectorMA(m->velocity, crandom() * 10, up, m->velocity);
    m->velocity[2] = 200;
    VectorSet(m->avelocity, 300, 300, 300);
    vectoangles(m->velocity, m->s.angles);

    VectorCopy(ent->s.origin, m->s.origin);
    VectorCopy(ent->s.origin, m->s.old_origin);
    gi.linkentity(m);
}

/*
=================
rockets

weapons.qc W_FireRocket and T_MissileTouch: 1000 units a second from 8 out
and 16 up, 100 to 120 on a direct hit and 120 splash to everyone else, the
blast 8 units back along the flight.
=================
*/
static void q1_rocket_touch(edict_t *self, edict_t *other, cplane_t *plane, csurface_t *surf)
{
    vec3_t  heading;
    int     damage;

    if (other == self->owner)
        return;

    if (surf && (surf->flags & SURF_SKY)) {
        G_FreeEdict(self);
        return;
    }

    if (other->takedamage) {
        damage = (100 + (int)(random() * 20)) * self->count;
        Q1W_Damage(other, self, self->owner, self->velocity, self->s.origin,
                   plane ? plane->normal : vec3_origin, damage, 0, MOD_Q1_ROCKET);
    }

    Q1W_RadiusDamage(self, self->owner, self->dmg, other, MOD_Q1_R_SPLASH);

    VectorNormalize2(self->velocity, heading);
    VectorMA(self->s.origin, -8, heading, self->s.origin);
    Q1W_TempEntity(TE_Q1_EXPLOSION, self->s.origin, MULTICAST_PHS);
    G_FreeEdict(self);
}

static void Q1W_FireRocket(edict_t *ent)
{
    vec3_t  forward, org;
    edict_t *m = G_Spawn();
    int     scale = Q1W_DamageScale(ent);

    AngleVectors(ent->client->v_angle, forward, NULL, NULL);
    Q1W_Source(ent, org);
    VectorMA(org, 8, forward, org);

    m->classname = "rocket";
    m->owner = ent;
    m->movetype = MOVETYPE_FLYMISSILE;
    m->clipmask = MASK_SHOT;
    m->solid = SOLID_BBOX;
    VectorClear(m->mins);
    VectorClear(m->maxs);
    m->count = scale;           // the direct hit's multiplier
    m->dmg = 120 * scale;       // the splash
    m->touch = q1_rocket_touch;
    m->think = G_FreeEdict;
    m->nextthink = level.framenum + 5 * BASE_FRAMERATE;
    m->s.modelindex = gi.modelindex(Q1_PATH("progs/missile.mdl"));
    m->s.effects = EF_ROCKET;
    VectorScale(forward, 1000, m->velocity);
    vectoangles(forward, m->s.angles);
    VectorCopy(org, m->s.origin);
    VectorCopy(org, m->s.old_origin);
    gi.linkentity(m);
}

/*
=================
Q1W_FireLightning

weapons.qc W_FireLightning and LightningDamage: the beam goes 600 units from
16 up and through players to the wall; three traces from the body's middle to
its end, the middle one and one 16 units to each side, hurt up to three
different things 30 each. Fired under water it lets every cell go at once,
35 damage each, on everyone near - the one holding it too.
=================
*/
static void Q1W_FireLightning(edict_t *ent)
{
    gclient_t   *client = ent->client;
    vec3_t      forward, org, end, p2, side, a, b;
    edict_t     *hit[3] = { NULL, NULL, NULL };
    trace_t     tr;
    int         i, cells, damage;

    if (ent->waterlevel > 1) {
        gitem_t *ammo = FindItem("Cells");

        cells = ammo ? client->pers.inventory[ITEM_INDEX(ammo)] : 0;
        if (ammo && !((int)dmflags->value & DF_INFINITE_AMMO))
            client->pers.inventory[ITEM_INDEX(ammo)] = 0;
        Q1W_RadiusDamage(ent, ent, 35 * cells, NULL, MOD_Q1_DISCHARGE);
        return;
    }

    Q1W_UseAmmo(ent, 1);

    if (client->q1_lightning_sound_framenum <= level.framenum) {
        Q1W_Sound(ent, CHAN_WEAPON, "weapons/lhit.wav");
        client->q1_lightning_sound_framenum = level.framenum + 6;
    }

    AngleVectors(client->v_angle, forward, NULL, NULL);
    Q1W_Source(ent, org);
    VectorMA(org, 600, forward, end);
    tr = gi.trace(org, NULL, NULL, end, ent, MASK_SOLID);

    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_Q1);
    gi.WriteByte(TE_Q1_LIGHTNING2);
    gi.WriteShort(ent - g_edicts);
    gi.WritePosition(org);
    gi.WritePosition(tr.endpos);
    gi.multicast(org, MULTICAST_PVS);

    // LightningDamage(self.origin, trace_endpos + v_forward * 4, self, 30)
    VectorMA(tr.endpos, 4, forward, p2);
    VectorSubtract(p2, ent->s.origin, side);
    VectorNormalize(side);
    VectorSet(side, -side[1], side[0], 0);
    VectorScale(side, 16, side);

    damage = 30 * Q1W_DamageScale(ent);
    for (i = 0; i < 3; i++) {
        float s = i == 0 ? 0 : i == 1 ? 1 : -1;

        VectorMA(ent->s.origin, s, side, a);
        VectorMA(p2, s, side, b);
        tr = gi.trace(a, NULL, NULL, b, ent, MASK_SHOT);
        if (!tr.ent || !tr.ent->takedamage || tr.ent == hit[0] || tr.ent == hit[1])
            continue;
        hit[i] = tr.ent;
        Q1W_Damage(tr.ent, ent, ent, forward, tr.endpos, tr.plane.normal, damage, 0, MOD_Q1_LIGHTNING);
    }
}

/*
==============================================================================

THE GUN IN HAND

player.qc's attack frames, one a think: the shotguns' and launchers' six,
the axe's four (one of two swings, the hit on the third), the nailguns'
eight and the Thunderbolt's four round and round while the trigger is held.
Changing guns is Quake's: at once, once the last attack is done.

==============================================================================
*/

static void Q1W_PlayerAttackFrames(edict_t *ent)
{
    gclient_t *client = ent->client;

    if (ent->s.modelindex != 255 || client->anim_priority > ANIM_ATTACK)
        return;
    client->anim_priority = ANIM_ATTACK;
    if (client->ps.pmove.pm_flags & PMF_DUCKED) {
        ent->s.frame = FRAME_crattak1 - 1;
        client->anim_end = FRAME_crattak9;
    } else {
        ent->s.frame = FRAME_attack1 - 1;
        client->anim_end = FRAME_attack8;
    }
}

// Quake's SuperDamageSound: each shot of a single-shot gun, once a second
// from a held one
static void Q1W_QuadSound(edict_t *ent, bool held)
{
    if (ent->client->quad_framenum <= level.framenum)
        return;
    if (held && (level.framenum % BASE_FRAMERATE))
        return;
    gi.sound(ent, CHAN_ITEM, gi.soundindex("items/damage3.wav"), 1, ATTN_NORM, 0);
}

static int Q1W_AmmoInHand(edict_t *ent)
{
    gclient_t *client = ent->client;

    if (!client->ammo_index || ((int)dmflags->value & DF_INFINITE_AMMO))
        return 999;
    return client->pers.inventory[client->ammo_index];
}

/*
=================
Q1W_Attack

weapons.qc W_Attack: one shot of the gun in hand, or the first of a held
nailgun or Thunderbolt.
=================
*/
static void Q1W_Attack(edict_t *ent, int tag)
{
    gclient_t   *client = ent->client;
    vec3_t      forward, right, org;
    int         ammo = Q1W_AmmoInHand(ent);

    AngleVectors(client->v_angle, forward, right, NULL);

    switch (tag) {
    case Q1W_AXE:
        Q1W_Sound(ent, CHAN_WEAPON, "weapons/ax1.wav");
        // player_axe1 / axeb1 / axec1 / axed1: two swings of the view axe
        if (random() < 0.5f) {
            client->ps.gunframe = 1;
            client->q1_anim_end = 4;
        } else {
            client->ps.gunframe = 5;
            client->q1_anim_end = 8;
        }
        client->q1_axe_frame = client->ps.gunframe + 2;
        break;

    case Q1W_SHOTGUN:
    case Q1W_SUPERSHOTGUN:
        if (tag == Q1W_SUPERSHOTGUN && ammo >= 2) {
            Q1W_Sound(ent, CHAN_WEAPON, "weapons/shotgn2.wav");
            Q1W_UseAmmo(ent, 2);
            Q1W_FireBullets(ent, 14, 0.14f, 0.08f, MOD_Q1_SSHOTGUN);
        } else {
            // the super shotgun with one shell fires it as the shotgun
            Q1W_Sound(ent, CHAN_WEAPON, "weapons/guncock.wav");
            Q1W_UseAmmo(ent, 1);
            Q1W_FireBullets(ent, 6, 0.04f, 0.04f, MOD_Q1_SHOTGUN);
        }
        client->ps.gunframe = 1;
        client->q1_anim_end = 6;
        Q1W_MuzzleFlash(ent);
        break;

    case Q1W_NAILGUN:
    case Q1W_SUPERNAILGUN:
        // player_nail1 / nail2: frames 1-8 round, a nail each, the nailgun's
        // from 4 units right and left in turn
        client->ps.gunframe = client->ps.gunframe >= 8 || client->ps.gunframe < 1 ? 1 : client->ps.gunframe + 1;
        client->q1_anim_end = 0;
        Q1W_Source(ent, org);
        if (tag == Q1W_SUPERNAILGUN && ammo >= 2) {
            Q1W_Sound(ent, CHAN_WEAPON, "weapons/spike2.wav");
            Q1W_UseAmmo(ent, 2);
            Q1W_LaunchSpike(ent, org, forward, true);
        } else {
            Q1W_Sound(ent, CHAN_WEAPON, "weapons/rocket1i.wav");
            Q1W_UseAmmo(ent, 1);
            VectorMA(org, (client->ps.gunframe & 1) ? 4 : -4, right, org);
            Q1W_LaunchSpike(ent, org, forward, false);
        }
        Q1W_MuzzleFlash(ent);
        break;

    case Q1W_GRENADE:
        Q1W_Sound(ent, CHAN_WEAPON, "weapons/grenade.wav");
        Q1W_UseAmmo(ent, 1);
        Q1W_FireGrenade(ent);
        client->ps.gunframe = 1;
        client->q1_anim_end = 6;
        Q1W_MuzzleFlash(ent);
        break;

    case Q1W_ROCKET:
        Q1W_Sound(ent, CHAN_WEAPON, "weapons/sgun1.wav");
        Q1W_UseAmmo(ent, 1);
        Q1W_FireRocket(ent);
        client->ps.gunframe = 1;
        client->q1_anim_end = 6;
        Q1W_MuzzleFlash(ent);
        break;

    case Q1W_LIGHTNING:
        // player_light1 / light2: frames 1-4 round, a cell each
        if (client->ps.gunframe < 1 || client->ps.gunframe > 4)
            Q1W_Sound(ent, CHAN_AUTO, "weapons/lstart.wav");
        client->ps.gunframe = client->ps.gunframe >= 4 || client->ps.gunframe < 1 ? 1 : client->ps.gunframe + 1;
        client->q1_anim_end = 0;
        Q1W_FireLightning(ent);
        Q1W_MuzzleFlash(ent);
        break;

    default:
        return;
    }

    client->q1_attack_ticks = q1w_info[tag].attack_ticks;
    Q1W_PlayerAttackFrames(ent);
    Q1W_QuadSound(ent, tag == Q1W_NAILGUN || tag == Q1W_SUPERNAILGUN || tag == Q1W_LIGHTNING);
    Q1W_Source(ent, org);
    PlayerNoise(ent, org, PNOISE_WEAPON);
}

/*
=================
Q1W_WeaponChanged

ChangeWeapon has just put a Quake gun in the player's hand: it is ready at
once, as Quake's are.
=================
*/
void Q1W_WeaponChanged(edict_t *ent)
{
    gclient_t *client = ent->client;

    client->weaponstate = WEAPON_READY;
    client->ps.gunframe = 0;
    client->q1_anim_end = 0;
    client->q1_axe_frame = 0;
    client->weapon_sound = 0;
}

// one think of the gun: one of player.qc's frames
static void Q1W_Tick(edict_t *ent)
{
    gclient_t   *client = ent->client;
    gitem_t     *weapon = client->pers.weapon;
    int         tag = weapon->tag;
    bool        attack;

    if (client->q1_attack_ticks > 0)
        client->q1_attack_ticks--;

    // the running animation: a frame on, and the axe's hit on its frame
    if (client->q1_anim_end) {
        if (client->ps.gunframe < client->q1_anim_end) {
            client->ps.gunframe++;
            if (tag == Q1W_AXE && client->ps.gunframe == client->q1_axe_frame) {
                Q1W_FireAxe(ent);
                client->q1_axe_frame = 0;
            }
        } else {
            client->ps.gunframe = 0;
            client->q1_anim_end = 0;
        }
    }

    if (client->q1_attack_ticks > 0)
        return;

    // a change of gun waits for the attack (ImpulseCommands after
    // attack_finished), then is at once
    if (client->newweapon && client->newweapon != weapon) {
        ChangeWeapon(ent);
        return;
    }

    attack = ((client->latched_buttons | client->buttons) & BUTTON_ATTACK) != 0;
    client->latched_buttons &= ~BUTTON_ATTACK;

    if (!attack) {
        // a held nailgun or Thunderbolt stops (player_run)
        if (!client->q1_anim_end)
            client->ps.gunframe = 0;
        client->weaponstate = WEAPON_READY;
        return;
    }

    // W_CheckNoAmmo: out, so the best gun with some, and no shot
    if (tag != Q1W_AXE && Q1W_AmmoInHand(ent) < 1) {
        gitem_t *best = Q1W_BestWeapon(ent);

        if (best && best != weapon) {
            client->newweapon = best;
            ChangeWeapon(ent);
        }
        return;
    }

    client->weaponstate = WEAPON_FIRING;
    Q1W_Attack(ent, tag);
}

/*
=================
Weapon_Q1

Think_Weapon, on the 10 Hz frame: twice under the dualfire or a haste, as
Quake II's guns' frames run twice.
=================
*/
void Weapon_Q1(edict_t *ent)
{
    gitem_t *weapon = ent->client->pers.weapon;

    if (!Q1W_IsGun(weapon) || ent->health < 1)
        return;

    Q1W_Tick(ent);
    if (Weapon_IsQuadfire() && ent->client->pers.weapon == weapon)
        Q1W_Tick(ent);
}

/*
=================
Q1W_KeepsOnDeath

TossClientWeapon: the axe and the shotgun everyone spawns with stay behind.
=================
*/
bool Q1W_KeepsOnDeath(const gitem_t *item)
{
    return item && (item->flags & IT_Q1) &&
           (item->tag == Q1W_AXE || item->tag == Q1W_SHOTGUN);
}

/*
=================
Q1W_Obituary

ClientObituary's message for a Quake gun (client.qc ClientObituary), NULL if
the death wasn't one. `self_kill` asks for the suicide line.
=================
*/
const char *Q1W_Obituary(int mod, bool self_kill, const char **message2, edict_t *self)
{
    *message2 = "";

    if (self_kill) {
        switch (mod) {
        case MOD_Q1_G_SPLASH:
            return "tries to put the pin back in";
        case MOD_Q1_R_SPLASH:
            return "becomes bored with life";
        case MOD_Q1_DISCHARGE:
            return "discharges into the water";
        default:
            return NULL;
        }
    }

    switch (mod) {
    case MOD_Q1_AXE:        return "was ax-murdered by";
    case MOD_Q1_SHOTGUN:    *message2 = "'s boomstick"; return "chewed on";
    case MOD_Q1_SSHOTGUN:   *message2 = "'s buckshot"; return "ate 2 loads of";
    case MOD_Q1_NAIL:       return "was nailed by";
    case MOD_Q1_SNAIL:      return "was punctured by";
    case MOD_Q1_GRENADE:
    case MOD_Q1_G_SPLASH:
        if (self->health < -40) {
            *message2 = "'s grenade";
            return "was gibbed by";
        }
        *message2 = "'s pineapple";
        return "eats";
    case MOD_Q1_ROCKET:
    case MOD_Q1_R_SPLASH:
        *message2 = "'s rocket";
        return self->health < -40 ? "was gibbed by" : "rides";
    case MOD_Q1_LIGHTNING:  *message2 = "'s shaft"; return "accepts";
    case MOD_Q1_DISCHARGE:  *message2 = "'s discharge"; return "accepts";
    default:                return NULL;
    }
}
