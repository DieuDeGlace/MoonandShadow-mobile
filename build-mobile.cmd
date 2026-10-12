@echo off
setlocal
cd /d "%~dp0"
if not exist build-mobile mkdir build-mobile
clang-cl /nologo /O2 /MD /std:c++17 /LD /Ivendor\cwmods MoonandShadow.cpp /Fobuild-mobile\MoonandShadow.obj /link d3d11.lib dxgi.lib d3dcompiler.lib user32.lib winmm.lib /OUT:build-mobile\MoonandShadow.dll
if errorlevel 1 exit /b 1
clang-cl /nologo /O2 /MD /std:c++17 /UNDEBUG /Ivendor\cwmods MoonLight\RulesTest.cpp /Fobuild-mobile\MoonRulesTest.obj /Febuild-mobile\MoonRulesTest.exe
if errorlevel 1 exit /b 1
build-mobile\MoonRulesTest.exe
if errorlevel 1 exit /b 1
if /i not "%~1"=="--gpu" exit /b 0
clang-cl /nologo /O2 /MD /std:c++17 /UNDEBUG  /Ivendor\cwmods MoonLight\GraphicsTest.cpp /Fobuild-mobile\MoonGraphicsTest.obj /Febuild-mobile\MoonGraphicsTest.exe /link d3d11.lib dxgi.lib d3dcompiler.lib user32.lib winmm.lib
if errorlevel 1 exit /b 1
build-mobile\MoonGraphicsTest.exe
if errorlevel 1 exit /b 1
clang-cl /nologo /O2 /MD /std:c++17 /UNDEBUG  /Ivendor\cwmods SteamShadows\SmokeTest.cpp /Fobuild-mobile\ShadowResourcesTest.obj /Febuild-mobile\ShadowResourcesTest.exe /link d3d11.lib dxgi.lib d3dcompiler.lib user32.lib winmm.lib
if errorlevel 1 exit /b 1
build-mobile\ShadowResourcesTest.exe
if errorlevel 1 exit /b 1
clang-cl /nologo /O2 /MD /std:c++17 /UNDEBUG  /Ivendor\cwmods SteamShadows\FilteringTest.cpp /Fobuild-mobile\FilteringTest.obj /Febuild-mobile\FilteringTest.exe /link d3d11.lib dxgi.lib d3dcompiler.lib user32.lib winmm.lib
if errorlevel 1 exit /b 1
build-mobile\FilteringTest.exe
if errorlevel 1 exit /b 1
clang-cl /nologo /O2 /MD /std:c++17 /UNDEBUG  /Ivendor\cwmods SteamShadows\CloudShadowTest.cpp /Fobuild-mobile\CloudShadowsTest.obj /Febuild-mobile\CloudShadowsTest.exe /link d3d11.lib dxgi.lib d3dcompiler.lib user32.lib winmm.lib
if errorlevel 1 exit /b 1
build-mobile\CloudShadowsTest.exe
if errorlevel 1 exit /b 1
clang-cl /nologo /O2 /MD /std:c++17 /UNDEBUG /DTEST_CASCADES /Ivendor\cwmods SteamShadows\FilteringTest.cpp /Fobuild-mobile\CascadeFilteringTest.obj /Febuild-mobile\CascadeFilteringTest.exe /link d3d11.lib dxgi.lib d3dcompiler.lib user32.lib winmm.lib
if errorlevel 1 exit /b 1
build-mobile\CascadeFilteringTest.exe
if errorlevel 1 exit /b 1
clang-cl /nologo /O2 /MD /std:c++17 /UNDEBUG /DTEST_MODEL_RECEIVER /Ivendor\cwmods SteamShadows\FilteringTest.cpp /Fobuild-mobile\ModelLightingTest.obj /Febuild-mobile\ModelLightingTest.exe /link d3d11.lib dxgi.lib d3dcompiler.lib user32.lib winmm.lib
if errorlevel 1 exit /b 1
build-mobile\ModelLightingTest.exe
if errorlevel 1 exit /b 1
clang-cl /nologo /O2 /MD /std:c++17 /UNDEBUG  /Ivendor\cwmods SteamShadows\ModelPlaneTest.cpp /Fobuild-mobile\ModelPlaneTest.obj /Febuild-mobile\ModelPlaneTest.exe /link d3d11.lib dxgi.lib d3dcompiler.lib user32.lib winmm.lib
if errorlevel 1 exit /b 1
build-mobile\ModelPlaneTest.exe
if errorlevel 1 exit /b 1
clang-cl /nologo /O2 /MD /std:c++17 /UNDEBUG  /Ivendor\cwmods SteamShadows\NativeClipTest.cpp /Fobuild-mobile\NativeClipTest.obj /Febuild-mobile\NativeClipTest.exe /link d3d11.lib dxgi.lib d3dcompiler.lib user32.lib winmm.lib
if errorlevel 1 exit /b 1
build-mobile\NativeClipTest.exe %2
if errorlevel 78 exit /b 1
if errorlevel 77 exit /b 0
if errorlevel 1 exit /b 1
exit /b 0
