// WebView2 "advanced" exploration sample in plain C.
//
// Build: clang "@advanced.response"
//
// This sample is intentionally verbose: it shows how to work with the COM-style
// WebView2 API from C without helper libraries.
//
// Suggested reading order for students:
// 1. WinMain
// 2. EnvCreated_Invoke
// 3. ControllerCreated_Invoke
// 4. SubscribeEvents
// 5. WebMessage_Invoke
// 6. WebResourceRequested_Invoke
// 7. the individual event handlers you want to copy into your own app

#include <windows.h>
#include <objbase.h>
#include <objidl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>

#include "WebView2.h"

typedef struct TokenSlot {
    EventRegistrationToken token;
    BOOL valid;
} TokenSlot;

// Global host state.
// This sample keeps one top-level WebView and stores event tokens so teardown is explicit.
static HWND g_hwnd = NULL;
static ICoreWebView2Environment* g_env = NULL;
static ICoreWebView2Controller* g_controller = NULL;
static ICoreWebView2* g_webview = NULL;
static BOOL g_page_ready = FALSE;

static TokenSlot g_tok_nav_starting = {{0}, FALSE};
static TokenSlot g_tok_nav_completed = {{0}, FALSE};
static TokenSlot g_tok_source_changed = {{0}, FALSE};
static TokenSlot g_tok_title_changed = {{0}, FALSE};
static TokenSlot g_tok_permission = {{0}, FALSE};
static TokenSlot g_tok_script_dialog = {{0}, FALSE};
static TokenSlot g_tok_new_window = {{0}, FALSE};
static TokenSlot g_tok_process_failed = {{0}, FALSE};
static TokenSlot g_tok_fullscreen = {{0}, FALSE};
static TokenSlot g_tok_window_close = {{0}, FALSE};
static TokenSlot g_tok_webmsg = {{0}, FALSE};
static TokenSlot g_tok_webresource = {{0}, FALSE};

// The sample serves its whole synthetic origin through WebResourceRequested.
// That makes the app self-contained and demonstrates request interception without a server.
static const wchar_t kAppOrigin[] = L"https://appassets.local";
static const wchar_t kAppOriginFilter[] = L"https://appassets.local/*";
static const wchar_t kAppStartUrl[] = L"https://appassets.local/index.html";
static const wchar_t kAssetsSubdir[] = L"\\advanced_assets";

// -----------------------------------------------------------------------------
// Logging and small host utilities
// -----------------------------------------------------------------------------

static void PostToPage(const wchar_t* msg) {
    if (!g_webview || !g_page_ready || !msg) {
        return;
    }
    (void)ICoreWebView2_PostWebMessageAsString(g_webview, msg);
}

static void LogLine(const wchar_t* msg) {
    if (!msg) {
        return;
    }
    OutputDebugStringW(msg);
    OutputDebugStringW(L"\r\n");
    PostToPage(msg);
}

static void LogFmt(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    (void)vswprintf_s(buf, _countof(buf), fmt, ap);
    va_end(ap);
    LogLine(buf);
}

static void PostResult(const wchar_t* text) {
    if (!text) {
        text = L"";
    }
    wchar_t buf[2048];
    (void)swprintf_s(buf, _countof(buf), L"result: %s", text);
    PostToPage(buf);
}

static void ShowHr(const wchar_t* where, HRESULT hr) {
    wchar_t msg[512];
    (void)swprintf_s(msg, _countof(msg), L"%s failed (HRESULT=0x%08lx)", where, (unsigned long)hr);
    MessageBoxW(NULL, msg, L"WebView2", MB_OK | MB_ICONERROR);
}

static BOOL StartsWithI(const wchar_t* s, const wchar_t* prefix) {
    size_t n = wcslen(prefix);
    return _wcsnicmp(s, prefix, n) == 0;
}

static void ResizeWebView(void) {
    if (!g_hwnd || !g_controller) {
        return;
    }
    RECT bounds;
    GetClientRect(g_hwnd, &bounds);
    (void)ICoreWebView2Controller_put_Bounds(g_controller, bounds);
}

static BOOL GetExeDir(wchar_t* out, size_t cap) {
    if (!out || cap == 0) {
        return FALSE;
    }
    DWORD n = GetModuleFileNameW(NULL, out, (DWORD)cap);
    if (n == 0 || n >= cap) {
        return FALSE;
    }
    for (DWORD i = n; i > 0; --i) {
        wchar_t c = out[i - 1];
        if (c == L'\\' || c == L'/') {
            out[i - 1] = L'\0';
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL PathAppendInPlace(wchar_t* path, size_t cap, const wchar_t* suffix) {
    if (!path || !suffix) return FALSE;
    size_t len = wcslen(path);
    size_t slen = wcslen(suffix);
    if (len + slen + 1 > cap) return FALSE;
    (void)wcscat_s(path, cap, suffix);
    return TRUE;
}

// -----------------------------------------------------------------------------
// Mini "app server" helpers for WebResourceRequested
// -----------------------------------------------------------------------------

static BOOL BuildAssetPath(const wchar_t* leafName, wchar_t* outPath, size_t cap) {
    if (!leafName || !outPath || cap == 0) {
        return FALSE;
    }
    // Keep the sample simple and safe: only serve known leaf files from advanced_assets.
    if (wcschr(leafName, L'\\') || wcschr(leafName, L'/') || wcschr(leafName, L':') ||
        wcsstr(leafName, L"..")) {
        return FALSE;
    }
    if (!GetExeDir(outPath, cap)) {
        return FALSE;
    }
    if (!PathAppendInPlace(outPath, cap, kAssetsSubdir)) {
        return FALSE;
    }
    if (!PathAppendInPlace(outPath, cap, L"\\")) {
        return FALSE;
    }
    if (!PathAppendInPlace(outPath, cap, leafName)) {
        return FALSE;
    }
    return TRUE;
}

static HRESULT LoadFileBytesAlloc(const wchar_t* path, BYTE** outBytes, size_t* outLen) {
    if (!path || !outBytes || !outLen) {
        return E_POINTER;
    }
    *outBytes = NULL;
    *outLen = 0;

    HANDLE file = CreateFileW(
        path,
        GENERIC_READ,
        FILE_SHARE_READ,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    if (file == INVALID_HANDLE_VALUE) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size)) {
        HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        CloseHandle(file);
        return hr;
    }
    if (size.QuadPart < 0 || (ULONGLONG)size.QuadPart > (ULONGLONG)SIZE_MAX) {
        CloseHandle(file);
        return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    }

    size_t len = (size_t)size.QuadPart;
    SIZE_T allocLen = len == 0 ? 1 : len;
    BYTE* bytes = (BYTE*)HeapAlloc(GetProcessHeap(), 0, allocLen);
    if (!bytes) {
        CloseHandle(file);
        return E_OUTOFMEMORY;
    }

    size_t total = 0;
    while (total < len) {
        size_t remaining = len - total;
        DWORD chunk = remaining > MAXDWORD ? MAXDWORD : (DWORD)remaining;
        DWORD got = 0;
        if (!ReadFile(file, bytes + total, chunk, &got, NULL)) {
            HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
            HeapFree(GetProcessHeap(), 0, bytes);
            CloseHandle(file);
            return hr;
        }
        if (got == 0) {
            HeapFree(GetProcessHeap(), 0, bytes);
            CloseHandle(file);
            return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
        }
        total += got;
    }

    CloseHandle(file);
    *outBytes = bytes;
    *outLen = len;
    return S_OK;
}

static HRESULT StreamFromBytes(const void* data, size_t len, IStream** outStream) {
    if (!outStream) return E_POINTER;
    *outStream = NULL;

    SIZE_T allocLen = len == 0 ? 1 : (SIZE_T)len;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, allocLen);
    if (!h) return E_OUTOFMEMORY;

    void* p = GlobalLock(h);
    if (!p) {
        GlobalFree(h);
        return HRESULT_FROM_WIN32(GetLastError());
    }

    if (len) {
        memcpy(p, data, len);
    }
    GlobalUnlock(h);

    IStream* stream = NULL;
    HRESULT hr = CreateStreamOnHGlobal(h, TRUE, &stream);
    if (FAILED(hr)) {
        GlobalFree(h);
        return hr;
    }
    *outStream = stream;
    return S_OK;
}

static HRESULT Utf8FromWideAlloc(const wchar_t* ws, char** outBytes, size_t* outLen) {
    if (!ws || !outBytes || !outLen) {
        return E_POINTER;
    }
    *outBytes = NULL;
    *outLen = 0;

    int needed = WideCharToMultiByte(CP_UTF8, 0, ws, -1, NULL, 0, NULL, NULL);
    if (needed <= 0) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    char* buf = (char*)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)needed);
    if (!buf) {
        return E_OUTOFMEMORY;
    }

    int written = WideCharToMultiByte(CP_UTF8, 0, ws, -1, buf, needed, NULL, NULL);
    if (written <= 0) {
        HeapFree(GetProcessHeap(), 0, buf);
        return HRESULT_FROM_WIN32(GetLastError());
    }

    *outBytes = buf;
    *outLen = (size_t)(written - 1);
    return S_OK;
}

static HRESULT MakeBytesResponse(
    int status,
    const wchar_t* reason,
    const wchar_t* contentType,
    const char* bytes,
    size_t len,
    ICoreWebView2WebResourceResponse** outResp
) {
    if (!outResp) {
        return E_POINTER;
    }
    *outResp = NULL;

    if (!g_env) {
        return E_UNEXPECTED;
    }

    IStream* stream = NULL;
    HRESULT hr = StreamFromBytes(bytes, len, &stream);
    if (FAILED(hr)) {
        return hr;
    }

    wchar_t headers[512];
    if (!contentType) {
        contentType = L"application/octet-stream";
    }
    (void)swprintf_s(
        headers,
        _countof(headers),
        L"Content-Type: %s\r\nCache-Control: no-store\r\n",
        contentType
    );

    ICoreWebView2WebResourceResponse* resp = NULL;
    hr = ICoreWebView2Environment_CreateWebResourceResponse(
        g_env,
        stream,
        status,
        reason ? reason : L"OK",
        headers,
        &resp
    );
    IStream_Release(stream);
    if (FAILED(hr)) {
        return hr;
    }

    *outResp = resp;
    return S_OK;
}

