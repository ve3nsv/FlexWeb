import assert from 'node:assert/strict';
import {sstvFskTones, SstvFskIdDecoder} from '../resources/web-shared/sstv.mjs';
function synth(tones, fs, offset=0) {
 const n=Math.round(tones.length*0.022*fs); const out=new Float32Array(n);let ph=0,at=0,t=0;
 for(const [f,ms] of tones){t+=ms/1000;const end=Math.round(t*fs);for(;at<end;at++){out[at]=0.7*Math.sin(ph);ph+=2*Math.PI*(f+offset)/fs;}}
 return out;
}
// Independent framing expectations from PySSTV's 0x20,0x2a header and 0x01 terminator.
const tones=sstvFskTones('VE3NSV');
assert.deepEqual(tones.slice(0,6).map(t=>t[0]),[2100,2100,2100,2100,2100,1900]);
assert.deepEqual(tones.slice(6,12).map(t=>t[0]),[2100,1900,2100,1900,2100,1900]);
assert.deepEqual(tones.slice(-6).map(t=>t[0]),[1900,2100,2100,2100,2100,2100]);
assert.equal(tones.length,(6+3)*6);
assert.deepEqual(sstvFskTones('bad <input>'),[]);
for(const fs of [12000,44100,48000]) for(const offset of [0,50,-60]) {
 const found=[];const dec=new SstvFskIdDecoder(fs,call=>found.push(call));
 const signal=synth(tones,fs,offset), out=new Float32Array(signal.length+Math.round(fs*.07));
 out.set(signal,Math.round(fs*.013));
 for(let p=0;p<out.length;p+=997) dec.push(out.subarray(p,p+997),offset);
 assert.deepEqual(found,['VE3NSV'],`FSK ${fs} Hz, offset ${offset}`);
}
const found=[];const dec=new SstvFskIdDecoder(12000,call=>found.push(call));
for(let i=0;i<60;i++) dec.push(new Float32Array(1200));
assert.deepEqual(found,[]);
console.log('PASS FSK ID framing, three sample rates, tuning offset and silence rejection');
