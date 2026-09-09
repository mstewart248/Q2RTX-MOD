typedef enum {
    P_bad,

    P_prethink,
    P_think,
    P_blocked,
    P_touch,
    P_use,
    P_pain,
    P_die,

    P_moveinfo_endfunc,

    P_monsterinfo_currentmove,
    P_monsterinfo_stand,
    P_monsterinfo_idle,
    P_monsterinfo_search,
    P_monsterinfo_walk,
    P_monsterinfo_run,
    P_monsterinfo_dodge,
    P_monsterinfo_attack,
    P_monsterinfo_melee,
    P_monsterinfo_sight,
    P_monsterinfo_checkattack,
    P_monsterinfo_blocked,

    // ROGUE/rerelease duck + sidestep.  Appended rather than filed next to
    // monsterinfo_dodge so the existing numbering, which the frozen version-2
    // table in g_ptrs_compat_v2.c is written against, does not move.
    P_monsterinfo_duck,
    P_monsterinfo_unduck,
    P_monsterinfo_sidestep
} ptr_type_t;

typedef struct {
    ptr_type_t type;
    void *ptr;
} save_ptr_t;

extern const save_ptr_t save_ptrs[];
extern const int num_save_ptrs;
extern const save_ptr_t save_ptrs_v2[];
extern const int num_save_ptrs_v2;