static HRESULT MakeTextResponseUtf8(
    int status,
    const wchar_t* reason,
    const wchar_t* contentType,
    const char* bytes,
    size_t len,
    ICoreWebView2WebResourceResponse** outResp
) {
    if (!contentType) {
        contentType = L"text/plain; charset=utf-8";
    }
    return MakeBytesResponse(status, reason, contentType, bytes, len, outResp);
}

static HRESULT MakeTextResponseWide(
    int status,
    const wchar_t* reason,
    const wchar_t* contentType,
    const wchar_t* text,
    ICoreWebView2WebResourceResponse** outResp
) {
    if (!text) {
        text = L"";
    }

    char* utf8 = NULL;
    size_t len = 0;
    HRESULT hr = Utf8FromWideAlloc(text, &utf8, &len);
    if (FAILED(hr)) {
        return hr;
    }

    hr = MakeTextResponseUtf8(status, reason, contentType, utf8, len, outResp);
    HeapFree(GetProcessHeap(), 0, utf8);
    return hr;
}

// -----------------------------------------------------------------------------
// COM callback pattern used throughout the sample
// -----------------------------------------------------------------------------

// NOTE: WebView2 is COM. From C, that means:
// - define a struct with the interface as the first field and a refcount
// - provide a vtable with QueryInterface/AddRef/Release/Invoke

#define DEFINE_IUNKNOWN_METHODS(StructType, InterfaceType, IID_Self)                                   \
    static ULONG STDMETHODCALLTYPE StructType##_AddRef(InterfaceType* This) {                          \
        StructType* self = (StructType*)This;                                                          \
        return (ULONG)InterlockedIncrement(&self->ref);                                                \
    }                                                                                                  \
    static ULONG STDMETHODCALLTYPE StructType##_Release(InterfaceType* This) {                         \
        StructType* self = (StructType*)This;                                                          \
        ULONG refs = (ULONG)InterlockedDecrement(&self->ref);                                          \
        if (refs == 0) {                                                                               \
            HeapFree(GetProcessHeap(), 0, self);                                                        \
        }                                                                                              \
        return refs;                                                                                   \
    }                                                                                                  \
    static HRESULT STDMETHODCALLTYPE StructType##_QueryInterface(                                      \
        InterfaceType* This, REFIID riid, void** ppvObject) {                                          \
        if (!ppvObject) {                                                                              \
            return E_POINTER;                                                                          \
        }                                                                                              \
        *ppvObject = NULL;                                                                             \
        if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_Self)) {                          \
            *ppvObject = This;                                                                         \
            StructType##_AddRef(This);                                                                 \
            return S_OK;                                                                               \
        }                                                                                              \
        return E_NOINTERFACE;                                                                          \
    }

typedef struct ExecScriptCompletedHandler {
    ICoreWebView2ExecuteScriptCompletedHandler iface;
    volatile LONG ref;
} ExecScriptCompletedHandler;

// Completion handlers are the "one-shot" async callbacks for host-initiated operations.
static HRESULT STDMETHODCALLTYPE ExecScriptCompleted_Invoke(
    ICoreWebView2ExecuteScriptCompletedHandler* This, HRESULT errorCode, LPCWSTR resultObjectAsJson
) {
    (void)This;
    if (FAILED(errorCode)) {
        LogFmt(L"[native] ExecuteScript completed: 0x%08lx", (unsigned long)errorCode);
        return S_OK;
    }

    PostResult(resultObjectAsJson ? resultObjectAsJson : L"(null)");
    return S_OK;
}

DEFINE_IUNKNOWN_METHODS(
    ExecScriptCompletedHandler,
    ICoreWebView2ExecuteScriptCompletedHandler,
    IID_ICoreWebView2ExecuteScriptCompletedHandler
)

static ICoreWebView2ExecuteScriptCompletedHandlerVtbl g_exec_script_completed_vtbl = {
    ExecScriptCompletedHandler_QueryInterface,
    ExecScriptCompletedHandler_AddRef,
    ExecScriptCompletedHandler_Release,
    ExecScriptCompleted_Invoke,
};

static ExecScriptCompletedHandler* ExecScriptCompletedHandler_Create(void) {
    ExecScriptCompletedHandler* self =
        (ExecScriptCompletedHandler*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*self));
    if (!self) {
        return NULL;
    }
    self->iface.lpVtbl = &g_exec_script_completed_vtbl;
    self->ref = 1;
    return self;
}

typedef struct DtpCompletedHandler {
    ICoreWebView2CallDevToolsProtocolMethodCompletedHandler iface;
    volatile LONG ref;
} DtpCompletedHandler;

static HRESULT STDMETHODCALLTYPE DtpCompleted_Invoke(
    ICoreWebView2CallDevToolsProtocolMethodCompletedHandler* This,
    HRESULT errorCode,
    LPCWSTR returnObjectAsJson
) {
    (void)This;
    if (FAILED(errorCode)) {
        LogFmt(L"[native] DevToolsProtocol completed: 0x%08lx", (unsigned long)errorCode);
        return S_OK;
    }

    PostResult(returnObjectAsJson ? returnObjectAsJson : L"(null)");
    return S_OK;
}

DEFINE_IUNKNOWN_METHODS(
    DtpCompletedHandler,
    ICoreWebView2CallDevToolsProtocolMethodCompletedHandler,
    IID_ICoreWebView2CallDevToolsProtocolMethodCompletedHandler
)

static ICoreWebView2CallDevToolsProtocolMethodCompletedHandlerVtbl g_dtp_completed_vtbl = {
    DtpCompletedHandler_QueryInterface,
    DtpCompletedHandler_AddRef,
    DtpCompletedHandler_Release,
    DtpCompleted_Invoke,
};

static DtpCompletedHandler* DtpCompletedHandler_Create(void) {
    DtpCompletedHandler* self =
        (DtpCompletedHandler*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*self));
    if (!self) {
        return NULL;
    }
    self->iface.lpVtbl = &g_dtp_completed_vtbl;
    self->ref = 1;
    return self;
}

typedef struct AddScriptCompletedHandler {
    ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler iface;
    volatile LONG ref;
} AddScriptCompletedHandler;

static HRESULT STDMETHODCALLTYPE AddScriptCompleted_Invoke(
    ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler* This,
    HRESULT errorCode,
    LPCWSTR id
) {
    (void)This;
    if (FAILED(errorCode)) {
        LogFmt(L"[native] AddScriptToExecuteOnDocumentCreated failed: 0x%08lx", (unsigned long)errorCode);
        return S_OK;
    }
    LogFmt(L"[native] injected script id: %s", id ? id : L"(null)");
    return S_OK;
}

DEFINE_IUNKNOWN_METHODS(
    AddScriptCompletedHandler,
    ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler,
    IID_ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler
)

static ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandlerVtbl g_addscript_completed_vtbl = {
    AddScriptCompletedHandler_QueryInterface,
    AddScriptCompletedHandler_AddRef,
    AddScriptCompletedHandler_Release,
    AddScriptCompleted_Invoke,
};

static AddScriptCompletedHandler* AddScriptCompletedHandler_Create(void) {
    AddScriptCompletedHandler* self =
        (AddScriptCompletedHandler*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*self));
    if (!self) {
        return NULL;
    }
    self->iface.lpVtbl = &g_addscript_completed_vtbl;
    self->ref = 1;
    return self;
}

// -----------------------------------------------------------------------------
// Host-side helpers that trigger WebView2 features from native code
// -----------------------------------------------------------------------------

static void DumpSettings(void) {
    if (!g_webview) {
        return;
    }

    ICoreWebView2Settings* s = NULL;
    HRESULT hr = ICoreWebView2_get_Settings(g_webview, &s);
    if (FAILED(hr) || !s) {
        LogFmt(L"[native] get_Settings failed: 0x%08lx", (unsigned long)hr);
        return;
    }

    BOOL b = FALSE;
    (void)ICoreWebView2Settings_get_IsScriptEnabled(s, &b);
    LogFmt(L"[settings] IsScriptEnabled=%d", (int)b);
    (void)ICoreWebView2Settings_get_IsWebMessageEnabled(s, &b);
    LogFmt(L"[settings] IsWebMessageEnabled=%d", (int)b);
    (void)ICoreWebView2Settings_get_AreDevToolsEnabled(s, &b);
    LogFmt(L"[settings] AreDevToolsEnabled=%d", (int)b);
    (void)ICoreWebView2Settings_get_AreDefaultContextMenusEnabled(s, &b);
    LogFmt(L"[settings] AreDefaultContextMenusEnabled=%d", (int)b);
    (void)ICoreWebView2Settings_get_AreDefaultScriptDialogsEnabled(s, &b);
    LogFmt(L"[settings] AreDefaultScriptDialogsEnabled=%d", (int)b);
    (void)ICoreWebView2Settings_get_IsStatusBarEnabled(s, &b);
    LogFmt(L"[settings] IsStatusBarEnabled=%d", (int)b);
    (void)ICoreWebView2Settings_get_IsZoomControlEnabled(s, &b);
    LogFmt(L"[settings] IsZoomControlEnabled=%d", (int)b);
    (void)ICoreWebView2Settings_get_IsBuiltInErrorPageEnabled(s, &b);
    LogFmt(L"[settings] IsBuiltInErrorPageEnabled=%d", (int)b);

    ICoreWebView2Settings2* s2 = NULL;
    hr = ICoreWebView2Settings_QueryInterface(s, &IID_ICoreWebView2Settings2, (void**)&s2);
    if (SUCCEEDED(hr) && s2) {
        LPWSTR ua = NULL;
        if (SUCCEEDED(ICoreWebView2Settings2_get_UserAgent(s2, &ua)) && ua) {
            LogFmt(L"[settings] UserAgent=%s", ua);
            CoTaskMemFree(ua);
        }
        ICoreWebView2Settings2_Release(s2);
    }

    ICoreWebView2Settings3* s3 = NULL;
    hr = ICoreWebView2Settings_QueryInterface(s, &IID_ICoreWebView2Settings3, (void**)&s3);
    if (SUCCEEDED(hr) && s3) {
        (void)ICoreWebView2Settings3_get_AreBrowserAcceleratorKeysEnabled(s3, &b);
        LogFmt(L"[settings] AreBrowserAcceleratorKeysEnabled=%d", (int)b);
        ICoreWebView2Settings3_Release(s3);
    }

    ICoreWebView2Settings_Release(s);
}

