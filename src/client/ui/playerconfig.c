/*
Copyright (C) 2003-2008 Andrey Nazarov

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
#include "../client.h"
#include "shared/bot_roster.h"


/*
=============================================================================

PLAYER MODEL PREVIEW

The turning, lit player model the player setup menu shows beside its
controls, and the bot picker beside its own.

=============================================================================
*/

typedef struct {
    refdef_t    refdef;
    entity_t    entities[2];    // the model and its weapon
    int         num_actors;     // how many of those loaded

    // A Quake III model (q3player.c): its legs, torso, head and the Quake II
    // male's gun, put together each frame by the same code as in the game -
    // standing, a gesture each time another model comes up.
    q3pmodel_t  q3;
    q3pstate_t  q3state;
    qhandle_t   q3weapon;
    bool        q3gesture;

    bool        q1;             // Quake's Ranger (q1player.c): entities[0], his frames

    // An invisible bounce card above and in front of the model (path tracer
    // only): a white quad drawn as a viewer model, so the camera never sees it
    // but bounce and shadow rays do. It sends some of the footlight back down
    // into the shadows. The quad is baked in place in the .md2 (written by a
    // script), so it is drawn at the origin unrotated and does not turn with
    // the model.
    entity_t    card;

    entity_t    scene[Q3P_NUM_PARTS + 1];   // what is handed to the renderer each frame

    int         time;
    int         oldTime;

    // turning the model by dragging it - see Preview_Keydown and Preview_UpdateDrag
    float       yaw;
    bool        dragging;
    int         drag_x;
} playerPreview_t;

// the way the model faces when the menu opens - three-quarters toward the camera
#define PLAYER_DEFAULT_YAW  260.0f

// how far the model turns per unit of mouse travel, in degrees: about a full turn
// across the half of the screen it stands in
#define PLAYER_DRAG_DEGREES 0.5f

extern cvar_t       *vid_rtx;

// The camera sits at the origin looking down +x; the model stands at x 40 with
// its feet at z -24, turned toward -y. Lit like a stage footlight: one white
// light just below its feet and a little in front, throwing light and shadows UP
// the body and face. Upstream's blue rim stays behind it, dimmer, so the
// silhouette still separates from the black. (Upstream's white key came from
// over the camera's shoulder instead.)
//
// Close underneath is what makes it dramatic - moved out in front (about 36
// degrees up at the face) it lit the face but went flat. The model holds its gun
// across its chest, so from here the gun shadows the chest and face; the bounce
// card above the head (playerPreview_t.card) fills that shadow, so the light can
// stay small. A sphere light's colour is its radiance, so its total output grows
// with the radius squared: radius 24 gave 16x the light of radius 6, and with the
// card that was too bright. Keep the sphere clear of the feet: at 24 its edge
// came within ~3 units of the nearest one.
static dlight_t dlights[] = {
	{.origin = { 24.f, -12.f, -48.f },.color = {1.f, 1.f, 1.f},.intensity = 60.f,.radius = 6.f },
	{.origin = { 100.f, 80.f, 20.f },.color = {0.5f, 0.5f, 1.f},.intensity = 120.f,.radius = 20.f }
};

static void Preview_Init(playerPreview_t *pv)
{
    static const vec3_t origin = { 40.0f, 0.0f, 0.0f };
    static const vec3_t angles = { 0.0f, PLAYER_DEFAULT_YAW, 0.0f };
    int i;

    for (i = 0; i < 2; i++) {
        pv->entities[i].flags = RF_FULLBRIGHT;
        pv->entities[i].id = i + 1; // Q2RTX: add entity id to fix motion vectors
        VectorCopy(angles, pv->entities[i].angles);
        VectorCopy(origin, pv->entities[i].origin);
        VectorCopy(origin, pv->entities[i].oldorigin);
    }

    // placed and tilted in the model file itself - origin, no rotation
    pv->card.flags = RF_VIEWERMODEL;
    pv->card.id = 3;

    pv->refdef.num_entities = 0;
    pv->refdef.entities = pv->scene;
    pv->refdef.rdflags = RDF_NOWORLDMODEL;

	if (CL_PathTraced())
	{
		pv->refdef.num_dlights = sizeof(dlights) / sizeof(*dlights);
		pv->refdef.dlights = dlights;
	}
	else
	{
		pv->refdef.num_dlights = 0;
	}
}

// the menu behind the model: see-through on the path tracers, so the model
// shows - all 2D stuff is rendered after 3D, in stretch_pics (D3D12's 2D layer
// too, at R_EndFrame)
static void Preview_MenuColor(menuFrameWork_t *menu)
{
	menu->image = uis.backgroundHandle;
	if (CL_PathTraced())
		menu->color.u32 = 0;
	else
		menu->color.u32 = MakeColor(0, 0, 0, 255);
	menu->transparent = uis.transparent;
}

static void Preview_Load(playerPreview_t *pv, const char *model, const char *skin)
{
    char scratch[MAX_QPATH];
    q3player_t *was_q3 = pv->q3.q3;

    pv->num_actors = 0;
    pv->q1 = false;

    pv->card.model = CL_PathTraced()
        ? R_RegisterModel("models/rtx/bounce_card/tris.md2") : 0;

    // Quake's Ranger: one model in its own skin, his gun in his hands
    if (Q1P_ModelExists(model)) {
        memset(&pv->q3, 0, sizeof(pv->q3));
        pv->entities[0].model = R_RegisterModel(Q1P_MODEL_PATH);
        pv->entities[0].skin = 0;
        if (pv->entities[0].model) {
            pv->num_actors = 1;
            pv->q1 = true;
        }
        return;
    }

    // a Quake III model, which holds the Quake II male's gun
    memset(&pv->q3, 0, sizeof(pv->q3));
    if (CL_Q3PlayersEnabled() && Q3P_Register(&pv->q3, model, skin)) {
        pv->q3weapon = 0;
        if (uis.weaponModel[0]) {
            Q_concat(scratch, sizeof(scratch), "players/male/", uis.weaponModel);
            pv->q3weapon = R_RegisterModel(scratch);
        }
        if (pv->q3.q3 != was_q3)
            pv->q3gesture = true;
        return;
    }

    Q_concat(scratch, sizeof(scratch), "players/", model, "/tris.md2");
    pv->entities[0].model = R_RegisterModel(scratch);
    if (!pv->entities[0].model)
        return;

    pv->num_actors++;

    Q_concat(scratch, sizeof(scratch), "players/", model, "/", skin, ".pcx");
    pv->entities[0].skin = R_RegisterSkin(scratch);

    if (!uis.weaponModel[0])
        return;

    Q_concat(scratch, sizeof(scratch), "players/", model, "/", uis.weaponModel);
    pv->entities[1].model = R_RegisterModel(scratch);
    if (!pv->entities[1].model)
        return;

    pv->num_actors++;
}

