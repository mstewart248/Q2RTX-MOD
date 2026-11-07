/*
Clustered dynamic geometry, API-free - see clusters.h.
*/

#include "clusters.h"

#include <string.h>

// the index list's room at the staging's start
#define INDEX_BYTES     256

bool pt_cluster_plan(pt_cluster_plan_t *plan, const uint32_t *num_prims, uint32_t num_runs)
{
    memset(plan, 0, sizeof(*plan));
    if (num_runs > PT_CLUSTER_MAX_BLASES)
        return false;
    plan->num_runs = num_runs;
    for (uint32_t i = 0; i < num_runs; i++) {
        const uint32_t n = num_prims[i];
        const uint32_t clusters = PT_CLUSTER_COUNT(n);
        plan->num_prims[i] = n;
        plan->first_cluster[i] = plan->num_clusters;
        plan->clusters[i] = clusters;
        plan->blas_slot[i] = -1;
        if (!n)
            continue;
        if (clusters - 1 > PT_CLUSTER_MAX_GEOMETRY_INDEX)
            return false;
        plan->blas_slot[i] = (int)plan->num_blases++;
        plan->num_clusters += clusters;
        plan->num_triangles += n;
        if (clusters > plan->max_clusters_per_blas)
            plan->max_clusters_per_blas = clusters;
    }
    return true;
}

void pt_cluster_layout(const pt_cluster_plan_t *plan, pt_cluster_layout_t *layout)
{
    layout->triangle_args = INDEX_BYTES;
    layout->blas_args = layout->triangle_args + (uint64_t)plan->num_clusters * sizeof(pt_cluster_args_t);
    layout->blas_addresses = layout->blas_args + (uint64_t)plan->num_blases * sizeof(pt_cluster_blas_args_t);
    layout->size = layout->blas_addresses + (uint64_t)plan->num_blases * sizeof(uint64_t);
}

void pt_cluster_stage(const pt_cluster_plan_t *plan, const pt_cluster_layout_t *layout,
                      void *staging, uint64_t staging_gpu, const uint64_t *run_vertices,
                      const uint64_t *blas_address, uint64_t clas_addresses, uint32_t blas_stride)
{
    uint8_t *base = staging;

    // every cluster's: vertex j of its unindexed list is vertex j
    for (int j = 0; j < PT_CLUSTER_VERTICES; j++)
        base[j] = (uint8_t)j;

    pt_cluster_args_t *tri = (pt_cluster_args_t *)(base + layout->triangle_args);
    pt_cluster_blas_args_t *blas = (pt_cluster_blas_args_t *)(base + layout->blas_args);
    uint64_t *addresses = (uint64_t *)(base + layout->blas_addresses);
    for (uint32_t i = 0; i < plan->num_runs; i++) {
        const int slot = plan->blas_slot[i];
        if (slot < 0)
            continue;
        uint32_t rest = plan->num_prims[i];
        for (uint32_t k = 0; k < plan->clusters[i]; k++) {
            const uint32_t triangles = rest < PT_CLUSTER_TRIANGLES ? rest : PT_CLUSTER_TRIANGLES;
            rest -= triangles;
            pt_cluster_args_t *a = &tri[plan->first_cluster[i] + k];
            memset(a, 0, sizeof(*a));
            a->cluster_id = k;
            a->counts = triangles | (3 * triangles) << 9 | PT_CLUSTER_INDEX_8BIT << 24;
            a->base_geometry_index_and_flags = k;
            a->index_stride = 1;
            a->vertex_stride = 3 * sizeof(float);
            a->indices = staging_gpu;
            a->vertices = run_vertices[i] + (uint64_t)k * PT_CLUSTER_TRIANGLES * 9 * sizeof(float);
        }
        blas[slot].cluster_count = plan->clusters[i];
        blas[slot].stride = blas_stride;
        blas[slot].clusters = clas_addresses + (uint64_t)plan->first_cluster[i] * sizeof(uint64_t);
        addresses[slot] = blas_address[i];
    }
}
