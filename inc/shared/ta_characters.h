/*
Team Arena's characters (baseq3/missionpack): eleven of them are not models
of their own but a head on one of two shared bodies - its scripts/bots.txt
gives Neptune "model james, headmodel *neptune" - and the bodies wear each
of its five teams' uniforms (models/players/james/Crusaders/upper_red.skin
and so on). Here a character is a Quake III model of its own name, so
"neptune/crusaders_red" works where "sarge/red" does: the client draws it
(client/q3player.c) and the game spawns and dresses it (baseq2/g_bot.c)
through this one table.

  model:  the character - its head is models/players/heads/<model>/<model>.md3
          with head_default/red/blue.skin; its body, animation.cfg and voice
          (sound/player/<body>/) are its body's
  skin:   default, red or blue - the body's own .skin at the top of its
          folder, a team colour taking ioq3's default teams' uniforms (Stroggs
          red, Pagans blue: DEFAULT_REDTEAM_NAME / DEFAULT_BLUETEAM_NAME) -
          or <team>, <team>_red, <team>_blue for a team's own, the team
          written as TA_TeamToken has it (crusaders, thefallen)

Fritzkrieg and Pi are whole models of their own, and need none of this.
*/

#ifndef TA_CHARACTERS_H
#define TA_CHARACTERS_H

typedef struct {
    const char  *name;      // the head, and the model's name here (lower case)
    const char  *body;      // models/players/<body>/
} ta_character_t;

static const ta_character_t ta_characters[] = {
    { "james",      "james" },
    { "janet",      "janet" },
    { "callisto",   "janet" },
    { "gammy",      "janet" },
    { "megan",      "janet" },
    { "ursula",     "janet" },
    { "neptune",    "james" },
    { "morgan",     "james" },
    { "khan",       "james" },
    { "gaunt",      "james" },
    { "flayer",     "james" },
};

#define TA_NUM_CHARACTERS   ((int)(sizeof(ta_characters) / sizeof(ta_characters[0])))

// teaminfo.txt's teams, as their uniforms' folders are named
static const char *const ta_team_dirs[] = { "Crusaders", "Intruders", "Pagans", "Stroggs", "The Fallen" };

#define TA_NUM_TEAMS        ((int)(sizeof(ta_team_dirs) / sizeof(ta_team_dirs[0])))
#define TA_TEAM_STROGGS     3
#define TA_TEAM_PAGANS      2

// every skin of a character, in this order: three of the body's own, then
// three of each team's. The order is the skinnum's (Q3P_PatchMD3), and the
// same for each of them, so one patched body serves all six of its heads.
#define TA_SKINS_PER_TEAM   3
#define TA_NUM_SKINS        (TA_SKINS_PER_TEAM + TA_NUM_TEAMS * TA_SKINS_PER_TEAM)

static const char *const ta_colors[TA_SKINS_PER_TEAM] = { "default", "red", "blue" };

static inline const ta_character_t *TA_Character(const char *model)
{
    int i;

    for (i = 0; i < TA_NUM_CHARACTERS; i++)
        if (!Q_stricmp(ta_characters[i].name, model))
            return &ta_characters[i];
    return NULL;
}

// "The Fallen" -> "thefallen": a team as a skin name has it
static inline void TA_TeamToken(int team, char *out, size_t size)
{
    const char  *s = ta_team_dirs[team];
    size_t      n = 0;

    for (; *s && n + 1 < size; s++)
        if (*s != ' ')
            out[n++] = Q_tolower(*s);
    out[n] = 0;
}

// a team's index from its name or token, -1 if none
static inline int TA_TeamIndex(const char *name)
{
    char    token[32], want[32];
    size_t  n = 0;
    int     i;

    for (; name && *name && n + 1 < sizeof(want); name++)
        if (*name != ' ')
            want[n++] = Q_tolower(*name);
    want[n] = 0;
    for (i = 0; i < TA_NUM_TEAMS; i++) {
        TA_TeamToken(i, token, sizeof(token));
        if (!strcmp(token, want))
            return i;
    }
    return -1;
}

// skin (index) of TA_NUM_SKINS: its name
static inline void TA_SkinName(int index, char *out, size_t size)
{
    char token[32];

    if (index < TA_SKINS_PER_TEAM) {
        Q_strlcpy(out, ta_colors[index], size);
        return;
    }
    index -= TA_SKINS_PER_TEAM;
    TA_TeamToken(index / TA_SKINS_PER_TEAM, token, sizeof(token));
    if (index % TA_SKINS_PER_TEAM)
        Q_snprintf(out, size, "%s_%s", token, ta_colors[index % TA_SKINS_PER_TEAM]);
    else
        Q_strlcpy(out, token, size);
}

/*
A skin's uniform: *team the team's index (-1: the body's own .skins) and
*color 0 default, 1 red, 2 blue. A plain red or blue is ioq3's default
teams'. False for a name that is none of TA_NUM_SKINS.
*/
static inline bool TA_ParseSkin(const char *skin, int *team, int *color)
{
    char        name[64];
    const char  *under;
    int         i;

    for (i = 0; i < TA_SKINS_PER_TEAM; i++) {
        if (!Q_stricmp(skin, ta_colors[i])) {
            *color = i;
            *team = i == 1 ? TA_TEAM_STROGGS : i == 2 ? TA_TEAM_PAGANS : -1;
            return true;
        }
    }

    under = strchr(skin, '_');
    Q_strlcpy(name, skin, sizeof(name));
    *color = 0;
    if (under && (size_t)(under - skin) < sizeof(name)) {
        name[under - skin] = 0;
        for (i = 1; i < TA_SKINS_PER_TEAM; i++)
            if (!Q_stricmp(under + 1, ta_colors[i]))
                break;
        if (i == TA_SKINS_PER_TEAM)
            return false;
        *color = i;
    }
    *team = TA_TeamIndex(name);
    return *team >= 0;
}

#endif // TA_CHARACTERS_H
