// SSTV panel (both builds). Picture being received, tuning strip, mode,
// signal, gallery of received pictures, and sending a picture.
//
// Host page helpers (same contract as js8-panel.mjs): send, streamDigiAudio,
// haltDigiTx, initDigiTxCtx, closeOtherModes, saveVfoSnapshot /
// restoreVfoSnapshot, currentFreq, currentMode, audioSampleRate, getMyCall,
// getMyGrid. RX audio arrives through window._sstvProcessAudioChunk(int16),
// called by the host's audio path. The clean receive path in index.html
// (modeCleanProfile 'sstv') sets the sideband by band and turns the noise
// tools off while body.sstv-open is set.
import { SSTV_MODES, sstvMode, sstvEncode, SstvDecoder } from './sstv.mjs';

const BANDS = [
    { label: '80 m 3.845 LSB', hz: 3845000 },
    { label: '40 m 7.171 LSB', hz: 7171000 },
    { label: '20 m 14.230 USB', hz: 14230000 },
    { label: '15 m 21.340 USB', hz: 21340000 },
    { label: '10 m 28.680 USB', hz: 28680000 },
];
const TX_MODES = ['s1', 'm1', 'r36', 'pd120', 'r72', 's2', 'm2', 'sdx', 'pd90'];

const MARKUP = `
<div id="sstvBar" class="sstv-bar hidden" aria-label="SSTV">
  <div class="sstv-head">
    <span class="sstv-chip sstv-chip-on">SSTV</span>
    <span class="sstv-chip" id="sstvFreq">---</span>
    <span class="sstv-chip" id="sstvState">RX: WAITING</span>
    <span class="sstv-info" id="sstvInfo"></span>
    <select id="sstvBandSel" class="sstv-sel" title="SSTV calling frequencies"></select>
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
            <button type="button" class="sstv-btn" id="sstvPickBtn">PICK IMAGE</button>
            <input type="file" id="sstvFile" accept="image/*" hidden>
            <input type="text" id="sstvTxText" class="sstv-text" maxlength="40" placeholder="CALL GRID or a message">
            <select id="sstvTxMode" class="sstv-sel"></select>
          </div>
        </div>
        <div class="sstv-send-row2">
          <button type="button" class="sstv-btn sstv-send" id="sstvSendBtn">SEND</button>
          <span class="sstv-info" id="sstvTxInfo"></span>
        </div>
      </div>
    </div>
  </div>
  <div class="sstv-view hidden" id="sstvView">
    <canvas id="sstvViewImg"></canvas>
    <div class="sstv-view-cap" id="sstvViewCap"></div>
    <div class="sstv-view-btns">
      <button type="button" class="sstv-btn" id="sstvViewSave">SAVE</button>
      <button type="button" class="sstv-btn" id="sstvViewDel">DELETE</button>
      <button type="button" class="sstv-btn" id="sstvViewClose">CLOSE</button>
    </div>
  </div>
</div>`;

const S = {
    visible: false, dec: null, rxMode: 'auto', fs: 0, state: 'idle', pic: null,
    txImg: null, txBusy: false, txTimer: null, fft: null, wfRow: 0, snr: null,
    gal: [], viewing: null, picMeta: null,
};
const $ = id => document.getElementById(id);
const host = name => (typeof window[name] !== 'undefined' ? window[name] : undefined);

function load(k, d) { try { const v = localStorage.getItem(k); return v === null ? d : v; } catch (e) { return d; } }
function save(k, v) { try { localStorage.setItem(k, v); } catch (e) {} }

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
async function dbPut(rec) {
    try {
        const db = await dbOpen();
        await new Promise((res, rej) => {
            const tx = db.transaction('pics', 'readwrite'); const q = tx.objectStore('pics').add(rec);
            q.onsuccess = () => { rec.id = q.result; res(); }; q.onerror = () => rej(q.error);
        });
    } catch (e) { rec.id = Date.now(); }
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
        + (rec.freq ? `_${Math.round(rec.freq / 1000)}kHz` : '') + `_${rec.mode.toUpperCase()}`;
}

