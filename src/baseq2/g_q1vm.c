/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2026 Q2RTX contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

// g_q1vm.c - the QuakeC machine: progs.dat, its strings and entities, the
// interpreter, entity spawning and the rerelease's localized strings. A port
// of Quake's pr_exec.c and pr_edict.c; see g_q1vm.h for the whole picture.

#include "g_q1vm.h"

qc_vm_t qvm;

enum {
    OP_DONE, OP_MUL_F, OP_MUL_V, OP_MUL_FV, OP_MUL_VF, OP_DIV_F, OP_ADD_F, OP_ADD_V,
    OP_SUB_F, OP_SUB_V, OP_EQ_F, OP_EQ_V, OP_EQ_S, OP_EQ_E, OP_EQ_FNC, OP_NE_F,
    OP_NE_V, OP_NE_S, OP_NE_E, OP_NE_FNC, OP_LE, OP_GE, OP_LT, OP_GT,
    OP_LOAD_F, OP_LOAD_V, OP_LOAD_S, OP_LOAD_ENT, OP_LOAD_FLD, OP_LOAD_FNC, OP_ADDRESS,
    OP_STORE_F, OP_STORE_V, OP_STORE_S, OP_STORE_ENT, OP_STORE_FLD, OP_STORE_FNC,
    OP_STOREP_F, OP_STOREP_V, OP_STOREP_S, OP_STOREP_ENT, OP_STOREP_FLD, OP_STOREP_FNC,
    OP_RETURN, OP_NOT_F, OP_NOT_V, OP_NOT_S, OP_NOT_ENT, OP_NOT_FNC, OP_IF, OP_IFNOT,
    OP_CALL0, OP_CALL1, OP_CALL2, OP_CALL3, OP_CALL4, OP_CALL5, OP_CALL6, OP_CALL7, OP_CALL8,
    OP_STATE, OP_GOTO, OP_AND, OP_OR, OP_BITAND, OP_BITOR,
    OP_NUMOPS
};

/*
==============================================================================

STRINGS

A string is an offset into qvm.strings: the progs' own table first, then a
ring of temporary results (ftos, vtos...), then a heap for the strings the
level adds (entity keys, player names). Everything is per level, as it was in
Quake, which reloaded the progs for every map.

==============================================================================
*/

const char *QC_Str(int s)
{
    if (s < 0 || s >= qvm.strings_size)
        return "";
    return qvm.strings + s;
}

int QC_NewString(const char *s)
{
    size_t len = strlen(s) + 1;
    int ofs;

    if (qvm.heap_used + len > (size_t)qvm.strings_size) {
        gi.dprintf("Quake 1 game: string heap full\n");
        return 0;
    }

    ofs = qvm.heap_used;
    // "\n" in a map's key is a newline, as Quake's ED_NewString had it
    {
        char *d = qvm.strings + ofs;
        for (; *s; s++) {
            if (s[0] == '\\' && s[1] == 'n') {
                *d++ = '\n';
                s++;
            } else {
                *d++ = *s;
            }
        }
        *d++ = 0;
        qvm.heap_used = (int)(d - qvm.strings);
    }
    return ofs;
}

int QC_TempString(const char *s)
{
    int ofs = qvm.progs_strings + qvm.temp_next * QC_TEMPSTRING_SIZE;

    qvm.temp_next = (qvm.temp_next + 1) % QC_TEMPSTRINGS;
    Q_strlcpy(qvm.strings + ofs, s, QC_TEMPSTRING_SIZE);
    return ofs;
}

int QC_EngineString(const char *s)
{
    if (s >= qvm.strings && s < qvm.strings + qvm.strings_size)
        return (int)(s - qvm.strings);
    return QC_NewString(s);
}

/*
==============================================================================

LOCALIZATION

The rerelease's progs print keys - "$qc_got_item" - that the engine looks up
in localization/loc_<language>.txt and fills in ("You got {0}"). Done here, on
the server, in English: the words reach the client already made.

==============================================================================
*/

typedef struct {
    char    *key;
    char    *value;
} qc_locentry_t;

static struct {
    char            episode[16];
    char            *text;
    qc_locentry_t   *entries;
    int             num;
    int             *index;     // open addressing, num * 2
    int             size;
} qc_loc;

static unsigned qc_hash(const char *s)
{
    unsigned h = 0;
    while (*s)
        h = h * 31 + (unsigned char)*s++;
    return h;
}

