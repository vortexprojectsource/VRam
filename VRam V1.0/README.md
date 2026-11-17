<p align="center">
  <img src="assets/logo_ui.png" alt="VRam Logo" width="140" />
</p>

<h1 align="center">VRam V1.0</h1>

<p align="center">
  Memory cleaner Windows berbasis native C++20, Win32 API, dan XAML Islands tanpa WebView.
</p>

<p align="center">
  <strong>Repository:</strong> <a href="https://github.com/vortexprojectsource">github.com/vortexprojectsource</a> &nbsp;|&nbsp;
  <strong>Author:</strong> @vinnotdkk &nbsp;|&nbsp;
  <strong>Support:</strong> <a href="https://sociabuzz.com/idnvinnmalay">sociabuzz.com/idnvinnmalay</a>
</p>

---

## Ringkasan

**VRam** adalah utilitas pembersih RAM ringan untuk Windows 10 dan Windows 11. Seluruh antarmuka dibangun langsung memakai **Win32 + C++/WinRT XAML Islands (`DesktopWindowXamlSource`)** dengan layout XML (`AppLayout.xaml`) dan ikon vektor SVG, tanpa runtime browser seperti WebView2 atau Electron.

Setelah melakukan purge, footprint proses VRam sendiri ikut dipangkas lewat `EmptyWorkingSet` sehingga tidak membebani memori saat berjalan di system tray.

---

## Fitur Utama

### 1. Pembersihan Memori 6-Tahap (`MemoryEngine`)
Saat tombol **Purge Memory** ditekan (atau dipicu oleh Auto Purge), VRam menjalankan urutan berikut:
1. **Working Sets Trim**: Menjalankan kernel `NtSetSystemInformation(SystemMemoryListInformation, MemoryEmptyWorkingSets)` serta mengiterasi proses yang dapat diakses memakai `SetProcessWorkingSetSizeEx(..., -1, -1, 0)` dan `EmptyWorkingSet`.
2. **System & Registry Cache Flush**: Membersihkan file cache sistem lewat `SetSystemFileCacheSize(-1, -1, 0)` dan mencoba pemangkasan cache registry (`SystemRegistryReconciliationInformation`).
3. **Modified Page List Flush**: Memindahkan halaman modified ke standby list (`MemoryFlushModifiedList`) sebelum pembersihan standby list dijalankan.
4. **Physical Page Combining**: Menggabungkan halaman memori fisik yang identik antar proses lewat `NtSetSystemInformation(SystemCombinePhysicalMemoryInformation)`.
5. **Standby List Double-Sweep**: Membersihkan `MemoryPurgeLowPriorityStandbyList` lalu `MemoryPurgeStandbyList`.
6. **Second-Pass Settle & Self Trim**: Jeda `70ms` diikuti penyapuan ulang working sets, file cache, standby list, `HeapCompact`, serta pemangkasan working set VRam sendiri.

> **Catatan Privilege**: Tanpa hak Administrator, Windows hanya mengizinkan pemangkasan working set proses milik user saat ini. Menjalankan `VRam.exe` sebagai Administrator (atau lewat tombol **Elevate** di menu Settings) mengaktifkan pembersihan Standby List, Modified Page List, dan System File Cache secara penuh.

### 2. Pemantauan & Tampilan
- Pembacaan penggunaan RAM fisik real-time melalui `GlobalMemoryStatusEx` dan `GetPerformanceInfo` (`USED`, `FREE`, `CACHED`).
- Custom frameless light UI (`368×488` DIPs) dengan fade-in/fade-out saat dibuka, di-minimize, disembunyikan ke tray, atau ditutup.
- Area drag jendela di header, sub-header Settings, serta area kosong halaman utama.

