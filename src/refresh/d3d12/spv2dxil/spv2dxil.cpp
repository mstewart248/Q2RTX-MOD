/*
spv2dxil - turns the Vulkan renderer's SPIR-V into DXIL for the D3D12 renderer.

    spv2dxil --dxc <dir of dxcompiler.dll> [--sm 6_6] [--hlsl out.hlsl] [--spec id=value]...
             [--freeze] [--raygen | --ser] in.spv out.dxil

--spec sets a specialization constant, as vkpt's VkSpecializationInfo does
(see to_dxil); cmake's compile_shader_d3d12_variant builds the variants.
--freeze puts the --spec values into the SPIR-V instead, and drops the code
they make dead before SPIRV-Cross sees it, with the SDK's spirv-opt
(freeze_specs says when that matters).
--raygen turns a compute shader into a ray generation shader in a DXIL library,
--ser does that and gives the SER marker its body (to_raygen).

ONE SHADER SOURCE. src/refresh/vkpt/shader/*.glsl stays the only copy of every
shader. The build compiles it with glslang a second time, with -DVKPT_D3D12
(cmake/compileShaders.cmake, compile_shader_d3d12), and this tool takes that
SPIR-V through SPIRV-Cross to HLSL and DXC to signed DXIL. A shader change
therefore reaches both renderers, which is the only way "the same features"
stays true for more than a week.

THE BINDING RULE (d3d12/d3d12.h, D3D12_SPACE). Vulkan binds by (set, binding),
where a binding may hold an array; HLSL binds by (register, space), where an
array of N takes N registers - so a straight set->space, binding->register map
overlaps as soon as a set has an array that is not last (light_stats_bufers[3]
at binding 9 runs into binding 10), and an unbounded array swallows everything
after it. Instead every Vulkan binding gets a register space of its own:

    (set s, binding b)   ->  register 0, space 1000 * (s + 1) + b
    push constants       ->  b0, space 0        (root constants)
    gl_NumWorkGroups     ->  b0, space 1        (a root CBV, when used)

The D3D12 side builds root signatures from the same rule, one descriptor
range per binding, so it never needs reflection data from here.

Descriptor kinds are fixed by the Vulkan descriptor type, never by how one
shader happens to use a resource: storage buffers are always UAVs (a readonly
SSBO would otherwise be an SRV in one shader and a UAV in the next, against
the same descriptor), storage images are always UAVs, sampled images SRVs,
acceleration structures SRVs, uniform buffers CBVs.

Unused resources are dropped (SPIRV-Cross's active-variable set), as every
shader includes global_textures.h and most of its bindings are dead in any
one of them.

Ray tracing PIPELINE stages (rgen/rchit/rahit/rmiss) are not supported by
SPIRV-Cross's HLSL backend. The D3D12 renderer is ray query only - the
faster mode on this renderer anyway (memory: rtapi-query-vs-pipeline).
*/

// no WIN32_LEAN_AND_MEAN: dxcapi.h needs the COM headers it drops (IUnknown, BSTR)
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#define NOMINMAX
#include <windows.h>

#include <dxc/dxcapi.h>
#include <spirv_cross/spirv_cross_c.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <string>
#include <vector>

static const unsigned SPACE_PER_SET = 1000;
static const unsigned SPACE_PUSH_CONSTANTS = 0;
static const unsigned SPACE_NUM_WORKGROUPS = 1;
// a descriptor set number no shader uses, to hang gl_NumWorkGroups on
static const unsigned NUM_WORKGROUPS_DESC_SET = 0x7ffe;

static bool g_verbose;

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "spv2dxil: error: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

static std::vector<char> read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        die("cannot open %s", path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<char> data(n > 0 ? (size_t)n : 0);
    if (n > 0 && fread(data.data(), 1, (size_t)n, f) != (size_t)n)
        die("cannot read %s", path);
    fclose(f);
    return data;
}

