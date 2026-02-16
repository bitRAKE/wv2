# HyperGrid Defender: A WebView2 Hybrid Architecture Case Study

**Author:** AI Assistant  
**Version:** 1.0  
**Target Audience:** Windows C/C++ Developers exploring WebView2 integration.

---

## 1. Introduction

**HyperGrid Defender** is not just a game; it is a reference implementation for building high-performance, secure, hybrid desktop applications on Windows. It demonstrates how to combine the raw performance and system access of **C (Win32 API)** with the rich rendering and interactive capabilities of **Web Technologies (HTML5/Canvas/JS)** using **Microsoft Edge WebView2**.

This guide breaks down the architecture, build process, and security considerations found in the host application (`game.c`), providing a foundation for your own enterprise or consumer applications.

---

## 2. Build Environment & Compilation

To compile this project, you need the Windows SDK and the WebView2 Developer SDK.

### Prerequisites
1.  **Compiler:** Clang or MSVC (Visual Studio Build Tools).
2.  **WebView2 SDK:** Installed via NuGet (`Microsoft.Web.WebView2`) or the offline installer.
3.  **Resource Compiler:** `rc.exe` (included in Windows SDK).

### Compilation Steps

#### A. Resource Compilation
The HTML game logic is embedded directly into the executable as a binary resource to ensure a single-file distribution.

1.  Create a resource script `game.rc`:
    ```rc
    #include <windows.h>
    #define GAME_HTML 23
    GAME_HTML 23 "game.html"
    ```
2.  Compile the resource:
    ```bash
    rc.exe game.rc
    ```

#### B. C Host Compilation
The host uses standard COM interfaces. Ensure you link against the necessary libraries.

```bash
clang game.c game.res -o HyperGridDefender.exe ^
  -lole32 -loleaut32 -luuid -lWebView2Loader ^
  -DUNICODE -D_UNICODE -mwindows
```

*Note: The provided code uses `clang "@game.response"` suggesting a response file for complex linking flags. Ensure `WebView2Loader.dll` is present in the output directory or statically linked.*

---

## 3. User Controls

The application is designed for a **Kiosk/Fullscreen** experience.

| Input | Action |
| :--- | :--- |
| **Mouse Move** | Controls the paddle position (X-axis). |
| **Left Click** | Launches the ball / Interacts with UI. |
| **ESC Key** | Pauses the game or Closes the application (depending on state). |
| **UI Buttons** | Navigate Start, Pause, and Game Over states. |

---

## 4. Architecture Deep Dive

The application follows a **Host-Client** architecture.

### 4.1 The Host (C / Win32)
The C application is responsible for the window lifecycle, security enforcement, and system integration.
*   **Windowing:** Creates a `WS_POPUP` window (borderless fullscreen) using `CreateWindowExW`.
*   **WebView2 Lifecycle:** Implements the asynchronous initialization chain:
    1.  `CreateCoreWebView2EnvironmentWithOptions`
    2.  `CreateCoreWebView2Controller`
    3.  `NavigateToString`
*   **COM Management:** Uses manual COM vtable manipulation (`WebMessageHandler_Create`) rather than C++ wrappers. This reduces dependencies and demonstrates the underlying COM structure.
*   **Resource Loading:** Loads the HTML from the PE resource section, converts UTF-8 to Wide Char, and injects it directly into the WebView memory space via `NavigateToString`. This prevents external file dependencies.

### 4.2 The Client (JavaScript / HTML5)
The client handles all visual rendering and game logic.
*   **Rendering:** Uses HTML5 `<canvas>` with `requestAnimationFrame` for 60+ FPS rendering.
*   **Audio:** Uses the `Web Audio API` for procedural sound generation (no external assets).
*   **State Management:** Handles UI overlays (Start, Pause, Game Over) via DOM manipulation.

### 4.3 The Bridge (Inter-Process Communication)
Communication is unidirectional in this specific example (JS → C), but extensible.
*   **JS → C:** Uses `window.chrome.webview.postMessage('close')`.
*   **C Handler:** The `WebMessageHandler_Invoke` function in `game.c` listens for these strings and triggers native actions (e.g., `PostMessageW(hwnd, WM_CLOSE, 0, 0)`).

