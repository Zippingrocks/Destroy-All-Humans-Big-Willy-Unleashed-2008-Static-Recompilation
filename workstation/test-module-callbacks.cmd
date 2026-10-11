@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
cd /d "%~dp0.."
set "SRC=github repos\ModernGekko-Template\lib\ModernGekko"
cl /nologo /EHsc /std:c++20 /I"%SRC%\vendor\dolphin\Source\Core" /I"%SRC%\vendor\dolphin\GXRuntime\include" /Foworkstation\logs\callback-audit-20261011\module-test.obj /Feworkstation\logs\callback-audit-20261011\module-test.exe workstation\tools\test_module_callbacks.cpp
if errorlevel 1 exit /b 1
workstation\logs\callback-audit-20261011\module-test.exe "%CD%\dist\flagship\gRDHE78_recomp.dll" workstation\logs\callback-audit-20261011\indirect-sites.txt %1
exit /b %errorlevel%
