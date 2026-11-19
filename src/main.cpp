#include "AppWindow.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <objbase.h>

#include <cstdio>
#include <exception>
#include <string>

namespace {

bool HasCommandLineFlag(const wchar_t* cmdLine, const wchar_t* flag) {
    if (!cmdLine || !flag) return false;
    return wcsstr(cmdLine, flag) != nullptr;
}

std::wstring FormatStartupError(const wchar_t* reason, HRESULT hr) {
    wchar_t buf[512]{};
    _snwprintf_s(buf, _TRUNCATE,
                 L"VRam V2.1 failed to initialize required Windows components.\n\n"
                 L"Detail: %s (HRESULT 0x%08X)\n\n"
                 L"Try running VRam as Administrator.",
                 reason ? reason : L"Unknown initialization failure",
                 static_cast<unsigned int>(hr));
    return buf;
}

}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int) {
    const bool elevatedRestart = HasCommandLineFlag(lpCmdLine, L"--elevated-restart");
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Local\\VRam_V2_SingleInstance_Mutex");
    if (!elevatedRestart && hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(L"VRam_V2_HostWindow", nullptr)) {
            PostMessageW(existing, vram::WM_APP_SHOW_WINDOW, 0, 0);
        }
        CloseHandle(hMutex);
        return 0;
    }

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icc);

    const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool comInitialized = SUCCEEDED(hrCom);
    if (FAILED(hrCom) && hrCom != RPC_E_CHANGED_MODE) {
        const std::wstring msg = FormatStartupError(L"COM apartment initialization failed", hrCom);
        MessageBoxW(nullptr, msg.c_str(), L"VRam V2.1 - Startup Error", MB_OK | MB_ICONERROR);
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    const bool startMinimized = HasCommandLineFlag(lpCmdLine, L"--minimized");

    int exitCode = 0;
    try {
        vram::AppWindow app(hInstance, startMinimized);
        if (!app.Create()) {
            MessageBoxW(nullptr,
                        L"VRam V2.1 could not create its main window.",
                        L"VRam V2.1 - Startup Error", MB_OK | MB_ICONERROR);
            if (hMutex) CloseHandle(hMutex);
            if (comInitialized) CoUninitialize();
            return 1;
        }

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (app.PreTranslateMessage(&msg)) {
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        exitCode = static_cast<int>(msg.wParam);
    } catch (const std::exception& ex) {
        wchar_t wbuf[256]{};
        MultiByteToWideChar(CP_UTF8, 0, ex.what(), -1, wbuf, 256);
        const std::wstring msg = FormatStartupError(wbuf, E_FAIL);
        MessageBoxW(nullptr, msg.c_str(), L"VRam V2.1 - Runtime Error", MB_OK | MB_ICONERROR);
        exitCode = 1;
    } catch (...) {
        MessageBoxW(nullptr, L"Unexpected unhandled exception in VRam V2.1.",
                    L"VRam V2.1 - Runtime Error", MB_OK | MB_ICONERROR);
        exitCode = 1;
    }

    if (comInitialized) {
        CoUninitialize();
    }
    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }
    return exitCode;
}
