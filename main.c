#include <Windows.h>
#include <ShlObj.h>
#include <shlwapi.h>
#include <TlHelp32.h>
#include <CommCtrl.h>
#include <richedit.h>

#include "binary.h"

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Comctl32.lib")

#define ERASE_ENTRY_POINT    TRUE

// ===================== GUI Kontrolleri =====================
#define ID_INJECT_BTN        1001
#define ID_STATUS_STATIC     1002
#define ID_PROGRESS_BAR      1003
#define ID_LOADING_ANIM      1004
#define ID_LOG_EDIT          1005
#define ID_STEAM_PATH_EDIT   1006
#define ID_BROWSE_BTN        1007
#define ID_AUTO_DETECT_BTN   1008
#define ID_CLEAR_LOG_BTN     1009

// ===================== Renk Tanımları =====================
#define COLOR_DARK_BG        RGB(20, 20, 25)
#define COLOR_DARK_PANEL     RGB(30, 30, 38)
#define COLOR_ACCENT         RGB(0, 150, 255)
#define COLOR_ACCENT_DARK    RGB(0, 100, 200)
#define COLOR_TEXT           RGB(220, 220, 230)
#define COLOR_TEXT_DIM       RGB(140, 140, 155)
#define COLOR_SUCCESS        RGB(0, 200, 100)
#define COLOR_ERROR          RGB(255, 80, 80)
#define COLOR_WARNING        RGB(255, 180, 50)

// ===================== Global Değişkenler =====================
HWND g_hMainWnd = NULL;
HWND g_hStatusStatic = NULL;
HWND g_hProgressBar = NULL;
HWND g_hLogEdit = NULL;
HWND g_hSteamPathEdit = NULL;
HINSTANCE g_hInstance = NULL;

// ===================== Loader Yapıları =====================
typedef struct {
    PBYTE baseAddress;
    HMODULE(WINAPI* loadLibraryA)(PCSTR);
    FARPROC(WINAPI* getProcAddress)(HMODULE, PCSTR);
    VOID(WINAPI* rtlZeroMemory)(PVOID, SIZE_T);
    DWORD imageBase;
    DWORD relocVirtualAddress;
    DWORD importVirtualAddress;
    DWORD addressOfEntryPoint;
} LoaderData;

// ===================== Fonksiyon Prototipleri =====================
DWORD WINAPI loadLibrary(LPVOID lpParam);
VOID stub(VOID);
VOID waitOnModule(DWORD processId, PCWSTR moduleName);
VOID killAnySteamProcess();
VOID AddLogMessage(LPCWSTR message, COLORREF color);
BOOL InjectVACBypass();
VOID UpdateStatus(LPCWSTR status, BOOL isProgress);
VOID ResetUI();

