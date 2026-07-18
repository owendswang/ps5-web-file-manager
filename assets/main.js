let cwd = "/";
let entries = [];
let selected = new Set();
let focusedPath = null;
let trackedTask = null;
let busy = false;
let clipboard = null;
let pendingOverlayText = "";
let pendingOverlayLabel = "";
let pendingAbortController = null;
let taskRefreshPath = "/";
let hoverPaused = false;
let hoverResumeTimer = 0;
let hoveredRow = null;
let loadingPath = null;
let directoryLoadingTimer = 0;
let directoryLoadingStartedAt = 0;
let taskOverlayTimer = 0;
let lastCompletionId = null;
let taskPollFailedAlertShown = false;
let textEditorPath = null;
let textEditorVersion = null;
let textEditorOriginal = "";
let textEditorBusy = false;
let L = {};

const APP_VERSION = "v0.9";
const LAST_PATH_KEY = "ps5-web-file-mgr:last-path";
const LOADING_DISPLAY_DELAY = 250;
const SELECT_ALL_LOADING_THRESHOLD = 1000;

const filesEl = document.getElementById("files");
const contentEl = document.getElementById("content");
const contentLoadingEl = document.getElementById("contentLoading");
const contentLoadingTextEl = contentLoadingEl.querySelector(".content-loading-text");
const emptyEl = document.getElementById("empty");
const pathEl = document.getElementById("path");
const spaceInfoEl = document.getElementById("spaceInfo");
const statusEl = document.getElementById("statusText");
const versionEl = document.getElementById("versionText");
const tasksEl = document.getElementById("tasks");
const overlayEl = document.getElementById("taskOverlay");
const selectAllEl = document.getElementById("selectAll");
const pasteBtn = document.getElementById("pasteBtn");
const pasteVerbEl = document.getElementById("pasteVerb");
const pasteNameEl = document.getElementById("pasteName");
const pasteCountEl = document.getElementById("pasteCount");
const pasteTargetTextEl = document.getElementById("pasteTargetText");
const clearClipboardBtn = document.getElementById("clearClipboardBtn");
const initLoadingEl = document.getElementById("initLoading");
const exitBtn = document.getElementById("exitBtn");
const textEditorOverlayEl = document.getElementById("textEditorOverlay");
const textEditorPathEl = document.getElementById("textEditorPath");
const textEditorEl = document.getElementById("textEditor");
const textEditorStatusEl = document.getElementById("textEditorStatus");
const textEditorCloseBtn = document.getElementById("textEditorCloseBtn");
const textEditorSaveBtn = document.getElementById("textEditorSaveBtn");
const newTextBtn = document.getElementById("newTextBtn");
const imagePreviewOverlayEl = document.getElementById("imagePreviewOverlay");
const imagePreviewNameEl = document.getElementById("imagePreviewName");
const imagePreviewEl = document.getElementById("imagePreview");
const imagePreviewCloseBtn = document.getElementById("imagePreviewCloseBtn");

function chooseLanguage() {
  const langs = navigator.languages && navigator.languages.length ? navigator.languages : [navigator.language || ""];
  const lang = String(langs[0] || "").toLowerCase();
  return lang.indexOf("zh") === 0 ? "zh" : "en";
}

function loadLanguage() {
  return new Promise(resolve => {
    const lang = chooseLanguage();
    initLoadingEl.textContent = lang === "zh" ? "加载中..." : "Loading...";
    const script = document.createElement("script");
    script.src = "/lang-" + lang + ".js";
    script.onload = () => {
      L = window.WFM_LANG || {};
      document.documentElement.lang = lang === "zh" ? "zh-CN" : "en";
      resolve();
    };
    script.onerror = () => {
      L = {};
      resolve();
    };
    document.head.appendChild(script);
  });
}

function t(key, params) {
  let text = L[key] || key;
  for (const name in params || {}) {
    text = text.replace(new RegExp("\\{" + name + "\\}", "g"), params[name]);
  }
  return text;
}

function backendErrorText(code, arg, fallback) {
  if (!code) return fallback || t("backendError");
  const params = { path: arg || "", arg: arg || "" };
  if (code === "no_space") {
    const parts = String(arg || "").split(",");
    params.required = formatBytes(parts[0] || 0, false);
    params.available = formatBytes(parts[1] || 0, false);
  }
  const key = "err_" + code;
  return L[key] ? t(key, params) : fallback || t("backendError");
}

function applyStaticText() {
  document.title = t("appTitle");
  for (const el of document.querySelectorAll("[data-i18n]")) {
    el.textContent = t(el.dataset.i18n);
  }
  exitBtn.title = t("exit");
  exitBtn.setAttribute("aria-label", t("exit"));
  versionEl.textContent = APP_VERSION;
  if (initLoadingEl) initLoadingEl.hidden = true;
}

function nextPaint() {
  return new Promise(resolve => {
    if (window.requestAnimationFrame) {
      requestAnimationFrame(() => requestAnimationFrame(() => resolve()));
    }
    else setTimeout(resolve, 0);
  });
}

