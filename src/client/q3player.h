/*
Quake III Arena player models, for players and bots (src/client/q3player.c).

A Quake III player is three .md3 models bolted together by tags - lower.md3
(legs), upper.md3 (torso, on the legs' tag_torso) and head.md3 (on the torso's
tag_head) - with the gun on the torso's tag_weapon. They animate apart: the
legs run while the torso shoots, the head looks where the player looks and the
torso only part of the way. animation.cfg says which frames are which.

Here a Quake II player's state (its frame, angles and motion) is turned into
that: the legs from how the body moves, the torso from the Quake II frame and
the muzzle flashes, so it works with any Quake II server and in demos. The
weapon is the Quake II one the player holds (players/male/w_*.md2), put in the
Quake III hand.

The model is named in "skin" like any other ("sarge/krusade"), and is a Quake
III one when players/<model>/tris.md2 does not exist and
models/players/<model>/ does (baseq3, files.c add_quake3_paths).
*/

#ifndef Q3PLAYER_H
#define Q3PLAYER_H

// every skin of a model is a skin of the one .md3 (Q3P_PatchMD3), picked by
// entity skinnum; MAX_ALIAS_SKINS in refresh/models.h
#define Q3P_MAX_SKINS   32

typedef struct q3player_s q3player_t;

// a Quake III model ready to draw in one of its skins
typedef struct {
    q3player_t  *q3;            // NULL: not a Quake III model
    qhandle_t   lower, upper, head;
    int         skin;           // entity skinnum for all three
} q3pmodel_t;

// Quake III's animation.cfg order, then the two its code makes by playing
// the walks backwards
typedef enum {
    Q3A_BOTH_DEATH1,
    Q3A_BOTH_DEAD1,
    Q3A_BOTH_DEATH2,
    Q3A_BOTH_DEAD2,
    Q3A_BOTH_DEATH3,
    Q3A_BOTH_DEAD3,
    Q3A_TORSO_GESTURE,
    Q3A_TORSO_ATTACK,
    Q3A_TORSO_ATTACK2,
    Q3A_TORSO_DROP,
    Q3A_TORSO_RAISE,
    Q3A_TORSO_STAND,
    Q3A_TORSO_STAND2,
    Q3A_LEGS_WALKCR,
    Q3A_LEGS_WALK,
    Q3A_LEGS_RUN,
    Q3A_LEGS_BACK,
    Q3A_LEGS_SWIM,
    Q3A_LEGS_JUMP,
    Q3A_LEGS_LAND,
    Q3A_LEGS_JUMPB,
    Q3A_LEGS_LANDB,
    Q3A_LEGS_IDLE,
    Q3A_LEGS_IDLECR,
    Q3A_LEGS_TURN,
    Q3A_FILE_COUNT,                     // the ones animation.cfg lists

    Q3A_LEGS_BACKCR = Q3A_FILE_COUNT,   // WALKCR backwards
    Q3A_LEGS_BACKWALK,                  // WALK backwards

    Q3A_COUNT
} q3anim_t;

// one part's place in an animation
typedef struct {
    int     anim;           // q3anim_t, -1 before the first
    float   phase;          // frames into it, with the fraction
    bool    done;           // a one-shot animation is on its last frame
    int     from_frame;     // frame being blended away from on a change, -1 none
    float   blend;          // 0..1 through that blend
    float   blend_rate;     // per millisecond
    int     frame, oldframe;
    float   backlerp;
} q3plerp_t;

// what one drawn Quake III player remembers from frame to frame
typedef struct {
    bool        valid;
    q3player_t  *q3;
    int         time;           // ms, of the last update

    q3plerp_t   legs, torso;

    float       torso_yaw, legs_yaw, torso_pitch;
    bool        torso_yawing, legs_yawing, torso_pitching;

    vec3_t      velocity;       // smoothed, units/s
    bool        onground;
    int         air_since;      // ms, when it left the ground
    int         jump_anim;      // LEGS_JUMP/JUMPB while in the air, 0 none
    int         land_anim;
    int         land_until;

    int         pain_time;
    bool        pain_dir;

    int         q2frame;        // the Quake II frame last seen, -1 none
    int         fire_time;      // last muzzle flash
    bool        fire_pending;

    qhandle_t   weapon_shown;   // the gun in the hand
    qhandle_t   weapon_pending; // the one it is changing to (TORSO_DROP)
    bool        gesture_pending;
} q3pstate_t;

// what a frame of a Quake III player is drawn from
typedef struct {
    int         time;           // ms
    int         q2frame;        // the Quake II player frame (m_player.h)
    vec3_t      origin;
    vec3_t      view;           // pitch (real, not the /3 sent) and yaw
    vec3_t      velocity;       // units/s
    bool        onground;
    bool        swimming;       // in water to the waist, off the ground
    qhandle_t   weapon;         // w_*.md2 held, 0 none
    bool        melee;          // the chainfist: gauntlet poses
    bool        fired;          // a muzzle flash since the last update
    bool        gesture;        // play TORSO_GESTURE (the menu preview)
} q3pinput_t;

// the parts it draws, in order
enum { Q3P_PART_LEGS, Q3P_PART_TORSO, Q3P_PART_HEAD, Q3P_PART_WEAPON, Q3P_NUM_PARTS };

bool        Q3P_ModelExists(const char *model);
q3player_t  *Q3P_Get(const char *model);
const char  *Q3P_ModelName(const q3player_t *q3);
char        Q3P_Sex(const q3player_t *q3);      // 'm', 'f' or 'n'
int         Q3P_SkinIndex(const q3player_t *q3, const char *skin);
int         Q3P_NumSkins(const q3player_t *q3);
const char  *Q3P_SkinName(const q3player_t *q3, int index);
int         Q3P_SkinList(const char *model, char (*out)[MAX_QPATH], int max);
bool        Q3P_Register(q3pmodel_t *m, const char *model, const char *skin);

// models.c: give each mesh of models/players/<model>/{lower,upper,head}.md3
// every skin of the model
bool        Q3P_PatchMD3(const char *path, byte **data, int *len);

// the parts as entities, from base (flags, alpha, id; origin = feet). Returns
// how many were written to out; the gun, when there is one, is the last.
int         Q3P_Animate(q3pstate_t *st, const q3pmodel_t *m, const q3pinput_t *in,
                        const entity_t *base, entity_t *out);

// a Quake III sound for a Quake II "*name.wav" sexed sound, or false
bool        Q3P_SexedSound(const char *model, const char *base, char *out, size_t size);

void        Q3P_Init(void);
void        Q3P_Flush(void);    // frees every q3player_t: drop any held pointers first

// cl_q3_players: 0 draws a Quake III skin as any missing model is, the male
bool        CL_Q3PlayersEnabled(void);

#endif // Q3PLAYER_H
