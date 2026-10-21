// SSTV (slow-scan television) encoder and decoder, pure JS, both builds.
//
// Modes: Robot 36 / 72, Martin M1 / M2, Scottie S1 / S2 / DX, PD 90 / 120.
// Every mode is described once, as the tone sequence of one scan line
// (lineLayout); the encoder plays that sequence and the decoder reads the
// same offsets back, so the two cannot disagree about timing.
//
// Decoder: audio -> analytic signal around 1900 Hz -> instantaneous
// frequency track (one value per sample at fs / D). A picture starts on
// its VIS header, or, without one, on a run of sync pulses whose spacing
// matches a mode (joining a picture already under way). Each line's sync
// pulse is found near where the timing so far predicts it; a straight-line
// fit of sync time against line number gives the real line period, which
// corrects slant (sound-card clock error) and the start offset. Pixels are
// read from that fit, so the whole picture is redrawn when it improves.
//
// No DOM: runs in a page, a worker or node (tests/sstv_sim.mjs).

export const FREQ_SYNC = 1200, FREQ_BLACK = 1500, FREQ_WHITE = 2300, FREQ_LEADER = 1900;
const FREQ_BIT1 = 1100, FREQ_BIT0 = 1300;

// One scan line as tone items: {f, ms} plain tone (f may be [even, odd]
// for Robot 36's separator), {sync: ms}, {ch, ms} pixels of one channel.
function martin(scan) {
    return () => [{ sync: 4.862 }, { f: 1500, ms: 0.572 }, { ch: 'g', ms: scan }, { f: 1500, ms: 0.572 },
        { ch: 'b', ms: scan }, { f: 1500, ms: 0.572 }, { ch: 'r', ms: scan }, { f: 1500, ms: 0.572 }];
}
function scottie(scan) {
    return () => [{ f: 1500, ms: 1.5 }, { ch: 'g', ms: scan }, { f: 1500, ms: 1.5 }, { ch: 'b', ms: scan },
        { sync: 9 }, { f: 1500, ms: 1.5 }, { ch: 'r', ms: scan }];
}
function pd(px) {
    return (w) => [{ sync: 20 }, { f: 1500, ms: 2.08 }, { ch: 'y0', ms: px * w }, { ch: 'v', ms: px * w },
        { ch: 'u', ms: px * w }, { ch: 'y1', ms: px * w }];
}

export const SSTV_MODES = [
    { id: 'r36', name: 'Robot 36', vis: 0x08, w: 320, h: 240, color: 'r36',
      layout: () => [{ sync: 9 }, { f: 1500, ms: 3 }, { ch: 'y', ms: 88 }, { f: [1500, 2300], ms: 4.5 },
                     { f: 1900, ms: 1.5 }, { ch: 'c', ms: 44 }] },
    { id: 'r72', name: 'Robot 72', vis: 0x0c, w: 320, h: 240, color: 'yuv',
      layout: () => [{ sync: 9 }, { f: 1500, ms: 3 }, { ch: 'y', ms: 138 }, { f: 1500, ms: 4.5 },
                     { f: 1900, ms: 1.5 }, { ch: 'v', ms: 69 }, { f: 2300, ms: 4.5 }, { f: 1900, ms: 1.5 },
                     { ch: 'u', ms: 69 }] },
    { id: 'm1', name: 'Martin M1', vis: 0x2c, w: 320, h: 256, color: 'rgb', layout: martin(146.432) },
    { id: 'm2', name: 'Martin M2', vis: 0x28, w: 160, h: 256, color: 'rgb', layout: martin(73.216) },
    { id: 's1', name: 'Scottie S1', vis: 0x3c, w: 320, h: 256, color: 'rgb', layout: scottie(138.24), startSync: 9 },
    { id: 's2', name: 'Scottie S2', vis: 0x38, w: 160, h: 256, color: 'rgb', layout: scottie(88.064), startSync: 9 },
    { id: 'sdx', name: 'Scottie DX', vis: 0x4c, w: 320, h: 256, color: 'rgb', layout: scottie(345.6), startSync: 9 },
    { id: 'pd90', name: 'PD 90', vis: 0x63, w: 320, h: 256, color: 'pd', layout: pd(0.532) },
    { id: 'pd120', name: 'PD 120', vis: 0x5f, w: 640, h: 496, color: 'pd', layout: pd(0.19) },
];