static void Preview_RunFrame(playerPreview_t *pv)
{
    int frame;
    int i;

    if (pv->time < uis.realtime) {
        pv->oldTime = pv->time;

        pv->time += 120;
        if (pv->time < uis.realtime) {
            pv->time = uis.realtime;
        }

        frame = (pv->time / 120) % 40;
        // Quake's Ranger stands in his own frames (q1player.c)
        if (pv->q1)
            frame = Q1P_Frame(frame, Q1P_GUN_SHOTGUN);

        for (i = 0; i < pv->num_actors; i++) {
            pv->entities[i].oldframe = pv->entities[i].frame;
            pv->entities[i].frame = frame;
        }
    }
}

static void Preview_SetYaw(playerPreview_t *pv, float yaw)
{
    int i;

    pv->yaw = anglemod(yaw);
    for (i = 0; i < 2; i++)
        pv->entities[i].angles[YAW] = pv->yaw;
}

// every opening starts from the same pose, not wherever the last drag left it,
// with oldframe set up correctly
static void Preview_Reset(playerPreview_t *pv)
{
    pv->dragging = false;
    Preview_SetYaw(pv, PLAYER_DEFAULT_YAW);

    pv->time = uis.realtime - 120;
    pv->oldTime = pv->time;
    Preview_RunFrame(pv);
}

// Follows the mouse while the drag Preview_Keydown started is held. Menus are
// told about key presses but not releases, so the button is polled here, once
// a frame.
static void Preview_UpdateDrag(playerPreview_t *pv)
{
    int x;

    if (!pv->dragging)
        return;

    if (!Key_IsDown(K_MOUSE1)) {
        pv->dragging = false;
        return;
    }

    // Dragging right brings the side facing the camera round to the right, as if
    // the model were grabbed by its front: screen right is -y from this camera,
    // and turning the facing toward -y is an increase in yaw.
    x = uis.mouseCoords[0];
    Preview_SetYaw(pv, pv->yaw + (x - pv->drag_x) * PLAYER_DRAG_DEGREES);
    pv->drag_x = x;
}

// A left click that does not land on a menu item - on the model, or anywhere on
// the empty screen - starts turning the model; Preview_UpdateDrag takes it from
// there.
static menuSound_t Preview_Keydown(playerPreview_t *pv, menuFrameWork_t *self, int key)
{
    if (key == K_MOUSE1 && !Menu_HitTest(self)) {
        pv->dragging = true;
        pv->drag_x = uis.mouseCoords[0];
        return QMS_SILENT;
    }

    return QMS_NOTHANDLED;
}

/*
A Quake III model's parts for this frame. It is animated facing +x, looking
straight ahead, and the whole of it then turned to the preview's yaw: the
game's head-first turn (the torso and legs follow the look with a lag) would
make a drag twist it like a person turning round, not a model on a turntable.
*/
static void Preview_AddQ3(playerPreview_t *pv)
{
    entity_t    base = pv->entities[0], parts[Q3P_NUM_PARTS];
    q3pinput_t  in;
    float       s, c;
    int         i, n;

    memset(&in, 0, sizeof(in));
    in.time = uis.realtime;
    in.q2frame = 0;                 // stand01: standing, legs and torso
    VectorCopy(base.origin, in.origin);
    in.onground = true;
    in.weapon = pv->q3weapon;
    in.gesture = pv->q3gesture;
    pv->q3gesture = false;

    VectorClear(base.angles);
    base.frame = base.oldframe = 0;
    base.backlerp = 0;

    n = Q3P_Animate(&pv->q3state, &pv->q3, &in, &base, parts);

    s = sinf(DEG2RAD(pv->yaw));
    c = cosf(DEG2RAD(pv->yaw));
    for (i = 0; i < n && pv->refdef.num_entities < q_countof(pv->scene) - 1; i++) {
        entity_t *e = &parts[i];
        float x = e->origin[0] - base.origin[0];
        float y = e->origin[1] - base.origin[1];

        e->origin[0] = base.origin[0] + x * c - y * s;
        e->origin[1] = base.origin[1] + x * s + y * c;
        VectorCopy(e->origin, e->oldorigin);
        e->angles[YAW] += pv->yaw;
        pv->scene[pv->refdef.num_entities++] = *e;
    }
}

static void Preview_Draw(playerPreview_t *pv, menuFrameWork_t *self)
{
    float backlerp;
    int i;

    pv->refdef.time = uis.realtime * 0.001f;

    Preview_UpdateDrag(pv);
    Preview_RunFrame(pv);

    if (pv->time == pv->oldTime) {
        backlerp = 0;
    } else {
        backlerp = 1 - (float)(uis.realtime - pv->oldTime) /
                   (float)(pv->time - pv->oldTime);
    }

    pv->refdef.num_entities = 0;
    if (pv->q3.q3)
        Preview_AddQ3(pv);
    for (i = 0; i < pv->num_actors; i++) {
        pv->entities[i].backlerp = backlerp;
        pv->scene[pv->refdef.num_entities++] = pv->entities[i];
    }
    if (pv->card.model)
        pv->scene[pv->refdef.num_entities++] = pv->card;

    Menu_Draw(self);

    R_RenderFrame(&pv->refdef, 0);

    R_SetScale(uis.scale);
}

// the right half of the screen, less a tenth top and bottom
static void Preview_Size(playerPreview_t *pv)
{
    int w = uis.width / uis.scale;
    int h = uis.height / uis.scale;

    pv->refdef.x = w / 2;
    pv->refdef.y = h / 10;
    pv->refdef.width = w / 2;
    pv->refdef.height = h - h / 5;

    pv->refdef.fov_x = 90;
    pv->refdef.fov_y = V_CalcFov(pv->refdef.fov_x,
                                 pv->refdef.width, pv->refdef.height);
}

// a "model/skin" (or "model\skin") cvar value as indexes into uis.pmi, or
// male/grunt's - or the first model's - when it is not installed
static void Preview_FindSkin(const char *value, int *model, int *skin)
{
    char directory[MAX_QPATH], name[MAX_QPATH];
    char *p;
    int i, j;

    *model = *skin = 0;

    Q_strlcpy(directory, value, sizeof(directory));
    if ((p = strchr(directory, '/')) || (p = strchr(directory, '\\'))) {
        *p++ = 0;
        Q_strlcpy(name, p, sizeof(name));
    } else {
        strcpy(directory, "male");
        strcpy(name, "grunt");
    }

    for (i = 0; i < uis.numPlayerModels; i++) {
        if (Q_stricmp(uis.pmi[i].directory, directory) == 0) {
            *model = i;

            for (j = 0; j < uis.pmi[i].nskins; j++) {
                if (Q_stricmp(uis.pmi[i].skindisplaynames[j], name) == 0) {
                    *skin = j;
                    break;
                }
            }
        }
    }
}


