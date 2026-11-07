/*
THE PARTITIONED TLAS, API-free (both renderers) - see ptlas.h.
*/

#include "ptlas.h"

#include <stdlib.h>
#include <string.h>

typedef char ptlas_instance_size_check[sizeof(ptlas_instance_t) == 64 ? 1 : -1];
typedef char ptlas_write_size_check[sizeof(ptlas_write_t) == 104 ? 1 : -1];
typedef char ptlas_op_size_check[sizeof(ptlas_op_t) == 24 ? 1 : -1];

// D3D12_RTAS_PARTITIONED_TLAS_OPERATION_WRITE_INSTANCE_ARGS (the preview's
// d3d12.h): the same fields as ptlas_write_t, the address before the indices
// and the bounds last
typedef struct {
    float       transform[12];
    uint32_t    instance_id;
    uint32_t    instance_mask;
    uint32_t    sbt_offset;
    uint32_t    flags;
    uint64_t    blas;
    uint32_t    instance_index;
    uint32_t    partition_index;
    float       aabb[6];
} ptlas_preview_write_t;
typedef char ptlas_preview_write_size_check[sizeof(ptlas_preview_write_t) == 104 ? 1 : -1];

static uint32_t ptlas_generation;

void ptlas_static_changed(void)
{
    ptlas_generation++;
}

bool ptlas_init(ptlas_t *p, uint32_t max_instances, uint32_t max_global)
{
    memset(p, 0, sizeof(*p));
    if (!max_instances)
        max_instances = 1;
    p->held = calloc(max_instances, sizeof(*p->held));
    p->writes = calloc(max_instances, sizeof(*p->writes));
    if (!p->held || !p->writes) {
        ptlas_shutdown(p);
        return false;
    }
    p->max_instances = max_instances;
    p->max_global = max_global;
    return true;
}

void ptlas_shutdown(ptlas_t *p)
{
    free(p->held);
    free(p->writes);
    memset(p, 0, sizeof(*p));
}

void ptlas_invalidate(ptlas_t *p)
{
    p->built = false;
}

uint32_t ptlas_partition_count(const ptlas_t *p)
{
    const uint32_t n = (p->max_instances + PTLAS_INSTANCES_PER_PARTITION - 1) / PTLAS_INSTANCES_PER_PARTITION;
    return n ? n : 1;
}

static void write_op(ptlas_write_t *w, const ptlas_instance_t *d, uint32_t index, uint32_t num_global)
{
    memset(w, 0, sizeof(*w));
    if (d) {
        memcpy(w->transform, d->transform, sizeof(w->transform));
        w->instance_id = d->id_mask & 0xffffffu;
        w->instance_mask = d->id_mask >> 24;
        w->sbt_offset = d->sbt_flags & 0xffffffu;
        // the low four bits are the same in every API; above them a TLAS
        // instance has the OMM flags and a PTLAS one ENABLE_EXPLICIT_AABB
        w->flags = (d->sbt_flags >> 24) & 0xfu;
        w->blas = d->blas;
    }
    w->instance_index = index;
    w->partition_index = index < num_global ? PTLAS_PARTITION_GLOBAL
                                            : (index - num_global) / PTLAS_INSTANCES_PER_PARTITION;
}

bool ptlas_diff(ptlas_t *p, const void *instances_v, uint32_t count, uint32_t num_global,
                uint32_t first_volatile, uint32_t build_flags)
{
    const ptlas_instance_t *instances = instances_v;
    p->num_writes = 0;
    if (!p->held)
        return false;
    if (count > p->max_instances)
        count = p->max_instances;
    if (num_global > p->max_global)
        num_global = p->max_global;
    if (num_global > count)
        num_global = count;

    // from nothing: a first build, other build flags, a different global split
    // (every partition index moves), a static BLAS rebuilt in place
    if (!p->built || build_flags != p->build_flags || num_global != p->held_global ||
        p->generation != ptlas_generation) {
        p->built = false;
        memset(p->held, 0, sizeof(*p->held) * p->max_instances);
        p->held_count = 0;
    }

    // every index at most once, so at most max(count, held_count) <= max_instances ops
    uint32_t n = 0;
    for (uint32_t i = 0; i < count; i++) {
        const ptlas_instance_t *d = &instances[i];
        if (p->built && i < first_volatile && !memcmp(&p->held[i], d, sizeof(*d)))
            continue;
        // absent in a fresh PTLAS already
        if (!p->built && !d->blas)
            continue;
        write_op(&p->writes[n++], d, i, num_global);
        p->held[i] = *d;
    }
    // indices that fell out of use: a null BLAS makes them inactive
    for (uint32_t i = count; i < p->held_count; i++) {
        if (!p->held[i].blas)
            continue;
        write_op(&p->writes[n++], NULL, i, num_global);
        memset(&p->held[i], 0, sizeof(p->held[i]));
    }

    p->num_writes = n;
    p->held_count = count;
    p->held_global = num_global;
    p->build_flags = build_flags;
    p->generation = ptlas_generation;
    // nothing changed since a build that is still good: the PTLAS stands as it is
    return !p->built || n != 0;
}

bool ptlas_diff_is_update(const ptlas_t *p)
{
    return p->built;
}

uint64_t ptlas_stage_size(uint32_t max_instances)
{
    const uint64_t size = PTLAS_STAGE_ARGS + (uint64_t)max_instances * sizeof(ptlas_write_t);
    return (size + 255) & ~(uint64_t)255;
}

void ptlas_stage(ptlas_t *p, void *staging, uint64_t staging_gpu, bool preview)
{
    uint8_t *bytes = staging;
    // a build from nothing with nothing to write: no op, an empty PTLAS
    const uint32_t op_count = p->num_writes ? 1 : 0;
    memcpy(bytes, &op_count, sizeof(op_count));

    ptlas_op_t op;
    memset(&op, 0, sizeof(op));
    op.type = PTLAS_OP_WRITE_INSTANCE;
    op.count = p->num_writes;
    op.address = staging_gpu + PTLAS_STAGE_ARGS;
    op.stride = sizeof(ptlas_write_t);
    memcpy(bytes + PTLAS_STAGE_OP, &op, sizeof(op));

    if (!preview) {
        memcpy(bytes + PTLAS_STAGE_ARGS, p->writes, sizeof(ptlas_write_t) * p->num_writes);
    } else {
        for (uint32_t i = 0; i < p->num_writes; i++) {
            const ptlas_write_t *w = &p->writes[i];
            ptlas_preview_write_t v;
            memcpy(v.transform, w->transform, sizeof(v.transform));
            v.instance_id = w->instance_id;
            v.instance_mask = w->instance_mask;
            v.sbt_offset = w->sbt_offset;
            v.flags = w->flags;
            v.blas = w->blas;
            v.instance_index = w->instance_index;
            v.partition_index = w->partition_index;
            memcpy(v.aabb, w->aabb, sizeof(v.aabb));
            memcpy(bytes + PTLAS_STAGE_ARGS + (uint64_t)i * sizeof(v), &v, sizeof(v));
        }
    }
    p->built = true;
}
