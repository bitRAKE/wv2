# `webview_advanced` (C + WebView2)

`webview_advanced` is a "wide surface area" companion to `webview_minimal`: it stays **plain C**, but exercises a larger portion of the **WebView2 COM interface** (events, settings, devtools protocol, web resource interception, virtual host mapping).

The intent is to serve as a reference for how to write correct COM vtable handlers from C and as a playground for exploring what the WebView2 API can do without switching to C++ or a helper library.

## Why WebView2 (Host Perspective)

WebView2 gives a native Win32/C app a modern, standards-compliant UI surface while keeping native code in control:

- **UI velocity**: build the UI in HTML/CSS/JS, iterate quickly, and keep your platform integration in C.
- **Capability boundary**: the web layer can be treated as an untrusted UI; native stays the authority for OS access.
- **Bridge**: first-class JS <-> native messaging plus deep host control via events and settings.
- **Engine leverage**: rendering, accessibility, devtools, input, layout, and modern web APIs come "for free" with Edge.

## What This Sample Demonstrates

- **Local UI assets** without an embedded HTTP server:
  - Maps `https://appassets.local/` to `webview_advanced_assets/` via `ICoreWebView2_3_SetVirtualHostNameToFolderMapping`.
- **Request interception**:
  - Uses `AddWebResourceRequestedFilter` + `WebResourceRequested` to implement:
    - `GET https://appassets.local/api/time` (JSON)
    - `GET https://appassets.local/api/echo?text=...` (text)
- **Events** (instrumented and logged):
  - Navigation: `NavigationStarting`, `NavigationCompleted`, `SourceChanged`, `DocumentTitleChanged`
  - Permissions: `PermissionRequested` (demo policy: allow common user-initiated cases, deny non-user-initiated)
  - Dialogs: `ScriptDialogOpening` (auto-accept; prompt gets a native result string)
  - Windows: `NewWindowRequested` (handled; navigates current view)
  - Process: `ProcessFailed` (logs `Kind` and, when available, `Reason/ExitCode/Description`)
  - Fullscreen: `ContainsFullScreenElementChanged`
  - Close: `WindowCloseRequested` (destroys host window)
- **DevTools / Diagnostics**
  - `OpenDevToolsWindow`
  - `CallDevToolsProtocolMethod("Browser.getVersion")`
  - Dumps key `ICoreWebView2Settings*` flags
- **JS <-> native messaging**
  - The UI posts `cmd ...` strings via `chrome.webview.postMessage`.
  - Native logs to `OutputDebugStringW` and mirrors logs into the page after the UI signals `cmd ready`.

## Build And Run

Build (PowerShell):

```powershell
clang "@webview_advanced.response"
```

PowerShell note: arguments beginning with `@` must be quoted (or use `--%`) because `@` is the splatting operator.

Run:

```powershell
.\webview_advanced.exe
```

## UI Walkthrough

Inside the app:

- Click `Open DevTools` to confirm DevTools wiring.
- Click `DevTools: Browser.getVersion` to test `CallDevToolsProtocolMethod`.
- Click `fetch /api/time` and `fetch /api/echo` to test `WebResourceRequested` + in-memory responses.
- Click `Geolocation`, `Notifications`, `Clipboard Read` to hit `PermissionRequested`.
- Click `alert()` / `prompt()` to hit `ScriptDialogOpening`.
- Click `window.open (intercept)` to hit `NewWindowRequested`.
- Click `Toggle Fullscreen` to hit `ContainsFullScreenElementChanged`.
- Click `window.close()` to hit `WindowCloseRequested`.

## Future Directions

If you want to keep pushing "how far can C go with WebView2", good next areas to explore:

- **Richer bridge**: move from string commands to JSON-RPC with request/response IDs and schema validation.
- **More events**: downloads, web resource responses, history changes, favicon changes, accelerator keys, authentication, and certificate errors.
- **Profile + storage**: profile, cookies, permissions persistence, and per-app user data layouts.
- **Security posture**: strict navigation policies, origin allowlists, script injection hardening, and request filtering.
- **Custom protocols**: app-owned scheme and/or a more complete "API layer" over `WebResourceRequested`.

