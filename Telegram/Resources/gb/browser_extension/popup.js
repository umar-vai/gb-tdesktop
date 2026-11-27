const api = typeof browser !== "undefined" ? browser : chrome;
const msg = document.getElementById("msg");

function say(text, kind) {
  msg.textContent = text;
  msg.className = kind || "";
}

async function send(message) {
  say("Sending…");
  const r = await api.runtime.sendMessage(message);
  if (r && r.ok) say(r.asked ? "✓ Opened in GB - choose where to save it there." : "✓ Added to GB downloads.", "ok");
  else say((r && r.error) || "Failed.", "bad");
}

document.getElementById("options").onclick = () => api.runtime.openOptionsPage();

(async () => {
  const dot = document.getElementById("dot");
  const state = document.getElementById("state");
  api.runtime.sendMessage({type: "gb-state"}).then(r => {
    if (r && r.connected) {
      dot.className = "dot ok";
      state.textContent = r.engine === false ? "Connected · yt-dlp missing in GB" : "Connected to GB";
    } else {
      dot.className = "dot bad";
      state.textContent = "GB is not running";
    }
  }).catch(() => {});

  const [tab] = await api.tabs.query({active: true, currentWindow: true});
  if (!tab) return;
  const http = /^https?:/i.test(tab.url || "");
  const page = document.getElementById("page");
  page.disabled = !http;
  page.onclick = () => send({type: "gb-send-page", url: tab.url, title: tab.title});

  const site = document.getElementById("site");
  const siteState = http ? await api.runtime.sendMessage({type: "gb-site-state", url: tab.url}) : null;
  if (!http) site.style.display = "none";
  else if (siteState && siteState.host) {
    site.textContent = siteState.blocked ? `Blocked on ${siteState.host} · edit in settings` : `Never use on ${siteState.host}`;
  }
  site.onclick = async () => {
    if (siteState && siteState.blocked) { api.runtime.openOptionsPage(); return; }
    await api.runtime.sendMessage({type: "gb-block-site", url: tab.url});
    site.textContent = "Blocked. Reload the page.";
  };

  const data = await api.runtime.sendMessage({type: "gb-menu", tabId: tab.id});
  if (!data || !data.groups || !data.groups.length) return;
  const list = document.getElementById("list");
  list.textContent = "";
  const groups = data.groups;
  const best = groups.find(g => !g.drm && g.master) || groups.find(g => !g.drm) || groups[0];
  document.getElementById("meta").textContent = best.meta || "";
  document.getElementById("title").textContent = best.title || "";
  for (const item of best.items) {
    const button = document.createElement("button");
    button.className = "row" + (item.best ? " best" : "");
    const label = document.createElement("span"); label.className = "label"; label.textContent = item.label;
    const detail = document.createElement("span"); detail.className = "detail"; detail.textContent = item.detail || "";
    button.append(label, detail);
    if (item.disabled) button.disabled = true;
    else button.onclick = () => send({type: "gb-send", item, tabId: tab.id, pageUrl: tab.url, title: tab.title});
    list.appendChild(button);
  }
  const others = groups.filter(g => g !== best && !g.drm).length;
  if (others) {
    const more = document.createElement("div");
    more.className = "hint";
    more.style.padding = "6px 16px 0";
    more.textContent = `${others} earlier stream${others > 1 ? "s" : ""} on this tab (another server or episode).`;
    list.appendChild(more);
  }
})();
