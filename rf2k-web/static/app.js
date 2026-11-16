// rf2k-web page. The server is the amp's only client: it pushes the amp's
// state here over Server-Sent Events, and commands go back as POSTs. Nothing
// on this page latches: every control repaints only from the amp's readback.
'use strict';

var FWD_MAX = 1600, REF_MAX = 200;
var HOLD_MS = 500, FALL = { fwd: 1000, ref: 200 };   // PEAK ballistics, W/s
var $ = function (id) { return document.getElementById(id); };

var st = null;            // the latest amp state
var stAt = 0;             // Date.now() when it arrived
var session = { canControl: false, loginRequired: false, signedIn: false, readOnly: false };
var sseUp = false;
var tuneSent = false;     // our TUNE is on its way: no second START
var resetConfirmUntil = 0;
var meterRaw = false;
try { meterRaw = localStorage.getItem('rf2kMeter') === 'raw'; } catch (e) {}
try { if (localStorage.getItem('rf2kCompact') === '1' || /[?&]compact\b/.test(location.search)) document.body.classList.add('compact'); } catch (e) {}

// ── events ──

var es = null;
function openEvents() {
    if (es) es.close();
    es = new EventSource('api/events');
    es.addEventListener('session', function (e) { session = JSON.parse(e.data); render(); });
    es.addEventListener('state', function (e) {
        st = JSON.parse(e.data); stAt = Date.now(); sseUp = true;
        feedPeak(st);
        render();
    });
    es.onopen = function () { sseUp = true; };
    es.onerror = function () { sseUp = false; render(); };   // EventSource retries by itself
}
openEvents();

// ── helpers ──

function up() { return !!(sseUp && st && st.connected); }
function silentSecs() {
    return st && st.silentMs !== undefined ? Math.floor((st.silentMs + Date.now() - stAt) / 1000) : 0;
}
function isSilent() { return up() && st.silentMs !== undefined; }
function text(el, t) { if (el.textContent !== t) el.textContent = t; }
function cls(el, name, on) { el.classList.toggle(name, !!on); }
function num(v, digits, unit) { return v === undefined || v === null ? '—' : v.toFixed(digits) + unit; }

// SWR on the amp's own scale: 1 / 1.5 / 2 / 3 / 5 / inf evenly spaced.
function swrFrac(swr) {
    var pts = [1, 1.5, 2, 3, 5];
    if (!(swr >= 1)) return 0;
    for (var i = 1; i < pts.length; i++)
        if (swr <= pts[i]) return (i - 1 + (swr - pts[i - 1]) / (pts[i] - pts[i - 1])) / 5;
    return Math.min(1, 0.8 + 0.2 * (1 - 5 / swr));
}
function swrLevel(swr) { return swr >= 2 ? 'bad' : swr >= 1.5 ? 'warn' : ''; }
// Reflected power coloured by the SWR it implies at the present forward power.
function refLevel(fwd, ref) {
    if (!(fwd >= 1) || ref === undefined) return '';
    var rho = Math.sqrt(ref / fwd);
    return swrLevel(rho < 1 ? (1 + rho) / (1 - rho) : 99);
}

// ── FWD / REF ballistics and the peak of the over ──
// A reading arrives every 250 ms in receive (25 ms in transmit), which on
// speech lands between syllables. PEAK: the bar rises at once, holds 500 ms,
// then falls at 1000 W/s (REF 200 W/s) toward the reading. RAW: as it comes.
// The peak of the over (tick + "pk NNN") stays after unkey and restarts at
// the next key-up; "keyed" is forward power >= 1 W (no radio connection).

var bal = { fwd: null, ref: null };     // {v, at, raw}
var peak = { fwd: null, ref: null };    // highest of the over
var overKeyed = false;

function shown(k, raw) {
    var m = bal[k];
    if (meterRaw || !m) return raw;
    var age = performance.now() - m.at - HOLD_MS;
    var v = age > 0 ? m.v - FALL[k] * age / 1000 : m.v;
    return Math.max(v, m.raw);
}

function feedPeak(a) {
    if (!a || !a.connected || a.fwd === undefined) {
        bal.fwd = bal.ref = null;
        if (!a || !a.connected) { peak.fwd = peak.ref = null; overKeyed = false; }
        return;
    }
    var keyed = !!a.keyed;
    if (keyed && !overKeyed) peak.fwd = peak.ref = null;   // a new over
    overKeyed = keyed;
    if (keyed) {
        ['fwd', 'ref'].forEach(function (k) {
            var v = Number(a[k]) || 0;
            if (!peak[k] || v > peak[k]) peak[k] = v;
        });
    }
    if (!(a.fwd >= 1)) { bal.fwd = bal.ref = null; return; }
    var t = performance.now();
    ['fwd', 'ref'].forEach(function (k) {
        var v = Number(a[k]) || 0, m = bal[k];
        if (!m || v >= shown(k, 0)) bal[k] = { v: v, at: t, raw: v };
        else m.raw = v;
    });
    kick();
}

