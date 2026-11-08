// d3d12_sound_bench.c's stand-in for a 4K path traced frame: passes that fill the
// whole GPU, as the game's do - ray queries from every pixel (incoherent after the
// first hit, like the bounce passes) and wide bandwidth passes (like the denoisers
// and DLSS), with a barrier between each. Built by the bench's header line:
//   dxc -T cs_6_5 -E ray_pass -Fo build/tools/sound_bench_ray.dxil   src/refresh/d3d12/tests/d3d12_sound_bench_frame.hlsl
//   dxc -T cs_6_5 -E blur_pass -Fo build/tools/sound_bench_blur.dxil src/refresh/d3d12/tests/d3d12_sound_bench_frame.hlsl

#define RS "SRV(t0), UAV(u0), UAV(u1), RootConstants(num32BitConstants=8, b0)"

RaytracingAccelerationStructure scene : register(t0);
RWStructuredBuffer<float4> image_a : register(u0);
RWStructuredBuffer<float4> image_b : register(u1);

cbuffer Pass : register(b0)
{
    uint pass_index;
    uint rays;          // ray_pass: rays per pixel; blur_pass: taps
    uint width;
    uint height;
    float3 eye;         // the camera, where the listener is
    uint pad;
};

uint hash(uint x)
{
    x ^= x >> 16; x *= 0x7feb352d;
    x ^= x >> 15; x *= 0x846ca68b;
    x ^= x >> 16;
    return x;
}

float3 random_dir(inout uint s)
{
    s = hash(s);
    const float u = (s & 0xffff) / 65535.0, v = (s >> 16) / 65535.0;
    const float z = u * 2 - 1, r = sqrt(saturate(1 - z * z)), a = v * 6.2831853;
    return float3(r * cos(a), r * sin(a), z);
}

[RootSignature(RS)]
[numthreads(8, 8, 1)]
void ray_pass(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;
    const uint idx = id.y * width + id.x;
    uint seed = hash(idx ^ (pass_index * 0x9e3779b9));

    // a camera at the listener, 90 degrees across
    float3 origin = eye;
    float3 dir = normalize(float3(1, (id.x / (float)width) * 2 - 1, 1 - (id.y / (float)height) * 2));
    float4 acc = image_a[idx];

    for (uint i = 0; i < rays; i++) {
        RayDesc r;
        r.Origin = origin;
        r.Direction = dir;
        r.TMin = 0.001;
        r.TMax = 1000;
        RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
        q.TraceRayInline(scene, RAY_FLAG_NONE, 0xff, r);
        q.Proceed();
        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
            const float t = q.CommittedRayT();
            origin += dir * t * 0.999;
            acc += float4(t, q.CommittedPrimitiveIndex() & 255, 1, 0) * 0.001;
        }
        dir = random_dir(seed);     // the next ray a bounce's
    }
    image_a[idx] = acc;
}

[RootSignature(RS)]
[numthreads(8, 8, 1)]
void blur_pass(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;
    const int step = (int)(1u << (pass_index & 3));
    float4 sum = 0;
    for (uint i = 0; i < rays; i++) {
        const int dx = ((int)(i % 5) - 2) * step, dy = ((int)(i / 5) - 2) * step;
        const uint x = (uint)clamp((int)id.x + dx, 0, (int)width - 1);
        const uint y = (uint)clamp((int)id.y + dy, 0, (int)height - 1);
        sum += image_a[y * width + x];
    }
    image_b[id.y * width + id.x] = sum / rays;
}
