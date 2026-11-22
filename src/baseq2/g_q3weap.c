/*
Copyright (C) 1999-2005 Id Software, Inc.

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

QUAKE III ARENA'S WEAPONS

The bot match's "weapons" option, g_q3_weapons, read into level.q3_weapons at
each map start (Q3_LevelRules) and sent to the clients in CS_Q3RULES:

  0  Quake II's guns for everyone - nothing here runs
  1  Quake III's guns for everyone
  2  by player model: whoever plays a Quake III model (models/players/<m>,
     no players/<m>/tris.md2) gets Quake III's guns, everyone else Quake II's

The guns are Quake III's in what they do - bg_pmove.c's PM_Weapon timing,
run on every usercmd in milliseconds rather than on the 10 Hz frame, and
g_weapon.c / g_missile.c's damage, spread, splash and projectile speeds - and
in how they sound. How they look is the client's: the view weapon is put
together from the Quake III hand, gun, barrel and flash models; rockets,
grenades, the rail and the BFG use the Flow effects Quake II's do; the rest
draw Quake III's own sprites and models.

The guns share Quake II's ammo: bullets, shells, grenades, rockets, slugs and
cells come in boxes of the same size in both games, and the boxes are always
Quake II's models. Only the Lightning Gun and the BFG10K have ammo of their
own (Quake III's maps place it; on Quake II's maps a bullets box also carries
lightning and a cells box BFG ammo, since the guns those maps stocked them for
are the ones the Lightning Gun and BFG10K stand in for).

On Quake II's maps each Quake II gun becomes its Quake III counterpart (the
chaingun the Lightning Gun, the hyperblaster the Plasma Gun, both shotguns the
Shotgun); on Quake III's maps its own guns are placed. With guns by player
model, a pickup carries both and each player takes their own game's - and
each player SEES their own game's model (RF_Q3_ITEM, see the client).

==============================================================================
*/

#include "g_local.h"

// one Quake III gun, by gitem_t->tag (Q3W_*)
typedef struct {
    const char  *classname;
    int         fire_time;      // ms between shots (bg_pmove.c PM_Weapon)
    int         pickup_ammo;    // bg_misc.c's quantity: ammo with the gun
    int         flash;          // MZ_Q3_*
    const char  *ready_sound;   // looped while held
    const char  *firing_sound;  // looped while the trigger is down
    const char  *flash_sound;   // each shot
} q3w_info_t;

static const q3w_info_t q3w_info[Q3W_NUM] = {
    [Q3W_GAUNTLET]  = { "weapon_q3_gauntlet",         400,   0, MZ_Q3_GAUNTLET,
                        NULL, "weapons/melee/fstrun.wav", "weapons/melee/fstatck.wav" },
    [Q3W_MACHINEGUN]= { "weapon_q3_machinegun",       100,  40, MZ_Q3_MACHINEGUN,
                        NULL, NULL, NULL },     // one of four, Q3W_Fire
    [Q3W_SHOTGUN]   = { "weapon_q3_shotgun",         1000,  10, MZ_Q3_SHOTGUN,
                        NULL, NULL, "weapons/shotgun/sshotf1b.wav" },
    [Q3W_GRENADE]   = { "weapon_q3_grenadelauncher",  800,  10, MZ_Q3_GRENADE,
                        NULL, NULL, "weapons/grenade/grenlf1a.wav" },
    [Q3W_ROCKET]    = { "weapon_q3_rocketlauncher",   800,  10, MZ_Q3_ROCKET,
                        NULL, NULL, "weapons/rocket/rocklf1a.wav" },
    [Q3W_LIGHTNING] = { "weapon_q3_lightning",         50, 100, MZ_Q3_LIGHTNING,
                        "weapons/melee/fsthum.wav", "weapons/lightning/lg_hum.wav", "weapons/lightning/lg_fire.wav" },
    [Q3W_RAILGUN]   = { "weapon_q3_railgun",         1500,  10, MZ_Q3_RAILGUN,
                        "weapons/railgun/rg_hum.wav", NULL, "weapons/railgun/railgf1a.wav" },
    [Q3W_PLASMA]    = { "weapon_q3_plasmagun",        100,  50, MZ_Q3_PLASMAGUN,
                        NULL, NULL, "weapons/plasma/hyprbf1a.wav" },
    [Q3W_BFG]       = { "weapon_q3_bfg",              200,  20, MZ_Q3_BFG,
                        "weapons/bfg/bfg_hum.wav", NULL, "weapons/bfg/bfg_fire.wav" },
};

// Quake III's numbers (g_weapon.c, g_missile.c)
#define Q3_MACHINEGUN_SPREAD    200
#define Q3_MACHINEGUN_DAMAGE    7
#define Q3_SHOTGUN_SPREAD       700
#define Q3_SHOTGUN_COUNT        11
#define Q3_SHOTGUN_DAMAGE       10
#define Q3_LIGHTNING_RANGE      768
#define Q3_LIGHTNING_DAMAGE     8
#define Q3_RAIL_DAMAGE          100
#define Q3_RAIL_HITS            4
#define Q3_GAUNTLET_RANGE       32
#define Q3_GAUNTLET_DAMAGE      50
#define Q3_MAX_AMMO             200
#define Q3_QUAD_FACTOR          3       // g_quadfactor
#define Q3_WEAPON_RESPAWN       5       // g_weaponrespawn
#define Q3_AMMO_RESPAWN         40
#define Q3_MISSILE_PRESTEP      0.05f   // MISSILE_PRESTEP_TIME, seconds

// The other game's gun on a Quake II map (or a Quake II gun dropped in a match
// by player model): Quake II's on the left, Quake III's on the right. A Quake
// III gun's Quake II counterpart is the first row naming it.
static const struct {
    const char  *q2, *q3;
} q3w_swap[] = {
    { "weapon_supershotgun",    "weapon_q3_shotgun" },
    { "weapon_shotgun",         "weapon_q3_shotgun" },
    { "weapon_machinegun",      "weapon_q3_machinegun" },
    { "weapon_chaingun",        "weapon_q3_lightning" },
    { "weapon_grenadelauncher", "weapon_q3_grenadelauncher" },
    { "weapon_rocketlauncher",  "weapon_q3_rocketlauncher" },
    { "weapon_hyperblaster",    "weapon_q3_plasmagun" },
    { "weapon_railgun",         "weapon_q3_railgun" },
    { "weapon_bfg",             "weapon_q3_bfg" },
    { "weapon_blaster",         "weapon_q3_gauntlet" },
    // the mission packs' guns, on the rerelease's deathmatch maps
    { "weapon_chainfist",       "weapon_q3_gauntlet" },
    { "weapon_etf_rifle",       "weapon_q3_machinegun" },
    { "weapon_proxlauncher",    "weapon_q3_grenadelauncher" },
    { "weapon_phalanx",         "weapon_q3_rocketlauncher" },
    { "weapon_boomer",          "weapon_q3_plasmagun" },
    { "weapon_plasmabeam",      "weapon_q3_lightning" },
    { "weapon_disintegrator",   "weapon_q3_railgun" },
};

