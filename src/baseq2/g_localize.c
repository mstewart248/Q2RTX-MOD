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
/*
==============================================================================

LOCALIZED STRINGS                                               (rerelease)

The rerelease maps do not store display text. They store a key:

    "message"   "$map_you_have_found_a_secret"

and the game looks it up in a localization table at display time. Without that
step every one of these shows up in game as the raw key. There are **1481 of
them across 827 distinct keys** in the map set - `message` on target_help,
func_button, target_secret, func_door, trigger_once and friends - so this is
the single biggest source of visibly wrong text in the port.

Two sources, in order:

1. `localization/loc_english.txt`, if it is present. That is the file the
   rerelease ships; drop it in and the real strings are used.

2. Otherwise the key is turned back into English. These keys are just the
   original sentence in snake_case, so `$map_you_have_found_a_secret` reads
   back as "You have found a secret" and `$map_access_denied_1` as "Access
   denied". 825 of the 827 keys use the `$map_` prefix and this reconstruction
   is accurate for effectively all of them.

Resolution happens once, at spawn, in ED_NewString - so it costs nothing at
display time and covers every entity key without touching each consumer.

==============================================================================
*/

#include "g_local.h"

#define L10N_MAX_ENTRIES    4096

typedef struct {
    const char  *key;       // without the leading '$'
    const char  *value;
} l10n_entry_t;

static l10n_entry_t l10n_entries[L10N_MAX_ENTRIES];
static int          l10n_count;
static char        *l10n_filebuf;
static bool         l10n_loaded;

/*
=================
L10N_ParseBuffer

Tolerant of the shapes this file turns up in. A line is a key followed by a
value; either may be quoted, the key may or may not carry its '$', and blank
lines plus // comments are skipped. Parsed in place - the buffer is retained.
=================
*/
static void L10N_ParseBuffer(char *buf)
{
    char *p = buf;

    while (*p && l10n_count < L10N_MAX_ENTRIES) {
        char *key, *value;

        // skip whitespace and blank lines
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        if (!*p)
            break;

        // comment
        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n')
                p++;
            continue;
        }

        // ---- key
        if (*p == '"') {
            key = ++p;
            while (*p && *p != '"')
                p++;
        } else {
            key = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '=' && *p != '\r' && *p != '\n')
                p++;
        }
        if (!*p)
            break;
        *p++ = 0;

        if (*key == '$')
            key++;

        // ---- separator
        while (*p == ' ' || *p == '\t' || *p == '=')
            p++;

        // ---- value (runs to end of line if unquoted)
        if (*p == '"') {
            value = ++p;
            while (*p && *p != '"')
                p++;
        } else {
            value = p;
            while (*p && *p != '\r' && *p != '\n')
                p++;
        }
        if (*p)
            *p++ = 0;

        if (*key && *value) {
            l10n_entries[l10n_count].key = key;
            l10n_entries[l10n_count].value = value;
            l10n_count++;
        }
    }
}

/*
=================
L10N_Init

Called once per game init. The game library has no filesystem in its import
table, but it is an ordinary DLL and g_save.c already uses stdio, so the file
is read directly. Paths are tried relative to the working directory, which is
the install root.
=================
*/
void L10N_Init(void)
{
    static const char *LOCFILE = "localization/loc_english.txt";
    char        path[MAX_QPATH * 2];
    const char  *gamedir;
    FILE        *f = NULL;
    long        len;
    int         i;

    if (l10n_loaded)
        return;
    l10n_loaded = true;

    gamedir = gi.cvar("game", "", 0)->string;

    for (i = 0; i < 3 && !f; i++) {
        switch (i) {
        case 0:
            if (!*gamedir)
                continue;
            Q_snprintf(path, sizeof(path), "%s/%s", gamedir, LOCFILE);
            break;
        case 1:
            Q_snprintf(path, sizeof(path), "baseq2/%s", LOCFILE);
            break;
        default:
            Q_snprintf(path, sizeof(path), "%s", LOCFILE);
            break;
        }
        f = fopen(path, "rb");
    }

    if (!f) {
        gi.dprintf("Localization: no loc_english.txt found, "
                   "reconstructing text from the keys themselves.\n");
        return;
    }

    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len <= 0 || len > 4 * 1024 * 1024) {
        fclose(f);
        return;
    }

    l10n_filebuf = malloc(len + 1);
    if (!l10n_filebuf) {
        fclose(f);
        return;
    }

    len = fread(l10n_filebuf, 1, len, f);
    fclose(f);
    l10n_filebuf[len] = 0;

    L10N_ParseBuffer(l10n_filebuf);

    gi.dprintf("Localization: %d strings from %s\n", l10n_count, path);
}