// ===================== Log Ekleme =====================
VOID AddLogMessage(LPCWSTR message, COLORREF color)
{
    if (!g_hLogEdit) return;

    CHARFORMAT2 cf;
    ZeroMemory(&cf, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR | CFM_BOLD;
    cf.crTextColor = color;

    SendMessage(g_hLogEdit, EM_SETSEL, -1, -1);
    SendMessage(g_hLogEdit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);

    // Zaman damgası ekle
    SYSTEMTIME st;
    GetLocalTime(&st);
    WCHAR timestamp[64];
    wsprintfW(timestamp, L"[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);

    SendMessage(g_hLogEdit, EM_REPLACESEL, FALSE, (LPARAM)timestamp);
    SendMessage(g_hLogEdit, EM_REPLACESEL, FALSE, (LPARAM)message);
    SendMessage(g_hLogEdit, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");

    // Scroll to bottom
    SendMessage(g_hLogEdit, EM_SCROLLCARET, 0, 0);
}

// ===================== Durum Güncelleme =====================
VOID UpdateStatus(LPCWSTR status, BOOL isProgress)
{
    if (g_hStatusStatic) {
        SetWindowTextW(g_hStatusStatic, status);
    }
    if (g_hProgressBar) {
        if (isProgress) {
            SendMessage(g_hProgressBar, PBM_SETSTATE, PBST_NORMAL, 0);
        } else {
            SendMessage(g_hProgressBar, PBM_SETSTATE, PBST_ERROR, 0);
        }
    }
}

// ===================== UI Sıfırlama =====================
VOID ResetUI()
{
    if (g_hProgressBar) {
        SendMessage(g_hProgressBar, PBM_SETPOS, 0, 0);
        SendMessage(g_hProgressBar, PBM_SETSTATE, PBST_NORMAL, 0);
    }
    UpdateStatus(L"Hazır", TRUE);
    EnableWindow(GetDlgItem(g_hMainWnd, ID_INJECT_BTN), TRUE);
}

// ===================== Steam Yolu Bulma =====================
BOOL FindSteamPath(WCHAR* buffer, DWORD bufferSize)
{
    HKEY key = NULL;
    if (!RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_QUERY_VALUE, &key)) {
        DWORD size = bufferSize - sizeof(WCHAR);
        if (!RegQueryValueExW(key, L"SteamExe", NULL, NULL, (LPBYTE)buffer, &size)) {
            RegCloseKey(key);
            return TRUE;
        }
        RegCloseKey(key);
    }
    return FALSE;
}

// ===================== Steam Süreçlerini Kapatma =====================
VOID killAnySteamProcess()
{
    HANDLE processSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (processSnapshot == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W processEntry;
    processEntry.dwSize = sizeof(processEntry);

    if (Process32FirstW(processSnapshot, &processEntry)) {
        PCWSTR steamProcesses[] = { L"Steam.exe", L"SteamService.exe", L"steamwebhelper.exe" };
        do {
            for (INT i = 0; i < _countof(steamProcesses); i++) {
                if (!lstrcmpiW(processEntry.szExeFile, steamProcesses[i])) {
                    HANDLE processHandle = OpenProcess(PROCESS_TERMINATE, FALSE, processEntry.th32ProcessID);
                    if (processHandle) {
                        TerminateProcess(processHandle, 0);
                        CloseHandle(processHandle);
                        AddLogMessage(L"Steam süreci kapatıldı", COLOR_WARNING);
                    }
                }
            }
        } while (Process32NextW(processSnapshot, &processEntry));
    }
    CloseHandle(processSnapshot);
}

// ===================== Modül Bekleme =====================
VOID waitOnModule(DWORD processId, PCWSTR moduleName)
{
    BOOL foundModule = FALSE;

    while (!foundModule) {
        HANDLE moduleSnapshot = INVALID_HANDLE_VALUE;

        while (moduleSnapshot == INVALID_HANDLE_VALUE)
            moduleSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, processId);

        MODULEENTRY32W moduleEntry;
        moduleEntry.dwSize = sizeof(moduleEntry);

        if (Module32FirstW(moduleSnapshot, &moduleEntry)) {
            do {
                if (!lstrcmpiW(moduleEntry.szModule, moduleName)) {
                    foundModule = TRUE;
                    break;
                }
            } while (Module32NextW(moduleSnapshot, &moduleEntry));
        }
        CloseHandle(moduleSnapshot);
        
        if (!foundModule) Sleep(100);
    }
}

// ===================== Loader Fonksiyonu =====================
DWORD WINAPI loadLibrary(LPVOID lpParam)
{
    LoaderData* loaderData = (LoaderData*)lpParam;

    PIMAGE_BASE_RELOCATION relocation = (PIMAGE_BASE_RELOCATION)(loaderData->baseAddress + loaderData->relocVirtualAddress);
    DWORD delta = (DWORD)(loaderData->baseAddress - loaderData->imageBase);
    while (relocation->VirtualAddress) {
        PWORD relocationInfo = (PWORD)(relocation + 1);
        for (int i = 0, count = (relocation->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD); i < count; i++)
            if (relocationInfo[i] >> 12 == IMAGE_REL_BASED_HIGHLOW)
                *(PDWORD)(loaderData->baseAddress + (relocation->VirtualAddress + (relocationInfo[i] & 0xFFF))) += delta;

        relocation = (PIMAGE_BASE_RELOCATION)((LPBYTE)relocation + relocation->SizeOfBlock);
    }

    PIMAGE_IMPORT_DESCRIPTOR importDirectory = (PIMAGE_IMPORT_DESCRIPTOR)(loaderData->baseAddress + loaderData->importVirtualAddress);

    while (importDirectory->Characteristics) {
        PIMAGE_THUNK_DATA originalFirstThunk = (PIMAGE_THUNK_DATA)(loaderData->baseAddress + importDirectory->OriginalFirstThunk);
        PIMAGE_THUNK_DATA firstThunk = (PIMAGE_THUNK_DATA)(loaderData->baseAddress + importDirectory->FirstThunk);

        HMODULE module = loaderData->loadLibraryA((LPCSTR)loaderData->baseAddress + importDirectory->Name);

        if (!module)
            return FALSE;

        while (originalFirstThunk->u1.AddressOfData) {
            DWORD Function = (DWORD)loaderData->getProcAddress(module, originalFirstThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG ? (LPCSTR)(originalFirstThunk->u1.Ordinal & 0xFFFF) : ((PIMAGE_IMPORT_BY_NAME)((LPBYTE)loaderData->baseAddress + originalFirstThunk->u1.AddressOfData))->Name);

            if (!Function)
                return FALSE;

            firstThunk->u1.Function = Function;
            originalFirstThunk++;
            firstThunk++;
        }
        importDirectory++;
    }

    if (loaderData->addressOfEntryPoint) {
        DWORD result = ((DWORD(__stdcall*)(HMODULE, DWORD, LPVOID))
            (loaderData->baseAddress + loaderData->addressOfEntryPoint))
            ((HMODULE)loaderData->baseAddress, DLL_PROCESS_ATTACH, NULL);

#if ERASE_ENTRY_POINT
        loaderData->rtlZeroMemory(loaderData->baseAddress + loaderData->addressOfEntryPoint, 32);
#endif

        return result;
    }
    return TRUE;
}

VOID stub(VOID) { }

// ===================== Enjeksiyon İşlemi =====================
BOOL InjectVACBypass()
{
    WCHAR steamPath[MAX_PATH];
    GetWindowTextW(g_hSteamPathEdit, steamPath, MAX_PATH);

    if (wcslen(steamPath) < 5) {
        AddLogMessage(L"HATA: Geçersiz Steam yolu!", COLOR_ERROR);
        UpdateStatus(L"Hata: Geçersiz yol", FALSE);
        return FALSE;
    }

    AddLogMessage(L"Steam başlatılıyor...", COLOR_ACCENT);
    UpdateStatus(L"Steam başlatılıyor...", TRUE);

    // İlerleme çubuğu
    SendMessage(g_hProgressBar, PBM_SETPOS, 10, 0);

    killAnySteamProcess();

    STARTUPINFOW info = { sizeof(info) };
    PROCESS_INFORMATION processInfo;

    WCHAR cmdLine[MAX_PATH + 64];
    wsprintfW(cmdLine, L"\"%s\"", steamPath);

    if (!CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &info, &processInfo)) {
        AddLogMessage(L"HATA: Steam başlatılamadı!", COLOR_ERROR);
        UpdateStatus(L"Hata: Steam başlatılamadı", FALSE);
        return FALSE;
    }

    SendMessage(g_hProgressBar, PBM_SETPOS, 30, 0);
    AddLogMessage(L"Steam başlatıldı, modül bekleniyor...", COLOR_ACCENT);
    UpdateStatus(L"Modül bekleniyor...", TRUE);

    waitOnModule(processInfo.dwProcessId, L"Steam.exe");
    
    SendMessage(g_hProgressBar, PBM_SETPOS, 50, 0);
    AddLogMessage(L"Steam.exe bulundu, işlem askıya alınıyor...", COLOR_ACCENT);
    UpdateStatus(L"İşlem askıya alınıyor...", TRUE);

    SuspendThread(processInfo.hThread);

    PIMAGE_NT_HEADERS ntHeaders = (PIMAGE_NT_HEADERS)(binary + ((PIMAGE_DOS_HEADER)binary)->e_lfanew);

    SendMessage(g_hProgressBar, PBM_SETPOS, 60, 0);
    AddLogMessage(L"Enjeksiyon hazırlanıyor...", COLOR_ACCENT);

    PBYTE executableImage = VirtualAllocEx(processInfo.hProcess, NULL, ntHeaders->OptionalHeader.SizeOfImage,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);

    PIMAGE_SECTION_HEADER sectionHeaders = (PIMAGE_SECTION_HEADER)(ntHeaders + 1);
    for (INT i = 0; i < ntHeaders->FileHeader.NumberOfSections; i++) {
        WriteProcessMemory(processInfo.hProcess, executableImage + sectionHeaders[i].VirtualAddress,
            binary + sectionHeaders[i].PointerToRawData, sectionHeaders[i].SizeOfRawData, NULL);
    }

    SendMessage(g_hProgressBar, PBM_SETPOS, 75, 0);
    AddLogMessage(L"Loader parametreleri hazırlanıyor...", COLOR_ACCENT);

    LoaderData* loaderMemory = VirtualAllocEx(processInfo.hProcess, NULL, 4096, MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READ);

    LoaderData loaderParams;
    loaderParams.baseAddress = executableImage;
    loaderParams.loadLibraryA = LoadLibraryA;
    loaderParams.getProcAddress = GetProcAddress;
    VOID(NTAPI* RtlZeroMemory)(VOID*, SIZE_T) = RtlZeroMemory;
    loaderParams.rtlZeroMemory = RtlZeroMemory;
    loaderParams.imageBase = ntHeaders->OptionalHeader.ImageBase;
    loaderParams.relocVirtualAddress = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress;
    loaderParams.importVirtualAddress = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    loaderParams.addressOfEntryPoint = ntHeaders->OptionalHeader.AddressOfEntryPoint;

    WriteProcessMemory(processInfo.hProcess, loaderMemory, &loaderParams, sizeof(LoaderData), NULL);
    WriteProcessMemory(processInfo.hProcess, (PBYTE)loaderMemory + 4096/2, loadLibrary,
        (DWORD)stub - (DWORD)loadLibrary, NULL);

    SendMessage(g_hProgressBar, PBM_SETPOS, 90, 0);
    AddLogMessage(L"Remote thread oluşturuluyor...", COLOR_ACCENT);

    HANDLE thread = CreateRemoteThread(processInfo.hProcess, NULL, 0, 
        (LPTHREAD_START_ROUTINE)((PBYTE)loaderMemory + 4096/2), loaderMemory, 0, NULL);

    ResumeThread(processInfo.hThread);
    WaitForSingleObject(thread, INFINITE);
    VirtualFreeEx(processInfo.hProcess, loaderMemory, 0, MEM_RELEASE);

    CloseHandle(processInfo.hProcess);
    CloseHandle(processInfo.hThread);

    SendMessage(g_hProgressBar, PBM_SETPOS, 100, 0);
    AddLogMessage(L"Enjeksiyon basariyla tamamlandi!", COLOR_SUCCESS);
    UpdateStatus(L"Enjeksiyon basarili!", TRUE);

    return TRUE;
}

// ===================== GUI Thread =====================
DWORD WINAPI InjectThread(LPVOID lpParam)
{
    EnableWindow(GetDlgItem(g_hMainWnd, ID_INJECT_BTN), FALSE);
    InjectVACBypass();
    EnableWindow(GetDlgItem(g_hMainWnd, ID_INJECT_BTN), TRUE);
    return 0;
}

// ===================== Pencere Prosedürü =====================
LRESULT CALLBACK WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg) {
    case WM_CTLCOLORSTATIC:
    {
        HDC hdcStatic = (HDC)wParam;
        SetBkMode(hdcStatic, TRANSPARENT);
        SetTextColor(hdcStatic, COLOR_TEXT);
        return (LRESULT)GetStockObject(NULL_BRUSH);
    }
    case WM_CTLCOLOREDIT:
    {
        HDC hdcEdit = (HDC)wParam;
        SetBkColor(hdcEdit, COLOR_DARK_PANEL);
        SetTextColor(hdcEdit, COLOR_TEXT);
        return (LRESULT)CreateSolidBrush(COLOR_DARK_PANEL);
    }
    case WM_COMMAND:
    {
        switch (LOWORD(wParam)) {
        case ID_INJECT_BTN:
        {
            AddLogMessage(L"--- Enjeksiyon baslatiliyor ---", COLOR_ACCENT);
            ResetUI();
            CreateThread(NULL, 0, InjectThread, NULL, 0, NULL);
            break;
        }
        case ID_BROWSE_BTN:
        {
            OPENFILENAMEW ofn = { 0 };
            WCHAR filePath[MAX_PATH] = L"";
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hWnd;
            ofn.lpstrFilter = L"Steam.exe\0Steam.exe\0Tum Dosyalar\0*.*\0";
            ofn.lpstrFile = filePath;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

            if (GetOpenFileNameW(&ofn)) {
                SetWindowTextW(g_hSteamPathEdit, filePath);
                AddLogMessage(L"Steam yolu secildi", COLOR_ACCENT);
            }
            break;
        }
        case ID_AUTO_DETECT_BTN:
        {
            WCHAR path[MAX_PATH];
            if (FindSteamPath(path, sizeof(path))) {
                SetWindowTextW(g_hSteamPathEdit, path);
                AddLogMessage(L"Steam otomatik olarak bulundu", COLOR_SUCCESS);
            } else {
                AddLogMessage(L"Steam bulunamadi, manuel secim yapin", COLOR_WARNING);
            }
            break;
        }
        case ID_CLEAR_LOG_BTN:
        {
            SetWindowTextW(g_hLogEdit, L"");
            break;
        }
        case IDCANCEL:
        {
            SendMessage(hWnd, WM_CLOSE, 0, 0);
            break;
        }
        }
        break;
    }
    case WM_CLOSE:
    {
        DestroyWindow(hWnd);
        break;
    }
    case WM_DESTROY:
    {
        PostQuitMessage(0);
        break;
    }
    default:
        return DefWindowProcW(hWnd, uMsg, wParam, lParam);
    }
    return 0;
}

