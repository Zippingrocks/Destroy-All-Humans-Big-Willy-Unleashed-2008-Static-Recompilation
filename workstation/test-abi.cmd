@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
cd /d R:\workstation\build\abi-check
cl /nologo /TP /EHsc /std:c++latest /I"R:/github repos/ModernGekko-Template/lib/ModernGekko/include" frontend.c /Fe:frontend.exe
if errorlevel 1 exit /b 1
cl /nologo /TC /std:c11 /I"R:/github repos/ModernGekko-Template/lib/ModernGekko/vendor/dolphin/GXRuntime/include" backend.c /Fe:backend.exe
if errorlevel 1 exit /b 1
frontend.exe > frontend.txt
backend.exe > backend.txt
fc /b frontend.txt backend.txt
exit /b %errorlevel%