### 3. System Tray & Otomatisasi
- **Tray Icon Style**: Pilih ikon logo `VR` atau indikator persentase RAM live yang dirender langsung memakai Direct2D & DirectWrite.
- **Minimize / Close to Tray**: Sembunyikan aplikasi ke system tray dan akses cepat melalui klik kanan (`Show VRam`, `Purge Memory Now`, `Settings`, `Exit VRam`).
- **Auto-Purge**:
  - Otomatis purge saat pemakaian RAM melewati ambang batas (`70%`, `75%`, `80%`, `85%`, `90%`) dengan cooldown `60` detik.
  - Atau purge berkala berdasarkan interval (`Off`, `5m`, `15m`, `30m`, `60m`).
- **Windows Startup**: Mendaftarkan entry di `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` (otomatis menyertakan argumen `--minimized` jika opsi *Start minimized* aktif).
- **Persistensi Konfigurasi XML**: Semua pengaturan disimpan langsung ke `%APPDATA%\VRam\config.xml` memakai parser/writer bawaan Windows `xmllite.dll`.

---

## Persyaratan Sistem & Build

- **OS**: Windows 10 versi 1903+ atau Windows 11 (x64)
- **Tools untuk Build**:
  - Visual Studio 2022 (atau Build Tools) dengan workload **Desktop development with C++** + Windows 10/11 SDK
  - CMake `3.20` atau lebih baru

---

## Cara Build

### Opsi 1: Memakai `build.bat` (Otomatis)
Jalankan `build.bat` langsung dari folder proyek:

```bat
build.bat
```

Script ini akan:
1. Mengecek ketersediaan `cmake.exe` dan compiler MSVC (`vswhere`).
2. Jika ada tools yang belum terpasang, menampilkan pilihan **[1] Download / Install sekarang** (via `winget` atau membuka halaman download resmi di browser) atau **[2] Batal**.
3. Memeriksa apakah `VRam.exe` sedang berjalan agar proses linking tidak terkunci.
4. Menjalankan konfigurasi CMake & compile Release x64 ke `build\Release\VRam.exe`.

### Opsi 2: Manual via CMake

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output executable berada di `build\Release\VRam.exe`.

### Argumen Baris Perintah

```text
VRam.exe --minimized
```

Menjalankan aplikasi langsung tersembunyi ke system tray tanpa memunculkan jendela utama.

---

## Mengganti Logo Aplikasi

Jika ingin mengganti logo tanpa compile ulang, letakkan file `logo.png` tepat di sebelah `VRam.exe`. Untuk mengubah ikon `.exe` permanen, ganti `assets/vram.ico` dan `assets/logo_ui.png` lalu jalankan `build.bat` ulang.

---

## Struktur Proyek

```text
VRam/
├── build.bat                   # Script build otomatis + pengecekan dependency
├── CMakeLists.txt              # Konfigurasi CMake C++20
├── README.md
├── .gitignore
├── assets/
│   ├── logo.png                # Logo sumber resolusi penuh
│   ├── logo_ui.png             # Logo header UI
│   ├── vram.ico                # Ikon aplikasi multi-resolusi (16..256px)
│   └── icons/                  # Aset vektor SVG
├── resources/
│   ├── VRam.rc                 # Resource ikon, manifest, XAML, PNG, dan VERSIONINFO
│   ├── app.manifest            # PerMonitorV2 DPI & kompatibilitas XAML Islands
│   ├── resource.h
│   └── ui/
│       └── AppLayout.xaml      # Definisi layout XAML, style custom, dan animasi
└── src/
    ├── main.cpp                # Entry point wWinMain, mutex single-instance, loop STA
    ├── AppWindow.hpp/.cpp      # Host HWND, XAML Islands bridge, tray, animasi fade & drag
    ├── MemoryEngine.hpp/.cpp   # Query RAM & engine pembersihan memori 6-tahap
    ├── ConfigManager.hpp/.cpp  # Baca/tulis config.xml (xmllite) & startup registry
    └── TrayIconRenderer.hpp/.cpp
```

---

## Kontribusi & Dukungan

- **Project**: Vortex Open Source Project ([https://github.com/vortexprojectsource](https://github.com/vortexprojectsource))
- **Creator**: `@vinnotdkk`
- **Support Developer**: [https://sociabuzz.com/idnvinnmalay](https://sociabuzz.com/idnvinnmalay)
