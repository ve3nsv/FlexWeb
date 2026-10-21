// SSTV round trip under node: encode a test card in every mode, pass it
// through a channel (noise, tuning offset, sound-card clock error, missing
// VIS), decode, and compare with the original. Run: node tests/sstv_sim.mjs
// [mode ...]. Exit code 1 on any failure. Wrapped by tests/test_sstv.py.
import { SSTV_MODES, sstvEncode, SstvDecoder, sstvMode } from '../resources/web-shared/sstv.mjs';
import { readFileSync } from 'node:fs';

export function testCard(w, h) {
    const img = new Uint8ClampedArray(w * h * 4);
    const bars = [[255, 255, 255], [255, 255, 0], [0, 255, 255], [0, 255, 0], [255, 0, 255], [255, 0, 0], [0, 0, 255], [0, 0, 0]];
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
        let c;
        if (y < h * 0.45) c = bars[Math.floor(x * 8 / w)];
        else if (y < h * 0.6) { const g = Math.round(255 * x / (w - 1)); c = [g, g, g]; }
        else {
            const cx = x - w / 2, cy = y - h * 0.8, r = Math.hypot(cx, cy * 1.5);
            c = r < h * 0.15 ? [230, 200, 60] : [40, 90, 160 + Math.round(60 * y / h)];
        }
        const i = (y * w + x) * 4;
        img[i] = c[0]; img[i + 1] = c[1]; img[i + 2] = c[2]; img[i + 3] = 255;
    }
    return img;
}

function gauss() { let u = 0, v = 0; while (!u) u = Math.random(); v = Math.random(); return Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * v); }

// channel: shift tuning by `offset` Hz (single-sideband shift via Hilbert
// approximation = re-encode is simpler: we shift by mixing the analytic
// signal), resample by `clock`, add noise at `snr` dB in 3 kHz.
export function channel(x, fs, { offset = 0, clock = 1, snr = 99, lead = 0.5 } = {}) {
    let y = x;
    if (offset) {
        // analytic signal by FIR Hilbert transformer, then mix
        const N = 127, M = 63, h = new Float32Array(N);
        for (let i = 0; i < N; i++) { const k = i - M; h[i] = k % 2 ? 2 / (Math.PI * k) * (0.54 - 0.46 * Math.cos(2 * Math.PI * i / (N - 1))) : 0; }
        const out = new Float32Array(y.length);
        const w = 2 * Math.PI * offset / fs;
        for (let n = 0; n < y.length; n++) {
            let q = 0;
            for (let i = 0; i < N; i++) { const j = n - i; if (j >= 0) q += h[i] * y[j]; }
            const re = n - M >= 0 ? y[n - M] : 0;
            out[n] = re * Math.cos(w * n) - q * Math.sin(w * n);
        }
        y = out;
    }
    if (clock !== 1) {
        const n2 = Math.floor(y.length / clock), out = new Float32Array(n2);
        for (let n = 0; n < n2; n++) { const t = n * clock, i = Math.floor(t), f = t - i; out[n] = (y[i] || 0) * (1 - f) + (y[i + 1] || 0) * f; }
        y = out;
    }
    const pad = Math.round(lead * fs);
    const out = new Float32Array(y.length + 2 * pad);
    out.set(y, pad);
    if (snr < 90) {
        // signal power ~ amp^2/2; noise in 3 kHz of fs/2
        let p = 0; for (let i = 0; i < y.length; i++) p += y[i] * y[i]; p /= y.length;
        const nb = p / Math.pow(10, snr / 10) * (fs / 2) / 3000;
        const s = Math.sqrt(nb);
        for (let i = 0; i < out.length; i++) out[i] += s * gauss();
    }
    return out;
}

export function decodeAll(audio, fs, mode = 'auto', chunk = 1024) {
    const pics = [];
    const dec = new SstvDecoder(fs, { mode, onEvent: ev => { if (ev.type === 'done') pics.push(ev); } });
    for (let i = 0; i < audio.length; i += chunk) dec.push(audio.subarray(i, i + chunk));
    dec.push(new Float32Array(Math.round(fs * 0.5)));
    dec.stop();
    return pics;
}

export function psnr(a, b, w, h, rows) {
    let se = 0, n = 0;
    const r0 = rows ? rows[0] : 0, r1 = rows ? rows[1] : h;
    for (let y = r0; y < r1; y++) for (let x = 2; x < w - 2; x++) for (let c = 0; c < 3; c++) {
        const i = (y * w + x) * 4 + c, d = a[i] - b[i]; se += d * d; n++;
    }
    return 10 * Math.log10(255 * 255 / (se / n));
}