static void qc_loc_parse(char *text, int *count, qc_locentry_t *out)
{
    char *line = text;

    *count = 0;
    while (*line) {
        char *end = line;
        while (*end && *end != '\n')
            end++;
        char *next = *end ? end + 1 : end;
        *end = 0;
        if (end > line && end[-1] == '\r')
            end[-1] = 0;

        // key = "value" ; // comments; blank lines
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p && !(p[0] == '/' && p[1] == '/')) {
            char *key = p;
            while (*p && *p != '=' && *p != ' ' && *p != '\t')
                p++;
            char *keyend = p;
            while (*p == ' ' || *p == '\t')
                p++;
            if (*p == '=') {
                p++;
                while (*p == ' ' || *p == '\t')
                    p++;
                *keyend = 0;
                char *value = p, *d = p;
                bool quoted = (*p == '"');
                if (quoted)
                    p++;
                while (*p) {
                    if (*p == '\\' && p[1]) {
                        switch (p[1]) {
                        case 'n': *d++ = '\n'; break;
                        case 't': *d++ = '\t'; break;
                        default: *d++ = p[1]; break;
                        }
                        p += 2;
                        continue;
                    }
                    if (quoted && *p == '"')
                        break;
                    *d++ = *p++;
                }
                *d = 0;
                if (!quoted)
                    while (d > value && (d[-1] == ' ' || d[-1] == '\t'))
                        *--d = 0;
                if (out) {
                    out[*count].key = key;
                    out[*count].value = value;
                }
                (*count)++;
            }
        }
        line = next;
    }
}

void QC_LoadLocalization(const char *episode)
{
    char    path[MAX_QPATH];
    void    *buf;
    int     len;

    if (qc_loc.text && !strcmp(qc_loc.episode, episode))
        return;

    if (qc_loc.text) {
        gi.TagFree(qc_loc.text);
        gi.TagFree(qc_loc.entries);
        gi.TagFree(qc_loc.index);
    }
    memset(&qc_loc, 0, sizeof(qc_loc));
    Q_strlcpy(qc_loc.episode, episode, sizeof(qc_loc.episode));

    Q_snprintf(path, sizeof(path), "id1/%s/localization/loc_english.txt", episode);
    len = gi.LoadFile(path, &buf);
    if (len <= 0 || !buf) {
        gi.dprintf("Quake 1 game: no %s\n", path);
        return;
    }

    // the episode's own additions (loc_english_mod.txt) go after id1's
    void *modbuf = NULL;
    int modlen = 0;
    Q_snprintf(path, sizeof(path), "id1/%s/localization/loc_english_mod.txt", episode);
    modlen = gi.LoadFile(path, &modbuf);
    if (modlen < 0)
        modlen = 0;

    qc_loc.text = gi.TagMalloc(len + modlen + 2, TAG_GAME);
    memcpy(qc_loc.text, buf, len);
    qc_loc.text[len] = '\n';
    if (modlen && modbuf)
        memcpy(qc_loc.text + len + 1, modbuf, modlen);
    qc_loc.text[len + 1 + modlen] = 0;
    gi.FreeFile(buf);
    if (modbuf)
        gi.FreeFile(modbuf);

    // count, then parse in place (parsing writes terminators, so count on a copy)
    char *copy = gi.TagMalloc(len + modlen + 2, TAG_GAME);
    memcpy(copy, qc_loc.text, len + modlen + 2);
    int count;
    qc_loc_parse(copy, &count, NULL);
    gi.TagFree(copy);

    qc_loc.entries = gi.TagMalloc(sizeof(qc_locentry_t) * (count + 1), TAG_GAME);
    qc_loc_parse(qc_loc.text, &qc_loc.num, qc_loc.entries);

    qc_loc.size = qc_loc.num * 2 + 1;
    qc_loc.index = gi.TagMalloc(sizeof(int) * qc_loc.size, TAG_GAME);
    memset(qc_loc.index, 0, sizeof(int) * qc_loc.size);
    for (int i = 0; i < qc_loc.num; i++) {
        unsigned pos = qc_hash(qc_loc.entries[i].key) % qc_loc.size;
        while (qc_loc.index[pos])
            pos = (pos + 1) % qc_loc.size;
        qc_loc.index[pos] = i + 1;  // a later duplicate is never found: the first wins
    }
}

const char *QC_Localize(const char *s)
{
    if (!s || s[0] != '$' || !qc_loc.size)
        return s;

    unsigned pos = qc_hash(s + 1) % qc_loc.size;
    while (qc_loc.index[pos]) {
        const qc_locentry_t *e = &qc_loc.entries[qc_loc.index[pos] - 1];
        if (!strcmp(e->key, s + 1))
            return e->value;
        pos = (pos + 1) % qc_loc.size;
    }
    return s;
}

/*
==============================================================================

THE INTERPRETER

==============================================================================
*/

static const char *qc_function_name(const qc_function_t *f)
{
    return f ? QC_Str(f->s_name) : "(none)";
}

void QC_PrintStack(void)
{
    if (qvm.depth == 0) {
        gi.dprintf("<NO STACK>\n");
        return;
    }

    qvm.stack[qvm.depth].f = qvm.xfunction;
    qvm.stack[qvm.depth].s = qvm.xstatement;
    for (int i = qvm.depth; i >= 0; i--) {
        qc_function_t *f = qvm.stack[i].f;
        if (!f)
            gi.dprintf("<NO FUNCTION>\n");
        else
            gi.dprintf("%12s : %s : statement %d\n", QC_Str(f->s_file), qc_function_name(f),
                       qvm.stack[i].s - f->first_statement);
    }
}

