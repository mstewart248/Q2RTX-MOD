# Repack baseq2/shaders.pkz from the compiled bytecode in baseq2/shader_vkpt.
#
# DELETE FIRST, THEN CREATE. The previous version of this script ran
#
#     exec_program(7za ARGS "a -tzip" ${out_file} ${SHADER_SOURCES})
#
# and "7za a" ADDS to an existing archive rather than replacing it, so a shader
# that was renamed or removed in the source tree stayed in the pkz forever. That
# is how the pkz in this tree came to be carrying the pre-1.7 ray tracing names
# (*.rgen.khr.spv, *.rgen.nv.spv) that nothing loads any more.
#
# It matters because the engine resolves "shader_vkpt/<name>.spv" out of either
# the loose directory or this archive, so stale entries do not sit harmlessly
# beside a fresh build - they can shadow it.
#
# Normal builds now repack this automatically (see the POST_BUILD command on the
# `shaders` target in src/CMakeLists.txt); this script remains for the install
# and packaging paths, and is kept consistent with it.
#
# cmake -E rather than 7za so no external tool is required and the behaviour is
# identical on every platform. "remove -f" rather than "rm -f" because
# cmake_minimum_required for this project is 3.9 and "rm" only arrived in 3.17.

set(out_file "${SOURCE}/baseq2/shaders.pkz")

if(NOT EXISTS "${SOURCE}/baseq2/shader_vkpt")
    message(FATAL_ERROR "package_shaders: ${SOURCE}/baseq2/shader_vkpt does not exist - build the `shaders` target first")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E remove -f "${out_file}"
    RESULT_VARIABLE rm_result)

# Archived from inside baseq2 with a relative path, so the entries are stored as
# "shader_vkpt/<name>.spv" - exactly the path the engine asks the filesystem for.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar "cf" "${out_file}" --format=zip -- "shader_vkpt"
    WORKING_DIRECTORY "${SOURCE}/baseq2"
    RESULT_VARIABLE tar_result)

if(NOT tar_result EQUAL 0)
    message(FATAL_ERROR "package_shaders: failed to create ${out_file} (${tar_result})")
endif()

message(STATUS "Packed ${out_file}")