/*
=============================================================================

THE MODEL GRID

Every model and skin as a thumbnail, on the left half of the screen with the
model turning on the right (see playerModelCell_t in ui.h for what is in it).
Quake's come first, then Quake II's, Quake III's and Team Arena's, each under
its own heading. Arrows,
Home/End and Page Up/Down move the choice, the wheel scrolls, a click picks;
Enter, a double click or Escape goes back with it.

=============================================================================
*/

typedef struct {
    int         first, count;   // cells; count 0 is a heading
    const char  *heading;
} gridRow_t;

typedef struct {
    int         sel;            // the cell chosen
    int         hover;          // the cell under the pointer, -1 none
    int         scroll;         // the first row shown
    int         cols, icon, pitch;
    vrect_t     area;           // where the rows go
    int         rows_shown;     // how many fitted last time it was drawn

    gridRow_t   *rows;
    int         numrows, maxrows;

    int         click_time, click_cell;
} playerGrid_t;

#define GRID_HEADING_HEIGHT (CHAR_HEIGHT * 2)

// what Grid_Key did
#define GRID_UNHANDLED  0
#define GRID_HANDLED    1
#define GRID_CHANGED    2       // another cell chosen
#define GRID_DONE       3       // Enter or a double click: go back with it

static int Grid_RowHeight(const playerGrid_t *g, int row)
{
    return g->rows[row].count ? g->pitch : GRID_HEADING_HEIGHT;
}

static void Grid_AddRow(playerGrid_t *g, int first, int count, const char *heading)
{
    if (g->numrows == g->maxrows)
        return;
    g->rows[g->numrows].first = first;
    g->rows[g->numrows].count = count;
    g->rows[g->numrows].heading = heading;
    g->numrows++;
}

// the rows for this many columns: a heading over each game's models, when
// there are both
static void Grid_BuildRows(playerGrid_t *g)
{
    static const char *const headings[4] = { "QUAKE", "QUAKE II", "QUAKE III ARENA", "TEAM ARENA" };
    const int n = uis.numPmCells;
    // each game's cells: Quake's, Quake II's, Quake III's, Team Arena's
    // (playermodels.c)
    const int start[5] = { 0, uis.pmQ2First, uis.pmQ3First, uis.pmTAFirst, n };
    int i, k, games = 0;

    for (k = 0; k < 4; k++)
        games += start[k + 1] > start[k];

    // a heading and a part row at most for each game, past n / cols full rows
    if (g->maxrows < n / g->cols + 10) {
        Z_Free(g->rows);
        g->maxrows = n / g->cols + 10;
        g->rows = UI_Malloc(sizeof(g->rows[0]) * g->maxrows);
    }
    g->numrows = 0;

    for (k = 0; k < 4; k++) {
        if (start[k + 1] <= start[k])
            continue;
        if (games > 1)
            Grid_AddRow(g, start[k], 0, headings[k]);
        for (i = start[k]; i < start[k + 1]; i += g->cols)
            Grid_AddRow(g, i, min(g->cols, start[k + 1] - i), NULL);
    }
}

static int Grid_RowOf(const playerGrid_t *g, int cell)
{
    int r;

    for (r = 0; r < g->numrows; r++)
        if (g->rows[r].count && cell >= g->rows[r].first && cell < g->rows[r].first + g->rows[r].count)
            return r;
    return 0;
}

// do rows from..to (inclusive) fit in the area?
static bool Grid_Fits(const playerGrid_t *g, int from, int to)
{
    int r, h = 0;

    for (r = from; r <= to && r < g->numrows; r++)
        h += Grid_RowHeight(g, r);
    return h <= g->area.height;
}

static void Grid_ShowSelection(playerGrid_t *g)
{
    int r = Grid_RowOf(g, g->sel);

    if (r <= g->scroll) {
        // its heading along with it
        g->scroll = (r > 0 && !g->rows[r - 1].count) ? r - 1 : r;
    } else {
        while (g->scroll < r && !Grid_Fits(g, g->scroll, r))
            g->scroll++;
    }
}

/*
The grid in the rectangle given, in the menu's (scaled) units: as many
64-unit thumbnails across as fit, smaller ones on a small screen.
*/
static void Grid_Layout(playerGrid_t *g, int x, int y, int w, int h)
{
    g->icon = (w >= 4 * 76 && h >= 3 * 76) ? 64 : 48;
    g->pitch = g->icon + g->icon / 6 + 2;
    g->cols = w / g->pitch;
    clamp(g->cols, 2, 12);

    g->area.x = x + (w - g->cols * g->pitch) / 2 + (g->pitch - g->icon) / 2;
    g->area.y = y;
    g->area.width = g->cols * g->pitch;
    g->area.height = h;

    Grid_BuildRows(g);
    Grid_ShowSelection(g);
}

static void Grid_Init(playerGrid_t *g, int cell)
{
    g->sel = cell;
    clamp(g->sel, 0, max(uis.numPmCells - 1, 0));
    g->hover = -1;
    g->scroll = 0;
    g->click_time = 0;
    g->click_cell = -1;
    if (g->numrows)
        Grid_ShowSelection(g);
}

static void Grid_Free(playerGrid_t *g)
{
    Z_Free(g->rows);
    g->rows = NULL;
    g->numrows = g->maxrows = 0;
}

static void Grid_Frame(int x, int y, int w, int h, int t, uint32_t color)
{
    R_DrawFill32(x, y, w, t, color);
    R_DrawFill32(x, y + h - t, w, t, color);
    R_DrawFill32(x, y + t, t, h - 2 * t, color);
    R_DrawFill32(x + w - t, y + t, t, h - 2 * t, color);
}