// Quake III's map classnames, as its guns and their own ammo; the Quake II
// item each would be otherwise is g_q3.c's table (q3_renames)
static const struct {
    const char  *map, *item;
} q3w_mapnames[] = {
    { "weapon_gauntlet",        "weapon_q3_gauntlet" },
    { "weapon_machinegun",      "weapon_q3_machinegun" },
    { "weapon_shotgun",         "weapon_q3_shotgun" },
    { "weapon_grenadelauncher", "weapon_q3_grenadelauncher" },
    { "weapon_rocketlauncher",  "weapon_q3_rocketlauncher" },
    { "weapon_lightning",       "weapon_q3_lightning" },
    { "weapon_railgun",         "weapon_q3_railgun" },
    { "weapon_plasmagun",       "weapon_q3_plasmagun" },
    { "weapon_bfg",             "weapon_q3_bfg" },
    { "ammo_lightning",         "ammo_q3_lightning" },
    { "ammo_bfg",               "ammo_q3_bfg" },
};

/*
==============================================================================

WHOSE GUNS

==============================================================================
*/

/*
=================
Q3W_ModelIsQ3

A Quake III player model: models/players/<model>/animation.cfg, and no Quake
II model of the name - the client's own test (q3player.c). Remembered, since
it is asked on every pickup.
=================
*/
#define Q3W_MODEL_CACHE 32

static bool Q3W_ModelIsQ3(const char *model)
{
    static struct {
        char    name[MAX_QPATH];
        bool    q3;
    } cache[Q3W_MODEL_CACHE];
    static int  next;
    bool        q3;
    int         i;

    if (!model || !*model || !gi.LoadFile)
        return false;

    for (i = 0; i < Q3W_MODEL_CACHE; i++)
        if (cache[i].name[0] && !Q_stricmp(cache[i].name, model))
            return cache[i].q3;

    q3 = gi.LoadFile(va("players/%s/tris.md2", model), NULL) < 0 &&
         gi.LoadFile(va("models/players/%s/animation.cfg", model), NULL) >= 0;

    Q_strlcpy(cache[next].name, model, sizeof(cache[next].name));
    cache[next].q3 = q3;
    next = (next + 1) % Q3W_MODEL_CACHE;
    return q3;
}

// the model half of a "model/skin" userinfo skin
static bool Q3W_SkinIsQ3(const char *skin)
{
    char        model[MAX_QPATH];
    const char  *slash = skin ? strchr(skin, '/') : NULL;
    size_t      len = slash ? (size_t)(slash - skin) : 0;

    if (!len || len >= sizeof(model))
        return false;
    memcpy(model, skin, len);
    model[len] = 0;
    return Q3W_ModelIsQ3(model);
}

/*
=================
Q3W_ClientQ3 / Q3W_PlayerQ3

Whether this player plays with Quake III's guns this level.
=================
*/
bool Q3W_ClientQ3(const gclient_t *client)
{
    if (!level.q3_weapons || !client)
        return false;
    if (level.q3_weapons == 1)
        return true;
    return Q3W_SkinIsQ3(Info_ValueForKey((char *)client->pers.userinfo, "skin"));
}

bool Q3W_PlayerQ3(const edict_t *ent)
{
    return ent && ent->client && Q3W_ClientQ3(ent->client);
}

static gitem_t *Q3W_Item(const char *classname)
{
    return FindItemByClassname((char *)classname);
}

/*
=================
Q3W_Counterpart

The other game's version of a gun, NULL if it has none.
=================
*/
gitem_t *Q3W_Counterpart(const gitem_t *item)
{
    int     i;

    if (!item || !item->classname || !(item->flags & IT_WEAPON))
        return NULL;

    for (i = 0; i < q_countof(q3w_swap); i++) {
        if (item->flags & IT_Q3) {
            if (!strcmp(item->classname, q3w_swap[i].q3))
                return Q3W_Item(q3w_swap[i].q2);
        } else {
            if (!strcmp(item->classname, q3w_swap[i].q2))
                return Q3W_Item(q3w_swap[i].q3);
        }
    }
    return NULL;
}

/*
=================
Q3W_FindItem

FindItem for this player: Quake III's guns share several names with Quake
II's ("Rocket Launcher", "Railgun"...), and FindItem takes the first. A
player of either game also gets their own gun for the other's name, so the
stock "use Chaingun" or "use HyperBlaster" binds pick the Lightning Gun or
the Plasma Gun.
=================
*/
gitem_t *Q3W_FindItem(edict_t *ent, const char *name)
{
    gitem_t     *it, *found = NULL, *other;
    bool        q3 = Q3W_PlayerQ3(ent);
    int         i;

    if (Q1W_PlayerQ1(ent))
        return Q1W_FindItem(name);     // Quake's guns (g_q1weap.c)
    if (!level.q3_weapons)
        return FindItem((char *)name);

    for (i = 0, it = itemlist; i < game.num_items; i++, it++) {
        if (!it->pickup_name || Q_stricmp(it->pickup_name, name))
            continue;
        if (!(it->flags & IT_WEAPON))
            return it;      // not a gun: the name is the item's
        if (!!(it->flags & IT_Q3) == q3)
            return it;      // this player's own
        if (!found)
            found = it;
    }

    // the other game's gun: the one standing in for it
    other = Q3W_Counterpart(found);
    if (other && ent && ent->client && ent->client->pers.inventory[ITEM_INDEX(other)])
        return other;
    return found;
}

/*
=================
Q3W_ItemFor

What a player takes from an item on the ground: in a match by player model,
a gun of the other game is its counterpart.
=================
*/
gitem_t *Q3W_ItemFor(const edict_t *item_ent, const edict_t *player)
{
    gitem_t *item = item_ent->item;

    // Quake's Ranger, or a Quake gun one dropped (g_q1weap.c)
    if (level.q1_by_model && item && (Q1W_PlayerQ1(player) || (item->flags & IT_Q1)))
        return Q1W_ItemFor(item, player);

    if (item_ent->q3_alt_item && item &&
        !!(item->flags & IT_Q3) != Q3W_PlayerQ3(player))
        return item_ent->q3_alt_item;
    return item;
}

/*
==============================================================================

ITEMS ON THE MAP

==============================================================================
*/

/*
=================
Q3W_SpawnClassname

ED_CallSpawn, for every entity of a Quake II map: which item a Quake II gun
spawns as. With Quake III's guns for everyone, its counterpart; by player
model, itself, with the counterpart kept for Quake III's players.
=================
*/
const char *Q3W_SpawnClassname(edict_t *ent, const char *classname)
{
    int     i;

    if (!level.q3_weapons || level.q3map)
        return classname;

    for (i = 0; i < q_countof(q3w_swap); i++) {
        if (strcmp(classname, q3w_swap[i].q2))
            continue;
        if (level.q3_weapons == 1)
            return q3w_swap[i].q3;
        ent->q3_alt_item = Q3W_Item(q3w_swap[i].q3);
        return classname;
    }
    return classname;
}

