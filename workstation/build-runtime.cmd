@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
cd /d R:\
cmake -S "github repos/ModernGekko-Template/lib/ModernGekko" -B workstation/build/runtime -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF -DMODERNGEKKO_FRONTEND_NAME="Destroy All Humans! Big Willy Unleashed" -DMODERNGEKKO_LAUNCHER_OUTPUT_NAME=BigWillyUnleashed -DMODERNGEKKO_RUNNER_OUTPUT_NAME=dahbwu -DMODERNGEKKO_REQUIRED_DISC_ID=RDHE78 -DMODERNGEKKO_DEFAULT_WINDOW_TITLE="Destroy All Humans! Big Willy Unleashed" -DMODERNGEKKO_PORTABLE_DEFAULT_GAME=ON
if errorlevel 1 exit /b 1
cmake --build workstation/build/runtime --target moderngekko-port moderngekko-launcher --parallel 6
exit /b %errorlevel%