static void ExecScript(const wchar_t* js) {
    if (!g_webview || !js || !js[0]) {
        return;
    }

    ExecScriptCompletedHandler* h = ExecScriptCompletedHandler_Create();
    if (!h) {
        return;
    }

    HRESULT hr = ICoreWebView2_ExecuteScript(
        g_webview, js, (ICoreWebView2ExecuteScriptCompletedHandler*)h
    );

    ICoreWebView2ExecuteScriptCompletedHandler_Release((ICoreWebView2ExecuteScriptCompletedHandler*)h);

    if (FAILED(hr)) {
        LogFmt(L"[native] ExecuteScript start failed: 0x%08lx", (unsigned long)hr);
    }
}

static void CallDtpBrowserGetVersion(void) {
    if (!g_webview) {
        return;
    }

    DtpCompletedHandler* h = DtpCompletedHandler_Create();
    if (!h) {
        return;
    }

    HRESULT hr = ICoreWebView2_CallDevToolsProtocolMethod(
        g_webview,
        L"Browser.getVersion",
        L"{}",
        (ICoreWebView2CallDevToolsProtocolMethodCompletedHandler*)h
    );

    ICoreWebView2CallDevToolsProtocolMethodCompletedHandler_Release(
        (ICoreWebView2CallDevToolsProtocolMethodCompletedHandler*)h
    );

    if (FAILED(hr)) {
        LogFmt(L"[native] CallDevToolsProtocolMethod start failed: 0x%08lx", (unsigned long)hr);
    }
}

static const wchar_t* PermissionKindStr(COREWEBVIEW2_PERMISSION_KIND k) {
    switch (k) {
    case COREWEBVIEW2_PERMISSION_KIND_MICROPHONE:
        return L"microphone";
    case COREWEBVIEW2_PERMISSION_KIND_CAMERA:
        return L"camera";
    case COREWEBVIEW2_PERMISSION_KIND_GEOLOCATION:
        return L"geolocation";
    case COREWEBVIEW2_PERMISSION_KIND_NOTIFICATIONS:
        return L"notifications";
    case COREWEBVIEW2_PERMISSION_KIND_OTHER_SENSORS:
        return L"other_sensors";
    case COREWEBVIEW2_PERMISSION_KIND_CLIPBOARD_READ:
        return L"clipboard_read";
    default:
        return L"unknown";
    }
}

static const wchar_t* ScriptDialogKindStr(COREWEBVIEW2_SCRIPT_DIALOG_KIND k) {
    switch (k) {
    case COREWEBVIEW2_SCRIPT_DIALOG_KIND_ALERT:
        return L"alert";
    case COREWEBVIEW2_SCRIPT_DIALOG_KIND_CONFIRM:
        return L"confirm";
    case COREWEBVIEW2_SCRIPT_DIALOG_KIND_PROMPT:
        return L"prompt";
    case COREWEBVIEW2_SCRIPT_DIALOG_KIND_BEFOREUNLOAD:
        return L"beforeunload";
    default:
        return L"unknown";
    }
}

static const wchar_t* ProcessFailedKindStr(COREWEBVIEW2_PROCESS_FAILED_KIND k) {
    switch (k) {
    case COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED:
        return L"browser_exited";
    case COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED:
        return L"render_exited";
    case COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE:
        return L"render_unresponsive";
    case COREWEBVIEW2_PROCESS_FAILED_KIND_FRAME_RENDER_PROCESS_EXITED:
        return L"frame_render_exited";
    case COREWEBVIEW2_PROCESS_FAILED_KIND_UTILITY_PROCESS_EXITED:
        return L"utility_exited";
    case COREWEBVIEW2_PROCESS_FAILED_KIND_SANDBOX_HELPER_PROCESS_EXITED:
        return L"sandbox_helper_exited";
    case COREWEBVIEW2_PROCESS_FAILED_KIND_GPU_PROCESS_EXITED:
        return L"gpu_exited";
    case COREWEBVIEW2_PROCESS_FAILED_KIND_PPAPI_PLUGIN_PROCESS_EXITED:
        return L"ppapi_plugin_exited";
    case COREWEBVIEW2_PROCESS_FAILED_KIND_PPAPI_BROKER_PROCESS_EXITED:
        return L"ppapi_broker_exited";
    case COREWEBVIEW2_PROCESS_FAILED_KIND_UNKNOWN_PROCESS_EXITED:
        return L"unknown_exited";
    default:
        return L"unknown";
    }
}

// -----------------------------------------------------------------------------
// Event handlers
// -----------------------------------------------------------------------------

typedef struct WebMessageHandler {
    ICoreWebView2WebMessageReceivedEventHandler iface;
    volatile LONG ref;
} WebMessageHandler;

static HRESULT STDMETHODCALLTYPE WebMessage_Invoke(
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

    // The page sends this once its JS event listeners are installed.
    // Until then, native logs only go to the debugger to avoid losing messages.
    if (_wcsicmp(msg, L"cmd ready") == 0) {
        g_page_ready = TRUE;
        LogLine(L"[native] page ready");

        LPWSTR v = NULL;
        if (SUCCEEDED(GetAvailableCoreWebView2BrowserVersionString(NULL, &v)) && v) {
            LogFmt(L"[native] runtime version: %s", v);
            CoTaskMemFree(v);
        }

        CoTaskMemFree(msg);
        return S_OK;
    }

    if (StartsWithI(msg, L"cmd ")) {
        const wchar_t* cmd = msg + 4;

        if (_wcsicmp(cmd, L"devtools") == 0) {
            if (g_webview) {
                (void)ICoreWebView2_OpenDevToolsWindow(g_webview);
            }
        } else if (_wcsicmp(cmd, L"dtp-version") == 0) {
            CallDtpBrowserGetVersion();
        } else if (_wcsicmp(cmd, L"version") == 0) {
            LPWSTR v = NULL;
            if (SUCCEEDED(GetAvailableCoreWebView2BrowserVersionString(NULL, &v)) && v) {
                PostResult(v);
                CoTaskMemFree(v);
            } else {
                PostResult(L"(version query failed)");
            }
        } else if (_wcsicmp(cmd, L"settings-dump") == 0) {
            DumpSettings();
        } else if (StartsWithI(cmd, L"nav ")) {
            const wchar_t* url = cmd + 4;
            if (g_webview && url[0]) {
                (void)ICoreWebView2_Navigate(g_webview, url);
            }
        } else if (_wcsicmp(cmd, L"back") == 0) {
            BOOL can = FALSE;
            if (g_webview && SUCCEEDED(ICoreWebView2_get_CanGoBack(g_webview, &can)) && can) {
                (void)ICoreWebView2_GoBack(g_webview);
            }
        } else if (_wcsicmp(cmd, L"forward") == 0) {
            BOOL can = FALSE;
            if (g_webview && SUCCEEDED(ICoreWebView2_get_CanGoForward(g_webview, &can)) && can) {
                (void)ICoreWebView2_GoForward(g_webview);
            }
        } else if (_wcsicmp(cmd, L"reload") == 0) {
            if (g_webview) {
                (void)ICoreWebView2_Reload(g_webview);
            }
        } else if (StartsWithI(cmd, L"exec ")) {
            const wchar_t* js = cmd + 5;
            ExecScript(js);
        } else if (_wcsicmp(cmd, L"injected") == 0) {
            LogLine(L"[event] injected script ran");
        } else {
            LogFmt(L"[native] unknown cmd: %s", cmd);
        }
    } else {
        LogFmt(L"[native] msg: %s", msg);
    }

    CoTaskMemFree(msg);
    return S_OK;
}

DEFINE_IUNKNOWN_METHODS(
    WebMessageHandler,
    ICoreWebView2WebMessageReceivedEventHandler,
    IID_ICoreWebView2WebMessageReceivedEventHandler
)

