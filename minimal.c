/*
    Minimal WebView2 (C) example.
    Build (PowerShell): clang "@minimal.response"


error in "WebView2.h"
    -Wno-extra-semi

unused code/data elimination helpers:
    -ffunction-sections,
    -fdata-sections

change stdlib interpretation:
    -ffreestanding
    -fno-builtin
    -nostdlib

*/

#include <windows.h>
#include <objbase.h>
#include <wchar.h>

#include "WebView2.h"

static HWND g_hwnd = NULL;
static ICoreWebView2Controller* g_controller = NULL;
static ICoreWebView2* g_webview = NULL;

static EventRegistrationToken g_msg_token = {0};
static BOOL g_msg_token_valid = FALSE;

static int CalculateSum(int a, int b) {
    return a + b;
}

static void ProcessData(const wchar_t* input, wchar_t* output, size_t outputSize) {
    if (!input) {
        input = L"";
    }
    (void)swprintf(output, outputSize, L"Processed: %s (from C!)", input);
}

static void ShowHr(const wchar_t* where, HRESULT hr) {
    wchar_t msg[512];
    (void)swprintf(msg, _countof(msg), L"%s failed (HRESULT=0x%08lx)", where, (unsigned long)hr);
    MessageBoxW(NULL, msg, L"WebView2", MB_OK | MB_ICONERROR);
}

static void ResizeWebView(void) {
    if (!g_hwnd || !g_controller) {
        return;
    }

    RECT bounds;
    GetClientRect(g_hwnd, &bounds);
    (void)ICoreWebView2Controller_put_Bounds(g_controller, bounds);
}

typedef struct WebMessageHandler {
    ICoreWebView2WebMessageReceivedEventHandler iface;
    volatile LONG ref;
} WebMessageHandler;

static HRESULT STDMETHODCALLTYPE WebMessageHandler_QueryInterface(
    ICoreWebView2WebMessageReceivedEventHandler* This, REFIID riid, void** ppvObject
) {
    if (!ppvObject) {
        return E_POINTER;
    }
    *ppvObject = NULL;

    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_ICoreWebView2WebMessageReceivedEventHandler)) {
        *ppvObject = This;
        ICoreWebView2WebMessageReceivedEventHandler_AddRef(This);
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
    if (refs == 0) {
        HeapFree(GetProcessHeap(), 0, self);
    }
    return refs;
}

static void PostToJs(const wchar_t* msg) {
    if (!g_webview || !msg) {
        return;
    }
    (void)ICoreWebView2_PostWebMessageAsString(g_webview, msg);
}

static HRESULT STDMETHODCALLTYPE WebMessageHandler_Invoke(
    ICoreWebView2WebMessageReceivedEventHandler* This,
    ICoreWebView2* sender,
    ICoreWebView2WebMessageReceivedEventArgs* args
) {
    (void)This;
    (void)sender;

    if (!args) {
        return S_OK;
    }

    LPWSTR msg = NULL;
    HRESULT hr = ICoreWebView2WebMessageReceivedEventArgs_TryGetWebMessageAsString(args, &msg);
    if (FAILED(hr) || !msg) {
        return S_OK;
    }

    int a = 0, b = 0;
    if (swscanf_s(msg, L"sum %d %d", &a, &b) == 2) {
        wchar_t out[64];
        (void)swprintf(out, _countof(out), L"%d", CalculateSum(a, b));
        PostToJs(out);
    } else if (wcsncmp(msg, L"process ", 8) == 0) {
        wchar_t processed[512];
        ProcessData(msg + 8, processed, _countof(processed));
        PostToJs(processed);
    } else {
        PostToJs(L"Unknown command. Try: 'sum 5 3' or 'process hello'");
    }

    CoTaskMemFree(msg);
    return S_OK;
}

static ICoreWebView2WebMessageReceivedEventHandlerVtbl g_webmsg_vtbl = {
    WebMessageHandler_QueryInterface,
    WebMessageHandler_AddRef,
    WebMessageHandler_Release,
    WebMessageHandler_Invoke,
};

static WebMessageHandler* WebMessageHandler_Create(void) {
    WebMessageHandler* self =
        (WebMessageHandler*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*self));
    if (!self) {
        return NULL;
    }
    self->iface.lpVtbl = &g_webmsg_vtbl;
    self->ref = 1;
    return self;
}

typedef struct ControllerCreatedHandler {
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler iface;
    volatile LONG ref;
} ControllerCreatedHandler;

static HRESULT STDMETHODCALLTYPE ControllerCreatedHandler_QueryInterface(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler* This, REFIID riid, void** ppvObject
) {
    if (!ppvObject) {
        return E_POINTER;
    }
    *ppvObject = NULL;

    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)) {
        *ppvObject = This;
        ICoreWebView2CreateCoreWebView2ControllerCompletedHandler_AddRef(This);
        return S_OK;
    }

    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE ControllerCreatedHandler_AddRef(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler* This
) {
    ControllerCreatedHandler* self = (ControllerCreatedHandler*)This;
    return (ULONG)InterlockedIncrement(&self->ref);
}

