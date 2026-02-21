// HyperGrid Defender - C Host Application
// Build: clang "@game.response"

#include <windows.h>
#include <objbase.h>
#include <objidl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "WebView2.h"


// Forward declarations
// Helper: Debug Log
static void DebugLog(const char* fmt, ...) {
    FILE* f = NULL;
    if (fopen_s(&f, "game.log", "a") == 0 && f) {
        va_list args;
        va_start(args, fmt);
        vfprintf(f, fmt, args);
        va_end(args);
        fprintf(f, "\n");
        fclose(f);
    }
}

static void ResizeWebView(void);
static void ShowHr(const wchar_t* where, HRESULT hr);

// Global state
static HWND g_hwnd = NULL;
static ICoreWebView2Controller* g_controller = NULL;
static ICoreWebView2* g_webview = NULL;
static EventRegistrationToken g_msg_token = {0};

// Add global for cursor state
static BOOL g_cursorLocked = FALSE;

// Add helper function
static void SetCursorLock(BOOL lock) {
    if (lock && !g_cursorLocked) {
        RECT clip;
        GetClientRect(g_hwnd, &clip);
        ClientToScreen(g_hwnd, (POINT*)&clip.left);
        ClientToScreen(g_hwnd, (POINT*)&clip.right);
        ClipCursor(&clip);
        g_cursorLocked = TRUE;
        ShowCursor(FALSE); // Hide system cursor
    } else if (!lock && g_cursorLocked) {
        ClipCursor(NULL);
        g_cursorLocked = FALSE;
        ShowCursor(TRUE); // Show system cursor
    }
}

// Helper: Load HTML from Resource
static wchar_t* LoadGameHtmlResource(void) {
    // RT_HTML is 23
    HRSRC hRes = FindResourceW(NULL, L"GAME_HTML", MAKEINTRESOURCEW(23));
    if (!hRes) {
        DebugLog("FindResourceW failed: 0x%08lx", GetLastError());
        return NULL;
    }

    HGLOBAL hData = LoadResource(NULL, hRes);
    if (!hData) {
        DebugLog("LoadResource failed: 0x%08lx", GetLastError());
        return NULL;
    }

    DWORD size = SizeofResource(NULL, hRes);
    const char* data = (const char*)LockResource(hData);
    if (!data || size == 0) {
        DebugLog("LockResource failed or size is 0");
        return NULL;
    }
    DebugLog("Resource found, size: %lu", size);

    // Convert UTF-8 to Wide String
    int wlen = MultiByteToWideChar(CP_UTF8, 0, data, (int)size, NULL, 0);
    if (wlen <= 0) {
        DebugLog("MultiByteToWideChar (calc) failed: 0x%08lx", GetLastError());
        return NULL;
    }

    wchar_t* wbuf = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, (wlen + 1) * sizeof(wchar_t));
    if (!wbuf) {
        DebugLog("HeapAlloc failed");
        return NULL;
    }

    MultiByteToWideChar(CP_UTF8, 0, data, (int)size, wbuf, wlen);
    wbuf[wlen] = L'\0';

    return wbuf;
}

// -------------------------------------------------------------------------
// ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler
// -------------------------------------------------------------------------