static ICoreWebView2WebMessageReceivedEventHandlerVtbl g_webmsg_vtbl = {
    WebMessageHandler_QueryInterface,
    WebMessageHandler_AddRef,
    WebMessageHandler_Release,
    WebMessage_Invoke,
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

typedef struct NavigationStartingHandler {
    ICoreWebView2NavigationStartingEventHandler iface;
    volatile LONG ref;
} NavigationStartingHandler;

static HRESULT STDMETHODCALLTYPE NavigationStarting_Invoke(
    ICoreWebView2NavigationStartingEventHandler* This,
    ICoreWebView2* sender,
    ICoreWebView2NavigationStartingEventArgs* args
) {
    (void)This;
    (void)sender;

    LPWSTR uri = NULL;
    BOOL user = FALSE;
    BOOL redir = FALSE;
    UINT64 navId = 0;
    if (args) {
        (void)ICoreWebView2NavigationStartingEventArgs_get_Uri(args, &uri);
        (void)ICoreWebView2NavigationStartingEventArgs_get_IsUserInitiated(args, &user);
        (void)ICoreWebView2NavigationStartingEventArgs_get_IsRedirected(args, &redir);
        (void)ICoreWebView2NavigationStartingEventArgs_get_NavigationId(args, &navId);
    }

    LogFmt(
        L"[event] NavigationStarting id=%llu user=%d redirected=%d uri=%s",
        (unsigned long long)navId,
        (int)user,
        (int)redir,
        uri ? uri : L"(null)"
    );

    if (uri) {
        CoTaskMemFree(uri);
    }

    return S_OK;
}

DEFINE_IUNKNOWN_METHODS(
    NavigationStartingHandler,
    ICoreWebView2NavigationStartingEventHandler,
    IID_ICoreWebView2NavigationStartingEventHandler
)

static ICoreWebView2NavigationStartingEventHandlerVtbl g_nav_starting_vtbl = {
    NavigationStartingHandler_QueryInterface,
    NavigationStartingHandler_AddRef,
    NavigationStartingHandler_Release,
    NavigationStarting_Invoke,
};

static NavigationStartingHandler* NavigationStartingHandler_Create(void) {
    NavigationStartingHandler* self =
        (NavigationStartingHandler*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*self));
    if (!self) {
        return NULL;
    }
    self->iface.lpVtbl = &g_nav_starting_vtbl;
    self->ref = 1;
    return self;
}

typedef struct NavigationCompletedHandler {
    ICoreWebView2NavigationCompletedEventHandler iface;
    volatile LONG ref;
} NavigationCompletedHandler;

static HRESULT STDMETHODCALLTYPE NavigationCompleted_Invoke(
    ICoreWebView2NavigationCompletedEventHandler* This,
    ICoreWebView2* sender,
    ICoreWebView2NavigationCompletedEventArgs* args
) {
    (void)This;
    (void)sender;

    BOOL ok = FALSE;
    COREWEBVIEW2_WEB_ERROR_STATUS st = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
    UINT64 navId = 0;
    if (args) {
        (void)ICoreWebView2NavigationCompletedEventArgs_get_IsSuccess(args, &ok);
        (void)ICoreWebView2NavigationCompletedEventArgs_get_WebErrorStatus(args, &st);
        (void)ICoreWebView2NavigationCompletedEventArgs_get_NavigationId(args, &navId);
    }

    LogFmt(
        L"[event] NavigationCompleted id=%llu success=%d webError=%d",
        (unsigned long long)navId,
        (int)ok,
        (int)st
    );

    return S_OK;
}

DEFINE_IUNKNOWN_METHODS(
    NavigationCompletedHandler,
    ICoreWebView2NavigationCompletedEventHandler,
    IID_ICoreWebView2NavigationCompletedEventHandler
)

static ICoreWebView2NavigationCompletedEventHandlerVtbl g_nav_completed_vtbl = {
    NavigationCompletedHandler_QueryInterface,
    NavigationCompletedHandler_AddRef,
    NavigationCompletedHandler_Release,
    NavigationCompleted_Invoke,
};

static NavigationCompletedHandler* NavigationCompletedHandler_Create(void) {
    NavigationCompletedHandler* self =
        (NavigationCompletedHandler*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*self));
    if (!self) {
        return NULL;
    }
    self->iface.lpVtbl = &g_nav_completed_vtbl;
    self->ref = 1;
    return self;
}

typedef struct SourceChangedHandler {
    ICoreWebView2SourceChangedEventHandler iface;
    volatile LONG ref;
} SourceChangedHandler;

static HRESULT STDMETHODCALLTYPE SourceChanged_Invoke(
    ICoreWebView2SourceChangedEventHandler* This,
    ICoreWebView2* sender,
    ICoreWebView2SourceChangedEventArgs* args
) {
    (void)This;

    BOOL isNew = FALSE;
    if (args) {
        (void)ICoreWebView2SourceChangedEventArgs_get_IsNewDocument(args, &isNew);
    }

    LPWSTR src = NULL;
    if (sender) {
        (void)ICoreWebView2_get_Source(sender, &src);
    }

    LogFmt(L"[event] SourceChanged newDoc=%d source=%s", (int)isNew, src ? src : L"(null)");

    if (src) {
        CoTaskMemFree(src);
    }

    return S_OK;
}

DEFINE_IUNKNOWN_METHODS(
    SourceChangedHandler,
    ICoreWebView2SourceChangedEventHandler,
    IID_ICoreWebView2SourceChangedEventHandler
)

static ICoreWebView2SourceChangedEventHandlerVtbl g_source_changed_vtbl = {
    SourceChangedHandler_QueryInterface,
    SourceChangedHandler_AddRef,
    SourceChangedHandler_Release,
    SourceChanged_Invoke,
};

static SourceChangedHandler* SourceChangedHandler_Create(void) {
    SourceChangedHandler* self =
        (SourceChangedHandler*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*self));
    if (!self) {
        return NULL;
    }
    self->iface.lpVtbl = &g_source_changed_vtbl;
    self->ref = 1;
    return self;
}

typedef struct TitleChangedHandler {
    ICoreWebView2DocumentTitleChangedEventHandler iface;
    volatile LONG ref;
} TitleChangedHandler;

static HRESULT STDMETHODCALLTYPE TitleChanged_Invoke(
    ICoreWebView2DocumentTitleChangedEventHandler* This,
    ICoreWebView2* sender,
    IUnknown* args
) {
    (void)This;
    (void)args;

    LPWSTR title = NULL;
    if (sender) {
        (void)ICoreWebView2_get_DocumentTitle(sender, &title);
    }

    if (title) {
        wchar_t winTitle[512];
        (void)swprintf_s(winTitle, _countof(winTitle), L"WebView2 Advanced (C) - %s", title);
        if (g_hwnd) {
            SetWindowTextW(g_hwnd, winTitle);
        }

        LogFmt(L"[event] DocumentTitleChanged title=%s", title);
        CoTaskMemFree(title);
    } else {
        LogLine(L"[event] DocumentTitleChanged title=(null)");
    }

    return S_OK;
}

DEFINE_IUNKNOWN_METHODS(
    TitleChangedHandler,
    ICoreWebView2DocumentTitleChangedEventHandler,
    IID_ICoreWebView2DocumentTitleChangedEventHandler
)

static ICoreWebView2DocumentTitleChangedEventHandlerVtbl g_title_changed_vtbl = {
    TitleChangedHandler_QueryInterface,
    TitleChangedHandler_AddRef,
    TitleChangedHandler_Release,
    TitleChanged_Invoke,
};

static TitleChangedHandler* TitleChangedHandler_Create(void) {
    TitleChangedHandler* self =
        (TitleChangedHandler*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*self));
    if (!self) {
        return NULL;
    }
    self->iface.lpVtbl = &g_title_changed_vtbl;
    self->ref = 1;
    return self;
}

typedef struct PermissionRequestedHandler {
    ICoreWebView2PermissionRequestedEventHandler iface;
    volatile LONG ref;
} PermissionRequestedHandler;

static HRESULT STDMETHODCALLTYPE PermissionRequested_Invoke(
    ICoreWebView2PermissionRequestedEventHandler* This,
    ICoreWebView2* sender,
    ICoreWebView2PermissionRequestedEventArgs* args
) {
    (void)This;
    (void)sender;

    COREWEBVIEW2_PERMISSION_KIND kind = (COREWEBVIEW2_PERMISSION_KIND)0;
    BOOL user = FALSE;
    LPWSTR uri = NULL;
    if (args) {
        (void)ICoreWebView2PermissionRequestedEventArgs_get_PermissionKind(args, &kind);
        (void)ICoreWebView2PermissionRequestedEventArgs_get_IsUserInitiated(args, &user);
        (void)ICoreWebView2PermissionRequestedEventArgs_get_Uri(args, &uri);
    }

    COREWEBVIEW2_PERMISSION_STATE newState = COREWEBVIEW2_PERMISSION_STATE_DEFAULT;
    // This is intentionally opinionated demo policy rather than production policy.
    // It shows where a host app can centralize allow/deny decisions.
    if (!user) {
        newState = COREWEBVIEW2_PERMISSION_STATE_DENY;
    } else {
        switch (kind) {
        case COREWEBVIEW2_PERMISSION_KIND_GEOLOCATION:
        case COREWEBVIEW2_PERMISSION_KIND_NOTIFICATIONS:
        case COREWEBVIEW2_PERMISSION_KIND_CLIPBOARD_READ:
            newState = COREWEBVIEW2_PERMISSION_STATE_ALLOW;
            break;
        default:
            newState = COREWEBVIEW2_PERMISSION_STATE_DEFAULT;
            break;
        }
    }

    LogFmt(
        L"[event] PermissionRequested kind=%s user=%d uri=%s -> state=%d",
        PermissionKindStr(kind),
        (int)user,
        uri ? uri : L"(null)",
        (int)newState
    );

    if (args && newState != COREWEBVIEW2_PERMISSION_STATE_DEFAULT) {
        (void)ICoreWebView2PermissionRequestedEventArgs_put_State(args, newState);
    }

    if (uri) {
        CoTaskMemFree(uri);
    }
    return S_OK;
}

DEFINE_IUNKNOWN_METHODS(
    PermissionRequestedHandler,
    ICoreWebView2PermissionRequestedEventHandler,
    IID_ICoreWebView2PermissionRequestedEventHandler
)

static ICoreWebView2PermissionRequestedEventHandlerVtbl g_permission_vtbl = {
    PermissionRequestedHandler_QueryInterface,
    PermissionRequestedHandler_AddRef,
    PermissionRequestedHandler_Release,
    PermissionRequested_Invoke,
};