static void Grid_Draw(playerGrid_t *g)
{
    const uint32_t  chosen = MakeColor(15, 128, 235, 255);
    const uint32_t  under = MakeColor(15, 128, 235, 120);
    const uint32_t  well = MakeColor(0, 0, 0, 110);
    int             r, k, y = g->area.y, shown = 0;

    g->hover = -1;

    for (r = g->scroll; r < g->numrows; r++) {
        const gridRow_t *row = &g->rows[r];
        int h = Grid_RowHeight(g, r);

        if (y + h > g->area.y + g->area.height)
            break;

        if (!row->count) {
            UI_DrawString(g->area.x - (g->pitch - g->icon) / 2, y + GRID_HEADING_HEIGHT - CHAR_HEIGHT - 3,
                          UI_LEFT | UI_ALTCOLOR, row->heading);
            y += h;
            shown++;
            continue;
        }

        for (k = 0; k < row->count; k++) {
            int         cell = row->first + k;
            int         x = g->area.x + k * g->pitch;
            vrect_t     rc = { x, y, g->icon, g->icon };
            qhandle_t   pic = PlayerModel_CellIcon(cell);

            R_DrawFill32(x - 1, y - 1, g->icon + 2, g->icon + 2, well);
            if (pic) {
                R_DrawStretchPic(x, y, g->icon, g->icon, pic);
            } else {
                // no icon: its model's name, as far as it fits
                const playerModelInfo_t *pmi = &uis.pmi[uis.pmCells[cell].model];
                char name[16];

                Q_strlcpy(name, pmi->directory, min(sizeof(name), (size_t)(g->icon / CHAR_WIDTH) + 1));
                UI_DrawString(x + g->icon / 2, y + g->icon / 2 - CHAR_HEIGHT / 2, UI_CENTER, name);
            }

            if (UI_CursorInRect(&rc))
                g->hover = cell;

            if (cell == g->sel)
                Grid_Frame(x - 3, y - 3, g->icon + 6, g->icon + 6, 2, chosen);
            else if (cell == g->hover)
                Grid_Frame(x - 2, y - 2, g->icon + 4, g->icon + 4, 1, under);
        }
        y += h;
        shown++;
    }
    g->rows_shown = shown;

    // where in the list this is, when it does not all fit
    if (g->scroll > 0 || g->scroll + shown < g->numrows) {
        int bx = g->area.x + g->area.width - (g->pitch - g->icon) / 2 + 2;
        int bh = g->area.height;
        int top = bh * g->scroll / g->numrows;
        int len = max(bh * shown / g->numrows, 8);

        R_DrawFill32(bx, g->area.y, 3, bh, MakeColor(255, 255, 255, 30));
        R_DrawFill32(bx, g->area.y + min(top, bh - len), 3, len, chosen);
    }
}

// the cell above or below `from` in the next row of cells that way, same column
static int Grid_Vertical(const playerGrid_t *g, int from, int dir)
{
    int r = Grid_RowOf(g, from), col = from - g->rows[r].first;

    for (r += dir; r >= 0 && r < g->numrows; r += dir) {
        if (g->rows[r].count)
            return g->rows[r].first + min(col, g->rows[r].count - 1);
    }
    return from;
}

static int Grid_Key(playerGrid_t *g, int key)
{
    int sel = g->sel, n = uis.numPmCells, i;
    int page = max(g->rows_shown - 1, 1);

    if (!n)
        return GRID_UNHANDLED;

    switch (key) {
    case K_LEFTARROW:
    case K_KP_LEFTARROW:
        sel--;
        break;
    case K_RIGHTARROW:
    case K_KP_RIGHTARROW:
        sel++;
        break;
    case K_UPARROW:
    case K_KP_UPARROW:
        sel = Grid_Vertical(g, sel, -1);
        break;
    case K_DOWNARROW:
    case K_KP_DOWNARROW:
        sel = Grid_Vertical(g, sel, 1);
        break;
    case K_HOME:
    case K_KP_HOME:
        sel = 0;
        break;
    case K_END:
    case K_KP_END:
        sel = n - 1;
        break;
    case K_PGUP:
    case K_KP_PGUP:
        for (i = 0; i < page; i++)
            sel = Grid_Vertical(g, sel, -1);
        break;
    case K_PGDN:
    case K_KP_PGDN:
        for (i = 0; i < page; i++)
            sel = Grid_Vertical(g, sel, 1);
        break;
    case K_MWHEELUP:
        if (g->scroll > 0)
            g->scroll--;
        return GRID_HANDLED;
    case K_MWHEELDOWN:
        if (g->scroll + g->rows_shown < g->numrows)
            g->scroll++;
        return GRID_HANDLED;
    case K_MOUSE1:
        if (g->hover < 0)
            return GRID_UNHANDLED;      // the preview's: turning the model
        if (g->hover == g->click_cell && uis.realtime - g->click_time < DOUBLE_CLICK_DELAY) {
            g->sel = g->hover;
            return GRID_DONE;
        }
        g->click_cell = g->hover;
        g->click_time = uis.realtime;
        sel = g->hover;
        break;
    case K_ENTER:
    case K_KP_ENTER:
        return GRID_DONE;
    default:
        return GRID_UNHANDLED;
    }

    clamp(sel, 0, n - 1);
    if (sel == g->sel)
        return GRID_HANDLED;
    g->sel = sel;
    Grid_ShowSelection(g);
    return GRID_CHANGED;
}

/*
A page made of the grid and the preview: the "playermodel" page, and the bot
picker when there are Quake III models. start is the cell it opened on, so
leaving it untouched writes nothing - a skin it does not show (a team
uniform) stays as it was.
*/
typedef struct {
    playerGrid_t    grid;
    playerPreview_t pv;
    int             start;
    const char      *heading;       // over the grid
    char            name[64];       // the choice's model, or the bot's name
    char            skin[64];       // and its skin
} gridPage_t;

static void GridPage_Load(gridPage_t *gp)
{
    const playerModelCell_t *cell = &uis.pmCells[gp->grid.sel];
    const playerModelInfo_t *pmi = &uis.pmi[cell->model];

    Preview_Load(&gp->pv, pmi->directory, pmi->skindisplaynames[cell->skin]);

    Q_strlcpy(gp->name, pmi->directory, sizeof(gp->name));
    Q_strlwr(gp->name);
    if (gp->name[0] >= 'a' && gp->name[0] <= 'z')
        gp->name[0] -= 'a' - 'A';
    Q_strlcpy(gp->skin, pmi->skindisplaynames[cell->skin], sizeof(gp->skin));
}

// opens on the cell for `value` ("model/skin"), or on cell `cell` when value is NULL
static bool GridPage_Push(gridPage_t *gp, const char *value, int cell)
{
    if (!uis.numPlayerModels)
        PlayerModel_Load();
    if (!uis.pmGrid || !uis.numPmCells)
        return false;

    gp->start = value ? PlayerModel_FindCell(value) : cell;
    clamp(gp->start, 0, uis.numPmCells - 1);
    Grid_Init(&gp->grid, gp->start);
    GridPage_Load(gp);
    Preview_Reset(&gp->pv);
    return true;
}

static void GridPage_Size(gridPage_t *gp)
{
    int w = uis.width, h = uis.height;
    int top = h / 10 + CHAR_HEIGHT * 3;
    int bottom = h - h / 10 - CHAR_HEIGHT * 4;

    Preview_Size(&gp->pv);
    Grid_Layout(&gp->grid, CHAR_WIDTH * 2, top, w / 2 - CHAR_WIDTH * 4, bottom - top);
}

