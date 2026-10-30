# Active graphics runtime

App 1.7.0 ships three native modules: the modified provider (`d3d11.dll`), `TextureQuestGraphicsHost.addon64`, and `TextureQuestGraphicsBootstrap.dll`. It does not build or launch the historical graphics coordinator/injector. Historical sources remain for old-version review.

Build the [ReShade provider overlay](../graphics-provider/README.md) first. In an x64 MSVC developer command prompt, with its headers available at `reshade-tq/include`, run from the repository root (use a fresh output directory):

```bat
mkdir native-build
cl /nologo /std:c++17 /O2 /MT /EHsc /W4 /Igraphics-native /Ireshade-tq\include /Fonative-build\host.obj /Fenative-build\TextureQuestGraphicsHost.addon64 /LD graphics-native\host.cpp /link /INCREMENTAL:NO /MACHINE:X64 kernel32.lib user32.lib psapi.lib bcrypt.lib shell32.lib
cl /nologo /std:c++17 /O2 /MT /EHsc /W4 /Igraphics-native /Ireshade-tq\include /Fonative-build\bootstrap.obj /Fenative-build\TextureQuestGraphicsBootstrap.dll /LD graphics-native\bootstrap_proxy.cpp /link /INCREMENTAL:NO /MACHINE:X64 kernel32.lib user32.lib psapi.lib bcrypt.lib shell32.lib
```

The Python builder consumes `--graphics-runtime-dir graphics-runtime` and `--loader-runtime-dir sideload-runtime`, verifies both complete rosters and packages them. `RUNTIME.json` identifies active native files and capabilities. Rebuilt modules need a fresh coherent descriptor with their actual sizes/hashes. No byte-identical build guarantee is made. Offline archive preparation still uses bundled Python helper/catalog resources, not a running archive-redirection hook.