static PermissionRequestedHandler* PermissionRequestedHandler_Create(void) {
    PermissionRequestedHandler* self =
        (PermissionRequestedHandler*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*self));
    if (!self) {
        return NULL;
    }
    self->iface.lpVtbl = &g_permission_vtbl;
    self->ref = 1;
    return self;
}

typedef struct ScriptDialogOpeningHandler {
    ICoreWebView2ScriptDialogOpeningEventHandler iface;
    volatile LONG ref;
} ScriptDialogOpeningHandler;

static HRESULT STDMETHODCALLTYPE ScriptDialogOpening_Invoke(
    ICoreWebView2ScriptDialogOpeningEventHandler* This,
    ICoreWebView2* sender,
    ICoreWebView2ScriptDialogOpeningEventArgs* args
) {
    (void)This;
    (void)sender;

    COREWEBVIEW2_SCRIPT_DIALOG_KIND kind = (COREWEBVIEW2_SCRIPT_DIALOG_KIND)0;
    LPWSTR message = NULL;
    LPWSTR uri = NULL;
    LPWSTR defText = NULL;

    if (args) {
        (void)ICoreWebView2ScriptDialogOpeningEventArgs_get_Kind(args, &kind);
        (void)ICoreWebView2ScriptDialogOpeningEventArgs_get_Message(args, &message);
        (void)ICoreWebView2ScriptDialogOpeningEventArgs_get_Uri(args, &uri);
        (void)ICoreWebView2ScriptDialogOpeningEventArgs_get_DefaultText(args, &defText);
    }

    LogFmt(
        L"[event] ScriptDialogOpening kind=%s uri=%s msg=%s default=%s",
        ScriptDialogKindStr(kind),
        uri ? uri : L"(null)",
        message ? message : L"(null)",
        defText ? defText : L"(null)"
    );

    if (args && kind == COREWEBVIEW2_SCRIPT_DIALOG_KIND_PROMPT) {
        (void)ICoreWebView2ScriptDialogOpeningEventArgs_put_ResultText(args, L"native prompt result");
    }
    if (args) {
        (void)ICoreWebView2ScriptDialogOpeningEventArgs_Accept(args);
    }

    if (message) {
        CoTaskMemFree(message);
    }
    if (uri) {
        CoTaskMemFree(uri);
    }
    if (defText) {
        CoTaskMemFree(defText);
    }

    return S_OK;
}

DEFINE_IUNKNOWN_METHODS(
    ScriptDialogOpeningHandler,
    ICoreWebView2ScriptDialogOpeningEventHandler,
    IID_ICoreWebView2ScriptDialogOpeningEventHandler
)

static ICoreWebView2ScriptDialogOpeningEventHandlerVtbl g_script_dialog_vtbl = {
    ScriptDialogOpeningHandler_QueryInterface,
    ScriptDialogOpeningHandler_AddRef,
    ScriptDialogOpeningHandler_Release,
    ScriptDialogOpening_Invoke,
};

static ScriptDialogOpeningHandler* ScriptDialogOpeningHandler_Create(void) {
    ScriptDialogOpeningHandler* self =
        (ScriptDialogOpeningHandler*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*self));
    if (!self) {
        return NULL;
    }
    self->iface.lpVtbl = &g_script_dialog_vtbl;
    self->ref = 1;
    return self;
}

typedef struct NewWindowRequestedHandler {
    ICoreWebView2NewWindowRequestedEventHandler iface;
    volatile LONG ref;
} NewWindowRequestedHandler;

static HRESULT STDMETHODCALLTYPE NewWindowRequested_Invoke(
    ICoreWebView2NewWindowRequestedEventHandler* This,
    ICoreWebView2* sender,
    ICoreWebView2NewWindowRequestedEventArgs* args
) {
    (void)This;
    (void)sender;

    LPWSTR uri = NULL;
    BOOL user = FALSE;
    if (args) {
        (void)ICoreWebView2NewWindowRequestedEventArgs_get_Uri(args, &uri);
        (void)ICoreWebView2NewWindowRequestedEventArgs_get_IsUserInitiated(args, &user);
    }

    LogFmt(L"[event] NewWindowRequested user=%d uri=%s (handled)", (int)user, uri ? uri : L"(null)");

    if (args) {
        (void)ICoreWebView2NewWindowRequestedEventArgs_put_Handled(args, TRUE);
    }
    // Re-route popups into the current view so the behavior is visible in a single window.
    if (g_webview && uri) {
        (void)ICoreWebView2_Navigate(g_webview, uri);
    }

    if (uri) {
        CoTaskMemFree(uri);
    }

    return S_OK;
}

DEFINE_IUNKNOWN_METHODS(
    NewWindowRequestedHandler,
    ICoreWebView2NewWindowRequestedEventHandler,
    IID_ICoreWebView2NewWindowRequestedEventHandler
)

static ICoreWebView2NewWindowRequestedEventHandlerVtbl g_new_window_vtbl = {
    NewWindowRequestedHandler_QueryInterface,
    NewWindowRequestedHandler_AddRef,
    NewWindowRequestedHandler_Release,
    NewWindowRequested_Invoke,
};

static NewWindowRequestedHandler* NewWindowRequestedHandler_Create(void) {
    NewWindowRequestedHandler* self =
        (NewWindowRequestedHandler*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*self));
    if (!self) {
        return NULL;
    }
    self->iface.lpVtbl = &g_new_window_vtbl;
    self->ref = 1;
    return self;
}

typedef struct ProcessFailedHandler {
    ICoreWebView2ProcessFailedEventHandler iface;
    volatile LONG ref;
} ProcessFailedHandler;

static HRESULT STDMETHODCALLTYPE ProcessFailed_Invoke(
    ICoreWebView2ProcessFailedEventHandler* This,
    ICoreWebView2* sender,
    ICoreWebView2ProcessFailedEventArgs* args
) {
    (void)This;
    (void)sender;

    COREWEBVIEW2_PROCESS_FAILED_KIND kind = (COREWEBVIEW2_PROCESS_FAILED_KIND)0;
    if (args) {
        (void)ICoreWebView2ProcessFailedEventArgs_get_ProcessFailedKind(args, &kind);
    }

    LogFmt(L"[event] ProcessFailed kind=%s", ProcessFailedKindStr(kind));

    if (args) {
        // Newer SDKs expose richer failure info behind a later interface revision.
        ICoreWebView2ProcessFailedEventArgs2* a2 = NULL;
        HRESULT hr = ICoreWebView2ProcessFailedEventArgs_QueryInterface(
            args, &IID_ICoreWebView2ProcessFailedEventArgs2, (void**)&a2
        );
        if (SUCCEEDED(hr) && a2) {
            COREWEBVIEW2_PROCESS_FAILED_REASON reason = (COREWEBVIEW2_PROCESS_FAILED_REASON)0;
            int exitCode = 0;
            LPWSTR desc = NULL;

            (void)ICoreWebView2ProcessFailedEventArgs2_get_Reason(a2, &reason);
            (void)ICoreWebView2ProcessFailedEventArgs2_get_ExitCode(a2, &exitCode);
            (void)ICoreWebView2ProcessFailedEventArgs2_get_ProcessDescription(a2, &desc);

            LogFmt(
                L"[event] ProcessFailed details reason=%d exit=%d desc=%s",
                (int)reason,
                exitCode,
                desc ? desc : L"(null)"
            );

            if (desc) {
                CoTaskMemFree(desc);
            }
            ICoreWebView2ProcessFailedEventArgs2_Release(a2);
        }
    }

    return S_OK;
}

DEFINE_IUNKNOWN_METHODS(
    ProcessFailedHandler,
    ICoreWebView2ProcessFailedEventHandler,
    IID_ICoreWebView2ProcessFailedEventHandler
)

static ICoreWebView2ProcessFailedEventHandlerVtbl g_process_failed_vtbl = {
    ProcessFailedHandler_QueryInterface,
    ProcessFailedHandler_AddRef,
    ProcessFailedHandler_Release,
    ProcessFailed_Invoke,
};

static ProcessFailedHandler* ProcessFailedHandler_Create(void) {
    ProcessFailedHandler* self =
        (ProcessFailedHandler*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*self));
    if (!self) {
        return NULL;
    }
    self->iface.lpVtbl = &g_process_failed_vtbl;
    self->ref = 1;
    return self;
}

typedef struct FullscreenChangedHandler {
    ICoreWebView2ContainsFullScreenElementChangedEventHandler iface;
    volatile LONG ref;
} FullscreenChangedHandler;

static HRESULT STDMETHODCALLTYPE FullscreenChanged_Invoke(
    ICoreWebView2ContainsFullScreenElementChangedEventHandler* This,
    ICoreWebView2* sender,
    IUnknown* args
) {
    (void)This;
    (void)args;

    BOOL fs = FALSE;
    if (sender) {
        (void)ICoreWebView2_get_ContainsFullScreenElement(sender, &fs);
    }

    LogFmt(L"[event] ContainsFullScreenElementChanged fullscreen=%d", (int)fs);
    return S_OK;
}

DEFINE_IUNKNOWN_METHODS(
    FullscreenChangedHandler,
    ICoreWebView2ContainsFullScreenElementChangedEventHandler,
    IID_ICoreWebView2ContainsFullScreenElementChangedEventHandler
)

static ICoreWebView2ContainsFullScreenElementChangedEventHandlerVtbl g_fullscreen_vtbl = {
    FullscreenChangedHandler_QueryInterface,
    FullscreenChangedHandler_AddRef,
    FullscreenChangedHandler_Release,
    FullscreenChanged_Invoke,
};

static FullscreenChangedHandler* FullscreenChangedHandler_Create(void) {
    FullscreenChangedHandler* self =
        (FullscreenChangedHandler*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*self));
    if (!self) {
        return NULL;
    }
    self->iface.lpVtbl = &g_fullscreen_vtbl;
    self->ref = 1;
    return self;
}

