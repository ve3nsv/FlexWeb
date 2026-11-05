// Punch list 68: the local monitor of digital TX audio follows VOL, MUTE and
// AF, before and during the over. The page's helpers are taken from
// resources/web/index.html and run against a fake AudioContext.
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

const src = grab('function rxVolumeGain()', 'function setRxMuted(') +
            grab('var digiMonGains = [];', '\nfunction setRxMuted(');
const slider = { value: '128' };
const ctx = {
    currentTime: 0,
    createGain() {
        const g = { context: ctx, gain: { value: 1, setTargetAtTime(v) { this.value = v; } } };
        return g;
    },
};
const sb = { rxVolume: 100, rxMuted: false, document: { getElementById: (id) => id === 'afGainSlider' ? slider : null } };
vm.createContext(sb);
vm.runInContext(src, sb);
const near = (a, b) => Math.abs(a - b) < 1e-6;

const g = sb.digiMonGainNode(ctx);
check(near(g.gain.value, 0.05 * 128 / 255), 'AF 50 %, VOL 100 %: the old level ' + g.gain.value);
sb.rxVolume = 50; sb.digiMonUpdate();
check(near(g.gain.value, 0.05 * 128 / 255 * 0.25), 'VOL 50 % during the over ' + g.gain.value);
sb.rxMuted = true; sb.digiMonUpdate();
check(g.gain.value === 0, 'MUTE silences it ' + g.gain.value);
sb.rxMuted = false; sb.rxVolume = 100; slider.value = '0'; sb.digiMonUpdate();
check(g.gain.value === 0, 'AF 0 silences it ' + g.gain.value);
slider.value = '255'; sb.digiMonUpdate();
check(near(g.gain.value, 0.05), 'AF 100 % ' + g.gain.value);
sb.rxVolume = 0;
const g2 = sb.digiMonGainNode(ctx);
check(g2.gain.value === 0, 'a new over at VOL 0 starts silent ' + g2.gain.value);
sb.digiMonRelease(g); sb.digiMonRelease(g2);
check(sb.digiMonGains.length === 0, 'released nodes are dropped');

// The page wires it: no fixed-level monitor left, and VOL / MUTE / AF call it.
check(!/monGain\.gain\.value\s*=\s*0\.025/.test(html), 'a monitor still has the fixed 0.025 level');
check(/function setRxVolume[\s\S]{0,400}digiMonUpdate\(\)/.test(html), 'setRxVolume does not update the monitor');
check(/if \(name === 'afGain'\) digiMonUpdate\(\);/.test(html), 'an AF reading does not update the monitor');
check((html.match(/digiMonGainNode\(txCtx\)/g) || []).length === 2, 'FT8 stream and TUNE monitors both use digiMonGainNode');

console.log(fails ? fails + ' FAILED' : 'all passed');
process.exit(fails ? 1 : 0);
