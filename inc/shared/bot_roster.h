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

// "male/grunt" -> "Grunt", "cyborg/ps9000" -> "PS9000", "homer/homer" -> "Homer"
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

    s = strchr(skin, '/');
    Q_strlcpy(out, s ? s + 1 : skin, size);
    if (out[0] >= 'a' && out[0] <= 'z')
        out[0] -= 'a' - 'A';
}

#endif // BOT_ROSTER_H