// Written to a temporary name and renamed, so a failed or interrupted run never
// leaves a truncated file that ninja would then think is up to date.
static void write_file(const std::string &path, const void *data, size_t size)
{
    std::string tmp = path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f)
        die("cannot write %s", tmp.c_str());
    if (size && fwrite(data, 1, size, f) != size)
        die("cannot write %s", tmp.c_str());
    fclose(f);
    if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
        die("cannot replace %s", path.c_str());
}

static void spvc_error(void *, const char *msg)
{
    fprintf(stderr, "spv2dxil: SPIRV-Cross: %s\n", msg);
}

static unsigned binding_space(unsigned set, unsigned binding)
{
    return SPACE_PER_SET * (set + 1) + binding;
}

static void map_binding(spvc_compiler comp, SpvExecutionModel model, unsigned set, unsigned binding, unsigned space)
{
    spvc_hlsl_resource_binding b;
    spvc_hlsl_resource_binding_init(&b);
    b.stage = model;
    b.desc_set = set;
    b.binding = binding;
    b.cbv.register_space = b.srv.register_space = b.uav.register_space = b.sampler.register_space = space;
    b.cbv.register_binding = b.srv.register_binding = b.uav.register_binding = b.sampler.register_binding = 0;
    if (spvc_compiler_hlsl_add_resource_binding(comp, &b) != SPVC_SUCCESS)
        die("cannot map set %u binding %u", set, binding);
}

