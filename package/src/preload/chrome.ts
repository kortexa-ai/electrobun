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

const CHROME_HTML = `
<header data-electrobun-chrome="true" style="
  position: fixed;
  top: 0; left: 0; right: 0;
  height: 44px;
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
    <button data-electrobun-chrome-fs type="button" aria-label="Fullscreen" style="
      appearance: none; -webkit-appearance: none;
      border: none; background: transparent;
      color: #fff5e6;
      font: inherit; font-size: 22px; line-height: 1;
      width: 44px; height: 44px;
      border-radius: 8px;
      cursor: pointer;
    ">&#x26F6;</button>
    <button data-electrobun-chrome-close type="button" aria-label="Close" style="
      appearance: none; -webkit-appearance: none;
      border: none; background: transparent;
      color: #cc3300;
      font: inherit; font-size: 22px; font-weight: 700; line-height: 1;
      width: 44px; height: 44px;
      border-radius: 8px;
      cursor: pointer;
    ">&#x2715;</button>
  </div>
</header>
<style data-electrobun-chrome-style>
  /* Nudge the document down so the chrome doesn't overlap content. We
     can't modify the body itself (apps may have already laid it out)
     but we can add a marker margin-top via a body padding shim. */
  body { padding-top: 44px !important; box-sizing: border-box; }
  body[data-electrobun-chrome-hidden] { padding-top: 0 !important; }
  /* !important needed because the chrome <header> has display:flex in its
     inline style (which wins over plain external CSS otherwise). */
  [data-electrobun-chrome="true"][data-hidden] { display: none !important; }
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
  const titleEl = document.querySelector<HTMLElement>('[data-electrobun-chrome-title]');
  if (!header || !closeBtn || !fsBtn || !titleEl) return;

  titleEl.textContent = document.title || "";
  // Keep the title in sync if the app sets document.title later.
  const titleObserver = new MutationObserver(() => {
    titleEl.textContent = document.title || "";
  });
  const titleNode = document.querySelector("title");
  if (titleNode) titleObserver.observe(titleNode, { childList: true, subtree: true });

  closeBtn.addEventListener("click", (e) => {
    e.stopPropagation();
    // WPE's navigation callback is synchronous and survives runtimes where
    // async FFI message callbacks are delayed. The backend blocks this
    // internal URL, then the Bun-side navigation event closes this view's
    // BrowserWindow (or quits when it is the last one).
    window.location.href = CHROME_CLOSE_URL;
  });

  // Fullscreen state persists across navigations via sessionStorage so
  // the user doesn't have to re-tap [⛶] every time they change pages.
  // Same origin (e.g. views://main) → shared storage automatically.
  // Note: this requires the views:// scheme to be registered as secure
  // in the native backend (see WpeBackend::primeWpeView for WPE) — without
  // that, WebKit silently no-ops DOM storage on custom schemes.
  const HIDE_KEY = "__electrobun_chrome_hidden";
  let lastHideAt = 0;
  const setHidden = (hidden: boolean) => {
    if (hidden) {
      header.setAttribute("data-hidden", "");
      document.body.setAttribute("data-electrobun-chrome-hidden", "");
      try { sessionStorage.setItem(HIDE_KEY, "1"); } catch {}
      lastHideAt = Date.now();
    } else {
      header.removeAttribute("data-hidden");
      document.body.removeAttribute("data-electrobun-chrome-hidden");
      try { sessionStorage.removeItem(HIDE_KEY); } catch {}
    }
  };
  // Restore hidden state on injection if it was set on a previous page.
  try {
    if (sessionStorage.getItem(HIDE_KEY) === "1") setHidden(true);
  } catch {}

  fsBtn.addEventListener("click", (e) => {
    e.stopPropagation();
    setHidden(true);
  });
  // Restore chrome only on a deliberate top-edge tap (within the strip of
  // pixels the chrome bar normally occupies). The original "tap anywhere"
  // gesture is hostile to touch UX — every interactive element on the page
  // (including app navigation buttons) would restore chrome before the
  // app's own onclick fired, which also defeats sessionStorage persistence
  // because the navigating tap clears the hide flag pre-navigation.
  // Top-edge restore is discoverable (chrome was visible there) and
  // disjoint from normal interactions further down the page.
  const RESTORE_ZONE_PX = 44; // matches CHROME_HTML's header height
  document.addEventListener("click", (e) => {
    if (!header.hasAttribute("data-hidden")) return;
    if (Date.now() - lastHideAt < 250) return;
    if (typeof e.clientY === "number" && e.clientY > RESTORE_ZONE_PX) return;
    setHidden(false);
  });
}

export function initChrome() {
  if (!window.__electrobunAutoInjectChrome) return;

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", injectChrome);
  } else {
    injectChrome();
  }
}
