// GB Downloader - background service worker.
//
// Watches each tab's network traffic for real media streams (HLS .m3u8, DASH
// .mpd and plain video/audio files), reads HLS master playlists to list every
// quality, and sends what the user picks to the GB app on this computer
// (127.0.0.1, ports 47740-47749) with the Referer/Origin/User-Agent the
// player used and the page's cookies.

const api = typeof browser !== "undefined" ? browser : chrome;
const APP = "GB";
const PORTS = Array.from({length: 10}, (_, i) => 47740 + i);
const URL_MEDIA_RE = /\.(m3u8|mpd|mp4|webm|mkv|mov|m4v|m4a|mp3|ogg|oga|opus|flac|wav)(\?|#|$)/i;
const SEGMENT_RE = /\.(ts|m4s|m4f|cmfv|cmfa|vtt|webvtt|srt|key|jpg|jpeg|png|webp|gif|js|css|json|html?)(\?|#|$)/i;
const SKIP_URL_RE = /[?&](range|bytestart|byteend)=|\/videoplayback\b|\/seg-\d+|\/segment[-_]?\d+/i;
const MIN_FILE_BYTES = 300 * 1024;
const MAX_STREAMS = 12;

const DEFAULT_CAPTURE_TYPES = "zip rar 7z tar gz tgz bz2 xz iso img bin exe msi msix apk dmg deb rpm pdf doc docx " +
  "xls xlsx ppt pptx epub torrent mp4 mkv webm mov avi wmv flv m4v 3gp mp3 m4a aac flac wav ogg opus wma";
let settings = {blocked: "", overlay: true, capture: true, captureTypes: DEFAULT_CAPTURE_TYPES, minSizeKb: 0};
const tabs = new Map();       // tabId -> {gen, navGen, origin, page, streams: [], children: []}
const pending = new Map();    // requestId -> captured request headers
const ownFetches = new Set(); // playlist URLs read by the extension itself

// ------------------------------------------------------------------ settings / blocked sites
async function loadSettings() {
  const data = await api.storage.local.get(["blocked", "overlay", "capture", "captureTypes", "minSizeKb"]);
  settings = {
    blocked: String(data.blocked || ""),
    overlay: data.overlay !== false,
    capture: data.capture !== false,
    captureTypes: typeof data.captureTypes === "string" ? data.captureTypes : DEFAULT_CAPTURE_TYPES,
    minSizeKb: Math.max(0, Number(data.minSizeKb) || 0),
  };
}
loadSettings();
api.storage.onChanged.addListener((changes, area) => {
  if (area === "local" && (changes.blocked || changes.overlay || changes.capture || changes.captureTypes ||
      changes.minSizeKb)) loadSettings();
});

function blockedRules() {
  return settings.blocked.split(/[\r\n,]+/).map(line => line.trim().toLowerCase())
    .filter(line => line && !line.startsWith("#"))
    .map(line => line.replace(/^[a-z]+:\/\//, "").replace(/^\*\./, "").replace(/^www\./, "").split(/[\/:?#\s]/)[0])
    .filter(Boolean);
}

function hostOf(url) {
  try { return new URL(url).hostname.toLowerCase(); } catch (e) { return ""; }
}

function isBlocked(url) {
  const host = hostOf(url);
  if (!host) return false;
  return blockedRules().some(rule => host === rule || host.endsWith("." + rule));
}

// ------------------------------------------------------------------ connection to GB
let appCapture = true;   // GB's "catch the browser's file downloads" option, from /ping
let appEngine = true;    // whether GB found yt-dlp
async function findPort() {
  const cached = (await api.storage.local.get("port")).port;
  const order = cached ? [cached, ...PORTS.filter(p => p !== cached)] : PORTS;
  for (const port of order) {
    try {
      const r = await fetch(`http://127.0.0.1:${port}/ping`, {cache: "no-store"});
      const j = await r.json();
      if (j && j.app === APP) {
        appCapture = j.capture !== false;
        appEngine = j.engine !== false;
        if (port !== cached) await api.storage.local.set({port});
        return port;
      }
    } catch (e) {}
  }
  return 0;
}

// Self-pairing: GB hands its token to browser extensions only (web pages
// cannot fake the Origin header), so nothing has to be copied or pasted.
async function pairWithApp(port) {
  try {
    // POST: Chrome sends no Origin header on an extension's GET requests.
    const r = await fetch(`http://127.0.0.1:${port}/pair`, {
      method: "POST",
      cache: "no-store",
      headers: {"Content-Type": "application/json"},
      body: "{}",
    });
    const j = await r.json();
    const token = String((j && j.token) || "").trim();
    if (r.ok && token) { await api.storage.local.set({token}); return token; }
  } catch (e) {}
  return "";
}

async function getToken() {
  return String((await api.storage.local.get("token")).token || "").trim();
}

// POST to GB, finding it and (re)pairing as needed.
async function appRequest(path, body) {
  const port = await findPort();
  if (!port) return {ok: false, status: 0, error: "GB is not running."};
  let token = await getToken();
  if (!token) token = await pairWithApp(port);
  for (let attempt = 0; attempt < 2; attempt++) {
    try {
      const r = await fetch(`http://127.0.0.1:${port}${path}`, {
        method: "POST",
        headers: {"Content-Type": "application/json", "X-GB-Token": token},
        body: JSON.stringify(body || {}),
      });
      const j = await r.json().catch(() => ({}));
      if (r.status === 401 && attempt === 0) {
        const fresh = await pairWithApp(port);
        if (fresh) { token = fresh; continue; }
      }
      return {...j, ok: r.ok && !!j.ok, status: r.status};
    } catch (e) {
      try { await api.storage.local.remove("port"); } catch (err) {}
      return {ok: false, status: 0, error: "Could not reach GB."};
    }
  }
  return {ok: false, status: 401, error: "GB refused the pairing."};
}

async function connectionState() {
  const port = await findPort();
  if (!port) return {connected: false};
  let token = await getToken();
  if (!token) token = await pairWithApp(port);
  return {connected: !!token, port, capture: appCapture, engine: appEngine};
}

async function cookiesFor(url) {
  if (!url || !/^https?:/i.test(url)) return [];
  try { return await api.cookies.getAll({url}); } catch (e) { return []; }
}

async function cookiesForAll(urls) {
  const seen = new Set();
  const keys = new Set();
  let out = [];
  for (const source of urls) {
    if (!source || seen.has(source)) continue;
    seen.add(source);
    for (const c of await cookiesFor(source)) {
      const key = c.domain + "\t" + c.path + "\t" + c.name;
      if (!keys.has(key)) { keys.add(key); out.push(c); }
    }
  }
  return out;
}

function cleanTitle(title) {
  let text = String(title || "").replace(/\s+/g, " ").trim();
  const parts = text.split(/\s+[-|–—]\s+/);
  if (parts.length > 1 && parts[parts.length - 1].length <= 30) text = parts.slice(0, -1).join(" - ");
  return text.replace(/^\(\d+\)\s*/, "").replace(/^watch\s+/i, "").replace(/\s+online(\s+free)?$/i, "")
    .replace(/\s+in\s+(full\s+)?hd$/i, "").slice(0, 180);
}

// payload: {url, referer, headers, title, quality, mode, pageUrl, kind, fileName, fileSize, mime}
async function sendToApp(payload) {
  const cookies = await cookiesForAll([payload.url, payload.referer, payload.pageUrl]);
  const result = await appRequest("/download", {
    url: payload.url,
    referer: payload.referer || "",
    headers: payload.headers || {},
    title: payload.title || "",
    quality: payload.quality || "",
    mode: payload.mode || "",
    pageUrl: payload.pageUrl || "",
    kind: payload.kind || "",
    fileName: payload.fileName || "",
    fileSize: payload.fileSize || 0,
    mime: payload.mime || "",
    qualities: Array.isArray(payload.qualities) ? payload.qualities : [],
    cookies,
  });
  if (result.ok) return {ok: true, asked: !!result.asked};
  return {ok: false, error: result.error || "GB refused the link."};
}

// "Which qualities does this video have?" - GB reads the video's own link
// with yt-dlp, using this browser's cookies for logged-in sites.
const analyzeCache = new Map(); // url -> {time, promise}
let prefetching = 0;
function analyzeVideo(url, pageUrl, prefetch) {
  const hit = analyzeCache.get(url);
  if (hit && Date.now() - hit.time < 8 * 60 * 1000) return hit.promise;
  if (prefetch && prefetching >= 2) return Promise.resolve({ok: false, skipped: true});
  if (prefetch) prefetching++;
  const promise = (async () => {
    const cookies = await cookiesForAll([url, pageUrl]);
    const result = await appRequest("/analyze", {url, pageUrl: pageUrl || "", cookies});
    if (!result.ok) analyzeCache.delete(url);
    return result;
  })();
  if (prefetch) promise.finally(() => { prefetching--; });
  analyzeCache.set(url, {time: Date.now(), promise});
  if (analyzeCache.size > 60) analyzeCache.delete(analyzeCache.keys().next().value);
  return promise;
}

// Tell GB this browser has the extension, so a link that only plays in a
// real browser can be opened here (see "gb-resolve" below).
let lastHello = 0;
function sayHello() {
  if (Date.now() - lastHello < 120000) return;
  lastHello = Date.now();
  appRequest("/hello", {browser: navigator.userAgent}).then(r => { if (!r.ok) lastHello = Date.now() - 90000; });
}
sayHello();

function notify(message) {
  try { api.notifications.create({type: "basic", iconUrl: "icons/icon128.png", title: "GB Downloader", message}); } catch (e) {}
}

async function sendAndNotify(payload) {
  const result = await sendToApp(payload);
  if (!result.asked) notify(result.ok ? "Added to GB downloads." : result.error);
  return result;
}

// ------------------------------------------------------------------ per-tab stream store
async function tabState(tabId) {
  if (tabs.has(tabId)) return tabs.get(tabId);
  let state = null;
  try { state = (await api.storage.session.get("t" + tabId))["t" + tabId]; } catch (e) {}
  if (!tabs.has(tabId)) tabs.set(tabId, state || {gen: 0, navGen: 0, origin: "", streams: [], children: []});
  return tabs.get(tabId);
}

function persist(tabId) {
  const state = tabs.get(tabId);
  try { if (state) api.storage.session.set({["t" + tabId]: state}); } catch (e) {}
}

function forget(tabId) {
  tabs.delete(tabId);
  try { api.storage.session.remove("t" + tabId); } catch (e) {}
  if (resolving.has(tabId)) finishResolve(tabId, [], false);
}

// ------------------------------------------------------------------ links GB could not read
// When yt-dlp cannot read a link, GB opens it here with "#gb-resolve=<job>".
// This tab starts the player, waits for its stream and reports it (best: the
// master playlist with every quality and audio) back, then closes.
const RESOLVE_RE = /[#&]gb-resolve=([0-9a-f]{16,40})/;
const resolving = new Map(); // tabId -> {job, started, firstStream, plays, timer, done}

function startResolve(tabId, job) {
  const current = resolving.get(tabId);
  if (current && current.job === job) return;
  if (current) clearInterval(current.timer);
  const entry = {job, started: Date.now(), firstStream: 0, plays: 0, done: false, timer: 0};
  entry.timer = setInterval(() => resolveTick(tabId), 1500);
  resolving.set(tabId, entry);
}

function resolveTick(tabId) {
  const entry = resolving.get(tabId);
  if (!entry || entry.done) return;
  const now = Date.now();
  if (entry.plays < 14) {
    entry.plays++;
    try { Promise.resolve(api.tabs.sendMessage(tabId, {type: "gb-autoplay"})).catch(() => {}); } catch (e) {}
  }
  const state = tabs.get(tabId);
  const streams = state ? currentStreams(state).filter(s => !s.drm) : [];
  if (streams.length && !entry.firstStream) entry.firstStream = now;
  const hasMaster = streams.some(s => s.master);
  const ready = streams.length && ((hasMaster && now - entry.firstStream > 2500) || now - entry.firstStream > 9000);
  if (ready) finishResolve(tabId, streams, true);
  else if (now - entry.started > 42000) finishResolve(tabId, streams, true);
}

async function finishResolve(tabId, streams, closeTab) {
  const entry = resolving.get(tabId);
  if (!entry || entry.done) return;
  entry.done = true;
  clearInterval(entry.timer);
  resolving.delete(tabId);
  let tab = null;
  try { tab = await api.tabs.get(tabId); } catch (e) {}
  const sorted = [...streams].sort((a, b) => (Number(!!b.master) - Number(!!a.master)) || (b.gen - a.gen));
  const out = [];
  for (const s of sorted.slice(0, 6)) {
    const headers = {...(s.headers || {})};
    const cookies = await cookiesFor(s.url);
    if (cookies.length) headers.Cookie = cookies.map(c => c.name + "=" + c.value).join("; ");
    out.push({
      url: s.url, kind: s.kind, master: !!s.master, referer: s.referer, headers,
      heights: (s.variants || []).map(v => v.height).filter(Boolean),
    });
  }
  await appRequest("/resolved", {
    id: entry.job,
    title: cleanTitle(tab && tab.title),
    pageUrl: tab && tab.url ? tab.url.replace(RESOLVE_RE, "") : "",
    streams: out,
  });
  if (closeTab && tab) { try { await api.tabs.remove(tabId); } catch (e) {} }
}

function announce(tabId) {
  const state = tabs.get(tabId);
  const count = state ? currentStreams(state).filter(s => !s.drm).length : 0;
  try {
    Promise.resolve(api.action.setBadgeBackgroundColor({tabId, color: "#3390ec"})).catch(() => {});
    Promise.resolve(api.action.setBadgeText({tabId, text: count ? String(count) : ""})).catch(() => {});
  } catch (e) {}
  try { Promise.resolve(api.tabs.sendMessage(tabId, {type: "gb-streams-changed", count})).catch(() => {}); } catch (e) {}
}

function header(list, name) {
  const item = (list || []).find(h => h.name.toLowerCase() === name);
  return item ? String(item.value || "") : "";
}

function classify(url, contentType, length, status, contentRange) {
  if (!/^https?:/i.test(url) || SKIP_URL_RE.test(url)) return null;
  const ct = contentType.toLowerCase().split(";")[0].trim();
  const urlMatch = (url.match(URL_MEDIA_RE) || [])[1];
  const ext = urlMatch ? urlMatch.toLowerCase() : "";
  if (ct.includes("mpegurl") || ext === "m3u8") return {kind: "hls", ext: "m3u8"};
  if (ct.includes("dash+xml") || ext === "mpd") return {kind: "dash", ext: "mpd"};
  if (ct === "video/mp2t" || ct.includes("iso.segment") || SEGMENT_RE.test(url)) return null;
  const isMediaType = /^(video|audio)\//.test(ct);
  if (!isMediaType && !(ext && (!ct || ct === "application/octet-stream" || ct === "binary/octet-stream"))) return null;
  let total = length;
  const range = /\/(\d+)\s*$/.exec(contentRange || "");
  if (range) total = Number(range[1]);
  if (total && total < MIN_FILE_BYTES) return null;
  if (!total && status !== 206) return null;
  const fromType = (ct.split("/")[1] || "").replace("x-matroska", "mkv").replace("mpeg", "mp3").replace("quicktime", "mov");
  return {kind: "file", ext: ext || fromType || "media", size: total || 0, audio: ct.startsWith("audio/")};
}

function parseAttributes(line) {
  const out = {};
  const body = line.slice(line.indexOf(":") + 1);
  const re = /([A-Z0-9-]+)=("[^"]*"|[^,]*)/g;
  let m;
  while ((m = re.exec(body))) out[m[1]] = m[2].replace(/^"|"$/g, "");
  return out;
}

function parsePlaylist(text, base) {
  const lines = text.split(/\r?\n/).map(l => l.trim());
  const result = {master: false, variants: [], renditions: [], duration: 0, drm: false};
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];
    if (line.startsWith("#EXT-X-STREAM-INF:")) {
      result.master = true;
      const a = parseAttributes(line);
      let j = i + 1;
      while (j < lines.length && (!lines[j] || lines[j].startsWith("#"))) j++;
      if (j >= lines.length) continue;
      const [w, h] = String(a.RESOLUTION || "").split("x").map(Number);
      let url = "";
      try { url = new URL(lines[j], base).href; } catch (e) { continue; }
      result.variants.push({url, width: w || 0, height: h || 0,
        bandwidth: Number(a["AVERAGE-BANDWIDTH"] || a.BANDWIDTH || 0), separateAudio: !!a.AUDIO});
      i = j;
    } else if (line.startsWith("#EXT-X-MEDIA:") || line.startsWith("#EXT-X-I-FRAME-STREAM-INF:")) {
      const a = parseAttributes(line);
      if (a.URI) { try { result.renditions.push(new URL(a.URI, base).href); } catch (e) {} }
    } else if (line.startsWith("#EXTINF:")) {
      result.duration += parseFloat(line.slice(8)) || 0;
    } else if (line.startsWith("#EXT-X-KEY:") || line.startsWith("#EXT-X-SESSION-KEY:")) {
      const a = parseAttributes(line);
      const method = String(a.METHOD || "").toUpperCase();
      const format = String(a.KEYFORMAT || "").toLowerCase();
      if (method.startsWith("SAMPLE-AES") || /widevine|playready|streamingkeydelivery|fairplay/.test(format)) result.drm = true;
    }
  }
  return result;
}

// Player CDNs usually refuse requests without the player's Referer/Origin. A
// short-lived session rule adds the values the player itself sent, for the
// extension's own requests only (tabId -1).
let ruleSeq = 4000;
async function withPlayerHeaders(url, ctx, fn) {
  const dnr = api.declarativeNetRequest;
  const requestHeaders = [];
  if (ctx && ctx.referer) requestHeaders.push({header: "referer", operation: "set", value: ctx.referer});
  const origin = ctx && ctx.headers && ctx.headers.Origin;
  if (origin) requestHeaders.push({header: "origin", operation: "set", value: origin});
  if (!dnr || !dnr.updateSessionRules || !requestHeaders.length) return fn();
  ruleSeq = ruleSeq >= 4900 ? 4000 : ruleSeq + 1;
  const id = ruleSeq;
  let added = false;
  try {
    await dnr.updateSessionRules({removeRuleIds: [id], addRules: [{
      id, priority: 1,
      action: {type: "modifyHeaders", requestHeaders},
      condition: {urlFilter: "|" + url.split("#")[0], tabIds: [-1], resourceTypes: ["xmlhttprequest", "other"]},
    }]});
    added = true;
  } catch (e) {}
  try { return await fn(); }
  finally { if (added) { try { await dnr.updateSessionRules({removeRuleIds: [id]}); } catch (e) {} } }
}

async function playerFrames(tabId, ctx) {
  const order = [];
  if (ctx && ctx.frameId >= 0) order.push(ctx.frameId);
  let frames = [];
  try { frames = (await api.webNavigation.getAllFrames({tabId})) || []; } catch (e) {}
  const want = (() => {
    try { return new URL((ctx && ctx.headers && ctx.headers.Origin) || (ctx && ctx.referer) || "").origin; }
    catch (e) { return ""; }
  })();
  const scored = frames.map(f => {
    let origin = "";
    try { origin = new URL(f.url).origin; } catch (e) {}
    return {id: f.frameId, score: (want && origin === want ? 2 : 0) + (f.frameId === 0 ? 0 : 1)};
  }).sort((a, b) => b.score - a.score);
  for (const f of scored) if (!order.includes(f.id)) order.push(f.id);
  if (!order.length) order.push(0);
  return order.slice(0, 6);
}

async function readPlaylist(url, tabId, ctx) {
  ownFetches.add(url);
  try {
    try {
      const text = await withPlayerHeaders(url, ctx, async () => {
        const r = await fetch(url, {credentials: "include", cache: "no-store"});
        return r.ok ? await r.text() : "";
      });
      if (text && text.includes("#EXTM3U")) return text;
    } catch (e) {}
    // Fallback: let the player's own frame read it (its origin and cookies).
    for (const frameId of await playerFrames(tabId, ctx)) {
      try {
        const r = await api.tabs.sendMessage(tabId, {type: "gb-fetch", url}, {frameId});
        if (r && typeof r.text === "string" && r.text.includes("#EXTM3U")) return r.text;
      } catch (e) {}
    }
    return "";
  } finally {
    setTimeout(() => ownFetches.delete(url), 4000);
  }
}

async function inspectHls(tabId, stream) {
  const text = await readPlaylist(stream.url, tabId, stream);
  const state = tabs.get(tabId);
  if (!text || !state) return;
  const parsed = parsePlaylist(text, stream.url);
  if (parsed.master) {
    const byHeight = new Map();
    for (const v of parsed.variants) {
      const key = v.height || ("bw" + v.bandwidth);
      const prev = byHeight.get(key);
      if (!prev || v.bandwidth > prev.bandwidth) byHeight.set(key, v);
    }
    stream.variants = [...byHeight.values()].sort((a, b) => (b.height - a.height) || (b.bandwidth - a.bandwidth));
    stream.master = true;
    stream.drm = parsed.drm;
    const children = parsed.variants.map(v => v.url).concat(parsed.renditions);
    state.children = state.children.concat(children).slice(-300);
    state.streams = state.streams.filter(s => s === stream || !children.includes(s.url));
    const top = stream.variants[0];
    if (top) {
      const media = await readPlaylist(top.url, tabId, stream);
      if (media) {
        const info = parsePlaylist(media, top.url);
        stream.duration = Math.round(info.duration);
        stream.drm = stream.drm || info.drm;
      }
    }
  } else if (state.children.includes(stream.url)) {
    state.streams = state.streams.filter(s => s !== stream);
  } else {
    stream.duration = Math.round(parsed.duration);
    stream.drm = parsed.drm;
  }
  persist(tabId);
  announce(tabId);
}

async function register(details, info, captured) {
  const tabId = details.tabId;
  let tab = null;
  try { tab = await api.tabs.get(tabId); } catch (e) {}
  const pageUrl = (tab && tab.url) || "";
  if (isBlocked(pageUrl)) return;
  const state = await tabState(tabId);
  const url = details.url;
  if (state.children.includes(url)) return;
  const existing = state.streams.find(s => s.url === url);
  if (existing) {
    // The player loaded this source again (e.g. back from Dub to Sub): make it current.
    if (existing.gen !== state.gen) { existing.gen = ++state.gen; persist(tabId); announce(tabId); }
    return;
  }
  const frameUrl = details.documentUrl || details.frameUrl || "";
  const referer = captured.referer || frameUrl || (details.initiator ? details.initiator + "/" : "") || pageUrl;
  const headers = {};
  if (captured.origin) headers.Origin = captured.origin;
  if (captured["user-agent"]) headers["User-Agent"] = captured["user-agent"];
  const stream = {
    id: Date.now().toString(36) + Math.random().toString(36).slice(2, 7),
    url, kind: info.kind, ext: info.ext, size: info.size || 0, audio: !!info.audio,
    frameId: details.frameId || 0, referer, headers, pageUrl,
    title: cleanTitle(tab && tab.title), time: Date.now(), gen: ++state.gen,
    master: false, variants: [], duration: 0, drm: false,
  };
  state.streams.push(stream);
  if (state.streams.length > MAX_STREAMS) state.streams.sort((a, b) => b.gen - a.gen).splice(MAX_STREAMS);
  persist(tabId);
  announce(tabId);
  if (info.kind === "hls") inspectHls(tabId, stream);
}

function foundInPage(tab, sender, url) {
  if (!/^https?:/i.test(url) || SKIP_URL_RE.test(url) || SEGMENT_RE.test(url)) return;
  const ext = ((url.match(URL_MEDIA_RE) || [])[1] || "").toLowerCase();
  if (!ext) return;
  const info = ext === "m3u8" ? {kind: "hls", ext} : ext === "mpd" ? {kind: "dash", ext}
    : {kind: "file", ext, size: 0, audio: /^(m4a|mp3|ogg|oga|opus|flac|wav)$/.test(ext)};
  const frameUrl = sender.url || tab.url || "";
  let origin = "";
  try { origin = new URL(frameUrl).origin; } catch (e) {}
  register({tabId: tab.id, url, frameId: sender.frameId || 0, documentUrl: frameUrl}, info, {referer: frameUrl, origin});
}

const FILTER = {urls: ["<all_urls>"], types: ["media", "xmlhttprequest", "other", "object"]};

function onSendHeaders(details) {
  if (details.tabId < 0 || ownFetches.has(details.url)) return;
  const captured = {};
  for (const h of details.requestHeaders || []) {
    const name = h.name.toLowerCase();
    if (name === "referer" || name === "origin" || name === "user-agent") captured[name] = h.value;
  }
  pending.set(details.requestId, captured);
  if (pending.size > 800) pending.delete(pending.keys().next().value);
}
try { api.webRequest.onSendHeaders.addListener(onSendHeaders, FILTER, ["requestHeaders", "extraHeaders"]); }
catch (e) { api.webRequest.onSendHeaders.addListener(onSendHeaders, FILTER, ["requestHeaders"]); }

api.webRequest.onHeadersReceived.addListener((details) => {
  const captured = pending.get(details.requestId) || {};
  pending.delete(details.requestId);
  if (details.tabId < 0 || ownFetches.has(details.url)) return;
  if (details.statusCode >= 300) return;
  const info = classify(
    details.url,
    header(details.responseHeaders, "content-type"),
    Number(header(details.responseHeaders, "content-length")) || 0,
    details.statusCode,
    header(details.responseHeaders, "content-range"),
  );
  if (info) register(details, info, captured);
}, FILTER, ["responseHeaders"]);

api.webRequest.onErrorOccurred.addListener(details => pending.delete(details.requestId), FILTER);

api.tabs.onRemoved.addListener(forget);
api.tabs.onUpdated.addListener(async (tabId, change) => {
  if (!change.url) return;
  sayHello();
  const marker = RESOLVE_RE.exec(change.url);
  if (marker) startResolve(tabId, marker[1]);
  const state = await tabState(tabId);
  const origin = (() => { try { return new URL(change.url).origin; } catch (e) { return ""; } })();
  if (state.origin && origin !== state.origin) {
    tabs.set(tabId, {gen: 0, navGen: 0, origin, page: change.url.split("#")[0], streams: [], children: []});
  } else {
    state.origin = origin;
    // A new page (a full load or an in-page route change): what was captured
    // so far belongs to the previous page. Hash-only changes keep the streams.
    const page = change.url.split("#")[0];
    if (change.status === "loading" || (state.page && page !== state.page)) state.navGen = state.gen;
    state.page = page;
    currentStreams(state);
  }
  persist(tabId);
  announce(tabId);
});

// ------------------------------------------------------------------ menu model (popup + in-page button)
function fmtDuration(sec) {
  if (!sec) return "";
  const h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60), s = Math.floor(sec % 60);
  return (h ? h + ":" + String(m).padStart(2, "0") : m) + ":" + String(s).padStart(2, "0");
}

function fmtSize(bytes) {
  if (!bytes) return "";
  return bytes >= 1 << 30 ? (bytes / (1 << 30)).toFixed(2) + " GB" : Math.max(1, Math.round(bytes / (1 << 20))) + " MB";
}

function itemsFor(stream) {
  const qualities = (stream.variants || []).map(v => v.height).filter(Boolean).map(h => h + "p");
  const base = {referer: stream.referer, headers: stream.headers, title: stream.title, pageUrl: stream.pageUrl,
    kind: stream.kind, qualities};
  if (stream.drm) return [{label: "DRM protected", detail: "This stream is encrypted and cannot be downloaded.", disabled: true}];
  if (stream.kind === "hls" && stream.master && stream.variants.length) {
    const top = stream.variants[0];
    const items = [{...base, url: stream.url, quality: "Best available", best: true,
      label: "Best quality" + (top.height ? " · " + top.height + "p" : ""), detail: "Highest resolution, with sound"}];
    for (const v of stream.variants) {
      const mbps = v.bandwidth ? (v.bandwidth / 1e6).toFixed(1) + " Mbps" : "";
      const res = v.width && v.height ? v.width + "×" + v.height : "";
      const detail = [res, mbps].filter(Boolean).join(" · ");
      if (v.height) {
        // The master playlist is kept so GB also gets the separate audio; the height picks the quality.
        items.push({...base, url: stream.url, quality: v.height + "p", label: v.height + "p", detail});
      } else {
        items.push({...base, url: v.url, quality: "Best available", label: "Audio / other", detail});
      }
    }
    items.push({...base, url: stream.url, quality: "Best available", mode: "Audio", label: "Audio only", detail: "MP3"});
    return items;
  }
  if (stream.kind === "hls") return [{...base, url: stream.url, quality: "Best available", best: true, label: "HLS stream", detail: "Single-quality stream"}];
  if (stream.kind === "dash") return [{...base, url: stream.url, quality: "Best available", best: true, label: "DASH stream · best quality", detail: ""}];
  return [{...base, url: stream.url, quality: "Best available", best: true, mode: stream.audio ? "Audio" : "",
    label: (stream.ext || "media").toUpperCase() + " file", detail: fmtSize(stream.size)}];
}

// Only streams of the page that is open now.
function currentStreams(state) {
  if (!state) return [];
  if (state.navGen && state.streams.some(s => s.gen <= state.navGen)) {
    state.streams = state.streams.filter(s => s.gen > state.navGen);
  }
  return state.streams;
}

async function menuFor(tabId) {
  const state = await tabState(tabId);
  const sorted = [...currentStreams(state)].sort((a, b) => b.gen - a.gen);
  let nowGiven = false;
  const groups = sorted.map(stream => {
    let label = "Earlier stream";
    if (!nowGiven) { label = "Now playing"; nowGiven = true; }
    const kind = stream.kind === "file" ? (stream.ext || "file").toUpperCase() : stream.kind.toUpperCase();
    const meta = [kind, fmtDuration(stream.duration), hostOf(stream.url)].filter(Boolean).join(" · ");
    const top = (stream.variants || [])[0];
    return {id: stream.id, label, meta, current: label === "Now playing", items: itemsFor(stream),
      duration: stream.duration || 0, title: stream.title || "", drm: !!stream.drm,
      height: (top && top.height) || 0, master: !!stream.master, kind: stream.kind};
  });
  return {groups};
}

// ------------------------------------------------------------------ browser UI hooks
try { api.runtime.onStartup.addListener(() => { lastHello = 0; sayHello(); }); } catch (e) {}

api.runtime.onInstalled.addListener(() => {
  lastHello = 0;
  sayHello();
  api.contextMenus.removeAll(() => {
    api.contextMenus.create({id: "gb-page", title: "Download this page with GB", contexts: ["page"]});
    api.contextMenus.create({id: "gb-link", title: "Download link with GB", contexts: ["link"]});
    api.contextMenus.create({id: "gb-media", title: "Download media with GB", contexts: ["video", "audio"]});
  });
});

api.contextMenus.onClicked.addListener((info, tab) => {
  const pageUrl = (tab && tab.url) || "";
  const title = cleanTitle(tab && tab.title);
  if (info.menuItemId === "gb-link" && info.linkUrl) sendAndNotify({url: info.linkUrl, referer: pageUrl, pageUrl, kind: "link"});
  else if (info.menuItemId === "gb-media" && info.srcUrl && !info.srcUrl.startsWith("blob:")) sendAndNotify({url: info.srcUrl, referer: info.frameUrl || pageUrl, pageUrl, title, kind: "media"});
  else if (pageUrl) sendAndNotify({url: info.pageUrl || pageUrl, pageUrl});
});

// ------------------------------------------------------------------ catch the browser's downloads
// A file the browser starts saving is paused, handed to GB, and removed from
// the browser only when GB took it. With GB closed the browser carries on.
// Alt+click on a link lets the browser keep that download.
let bypassUntil = 0;
const capturedIds = new Set();

function fileNameOf(item, url) {
  const fromItem = String(item.filename || "").split(/[\\/]/).pop();
  if (fromItem) return fromItem;
  try { return decodeURIComponent(new URL(url).pathname.split("/").pop() || ""); } catch (e) { return ""; }
}

function extOf(name) {
  const match = /\.([a-z0-9]{1,9})$/i.exec(String(name || ""));
  return match ? match[1].toLowerCase() : "";
}

function captureAllowed(name, mime) {
  const types = settings.captureTypes.toLowerCase().split(/[\s,;]+/).map(t => t.replace(/^\*?\./, "")).filter(Boolean);
  const ext = extOf(name);
  if (!types.length) return !/^(text\/html|application\/xhtml)/i.test(mime || "");
  if (ext) return types.includes(ext);
  return /^(video|audio)\//i.test(mime || "") || /zip|rar|7z|x-msdownload|octet-stream|pdf|iso9660|android\.package/i.test(mime || "");
}

async function captureDownload(item) {
  if (!item || capturedIds.has(item.id)) return;
  await loadSettings();
  const url = item.finalUrl || item.url || "";
  if (!settings.capture || !/^https?:/i.test(url)) return;
  if (item.byExtensionId || (item.state && item.state !== "in_progress")) return;
  if (Date.now() < bypassUntil) { bypassUntil = 0; return; }
  const pageUrl = item.referrer || "";
  if (isBlocked(url) || (pageUrl && isBlocked(pageUrl))) return;
  const name = fileNameOf(item, url);
  if (!captureAllowed(name, item.mime)) return;
  const size = Number(item.totalBytes || item.fileSize || 0);
  if (settings.minSizeKb && size > 0 && size < settings.minSizeKb * 1024) return;
  if (!(await findPort()) || !appCapture) return;
  capturedIds.add(item.id);
  try { await api.downloads.pause(item.id); } catch (e) {}
  const result = await sendToApp({url, referer: pageUrl, pageUrl, kind: "file", fileName: name, fileSize: size,
                                  mime: item.mime || "", title: name.replace(/\.[a-z0-9]{1,9}$/i, "")});
  if (result.ok) {
    try { await api.downloads.cancel(item.id); } catch (e) {}
    try { await api.downloads.erase({id: item.id}); } catch (e) {}
    notify("GB is downloading " + (name || "the file") + ".");
  } else {
    capturedIds.delete(item.id);
    try { await api.downloads.resume(item.id); } catch (e) {}
  }
}

try { api.downloads.onCreated.addListener(item => { captureDownload(item); }); } catch (e) {}

api.commands.onCommand.addListener(async (command) => {
  if (command !== "send-page") return;
  const [tab] = await api.tabs.query({active: true, currentWindow: true});
  if (tab && tab.url) sendAndNotify({url: tab.url, pageUrl: tab.url, title: cleanTitle(tab.title)});
});

async function addBlockedSite(url) {
  const host = hostOf(url).replace(/^www\./, "");
  if (!host) return false;
  await loadSettings();
  if (isBlocked(url)) return true;
  const blocked = (settings.blocked.trim() ? settings.blocked.trim() + "\n" : "") + host;
  await api.storage.local.set({blocked});
  settings.blocked = blocked;
  return true;
}

api.runtime.onMessage.addListener((msg, sender, sendResponse) => {
  const tab = sender.tab;
  const tabId = tab ? tab.id : msg.tabId;
  if (msg.type === "gb-hello") {
    loadSettings().then(() => sendResponse({enabled: settings.overlay && !isBlocked(tab ? tab.url : "")}));
    return true;
  }
  if (msg.type === "gb-menu") { menuFor(tabId).then(sendResponse); return true; }
  if (msg.type === "gb-analyze") {
    if (!/^https?:/i.test(msg.url || "")) { sendResponse({ok: false, error: "No link for this video."}); return; }
    analyzeVideo(msg.url, msg.pageUrl || (tab && tab.url) || "", !!msg.prefetch).then(sendResponse);
    return true;
  }
  if (msg.type === "gb-found") {
    // Media links written in the page itself (<video src>, og:video,
    // playlists in the player's config): listed before the player asks.
    if (tab && tab.id >= 0 && Array.isArray(msg.urls)) {
      for (const url of msg.urls.slice(0, 20)) foundInPage(tab, sender, String(url || ""));
    }
    return;
  }
  if (msg.type === "gb-send") {
    const item = msg.item || {};
    const streamPage = item.pageUrl || (tab && tab.url) || msg.pageUrl || "";
    // The title is read again at the click: on sites that switch videos
    // without a page load the stream may remember the previous title.
    (async () => {
      let fresh = null;
      try { if (tabId !== undefined && tabId >= 0) fresh = await api.tabs.get(tabId); } catch (e) {}
      const here = fresh && fresh.url ? fresh.url.replace(RESOLVE_RE, "") : "";
      const samePage = !!here && (!streamPage || here.split("#")[0] === streamPage.split("#")[0]);
      const liveTitle = samePage ? cleanTitle(fresh && fresh.title) : "";
      const pageUrl = samePage ? here : streamPage;
      sendToApp({...item, pageUrl, title: liveTitle || item.title || msg.title || ""}).then(sendResponse);
    })();
    return true;
  }
  if (msg.type === "gb-send-page") {
    const url = (tab && tab.url) || msg.url;
    sendToApp({url, pageUrl: url, title: msg.title || ""}).then(sendResponse);
    return true;
  }
  if (msg.type === "gb-block-site") {
    addBlockedSite((tab && tab.url) || msg.url || "").then(ok => sendResponse({ok}));
    return true;
  }
  if (msg.type === "gb-site-state") {
    loadSettings().then(() => sendResponse({blocked: isBlocked(msg.url || ""), host: hostOf(msg.url || "")}));
    return true;
  }
  if (msg.type === "gb-bypass") { bypassUntil = Date.now() + 8000; return; }
  if (msg.type === "gb-state") { connectionState().then(sendResponse); return true; }
});