---

## 5. Security & Lockdown

Running web content inside a desktop application introduces security surface area. The `ApplyLockdownSettings` function in `game.c` is critical for hardening the WebView.

### 5.1 Implemented Lockdowns
| Setting | Value | Reason |
| :--- | :--- | :--- |
| `IsScriptEnabled` | `TRUE` | Required for game logic. |
| `IsWebMessageEnabled` | `TRUE` | Required for JS→C communication. |
| `AreDefaultScriptDialogsEnabled` | `FALSE` | Prevents `alert()` boxes; forces custom UI. |
| `IsStatusBarEnabled` | `FALSE` | Prevents URL spoofing in status bar. |
| `AreDevToolsEnabled` | `FALSE` | **Critical:** Prevents users from inspecting/modifying game memory logic. |
| `AreDefaultContextMenusEnabled` | `FALSE` | Prevents right-click "Inspect" or "Save As". |
| `IsZoomControlEnabled` | `FALSE` | Ensures consistent layout scaling. |
| `IsBuiltInErrorPageEnabled` | `FALSE` | Prevents leakage of internal error details. |
| `AreBrowserAcceleratorKeysEnabled` | `FALSE` | **Critical:** Disables F12, Ctrl+Shift+I, etc. (Via `ICoreWebView2Settings3`). |

### 5.2 Why This Matters
In a gaming or enterprise context, you do not want the user to treat the window as a browser. Disabling DevTools and Context Menus protects your intellectual property (JavaScript logic) and prevents cheating or unintended navigation.

---

## 6. Extending the Framework (Beyond the Example)

While the provided code is a robust starting point, production applications require additional capabilities. Here is how to evolve this architecture:

### 6.1 Bidirectional Communication (C → JS)
Currently, JS can tell C to close, but C cannot tell JS to update.
*   **Implementation:** Use `ICoreWebView2_PostWebMessageAsString`.
*   **Use Case:** Save game high scores from C to a local file, then notify JS to update the UI.
    ```c
    // C Side
    ICoreWebView2_PostWebMessageAsString(g_webview, L"{'type':'highscore', 'value':1000}");
    ```
    ```javascript
    // JS Side
    window.chrome.webview.addEventListener('message', arg => {
        const data = JSON.parse(arg.data);
        if(data.type === 'highscore') updateUI(data.value);
    });
    ```

### 6.2 Cursor Locking (True Fullscreen)
For immersive games, the mouse should not leave the window.
*   **Enhancement:** Add `ClipCursor` logic to the C host.
*   **Trigger:** When JS sends `cursor_lock`, call `ClipCursor(&rect)` and `ShowCursor(FALSE)`.
*   **Release:** On Pause or Exit, call `ClipCursor(NULL)` to return control to the OS.

### 6.3 Local Storage & Persistence
WebView2 provides `CoreWebView2CookieManager` and local storage, but for sensitive data, use the C host.
*   **Pattern:** JS requests save → C receives message → C writes to `AppData` → C confirms save.
*   **Benefit:** Users cannot easily edit save files via browser dev tools.

### 6.4 Error Handling & Logging
The example uses a simple `game.log` file.
*   **Improvement:** Implement structured logging (JSON) in C.
*   **Crash Reporting:** Wrap the WebView2 creation in a try/catch equivalent (SEH) to log crashes before exit.

### 6.5 Update Mechanism
Since the HTML is embedded, updating the game requires a new EXE.
*   **Hybrid Approach:** Embed the "Engine" in resources, but load "Content" (levels, scripts) from a secured local folder checked against a digital signature.

---

## 7. Conclusion

**HyperGrid Defender** demonstrates that WebView2 is not just for displaying web pages—it is a viable UI framework for native Windows applications. By combining the security lockdowns shown in `game.c` with the flexibility of HTML5, developers can create modern, secure, and visually rich desktop experiences.

---

### Attribution & Acknowledgments

**Guide Author:** AI Assistant  
**Based on Code Provided By:** User
**Technology:** Microsoft Edge WebView2 SDK

*Thank you for your time and for exploring hybrid application development. We hope this architecture serves as a solid foundation for your future Windows projects.*