void QC_RunError(const char *fmt, ...)
{
    va_list argptr;
    char    msg[1024];

    va_start(argptr, fmt);
    Q_vsnprintf(msg, sizeof(msg), fmt, argptr);
    va_end(argptr);

    gi.dprintf("Quake 1 game error in %s: %s\n", qc_function_name(qvm.xfunction), msg);
    QC_PrintStack();
    qvm.depth = 0;
    qvm.localstack_used = 0;

    // Quake ended the server; so does Quake II
    gi.error("Quake 1 game: %s (in %s)", msg, qc_function_name(qvm.xfunction));
}

static int qc_enter_function(qc_function_t *f)
{
    int i, j, c, o;

    qvm.stack[qvm.depth].s = qvm.xstatement;
    qvm.stack[qvm.depth].f = qvm.xfunction;
    qvm.depth++;
    if (qvm.depth >= QC_MAX_STACK_DEPTH - 1)
        QC_RunError("stack overflow");

    // save off any locals that the new function steps on
    c = f->locals;
    if (qvm.localstack_used + c > QC_LOCALSTACK_SIZE)
        QC_RunError("locals stack overflow");

    for (i = 0; i < c; i++)
        qvm.localstack[qvm.localstack_used + i] = ((int *)qvm.globals)[f->parm_start + i];
    qvm.localstack_used += c;

    // copy parameters
    o = f->parm_start;
    for (i = 0; i < f->numparms && i < QC_MAX_PARMS; i++) {
        for (j = 0; j < f->parm_size[i]; j++) {
            ((int *)qvm.globals)[o] = ((int *)qvm.globals)[QC_OFS_PARM0 + i * 3 + j];
            o++;
        }
    }

    qvm.xfunction = f;
    return f->first_statement - 1;  // offset the s++
}

static int qc_leave_function(void)
{
    int i, c;

    if (qvm.depth <= 0)
        QC_RunError("prog stack underflow");

    // restore locals from the stack
    c = qvm.xfunction->locals;
    qvm.localstack_used -= c;
    if (qvm.localstack_used < 0)
        QC_RunError("locals stack underflow");

    for (i = 0; i < c; i++)
        ((int *)qvm.globals)[qvm.xfunction->parm_start + i] = qvm.localstack[qvm.localstack_used + i];

    // up stack
    qvm.depth--;
    qvm.xfunction = qvm.stack[qvm.depth].f;
    return qvm.stack[qvm.depth].s;
}

// an entity's field address, as OP_ADDRESS makes it and OP_STOREP takes it:
// a byte offset into qvm.fields
// (size) is the bytes the op reads or writes there: 12 for a vector, else 4.
// The last field of all is a float, and a 12 byte check refused it (e2m6's
// zombies, mg1's horde waves, ctf's items).
static inline qc_eval_t *qc_pointer(int ofs, int size)
{
    if (ofs < 0 || (size_t)ofs + size > (size_t)qvm.max_edicts * qvm.edict_size)
        QC_RunError("bad pointer %d", ofs);
    return (qc_eval_t *)(qvm.fields + ofs);
}

static inline qc_eval_t *qc_load(int edict, int field, int size)
{
    if (edict < 0 || edict >= qvm.max_edicts * qvm.edict_size || (edict % qvm.edict_size))
        QC_RunError("bad entity %d", edict);
    if (field < 0 || field * 4 + size > qvm.edict_size)
        QC_RunError("bad field %d", field);
    return (qc_eval_t *)(qvm.fields + edict + field * 4);
}

