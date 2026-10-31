# Historical graphics runtime

These sources built the app 1.7.0 Graphics hook modules `TextureQuestGraphicsHost.addon64` and `TextureQuestGraphicsBootstrap.dll`. They remain for old-version review and are not packaged. The current Graphics hook is the [TextureQuest add-on](../graphics-addon/BUILDING.md).

These sources also need the provider overlay headers from the `v1.7.0` tag (`graphics-provider/overlay/include`). In an x64 MSVC developer command prompt, with ReShade headers available at `reshade-tq/include`, run from the repository root (use a fresh output directory):

```bat
mkdir native-build
cl /nologo /std:c++17 /O2 /MT /EHsc /W4 /Igraphics-native /Ireshade-tq\include /Fonative-build\host.obj /Fenative-build\TextureQuestGraphicsHost.addon64 /LD graphics-native\host.cpp /link /INCREMENTAL:NO /MACHINE:X64 kernel32.lib user32.lib psapi.lib bcrypt.lib shell32.lib
cl /nologo /std:c++17 /O2 /MT /EHsc /W4 /Igraphics-native /Ireshade-tq\include /Fonative-build\bootstrap.obj /Fenative-build\TextureQuestGraphicsBootstrap.dll /LD graphics-native\bootstrap_proxy.cpp /link /INCREMENTAL:NO /MACHINE:X64 kernel32.lib user32.lib psapi.lib bcrypt.lib shell32.lib
```

No byte-identical build guarantee is made.