// Check ICoreWebView2Settings3 for AcceleratorKeys
static void ApplyLockdownSettings(ICoreWebView2Settings* settings) {
    if (!settings) return;

    // Standard settings
    ICoreWebView2Settings_put_IsScriptEnabled(settings, TRUE);
    ICoreWebView2Settings_put_IsWebMessageEnabled(settings, TRUE);
    ICoreWebView2Settings_put_AreDefaultScriptDialogsEnabled(settings, FALSE); // Draw our own UI
    ICoreWebView2Settings_put_IsStatusBarEnabled(settings, FALSE);
    ICoreWebView2Settings_put_AreDevToolsEnabled(settings, FALSE);
    ICoreWebView2Settings_put_AreDefaultContextMenusEnabled(settings, FALSE);
    ICoreWebView2Settings_put_IsZoomControlEnabled(settings, FALSE);
    ICoreWebView2Settings_put_IsBuiltInErrorPageEnabled(settings, FALSE);

    // Advanced settings (Settings3 for accelerator keys)
    ICoreWebView2Settings3* settings3 = NULL;
    if (SUCCEEDED(ICoreWebView2Settings_QueryInterface(settings, &IID_ICoreWebView2Settings3, (void**)&settings3))) {
        ICoreWebView2Settings3_put_AreBrowserAcceleratorKeysEnabled(settings3, FALSE);
        ICoreWebView2Settings3_Release(settings3);
    }
}

// Handler for WebMessageReceived (JS -> C)
typedef struct WebMessageHandler {
    ICoreWebView2WebMessageReceivedEventHandler iface;
    volatile LONG ref;
} WebMessageHandler;

static HRESULT STDMETHODCALLTYPE WebMessageHandler_QueryInterface(
    ICoreWebView2WebMessageReceivedEventHandler* This, REFIID riid, void** ppvObject
) {
    if (!ppvObject) return E_POINTER;
    *ppvObject = NULL;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ICoreWebView2WebMessageReceivedEventHandler)) {
        *ppvObject = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE WebMessageHandler_AddRef(ICoreWebView2WebMessageReceivedEventHandler* This) {
    WebMessageHandler* self = (WebMessageHandler*)This;
    return (ULONG)InterlockedIncrement(&self->ref);
}

static ULONG STDMETHODCALLTYPE WebMessageHandler_Release(ICoreWebView2WebMessageReceivedEventHandler* This) {
    WebMessageHandler* self = (WebMessageHandler*)This;
    ULONG refs = (ULONG)InterlockedDecrement(&self->ref);
    if (refs == 0) HeapFree(GetProcessHeap(), 0, self);
    return refs;
}

static HRESULT STDMETHODCALLTYPE WebMessageHandler_Invoke(
    ICoreWebView2WebMessageReceivedEventHandler* This,
    ICoreWebView2* sender,
    ICoreWebView2WebMessageReceivedEventArgs* args
) {
    (void)This; (void)sender;
    LPWSTR msg = NULL;
    if (SUCCEEDED(ICoreWebView2WebMessageReceivedEventArgs_TryGetWebMessageAsString(args, &msg)) && msg) {
        if (wcscmp(msg, L"close") == 0) {
            SetCursorLock(FALSE); // Release cursor before closing
            PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        }
        // ✅ NEW: Cursor lock/unlock messages
        else if (wcscmp(msg, L"cursor_lock") == 0) {
            SetCursorLock(TRUE);
        }
        else if (wcscmp(msg, L"cursor_unlock") == 0) {
            SetCursorLock(FALSE);
        }
        CoTaskMemFree(msg);
    }
    return S_OK;
}

static ICoreWebView2WebMessageReceivedEventHandlerVtbl g_webmsg_vtbl = {
    WebMessageHandler_QueryInterface,
    WebMessageHandler_AddRef,
    WebMessageHandler_Release,
    WebMessageHandler_Invoke,
};

static WebMessageHandler* WebMessageHandler_Create(void) {
    WebMessageHandler* self = (WebMessageHandler*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*self));
    if (self) {
        self->iface.lpVtbl = &g_webmsg_vtbl;
        self->ref = 1;
    }
    return self;
}

// Handler for ControllerCreated
typedef struct ControllerCreatedHandler {
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler iface;
    volatile LONG ref;
} ControllerCreatedHandler;