static std::string to_hlsl(const std::vector<char> &spv, unsigned sm, const char **profile_stage)
{
    if (spv.size() % 4 || spv.size() < 20)
        die("not SPIR-V");

    spvc_context ctx = nullptr;
    if (spvc_context_create(&ctx) != SPVC_SUCCESS)
        die("spvc_context_create failed");
    spvc_context_set_error_callback(ctx, spvc_error, nullptr);

    spvc_parsed_ir ir = nullptr;
    if (spvc_context_parse_spirv(ctx, (const SpvId *)spv.data(), spv.size() / 4, &ir) != SPVC_SUCCESS)
        die("cannot parse SPIR-V");

    spvc_compiler comp = nullptr;
    if (spvc_context_create_compiler(ctx, SPVC_BACKEND_HLSL, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &comp) != SPVC_SUCCESS)
        die("cannot create the HLSL compiler");

    SpvExecutionModel model = spvc_compiler_get_execution_model(comp);
    switch (model) {
    case SpvExecutionModelGLCompute: *profile_stage = "cs"; break;
    case SpvExecutionModelVertex:    *profile_stage = "vs"; break;
    case SpvExecutionModelFragment:  *profile_stage = "ps"; break;
    default:
        die("execution model %d has no HLSL form here (ray tracing pipeline stages are not supported; "
            "the D3D12 renderer is ray query only)", (int)model);
    }

    spvc_compiler_options opts = nullptr;
    spvc_compiler_create_compiler_options(comp, &opts);
    spvc_compiler_options_set_uint(opts, SPVC_COMPILER_OPTION_HLSL_SHADER_MODEL, sm);
    spvc_compiler_options_set_bool(opts, SPVC_COMPILER_OPTION_HLSL_ENABLE_16BIT_TYPES, SPVC_TRUE);
    spvc_compiler_options_set_bool(opts, SPVC_COMPILER_OPTION_HLSL_FORCE_STORAGE_BUFFER_AS_UAV, SPVC_TRUE);
    spvc_compiler_options_set_bool(opts, SPVC_COMPILER_OPTION_HLSL_NONWRITABLE_UAV_TEXTURE_AS_SRV, SPVC_FALSE);
    // Vulkan clip space has +y down, D3D +y up. Flipping the vertex shader's
    // output y puts every pixel in the same row of the same image as on Vulkan
    // (row 0 at the top), so compute shaders reading what a vertex pipeline drew
    // need no change. It also mirrors the winding: D3D12 pipelines built from
    // these must cull with that in mind (draw.c culls nothing).
    if (model == SpvExecutionModelVertex)
        spvc_compiler_options_set_bool(opts, SPVC_COMPILER_OPTION_FLIP_VERTEX_Y, SPVC_TRUE);
    spvc_compiler_install_compiler_options(comp, opts);

    // gl_NumWorkGroups has no HLSL builtin; SPIRV-Cross reads it from a cbuffer
    // it adds here. That cbuffer is never "active" (the shader loads the builtin,
    // not it), so the active-variable filter below would hide its declaration.
    spvc_variable_id nwg = spvc_compiler_hlsl_remap_num_workgroups_builtin(comp);

    // Only what this shader actually touches - except when the cbuffer above is
    // needed, then everything is declared and DXC drops the dead ones itself
    // (every declared one is still mapped below, so none can overlap).
    spvc_set active = nullptr;
    spvc_compiler_get_active_interface_variables(comp, &active);
    if (!nwg)
        spvc_compiler_set_enabled_interface_variables(comp, active);
    spvc_resources res = nullptr;
    if (spvc_compiler_create_shader_resources(comp, &res) != SPVC_SUCCESS)
        die("cannot reflect resources");

    static const spvc_resource_type kinds[] = {
        SPVC_RESOURCE_TYPE_UNIFORM_BUFFER,
        SPVC_RESOURCE_TYPE_STORAGE_BUFFER,
        SPVC_RESOURCE_TYPE_STORAGE_IMAGE,
        SPVC_RESOURCE_TYPE_SAMPLED_IMAGE,
        SPVC_RESOURCE_TYPE_SEPARATE_IMAGE,
        SPVC_RESOURCE_TYPE_SEPARATE_SAMPLERS,
        SPVC_RESOURCE_TYPE_ACCELERATION_STRUCTURE,
    };
    for (spvc_resource_type kind : kinds) {
        const spvc_reflected_resource *list = nullptr;
        size_t count = 0;
        spvc_resources_get_resource_list_for_type(res, kind, &list, &count);
        for (size_t i = 0; i < count; i++) {
            unsigned set = spvc_compiler_get_decoration(comp, list[i].id, SpvDecorationDescriptorSet);
            unsigned binding = spvc_compiler_get_decoration(comp, list[i].id, SpvDecorationBinding);
            map_binding(comp, model, set, binding, binding_space(set, binding));
            if (g_verbose)
                printf("  %-40s set %u binding %u -> space %u\n", list[i].name, set, binding, binding_space(set, binding));
        }
    }

    // push constants -> root constants at b0 space0
    map_binding(comp, model, SPVC_HLSL_PUSH_CONSTANT_DESC_SET, SPVC_HLSL_PUSH_CONSTANT_BINDING, SPACE_PUSH_CONSTANTS);

    if (nwg) {
        spvc_compiler_set_decoration(comp, nwg, SpvDecorationDescriptorSet, NUM_WORKGROUPS_DESC_SET);
        spvc_compiler_set_decoration(comp, nwg, SpvDecorationBinding, 0);
        map_binding(comp, model, NUM_WORKGROUPS_DESC_SET, 0, SPACE_NUM_WORKGROUPS);
    }

    const char *src = nullptr;
    if (spvc_compiler_compile(comp, &src) != SPVC_SUCCESS)
        die("SPIRV-Cross could not write HLSL: %s", spvc_context_get_last_error_string(ctx));
    std::string out = src;
    spvc_context_destroy(ctx);
    return out;
}

