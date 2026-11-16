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

    // An invisible bounce card above and in front of the model (path tracer
    // only): a white quad drawn as a viewer model, so the camera never sees it
    // but bounce and shadow rays do. It sends some of the footlight back down
    // into the shadows. The quad is baked in place in the .md2 (written by a
    // script), so it is drawn at the origin unrotated and does not turn with
    // the model.
    entity_t    card;

    entity_t    scene[3];       // what is handed to the renderer each frame

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

    pv->num_actors = 0;

    pv->card.model = CL_PathTraced()
        ? R_RegisterModel("models/rtx/bounce_card/tris.md2") : 0;

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
    Preview_Load(&m_player.pv, uis.pmi[m_player.model.curvalue].directory,
                 uis.pmi[m_player.model.curvalue].skindisplaynames[m_player.skin.curvalue]);
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

    Q_concat(scratch, sizeof(scratch),
             uis.pmi[m_player.model.curvalue].directory, "/",
             uis.pmi[m_player.model.curvalue].skindisplaynames[m_player.skin.curvalue]);

    Cvar_SetEx("skin", scratch, FROM_CONSOLE);

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

    m_player.model.curvalue = currentdirectoryindex;
    m_player.model.itemnames = m_player.pmnames;

    m_player.skin.curvalue = currentskinindex;
    m_player.skin.itemnames = uis.pmi[currentdirectoryindex].skindisplaynames;
    m_player.model_skin[currentdirectoryindex] = currentskinindex;

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
} m_botpick_t;

static m_botpick_t  m_botpick;

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
    return Preview_Keydown(&m_botpick.pv, self, key);
}

static void BotPick_Draw(menuFrameWork_t *self)
{
    Preview_Draw(&m_botpick.pv, self);
}

static void BotPick_Size(menuFrameWork_t *self)
{
    int x = uis.width / 2;
    int y = uis.height / 2 - MENU_SPACING;

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