// Derived timing: lineMs, syncAt / syncMs, channel segments, lines sent.
for (const m of SSTV_MODES) {
    const items = m.layout(m.w);
    let t = 0;
    m.segs = [];
    for (const it of items) {
        const ms = it.sync !== undefined ? it.sync : it.ms;
        if (it.sync !== undefined) { m.syncAt = t; m.syncMs = it.sync; }
        if (it.ch) m.segs.push({ ch: it.ch, at: t, ms });
        t += ms;
    }
    m.items = items;
    m.lineMs = t;
    m.lines = m.color === 'pd' ? m.h / 2 : m.h;
    m.seconds = (m.lines * m.lineMs + 910 + (m.startSync || 0)) / 1000;
}
export function sstvMode(id) { return SSTV_MODES.find(m => m.id === id || m.vis === id) || null; }

// ---------------------------------------------------------------------
// Colour
// ---------------------------------------------------------------------
const clamp8 = v => v < 0 ? 0 : v > 255 ? 255 : v;
function rgbToYuv(r, g, b) {   // JPEG / full-range YCbCr, as PySSTV, MMSSTV, slowrx
    return [0.299 * r + 0.587 * g + 0.114 * b,
            128 - 0.168736 * r - 0.331264 * g + 0.5 * b,
            128 + 0.5 * r - 0.418688 * g - 0.081312 * b];
}
function yuvToRgb(y, u, v) {
    return [clamp8(y + 1.402 * (v - 128)),
            clamp8(y - 0.344136 * (u - 128) - 0.714136 * (v - 128)),
            clamp8(y + 1.772 * (u - 128))];
}

// ---------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------
// rgba: Uint8(Clamped)Array of mode.w x mode.h pixels. Returns Float32Array
// audio at fs, amplitude `amp`, continuous phase throughout.
export function sstvEncode(modeId, rgba, fs = 12000, amp = 0.8) {
    const m = sstvMode(modeId);
    if (!m) throw new Error('SSTV: unknown mode ' + modeId);
    const W = m.w, H = m.h;
    const px = (x, y) => { const i = (Math.min(H - 1, y) * W + x) * 4; return [rgba[i], rgba[i + 1], rgba[i + 2]]; };
    const yuvAt = (x, y) => { const p = px(x, y); return rgbToYuv(p[0], p[1], p[2]); };
    const chan = (line, ch) => {
        const out = new Float32Array(W);
        for (let x = 0; x < W; x++) {
            let v;
            if (ch === 'r' || ch === 'g' || ch === 'b') v = px(x, line)['rgb'.indexOf(ch)];
            else if (m.color === 'pd') {
                const a = yuvAt(x, 2 * line), b = yuvAt(x, 2 * line + 1);
                v = ch === 'y0' ? a[0] : ch === 'y1' ? b[0] : ch === 'u' ? (a[1] + b[1]) / 2 : (a[2] + b[2]) / 2;
            } else {
                const q = yuvAt(x, line);
                v = ch === 'y' ? q[0] : ch === 'u' ? q[1] : ch === 'v' ? q[2] : (line % 2 ? q[1] : q[2]);
            }
            out[x] = v;
        }
        return out;
    };
    const seq = [];   // [freq, ms]
    seq.push([1900, 300], [1200, 10], [1900, 300], [1200, 30]);
    let ones = 0;
    for (let k = 0; k < 7; k++) { const bit = (m.vis >> k) & 1; ones += bit; seq.push([bit ? FREQ_BIT1 : FREQ_BIT0, 30]); }
    seq.push([ones % 2 ? FREQ_BIT1 : FREQ_BIT0, 30], [1200, 30]);
    if (m.startSync) seq.push([1200, m.startSync]);
    for (let line = 0; line < m.lines; line++) {
        for (const it of m.items) {
            if (it.sync !== undefined) seq.push([FREQ_SYNC, it.sync]);
            else if (it.ch) {
                const vals = chan(line, it.ch), d = it.ms / W;
                for (let x = 0; x < W; x++) seq.push([FREQ_BLACK + (FREQ_WHITE - FREQ_BLACK) * vals[x] / 255, d]);
            } else seq.push([Array.isArray(it.f) ? it.f[line % 2] : it.f, it.ms]);
        }
    }
    let total = 0;
    for (const s of seq) total += s[1];
    const out = new Float32Array(Math.ceil(total * fs / 1000) + 1);
    let t = 0, n = 0, phase = 0;
    for (const [f, ms] of seq) {
        t += ms;
        const end = Math.round(t * fs / 1000);
        const dp = 2 * Math.PI * f / fs;
        for (; n < end; n++) { out[n] = amp * Math.sin(phase); phase += dp; }
        if (phase > 1e6) phase %= 2 * Math.PI;
    }
    return out.subarray(0, n);
}
export function sstvSeconds(modeId) { const m = sstvMode(modeId); return m ? m.seconds : 0; }