typedef struct WindowCloseRequestedHandler {
    ICoreWebView2WindowCloseRequestedEventHandler iface;
    volatile LONG ref;
} WindowCloseRequestedHandler;

static HRESULT STDMETHODCALLTYPE WindowCloseRequested_Invoke(
    ICoreWebView2WindowCloseRequestedEventHandler* This,
    ICoreWebView2* sender,
    IUnknown* args
) {
    (void)This;
    (void)sender;
    (void)args;

    LogLine(L"[event] WindowCloseRequested (DestroyWindow)");
    if (g_hwnd) {
        DestroyWindow(g_hwnd);
    }
    return S_OK;
}

DEFINE_IUNKNOWN_METHODS(
    WindowCloseRequestedHandler,
    ICoreWebView2WindowCloseRequestedEventHandler,
    IID_ICoreWebView2WindowCloseRequestedEventHandler
)

static ICoreWebView2WindowCloseRequestedEventHandlerVtbl g_window_close_vtbl = {
    WindowCloseRequestedHandler_QueryInterface,
    WindowCloseRequestedHandler_AddRef,
    WindowCloseRequestedHandler_Release,
    WindowCloseRequested_Invoke,
};

static WindowCloseRequestedHandler* WindowCloseRequestedHandler_Create(void) {
    WindowCloseRequestedHandler* self =
        (WindowCloseRequestedHandler*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*self));
    if (!self) {
        return NULL;
    }
    self->iface.lpVtbl = &g_window_close_vtbl;
    self->ref = 1;
    return self;
}

static int HexVal(wchar_t c) {
    if (c >= L'0' && c <= L'9') {
        return (int)(c - L'0');
    }
    if (c >= L'a' && c <= L'f') {
        return 10 + (int)(c - L'a');
    }
    if (c >= L'A' && c <= L'F') {
        return 10 + (int)(c - L'A');
    }
    return -1;
}

static HRESULT UrlDecodeUtf8ToWideAlloc(const wchar_t* enc, size_t encLen, wchar_t** outWide) {
    if (!outWide) {
        return E_POINTER;
    }
    *outWide = NULL;
    if (!enc || encLen == 0) {
        wchar_t* empty = (wchar_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(wchar_t));
        if (!empty) {
            return E_OUTOFMEMORY;
        }
        *outWide = empty;
        return S_OK;
    }

    char* bytes = (char*)HeapAlloc(GetProcessHeap(), 0, encLen + 1);
    if (!bytes) {
        return E_OUTOFMEMORY;
    }

    size_t j = 0;
    for (size_t i = 0; i < encLen; ++i) {
        wchar_t c = enc[i];
        if (c == L'%' && i + 2 < encLen) {
            int hi = HexVal(enc[i + 1]);
            int lo = HexVal(enc[i + 2]);
            if (hi >= 0 && lo >= 0) {
                bytes[j++] = (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        if (c == L'+') {
            bytes[j++] = ' ';
        } else if (c <= 0x7f) {
            bytes[j++] = (char)c;
        } else {
            bytes[j++] = '?';
        }
    }
    bytes[j] = '\0';

    int needed = MultiByteToWideChar(CP_UTF8, 0, bytes, (int)j, NULL, 0);
    if (needed <= 0) {
        HeapFree(GetProcessHeap(), 0, bytes);
        return HRESULT_FROM_WIN32(GetLastError());
    }

    wchar_t* ws = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, ((size_t)needed + 1) * sizeof(wchar_t));
    if (!ws) {
        HeapFree(GetProcessHeap(), 0, bytes);
        return E_OUTOFMEMORY;
    }

    int written = MultiByteToWideChar(CP_UTF8, 0, bytes, (int)j, ws, needed);
    HeapFree(GetProcessHeap(), 0, bytes);
    if (written <= 0) {
        HeapFree(GetProcessHeap(), 0, ws);
        return HRESULT_FROM_WIN32(GetLastError());
    }
    ws[written] = L'\0';

    *outWide = ws;
    return S_OK;
}

// Strip query/fragment and normalize "https://appassets.local[/...]" into a route string.
static BOOL GetAppRouteFromUri(const wchar_t* uri, wchar_t* outRoute, size_t cap) {
    if (!uri || !outRoute || cap == 0) {
        return FALSE;
    }

    size_t baseLen = wcslen(kAppOrigin);
    if (_wcsnicmp(uri, kAppOrigin, baseLen) != 0) {
        return FALSE;
    }

    const wchar_t* route = uri + baseLen;
    if (route[0] == L'\0') {
        route = L"/";
    } else if (route[0] != L'/') {
        return FALSE;
    }

    const wchar_t* end = route;
    while (*end && *end != L'?' && *end != L'#') {
        ++end;
    }

    size_t routeLen = (size_t)(end - route);
    if (routeLen + 1 > cap) {
        return FALSE;
    }

    wmemcpy(outRoute, route, routeLen);
    outRoute[routeLen] = L'\0';
    return TRUE;
}

static HRESULT MakeAssetResponseForRoute(
    const wchar_t* route, ICoreWebView2WebResourceResponse** outResp
) {
    if (!route || !outResp) {
        return E_POINTER;
    }

    // Keep the dispatch table explicit so students can see exactly what is being served.
    const wchar_t* leafName = NULL;
    const wchar_t* contentType = NULL;
    if (_wcsicmp(route, L"/") == 0 || _wcsicmp(route, L"/index.html") == 0) {
        leafName = L"index.html";
        contentType = L"text/html; charset=utf-8";
    } else if (_wcsicmp(route, L"/app.js") == 0) {
        leafName = L"app.js";
        contentType = L"text/javascript; charset=utf-8";
    } else if (_wcsicmp(route, L"/style.css") == 0) {
        leafName = L"style.css";
        contentType = L"text/css; charset=utf-8";
    } else {
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }

    wchar_t path[MAX_PATH];
    if (!BuildAssetPath(leafName, path, _countof(path))) {
        return E_FAIL;
    }

    BYTE* bytes = NULL;
    size_t len = 0;
    HRESULT hr = LoadFileBytesAlloc(path, &bytes, &len);
    if (FAILED(hr)) {
        return hr;
    }

    hr = MakeBytesResponse(200, L"OK", contentType, (const char*)bytes, len, outResp);
    HeapFree(GetProcessHeap(), 0, bytes);
    return hr;
}

typedef struct WebResourceRequestedHandler {
    ICoreWebView2WebResourceRequestedEventHandler iface;
    volatile LONG ref;
} WebResourceRequestedHandler;

// This handler acts like a tiny HTTP router implemented in C.
// In a real app you might swap this out for stricter routing or a generated API layer.
static HRESULT STDMETHODCALLTYPE WebResourceRequested_Invoke(
    ICoreWebView2WebResourceRequestedEventHandler* This,
    ICoreWebView2* sender,
    ICoreWebView2WebResourceRequestedEventArgs* args
) {
    (void)This;
    (void)sender;

    if (!args) {
        return S_OK;
    }

    ICoreWebView2WebResourceRequest* req = NULL;
    HRESULT hr = ICoreWebView2WebResourceRequestedEventArgs_get_Request(args, &req);
    if (FAILED(hr) || !req) {
        return S_OK;
    }

    LPWSTR uri = NULL;
    LPWSTR method = NULL;
    (void)ICoreWebView2WebResourceRequest_get_Uri(req, &uri);
    (void)ICoreWebView2WebResourceRequest_get_Method(req, &method);

    if (uri) {
        LogFmt(L"[event] WebResourceRequested %s %s", method ? method : L"(null)", uri);
    }

    wchar_t route[128];
    BOOL haveRoute = GetAppRouteFromUri(uri, route, _countof(route));

    ICoreWebView2WebResourceResponse* resp = NULL;
    // Simple route table:
    // - /api/* returns generated responses
    // - everything else is served from advanced_assets
    if (haveRoute && _wcsicmp(route, L"/api/time") == 0) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t json[512];
        (void)swprintf_s(
            json,
            _countof(json),
            L"{\"ok\":true,\"pid\":%lu,\"local\":\"%04u-%02u-%02u %02u:%02u:%02u.%03u\"}\n",
            (unsigned long)GetCurrentProcessId(),
            (unsigned)st.wYear,
            (unsigned)st.wMonth,
            (unsigned)st.wDay,
            (unsigned)st.wHour,
            (unsigned)st.wMinute,
            (unsigned)st.wSecond,
            (unsigned)st.wMilliseconds
        );
        hr = MakeTextResponseWide(200, L"OK", L"application/json; charset=utf-8", json, &resp);
    } else if (haveRoute && _wcsicmp(route, L"/api/echo") == 0) {
        const wchar_t* q = wcschr(uri, L'?');
        const wchar_t* textParam = NULL;
        if (q) {
            textParam = wcsstr(q + 1, L"text=");
            if (textParam) {
                textParam += 5;
            }
        }

        wchar_t* decoded = NULL;
        if (textParam && textParam[0]) {
            const wchar_t* end = wcschr(textParam, L'&');
            size_t n = end ? (size_t)(end - textParam) : wcslen(textParam);
            hr = UrlDecodeUtf8ToWideAlloc(textParam, n, &decoded);
        } else {
            hr = UrlDecodeUtf8ToWideAlloc(L"", 0, &decoded);
        }

        if (SUCCEEDED(hr) && decoded) {
            wchar_t text[1024];
            (void)swprintf_s(text, _countof(text), L"echo: %s\n", decoded);
            hr = MakeTextResponseWide(200, L"OK", L"text/plain; charset=utf-8", text, &resp);
            HeapFree(GetProcessHeap(), 0, decoded);
        }
    } else if (haveRoute) {
        hr = MakeAssetResponseForRoute(route, &resp);
        if (FAILED(hr)) {
            wchar_t msg[256];
            (void)swprintf_s(msg, _countof(msg), L"not found: %s\n", route);
            hr = MakeTextResponseWide(404, L"Not Found", L"text/plain; charset=utf-8", msg, &resp);
        }
    } else {
        hr = MakeTextResponseWide(
            404,
            L"Not Found",
            L"text/plain; charset=utf-8",
            L"not found: unsupported uri\n",
            &resp
        );
    }

    if (SUCCEEDED(hr) && resp) {
        (void)ICoreWebView2WebResourceRequestedEventArgs_put_Response(args, resp);
        ICoreWebView2WebResourceResponse_Release(resp);
    }

    if (method) {
        CoTaskMemFree(method);
    }
    if (uri) {
        CoTaskMemFree(uri);
    }
    ICoreWebView2WebResourceRequest_Release(req);
    return S_OK;
}

DEFINE_IUNKNOWN_METHODS(
    WebResourceRequestedHandler,
    ICoreWebView2WebResourceRequestedEventHandler,
    IID_ICoreWebView2WebResourceRequestedEventHandler
)

static ICoreWebView2WebResourceRequestedEventHandlerVtbl g_webresource_vtbl = {
    WebResourceRequestedHandler_QueryInterface,
    WebResourceRequestedHandler_AddRef,
    WebResourceRequestedHandler_Release,
    WebResourceRequested_Invoke,
};

static WebResourceRequestedHandler* WebResourceRequestedHandler_Create(void) {
    WebResourceRequestedHandler* self =
        (WebResourceRequestedHandler*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*self));
    if (!self) {
        return NULL;
    }
    self->iface.lpVtbl = &g_webresource_vtbl;
    self->ref = 1;
    return self;
}

