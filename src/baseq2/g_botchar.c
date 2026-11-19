/*
Copyright (C) 1999-2005 Id Software, Inc.
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

QUAKE III BOT CHARACTERS

Each of Quake III's bots has a character file in its pak0,
botfiles/bots/<name>_c.c: how well it aims, how fast it turns and reacts,
whether it walks or jumps about, how often it holds the trigger - written out
for skills 1, 4 and 5. A skill in between is a blend of the two around it,
and anything a file leaves out comes from default_c.c at the same skill:
botlib's be_ai_char.c (BotLoadCharacter, BotLoadCharacterSkill,
BotInterpolateCharacters), done again here for the few characteristics
g_bot.c plays by.

This is why Crash, the first fight, is the easiest one: at "I Can Win" her
aim accuracy is 0.2 of 1, she turns at a fifth of the way a tick, she walks
everywhere and she fires in short bursts, where Xaero, the last, has 0.75 and
0.95 at the same skill.

==============================================================================
*/

#include "g_local.h"

static const char *const botchar_names[BCH_NUM] = {
    [BCH_ATTACK_SKILL]                  = "ATTACK_SKILL",
    [BCH_AIM_SKILL]                     = "AIM_SKILL",
    [BCH_AIM_ACCURACY]                  = "AIM_ACCURACY",
    [BCH_VIEW_FACTOR]                   = "VIEW_FACTOR",
    [BCH_VIEW_MAXCHANGE]                = "VIEW_MAXCHANGE",
    [BCH_REACTIONTIME]                  = "REACTIONTIME",
    [BCH_FIRETHROTTLE]                  = "FIRETHROTTLE",
    [BCH_ALERTNESS]                     = "ALERTNESS",
    [BCH_WALKER]                        = "WALKER",
    [BCH_JUMPER]                        = "JUMPER",
    [BCH_CROUCHER]                      = "CROUCHER",
    [BCH_AIM_ACCURACY_MACHINEGUN]       = "AIM_ACCURACY_MACHINEGUN",
    [BCH_AIM_ACCURACY_SHOTGUN]          = "AIM_ACCURACY_SHOTGUN",
    [BCH_AIM_ACCURACY_ROCKETLAUNCHER]   = "AIM_ACCURACY_ROCKETLAUNCHER",
    [BCH_AIM_ACCURACY_GRENADELAUNCHER]  = "AIM_ACCURACY_GRENADELAUNCHER",
    [BCH_AIM_ACCURACY_LIGHTNING]        = "AIM_ACCURACY_LIGHTNING",
    [BCH_AIM_ACCURACY_PLASMAGUN]        = "AIM_ACCURACY_PLASMAGUN",
    [BCH_AIM_ACCURACY_RAILGUN]          = "AIM_ACCURACY_RAILGUN",
    [BCH_AIM_ACCURACY_BFG10K]           = "AIM_ACCURACY_BFG10K",
    [BCH_AIM_SKILL_ROCKETLAUNCHER]      = "AIM_SKILL_ROCKETLAUNCHER",
    [BCH_AIM_SKILL_GRENADELAUNCHER]     = "AIM_SKILL_GRENADELAUNCHER",
    [BCH_AIM_SKILL_PLASMAGUN]           = "AIM_SKILL_PLASMAGUN",
    [BCH_AIM_SKILL_BFG10K]              = "AIM_SKILL_BFG10K",
};

#define BOTCHAR_SKILLS  6       // a file's "skill N" blocks, 1..5

typedef struct {
    bool    set[BCH_NUM];
    float   value[BCH_NUM];
} botchar_block_t;

typedef struct {
    bool            have[BOTCHAR_SKILLS];
    botchar_block_t block[BOTCHAR_SKILLS];
} botchar_file_t;

