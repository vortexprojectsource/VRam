#include "AppWindow.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>

#include <winrt/base.h>

#include <cstring>

namespace {

bool HasCommandLineFlag(const wchar_t* cmdLine, const wchar_t* flag) {
    if (!cmdLine || !flag) return false;
    return wcsstr(cmdLine, flag) != nullptr;
}

}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int) {
    const bool elevatedRestart = HasCommandLineFlag(lpCmdLine, L"--elevated-restart");
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Local\\VRam_V1_SingleInstance_Mutex");
    if (!elevatedRestart && hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(L"VRam_V1_HostWindow", nullptr)) {
            PostMessageW(existing, vram::WM_APP_SHOW_WINDOW, 0, 0);
        }
        CloseHandle(hMutex);
        return 0;
    }

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icc);

    winrt::init_apartment(winrt::apartment_type::single_threaded);

    const bool startMinimized = HasCommandLineFlag(lpCmdLine, L"--minimized");

    int exitCode = 0;
    {
        vram::AppWindow app(hInstance, startMinimized);
        if (!app.Create()) {
            if (hMutex) CloseHandle(hMutex);
            winrt::uninit_apartment();
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
    }

    winrt::uninit_apartment();
    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }
    return exitCode;
}