async function galleryAdd(ev) {
    const c = picCanvas(ev.w, ev.h, ev.rgba);
    const blob = await new Promise(res => c.toBlob(res, 'image/png'));
    const rec = { t: Date.now(), mode: ev.mode, name: ev.name, w: ev.w, h: ev.h, freq: host('currentFreq') || 0,
                  lines: ev.line, of: ev.lines, why: ev.why, png: blob };
    await dbPut(rec);
    S.gal.unshift(rec);
    while (S.gal.length > GAL_MAX) { const old = S.gal.pop(); dbDel(old.id); }
    renderGallery();
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
        im.title = `${rec.name} · ${new Date(rec.t).toLocaleString()}` + (rec.freq ? ` · ${(rec.freq / 1e6).toFixed(3)} MHz` : '');
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
    $('sstvViewCap').textContent = `${rec.name} · ${new Date(rec.t).toLocaleString()}`
        + (rec.freq ? ` · ${(rec.freq / 1e6).toFixed(3)} MHz` : '') + (rec.lines < rec.of ? ` · ${rec.lines}/${rec.of} lines` : '');
    $('sstvView').classList.remove('hidden');
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
        S.state = 'rx'; S.pic = ev; S.picMeta = { t0: Date.now() };
        c.width = ev.w; c.height = ev.h;
        const g = c.getContext('2d'); g.fillStyle = '#000'; g.fillRect(0, 0, ev.w, ev.h);
        setState(`RX: ${ev.name.toUpperCase()}`, 'ok');
        setInfo(ev.late ? 'joined a picture under way (no VIS)' : 'VIS received');
    } else if (ev.type === 'progress') {
        drawPic(ev);
        setInfo(`line ${ev.line} / ${ev.lines} · slant ${fmtPpm(ev.slant)}${S.pic && S.pic.late ? ' · joined late' : ''}`);
        setSignal(ev);
    } else if (ev.type === 'done') {
        drawPic(ev);
        S.state = 'idle';
        setState('RX: WAITING', '');
        setInfo(`${ev.name} ${ev.why} · ${ev.line} / ${ev.lines} lines`);
        if (ev.line >= 0.3 * ev.lines) galleryAdd(ev);
    }
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
    if (S.snr !== null) parts.push(`SNR ${Math.round(S.snr)} dB`);
    parts.push(`offset ${ev.offset >= 0 ? '+' : ''}${Math.round(ev.offset)} Hz`);
    $('sstvSignal').textContent = parts.join(' · ');
    const pct = S.snr === null ? ev.syncPct : Math.max(0, Math.min(100, (S.snr + 5) * 3));
    $('sstvSnrBar').style.width = pct + '%';
}