// the next word, "quoted string", or brace; NULL at the end. Skips // and
// /* */ comments, and the #include "chars.h" each file starts with.
static const char *BotChar_Token(const char **data_p, char *out, size_t size)
{
    const char  *s = *data_p;
    size_t      len = 0;

    out[0] = 0;
    for (;;) {
        while (*s && (unsigned char)*s <= ' ')
            s++;
        if (s[0] == '/' && s[1] == '/') {
            while (*s && *s != '\n')
                s++;
            continue;
        }
        if (s[0] == '/' && s[1] == '*') {
            s += 2;
            while (*s && !(s[0] == '*' && s[1] == '/'))
                s++;
            if (*s)
                s += 2;
            continue;
        }
        break;
    }
    if (!*s) {
        *data_p = s;
        return NULL;
    }

    if (*s == '"') {
        s++;
        while (*s && *s != '"') {
            if (len < size - 1)
                out[len++] = *s;
            s++;
        }
        if (*s)
            s++;
    } else if (*s == '{' || *s == '}') {
        out[len++] = *s++;
    } else {
        while (*s && (unsigned char)*s > ' ' && *s != '{' && *s != '}') {
            if (len < size - 1)
                out[len++] = *s;
            s++;
        }
    }
    out[len] = 0;
    *data_p = s;
    return out;
}

static bool BotChar_Parse(const char *path, botchar_file_t *f)
{
    void        *buf = NULL;
    char        *text, tok[MAX_QPATH], key[MAX_QPATH];
    const char  *data;
    int         len, skill, i;

    memset(f, 0, sizeof(*f));
    if (!gi.LoadFile)
        return false;
    len = gi.LoadFile(path, &buf);
    if (len <= 0 || !buf)
        return false;

    // the loader's buffer is not terminated
    text = gi.TagMalloc(len + 1, TAG_GAME);
    memcpy(text, buf, len);
    text[len] = 0;
    gi.FreeFile(buf);

    data = text;
    while (BotChar_Token(&data, tok, sizeof(tok))) {
        botchar_block_t *b;

        if (Q_stricmp(tok, "skill"))
            continue;
        if (!BotChar_Token(&data, tok, sizeof(tok)))
            break;
        skill = atoi(tok);
        if (!BotChar_Token(&data, tok, sizeof(tok)) || strcmp(tok, "{"))
            break;
        if (skill < 1 || skill >= BOTCHAR_SKILLS)
            skill = 0;                  // read past it
        f->have[skill] = skill > 0;
        b = &f->block[skill];

        while (BotChar_Token(&data, key, sizeof(key)) && strcmp(key, "}")) {
            if (!BotChar_Token(&data, tok, sizeof(tok)))
                break;
            if (Q_strncasecmp(key, "CHARACTERISTIC_", 15))
                continue;
            for (i = 0; i < BCH_NUM; i++) {
                if (!Q_stricmp(key + 15, botchar_names[i])) {
                    b->set[i] = true;
                    b->value[i] = atof(tok);
                    break;
                }
            }
        }
    }

    gi.TagFree(text);
    return true;
}

/*
=================
BotChar_AIFile

scripts/bots.txt names each bot's character file ("aifile bots/crash_c.c");
failing that, botfiles/bots/<name>_c.c.
=================
*/
static void BotChar_AIFile(const char *name, char *out, size_t size)
{
    void        *buf = NULL;
    char        *text, tok[MAX_QPATH], key[MAX_QPATH], who[MAX_QPATH], file[MAX_QPATH];
    const char  *data;
    int         len;

    Q_snprintf(out, size, "baseq3/botfiles/bots/%s_c.c", name);
    Q_strlwr(out);

    len = gi.LoadFile ? gi.LoadFile("baseq3/scripts/bots.txt", &buf) : -1;
    if (len <= 0 || !buf)
        return;
    text = gi.TagMalloc(len + 1, TAG_GAME);
    memcpy(text, buf, len);
    text[len] = 0;
    gi.FreeFile(buf);

    data = text;
    while (BotChar_Token(&data, tok, sizeof(tok))) {
        if (strcmp(tok, "{"))
            continue;
        who[0] = file[0] = 0;
        while (BotChar_Token(&data, key, sizeof(key)) && strcmp(key, "}")) {
            if (!BotChar_Token(&data, tok, sizeof(tok)))
                break;
            if (!Q_stricmp(key, "name"))
                Q_strlcpy(who, tok, sizeof(who));
            else if (!Q_stricmp(key, "aifile"))
                Q_strlcpy(file, tok, sizeof(file));
        }
        if (who[0] && file[0] && !Q_stricmp(who, name)) {
            Q_snprintf(out, size, "baseq3/botfiles/%s", file);
            break;
        }
    }
    gi.TagFree(text);
}

