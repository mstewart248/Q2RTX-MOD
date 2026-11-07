# D3D12 SHADERS: THE VULKAN SHADERS, TRANSLATED.
#
# The D3D12 renderer (src/refresh/d3d12, vid_rtx 3) has one shader source of its
# own (d3d12/shader/postfx_copy.comp, listed in src/CMakeLists.txt). Every shader
# compileShaders.cmake builds is compiled a second time with
# -DVKPT_D3D12 (the few spots where HLSL needs something else are #ifdef'd in the
# GLSL) and spv2dxil turns that SPIR-V into signed DXIL:
#
#     src/refresh/vkpt/shader/foo.comp
#         -> build/shader_d3d12_spv/foo.comp.spv      glslangValidator -DVKPT_D3D12
#         -> baseq2/shader_d3d12/foo.comp.dxil        spv2dxil (SPIRV-Cross + DXC)
#
# Ray tracing pipeline stages (.rchit, .rmiss, .rahit, .rint) are not built:
# SPIRV-Cross cannot write them as HLSL, and the D3D12 renderer traces with ray
# queries only. The .rgen files build as their ray query compute variant, the
# same ".query" one Vulkan has.
#
# Include after compileShaders.cmake: this uses its GLSLANG_COMPILER,
# GLSLANG_ARGS, SHADER_SOURCE_DEPENDENCIES and COMPILE_ARGS_DEP.

set(D3D12_SPV_DIR "${CMAKE_BINARY_DIR}/shader_d3d12_spv")
set(D3D12_DXIL_DIR "${CMAKE_SOURCE_DIR}/baseq2/shader_d3d12")
set(D3D12_SHADER_MODEL "6_6" CACHE STRING
    "Shader model the D3D12 shaders are compiled for (6_6: ray query, dynamic resources; 6_9: SER)")