static void GridPage_Draw(gridPage_t *gp, menuFrameWork_t *self)
{
    int cx = uis.width / 4;
    int y = gp->grid.area.y + gp->grid.area.height + CHAR_HEIGHT;

    // Preview_Draw draws the menu (its background and status) and then the
    // model over it; the grid goes on top of both
    Preview_Draw(&gp->pv, self);

    UI_DrawString(cx, uis.height / 10, UI_CENTER | UI_ALTCOLOR, gp->heading);
    Grid_Draw(&gp->grid);

    UI_DrawString(cx, y, UI_CENTER | UI_ALTCOLOR, gp->name);
    UI_DrawString(cx, y + CHAR_HEIGHT + 2, UI_CENTER, gp->skin);
}

static menuSound_t GridPage_Keydown(gridPage_t *gp, menuFrameWork_t *self, int key)
{
    switch (Grid_Key(&gp->grid, key)) {
    case GRID_CHANGED:
        GridPage_Load(gp);
        return QMS_MOVE;
    case GRID_DONE:
        UI_PopMenu();
        return QMS_OUT;
    case GRID_HANDLED:
        return QMS_SILENT;
    default:
        return Preview_Keydown(&gp->pv, self, key);
    }
}

#define GRID_STATUS "arrows / click: choose   enter / esc: done   drag: turn it"


/*
=============================================================================

PLAYER MODEL PAGE

The grid for your own model, opened from Player Setup's model row. Leaving
it sets "skin"; Player Setup picks the new one up when it comes back.

=============================================================================
*/

// in Player Setup, below
static bool PlayerSetup_GridCell(int *cell);
static void PlayerSetup_GridChosen(int cell);

typedef struct {
    menuFrameWork_t menu;
    gridPage_t      gp;
} m_pmgrid_t;

static m_pmgrid_t   m_pmgrid;

static bool PMGrid_Push(menuFrameWork_t *self)
{
    char current[MAX_QPATH];
    int cell;

    m_pmgrid.gp.heading = "PLAYER MODEL";

    // from Player Setup: on its row's choice, which the arrows may have
    // moved without setting "skin" yet
    if (PlayerSetup_GridCell(&cell))
        return GridPage_Push(&m_pmgrid.gp, NULL, cell);

    Cvar_VariableStringBuffer("skin", current, sizeof(current));
    return GridPage_Push(&m_pmgrid.gp, current, 0);
}

/*
A new choice sets "skin" - and goes back to Player Setup's row when it is
underneath, which then has nothing more to set. (When every menu closes at
once, Player Setup's pop runs first, so this one's "skin" is the one kept.)
*/
static void PMGrid_Pop(menuFrameWork_t *self)
{
    char value[MAX_QPATH];
    int sel = m_pmgrid.gp.grid.sel;

    if (sel == m_pmgrid.gp.start)
        return;
    PlayerModel_CellValue(sel, value, sizeof(value));
    Cvar_SetEx("skin", value, FROM_CONSOLE);
    PlayerSetup_GridChosen(sel);
}

static void PMGrid_Size(menuFrameWork_t *self)
{
    GridPage_Size(&m_pmgrid.gp);
}

static void PMGrid_Draw(menuFrameWork_t *self)
{
    GridPage_Draw(&m_pmgrid.gp, self);
}

static menuSound_t PMGrid_Keydown(menuFrameWork_t *self, int key)
{
    return GridPage_Keydown(&m_pmgrid.gp, self, key);
}

static void PMGrid_Free(menuFrameWork_t *self)
{
    Grid_Free(&m_pmgrid.gp.grid);
    Z_Free(m_pmgrid.menu.items);
    memset(&m_pmgrid, 0, sizeof(m_pmgrid));
}

void M_Menu_PlayerModelGrid(void)
{
    m_pmgrid.menu.name = "playermodel";
    m_pmgrid.menu.push = PMGrid_Push;
    m_pmgrid.menu.pop = PMGrid_Pop;
    m_pmgrid.menu.size = PMGrid_Size;
    m_pmgrid.menu.draw = PMGrid_Draw;
    m_pmgrid.menu.keydown = PMGrid_Keydown;
    m_pmgrid.menu.free = PMGrid_Free;
    m_pmgrid.menu.status = GRID_STATUS;
    Preview_MenuColor(&m_pmgrid.menu);

    Preview_Init(&m_pmgrid.gp.pv);

    List_Append(&ui_menus, &m_pmgrid.menu.entry);
}


/*
=============================================================================

PLAYER CONFIG MENU

=============================================================================
*/

#define ID_MODEL 103
#define ID_SKIN    104

typedef struct m_player_s {
    menuFrameWork_t     menu;
    menuField_t         name;
    menuSpinControl_t   model;
    menuSpinControl_t   skin;
	menuSpinControl_t   hand;
    menuSpinControl_t   aimfix;
    menuSpinControl_t   weapswitch;
    menuSpinControl_t   view;

    playerPreview_t     pv;

    // the skin last chosen for each model (uis.pmi index), so stepping to another
    // model and back brings its skin back instead of the first one in the list.
    // Kept across openings of the menu; Free clears it with the rest.
    int         model_skin[MAX_PLAYERMODELS];

    char *pmnames[MAX_PLAYERMODELS];

    // With Quake III models (uis.pmGrid) the model row is every model and
    // skin - the model grid's cells - and Enter on it opens the grid page;
    // the skin row is hidden. grid_start is the cell it opened on.
    bool        grid;
    int         grid_start;
} m_player_t;

static m_player_t    m_player;

// Maps onto the two rerelease cvars the game DLL reads. Only has an effect in
// `game rerelease`; stock baseq2 always uses classic timing.
static const char *weapon_switch_modes[] = {
    "classic",
    "fast",
    "instant",
    NULL
};

static const char *handedness[] = {
    "right",
    "left",
    "center",
    NULL
};

static const char *aiming_points[] = {
    "default",
    "at crosshair",
    NULL
};

static const char *viewmodes[] = {
	"nothing",
	"just the gun",
	"first person model",
	"third person",
	NULL
};

static void ReloadMedia(void)
{
    if (m_player.grid) {
        const playerModelCell_t *cell = &uis.pmCells[m_player.model.curvalue];

        Preview_Load(&m_player.pv, uis.pmi[cell->model].directory,
                     uis.pmi[cell->model].skindisplaynames[cell->skin]);
        return;
    }

    Preview_Load(&m_player.pv, uis.pmi[m_player.model.curvalue].directory,
                 uis.pmi[m_player.model.curvalue].skindisplaynames[m_player.skin.curvalue]);
}

// the cell on the model row, when Player Setup is open with the grid
// (the model page asks as it opens over it)
static bool PlayerSetup_GridCell(int *cell)
{
    int i;

    if (!m_player.grid)
        return false;
    for (i = 0; i < uis.menuDepth; i++) {
        if (uis.layers[i] == &m_player.menu) {
            *cell = m_player.model.curvalue;
            return true;
        }
    }
    return false;
}