static ULONG STDMETHODCALLTYPE ControllerCreatedHandler_Release(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler* This
) {
    ControllerCreatedHandler* self = (ControllerCreatedHandler*)This;
    ULONG refs = (ULONG)InterlockedDecrement(&self->ref);
    if (refs == 0) {
        HeapFree(GetProcessHeap(), 0, self);
    }
    return refs;
}

static HRESULT STDMETHODCALLTYPE ControllerCreatedHandler_Invoke(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler* This,
    HRESULT errorCode,
    ICoreWebView2Controller* createdController
) {
    if (FAILED(errorCode) || !createdController) {
        ShowHr(L"CreateCoreWebView2Controller", errorCode);
        ICoreWebView2CreateCoreWebView2ControllerCompletedHandler_Release(This);
        return errorCode;
    }

    if (g_controller) {
        ICoreWebView2Controller_Release(g_controller);
        g_controller = NULL;
    }

    g_controller = createdController;
    ICoreWebView2Controller_AddRef(g_controller);

    if (g_webview) {
        ICoreWebView2_Release(g_webview);
        g_webview = NULL;
    }

    HRESULT hr = ICoreWebView2Controller_get_CoreWebView2(g_controller, &g_webview);
    if (FAILED(hr) || !g_webview) {
        ShowHr(L"get_CoreWebView2", hr);
        ICoreWebView2CreateCoreWebView2ControllerCompletedHandler_Release(This);
        return hr;
    }

    ResizeWebView();

    WebMessageHandler* handler = WebMessageHandler_Create();
    if (!handler) {
        ICoreWebView2CreateCoreWebView2ControllerCompletedHandler_Release(This);
        return E_OUTOFMEMORY;
    }

    HRESULT hr2 = ICoreWebView2_add_WebMessageReceived(
        g_webview, (ICoreWebView2WebMessageReceivedEventHandler*)handler, &g_msg_token
    );
    // WebView2 holds its own ref to the event handler; drop ours.
    ICoreWebView2WebMessageReceivedEventHandler_Release((ICoreWebView2WebMessageReceivedEventHandler*)handler);
    if (FAILED(hr2)) {
        ShowHr(L"add_WebMessageReceived", hr2);
        ICoreWebView2CreateCoreWebView2ControllerCompletedHandler_Release(This);
        return hr2;
    }
    g_msg_token_valid = TRUE;

    // JS->native messages are plain strings:
    //   "sum <a> <b>"
    //   "process <text>"
    const wchar_t* html =
        L"<!doctype html>"
        L"<html><head><meta charset='utf-8'/>"
        L"<style>"
        L"body{background:#202020;color:#fff;font-family:'Segoe UI',sans-serif;margin:40px}"
        L"button{background:#0078d4;color:#fff;border:0;padding:10px 20px;font-size:14px;cursor:pointer;"
        L"border-radius:4px;margin:5px}"
        L"button:hover{background:#106ebe}"
        L"input{background:#2d2d2d;color:#fff;border:1px solid #3e3e3e;padding:8px;font-size:14px;"
        L"border-radius:4px;margin:5px}"
        L"#output{background:#2d2d2d;padding:15px;border-radius:4px;margin-top:20px;min-height:60px;"
        L"white-space:pre-wrap}"
        L"</style></head><body>"
        L"<h2>WebView2 + C Logic (Minimal)</h2>"
        L"<div>"
        L"<input id='input1' type='number' value='5'/>"
        L"<input id='input2' type='number' value='3'/>"
        L"<button onclick='callSum()'>Sum</button>"
        L"</div>"
        L"<div>"
        L"<input id='textInput' type='text' value='Hello from JavaScript'/>"
        L"<button onclick='callProcess()'>Process</button>"
        L"</div>"
        L"<div id='output'>Results appear here...</div>"
        L"<script>"
        L"const out=document.getElementById('output');"
        L"window.chrome.webview.addEventListener('message', e=>{ out.textContent='C says: '+e.data; });"
        L"function callSum(){"
        L"  const a=parseInt(document.getElementById('input1').value,10)||0;"
        L"  const b=parseInt(document.getElementById('input2').value,10)||0;"
        L"  window.chrome.webview.postMessage(`sum ${a} ${b}`);"
        L"}"
        L"function callProcess(){"
        L"  const t=document.getElementById('textInput').value||'';"
        L"  window.chrome.webview.postMessage('process '+t);"
        L"}"
        L"</script>"
        L"</body></html>";

    HRESULT hr3 = ICoreWebView2_NavigateToString(g_webview, html);
    if (FAILED(hr3)) {
        ShowHr(L"NavigateToString", hr3);
    }

    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler_Release(This);
    return S_OK;
}

static ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl g_controller_created_vtbl = {
    ControllerCreatedHandler_QueryInterface,
    ControllerCreatedHandler_AddRef,
    ControllerCreatedHandler_Release,
    ControllerCreatedHandler_Invoke,
};

