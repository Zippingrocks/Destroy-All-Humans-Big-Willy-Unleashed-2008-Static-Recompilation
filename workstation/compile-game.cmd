@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
cd /d R:\
cmake --build workstation/build/game-module --parallel 8
exit /b %errorlevel%
