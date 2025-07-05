#include "../include/SoundTrackerGUI.h"
#include <windows.h>
#include <iostream>
#include <shellapi.h>

// Enable visual styles
#pragma comment(linker,"\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

BOOL IsRunAsAdmin() {
    BOOL isAdmin = FALSE;
    HANDLE hToken = NULL;
    
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION elevation = { 0 };
        DWORD dwSize = 0;
        
        if (GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &dwSize)) {
            isAdmin = elevation.TokenIsElevated;
        }
        
        CloseHandle(hToken);
    }
    
    return isAdmin;
}

void ShowErrorAndExit(LPCWSTR message) {
    MessageBox(NULL, message, L"Sound Tracker Error", MB_ICONERROR | MB_OK);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    // Check for administrator privileges
    if (!IsRunAsAdmin()) {
        int result = MessageBox(NULL, 
            L"This application requires Administrator privileges to monitor all system sounds.\n\n"
            L"Would you like to restart as Administrator?", 
            L"Administrator Privileges Required", 
            MB_YESNO | MB_ICONWARNING);
            
        if (result == IDYES) {
            // Restart as admin
            WCHAR szPath[MAX_PATH];
            if (GetModuleFileName(NULL, szPath, ARRAYSIZE(szPath))) {
                SHELLEXECUTEINFO sei = { sizeof(sei) };
                sei.lpVerb = L"runas";
                sei.lpFile = szPath;
                sei.hwnd = NULL;
                sei.nShow = SW_NORMAL;
                
                if (!ShellExecuteEx(&sei)) {
                    DWORD dwError = GetLastError();
                    if (dwError == ERROR_CANCELLED) {
                        // User refused elevation
                        ShowErrorAndExit(L"Administrator privileges are required to run this application.");
                    }
                }
            }
        } else {
            ShowErrorAndExit(L"Administrator privileges are required to run this application.");
        }
        return 1;
    }
    
    // Initialize COM with multithreaded model to match SoundTracker
    // This ensures consistent threading model across the application
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        ShowErrorAndExit(L"Failed to initialize COM library.");
        return 1;
    }
    
    // Create and run the application
    {
        SoundTrackerGUI app;
        
        if (!app.Initialize(hInstance)) {
            ShowErrorAndExit(L"Failed to initialize the application.");
            CoUninitialize();
            return 1;
        }
        
        // Run message loop
        int result = app.Run();
        
        // Cleanup is handled in destructor
        CoUninitialize();
        return result;
    }
}