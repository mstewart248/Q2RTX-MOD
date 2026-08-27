SET(MEDIA_SOURCES
    ${SOURCE}/baseq2/env
    ${SOURCE}/baseq2/maps
    ${SOURCE}/baseq2/models
    ${SOURCE}/baseq2/overrides
    ${SOURCE}/baseq2/pics
    ${SOURCE}/baseq2/sound
    ${SOURCE}/baseq2/sprites
    ${SOURCE}/baseq2/textures
    ${SOURCE}/baseq2/materials
    ${SOURCE}/baseq2/prefetch.txt
    ${SOURCE}/baseq2/pt_toggles.cfg
    ${SOURCE}/baseq2/q2rtx.cfg
    ${SOURCE}/baseq2/q2rtx.menu
)
set(out_file "${SOURCE}/baseq2/q2rtx_media.pkz")
exec_program(7za ARGS "a -tzip" ${out_file} ${MEDIA_SOURCES})

SET(MEDIA_SOURCES_ROGUE
    ${SOURCE}/rogue/maps
)
set(out_file_rogue "${SOURCE}/rogue/q2rtx_media.pkz")
exec_program(7za ARGS "a -tzip" ${out_file_rogue} ${MEDIA_SOURCES_ROGUE})

# The rerelease game dir. Only content authored or repacked in THIS repo goes in
# here - never anything extracted from the retail rerelease paks. `overrides` is
# the Q2RTX texture set, which the rerelease maps need to look like Q2RTX rather
# than like 1997; `models` is the MD5 PBR sidecar maps; `maps` the per-map
# material .cfg files.
SET(MEDIA_SOURCES_RERELEASE
    ${SOURCE}/rerelease/overrides
    ${SOURCE}/rerelease/models
    ${SOURCE}/rerelease/maps
    ${SOURCE}/rerelease/materials
)
set(out_file_rerelease "${SOURCE}/rerelease/q2rtx_media.pkz")
exec_program(7za ARGS "a -tzip" ${out_file_rerelease} ${MEDIA_SOURCES_RERELEASE})