void QC_Execute(int fnum)
{
    qc_eval_t       *a, *b, *c;
    int             s;
    qc_statement_t  *st;
    qc_function_t   *f, *newf;
    int             runaway;
    int             exitdepth;
    float           *glob = qvm.globals;

    if (!fnum || fnum >= qvm.progs->numfunctions) {
        if (qvm.g->self)
            QC_PrintEdict(QC_FromProg(qvm.g->self));
        QC_RunError("NULL function");
    }

    f = &qvm.functions[fnum];

    // a builtin called directly (a field holding one)
    if (f->first_statement < 0) {
        int i = -f->first_statement;
        if (i >= QC_MAX_BUILTINS || !qc_builtins[i])
            QC_RunError("bad builtin %d", i);
        qvm.argc = f->numparms;
        qc_builtins[i]();
        return;
    }

    runaway = 10000000;     // Quake's was 100000; the rerelease's AI does more per call

    // make a stack frame
    exitdepth = qvm.depth;

    s = qc_enter_function(f);

    while (1) {
        s++;    // next statement

        st = &qvm.statements[s];
        a = (qc_eval_t *)&glob[(unsigned short)st->a];
        b = (qc_eval_t *)&glob[(unsigned short)st->b];
        c = (qc_eval_t *)&glob[(unsigned short)st->c];

        if (!--runaway)
            QC_RunError("runaway loop error");

        qvm.xstatement = s;

        switch (st->op) {
        case OP_ADD_F:
            c->_float = a->_float + b->_float;
            break;
        case OP_ADD_V:
            c->vector[0] = a->vector[0] + b->vector[0];
            c->vector[1] = a->vector[1] + b->vector[1];
            c->vector[2] = a->vector[2] + b->vector[2];
            break;

        case OP_SUB_F:
            c->_float = a->_float - b->_float;
            break;
        case OP_SUB_V:
            c->vector[0] = a->vector[0] - b->vector[0];
            c->vector[1] = a->vector[1] - b->vector[1];
            c->vector[2] = a->vector[2] - b->vector[2];
            break;

        case OP_MUL_F:
            c->_float = a->_float * b->_float;
            break;
        case OP_MUL_V:
            c->_float = a->vector[0] * b->vector[0]
                        + a->vector[1] * b->vector[1]
                        + a->vector[2] * b->vector[2];
            break;
        case OP_MUL_FV:
            c->vector[0] = a->_float * b->vector[0];
            c->vector[1] = a->_float * b->vector[1];
            c->vector[2] = a->_float * b->vector[2];
            break;
        case OP_MUL_VF:
            c->vector[0] = b->_float * a->vector[0];
            c->vector[1] = b->_float * a->vector[1];
            c->vector[2] = b->_float * a->vector[2];
            break;

        case OP_DIV_F:
            c->_float = a->_float / b->_float;
            break;

        case OP_BITAND:
            c->_float = (int)a->_float & (int)b->_float;
            break;
        case OP_BITOR:
            c->_float = (int)a->_float | (int)b->_float;
            break;

        case OP_GE:
            c->_float = a->_float >= b->_float;
            break;
        case OP_LE:
            c->_float = a->_float <= b->_float;
            break;
        case OP_GT:
            c->_float = a->_float > b->_float;
            break;
        case OP_LT:
            c->_float = a->_float < b->_float;
            break;
        case OP_AND:
            c->_float = a->_float && b->_float;
            break;
        case OP_OR:
            c->_float = a->_float || b->_float;
            break;

        case OP_NOT_F:
            c->_float = !a->_float;
            break;
        case OP_NOT_V:
            c->_float = !a->vector[0] && !a->vector[1] && !a->vector[2];
            break;
        case OP_NOT_S:
            c->_float = !a->string || !*QC_Str(a->string);
            break;
        case OP_NOT_FNC:
            c->_float = !a->function;
            break;
        case OP_NOT_ENT:
            c->_float = (a->edict == 0);
            break;

        case OP_EQ_F:
            c->_float = a->_float == b->_float;
            break;
        case OP_EQ_V:
            c->_float = (a->vector[0] == b->vector[0]) &&
                        (a->vector[1] == b->vector[1]) &&
                        (a->vector[2] == b->vector[2]);
            break;
        case OP_EQ_S:
            c->_float = !strcmp(QC_Str(a->string), QC_Str(b->string));
            break;
        case OP_EQ_E:
            c->_float = a->_int == b->_int;
            break;
        case OP_EQ_FNC:
            c->_float = a->function == b->function;
            break;

        case OP_NE_F:
            c->_float = a->_float != b->_float;
            break;
        case OP_NE_V:
            c->_float = (a->vector[0] != b->vector[0]) ||
                        (a->vector[1] != b->vector[1]) ||
                        (a->vector[2] != b->vector[2]);
            break;
        case OP_NE_S:
            c->_float = strcmp(QC_Str(a->string), QC_Str(b->string)) != 0;
            break;
        case OP_NE_E:
            c->_float = a->_int != b->_int;
            break;
        case OP_NE_FNC:
            c->_float = a->function != b->function;
            break;

        case OP_STORE_F:
        case OP_STORE_ENT:
        case OP_STORE_FLD:      // integers
        case OP_STORE_S:
        case OP_STORE_FNC:      // pointers
            b->_int = a->_int;
            break;
        case OP_STORE_V:
            b->vector[0] = a->vector[0];
            b->vector[1] = a->vector[1];
            b->vector[2] = a->vector[2];
            break;

        case OP_STOREP_F:
        case OP_STOREP_ENT:
        case OP_STOREP_FLD:     // integers
        case OP_STOREP_S:
        case OP_STOREP_FNC: {   // pointers
            qc_eval_t *ptr = qc_pointer(b->_int, 4);
            ptr->_int = a->_int;
            break;
        }
        case OP_STOREP_V: {
            qc_eval_t *ptr = qc_pointer(b->_int, 12);
            ptr->vector[0] = a->vector[0];
            ptr->vector[1] = a->vector[1];
            ptr->vector[2] = a->vector[2];
            break;
        }

        case OP_ADDRESS:
            if (a->edict == 0 && !qvm.loading)
                QC_RunError("assignment to world entity");
            qc_load(a->edict, b->_int, 4);     // range check
            c->_int = a->edict + b->_int * 4;
            break;

        case OP_LOAD_F:
        case OP_LOAD_FLD:
        case OP_LOAD_ENT:
        case OP_LOAD_S:
        case OP_LOAD_FNC:
            c->_int = qc_load(a->edict, b->_int, 4)->_int;
            break;

        case OP_LOAD_V: {
            qc_eval_t *v = qc_load(a->edict, b->_int, 12);
            c->vector[0] = v->vector[0];
            c->vector[1] = v->vector[1];
            c->vector[2] = v->vector[2];
            break;
        }

        case OP_IFNOT:
            if (!a->_int)
                s += st->b - 1;     // offset the s++
            break;

        case OP_IF:
            if (a->_int)
                s += st->b - 1;     // offset the s++
            break;

        case OP_GOTO:
            s += st->a - 1;         // offset the s++
            break;

        case OP_CALL0:
        case OP_CALL1:
        case OP_CALL2:
        case OP_CALL3:
        case OP_CALL4:
        case OP_CALL5:
        case OP_CALL6:
        case OP_CALL7:
        case OP_CALL8:
            qvm.argc = st->op - OP_CALL0;
            if (!a->function || a->function >= qvm.progs->numfunctions)
                QC_RunError("NULL function");

            newf = &qvm.functions[a->function];

            if (newf->first_statement < 0) {
                // negative statements are built in functions
                int i = -newf->first_statement;
                if (i >= QC_MAX_BUILTINS || !qc_builtins[i])
                    QC_RunError("bad builtin call number %d (%s)", i, QC_Str(newf->s_name));
                qc_builtins[i]();
                // a builtin may have run more QuakeC (touch functions from
                // walkmove, say); the globals pointer itself never moves
                break;
            }

            s = qc_enter_function(newf);
            break;

        case OP_DONE:
        case OP_RETURN:
            glob[QC_OFS_RETURN] = glob[(unsigned short)st->a];
            glob[QC_OFS_RETURN + 1] = glob[(unsigned short)st->a + 1];
            glob[QC_OFS_RETURN + 2] = glob[(unsigned short)st->a + 2];

            s = qc_leave_function();
            if (qvm.depth == exitdepth)
                return;     // all done
            break;

        case OP_STATE: {
            int self = QC_FromProg(qvm.g->self);
            qc_entvars_t *ev = QC_EV(self);
            ev->nextthink = qvm.g->time + 0.1f;
            if (a->_float != ev->frame)
                ev->frame = a->_float;
            ev->think = b->function;
            break;
        }

        default:
            QC_RunError("bad opcode %i", st->op);
        }
    }
}

