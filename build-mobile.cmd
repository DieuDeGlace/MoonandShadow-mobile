@echo off
setlocal
cd /d "%~dp0"
if not exist build-mobile mkdir build-mobile
clang-cl /nologo /O2 /MD /std:c++17 /LD /Ivendor\cwmods MoonandShadow.cpp /Fobuild-mobile\MoonandShadow.obj /link d3d11.lib dxgi.lib d3dcompiler.lib user32.lib winmm.lib /OUT:build-mobile\MoonandShadow.dll
if errorlevel 1 exit /b 1
clang-cl /nologo /O2 /MD /std:c++17 /UNDEBUG /Ivendor\cwmods MoonLight\RulesTest.cpp /Fobuild-mobile\MoonRulesTest.obj /Febuild-mobile\MoonRulesTest.exe
if errorlevel 1 exit /b 1
build-mobile\MoonRulesTest.exe