// ===================== Ana Giriş =====================
INT WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, INT nShowCmd)
{
    g_hInstance = hInstance;

    // Init Common Controls
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    // Pencere sınıfı
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hbrBackground = CreateSolidBrush(COLOR_DARK_BG);
    wc.lpszClassName = L"XipLarexLoaderClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);

    if (!RegisterClassW(&wc)) {
        MessageBoxW(NULL, L"Pencere sinifi kaydedilemedi!", L"Hata", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Ana pencere
    g_hMainWnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        L"XipLarexLoaderClass",
        L"XipLarex VAC Bypass - Loader",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 540, 540,
        NULL, NULL, hInstance, NULL
    );

    if (!g_hMainWnd) {
        MessageBoxW(NULL, L"Pencere olusturulamadi!", L"Hata", MB_OK | MB_ICONERROR);
        return 1;
    }

    // ======== UI Elemanları ========
    // Logo / Başlık
    CreateWindowW(L"STATIC", L"XipLarex VAC Bypass", 
        WS_VISIBLE | WS_CHILD | SS_CENTER,
        10, 10, 500, 40, g_hMainWnd, NULL, hInstance, NULL);

    // Steam Yolu
    CreateWindowW(L"STATIC", L"Steam Yolu:", 
        WS_VISIBLE | WS_CHILD | SS_LEFT,
        10, 65, 80, 20, g_hMainWnd, NULL, hInstance, NULL);

    g_hSteamPathEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL,
        95, 63, 300, 24, g_hMainWnd, (HMENU)ID_STEAM_PATH_EDIT, hInstance, NULL);

    CreateWindowW(L"BUTTON", L"Gozat", 
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        400, 63, 60, 24, g_hMainWnd, (HMENU)ID_BROWSE_BTN, hInstance, NULL);

    CreateWindowW(L"BUTTON", L"Otomatik Bul", 
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        465, 63, 50, 24, g_hMainWnd, (HMENU)ID_AUTO_DETECT_BTN, hInstance, NULL);

    // Ayırıcı
    CreateWindowW(L"STATIC", L"", 
        WS_VISIBLE | WS_CHILD | SS_ETCHEDHORZ,
        10, 95, 500, 2, g_hMainWnd, NULL, hInstance, NULL);

    // Durum
    g_hStatusStatic = CreateWindowW(L"STATIC", L"Hazir", 
        WS_VISIBLE | WS_CHILD | SS_CENTER,
        10, 105, 500, 25, g_hMainWnd, (HMENU)ID_STATUS_STATIC, hInstance, NULL);

    // Progress Bar
    g_hProgressBar = CreateWindowW(PROGRESS_CLASSW, NULL,
        WS_VISIBLE | WS_CHILD | PBS_SMOOTH,
        10, 135, 500, 20, g_hMainWnd, (HMENU)ID_PROGRESS_BAR, hInstance, NULL);
    SendMessage(g_hProgressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessage(g_hProgressBar, PBM_SETSTEP, 1, 0);

    // Log
    CreateWindowW(L"STATIC", L"Log:", 
        WS_VISIBLE | WS_CHILD | SS_LEFT,
        10, 165, 40, 20, g_hMainWnd, NULL, hInstance, NULL);

    CreateWindowW(L"BUTTON", L"Temizle", 
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        470, 163, 50, 20, g_hMainWnd, (HMENU)ID_CLEAR_LOG_BTN, hInstance, NULL);

    g_hLogEdit = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        RICHEDIT_CLASSW, L"",
        WS_VISIBLE | WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_NOHIDESEL,
        10, 188, 500, 210,
        g_hMainWnd, (HMENU)ID_LOG_EDIT, hInstance, NULL
    );

    // RichEdit formatını ayarla
    CHARFORMAT2 cf;
    ZeroMemory(&cf, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_FACE | CFM_SIZE | CFM_BOLD;
    cf.dwEffects = CFE_BOLD;
    cf.yHeight = 200;
    wcscpy_s(cf.szFaceName, _countof(cf.szFaceName), L"Consolas");
    SendMessage(g_hLogEdit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);

    // Butonlar
    CreateWindowW(L"BUTTON", L"Enjekte Et", 
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        10, 415, 150, 40, g_hMainWnd, (HMENU)ID_INJECT_BTN, hInstance, NULL);

    CreateWindowW(L"BUTTON", L"Cikis", 
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        360, 415, 150, 40, g_hMainWnd, (HMENU)IDCANCEL, hInstance, NULL);

    // Başlangıç log mesajı
    AddLogMessage(L"XipLarex VAC Bypass - Loader baslatildi", COLOR_ACCENT);
    AddLogMessage(L"Steam yolunu secin ve 'Enjekte Et' butonuna tiklayin", COLOR_TEXT_DIM);

    // Otomatik Steam bul
    WCHAR autoPath[MAX_PATH];
    if (FindSteamPath(autoPath, sizeof(autoPath))) {
        SetWindowTextW(g_hSteamPathEdit, autoPath);
        AddLogMessage(L"Steam otomatik olarak bulundu", COLOR_SUCCESS);
    }

    ShowWindow(g_hMainWnd, nShowCmd);
    UpdateWindow(g_hMainWnd);

    // Mesaj döngüsü
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (INT)msg.wParam;
}