// Auto-injected chrome bar (§18 in linux-wpe.md).
//
// Electrobun injects a top chrome bar (title + close + fullscreen) into every
// page the webview navigates to, *but only* when there is no OS-provided
// chrome to fall back on. Today that means: linux-embedded (bare-DRM, no
// compositor) with titleBarStyle "default". On every other surface — macOS /
// Windows / Linux-desktop with any style, or linux-embedded with
// "hidden"/"hiddenInset" — the boolean is false and this module is a no-op.
//
// The decision is computed bun-side (in proc/native.ts) and exposed as
// `window.__electrobunAutoInjectChrome`; this module stays platform-agnostic.
// The injection runs at DOMContentLoaded so it works across SPA navigations
// AND multi-page apps.

import "./globals.d.ts";

const CHROME_CLOSE_URL = "electrobun://chrome/close";
const CHROME_MAXIMIZE_URL = "electrobun://chrome/maximize";
const CHROME_RESTORE_URL = "electrobun://chrome/restore";
const CHROME_REVEAL_URL = "electrobun://chrome/reveal";

type NativeChromeBridge = {
  postMessage: (action: ChromeAction) => void;
};

type ChromeAction = "close" | "maximize" | "restore" | "reveal";

function postChromeAction(action: ChromeAction) {
  const bridge = (
    window as unknown as {
      webkit?: { messageHandlers?: { electrobunChrome?: NativeChromeBridge } };
    }
  ).webkit?.messageHandlers?.electrobunChrome;

  if (bridge) {
    bridge.postMessage(action);
    return;
  }

  // Compatibility fallback for an older WPE runtime paired with this preload.
  window.location.href =
    action === "close"
      ? CHROME_CLOSE_URL
      : action === "maximize"
        ? CHROME_MAXIMIZE_URL
        : action === "restore"
          ? CHROME_RESTORE_URL
          : CHROME_REVEAL_URL;
}

function initCompositedChromeGesture() {
  const MAXIMIZED_KEY = "__electrobun_composited_chrome_maximized";
  let maximized = false;
  try {
    maximized = sessionStorage.getItem(MAXIMIZED_KEY) === "1";
  } catch {}

  window.__electrobunSetChromeMaximized = (value: boolean) => {
    maximized = value;
    try {
      if (value) {
        sessionStorage.setItem(MAXIMIZED_KEY, "1");
      } else {
        sessionStorage.removeItem(MAXIMIZED_KEY);
      }
    } catch {}
  };

  const PULL_START_ZONE_PX = 16;
  const PULL_THRESHOLD_PX = 36;
  let pull:
    | { identifier: number; startX: number; startY: number }
    | undefined;
  let suppressClickUntil = 0;

  const findTouch = (touches: TouchList, identifier: number) => {
    for (let i = 0; i < touches.length; i++) {
      const touch = touches.item(i);
      if (touch?.identifier === identifier) return touch;
    }
  };

  document.addEventListener("touchstart", (e) => {
    if (!maximized || e.touches.length !== 1) return;
    const touch = e.touches.item(0);
    if (!touch || touch.clientY > PULL_START_ZONE_PX) return;
    pull = {
      identifier: touch.identifier,
      startX: touch.clientX,
      startY: touch.clientY,
    };
  }, { capture: true, passive: true });

  document.addEventListener("touchmove", (e) => {
    if (!pull || !maximized) return;
    const touch = findTouch(e.touches, pull.identifier);
    if (!touch) return;
    const dx = touch.clientX - pull.startX;
    const dy = touch.clientY - pull.startY;
    if (dy < PULL_THRESHOLD_PX || dy <= Math.abs(dx) * 1.2) return;

    pull = undefined;
    suppressClickUntil = Date.now() + 500;
    e.preventDefault();
    e.stopImmediatePropagation();
    postChromeAction("reveal");
  }, { capture: true, passive: false });

  const finishPull = () => {
    pull = undefined;
  };
  document.addEventListener("touchend", finishPull, { capture: true });
  document.addEventListener("touchcancel", finishPull, { capture: true });
  document.addEventListener("click", (e) => {
    if (Date.now() >= suppressClickUntil) return;
    e.preventDefault();
    e.stopImmediatePropagation();
  }, { capture: true });
}

