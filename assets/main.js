let cwd = "/";
let entries = [];
let selected = new Set();
let focusedPath = null;
let trackedTask = null;
let busy = false;
let clipboard = null;
let pendingOverlayText = "";
let pendingAbortController = null;
let taskRefreshPath = "/";
let hoverPaused = false;
let hoverResumeTimer = 0;
let L = {};

const APP_VERSION = "v0.2";
const LAST_PATH_KEY = "ps5-web-file-mgr:last-path";

const filesEl = document.getElementById("files");
const contentEl = document.getElementById("content");
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
const pasteTargetTextEl = document.getElementById("pasteTargetText");
const clearClipboardBtn = document.getElementById("clearClipboardBtn");
const initLoadingEl = document.getElementById("initLoading");
const exitBtn = document.getElementById("exitBtn");

function chooseLanguage() {
  const langs = navigator.languages && navigator.languages.length ? navigator.languages : [navigator.language || ""];
  const lang = String(langs[0] || "").toLowerCase();
  return lang.indexOf("zh") === 0 ? "zh" : "en";
}

function loadLanguage() {
  return new Promise(resolve => {
    const lang = chooseLanguage();
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

function api(path, params, options) {
  const qs = new URLSearchParams(params || {});
  const fetchOptions = Object.assign({ method: "POST" }, options || {});
  return fetch(path + (qs.toString() ? "?" + qs.toString() : ""), fetchOptions)
    .then(async response => {
      const data = await response.json();
      if (!response.ok || !data.ok) {
        throw new Error(backendErrorText(data.error_code, data.error_arg, data.error));
      }
      return data;
    });
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
  return seconds ? new Date(seconds * 1000).toLocaleString() : "";
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
  if (hours) return hours + "h " + minutes + "m";
  if (minutes) return minutes + "m " + secs + "s";
  return secs + "s";
}

function averageEta(task, done, total) {
  if (!total || !done || done >= total) return "--";
  const elapsed = Math.max(1, Number(task.elapsed || 0));
  const averageSpeed = done / elapsed;
  return averageSpeed > 0 ? formatDuration((total - done) / averageSpeed) : "--";
}

function opLabel(op) {
  return { copy: t("copy"), move: t("move"), delete: t("delete") }[op] || op;
}

function stateLabel(state) {
  return { queued: t("queued"), running: t("running"), done: t("done"), failed: t("failed"), canceled: t("canceled") }[state] || state;
}

function isTerminalTask(task) {
  return ["done", "failed", "canceled"].includes(task.state);
}

function trackTask(id, fromClipboard) {
  trackedTask = {
    id,
    fromClipboard: Boolean(fromClipboard),
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
    if (trackedTask && trackedTask.id === task.id && trackedTask.fromClipboard) clearClipboardAfterPaste();
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
  return items.length === 1 ? items[0].name : t("selectedItems", { name: items[0].name, count: items.length });
}

function pathName(path) {
  const clean = String(path || "").replace(/\/+$/, "");
  const pos = clean.lastIndexOf("/");
  return pos >= 0 ? clean.slice(pos + 1) || "/" : clean;
}

function renderTaskName(el, task) {
  const count = Number(task.src_count || 0);
  const target = count > 1 ? t("countItems", { count }) : pathName(task.src);
  const op = document.createElement("span");
  op.className = "task-name-op";
  op.textContent = opLabel(task.op);
  const subject = document.createElement("span");
  subject.className = "task-name-subject";
  subject.textContent = target;
  subject.title = target;
  const state = document.createElement("span");
  state.className = "task-name-state";
  state.textContent = stateLabel(task.state);
  appendChildren(el, op, subject, state);
}

function renderClipboard() {
  const hasClipboard = Boolean(clipboard && clipboard.items.length);
  pasteBtn.hidden = !hasClipboard;
  clearClipboardBtn.hidden = !hasClipboard;
  if (!hasClipboard) {
    pasteVerbEl.textContent = t("paste");
    pasteNameEl.textContent = "";
    pasteBtn.title = "";
    pasteBtn.disabled = true;
    clearClipboardBtn.disabled = true;
    return;
  }

  const title = clipboardTitle();
  pasteVerbEl.textContent = clipboard.op === "move" ? t("move") : t("paste");
  pasteNameEl.textContent = title;
  pasteBtn.title = t("pasteTitle", { label: clipboard.op === "move" ? t("move") : t("paste"), name: title, path: cwd });
  pasteBtn.disabled = busy;
  clearClipboardBtn.disabled = busy;
}

function singleSelected() {
  const items = selectedEntries();
  return items.length === 1 ? items[0] : null;
}

function updateButtons() {
  const items = selectedEntries();
  document.getElementById("copyBtn").disabled = busy || items.length === 0;
  document.getElementById("moveBtn").disabled = busy || items.length === 0;
  document.getElementById("renameBtn").disabled = busy || items.length !== 1;
  document.getElementById("deleteBtn").disabled = busy || items.length === 0;
  document.getElementById("mkdirBtn").disabled = busy;
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

function hoverPath(path) {
  if (hoverPaused) return;
  const old = filesEl.querySelector("tr.hovered");
  if (old && old.dataset.path === path) return;
  const next = path ? filesEl.querySelector("tr[data-path=\"" + cssEscape(path) + "\"]") : null;
  if (old) old.classList.remove("hovered");
  if (next) next.classList.add("hovered");
}

function clearHoverPath() {
  const old = filesEl.querySelector("tr.hovered");
  if (old) old.classList.remove("hovered");
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

function setClass(el, name, enabled) {
  if (enabled) el.classList.add(name);
  else el.classList.remove(name);
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
  render();
}

function render() {
  filesEl.innerHTML = "";
  emptyEl.hidden = entries.length !== 0;

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
    setClass(tr, "selected", !isParent && selected.has(item.path));
    setClass(tr, "focused", focusedPath === item.path);

    const selectTd = document.createElement("td");
    selectTd.className = "select-cell";
    if (!isParent) {
      const label = document.createElement("label");
      label.className = "select-hit";
      const checkbox = document.createElement("input");
      checkbox.type = "checkbox";
      checkbox.checked = selected.has(item.path);
      checkbox.addEventListener("focus", () => focusPath(item.path));
      checkbox.addEventListener("change", () => togglePath(item.path, checkbox.checked));
      label.appendChild(checkbox);
      selectTd.appendChild(label);
    }

    const nameTd = document.createElement("td");
    nameTd.className = "name-col";
    const nameBtn = document.createElement("button");
    nameBtn.className = "row-action";
    nameBtn.type = "button";
    const icon = document.createElement("span");
    icon.className = "icon";
    const iconImg = document.createElement("img");
    iconImg.src = isParent ? "/icon-up.png" : item.type === "d" ? "/icon-folder.png" : "/icon-file.png";
    iconImg.alt = "";
    icon.appendChild(iconImg);
    const nameText = document.createElement("span");
    nameText.className = "name-text";
    nameText.textContent = item.name;
    nameBtn.title = item.path;
    nameBtn.addEventListener("focus", () => focusPath(item.path));
    nameBtn.addEventListener("click", () => {
      focusPath(item.path);
      if (isParent || item.type === "d") load(item.path);
      else togglePath(item.path, !selected.has(item.path));
    });
    appendChildren(nameBtn, icon, nameText);
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
    filesEl.appendChild(tr);
  }
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
  if (busy && !force) return;
  try {
    setStatus(t("readDir"));
    const data = await api("/api/list", { path });
    cwd = data.path;
    pathEl.textContent = cwd;
    savePath(cwd);
    refreshSpaces();
    entries = data.entries.sort((a, b) => {
      if (a.type === "d" && b.type !== "d") return -1;
      if (a.type !== "d" && b.type === "d") return 1;
      return a.name.localeCompare(b.name);
    });
    selected.clear();
    focusedPath = null;
    render();
    if (shouldScrollTop) resetScrollTop();
    setStatus(t("totalItems", { count: entries.length }));
  } catch (err) {
    setStatus(err.message);
  }
}

async function runAction(label, fn) {
  try {
    taskRefreshPath = cwd;
    setBusy(true);
    setStatus(t("actionBusy", { label }));
    const data = await fn();
    setStatus(t("taskCreated", { label }));
    trackTask(data.task_id, false);
    selected.clear();
    render();
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
    selected.clear();
    render();
    await load(refreshPath, false, true);
    setStatus(t("actionDone", { label }));
  } catch (err) {
    showActionFailed(label, err.message);
  } finally {
    setBusy(false);
  }
}

function setClipboard(op) {
  if (busy || selected.size === 0) return;
  const items = selectedEntries().map(item => ({ path: item.path, name: item.name, type: item.type }));
  if (items.length === 0) return;
  clipboard = { op, items };
  selected.clear();
  render();
  setStatus(t("selectedForPaste", { name: itemTitle(items), verb: op === "move" ? t("move") : t("paste") }));
}

function clearClipboard() {
  clipboard = null;
  renderClipboard();
  setStatus(t("clipboardCleared"));
}

function validatePasteTarget() {
  const byName = new Map(entries.map(item => [item.name, item]));
  const sameFiles = [];
  const sameDirs = [];

  for (const item of clipboard.items) {
    const targetPath = pathJoin(cwd, item.name);
    const existing = byName.get(item.name);
    if (item.path === targetPath) {
      return {
        ok: false,
        message: t("sameSourceTarget", { label: clipboard.op === "move" ? t("move") : t("copy"), name: item.name })
      };
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

  return { ok: true, sameFiles, sameDirs };
}

function conflictText(names) {
  if (!names.length) return "";
  const shown = names.slice(0, 4).join(", ");
  return names.length > 4 ? t("selectedItems", { name: shown, count: names.length }) : shown;
}

function renderPendingOverlay(text) {
  tasksEl.innerHTML = "";
  const div = document.createElement("div");
  div.className = "task running";

  const head = document.createElement("div");
  head.className = "task-head has-amount";
  const name = document.createElement("div");
  name.className = "task-name";
  name.textContent = t("preparingTask");
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
    overlayEl.hidden = true;
    setStatus(t("cancelPrepare"));
    setBusy(false);
  });

  appendChildren(div, head, current, progress, cancel);
  tasksEl.appendChild(div);
}

function showPendingOverlay(text) {
  pendingOverlayText = text;
  overlayEl.hidden = false;
  renderPendingOverlay(text);
  setBusy(true);
}

async function actionPaste() {
  if (busy || !clipboard || clipboard.items.length === 0) return;
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

  const payload = {
    paths: clipboard.items.map(item => item.path).join("\n"),
    dst: cwd,
    overwrite: hasFileConflicts || hasDirConflicts ? "1" : "0"
  };
  try {
    pendingAbortController = new AbortController();
    taskRefreshPath = cwd;
    showPendingOverlay(t("preparingPaste", { label }));
    setStatus(t("preparingStatus", { label }));
    const data = await api("/api/" + op, payload, { signal: pendingAbortController.signal });
    pendingAbortController = null;
    pendingOverlayText = "";
    setStatus(t("taskCreated", { label }));
    trackTask(data.task_id, true);
    selected.clear();
    render();
    await pollTasks();
  } catch (err) {
    const aborted = err && err.name === "AbortError";
    pendingAbortController = null;
    pendingOverlayText = "";
    overlayEl.hidden = true;
    if (aborted) setStatus(t("cancelPrepare"));
    else showActionFailed(label, err.message);
    renderClipboard();
    setBusy(false);
  }
}

function renderTasks(tasks) {
  const active = tasks.find(task => task.state === "queued" || task.state === "running");
  if (!active && pendingOverlayText) {
    overlayEl.hidden = false;
    renderPendingOverlay(pendingOverlayText);
    setBusy(true);
    return;
  }

  tasksEl.innerHTML = "";
  const hasActive = Boolean(active);
  overlayEl.hidden = !hasActive;
  setBusy(hasActive);

  if (!hasActive) return;

  const task = active;
  const total = Number(task.total || 0);
  const done = Number(task.done || 0);
  const speed = Number(task.speed || 0);
  const isDelete = task.op === "delete";
  const isPreparing = (task.op === "copy" || task.op === "move") &&
    task.state === "running" && done === 0;
  const isFinishing = !isDelete && task.state === "running" && total > 0 && done >= total;
  const pct = total > 0 ? Math.min(100, Math.round(done * 100 / total)) : 0;
  const div = document.createElement("div");
  div.className = "task " + task.state;

  const head = document.createElement("div");
  head.className = "task-head";
  const name = document.createElement("div");
  name.className = "task-name";
  renderTaskName(name, task);
  head.appendChild(name);

  const current = document.createElement("div");
  current.className = "task-current";
  current.textContent = task.error || task.current || task.src;

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
}

async function pollTasks() {
  try {
    const data = await api("/api/tasks");
    const tasks = data.tasks || [];
    const wasBusy = busy;
    renderTasks(tasks);
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
      if (trackedTask.fromClipboard && !trackedTask.cancelRequested) clearClipboardAfterPaste();
      clearTrackedTask();
      shouldRefresh = true;
    }
    if (shouldRefresh || (wasBusy && !busy)) {
      await load(taskRefreshPath || cwd, false);
      await refreshSpaces();
    }
  } catch (err) {
    setStatus(t("tasksPollFailed", { error: err.message }));
  }
}

function actionCopy() {
  setClipboard("copy");
}

function actionMove() {
  setClipboard("move");
}

function actionRename() {
  if (busy) return;
  const item = singleSelected();
  if (!item) return;
  const name = prompt(t("renamePrompt"), item.name);
  if (!name || name === item.name) return;
  runImmediateAction(t("rename"), () => api("/api/rename", { path: item.path, name }));
}

function actionDelete() {
  if (busy || selected.size === 0) return;
  const items = selectedEntries();
  const paths = items.map(item => item.path);
  const target = itemTitle(items);
  const confirmKey = items.some(item => item.type === "d") ?
    "deleteConfirmRecursive" : "deleteConfirm";
  if (!confirm(t(confirmKey, { name: target }))) return;
  runAction(t("delete"), () => api("/api/delete", { paths: paths.join("\n") }));
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
  if (busy) return;
  const path = cwd;
  const name = prompt(t("mkdirPrompt"));
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
selectAllEl.addEventListener("change", () => {
  selected.clear();
  if (selectAllEl.checked) entries.forEach(item => selected.add(item.path));
  render();
});
contentEl.addEventListener("mousemove", event => {
  const row = findRowFromTarget(event.target);
  hoverPath(row ? row.getAttribute("data-path") : null);
});
contentEl.addEventListener("mouseleave", () => hoverPath(null));
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
  pathEl.textContent = cwd;
  taskRefreshPath = savedPath;
  refreshSpaces();
  await pollTasks();
  if (!busy) await load("/");
  else setStatus(t("activeTask"));
}

init();
setInterval(pollTasks, 1000);
setInterval(refreshSpaces, 10000);
