const vm = require('node:vm'), fs = require('node:fs'), assert = require('node:assert/strict');
const source = fs.readFileSync('resources/web/performance.js','utf8');
let timer, report;
const context = {location:{search:'?metrics=1'}, URLSearchParams, performance:{now:()=>2000},
 requestAnimationFrame:()=>{}, setInterval:f=>timer=f, setTimeout:f=>f(),
 navigator:{userAgent:'test'}, innerWidth:800, innerHeight:600, devicePixelRatio:1,
 TextEncoder, Uint8Array, Blob:class {constructor(parts){report=JSON.parse(parts[0]);}},
 URL:{createObjectURL:()=> 'blob:test',revokeObjectURL:()=>{}},
 document:{hidden:false,body:{appendChild:()=>{}},createElement:()=>({style:{},click:()=>{}})}, window:{}};
vm.runInNewContext(source, context);
context.window.flexMetrics.binary(new Uint8Array([1,0,0]).buffer);
context.window.flexMetrics.json({type:'serverLoad',procPct:5});
timer(); context.window.flexMetrics.export();
assert.equal(report.samples[0].payloadBytes.spectrum,3);
assert.equal(report.samples[0].server.procPct,5);
assert.equal(report.samples[0].jsHeapBytes,null);
assert.equal(report.samples[0].longTasks,null);
for(let i=0;i<1810;i++)timer();
context.window.flexMetrics.export();assert.equal(report.samples.length,1800);
const off={...context,location:{search:''},window:{}};
vm.runInNewContext(source,off);assert.equal(off.window.flexMetrics,undefined);
console.log('metrics: counters, bounded history, unavailable sensors and opt-in passed');
