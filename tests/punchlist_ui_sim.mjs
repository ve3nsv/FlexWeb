// Execute the shipped meter animation and video-mixer delay with deterministic
// clocks, checking motion and actual emitted audio rather than source strings.
import vm from 'node:vm';
import {readFileSync} from 'node:fs';
import assert from 'node:assert/strict';
const html = readFileSync(new URL('../resources/web/index.html', import.meta.url), 'utf8');
let pending;
const context = vm.createContext({Math, Number, document: {hidden: false}, isTransmitting: false,
    meterVal: -54, drawMeter() {}, requestAnimationFrame(fn) { pending = fn; return 1; }});
vm.runInContext(html.slice(html.indexOf('var meterTarget ='), html.indexOf('// Every S-meter reading')), context);
vm.runInContext('updateSMeter(0)', context);
function frame(t) { const fn = pending; pending = null; fn(t); }
frame(1000);
for (let t=1016;t<=1112;t+=16) frame(t);
assert(context.meterVal > -6 && context.meterVal <= 0, '45 ms attack should approach S9 promptly');
vm.runInContext('updateSMeter(-54)', context);
const high = context.meterVal;
frame(1128); frame(1144); frame(1160);
assert(context.meterVal < high && context.meterVal > -25, 'release should fall more slowly than attack');
assert(context.meterPeakDb > context.meterVal, 'peak should fall independently');
const before = context.meterTarget;
vm.runInContext('updateSMeter(NaN)', context);
assert.equal(context.meterTarget, before);
const recorder = readFileSync(new URL('../resources/web-shared/recorder.js', import.meta.url), 'utf8');
let Mixer;
const worklet = vm.createContext({Float32Array, Int16Array, Math, Number, sampleRate: 1000,
    AudioWorkletProcessor: class { constructor() { this.port = {postMessage() {}}; } },
    registerProcessor(name, impl) { Mixer = impl; }});
const start=recorder.indexOf('function mixerWorklet()');
const end=recorder.indexOf('// ---------------------------------------------------------------------------', start+30);
vm.runInContext(recorder.slice(start,end)+'\nmixerWorklet();', worklet);
function firstAudio(delay) {
    const mixer = new Mixer();
    mixer.port.onmessage({data:{k:'delay', seconds:delay}});
    for (let block=0; block<100; block++) {
        mixer.port.onmessage({data:{k:'rx', d:new Float32Array(10).fill(0.5)}});
        const output = new Float32Array(10);
        mixer.process([],[[output]]);
        if (output.some(x=>x!==0)) return block * 10;
    }
    throw Error('no audio emitted');
}
assert.equal(firstAudio(0.2), 200);
assert.equal(firstAudio(0.4), 400);
assert.equal(firstAudio(0), 10);
// Increasing the display buffer after recording starts must also increase
// the audio delay, rather than merely changing the next underrun threshold.
{
    const mixer = new Mixer();
    mixer.port.onmessage({data:{k:'delay',seconds:0.2}});
    for(let i=0;i<30;i++) {
        mixer.port.onmessage({data:{k:'rx',d:new Float32Array(10).fill(0.5)}});
        mixer.process([],[[new Float32Array(10)]]);
    }
    mixer.port.onmessage({data:{k:'delay',seconds:0.4}});
    let silent=0;
    for(let i=0;i<30;i++) {
        mixer.port.onmessage({data:{k:'rx',d:new Float32Array(10).fill(0.5)}});
        const output=new Float32Array(10);mixer.process([],[[output]]);
        if(output.every(x=>x===0)) silent+=10;
    }
    assert(silent>=190 && silent<=220, 'adaptive delay must add about 200 ms of buffering');
}
console.log('PASS S-meter motion, peak and recorder presentation delay');
