# Background loader native source

These C++ and header files are the byte-exact source for the reviewed portable background loader included in TextureQuest 1.4.1. The watcher executable detects future launches of the selected game executable; the DLL redirects selected file reads to prepared private archives. Building does not install the loader, register startup, launch a game or generate textures.

The ready-to-package runtime is already in `../sideload-runtime/`: its descriptor pins the two native binaries, preparation helper, released-member catalog and Microsoft Detours license. Use that directory for the app build:

```powershell
python build_installer.py --manifest release-manifest-1.3.7.json --version 1.4.1 --loader-runtime-dir sideload-runtime
```

Run the command from the repository root with the pinned Python build dependencies installed. This is the supported way to package the included reviewed payload. Native compilation is separate and is not needed to build the app with that payload.

## Native rebuild

Use a current MSVC x64 C++ toolchain, the Windows 10/11 SDK, and an **x64 Native Tools Command Prompt**. The source uses C++17. Obtain Microsoft Detours from its official repository at the exact source commit [1dbd4ea281d73d478aa8d09a8b83fbd265f715f7](https://github.com/microsoft/Detours/tree/1dbd4ea281d73d478aa8d09a8b83fbd265f715f7). Its MIT license is included at [LICENSE-Detours.md](../sideload-runtime/sideload/LICENSE-Detours.md). No Detours source, compiled library or historical local build paths are vendored in this native-source directory.

Choose separate paths for this source checkout, the Detours `src` directory and a fresh empty output directory on D:. Adjust these example variables, create the empty output directory, then run:

```bat
set "TQ_NATIVE=D:/TextureQuest/sideload-native"
set "TQ_DETOURS=D:/Detours/src"
set "TQ_NATIVE_OUT=D:/TextureQuest-native-build"
cd /d "%TQ_NATIVE_OUT%"
for %S in (detours modules disasm image creatwth disolx86 disolx64 disolia64 disolarm disolarm64) do cl /nologo /W4 /EHsc /O2 /MT /Gy /DDETOUR_DEBUG=0 /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /DNTDDI_VERSION=0x0A000002 /c "%TQ_DETOURS%/%S.cpp" /Fo:%S.obj
lib /nologo /out:detours.lib detours.obj modules.obj disasm.obj image.obj creatwth.obj disolx86.obj disolx64.obj disolia64.obj disolarm.obj disolarm64.obj
cl /nologo /std:c++17 /W4 /EHsc /O2 /MT /Gy /guard:cf /D_WIN32_WINNT=0x0A00 /DNTDDI_VERSION=0x0A000002 /I"%TQ_DETOURS%" /LD "%TQ_NATIVE%/background_dll.cpp" detours.lib Psapi.lib /Fo:bg.obj /Fe:TextureQuestBackground.dll /link /INCREMENTAL:NO /OPT:REF /DYNAMICBASE /NXCOMPAT /guard:cf /IMPLIB:bg.lib /MAP:bg.map
cl /nologo /std:c++17 /W4 /EHsc /O2 /MT /Gy /guard:cf /D_WIN32_WINNT=0x0A00 /DNTDDI_VERSION=0x0A000002 "%TQ_NATIVE%/watcher.cpp" /Fo:watch.obj /Fe:TextureQuestWatch.exe /link /SUBSYSTEM:WINDOWS /INCREMENTAL:NO /OPT:REF /DYNAMICBASE /NXCOMPAT /guard:cf /MAP:watch.map
```

Check each command's exit status before continuing; stop if any compilation or link fails. `%S` is the interactive Command Prompt syntax; a batch file uses `%%S`. This documentation does not include a test runner or case suite. Rebuilt binaries can differ with the compiler, SDK and build environment and have not automatically inherited the included payload's reviewed identity or runtime evidence.

Do not overwrite the bundled runtime or edit its descriptor just to bypass a hash mismatch. A separately rebuilt runtime needs its own validation and coherent descriptor, including updated file sizes, SHA256 values and payload identity. Preserve the helper, catalog and Detours license and pass that separately validated runtime to `--loader-runtime-dir` explicitly. The included descriptor continues to identify only the included exact payload.

## Current scope

The first in-world proof is one Windows Origins client. Other client builds, every possible launch path and automatic archive/index refresh after game patches remain unproven or unfinished. Background mode does not modify game archives, settings or shortcuts. Uninstall stops future attachments; an attached client retains its loader until it closes. See the main README for installation, update and private-cache refresh requirements.
