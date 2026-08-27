@echo off
SET SEVENZIP="C:\Program Files\7-Zip\7z.exe"
SET DEST=q2rtx_media.pkz
PUSHD ..\baseq2
SET SOURCES=env maps models overrides pics sound sprites textures materials prefetch.txt pt_toggles.cfg q2rtx.cfg q2rtx.menu
IF EXIST %DEST% DEL %DEST%
%SEVENZIP% a -tzip %DEST% %SOURCES%
POPD

PUSHD ..\rogue
SET SOURCES_ROGUE=maps
IF EXIST %DEST% DEL %DEST%
%SEVENZIP% a -tzip %DEST% %SOURCES_ROGUE%
POPD

REM The rerelease game dir. Only content authored or repacked in THIS repo -
REM never anything extracted from the retail rerelease paks. overrides is the
REM Q2RTX texture set the rerelease maps need, models the MD5 PBR sidecar maps.
PUSHD ..\rerelease
SET SOURCES_RERELEASE=overrides models maps materials
IF EXIST %DEST% DEL %DEST%
%SEVENZIP% a -tzip %DEST% %SOURCES_RERELEASE%
POPD