// -----------------------------------------------------------------------------
// Event subscription lifetime
// -----------------------------------------------------------------------------

static void TokenSlot_Clear(TokenSlot* s) {
    if (!s) {
        return;
    }
    ZeroMemory(&s->token, sizeof(s->token));
    s->valid = FALSE;
}

static void SubscribeEvents(void) {
    if (!g_webview) {
        return;
    }

    // Register every event before the first navigation so startup activity is observable.
    NavigationStartingHandler* navStart = NavigationStartingHandler_Create();
    if (navStart) {
        HRESULT hr = ICoreWebView2_add_NavigationStarting(
            g_webview, (ICoreWebView2NavigationStartingEventHandler*)navStart, &g_tok_nav_starting.token
        );
        ICoreWebView2NavigationStartingEventHandler_Release(
            (ICoreWebView2NavigationStartingEventHandler*)navStart
        );
        if (SUCCEEDED(hr)) {
            g_tok_nav_starting.valid = TRUE;
        } else {
            LogFmt(L"[native] add_NavigationStarting failed: 0x%08lx", (unsigned long)hr);
        }
    }

    NavigationCompletedHandler* navDone = NavigationCompletedHandler_Create();
    if (navDone) {
        HRESULT hr = ICoreWebView2_add_NavigationCompleted(
            g_webview, (ICoreWebView2NavigationCompletedEventHandler*)navDone, &g_tok_nav_completed.token
        );
        ICoreWebView2NavigationCompletedEventHandler_Release(
            (ICoreWebView2NavigationCompletedEventHandler*)navDone
        );
        if (SUCCEEDED(hr)) {
            g_tok_nav_completed.valid = TRUE;
        } else {
            LogFmt(L"[native] add_NavigationCompleted failed: 0x%08lx", (unsigned long)hr);
        }
    }

    SourceChangedHandler* src = SourceChangedHandler_Create();
    if (src) {
        HRESULT hr = ICoreWebView2_add_SourceChanged(
            g_webview, (ICoreWebView2SourceChangedEventHandler*)src, &g_tok_source_changed.token
        );
        ICoreWebView2SourceChangedEventHandler_Release((ICoreWebView2SourceChangedEventHandler*)src);
        if (SUCCEEDED(hr)) {
            g_tok_source_changed.valid = TRUE;
        } else {
            LogFmt(L"[native] add_SourceChanged failed: 0x%08lx", (unsigned long)hr);
        }
    }

    TitleChangedHandler* title = TitleChangedHandler_Create();
    if (title) {
        HRESULT hr = ICoreWebView2_add_DocumentTitleChanged(
            g_webview,
            (ICoreWebView2DocumentTitleChangedEventHandler*)title,
            &g_tok_title_changed.token
        );
        ICoreWebView2DocumentTitleChangedEventHandler_Release(
            (ICoreWebView2DocumentTitleChangedEventHandler*)title
        );
        if (SUCCEEDED(hr)) {
            g_tok_title_changed.valid = TRUE;
        } else {
            LogFmt(L"[native] add_DocumentTitleChanged failed: 0x%08lx", (unsigned long)hr);
        }
    }

    PermissionRequestedHandler* perm = PermissionRequestedHandler_Create();
    if (perm) {
        HRESULT hr = ICoreWebView2_add_PermissionRequested(
            g_webview, (ICoreWebView2PermissionRequestedEventHandler*)perm, &g_tok_permission.token
        );
        ICoreWebView2PermissionRequestedEventHandler_Release(
            (ICoreWebView2PermissionRequestedEventHandler*)perm
        );
        if (SUCCEEDED(hr)) {
            g_tok_permission.valid = TRUE;
        } else {
            LogFmt(L"[native] add_PermissionRequested failed: 0x%08lx", (unsigned long)hr);
        }
    }

    ScriptDialogOpeningHandler* dlg = ScriptDialogOpeningHandler_Create();
    if (dlg) {
        HRESULT hr = ICoreWebView2_add_ScriptDialogOpening(
            g_webview, (ICoreWebView2ScriptDialogOpeningEventHandler*)dlg, &g_tok_script_dialog.token
        );
        ICoreWebView2ScriptDialogOpeningEventHandler_Release(
            (ICoreWebView2ScriptDialogOpeningEventHandler*)dlg
        );
        if (SUCCEEDED(hr)) {
            g_tok_script_dialog.valid = TRUE;
        } else {
            LogFmt(L"[native] add_ScriptDialogOpening failed: 0x%08lx", (unsigned long)hr);
        }
    }

    NewWindowRequestedHandler* nw = NewWindowRequestedHandler_Create();
    if (nw) {
        HRESULT hr = ICoreWebView2_add_NewWindowRequested(
            g_webview, (ICoreWebView2NewWindowRequestedEventHandler*)nw, &g_tok_new_window.token
        );
        ICoreWebView2NewWindowRequestedEventHandler_Release(
            (ICoreWebView2NewWindowRequestedEventHandler*)nw
        );
        if (SUCCEEDED(hr)) {
            g_tok_new_window.valid = TRUE;
        } else {
            LogFmt(L"[native] add_NewWindowRequested failed: 0x%08lx", (unsigned long)hr);
        }
    }

    ProcessFailedHandler* pf = ProcessFailedHandler_Create();
    if (pf) {
        HRESULT hr = ICoreWebView2_add_ProcessFailed(
            g_webview, (ICoreWebView2ProcessFailedEventHandler*)pf, &g_tok_process_failed.token
        );
        ICoreWebView2ProcessFailedEventHandler_Release((ICoreWebView2ProcessFailedEventHandler*)pf);
        if (SUCCEEDED(hr)) {
            g_tok_process_failed.valid = TRUE;
        } else {
            LogFmt(L"[native] add_ProcessFailed failed: 0x%08lx", (unsigned long)hr);
        }
    }

    FullscreenChangedHandler* fs = FullscreenChangedHandler_Create();
    if (fs) {
        HRESULT hr = ICoreWebView2_add_ContainsFullScreenElementChanged(
            g_webview,
            (ICoreWebView2ContainsFullScreenElementChangedEventHandler*)fs,
            &g_tok_fullscreen.token
        );
        ICoreWebView2ContainsFullScreenElementChangedEventHandler_Release(
            (ICoreWebView2ContainsFullScreenElementChangedEventHandler*)fs
        );
        if (SUCCEEDED(hr)) {
            g_tok_fullscreen.valid = TRUE;
        } else {
            LogFmt(
                L"[native] add_ContainsFullScreenElementChanged failed: 0x%08lx",
                (unsigned long)hr
            );
        }
    }

    WindowCloseRequestedHandler* wc = WindowCloseRequestedHandler_Create();
    if (wc) {
        HRESULT hr = ICoreWebView2_add_WindowCloseRequested(
            g_webview, (ICoreWebView2WindowCloseRequestedEventHandler*)wc, &g_tok_window_close.token
        );
        ICoreWebView2WindowCloseRequestedEventHandler_Release(
            (ICoreWebView2WindowCloseRequestedEventHandler*)wc
        );
        if (SUCCEEDED(hr)) {
            g_tok_window_close.valid = TRUE;
        } else {
            LogFmt(L"[native] add_WindowCloseRequested failed: 0x%08lx", (unsigned long)hr);
        }
    }

    WebMessageHandler* wm = WebMessageHandler_Create();
    if (wm) {
        HRESULT hr = ICoreWebView2_add_WebMessageReceived(
            g_webview, (ICoreWebView2WebMessageReceivedEventHandler*)wm, &g_tok_webmsg.token
        );
        ICoreWebView2WebMessageReceivedEventHandler_Release(
            (ICoreWebView2WebMessageReceivedEventHandler*)wm
        );
        if (SUCCEEDED(hr)) {
            g_tok_webmsg.valid = TRUE;
        } else {
            LogFmt(L"[native] add_WebMessageReceived failed: 0x%08lx", (unsigned long)hr);
        }
    }

    // The whole sample origin is WRR-backed, so this filter must be active before navigation.
    HRESULT hrFilter = ICoreWebView2_AddWebResourceRequestedFilter(
        g_webview, kAppOriginFilter, COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL
    );
    if (FAILED(hrFilter)) {
        LogFmt(L"[native] AddWebResourceRequestedFilter failed: 0x%08lx", (unsigned long)hrFilter);
    } else {
        WebResourceRequestedHandler* wr = WebResourceRequestedHandler_Create();
        if (wr) {
            HRESULT hr = ICoreWebView2_add_WebResourceRequested(
                g_webview, (ICoreWebView2WebResourceRequestedEventHandler*)wr, &g_tok_webresource.token
            );
            ICoreWebView2WebResourceRequestedEventHandler_Release(
                (ICoreWebView2WebResourceRequestedEventHandler*)wr
            );
            if (SUCCEEDED(hr)) {
                g_tok_webresource.valid = TRUE;
            } else {
                LogFmt(L"[native] add_WebResourceRequested failed: 0x%08lx", (unsigned long)hr);
            }
        }
    }
}