function delay(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

function showContentLoading(text) {
  contentLoadingTextEl.textContent = text || t("readDir");
  contentLoadingEl.style.top = contentEl.scrollTop + "px";
  contentLoadingEl.hidden = false;
  contentEl.classList.add("loading");
  updateButtons();
}

function startContentLoadingTimer() {
  if (!contentLoadingEl.hidden || directoryLoadingTimer) return;
  directoryLoadingStartedAt = Date.now();
  directoryLoadingTimer = setTimeout(() => {
    directoryLoadingTimer = 0;
    showContentLoading();
  }, LOADING_DISPLAY_DELAY);
}

async function showContentLoadingAfterDelay() {
  if (!contentLoadingEl.hidden) return;
  const elapsed = directoryLoadingStartedAt ? Date.now() - directoryLoadingStartedAt : 0;
  const remaining = LOADING_DISPLAY_DELAY - elapsed;
  if (remaining > 0) await delay(remaining);
  clearTimeout(directoryLoadingTimer);
  directoryLoadingTimer = 0;
  showContentLoading();
}

function hideContentLoading() {
  clearTimeout(directoryLoadingTimer);
  directoryLoadingTimer = 0;
  directoryLoadingStartedAt = 0;
  contentEl.classList.remove("loading");
  contentLoadingEl.hidden = true;
  updateButtons();
}

async function request(path, params, options) {
  const qs = new URLSearchParams(params || {});
  const fetchOptions = Object.assign({ method: "POST" }, options || {});
  const response = await fetch(path + (qs.toString() ? "?" + qs.toString() : ""), fetchOptions);
  if (!response.ok) {
    const data = await response.json();
    throw new Error(backendErrorText(data.error_code, data.error_arg, data.error));
  }
  return response;
}

async function api(path, params, options) {
  const data = await (await request(path, params, options)).json();
  if (!data.ok) throw new Error(backendErrorText(data.error_code, data.error_arg, data.error));
  return data;
}

async function apiForm(path, params, options) {
  const fetchOptions = Object.assign({
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: new URLSearchParams(params || {}).toString()
  }, options || {});
  const data = await (await request(path, null, fetchOptions)).json();
  if (!data.ok) throw new Error(backendErrorText(data.error_code, data.error_arg, data.error));
  return data;
}

function decodeFsText(text) {
  text = String(text || "");
  let needsDecode = false;
  for (let i = 0; i < text.length; i++) {
    const code = text.charCodeAt(i);
    if (code > 0xff) return text;
    if (code >= 0x80) needsDecode = true;
  }
  if (!needsDecode || typeof TextDecoder === "undefined" || typeof Uint8Array === "undefined") return text;

  const bytes = new Uint8Array(text.length);
  for (let i = 0; i < text.length; i++) bytes[i] = text.charCodeAt(i) & 0xff;
  if (isUtf8Bytes(bytes)) {
    try {
      return new TextDecoder("utf-8").decode(bytes);
    } catch (err) {
      return text;
    }
  }
  try {
    return new TextDecoder("gbk").decode(bytes);
  } catch (err) {
    try {
      return new TextDecoder("gb18030").decode(bytes);
    } catch (err2) {
      return text;
    }
  }
}

function isUtf8Bytes(bytes) {
  for (let i = 0; i < bytes.length;) {
    const c = bytes[i];
    let n = 0;
    let cp = 0;

    if (c < 0x80) {
      i++;
      continue;
    }
    if ((c & 0xe0) === 0xc0) {
      n = 2;
      cp = c & 0x1f;
    } else if ((c & 0xf0) === 0xe0) {
      n = 3;
      cp = c & 0x0f;
    } else if ((c & 0xf8) === 0xf0) {
      n = 4;
      cp = c & 0x07;
    } else {
      return false;
    }
    if (i + n > bytes.length) return false;
    for (let j = 1; j < n; j++) {
      const t = bytes[i + j];
      if ((t & 0xc0) !== 0x80) return false;
      cp = (cp << 6) | (t & 0x3f);
    }
    if ((n === 2 && cp < 0x80) ||
        (n === 3 && cp < 0x800) ||
        (n === 4 && (cp < 0x10000 || cp > 0x10ffff)) ||
        (cp >= 0xd800 && cp <= 0xdfff)) return false;
    i += n;
  }
  return true;
}

function displayName(item) {
  return item.displayName || decodeFsText(item.name);
}

function displayPath(path) {
  return decodeFsText(path);
}

async function fetchText(path, params) {
  const response = await request(path, params, { method: "GET" });
  return {
    text: await response.text(),
    version: response.headers.get("X-Text-Version") || ""
  };
}

function formatSize(size, type) {
  if (type === "d" || type === "parent") return "";
  return formatBytes(size, true);
}

function formatBytes(size, decimals) {
  const units = ["B", "KB", "MB", "GB", "TB"];
  let value = Number(size || 0);
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit++;
  }
  if (!unit) return value + " B";
  return (decimals ? value.toFixed(2) : Math.round(value)) + " " + units[unit];
}

function formatTime(seconds) {
  if (!seconds) return "";
  const date = new Date(seconds * 1000);
  const year = date.getFullYear();
  const month = date.getMonth() + 1;
  const day = date.getDate();
  const time = [date.getHours(), date.getMinutes(), date.getSeconds()]
    .map(value => value < 10 ? "0" + value : value).join(":");
  return document.documentElement.lang === "en" ?
    month + "/" + day + "/" + year + ", " + time :
    year + "/" + month + "/" + day + " " + time;
}

function formatSpeed(bytes) {
  return formatSize(bytes) + "/s";
}