/*
=================
Q3W_MapClassname

Q3_SpawnEntity, before its own renames: a Quake III map's gun or Lightning
Gun / BFG ammo as Quake III's, with the Quake II item it would otherwise be
(`q2_name`) kept for Quake II's players in a match by player model.
=================
*/
const char *Q3W_MapClassname(edict_t *ent, const char *classname, const char *q2_name)
{
    int     i;

    if (!level.q3_weapons)
        return classname;

    for (i = 0; i < q_countof(q3w_mapnames); i++) {
        if (Q_stricmp(classname, q3w_mapnames[i].map))
            continue;
        if (level.q3_weapons == 2 && q2_name)
            ent->q3_alt_item = Q3W_Item(q2_name);
        return q3w_mapnames[i].item;
    }
    return classname;
}

/*
=================
Q3W_ItemModel

droptofloor and Drop_Item, once the item has its model: a Quake III gun is
drawn half as big again (cg_ents.c CG_Item). A pickup carrying both games'
guns sends both models, Quake II's in modelindex and Quake III's in
modelindex2, and RF_Q3_ITEM has each client draw its own player's.
=================
*/
void Q3W_ItemModel(edict_t *ent)
{
    gitem_t *q2 = NULL, *q3 = NULL;

    if (!level.q3_weapons || !ent->item)
        return;

    if (ent->item->flags & IT_Q3)
        q3 = ent->item;
    else
        q2 = ent->item;
    if (ent->q3_alt_item) {
        if (ent->q3_alt_item->flags & IT_Q3)
            q3 = ent->q3_alt_item;
        else
            q2 = ent->q3_alt_item;
    }

    if (!q3)
        return;     // Quake II's gun or ammo

    if (!(q3->flags & IT_WEAPON)) {
        // Quake III's ammo, which wears Quake II's models but for the BFG's
        // (g_items.c). Where they differ and a Quake II item rides along,
        // each player sees their own game's box, as for the guns: s.scale 1
        // keeps the client from drawing Quake III's half as big again, which
        // Quake III does to its guns alone
        if (q2 && q3->world_model && q2->world_model && strcmp(q3->world_model, q2->world_model)) {
            ent->s.modelindex = gi.modelindex(q2->world_model);
            ent->s.modelindex2 = gi.modelindex(q3->world_model);
            ent->s.renderfx |= RF_Q3_ITEM;
            ent->s.scale = 1.0f;
        }
        return;
    }

    if (!q2) {
        ent->s.scale = 1.5f;
        return;
    }

    ent->s.modelindex = gi.modelindex(q2->world_model);
    ent->s.modelindex2 = gi.modelindex(q3->world_model);
    ent->s.renderfx |= RF_Q3_ITEM;
}

/*
=================
Q3W_DroppedItem

Drop_Item: a gun dropped in a match by player model carries its counterpart,
so either game's player picks up their own. Q3W_ItemModel follows.
=================
*/
void Q3W_DroppedItem(edict_t *dropped)
{
    if (level.q3_weapons != 2 || !dropped->item)
        return;
    dropped->q3_alt_item = Q3W_Counterpart(dropped->item);
}

/*
=================
Pickup_Q3Weapon

Quake III's rule (g_items.c Pickup_Weapon): a gun from the map tops the ammo
up to its own amount, or adds one shot if there was already that much; a
dropped one gives its whole amount. Back in five seconds.
=================
*/
bool Pickup_Q3Weapon(edict_t *ent, edict_t *other)
{
    gclient_t   *client = other->client;
    gitem_t     *item = ent->item;
    gitem_t     *ammo = item->ammo ? FindItem(item->ammo) : NULL;
    int         index = ITEM_INDEX(item);
    bool        had = client->pers.inventory[index] > 0;
    int         quantity, have;

    if (((int)dmflags->value & DF_WEAPONS_STAY) && had &&
        !(ent->spawnflags & (DROPPED_ITEM | DROPPED_PLAYER_ITEM)))
        return false;   // leave the weapon for others to pickup

    client->pers.inventory[index] = 1;

    if (ammo) {
        quantity = item->tag > 0 && item->tag < Q3W_NUM ? q3w_info[item->tag].pickup_ammo : 0;
        if (ent->count)
            quantity = ent->count;
        if (!(ent->spawnflags & (DROPPED_ITEM | DROPPED_PLAYER_ITEM))) {
            have = client->pers.inventory[ITEM_INDEX(ammo)];
            quantity = have < quantity ? quantity - have : 1;
        }
        if ((int)dmflags->value & DF_INFINITE_AMMO)
            quantity = 1000;
        Add_Ammo(other, ammo, quantity);
    }

    if (!(ent->spawnflags & (DROPPED_ITEM | DROPPED_PLAYER_ITEM)) && deathmatch->value) {
        if ((int)dmflags->value & DF_WEAPONS_STAY)
            ent->flags |= FL_RESPAWN;
        else
            SetRespawn(ent, Q3_WEAPON_RESPAWN);
    }

    // switch to a gun new to us (Quake III's cg_autoswitch), bar the
    // machinegun everyone already has
    if (!had && client->pers.weapon != item && item->tag != Q3W_MACHINEGUN)
        client->newweapon = item;

    return true;
}

/*
=================
Q3W_PickupAmmo

Pickup_Ammo, for a player with Quake III's guns: Quake III's amounts (a cells
box holds 30, not 50) and respawn, and on Quake II's maps the Lightning Gun's
and BFG10K's ammo out of the bullets and cells boxes. False to leave the box.
=================
*/
bool Q3W_PickupAmmo(edict_t *ent, edict_t *other)
{
    gitem_t *item = ent->item;
    gitem_t *extra = NULL;
    int     count, extra_count = 0;
    bool    took;

    if (ent->count)
        count = ent->count;
    else if (item->tag == AMMO_CELLS)
        count = 30;
    else
        count = item->quantity;

    if (!level.q3map) {
        if (item->tag == AMMO_BULLETS) {
            extra = Q3W_Item("ammo_q3_lightning");
            extra_count = 60;
        } else if (item->tag == AMMO_CELLS) {
            extra = Q3W_Item("ammo_q3_bfg");
            extra_count = 15;
        }
    }

    took = Add_Ammo(other, item, count);
    if (extra && Add_Ammo(other, extra, extra_count))
        took = true;
    if (!took)
        return false;

    if (!(ent->spawnflags & (DROPPED_ITEM | DROPPED_PLAYER_ITEM)) && deathmatch->value)
        SetRespawn(ent, Q3_AMMO_RESPAWN);
    return true;
}