// ---------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------
function lowpass(taps, fc, fs) {
    const h = new Float32Array(taps), M = (taps - 1) / 2;
    let sum = 0;
    for (let i = 0; i < taps; i++) {
        const x = i - M;
        const s = x === 0 ? 2 * fc / fs : Math.sin(2 * Math.PI * fc / fs * x) / (Math.PI * x);
        const w = 0.42 - 0.5 * Math.cos(2 * Math.PI * i / (taps - 1)) + 0.08 * Math.cos(4 * Math.PI * i / (taps - 1));
        h[i] = s * w; sum += h[i];
    }
    for (let i = 0; i < taps; i++) h[i] /= sum;
    return h;
}

export class SstvDecoder {
    // fs: input rate. opts.mode: 'auto' or a mode id. opts.onEvent(ev).
    constructor(fs, opts = {}) {
        this.D = Math.max(1, Math.round(fs / 12000));
        this.R = fs / this.D;                       // internal rate
        this.modeSel = opts.mode || 'auto';
        this.onEvent = opts.onEvent || (() => {});
        this.h = lowpass(49, 1100, this.R);
        this.reBuf = new Float32Array(this.h.length);
        this.imBuf = new Float32Array(this.h.length);
        this.bufPos = 0;
        this.decAcc = 0; this.decN = 0;
        this.ph = 0; this.dph = 2 * Math.PI * FREQ_LEADER / this.R;
        this.pRe = 1; this.pIm = 0;
        this.F = new Float32Array(this.R * 30);      // frequency track
        this.base = 0;                               // absolute index of F[0]
        this.len = 0;                                // valid values in F
        this.off = 0;                                // tuning offset, Hz
        this.pic = null;
        this.nextVisCheck = 0;
        this.nextChainCheck = 0;
        this.lastPicEnd = -1e12;
    }
    setMode(id) { this.modeSel = id || 'auto'; }
    reset() { this.pic = null; this.len = 0; this.base = 0; }
    stop() { if (this.pic) this._finish('stopped'); }

    push(samples) {
        const D = this.D, h = this.h, L = h.length;
        for (let i = 0; i < samples.length; i++) {
            this.decAcc += samples[i];
            if (++this.decN < D) continue;
            const x = this.decAcc / D;
            this.decAcc = 0; this.decN = 0;
            // mix to baseband around 1900 Hz, low-pass, discriminate
            this.ph -= this.dph;
            if (this.ph < -Math.PI) this.ph += 2 * Math.PI;
            this.reBuf[this.bufPos] = x * Math.cos(this.ph);
            this.imBuf[this.bufPos] = x * Math.sin(this.ph);
            this.bufPos = (this.bufPos + 1) % L;
            let re = 0, im = 0;
            for (let k = 0, j = this.bufPos; k < L; k++, j = (j + 1) % L) { re += h[k] * this.reBuf[j]; im += h[k] * this.imBuf[j]; }
            const dRe = re * this.pRe + im * this.pIm, dIm = im * this.pRe - re * this.pIm;
            this.pRe = re; this.pIm = im;
            let f = FREQ_LEADER + Math.atan2(dIm, dRe) * this.R / (2 * Math.PI);
            if (!(f > 0)) f = FREQ_LEADER;
            this._append(f);
        }
        this._work();
    }

