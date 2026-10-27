// SSTV panel (both builds). Picture being received, tuning strip, mode,
// signal, gallery of received pictures, and sending a picture.
//
// Host page helpers (same contract as js8-panel.mjs): send, streamDigiAudio,
// haltDigiTx, initDigiTxCtx, closeOtherModes, saveVfoSnapshot /
// restoreVfoSnapshot, currentFreq, currentMode, audioSampleRate, getMyCall,
// getMyGrid. RX audio arrives through window._sstvProcessAudioChunk(int16),
// called by the host's audio path. The clean receive path in index.html
// (modeCleanProfile 'sstv') sets the sideband by band - or the one a
// frequency entry names (window.sstvSidebandFor) - and turns the noise
// tools off while body.sstv-open is set.
//
// Band list and picture templates: on the server build they live in the
// settings file ([SSTV] Frequencies / Templates), arrive in rigInfo as
// window.sstvServerSettings and are saved with {cmd:'setSstvSettings'}, so
// every browser shows the same; the Standalone build keeps them in
// localStorage.
import { SSTV_MODES, sstvMode, sstvEncode, SstvDecoder, fskIdCodes, FSKID_BIT_MS } from './sstv.mjs';

const DEFAULT_FREQS = [
    { band: '80 m', hz: 3845000 },
    { band: '40 m', hz: 7171000 },
    { band: '20 m', hz: 14230000 },
    { band: '15 m', hz: 21340000 },
    { band: '10 m', hz: 28680000 },
];
const TX_MODES = ['s1', 'm1', 'r36', 'pd120', 'r72', 's2', 'm2', 'sdx', 'pd90'];

// Picture templates (QSSTV-style fields, filled at send time):
// %m my call, %l my locator, %f / %n my name, %c their call, %o their name,
// %g their locator, %r RSV report, %t time (UTC), %d date, %x %y %z comments.
// Items: {t:'text', text, x, y, size, color, align, box} or {t:'inset', x, y,
// w, h} (the picture received from them); x / y / w / h and size are
// fractions of the picture's width / height; x, y is the item's anchor
// (text: its baseline at align).
const BUILTIN_TEMPLATES = [
    { id: 'cq', name: 'CQ', bg: 'image', color: '#16222e', items: [
        { t: 'text', text: 'CQ de %m', x: 0.5, y: 0.15, size: 0.13, color: '#ffffff', align: 'center', box: '' },
        { t: 'text', text: '%l', x: 0.5, y: 0.94, size: 0.09, color: '#ffe066', align: 'center', box: '' },
    ] },
    { id: 'qsl', name: 'Reply / QSL', bg: 'color', color: '#14263d', items: [
        { t: 'text', text: 'TNX', x: 0.03, y: 0.11, size: 0.085, color: '#ffffff', align: 'left', box: '#b3261e' },
        { t: 'text', text: '%c . %r', x: 0.5, y: 0.11, size: 0.085, color: '#ffffff', align: 'center', box: '#1f6f43' },
        { t: 'text', text: '73', x: 0.97, y: 0.11, size: 0.085, color: '#ffffff', align: 'right', box: '#b3261e' },
        { t: 'inset', x: 0.03, y: 0.19, w: 0.58, h: 0.77 },
        { t: 'text', text: 'QSL', x: 0.81, y: 0.3, size: 0.1, color: '#ffe066', align: 'center', box: '' },
        { t: 'text', text: '%c', x: 0.81, y: 0.45, size: 0.085, color: '#ffffff', align: 'center', box: '' },
        { t: 'text', text: '%g', x: 0.81, y: 0.56, size: 0.07, color: '#cfe3f5', align: 'center', box: '' },
        { t: 'text', text: 'RSV %r', x: 0.81, y: 0.68, size: 0.07, color: '#cfe3f5', align: 'center', box: '' },
        { t: 'text', text: '73', x: 0.81, y: 0.92, size: 0.2, color: '#ffffff', align: 'center', box: '' },
    ] },
    { id: '73', name: '73', bg: 'image', color: '#16222e', items: [
        { t: 'text', text: '73 de %m', x: 0.5, y: 0.86, size: 0.14, color: '#ffffff', align: 'center', box: '' },
        { t: 'text', text: '%t UTC %d', x: 0.5, y: 0.95, size: 0.06, color: '#ffe066', align: 'center', box: '' },
    ] },
];
const RSV_LIST = ['595', '585', '575', '565', '555', '545', '535', '495', '485', '475', '455', '355', '255'];
// fields typed in the SEND tile, shown only when the template uses them
const FIELD_INPUTS = [
    { k: 'c', label: 'THEIR CALL', ph: 'Call', w: 8 },
    { k: 'o', label: 'NAME', ph: 'Their name', w: 10 },
    { k: 'g', label: 'LOC', ph: 'Locator', w: 6 },
    { k: 'r', label: 'RSV', ph: '595', w: 4, list: 'sstvRsvList' },
    { k: 'f', label: 'MY NAME', ph: 'Your name', w: 10, keep: true },
    { k: 'x', label: '%x', ph: 'Comment', w: 12, keep: true },
    { k: 'y', label: '%y', ph: 'Comment', w: 12, keep: true },
    { k: 'z', label: '%z', ph: 'Comment', w: 12, keep: true },
];

