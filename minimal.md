# `webview_minimal` (C + WebView2)

`webview_minimal` is a tiny, single-file example of embedding Microsoft Edge (Chromium) via **WebView2** inside a native Win32 window, written in **plain C** (no C++ required).

The point of this sample is not "the prettiest app", but a **reliable minimal foundation** you can grow into a real product without immediately pulling in frameworks.

## Why WebView2

WebView2 gives you a modern UI surface (HTML/CSS/JS) while keeping native code in charge:

- **Modern UI without rewriting your app**: build the UI with web tech, keep your core logic in C.
- **Fast iteration**: tweak layout/behavior in HTML/JS without rebuilding the native layer for every UI change.
- **Leverages the Edge runtime**: Chromium-based rendering, standards compliance, devtools, accessibility, input, and layout are handled by the engine.
- **Clean separation of concerns**: C owns OS integration and hard stuff (files, sockets, crypto, perf-critical code). Web owns UI/UX.
- **Bridgeable**: WebView2 has a first-class messaging/channel model for JS <-> native communication.

## What This Minimal Sample Demonstrates

- Creating a Win32 `HWND`
- Initializing COM and creating a WebView2 environment and controller
- Navigating to an HTML string (`NavigateToString`)
- JS <-> native messaging using plain strings
  - JS posts `"sum <a> <b>"` and `"process <text>"`
  - Native replies with a string via `PostWebMessageAsString`

This sample intentionally keeps the bridge simple (plain strings) to avoid JSON parsing dependencies in the "minimal" baseline.

## Build And Run

The build uses a clang response file for flags, include paths, and libraries:

```powershell
clang "@webview_minimal.response"
```

PowerShell note: arguments beginning with `@` must be quoted (or use `--%`) because `@` is the splatting operator.

Run:

```powershell
.\webview_minimal.exe
```

## Benefits Of A C-First WebView2 Baseline

- **You can stay in C**: WebView2 is COM-based, and this sample shows the C-style vtables and handlers working end-to-end.
- **Minimal dependencies**: no additional UI toolkit required; no JSON parser required for the baseline.
- **Easier to evolve**: once you have a stable "create window -> create WebView2 -> bridge messages" pipeline, you can add features incrementally without redesigning the app.

## Future Directions

Here are practical next steps that scale this sample into something production-ish:

- Serve real UI assets:
  - Use `SetVirtualHostNameToFolderMapping` to map `https://app.local/` to a local folder (HTML/CSS/JS), instead of `NavigateToString`.
  - Add a build step to bundle/minify assets if desired.
- Stronger messaging:
  - Move from plain strings to JSON (define a schema, version it, validate it).
  - Add request/response correlation IDs so JS can await native operations.
- Host objects / richer API surface:
  - Expose a small native API to JS (capability-gated), or keep a strict message-only bridge for simplicity and security.
- Security hardening:
  - Restrict navigation, disable unwanted features, and filter requests (`add_WebResourceRequested`, navigation starting events).
  - Treat the web surface as untrusted input: validate everything crossing the boundary.
- App integration:
  - File pickers, drag/drop, shell integration, printing, clipboard, custom protocols.
  - Persist settings and data in a dedicated user data folder.
- Diagnostics and tooling:
  - Enable/disable devtools based on build configuration.
  - Add logging of bridge messages and HRESULT errors.
- Packaging:
  - Decide between Evergreen runtime vs Fixed Version runtime distribution strategy.
  - Add an installer or self-contained layout that includes the loader/runtime requirements.

## Notes

- WebView2 is asynchronous: environment creation, controller creation, and events happen via COM callbacks.
- The sample uses strict compile flags (`-Werror`) and explicitly configures the WebView2 header for C (`CINTERFACE`, `COBJMACROS`).