const CHROME_HTML = `
<header data-electrobun-chrome="true" style="
  position: fixed;
  top: 0; left: 0; right: 0;
  height: 60px;
  background: #1a1a1a;
  color: #fff5e6;
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 0 0.6em;
  z-index: 2147483647;
  box-shadow: 0 2px 8px rgba(0, 0, 0, 0.4);
  font-family: sans-serif;
  font-size: 16px;
  pointer-events: auto;
">
  <div data-electrobun-chrome-title style="
    padding-left: 0.4em;
    font-weight: 600;
    letter-spacing: 0.5px;
    opacity: 0.85;
  "></div>
  <div style="display: flex; align-items: center; gap: 0.3em;">
    <button data-electrobun-chrome-fs type="button" aria-label="Maximize" style="
      appearance: none; -webkit-appearance: none;
      border: none; background: transparent;
      color: #fff5e6;
      font: inherit; font-size: 26px; line-height: 1;
      width: 68px; height: 60px;
      border-radius: 10px;
      cursor: pointer;
    ">&#x26F6;</button>
    <button data-electrobun-chrome-close type="button" aria-label="Close" style="
      appearance: none; -webkit-appearance: none;
      border: none; background: transparent;
      color: #cc3300;
      font: inherit; font-size: 26px; font-weight: 700; line-height: 1;
      width: 68px; height: 60px;
      border-radius: 10px;
      cursor: pointer;
    ">&#x2715;</button>
  </div>
</header>
<button data-electrobun-chrome-restore type="button" aria-label="Reveal window controls" style="
  appearance: none; -webkit-appearance: none;
  position: fixed;
  top: 0; left: 50%;
  transform: translateX(-50%);
  z-index: 2147483647;
  display: none;
  width: 76px; height: 28px;
  padding: 0;
  border: none;
  border-radius: 0 0 10px 10px;
  background: rgba(26, 26, 26, 0.82);
  color: #fff5e6;
  font: 700 19px/28px sans-serif;
  cursor: pointer;
  touch-action: none;
">&#x25BE;</button>
<style data-electrobun-chrome-style>
  /* Nudge the document down so the chrome doesn't overlap content. We
     can't modify the body itself (apps may have already laid it out)
     but we can add a marker margin-top via a body padding shim. */
  body { padding-top: 60px !important; box-sizing: border-box; }
  body[data-electrobun-chrome-maximized] { padding-top: 0 !important; }
  [data-electrobun-chrome="true"] {
    transition: transform 160ms ease-out, opacity 160ms ease-out;
    will-change: transform, opacity;
  }
  [data-electrobun-chrome="true"][data-hidden] {
    transform: translateY(-100%) !important;
    opacity: 0 !important;
    pointer-events: none !important;
  }
  body[data-electrobun-chrome-concealed] [data-electrobun-chrome-restore] {
    display: block !important;
  }
  [data-electrobun-chrome-fs]:hover,
  [data-electrobun-chrome-close]:hover {
    background: rgba(255, 255, 255, 0.08) !important;
  }
  [data-electrobun-chrome-fs]:active,
  [data-electrobun-chrome-close]:active { transform: scale(0.92); }
</style>
`;