function(compile_shader_d3d12)
    set(options "")
    set(oneValueArgs SOURCE_FILE OUTPUT_FILE_NAME OUTPUT_FILE_LIST STAGE)
    set(multiValueArgs DEFINES INCLUDES)
    cmake_parse_arguments(params "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if (NOT params_SOURCE_FILE OR NOT params_OUTPUT_FILE_LIST)
        message(FATAL_ERROR "compile_shader_d3d12: SOURCE_FILE and OUTPUT_FILE_LIST are required")
    endif()

    set(src_file "${CMAKE_CURRENT_SOURCE_DIR}/${params_SOURCE_FILE}")
    if (params_OUTPUT_FILE_NAME)
        set(output_file_name ${params_OUTPUT_FILE_NAME})
    else()
        get_filename_component(output_file_name ${src_file} NAME)
    endif()

    if (params_STAGE)
        set(stage -S ${params_STAGE})
    else()
        set(stage)
    endif()

    set(spv_file "${D3D12_SPV_DIR}/${output_file_name}.spv")
    set(dxil_file "${D3D12_DXIL_DIR}/${output_file_name}.dxil")

    # Not MAIN_DEPENDENCY: compile_shader already made the source the main
    # dependency of its Vulkan rule, and a file can only be that once.
    add_custom_command(OUTPUT ${dxil_file}
                       BYPRODUCTS ${spv_file}
                       DEPENDS ${src_file} ${SHADER_SOURCE_DEPENDENCIES} ${COMPILE_ARGS_DEP} spv2dxil
                       COMMAND ${CMAKE_COMMAND} -E make_directory ${D3D12_SPV_DIR}
                       COMMAND ${CMAKE_COMMAND} -E make_directory ${D3D12_DXIL_DIR}
                       COMMAND ${GLSLANG_COMPILER} ${stage} --target-env vulkan1.2 --quiet
                               -DVKPT_SHADER -DVKPT_D3D12 -V ${GLSLANG_ARGS} ${params_DEFINES} ${params_INCLUDES}
                               "${src_file}" -o "${spv_file}"
                       COMMAND $<TARGET_FILE:spv2dxil> --dxc "${D3D12_VULKAN_SDK}/Bin" --sm ${D3D12_SHADER_MODEL}
                               "${spv_file}" "${dxil_file}"
                       COMMENT "D3D12 shader ${output_file_name}"
                       VERBATIM)

    set(${params_OUTPUT_FILE_LIST} ${${params_OUTPUT_FILE_LIST}} ${dxil_file} PARENT_SCOPE)
endfunction()

# A SPECIALIZATION VARIANT of a shader compile_shader_d3d12 already builds: what
# vkpt makes with VkSpecializationInfo (direct lighting with caustics, the
# second indirect bounce, A-SVGF's a-trous iterations, HDR tone mapping...).
# It reuses the base's SPIR-V and only runs spv2dxil again, with --spec. The
# output is named after the values, which the C side composes the same way:
#
#     compile_shader_d3d12_variant(BASE_NAME indirect_lighting.rgen.query SPEC 0=1 ...)
#         -> baseq2/shader_d3d12/indirect_lighting.rgen.query.spec_0_1.dxil
#
# SPEC lists constant_id=value pairs; a constant left out keeps its default.
#
# ENTRY raygen or ser builds the compute shader as a ray generation shader in a
# DXIL library (spv2dxil --raygen / --ser, shader model 6.9) for the D3D12
# renderer's SER A/B (pt_ser), named after the specialization:
#
#     compile_shader_d3d12_variant(BASE_NAME indirect_lighting.rgen.query SPEC 0=1 ENTRY ser ...)
#         -> baseq2/shader_d3d12/indirect_lighting.rgen.query.spec_0_1.ser.dxil
function(compile_shader_d3d12_variant)
    set(options FREEZE)
    set(oneValueArgs BASE_NAME OUTPUT_FILE_LIST ENTRY)
    set(multiValueArgs SPEC)
    cmake_parse_arguments(params "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if (NOT params_BASE_NAME OR NOT params_OUTPUT_FILE_LIST OR (NOT params_SPEC AND NOT params_ENTRY))
        message(FATAL_ERROR "compile_shader_d3d12_variant: BASE_NAME, OUTPUT_FILE_LIST and SPEC or ENTRY are required")
    endif()

    set(suffix "")
    set(spec_args)
    if (params_SPEC)
        set(suffix ".spec")
    endif()
    foreach(s ${params_SPEC})
        if (NOT s MATCHES "^([0-9]+)=([0-9]+)$")
            message(FATAL_ERROR "compile_shader_d3d12_variant: SPEC entry '${s}' is not id=value")
        endif()
        string(APPEND suffix "_${CMAKE_MATCH_1}_${CMAKE_MATCH_2}")
        list(APPEND spec_args --spec ${s})
    endforeach()

    # FREEZE: the values go into the SPIR-V and the code they kill goes before
    # SPIRV-Cross (spv2dxil's freeze_specs says why a variant would need it)
    if (params_FREEZE)
        list(APPEND spec_args --freeze)
    endif()

    set(shader_model ${D3D12_SHADER_MODEL})
    if (params_ENTRY)
        if (NOT params_ENTRY MATCHES "^(raygen|ser)$")
            message(FATAL_ERROR "compile_shader_d3d12_variant: ENTRY is raygen or ser, not '${params_ENTRY}'")
        endif()
        string(APPEND suffix ".${params_ENTRY}")
        list(APPEND spec_args --${params_ENTRY})
        set(shader_model 6_9)
    endif()

    set(spv_file "${D3D12_SPV_DIR}/${params_BASE_NAME}.spv")
    set(base_dxil "${D3D12_DXIL_DIR}/${params_BASE_NAME}.dxil")
    set(dxil_file "${D3D12_DXIL_DIR}/${params_BASE_NAME}${suffix}.dxil")

    # the base's rule makes the .spv (as a byproduct) - depending on the base's
    # .dxil orders this after it and reruns it whenever the source changes
    add_custom_command(OUTPUT ${dxil_file}
                       DEPENDS ${base_dxil} spv2dxil
                       COMMAND $<TARGET_FILE:spv2dxil> --dxc "${D3D12_VULKAN_SDK}/Bin" --sm ${shader_model}
                               ${spec_args} "${spv_file}" "${dxil_file}"
                       COMMENT "D3D12 shader ${params_BASE_NAME}${suffix}"
                       VERBATIM)

    set(${params_OUTPUT_FILE_LIST} ${${params_OUTPUT_FILE_LIST}} ${dxil_file} PARENT_SCOPE)
endfunction()
