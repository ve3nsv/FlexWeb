// Punch list 94: while a LAN radio is being opened the page says
// "Connecting to <radio>..." with no Power On button, instead of "Radio is
// powered off" - which had Power On pressed on every page load. The page's
// helpers are taken from resources/web/index.html and run against a fake DOM.
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

const html = readFileSync(new URL('../resources/web/index.html', import.meta.url), 'utf8');
const grab = (from, to) => {
    const a = html.indexOf(from), b = html.indexOf(to, a);
    if (a < 0 || b < 0) throw new Error('not found: ' + from);
    return html.slice(a, b);
};
let fails = 0;
const check = (ok, what) => { if (!ok) { console.log('FAIL ' + what); fails++; } };

const els = {};
const el = (id) => els[id] || (els[id] = {
    id, textContent: '', className: '', title: '', style: { display: '' },
    classList: { set: new Set(), add(c) { this.set.add(c); }, remove(c) { this.set.delete(c); },
                 contains(c) { return this.set.has(c); } },
});
const timers = [];
const sb = {
    document: { getElementById: el },
    radioList: [{ name: 'IC-7610' }], radioIndex: 0, rigModelName: '',
    isLan: false, lanConnected: false, rigPoweringOn: false, rigPoweredOn: true,
    audioEnabled: false, stopAudio() {},
    setTimeout(fn, ms) { timers.push({ fn, ms }); return timers.length; },
    clearTimeout(id) { if (id) timers[id - 1] = null; },
};
vm.createContext(sb);
vm.runInContext(grab('var rigConnecting = false;', 'function toggleLanConnection()'), sb);
const run = (js) => vm.runInContext(js, sb);
const visible = () => el('powerOffOverlay').classList.contains('visible');

// The rigInfo of a page that opened while the server is still opening the link.
run('updateLanStatus(true, true); updatePowerState(false); if (isLan && lanConnected) showRigConnecting();');
check(visible(), 'overlay shown while connecting');
check(el('powerOffMsg').textContent === 'Connecting to IC-7610…', 'message: ' + el('powerOffMsg').textContent);
check(el('powerOnBtn').style.display === 'none', 'no Power On while connecting');
check(el('powerOnSpinner').style.display === 'block', 'spinner while connecting');

// The radio answers: the status after rigConnected carries powerState.
run('updatePowerState(true)');
check(!visible(), 'overlay gone once the radio answers');
check(timers.filter(Boolean).length === 0, 'the 30 s wait cancelled');

// It never answers: after 30 s Power On is offered with the reason.
run('updatePowerState(false); showRigConnecting();');
const t = timers.filter(Boolean).pop();
check(t && t.ms === 30000, 'a 30 s wait');
t.fn();
check(el('powerOnBtn').style.display === '', 'Power On offered after the wait');
check(/did not answer in 30 s/.test(el('powerOffMsg').textContent), 'reason: ' + el('powerOffMsg').textContent);

// The link closes while connecting: the Connect LAN button at once.
run('updatePowerState(false); showRigConnecting(); updateLanStatus(true, false);');
check(el('lanConnBtn').textContent === 'Connect LAN' && el('lanConnBtn').style.display === '', 'Connect LAN at once');
check(el('powerOnSpinner').style.display === 'none', 'spinner stopped when the link closed');

// A radio that answers as off still gets "Radio is powered off" + Power On.
run('updateLanStatus(true, true); updatePowerState(false);');
check(el('powerOffMsg').textContent === 'Radio is powered off' && el('powerOnBtn').style.display === '',
      'off radio: ' + el('powerOffMsg').textContent);

if (fails) { console.log(fails + ' failure(s)'); process.exit(1); }
console.log('power overlay: all checks passed');
