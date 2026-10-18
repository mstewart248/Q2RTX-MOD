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

    int        time;
    int        oldTime;

    // turning the model by dragging it - see Keydown and UpdateDrag
    float       yaw;
    bool        dragging;
    int         drag_x;

    // the skin last chosen for each model (uis.pmi index), so stepping to another
    // model and back brings its skin back instead of the first one in the list.
    // Kept across openings of the menu; Free clears it with the rest.
    int         model_skin[MAX_PLAYERMODELS];

    char *pmnames[MAX_PLAYERMODELS];
} m_player_t;

// the way the model faces when the menu opens - three-quarters toward the camera
#define PLAYER_DEFAULT_YAW  260.0f

// how far the model turns per unit of mouse travel, in degrees: about a full turn
// across the half of the screen it stands in
#define PLAYER_DRAG_DEGREES 0.5f

static m_player_t    m_player;

extern cvar_t       *vid_rtx;

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
// card above the head (m_player.card) fills that shadow, so the light can stay
// small. A sphere light's colour is its radiance, so its total output grows with
// the radius squared: radius 24 gave 16x the light of radius 6, and with the
// card that was too bright. Keep the sphere clear of the feet: at 24 its edge
// came within ~3 units of the nearest one.
static dlight_t dlights[] = {
	{.origin = { 24.f, -12.f, -48.f },.color = {1.f, 1.f, 1.f},.intensity = 60.f,.radius = 6.f },
	{.origin = { 100.f, 80.f, 20.f },.color = {0.5f, 0.5f, 1.f},.intensity = 120.f,.radius = 20.f }
};

static void ReloadMedia(void)
{
    char scratch[MAX_QPATH];
    char *model = uis.pmi[m_player.model.curvalue].directory;
    char *skin = uis.pmi[m_player.model.curvalue].skindisplaynames[m_player.skin.curvalue];

    m_player.num_actors = 0;

    m_player.card.model = (cls.ref_type == REF_TYPE_VKPT)
        ? R_RegisterModel("models/rtx/bounce_card/tris.md2") : 0;

    Q_concat(scratch, sizeof(scratch), "players/", model, "/tris.md2");
    m_player.entities[0].model = R_RegisterModel(scratch);
    if (!m_player.entities[0].model)
        return;

    m_player.num_actors++;

    Q_concat(scratch, sizeof(scratch), "players/", model, "/", skin, ".pcx");
    m_player.entities[0].skin = R_RegisterSkin(scratch);

    if (!uis.weaponModel[0])
        return;

    Q_concat(scratch, sizeof(scratch), "players/", model, "/", uis.weaponModel);
    m_player.entities[1].model = R_RegisterModel(scratch);
    if (!m_player.entities[1].model)
        return;

    m_player.num_actors++;
}

static void RunFrame(void)
{
    int frame;
    int i;

    if (m_player.time < uis.realtime) {
        m_player.oldTime = m_player.time;

        m_player.time += 120;
        if (m_player.time < uis.realtime) {
            m_player.time = uis.realtime;
        }

        frame = (m_player.time / 120) % 40;

        for (i = 0; i < m_player.num_actors; i++) {
            m_player.entities[i].oldframe = m_player.entities[i].frame;
            m_player.entities[i].frame = frame;
        }
    }
}

static void SetYaw(float yaw)
{
    int i;

    m_player.yaw = anglemod(yaw);
    for (i = 0; i < 2; i++)
        m_player.entities[i].angles[YAW] = m_player.yaw;
}

// Follows the mouse while the drag Keydown started is held. Menus are told about
// key presses but not releases, so the button is polled here, once a frame.
static void UpdateDrag(void)
{
    int x;

    if (!m_player.dragging)
        return;

    if (!Key_IsDown(K_MOUSE1)) {
        m_player.dragging = false;
        return;
    }

    // Dragging right brings the side facing the camera round to the right, as if
    // the model were grabbed by its front: screen right is -y from this camera,
    // and turning the facing toward -y is an increase in yaw.
    x = uis.mouseCoords[0];
    SetYaw(m_player.yaw + (x - m_player.drag_x) * PLAYER_DRAG_DEGREES);
    m_player.drag_x = x;
}

// A left click that does not land on a menu item - on the model, or anywhere on
// the empty screen - starts turning the model; UpdateDrag takes it from there.
static menuSound_t Keydown(menuFrameWork_t *self, int key)
{
    if (key == K_MOUSE1 && !Menu_HitTest(self)) {
        m_player.dragging = true;
        m_player.drag_x = uis.mouseCoords[0];
        return QMS_SILENT;
    }

    return QMS_NOTHANDLED;
}

static void Draw(menuFrameWork_t *self)
{
    float backlerp;
    int i;

    m_player.refdef.time = uis.realtime * 0.001f;

    UpdateDrag();
    RunFrame();

    if (m_player.time == m_player.oldTime) {
        backlerp = 0;
    } else {
        backlerp = 1 - (float)(uis.realtime - m_player.oldTime) /
                   (float)(m_player.time - m_player.oldTime);
    }

    m_player.refdef.num_entities = 0;
    for (i = 0; i < m_player.num_actors; i++) {
        m_player.entities[i].backlerp = backlerp;
        m_player.scene[m_player.refdef.num_entities++] = m_player.entities[i];
    }
    if (m_player.card.model)
        m_player.scene[m_player.refdef.num_entities++] = m_player.card;

    Menu_Draw(self);

    R_RenderFrame(&m_player.refdef, 0);

    R_SetScale(uis.scale);
}

