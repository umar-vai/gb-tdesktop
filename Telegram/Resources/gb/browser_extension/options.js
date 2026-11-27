const api = typeof browser !== "undefined" ? browser : chrome;
const overlay = document.getElementById("overlay");
const blocked = document.getElementById("blocked");
const capture = document.getElementById("capture");
const types = document.getElementById("types");
const minsize = document.getElementById("minsize");
const DEFAULT_TYPES = "zip rar 7z tar gz tgz bz2 xz iso img bin exe msi msix apk dmg deb rpm pdf doc docx " +
  "xls xlsx ppt pptx epub torrent mp4 mkv webm mov avi wmv flv m4v 3gp mp3 m4a aac flac wav ogg opus wma";

api.runtime.sendMessage({type: "gb-state"}).then(r => {
  const text = document.getElementById("pairstate");
  const dot = document.getElementById("dot");
  if (r && r.connected) {
    dot.className = "dot ok";
    text.textContent = "Connected to GB" + (r.capture === false ? " · GB's \"catch browser downloads\" is off" : "");
  } else {
    dot.className = "dot bad";
    text.textContent = "GB is not running. Start GB; the extension connects by itself.";
  }
}).catch(() => {});

api.storage.local.get(["overlay", "blocked", "capture", "captureTypes", "minSizeKb"]).then(d => {
  overlay.checked = d.overlay !== false;
  blocked.value = d.blocked || "";
  capture.checked = d.capture !== false;
  types.value = typeof d.captureTypes === "string" ? d.captureTypes : DEFAULT_TYPES;
  minsize.value = String(Math.max(0, Number(d.minSizeKb) || 0));
});

document.getElementById("resettypes").onclick = () => { types.value = DEFAULT_TYPES; };
document.getElementById("save").onclick = async () => {
  await api.storage.local.set({
    overlay: overlay.checked,
    blocked: blocked.value.trim(),
    capture: capture.checked,
    captureTypes: types.value.trim(),
    minSizeKb: Math.max(0, Number(minsize.value) || 0),
  });
  const saved = document.getElementById("saved");
  saved.textContent = "✓ Saved";
  setTimeout(() => { saved.textContent = ""; }, 2500);
};