static ControllerCreatedHandler* ControllerCreatedHandler_Create(void) {
    ControllerCreatedHandler* self =
        (ControllerCreatedHandler*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*self));
    if (!self) {
        return NULL;
    }
    self->iface.lpVtbl = &g_controller_created_vtbl;
    self->ref = 1;
    return self;
}

typedef struct EnvCreatedHandler {
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler iface;
    volatile LONG ref;
} EnvCreatedHandler;

static HRESULT STDMETHODCALLTYPE EnvCreatedHandler_QueryInterface(
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* This, REFIID riid, void** ppvObject
) {
    if (!ppvObject) {
        return E_POINTER;
    }
    *ppvObject = NULL;

    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)) {
        *ppvObject = This;
        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler_AddRef(This);
        return S_OK;
    }

    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE EnvCreatedHandler_AddRef(
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* This
) {
    EnvCreatedHandler* self = (EnvCreatedHandler*)This;
    return (ULONG)InterlockedIncrement(&self->ref);
}

static ULONG STDMETHODCALLTYPE EnvCreatedHandler_Release(
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* This
) {
    EnvCreatedHandler* self = (EnvCreatedHandler*)This;
    ULONG refs = (ULONG)InterlockedDecrement(&self->ref);
    if (refs == 0) {
        HeapFree(GetProcessHeap(), 0, self);
    }
    return refs;
}

static HRESULT STDMETHODCALLTYPE EnvCreatedHandler_Invoke(
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* This,
    HRESULT errorCode,
    ICoreWebView2Environment* createdEnvironment
) {
    if (FAILED(errorCode) || !createdEnvironment) {
        ShowHr(L"CreateCoreWebView2EnvironmentWithOptions", errorCode);
        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler_Release(This);
        return errorCode;
    }

    ControllerCreatedHandler* controllerHandler = ControllerCreatedHandler_Create();
    if (!controllerHandler) {
        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler_Release(This);
        return E_OUTOFMEMORY;
    }

    HRESULT hr = ICoreWebView2Environment_CreateCoreWebView2Controller(
        createdEnvironment, g_hwnd, (ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*)controllerHandler
    );
    // WebView2 should hold a ref to controllerHandler until Invoke; drop ours now.
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler_Release(
        (ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*)controllerHandler
    );

    if (FAILED(hr)) {
        ShowHr(L"CreateCoreWebView2Controller", hr);
    }

    // Release our initial self-reference.
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler_Release(This);
    return hr;
}

static ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl g_env_created_vtbl = {
    EnvCreatedHandler_QueryInterface,
    EnvCreatedHandler_AddRef,
    EnvCreatedHandler_Release,
    EnvCreatedHandler_Invoke,
};

static EnvCreatedHandler* EnvCreatedHandler_Create(void) {
    EnvCreatedHandler* self =
        (EnvCreatedHandler*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*self));
    if (!self) {
        return NULL;
    }
    self->iface.lpVtbl = &g_env_created_vtbl;
    self->ref = 1;
    return self;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    (void)wParam;
    (void)lParam;

    switch (msg) {
    case WM_SIZE:
        ResizeWebView();
        return 0;
    case WM_DESTROY:
        if (g_webview && g_msg_token_valid) {
            (void)ICoreWebView2_remove_WebMessageReceived(g_webview, g_msg_token);
            g_msg_token_valid = FALSE;
        }
        if (g_webview) {
            ICoreWebView2_Release(g_webview);
            g_webview = NULL;
        }
        if (g_controller) {
            ICoreWebView2Controller_Release(g_controller);
            g_controller = NULL;
        }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

//extern __ImageBase;
//__declspec(noreturn) void WinMainCRTStartup(void) {
//    HINSTANCE hInstance = __ImageBase;
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        ShowHr(L"CoInitializeEx", hr);
        return EXIT_FAILURE;
    }

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"WebView2Window";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    if (!RegisterClassW(&wc)) {
        MessageBoxW(NULL, L"RegisterClassW failed", L"WebView2", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return EXIT_FAILURE;
    }

    g_hwnd = CreateWindowExW(
        0,
        L"WebView2Window",
        L"WebView2 + C Logic (Minimal)",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        900,
        600,
        NULL,
        NULL,
        hInstance,
        NULL
    );
    if (!g_hwnd) {
        MessageBoxW(NULL, L"CreateWindowExW failed", L"WebView2", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return EXIT_FAILURE;
    }

    ShowWindow(g_hwnd, nCmdShow);

    EnvCreatedHandler* envHandler = EnvCreatedHandler_Create();
    if (!envHandler) {
        CoUninitialize();
        return EXIT_FAILURE;
    }

    hr = CreateCoreWebView2EnvironmentWithOptions(
        NULL, NULL, NULL, (ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*)envHandler
    );
    if (FAILED(hr)) {
        ShowHr(L"CreateCoreWebView2EnvironmentWithOptions", hr);
        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler_Release(
            (ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*)envHandler
        );
        CoUninitialize();
        return EXIT_FAILURE;
    }

    // WebView2 will invoke envHandler asynchronously; it releases itself in Invoke.

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return (int)msg.wParam;
}
