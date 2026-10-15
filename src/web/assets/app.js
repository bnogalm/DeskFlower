'use strict';

const CHUNK = 4 * 1024 * 1024;
const IMAGE = /\.(jpe?g|png|gif|webp|bmp|heic|heif)$/i;
// What the browser may open (the computer serves anything else as a download only).
const OPENS = /\.(jpe?g|png|gif|webp|bmp|avif|heic|heif|mp4|m4v|mov|webm|mkv|3gp|mp3|m4a|aac|wav|ogg|oga|opus|flac|txt|log|pdf)$/i;
let currentPath = '';

const $ = (id) => document.getElementById(id);

function toast(message) {
  const t = $('toast');
  t.textContent = message;
  t.hidden = false;
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => { t.hidden = true; }, 3000);
}

async function api(url, options = {}) {
  const response = await fetch(url, { credentials: 'same-origin', ...options });
  if (response.status === 401) {
    location.reload(); // shows the "scan again" page
    throw new Error('Session expired');
  }
  return response;
}

function formatSize(bytes) {
  if (bytes < 1024) return `${bytes} B`;
  const units = ['KB', 'MB', 'GB'];
  let value = bytes / 1024;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) { value /= 1024; unit++; }
  return `${value.toFixed(value < 10 ? 1 : 0)} ${units[unit]}`;
}

function joinPath(dir, name) { return dir ? `${dir}/${name}` : name; }

// --- Listing -------------------------------------------------------------------------------------

function renderCrumbs(path) {
  const nav = $('crumbs');
  nav.replaceChildren();
  const parts = path ? path.split('/') : [];
  const add = (label, target) => {
    const b = document.createElement('button');
    b.type = 'button';
    b.textContent = label;
    b.addEventListener('click', () => load(target));
    nav.append(b);
  };
  add('Shared folder', '');
  parts.forEach((part, i) => add(part, parts.slice(0, i + 1).join('/')));
}

function entryElement(entry) {
  const li = document.createElement('li');
  const path = joinPath(currentPath, entry.name);
  const link = document.createElement(entry.dir ? 'button' : 'a');
  link.className = 'entry';
  const thumb = document.createElement('div');
  thumb.className = 'thumb';

  if (entry.dir) {
    link.type = 'button';
    link.addEventListener('click', () => load(path));
    thumb.textContent = '📁';
  } else {
    link.href = `/api/file?path=${encodeURIComponent(path)}`;
    link.target = '_blank';
    link.rel = 'noopener';
    if (!OPENS.test(entry.name)) {
      // Not opened here, for safety: offer the download instead.
      link.addEventListener('click', (event) => {
        event.preventDefault();
        if (confirm(`"${entry.name}" cannot be opened in the browser, for safety. Download it to this phone?`))
          window.location.href = `/api/file?path=${encodeURIComponent(path)}&dl=1`;
      });
    }
    if (IMAGE.test(entry.name)) {
      const img = document.createElement('img');
      img.loading = 'lazy';
      img.alt = '';
      img.src = `/api/thumb?path=${encodeURIComponent(path)}&v=${entry.mtime}`;
      img.addEventListener('error', () => { thumb.textContent = '🖼️'; });
      thumb.append(img);
    } else {
      thumb.textContent = '📄';
    }
  }

  const meta = document.createElement('div');
  meta.className = 'meta';
  const title = document.createElement('div');
  title.className = 'title';
  title.textContent = entry.name;
  const sub = document.createElement('div');
  sub.className = 'sub';
  sub.textContent = entry.dir ? 'Folder' : `${formatSize(entry.size)} · ${new Date(entry.mtime).toLocaleDateString()}`;
  meta.append(title, sub);
  link.append(thumb, meta);
  li.append(link);

  if (!entry.dir) {
    const dl = document.createElement('a');
    dl.className = 'button download';
    dl.href = `/api/file?path=${encodeURIComponent(path)}&dl=1`;
    dl.textContent = 'Download';
    li.append(dl);
  }
  return li;
}