window._sstvProcessAudioChunk = function (int16) {
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
const WF_LO = 900, WF_HI = 2500;
function wfRow(buf, R) {
    const c = $('sstvWf'); if (!c) return;
    const g = c.getContext('2d'), W = c.width, H = c.height;
    const m = fftMag(buf), hz = R / buf.length;
    // SNR: picture band 1100-2300 Hz against the quiet 2500-2800 Hz above it
    let ps = 0, pn = 0, ns = 0, nn = 0;
    for (let i = 0; i < m.length; i++) {
        const f = i * hz;
        if (f >= 1100 && f <= 2300) { ps += m[i]; ns++; } else if (f >= 2500 && f <= 2800) { pn += m[i]; nn++; }
    }
    if (nn && ns && pn > 0) {
        const nd = pn / nn, sig = Math.max(1e-12, ps - nd * ns), snr = 10 * Math.log10(sig / (nd * 2500 / hz));
        S.snr = S.snr === null ? snr : S.snr + 0.1 * (snr - S.snr);
    }
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
// Send
// ---------------------------------------------------------------------
function txMode() { return sstvMode($('sstvTxMode').value) || sstvMode('s1'); }
function renderTxPreview() {
    const m = txMode(), c = $('sstvTxPrev');
    c.width = m.w; c.height = m.h;
    const g = c.getContext('2d');
    g.fillStyle = '#16222e'; g.fillRect(0, 0, m.w, m.h);
    if (S.txImg) {
        // cover: crop the centre to the mode's aspect
        const iw = S.txImg.naturalWidth || S.txImg.width, ih = S.txImg.naturalHeight || S.txImg.height;
        const k = Math.max(m.w / iw, m.h / ih), w = iw * k, h = ih * k;
        g.drawImage(S.txImg, (m.w - w) / 2, (m.h - h) / 2, w, h);
    } else {
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
    $('sstvTxInfo').textContent = `${m.name} · ${Math.round(m.seconds)} s on air`;
    $('sstvSendBtn').textContent = S.txBusy ? 'STOP' : `SEND · ${Math.round(m.seconds)} s`;
}
function send_(o) { if (typeof send === 'function') send(o); }
function startTx() {
    if (S.txBusy) { stopTx(); return; }
    if (!S.txImg) { $('sstvTxInfo').textContent = 'Pick an image first'; return; }
    if (typeof streamDigiAudio !== 'function') { $('sstvTxInfo').textContent = 'Sending is not available here'; return; }
    renderTxPreview();
    const m = txMode(), c = $('sstvTxPrev');
    const rgba = c.getContext('2d').getImageData(0, 0, m.w, m.h).data;
    const audio = sstvEncode(m.id, rgba, 12000, 0.8);
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
            $('sstvTxInfo').textContent = `Sending ${m.name} · ${Math.round(s)} / ${Math.round(m.seconds)} s`;
        }, 500);
    });
}
function stopTx() {
    if (!S.txBusy) return;
    if (typeof window.haltDigiTx === 'function') window.haltDigiTx();
    else { send_({ cmd: 'setPTT', value: false }); S.txBusy = false; }
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
    for (const b of BANDS) { const x = Math.abs(f - b.hz); if (x < d) { d = x; best = b; } }
    return d < 500000 ? best : null;
}
function tick() {
    const f = host('currentFreq'), mode = host('currentMode');
    $('sstvFreq').textContent = f ? `${(f / 1e6).toFixed(3)} ${mode || ''}` : '---';
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
    if (b) {
        $('sstvBandSel').value = String(b.hz);
        if (Math.abs(f - b.hz) > 3000) send_({ cmd: 'setFrequency', value: b.hz });
    }
    if (typeof initDigiTxCtx === 'function') initDigiTxCtx();
    newDecoder();
    S.state = 'idle'; S.snr = null;
    setState('RX: WAITING', '');
    setInfo('Waiting for a picture');
    if (S.tickTimer) clearInterval(S.tickTimer);
    S.tickTimer = setInterval(tick, 500); tick();
    renderTxPreview();
    if (typeof aeTick === 'function') { window.aeLastSig = ''; aeTick(); }
    if (typeof resizeCanvases === 'function') setTimeout(resizeCanvases, 0);
}
function close() {
    if (!S.visible) return;
    S.visible = false;
    $('sstvBar').classList.add('hidden');
    $('sstvView').classList.add('hidden');
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
    const scope = document.getElementById('scopeArea') || document.body;
    if (!$('sstvBar')) scope.insertAdjacentHTML('beforeend', MARKUP);
    const bs = $('sstvBandSel');
    bs.innerHTML = '<option value="">BAND</option>' + BANDS.map(b => `<option value="${b.hz}">${b.label}</option>`).join('');
    bs.onchange = () => { if (bs.value) send_({ cmd: 'setFrequency', value: +bs.value }); };
    const tm = $('sstvTxMode');
    tm.innerHTML = TX_MODES.map(id => { const m = sstvMode(id); return `<option value="${id}">${m.name} (${Math.round(m.seconds)} s)</option>`; }).join('');
    tm.value = load('wfSstvTxMode', 's1');
    tm.onchange = () => { save('wfSstvTxMode', tm.value); renderTxPreview(); };
    S.rxMode = load('wfSstvRxMode', 'auto');
    const txt = $('sstvTxText');
    const myCall = typeof getMyCall === 'function' ? getMyCall() : '';
    const myGrid = typeof getMyGrid === 'function' ? getMyGrid() : '';
    txt.value = load('wfSstvTxText', [myCall, myGrid].filter(Boolean).join(' '));
    txt.oninput = () => { save('wfSstvTxText', txt.value); renderTxPreview(); };
    $('sstvPickBtn').onclick = () => $('sstvFile').click();
    $('sstvFile').onchange = e => {
        const f = e.target.files && e.target.files[0]; if (!f) return;
        const img = new Image();
        img.onload = () => { S.txImg = img; renderTxPreview(); };
        img.src = URL.createObjectURL(f);
    };
    $('sstvTxPrev').onclick = () => $('sstvFile').click();
    $('sstvSendBtn').onclick = startTx;
    $('sstvCloseBtn').onclick = close;
    $('sstvViewClose').onclick = () => { $('sstvView').classList.add('hidden'); S.viewing = null; };
    $('sstvViewSave').onclick = () => {
        const rec = S.viewing; if (!rec) return;
        const a = document.createElement('a'); a.href = rec.url; a.download = fileStem(rec) + '.png';
        document.body.appendChild(a); a.click(); a.remove();
    };
    $('sstvViewDel').onclick = async () => {
        const rec = S.viewing; if (!rec) return;
        await dbDel(rec.id);
        S.gal = S.gal.filter(r => r !== rec);
        if (rec.url) URL.revokeObjectURL(rec.url);
        $('sstvView').classList.add('hidden'); S.viewing = null;
        renderGallery();
    };
    renderModeChips();
    renderTxPreview();
    galleryLoad();
    window.toggleSstvPanel = () => (S.visible ? close() : open());
    window.sstvPanelVisible = () => S.visible;
    window.sstvTxBusy = () => S.txBusy;
    if (load('sstvOpen', '0') === '1') setTimeout(open, 1500);
})();
