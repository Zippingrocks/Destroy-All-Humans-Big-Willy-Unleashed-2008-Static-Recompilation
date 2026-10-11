@echo off
setlocal
 dotnet publish "%~dp0tools\gui-launcher\BigWillyGuiLauncher.csproj" -c Release -r win-x64 --no-self-contained -o "%~dp0build\flagship-launcher"
exit /b %errorlevel%
