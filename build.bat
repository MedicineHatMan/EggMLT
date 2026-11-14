@echo off
setlocal
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (
	echo emalt: Visual Studio C++ tools were not found
	exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if not exist "%~dp0build" mkdir "%~dp0build"
cl /nologo /std:c++17 /EHsc /O2 /W3 /MP ^
  /I "%~dp0src" /I "%~dp0third_party\imgui" /I "%~dp0third_party\imgui\backends" ^
  /Fo:"%~dp0build\\" /Fe:"%~dp0emalt.exe" ^
  "%~dp0src\main.cpp" "%~dp0src\emalt.cpp" "%~dp0src\gui.cpp" "%~dp0src\audio.cpp" ^
  "%~dp0third_party\imgui\imgui.cpp" ^
  "%~dp0third_party\imgui\imgui_draw.cpp" ^
  "%~dp0third_party\imgui\imgui_tables.cpp" ^
  "%~dp0third_party\imgui\imgui_widgets.cpp" ^
  "%~dp0third_party\imgui\backends\imgui_impl_win32.cpp" ^
  "%~dp0third_party\imgui\backends\imgui_impl_dx11.cpp" ^
  /link /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup
if errorlevel 1 exit /b 1
echo built %~dp0emalt.exe
exit /b 0