/*
==============================================================================

LOOKUPS

==============================================================================
*/

qc_function_t *QC_FindFunction(const char *name)
{
    for (int i = 0; i < qvm.progs->numfunctions; i++) {
        qc_function_t *f = &qvm.functions[i];
        if (!strcmp(QC_Str(f->s_name), name))
            return f;
    }
    return NULL;
}

qc_def_t *QC_FindField(const char *name)
{
    for (int i = 0; i < qvm.progs->numfielddefs; i++) {
        qc_def_t *d = &qvm.fielddefs[i];
        if (!strcmp(QC_Str(d->s_name), name))
            return d;
    }
    return NULL;
}

qc_def_t *QC_FindGlobal(const char *name)
{
    for (int i = 0; i < qvm.progs->numglobaldefs; i++) {
        qc_def_t *d = &qvm.globaldefs[i];
        if (!strcmp(QC_Str(d->s_name), name))
            return d;
    }
    return NULL;
}

static qc_def_t *qc_field_at(int ofs)
{
    for (int i = 0; i < qvm.progs->numfielddefs; i++) {
        qc_def_t *d = &qvm.fielddefs[i];
        if (d->ofs == ofs)
            return d;
    }
    return NULL;
}

/*
==============================================================================

ENTITIES

==============================================================================
*/

void QC_ClearEdict(int num)
{
    memset(QC_EV(num), 0, qvm.edict_size);
    qvm.ed[num].free = false;
    qvm.ed[num].mdl_flags = 0;
}

/*
Either finds a free entity or allocates a new one. An entity freed less than
half a second ago is not reused, so a client still lerping it is not confused
- Quake's rule.
*/
int QC_Alloc(void)
{
    int i;

    for (i = game.maxclients + 1; i < qvm.num_edicts; i++) {
        qc_edict_t *e = &qvm.ed[i];
        if (e->free && !e->isstatic && (e->freetime < 2 || qvm.time - e->freetime > 0.5f)) {
            QC_ClearEdict(i);
            return i;
        }
    }

    if (i >= qvm.max_edicts) {
        // out of entities with gibs kept (g_no_janitor): the oldest gib goes
        int gib = KEEP_GIBS() ? Q1_OldestKeptGib() : 0;
        if (gib > 0) {
            QC_Free(gib);
            QC_ClearEdict(gib);
            return gib;
        }
        QC_RunError("no free edicts (%d)", qvm.max_edicts);
    }

    qvm.num_edicts++;
    if (globals.num_edicts < qvm.num_edicts)
        globals.num_edicts = qvm.num_edicts;
    QC_ClearEdict(i);
    return i;
}

/*
Marks the entity as free. Fields that might still be looked at (the free
flag, origin, model) are cleared and the entity is taken off the network.
*/
void QC_Free(int num)
{
    qc_entvars_t *ev = QC_EV(num);

    Q1_UnlinkEdict(num);

    qvm.ed[num].free = true;
    ev->model = 0;
    ev->takedamage = 0;
    ev->modelindex = 0;
    ev->colormap = 0;
    ev->skin = 0;
    ev->frame = 0;
    VectorClear(ev->origin);
    VectorClear(ev->angles);
    ev->nextthink = -1;
    ev->solid = 0;

    qvm.ed[num].freetime = qvm.time;

    Q1VM_SyncEdict(num);
}