// the model page set "skin" to this cell: the row shows it, and Pop has
// nothing more to set
static void PlayerSetup_GridChosen(int cell)
{
    int open;

    if (!PlayerSetup_GridCell(&open))
        return;
    m_player.model.curvalue = m_player.grid_start = cell;
    ReloadMedia();
}

static menuSound_t Keydown(menuFrameWork_t *self, int key)
{
    return Preview_Keydown(&m_player.pv, self, key);
}

static void Draw(menuFrameWork_t *self)
{
    Preview_Draw(&m_player.pv, self);
}

static void Size(menuFrameWork_t *self)
{
    int h;
    int x = uis.width / 2;
    int y = uis.height / 2 - MENU_SPACING * 5 / 2;

    Preview_Size(&m_player.pv);

    if (uis.width < 800 && uis.width >= 640) {
        x -= CHAR_WIDTH * 10;
    }

    if (m_player.menu.banner) {
        h = GENERIC_SPACING(m_player.menu.banner_rc.height);
        m_player.menu.banner_rc.x = x - m_player.menu.banner_rc.width / 2;
        m_player.menu.banner_rc.y = y - h / 2;
        y += h / 2;
    }

    if (uis.width < 640) {
        x -= CHAR_WIDTH * 10;
        m_player.hand.generic.name = "hand";
    } else {
        m_player.hand.generic.name = "handedness";
    }

    m_player.name.generic.x     = x;
    m_player.name.generic.y     = y;
    y += MENU_SPACING * 2;

    m_player.model.generic.x    = x;
    m_player.model.generic.y    = y;
    y += MENU_SPACING;

    m_player.skin.generic.x     = x;
    m_player.skin.generic.y     = y;
    if (!(m_player.skin.generic.flags & QMF_HIDDEN))
        y += MENU_SPACING;

    m_player.hand.generic.x     = x;
    m_player.hand.generic.y     = y;
	y += MENU_SPACING;

	m_player.aimfix.generic.x   = x;
    m_player.aimfix.generic.y   = y;
    y += MENU_SPACING;

    m_player.weapswitch.generic.x   = x;
    m_player.weapswitch.generic.y   = y;
    y += MENU_SPACING;

	m_player.view.generic.x     = x;
	m_player.view.generic.y     = y;
}

static menuSound_t Change(menuCommon_t *self)
{
    // the arrows step through the grid's cells
    if (m_player.grid) {
        if (self->id == ID_MODEL)
            ReloadMedia();
        return QMS_MOVE;
    }

    switch (self->id) {
    case ID_MODEL:
        m_player.skin.itemnames =
            uis.pmi[m_player.model.curvalue].skindisplaynames;
        m_player.skin.curvalue = m_player.model_skin[m_player.model.curvalue];
        if (m_player.skin.curvalue < 0 || m_player.skin.curvalue >= uis.pmi[m_player.model.curvalue].nskins)
            m_player.skin.curvalue = 0;
        SpinControl_Init(&m_player.skin);
        // fall through
    case ID_SKIN:
        m_player.model_skin[m_player.model.curvalue] = m_player.skin.curvalue;
        ReloadMedia();
        break;
    default:
        break;
    }
    return QMS_MOVE;
}

static void Pop(menuFrameWork_t *self)
{
    char scratch[MAX_OSPATH];

    Cvar_SetEx("name", m_player.name.field.text, FROM_CONSOLE);

    if (m_player.grid) {
        // untouched, the skin stays - it may be one the grid does not show
        if (m_player.model.curvalue != m_player.grid_start) {
            PlayerModel_CellValue(m_player.model.curvalue, scratch, sizeof(scratch));
            Cvar_SetEx("skin", scratch, FROM_CONSOLE);
        }
    } else {
        Q_concat(scratch, sizeof(scratch),
                 uis.pmi[m_player.model.curvalue].directory, "/",
                 uis.pmi[m_player.model.curvalue].skindisplaynames[m_player.skin.curvalue]);

        Cvar_SetEx("skin", scratch, FROM_CONSOLE);
    }

	Cvar_SetEx("hand", va("%d", m_player.hand.curvalue), FROM_CONSOLE);

	Cvar_SetEx("aimfix", va("%d", m_player.aimfix.curvalue), FROM_CONSOLE);

    // one control, two cvars - id keeps them separate and so do we
    Cvar_SetEx("g_quick_weapon_switch",
               va("%d", m_player.weapswitch.curvalue == 1), FROM_CONSOLE);
    Cvar_SetEx("g_instant_weapon_switch",
               va("%d", m_player.weapswitch.curvalue == 2), FROM_CONSOLE);

	Cvar_SetEx("cl_player_model", va("%d", m_player.view.curvalue), FROM_CONSOLE);
}

static bool Push(menuFrameWork_t *self)
{
    char currentskin[MAX_QPATH];
    int i;
    int currentdirectoryindex;
    int currentskinindex;

    // find and register all player models
    if (!uis.numPlayerModels) {
        PlayerModel_Load();
        if (!uis.numPlayerModels) {
            Com_Printf("No player models found.\n");
            return false;
        }
    }

    Cvar_VariableStringBuffer("skin", currentskin, sizeof(currentskin));
    Preview_FindSkin(currentskin, &currentdirectoryindex, &currentskinindex);

    for (i = 0; i < uis.numPlayerModels; i++) {
        m_player.pmnames[i] = uis.pmi[i].directory;
    }

    IF_Init(&m_player.name.field, m_player.name.width, m_player.name.width);
    IF_Replace(&m_player.name.field, Cvar_VariableString("name"));

    m_player.grid = uis.pmGrid;
    if (m_player.grid) {
        // one row for model and skin, which Enter opens the grid page from
        m_player.grid_start = PlayerModel_FindCell(currentskin);
        m_player.model.curvalue = m_player.grid_start;
        m_player.model.itemnames = uis.pmCellNames;
        m_player.model.cmd = "pushmenu playermodel";
        m_player.model.generic.status = "enter: all models as pictures   left / right: step through them";
        m_player.skin.generic.flags |= QMF_HIDDEN;
        if (m_player.skin.generic.flags & QMF_HASFOCUS) {
            m_player.skin.generic.flags &= ~QMF_HASFOCUS;
            m_player.model.generic.flags |= QMF_HASFOCUS;
        }
        m_player.skin.itemnames = uis.pmi[currentdirectoryindex].skindisplaynames;
        m_player.skin.curvalue = 0;
    } else {
        m_player.model.curvalue = currentdirectoryindex;
        m_player.model.itemnames = m_player.pmnames;
        m_player.model.cmd = NULL;
        m_player.model.generic.status = NULL;
        m_player.skin.generic.flags &= ~QMF_HIDDEN;

        m_player.skin.curvalue = currentskinindex;
        m_player.skin.itemnames = uis.pmi[currentdirectoryindex].skindisplaynames;
        m_player.model_skin[currentdirectoryindex] = currentskinindex;
    }

    m_player.hand.curvalue = Cvar_VariableInteger("hand");
    clamp(m_player.hand.curvalue, 0, 2);

    m_player.aimfix.curvalue = Cvar_VariableInteger("aimfix");
    clamp(m_player.aimfix.curvalue, 0, 1);

    // instant wins if both are set, matching how the game code tests them
    if (Cvar_VariableInteger("g_instant_weapon_switch"))
        m_player.weapswitch.curvalue = 2;
    else if (Cvar_VariableInteger("g_quick_weapon_switch"))
        m_player.weapswitch.curvalue = 1;
    else
        m_player.weapswitch.curvalue = 0;

	m_player.view.curvalue = Cvar_VariableInteger("cl_player_model");
	clamp(m_player.view.curvalue, 0, 3);

    m_player.menu.banner = R_RegisterPic("m_banner_plauer_setup");
    if (m_player.menu.banner) {
        R_GetPicSize(&m_player.menu.banner_rc.width,
                     &m_player.menu.banner_rc.height, m_player.menu.banner);
        m_player.menu.title = NULL;
    } else {
        m_player.menu.title = "Player Setup";
    }

    ReloadMedia();
    Preview_Reset(&m_player.pv);

    return true;
}