function injectChrome() {
  // Idempotent: don't double-inject on SPA route changes that don't reload.
  if (document.querySelector('[data-electrobun-chrome="true"]')) return;
  if (!document.body) return;

  const wrap = document.createElement("div");
  wrap.innerHTML = CHROME_HTML;
  // Move children of wrap into body in order; <style> can sit anywhere.
  while (wrap.firstChild) document.body.insertBefore(wrap.firstChild, document.body.firstChild);

  const header = document.querySelector<HTMLElement>('[data-electrobun-chrome="true"]');
  const closeBtn = document.querySelector<HTMLButtonElement>('[data-electrobun-chrome-close]');
  const fsBtn = document.querySelector<HTMLButtonElement>('[data-electrobun-chrome-fs]');
  const restoreBtn = document.querySelector<HTMLButtonElement>('[data-electrobun-chrome-restore]');
  const titleEl = document.querySelector<HTMLElement>('[data-electrobun-chrome-title]');
  if (!header || !closeBtn || !fsBtn || !restoreBtn || !titleEl) return;

  titleEl.textContent = document.title || "";
  // Keep the title in sync if the app sets document.title later.
  const titleObserver = new MutationObserver(() => {
    titleEl.textContent = document.title || "";
  });
  const titleNode = document.querySelector("title");
  if (titleNode) titleObserver.observe(titleNode, { childList: true, subtree: true });

  closeBtn.addEventListener("click", (e) => {
    e.preventDefault();
    e.stopPropagation();
    postChromeAction("close");
  });

  // Maximized state persists across navigations via sessionStorage so the
  // user doesn't have to re-tap [⛶] every time they change pages.
  // Same origin (e.g. views://main) → shared storage automatically.
  // Note: this requires the views:// scheme to be registered as secure
  // in the native backend (see WpeBackend::primeWpeView for WPE) — without
  // that, WebKit silently no-ops DOM storage on custom schemes.
  const MAXIMIZED_KEY = "__electrobun_chrome_maximized";
  let maximized = false;

  const updateMaximizeButton = () => {
    fsBtn.setAttribute(
      "aria-label",
      maximized ? "Restore window" : "Maximize window",
    );
    fsBtn.textContent = maximized ? "\u2750" : "\u26F6";
  };

  const concealChrome = () => {
    header.setAttribute("data-hidden", "");
    document.body.setAttribute("data-electrobun-chrome-concealed", "");
  };

  const revealChrome = () => {
    if (!maximized) return;
    header.removeAttribute("data-hidden");
    document.body.removeAttribute("data-electrobun-chrome-concealed");
  };

  const maximizeWindow = () => {
    maximized = true;
    document.body.setAttribute("data-electrobun-chrome-maximized", "");
    concealChrome();
    updateMaximizeButton();
    try { sessionStorage.setItem(MAXIMIZED_KEY, "1"); } catch {}
    postChromeAction("maximize");
  };

  const restoreWindow = () => {
    maximized = false;
    header.removeAttribute("data-hidden");
    document.body.removeAttribute("data-electrobun-chrome-concealed");
    document.body.removeAttribute("data-electrobun-chrome-maximized");
    updateMaximizeButton();
    try { sessionStorage.removeItem(MAXIMIZED_KEY); } catch {}
    postChromeAction("restore");
  };

  // Restore maximized + concealed state on injection if it was set on a
  // previous page in this webview.
  try {
    if (sessionStorage.getItem(MAXIMIZED_KEY) === "1") {
      maximized = true;
      document.body.setAttribute("data-electrobun-chrome-maximized", "");
      concealChrome();
    }
  } catch {}
  updateMaximizeButton();

  fsBtn.addEventListener("click", (e) => {
    e.preventDefault();
    e.stopPropagation();
    if (maximized) {
      restoreWindow();
    } else {
      maximizeWindow();
    }
  });

  restoreBtn.addEventListener("click", (e) => {
    e.preventDefault();
    e.stopPropagation();
    revealChrome();
  });

  // Pull down from the physical top edge to reveal the titlebar over the
  // still-maximized content. The titlebar's maximize button then performs
  // the actual restore. Merely tapping the edge is left entirely to the app,
  // so its own buttons keep working. We only claim the gesture after a
  // clearly vertical drag crosses the threshold. The visible tab is also a
  // larger, tappable reveal fallback.
  const PULL_START_ZONE_PX = 16;
  const PULL_THRESHOLD_PX = 36;
  let pull:
    | { identifier: number; startX: number; startY: number }
    | undefined;
  let suppressClickUntil = 0;

  const findTouch = (touches: TouchList, identifier: number) => {
    for (let i = 0; i < touches.length; i++) {
      const touch = touches.item(i);
      if (touch?.identifier === identifier) return touch;
    }
  };

  document.addEventListener("touchstart", (e) => {
    if (!header.hasAttribute("data-hidden") || e.touches.length !== 1) return;
    const touch = e.touches.item(0);
    if (!touch) return;
    const target = e.target;
    const startedOnHandle =
      target instanceof Node && restoreBtn.contains(target);
    if (touch.clientY > PULL_START_ZONE_PX && !startedOnHandle) return;
    pull = {
      identifier: touch.identifier,
      startX: touch.clientX,
      startY: touch.clientY,
    };
  }, { capture: true, passive: true });

  document.addEventListener("touchmove", (e) => {
    if (!pull || !header.hasAttribute("data-hidden")) return;
    const touch = findTouch(e.touches, pull.identifier);
    if (!touch) return;
    const dx = touch.clientX - pull.startX;
    const dy = touch.clientY - pull.startY;

    if (dy < PULL_THRESHOLD_PX || dy <= Math.abs(dx) * 1.2) return;

    pull = undefined;
    suppressClickUntil = Date.now() + 500;
    e.preventDefault();
    e.stopImmediatePropagation();
    revealChrome();
  }, { capture: true, passive: false });

  const finishPull = () => {
    pull = undefined;
  };
  document.addEventListener("touchend", finishPull, { capture: true });
  document.addEventListener("touchcancel", finishPull, { capture: true });

  // A browser may synthesize a click after touchend even though the pull was
  // consumed. Swallow only that short-lived click; ordinary taps are untouched.
  document.addEventListener("click", (e) => {
    if (Date.now() >= suppressClickUntil) return;
    e.preventDefault();
    e.stopImmediatePropagation();
  }, { capture: true });
}

export function initChrome() {
  if (window.__electrobunCompositedChrome) {
    initCompositedChromeGesture();
    return;
  }
  if (!window.__electrobunAutoInjectChrome) return;

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", injectChrome);
  } else {
    injectChrome();
  }
}
