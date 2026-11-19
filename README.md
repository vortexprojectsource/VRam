# VRam V2.0 — Windows Native Memory Cleaner & Game Shield

**VRam V2.0** is an open-source, standalone native memory cleaner and game RAM shield for **Windows 10** (1903+ / build 18362+) and **Windows 11**, written in **C++20**, **Win32**, **Direct2D/DirectWrite**, and **XAML Islands** (`DesktopWindowXamlSource`). It statically links the MSVC runtime (`/MT`) so it runs out of the box on clean Windows machines without requiring any VC++ Redistributable, WebView2, Electron, or external frameworks.

> [!NOTE]
> **Project Identity**
> - **Product Name:** `VRam V2.0`
> - **File Description:** `VRam V2.0 - Memory Cleaner & Game Shield`
> - **Copyright:** `Vortex Open Source Project`
> - **Created by:** `@vinnotdkk`
> - **Repository:** https://github.com/vortexprojectsource/VRam

---

## Table of Contents

1. [What's New in V2.0](#whats-new-in-v20)
2. [Architecture Overview](#architecture-overview)
3. [Technical Stack & Linked Libraries](#technical-stack--linked-libraries)
4. [Memory Purge Pipeline & Game Shield](#memory-purge-pipeline--game-shield)
5. [Privilege Model (Standard User vs. Administrator)](#privilege-model-standard-user-vs-administrator)
6. [Notifications & System Tray Features](#notifications--system-tray-features)
7. [Configuration & Persistence (`config.xml`)](#configuration--persistence-configxml)
8. [Command-Line Arguments](#command-line-arguments)
9. [Building from Source](#building-from-source)
10. [Custom Branding & Icon Overrides](#custom-branding--icon-overrides)
11. [Repository Structure](#repository-structure)
12. [License & Support](#license--support)

---

## What's New in V2.0

- **Zero-Redist Portable Binary (`/MT` Static CRT):** Statically links MSVC runtime libraries (`MultiThreaded`), eliminating `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll`, and `MSVCP140.dll` missing-DLL startup failures on fresh PCs.
- **Cross-Device Stability & Crash Hardening:**
  - Top-level WinRT/COM and `OnCreate()` XAML Island exception catching with clear diagnostic `MessageBoxW` reporting instead of silent termination on stripped or legacy Windows builds.
  - Safe `.try_as<T>()` control binding, `m_dpi` zero-guards, `_snwprintf_s(..., _TRUNCATE)` bounded string formatting, WIC `Lock` / D2D brush `HRESULT` validation, and a pure-GDI fallback renderer for `%` tray icons.
  - Protected core Windows process exclusion list (`csrss.exe`, `smss.exe`, `wininit.exe`, `services.exe`, `lsass.exe`, `dwm.exe`, `audiodg.exe`, `svchost.exe`, `Memory Compression`, etc.) during elevated working-set sweeps.
- **Floating Purge Toast Notification (`PurgeToastWindow`):** Compact non-activating layered `HWND_TOPMOST` toast (`356 x 118` DIP) in VRam's white-purple-black theme (`Direct2D` + `DirectWrite` + `UpdateLayeredWindow`). Shows the embedded VR logo and a spinning violet ring while purging, transitions to a purple check badge with freed RAM and protected game metrics when complete, and auto-fades out via a slim bottom countdown bar after ~3 seconds.
- **Game Shield (Select, Exclude & Maximize Game RAM):** Select any game `.exe` (via file browser or one-click picker from live running apps) so it is bypassed during memory purges while receiving elevated process/memory/I/O prioritization and soft working-set expansion headroom.

---

## Architecture Overview

VRam separates native Windows system integration from its UI layer so memory operations never block user interaction:

```text
+-----------------------------------------------------------------------+
|                         wWinMain (src/main.cpp)                       |
|  - Enforces single instance via Local\VRam_V2_SingleInstance_Mutex    |
|  - Parses --minimized / --elevated-restart startup flags              |
|  - Initializes STA WinRT cleanly & catches unhandled startup errors   |
+-----------------------------------+-----------------------------------+
                                    |
          +-------------------------+-------------------------+
          v                                                   v
+-----------------------------+               +-----------------------------+
|   AppWindow (Win32 + XAML)  |               |  MemoryEngine (Background)  |
| - Frameless 368x524 DIP UI  |               | - GlobalMemoryStatusEx()    |
| - Main / Game Shield /Cfg   |  Worker Req   | - GetPerformanceInfo()      |
| - WS_EX_LAYERED alpha fade  | ------------> | - NtSetSystemInformation()  |
| - 2000ms telemetry polling  | <------------ | - SetSystemFileCacheSize()  |
| - Auto-Purge & Toast host   |  PurgeResult  | - Game Shield prioritization|
+--------------+--------------+               +-----------------------------+
               |
    +----------+--------------------+--------------------+
    v                               v                    v
+--------------------+  +------------------------+  +-------------------+
|  TrayIconRenderer  |  |  PurgeToastWindow      |  |  ConfigManager    |
| - Direct2D/DWrite  |  | - UpdateLayeredWindow  |  | - Native xmllite  |
| - Live RAM % + GDI |  | - Topmost non-focusing |  | - %APPDATA%\VRam  |
| - SVG/ICO fallback |  | - Auto-close countdown |  | - HKCU Run key    |
+--------------------+  +------------------------+  +-------------------+
```

---

## Technical Stack & Linked Libraries

Every component in VRam relies directly on APIs built into Windows 10 and Windows 11 with zero external runtime DLL requirements:

| Component | Implementation | Purpose |
| :--- | :--- | :--- |
| **Language & Runtime** | **C++20** (`/W4 /EHsc /permissive- /utf-8`, `/MT` static CRT) | Self-contained x64 binary (`CMakeLists.txt`) |
| **Window Host & DPI** | **Win32 (`HWND`)** + `PerMonitorV2` manifest (`resources/app.manifest`) | Frameless windowing, custom fade transitions (`WS_EX_LAYERED`), sharp DPI scaling |
| **UI Rendering** | **XAML Islands** (`DesktopWindowXamlSource`, `RequestedTheme="Light"`) | Hardware-accelerated vector UI defined in `resources/ui/AppLayout.xaml` |
| **2D / Toast Graphics** | **Direct2D (`d2d1`)**, **DirectWrite (`dwrite`)**, **Direct3D 11 (`d3d11`)**, **WIC** | Floating layered purge notification card, dynamic RAM % tray badges, SVG/PNG icons |
| **Memory & Process APIs** | `GlobalMemoryStatusEx`, `GetPerformanceInfo`, `psapi`, `tlhelp32` | Live physical RAM metrics, app enumeration, and Game Shield inspection |
| **Kernel & Boost APIs** | `NtSetSystemInformation`, `NtSetInformationProcess`, `SetProcessInformation` | Memory list purging, EcoQoS off, Memory Priority Normal, I/O & soft working-set headroom |
| **Dialogs & Persistence** | **Windows XML Lite (`xmllite.dll`)**, `comdlg32`, `shlwapi` | Native `config.xml` I/O and `.exe` file picker dialog |
| **System Integration** | `advapi32`, `shell32`, `dwmapi`, `comctl32`, `ole32` | Registry startup entry, UAC elevation, Shell tray menu & theme integration |

---

## Memory Purge Pipeline & Game Shield

### 6-Stage Memory Purge Pipeline

When you trigger **Purge Memory** or an **Auto Purge** schedule fires, **`MemoryEngine::Purge`** runs off the UI thread after requesting **`SE_INCREASE_QUOTA_NAME`**, **`SE_PROF_SINGLE_PROCESS_NAME`**, **`SE_INC_BASE_PRIORITY_NAME`**, and **`SE_DEBUG_NAME`**:

| Stage | Operation | Underlying Windows API / Mechanism | Notes |
| :---: | :--- | :--- | :--- |
| **1** | **Protected Working Sets Trim** | Trims accessible non-system, non-shielded background processes via `SetProcessWorkingSetSizeEx(..., -1, -1, 0)` and `EmptyWorkingSet`. Calls `MemoryEmptyWorkingSets [2]` only when no protected game is actively running. | Bypasses core Windows OS processes and all selected **Game Shield** executables |
| **2** | **System & Registry Cache Flush** | Clears filesystem cache bounds (`SetSystemFileCacheSize`) and issues `NtSetSystemInformation(SystemRegistryReconciliationInformation [155])`. | Controlled by `options.systemCache` |
| **3** | **Modified Page List Flush** | Flushes modified pages to backing storage using `MemoryFlushModifiedList [3]`. | Controlled by `options.modifiedList` |
| **4** | **Physical Page Combining** | Deduplicates identical physical pages using `SystemCombinePhysicalMemoryInformation [130]`. | Runs when `ntdll` resolves `NtSetSystemInformation` |
| **5** | **Standby List Double-Sweep** | Releases `MemoryPurgeLowPriorityStandbyList [5]` then `MemoryPurgeStandbyList [4]`, converting cached standby pages back into free physical RAM without trimming active protected games. | Controlled by `options.standbyList` |
| **6** | **Settle Pass & Re-Boost** | Waits **70 ms**, performs settle trim & standby pass, reapplies **Game Shield** boost if active, compacts VRam's own heap, waits **80 ms**, and computes `freedBytes`. | Updates gauge & toast summary |

### Game Shield (RAM Protection & Maximization)

Configured games in `config.xml` receive continuous protection and non-destructive OS tuning when **Maximize game RAM** (`maximizeGameRam`) is enabled:
- **Purge Exclusion:** All matching running `.exe` processes are skipped during working-set trimming, and kernel-wide `MemoryEmptyWorkingSets` (`[2]`) is suppressed while any shielded game is active so Windows never shrinks its working set.
- **Page Retention & Memory Priority:** Applies `SetProcessInformation(..., ProcessMemoryPriority, ...)` = **`5` (`MEMORY_PRIORITY_NORMAL`)** so Windows Memory Manager avoids aggressively paging out the game under pressure.
- **CPU & EcoQoS Unthrottling:** Clears `PROCESS_POWER_THROTTLING_EXECUTION_SPEED` via `ProcessPowerThrottling` (disabling EcoQoS), enables dynamic priority boost (`SetProcessPriorityBoost(..., FALSE)`), raises `NORMAL`/`BELOW_NORMAL` priority classes to **`ABOVE_NORMAL_PRIORITY_CLASS`**, and sets high I/O hint (`NtSetInformationProcess(..., ProcessIoPriority, 3)`).
- **Soft Working-Set Headroom:** Expands soft min/max working-set guidance using `QUOTA_LIMITS_HARDWS_MIN_DISABLE | QUOTA_LIMITS_HARDWS_MAX_DISABLE` clamped safely beneath physical RAM minus a `1.5 GB` OS reserve (never enforcing hard limits that could cause out-of-memory errors).

> [!WARNING]
> Some games protected by kernel-level anti-cheat drivers (e.g., Vanguard, EAC, BattlEye) restrict external handle access (`PROCESS_SET_INFORMATION` / `PROCESS_SET_QUOTA`). When access is blocked by anti-cheat, VRam safely skips handle mutation and still shields the game from VRam's own purge passes (`MemoryEmptyWorkingSets` suppression + process skip).

---

## Privilege Model (Standard User vs. Administrator)

VRam's manifest uses **`requestedExecutionLevel level="asInvoker"`** so it starts at login without intrusive UAC dialogs.

| Feature / Scope | Standard User Mode | Administrator Mode (Elevated) |
| :--- | :--- | :--- |
| **Real-Time RAM Gauge & Toast Notification** | **Full Support** | **Full Support** |
| **Game Shield Selection & Exclusion** | **Full Support** | **Full Support** |
| **User-Owned Process Trim & Game Boost** | **Full Support** | **Full Support** (`SE_INC_BASE_PRIORITY_NAME` improves high I/O priority) |
| **Cross-Session / Background Trim** | **Limited** | **Full Support** (`SE_DEBUG_NAME`, core OS processes still skipped safely) |
| **Standby List, Modified List & File Cache** | **Blocked by Kernel** | **Full Support** (`SE_PROF_SINGLE_PROCESS_NAME` + `SE_INCREASE_QUOTA_NAME`) |

Open **Settings -> Elevate** at any time to relaunch elevated via `ShellExecuteExW(..., L"runas", ...)` with `--elevated-restart`.

---

## Notifications & System Tray Features

- **Compact Frameless Window (`368 x 524` DIP):** Clean light-mode interface with an animated ring gauge, **USED / FREE / CACHED** metrics, **Game Shield** status bar, **Game Shield** view, and **Settings**.
- **VRam Purge Toast (`PurgeToastWindow`):** Matches VRam's white (`#FFFFFF`), purple (`#6D28D9` / `#F3EEFF`), and black (`#111113`) palette cleanly at bottom-right (`WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE`) whenever manual or auto purge runs — even while minimized to tray — and closes automatically after completion.
- **Two System Tray Icon Styles:**
  - **`Icon` (`logo`):** Crisp brand icon (`assets/vram.ico` or external `logo.svg`).
  - **`RAM %` (`percent`):** Live percentage badge via **Direct2D + DirectWrite** (with automatic pure-GDI fallback renderer).
- **Tray Context Menu:** Right-click the tray icon for **`Hide to Tray` / `Show VRam V2.0`**, **`Purge Memory Now`**, **`Game Shield`**, **`Settings`**, and **`Exit VRam`**.

---

## Configuration & Persistence (`config.xml`)

Settings are stored natively via **`xmllite.dll`** at `%APPDATA%\VRam\config.xml` (existing V1.0 configs migrate automatically):

```xml
<?xml version="1.0"?>
<VRamConfig version="2.0">
  <Startup autoStart="false" startMinimized="false" />
  <Window minimizeToTray="true" closeToTray="true" alwaysOnTop="false" trayStyle="logo" />
  <Purge workingSets="true" systemCache="true" standbyList="true" modifiedList="true" />
  <Automation autoPurgeEnabled="false" thresholdPercent="80" intervalMinutes="0" showNotifications="true" />
  <GameProtection maximizeRam="true">
    <Game exe="cs2.exe" />
  </GameProtection>
</VRamConfig>
```

---

## Command-Line Arguments

| Argument | Behavior |
| :--- | :--- |
| **`--minimized`** | Starts VRam directly in the system tray without showing the main window on launch. |
| **`--elevated-restart`** | Internal flag passed when relaunching via **Settings -> Elevate**. |

---

## Building from Source

### Prerequisites
- **Windows 10 / 11 (x64)**
- **CMake `3.20+`** (`Kitware.CMake`)
- **Visual Studio 2019 / 2022 / 2026** or **Build Tools** with **Desktop development with C++**

### Automated or Manual Build

```bat
build.bat
```

Or manually via CMake:

```bat
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output executable: `build\Release\VRam.exe` (statically linked `/MT`).

---

## Custom Branding & Icon Overrides

Place `logo.png` (header emblem override) or `logo.svg` (tray icon override) in the same directory as `VRam.exe` to customize branding at runtime without recompiling.

---

## Repository Structure

```text
VRam/
├── assets/
│   ├── icons/                        # Monochrome SVG icons
│   ├── logo.png / logo_ui.png        # High-res & tight-cropped VR logos
│   └── vram.ico                      # Multi-resolution Windows icon (16..256 px)
├── resources/
│   ├── ui/AppLayout.xaml             # XAML Islands UI layout (Main, Game Shield, Settings)
│   ├── app.manifest                  # PerMonitorV2 DPI, XAML compatibility & asInvoker manifest
│   ├── resource.h                    # Win32 resource IDs
│   └── VRam.rc                       # Embedded resources & V2.0 PE VersionInfo metadata
├── src/
│   ├── main.cpp                      # wWinMain, single-instance mutex & exception guards
│   ├── AppWindow.hpp / .cpp          # Win32 host window, XAML bindings, Game Shield & polling
│   ├── MemoryEngine.hpp / .cpp       # RAM telemetry, 6-stage purge & Game Shield prioritization
│   ├── PurgeToastWindow.hpp / .cpp   # Layered white-purple-black purge toast notification
│   ├── ConfigManager.hpp / .cpp      # xmllite XML config I/O & HKCU startup entry
│   └── TrayIconRenderer.hpp / .cpp   # Direct2D/DWrite + GDI fallback tray renderer
├── .gitignore
├── build.bat                         # CMake + MSVC Release x64 builder
├── CMakeLists.txt                    # C++20 + /MT static runtime CMake build config
└── README.md
```

---

## License & Support

- **Project:** **Vortex Open Source Project** — [`github.com/vortexprojectsource`](https://github.com/vortexprojectsource)
- **Author:** **`@vinnotdkk`**
- **Support Development:** https://sociabuzz.com/idnvinnmalay
