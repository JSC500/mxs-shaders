@echo off
setlocal
cd /d "%~dp0"

if defined VSCMD_VER goto :have_vs
set "VSDIR="
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
	echo error: Visual Studio with the C++ build tools was not found
	exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:have_vs

if not exist build\gen mkdir build\gen
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\gen_exports.ps1 -OutDir build\gen -Hooks src\static_hooks.txt || exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\gen_pack.ps1 -Pack shaderpacks\default -Out build\gen\default_pack.c || exit /b 1

ml64 /nologo /c /Fobuild\gen\stubs.obj build\gen\stubs.asm || exit /b 1
cl /nologo /O2 /MT /W3 /D_CRT_SECURE_NO_WARNINGS /Fobuild\gen\ /Isrc src\proxy.c src\hooks.c src\log.c src\pack.c src\post.c src\ui.c build\gen\real_table.c build\gen\default_pack.c build\gen\stubs.obj ^
	/LD /Fe:build\opengl32.dll /link /DEF:build\gen\exports.def /NOLOGO user32.lib gdi32.lib || exit /b 1

echo Built build\opengl32.dll

rem Deploy to the dev copy of the game and the pack folder when they exist.
set "DATA=%LOCALAPPDATA%\MX Simulator\mxs-shaders"
if exist "%DATA%\game" copy /y build\opengl32.dll "%DATA%\game\" >nul
xcopy /y /s /i /q shaderpacks "%DATA%\shaderpacks" >nul