let listTag = '';  // ETag of the listing on screen
let loading = 0;   // generation: a newer load wins over a slower older one

// quiet: a refresh in the background (no message if it fails, nothing redrawn if unchanged).
async function load(path, quiet = false) {
  const generation = ++loading;
  const headers = quiet && path === currentPath && listTag ? { 'If-None-Match': listTag } : {};
  let response;
  try {
    response = await api(`/api/list?path=${encodeURIComponent(path)}`, { headers, cache: 'no-store' });
  } catch (e) {
    if (!quiet) toast('The computer is not reachable');
    return;
  }
  if (generation !== loading || response.status === 304) return;
  if (!response.ok) {
    if (quiet && path) { load('', true); return; } // the folder went away
    if (!quiet) toast('This folder is not available');
    if (path) load('');
    return;
  }
  const data = await response.json();
  if (generation !== loading) return;
  listTag = response.headers.get('ETag') || '';
  currentPath = data.path || '';
  renderCrumbs(currentPath);
  $('files').replaceChildren(...data.entries.map(entryElement));
  $('empty').hidden = data.entries.length > 0;
}

// Live view of the shared folder: polled while the page is visible.
const REFRESH_MS = 2000;
setInterval(() => { if (!document.hidden) load(currentPath, true); }, REFRESH_MS);
document.addEventListener('visibilitychange', () => { if (!document.hidden) load(currentPath, true); });

// --- Uploads -------------------------------------------------------------------------------------

async function upload(file) {
  const row = document.createElement('li');
  const name = document.createElement('div');
  name.className = 'name';
  name.textContent = file.name || 'photo.jpg';
  const bar = document.createElement('progress');
  bar.max = Math.max(file.size, 1);
  bar.value = 0;
  row.append(name, bar);
  $('uploads').prepend(row);

  const dir = currentPath;
  let offset = 0;
  let retries = 0;
  try {
    do {
      const chunk = file.slice(offset, offset + CHUNK);
      const query = new URLSearchParams({ path: dir, name: file.name || 'photo.jpg', offset, total: file.size });
      let response;
      try {
        response = await api(`/api/upload?${query}`, { method: 'PUT', body: chunk });
      } catch (e) {
        if (++retries > 5) throw e;
        await new Promise((r) => setTimeout(r, 1000 * retries));
        continue;
      }
      if (response.status === 409) { // resume where the computer has it
        offset = (await response.json()).expected;
        continue;
      }
      if (!response.ok) throw new Error((await response.json()).error || response.statusText);
      offset += chunk.size;
      retries = 0;
      bar.value = offset;
      if (offset >= file.size) {
        const result = await response.json();
        name.textContent = `✓ ${result.name || file.name}`;
        bar.remove();
      }
    } while (offset < file.size);
  } catch (e) {
    name.textContent = `${file.name}: ${e.message}`;
    name.className = 'name error';
    return;
  }
  setTimeout(() => row.remove(), 4000);
}

async function uploadAll(files) {
  for (const file of files) await upload(file); // one at a time keeps memory and Wi-Fi happy
  load(currentPath);
}

// --- Wiring --------------------------------------------------------------------------------------

document.addEventListener('DOMContentLoaded', async () => {
  for (const id of ['pick-files', 'take-photo', 'record-video']) {
    $(id).addEventListener('change', (event) => {
      const files = [...event.target.files];
      event.target.value = '';
      if (files.length) uploadAll(files);
    });
  }

  $('text-form').addEventListener('submit', async (event) => {
    event.preventDefault();
    const text = $('text').value;
    if (!text) return;
    const response = await api('/api/text', { method: 'POST', body: text,
      headers: { 'Content-Type': 'text/plain;charset=utf-8' } });
    if (response.ok) {
      $('text').value = '';
      toast('Sent to the clipboard');
    }
  });

  $('logout').addEventListener('click', async () => {
    await fetch('/api/logout', { method: 'POST', credentials: 'same-origin' });
    location.reload();
  });

  const info = await api('/api/info');
  if (info.ok) $('device').textContent = (await info.json()).name;
  load('');
});
