@echo off
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvars64.bat`) do set "VCVARS=%%i"
if not defined VCVARS (echo vcvars64.bat not found & exit /b 1)
call "%VCVARS%" >nul || exit /b 1
cd /d "%~dp0"
if not exist out\obj mkdir out\obj
cl /nologo /std:c++20 /O2 /MT /EHsc /W4 /permissive- /DNDEBUG /DNOMINMAX /Ideps\reshade\include /Ideps\imgui addon.cpp /Foout\obj\ /LD /Feout\EQ2Graphics.addon64 /link || exit /b 1
echo Build succeeded.