const CASES = [
    { name: 'clean 12k', fs: 12000, ch: {}, min: 24 },
    { name: 'clean 48k', fs: 48000, ch: {}, min: 24 },
    { name: '44.1k, +60 Hz, -0.1 % clock', fs: 44100, ch: { offset: 60, clock: 1.001 }, min: 21 },
    { name: '12k, -80 Hz, +0.2 % clock, 15 dB', fs: 12000, ch: { offset: -80, clock: 0.998, snr: 15 }, min: 16 },
    { name: '12k, 6 dB', fs: 12000, ch: { snr: 6 }, min: 11 },
];

async function main() {
    const only = process.argv.slice(2);
    let fail = 0;
    const modes = SSTV_MODES.filter(m => !only.length || only.includes(m.id));
    for (const m of modes) {
        const card = testCard(m.w, m.h);
        for (const c of CASES) {
            if (m.id === 'pd120' && c.fs > 12000 && c.ch.offset) continue;   // slow under node; covered at 12k
            const audio = channel(sstvEncode(m.id, card, c.fs), c.fs, c.ch);
            const pics = decodeAll(audio, c.fs);
            const p = pics.find(x => x.mode === m.id && !x.late);
            let msg, ok;
            if (!p) { ok = false; msg = 'no picture (' + pics.map(x => x.mode + '/' + x.why).join(',') + ')'; }
            else {
                const q = psnr(card, p.rgba, m.w, m.h);
                ok = q >= c.min && p.line === p.lines;
                msg = `PSNR ${q.toFixed(1)} dB, lines ${p.line}/${p.lines}, sync ${p.syncPct} %, off ${p.offset.toFixed(0)} Hz, slant ${p.slant.toFixed(0)} ppm`;
            }
            if (!ok) fail++;
            console.log(`${ok ? 'ok  ' : 'FAIL'} ${m.name.padEnd(11)} ${c.name.padEnd(36)} ${msg}`);
        }
        // no VIS: join a picture under way (drop the header and 10 lines)
        {
            const fs = 12000, full = sstvEncode(m.id, card, fs);
            const cut = Math.round((0.91 + (m.startSync || 0) / 1000 + 10 * m.lineMs / 1000) * fs);
            const audio = channel(full.subarray(cut), fs, { snr: 20 });
            const pics = decodeAll(audio, fs);
            const p = pics.find(x => x.mode === m.id);
            const ok = !!p && p.late;
            if (!ok) fail++;
            console.log(`${ok ? 'ok  ' : 'FAIL'} ${m.name.padEnd(11)} ${'late join, no VIS'.padEnd(36)} ${p ? `lines ${p.line}/${p.lines}, ${p.why}` : 'no picture (' + pics.map(x => x.mode).join(',') + ')'}`);
        }
    }
    // noise alone must not produce pictures
    {
        const fs = 12000, n = new Float32Array(fs * 60);
        for (let i = 0; i < n.length; i++) n[i] = 0.3 * gauss();
        const pics = decodeAll(n, fs);
        const ok = pics.length === 0;
        if (!ok) fail++;
        console.log(`${ok ? 'ok  ' : 'FAIL'} noise only, 60 s: ${pics.length} pictures`);
    }
    // optional: WAV files from an independent encoder (PySSTV), given as
    // SSTV_WAVS=mode:file.wav:card.rgba,...
    if (process.env.SSTV_WAVS) {
        for (const spec of process.env.SSTV_WAVS.split(',')) {
            const [id, wav, rgbaFile] = spec.split(':');
            const m = sstvMode(id), buf = readFileSync(wav);
            const fs = buf.readUInt32LE(24), data = buf.indexOf('data') + 8;
            const n = (buf.length - data) >> 1, x = new Float32Array(n);
            for (let i = 0; i < n; i++) x[i] = buf.readInt16LE(data + 2 * i) / 32768;
            const pics = decodeAll(channel(x, fs, {}), fs);
            const p = pics.find(q => q.mode === id);
            const card = new Uint8ClampedArray(readFileSync(rgbaFile));
            const q = p ? psnr(card, p.rgba, m.w, m.h) : 0;
            // PySSTV's Scottie scan is 1.5 ms shorter than the standard (Barber's
            // Dayton paper, MMSSTV), so its right edge lands a few pixels off
            const ok = !!p && q > (id[0] === 's' ? 17 : 20);
            if (!ok) fail++;
            console.log(`${ok ? 'ok  ' : 'FAIL'} PySSTV ${m.name.padEnd(11)} ${fs} Hz: ${p ? `PSNR ${q.toFixed(1)} dB, lines ${p.line}/${p.lines}` : 'no picture'}`);
        }
    }
    console.log(fail ? `${fail} FAILED` : 'all passed');
    process.exit(fail ? 1 : 0);
}
if (import.meta.url === 'file://' + process.argv[1]) main();