/*
=================
Q3W_GiveSpawnKit

InitClientPersistant, for a player with Quake III's guns: Quake III's spawn -
the Gauntlet and the Machinegun with 100 bullets, no blaster - and its ammo
limit of 200 for everything.
=================
*/
void Q3W_GiveSpawnKit(gclient_t *client)
{
    gitem_t *gauntlet = Q3W_Item("weapon_q3_gauntlet");
    gitem_t *mg = Q3W_Item("weapon_q3_machinegun");
    gitem_t *bullets = Q3W_Item("ammo_bullets");
    gitem_t *blaster = Q3W_Item("weapon_blaster");

    if (!gauntlet || !mg || !bullets)
        return;

    if (blaster)
        client->pers.inventory[ITEM_INDEX(blaster)] = 0;
    client->pers.inventory[ITEM_INDEX(gauntlet)] = 1;
    client->pers.inventory[ITEM_INDEX(mg)] = 1;
    client->pers.inventory[ITEM_INDEX(bullets)] = 100;
    client->pers.weapon = mg;
    client->pers.selected_item = ITEM_INDEX(mg);

    client->pers.max_bullets = Q3_MAX_AMMO;
    client->pers.max_shells = Q3_MAX_AMMO;
    client->pers.max_rockets = Q3_MAX_AMMO;
    client->pers.max_grenades = Q3_MAX_AMMO;
    client->pers.max_cells = Q3_MAX_AMMO;
    client->pers.max_slugs = Q3_MAX_AMMO;
}

/*
=================
Q3W_Precache

SP_worldspawn, in a Quake III weapons game: every gun and what it shoots
(a spawn kit or a dropped gun needs them without one on the map), and the
third-person models after the grapple's slot (WEAP_Q3_*).
=================
*/
void Q3W_Precache(void)
{
    static const char *const sounds[] = {
        "weapons/change.wav", "weapons/noammo.wav",
        "weapons/machinegun/machgf1b.wav", "weapons/machinegun/machgf2b.wav",
        "weapons/machinegun/machgf3b.wav", "weapons/machinegun/machgf4b.wav",
        "weapons/rocket/rockfly.wav", "weapons/plasma/lasfly.wav",
        "weapons/grenade/hgrenb1a.wav", "weapons/grenade/hgrenb2a.wav",
        "items/damage3.wav",
    };
    int     i;

    if (!level.q3_weapons)
        return;

    for (i = 1; i < Q3W_NUM; i++) {
        const q3w_info_t *w = &q3w_info[i];

        PrecacheItem(Q3W_Item(w->classname));
        if (w->ready_sound)
            Q3_SoundIndex(w->ready_sound);
        if (w->firing_sound)
            Q3_SoundIndex(w->firing_sound);
        if (w->flash_sound)
            Q3_SoundIndex(w->flash_sound);
    }
    for (i = 0; i < q_countof(sounds); i++)
        Q3_SoundIndex(sounds[i]);
    PrecacheItem(Q3W_Item("ammo_q3_lightning"));
    PrecacheItem(Q3W_Item("ammo_q3_bfg"));

    gi.modelindex("models/ammo/rocket/rocket.md3");
    gi.modelindex("models/ammo/grenade1.md3");
    gi.modelindex("sprites/s_bfg1.sp2");

    // WEAP_GRAPPLE (20) is CTF_Precache's; hold it when there is no CTF so
    // Quake III's guns land on 21..29 either way
    if (!G_CTF())
        gi.modelindex("#w_grapple.md2");
    for (i = 1; i < Q3W_NUM; i++)
        gi.modelindex(va("#%s", Q3W_Item(q3w_info[i].classname)->world_model));
}

/*
==============================================================================

FIRING

==============================================================================
*/

static int Q3W_DamageScale(edict_t *ent)
{
    int scale = 1;

    if (ent->client->quad_framenum > level.framenum)
        scale *= Q3_QUAD_FACTOR;
    if (ent->client->double_framenum > level.framenum)
        scale *= 2;
    return scale;
}

/*
=================
Q3W_RadiusDamage

g_combat.c G_RadiusDamage: the damage falls off with the distance to the
nearest point of the target's box, not its centre, and the push is aimed 24
units above the blast so it lifts. Your own share is halved here only with
Quake II's physics; Quake III's halves it in T_Damage, after the push.
=================
*/
// true when it reached someone, for the campaign's accuracy (G_RadiusDamage's hitClient)
static bool Q3W_RadiusDamage(edict_t *inflictor, edict_t *attacker, float damage,
                             float radius, edict_t *ignore, int mod)
{
    edict_t *ent = NULL;
    vec3_t  v, dir;
    float   dist, points;
    int     i;
    bool    hit_client = false;

    if (radius < 1)
        radius = 1;

    // findradius measures to the centre: look far enough for a box's edge
    while ((ent = findradius(ent, inflictor->s.origin, radius + 48)) != NULL) {
        if (ent == ignore || !ent->takedamage)
            continue;

        for (i = 0; i < 3; i++) {
            if (inflictor->s.origin[i] < ent->absmin[i])
                v[i] = ent->absmin[i] - inflictor->s.origin[i];
            else if (inflictor->s.origin[i] > ent->absmax[i])
                v[i] = inflictor->s.origin[i] - ent->absmax[i];
            else
                v[i] = 0;
        }
        dist = VectorLength(v);
        if (dist >= radius)
            continue;

        points = damage * (1.0f - dist / radius);
        if (ent == attacker && !level.q3_physics)
            points *= 0.5f;
        if (points < 1 || !CanDamage(ent, inflictor))
            continue;

        VectorSubtract(ent->s.origin, inflictor->s.origin, dir);
        dir[2] += 24;
        if (SP_AccuracyTarget(attacker, ent))
            hit_client = true;
        T_Damage(ent, inflictor, attacker, dir, inflictor->s.origin, vec3_origin,
                 (int)points, (int)points, DAMAGE_RADIUS, mod);
    }
    return hit_client;
}

static void Q3W_TempEntity(int type, const vec3_t pos, const vec3_t dir)
{
    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(type);
    gi.WritePosition(pos);
    if (dir)
        gi.WriteDir(dir);
    gi.multicast((vec_t *)pos, MULTICAST_PVS);
}

/*
=================
Q3W_Bullet

g_weapon.c Bullet_Fire: a round spread, seen at 8192 * 16 units.
=================
*/
// true when it hit someone, for the campaign's accuracy (g_sp.c)
static bool Q3W_Bullet(edict_t *ent, const vec3_t muzzle, const vec3_t forward,
                       const vec3_t right, const vec3_t up, float r, float u,
                       int damage, int mod, int impact)
{
    vec3_t  dir, end;
    trace_t tr;
    bool    hit;

    VectorScale(forward, 8192 * 16, dir);
    VectorMA(dir, r, right, dir);
    VectorMA(dir, u, up, dir);
    VectorNormalize(dir);
    VectorMA(muzzle, 8192, dir, end);

    tr = gi.trace((vec_t *)muzzle, NULL, NULL, end, ent, MASK_SHOT);
    if (tr.fraction == 1.0f || (tr.surface && (tr.surface->flags & SURF_SKY)))
        return false;

    if (tr.ent && tr.ent->takedamage) {
        hit = SP_AccuracyTarget(ent, tr.ent);
        T_Damage(tr.ent, ent, ent, dir, tr.endpos, tr.plane.normal, damage, damage, DAMAGE_BULLET, mod);
        return hit;
    }

    Q3W_TempEntity(impact, tr.endpos, tr.plane.normal);
    return false;
}