/*
=================
BotChar_EngineFile

The same file as Quake III's bot library opens it, under botfiles/
("bots/crash_c.c"): for the bot engine (g_q3bot.c). NULL `name`, a bot
that is not one of Quake III's, plays on its default character.
=================
*/
void BotChar_EngineFile(const char *name, char *out, size_t size)
{
    char path[MAX_QPATH];

    if (!name) {
        Q_strlcpy(out, "bots/default_c.c", size);
        return;
    }
    BotChar_AIFile(name, path, sizeof(path));
    if (!Q_strncasecmp(path, "baseq3/botfiles/", 16))
        Q_strlcpy(out, path + 16, size);
    else
        Q_strlcpy(out, path, size);
}

// BotLoadCharacterSkill: one of the file's own skills, gaps from default_c.c
static void BotChar_Skill(const botchar_file_t *bot, const botchar_file_t *def, int skill, float *out)
{
    const botchar_block_t   *b = bot->have[skill] ? &bot->block[skill] : NULL;
    const botchar_block_t   *d = def->have[skill] ? &def->block[skill] : NULL;
    int                     i;

    for (i = 0; i < BCH_NUM; i++) {
        if (b && b->set[i])
            out[i] = b->value[i];
        else if (d && d->set[i])
            out[i] = d->value[i];
        else
            out[i] = 0;
    }
}

/*
=================
BotChar_Load

The character of Quake III's bot `name` (NULL: default_c.c's own) at Quake
III skill 1..5, fractions blended as BotLoadCharacter does: 1, 4 and 5 are
in the files, below 4 is a blend of 1 and 4, above it of 4 and 5.
=================
*/
bool BotChar_Load(const char *name, float skill, bot_character_t *out)
{
    static botchar_file_t   def, bot;       // static: a few KB each
    char                    path[MAX_QPATH];
    float                   lo[BCH_NUM], hi[BCH_NUM];
    int                     a, b, i;

    memset(out, 0, sizeof(*out));

    if (!BotChar_Parse("baseq3/botfiles/bots/default_c.c", &def))
        return false;
    if (name) {
        BotChar_AIFile(name, path, sizeof(path));
        if (!BotChar_Parse(path, &bot)) {
            gi.dprintf("Bots: no character file %s, Quake III's default instead\n", path);
            bot = def;
        }
    } else {
        bot = def;
    }

    skill = max(1.0f, min(5.0f, skill));
    if (skill == 1 || skill == 4 || skill == 5) {
        BotChar_Skill(&bot, &def, (int)skill, out->c);
    } else {
        a = skill < 4 ? 1 : 4;
        b = skill < 4 ? 4 : 5;
        BotChar_Skill(&bot, &def, a, lo);
        BotChar_Skill(&bot, &def, b, hi);
        for (i = 0; i < BCH_NUM; i++)
            out->c[i] = lo[i] + (hi[i] - lo[i]) * (skill - a) / (b - a);
    }

    out->loaded = true;
    out->skill = skill;
    Q_strlcpy(out->name, name ? name : "default", sizeof(out->name));
    return true;
}

// Characteristic_BFloat: a characteristic, held between lo and hi
float BotChar_BFloat(const bot_character_t *ch, int index, float lo, float hi)
{
    float v = ch->c[index];

    return v < lo ? lo : v > hi ? hi : v;
}