static void Free(menuFrameWork_t *self)
{
    Z_Free(m_player.menu.items);
    memset(&m_player, 0, sizeof(m_player));
}

void M_Menu_PlayerConfig(void)
{
    m_player.menu.name = "players";
    m_player.menu.push = Push;
    m_player.menu.pop = Pop;
    m_player.menu.size = Size;
    m_player.menu.draw = Draw;
    m_player.menu.keydown = Keydown;
    m_player.menu.free = Free;
    Preview_MenuColor(&m_player.menu);

    Preview_Init(&m_player.pv);

    m_player.name.generic.type = MTYPE_FIELD;
    m_player.name.generic.flags = QMF_HASFOCUS;
    m_player.name.generic.name = "name";
    m_player.name.width = MAX_CLIENT_NAME - 1;

    m_player.model.generic.type = MTYPE_SPINCONTROL;
    m_player.model.generic.id = ID_MODEL;
    m_player.model.generic.name = "model";
    m_player.model.generic.change = Change;

    m_player.skin.generic.type = MTYPE_SPINCONTROL;
    m_player.skin.generic.id = ID_SKIN;
    m_player.skin.generic.name = "skin";
    m_player.skin.generic.change = Change;

    m_player.hand.generic.type = MTYPE_SPINCONTROL;
    m_player.hand.generic.name = "handedness";
    m_player.hand.itemnames = (char **)handedness;

    m_player.weapswitch.generic.type = MTYPE_SPINCONTROL;
    m_player.weapswitch.generic.name = "weapon switch";
    m_player.weapswitch.generic.status = "rerelease campaign only: fast raises and lowers weapons at double rate, instant skips the animation";
    m_player.weapswitch.itemnames = (char**)weapon_switch_modes;

    m_player.aimfix.generic.type = MTYPE_SPINCONTROL;
    m_player.aimfix.generic.name = "aiming point";
    m_player.aimfix.itemnames = (char**)aiming_points;

	m_player.view.generic.type = MTYPE_SPINCONTROL;
	m_player.view.generic.name = "view";
	m_player.view.itemnames = (char **)viewmodes;

    Menu_AddItem(&m_player.menu, &m_player.name);
    Menu_AddItem(&m_player.menu, &m_player.model);
    Menu_AddItem(&m_player.menu, &m_player.skin);
    Menu_AddItem(&m_player.menu, &m_player.hand);
    Menu_AddItem(&m_player.menu, &m_player.aimfix);
    Menu_AddItem(&m_player.menu, &m_player.weapswitch);
	Menu_AddItem(&m_player.menu, &m_player.view);

    List_Append(&ui_menus, &m_player.menu.entry);
}


/*
=============================================================================

BOT PICKER

What one bot of the Bot Match line-up wears, picked like your own model: the
model and skin beside the model itself. The page's bot rows open it ("set
_ui_botslot N; pushmenu botpick"); leaving it writes bot_skinN, and the bot
goes by that skin's name, which the title shows as it changes. Every
installed model is here, custom ones too - only the random line-up keeps to
the stock skins (Bot_SkinUsable).

=============================================================================
*/

#define ID_BOT_MODEL    203
#define ID_BOT_SKIN     204

typedef struct {
    menuFrameWork_t     menu;
    menuSpinControl_t   model;
    menuSpinControl_t   skin;

    playerPreview_t     pv;

    int         slot;           // 1 to BOT_MAX_MATCH
    char        title[64];
    int         model_skin[MAX_PLAYERMODELS];
    char        *pmnames[MAX_PLAYERMODELS];

    // with Quake III models (uis.pmGrid): the model grid instead of the two
    // spin controls, the bot's name over it
    bool        grid;
    gridPage_t  gp;
} m_botpick_t;

static m_botpick_t  m_botpick;

static void BotPick_GridTitle(void)
{
    char skin[MAX_QPATH], name[32];

    PlayerModel_CellValue(m_botpick.gp.grid.sel, skin, sizeof(skin));
    Bot_SkinName(skin, name, sizeof(name));
    Q_snprintf(m_botpick.title, sizeof(m_botpick.title), "BOT %d: %s", m_botpick.slot, name);
    Q_strupr(m_botpick.title);
    m_botpick.gp.heading = m_botpick.title;
}

static void BotPick_Update(void)
{
    char skin[MAX_QPATH], name[32];

    Q_concat(skin, sizeof(skin), uis.pmi[m_botpick.model.curvalue].directory, "/",
             uis.pmi[m_botpick.model.curvalue].skindisplaynames[m_botpick.skin.curvalue]);
    Bot_SkinName(skin, name, sizeof(name));
    Q_snprintf(m_botpick.title, sizeof(m_botpick.title), "%s", name);
    m_botpick.menu.title = m_botpick.title;

    Preview_Load(&m_botpick.pv, uis.pmi[m_botpick.model.curvalue].directory,
                 uis.pmi[m_botpick.model.curvalue].skindisplaynames[m_botpick.skin.curvalue]);
}

static menuSound_t BotPick_Keydown(menuFrameWork_t *self, int key)
{
    if (m_botpick.grid) {
        menuSound_t sound = GridPage_Keydown(&m_botpick.gp, self, key);

        BotPick_GridTitle();
        return sound;
    }
    return Preview_Keydown(&m_botpick.pv, self, key);
}

static void BotPick_Draw(menuFrameWork_t *self)
{
    if (m_botpick.grid)
        GridPage_Draw(&m_botpick.gp, self);
    else
        Preview_Draw(&m_botpick.pv, self);
}

