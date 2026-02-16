/* global chrome */

function $(id) {
  return document.getElementById(id);
}

const logEl = $("log");
const outEl = $("output");

function appendLog(line) {
  const s = String(line ?? "");
  logEl.textContent += (logEl.textContent ? "\n" : "") + s;
  logEl.scrollTop = logEl.scrollHeight;
}

function setOutput(text) {
  outEl.textContent = String(text ?? "");
}

function post(cmd) {
  appendLog(`[js] ${cmd}`);
  try {
    chrome.webview.postMessage(`cmd ${cmd}`);
  } catch (e) {
    appendLog(`[js] postMessage failed: ${e}`);
  }
}

function postRaw(raw) {
  appendLog(`[js] ${raw}`);
  try {
    chrome.webview.postMessage(raw);
  } catch (e) {
    appendLog(`[js] postMessage failed: ${e}`);
  }
}

function hasWebViewBridge() {
  return typeof chrome !== "undefined" && chrome.webview;
}

if (!hasWebViewBridge()) {
  appendLog("[js] chrome.webview is not available.");
} else {
  chrome.webview.addEventListener("message", (event) => {
    const msg = String(event.data ?? "");
    if (msg.startsWith("result:")) {
      setOutput(msg.slice("result:".length).trimStart());
      return;
    }
    appendLog(msg);
  });

  // Tell native we're ready to receive logs.
  postRaw("cmd ready");
}

document.querySelectorAll("button[data-cmd]").forEach((b) => {
  b.addEventListener("click", () => post(b.getAttribute("data-cmd")));
});

$("navGo").addEventListener("click", () => {
  const url = $("navUrl").value.trim();
  if (url) post(`nav ${url}`);
});

$("openBlank").addEventListener("click", () => {
  appendLog("[js] window.open https://example.com (expected: native intercept)");
  window.open("https://example.com", "_blank");
});

$("doAlert").addEventListener("click", () => {
  alert("Hello from JS. Native may auto-accept this dialog via ScriptDialogOpening.");
});

$("doPrompt").addEventListener("click", () => {
  const v = prompt("Type something (prompt). Native may set a default text.", "from prompt()");
  appendLog(`[js] prompt() -> ${JSON.stringify(v)}`);
});

$("doFullscreen").addEventListener("click", async () => {
  try {
    if (document.fullscreenElement) {
      await document.exitFullscreen();
    } else {
      await document.documentElement.requestFullscreen();
    }
  } catch (e) {
    appendLog(`[js] fullscreen error: ${e}`);
  }
});

$("doClose").addEventListener("click", () => {
  appendLog("[js] window.close()");
  window.close();
});

$("geo").addEventListener("click", () => {
  if (!navigator.geolocation) {
    appendLog("[js] geolocation not available");
    return;
  }
  navigator.geolocation.getCurrentPosition(
    (pos) => {
      setOutput(JSON.stringify({ ok: true, coords: pos.coords }, null, 2));
    },
    (err) => {
      setOutput(JSON.stringify({ ok: false, error: String(err && err.message) }, null, 2));
    },
    { enableHighAccuracy: false, timeout: 7000, maximumAge: 0 }
  );
});

$("notify").addEventListener("click", async () => {
  if (typeof Notification === "undefined") {
    appendLog("[js] Notification API not available");
    return;
  }
  try {
    const perm = await Notification.requestPermission();
    setOutput(`Notification permission: ${perm}`);
    if (perm === "granted") new Notification("WebView2 Advanced", { body: "Notifications work." });
  } catch (e) {
    setOutput(`Notification error: ${e}`);
  }
});

$("clipboard").addEventListener("click", async () => {
  if (!navigator.clipboard) {
    appendLog("[js] clipboard not available");
    return;
  }
  try {
    const text = await navigator.clipboard.readText();
    setOutput(`clipboard.readText(): ${JSON.stringify(text)}`);
  } catch (e) {
    setOutput(`clipboard.readText() error: ${e}`);
  }
});

$("apiTime").addEventListener("click", async () => {
  try {
    const r = await fetch("/api/time", { cache: "no-store" });
    const t = await r.text();
    setOutput(t);
  } catch (e) {
    setOutput(`fetch /api/time error: ${e}`);
  }
});

$("apiEcho").addEventListener("click", async () => {
  const text = $("echoText").value ?? "";
  try {
    const r = await fetch(`/api/echo?text=${encodeURIComponent(text)}`, { cache: "no-store" });
    const t = await r.text();
    setOutput(t);
  } catch (e) {
    setOutput(`fetch /api/echo error: ${e}`);
  }
});

$("execJs").addEventListener("click", () => {
  const expr = $("jsExpr").value ?? "";
  postRaw(`cmd exec ${expr}`);
});