var raf = 0;
function kick() {
    if (raf) return;
    raf = requestAnimationFrame(function step() {
        raf = 0;
        paintBars();
        if (!meterRaw && (bal.fwd || bal.ref)) raf = requestAnimationFrame(step);
    });
}

function paintPowerBar(id, k, max, reading, level, show) {
    var bar = $(id), fill = bar.querySelector('.fill'), tick = bar.querySelector('.peak');
    var v = show && reading !== undefined ? (bal[k] ? shown(k, bal[k].raw) : reading) : undefined;
    fill.style.width = (v === undefined ? 0 : Math.max(0, Math.min(100, v / max * 100))) + '%';
    fill.className = 'fill' + (level ? ' ' + level : '');
    text(bar.querySelector('.num'), v === undefined ? '—' : Math.round(v) + ' W');
    var p = show ? peak[k] : null;
    if (p === null || p === undefined) {
        tick.style.display = 'none';
        text(bar.querySelector('.pk'), '');
    } else {
        tick.style.display = 'block';
        tick.style.left = Math.max(0, Math.min(100, p / max * 100)) + '%';
        text(bar.querySelector('.pk'), 'pk ' + Math.round(p));
    }
}

function paintBars() {
    var a = st || {}, on = up();
    var carrier = on && a.fwd >= 1;
    paintPowerBar('barFwd', 'fwd', FWD_MAX, on ? a.fwd : undefined, '', on);
    // REF and SWR stay blank below 1 W forward (the peak tick still shows).
    paintPowerBar('barRef', 'ref', REF_MAX, carrier ? a.ref : undefined,
                  carrier ? refLevel(a.fwd, a.ref) : '', on);
    if (!carrier) text($('barRef').querySelector('.num'), on && a.fwd !== undefined ? '' : '—');
    var swr = carrier ? a.swr : undefined, sb = $('barSwr'), sf = sb.querySelector('.fill');
    sf.style.width = (swr === undefined ? 0 : swrFrac(swr) * 100) + '%';
    sf.className = 'fill' + (swr !== undefined && swrLevel(swr) ? ' ' + swrLevel(swr) : '');
    text(sb.querySelector('.num'), swr === undefined ? (on && a.fwd !== undefined ? '' : '—') : swr.toFixed(2) + ':1');
}

// ── render ──

function offlineShort(a) {
    switch (a && a.offlineReason) {
    case 'refused': return 'AMP NOT RUNNING';
    case 'noanswer': return 'AMP NOT ANSWERING';
    case 'noroute': return 'AMP UNREACHABLE';
    case 'notrfkit': return 'AMP OFFLINE';
    }
    return 'AMP CONNECTING';
}

function renderChip() {
    var c = $('chip'), a = st;
    c.classList.remove('operate', 'fault', 'stale');
    if (!sseUp) { text(c, 'AMP SERVER OFFLINE'); c.classList.add('fault'); c.title = 'The rf2k-web server is not reachable'; return; }
    if (!a || !a.connected) {
        text(c, offlineShort(a));
        if (a && a.offlineReason) c.classList.add('fault');
        c.title = (a && a.offlineText) || 'Connecting to the amplifier';
        return;
    }
    if (a.waking) { text(c, 'AMP WAKING'); c.title = 'The wake request is on its way (a sleeping amp takes ~5 s)'; return; }
    if (isSilent()) {
        text(c, 'AMP NO ANSWER ' + silentSecs() + ' s'); c.classList.add('stale');
        c.title = 'The amplifier has stopped answering; the readings shown are its last ones';
        return;
    }
    c.title = 'Tap for the compact view';
    if (a.operate === undefined) text(c, 'AMP —');
    else text(c, a.operate ? 'AMP OPR' + (a.fwd !== undefined ? ' ' + a.fwd + ' W' : '') : 'AMP STBY');
    if (a.ampError) c.classList.add('fault');
    else if (a.operate) c.classList.add('operate');
}

function tile(id, value, state) {
    var el = $(id);
    text(el.querySelector('span'), value === undefined || value === '' ? '—' : String(value));
    cls(el, 'bad', state === 'bad');
    cls(el, 'good', state === 'good');
}

function lockReason() {
    if (session.readOnly) return 'View only';
    if (!session.canControl) return session.loginRequired ? 'View only — sign in to control the amp' : 'View only';
    return '';
}