static void BotPick_Size(menuFrameWork_t *self)
{
    int x = uis.width / 2;
    int y = uis.height / 2 - MENU_SPACING;

    if (m_botpick.grid) {
        GridPage_Size(&m_botpick.gp);
        return;
    }

    Preview_Size(&m_botpick.pv);

    if (uis.width < 800) {
        x -= CHAR_WIDTH * 10;
    }

    m_botpick.model.generic.x = x;
    m_botpick.model.generic.y = y;
    y += MENU_SPACING;

    m_botpick.skin.generic.x = x;
    m_botpick.skin.generic.y = y;
}

static menuSound_t BotPick_Change(menuCommon_t *self)
{
    switch (self->id) {
    case ID_BOT_MODEL:
        m_botpick.skin.itemnames =
            uis.pmi[m_botpick.model.curvalue].skindisplaynames;
        m_botpick.skin.curvalue = m_botpick.model_skin[m_botpick.model.curvalue];
        if (m_botpick.skin.curvalue < 0 || m_botpick.skin.curvalue >= uis.pmi[m_botpick.model.curvalue].nskins)
            m_botpick.skin.curvalue = 0;
        SpinControl_Init(&m_botpick.skin);
        // fall through
    case ID_BOT_SKIN:
        m_botpick.model_skin[m_botpick.model.curvalue] = m_botpick.skin.curvalue;
        BotPick_Update();
        break;
    default:
        break;
    }
    return QMS_MOVE;
}

static void BotPick_Pop(menuFrameWork_t *self)
{
    char scratch[MAX_QPATH];

    if (m_botpick.grid) {
        // untouched, the skin stays - it may be one the grid does not show
        if (m_botpick.gp.grid.sel != m_botpick.gp.start) {
            PlayerModel_CellValue(m_botpick.gp.grid.sel, scratch, sizeof(scratch));
            Cvar_Set(va("bot_skin%d", m_botpick.slot), scratch);
        }
        return;
    }

    Q_concat(scratch, sizeof(scratch),
             uis.pmi[m_botpick.model.curvalue].directory, "/",
             uis.pmi[m_botpick.model.curvalue].skindisplaynames[m_botpick.skin.curvalue]);

    // its changed callback (ui.c) renames the bot's row to match
    Cvar_Set(va("bot_skin%d", m_botpick.slot), scratch);
}

static bool BotPick_Push(menuFrameWork_t *self)
{
    char current[MAX_QPATH];
    int i, model, skin;

    if (!uis.numPlayerModels) {
        PlayerModel_Load();
        if (!uis.numPlayerModels) {
            Com_Printf("No player models found.\n");
            return false;
        }
    }

    m_botpick.slot = Cvar_VariableInteger("_ui_botslot");
    clamp(m_botpick.slot, 1, BOT_MAX_MATCH);

    Cvar_VariableStringBuffer(va("bot_skin%d", m_botpick.slot), current, sizeof(current));

    m_botpick.grid = uis.pmGrid;
    if (m_botpick.grid) {
        m_botpick.model.generic.flags |= QMF_HIDDEN;
        m_botpick.skin.generic.flags |= QMF_HIDDEN;
        m_botpick.model.generic.flags &= ~QMF_HASFOCUS;
        m_botpick.skin.generic.flags &= ~QMF_HASFOCUS;
        m_botpick.menu.title = NULL;
        m_botpick.menu.status = GRID_STATUS;
        // hidden, but Menu_Init still sets them up: SpinControl_Init counts
        // their names, so they need lists (a NULL one crashed the first opening)
        m_botpick.model.itemnames = uis.pmCellNames;
        m_botpick.model.curvalue = 0;
        m_botpick.skin.itemnames = uis.pmi[0].skindisplaynames;
        m_botpick.skin.curvalue = 0;
        if (!GridPage_Push(&m_botpick.gp, current, 0))
            return false;
        BotPick_GridTitle();
        return true;
    }
    m_botpick.model.generic.flags &= ~QMF_HIDDEN;
    m_botpick.skin.generic.flags &= ~QMF_HIDDEN;
    if (!(m_botpick.skin.generic.flags & QMF_HASFOCUS))
        m_botpick.model.generic.flags |= QMF_HASFOCUS;
    m_botpick.menu.status = NULL;

    Preview_FindSkin(current, &model, &skin);

    for (i = 0; i < uis.numPlayerModels; i++) {
        m_botpick.pmnames[i] = uis.pmi[i].directory;
        m_botpick.model_skin[i] = 0;
    }

    m_botpick.model.curvalue = model;
    m_botpick.model.itemnames = m_botpick.pmnames;

    m_botpick.skin.curvalue = skin;
    m_botpick.skin.itemnames = uis.pmi[model].skindisplaynames;
    m_botpick.model_skin[model] = skin;

    BotPick_Update();
    Preview_Reset(&m_botpick.pv);

    return true;
}

static void BotPick_Free(menuFrameWork_t *self)
{
    Grid_Free(&m_botpick.gp.grid);
    Z_Free(m_botpick.menu.items);
    memset(&m_botpick, 0, sizeof(m_botpick));
}

void M_Menu_BotPick(void)
{
    m_botpick.menu.name = "botpick";
    m_botpick.menu.push = BotPick_Push;
    m_botpick.menu.pop = BotPick_Pop;
    m_botpick.menu.size = BotPick_Size;
    m_botpick.menu.draw = BotPick_Draw;
    m_botpick.menu.keydown = BotPick_Keydown;
    m_botpick.menu.free = BotPick_Free;
    Preview_MenuColor(&m_botpick.menu);

    Preview_Init(&m_botpick.pv);
    Preview_Init(&m_botpick.gp.pv);

    m_botpick.model.generic.type = MTYPE_SPINCONTROL;
    m_botpick.model.generic.id = ID_BOT_MODEL;
    m_botpick.model.generic.flags = QMF_HASFOCUS;
    m_botpick.model.generic.name = "model";
    m_botpick.model.generic.status = "what this bot wears - it goes by the skin's name; in team games, in its team's uniform";
    m_botpick.model.generic.change = BotPick_Change;

    m_botpick.skin.generic.type = MTYPE_SPINCONTROL;
    m_botpick.skin.generic.id = ID_BOT_SKIN;
    m_botpick.skin.generic.name = "skin";
    m_botpick.skin.generic.status = "what this bot wears - it goes by the skin's name; in team games, in its team's uniform";
    m_botpick.skin.generic.change = BotPick_Change;

    Menu_AddItem(&m_botpick.menu, &m_botpick.model);
    Menu_AddItem(&m_botpick.menu, &m_botpick.skin);

    List_Append(&ui_menus, &m_botpick.menu.entry);
}