const MARKUP = `
<div id="sstvBar" class="sstv-bar hidden" aria-label="SSTV">
  <div class="sstv-head">
    <span class="sstv-chip sstv-chip-on">SSTV</span>
    <span class="sstv-chip" id="sstvFreq">---</span>
    <span class="sstv-chip" id="sstvState">RX: WAITING</span>
    <span class="sstv-info" id="sstvInfo"></span>
    <div class="sstv-bands" id="sstvBands" role="group" aria-label="SSTV calling frequencies"></div>
    <button type="button" class="sstv-btn sstv-band-edit" id="sstvBandEdit" title="Edit the SSTV frequencies">&#x270E;</button>
    <button type="button" class="sstv-btn" id="sstvCloseBtn">CLOSE</button>
  </div>
  <div class="sstv-body">
    <div class="sstv-left">
      <div class="sstv-pic-wrap"><canvas id="sstvImg" width="320" height="256"></canvas></div>
      <canvas id="sstvWf" width="400" height="48"></canvas>
      <div class="sstv-wf-scale"><span style="left:18.75%">1200 sync</span><span style="left:37.5%">1500 black</span><span style="left:87.5%">2300 white</span></div>
    </div>
    <div class="sstv-right">
      <div class="sstv-tile"><div class="sstv-lbl">MODE</div><div class="sstv-chips" id="sstvModeChips"></div></div>
      <div class="sstv-tile"><div class="sstv-lbl">SIGNAL</div>
        <div id="sstvSignal" class="sstv-sig">Waiting for a picture</div>
        <div class="sstv-meter"><div id="sstvSnrBar"></div></div></div>
      <div class="sstv-tile sstv-gal-tile"><div class="sstv-lbl" id="sstvGalLbl">RECEIVED</div>
        <div class="sstv-gal" id="sstvGal"></div></div>
      <div class="sstv-tile"><div class="sstv-lbl">SEND</div>
        <div class="sstv-send-row">
          <canvas id="sstvTxPrev" width="160" height="128" title="Picture to send"></canvas>
          <div class="sstv-send-ctl">
            <div class="sstv-send-line">
              <button type="button" class="sstv-btn" id="sstvPickBtn">PICK IMAGE</button>
              <select id="sstvTplSel" class="sstv-sel" title="Picture template"></select>
              <button type="button" class="sstv-btn" id="sstvTplEdit" title="Edit the templates">&#x270E;</button>
            </div>
            <input type="file" id="sstvFile" accept="image/*" hidden>
            <input type="text" id="sstvTxText" class="sstv-text" maxlength="40" placeholder="CALL GRID or a message">
            <div class="sstv-fields" id="sstvFields"></div>
            <datalist id="sstvRsvList">${RSV_LIST.map(v => `<option value="${v}">`).join('')}</datalist>
            <select id="sstvTxMode" class="sstv-sel"></select>
          </div>
        </div>
        <div class="sstv-send-row2">
          <button type="button" class="sstv-btn sstv-send" id="sstvSendBtn">SEND</button>
          <button type="button" class="sstv-chip sstv-chip-btn" id="sstvFskBtn" title="Send your callsign as FSK ID after the picture (MMSSTV / QSSTV format)">FSK ID</button>
          <span class="sstv-info" id="sstvTxInfo"></span>
        </div>
      </div>
    </div>
  </div>
  <div class="sstv-view hidden" id="sstvView">
    <div class="sstv-rx-banner hidden" id="sstvRxBanner" role="status" title="Show the picture being received">
      <canvas id="sstvRxThumb" width="64" height="51"></canvas><span id="sstvRxBannerTxt"></span>
    </div>
    <canvas id="sstvViewImg"></canvas>
    <div class="sstv-view-cap" id="sstvViewCap"></div>
    <div class="sstv-view-btns">
      <button type="button" class="sstv-btn" id="sstvViewSave">SAVE</button>
      <button type="button" class="sstv-btn" id="sstvViewReply" title="Use this picture as the inset of the Reply / QSL template">REPLY</button>
      <button type="button" class="sstv-btn" id="sstvViewDel">DELETE</button>
      <button type="button" class="sstv-btn" id="sstvViewClose">CLOSE</button>
    </div>
  </div>
  <div class="sstv-edit hidden" id="sstvFreqEdit" role="dialog" aria-label="SSTV frequencies">
    <div class="sstv-lbl">SSTV FREQUENCIES</div>
    <div class="sstv-edit-note">Band label, frequency in MHz, and the sideband (AUTO = LSB below 10 MHz, USB above).</div>
    <div class="sstv-edit-rows" id="sstvFreqRows"></div>
    <div class="sstv-edit-btns">
      <button type="button" class="sstv-btn" id="sstvFreqAdd">ADD</button>
      <button type="button" class="sstv-btn" id="sstvFreqReset">RESET</button>
      <span class="sstv-info" id="sstvFreqMsg"></span>
      <button type="button" class="sstv-btn" id="sstvFreqCancel">CANCEL</button>
      <button type="button" class="sstv-btn sstv-ok" id="sstvFreqSave">SAVE</button>
    </div>
  </div>
  <div class="sstv-edit hidden" id="sstvTplEditBox" role="dialog" aria-label="SSTV templates">
    <div class="sstv-edit-top">
      <select id="sstvTplEditSel" class="sstv-sel"></select>
      <input type="text" id="sstvTplName" class="sstv-text" maxlength="24" placeholder="Name">
      <button type="button" class="sstv-btn" id="sstvTplNew">NEW</button>
      <button type="button" class="sstv-btn" id="sstvTplDel">DELETE</button>
    </div>
    <div class="sstv-tpl-main">
      <canvas id="sstvTplPrev" width="320" height="256" title="Drag a text or the picture to move it"></canvas>
      <div class="sstv-tpl-side">
        <div class="sstv-edit-line">BACKGROUND
          <button type="button" class="sstv-chip sstv-chip-btn" id="sstvTplBgImg">PICKED IMAGE</button>
          <button type="button" class="sstv-chip sstv-chip-btn" id="sstvTplBgCol">COLOUR</button>
          <input type="color" id="sstvTplBgColor">
        </div>
        <div class="sstv-edit-note">Fields: %m my call, %l my locator, %f my name, %c their call, %o their name, %g their locator, %r RSV, %t time, %d date, %x %y %z comments. The picture item is the received picture (REPLY in the gallery, else the newest).</div>
        <div class="sstv-edit-rows" id="sstvTplItems"></div>
        <div class="sstv-edit-line">
          <button type="button" class="sstv-btn" id="sstvTplAddText">ADD TEXT</button>
          <button type="button" class="sstv-btn" id="sstvTplAddInset">ADD PICTURE</button>
        </div>
      </div>
    </div>
    <div class="sstv-edit-btns">
      <button type="button" class="sstv-btn" id="sstvTplResetAll" title="Put the built-in templates back and remove your own">RESET ALL</button>
      <span class="sstv-info" id="sstvTplMsg"></span>
      <button type="button" class="sstv-btn" id="sstvTplCancel">CANCEL</button>
      <button type="button" class="sstv-btn sstv-ok" id="sstvTplSave">SAVE</button>
    </div>
  </div>
</div>`;

const S = {
    visible: false, dec: null, rxMode: 'auto', fs: 0, state: 'idle', pic: null,
    txImg: null, txBusy: false, txTimer: null, fft: null, wfRow: 0, snr: null, snrSig: null, noiseHist: [],
    gal: [], viewing: null, picMeta: null,
    freqs: DEFAULT_FREQS.slice(), templates: null, fields: {}, insetRec: null, insetImg: null, insetFor: null,
    lastDone: null, edit: null,
};
const $ = id => document.getElementById(id);
const W_ = typeof window !== 'undefined' ? window : {};   // node: tests import the helpers
const host = name => (typeof W_[name] !== 'undefined' ? W_[name] : undefined);

function load(k, d) { try { const v = localStorage.getItem(k); return v === null ? d : v; } catch (e) { return d; } }
function save(k, v) { try { localStorage.setItem(k, v); } catch (e) {} }
function send_(o) { if (typeof send === 'function') send(o); }
const clone = o => JSON.parse(JSON.stringify(o));
const esc = s => String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