function render() {
    var a = st || {}, on = up(), lock = lockReason();
    renderChip();
    document.title = $('chip').textContent.replace(/^AMP/, 'RF2K-S');

    // sign-in
    $('signBtn').hidden = !session.loginRequired;
    text($('signBtn'), session.signedIn ? 'Sign out' : 'Sign in');
    text($('who'), session.readOnly ? 'read-only' : session.loginRequired ? (session.signedIn ? 'control' : 'view only') : '');

    // header
    text($('devName'), a.device || 'RF-KIT');
    var ls = $('linkState');
    cls(ls, 'stale-text', on && (isSilent() || a.waking));
    text(ls, !sseUp ? 'server offline' : !a.connected ? (a.offlineReason ? 'offline' : 'connecting') :
         a.waking ? 'waking' : isSilent() ? 'no answer ' + silentSecs() + ' s' : 'connected');
    text($('target'), a.target || '');
    var off = $('offline');
    off.hidden = !sseUp || on || !a.offlineReason;
    text(off, a.offlineText ? a.offlineText + (a.error ? ' (' + a.error + ')' : '') : (a.error || ''));
    document.body.classList.toggle('stale', on && isSilent());

    // error banner: the amp's own text
    var err = on ? (a.ampError || '') : '';
    $('errBanner').hidden = !err;
    text($('errText'), err);

    paintBars();

    // tiles
    tile('tBand', on ? a.band : undefined);
    tile('tFreq', on ? a.freq : undefined);
    tile('tTemp', on && a.temp !== undefined ? a.temp.toFixed(0) + ' °C' : undefined, on && a.temp >= 60 ? 'bad' : '');
    tile('tVolts', on ? num(a.volts, 1, ' V') : undefined);
    tile('tAmps', on ? num(a.amps, 1, ' A') : undefined);
    var t = on ? a.tuner : null;
    tile('tAtu', t ? t.pill.replace(/^ATU /, '') : undefined, t ? (t.alert ? 'bad' : t.inUse ? 'good' : '') : '');
    $('tAtu').title = !t ? 'Tuner state unknown'
        : t.alert ? 'Not tuned: the tuner is in line with no stored tuning here, and the amplifier blocks TX until it is tuned.'
        : 'Tuner ' + t.mode + ', setup ' + t.setup + (t.L ? ', L ' + t.L : '') + (t.C ? ', C ' + t.C : '') +
          (t.freq ? ', tuned at ' + t.freq : '') + (t.segment ? ', segment ' + t.segment : '');
    tile('tIface', on ? a.iface : undefined);

    // OPERATE / STANDBY: unknown until /operate-mode has answered
    var op = $('opBtn'), opKnown = on && a.operate !== undefined;
    op.className = 'ctl' + (opKnown ? (a.operate ? ' op-on' : ' op-standby') : '');
    text(op, opKnown ? (a.operate ? 'OPERATE' : 'STANDBY') : '—');
    op.disabled = !opKnown || !!lock;
    op.title = lock || (!on ? 'Amplifier offline' : !opKnown ? 'Amplifier state unknown'
             : a.operate ? 'In OPERATE: tap for STANDBY' : 'In STANDBY: tap for OPERATE');

    // TUNE: only on a server with PUT /tuner; enabled only with the tuner
    // idle in AUTO or MANUAL; TUNING from the amp's own readback.
    var tb = $('tuneBtn'), showTune = on && a.tuneApi === 'supported';
    tb.hidden = !showTune;
    var tuning = !!(t && t.mode === 'TUNING');
    var ready = showTune && !lock && t && t.allowed && !tuning && !a.tunePending && !tuneSent;
    var drive = a.tuneDrive === 'good' || a.tuneDrive === 'low';
    tb.className = 'ctl tune' + (tuning ? ' tuning' : ready ? ' ready' : '') + (drive && !tuning ? ' drive-ok' : '');
    text(tb, tuning ? 'TUNING' : 'TUNE');
    tb.disabled = !ready;
    tb.title = lock || (tuning ? 'The amplifier is tuning'
        : !t || !t.allowed ? 'TUNE needs the tuner in AUTO or MANUAL (not OFF or BYPASS)'
        : a.tuneDrive === 'good' ? 'Start the amplifier\'s autotune'
        : a.tuneDrive === 'low' ? 'Start the autotune. Drive is in range; 10 W or more gives a more precise tune'
        : 'Key a carrier of 4-39 W (10 W or more recommended), then TUNE. This page never keys the radio.');

    // RESET: red and enabled only while there is an error; a second tap within 3 s
    var rs = $('resetBtn'), confirming = err && resetConfirmUntil > Date.now();
    rs.className = 'ctl' + (err ? ' reset-fault' : '');
    text(rs, confirming ? 'CONFIRM?' : 'RESET');
    rs.disabled = !err || !!lock;
    rs.title = lock || (err ? 'Reset the amplifier\'s error (' + err + '). It comes back in STANDBY.' : 'No error to reset');

    // antennas
    var ants = (on && a.antennas) || [];
    document.querySelectorAll('.ant').forEach(function (b) {
        var key = b.dataset.ant, ext = key === 'ext', info = null;
        ants.forEach(function (x) { if (ext ? x.external : (!x.external && x.num === Number(key))) info = x; });
        var active = !!(info && info.state === 'active');
        cls(b, 'active', active);
        if (ext) {
            text(b, info && info.port ? 'EXT ' + info.port : 'EXT');
            b.disabled = true;
        } else {
            b.disabled = !on || !info || info.state === 'disabled' || active || !!lock;
            b.title = lock || ('ANT ' + key + (info ? ' (' + info.state + ')' : ' (not listed by the amplifier)'));
        }
    });

    var note = lock;
    if (!note && showTune && t && t.allowed && !tuning && !(a.fwd >= 1))
        note = 'TUNE: key a carrier of 4–39 W yourself (10 W or more recommended), then press TUNE.';
    text($('note'), note);
    text($('meterMode'), 'METER: ' + (meterRaw ? 'RAW' : 'PEAK'));
    document.body.classList.toggle('raw', meterRaw);
    text($('fw'), on && a.firmware ? a.model + ' · GUI ' + a.firmware.gui + ' · controller ' + a.firmware.controller : '');
}

