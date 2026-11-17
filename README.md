# VRam V1.0 — Windows Native Memory Cleaner

**VRam V1.0** is an open-source, lightweight memory management utility for **Windows 10** and **Windows 11**, written in native **C++20**, **Win32**, and **XAML Islands** (`DesktopWindowXamlSource`). It compiles directly to a standalone Win32 x64 executable without WebView2, Electron, Chromium, or external runtime dependencies.

> [!NOTE]
> **Project Identity**
> - **Product Name:** `VRam Free V1.0`
> - **File Description:** `VRam V1.0 - Memory Cleaner`
> - **Copyright:** `Vortex Open Source Project`
> - **Created by:** `@vinnotdkk`
> - **Repository:** https://github.com/vortexprojectsource/VRam

---

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Technical Stack & Linked Libraries](#technical-stack--linked-libraries)
3. [6-Stage Memory Purge Pipeline](#6-stage-memory-purge-pipeline)
4. [Privilege Model (Standard User vs. Administrator)](#privilege-model-standard-user-vs-administrator)
5. [User Interface & System Tray Features](#user-interface--system-tray-features)
6. [Configuration & Persistence (`config.xml`)](#configuration--persistence-configxml)
7. [Command-Line Arguments](#command-line-arguments)
8. [Building from Source](#building-from-source)
9. [Custom Branding & Icon Overrides](#custom-branding--icon-overrides)
10. [Repository Structure](#repository-structure)
11. [License & Support](#license--support)

---

## Architecture Overview

VRam separates native Windows system integration from its UI layer so memory operations never block user interaction:

```text
+-----------------------------------------------------------------------+
|                         wWinMain (src/main.cpp)                       |
|  - Enforces single instance via Local\VRam_V1_SingleInstance_Mutex    |
|  - Parses --minimized / --elevated-restart startup flags              |
|  - Initializes STA COM/WinRT & dispatches XAML Islands message loop   |
+-----------------------------------+-----------------------------------+
                                    |
          +-------------------------+-------------------------+
          v                                                   v
+-----------------------------+               +-----------------------------+
|   AppWindow (Win32 + XAML)  |               |  MemoryEngine (Background)  |
| - Frameless 368x488 DIP UI  |               | - GlobalMemoryStatusEx()    |
| - Custom pointer-drag window|  Worker Req   | - GetPerformanceInfo()      |
| - WS_EX_LAYERED alpha fade  | ------------> | - NtSetSystemInformation()  |
| - 2000ms telemetry polling  | <------------ | - SetSystemFileCacheSize()  |
| - Auto-Purge timer/threshold|  PurgeResult  | - SetProcessWorkingSetSizeEx|
+--------------+--------------+               +-----------------------------+
               |
    +----------+----------+
    v                     v
+--------------------+  +------------------------------+
|  TrayIconRenderer  |  |  ConfigManager               |
| - Direct2D/DWrite  |  | - Native xmllite.dll I/O     |
| - Live RAM % icon  |  | - %APPDATA%\VRam\config.xml  |
| - SVG/ICO fallback |  | - HKCU Run startup entry     |
+--------------------+  +------------------------------+
```

---

## Technical Stack & Linked Libraries

Every component in VRam relies directly on APIs built into Windows 10 and Windows 11.

| Component | Implementation | Purpose |
| :--- | :--- | :--- |
| **Language & Standard** | **C++20** (`/W4 /EHsc /permissive- /utf-8 /Zc:__cplusplus`) | Native Win32/WinRT systems implementation (`CMakeLists.txt`) |
| **Window Host & DPI** | **Win32 (`HWND`)** + `PerMonitorV2` manifest (`resources/app.manifest`) | Frameless windowing, custom fade transitions (`WS_EX_LAYERED`), sharp DPI scaling |
| **UI Rendering** | **XAML Islands** (`DesktopWindowXamlSource`, `RequestedTheme="Light"`) | Hardware-accelerated vector UI defined in `resources/ui/AppLayout.xaml` |
| **2D / Text Graphics** | **Direct2D (`d2d1`)**, **DirectWrite (`dwrite`)**, **Direct3D 11 (`d3d11`)**, **WIC (`windowscodecs`)** | Dynamic live-percentage system tray icons and external SVG/PNG rendering |
| **Memory Telemetry** | `GlobalMemoryStatusEx`, `GetPerformanceInfo`, `psapi` | Real-time physical RAM usage %, **USED**, **FREE**, and **CACHED** metrics |
| **Kernel Memory APIs** | `NtSetSystemInformation` (`ntdll.dll`), `SetSystemFileCacheSize`, `EmptyWorkingSet` | Working set trimming, cache flushing, page combining, and standby purging |
| **XML Persistence** | **Windows XML Lite (`xmllite.dll`)** + `shlwapi` | Zero-dependency XML serialization for `config.xml` |
| **System Integration** | `advapi32`, `shell32`, `dwmapi`, `comctl32`, `ole32` | Registry startup key, UAC elevation prompt, Shell tray notifications |

---

## 6-Stage Memory Purge Pipeline

When you click **Purge Memory** (from the main window or tray menu) or an **Auto Purge** trigger fires, **`MemoryEngine::Purge`** runs a deterministic 6-stage pipeline off the UI thread after requesting **`SE_INCREASE_QUOTA_NAME`**, **`SE_PROF_SINGLE_PROCESS_NAME`**, and **`SE_DEBUG_NAME`**:

| Stage | Operation | Underlying Windows API / Mechanism | Condition / `PurgeOptions` Flag |
| :---: | :--- | :--- | :--- |
| **1** | **Working Sets Trim** | Calls `NtSetSystemInformation(SystemMemoryListInformation [80], MemoryEmptyWorkingSets [2])` and enumerates accessible processes via `CreateToolhelp32Snapshot`, applying `SetProcessWorkingSetSizeEx(..., -1, -1, 0)` and `EmptyWorkingSet`. | `options.workingSets` |
| **2** | **System & Registry Cache Flush** | Clears filesystem cache size bounds via `SetSystemFileCacheSize((SIZE_T)-1, (SIZE_T)-1, 0)` and issues `NtSetSystemInformation(SystemRegistryReconciliationInformation [155])`. | `options.systemCache` |
| **3** | **Modified Page List Flush** | Flushes modified pages to their backing stores using `NtSetSystemInformation(SystemMemoryListInformation [80], MemoryFlushModifiedList [3])` before standby cleanup. | `options.modifiedList` |
| **4** | **Physical Page Combining** | Deduplicates identical physical memory pages across processes using `NtSetSystemInformation(SystemCombinePhysicalMemoryInformation [130])` with `MEMORY_COMBINE_INFORMATION_EX`. | Runs whenever `NtSetSystemInformation` is resolved |
| **5** | **Standby List Double-Sweep** | Purges `MemoryPurgeLowPriorityStandbyList [5]` first, followed immediately by `MemoryPurgeStandbyList [4]` (`SystemMemoryListInformation [80]`). | `options.standbyList` |
| **6** | **Settle Pass & Self-Trim** | Waits **70 ms** (`Sleep(70)`), repeats **Working Sets** (`workingSets`), **System File Cache** (`systemCache`), and **Standby List** (`standbyList`), runs `HeapCompact(GetProcessHeap(), 0)`, trims VRam's own process via `EmptyWorkingSet(GetCurrentProcess())`, and waits **80 ms** before reading the post-purge snapshot. | Finishes every purge cycle |

> [!TIP]
> **How Freed Memory Is Calculated**
> **`PurgeResult`** compares pre-purge and post-purge snapshots from `MemoryEngine::Query()` (`beforeUsedBytes` vs `afterUsedBytes`), returning `freedBytes`, percentage delta (`beforePercent` -> `afterPercent`), `trimmedProcesses`, `kernelPurgeSucceeded`, and `privilegeLimited`.

---

## Privilege Model (Standard User vs. Administrator)

VRam's manifest sets **`requestedExecutionLevel level="asInvoker"`** so the application launches immediately at user login without forcing a UAC prompt every time Windows boots.

| Feature / Scope | Standard User Mode | Administrator Mode (Elevated) |
| :--- | :--- | :--- |
| **Real-Time RAM Gauge & Monitoring** | **Full Support** (`GlobalMemoryStatusEx` / `GetPerformanceInfo`) | **Full Support** |
| **System Tray Icon & Live RAM % Badge** | **Full Support** | **Full Support** |
| **User-Owned Process Working Set Trim** | **Full Support** | **Full Support** |
| **Cross-Session / System Process Trimming** | **Limited** (non-accessible handles skipped) | **Full Support** (`SE_DEBUG_NAME` enabled) |
| **Standby List, Modified List & File Cache** | **Blocked by Kernel** (`privilegeLimited = true`) | **Full Support** (`SE_PROF_SINGLE_PROCESS_NAME` + `SE_INCREASE_QUOTA_NAME`) |

> [!IMPORTANT]
> **On-Demand UAC Elevation**
> If you run VRam un-elevated, it still trims accessible user-owned process working sets and notes **`| Elevate for standby`** when kernel privileges are restricted. Open **Settings** inside VRam and click **Elevate** to relaunch elevated via `ShellExecuteExW(..., L"runas", ...)` with `--elevated-restart`.

---

## User Interface & System Tray Features

- **Compact Frameless Window:** Fixed **`368 x 488` DIP** light interface (`#FFFFFF` root surface with `#6D28D9` primary purple accent and monochrome vector icons).
- **Window Dragging:** Click and drag on the **48px top header** or empty areas of the window (`CapturePointer` + `SetWindowPos`) without interfering with interactive buttons.
- **Smooth Layered Transitions:** Uses `WS_EX_LAYERED` + `SetLayeredWindowAttributes` (`12 ms` timer steps) for fade-in and fade-out transitions when opening, minimizing, or hiding to tray.
- **Live Ring Telemetry (`2000 ms` Polling):** Displays active physical RAM utilization percentage alongside **USED**, **FREE**, and **CACHED** readouts.
- **Two System Tray Icon Styles:**
  - **`Icon` Mode (`logo`):** Renders VRam's multi-resolution `VR` brand icon (`assets/vram.ico` or external `logo.svg`).
  - **`RAM %` Mode (`percent`):** Uses **Direct2D** + **DirectWrite** to dynamically render a rounded tray badge (`#F4F4F5` background, `#D4D4D8` border, `#18181B` digits) with a fill bar (`#6D28D9` normal, shifting to **`#DC2626`** at **>= 90%** usage).
- **Tray Context Menu:** Right-click the tray icon for quick actions: **`Hide to Tray`** / **`Show VRam`**, **`Purge Memory Now`**, **`Settings`**, and **`Exit VRam`**.
- **Auto-Purge Scheduler:**
  - **Threshold Trigger:** Automatically purges RAM at **`70%`**, **`75%`**, **`80%`**, **`85%`**, or **`90%`** (with a **60-second cooldown** between automatic triggers).
  - **Interval Trigger:** Scheduled background purge every **`5`**, **`15`**, **`30`**, or **`60` minutes** (or disabled at **`0`**).
- **Auto-Start with Windows:** Registers `"VRam"` under **`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`**, optionally appending `--minimized` to start directly in the system tray.

---

## Configuration & Persistence (`config.xml`)

VRam saves all preferences natively using **`xmllite.dll`** without third-party JSON/YAML parsers.

**Configuration File Path:**
```text
%APPDATA%\VRam\config.xml
```
*(Falls back to `<exe_dir>\VRam\config.xml` if `%APPDATA%` cannot be resolved.)*

**Default XML Output (`ConfigManager::Save`):**

```xml
<?xml version="1.0"?>
<VRamConfig version="1.0">
  <Startup autoStart="false" startMinimized="false" />
  <Window minimizeToTray="true" closeToTray="true" alwaysOnTop="false" trayStyle="logo" />
  <Purge workingSets="true" systemCache="true" standbyList="true" modifiedList="true" />
  <Automation autoPurgeEnabled="false" thresholdPercent="80" intervalMinutes="0" showNotifications="true" />
</VRamConfig>
```

| Element | Attribute | Accepted Values | Default | Description |
| :--- | :--- | :--- | :--- | :--- |
| **`Startup`** | `autoStart` | `true` / `false` (or `1` / `0`) | `false` | Adds or removes `"VRam"` in `HKCU\...\Run` |
| **`Startup`** | `startMinimized` | `true` / `false` (or `1` / `0`) | `false` | Appends `--minimized` to the startup launch command |
| **`Window`** | `minimizeToTray` | `true` / `false` (or `1` / `0`) | `true` | Hides window to tray instead of minimizing to taskbar |
| **`Window`** | `closeToTray` | `true` / `false` (or `1` / `0`) | `true` | Clicking close (`×`) hides to tray instead of exiting |
| **`Window`** | `alwaysOnTop` | `true` / `false` (or `1` / `0`) | `false` | Keeps the VRam window pinned above other windows |
| **`Window`** | `trayStyle` | `"logo"` / `"percent"` | `"logo"` | Selects between brand icon and Direct2D live RAM % badge |
| **`Purge`** | `workingSets` | `true` / `false` (or `1` / `0`) | `true` | Enables Stage 1 working set trimming |
| **`Purge`** | `systemCache` | `true` / `false` (or `1` / `0`) | `true` | Enables Stage 2 system & registry cache flushing |
| **`Purge`** | `standbyList` | `true` / `false` (or `1` / `0`) | `true` | Enables Stage 5 standby list cleanup |
| **`Purge`** | `modifiedList` | `true` / `false` (or `1` / `0`) | `true` | Enables Stage 3 modified page list flush |
| **`Automation`** | `autoPurgeEnabled` | `true` / `false` (or `1` / `0`) | `false` | Master switch for threshold/interval auto-purge |
| **`Automation`** | `thresholdPercent` | `70`, `75`, `80`, `85`, `90` in UI (`50`–`95` clamped) | `80` | RAM % threshold trigger (with 60s cooldown) |
| **`Automation`** | `intervalMinutes` | `0`, `5`, `15`, `30`, `60` in UI (`0`–`1440` clamped) | `0` | Periodic cleanup interval in minutes (`0` = off) |
| **`Automation`** | `showNotifications` | `true` / `false` (or `1` / `0`) | `true` | Displays tray balloon notification after a purge completes |

---

## Command-Line Arguments

| Argument | Behavior |
| :--- | :--- |
| **`--minimized`** | Starts VRam directly in the system tray without showing the main window on launch. Passed automatically when **Start Minimized** is enabled with **Run at Startup**. |
| **`--elevated-restart`** | Internal flag passed when relaunching via the **Elevate** button in **Settings** (`ShellExecuteExW` with `"runas"`). |

---

## Building from Source

### Prerequisites

| Requirement | Component / Version |
| :--- | :--- |
| **Operating System** | Windows 10 or Windows 11 (x64) |
| **CMake** | `3.20` or newer (`Kitware.CMake`) |
| **Compiler & SDK** | Visual Studio 2019 / 2022 / 2026 or **Visual Studio Build Tools** with **Desktop development with C++** (`Microsoft.VisualStudio.Component.VC.Tools.x86.x64`) and Windows SDK |

### Option 1: Automated Build Script (`build.bat`)

Run **`build.bat`** from the project root directory. The script automatically performs these steps:

1. Searches **`PATH`** and `%ProgramFiles%\CMake\bin\cmake.exe` for CMake.
2. Queries **`vswhere.exe`** for an installed MSVC x64 C++ toolchain (`Microsoft.VisualStudio.Component.VC.Tools.x86.x64`).
3. If **CMake** or **MSVC Build Tools** is missing, asks you to choose:
   - **`[1] Download / Install sekarang`** — installs missing dependencies via `winget` (or opens the official download pages in your browser if `winget` is unavailable).
   - **`[2] Batal`** — aborts the build cleanly.
4. Detects if **`VRam.exe`** is running (`tasklist`) and offers to close it so linker output is not locked.
5. Selects the matching Visual Studio generator (`2019` / `2022` / `2026`), runs `cmake -B build -A x64`, and builds **`Release`** (`build\Release\VRam.exe`).

```bat
build.bat
```

### Option 2: Manual CMake Commands

```bat
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

**Compiled Output:**
```text
build\Release\VRam.exe
```

---

## Custom Branding & Icon Overrides

Without recompiling the binary, you can override the header logo or system tray icon at runtime by placing custom files in the **same folder as `VRam.exe`**:

| External File (Beside `VRam.exe`) | Overrides | Fallback When Absent |
| :--- | :--- | :--- |
| **`logo.png`** | Top-left header emblem in the main window (`BitmapImage`, `DecodePixelHeight(44)`) | Embedded `IDR_PNG_LOGO` (`assets/logo_ui.png`) |
| **`logo.svg`** | System tray icon when `trayStyle` is `"logo"` (rendered via `ID2D1DeviceContext5::CreateSvgDocument`) | Embedded `IDI_VRAM_APP` (`assets/vram.ico` multi-res `16..256px`) |

---

## Repository Structure

```text
VRam V1.0/
├── assets/
│   ├── icons/                       # Monochrome SVG assets for UI & tray renderer
│   │   ├── back.svg, bell.svg, cache.svg, clock.svg, flush.svg
│   │   ├── gauge.svg, logo.svg, memory.svg, pin.svg, purge.svg
│   │   └── settings.svg, shield.svg, standby.svg, startup.svg, tray.svg
│   ├── logo.png                     # High-res 1024x1024 project logo source
│   ├── logo_ui.png                  # Tight-cropped header logo embedded as IDR_PNG_LOGO
│   └── vram.ico                     # Multi-resolution Windows icon (16, 24, 32, 48, 256 px)
├── resources/
│   ├── ui/
│   │   └── AppLayout.xaml           # Light-theme XAML Islands layout, styles & storyboards
│   ├── app.manifest                 # PerMonitorV2 DPI & asInvoker UAC manifest
│   ├── resource.h                   # Win32 resource IDs
│   └── VRam.rc                      # Embedded icon, PNG, XAML & PE VersionInfo metadata
├── src/
│   ├── main.cpp                     # wWinMain, single-instance mutex & message loop
│   ├── AppWindow.hpp / .cpp         # Win32 host window, XAML bindings, drag, fade & timers
│   ├── MemoryEngine.hpp / .cpp      # RAM telemetry & 6-stage kernel/user memory cleaner
│   ├── ConfigManager.hpp / .cpp     # xmllite XML config persistence & HKCU startup entry
│   └── TrayIconRenderer.hpp / .cpp  # Direct2D/DirectWrite tray badges & resource loader
├── .gitignore
├── build.bat                        # Automated CMake + MSVC dependency check & Release x64 build
├── CMakeLists.txt                   # C++20 CMake build definition
└── README.md
```

---

## License & Support

- **Project:** **Vortex Open Source Project** — [`github.com/vortexprojectsource`](https://github.com/vortexprojectsource)
- **Author:** **`@vinnotdkk`**
- **Support Development:** https://sociabuzz.com/idnvinnmalay