function formatDuration(seconds) {
  if (!isFinite(seconds) || seconds < 0) return "--";
  seconds = Math.ceil(seconds);
  const hours = Math.floor(seconds / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  const secs = seconds % 60;
  if (hours) return t("durationHours", { hours, minutes });
  if (minutes) return t("durationMinutes", { minutes, seconds: secs });
  return t("durationSeconds", { seconds: secs });
}

function averageEta(task, done, total) {
  if (!total || !done || done >= total) return "--";
  const eta = Number(task.eta || 0);
  return eta > 0 ? formatDuration(eta) : "--";
}

function taskElapsed(task) {
  if (!task || !Object.prototype.hasOwnProperty.call(task, "total_elapsed")) return "--";
  return formatDuration(Number(task.total_elapsed || 0));
}

function opLabel(op) {
  return { copy: t("copy"), move: t("move"), delete: t("delete") }[op] || op;
}

function taskOpLabel(op) {
  return { copy: t("copying"), move: t("moving"), delete: t("deleting") }[op] || op;
}

function stateLabel(state) {
  return { queued: t("queued"), running: t("running"), done: t("done"), failed: t("failed"), canceled: t("canceled") }[state] || state;
}

function isTerminalTask(task) {
  return ["done", "failed", "canceled"].includes(task.state);
}

function trackTask(id, op, fromClipboard, clearClipboardOnDone) {
  trackedTask = {
    id,
    op,
    fromClipboard: Boolean(fromClipboard),
    clearClipboardOnDone: Boolean(clearClipboardOnDone),
    cancelRequested: false,
    state: "queued"
  };
}

function requestTaskCancel(id) {
  if (trackedTask && trackedTask.id === id) trackedTask.cancelRequested = true;
}

function clearTrackedTask() {
  trackedTask = null;
}

function clearClipboardAfterPaste() {
  clipboard = null;
  renderClipboard();
}

function taskFailureMessage(task) {
  return t("taskFailed", {
    label: opLabel(task.op),
    error: backendErrorText(task.error_code, task.error_arg, task.error || task.current)
  });
}

function handleTerminalTask(task) {
  if (task.state === "failed") {
    clearTrackedTask();
    const message = taskFailureMessage(task);
    setStatus(message);
    alert(message);
    return;
  }
  if (task.state === "canceled") {
    clearTrackedTask();
    setStatus(t("taskCanceled", { label: opLabel(task.op) }));
    return;
  }
  if (task.state === "done") {
    if (trackedTask && trackedTask.id === task.id &&
        (trackedTask.fromClipboard || trackedTask.clearClipboardOnDone)) clearClipboardAfterPaste();
    clearTrackedTask();
    setStatus(t("actionDone", { label: opLabel(task.op) }));
  }
}

function setStatus(text) {
  statusEl.textContent = text;
}

function setBusy(value) {
  busy = value;
  updateButtons();
}

function showTaskOverlay(immediate) {
  setBusy(true);
  if (!overlayEl.hidden || taskOverlayTimer) return;
  if (immediate) {
    overlayEl.hidden = false;
    return;
  }
  taskOverlayTimer = setTimeout(() => {
    taskOverlayTimer = 0;
    overlayEl.hidden = false;
  }, LOADING_DISPLAY_DELAY);
}

function hideTaskOverlay() {
  clearTimeout(taskOverlayTimer);
  taskOverlayTimer = 0;
  overlayEl.hidden = true;
}

function showActionFailed(label, error) {
  const message = t("actionFailed", { label, error });
  setStatus(message);
  alert(message);
}

function readSavedPath() {
  try {
    return localStorage.getItem(LAST_PATH_KEY) || "/";
  } catch (err) {
    return "/";
  }
}

function savePath(path) {
  try {
    localStorage.setItem(LAST_PATH_KEY, path || "/");
  } catch (err) {
  }
}

async function refreshSpaces() {
  try {
    const data = await api("/api/space", { path: cwd });
    spaceInfoEl.innerHTML = "";
    if ((data.spaces || []).length) {
      const prefix = document.createElement("span");
      prefix.className = "space-prefix";
      prefix.textContent = t("freeSpace");
      spaceInfoEl.appendChild(prefix);
    }
    for (const item of data.spaces || []) {
      const span = document.createElement("span");
      span.className = "space-item" + (item.current ? " current" : "");
      const label = item.label_key ? t(item.label_key) : item.label;
      span.textContent = label + ": " +
        formatBytes(item.free, false) + "/" + formatBytes(item.total, false);
      span.title = item.path + " " + span.textContent;
      spaceInfoEl.appendChild(span);
    }
  } catch (err) {
    spaceInfoEl.textContent = "";
  }
}

function selectedEntries() {
  return entries.filter(item => selected.has(item.path));
}

function pathJoin(dir, name) {
  return dir === "/" ? "/" + name : dir.replace(/\/+$/, "") + "/" + name;
}

function pathIsSameOrChild(parent, child) {
  parent = String(parent || "").replace(/\/+$/, "") || "/";
  child = String(child || "").replace(/\/+$/, "") || "/";
  return child === parent || (parent !== "/" && child.indexOf(parent + "/") === 0);
}

function typeLabel(type) {
  return type === "d" ? t("dir") : t("file");
}

function isEditableText(item) {
  return item.type === "-" &&
    /\.(txt|json|xml|ini|cfg|conf|md|log|lua|js|css|html?|c|h|cpp|hpp|sh|csv|ya?ml)$/i.test(item.name);
}

function isPreviewableImage(item) {
  return item.type === "-" && /\.(png|jpe?g|gif|bmp|webp)$/i.test(item.name);
}

function openImagePreview(item) {
  if (busy) return;
  imagePreviewNameEl.textContent = displayName(item);
  imagePreviewNameEl.title = displayName(item);
  imagePreviewEl.src = "/fs?path=" + encodeURIComponent(item.path);
  imagePreviewOverlayEl.hidden = false;
  imagePreviewCloseBtn.focus();
}

function closeImagePreview() {
  imagePreviewOverlayEl.hidden = true;
  imagePreviewNameEl.textContent = "";
  imagePreviewNameEl.title = "";
  imagePreviewEl.removeAttribute("src");
}

function setTextEditorBusy(value, status) {
  textEditorBusy = value;
  textEditorEl.disabled = value;
  textEditorCloseBtn.disabled = value;
  textEditorSaveBtn.disabled = value;
  textEditorStatusEl.textContent = status || "";
}

function closeTextEditor() {
  if (textEditorBusy) return;
  textEditorOverlayEl.hidden = true;
  textEditorPath = null;
  textEditorVersion = null;
  textEditorOriginal = "";
  textEditorEl.value = "";
  textEditorStatusEl.textContent = "";
  return load(cwd, false, true);
}

function requestCloseTextEditor() {
  if (textEditorBusy) return;
  if (textEditorEl.value !== textEditorOriginal &&
      confirm(t("unsavedSaveConfirm"))) return saveTextEditor();
  return closeTextEditor();
}

function showTextEditor(item, text, version) {
  textEditorPath = item.path;
  textEditorVersion = version;
  textEditorPathEl.textContent = displayPath(item.path);
  textEditorPathEl.title = displayPath(item.path);
  textEditorEl.value = text;
  textEditorOriginal = textEditorEl.value;
  textEditorOverlayEl.hidden = false;
  setTextEditorBusy(false, "");
  textEditorEl.focus();
}

async function openTextEditor(item) {
  if (busy || textEditorBusy) return;
  textEditorPath = item.path;
  textEditorVersion = null;
  textEditorOriginal = "";
  textEditorPathEl.textContent = displayPath(item.path);
  textEditorPathEl.title = displayPath(item.path);
  textEditorEl.value = "";
  textEditorOverlayEl.hidden = false;
  setTextEditorBusy(true, t("loadingText"));
  try {
    const data = await fetchText("/api/text", { path: item.path });
    showTextEditor(item, data.text, data.version);
  } catch (err) {
    setTextEditorBusy(false, "");
    textEditorOverlayEl.hidden = true;
    textEditorPath = null;
    const message = t("openTextFailed", { error: err.message });
    setStatus(message);
    alert(message);
  }
}

async function saveTextEditor() {
  if (textEditorBusy || !textEditorPath || !textEditorVersion) return;
  if (textEditorEl.value === textEditorOriginal) {
    await closeTextEditor();
    return;
  }
  const path = textEditorPath;
  setTextEditorBusy(true, t("savingText"));
  try {
    await api("/api/text/save", { path, version: textEditorVersion }, {
      body: textEditorEl.value,
      headers: { "Content-Type": "text/plain; charset=utf-8" }
    });
    setTextEditorBusy(false, "");
    await closeTextEditor();
    setStatus(t("textSaved"));
  } catch (err) {
    setTextEditorBusy(false, "");
    const message = t("saveTextFailed", { error: err.message });
    setStatus(message);
    alert(message);
  }
}

async function actionNewText() {
  if (busy || loadingPath) return;
  const dir = cwd;
  const input = prompt(t("newTextPrompt"));
  if (input === null) return;
  const name = input.trim();
  if (!name) return;
  const item = { name, path: pathJoin(dir, name), type: "-" };

  try {
    setBusy(true);
    setStatus(t("creatingText"));
    const data = await api("/api/text/create", { path: dir, name });
    setBusy(false);
    showTextEditor(item, "", data.version);
  } catch (err) {
    setBusy(false);
    const message = t("createTextFailed", { error: err.message });
    setStatus(message);
    alert(message);
  }
}

function resetScrollTop() {
  const apply = () => {
    contentEl.scrollTop = 0;
    document.documentElement.scrollTop = 0;
    document.body.scrollTop = 0;
    window.scrollTo(0, 0);
  };
  apply();
  setTimeout(apply, 0);
  if (window.requestAnimationFrame) requestAnimationFrame(apply);
}

function clipboardTitle() {
  if (!clipboard || clipboard.items.length === 0) return "";
  return itemTitle(clipboard.items);
}

function itemTitle(items) {
  if (!items.length) return "";
  return items.length === 1 ? displayName(items[0]) : t("selectedItems", { name: displayName(items[0]), count: items.length });
}

function itemCountSuffix(items) {
  if (items.length <= 1) return "";
  const text = t("selectedItems", { name: "", count: items.length });
  return text.replace(/^\s+/, "");
}

function pathName(path) {
  const clean = String(path || "").replace(/\/+$/, "");
  const pos = clean.lastIndexOf("/");
  return displayPath(pos >= 0 ? clean.slice(pos + 1) || "/" : clean);
}

function taskSubject(task) {
  const count = Number(task.src_count || 0);
  return count > 1 ? t("countItems", { count }) : pathName(task.src);
}

function renderTaskName(el, task) {
  const target = taskSubject(task);
  const op = document.createElement("span");
  op.className = "task-name-op";
  op.textContent = taskOpLabel(task.op);
  const subject = document.createElement("span");
  subject.className = "task-name-subject";
  subject.textContent = target;
  subject.title = target;
  appendChildren(el, op, subject);
}

function renderClipboard() {
  const hasClipboard = Boolean(clipboard && clipboard.items.length);
  const locked = busy || contentEl.classList.contains("loading");
  pasteBtn.hidden = !hasClipboard;
  clearClipboardBtn.hidden = !hasClipboard;
  if (!hasClipboard) {
    pasteVerbEl.textContent = t("paste");
    pasteNameEl.textContent = "";
    pasteCountEl.textContent = "";
    pasteBtn.title = "";
    pasteBtn.disabled = true;
    clearClipboardBtn.disabled = true;
    return;
  }

  const title = clipboardTitle();
  pasteVerbEl.textContent = clipboard.op === "move" ? t("move") : t("paste");
  pasteNameEl.textContent = clipboard.items.length === 1 ? title : displayName(clipboard.items[0]);
  pasteCountEl.textContent = itemCountSuffix(clipboard.items);
  pasteBtn.title = t("pasteTitle", { label: clipboard.op === "move" ? t("move") : t("paste"), name: title, path: cwd });
  pasteBtn.disabled = locked;
  clearClipboardBtn.disabled = locked;
}

function singleSelected() {
  const items = selectedEntries();
  return items.length === 1 ? items[0] : null;
}

function updateButtons() {
  const items = selectedEntries();
  const locked = busy || contentEl.classList.contains("loading");
  document.getElementById("copyBtn").disabled = locked || items.length === 0;
  document.getElementById("moveBtn").disabled = locked || items.length === 0;
  document.getElementById("renameBtn").disabled = locked || items.length !== 1;
  document.getElementById("deleteBtn").disabled = locked || items.length === 0;
  document.getElementById("refreshBtn").disabled = locked;
  document.getElementById("mkdirBtn").disabled = locked;
  newTextBtn.disabled = locked;
  selectAllEl.disabled = locked;
  selectAllEl.checked = entries.length > 0 && items.length === entries.length;
  selectAllEl.indeterminate = items.length > 0 && items.length < entries.length;
  renderClipboard();
}

function focusPath(path) {
  if (focusedPath === path) return;
  focusedPath = path;
  const old = filesEl.querySelector("tr.focused");
  const next = filesEl.querySelector("tr[data-path=\"" + cssEscape(path) + "\"]");
  if (old) old.classList.remove("focused");
  if (next) next.classList.add("focused");
}

function hoverRow(row) {
  if (hoverPaused) return;
  if (hoveredRow === row) return;
  clearHoverPath();
  hoveredRow = row;
  if (hoveredRow) hoveredRow.classList.add("hovered");
}

function clearHoverPath() {
  if (hoveredRow) hoveredRow.classList.remove("hovered");
  hoveredRow = null;
}

function cssEscape(value) {
  if (window.CSS && CSS.escape) return CSS.escape(value);
  return String(value).replace(/["\\]/g, "\\$&");
}

function appendChildren(parent) {
  for (let i = 1; i < arguments.length; i++) {
    parent.appendChild(arguments[i]);
  }
}

function bindPress(el, fn) {
  let handled = false;
  const run = event => {
    if (handled) {
      if (event && event.preventDefault) event.preventDefault();
      return;
    }
    handled = true;
    if (event && event.preventDefault) event.preventDefault();
    fn();
    setTimeout(() => {
      handled = false;
    }, 350);
  };
  el.addEventListener("mousedown", run);
  el.addEventListener("click", run);
}

function findRowFromTarget(target) {
  while (target && target !== contentEl) {
    if (target.tagName === "TR" && target.getAttribute("data-path") !== null) return target;
    target = target.parentNode;
  }
  return null;
}

function togglePath(path, checked) {
  if (checked) selected.add(path);
  else selected.delete(path);
  const row = filesEl.querySelector("tr[data-path=\"" + cssEscape(path) + "\"]");
  if (row) {
    if (checked) row.classList.add("selected");
    else row.classList.remove("selected");
    const checkbox = row.querySelector("input[type=\"checkbox\"]");
    if (checkbox) checkbox.checked = checked;
  }
  updateButtons();
}

function clearSelection() {
  if (selected.size === 0) {
    updateButtons();
    return;
  }
  selected.forEach(path => {
    const row = filesEl.querySelector("tr[data-path=\"" + cssEscape(path) + "\"]");
    if (!row) return;
    row.classList.remove("selected");
    const checkbox = row.querySelector("input[type=\"checkbox\"]");
    if (checkbox) checkbox.checked = false;
  });
  selected.clear();
  updateButtons();
}

function render() {
  hoveredRow = null;
  filesEl.innerHTML = "";
  emptyEl.hidden = entries.length !== 0;
  const fragment = document.createDocumentFragment();

  const rows = cwd === "/" ? entries : [{
    name: "..",
    path: parentPath(cwd),
    type: "parent",
    mode: 0,
    size: 0,
    mtime: 0
  }].concat(entries);

  for (const item of rows) {
    const isParent = item.type === "parent";
    const tr = document.createElement("tr");
    tr.dataset.path = item.path;
    tr.dataset.name = item.name;
    tr.dataset.type = item.type;
    tr.className = (!isParent && selected.has(item.path) ? "selected " : "") +
      (focusedPath === item.path ? "focused" : "");

    const selectTd = document.createElement("td");
    selectTd.className = "select-cell";
    if (!isParent) {
      const label = document.createElement("label");
      label.className = "select-hit";
      const checkbox = document.createElement("input");
      checkbox.type = "checkbox";
      checkbox.checked = selected.has(item.path);
      label.appendChild(checkbox);
      selectTd.appendChild(label);
    }

    const nameTd = document.createElement("td");
    nameTd.className = "name-col";
    const nameBtn = document.createElement("button");
    nameBtn.className = "row-action";
    nameBtn.type = "button";
    const nameText = document.createElement("span");
    nameText.className = "name-text";
    nameText.textContent = displayName(item);
    nameBtn.title = displayPath(item.path);
    const iconImg = document.createElement("img");
    iconImg.className = "icon";
    iconImg.src = isParent ? "/icon-up.png" : item.type === "d" ? "/icon-folder.png" :
      isPreviewableImage(item) ? "/icon-image.png" :
      isEditableText(item) ? "/icon-file.png" : "/icon-generic.png";
    iconImg.alt = "";
    nameBtn.appendChild(iconImg);
    nameBtn.appendChild(nameText);
    nameTd.appendChild(nameBtn);

    appendChildren(
      tr,
      selectTd,
      nameTd,
      classCell("type-col", isParent ? t("parent") : item.type === "d" ? t("dir") : t("file")),
      classCell("size-col", formatSize(item.size, item.type)),
      classCell("time-col", formatTime(item.mtime)),
      classCell("mode-col", isParent ? "" : "0" + Number(item.mode).toString(8))
    );
    fragment.appendChild(tr);
  }
  filesEl.appendChild(fragment);
  updateButtons();
}

function parentPath(path) {
  if (path === "/") return "/";
  const parts = path.replace(/\/+$/, "").split("/");
  parts.pop();
  return parts.join("/") || "/";
}

function cell(text) {
  const td = document.createElement("td");
  td.textContent = text;
  td.title = text;
  return td;
}

function classCell(className, text) {
  const td = cell(text);
  td.className = className;
  return td;
}

async function load(path, scrollTop, force) {
  const shouldScrollTop = scrollTop !== false;
  if ((busy && !force) || loadingPath) return;
  loadingPath = path;
  startContentLoadingTimer();
  try {
    setStatus(t("readDir"));
    const data = await api("/api/list", { path });
    if (contentLoadingEl.hidden && data.entries && data.entries.length > 80) {
      await showContentLoadingAfterDelay();
    }
    if (!contentLoadingEl.hidden) await nextPaint();
    cwd = data.path;
    pathEl.textContent = displayPath(cwd);
    savePath(cwd);
    refreshSpaces();
    entries = data.entries.sort((a, b) => {
      if (a.type === "d" && b.type !== "d") return -1;
      if (a.type !== "d" && b.type === "d") return 1;
      return displayName(a).localeCompare(displayName(b));
    });
    selected.clear();
    focusedPath = null;
    render();
    if (shouldScrollTop) resetScrollTop();
    setStatus(t("totalItems", { count: entries.length }));
  } catch (err) {
    setStatus(err.message);
  } finally {
    hideContentLoading();
    loadingPath = null;
  }
}

async function runAction(label, fn, options) {
  try {
    taskRefreshPath = cwd;
    setBusy(true);
    setStatus(t("actionBusy", { label }));
    const data = await fn();
    setStatus(t("taskCreated", { label }));
    trackTask(data.task_id, "delete", false, options && options.clearClipboardOnDone);
    clearSelection();
    await pollTasks();
  } catch (err) {
    setBusy(false);
    showActionFailed(label, err.message);
  }
}

async function runImmediateAction(label, fn) {
  const refreshPath = cwd;
  try {
    setBusy(true);
    setStatus(t("actionBusy", { label }));
    await fn();
    clearSelection();
    await load(refreshPath, false, true);
    setStatus(t("actionDone", { label }));
  } catch (err) {
    showActionFailed(label, err.message);
  } finally {
    setBusy(false);
  }
}

function setClipboard(op) {
  if (busy || loadingPath || selected.size === 0) return;
  const items = selectedEntries().map(item => ({ path: item.path, name: item.name, type: item.type }));
  if (items.length === 0) return;
  clipboard = { op, items };
  renderClipboard();
  setStatus(t("selectedForPaste", { name: itemTitle(items), verb: op === "move" ? t("move") : t("paste") }));
}

function clearClipboard() {
  if (busy || loadingPath) return;
  clipboard = null;
  renderClipboard();
  setStatus(t("clipboardCleared"));
}

function pathsTouchClipboard(paths) {
  if (!clipboard || !clipboard.items.length) return false;
  for (const path of paths) {
    for (const item of clipboard.items) {
      if (pathIsSameOrChild(path, item.path)) return true;
    }
  }
  return false;
}

function validatePasteTarget() {
  const byName = new Map(entries.map(item => [item.name, item]));
  const sameTargets = [];
  const sameFiles = [];
  const sameDirs = [];

  for (const item of clipboard.items) {
    const targetPath = pathJoin(cwd, item.name);
    const existing = byName.get(item.name);
    if (item.path === targetPath) {
      sameTargets.push(item.name);
      continue;
    }
    if (item.type === "d" && pathIsSameOrChild(item.path, targetPath)) {
      return {
        ok: false,
        message: t("err_destination_inside_source")
      };
    }
    if (!existing) continue;

    if ((item.type === "d") !== (existing.type === "d")) {
      return {
        ok: false,
        message: t("removeConflictFirst", {
          existingType: typeLabel(existing.type),
          name: item.name,
          label: clipboard.op === "move" ? t("move") : t("copy"),
          sourceType: typeLabel(item.type)
        })
      };
    }
    if (item.type === "d") sameDirs.push(item.name);
    else sameFiles.push(item.name);
  }
  if (sameTargets.length) {
    return {
      ok: false,
      message: t("sameSourceTarget", {
        label: clipboard.op === "move" ? t("move") : t("copy"),
        name: conflictText(sameTargets)
      })
    };
  }

  return { ok: true, sameFiles, sameDirs };
}

function conflictText(names) {
  if (!names.length) return "";
  const shown = names.slice(0, 4).map(decodeFsText).join(", ");
  return names.length > 4 ? t("selectedItems", { name: shown, count: names.length }) : shown;
}

function renderPendingOverlay(text, label) {
  tasksEl.innerHTML = "";
  const div = document.createElement("div");
  div.className = "task running";

  const head = document.createElement("div");
  head.className = "task-head has-amount";
  const name = document.createElement("div");
  name.className = "task-name";
  name.textContent = label || t("preparingTask");
  const amount = document.createElement("div");
  amount.className = "task-amount";
  amount.textContent = t("pleaseWait");
  appendChildren(head, name, amount);

  const current = document.createElement("div");
  current.className = "task-current";
  current.textContent = text;

  const progress = document.createElement("div");
  progress.className = "progress";
  const progressText = document.createElement("div");
  progressText.className = "progress-text";
  progressText.textContent = t("checking");
  progress.appendChild(progressText);

  const cancel = document.createElement("button");
  cancel.className = "danger";
  cancel.textContent = t("cancel");
  bindPress(cancel, () => {
    if (pendingAbortController) pendingAbortController.abort();
    pendingAbortController = null;
    pendingOverlayText = "";
    pendingOverlayLabel = "";
    hideTaskOverlay();
    setStatus(t("cancelPrepare"));
    setBusy(false);
  });

  appendChildren(div, head, current, progress, cancel);
  tasksEl.appendChild(div);
}

function showPendingOverlay(text, label) {
  pendingOverlayText = text;
  pendingOverlayLabel = label || "";
  renderPendingOverlay(text, pendingOverlayLabel);
  showTaskOverlay(true);
}

function renderTaskPath(element, path) {
  path = displayPath(path);
  element.title = path;
  element.textContent = path;
  if (element.scrollWidth <= element.clientWidth) return;

  let low = 0;
  let high = path.length;
  while (low < high) {
    const count = Math.ceil((low + high) / 2);
    const left = Math.ceil(count / 2);
    const right = Math.floor(count / 2);
    element.textContent = path.slice(0, left) + "..." +
      (right ? path.slice(-right) : "");
    if (element.scrollWidth <= element.clientWidth) low = count;
    else high = count - 1;
  }

  const left = Math.ceil(low / 2);
  const right = Math.floor(low / 2);
  element.textContent = path.slice(0, left) + "..." +
    (right ? path.slice(-right) : "");
}

async function actionPaste() {
  if (busy || loadingPath || !clipboard || clipboard.items.length === 0) return;
  const op = clipboard.op;
  const label = clipboard.op === "move" ? t("move") : t("copy");
  const check = validatePasteTarget();
  if (!check.ok) {
    alert(check.message);
    setStatus(check.message);
    return;
  }

  const hasFileConflicts = check.sameFiles.length > 0;
  const hasDirConflicts = check.sameDirs.length > 0;
  if (hasFileConflicts || hasDirConflicts) {
    const lines = [];
    if (hasFileConflicts) lines.push(t("overwriteFiles", { names: conflictText(check.sameFiles) }));
    if (hasDirConflicts) lines.push(t("mergeDirs", { names: conflictText(check.sameDirs) }));
    if (!confirm(lines.join("\n") + "\n\n" + t("continueConfirm"))) return;
  }

  try {
    pendingAbortController = new AbortController();
    taskRefreshPath = cwd;
    showPendingOverlay(t("preparingPaste", { label }), label);
    setBusy(true);
    setStatus(t("preparingStatus", { label }));
    await nextPaint();
    const payload = {
      paths: clipboard.items.map(item => item.path).join("\n"),
      dst: cwd,
      overwrite: hasFileConflicts || hasDirConflicts ? "1" : "0"
    };
    const data = await apiForm("/api/" + op, payload, { signal: pendingAbortController.signal });
    pendingAbortController = null;
    pendingOverlayText = "";
    pendingOverlayLabel = "";
    setStatus(t("taskCreated", { label }));
    trackTask(data.task_id, op, true);
    clearSelection();
    await pollTasks();
  } catch (err) {
    const aborted = err && err.name === "AbortError";
    pendingAbortController = null;
    pendingOverlayText = "";
    pendingOverlayLabel = "";
    hideTaskOverlay();
    if (aborted) setStatus(t("cancelPrepare"));
    else showActionFailed(label, err.message);
    renderClipboard();
    setBusy(false);
  }
}

function renderTasks(tasks) {
  const active = tasks.find(task => task.state === "queued" || task.state === "running");
  if (!active && pendingOverlayText) {
    renderPendingOverlay(pendingOverlayText, pendingOverlayLabel);
    showTaskOverlay();
    return;
  }

  tasksEl.innerHTML = "";
  const hasActive = Boolean(active);
  if (hasActive) showTaskOverlay();
  else {
    hideTaskOverlay();
    setBusy(false);
  }

  if (!hasActive) return;

  const task = active;
  const total = Number(task.total || 0);
  const done = Number(task.done || 0);
  const speed = Number(task.speed || 0);
  const isDelete = task.op === "delete";
  const isPreparing = (task.op === "copy" || task.op === "move") &&
    task.state === "running" && done === 0;
  const isFinishing = !isDelete && task.state === "running" && total > 0 && done >= total;
  const pct = total > 0 ? Math.min(100, Math.floor(done * 100 / total)) : 0;
  const div = document.createElement("div");
  div.className = "task " + task.state;

  const head = document.createElement("div");
  head.className = "task-head";
  const name = document.createElement("div");
  name.className = "task-name";
  renderTaskName(name, task);
  head.appendChild(name);
  const elapsed = document.createElement("div");
  elapsed.className = "task-elapsed";
  elapsed.textContent = t("elapsedLabel") + ": " + taskElapsed(task);
  head.appendChild(elapsed);

  const current = document.createElement("div");
  current.className = "task-current";
  if (task.error) {
    current.className += " task-current-plain";
    current.textContent = task.error;
  }
  else current.textContent = displayPath(task.current || task.src);

  const meta = document.createElement("div");
  meta.className = "task-meta";
  const speedItem = document.createElement("div");
  speedItem.textContent = t("speedLabel") + ": " + formatSpeed(speed);
  const progressItem = document.createElement("div");
  progressItem.textContent = t("progressLabel") + ": " + formatSize(done) + " / " + formatSize(total);
  const etaItem = document.createElement("div");
  etaItem.textContent = t("etaLabel") + ": " + averageEta(task, done, total);
  appendChildren(meta, speedItem, progressItem, etaItem);

  const progress = document.createElement("div");
  progress.className = "progress";
  const bar = document.createElement("div");
  bar.className = "progress-bar";
  bar.style.width = isDelete ? "0" : pct + "%";
  const progressText = document.createElement("div");
  progressText.className = "progress-text";
  progressText.textContent = isPreparing ? t("preparing") : isFinishing ? t("finishing") : total ? pct + "%" : stateLabel(task.state);
  appendChildren(progress, bar, progressText);

  const cancel = document.createElement("button");
  cancel.className = "danger";
  cancel.textContent = task.cancel_requested ? t("canceling") : t("cancel");
  cancel.disabled = task.cancel_requested;
  bindPress(cancel, () => {
    if (confirm(t("cancelTaskConfirm", { label: opLabel(task.op) }))) {
      requestTaskCancel(task.id);
      api("/api/cancel", { id: task.id }).then(pollTasks).catch(err => {
        if (trackedTask && trackedTask.id === task.id) trackedTask.cancelRequested = false;
        setStatus(t("cancelFailed", { error: err.message }));
      });
    }
  });

  if (isDelete) appendChildren(div, head, current, cancel);
  else if (total && !isFinishing) appendChildren(div, head, current, meta, progress, cancel);
  else appendChildren(div, head, current, progress, cancel);
  tasksEl.appendChild(div);
  if (!task.error) renderTaskPath(current, task.current || task.src);
}

async function pollTasks() {
  try {
    const data = await api("/api/tasks");
    taskPollFailedAlertShown = false;
    const tasks = data.tasks || [];
    const completion = data.completion;
    if (lastCompletionId === null) {
      lastCompletionId = completion ? completion.id : 0;
    } else if (completion && completion.id !== lastCompletionId) {
      lastCompletionId = completion.id;
      alert(t(completion.file_count ? "transferCompleteFiles" : "transferComplete", {
        label: opLabel(completion.op),
        name: taskSubject(completion),
        duration: formatDuration(Number(completion.elapsed || 0)),
        size: formatBytes(completion.total, true),
        count: completion.file_count
      }));
    }
    const wasBusy = busy;
    renderTasks(tasks);
    if (wasBusy && !busy) startContentLoadingTimer();
    let shouldRefresh = false;
    let sawTrackedTask = false;
    for (const task of tasks) {
      if (trackedTask && trackedTask.id === task.id) {
        sawTrackedTask = true;
        if (task.cancel_requested) trackedTask.cancelRequested = true;
        if (trackedTask.state === task.state) continue;
        trackedTask.state = task.state;
      }
      if (isTerminalTask(task)) {
        shouldRefresh = true;
        handleTerminalTask(task);
      }
    }
    if (trackedTask && wasBusy && !busy && !sawTrackedTask) {
      if ((trackedTask.fromClipboard || trackedTask.clearClipboardOnDone) &&
          !trackedTask.cancelRequested) clearClipboardAfterPaste();
      clearTrackedTask();
      shouldRefresh = true;
    }
    if (shouldRefresh || (wasBusy && !busy)) {
      await load(taskRefreshPath || cwd, false);
      await refreshSpaces();
    }
  } catch (err) {
    const message = t("tasksPollFailed", { error: err.message });
    setStatus(message);
  }
}

function actionCopy() {
  setClipboard("copy");
}

function actionMove() {
  setClipboard("move");
}

function actionRename() {
  if (busy || loadingPath) return;
  const item = singleSelected();
  if (!item) return;
  const name = (prompt(t("renamePrompt"), displayName(item)) || "").trim();
  if (!name || name === displayName(item)) return;
  runImmediateAction(t("rename"), () => api("/api/rename", { path: item.path, name }));
}

function actionDelete() {
  if (busy || loadingPath || selected.size === 0) return;
  const items = selectedEntries();
  const paths = items.map(item => item.path);
  const target = itemTitle(items);
  const confirmKey = items.some(item => item.type === "d") ?
    "deleteConfirmRecursive" : "deleteConfirm";
  if (!confirm(t(confirmKey, { name: target }))) return;
  runAction(t("delete"), () => apiForm("/api/delete", { paths: paths.join("\n") }), {
    clearClipboardOnDone: pathsTouchClipboard(paths)
  });
}

function actionExit() {
  setStatus(t("exiting"));
  exitBtn.disabled = true;
  api("/api/exit").catch(err => {
    exitBtn.disabled = false;
    showActionFailed(t("exit"), err.message);
  });
}

document.getElementById("refreshBtn").addEventListener("click", () => load(cwd, false));
document.getElementById("mkdirBtn").addEventListener("click", () => {
  if (busy || loadingPath) return;
  const path = cwd;
  const name = (prompt(t("mkdirPrompt")) || "").trim();
  if (!name) return;
  runImmediateAction(t("mkdir"), () => api("/api/mkdir", { path, name }));
});
document.getElementById("copyBtn").addEventListener("click", actionCopy);
document.getElementById("moveBtn").addEventListener("click", actionMove);
pasteBtn.addEventListener("click", actionPaste);
clearClipboardBtn.addEventListener("click", clearClipboard);
document.getElementById("renameBtn").addEventListener("click", actionRename);
document.getElementById("deleteBtn").addEventListener("click", actionDelete);
exitBtn.addEventListener("click", actionExit);
textEditorCloseBtn.addEventListener("click", requestCloseTextEditor);
textEditorSaveBtn.addEventListener("click", saveTextEditor);
newTextBtn.addEventListener("click", actionNewText);
imagePreviewCloseBtn.addEventListener("click", closeImagePreview);
textEditorOverlayEl.addEventListener("click", event => {
  if (event.target === textEditorOverlayEl) requestCloseTextEditor();
});
imagePreviewOverlayEl.addEventListener("click", event => {
  if (event.target === imagePreviewOverlayEl) closeImagePreview();
});
contentEl.addEventListener("click", event => {
  if (!loadingPath) return;
  event.preventDefault();
  event.stopPropagation();
}, true);
filesEl.addEventListener("focusin", event => {
  const row = findRowFromTarget(event.target);
  if (row) focusPath(row.dataset.path);
});
filesEl.addEventListener("change", event => {
  const row = findRowFromTarget(event.target);
  if (row && event.target.type === "checkbox") togglePath(row.dataset.path, event.target.checked);
});
filesEl.addEventListener("click", event => {
  const row = findRowFromTarget(event.target);
  if (!row) return;
  let target = event.target;
  while (target && target !== row && !target.classList.contains("row-action")) target = target.parentNode;
  if (target === row && row.dataset.type !== "parent") return;
  const item = {
    path: row.dataset.path,
    name: row.dataset.name,
    type: row.dataset.type,
    displayName: decodeFsText(row.dataset.name)
  };
  focusPath(item.path);
  if (item.type === "parent" || item.type === "d") setTimeout(() => load(item.path), 0);
  else if (isPreviewableImage(item)) openImagePreview(item);
  else if (isEditableText(item)) openTextEditor(item);
  else togglePath(item.path, !selected.has(item.path));
});
selectAllEl.addEventListener("change", async () => {
  const checked = selectAllEl.checked;
  const useLoading = entries.length >= SELECT_ALL_LOADING_THRESHOLD;
  if (useLoading) {
    showContentLoading(t("processing"));
    await nextPaint();
  }
  selected.clear();
  if (checked) entries.forEach(item => selected.add(item.path));
  render();
  if (useLoading) hideContentLoading();
});
contentEl.addEventListener("mousemove", event => {
  hoverRow(findRowFromTarget(event.target));
});
contentEl.addEventListener("mouseleave", () => hoverRow(null));
contentEl.addEventListener("scroll", () => {
  hoverPaused = true;
  clearHoverPath();
  clearTimeout(hoverResumeTimer);
  hoverResumeTimer = setTimeout(() => {
    hoverPaused = false;
  }, 160);
});

async function init() {
  await loadLanguage();
  applyStaticText();
  const savedPath = readSavedPath();
  cwd = savedPath;
  pathEl.textContent = displayPath(cwd);
  taskRefreshPath = savedPath;
  refreshSpaces();
  await pollTasks();
  if (!busy) await load("/");
  else setStatus(t("activeTask"));
}

init();
setInterval(pollTasks, 1000);
setInterval(refreshSpaces, 10000);