/* THE RAY GENERATION FORM (--raygen, --ser; the D3D12 renderer's pt_ser).
   MaybeReorderThread exists only in ray generation shaders, so a pass that wants SER
   is built again as one. SPIRV-Cross ends every compute shader with the same entry:

       [numthreads(8, 8, 1)]
       void main(SPIRV_Cross_Input stage_input)
       {
           gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
           comp_main();
       }

   and that is all that changes:

       [shader("raygeneration")]
       void main()
       {
           gl_GlobalInvocationID = DispatchRaysIndex();
           comp_main();
       }

   The renderer dispatches the same grid (DispatchRays over every thread the groups
   had), so each invocation keeps its launch ID; workgroup IDs follow from it. Tracing
   stays inline RayQuery: the library needs no hit groups. What a ray generation shader
   cannot have - groupshared memory, group barriers, gl_NumWorkGroups - stops the build.

   --ser also replaces the empty body of the GLSL marker d3d12_ser_reorder(hint, bits)
   (path_tracer_rgen.h) with dx::MaybeReorderThread(hint, bits). Without --ser it stays
   empty, so --raygen measures what the dispatch path costs by itself. */
static std::string to_raygen(const std::string &hlsl, const char *name, bool ser)
{
    for (const char *bad : { "groupshared", "GroupMemoryBarrier", "AllMemoryBarrier", "SPIRV_Cross_NumWorkgroups" })
        if (hlsl.find(bad) != std::string::npos)
            die("%s: --raygen, but the shader uses %s, which a ray generation shader cannot", name, bad);

    std::smatch m;
    static const std::regex entry_re(
        "\\[numthreads\\((\\d+), (\\d+), (\\d+)\\)\\]\\s*\\n"
        "void main\\(SPIRV_Cross_Input stage_input\\)\\s*\\n\\{\\n([^}]*)\\}");
    if (!std::regex_search(hlsl, m, entry_re))
        die("%s: --raygen, but no SPIRV-Cross compute entry was found", name);
    const std::string size = "uint3(" + m[1].str() + ", " + m[2].str() + ", " + m[3].str() + ")";

    std::string body;
    std::string lines = m[4].str();
    static const std::regex input_re("^\\s*(\\w+) = stage_input\\.(\\w+);$");
    size_t pos = 0;
    while (pos < lines.size()) {
        size_t end = lines.find('\n', pos);
        if (end == std::string::npos)
            end = lines.size();
        std::string line = lines.substr(pos, end - pos);
        pos = end + 1;
        if (line.find_first_not_of(" \t\r") == std::string::npos)
            continue;
        std::smatch im;
        if (std::regex_match(line, im, input_re)) {
            const std::string var = im[1].str();
            std::string value;
            if (var == "gl_GlobalInvocationID")
                value = "DispatchRaysIndex()";
            else if (var == "gl_WorkGroupID")
                value = "DispatchRaysIndex() / " + size;
            else if (var == "gl_LocalInvocationID")
                value = "DispatchRaysIndex() % " + size;
            else if (var == "gl_LocalInvocationIndex")
                value = "dot(DispatchRaysIndex() % " + size + ", uint3(1, " + m[1].str() + ", " + m[1].str() +
                        " * " + m[2].str() + "))";
            else
                die("%s: --raygen, but the entry reads %s, which has no ray generation form here", name, var.c_str());
            body += "    " + var + " = " + value + ";\n";
        } else if (line.find("comp_main();") != std::string::npos) {
            body += "    comp_main();\n";
        } else {
            die("%s: --raygen, but the entry has a line this does not know: %s", name, line.c_str());
        }
    }

    std::string out = hlsl.substr(0, m.position(0)) + "[shader(\"raygeneration\")]\nvoid main()\n{\n" + body + "}" +
                      hlsl.substr(m.position(0) + m.length(0));

    if (ser) {
        static const std::regex marker_re(
            "void d3d12_ser_reorder\\(uint (\\w+), uint (\\w+)\\)\\s*\\n\\{\\s*\\}");
        std::smatch mm;
        if (!std::regex_search(out, mm, marker_re))
            die("%s: --ser, but the shader has no empty d3d12_ser_reorder(uint, uint) (path_tracer_rgen.h)", name);
        out = out.substr(0, mm.position(0)) + "void d3d12_ser_reorder(uint " + mm[1].str() + ", uint " + mm[2].str() +
              ")\n{\n    dx::MaybeReorderThread(" + mm[1].str() + ", " + mm[2].str() + ");\n}" +
              out.substr(mm.position(0) + mm.length(0));
    }
    return out;
}

