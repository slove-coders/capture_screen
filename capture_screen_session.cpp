#include <iostream>
#include <windows.h>
#include <wtsapi32.h>
#include <tchar.h>
#include <userenv.h>
#include <string>

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "user32.lib")

void EnablePrivilege(LPCSTR privilege) {
    HANDLE hToken;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        std::cerr << "Failed to open process token. Error: " << GetLastError() << std::endl;
        return;
    }

    TOKEN_PRIVILEGES tp;
    if (!LookupPrivilegeValue(NULL, privilege, &tp.Privileges[0].Luid)) {
        std::cerr << "Failed to lookup privilege value. Error: " << GetLastError() << std::endl;
        CloseHandle(hToken);
        return;
    }

    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), NULL, NULL)) {
        std::cerr << "Failed to adjust token privileges. Error: " << GetLastError() << std::endl;
    }

    CloseHandle(hToken);
}

DWORD GetActiveSessionId() {
    DWORD activeSessionId = WTSGetActiveConsoleSessionId();
    if (activeSessionId == 0xFFFFFFFF) {
        std::cerr << "No active session found. Defaulting to session 1." << std::endl;
        return 1;  // 默认回到 Session 1
    }
    return activeSessionId;
}

bool ImpersonateActiveUser(DWORD sessionId) {
    HANDLE hToken;
    if (!WTSQueryUserToken(sessionId, &hToken)) {
        std::cerr << "Failed to query user token. Error: " << GetLastError() << std::endl;
        return false;
    }

    HANDLE hDupToken;
    if (!DuplicateTokenEx(hToken, MAXIMUM_ALLOWED, NULL, SecurityIdentification, TokenPrimary, &hDupToken)) {
        std::cerr << "Failed to duplicate token. Error: " << GetLastError() << std::endl;
        CloseHandle(hToken);
        return false;
    }

    if (!SetTokenInformation(hDupToken, TokenSessionId, &sessionId, sizeof(DWORD))) {
        std::cerr << "Failed to set token session ID. Error: " << GetLastError() << std::endl;
        CloseHandle(hDupToken);
        CloseHandle(hToken);
        return false;
    }

    if (!ImpersonateLoggedOnUser(hDupToken)) {
        std::cerr << "Failed to impersonate user. Error: " << GetLastError() << std::endl;
        CloseHandle(hDupToken);
        CloseHandle(hToken);
        return false;
    }

    CloseHandle(hDupToken);
    CloseHandle(hToken);
    return true;
}

bool SwitchToDesktop(const char* desktopName) {
    HDESK hDesk = OpenDesktopA(desktopName, 0, FALSE, GENERIC_ALL);
    if (!hDesk) {
        std::cerr << "Failed to open desktop " << desktopName << ". Error: " << GetLastError() << std::endl;
        return false;
    }

    if (!SetThreadDesktop(hDesk)) {
        std::cerr << "Failed to set thread desktop. Error: " << GetLastError() << std::endl;
        CloseDesktop(hDesk);
        return false;
    }

    std::cout << "Switched to desktop: " << desktopName << std::endl;
    return true;
}

bool CaptureScreenToBitmap(LPCSTR fileName) {
    int screenX = GetSystemMetrics(SM_CXSCREEN);
    int screenY = GetSystemMetrics(SM_CYSCREEN);

    HDC hdcScreen = GetDC(NULL);
    if (!hdcScreen) {
        std::cerr << "Failed to get screen DC. Error: " << GetLastError() << std::endl;
        return false;
    }

    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hBitmap = CreateCompatibleBitmap(hdcScreen, screenX, screenY);
    SelectObject(hdcMem, hBitmap);

    if (!BitBlt(hdcMem, 0, 0, screenX, screenY, hdcScreen, 0, 0, SRCCOPY)) {
        std::cerr << "BitBlt failed! Error: " << GetLastError() << std::endl;
        DeleteObject(hBitmap);
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
        return false;
    }

    BITMAPFILEHEADER bmfHeader;
    BITMAPINFOHEADER bi;
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = screenX;
    bi.biHeight = -screenY;  // Negative to store top-down image
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;
    bi.biSizeImage = 0;
    bi.biXPelsPerMeter = 0;
    bi.biYPelsPerMeter = 0;
    bi.biClrUsed = 0;
    bi.biClrImportant = 0;

    DWORD dwBmpSize = ((screenX * bi.biBitCount + 31) / 32) * 4 * screenY;
    HANDLE hDIB = GlobalAlloc(GHND, dwBmpSize);
    char* lpbitmap = (char*)GlobalLock(hDIB);

    GetDIBits(hdcScreen, hBitmap, 0, (UINT)screenY, lpbitmap, (BITMAPINFO*)&bi, DIB_RGB_COLORS);

    HANDLE hFile = CreateFileA(fileName, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        std::cerr << "Failed to create file. Error: " << GetLastError() << std::endl;
        GlobalUnlock(hDIB);
        GlobalFree(hDIB);
        DeleteObject(hBitmap);
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
        return false;
    }

    DWORD dwSizeofDIB = dwBmpSize + sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bmfHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bmfHeader.bfSize = dwSizeofDIB;
    bmfHeader.bfType = 0x4D42; // BM

    DWORD dwBytesWritten;
    WriteFile(hFile, (LPSTR)&bmfHeader, sizeof(BITMAPFILEHEADER), &dwBytesWritten, NULL);
    WriteFile(hFile, (LPSTR)&bi, sizeof(BITMAPINFOHEADER), &dwBytesWritten, NULL);
    WriteFile(hFile, (LPSTR)lpbitmap, dwBmpSize, &dwBytesWritten, NULL);

    GlobalUnlock(hDIB);
    GlobalFree(hDIB);
    CloseHandle(hFile);

    DeleteObject(hBitmap);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);

    std::cout << "Screenshot saved to " << fileName << std::endl;
    return true;
}

int main() {
    
    if (!CaptureScreenToBitmap("screenshot2.bmp")) {
        std::cerr << "Failed to capture screen." << std::endl;
        return 1;
    }

    std::cout << "Screen captured successfully!" << std::endl;
    return 0;
}
