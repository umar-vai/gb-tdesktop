// GB Downloader - in-page script: a "Download" button on every video and
// audio player, with a menu of the qualities the player loaded (or GB read
// from the video's own link).
(() => {
  if (window.__gbDownloader) return;
  window.__gbDownloader = true;
  const api = typeof browser !== "undefined" ? browser : chrome;
  const MIN_W = 200, MIN_H = 110, HIDE_AFTER = 2600;

  // Alt+click on a link: the browser keeps that download.
  document.addEventListener("click", event => {
    if (!event.altKey) return;
    const link = event.target && event.target.closest ? event.target.closest("a[href]") : null;
    if (link) { try { api.runtime.sendMessage({type: "gb-bypass"}); } catch (e) {} }
  }, true);

  api.runtime.onMessage.addListener((msg, _sender, sendResponse) => {
    if (msg && msg.type === "gb-fetch") {
      // A playlist only the player's own origin can read.
      fetch(msg.url, {credentials: "include"}).then(r => r.ok ? r.text() : "")
        .then(text => sendResponse({text}), () => sendResponse({text: ""}));
      return true;
    }
    if (msg && msg.type === "gb-autoplay") {
      // GB asked this tab to find the stream: start the player muted.
      const videos = Array.from(document.querySelectorAll("video"));
      for (const v of videos) {
        try { v.muted = true; if (v.paused) Promise.resolve(v.play()).catch(() => {}); } catch (e) {}
      }
      if (!videos.length || videos.every(v => v.paused)) {
        const selectors = [".jw-icon-display", ".jw-display-icon-container", ".vjs-big-play-button",
          ".plyr__control--overlaid", ".art-control-play", ".play-button", ".play-btn", "[aria-label*=play i]"];
        for (const s of selectors) {
          const el = document.querySelector(s);
          if (el) { try { el.click(); } catch (e) {} break; }
        }
      }
    }
  });

  let enabled = false, host = null, root = null, button = null, menu = null;
  let target = null, shownUntil = 0, menuOpen = false, timer = 0, lastMove = 0, count = 0;
  let pointerX = -1, pointerY = -1, pointerAt = 0;
  let medias = [];

  // ---------------------------------------------------------------- the video's own link
  // Many sites stream in byte-range pieces (video and audio apart) that cannot
  // be saved as one file; their own link (reel, post, watch page) can: GB
  // reads it and lists every quality with sound.
  const SITES = [
    {re: /(^|\.)youtube(-nocookie)?\.com$|(^|\.)youtu\.be$/, link: /\/watch\?(.*&)?v=|\/shorts\/|\/live\/|\/embed\//, page: /\/watch\?|\/shorts\/|\/live\/|\/embed\//},
    {re: /(^|\.)facebook\.com$|(^|\.)fb\.watch$/, link: /\/reel\/\d|\/videos\/|\/watch\/?\?v=|\/share\/[rv]\/|story_fbid=|\/posts\//, page: /\/reel\/\d|\/videos\/|\/watch\/?\?v=|story_fbid=/},
    {re: /(^|\.)instagram\.com$/, link: /\/(p|reel|reels|tv)\/[\w-]+/, page: /\/(p|reel|reels|tv)\/[\w-]+/},
    {re: /(^|\.)(twitter|x)\.com$/, link: /\/status\/\d+/, page: /\/status\/\d+/},
    {re: /(^|\.)tiktok\.com$/, link: /\/video\/\d+|\/photo\/\d+/, page: /\/video\/\d+/},
    {re: /(^|\.)threads\.(net|com)$/, link: /\/post\/[\w-]+/, page: /\/post\/[\w-]+/},
    {re: /(^|\.)reddit\.com$/, link: /\/comments\/\w+/, page: /\/comments\/\w+/},
    {re: /(^|\.)(vimeo|dailymotion|twitch|bilibili|pinterest|linkedin|rumble|streamable|vk|ok|soundcloud|bitchute|9gag|imgur|tumblr|snapchat|likee|kick)\.(com|tv|ru|be|tw)$/, link: null, page: null},
  ];
  const FEED_ITEMS = "ytd-rich-item-renderer, ytd-video-renderer, ytd-compact-video-renderer, ytd-reel-item-renderer, ytd-playlist-panel-video-renderer";

  function siteFor(hostname) {
    return SITES.find(site => site.re.test(hostname)) || null;
  }

  function videoLink(el) {
    const site = siteFor(location.hostname.toLowerCase());
    const here = location.href.split("#")[0];
    if (!site) return {url: here, known: false};
    const inList = el && el.closest && el.closest(FEED_ITEMS);
    if (!site.link || (site.page && site.page.test(location.pathname + location.search) && !inList)) {
      return {url: here, known: true};
    }
    // Feeds show many videos: take the link of the post that holds this one.
    let node = el;
    for (let depth = 0; node && depth < 25; depth++, node = node.parentElement) {
      const anchors = node.querySelectorAll ? node.querySelectorAll("a[href]") : [];
      for (const a of anchors) {
        const href = a.href || "";
        if (site.link.test(href) && !/[?&](comment_id|reply_comment_id)=/.test(href)) {
          try { const u = new URL(href, location.href); u.hash = ""; return {url: u.href, known: true}; } catch (e) {}
        }
      }
    }
    return {url: here, known: true};
  }

  function ask(message) {
    return new Promise(resolve => {
      try { Promise.resolve(api.runtime.sendMessage(message)).then(resolve, () => resolve(null)); }
      catch (e) { resolve(null); }
    });
  }

  function collect() {
    medias = Array.from(document.querySelectorAll("video, audio"));
  }

  // ---------------------------------------------------------------- links written in the page
  const MEDIA_LINK_RE = /\.(m3u8|mpd|mp4|webm|mkv|mov|m4v|m4a|mp3|ogg|opus|flac|wav)(\?|#|$)/i;
  const SCRIPT_PLAYLIST_RE = /https?:\\?\/\\?\/[^"'\s<>()]+?\.(?:m3u8|mpd)(?:\?[^"'\s<>()]*)?/gi;
  const reported = new Set();
  function absolute(url) {
    try { return new URL(String(url).replace(/\\\//g, "/").replace(/&amp;/g, "&"), location.href).href; } catch (e) { return ""; }
  }
  function scanPage() {
    if (!enabled) return;
    const urls = [];
    const add = url => {
      const href = absolute(url || "");
      if (href && /^https?:/i.test(href) && MEDIA_LINK_RE.test(href) && !reported.has(href)) { reported.add(href); urls.push(href); }
    };
    for (const el of document.querySelectorAll("video[src], audio[src], video source[src], audio source[src]")) {
      const player = el.closest("video, audio");
      if (player && player.tagName === "VIDEO") {
        // Thumbnail hover previews are small videos too: only real players count.
        const r = player.getBoundingClientRect();
        if (r.width < MIN_W || r.height < MIN_H) continue;
      }
      add(el.getAttribute("src"));
    }
    for (const el of document.querySelectorAll('meta[property="og:video"], meta[property="og:video:url"], meta[property="og:video:secure_url"], meta[name="twitter:player:stream"], meta[itemprop="contentUrl"]')) {
      add(el.getAttribute("content"));
    }
    for (const el of document.querySelectorAll('script[type="application/ld+json"]')) {
      const found = (el.textContent || "").match(/"contentUrl"\s*:\s*"[^"]+"/g) || [];
      for (const item of found) add(item.replace(/^"contentUrl"\s*:\s*"/, "").replace(/"$/, ""));
    }
    for (const el of document.querySelectorAll("script:not([src])")) {
      const text = el.textContent || "";
      if (text.length > 400000 || !/m3u8|mpd/i.test(text)) continue;
      for (const m of text.match(SCRIPT_PLAYLIST_RE) || []) add(m);
    }
    if (urls.length) ask({type: "gb-found", urls});
  }

  // ---------------------------------------------------------------- read qualities early
  const prefetched = new Set();
  let dwellTimer = 0;
  function prefetch(el) {
    if (!enabled) return;
    const video = videoLink(el);
    if (!video.known || prefetched.has(video.url)) return;
    const site = siteFor(location.hostname.toLowerCase());
    if (site && site.link && video.url === location.href.split("#")[0]
        && !(site.page && site.page.test(location.pathname + location.search))) return;
    prefetched.add(video.url);
    ask({type: "gb-analyze", url: video.url, pageUrl: location.href, prefetch: true}).then(r => {
      if (!r || !r.ok) prefetched.delete(video.url);
    });
  }

  function qualifies(el) {
    if (!el || !el.isConnected) return false;
    const r = el.getBoundingClientRect();
    if (el.tagName === "AUDIO") return el.controls && r.width >= 120 && r.height >= 20;
    if (r.width < MIN_W || r.height < MIN_H) return false;
    const style = getComputedStyle(el);
    return style.visibility !== "hidden" && style.display !== "none" && Number(style.opacity || 1) > 0.05;
  }

  function mediaAt(x, y) {
    let best = null, area = 0;
    for (const el of medias) {
      if (!qualifies(el)) continue;
      const r = el.getBoundingClientRect();
      if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom && r.width * r.height > area) { best = el; area = r.width * r.height; }
    }
    return best;
  }

  // ---------------------------------------------------------------- the button and menu (Telegram look)
  const ICON_DOWN = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.3" stroke-linecap="round" stroke-linejoin="round"><path d="M12 4v11"/><path d="m6.5 10 5.5 5.5 5.5-5.5"/><path d="M5 20h14"/></svg>';
  function build() {
    host = document.createElement("gb-download-button");
    host.style.cssText = "all:initial;position:fixed;left:0;top:0;width:0;height:0;z-index:2147483647;";
    root = host.attachShadow({mode: "open"});
    root.innerHTML = `
<style>
  :host{all:initial;
    --bg:#ffffff;--fg:#000000;--sub:#707579;--accent:#3390ec;--hover:rgba(112,117,121,.08);--line:#dfe1e5;
    --shadow:0 4px 24px rgba(0,0,0,.18),0 1px 4px rgba(0,0,0,.08)}
  @media (prefers-color-scheme: dark){:host{
    --bg:#17212b;--fg:#f5f5f5;--sub:#708499;--accent:#5eb5f7;--hover:rgba(255,255,255,.06);--line:#0e1621;
    --shadow:0 6px 28px rgba(0,0,0,.5)}}
  *{box-sizing:border-box;font-family:"Segoe UI","Open Sans",Roboto,system-ui,sans-serif;-webkit-font-smoothing:antialiased}
  .btn{position:fixed;display:none;align-items:center;gap:6px;height:32px;padding:0 12px 0 10px;border:0;
    border-radius:16px;background:rgba(0,0,0,.55);color:#fff;font-size:13px;font-weight:600;cursor:pointer;
    box-shadow:0 2px 10px rgba(0,0,0,.35);backdrop-filter:blur(8px);white-space:nowrap;line-height:1;user-select:none;
    transition:background .15s ease}
  .btn:hover{background:#3390ec}
  .btn svg{width:16px;height:16px;flex:none}
  .logo{width:18px;height:18px;border-radius:50%;background:#3390ec;display:inline-flex;align-items:center;justify-content:center;flex:none}
  .btn:hover .logo{background:rgba(255,255,255,.22)}
  .logo svg{width:12px;height:12px}
  .count{min-width:18px;height:18px;padding:0 5px;border-radius:9px;background:rgba(255,255,255,.22);font-size:11px;display:inline-flex;align-items:center;justify-content:center}
  .count:empty{display:none}
  .menu{position:fixed;display:none;width:340px;max-width:calc(100vw - 16px);overflow:auto;border-radius:12px;
    background:var(--bg);color:var(--fg);box-shadow:var(--shadow);padding:6px 0;font-size:14px;animation:pop .14s ease-out}
  @keyframes pop{from{opacity:0;transform:scale(.96) translateY(-4px)}to{opacity:1;transform:none}}
  .head{display:flex;align-items:center;gap:10px;padding:8px 14px 6px}
  .head .logo{width:28px;height:28px}
  .head .logo svg{width:16px;height:16px;color:#fff}
  .head .text{min-width:0;flex:1}
  .head .name{font-weight:600;font-size:14px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
  .head .meta{font-size:12.5px;color:var(--sub);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
  .section{padding:8px 14px 4px;font-size:13px;font-weight:600;color:var(--accent)}
  .item{display:flex;justify-content:space-between;align-items:center;gap:12px;padding:9px 14px;cursor:pointer;transition:background .1s}
  .item:hover{background:var(--hover)}
  .item.best .label{font-weight:600}
  .item.best .label::after{content:"";display:inline-block;width:6px;height:6px;border-radius:50%;background:var(--accent);margin-left:8px;vertical-align:middle}
  .item.disabled{cursor:default;opacity:.6}
  .item.disabled:hover{background:none}
  .label{white-space:nowrap}
  .detail{color:var(--sub);font-size:12.5px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
  .sep{height:1px;background:var(--line);margin:6px 0;opacity:.7}
  .muted{padding:12px 14px;color:var(--sub);line-height:1.4;display:flex;align-items:center;gap:10px}
  .spin{width:16px;height:16px;border-radius:50%;border:2px solid var(--accent);border-right-color:transparent;animation:spin .8s linear infinite;flex:none}
  @keyframes spin{to{transform:rotate(360deg)}}
  .foot .label{color:var(--sub);font-size:13px}
</style>
<div class="btn" part="button"><span class="logo">${ICON_DOWN}</span><span class="text">Download</span><span class="count"></span></div>
<div class="menu" role="menu"></div>`;
    button = root.querySelector(".btn");
    menu = root.querySelector(".menu");
    for (const type of ["pointerdown", "mousedown", "mouseup", "click", "dblclick", "contextmenu", "touchstart"]) {
      host.addEventListener(type, e => e.stopPropagation());
    }
    button.addEventListener("click", e => { e.preventDefault(); menuOpen ? closeMenu() : openMenu(); });
    setCount(count);
    mount();
  }

  function mount() {
    const fs = document.fullscreenElement || document.webkitFullscreenElement;
    const parent = fs && fs.tagName !== "VIDEO" ? fs : document.documentElement;
    if (host && host.parentNode !== parent) parent.appendChild(host);
  }

  function place() {
    if (!target || !qualifies(target)) return false;
    const r = target.getBoundingClientRect();
    const vw = document.documentElement.clientWidth || innerWidth;
    const bw = button.offsetWidth || 120;
    const left = Math.max(4, Math.min(r.right - bw - 10, vw - bw - 4));
    const top = Math.max(4, r.top + 10);
    button.style.left = left + "px";
    button.style.top = top + "px";
    if (menuOpen) {
      const mw = Math.min(340, vw - 16);
      menu.style.left = Math.max(8, Math.min(left + bw - mw, vw - mw - 8)) + "px";
      menu.style.top = (top + 38) + "px";
      menu.style.maxHeight = Math.max(160, innerHeight - top - 54) + "px";
    }
    return true;
  }

  function show(el, ms) {
    if (!enabled || !el) return;
    if (!host) build();
    if (target !== el && menuOpen) closeMenu();
    target = el;
    shownUntil = Math.max(shownUntil, Date.now() + (ms || HIDE_AFTER));
    mount();
    if (place()) button.style.display = "inline-flex";
    if (!timer) timer = setInterval(tick, 200);
  }

  function hide() {
    if (button) button.style.display = "none";
    closeMenu();
    clearInterval(timer); timer = 0; target = null;
  }

  function tick() {
    if (menuOpen) { if (!place()) hide(); return; }
    if (!place()) { hide(); return; }
    const now = Date.now();
    const r = target.getBoundingClientRect();
    const over = pointerX >= r.left && pointerX <= r.right && pointerY >= r.top && pointerY <= r.bottom;
    if (over && now - pointerAt < 6000) shownUntil = Math.max(shownUntil, now + 600);
    if (now > shownUntil) hide();
  }

  function setCount(n) {
    count = n || 0;
    if (root) root.querySelector(".count").textContent = count ? String(count) : "";
  }

  function flash(text, ms) {
    if (!button) return;
    const label = root.querySelector(".text");
    label.textContent = text;
    shownUntil = Date.now() + (ms || 2600);
    setTimeout(() => { label.textContent = "Download"; }, ms || 2600);
  }

  function row(cls, label, detail) {
    const div = document.createElement("div");
    div.className = "item " + cls;
    const a = document.createElement("span"); a.className = "label"; a.textContent = label;
    const b = document.createElement("span"); b.className = "detail"; b.textContent = detail || "";
    div.append(a, b);
    return div;
  }

  function section(text) {
    return Object.assign(document.createElement("div"), {className: "section", textContent: text});
  }

  function waiting(text) {
    const div = document.createElement("div");
    div.className = "muted";
    div.append(Object.assign(document.createElement("span"), {className: "spin"}), document.createTextNode(text));
    return div;
  }

  async function send(message) {
    closeMenu();
    flash("Sending…", 8000);
    const result = await ask(message);
    flash(result && result.ok
      ? (result.asked ? "✓ Opened in GB" : "✓ Added to GB")
      : ((result && result.error) || "Could not send"), 3500);
  }

  let renderSeq = 0;

  function fmtLength(sec) {
    sec = Math.round(Number(sec) || 0);
    if (!sec) return "";
    const h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60), s = sec % 60;
    return (h ? h + ":" + String(m).padStart(2, "0") : m) + ":" + String(s).padStart(2, "0");
  }

  const PLACEHOLDER_RE = /^(untitled( video)?|no title|video|media|browser media|media[ _]download|download|watch|player|embed|index|master|playlist|stream|videoplayback)$/i;
  function goodTitle(text) {
    text = String(text || "").replace(/\s+/g, " ").trim();
    return text && !PLACEHOLDER_RE.test(text) ? text.slice(0, 180) : "";
  }
  function pageTitle() {
    const meta = document.querySelector('meta[property="og:title"], meta[name="twitter:title"]');
    for (const raw of [document.title, meta && meta.getAttribute("content")]) {
      let text = String(raw || "").replace(/\s+/g, " ").trim();
      const parts = text.split(/\s+[-|–—]\s+/);
      if (parts.length > 1 && parts[parts.length - 1].length <= 30) text = parts.slice(0, -1).join(" - ");
      text = goodTitle(text.replace(/^\(\d+\)\s*/, "").replace(/^watch\s+/i, ""));
      if (text) return text;
    }
    return "";
  }
  function heightOf(label) {
    const m = /(\d{3,4})p\b/.exec(String(label || ""));
    return m ? Number(m[1]) : 0;
  }
  function sep() { return Object.assign(document.createElement("div"), {className: "sep"}); }

  // One list: the stream the player already loaded is shown at once; on known
  // video sites GB also reads the video's own link, and its list replaces the
  // captured one only when it offers a higher quality.
  async function renderMenu() {
    const seq = ++renderSeq;
    const data = await ask({type: "gb-menu"});
    if (!menuOpen || seq !== renderSeq) return;
    menu.textContent = "";
    const groups = (data && data.groups) || [];
    const video = videoLink(target);
    const ownPage = !video.known || video.url === location.href.split("#")[0];
    const stream = ownPage ? (groups.find(g => !g.drm && g.master) || groups.find(g => !g.drm) || groups[0] || null) : null;
    let items = stream ? stream.items.slice() : [];
    const src = target && (target.currentSrc || target.src);
    if (!items.length && src && /^https?:/i.test(src)) {
      const audio = target.tagName === "AUDIO";
      items = [{url: src, referer: location.href, quality: "Best available", best: true, mode: audio ? "Audio" : "",
        label: (audio ? "Audio" : "Video") + " file", detail: ""}];
    }
    let siteName = "";
    try { siteName = new URL(video.url).hostname.replace(/^www\./, ""); } catch (e) {}

    const head = document.createElement("div");
    head.className = "head";
    const logo = Object.assign(document.createElement("span"), {className: "logo"});
    logo.innerHTML = ICON_DOWN;
    const text = Object.assign(document.createElement("div"), {className: "text"});
    const name = Object.assign(document.createElement("div"), {className: "name"});
    const meta = Object.assign(document.createElement("div"), {className: "meta"});
    text.append(name, meta);
    head.append(logo, text);
    menu.appendChild(head);
    menu.appendChild(section("Quality"));
    const body = document.createElement("div");
    menu.appendChild(body);
    let shownTitle = pageTitle() || goodTitle(stream && stream.title);
    const setTitle = (title, duration) => {
      if (title) shownTitle = title;
      name.textContent = shownTitle || location.hostname;
      name.title = shownTitle;
      meta.textContent = [siteName, duration ? fmtLength(duration) : "", stream ? stream.kind.toUpperCase() : ""]
        .filter(Boolean).join(" · ");
    };
    setTitle("", stream && stream.duration);

    const showItems = list => {
      body.textContent = "";
      for (const item of list) {
        const el = row((item.best ? "best " : "") + (item.disabled ? "disabled" : ""), item.label, item.detail);
        if (!item.disabled) el.addEventListener("click", () => send({type: "gb-send", item: {...item, title: goodTitle(item.title) || shownTitle}}));
        body.appendChild(el);
      }
      place();
    };
    const capturedBest = Math.max(0, ...items.map(i => heightOf(i.label)));

    async function lookUp(wait) {
      if (wait) {
        body.textContent = "";
        body.appendChild(waiting("Finding qualities…"));
        place();
      }
      const r = await ask({type: "gb-analyze", url: video.url, pageUrl: location.href});
      if (!menuOpen || seq !== renderSeq) return;
      if (r && r.ok) {
        const title = goodTitle(r.title) || shownTitle;
        const qualities = Array.isArray(r.qualities) ? r.qualities : [];
        const base = {url: video.url, referer: location.href, pageUrl: video.url, title, qualities};
        const list = [{...base, quality: "Best available", best: true,
          label: "Best quality" + (qualities[0] ? " · " + qualities[0] : ""), detail: "video + audio"}];
        for (const q of qualities) list.push({...base, quality: q, label: q, detail: ""});
        list.push({...base, quality: "Best available", mode: "Audio", label: "Audio only", detail: "MP3"});
        if (!items.length || heightOf(list[0].label) > capturedBest) {
          setTitle(title, r.duration);
          showItems(list);
        } else {
          setTitle(title, (stream && stream.duration) || r.duration);
        }
        return;
      }
      if (items.length) return;
      body.textContent = "";
      const fail = row("disabled", "Could not read this video", (r && r.error) || "");
      fail.title = (r && r.error) || "";
      body.appendChild(fail);
      const retry = row("", "Try again", "");
      retry.addEventListener("click", () => lookUp(true));
      body.appendChild(retry);
      const anyway = row("", "Send the link to GB anyway", "");
      anyway.addEventListener("click", () => send({type: "gb-send", item: {url: video.url, referer: location.href,
        pageUrl: video.url, quality: "Best available", title: shownTitle}}));
      body.appendChild(anyway);
      place();
    }

    if (items.length) showItems(items);
    if (video.known || !items.length) lookUp(!items.length);
    menu.appendChild(sep());
    const page = row("foot", "Send page link instead", "GB finds the best quality");
    page.addEventListener("click", () => send({type: "gb-send-page", title: shownTitle}));
    menu.appendChild(page);
    const block = row("foot", "Never show on this site", "");
    block.addEventListener("click", async () => { await ask({type: "gb-block-site"}); enabled = false; hide(); });
    menu.appendChild(block);
    place();
  }

  function openMenu() {
    menuOpen = true;
    menu.style.display = "block";
    menu.textContent = "";
    menu.appendChild(waiting("Loading…"));
    place();
    renderMenu();
  }

  function closeMenu() {
    menuOpen = false;
    if (menu) menu.style.display = "none";
    shownUntil = Date.now() + 1200;
  }

  function onMove(e) {
    const now = Date.now();
    pointerX = e.clientX; pointerY = e.clientY; pointerAt = now;
    if (now - lastMove < 80) return;
    lastMove = now;
    const el = mediaAt(e.clientX, e.clientY);
    if (el) {
      show(el);
      clearTimeout(dwellTimer);
      dwellTimer = setTimeout(() => { if (target === el) prefetch(el); }, 450);
    }
  }

  async function start() {
    const state = await ask({type: "gb-hello"});
    enabled = !!(state && state.enabled);
    if (!enabled) { hide(); return; }
    collect();
    if (start.done) return;
    start.done = true;
    let pendingScan = 0, pendingPageScan = 0;
    new MutationObserver(() => {
      if (!pendingScan) pendingScan = setTimeout(() => { pendingScan = 0; collect(); }, 400);
      if (!pendingPageScan) pendingPageScan = setTimeout(() => { pendingPageScan = 0; scanPage(); }, 1200);
    }).observe(document.documentElement, {childList: true, subtree: true});
    if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", () => { collect(); scanPage(); }, {once: true});
    else scanPage();
    document.addEventListener("mousemove", onMove, {capture: true, passive: true});
    if (window === window.top) {
      // On a video's own page read the qualities as soon as it opens, so the
      // menu is ready on the first click. Single-page sites change the address
      // without a reload: check it every second.
      let lastPage = "";
      const early = () => {
        const here = location.href.split("#")[0];
        if (here === lastPage || !enabled) return;
        lastPage = here;
        const site = siteFor(location.hostname.toLowerCase());
        if (site && site.page && site.page.test(location.pathname + location.search)) prefetch(null);
      };
      early();
      setInterval(early, 1000);
    }
    document.addEventListener("playing", e => {
      if (!(e.target instanceof HTMLMediaElement)) return;
      collect();
      if (qualifies(e.target)) { show(e.target, 4000); prefetch(e.target); }
      scanPage();
    }, true);
    document.addEventListener("pointerdown", e => { if (menuOpen && !e.composedPath().includes(host)) closeMenu(); }, true);
    document.addEventListener("keydown", e => { if (e.key === "Escape" && menuOpen) closeMenu(); }, true);
    document.addEventListener("fullscreenchange", mount);
    document.addEventListener("webkitfullscreenchange", mount);
    addEventListener("scroll", () => { if (target) place(); }, {capture: true, passive: true});
    addEventListener("resize", () => { if (target) place(); });
    api.runtime.onMessage.addListener(msg => {
      if (!msg || msg.type !== "gb-streams-changed") return;
      const grew = (msg.count || 0) > count;
      setCount(msg.count);
      if (menuOpen) renderMenu();
      else if (grew && target) show(target, 3000);
    });
    api.storage.onChanged.addListener((changes, area) => {
      if (area === "local" && (changes.blocked || changes.overlay)) start();
    });
    const data = await ask({type: "gb-menu"});
    setCount(data && data.groups ? data.groups.filter(g => !g.items.every(i => i.disabled)).length : 0);
  }

  start();
})();