    _append(f) {
        if (this.len >= this.F.length) {
            if (!this.pic) {
                // idle: keep the last 12 s
                const keep = Math.round(this.R * 12), drop = this.len - keep;
                this.F.copyWithin(0, drop, this.len);
                this.base += drop; this.len = keep;
            } else {
                const n = new Float32Array(this.F.length * 2);
                n.set(this.F); this.F = n;
            }
        }
        this.F[this.len++] = f;
    }
    _at(abs) { const i = abs - this.base; return i >= 0 && i < this.len ? this.F[i] : NaN; }
    get end() { return this.base + this.len; }

    // mean of F over [a, b) (absolute, fractional)
    _mean(a, b) {
        const i0 = Math.max(0, a - this.base), i1 = Math.min(this.len, b - this.base);
        if (i1 - i0 < 1) {
            const c = (a + b) / 2 - this.base, j = Math.floor(c), fr = c - j;
            if (j < 0 || j + 1 >= this.len) return NaN;
            return this.F[j] * (1 - fr) + this.F[j + 1] * fr;
        }
        const j0 = Math.ceil(i0), j1 = Math.floor(i1);
        let s = 0, w = 0;
        for (let j = j0; j < j1; j++) { s += this.F[j]; w++; }
        if (j0 > i0 && j0 - 1 >= 0) { const fr = j0 - i0; s += this.F[j0 - 1] * fr; w += fr; }
        if (i1 > j1 && j1 < this.len) { const fr = i1 - j1; s += this.F[j1] * fr; w += fr; }
        return w > 0 ? s / w : NaN;
    }

    _work() {
        if (this.pic) this._decodePic();
        // look for a VIS every 10 ms of audio, however large the pushes are
        const step = Math.round(this.R * 0.01);
        if (this.nextVisCheck < this.end - 20 * step) this.nextVisCheck = this.end - 20 * step;
        while (this.nextVisCheck <= this.end) { this._findVis(this.nextVisCheck); this.nextVisCheck += step; }
        if (!this.pic && this.end >= this.nextChainCheck) {
            this.nextChainCheck = this.end + Math.round(this.R * 0.25);
            this._findChain();
        }
    }

    // ---- VIS header: 1900 leader, 1200 start bit, 7 data bits + even
    // parity at 30 ms (1100 = 1, 1300 = 0), 1200 stop bit.
    _findVis(at) {
        const R = this.R, ms = R / 1000;
        const s = at - Math.round(310 * ms);           // candidate start-bit edge region
        if (s - this.base < 220 * ms) return;
        // leader: 200 ms before s
        const lead = this._mean(s - 200 * ms, s - 20 * ms);
        if (!(lead > 1750 && lead < 2050)) return;
        let ok = 0, n = 0;
        for (let t = s - 200 * ms; t < s - 20 * ms; t += ms) { n++; if (Math.abs(this._mean(t, t + ms) - lead) < 120) ok++; }
        if (ok < 0.8 * n) return;
        const off = lead - FREQ_LEADER;
        // find the leader -> start-bit edge within s - 20 ms .. s + 10 ms
        let edge = -1;
        for (let t = Math.round(s - 20 * ms); t < s + 10 * ms; t++) {
            if (this._mean(t, t + 2 * ms) < 1550 + off && this._mean(t - 3 * ms, t) > 1650 + off) { edge = t; break; }
        }
        if (edge < 0) return;
        if (!(this._mean(edge + 5 * ms, edge + 25 * ms) < 1350 + off)) return;    // start bit 1200
        let code = 0, ones = 0;
        for (let k = 0; k < 8; k++) {
            const a = edge + (30 * (k + 1) + 6) * ms, b = edge + (30 * (k + 2) - 6) * ms;
            const f = this._mean(a, b) - off;
            if (!(f > 1000 && f < 1400)) return;
            const bit = f < 1200 ? 1 : 0;
            ones += bit;
            if (k < 7) code |= bit << k;
        }
        if (ones % 2) return;
        if (!(this._mean(edge + 276 * ms, edge + 294 * ms) - off < 1350)) return;  // stop bit
        const m = sstvMode(code);
        if (!m || (this.modeSel !== 'auto' && this.modeSel !== m.id)) return;
        if (this.pic && edge < this.pic.lastSync) return;
        if (this.pic) this._finish('new picture');
        const start = edge + 300 * ms + (m.startSync || 0) * ms;
        this._startPic(m, start + m.syncAt * ms, off, false, edge);
    }