/* SPECIALIZATION CONSTANTS. SPIRV-Cross writes each one as
       #ifndef SPIRV_CROSS_CONSTANT_ID_<id>
       #define SPIRV_CROSS_CONSTANT_ID_<id> <default>
       #endif
   so a variant is the same HLSL compiled with -D SPIRV_CROSS_CONSTANT_ID_<id>=<value>u -
   what vkpt passes as VkSpecializationInfo at pipeline creation. */
struct spec_value_t {
    unsigned id;
    unsigned value;
};

static std::vector<char> to_dxil(const std::string &hlsl, const char *dxc_dir, const char *stage, const char *sm,
                                 const char *src_name, const std::vector<spec_value_t> &specs)
{
    std::string dll = std::string(dxc_dir) + "\\dxcompiler.dll";
    HMODULE lib = LoadLibraryExA(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!lib)
        die("cannot load %s", dll.c_str());
    auto create = (DxcCreateInstanceProc)GetProcAddress(lib, "DxcCreateInstance");
    if (!create)
        die("%s has no DxcCreateInstance", dll.c_str());

    IDxcCompiler3 *compiler = nullptr;
    IDxcUtils *utils = nullptr;
    if (FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))) ||
        FAILED(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils))))
        die("DxcCreateInstance failed");

    std::wstring profile;
    for (const char *p = stage; *p; p++) profile += (wchar_t)*p;
    profile += L'_';
    for (const char *p = sm; *p; p++) profile += (wchar_t)*p;
    std::wstring name;
    for (const char *p = src_name; *p; p++) name += (wchar_t)*p;

    // a library (--raygen) exports its entries by their [shader] attributes: no -E
    const bool library = !strcmp(stage, "lib");
    std::vector<LPCWSTR> args = {
        name.c_str(),
        L"-T", profile.c_str(),
        L"-HV", L"2021",
        L"-enable-16bit-types",
        L"-O3",
        L"-Qstrip_debug",
        // SPIRV-Cross writes (1 << n) for GLSL's int shifts; DXC warns that the
        // literal's width is ambiguous, but SPIR-V already fixed it at 32 bits
        L"-Wno-ambig-lit-shift",
    };
    if (!library) {
        args.push_back(L"-E");
        args.push_back(L"main");
    } else {
        // only the entry is exported; everything else inlines into it as in the compute build
        args.push_back(L"-default-linkage");
        args.push_back(L"internal");
    }
    std::vector<std::wstring> defines;
    defines.reserve(specs.size());
    for (const spec_value_t &s : specs) {
        char d[64];
        snprintf(d, sizeof(d), "SPIRV_CROSS_CONSTANT_ID_%u=%uu", s.id, s.value);
        std::wstring w;
        for (const char *p = d; *p; p++) w += (wchar_t)*p;
        defines.push_back(w);
    }
    for (const std::wstring &d : defines) {
        args.push_back(L"-D");
        args.push_back(d.c_str());
    }

    DxcBuffer buf = {};
    buf.Ptr = hlsl.data();
    buf.Size = hlsl.size();
    buf.Encoding = DXC_CP_UTF8;

    IDxcResult *result = nullptr;
    if (FAILED(compiler->Compile(&buf, args.data(), (UINT32)args.size(), nullptr, IID_PPV_ARGS(&result))))
        die("IDxcCompiler3::Compile failed");

    IDxcBlobUtf8 *errors = nullptr;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
    HRESULT status = E_FAIL;
    result->GetStatus(&status);
    if (errors && errors->GetStringLength() && (FAILED(status) || g_verbose))
        fprintf(stderr, "%s", errors->GetStringPointer());
    if (FAILED(status))
        die("DXC failed on %s", src_name);

    IDxcBlob *object = nullptr;
    if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) || !object)
        die("DXC returned no object");

    // A DXIL container's 16-byte digest is zero when it went out unsigned, and
    // D3D12 refuses unsigned DXIL outside developer mode. DXC 1.8.2505+ signs
    // by itself; an older dxcompiler.dll needs dxil.dll beside it.
    const unsigned char *bytes = (const unsigned char *)object->GetBufferPointer();
    static const unsigned char zero[16] = { 0 };
    if (object->GetBufferSize() < 20 || memcmp(bytes, "DXBC", 4) || !memcmp(bytes + 4, zero, 16))
        die("%s: DXIL is not signed - use a newer dxcompiler.dll or put dxil.dll beside it", src_name);

    std::vector<char> out((const char *)object->GetBufferPointer(),
                          (const char *)object->GetBufferPointer() + object->GetBufferSize());
    object->Release();
    if (errors) errors->Release();
    result->Release();
    utils->Release();
    compiler->Release();
    return out;
}

