#include <windows.h>
#include <windowsx.h>
#include <shobjidl.h>
#include <dwmapi.h>
#include <string>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdi32.lib")

HWND g_hwnd = NULL;
IExplorerBrowser* g_pBrowser = NULL;
int g_alpha = 180;
HHOOK g_hook = NULL;
const int EDGE_WIDTH = 20; // 20像素的边缘隐形控制区

LRESULT CALLBACK MsgHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code >= 0 && wParam == PM_REMOVE) {
        MSG* msg = (MSG*)lParam;
        if (msg->message == WM_MOUSEWHEEL) {
            if (msg->hwnd == g_hwnd || IsChild(g_hwnd, msg->hwnd)) {
                POINT pt = msg->pt; 
                RECT rc; GetWindowRect(g_hwnd, &rc);
                
                // 如果鼠标在边缘的隐形控制区内滚动，则调节透明度
                if (pt.x < rc.left + EDGE_WIDTH || pt.x > rc.right - EDGE_WIDTH ||
                    pt.y < rc.top + EDGE_WIDTH || pt.y > rc.bottom - EDGE_WIDTH) {
                    
                    int delta = GET_WHEEL_DELTA_WPARAM(msg->wParam);
                    g_alpha += (delta > 0) ? 15 : -15;
                    if (g_alpha < 20) g_alpha = 20;
                    if (g_alpha > 255) g_alpha = 255;
                    SetLayeredWindowAttributes(g_hwnd, 0, g_alpha, LWA_ALPHA);
                    
                    msg->message = WM_NULL; // 拦截信号，防止里面的文件夹跟着滚动
                }
            }
        }
    }
    return CallNextHookEx(g_hook, code, wParam, lParam);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        // 铺一层深色半透明底，防止纯透明区域无法点击
        HBRUSH hBrush = CreateSolidBrush(RGB(20, 20, 20));
        FillRect(hdc, &rc, hBrush);
        DeleteObject(hBrush);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_NCHITTEST: {
        POINT pt;
        pt.x = GET_X_LPARAM(lParam);
        pt.y = GET_Y_LPARAM(lParam);
        RECT rc;
        GetWindowRect(hwnd, &rc);
        // 把边缘区域伪装成标题栏，实现纯鼠标左键拖拽
        if (pt.x < rc.left + EDGE_WIDTH || pt.x > rc.right - EDGE_WIDTH ||
            pt.y < rc.top + EDGE_WIDTH || pt.y > rc.bottom - EDGE_WIDTH) {
            return HTCAPTION;
        }
        return HTCLIENT;
    }
    case WM_NCRBUTTONUP: {
        // 在边缘隐形控制区点击右键，呼出关闭提示
        if (wParam == HTCAPTION) {
            if (MessageBoxW(hwnd, L"Close this mapped folder?", L"DeskManage", MB_YESNO | MB_ICONINFORMATION) == IDYES) {
                PostMessage(hwnd, WM_CLOSE, 0, 0);
            }
            return 0;
        }
        break;
    }
    case WM_WINDOWPOSCHANGING: {
        // 强制桌面焊死机制：拦截任何试图把窗口推向顶层的操作，永远置底
        WINDOWPOS* wp = (WINDOWPOS*)lParam;
        wp->hwndInsertAfter = HWND_BOTTOM;
        wp->flags &= ~SWP_NOZORDER;
        return 0;
    }
    case WM_SIZE: {
        if (g_pBrowser) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            // 给浏览器控件四周留出几个像素，防止它完全盖住隐形控制区
            rc.left += 2; rc.right -= 2; rc.top += 2; rc.bottom -= 2;
            g_pBrowser->SetRect(NULL, rc);
        }
        return 0;
    }
    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        if (g_pBrowser) {
            g_pBrowser->Destroy();
            g_pBrowser->Release();
        }
        return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

std::wstring PickFolder() {
    std::wstring result = L"";
    IFileOpenDialog* pfd = NULL;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd)))) {
        DWORD dwOptions;
        if (SUCCEEDED(pfd->GetOptions(&dwOptions))) {
            pfd->SetOptions(dwOptions | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        }
        if (SUCCEEDED(pfd->Show(NULL))) {
            IShellItem* psi = NULL;
            if (SUCCEEDED(pfd->GetResult(&psi))) {
                PWSTR pszPath = NULL;
                if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath))) {
                    result = pszPath;
                    CoTaskMemFree(pszPath);
                }
                psi->Release();
            }
        }
        pfd->Release();
    }
    return result;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    SetProcessDPIAware();
    if (FAILED(OleInitialize(NULL))) return -1;

    std::wstring folderPath = PickFolder();
    if (folderPath.empty()) {
        OleUninitialize();
        return 0; // 不选文件夹直接静默退出
    }

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"PureDeskMapClass";
    RegisterClassW(&wc);

    g_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW, 
        L"PureDeskMapClass", L"DeskMap", 
        WS_POPUP | WS_CLIPCHILDREN, 
        200, 200, 600, 450, NULL, NULL, hInstance, NULL);

    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_hwnd, 20, &dark, sizeof(dark));
    SetLayeredWindowAttributes(g_hwnd, 0, g_alpha, LWA_ALPHA);

    if (SUCCEEDED(CoCreateInstance(CLSID_ExplorerBrowser, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_pBrowser)))) {
        RECT rc = { 0, 0, 600, 450 };
        FOLDERSETTINGS fs = { 0 };
        fs.ViewMode = FVM_ICON;
        // 彻底隐藏所有外观元素
        fs.fFlags = FWF_AUTOARRANGE | FWF_NOWEBVIEW | FWF_HIDEFILENAMES | FWF_TRANSPARENT;

        g_pBrowser->Initialize(g_hwnd, &rc, &fs);
        g_pBrowser->SetOptions(EBO_NOBORDER);

        PIDLIST_ABSOLUTE pidl = NULL;
        if (SUCCEEDED(SHParseDisplayName(folderPath.c_str(), NULL, &pidl, 0, NULL))) {
            g_pBrowser->BrowseToIDList(pidl, SBSP_ABSOLUTE);
            CoTaskMemFree(pidl);
        }
    }

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
    
    // 初始化时直接垫底
    SetWindowPos(g_hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    g_hook = SetWindowsHookExW(WH_GETMESSAGE, MsgHook, NULL, GetCurrentThreadId());

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_hook) UnhookWindowsHookEx(g_hook);
    OleUninitialize();
    return 0;
}