    // ---- No VIS: a run of 4 sync pulses spaced like a mode's lines.
    _findChain() {
        const R = this.R, ms = R / 1000;
        if (this.end - this.lastPicEnd < 2 * R) return;
        const modes = this.modeSel === 'auto' ? SSTV_MODES.slice().sort((a, b) => a.lineMs - b.lineMs)
                                              : [sstvMode(this.modeSel)];
        for (const m of modes) {
            const T = m.lineMs * ms, L = m.syncMs * ms;
            if (this.len < 4.2 * T) continue;
            const from = this.end - Math.round(4.2 * T);
            // strongest sync in the first line's worth, then 3 more at ~T
            const p0 = this._bestSync(from, from + T, L);
            if (!p0 || p0.score < 0.6) continue;
            let p = p0.pos, okAll = true, pts = [p];
            for (let k = 1; k < 4; k++) {
                const q = this._bestSync(p + T * 0.985, p + T * 1.015 + 1, L);
                if (!q || q.score < 0.6) { okAll = false; break; }
                pts.push(q.pos); p = q.pos;
            }
            if (!okAll) continue;
            // the line between pulses must not hold another sync (rejects a
            // mode whose line is a multiple of the real one)
            const mid = this._bestSync(pts[0] + 0.3 * T, pts[0] + 0.7 * T, L);
            if (mid && mid.score > 0.4) continue;
            // and the rest of the line must look like picture (1500-2300 Hz),
            // not speech, whose energy sits low and scores like sync
            let inside = 0, tot = 0;
            for (let t = Math.round(pts[0] + L + 2 * ms); t < pts[3] - 2 * ms; t += 3) {
                const f = this._at(t); tot++;
                if (f > 1400 && f < 2450) inside++;
            }
            if (inside < 0.7 * tot) continue;
            const off = this._syncOffset(pts, L);
            this._startPic(m, pts[0], off, true, pts[0]);
            return;
        }
    }

    _score(pos, L, off) {
        // +1 for 1200 Hz, 0 at 1500, -1 at 1800 and above
        let s = 0, n = 0;
        const thr = FREQ_BLACK + (off || 0);
        for (let t = Math.round(pos); t < pos + L; t++) {
            const f = this._at(t);
            if (f !== f) return -1;
            let v = (thr - f) / 300;
            s += v > 1 ? 1 : v < -1 ? -1 : v; n++;
        }
        return n ? s / n : -1;
    }
    _bestSync(a, b, L, off) {
        let best = null;
        const step = Math.max(1, Math.round(this.R / 4000));
        for (let p = Math.round(a); p < b; p += step) {
            const sc = this._score(p, L, off);
            if (!best || sc > best.score) best = { pos: p, score: sc };
        }
        if (best && step > 1) {
            for (let p = best.pos - step; p <= best.pos + step; p++) {
                const sc = this._score(p, L, off);
                if (sc > best.score) best = { pos: p, score: sc };
            }
        }
        // the pulse's leading edge: score plateaus, so take the plateau's middle
        if (best) {
            let lo = best.pos, hi = best.pos;
            while (this._score(lo - 1, L, off) >= best.score - 0.02 && lo > best.pos - L / 4) lo--;
            while (this._score(hi + 1, L, off) >= best.score - 0.02 && hi < best.pos + L / 4) hi++;
            best.pos = (lo + hi) / 2;
        }
        return best;
    }
    _syncOffset(pts, L) {
        const v = pts.map(p => this._mean(p + L * 0.2, p + L * 0.8)).filter(f => f === f).sort((a, b) => a - b);
        return v.length ? v[v.length >> 1] - FREQ_SYNC : 0;
    }

