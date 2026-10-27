// Automatic scope / S-meter / audio sync (punch list 7).
//
// The page records, at arrival, three time series: the received audio's
// loudness (dB per audio chunk), the scope's level inside the receive
// passband (per scope frame) and the S-meter readings. A voice or a fading
// carrier moves all three together; cross-correlating each against the
// audio says how much later its content arrives. The page then holds the
// scope and the meter back by the audio's playout delay minus that lag, so
// they move with what is heard.
//
// estimateLag() is pure (no DOM): tests/sync_estimator_sim.mjs drives it
// under node.
(function (global) {
'use strict';

var GRID_MS = 20;          // resampling step
var WINDOW_MS = 15000;     // analysis window
var SMOOTH_MS = 1000;      // high-pass: remove the slow level, keep syllables / fades
var MAX_LAG_MS = 600;

// [[t, v], ...] (t ascending, ms) -> values on the grid t0, t0 + GRID_MS, ...
// Cells between samples are interpolated linearly: holding each value until
// the next would add half the sample interval to the lag, which is not
// content delay (the page cannot remove it by holding the data back less).
function resample(series, t0, n) {
    var out = new Float64Array(n), sum = new Float64Array(n), cnt = new Uint16Array(n);
    for (var i = 0; i < series.length; i++) {
        var k = Math.round((series[i][0] - t0) / GRID_MS);
        if (k < 0 || k >= n) continue;
        sum[k] += series[i][1]; cnt[k]++;
    }
    var filled = 0, prev = -1;
    for (var j = 0; j < n; j++) {
        if (!cnt[j]) continue;
        out[j] = sum[j] / cnt[j]; filled++;
        if (prev < 0) { for (var q = 0; q < j; q++) out[q] = out[j]; }
        else for (q = prev + 1; q < j; q++) out[q] = out[prev] + (out[j] - out[prev]) * (q - prev) / (j - prev);
        prev = j;
    }
    if (prev < 0) out.fill(NaN);
    else for (q = prev + 1; q < n; q++) out[q] = out[prev];
    return { v: out, filled: filled };
}

function highpass(v) {
    var n = v.length, w = Math.max(3, Math.round(SMOOTH_MS / GRID_MS)), half = w >> 1;
    var out = new Float64Array(n), pre = new Float64Array(n + 1);
    for (var i = 0; i < n; i++) pre[i + 1] = pre[i] + v[i];
    for (i = 0; i < n; i++) {
        var a = Math.max(0, i - half), b = Math.min(n, i + half + 1);
        out[i] = v[i] - (pre[b] - pre[a]) / (b - a);
    }
    return out;
}

// How much later sig's content arrives than ref's (ms; negative = earlier),
// or null without a clear answer. opts.minR: least correlation accepted.
function estimateLag(ref, sig, tEnd, opts) {
    opts = opts || {};
    var n = Math.round(WINDOW_MS / GRID_MS), t0 = tEnd - WINDOW_MS;
    var A = resample(ref, t0, n), B = resample(sig, t0, n);
    // both series need enough real samples (a stalled meter is not evidence)
    if (A.filled < n * 0.5 || B.filled < Math.min(n * 0.2, opts.minSigSamples || 40)) return null;
    if (!(A.v[0] === A.v[0]) || !(B.v[0] === B.v[0])) return null;
    var a = highpass(A.v), b = highpass(B.v);
    var maxL = Math.round(MAX_LAG_MS / GRID_MS), r = [], best = -2, bestL = 0;
    for (var L = -maxL; L <= maxL; L++) {
        var s = 0, sa = 0, sb = 0;
        for (var i = maxL; i < n - maxL; i++) {
            var x = a[i], y = b[i + L];
            s += x * y; sa += x * x; sb += y * y;
        }
        var c = (sa > 0 && sb > 0) ? s / Math.sqrt(sa * sb) : 0;
        r.push(c);
        if (c > best) { best = c; bestL = L; }
    }
    if (best < (opts.minR || 0.45)) return null;
    if (bestL === -maxL || bestL === maxL) return null;          // on the edge: not a peak
    // a clear peak: nothing comparable more than 100 ms away
    var side = -2, idx = bestL + maxL, sep = Math.round(100 / GRID_MS);
    for (var k = 0; k < r.length; k++) if (Math.abs(k - idx) > sep && r[k] > side) side = r[k];
    if (side > 0.85 * best) return null;
    // sub-step: parabola through the peak and its neighbours
    var y0 = r[idx - 1], y1 = r[idx], y2 = r[idx + 1], den = y0 - 2 * y1 + y2;
    var frac = den < 0 ? 0.5 * (y0 - y2) / den : 0;
    return { lagMs: (bestL + Math.max(-0.5, Math.min(0.5, frac))) * GRID_MS, r: best };
}

// The page's running estimator: feed it samples; it re-estimates every few
// seconds and keeps the last good values.
function SyncEstimator() {
    this.audio = []; this.scope = []; this.meter = [];
    this.scopeLag = null; this.meterLag = null;
    this.scopeR = 0; this.meterR = 0;
    this.nextRun = 0;
}
SyncEstimator.prototype.trim = function (t) {
    var cut = t - WINDOW_MS - 2000;
    ['audio', 'scope', 'meter'].forEach(function (k) {
        var s = this[k], i = 0;
        while (i < s.length && s[i][0] < cut) i++;
        if (i) s.splice(0, i);
    }, this);
};
SyncEstimator.prototype.add = function (kind, t, v) {
    if (!(v === v) || !isFinite(v)) return;
    this[kind].push([t, v]);
};
SyncEstimator.prototype.reset = function () { this.audio = []; this.scope = []; this.meter = []; };
// Returns true when an estimate changed.
SyncEstimator.prototype.run = function (t) {
    if (t < this.nextRun) return false;
    this.nextRun = t + 3000;
    this.trim(t);
    var changed = false, self = this;
    function upd(key, rkey, est) {
        if (!est) return;
        var prev = self[key];
        self[key] = prev === null ? est.lagMs : prev + 0.35 * (est.lagMs - prev);
        self[rkey] = est.r;
        changed = true;
    }
    upd('scopeLag', 'scopeR', estimateLag(this.audio, this.scope, t));
    upd('meterLag', 'meterR', estimateLag(this.audio, this.meter, t, { minSigSamples: 40 }));
    return changed;
};

var api = { estimateLag: estimateLag, SyncEstimator: SyncEstimator, GRID_MS: GRID_MS, WINDOW_MS: WINDOW_MS };
if (typeof module !== 'undefined' && module.exports) module.exports = api;
global.WfSync = api;
})(typeof window !== 'undefined' ? window : globalThis);
