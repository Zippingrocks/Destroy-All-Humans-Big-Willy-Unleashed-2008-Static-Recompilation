@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
cd /d R:\
set "MG=R:/github repos/ModernGekko-Template/lib/ModernGekko"
cmake -S "%MG%/vendor/dolphin/DolRecomp" -B workstation/build/pinned-recompiler -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
if errorlevel 1 exit /b 1
cmake --build workstation/build/pinned-recompiler --target dolrecomp --parallel 2
if errorlevel 1 exit /b 1
workstation\build\pinned-recompiler\dolrecomp.exe --cpu broadway --map workstation/symbols/main.map workstation/input/main.dol RDHE78 "workstation/source code tree/runtime-generated"
if errorlevel 1 exit /b 1
copy /Y "workstation\source code tree\runtime-generated\RDHE78_generated\RDHE78.h" "workstation\source code tree\runtime-generated\RDHE78_generated\generated.h" >nul
copy /Y "workstation\source code tree\runtime-generated\RDHE78_generated\RDHE78_smc.txt" "workstation\source code tree\runtime-generated\RDHE78_generated\generated_smc.txt" >nul
copy /Y workstation\input\main.dol "workstation\source code tree\runtime-generated\RDHE78_generated\main.dol" >nul
cmake -S "%MG%/vendor/dolphin/module-template" -B workstation/build/game-module -G Ninja -DCMAKE_BUILD_TYPE=Release -DGAME_ID=RDHE78 -DGENERATED_DIR="R:/workstation/source code tree/runtime-generated/RDHE78_generated" -DGXRUNTIME_DIR="%MG%/vendor/dolphin/GXRuntime" -DCHASSIS_ABI_DIR="%MG%/vendor/dolphin/Source/Core/Core/PowerPC/StaticRecomp"
if errorlevel 1 exit /b 1
cmake -B workstation/build/game-module -DRECOMPCORE_UNOPTIMIZED_CHUNKS=chunk_0248_text1_803EBF60.c
if errorlevel 1 exit /b 1
cmake --build workstation/build/game-module --parallel 8
exit /b %errorlevel%