static HRESULT STDMETHODCALLTYPE ControllerCreatedHandler_QueryInterface(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler* This, REFIID riid, void** ppvObject
) {
    if (!ppvObject) return E_POINTER;
    *ppvObject = NULL;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)) {
        *ppvObject = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE ControllerCreatedHandler_AddRef(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler* This) {
    (void)This;
    ControllerCreatedHandler* self = (ControllerCreatedHandler*)This;
    return (ULONG)InterlockedIncrement(&self->ref);
}

static ULONG STDMETHODCALLTYPE ControllerCreatedHandler_Release(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler* This) {
    ControllerCreatedHandler* self = (ControllerCreatedHandler*)This;
    ULONG refs = (ULONG)InterlockedDecrement(&self->ref);
    if (refs == 0) HeapFree(GetProcessHeap(), 0, self);
    return refs;
}

static HRESULT STDMETHODCALLTYPE ControllerCreatedHandler_Invoke(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler* This,
    HRESULT errorCode,
    ICoreWebView2Controller* createdController
) {
    (void)This;
    DebugLog("ControllerCreatedHandler_Invoke called");
    if (FAILED(errorCode) || !createdController) {
        ShowHr(L"CreateController", errorCode);
        return S_OK;
    }

    g_controller = createdController;
    ICoreWebView2Controller_AddRef(g_controller);

    ICoreWebView2Controller_get_CoreWebView2(g_controller, &g_webview);
    if (!g_webview) return S_OK;

    // Setup bounds
    ResizeWebView();

    // Lockdown
    ICoreWebView2Settings* settings = NULL;
    if (SUCCEEDED(ICoreWebView2_get_Settings(g_webview, &settings))) {
        ApplyLockdownSettings(settings);
        ICoreWebView2Settings_Release(settings);
    }
    
    // Add WebMessage listener
    WebMessageHandler* msgHandler = WebMessageHandler_Create();
    if (msgHandler) {
        ICoreWebView2_add_WebMessageReceived(g_webview, (ICoreWebView2WebMessageReceivedEventHandler*)msgHandler, &g_msg_token);
        ICoreWebView2WebMessageReceivedEventHandler_Release((ICoreWebView2WebMessageReceivedEventHandler*)msgHandler);
    }

    // Navigate to embedded game.html
    wchar_t* htmlContent = LoadGameHtmlResource();
    if (htmlContent) {
        ICoreWebView2_NavigateToString(g_webview, htmlContent);
        HeapFree(GetProcessHeap(), 0, htmlContent);
    } else {
        MessageBoxW(NULL, L"Could not load embedded game.html resource", L"Error", MB_OK);
    }

    return S_OK;
}

static ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl g_controller_created_vtbl = {
    ControllerCreatedHandler_QueryInterface,
    ControllerCreatedHandler_AddRef,
    ControllerCreatedHandler_Release,
    ControllerCreatedHandler_Invoke,
};

static ControllerCreatedHandler* ControllerCreatedHandler_Create(void) {
    ControllerCreatedHandler* self = (ControllerCreatedHandler*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*self));
    if (self) {
        self->iface.lpVtbl = &g_controller_created_vtbl;
        self->ref = 1;
    }
    return self;
}

// Handler for EnvironmentCreated
typedef struct EnvCreatedHandler {
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler iface;
    volatile LONG ref;
} EnvCreatedHandler;