static void Size(menuFrameWork_t *self)
{
    int w = uis.width / uis.scale;
    int h = uis.height / uis.scale;
    int x = uis.width / 2;
    int y = uis.height / 2 - MENU_SPACING * 5 / 2;

    m_player.refdef.x = w / 2;
    m_player.refdef.y = h / 10;
    m_player.refdef.width = w / 2;
    m_player.refdef.height = h - h / 5;

    m_player.refdef.fov_x = 90;
    m_player.refdef.fov_y = V_CalcFov(m_player.refdef.fov_x,
                                      m_player.refdef.width, m_player.refdef.height);

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
    char currentdirectory[MAX_QPATH];
    char currentskin[MAX_QPATH];
    int i, j;
    int currentdirectoryindex = 0;
    int currentskinindex = 0;
    char *p;

    // find and register all player models
    if (!uis.numPlayerModels) {
        PlayerModel_Load();
        if (!uis.numPlayerModels) {
            Com_Printf("No player models found.\n");
            return false;
        }
    }

    Cvar_VariableStringBuffer("skin", currentdirectory, sizeof(currentdirectory));

    if ((p = strchr(currentdirectory, '/')) || (p = strchr(currentdirectory, '\\'))) {
        *p++ = 0;
        Q_strlcpy(currentskin, p, sizeof(currentskin));
    } else {
        strcpy(currentdirectory, "male");
        strcpy(currentskin, "grunt");
    }

    for (i = 0; i < uis.numPlayerModels; i++) {
        m_player.pmnames[i] = uis.pmi[i].directory;
        if (Q_stricmp(uis.pmi[i].directory, currentdirectory) == 0) {
            currentdirectoryindex = i;

            for (j = 0; j < uis.pmi[i].nskins; j++) {
                if (Q_stricmp(uis.pmi[i].skindisplaynames[j], currentskin) == 0) {
                    currentskinindex = j;
                    break;
                }
            }
        }
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

    // every opening starts from the same pose, not wherever the last drag left it
    m_player.dragging = false;
    SetYaw(PLAYER_DEFAULT_YAW);

    // set up oldframe correctly
    m_player.time = uis.realtime - 120;
    m_player.oldTime = m_player.time;
    RunFrame();

    return true;
}

static void Free(menuFrameWork_t *self)
{
    Z_Free(m_player.menu.items);
    memset(&m_player, 0, sizeof(m_player));
}

void M_Menu_PlayerConfig(void)
{
    static const vec3_t origin = { 40.0f, 0.0f, 0.0f };
    static const vec3_t angles = { 0.0f, PLAYER_DEFAULT_YAW, 0.0f };

    m_player.menu.name = "players";
    m_player.menu.push = Push;
    m_player.menu.pop = Pop;
    m_player.menu.size = Size;
    m_player.menu.draw = Draw;
    m_player.menu.keydown = Keydown;
    m_player.menu.free = Free;
	m_player.menu.image = uis.backgroundHandle;

	if (cls.ref_type == REF_TYPE_VKPT)
	{
		// Q2RTX: make the player menu transparent so that we can see 
		// the model below: all 2D stuff is rendered after 3D, in stretch_pics.
		m_player.menu.color.u32 = 0;
	}
	else
	{
		m_player.menu.color.u32 = MakeColor(0, 0, 0, 255);
	}

	m_player.menu.transparent = uis.transparent;

    m_player.entities[0].flags = RF_FULLBRIGHT;
	m_player.entities[0].id = 1; // Q2RTX: add entity id to fix motion vectors
    VectorCopy(angles, m_player.entities[0].angles);
    VectorCopy(origin, m_player.entities[0].origin);
    VectorCopy(origin, m_player.entities[0].oldorigin);

    m_player.entities[1].flags = RF_FULLBRIGHT;
	m_player.entities[1].id = 2; // Q2RTX: add entity id to fix motion vectors
	VectorCopy(angles, m_player.entities[1].angles);
    VectorCopy(origin, m_player.entities[1].origin);
    VectorCopy(origin, m_player.entities[1].oldorigin);

    // placed and tilted in the model file itself - origin, no rotation
    m_player.card.flags = RF_VIEWERMODEL;
    m_player.card.id = 3;

    m_player.refdef.num_entities = 0;
    m_player.refdef.entities = m_player.scene;
    m_player.refdef.rdflags = RDF_NOWORLDMODEL;

	if (cls.ref_type == REF_TYPE_VKPT)
	{
		m_player.refdef.num_dlights = sizeof(dlights) / sizeof(*dlights);
		m_player.refdef.dlights = dlights;
	}
	else
	{
		m_player.refdef.num_dlights = 0;
	}

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