/*
=================
Q3W_GauntletHit

g_weapon.c CheckGauntletAttack: the gauntlet only "fires" when there is
something to hit, 32 units in front of the muzzle.
=================
*/
static bool Q3W_GauntletHit(edict_t *ent, const vec3_t muzzle, const vec3_t forward)
{
    vec3_t  end;
    trace_t tr;
    int     damage;

    VectorMA(muzzle, Q3_GAUNTLET_RANGE, forward, end);
    tr = gi.trace((vec_t *)muzzle, NULL, NULL, end, ent, MASK_SHOT);
    if (tr.fraction == 1.0f || !tr.ent || !tr.ent->takedamage)
        return false;
    if (tr.surface && (tr.surface->flags & SURF_SKY))
        return false;

    damage = Q3_GAUNTLET_DAMAGE * Q3W_DamageScale(ent);
    T_Damage(tr.ent, ent, ent, (vec_t *)forward, tr.endpos, tr.plane.normal, damage, damage, 0, MOD_Q3_GAUNTLET);
    return true;
}

/*
=================
Q3W_Rail

g_weapon.c weapon_railgun_fire: through up to four players, stopped by the
first solid thing. The trail starts a little to the right of the muzzle, as
Quake III's does, nearer the drawn gun.
=================
*/
static void Q3W_Rail(edict_t *ent, const vec3_t muzzle, const vec3_t forward,
                     const vec3_t right, const vec3_t up)
{
    vec3_t  from, end, start;
    edict_t *ignore = ent;
    trace_t tr;
    int     damage = Q3_RAIL_DAMAGE * Q3W_DamageScale(ent);
    int     hits = 0;
    int     players_hit = 0;    // toward "impressive" (g_sp.c)

    VectorCopy(muzzle, from);
    VectorMA(muzzle, 8192, forward, end);

    while (1) {
        tr = gi.trace(from, NULL, NULL, end, ignore, MASK_SHOT);
        if (tr.fraction == 1.0f || !tr.ent)
            break;
        if (tr.ent != ent && tr.ent->takedamage) {
            if (SP_AccuracyTarget(ent, tr.ent))
                players_hit++;
            T_Damage(tr.ent, ent, ent, (vec_t *)forward, tr.endpos, tr.plane.normal, damage, damage, 0, MOD_Q3_RAILGUN);
        }
        // through players and bodies, not walls
        if (!(tr.ent->client || (tr.ent->svflags & SVF_MONSTER) || tr.ent->solid == SOLID_BBOX))
            break;
        if (++hits >= Q3_RAIL_HITS)
            break;
        ignore = tr.ent;
        VectorCopy(tr.endpos, from);
    }

    SP_RailHits(ent, players_hit);

    VectorMA(muzzle, 4, right, start);
    VectorMA(start, -1, up, start);

    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_Q3_RAILTRAIL);
    gi.WritePosition(start);
    gi.WritePosition(tr.endpos);
    gi.multicast(ent->s.origin, MULTICAST_PHS);
}

/*
=================
Q3W_Lightning

g_weapon.c Weapon_LightningFire: 768 units, hitscan. The beam itself goes
out once a frame (it fires every 50 ms) and the client draws it from the gun
for a frame.
=================
*/
static bool Q3W_Lightning(edict_t *ent, const vec3_t muzzle, const vec3_t forward)
{
    vec3_t  end;
    trace_t tr;
    bool    hit;
    int     damage = Q3_LIGHTNING_DAMAGE * Q3W_DamageScale(ent);

    VectorMA(muzzle, Q3_LIGHTNING_RANGE, forward, end);
    tr = gi.trace((vec_t *)muzzle, NULL, NULL, end, ent, MASK_SHOT);

    if (tr.fraction < 1.0f && tr.ent && tr.ent->takedamage) {
        if (SP_AccuracyTarget(ent, tr.ent))
            SP_AccuracyHit(ent);
        T_Damage(tr.ent, ent, ent, (vec_t *)forward, tr.endpos, tr.plane.normal, damage, damage, 0, MOD_Q3_LIGHTNING);
    }

    if (ent->client->q3_lightning_framenum == level.framenum)
        return false;
    ent->client->q3_lightning_framenum = level.framenum;

    hit = tr.fraction < 1.0f && !(tr.surface && (tr.surface->flags & SURF_SKY));

    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_Q3_LIGHTNING);
    gi.WriteShort(ent - g_edicts);
    gi.WriteShort(hit ? 1 : 0);
    gi.WritePosition((vec_t *)muzzle);
    gi.WritePosition(tr.endpos);
    gi.multicast(ent->s.origin, MULTICAST_PVS);
    return true;
}

/*
==============================================================================

PROJECTILES

The rocket, grenade, plasma ball and BFG ball. `style` is the gun (Q3W_*),
`dmg` the direct hit, `radius_dmg` and `dmg_radius` the splash.

==============================================================================
*/

static int Q3W_MissileMod(const edict_t *self, bool splash)
{
    switch (self->style) {
    case Q3W_GRENADE:   return splash ? MOD_Q3_G_SPLASH : MOD_Q3_GRENADE;
    case Q3W_ROCKET:    return splash ? MOD_Q3_R_SPLASH : MOD_Q3_ROCKET;
    case Q3W_PLASMA:    return splash ? MOD_Q3_P_SPLASH : MOD_Q3_PLASMA;
    default:            return splash ? MOD_Q3_BFG_SPLASH : MOD_Q3_BFG;
    }
}

