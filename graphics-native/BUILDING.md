# Graphics runtime source

host.cpp, bootstrap.cpp and graphics_coordinator.cpp build the add-on, bootstrap and early attach helper respectively with MSVC x64 C++17 /O2 /MT. Link kernel32, user32, psapi and bcrypt; build host/bootstrap as DLLs. Headers under native/ and this folder are required. The provider is a modified ReShade 6.8.0 build; its TextureQuest creation/mutation interfaces are included here. Runtime binary identities are in ../graphics-runtime/RUNTIME.json. ReShade license is bundled there.

The controller is built from ../src/texturequest_graphics_control.py using PyInstaller onefile with ../src on its import path. ReShade source/build provenance is retained in the release development receipts; do not substitute a stock ReShade DLL because the provider interface differs. The portable app builder consumes --graphics-runtime-dir and --loader-runtime-dir and verifies their complete rosters.