static HRESULT STDMETHODCALLTYPE EnvCreatedHandler_QueryInterface(
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* This, REFIID riid, void** ppvObject
) {
    if (!ppvObject) return E_POINTER;
    *ppvObject = NULL;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)) {
        *ppvObject = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE EnvCreatedHandler_AddRef(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* This) {
    (void)This;
    EnvCreatedHandler* self = (EnvCreatedHandler*)This;
    return (ULONG)InterlockedIncrement(&self->ref);
}

static ULONG STDMETHODCALLTYPE EnvCreatedHandler_Release(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* This) {
    EnvCreatedHandler* self = (EnvCreatedHandler*)This;
    ULONG refs = (ULONG)InterlockedDecrement(&self->ref);
    if (refs == 0) HeapFree(GetProcessHeap(), 0, self);
    return refs;
}

static HRESULT STDMETHODCALLTYPE EnvCreatedHandler_Invoke(
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* This,
    HRESULT errorCode,
    ICoreWebView2Environment* createdEnvironment
) {
    (void)This;
    DebugLog("EnvCreatedHandler_Invoke called");
    if (FAILED(errorCode) || !createdEnvironment) {
        ShowHr(L"CreateEnvironment", errorCode);
        return S_OK;
    }

    ControllerCreatedHandler* handler = ControllerCreatedHandler_Create();
    if (handler) {
        ICoreWebView2Environment_CreateCoreWebView2Controller(
            createdEnvironment, g_hwnd, (ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*)handler
        );
        ICoreWebView2CreateCoreWebView2ControllerCompletedHandler_Release((ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*)handler);
    }
    return S_OK;
}

static ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl g_env_created_vtbl = {
    EnvCreatedHandler_QueryInterface,
    EnvCreatedHandler_AddRef,
    EnvCreatedHandler_Release,
    EnvCreatedHandler_Invoke,
};

static EnvCreatedHandler* EnvCreatedHandler_Create(void) {
    EnvCreatedHandler* self = (EnvCreatedHandler*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*self));
    if (self) {
        self->iface.lpVtbl = &g_env_created_vtbl;
        self->ref = 1;
    }
    return self;
}

// -------------------------------------------------------------------------
// Window Procedure
// -------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        ResizeWebView();
        return 0;
    case WM_DESTROY:
        SetCursorLock(FALSE); // ✅ Release cursor on exit
        if (g_webview) {
            ICoreWebView2_remove_WebMessageReceived(g_webview, g_msg_token);
            ICoreWebView2_Release(g_webview);
            g_webview = NULL;
        }
        if (g_controller) {
            ICoreWebView2Controller_Release(g_controller);
            g_controller = NULL;
        }
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            SetCursorLock(FALSE); // ✅ Release cursor before closing
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

static void ResizeWebView(void) {
    if (!g_hwnd || !g_controller) return;
    RECT bounds;
    GetClientRect(g_hwnd, &bounds);
    ICoreWebView2Controller_put_Bounds(g_controller, bounds);
}

static void ShowHr(const wchar_t* where, HRESULT hr) {
    wchar_t msg[256];
    swprintf_s(msg, 256, L"%s failed (0x%08lx)", where, hr);
    MessageBoxW(NULL, msg, L"Error", MB_OK | MB_ICONERROR);
}

// -------------------------------------------------------------------------
// Entry Point
// -------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance; (void)lpCmdLine;

    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) {
        DebugLog("CoInitializeEx failed");
        return 1;
    }
    DebugLog("Started");

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"HybridGameHost";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    // Fullscreen: user expects game to take over
    int w = GetSystemMetrics(SM_CXSCREEN);
    int h = GetSystemMetrics(SM_CYSCREEN);

    g_hwnd = CreateWindowExW(
        0,
        L"HybridGameHost", 
        L"HyperGrid Defender", 
        WS_POPUP | WS_VISIBLE, // Popup = no border/titlebar
        0, 0, w, h,
        NULL, NULL, hInstance, NULL
    );

    if (!g_hwnd) return 1;

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);
    DebugLog("Window shown");

    EnvCreatedHandler* handler = EnvCreatedHandler_Create();
    if (handler) {
        DebugLog("Creating WebView2 Environment...");
        HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
            NULL, NULL, NULL, (ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*)handler
        );
        if (FAILED(hr)) {
             DebugLog("CreateCoreWebView2EnvironmentWithOptions failed: 0x%08lx", hr);
             ShowHr(L"CreateEnvironment", hr);
        }
        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler_Release((ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*)handler);
    } else {
        DebugLog("Failed to create EnvHandler");
    }

    MSG msg;
    DebugLog("Entering message loop");
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return (int)msg.wParam;
}