    _startPic(m, sync0, off, late, visAt) {
        const ms = this.R / 1000;
        const W = m.w, H = m.h;
        this.off = off;
        this.pic = {
            m, late, T0: m.lineMs * ms, L: m.syncMs * ms, a: sync0, T: m.lineMs * ms,
            syncs: [], lastSync: sync0, done: 0, searched: 0, missRun: 0,
            chans: new Array(m.lines), rgba: new Uint8ClampedArray(W * H * 4), dirtyFrom: 0,
            started: Date.now(), visAt,
        };
        for (let i = 3; i < this.pic.rgba.length; i += 4) this.pic.rgba[i] = 255;
        this.onEvent({ type: 'start', mode: m.id, name: m.name, w: W, h: H, late, offset: off });
    }

    _fit() {
        const P = this.pic, pts = P.syncs;
        if (pts.length < 4) {
            if (pts.length) {
                const r = pts.map(([k, t]) => t - k * P.T0).sort((a, b) => a - b);
                return { a: r[r.length >> 1], T: P.T0 };
            }
            return { a: P.a, T: P.T0 };
        }
        let use = pts;
        let a = P.a, T = P.T;
        for (let it = 0; it < 3; it++) {
            let sk = 0, st = 0, skk = 0, skt = 0, n = use.length;
            for (const [k, t] of use) { sk += k; st += t; skk += k * k; skt += k * t; }
            const den = n * skk - sk * sk;
            if (den <= 0) break;
            T = (n * skt - sk * st) / den;
            a = (st - T * sk) / n;
            T = Math.min(P.T0 * 1.012, Math.max(P.T0 * 0.988, T));
            const tol = Math.max(2, this.R / 1000 * 2.5);
            const keep = pts.filter(([k, t]) => Math.abs(t - (a + k * T)) < tol);
            if (keep.length < 4 || keep.length === use.length) { use = keep.length >= 4 ? keep : use; break; }
            use = keep;
        }
        return { a, T };
    }

    _decodePic() {
        const P = this.pic, m = P.m, R = this.R, ms = R / 1000;
        // search syncs for the lines the audio has reached
        let changed = false;
        while (P.searched < m.lines) {
            const k = P.searched;
            const pred = P.a + k * P.T;
            // wider for the first lines: the start may be off by a start-sync
            // pulse (Scottie) or by the VIS edge estimate
            const win = Math.max(8 * ms, 0.012 * P.T0) + (P.syncs.length < 3 ? 15 * ms : 0);
            if (pred + win + P.L + 2 >= this.end) break;
            const best = this._bestSync(pred - win, pred + win, P.L, this.off);
            if (best && best.score > 0.45) {
                P.syncs.push([k, best.pos]); P.lastSync = best.pos; P.missRun = 0;
                const fsync = this._mean(best.pos + P.L * 0.2, best.pos + P.L * 0.8);
                if (fsync === fsync && best.score > 0.7) this.off += 0.05 * ((fsync - FREQ_SYNC) - this.off);
            } else P.missRun++;
            P.searched++;
            const f = this._fit();
            if (Math.abs(f.a - P.a) > 0.4 || Math.abs((f.T - P.T) * k) > 0.4) {
                P.a = f.a; P.T = f.T; changed = true;
            }
        }
        if (changed) P.done = 0;   // timing moved: redraw everything
        // decode the lines whose audio is complete
        const before = P.done;
        while (P.done < m.lines) {
            const k = P.done;
            const lineStart = P.a + k * P.T - m.syncAt * ms * (P.T / P.T0);
            if (lineStart + P.T + 2 >= this.end) break;
            this._decodeLine(k, lineStart);
            P.done++;
        }
        if (P.done !== before || changed) {
            this.onEvent({ type: 'progress', line: P.done, lines: m.lines, w: m.w, h: m.h, rgba: P.rgba,
                           offset: this.off, syncPct: P.searched ? Math.round(100 * P.syncs.length / P.searched) : 0,
                           slant: (P.T / P.T0 - 1) * 1e6 });
        }
        if (P.done >= m.lines) this._finish('complete');
        else if (P.missRun >= 30 && P.missRun > 0.25 * m.lines) this._finish('signal lost');
    }