static const char *qc_value_string(int type, const qc_eval_t *val)
{
    static char line[256];

    switch (type & ~QC_DEF_SAVEGLOBAL) {
    case QC_ev_string:
        Q_snprintf(line, sizeof(line), "%s", QC_Str(val->string));
        break;
    case QC_ev_entity:
        Q_snprintf(line, sizeof(line), "entity %i", QC_FromProg(val->edict));
        break;
    case QC_ev_function:
        if (val->function > 0 && val->function < qvm.progs->numfunctions)
            Q_snprintf(line, sizeof(line), "%s()", QC_Str(qvm.functions[val->function].s_name));
        else
            Q_snprintf(line, sizeof(line), "function %i", val->function);
        break;
    case QC_ev_field: {
        qc_def_t *def = qc_field_at(val->_int);
        Q_snprintf(line, sizeof(line), ".%s", def ? QC_Str(def->s_name) : "?");
        break;
    }
    case QC_ev_void:
        Q_snprintf(line, sizeof(line), "void");
        break;
    case QC_ev_float:
        Q_snprintf(line, sizeof(line), "%5.1f", val->_float);
        break;
    case QC_ev_vector:
        Q_snprintf(line, sizeof(line), "'%5.1f %5.1f %5.1f'", val->vector[0], val->vector[1], val->vector[2]);
        break;
    case QC_ev_pointer:
        Q_snprintf(line, sizeof(line), "pointer");
        break;
    default:
        Q_snprintf(line, sizeof(line), "bad type %i", type);
        break;
    }
    return line;
}

void QC_PrintEdict(int num)
{
    if (num < 0 || num >= qvm.num_edicts)
        return;
    if (qvm.ed[num].free) {
        gi.dprintf("EDICT %i: FREE\n", num);
        return;
    }

    gi.dprintf("EDICT %i:\n", num);
    for (int i = 1; i < qvm.progs->numfielddefs; i++) {
        qc_def_t *d = &qvm.fielddefs[i];
        const char *name = QC_Str(d->s_name);
        size_t len = strlen(name);
        if (len > 1 && name[len - 2] == '_')
            continue;   // skip _x, _y, _z vars

        int type = d->type & ~QC_DEF_SAVEGLOBAL;
        const int *v = (const int *)QC_Field(num, d->ofs);
        int words = type == QC_ev_vector ? 3 : 1;
        int j;
        for (j = 0; j < words; j++)
            if (v[j])
                break;
        if (j == words)
            continue;

        gi.dprintf("  %-15s %s\n", name, qc_value_string(type, (const qc_eval_t *)v));
    }
}

/*
==============================================================================

PROGS.DAT

==============================================================================
*/

void QC_FreeProgs(void)
{
    // everything is TAG_LEVEL, freed with the level
    memset(&qvm, 0, sizeof(qvm));
}

// the engine's fields must sit where qc_entvars_t says
static bool qc_check_fields(void)
{
    static const struct {
        const char  *name;
        int         ofs;
    } check[] = {
        { "modelindex", 0 }, { "origin", 10 }, { "classname", 28 }, { "touch", 42 },
        { "nextthink", 46 }, { "items", 58 }, { "view_ofs", 62 }, { "v_angle", 70 },
        { "flags", 76 }, { "spawnflags", 89 }, { "owner", 95 }, { "noise3", 104 },
    };

    for (int i = 0; i < q_countof(check); i++) {
        qc_def_t *d = QC_FindField(check[i].name);
        if (!d || d->ofs != check[i].ofs) {
            gi.dprintf("Quake 1 game: progs field %s is not at %d\n", check[i].name, check[i].ofs);
            return false;
        }
    }
    return true;
}