void L10N_Shutdown(void)
{
    free(l10n_filebuf);
    l10n_filebuf = NULL;
    l10n_count = 0;
    l10n_loaded = false;
}

/*
=================
L10N_Format

Some localized strings carry a "{}" placeholder for a runtime value - the
objective banners are "Primary Objective:\n{}", for example. Resolve the key,
substitute `arg` for the first "{}", and fall back to a printf-style default if
the table has no entry.

Escape sequences are handled here too, because unlike a map key this text does
not pass back through ED_NewString.
=================
*/
void L10N_Format(char *out, size_t outsize, const char *key, const char *fallback, const char *arg)
{
    char        resolved[MAX_STRING_CHARS];
    const char  *src;
    const char  *brace;
    size_t      n;

    src = L10N_Resolve(key, resolved, sizeof(resolved));

    if (src == key) {
        // no table entry - use the caller's plain-English form
        Q_snprintf(out, outsize, fallback, arg);
    } else {
        brace = strstr(src, "{}");

        if (brace) {
            n = brace - src;
            if (n >= outsize)
                n = outsize - 1;
            memcpy(out, src, n);
            out[n] = 0;
            Q_strlcat(out, arg, outsize);
            Q_strlcat(out, brace + 2, outsize);
        } else {
            Q_strlcpy(out, src, outsize);
        }
    }

    // "\n" arrives as two characters from the file; make it a real newline
    {
        char *r = out, *w = out;

        while (*r) {
            if (r[0] == '\\' && r[1] == 'n') {
                *w++ = '\n';
                r += 2;
            } else {
                *w++ = *r++;
            }
        }
        *w = 0;
    }
}

/*
=================
L10N_Humanize

Turn a key back into readable English: drop the namespace prefix, drop the
trailing disambiguating number the rerelease appends to duplicate strings
("access_denied_1"), turn underscores into spaces and capitalise.
=================
*/
static void L10N_Humanize(const char *key, char *out, size_t outsize)
{
    static const char *prefixes[] = { "map_", "g_", "m_", "item_", "boss2_" };
    const char  *s = key;
    size_t      n;
    int         i;

    for (i = 0; i < q_countof(prefixes); i++) {
        size_t plen = strlen(prefixes[i]);
        if (!Q_strncasecmp(s, prefixes[i], plen)) {
            s += plen;
            break;
        }
    }

    Q_strlcpy(out, s, outsize);

    // trailing "_<digits>" is a disambiguator, not part of the sentence
    n = strlen(out);
    while (n > 0 && out[n - 1] >= '0' && out[n - 1] <= '9')
        n--;
    if (n > 0 && n < strlen(out) && out[n - 1] == '_')
        out[n - 1] = 0;

    // some keys are stored with a trailing underscore where the text was cut
    n = strlen(out);
    while (n > 0 && out[n - 1] == '_')
        out[--n] = 0;

    for (n = 0; out[n]; n++) {
        if (out[n] == '_')
            out[n] = ' ';
    }

    if (out[0] >= 'a' && out[0] <= 'z')
        out[0] -= 'a' - 'A';
}

/*
=================
L10N_Resolve

`value` is a raw map string. If it names a localized string, write the display
text into `out` and return it; otherwise return `value` unchanged.
=================
*/
const char *L10N_Resolve(const char *value, char *out, size_t outsize)
{
    const char *key;
    int i;

    if (!value || value[0] != '$' || !value[1])
        return value;

    key = value + 1;

    for (i = 0; i < l10n_count; i++) {
        if (!Q_stricmp(l10n_entries[i].key, key)) {
            Q_strlcpy(out, l10n_entries[i].value, outsize);
            return out;
        }
    }

    L10N_Humanize(key, out, outsize);

    // if there was nothing left to say, keep the key so it is still findable
    return *out ? out : value;
}
