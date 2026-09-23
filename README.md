# SkyNet Browser

SkyNet is a native SKSE plugin that adds a desktop-style web browser to Skyrim.
It hosts remote pages as top-level browser documents, includes a SkyNet home page
and splash video, keeps bookmarks and history in the current Windows profile,
and packages SkyNet AdBlocker (uBlock Origin Lite) alongside a native ad shield.

> **Release status:** version 1.0.1 has been tested on Steam Skyrim SE/AE
> runtime **1.6.1170**. 

## Runtime requirements

- Skyrim Special Edition / Anniversary Edition on Windows 10 or later.
- SKSE64 that exactly matches the installed Skyrim runtime.
- Address Library for SKSE Plugins that matches the runtime.
- PrismaUI with API v2 support.
- SKSE Menu Framework.
- Microsoft Edge WebView2 Evergreen Runtime, x64. The full Microsoft Edge
  browser is not required. Microsoft provides both online and offline runtime
  installers: <https://developer.microsoft.com/microsoft-edge/webview2/>.

SkyNet AdBlocker is bundled. Players do **not** need a separate uBlock
installation.

## Repository layout

```text
assets/                 Browser pages, logo, splash video, bundled extension
extern/CommonLibSSE-NG/ CommonLibSSE-NG submodule (initialize after cloning)
extern/WebView2/        WebView2 SDK headers and static loader library
include/                SkyNet headers and cpp-httplib
PrismaUI/Mods/SkyNet/   PrismaUI dependency view
src/                    C++ plugin source
```

Build directories, downloaded NuGet packages, browser profiles, and game/mod
manager files are deliberately excluded.

## Build prerequisites

1. Windows 10 or newer, x64.
2. Visual Studio 2022 with **Desktop development with C++**, MSVC v143, and a
   current Windows SDK.
3. CMake 3.21 or newer.
4. Git.
5. [vcpkg](https://github.com/microsoft/vcpkg), with the `VCPKG_ROOT`
   environment variable set. The included `vcpkg.json` supplies OpenSSL and
   the CommonLibSSE-NG build dependencies.

The WebView2 SDK headers and static loader used by the project are already in
`extern/WebView2`; 

## Clone and build

Clone with CommonLibSSE-NG:

```powershell
git clone --recurse-submodules https://github.com/Degdatar/SkyNet.git
Set-Location SkyNet
```

If the repository was cloned without submodules, run:

```powershell
git submodule update --init --recursive
```

Configure and build a Release package:

```powershell
cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"
cmake --build build --config Release --target SkyNet --parallel
```

The ready-to-install mod files are written to:

```text
build/Release/Data/
```

Install that `Data` folder with MO2/Vortex, or merge its contents into a manual
Skyrim installation. Launch Skyrim through SKSE.

## Player controls

- Open **SKSE Menu Framework** with its normal **F1** key, choose **SkyNet**,
  then select **Open SkyNet**.
- Press **Escape** to close SkyNet.
- Use the native `−` button to minimize SkyNet; press **F2** to restore it.
- Browser settings include toggles for the splash screen and `Splash.mp4`
  video; the bundled video is enabled by default.