bool QC_LoadProgs(const char *episode)
{
    char    path[MAX_QPATH];
    void    *buf;
    int     len, i;

    memset(&qvm, 0, sizeof(qvm));
    Q_strlcpy(qvm.episode, episode, sizeof(qvm.episode));

    Q_snprintf(path, sizeof(path), "id1/%s/progs.dat", episode);
    len = gi.LoadFile(path, &buf);
    if (len < (int)sizeof(qc_progs_t) || !buf) {
        gi.dprintf("Quake 1 game: can't load %s\n", path);
        if (buf)
            gi.FreeFile(buf);
        return false;
    }

    // own copy, freed with the level
    qvm.file = gi.TagMalloc(len, TAG_LEVEL);
    memcpy(qvm.file, buf, len);
    gi.FreeFile(buf);

    qvm.progs = (qc_progs_t *)qvm.file;
    qc_progs_t *p = qvm.progs;

    if (p->version != QC_PROG_VERSION) {
        gi.dprintf("Quake 1 game: %s is version %i, not %i\n", path, p->version, QC_PROG_VERSION);
        return false;
    }
    if (p->crc != QC_PROGHEADER_CRC) {
        gi.dprintf("Quake 1 game: %s has the wrong header crc %i\n", path, p->crc);
        return false;
    }

#define QC_CHECK_LUMP(ofs, num, size) \
    if ((ofs) < 0 || (num) < 0 || (int64_t)(ofs) + (int64_t)(num) * (size) > len) { \
        gi.dprintf("Quake 1 game: %s is damaged\n", path); return false; }

    QC_CHECK_LUMP(p->ofs_statements, p->numstatements, sizeof(qc_statement_t));
    QC_CHECK_LUMP(p->ofs_globaldefs, p->numglobaldefs, sizeof(qc_def_t));
    QC_CHECK_LUMP(p->ofs_fielddefs, p->numfielddefs, sizeof(qc_def_t));
    QC_CHECK_LUMP(p->ofs_functions, p->numfunctions, sizeof(qc_function_t));
    QC_CHECK_LUMP(p->ofs_strings, p->numstrings, 1);
    QC_CHECK_LUMP(p->ofs_globals, p->numglobals, 4);

    qvm.functions = (qc_function_t *)((byte *)p + p->ofs_functions);
    qvm.statements = (qc_statement_t *)((byte *)p + p->ofs_statements);
    qvm.globaldefs = (qc_def_t *)((byte *)p + p->ofs_globaldefs);
    qvm.fielddefs = (qc_def_t *)((byte *)p + p->ofs_fielddefs);
    qvm.globals = (float *)((byte *)p + p->ofs_globals);
    qvm.g = (qc_globals_t *)qvm.globals;

    if (p->numglobals < (int)(sizeof(qc_globals_t) / 4) || p->entityfields < QC_SYSTEM_FIELDS) {
        gi.dprintf("Quake 1 game: %s is too small to be a Quake progs\n", path);
        return false;
    }

    // strings: the progs' table, the temp ring, the heap
    qvm.progs_strings = p->numstrings;
    qvm.strings_size = p->numstrings + QC_TEMPSTRINGS * QC_TEMPSTRING_SIZE + QC_STRING_HEAP;
    qvm.strings = gi.TagMalloc(qvm.strings_size, TAG_LEVEL);
    memset(qvm.strings, 0, qvm.strings_size);
    memcpy(qvm.strings, (byte *)p + p->ofs_strings, p->numstrings);
    qvm.heap_used = p->numstrings + QC_TEMPSTRINGS * QC_TEMPSTRING_SIZE;

    if (!qc_check_fields())
        return false;

    // a builtin declared "#0:name" (every rerelease ex_*) is found by name
    // and given a number of its own past the classic ones
    for (i = 0; i < p->numfunctions; i++) {
        qc_function_t *f = &qvm.functions[i];
        if (f->first_statement == 0 && i > 0) {
            int n = QC_ExBuiltinNumber(QC_Str(f->s_name));
            if (n > 0)
                f->first_statement = -n;
            else
                gi.dprintf("Quake 1 game: no builtin %s\n", QC_Str(f->s_name));
        }
        if (f->first_statement >= p->numstatements) {
            gi.dprintf("Quake 1 game: %s's function %s is out of range\n", path, QC_Str(f->s_name));
            return false;
        }
    }

    // entities
    qvm.edict_size = p->entityfields * 4;
    qvm.max_edicts = game.maxentities;
    qvm.fields = gi.TagMalloc((size_t)qvm.max_edicts * qvm.edict_size, TAG_LEVEL);
    memset(qvm.fields, 0, (size_t)qvm.max_edicts * qvm.edict_size);
    qvm.ed = gi.TagMalloc(sizeof(qc_edict_t) * qvm.max_edicts, TAG_LEVEL);
    memset(qvm.ed, 0, sizeof(qc_edict_t) * qvm.max_edicts);

    qc_def_t *d;
    qvm.fld_alpha = (d = QC_FindField("alpha")) && (d->type & ~QC_DEF_SAVEGLOBAL) == QC_ev_float ? d->ofs : -1;
    qvm.fld_scale = (d = QC_FindField("scale")) && (d->type & ~QC_DEF_SAVEGLOBAL) == QC_ev_float ? d->ofs : -1;
    qvm.fld_gravity = (d = QC_FindField("gravity")) && (d->type & ~QC_DEF_SAVEGLOBAL) == QC_ev_float ? d->ofs : -1;

    Q1_InitBuiltins();
    QC_LoadLocalization(episode);

    gi.dprintf("Quake 1 game: %s, %i functions, %i entity fields\n", path, p->numfunctions, p->entityfields);
    return true;
}

/*
==============================================================================

SPAWNING THE MAP

==============================================================================
*/

/*
Can parse either fields or globals. Returns false if there was an error.
*/
bool QC_ParseEpair(int num, qc_def_t *key, const char *s)
{
    qc_eval_t *d = QC_Field(num, key->ofs);

    switch (key->type & ~QC_DEF_SAVEGLOBAL) {
    case QC_ev_string:
        d->string = QC_NewString(s);
        break;

    case QC_ev_float:
        d->_float = atof(s);
        break;

    case QC_ev_vector: {
        vec3_t v = { 0, 0, 0 };
        sscanf(s, "%f %f %f", &v[0], &v[1], &v[2]);
        VectorCopy(v, d->vector);
        break;
    }

    case QC_ev_entity: {
        int n = atoi(s);
        d->edict = (n >= 0 && n < qvm.max_edicts) ? QC_ToProg(n) : 0;
        break;
    }

    case QC_ev_field: {
        qc_def_t *def = QC_FindField(s);
        if (!def) {
            gi.dprintf("Quake 1 game: can't find field %s\n", s);
            return false;
        }
        d->_int = QC_G_INT(def->ofs);
        break;
    }

    case QC_ev_function: {
        qc_function_t *func = QC_FindFunction(s);
        if (!func) {
            gi.dprintf("Quake 1 game: can't find function %s\n", s);
            return false;
        }
        d->function = (int)(func - qvm.functions);
        break;
    }

    default:
        break;
    }
    return true;
}

