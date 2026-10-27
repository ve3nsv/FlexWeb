// The scope / S-meter / audio lag estimator (resources/web/sync-estimator.js)
// under node: a speech-like loudness envelope arrives as audio chunks; the
// same envelope (with noise, quantisation and its own rate) arrives as scope
// frames and S-meter readings some ms later. The estimator must find the
// lags, and must stay silent on noise alone. Run: node tests/sync_estimator_sim.mjs
import { createRequire } from 'node:module';
const require = createRequire(import.meta.url);
const { estimateLag, SyncEstimator } = require('../resources/web/sync-estimator.js');

let seed = 7;
const rnd = () => { seed = (seed * 1664525 + 1013904223) >>> 0; return seed / 4294967296; };
const gauss = () => { let u = 0; while (!u) u = rnd(); return Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * rnd()); };

// syllables: 80-250 ms bursts, gaps, slow fading - in dB
function envelope(ms) {
    const e = new Float64Array(ms);
    let t = 0;
    while (t < ms) {
        const on = 80 + rnd() * 170, off = 40 + rnd() * 250, lvl = 15 + rnd() * 20;
        for (let i = 0; i < on && t < ms; i++, t++) e[t] = lvl;
        for (let i = 0; i < off && t < ms; i++, t++) e[t] = 0;
    }
    for (let i = 0; i < ms; i++) e[i] += 6 * Math.sin(2 * Math.PI * i / 7000);
    return e;
}
const at = (e, t) => e[Math.max(0, Math.min(e.length - 1, Math.round(t)))];
// mean over the last `span` ms (a meter's / FFT's integration)
function avg(e, t, span) { let s = 0, n = 0; for (let k = t - span; k <= t; k++) { s += at(e, k); n++; } return s / n; }

let fail = 0;
function check(name, ok, msg) { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'} ${name.padEnd(46)} ${msg}`); }

const MS = 20000;
for (const [scopeLag, meterLag] of [[170, 330], [0, 120], [-60, 200], [250, 250]]) {
    const e = envelope(MS + 2000);
    const audio = [], scope = [], meter = [];
    for (let t = 1000; t < MS; t += 20) audio.push([t + gauss() * 3, at(e, t) + gauss() * 1.5]);                 // 20 ms chunks, jittered arrival
    for (let t = 1000; t < MS; t += 40) scope.push([t + scopeLag + gauss() * 4, Math.round((avg(e, t, 40) + gauss() * 2) * 255 / 45)]); // FFT frames, counts
    for (let t = 1000; t < MS; t += 100) meter.push([t + meterLag + gauss() * 5, avg(e, t, 100) - 60 + gauss()]);  // ~10 Hz LEVEL meter
    const s = estimateLag(audio, scope, MS), m = estimateLag(audio, meter, MS);
    // the integration windows shift the centre of mass by half their length
    check(`scope lag ${scopeLag} ms`, s && Math.abs(s.lagMs - (scopeLag + 20)) <= 25, s ? `found ${s.lagMs.toFixed(0)} ms, r ${s.r.toFixed(2)}` : 'none');
    check(`meter lag ${meterLag} ms`, m && Math.abs(m.lagMs - (meterLag + 50)) <= 40, m ? `found ${m.lagMs.toFixed(0)} ms, r ${m.r.toFixed(2)}` : 'none');
}

// running estimator: converges and keeps the last good value through silence
{
    const est = new SyncEstimator(), e = envelope(60000);
    let t = 0;
    for (; t < 40000; t += 20) {
        est.add('audio', t, at(e, t) + gauss());
        if (t % 40 === 0) est.add('scope', t + 150, avg(e, t, 40) + gauss());
        if (t % 100 === 0) est.add('meter', t + 300, avg(e, t, 100) + gauss());
        est.run(t);
    }
    const s1 = est.scopeLag, m1 = est.meterLag;
    for (; t < 60000; t += 20) {                     // band goes quiet: noise only
        est.add('audio', t, gauss());
        if (t % 40 === 0) est.add('scope', t + 150, gauss());
        if (t % 100 === 0) est.add('meter', t + 300, gauss());
        est.run(t);
    }
    check('running estimator converges', Math.abs(s1 - 170) < 30 && Math.abs(m1 - 350) < 50, `scope ${s1 && s1.toFixed(0)}, meter ${m1 && m1.toFixed(0)}`);
    check('keeps the last good value on noise', Math.abs(est.scopeLag - s1) < 15 && Math.abs(est.meterLag - m1) < 25,
          `scope ${est.scopeLag.toFixed(0)}, meter ${est.meterLag.toFixed(0)}`);
}

// noise alone: no estimate
{
    let found = 0;
    for (let k = 0; k < 20; k++) {
        const audio = [], scope = [];
        for (let t = 0; t < MS; t += 20) audio.push([t, gauss()]);
        for (let t = 0; t < MS; t += 40) scope.push([t, gauss()]);
        if (estimateLag(audio, scope, MS)) found++;
    }
    check('noise alone gives no estimate', found === 0, `${found} / 20`);
}

console.log(fail ? `${fail} FAILED` : 'all passed');
process.exit(fail ? 1 : 0);