// the NO ANSWER counter ticks without a message
setInterval(function () { if (isSilent() || resetConfirmUntil) render(); }, 500);

// ── commands ──

var toastTimer = 0;
function toast(msg) {
    var t = $('toast');
    text(t, msg);
    t.hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { t.hidden = true; }, 6000);
}

function post(path, body) {
    return fetch(path, { method: 'POST', headers: { 'Content-Type': 'application/json' },
                         body: JSON.stringify(body || {}), credentials: 'same-origin' })
        .then(function (r) {
            return r.json().catch(function () { return {}; }).then(function (j) { j.httpStatus = r.status; return j; });
        });
}

function command(cmd, value) {
    return post('api/command', { cmd: cmd, value: value }).then(function (r) {
        if (r.httpStatus !== 200) toast(r.error || ('Refused (HTTP ' + r.httpStatus + ')'));
        else if (!r.ok) toast((r.what || cmd.toUpperCase()) + ' refused by the amplifier: ' + (r.reason || 'no reason given'));
        return r;
    }).catch(function () { toast('The rf2k-web server did not answer'); });
}

$('opBtn').onclick = function () {
    if (!st || st.operate === undefined) return;
    command('operate', !st.operate);
};
$('tuneBtn').onclick = function () {
    if (this.disabled || tuneSent) return;
    tuneSent = true;            // guard against a double click sending START twice
    render();
    command('tune').then(function () { tuneSent = false; render(); });
};
$('resetBtn').onclick = function () {
    if (resetConfirmUntil > Date.now()) {
        resetConfirmUntil = 0;
        command('reset');
    } else {
        resetConfirmUntil = Date.now() + 3000;
        setTimeout(function () { if (resetConfirmUntil && resetConfirmUntil <= Date.now()) { resetConfirmUntil = 0; render(); } }, 3100);
    }
    render();
};
document.querySelectorAll('.ant:not(.ext)').forEach(function (b) {
    b.onclick = function () { command('antenna', Number(b.dataset.ant)); };
});
$('meterMode').onclick = function () {
    meterRaw = !meterRaw;
    try { localStorage.setItem('rf2kMeter', meterRaw ? 'raw' : 'peak'); } catch (e) {}
    bal.fwd = bal.ref = null;
    render();
};
$('chip').onclick = function () {
    var on = document.body.classList.toggle('compact');
    try { localStorage.setItem('rf2kCompact', on ? '1' : '0'); } catch (e) {}
};

// ── sign-in ──

$('signBtn').onclick = function () {
    if (session.signedIn) {
        post('api/logout').then(openEvents);
    } else {
        $('signForm').hidden = false;
        $('signPw').focus();
    }
};
$('signCancel').onclick = function () { $('signForm').hidden = true; };
$('signForm').onsubmit = function (e) {
    e.preventDefault();
    post('api/login', { password: $('signPw').value }).then(function (r) {
        if (r.httpStatus === 200) {
            $('signPw').value = '';
            $('signForm').hidden = true;
            openEvents();          // the event stream carries who we are
        } else {
            toast(r.error || 'Sign-in failed');
        }
    });
};

render();
