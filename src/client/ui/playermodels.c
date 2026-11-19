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

#include "ui.h"
#include "common/files.h"

/*
=============================================================================

PLAYER MODELS

=============================================================================
*/

static void PlayerModel_LoadQ3(void);
static void PlayerModel_BuildCells(void);

static bool IconOfSkinExists(char *skin, char **pcxfiles, int npcxfiles)
{
    int i;
    char scratch[MAX_OSPATH];

    COM_StripExtension(scratch, skin, sizeof(scratch));
    Q_strlcat(scratch, "_i.pcx", sizeof(scratch));

    for (i = 0; i < npcxfiles; i++) {
        if (Q_stricmp(pcxfiles[i], scratch) == 0)
            return true;
    }

    return false;
}

static int pmicmpfnc(const void *_a, const void *_b)
{
    const playerModelInfo_t *a = (const playerModelInfo_t *)_a;
    const playerModelInfo_t *b = (const playerModelInfo_t *)_b;

    // Quake II's, then Quake III's
    if (a->q3 != b->q3)
        return a->q3 ? 1 : -1;
    if (a->q3)
        return Q_stricmp(a->directory, b->directory);

    /*
    ** sort by male, female, then alphabetical
    */
    if (strcmp(a->directory, "male") == 0)
        return -1;
    else if (strcmp(b->directory, "male") == 0)
        return 1;

    if (strcmp(a->directory, "female") == 0)
        return -1;
    else if (strcmp(b->directory, "female") == 0)
        return 1;

    return strcmp(a->directory, b->directory);
}

void PlayerModel_Load(void)
{
    char scratch[MAX_QPATH];
    int ndirs = 0;
    char *dirnames[MAX_PLAYERMODELS];
    int i, j;
    char **list;
    char *s, *p;
    int numFiles;
    playerModelInfo_t *pmi;

    uis.numPlayerModels = 0;

    // get a list of directories
    if (!(list = (char **)FS_ListFiles(NULL, "players/*/tris.md2", FS_SEARCH_BYFILTER | FS_SEARCH_SAVEPATH, &numFiles))) {
        return;
    }

    for (i = 0; i < numFiles; i++) {
        if (Q_strlcpy(scratch, list[i], sizeof(scratch)) >= sizeof(scratch))
            continue;

        // make short name for the model
        if (!(s = strchr(scratch, '/')))
            continue;
        s++;

        if (!(p = strchr(s, '/')))
            continue;
        *p = 0;

        for (j = 0; j < ndirs; j++) {
            if (!strcmp(dirnames[j], s)) {
                break;
            }
        }

        if (j != ndirs) {
            continue;
        }

        dirnames[ndirs++] = UI_CopyString(s);
        if (ndirs == MAX_PLAYERMODELS) {
            break;
        }
    }

    FS_FreeList((void **)list);

    if (!ndirs) {
        return;
    }

    // go through the subdirectories
    for (i = 0; i < ndirs; i++) {
        int k, s;
        char **pcxnames;
        char **skinnames;
        int npcxfiles;
        int nskins = 0;

        // verify the existence of tris.md2
        Q_concat(scratch, sizeof(scratch), "players/", dirnames[i], "/tris.md2");
        if (!FS_FileExists(scratch)) {
            goto skip;
        }

        // verify the existence of at least one pcx skin
        Q_concat(scratch, sizeof(scratch), "players/", dirnames[i]);
        pcxnames = (char **)FS_ListFiles(scratch, ".pcx", 0, &npcxfiles);
        if (!pcxnames) {
            goto skip;
        }

        // count valid skins, which consist of a skin with a matching "_i" icon
        for (k = 0; k < npcxfiles; k++) {
            if (!strstr(pcxnames[k], "_i.pcx")) {
                if (IconOfSkinExists(pcxnames[k], pcxnames, npcxfiles)) {
                    nskins++;
                }
            }
        }

        if (!nskins) {
            FS_FreeList((void **)pcxnames);
            goto skip;
        }

        skinnames = UI_Malloc(sizeof(char *) * (nskins + 1));
        skinnames[nskins] = NULL;

        // copy the valid skins
        for (s = 0, k = 0; k < npcxfiles; k++) {
            if (!strstr(pcxnames[k], "_i.pcx")) {
                if (IconOfSkinExists(pcxnames[k], pcxnames, npcxfiles)) {
                    COM_StripExtension(scratch, pcxnames[k], sizeof(scratch));
                    skinnames[s++] = UI_CopyString(scratch);
                }
            }
        }

        FS_FreeList((void **)pcxnames);

        // at this point we have a valid player model
        pmi = &uis.pmi[uis.numPlayerModels++];
        pmi->nskins = nskins;
        pmi->skindisplaynames = skinnames;
        pmi->directory = dirnames[i];
        continue;

skip:
        Z_Free(dirnames[i]);
    }

    PlayerModel_LoadQ3();

    qsort(uis.pmi, uis.numPlayerModels, sizeof(uis.pmi[0]), pmicmpfnc);

    PlayerModel_BuildCells();
}