static void Q3W_MissileExplode(edict_t *self, edict_t *other, cplane_t *plane)
{
    vec3_t  origin, heading;
    float   back;
    int     te;
    bool    hit;

    // The blast a little back from the surface, as Quake II's rockets do: 1/50
    // of a second of flight, but no more than a Quake III rocket's 18 units.
    // The plasma and the BFG fly at 2000, and 40 units back put the plasma's
    // ring in the air, its 20-unit splash out of reach of anyone at the wall,
    // and its burn nowhere - the client looks for the wall 8 units behind the
    // point (q3weapons.c q3w_mark), and for a rocket's or the BFG's scorch 32
    // (CL_Q3W_Explosion). Matt saw no plasma scorches (11-18). The plasma's
    // message carries the surface, so it goes off on it, 1 unit out, where
    // Quake III's does.
    if (self->style == Q3W_PLASMA && plane) {
        VectorMA(self->s.origin, 1, plane->normal, origin);
    } else {
        back = 0.02f * VectorNormalize2(self->velocity, heading);
        VectorMA(self->s.origin, -min(back, 18.0f), heading, origin);
    }
    VectorCopy(origin, self->s.origin);

    // the campaign's accuracy: a hit, direct or splash, counts once (G_MissileImpact)
    hit = false;
    if (other && other->takedamage) {
        hit = SP_AccuracyTarget(self->owner, other);
        T_Damage(other, self, self->owner, self->velocity, self->s.origin,
                 plane ? plane->normal : vec3_origin, self->dmg, self->dmg, 0, Q3W_MissileMod(self, false));
    }

    if (self->radius_dmg &&
        Q3W_RadiusDamage(self, self->owner, self->radius_dmg, self->dmg_radius, other, Q3W_MissileMod(self, true)))
        hit = true;
    if (hit)
        SP_AccuracyHit(self->owner);

    switch (self->style) {
    case Q3W_GRENADE:   te = TE_Q3_GRENADE_EXPLOSION; break;
    case Q3W_ROCKET:    te = TE_Q3_ROCKET_EXPLOSION; break;
    case Q3W_PLASMA:    te = TE_Q3_PLASMA_EXPLOSION; break;
    default:            te = TE_Q3_BFG_EXPLOSION; break;
    }
    if (te == TE_Q3_PLASMA_EXPLOSION) {
        vec3_t dir;

        if (plane) {
            VectorCopy(plane->normal, dir);
        } else {
            VectorNormalize2(self->velocity, dir);
            VectorInverse(dir);
        }
        Q3W_TempEntity(te, origin, dir);
    } else {
        gi.WriteByte(svc_temp_entity);
        gi.WriteByte(te);
        gi.WritePosition(origin);
        gi.multicast(origin, MULTICAST_PHS);
    }

    G_FreeEdict(self);
}

static void q3_missile_think(edict_t *self)
{
    Q3W_MissileExplode(self, NULL, NULL);
}

static void q3_missile_touch(edict_t *self, edict_t *other, cplane_t *plane, csurface_t *surf)
{
    if (other == self->owner)
        return;

    if (surf && (surf->flags & SURF_SKY)) {
        G_FreeEdict(self);
        return;
    }

    // a grenade bounces off anything it can't hurt (EF_BOUNCE_HALF)
    if (self->style == Q3W_GRENADE && !other->takedamage) {
        gi.sound(self, CHAN_VOICE, Q3_SoundIndex((Q_rand() & 1) ? "weapons/grenade/hgrenb1a.wav" :
                                                 "weapons/grenade/hgrenb2a.wav"), 1, ATTN_NORM, 0);
        return;
    }

    Q3W_MissileExplode(self, other, plane);
}

/*
=================
Q3W_Missile

The missile and its first 50 ms in one go (MISSILE_PRESTEP_TIME): Quake III
starts its missiles that far along, which a point-blank shot hits through.
=================
*/
static void Q3W_Missile(edict_t *ent, int weapon, const vec3_t muzzle, const vec3_t dir, int late_ms)
{
    edict_t *m;
    vec3_t  end;
    trace_t tr;
    int     scale = Q3W_DamageScale(ent);
    float   speed;

    m = G_Spawn();
    m->classname = "q3_missile";
    m->style = weapon;
    m->owner = ent;
    m->movetype = MOVETYPE_FLYMISSILE;
    m->clipmask = MASK_SHOT;
    m->solid = SOLID_BBOX;
    VectorClear(m->mins);
    VectorClear(m->maxs);
    m->touch = q3_missile_touch;
    m->think = q3_missile_think;

    switch (weapon) {
    case Q3W_GRENADE:
        speed = 700;
        m->movetype = MOVETYPE_BOUNCE;
        m->dmg = 100 * scale;
        m->radius_dmg = 100 * scale;
        m->dmg_radius = 150;
        m->nextthink = level.framenum + 25 * BASE_FRAMERATE / 10;
        m->s.modelindex = gi.modelindex("models/ammo/grenade1.md3");
        m->s.effects = EF_GRENADE;
        VectorSet(m->avelocity, 300, 300, 300);
        break;
    case Q3W_ROCKET:
        speed = 900;
        m->dmg = 100 * scale;
        m->radius_dmg = 100 * scale;
        m->dmg_radius = 120;
        m->nextthink = level.framenum + 15 * BASE_FRAMERATE;
        m->s.modelindex = gi.modelindex("models/ammo/rocket/rocket.md3");
        m->s.effects = EF_ROCKET;
        m->s.sound = Q3_SoundIndex("weapons/rocket/rockfly.wav");
        break;
    case Q3W_PLASMA:
        speed = 2000;
        m->dmg = 20 * scale;
        m->radius_dmg = 15 * scale;
        m->dmg_radius = 20;
        m->nextthink = level.framenum + 10 * BASE_FRAMERATE;
        // no model: the client draws Quake III's plasma sprite
        m->s.renderfx = RF_Q3_SPRITE;
        m->s.frame = Q3_SPRITE_PLASMA;
        // its shooter: the client draws a new ball from that gun's muzzle,
        // not 100 units out where the prestep below puts it (CL_Q3W_AddSprite)
        m->s.skinnum = ent->s.number;
        m->s.sound = Q3_SoundIndex("weapons/plasma/lasfly.wav");
        break;
    default:    // Q3W_BFG
        speed = 2000;
        m->dmg = 100 * scale;
        m->radius_dmg = 100 * scale;
        m->dmg_radius = 120;
        m->nextthink = level.framenum + 10 * BASE_FRAMERATE;
        // Quake II's BFG ball, smaller (the client scales its Flow ball by this)
        m->s.modelindex = gi.modelindex("sprites/s_bfg1.sp2");
        m->s.effects = EF_BFG | EF_ANIM_ALLFAST;
        m->s.scale = 0.5f;
        m->s.sound = Q3_SoundIndex("weapons/rocket/rockfly.wav");
        break;
    }

    VectorScale(dir, speed, m->velocity);
    vectoangles((vec_t *)dir, m->s.angles);
    VectorCopy(muzzle, m->s.origin);
    VectorCopy(muzzle, m->s.old_origin);
    gi.linkentity(m);

    // Where in the server's 100 ms it was fired: its first move takes only the
    // rest of that frame (SV_Physics_Toss). Two plasma balls fired 0 and 50 ms
    // into one frame went out together and flew as a pair, then a gap - the
    // gun fired in spurts. A bot's one command a frame is at 0: unchanged.
    m->q3_late_ms = late_ms < 0 ? 0 : late_ms > 100 ? 100 : late_ms;

    // the prestep
    VectorMA(muzzle, Q3_MISSILE_PRESTEP, m->velocity, end);
    tr = gi.trace((vec_t *)muzzle, NULL, NULL, end, ent, MASK_SHOT);
    VectorCopy(tr.endpos, m->s.origin);
    gi.linkentity(m);
    if (tr.fraction < 1.0f && tr.ent && !tr.startsolid)
        m->touch(m, tr.ent, &tr.plane, tr.surface);
}