    _decodeLine(k, lineStart) {
        const P = this.pic, m = P.m, W = m.w, ms = this.R / 1000, sc = P.T / P.T0;
        const ch = {};
        for (const s of m.segs) {
            const v = new Float32Array(W), d = s.ms / W * ms * sc;
            let t = lineStart + s.at * ms * sc;
            for (let x = 0; x < W; x++, t += d) {
                const f = this._mean(t + d * 0.1, t + d * 0.9) - this.off;
                v[x] = f === f ? clamp8((f - FREQ_BLACK) / (FREQ_WHITE - FREQ_BLACK) * 255) : 0;
            }
            ch[s.ch] = v;
        }
        P.chans[k] = ch;
        this._paint(k);
        if (m.color === 'r36' && k > 0) this._paint(k - 1);   // the pair's other chroma just arrived
    }

    _paint(k) {
        const P = this.pic, m = P.m, W = m.w, img = P.rgba, c = P.chans[k];
        if (!c) return;
        const put = (row, x, rgb) => { const i = (row * W + x) * 4; img[i] = rgb[0]; img[i + 1] = rgb[1]; img[i + 2] = rgb[2]; };
        for (let x = 0; x < W; x++) {
            if (m.color === 'rgb') put(k, x, [c.r[x], c.g[x], c.b[x]]);
            else if (m.color === 'yuv') put(k, x, yuvToRgb(c.y[x], c.u[x], c.v[x]));
            else if (m.color === 'pd') {
                put(2 * k, x, yuvToRgb(c.y0[x], c.u[x], c.v[x]));
                put(2 * k + 1, x, yuvToRgb(c.y1[x], c.u[x], c.v[x]));
            } else {   // Robot 36: even lines carry V, odd lines U
                const mate = P.chans[k % 2 ? k - 1 : k + 1];
                const other = mate ? mate.c[x] : 128;
                const u = k % 2 ? c.c[x] : other, v = k % 2 ? other : c.c[x];
                put(k, x, yuvToRgb(c.y[x], u, v));
            }
        }
    }

    _finish(why) {
        const P = this.pic;
        if (!P) return;
        this.pic = null;
        this.lastPicEnd = this.end;
        this.onEvent({ type: 'done', why, mode: P.m.id, name: P.m.name, w: P.m.w, h: P.m.h, rgba: P.rgba,
                       line: P.done, lines: P.m.lines, late: P.late, offset: this.off,
                       syncPct: P.searched ? Math.round(100 * P.syncs.length / P.searched) : 0,
                       slant: (P.T / P.T0 - 1) * 1e6 });
        // drop the picture's audio, keep a little for the next VIS
        const keep = Math.min(this.len, Math.round(this.R * 2));
        this.F.copyWithin(0, this.len - keep, this.len);
        this.base += this.len - keep; this.len = keep;
        if (this.F.length > this.R * 60) { const n = new Float32Array(this.R * 30); n.set(this.F.subarray(0, this.len)); this.F = n; }
    }
}