/*
Quake III's models (baseq3, q3player.c): models/players/<model>/ with its three
parts and animation.cfg, and a skin for every lower_<skin>.skin that has its
icon_<skin>.tga - Quake III's own menu shows no other.
*/
static void PlayerModel_LoadQ3(void)
{
    char    scratch[MAX_QPATH];
    char    (*skins)[MAX_QPATH];
    char    **list;
    int     i, j, k, numFiles, numSkins, n;

    if (!CL_Q3PlayersEnabled())
        return;

    list = (char **)FS_ListFiles(NULL, "models/players/*/animation.cfg",
                                 FS_SEARCH_BYFILTER | FS_SEARCH_SAVEPATH, &numFiles);
    if (!list)
        return;

    skins = Z_Malloc(sizeof(*skins) * Q3P_MAX_SKINS);

    for (i = 0; i < numFiles && uis.numPlayerModels < MAX_PLAYERMODELS; i++) {
        char *s, *p;
        playerModelInfo_t *pmi;

        // models/players/<model>/animation.cfg
        if (Q_strlcpy(scratch, list[i], sizeof(scratch)) >= sizeof(scratch))
            continue;
        if (!(s = strchr(scratch, '/')) || !(s = strchr(s + 1, '/')))
            continue;
        s++;
        if (!(p = strchr(s, '/')))
            continue;
        *p = 0;

        for (j = 0; j < uis.numPlayerModels; j++)
            if (!Q_stricmp(uis.pmi[j].directory, s))
                break;
        if (j < uis.numPlayerModels || !Q3P_ModelExists(s))
            continue;

        numSkins = Q3P_SkinList(s, skins, Q3P_MAX_SKINS);
        for (k = n = 0; k < numSkins; k++) {
            char icon[MAX_QPATH];

            if (Q_concat(icon, sizeof(icon), "models/players/", s, "/icon_", skins[k], ".tga") < sizeof(icon)
                && FS_FileExists(icon))
                Q_strlcpy(skins[n++], skins[k], MAX_QPATH);
        }
        if (!n)
            continue;

        pmi = &uis.pmi[uis.numPlayerModels++];
        pmi->q3 = true;
        pmi->nskins = n;
        pmi->skindisplaynames = UI_Malloc(sizeof(char *) * (n + 1));
        for (k = 0; k < n; k++)
            pmi->skindisplaynames[k] = UI_CopyString(skins[k]);
        pmi->skindisplaynames[n] = NULL;
        pmi->directory = UI_CopyString(Q_strlwr(s));
    }

    Z_Free(skins);
    FS_FreeList((void **)list);
}

// skins the game puts on by itself: team uniforms, the spy's disguise
static bool PlayerModel_GridSkip(const playerModelInfo_t *pmi, const char *skin)
{
    if (pmi->q3)
        return !Q_stricmp(skin, "red") || !Q_stricmp(skin, "blue");
    return !Q_stricmp(skin, "ctf_r") || !Q_stricmp(skin, "ctf_b") || !Q_stricmp(skin, "disguise");
}

static void PlayerModel_BuildCells(void)
{
    int i, j, n = 0;
    bool q3 = false;

    for (i = 0; i < uis.numPlayerModels; i++) {
        n += uis.pmi[i].nskins;
        q3 |= uis.pmi[i].q3;
    }

    uis.pmGrid = q3;
    uis.numPmCells = 0;
    uis.pmQ3First = 0;
    if (!q3 || !n)
        return;

    uis.pmCells = UI_Mallocz(sizeof(uis.pmCells[0]) * n);
    uis.pmCellNames = UI_Mallocz(sizeof(uis.pmCellNames[0]) * (n + 1));

    uis.pmQ3First = -1;
    for (i = 0; i < uis.numPlayerModels; i++) {
        const playerModelInfo_t *pmi = &uis.pmi[i];

        for (j = 0; j < pmi->nskins; j++) {
            playerModelCell_t *cell;

            if (PlayerModel_GridSkip(pmi, pmi->skindisplaynames[j]))
                continue;
            if (pmi->q3 && uis.pmQ3First < 0)
                uis.pmQ3First = uis.numPmCells;

            cell = &uis.pmCells[uis.numPmCells];
            cell->model = i;
            cell->skin = j;
            uis.pmCellNames[uis.numPmCells] = UI_CopyString(
                va("%s / %s", pmi->directory, pmi->skindisplaynames[j]));
            uis.numPmCells++;
        }
    }
    if (uis.pmQ3First < 0)
        uis.pmQ3First = uis.numPmCells;
    uis.pmCellNames[uis.numPmCells] = NULL;

    if (!uis.numPmCells)
        uis.pmGrid = false;
}