static void UnsubscribeEvents(void) {
    if (!g_webview) {
        return;
    }

    if (g_tok_nav_starting.valid) {
        (void)ICoreWebView2_remove_NavigationStarting(g_webview, g_tok_nav_starting.token);
        TokenSlot_Clear(&g_tok_nav_starting);
    }
    if (g_tok_nav_completed.valid) {
        (void)ICoreWebView2_remove_NavigationCompleted(g_webview, g_tok_nav_completed.token);
        TokenSlot_Clear(&g_tok_nav_completed);
    }
    if (g_tok_source_changed.valid) {
        (void)ICoreWebView2_remove_SourceChanged(g_webview, g_tok_source_changed.token);
        TokenSlot_Clear(&g_tok_source_changed);
    }
    if (g_tok_title_changed.valid) {
        (void)ICoreWebView2_remove_DocumentTitleChanged(g_webview, g_tok_title_changed.token);
        TokenSlot_Clear(&g_tok_title_changed);
    }
    if (g_tok_permission.valid) {
        (void)ICoreWebView2_remove_PermissionRequested(g_webview, g_tok_permission.token);
        TokenSlot_Clear(&g_tok_permission);
    }
    if (g_tok_script_dialog.valid) {
        (void)ICoreWebView2_remove_ScriptDialogOpening(g_webview, g_tok_script_dialog.token);
        TokenSlot_Clear(&g_tok_script_dialog);
    }
    if (g_tok_new_window.valid) {
        (void)ICoreWebView2_remove_NewWindowRequested(g_webview, g_tok_new_window.token);
        TokenSlot_Clear(&g_tok_new_window);
    }
    if (g_tok_process_failed.valid) {
        (void)ICoreWebView2_remove_ProcessFailed(g_webview, g_tok_process_failed.token);
        TokenSlot_Clear(&g_tok_process_failed);
    }
    if (g_tok_fullscreen.valid) {
        (void)ICoreWebView2_remove_ContainsFullScreenElementChanged(g_webview, g_tok_fullscreen.token);
        TokenSlot_Clear(&g_tok_fullscreen);
    }
    if (g_tok_window_close.valid) {
        (void)ICoreWebView2_remove_WindowCloseRequested(g_webview, g_tok_window_close.token);
        TokenSlot_Clear(&g_tok_window_close);
    }
    if (g_tok_webmsg.valid) {
        (void)ICoreWebView2_remove_WebMessageReceived(g_webview, g_tok_webmsg.token);
        TokenSlot_Clear(&g_tok_webmsg);
    }
    if (g_tok_webresource.valid) {
        (void)ICoreWebView2_remove_WebResourceRequested(g_webview, g_tok_webresource.token);
        TokenSlot_Clear(&g_tok_webresource);
    }

    g_page_ready = FALSE;
}

// -----------------------------------------------------------------------------
// Async startup pipeline: environment -> controller -> settings -> navigation
// -----------------------------------------------------------------------------

typedef struct ControllerCreatedHandler {
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler iface;
    volatile LONG ref;
} ControllerCreatedHandler;

static HRESULT STDMETHODCALLTYPE ControllerCreated_Invoke(
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

    (void)ICoreWebView2Controller_put_IsVisible(g_controller, TRUE);
    ResizeWebView();

    ICoreWebView2Settings* s = NULL;
    hr = ICoreWebView2_get_Settings(g_webview, &s);
    if (SUCCEEDED(hr) && s) {
        (void)ICoreWebView2Settings_put_IsWebMessageEnabled(s, TRUE);
        (void)ICoreWebView2Settings_put_AreDevToolsEnabled(s, TRUE);
        (void)ICoreWebView2Settings_put_IsStatusBarEnabled(s, FALSE);
        // Disable the default dialogs so ScriptDialogOpening is the "real" handler.
        (void)ICoreWebView2Settings_put_AreDefaultScriptDialogsEnabled(s, FALSE);
        ICoreWebView2Settings_Release(s);
    }

    // Subscribe before navigating so the sample shows the full lifecycle from the first request.
    SubscribeEvents();

    // Inject a tiny script into every document so the sample can prove document-created injection works.
    const wchar_t* inject =
        L"(() => { try { chrome.webview.postMessage('cmd injected'); } catch (e) {} })();";
    AddScriptCompletedHandler* injDone = AddScriptCompletedHandler_Create();
    if (injDone) {
        HRESULT hr2 = ICoreWebView2_AddScriptToExecuteOnDocumentCreated(
            g_webview,
            inject,
            (ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler*)injDone
        );
        ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler_Release(
            (ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler*)injDone
        );
        if (FAILED(hr2)) {
            LogFmt(L"[native] AddScriptToExecuteOnDocumentCreated failed: 0x%08lx", (unsigned long)hr2);
        }
    }

    LogLine(L"[native] serving appassets.local entirely via WebResourceRequested");
    (void)ICoreWebView2_Navigate(g_webview, kAppStartUrl);

    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler_Release(This);
    return S_OK;
}

DEFINE_IUNKNOWN_METHODS(
    ControllerCreatedHandler,
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
    IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler
)

static ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl g_controller_created_vtbl = {
    ControllerCreatedHandler_QueryInterface,
    ControllerCreatedHandler_AddRef,
    ControllerCreatedHandler_Release,
    ControllerCreated_Invoke,
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

static HRESULT STDMETHODCALLTYPE EnvCreated_Invoke(
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* This,
    HRESULT errorCode,
    ICoreWebView2Environment* createdEnvironment
) {
    if (FAILED(errorCode) || !createdEnvironment) {
        ShowHr(L"CreateCoreWebView2EnvironmentWithOptions", errorCode);
        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler_Release(This);
        return errorCode;
    }

    if (g_env) {
        ICoreWebView2Environment_Release(g_env);
        g_env = NULL;
    }
    g_env = createdEnvironment;
    ICoreWebView2Environment_AddRef(g_env);

    ControllerCreatedHandler* controllerHandler = ControllerCreatedHandler_Create();
    if (!controllerHandler) {
        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler_Release(This);
        return E_OUTOFMEMORY;
    }

    HRESULT hr = ICoreWebView2Environment_CreateCoreWebView2Controller(
        createdEnvironment,
        g_hwnd,
        (ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*)controllerHandler
    );
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler_Release(
        (ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*)controllerHandler
    );

    if (FAILED(hr)) {
        ShowHr(L"CreateCoreWebView2Controller", hr);
    }

    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler_Release(This);
    return hr;
}

DEFINE_IUNKNOWN_METHODS(
    EnvCreatedHandler,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
    IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler
)

static ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl g_env_created_vtbl = {
    EnvCreatedHandler_QueryInterface,
    EnvCreatedHandler_AddRef,
    EnvCreatedHandler_Release,
    EnvCreated_Invoke,
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

// -----------------------------------------------------------------------------
// Win32 shell
// -----------------------------------------------------------------------------

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        ResizeWebView();
        return 0;
    case WM_DESTROY:
        // Tear down in the reverse order of setup so event tokens and COM objects stay tidy.
        UnsubscribeEvents();
        if (g_webview) {
            ICoreWebView2_Release(g_webview);
            g_webview = NULL;
        }
        if (g_controller) {
            ICoreWebView2Controller_Release(g_controller);
            g_controller = NULL;
        }
        if (g_env) {
            ICoreWebView2Environment_Release(g_env);
            g_env = NULL;
        }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        ShowHr(L"CoInitializeEx", hr);
        return EXIT_FAILURE;
    }

    const wchar_t* cls = L"WebView2AdvancedWindow";
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = cls;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    if (!RegisterClassW(&wc)) {
        MessageBoxW(NULL, L"RegisterClassW failed", L"WebView2", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return EXIT_FAILURE;
    }

    g_hwnd = CreateWindowExW(
        0,
        cls,
        L"WebView2 Advanced (C)",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1100,
        800,
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

    wchar_t userDataDir[MAX_PATH];
    if (!GetExeDir(userDataDir, _countof(userDataDir))) {
        MessageBoxW(NULL, L"GetExeDir failed", L"WebView2", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return EXIT_FAILURE;
    }
    if (!PathAppendInPlace(userDataDir, _countof(userDataDir), L"\\advanced_userdata")) {
        MessageBoxW(NULL, L"user data dir path too long", L"WebView2", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return EXIT_FAILURE;
    }
    // A dedicated user-data folder makes the sample's cookies/cache/profile easy to inspect.
    (void)CreateDirectoryW(userDataDir, NULL);

    EnvCreatedHandler* envHandler = EnvCreatedHandler_Create();
    if (!envHandler) {
        CoUninitialize();
        return EXIT_FAILURE;
    }

    hr = CreateCoreWebView2EnvironmentWithOptions(
        NULL,
        userDataDir,
        NULL,
        (ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*)envHandler
    );
    if (FAILED(hr)) {
        ShowHr(L"CreateCoreWebView2EnvironmentWithOptions", hr);
        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler_Release(
            (ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*)envHandler
        );
        CoUninitialize();
        return EXIT_FAILURE;
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return (int)msg.wParam;
}