// ---------------------------------------------------------------------
// Shared settings: band list + templates
// ---------------------------------------------------------------------
function validFreqs(list) {
    if (!Array.isArray(list)) return null;
    const out = [];
    for (const e of list) {
        if (!e || typeof e !== 'object') continue;
        const hz = Math.round(+e.hz), band = String(e.band || '').trim().slice(0, 8);
        if (!(hz >= 100000 && hz <= 450000000) || !band) continue;
        out.push({ band, hz, sb: e.sb === 'USB' || e.sb === 'LSB' ? e.sb : '' });
    }
    return out.length ? out.sort((a, b) => a.hz - b.hz).slice(0, 20) : null;
}
function validTemplates(list) {
    if (!Array.isArray(list) || !list.length) return null;
    const out = [];
    const num = (v, d) => (typeof v === 'number' && isFinite(v) ? Math.max(0, Math.min(1, v)) : d);
    const col = (v, d) => (/^#[0-9a-fA-F]{6}$/.test(v || '') ? v : d);
    for (const t of list) {
        if (!t || !Array.isArray(t.items)) continue;
        const items = [];
        for (const it of t.items.slice(0, 20)) {
            if (it && it.t === 'inset') items.push({ t: 'inset', x: num(it.x, 0), y: num(it.y, 0), w: num(it.w, 0.5) || 0.5, h: num(it.h, 0.5) || 0.5 });
            else if (it && it.t === 'text') items.push({ t: 'text', text: String(it.text || '').slice(0, 60), x: num(it.x, 0.5), y: num(it.y, 0.5),
                size: Math.max(0.02, num(it.size, 0.1)), color: col(it.color, '#ffffff'), align: ['left', 'center', 'right'].includes(it.align) ? it.align : 'left',
                box: col(it.box, '') });
        }
        out.push({ id: String(t.id || ('u' + out.length)).slice(0, 16), name: String(t.name || 'Template').slice(0, 24),
                   bg: t.bg === 'color' ? 'color' : 'image', color: col(t.color, '#16222e'), items });
    }
    return out.length ? out.slice(0, 20) : null;
}
function templates() { return S.templates || BUILTIN_TEMPLATES; }
function serverBacked() { return typeof window.sstvServerSettings === 'object' && window.sstvServerSettings !== null; }
function applySettings(ss) {
    S.freqs = validFreqs(ss && ss.freqs) || DEFAULT_FREQS.slice();
    S.templates = validTemplates(ss && ss.templates);
    renderBands();
    renderTplSel();
    renderTxPreview();
}
W_.sstvApplySettings = applySettings;
function saveSettings(part) {
    if (serverBacked()) {
        send_(Object.assign({ cmd: 'setSstvSettings' }, part));
        window.sstvServerSettings = Object.assign({}, window.sstvServerSettings, part);
        applySettings(window.sstvServerSettings);
    } else {
        if (part.freqs) save('wfSstvFreqs', JSON.stringify(part.freqs));
        if (part.templates) save('wfSstvTemplates', JSON.stringify(part.templates));
        applySettings(localSettings());
    }
}
function localSettings() {
    let freqs = [], tpls = [];
    try { freqs = JSON.parse(load('wfSstvFreqs', '[]')); } catch (e) {}
    try { tpls = JSON.parse(load('wfSstvTemplates', '[]')); } catch (e) {}
    return { freqs, templates: tpls };
}
// An entry may name its sideband; the host's clean profile asks here.
W_.sstvSidebandFor = function (f) {
    const e = S.freqs.find(x => Math.abs(x.hz - f) <= 5000 && x.sb);
    return e ? e.sb : '';
};
function sidebandOf(e) { return e.sb || (e.hz >= 10000000 ? 'USB' : 'LSB'); }

// ---------------------------------------------------------------------
// Gallery (IndexedDB, per browser)
// ---------------------------------------------------------------------
const DB = { db: null };
function dbOpen() {
    if (DB.db) return Promise.resolve(DB.db);
    return new Promise((res, rej) => {
        let r;
        try { r = indexedDB.open('flexweb-sstv', 1); } catch (e) { rej(e); return; }
        r.onupgradeneeded = () => r.result.createObjectStore('pics', { keyPath: 'id', autoIncrement: true });
        r.onsuccess = () => { DB.db = r.result; res(DB.db); };
        r.onerror = () => rej(r.error);
    });
}
async function dbAll() {
    try {
        const db = await dbOpen();
        return await new Promise((res, rej) => {
            const q = db.transaction('pics').objectStore('pics').getAll();
            q.onsuccess = () => res(q.result || []); q.onerror = () => rej(q.error);
        });
    } catch (e) { return S.gal.slice(); }
}
function dbRec(rec) { const r = Object.assign({}, rec); delete r.url; return r; }
async function dbPut(rec) {
    try {
        const db = await dbOpen();
        await new Promise((res, rej) => {
            const tx = db.transaction('pics', 'readwrite'); const q = tx.objectStore('pics').add(dbRec(rec));
            q.onsuccess = () => { rec.id = q.result; res(); }; q.onerror = () => rej(q.error);
        });
    } catch (e) { rec.id = Date.now(); }
}
async function dbUpdate(rec) {
    try {
        const db = await dbOpen();
        await new Promise(res => { const tx = db.transaction('pics', 'readwrite'); tx.objectStore('pics').put(dbRec(rec)); tx.oncomplete = res; tx.onerror = res; });
    } catch (e) {}
}
async function dbDel(id) {
    try {
        const db = await dbOpen();
        await new Promise(res => { const tx = db.transaction('pics', 'readwrite'); tx.objectStore('pics').delete(id); tx.oncomplete = res; tx.onerror = res; });
    } catch (e) {}
}
const GAL_MAX = 60;

function picCanvas(w, h, rgba) {
    const c = document.createElement('canvas'); c.width = w; c.height = h;
    c.getContext('2d').putImageData(new ImageData(new Uint8ClampedArray(rgba), w, h), 0, 0);
    return c;
}
function fileStem(rec) {
    const d = new Date(rec.t), p = n => String(n).padStart(2, '0');
    const call = (typeof getMyCall === 'function' && getMyCall()) || 'SSTV';
    return `FlexWeb_SSTV_${call}_${d.getUTCFullYear()}${p(d.getUTCMonth() + 1)}${p(d.getUTCDate())}_${p(d.getUTCHours())}${p(d.getUTCMinutes())}${p(d.getUTCSeconds())}`
        + (rec.freq ? `_${Math.round(rec.freq / 1000)}kHz` : '') + `_${rec.mode.toUpperCase()}` + (rec.call ? `_${rec.call.replace(/[^A-Z0-9]/g, '')}` : '');
}
function recTitle(rec) {
    return `${rec.name}${rec.call ? ' · ' + rec.call : ''} · ${new Date(rec.t).toLocaleString()}` + (rec.freq ? ` · ${(rec.freq / 1e6).toFixed(3)} MHz` : '');
}

async function galleryAdd(ev) {
    const c = picCanvas(ev.w, ev.h, ev.rgba);
    const blob = await new Promise(res => c.toBlob(res, 'image/png'));
    const rec = { t: Date.now(), mode: ev.mode, name: ev.name, w: ev.w, h: ev.h, freq: host('currentFreq') || 0,
                  lines: ev.line, of: ev.lines, why: ev.why, png: blob };
    // an FSK ID that arrived while the PNG was being made
    if (S.lastFsk && Date.now() - S.lastFsk.t < 15000 && !S.lastFsk.used) { rec.call = S.lastFsk.text; S.lastFsk.used = true; }
    await dbPut(rec);
    S.gal.unshift(rec);
    S.lastDone = { rec, t: Date.now() };
    while (S.gal.length > GAL_MAX) { const old = S.gal.pop(); dbDel(old.id); }
    renderGallery();
    renderTxPreview();
}
async function galleryLoad() {
    const all = await dbAll();
    S.gal = all.sort((a, b) => b.t - a.t).slice(0, GAL_MAX);
    renderGallery();
}
function renderGallery() {
    const g = $('sstvGal'); if (!g) return;
    g.innerHTML = '';
    $('sstvGalLbl').textContent = 'RECEIVED · ' + S.gal.length;
    for (const rec of S.gal) {
        const im = document.createElement('img');
        if (!rec.url && rec.png) rec.url = URL.createObjectURL(rec.png);
        im.src = rec.url || '';
        im.title = recTitle(rec);
        if (rec === S.insetRec) im.className = 'sstv-gal-reply';
        im.onclick = () => openView(rec);
        g.appendChild(im);
    }
    if (!S.gal.length) g.innerHTML = '<div class="sstv-empty">Pictures you receive appear here.</div>';
}
function openView(rec) {
    S.viewing = rec;
    const img = new Image();
    img.onload = () => {
        const c = $('sstvViewImg'); c.width = img.naturalWidth; c.height = img.naturalHeight;
        c.getContext('2d').drawImage(img, 0, 0);
    };
    img.src = rec.url;
    $('sstvViewCap').textContent = recTitle(rec) + (rec.lines < rec.of ? ` · ${rec.lines}/${rec.of} lines` : '');
    $('sstvView').classList.remove('hidden');
    updateRxBanner();
}
function closeView() { $('sstvView').classList.add('hidden'); S.viewing = null; }

// "Receiving" banner over the full-size viewer (punch list 14): a picture
// can start while an old one is open; the viewer stays, the banner shows
// the new one's progress and a tap goes to it.
function updateRxBanner(done) {
    const b = $('sstvRxBanner');
    if (!b) return;
    const viewerOpen = !$('sstvView').classList.contains('hidden');
    if (!viewerOpen || (S.state !== 'rx' && !done && !b.dataset.done)) { b.classList.add('hidden'); if (!viewerOpen) delete b.dataset.done; return; }
    const t = $('sstvRxBannerTxt');
    if (done) { b.dataset.done = '1'; t.textContent = 'New picture received - tap to view'; }
    else if (S.state === 'rx' && S.pic) {
        delete b.dataset.done;
        t.textContent = `Receiving ${S.pic.name}... line ${S.progLine || 0} / ${S.pic.lines || '?'}`;
    }
    const th = $('sstvRxThumb'), src = $('sstvImg');
    th.getContext('2d').drawImage(src, 0, 0, th.width, th.height);
    b.classList.remove('hidden');
}

// ---------------------------------------------------------------------
// Receive
// ---------------------------------------------------------------------
function newDecoder() {
    S.fs = host('audioSampleRate') || 48000;
    S.dec = new SstvDecoder(S.fs, { mode: S.rxMode, onEvent: onDecEvent });
}
function onDecEvent(ev) {
    const c = $('sstvImg');
    if (ev.type === 'start') {
        S.state = 'rx'; S.pic = Object.assign({}, ev, { lines: sstvMode(ev.mode).lines }); S.picMeta = { t0: Date.now() };
        S.progLine = 0;
        c.width = ev.w; c.height = ev.h;
        const g = c.getContext('2d'); g.fillStyle = '#000'; g.fillRect(0, 0, ev.w, ev.h);
        setState(`RX: ${ev.name.toUpperCase()}`, 'ok');
        setInfo(ev.late ? 'joined a picture under way (no VIS)' : 'VIS received');
        updateRxBanner();
    } else if (ev.type === 'progress') {
        drawPic(ev);
        S.progLine = ev.line;
        setInfo(`line ${ev.line} / ${ev.lines} · slant ${fmtPpm(ev.slant)}${S.pic && S.pic.late ? ' · joined late' : ''}`);
        setSignal(ev);
        updateRxBanner();
    } else if (ev.type === 'done') {
        drawPic(ev);
        S.state = 'idle';
        setState('RX: WAITING', '');
        setInfo(`${ev.name} ${ev.why} · ${ev.line} / ${ev.lines} lines`);
        const keep = ev.line >= 0.3 * ev.lines;
        if (keep) galleryAdd(ev);
        updateRxBanner(keep);
    } else if (ev.type === 'fskid') {
        onFskId(ev.text);
    }
}
// FSK ID after a picture (punch list 11): the sender's call goes on the
// picture in the gallery, the status line and THEIR CALL (%c).
function onFskId(text) {
    S.lastFsk = { text, t: Date.now(), used: false };
    setInfo(($('sstvInfo').textContent ? $('sstvInfo').textContent + ' · ' : '') + 'FSK ID ' + text);
    S.fields.c = text;
    const inp = $('sstvFld_c'); if (inp) inp.value = text;
    const last = S.gal[0];
    if (last && S.lastDone && S.lastDone.rec === last && Date.now() - S.lastDone.t < 15000 && !last.call) {
        last.call = text; S.lastFsk.used = true;
        dbUpdate(last);
        renderGallery();
    }
    renderTxPreview();
}
function fmtPpm(p) { return Math.abs(p) < 30 ? 'none' : (p > 0 ? '+' : '') + Math.round(p) + ' ppm'; }
function drawPic(ev) {
    const c = $('sstvImg');
    if (c.width !== ev.w || c.height !== ev.h) { c.width = ev.w; c.height = ev.h; }
    c.getContext('2d').putImageData(new ImageData(new Uint8ClampedArray(ev.rgba), ev.w, ev.h), 0, 0);
    const y = Math.min(ev.h - 1, Math.round(ev.line * ev.h / ev.lines));
    if (ev.type === 'progress' && ev.line < ev.lines) {
        const g = c.getContext('2d'); g.fillStyle = '#3ad'; g.fillRect(0, y, ev.w, 1);
    }
}
function setState(t, cls) { const el = $('sstvState'); el.textContent = t; el.className = 'sstv-chip' + (cls ? ' sstv-chip-' + cls : ''); }
function setInfo(t) { $('sstvInfo').textContent = t; }
function setSignal(ev) {
    const parts = [];
    parts.push(ev.syncPct >= 80 ? 'Sync locked' : ev.syncPct >= 40 ? 'Sync weak' : 'Sync lost');
    parts.push(S.snr === null ? 'SNR -' : `SNR ${Math.round(S.snr)} dB`);
    parts.push(`offset ${ev.offset >= 0 ? '+' : ''}${Math.round(ev.offset)} Hz`);
    $('sstvSignal').textContent = parts.join(' · ');
    const pct = S.snr === null ? ev.syncPct : Math.max(0, Math.min(100, (S.snr + 5) * 3));
    $('sstvSnrBar').style.width = pct + '%';
}

W_._sstvProcessAudioChunk = function (int16) {
    if (!S.visible || !S.dec || S.txBusy) return;
    const x = new Float32Array(int16.length);
    for (let i = 0; i < int16.length; i++) x[i] = int16[i] / 32768;
    S.dec.push(x);
    feedWaterfall(x);
};

// Tuning strip: 512-point FFT of the audio at ~12 kHz, 900..2500 Hz.
function feedWaterfall(x) {
    const D = Math.max(1, Math.round(S.fs / 12000)), R = S.fs / D;
    if (!S.fft) S.fft = { buf: new Float32Array(512), pos: 0, ph: 0, acc: 0 };
    const F = S.fft;
    for (let i = 0; i < x.length; i++) {
        F.acc += x[i];
        if (++F.ph < D) continue;
        F.buf[F.pos++] = F.acc / D; F.ph = 0; F.acc = 0;
        if (F.pos === 512) { F.pos = 0; wfRow(F.buf, R); }
    }
}
function fftMag(buf) {
    const N = buf.length, re = new Float32Array(N), im = new Float32Array(N);
    for (let i = 0; i < N; i++) re[i] = buf[i] * (0.5 - 0.5 * Math.cos(2 * Math.PI * i / (N - 1)));
    for (let i = 1, j = 0; i < N; i++) {
        let b = N >> 1; for (; j & b; b >>= 1) j ^= b; j ^= b;
        if (i < j) { [re[i], re[j]] = [re[j], re[i]]; }
    }
    for (let len = 2; len <= N; len <<= 1) {
        const a = -2 * Math.PI / len;
        for (let i = 0; i < N; i += len) for (let k = 0; k < len / 2; k++) {
            const c = Math.cos(a * k), s = Math.sin(a * k), u = i + k, v = u + len / 2;
            const tr = re[v] * c - im[v] * s, ti = re[v] * s + im[v] * c;
            re[v] = re[u] - tr; im[v] = im[u] - ti; re[u] += tr; im[u] += ti;
        }
    }
    const m = new Float32Array(N / 2);
    for (let i = 0; i < N / 2; i++) m[i] = re[i] * re[i] + im[i] * im[i];
    return m;
}
// SNR inside the receive passband (punch list 12): the old estimate used
// 2500-2800 Hz as the noise, which a 2.7 kHz filter removes (-157 dB). The
// SSTV tone occupies a few bins of the 1000-2400 Hz band at a time, so the
// lower bins of each spectrum are noise: their level over the last ~2 s
// gives the noise density.
const SNR_LO = 1000, SNR_HI = 2400;
export function sstvSnrStep(state, m, hz) {
    const band = [];
    for (let i = Math.ceil(SNR_LO / hz); i <= Math.floor(SNR_HI / hz) && i < m.length; i++) band.push(m[i]);
    if (band.length < 8) return state.snr;
    let total = 0; for (const v of band) total += v;
    const sorted = band.slice().sort((a, b) => a - b);
    const floor = sorted[Math.floor(sorted.length * 0.25)];
    state.noiseHist.push(floor);
    if (state.noiseHist.length > 48) state.noiseHist.shift();
    // the 25th percentile of noise-only bins (exponentially distributed
    // power) is -ln(0.75) = 0.288 of their mean. During a picture line the
    // tones sweep across the band, so take the quietest frame of ~2 s (a
    // sync or porch, where the tone sits in a few bins). Checked against
    // known noise: within ~3 dB from 0 to 30 dB.
    const hist = state.noiseHist.slice().sort((a, b) => a - b);
    const nd = hist[0] / 0.2877;
    if (!(nd > 0) || !(total > 0)) return state.snr;
    const sig = total - nd * band.length;
    state.snrSig = state.snrSig === null ? sig : state.snrSig + 0.1 * (sig - state.snrSig);
    if (!(state.snrSig > 0)) return null;
    // signal power against the noise in a 2.5 kHz SSB channel
    const snr = 10 * Math.log10(state.snrSig / (nd * 2500 / hz));
    return Math.max(-10, Math.min(50, snr));
}
const WF_LO = 900, WF_HI = 2500;
function wfRow(buf, R) {
    const c = $('sstvWf'); if (!c) return;
    const g = c.getContext('2d'), W = c.width, H = c.height;
    const m = fftMag(buf), hz = R / buf.length;
    S.snr = sstvSnrStep(S, m, hz);
    g.drawImage(c, 0, 0, W, H - 1, 0, 1, W, H - 1);
    const row = g.createImageData(W, 1);
    let mx = 1e-12; for (let i = 0; i < m.length; i++) if (m[i] > mx) mx = m[i];
    for (let x = 0; x < W; x++) {
        const f = WF_LO + (WF_HI - WF_LO) * x / W, b = Math.min(m.length - 1, Math.round(f / hz));
        const v = Math.max(0, Math.min(1, (10 * Math.log10(m[b] / mx) + 50) / 50));
        const i = x * 4;
        row.data[i] = Math.round(255 * Math.max(0, v * 2 - 1)); row.data[i + 1] = Math.round(255 * Math.min(1, v * 1.6) * 0.85);
        row.data[i + 2] = Math.round(255 * Math.min(1, 0.25 + v)); row.data[i + 3] = 255;
    }
    g.putImageData(row, 0, 0);
    // markers
    for (const [f, col] of [[1200, '#ff5050'], [1500, '#aaaaaa'], [2300, '#ffffff']]) {
        const x = Math.round((f - WF_LO) / (WF_HI - WF_LO) * W);
        g.fillStyle = col; g.fillRect(x, 0, 1, 4);
    }
}

// ---------------------------------------------------------------------
// Templates
// ---------------------------------------------------------------------
function pad2(n) { return String(n).padStart(2, '0'); }
function fieldValues() {
    const now = new Date();
    const f = S.fields;
    const call = (typeof getMyCall === 'function' && getMyCall()) || '';
    const grid = (typeof getMyGrid === 'function' && getMyGrid()) || '';
    return {
        m: call.toUpperCase(), l: grid.toUpperCase(), f: f.f || '', n: f.f || '', c: (f.c || '').toUpperCase(), o: f.o || '',
        g: (f.g || '').toUpperCase(), r: f.r || '595', t: `${pad2(now.getUTCHours())}:${pad2(now.getUTCMinutes())}`,
        d: `${now.getUTCFullYear()}-${pad2(now.getUTCMonth() + 1)}-${pad2(now.getUTCDate())}`, x: f.x || '', y: f.y || '', z: f.z || '',
    };
}
function expandFields(text, vals) { return String(text).replace(/%([a-z])/g, (m, k) => (k in vals ? vals[k] : m)); }
function usedFields(tpl) {
    const used = new Set();
    for (const it of tpl.items) if (it.t === 'text') for (const m of it.text.matchAll(/%([a-z])/g)) used.add(m[1] === 'n' ? 'f' : m[1]);
    return used;
}
function drawCover(g, img, x, y, w, h) {
    const iw = img.naturalWidth || img.width, ih = img.naturalHeight || img.height;
    if (!iw || !ih) return;
    const k = Math.max(w / iw, h / ih), sw = w / k, sh = h / k;
    g.drawImage(img, (iw - sw) / 2, (ih - sh) / 2, sw, sh, x, y, w, h);
}
// The picture for an inset: REPLY in the viewer, else the newest received.
function insetSource() {
    const rec = S.insetRec || S.gal[0] || null;
    if (!rec || !rec.url) return null;
    if (S.insetFor !== rec) {
        S.insetFor = rec; S.insetImg = null;
        const img = new Image();
        img.onload = () => { if (S.insetFor === rec) { S.insetImg = img; renderTxPreview(); if (S.edit) renderTplEditPreview(); } };
        img.src = rec.url;
    }
    return S.insetImg;
}
function renderTemplate(g, W, H, tpl, vals, opts = {}) {
    if (tpl.bg === 'image' && S.txImg) drawCover(g, S.txImg, 0, 0, W, H);
    else { g.fillStyle = tpl.color || '#16222e'; g.fillRect(0, 0, W, H); }
    tpl.items.forEach((it, idx) => {
        if (it.t === 'inset') {
            const x = it.x * W, y = it.y * H, w = it.w * W, h = it.h * H;
            const img = insetSource();
            if (img) drawCover(g, img, x, y, w, h);
            else {
                g.fillStyle = '#26384a'; g.fillRect(x, y, w, h);
                g.fillStyle = '#8fb3cc'; g.font = `bold ${Math.round(H / 16)}px sans-serif`; g.textAlign = 'center';
                g.fillText('RECEIVED PICTURE', x + w / 2, y + h / 2); g.textAlign = 'left';
            }
            g.strokeStyle = '#ffffff'; g.lineWidth = Math.max(1, W / 200); g.strokeRect(x, y, w, h);
        } else {
            const text = expandFields(it.text, vals);
            if (!text.trim()) return;
            const fsz = Math.max(6, Math.round(it.size * H));
            g.font = `bold ${fsz}px sans-serif`;
            g.textAlign = it.align; g.textBaseline = 'alphabetic';
            const x = it.x * W, y = it.y * H, tw = g.measureText(text).width;
            const left = it.align === 'center' ? x - tw / 2 : it.align === 'right' ? x - tw : x;
            if (it.box) {
                const p = fsz * 0.25;
                g.fillStyle = it.box; g.fillRect(left - p, y - fsz * 0.85, tw + 2 * p, fsz * 1.1);
            } else {
                g.lineWidth = Math.max(2, fsz / 6); g.strokeStyle = '#000'; g.strokeText(text, x, y);
            }
            g.fillStyle = it.color; g.fillText(text, x, y);
            g.textAlign = 'left';
        }
        if (opts.sel === idx) {
            const b = itemBox(g, it, W, H, vals);
            g.strokeStyle = '#ffd400'; g.lineWidth = 1; g.setLineDash([4, 3]); g.strokeRect(b.x, b.y, b.w, b.h); g.setLineDash([]);
        }
    });
}
function itemBox(g, it, W, H, vals) {
    if (it.t === 'inset') return { x: it.x * W, y: it.y * H, w: it.w * W, h: it.h * H };
    const fsz = Math.max(6, Math.round(it.size * H));
    g.font = `bold ${fsz}px sans-serif`;
    const tw = Math.max(fsz, g.measureText(expandFields(it.text, vals)).width);
    const x = it.x * W, left = it.align === 'center' ? x - tw / 2 : it.align === 'right' ? x - tw : x;
    return { x: left, y: it.y * H - fsz * 0.9, w: tw, h: fsz * 1.15 };
}
function currentTemplate() {
    const id = $('sstvTplSel') ? $('sstvTplSel').value : '';
    return templates().find(t => t.id === id) || null;
}
function renderTplSel() {
    const sel = $('sstvTplSel'); if (!sel) return;
    const want = sel.value || load('wfSstvTpl', '');
    sel.innerHTML = '<option value="">Text only</option>' + templates().map(t => `<option value="${esc(t.id)}">${esc(t.name)}</option>`).join('');
    sel.value = templates().some(t => t.id === want) ? want : '';
    renderFields();
}
function renderFields() {
    const box = $('sstvFields'); if (!box) return;
    const tpl = currentTemplate();
    $('sstvTxText').classList.toggle('hidden', !!tpl);
    box.innerHTML = '';
    if (!tpl) return;
    const used = usedFields(tpl);
    for (const f of FIELD_INPUTS) {
        if (!used.has(f.k)) continue;
        const lab = document.createElement('label');
        lab.className = 'sstv-field';
        lab.innerHTML = `<span>${esc(f.label)}</span>`;
        const inp = document.createElement('input');
        inp.type = 'text'; inp.className = 'sstv-text'; inp.id = 'sstvFld_' + f.k;
        inp.placeholder = f.ph; inp.size = f.w; inp.maxLength = 24;
        if (f.list) inp.setAttribute('list', f.list);
        inp.value = S.fields[f.k] || (f.k === 'r' ? '595' : '');
        inp.oninput = () => {
            S.fields[f.k] = inp.value;
            if (f.keep) save('wfSstvFld_' + f.k, inp.value);
            renderTxPreview();
        };
        lab.appendChild(inp);
        box.appendChild(lab);
    }
}

// ---------------------------------------------------------------------
// Send
// ---------------------------------------------------------------------
function txMode() { return sstvMode($('sstvTxMode').value) || sstvMode('s1'); }
function fskOn() { return load('wfSstvFskId', '1') === '1'; }
function fskCall() { return fskOn() ? ((typeof getMyCall === 'function' && getMyCall()) || '').toUpperCase() : ''; }
function txSeconds(m) { return m.seconds + fskIdCodes(fskCall()).length * 6 * FSKID_BIT_MS / 1000; }
function renderTxPreview() {
    const c = $('sstvTxPrev'); if (!c) return;
    const m = txMode();
    c.width = m.w; c.height = m.h;
    const g = c.getContext('2d');
    const tpl = currentTemplate();
    if (tpl) {
        renderTemplate(g, m.w, m.h, tpl, fieldValues());
    } else {
        g.fillStyle = '#16222e'; g.fillRect(0, 0, m.w, m.h);
        if (S.txImg) drawCover(g, S.txImg, 0, 0, m.w, m.h);
        else {
            g.fillStyle = '#6f8aa0'; g.font = `bold ${Math.round(m.h / 12)}px sans-serif`; g.textAlign = 'center';
            g.fillText('PICK IMAGE', m.w / 2, m.h / 2); g.textAlign = 'left';
        }
        const text = $('sstvTxText').value.trim();
        if (text) {
            const fsz = Math.round(m.h / 9);
            g.font = `bold ${fsz}px sans-serif`;
            g.lineWidth = Math.max(2, fsz / 6); g.strokeStyle = '#000'; g.fillStyle = '#fff';
            g.strokeText(text, fsz / 2, fsz * 1.15); g.fillText(text, fsz / 2, fsz * 1.15);
        }
    }
    const secs = Math.round(txSeconds(m));
    $('sstvTxInfo').textContent = `${m.name} · ${secs} s on air`;
    $('sstvSendBtn').textContent = S.txBusy ? 'STOP' : `SEND · ${secs} s`;
    const fb = $('sstvFskBtn');
    fb.className = 'sstv-chip sstv-chip-btn' + (fskOn() ? ' sstv-chip-on' : '');
}
function canSend() {
    const tpl = currentTemplate();
    return S.txImg || (tpl && tpl.bg === 'color');
}
function startTx() {
    if (S.txBusy) { stopTx(); return; }
    if (!canSend()) { $('sstvTxInfo').textContent = 'Pick an image first'; return; }
    if (typeof streamDigiAudio !== 'function') { $('sstvTxInfo').textContent = 'Sending is not available here'; return; }
    renderTxPreview();
    const m = txMode(), c = $('sstvTxPrev');
    const rgba = c.getContext('2d').getImageData(0, 0, m.w, m.h).data;
    const audio = sstvEncode(m.id, rgba, 12000, 0.8, { fskId: fskCall() });
    const secs = audio.length / 12000;
    // show what is going out in the main picture
    const big = $('sstvImg'); big.width = m.w; big.height = m.h; big.getContext('2d').drawImage(c, 0, 0);
    if (typeof initDigiTxCtx === 'function') initDigiTxCtx();
    S.txBusy = true;
    if (S.dec) S.dec.stop();
    if (typeof digiTxActive !== 'undefined') window.digiTxActive = true;
    setState('TX: ' + m.name.toUpperCase(), 'tx');
    renderTxPreview();
    send_({ cmd: 'enableMic', value: true });
    let t0 = 0;
    streamDigiAudio(audio, function (aborted) {
        S.txBusy = false;
        if (S.txTimer) { clearInterval(S.txTimer); S.txTimer = null; }
        if (typeof digiTxActive !== 'undefined') window.digiTxActive = false;
        send_({ cmd: 'setPTT', value: false });
        setState('RX: WAITING', '');
        renderTxPreview();
        $('sstvTxInfo').textContent = aborted ? 'Stopped' : `Sent ${m.name}`;
        if (S.dec) S.dec.reset();
    }, function () {
        send_({ cmd: 'setPTT', value: true });
        t0 = Date.now();
        S.txTimer = setInterval(function () {
            const s = (Date.now() - t0) / 1000;
            $('sstvTxInfo').textContent = `Sending ${m.name} · ${Math.round(s)} / ${Math.round(secs)} s`;
        }, 500);
    });
}
function stopTx() {
    if (!S.txBusy) return;
    if (typeof window.haltDigiTx === 'function') window.haltDigiTx();
    else { send_({ cmd: 'setPTT', value: false }); S.txBusy = false; }
}

// ---------------------------------------------------------------------
// Band buttons + frequency editor (punch list 9)
// ---------------------------------------------------------------------
function renderBands() {
    const box = $('sstvBands'); if (!box) return;
    const f = host('currentFreq') || 0;
    box.innerHTML = '';
    for (const e of S.freqs) {
        const b = document.createElement('button');
        b.type = 'button';
        b.className = 'sstv-band' + (Math.abs(f - e.hz) <= 3000 ? ' on' : '');
        const sb = sidebandOf(e);
        b.title = `${(e.hz / 1e6).toFixed(3)} MHz ${sb}`;
        b.innerHTML = `<b>${esc(e.band)}</b><small>${(e.hz / 1e6).toFixed(3)} ${sb}</small>`;
        b.onclick = () => send_({ cmd: 'setFrequency', value: e.hz });
        box.appendChild(b);
    }
    box.dataset.f = String(Math.round(f / 1000));
}
function freqRow(e) {
    const row = document.createElement('div');
    row.className = 'sstv-edit-row';
    row.innerHTML = `<input type="text" class="sstv-text fe-band" maxlength="8" size="6" placeholder="Band" value="${esc(e.band || '')}">
      <input type="number" class="sstv-text fe-mhz" step="0.001" min="0.1" max="450" placeholder="MHz" value="${e.hz ? (e.hz / 1e6).toFixed(3) : ''}">
      <select class="sstv-sel fe-sb"><option value="">AUTO</option><option>USB</option><option>LSB</option></select>
      <button type="button" class="sstv-btn fe-del" title="Remove">&#x2715;</button>`;
    row.querySelector('.fe-sb').value = e.sb || '';
    row.querySelector('.fe-del').onclick = () => row.remove();
    return row;
}
function openFreqEdit() {
    const rows = $('sstvFreqRows'); rows.innerHTML = '';
    for (const e of S.freqs) rows.appendChild(freqRow(e));
    $('sstvFreqMsg').textContent = '';
    $('sstvFreqEdit').classList.remove('hidden');
}
function saveFreqEdit() {
    const list = [];
    for (const row of $('sstvFreqRows').children) {
        const band = row.querySelector('.fe-band').value.trim(), mhz = parseFloat(row.querySelector('.fe-mhz').value);
        if (!band && !(mhz > 0)) continue;
        if (!band || !(mhz >= 0.1 && mhz <= 450)) { $('sstvFreqMsg').textContent = 'Each row needs a band label and a frequency (MHz).'; return; }
        list.push({ band, hz: Math.round(mhz * 1e6), sb: row.querySelector('.fe-sb').value });
    }
    if (!list.length) { $('sstvFreqMsg').textContent = 'Keep at least one frequency (RESET for the defaults).'; return; }
    list.sort((a, b) => a.hz - b.hz);
    saveSettings({ freqs: list.slice(0, 20) });
    $('sstvFreqEdit').classList.add('hidden');
}

// ---------------------------------------------------------------------
// Template editor (punch list 10)
// ---------------------------------------------------------------------
function openTplEdit() {
    S.edit = { list: clone(templates()), id: (currentTemplate() || templates()[0]).id, sel: -1 };
    $('sstvTplMsg').textContent = '';
    $('sstvTplEditBox').classList.remove('hidden');
    renderTplEdit();
}
function editTpl() { return S.edit.list.find(t => t.id === S.edit.id) || S.edit.list[0]; }
function renderTplEdit() {
    const E = S.edit, t = editTpl();
    if (!t) return;
    E.id = t.id;
    const sel = $('sstvTplEditSel');
    sel.innerHTML = E.list.map(x => `<option value="${esc(x.id)}">${esc(x.name)}</option>`).join('');
    sel.value = t.id;
    $('sstvTplName').value = t.name;
    $('sstvTplBgImg').className = 'sstv-chip sstv-chip-btn' + (t.bg === 'image' ? ' sstv-chip-on' : '');
    $('sstvTplBgCol').className = 'sstv-chip sstv-chip-btn' + (t.bg === 'color' ? ' sstv-chip-on' : '');
    $('sstvTplBgColor').value = t.color || '#16222e';
    $('sstvTplDel').disabled = E.list.length <= 1;
    const box = $('sstvTplItems'); box.innerHTML = '';
    t.items.forEach((it, i) => {
        const row = document.createElement('div');
        row.className = 'sstv-edit-row sstv-tpl-row' + (E.sel === i ? ' sel' : '');
        const pct = v => Math.round(v * 100);
        if (it.t === 'inset') {
            row.innerHTML = `<span class="sstv-tpl-kind">PICTURE</span>
              X<input type="number" class="sstv-text ti-x" min="0" max="100" value="${pct(it.x)}">
              Y<input type="number" class="sstv-text ti-y" min="0" max="100" value="${pct(it.y)}">
              W<input type="number" class="sstv-text ti-w" min="5" max="100" value="${pct(it.w)}">
              H<input type="number" class="sstv-text ti-h" min="5" max="100" value="${pct(it.h)}">
              <button type="button" class="sstv-btn ti-del" title="Remove">&#x2715;</button>`;
        } else {
            row.innerHTML = `<input type="text" class="sstv-text ti-text" maxlength="60" value="${esc(it.text)}">
              X<input type="number" class="sstv-text ti-x" min="0" max="100" value="${pct(it.x)}">
              Y<input type="number" class="sstv-text ti-y" min="0" max="100" value="${pct(it.y)}">
              SIZE<input type="number" class="sstv-text ti-size" min="2" max="60" value="${pct(it.size)}">
              <select class="sstv-sel ti-align"><option value="left">LEFT</option><option value="center">CENTRE</option><option value="right">RIGHT</option></select>
              <input type="color" class="ti-color" value="${it.color}" title="Text colour">
              <label class="ti-boxl" title="Box behind the text"><input type="checkbox" class="ti-boxon"${it.box ? ' checked' : ''}>BOX</label>
              <input type="color" class="ti-box" value="${it.box || '#b3261e'}" title="Box colour">
              <button type="button" class="sstv-btn ti-del" title="Remove">&#x2715;</button>`;
            row.querySelector('.ti-align').value = it.align;
        }
        const num = (cls, key, min) => { const el = row.querySelector(cls); if (el) el.oninput = () => { const v = parseFloat(el.value); if (v >= 0) { it[key] = Math.max(min || 0, Math.min(100, v)) / 100; renderTplEditPreview(); } }; };
        num('.ti-x', 'x'); num('.ti-y', 'y'); num('.ti-w', 'w', 5); num('.ti-h', 'h', 5); num('.ti-size', 'size', 2);
        const on = (cls, ev, fn) => { const el = row.querySelector(cls); if (el) el[ev] = () => { fn(el); renderTplEditPreview(); }; };
        on('.ti-text', 'oninput', el => { it.text = el.value; });
        on('.ti-align', 'onchange', el => { it.align = el.value; });
        on('.ti-color', 'oninput', el => { it.color = el.value; });
        on('.ti-boxon', 'onchange', el => { it.box = el.checked ? row.querySelector('.ti-box').value : ''; });
        on('.ti-box', 'oninput', el => { if (row.querySelector('.ti-boxon').checked) it.box = el.value; });
        row.querySelector('.ti-del').onclick = () => { t.items.splice(i, 1); E.sel = -1; renderTplEdit(); };
        row.onfocusin = () => { if (E.sel !== i) { E.sel = i; box.querySelectorAll('.sstv-tpl-row').forEach((r, j) => r.classList.toggle('sel', j === i)); renderTplEditPreview(); } };
        box.appendChild(row);
    });
    renderTplEditPreview();
}
function renderTplEditPreview() {
    const c = $('sstvTplPrev'), m = txMode();
    c.width = m.w; c.height = m.h;
    renderTemplate(c.getContext('2d'), m.w, m.h, editTpl(), fieldValues(), { sel: S.edit.sel });
}
// drag an item on the preview
function tplPointer(e) {
    if (!S.edit) return;
    const c = $('sstvTplPrev'), r = c.getBoundingClientRect();
    const px = (e.clientX - r.left) / r.width, py = (e.clientY - r.top) / r.height;
    const t = editTpl(), g = c.getContext('2d'), vals = fieldValues();
    if (e.type === 'pointerdown') {
        let hit = -1;
        for (let i = t.items.length - 1; i >= 0; i--) {
            const b = itemBox(g, t.items[i], c.width, c.height, vals);
            if (px * c.width >= b.x && px * c.width <= b.x + b.w && py * c.height >= b.y && py * c.height <= b.y + b.h) { hit = i; break; }
        }
        S.edit.sel = hit;
        if (hit >= 0) { S.edit.drag = { px, py, x: t.items[hit].x, y: t.items[hit].y }; c.setPointerCapture(e.pointerId); }
        renderTplEdit();
    } else if (e.type === 'pointermove' && S.edit.drag && S.edit.sel >= 0) {
        const it = t.items[S.edit.sel], d = S.edit.drag;
        it.x = Math.max(0, Math.min(1, d.x + px - d.px));
        it.y = Math.max(0, Math.min(1, d.y + py - d.py));
        const row = $('sstvTplItems').children[S.edit.sel];
        if (row) { row.querySelector('.ti-x').value = Math.round(it.x * 100); row.querySelector('.ti-y').value = Math.round(it.y * 100); }
        renderTplEditPreview();
    } else if (e.type === 'pointerup' || e.type === 'pointercancel') {
        S.edit.drag = null;
    }
}
function saveTplEdit() {
    const list = validTemplates(S.edit.list);
    if (!list) { $('sstvTplMsg').textContent = 'Nothing to save.'; return; }
    const same = JSON.stringify(list) === JSON.stringify(validTemplates(BUILTIN_TEMPLATES));
    saveSettings({ templates: same ? [] : list });
    $('sstvTplSel').value = S.edit.id;
    save('wfSstvTpl', S.edit.id);
    renderFields();
    renderTxPreview();
    S.edit = null;
    $('sstvTplEditBox').classList.add('hidden');
}

// ---------------------------------------------------------------------
// Panel
// ---------------------------------------------------------------------
function renderModeChips() {
    const box = $('sstvModeChips'); box.innerHTML = '';
    const all = [{ id: 'auto', name: 'AUTO' }].concat(SSTV_MODES);
    for (const m of all) {
        const b = document.createElement('button');
        b.type = 'button';
        b.className = 'sstv-chip sstv-chip-btn' + (S.rxMode === m.id ? ' sstv-chip-on' : '');
        b.textContent = m.id === 'auto' ? 'AUTO' : m.name.toUpperCase();
        b.title = m.id === 'auto' ? 'Pick the mode from the VIS code (or the line rate)' : `Receive ${m.name} only`;
        b.onclick = () => { S.rxMode = m.id; save('wfSstvRxMode', m.id); if (S.dec) S.dec.setMode(m.id); renderModeChips(); };
        box.appendChild(b);
    }
}
function nearestBand() {
    const f = host('currentFreq') || 0;
    let best = null, d = 1e12;
    for (const b of S.freqs) { const x = Math.abs(f - b.hz); if (x < d) { d = x; best = b; } }
    return d < 500000 ? best : null;
}
function tick() {
    const f = host('currentFreq'), mode = host('currentMode');
    $('sstvFreq').textContent = f ? `${(f / 1e6).toFixed(3)} ${mode || ''}` : '---';
    if ($('sstvBands').dataset.f !== String(Math.round((f || 0) / 1000))) renderBands();
}
function open() {
    if (S.visible) return;
    S.visible = true;
    $('sstvBar').classList.remove('hidden');
    document.body.classList.add('sstv-open');
    save('sstvOpen', '1');
    if (typeof closeOtherModes === 'function') closeOtherModes('sstv');
    if (typeof window.saveVfoSnapshot === 'function') window.saveVfoSnapshot();
    const b = nearestBand(), f = host('currentFreq') || 0;
    if (b && Math.abs(f - b.hz) > 3000) send_({ cmd: 'setFrequency', value: b.hz });
    if (typeof initDigiTxCtx === 'function') initDigiTxCtx();
    newDecoder();
    S.state = 'idle'; S.snr = null; S.snrSig = null; S.noiseHist = [];
    setState('RX: WAITING', '');
    setInfo('Waiting for a picture');
    if (S.tickTimer) clearInterval(S.tickTimer);
    S.tickTimer = setInterval(tick, 500); tick();
    renderBands();
    renderTxPreview();
    if (typeof aeTick === 'function') { window.aeLastSig = ''; aeTick(); }
    if (typeof resizeCanvases === 'function') setTimeout(resizeCanvases, 0);
}
function close() {
    if (!S.visible) return;
    S.visible = false;
    $('sstvBar').classList.add('hidden');
    closeView();
    $('sstvFreqEdit').classList.add('hidden');
    $('sstvTplEditBox').classList.add('hidden'); S.edit = null;
    document.body.classList.remove('sstv-open');
    save('sstvOpen', '0');
    if (S.txBusy) stopTx();
    if (S.dec) { S.dec.stop(); S.dec = null; }
    if (S.tickTimer) { clearInterval(S.tickTimer); S.tickTimer = null; }
    if (typeof window.restoreVfoSnapshot === 'function') window.restoreVfoSnapshot();
    if (typeof aeTick === 'function') { window.aeLastSig = ''; aeTick(); }
    if (typeof resizeCanvases === 'function') setTimeout(resizeCanvases, 0);
}

(function init() {
    if (typeof document === 'undefined') return;   // node (tests import the helpers)
    const scope = document.getElementById('scopeArea') || document.body;
    if (!$('sstvBar')) scope.insertAdjacentHTML('beforeend', MARKUP);
    const tm = $('sstvTxMode');
    tm.innerHTML = TX_MODES.map(id => { const m = sstvMode(id); return `<option value="${id}">${m.name} (${Math.round(m.seconds)} s)</option>`; }).join('');
    tm.value = load('wfSstvTxMode', 's1');
    tm.onchange = () => { save('wfSstvTxMode', tm.value); renderTxPreview(); if (S.edit) renderTplEditPreview(); };
    S.rxMode = load('wfSstvRxMode', 'auto');
    for (const f of FIELD_INPUTS) if (f.keep) S.fields[f.k] = load('wfSstvFld_' + f.k, '');
    const txt = $('sstvTxText');
    const myCall = typeof getMyCall === 'function' ? getMyCall() : '';
    const myGrid = typeof getMyGrid === 'function' ? getMyGrid() : '';
    txt.value = load('wfSstvTxText', [myCall, myGrid].filter(Boolean).join(' '));
    txt.oninput = () => { save('wfSstvTxText', txt.value); renderTxPreview(); };
    $('sstvTplSel').onchange = () => { save('wfSstvTpl', $('sstvTplSel').value); renderFields(); renderTxPreview(); };
    $('sstvPickBtn').onclick = () => $('sstvFile').click();
    $('sstvFile').onchange = e => {
        const f = e.target.files && e.target.files[0]; if (!f) return;
        const img = new Image();
        img.onload = () => { S.txImg = img; renderTxPreview(); if (S.edit) renderTplEditPreview(); };
        img.src = URL.createObjectURL(f);
    };
    $('sstvTxPrev').onclick = () => $('sstvFile').click();
    $('sstvSendBtn').onclick = startTx;
    $('sstvFskBtn').onclick = () => { save('wfSstvFskId', fskOn() ? '0' : '1'); renderTxPreview(); };
    $('sstvCloseBtn').onclick = close;
    $('sstvViewClose').onclick = closeView;
    $('sstvRxBanner').onclick = () => { delete $('sstvRxBanner').dataset.done; closeView(); updateRxBanner(); };
    $('sstvViewSave').onclick = () => {
        const rec = S.viewing; if (!rec) return;
        const a = document.createElement('a'); a.href = rec.url; a.download = fileStem(rec) + '.png';
        document.body.appendChild(a); a.click(); a.remove();
    };
    $('sstvViewReply').onclick = () => {
        const rec = S.viewing; if (!rec) return;
        S.insetRec = rec;
        if (rec.call) { S.fields.c = rec.call; }
        const qsl = templates().find(t => t.items.some(it => it.t === 'inset'));
        if (qsl) { $('sstvTplSel').value = qsl.id; save('wfSstvTpl', qsl.id); }
        renderFields(); renderGallery(); renderTxPreview();
        closeView();
    };
    $('sstvViewDel').onclick = async () => {
        const rec = S.viewing; if (!rec) return;
        await dbDel(rec.id);
        S.gal = S.gal.filter(r => r !== rec);
        if (S.insetRec === rec) S.insetRec = null;
        if (rec.url) URL.revokeObjectURL(rec.url);
        closeView();
        renderGallery();
        renderTxPreview();
    };
    // band list editor
    $('sstvBandEdit').onclick = openFreqEdit;
    $('sstvFreqAdd').onclick = () => $('sstvFreqRows').appendChild(freqRow({ band: '', hz: 0, sb: '' }));
    $('sstvFreqReset').onclick = () => { saveSettings({ freqs: [] }); $('sstvFreqEdit').classList.add('hidden'); };
    $('sstvFreqCancel').onclick = () => $('sstvFreqEdit').classList.add('hidden');
    $('sstvFreqSave').onclick = saveFreqEdit;
    // template editor
    $('sstvTplEdit').onclick = openTplEdit;
    $('sstvTplEditSel').onchange = () => { S.edit.id = $('sstvTplEditSel').value; S.edit.sel = -1; renderTplEdit(); };
    $('sstvTplName').oninput = () => { editTpl().name = $('sstvTplName').value.slice(0, 24) || 'Template'; $('sstvTplEditSel').selectedOptions[0].textContent = editTpl().name; };
    $('sstvTplNew').onclick = () => {
        const id = 'u' + Date.now().toString(36);
        S.edit.list.push({ id, name: 'New template', bg: 'image', color: '#16222e',
                           items: [{ t: 'text', text: '%m', x: 0.5, y: 0.15, size: 0.12, color: '#ffffff', align: 'center', box: '' }] });
        S.edit.id = id; S.edit.sel = -1; renderTplEdit();
    };
    $('sstvTplDel').onclick = () => {
        if (S.edit.list.length <= 1) return;
        S.edit.list = S.edit.list.filter(t => t.id !== S.edit.id);
        S.edit.id = S.edit.list[0].id; S.edit.sel = -1; renderTplEdit();
    };
    $('sstvTplBgImg').onclick = () => { editTpl().bg = 'image'; renderTplEdit(); };
    $('sstvTplBgCol').onclick = () => { editTpl().bg = 'color'; renderTplEdit(); };
    $('sstvTplBgColor').oninput = () => { editTpl().color = $('sstvTplBgColor').value; editTpl().bg = 'color'; renderTplEdit(); };
    $('sstvTplAddText').onclick = () => { const t = editTpl(); t.items.push({ t: 'text', text: '%c', x: 0.5, y: 0.5, size: 0.1, color: '#ffffff', align: 'center', box: '' }); S.edit.sel = t.items.length - 1; renderTplEdit(); };
    $('sstvTplAddInset').onclick = () => { const t = editTpl(); t.items.push({ t: 'inset', x: 0.05, y: 0.2, w: 0.5, h: 0.6 }); S.edit.sel = t.items.length - 1; renderTplEdit(); };
    $('sstvTplResetAll').onclick = () => { S.edit.list = clone(BUILTIN_TEMPLATES); S.edit.id = S.edit.list[0].id; S.edit.sel = -1; renderTplEdit(); };
    $('sstvTplCancel').onclick = () => { S.edit = null; $('sstvTplEditBox').classList.add('hidden'); };
    $('sstvTplSave').onclick = saveTplEdit;
    const pv = $('sstvTplPrev');
    for (const ev of ['pointerdown', 'pointermove', 'pointerup', 'pointercancel']) pv.addEventListener(ev, tplPointer);
    applySettings(serverBacked() ? window.sstvServerSettings : localSettings());
    renderModeChips();
    galleryLoad();
    window.toggleSstvPanel = () => (S.visible ? close() : open());
    window.sstvPanelVisible = () => S.visible;
    window.sstvTxBusy = () => S.txBusy;
    if (load('sstvOpen', '0') === '1') setTimeout(open, 1500);
})();

export { renderTemplate, expandFields, BUILTIN_TEMPLATES, validTemplates, validFreqs };