/*
The cell a "model/skin" (or "model\skin") value is: that skin, or failing
that its model's first cell (a team uniform comes here), then male/grunt's,
then the first.
*/
int PlayerModel_FindCell(const char *value)
{
    char model[MAX_QPATH], *skin;
    int i, first = -1, male = -1;

    Q_strlcpy(model, value, sizeof(model));
    if ((skin = strchr(model, '/')) || (skin = strchr(model, '\\')))
        *skin++ = 0;
    else
        skin = "";

    for (i = 0; i < uis.numPmCells; i++) {
        const playerModelInfo_t *pmi = &uis.pmi[uis.pmCells[i].model];
        const char *s = pmi->skindisplaynames[uis.pmCells[i].skin];

        if (!Q_stricmp(pmi->directory, model)) {
            if (!Q_stricmp(s, skin))
                return i;
            if (first < 0)
                first = i;
        }
        if (male < 0 && !Q_stricmp(pmi->directory, "male") && !Q_stricmp(s, "grunt"))
            male = i;
    }

    if (first >= 0)
        return first;
    return male >= 0 ? male : 0;
}

void PlayerModel_CellValue(int cell, char *buf, size_t size)
{
    const playerModelInfo_t *pmi;

    if (cell < 0 || cell >= uis.numPmCells) {
        Q_strlcpy(buf, "male/grunt", size);
        return;
    }
    pmi = &uis.pmi[uis.pmCells[cell].model];
    Q_concat(buf, size, pmi->directory, "/", pmi->skindisplaynames[uis.pmCells[cell].skin]);
}

// bumped by every map load (src/client/refresh.c); a renderer restart frees the
// cells outright (UI_Shutdown -> PlayerModel_Free)
extern int registration_sequence;

/* Registered the first time the cell is drawn, and again after a map load.
   The end of a map's registration frees every image the map did not register
   again, the icons included, and their slots go to the map's own textures. The
   handles kept from before then drew whatever took the slot: Matt's grid,
   opened after playing a while, came up mostly white boxes with a skin sheet, a
   gun skin and a mask among them (2026-11-17). */
qhandle_t PlayerModel_CellIcon(int cell)
{
    static int icons_sequence;
    playerModelCell_t *c;
    const playerModelInfo_t *pmi;
    char path[MAX_QPATH];

    if (cell < 0 || cell >= uis.numPmCells)
        return 0;
    if (icons_sequence != registration_sequence) {
        for (int i = 0; i < uis.numPmCells; i++)
            uis.pmCells[i].icon = 0;
        icons_sequence = registration_sequence;
    }
    c = &uis.pmCells[cell];
    if (!c->icon) {
        pmi = &uis.pmi[c->model];
        if (pmi->q3)
            Q_concat(path, sizeof(path), "/models/players/", pmi->directory, "/icon_",
                     pmi->skindisplaynames[c->skin], ".tga");
        else
            Q_concat(path, sizeof(path), "/players/", pmi->directory, "/",
                     pmi->skindisplaynames[c->skin], "_i.pcx");
        c->icon = R_RegisterPic2(path);
        if (!c->icon)
            c->icon = -1;
    }
    return c->icon > 0 ? c->icon : 0;
}

void PlayerModel_Free(void)
{
    playerModelInfo_t *pmi;
    int i, j;

    for (i = 0; i < uis.numPmCells; i++)
        Z_Free(uis.pmCellNames[i]);
    Z_Free(uis.pmCellNames);
    Z_Free(uis.pmCells);
    uis.pmCellNames = NULL;
    uis.pmCells = NULL;
    uis.numPmCells = 0;
    uis.pmQ3First = 0;
    uis.pmGrid = false;

    for (i = 0, pmi = uis.pmi; i < uis.numPlayerModels; i++, pmi++) {
        if (pmi->skindisplaynames) {
            for (j = 0; j < pmi->nskins; j++) {
                Z_Free(pmi->skindisplaynames[j]);
            }
            Z_Free(pmi->skindisplaynames);
        }
        Z_Free(pmi->directory);
        memset(pmi, 0, sizeof(*pmi));
    }

    uis.numPlayerModels = 0;
}

