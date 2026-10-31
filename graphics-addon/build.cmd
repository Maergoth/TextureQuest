@echo off
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvars64.bat`) do set "VCVARS=%%i"
if not defined VCVARS (echo vcvars64.bat not found & exit /b 1)
call "%VCVARS%" >nul || exit /b 1
cd /d "%~dp0"
if not exist out\obj mkdir out\obj
set INCLUDES=/Ideps\reshade\include /Ideps\xxhash /Ideps\libdeflate
set COMMON=/nologo /Brepro /std:c++20 /O2 /MT /EHsc /W4 /permissive- /DNDEBUG /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 %INCLUDES%
set DEFLATE=deps\libdeflate\lib\deflate_decompress.c deps\libdeflate\lib\zlib_decompress.c deps\libdeflate\lib\adler32.c deps\libdeflate\lib\utils.c deps\libdeflate\lib\x86\cpu_features.c
cl %COMMON% addon.cpp %DEFLATE% /Foout\obj\ /LD /Feout\TextureQuest.addon64 /link /Brepro user32.lib || exit /b 1
cl %COMMON% selfcheck.cpp %DEFLATE% /Foout\obj\ /Feout\selfcheck.exe /link /Brepro user32.lib || exit /b 1
echo Build succeeded.