// whether the module decorates a specialization constant with this SpecId
static bool spv_has_spec_id(const std::vector<char> &spv, unsigned id)
{
    const uint32_t *w = (const uint32_t *)spv.data();
    const size_t n = spv.size() / 4;
    for (size_t i = 5; i < n;) {
        const uint32_t count = w[i] >> 16, op = w[i] & 0xffff;
        if (count == 0)
            break;
        // OpDecorate <target> SpecId <literal>
        if (op == 71 && count >= 4 && i + 3 < n && w[i + 2] == 1 && w[i + 3] == id)
            return true;
        i += count;
    }
    return false;
}

/* --freeze: the --spec values go into the SPIR-V itself, and what they make dead goes
before SPIRV-Cross sees it - by spirv-opt.exe from the same SDK as DXC (beside
dxcompiler.dll; the SDK's SPIRV-Tools DLL has no optimizer in it). Without it, DXC
folds the constants (#defines, to_dxil) only after it has allocated every RayQuery
the HLSL declares at file scope - SPIRV-Cross puts a GLSL global rayQueryEXT there -
and a variant whose constants leave no ray query at all then fails validation,
"Flags must match usage" (Raytracing_Tier_1_1 declared, not used): primary_rays'
Flow pass, spec_flow_mode 2. The other variants keep the #define route, which is
what they were checked with. */
static std::vector<char> freeze_specs(const std::vector<char> &spv, const char *in, const char *out,
                                      const char *dxc_dir, const std::vector<spec_value_t> &specs, const char *base)
{
    std::string values;
    for (const spec_value_t &s : specs) {
        if (!spv_has_spec_id(spv, s.id))
            die("%s: --spec %u=%u, but the shader has no specialization constant %u", base, s.id, s.value, s.id);
        char pair[32];
        snprintf(pair, sizeof(pair), "%s%u:%u", values.empty() ? "" : " ", s.id, s.value);
        values += pair;
    }

    // the values, every constant frozen at them or its default, the expressions
    // over them folded, then the dead branches, functions and globals gone
    const std::string frozen = std::string(out) + ".frozen.spv";
    const std::string cmd = "\"" + std::string(dxc_dir) + "\\spirv-opt.exe\" --target-env=vulkan1.2"
        " --set-spec-const-default-value \"" + values + "\""
        " --freeze-spec-const --fold-spec-const-op-composite --eliminate-dead-branches"
        " --eliminate-dead-functions --eliminate-dead-code-aggressive --eliminate-dead-variables"
        " \"" + std::string(in) + "\" -o \"" + frozen + "\"";
    if (g_verbose)
        fprintf(stderr, "spv2dxil: %s\n", cmd.c_str());

    std::vector<char> line(cmd.begin(), cmd.end());
    line.push_back(0);
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        die("cannot run %s\\spirv-opt.exe (--freeze)", dxc_dir);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code != 0)
        die("%s: spirv-opt could not freeze the specialization constants (exit %lu)", base, (unsigned long)code);

    std::vector<char> result = read_file(frozen.c_str());
    DeleteFileA(frozen.c_str());
    return result;
}