/*
=================
Q3W_Fire

One shot of the gun in hand. Quake III fires from 14 units in front of the
eye (CalcMuzzlePoint): no hand offset, so nothing to converge.
=================
*/
static void Q3W_Fire(edict_t *ent, int weapon, int late_ms)
{
    gclient_t   *client = ent->client;
    vec3_t      forward, right, up, muzzle, dir;
    const char  *sound = q3w_info[weapon].flash_sound;
    bool        flash = true;
    int         i, scale;

    AngleVectors(client->v_angle, forward, right, up);
    VectorCopy(ent->s.origin, muzzle);
    muzzle[2] += ent->viewheight;
    VectorMA(muzzle, 14, forward, muzzle);

    scale = Q3W_DamageScale(ent);

    // the campaign's accuracy: every shot but the gauntlet's (g_sp.c)
    if (weapon != Q3W_GAUNTLET)
        SP_AccuracyShot(ent);

    switch (weapon) {
    case Q3W_MACHINEGUN: {
        float r = random() * M_PI * 2;
        float u = sinf(r) * crandom() * Q3_MACHINEGUN_SPREAD * 16;

        r = cosf(r) * crandom() * Q3_MACHINEGUN_SPREAD * 16;
        if (Q3W_Bullet(ent, muzzle, forward, right, up, r, u, Q3_MACHINEGUN_DAMAGE * scale,
                       MOD_Q3_MACHINEGUN, TE_Q3_BULLET))
            SP_AccuracyHit(ent);
        sound = va("weapons/machinegun/machgf%db.wav", 1 + (Q_rand() & 3));
        break;
    }
    case Q3W_SHOTGUN: {
        bool hit = false;   // the shot, not each pellet, is a hit

        for (i = 0; i < Q3_SHOTGUN_COUNT; i++)
            if (Q3W_Bullet(ent, muzzle, forward, right, up,
                           crandom() * Q3_SHOTGUN_SPREAD * 16, crandom() * Q3_SHOTGUN_SPREAD * 16,
                           Q3_SHOTGUN_DAMAGE * scale, MOD_Q3_SHOTGUN, TE_Q3_SHOTGUN))
                hit = true;
        if (hit)
            SP_AccuracyHit(ent);
        break;
    }
    case Q3W_GRENADE:
        VectorCopy(forward, dir);
        dir[2] += 0.2f;
        VectorNormalize(dir);
        Q3W_Missile(ent, weapon, muzzle, dir, late_ms);
        break;
    case Q3W_ROCKET:
    case Q3W_PLASMA:
    case Q3W_BFG:
        Q3W_Missile(ent, weapon, muzzle, forward, late_ms);
        break;
    case Q3W_RAILGUN:
        Q3W_Rail(ent, muzzle, forward, right, up);
        break;
    case Q3W_LIGHTNING:
        // the beam and the muzzle light go out once a frame; it fires twice
        flash = Q3W_Lightning(ent, muzzle, forward);
        // the fire sound only as the trigger goes down; the hum loops after
        if (client->weaponstate == WEAPON_FIRING)
            sound = NULL;
        break;
    default:
        break;
    }

    if (sound)
        gi.sound(ent, CHAN_WEAPON, Q3_SoundIndex(sound), 1, ATTN_NORM, 0);
    if (client->quad_framenum > level.framenum)
        gi.sound(ent, CHAN_ITEM, Q3_SoundIndex("items/damage3.wav"), 1, ATTN_NORM, 0);

    if (flash) {
        gi.WriteByte(svc_muzzleflash);
        gi.WriteShort(ent - g_edicts);
        gi.WriteByte(q3w_info[weapon].flash);
        gi.multicast(ent->s.origin, MULTICAST_PVS);
    }

    PlayerNoise(ent, muzzle, PNOISE_WEAPON);
}

/*
==============================================================================

THE GUN IN HAND

bg_pmove.c PM_Weapon, PM_BeginWeaponChange and PM_FinishWeaponChange, on
every usercmd. Quake II's weaponstate carries Quake III's: READY, RAISING
(WEAPON_ACTIVATING), DROPPING and FIRING; client->q3_weapon_time its
weaponTime. The view weapon learns it through ps.gunframe: the state in the
top two bits, the shots fired below (Q3_GUNFRAME_*, shared.h).

==============================================================================
*/

static int Q3W_SelectableScore(edict_t *ent, const gitem_t *it)
{
    const int   *inv = ent->client->pers.inventory;
    gitem_t     *ammo;

    if (!it || !(it->flags & IT_Q3) || !inv[ITEM_INDEX(it)])
        return 0;
    if (it->ammo) {
        ammo = FindItem(it->ammo);
        if (ammo && !inv[ITEM_INDEX(ammo)] && !((int)dmflags->value & DF_INFINITE_AMMO))
            return 0;
    }
    return it->tag;     // Quake III takes the highest numbered gun
}

/*
=================
Q3W_NoAmmoChange

cg_weapons.c CG_OutOfAmmoChange: the highest numbered gun with ammo.
=================
*/
static void Q3W_NoAmmoChange(edict_t *ent)
{
    gitem_t *best = NULL, *it;
    int     i, score, best_score = 0;

    for (i = 1; i < Q3W_NUM; i++) {
        it = Q3W_Item(q3w_info[i].classname);
        score = Q3W_SelectableScore(ent, it);
        if (score > best_score) {
            best_score = score;
            best = it;
        }
    }
    if (best && best != ent->client->pers.weapon)
        ent->client->newweapon = best;
}

static void Q3W_SetGunframe(gclient_t *client)
{
    int state;

    switch (client->weaponstate) {
    case WEAPON_ACTIVATING: state = Q3_GUNFRAME_RAISING; break;
    case WEAPON_DROPPING:   state = Q3_GUNFRAME_DROPPING; break;
    case WEAPON_FIRING:     state = Q3_GUNFRAME_FIRING; break;
    default:                state = Q3_GUNFRAME_READY; break;
    }
    client->ps.gunframe = state | (client->q3_shots & Q3_GUNFRAME_SHOTS);
}

/*
=================
Q3W_WeaponChanged

ChangeWeapon has just put a Quake III gun in the player's hand: raise it.
=================
*/
void Q3W_WeaponChanged(edict_t *ent)
{
    gclient_t *client = ent->client;

    client->weaponstate = WEAPON_ACTIVATING;
    if (client->q3_weapon_time < 0)
        client->q3_weapon_time = 0;
    client->q3_weapon_time += 250;
    Q3W_SetGunframe(client);
}

// the gun's think from Think_Weapon, on the 10 Hz frame: all it does is
// keep the view weapon's state current, the gun itself runs in Q3W_Think
void Weapon_Q3(edict_t *ent)
{
    Q3W_SetGunframe(ent->client);
}

static void Q3W_WeaponSound(edict_t *ent, bool firing)
{
    gclient_t           *client = ent->client;
    const q3w_info_t    *w = &q3w_info[client->pers.weapon->tag];
    const char          *loop = NULL;

    if (firing && w->firing_sound)
        loop = w->firing_sound;
    else if (w->ready_sound)
        loop = w->ready_sound;
    client->weapon_sound = loop ? Q3_SoundIndex(loop) : 0;
}

