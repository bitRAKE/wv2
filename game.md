# HyperGrid Defender - Game (C + WebView2)

**HyperGrid Defender** is a single-file, fullscreen "Kiosk Mode" game tailored for Windows. It embeds a modern HTML5 Canvas game inside a native C host application using WebView2.

## Features

- **Single Executable**: The game logic (`game.html`) is embedded directly into `game.exe`. No external files required.
- **Kiosk/Arcade Mode**:
  - Fullscreen borderless window.
  - Context menus, DevTools, and accelerator keys are disabled.
  - "Exit" button sends a message to the C host to terminate the process.
- **Neon Aesthetics**: High-performance HTML5 Canvas rendering with glow effects.

## Controls

- **Mouse / Trackpad**: Move ship and aim.
- **Left Click**: Shoot.
- **P**: Pause / Unpause.
- **Exit Button**: Close the application.

## Build

The build process uses `clang` and `rc` (Resource Compiler).

1.  Compile the resource script:
    ```powershell
    rc /nologo /fo game.res game.rc
    ```
2.  Compile and link the executable:
    ```powershell
    clang "@game.response"
    ```

## Architecture

1.  **Host (`game.c`)**:
    - Initializes a Win32 popup window.
    - Sets up the WebView2 environment.
    - Loads `game.html` from the embedded `RT_HTML` resource.
    - Locks down browser features (no right-click, no zooming).
    - Listens for `close` message from JavaScript.

2.  **Game (`game.html`)**:
    - Self-contained HTML/CSS/JS.
    - Handles game loop, input, and rendering.
    - Communicates with host via `window.chrome.webview.postMessage`.

## Lockdown & Kiosk Mode Verification

This application is designed to be a "sealed" experience, similar to an arcade cabinet or kiosk. Here is how to verify the lockdown and understand the trade-offs.

### How to Test
1.  **Right-Click**: Attempt to right-click anywhere.
    - *Expected*: No context menu appears.
2.  **DevTools Shortcuts**: Press `F12` or `Ctrl+Shift+I`.
    - *Expected*: Developer Tools do not open.
3.  **Zoom**: Hold `Ctrl` and scroll the mouse wheel, or press `Ctrl` + `+`/`-`.
    - *Expected*: The page scale does not change.
4.  **Refresh**: Press `F5` or `Ctrl+R`.
    - *Expected*: The game does not reload.
5.  **Status Bar**: Hover over any link (if added) or during loading.
    - *Expected*: No URL preview status bar appears at the bottom.

### Benefits
-   **Immersion**: The user cannot accidentally break the game state or see browser UI elements.
-   **Security**: Prevents users from navigating to arbitrary URLs or inspecting code/network traffic easily.
-   **Consistency**: Ensures the visual layout remains fixed (no accidental zoom).

### Potential Issues
-   **Debugging**: Since DevTools are disabled, debugging rendering or JS errors requires a rebuild with `AreDevToolsEnabled = TRUE`.
-   **Accessibility**: Disabling zoom may hinder users who need larger text/UI.
-   **Panic Exit**: If the in-game "Exit" button fails, users must know to use `Alt+F4` or Task Manager to close the app, as there is no window chrome.

## Troubleshooting

- View `game.log` for runtime diagnostics (initialization steps, resource loading).
