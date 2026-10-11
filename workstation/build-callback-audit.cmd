@echo off
setlocal
cd /d "%~dp0.."
powershell -NoProfile -Command "$expected='W:\: => '+(Get-Location).Path; $mapping=subst; if ($mapping -notcontains $expected) { throw 'W: must map to this Big Willy workspace. Do not replace a drive used by another project.' }"
if errorlevel 1 exit /b 1
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
cd /d W:\
set "SRC=github repos\ModernGekko-Template\lib\ModernGekko"
cl /nologo /EHsc /std:c++20 /I"%SRC%\vendor\dolphin\Source\Core" /I"%SRC%\vendor\dolphin\GXRuntime\include" /Foworkstation\logs\callback-audit-20261011\guest-call-test.obj /Feworkstation\logs\callback-audit-20261011\guest-call-test.exe workstation\tools\test_guest_call_context.cpp
if errorlevel 1 exit /b 1
workstation\logs\callback-audit-20261011\guest-call-test.exe
if errorlevel 1 exit /b 1
cmake -S "%SRC%" -B workstation/build/runtime-callback-audit -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DMODERNGEKKO_FRONTEND_NAME="Destroy All Humans! Big Willy Unleashed" -DMODERNGEKKO_LAUNCHER_OUTPUT_NAME=BigWillyUnleashed -DMODERNGEKKO_RUNNER_OUTPUT_NAME=dahbwu -DMODERNGEKKO_REQUIRED_DISC_ID=RDHE78 -DMODERNGEKKO_DEFAULT_WINDOW_TITLE="Destroy All Humans! Big Willy Unleashed" -DMODERNGEKKO_PORTABLE_DEFAULT_GAME=ON
if errorlevel 1 exit /b 1
cmake --build workstation/build/runtime-callback-audit --target moderngekko-run --parallel 8
exit /b %errorlevel%