int main(int argc, char **argv)
{
    const char *dxc_dir = nullptr, *sm = "6_6", *hlsl_out = nullptr, *in = nullptr, *out = nullptr;
    std::vector<spec_value_t> specs;
    bool raygen = false, ser = false, freeze = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dxc") && i + 1 < argc) dxc_dir = argv[++i];
        else if (!strcmp(argv[i], "--freeze")) freeze = true;
        else if (!strcmp(argv[i], "--raygen")) raygen = true;
        else if (!strcmp(argv[i], "--ser")) raygen = ser = true;
        else if (!strcmp(argv[i], "--sm") && i + 1 < argc) sm = argv[++i];
        else if (!strcmp(argv[i], "--hlsl") && i + 1 < argc) hlsl_out = argv[++i];
        else if (!strcmp(argv[i], "--spec") && i + 1 < argc) {
            spec_value_t s;
            char tail;
            if (sscanf(argv[++i], "%u=%u%c", &s.id, &s.value, &tail) != 2)
                die("--spec wants <constant_id>=<value>, got %s", argv[i]);
            specs.push_back(s);
        }
        else if (!strcmp(argv[i], "-v")) g_verbose = true;
        else if (!in) in = argv[i];
        else if (!out) out = argv[i];
        else die("unexpected argument %s", argv[i]);
    }
    if (!dxc_dir || !in || !out)
        die("usage: spv2dxil --dxc <dir> [--sm 6_6] [--hlsl out.hlsl] [--spec id=value]... [--freeze] "
            "[--raygen | --ser] [-v] in.spv out.dxil");
    if (freeze && specs.empty())
        die("--freeze wants --spec values to freeze");
    unsigned sm_num = 0;
    if (strlen(sm) != 3 || sm[1] != '_' || (sm_num = (sm[0] - '0') * 10 + (sm[2] - '0')) < 60 || sm_num > 69)
        die("--sm wants 6_0 .. 6_9, got %s", sm);

    const char *base = strrchr(in, '\\');
    const char *base2 = strrchr(in, '/');
    if (base2 > base) base = base2;
    base = base ? base + 1 : in;

    if (ser && sm_num < 69)
        die("--ser needs --sm 6_9 (MaybeReorderThread)");
    if (raygen && sm_num < 63)
        die("--raygen needs --sm 6_3 or later (DXIL libraries)");

    std::vector<char> spv = read_file(in);
    if (freeze) {
        // the values are in the SPIR-V now: none for DXC, and none for the check below
        spv = freeze_specs(spv, in, out, dxc_dir, specs, base);
        specs.clear();
    }
    const char *stage = nullptr;
    // the ray generation form keeps the HLSL of the compute one: SPIRV-Cross writes for
    // 6.6 either way, only DXC's target moves up
    std::string hlsl = to_hlsl(spv, raygen ? std::min(sm_num, 66u) : sm_num, &stage);
    if (raygen) {
        if (strcmp(stage, "cs"))
            die("%s: --raygen wants a compute shader", base);
        hlsl = to_raygen(hlsl, base, ser);
        stage = "lib";
    }
    if (hlsl_out)
        write_file(hlsl_out, hlsl.data(), hlsl.size());

    // a constant the shader does not have would leave the variant identical to
    // the default without a word - the caller named the wrong id
    for (const spec_value_t &s : specs) {
        char macro[64];
        snprintf(macro, sizeof(macro), "#ifndef SPIRV_CROSS_CONSTANT_ID_%u\n", s.id);
        if (hlsl.find(macro) == std::string::npos)
            die("%s: --spec %u=%u, but the shader has no specialization constant %u", base, s.id, s.value, s.id);
    }

    std::string hlsl_name = std::string(base) + ".hlsl";
    std::vector<char> dxil = to_dxil(hlsl, dxc_dir, stage, sm, hlsl_name.c_str(), specs);
    write_file(out, dxil.data(), dxil.size());
    return 0;
}
