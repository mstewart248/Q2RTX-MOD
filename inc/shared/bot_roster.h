/*
Bots are named after the skin they wear: the game spawns them
(src/baseq2/g_bot.c) and the bot match menu rolls and lists them
(src/client/ui/ui.c), so both name them through this one header.

The table is every skin id ships for a player, and how to write its name
where plain capitalising would not do (PS9000, not Ps9000). Any other skin
(`sv addbot` takes any model/skin) is named by capitalising its file name.
*/

#ifndef BOT_ROSTER_H
#define BOT_ROSTER_H

typedef struct {
    const char  *name;
    const char  *skin;
} bot_roster_entry_t;

static const bot_roster_entry_t bot_classic_roster[] = {
    { "Grunt",      "male/grunt" },
    { "Claymore",   "male/claymore" },
    { "Flak",       "male/flak" },
    { "Howitzer",   "male/howitzer" },
    { "Major",      "male/major" },
    { "Nightops",   "male/nightops" },
    { "Pointman",   "male/pointman" },
    { "Psycho",     "male/psycho" },
    { "Rampage",    "male/rampage" },
    { "Razor",      "male/razor" },
    { "Recon",      "male/recon" },
    { "Scout",      "male/scout" },
    { "Sniper",     "male/sniper" },
    { "Viper",      "male/viper" },
    { "Cipher",     "male/cipher" },
    { "Athena",     "female/athena" },
    { "Brianna",    "female/brianna" },
    { "Cobalt",     "female/cobalt" },
    { "Ensign",     "female/ensign" },
    { "Jezebel",    "female/jezebel" },
    { "Jungle",     "female/jungle" },
    { "Lotus",      "female/lotus" },
    { "Stiletto",   "female/stiletto" },
    { "Venus",      "female/venus" },
    { "Voodoo",     "female/voodoo" },
    { "Oni911",     "cyborg/oni911" },
    { "PS9000",     "cyborg/ps9000" },
    { "Tyr574",     "cyborg/tyr574" },
    // the rerelease's own additions (rerelease/materialDev/players), worn by
    // its bots; the classic game does not have them, so they go unpicked
    { "Deaddude",   "male/deaddude" },
    { "Insane1",    "male/insane1" },
    { "Insane2",    "male/insane2" },
    { "Insane3",    "male/insane3" },
};

#define BOT_CLASSIC_ROSTER_SIZE \
    ((int)(sizeof(bot_classic_roster) / sizeof(bot_classic_roster[0])))

/*
Quake III Arena's bots, as its scripts/bots.txt dresses them - worn when baseq3
has its player models (client/q3player.c draws them). A Quake III skin is
"model/skin" like any other; a model's own look is its "default".
*/
static const bot_roster_entry_t bot_q3_roster[] = {
    { "Sarge",      "sarge/default" },
    { "Anarki",     "anarki/default" },
    { "Angel",      "lucy/angel" },
    { "Biker",      "biker/default" },
    { "Bitterman",  "bitterman/default" },
    { "Bones",      "bones/default" },
    { "Cadavre",    "biker/cadavre" },
    { "Crash",      "crash/default" },
    { "Daemia",     "major/daemia" },
    { "Doom",       "doom/default" },
    { "Gorre",      "visor/gorre" },
    { "Grunt",      "grunt/default" },
    { "Hossman",    "biker/hossman" },
    { "Hunter",     "hunter/default" },
    { "Keel",       "keel/default" },
    { "Klesk",      "klesk/default" },
    { "Lucy",       "lucy/default" },
    { "Major",      "major/default" },
    { "Mynx",       "mynx/default" },
    { "Orbb",       "orbb/default" },
    { "Patriot",    "razor/patriot" },
    { "Phobos",     "doom/phobos" },
    { "Ranger",     "ranger/default" },
    { "Razor",      "razor/default" },
    { "Slash",      "slash/default" },
    { "Sorlag",     "sorlag/default" },
    { "Stripe",     "grunt/stripe" },
    { "TankJr",     "tankjr/default" },
    { "Uriel",      "uriel/default" },
    { "Visor",      "visor/default" },
    { "Wrack",      "ranger/wrack" },
    { "Xaero",      "xaero/default" },
};

#define BOT_Q3_ROSTER_SIZE \
    ((int)(sizeof(bot_q3_roster) / sizeof(bot_q3_roster[0])))