/*
Parses an entity out of the map's string, writing its fields. Quake's hacks
included: "angle" is a yaw, "light" is light_lev, keys starting with "_" are
the tools' and are skipped.
*/
static const char *qc_parse_edict(const char *data, int num)
{
    bool    init = false;
    char    keyname[256];
    char    value[1024];

    // clear it (the world was set up before)
    if (num != 0)
        memset(QC_EV(num), 0, qvm.edict_size);

    // go through all the dictionary pairs
    while (1) {
        const char *token;
        bool anglehack;

        // parse key
        token = COM_Parse(&data);
        if (token[0] == '}')
            break;
        if (!data)
            gi.error("Quake 1 game: entity text ends without a closing brace");

        Q_strlcpy(keyname, token, sizeof(keyname));
        if (!strcmp(keyname, "angle")) {
            Q_strlcpy(keyname, "angles", sizeof(keyname));
            anglehack = true;
        } else {
            anglehack = false;
            if (!strcmp(keyname, "light"))
                Q_strlcpy(keyname, "light_lev", sizeof(keyname));
        }

        // keynames with trailing spaces
        size_t n = strlen(keyname);
        while (n && keyname[n - 1] == ' ')
            keyname[--n] = 0;

        // parse value
        token = COM_Parse(&data);
        if (!data)
            gi.error("Quake 1 game: entity text ends without a closing brace");
        if (token[0] == '}')
            gi.error("Quake 1 game: closing brace without data");
        Q_strlcpy(value, token, sizeof(value));

        init = true;

        // keynames with a leading underscore are used for utility comments,
        // and are immediately discarded by quake
        if (keyname[0] == '_')
            continue;

        qc_def_t *key = QC_FindField(keyname);
        if (!key) {
            // Quake printed these; most are compiler keys ("wad", "origin" on a
            // brush model is not among them)
            continue;
        }

        if (anglehack) {
            char temp[64];
            Q_strlcpy(temp, value, sizeof(temp));
            Q_snprintf(value, sizeof(value), "0 %s 0", temp);
        }

        if (!QC_ParseEpair(num, key, value))
            gi.dprintf("Quake 1 game: bad value \"%s\" for %s\n", value, keyname);
    }

    if (!init)
        qvm.ed[num].free = true;

    return data;
}

/*
The world, then every entity in turn: parsed, dropped for the wrong skill or
deathmatch, then handed to the progs' function of the classname's name.
*/
void QC_LoadEntities(const char *data)
{
    int num = -1;
    int inhibit = 0;
    int skill = Q1VM_Skill();

    qvm.g->time = qvm.time;
    qvm.loading = true;

    while (1) {
        const char *token = COM_Parse(&data);
        if (!data)
            break;
        if (token[0] != '{')
            gi.error("Quake 1 game: found %s when expecting {", token);

        if (num < 0)
            num = 0;
        else
            num = QC_Alloc();

        data = qc_parse_edict(data, num);
        qc_entvars_t *ev = QC_EV(num);

        // remove things from different skill levels or deathmatch
        if (num != 0) {
            int sf = (int)ev->spawnflags;
            if (deathmatch->value) {
                if (sf & Q1_SPAWNFLAG_NOT_DEATHMATCH) {
                    QC_Free(num);
                    inhibit++;
                    continue;
                }
            } else if ((skill == 0 && (sf & Q1_SPAWNFLAG_NOT_EASY)) ||
                       (skill == 1 && (sf & Q1_SPAWNFLAG_NOT_MEDIUM)) ||
                       (skill >= 2 && (sf & Q1_SPAWNFLAG_NOT_HARD))) {
                QC_Free(num);
                inhibit++;
                continue;
            }
        }

        if (!ev->classname) {
            gi.dprintf("Quake 1 game: no classname for entity %i\n", num);
            if (num)
                QC_Free(num);
            continue;
        }

        const char *classname = QC_Str(ev->classname);
        if (num && nomonsters->value && !strncmp(classname, "monster_", 8)) {
            QC_Free(num);
            inhibit++;
            continue;
        }

        // look for the spawn function
        qc_function_t *func = QC_FindFunction(classname);
        if (!func) {
            gi.dprintf("Quake 1 game: no spawn function for %s\n", classname);
            if (num)
                QC_Free(num);
            continue;
        }

        // (any spawn function may write the world's fields while the map
        // loads, as in Quake: its check was sv.state == ss_active)
        qvm.g->self = QC_ToProg(num);
        QC_Execute((int)(func - qvm.functions));
    }

    qvm.loading = false;
    gi.dprintf("Quake 1 game: %i entities inhibited\n", inhibit);
}