/*
=================
Q3W_Think

ClientThink, after the move, for a player holding a Quake III gun.
=================
*/
void Q3W_Think(edict_t *ent, const usercmd_t *ucmd)
{
    gclient_t   *client = ent->client;
    gitem_t     *weapon = client->pers.weapon;
    int         tag, add;
    bool        attack;
    // ms into the server's frame this command starts (G_RunFrame resets it)
    int         frame_ms = client->q3_cmd_msec;

    client->q3_cmd_msec += ucmd->msec;

    if (!weapon || !(weapon->flags & IT_Q3) || ent->health < 1 ||
        client->resp.spectator || level.intermission_framenum)
        return;

    tag = weapon->tag;
    if (tag <= Q3W_NONE || tag >= Q3W_NUM)
        return;

    attack = (ucmd->buttons & BUTTON_ATTACK) != 0;
    client->latched_buttons &= ~BUTTON_ATTACK;

    if (client->q3_weapon_time > 0)
        client->q3_weapon_time -= ucmd->msec;

    // a change: not while a shot is still going, but again while lowering
    // or raising
    if (client->newweapon && client->newweapon != weapon &&
        (client->q3_weapon_time <= 0 || client->weaponstate != WEAPON_FIRING) &&
        client->weaponstate != WEAPON_DROPPING) {
        client->weaponstate = WEAPON_DROPPING;
        client->q3_weapon_time += 200;
        gi.sound(ent, CHAN_AUTO, Q3_SoundIndex("weapons/change.wav"), 1, ATTN_NORM, 0);
    }

    if (client->q3_weapon_time > 0) {
        Q3W_WeaponSound(ent, attack && client->weaponstate == WEAPON_FIRING);
        Q3W_SetGunframe(client);
        return;
    }

    if (client->weaponstate == WEAPON_DROPPING) {
        if (client->newweapon && client->newweapon != weapon) {
            ChangeWeapon(ent);      // Q3W_WeaponChanged, for a Quake III gun
            return;
        }
        client->newweapon = NULL;
        client->weaponstate = WEAPON_READY;
    }

    if (client->weaponstate == WEAPON_ACTIVATING) {
        client->weaponstate = WEAPON_READY;
        Q3W_SetGunframe(client);
        return;
    }

    if (!attack) {
        client->q3_weapon_time = 0;
        client->weaponstate = WEAPON_READY;
        Q3W_WeaponSound(ent, false);
        Q3W_SetGunframe(client);
        return;
    }

    if (tag == Q3W_GAUNTLET) {
        vec3_t  forward, muzzle;

        AngleVectors(client->v_angle, forward, NULL, NULL);
        VectorCopy(ent->s.origin, muzzle);
        muzzle[2] += ent->viewheight;
        VectorMA(muzzle, 14, forward, muzzle);

        // the gauntlet whirs while held down, and only spends its time on a hit
        Q3W_WeaponSound(ent, true);
        if (!Q3W_GauntletHit(ent, muzzle, forward)) {
            client->q3_weapon_time = 0;
            client->weaponstate = WEAPON_FIRING;
            Q3W_SetGunframe(client);
            return;
        }
    }

    // out of ammo: the click, half a second, and the best gun with some
    if (client->ammo_index && client->pers.inventory[client->ammo_index] <= 0 &&
        !((int)dmflags->value & DF_INFINITE_AMMO)) {
        gi.sound(ent, CHAN_ITEM, Q3_SoundIndex("weapons/noammo.wav"), 1, ATTN_NORM, 0);
        client->weaponstate = WEAPON_READY;
        client->q3_weapon_time += 500;
        Q3W_WeaponSound(ent, false);
        Q3W_NoAmmoChange(ent);
        Q3W_SetGunframe(client);
        return;
    }

    if (client->ammo_index && !((int)dmflags->value & DF_INFINITE_AMMO))
        client->pers.inventory[client->ammo_index]--;

    Q3W_Fire(ent, tag, frame_ms);
    client->weaponstate = WEAPON_FIRING;
    client->q3_shots++;

    add = q3w_info[tag].fire_time;
    if (client->q3_haste_framenum > level.framenum)
        add = (int)(add / 1.3f);
    if (CTF_HasHaste(ent))
        add /= 2;
    client->q3_weapon_time += add;

    Q3W_WeaponSound(ent, true);
    Q3W_SetGunframe(client);
}

/*
=================
Q3W_TossWeapon

TossClientWeapon: Quake III's dead drop the gun in hand, unless it is one
everyone spawns with.
=================
*/
bool Q3W_KeepsOnDeath(const gitem_t *item)
{
    return item && (item->flags & IT_Q3) &&
           (item->tag == Q3W_GAUNTLET || item->tag == Q3W_MACHINEGUN);
}

/*
=================
Q3W_Obituary

ClientObituary's message for a Quake III gun, NULL if the death wasn't one.
`self_kill` asks for the suicide line.
=================
*/
const char *Q3W_Obituary(int mod, bool self_kill, const char **message2, edict_t *self)
{
    *message2 = "";

    if (self_kill) {
        switch (mod) {
        case MOD_Q3_G_SPLASH:
            return IsFemale(self) ? "tripped on her own grenade" :
                   IsNeutral(self) ? "tripped on its own grenade" : "tripped on his own grenade";
        case MOD_Q3_R_SPLASH:
            return IsFemale(self) ? "blew herself up" :
                   IsNeutral(self) ? "blew itself up" : "blew himself up";
        case MOD_Q3_P_SPLASH:
            return IsFemale(self) ? "melted herself" :
                   IsNeutral(self) ? "melted itself" : "melted himself";
        case MOD_Q3_BFG_SPLASH:
            return "should have used a smaller gun";
        default:
            return NULL;
        }
    }

    switch (mod) {
    case MOD_Q3_GAUNTLET:   return "was pummeled by";
    case MOD_Q3_MACHINEGUN: return "was machinegunned by";
    case MOD_Q3_SHOTGUN:    return "was gunned down by";
    case MOD_Q3_GRENADE:    *message2 = "'s grenade"; return "ate";
    case MOD_Q3_G_SPLASH:   *message2 = "'s shrapnel"; return "was shredded by";
    case MOD_Q3_ROCKET:     *message2 = "'s rocket"; return "ate";
    case MOD_Q3_R_SPLASH:   *message2 = "'s rocket"; return "almost dodged";
    case MOD_Q3_PLASMA:
    case MOD_Q3_P_SPLASH:   *message2 = "'s plasmagun"; return "was melted by";
    case MOD_Q3_RAILGUN:    return "was railed by";
    case MOD_Q3_LIGHTNING:  return "was electrocuted by";
    case MOD_Q3_BFG:
    case MOD_Q3_BFG_SPLASH: *message2 = "'s BFG"; return "was blasted by";
    default:                return NULL;
    }
}