/*
Team Arena's (baseq3/missionpack scripts/bots.txt): two whole models and the
eleven characters that are a head on a shared body (shared/ta_characters.h).
Not in a random bot's pool - its skirmish names them (client/ui/q3sp.c), and
the bot picker lists them - so a Quake III bot match with Team Arena
installed rolls the same bots as before.
*/
static const bot_roster_entry_t bot_ta_roster[] = {
    { "Fritzkrieg", "fritzkrieg/default" },
    { "Pi",         "pi/default" },
    { "James",      "james/default" },
    { "Janet",      "janet/default" },
    { "Callisto",   "callisto/default" },
    { "Gammy",      "gammy/default" },
    { "Gaunt",      "gaunt/default" },
    { "Khan",       "khan/default" },
    { "Megan",      "megan/default" },
    { "Morgan",     "morgan/default" },
    { "Neptune",    "neptune/default" },
    { "Ursula",     "ursula/default" },
    { "Flayer",     "flayer/default" },
};

#define BOT_TA_ROSTER_SIZE \
    ((int)(sizeof(bot_ta_roster) / sizeof(bot_ta_roster[0])))

/*
bot_models: whose models a random bot wears - 0 Quake II's, 1 Quake III's
(baseq3), 2 both. Archived, set from the Bot Match menu. Quake III's need
its data, which the menu checks and the game finds (Bot_SkinExists).
*/
#define BOT_MODELS_Q2       0
#define BOT_MODELS_Q3       1
#define BOT_MODELS_BOTH     2

// the rerelease's six bot skill levels, in order (bots/settings_PC.txt)
#define BOT_NUM_SKILLS  6

// the most bots a match takes: sixteen players, one of them you
#define BOT_MAX_MATCH   15

/*
What a bot picks from, for now: only the skins above, the ones id shipped
for male, female and cyborg. Skins downloaded from servers land in those
same folders (male/adolf, female/assasinf...), so the folder is no test;
custom models are left out too, as are the team colours and the disguise.
Each pick is checked against what the game can actually load.
*/
static inline bool Bot_SkinUsable(const char *skin)
{
    int i;

    for (i = 0; i < BOT_CLASSIC_ROSTER_SIZE; i++)
        if (!Q_stricmp(bot_classic_roster[i].skin, skin))
            return true;

    return false;
}

static inline bool Bot_SkinUsableQ3(const char *skin)
{
    int i;

    for (i = 0; i < BOT_Q3_ROSTER_SIZE; i++)
        if (!Q_stricmp(bot_q3_roster[i].skin, skin))
            return true;

    return false;
}

// a random bot may wear it, with bot_models at `models`
static inline bool Bot_SkinInPool(const char *skin, int models)
{
    if (models != BOT_MODELS_Q3 && Bot_SkinUsable(skin))
        return true;
    if (models != BOT_MODELS_Q2 && Bot_SkinUsableQ3(skin))
        return true;
    return false;
}

// "male/grunt" -> "Grunt", "cyborg/ps9000" -> "PS9000", "homer/homer" -> "Homer",
// "sarge/default" -> "Sarge", "ranger/wrack" -> "Wrack"
static inline void Bot_SkinName(const char *skin, char *out, size_t size)
{
    const char  *s;
    int         i;

    for (i = 0; i < BOT_CLASSIC_ROSTER_SIZE; i++) {
        if (!Q_stricmp(bot_classic_roster[i].skin, skin)) {
            Q_strlcpy(out, bot_classic_roster[i].name, size);
            return;
        }
    }
    for (i = 0; i < BOT_Q3_ROSTER_SIZE; i++) {
        if (!Q_stricmp(bot_q3_roster[i].skin, skin)) {
            Q_strlcpy(out, bot_q3_roster[i].name, size);
            return;
        }
    }
    for (i = 0; i < BOT_TA_ROSTER_SIZE; i++) {
        if (!Q_stricmp(bot_ta_roster[i].skin, skin)) {
            Q_strlcpy(out, bot_ta_roster[i].name, size);
            return;
        }
    }

    // a Quake III model's own look goes by the model's name
    s = strchr(skin, '/');
    if (s && !Q_stricmp(s + 1, "default")) {
        size_t len = (size_t)(s - skin) + 1;

        Q_strlcpy(out, skin, min(size, len));
    } else {
        Q_strlcpy(out, s ? s + 1 : skin, size);
    }
    if (out[0] >= 'a' && out[0] <= 'z')
        out[0] -= 'a' - 'A';
}

#endif // BOT_ROSTER_H
