const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const html=fs.readFileSync('resources/web/index.html','utf8');
function fn(name){const start=html.indexOf('function '+name+'(');return html.slice(start,html.indexOf('\n}',start)+2);}
let height=4;
const ctx={wfImageData:null,wfRingHead:0,wfDirtyRows:0,wfNeedsFullUpload:true,wfBufDirty:true,
 scopeDpr:1,wfOffCanvas:null,wfOffCtx:null,spectData:null,spectOor:true,wfPrevSweep:null,wfSpeed:0,WF_SPEED_ROWS:[1],
 colorTable:Array.from({length:256},(_,x)=>[x,x,x]),wfColorIndex:x=>x,
 wfFillBlank:(data,a,b)=>data.fill(0,a,b),
 document:{getElementById:()=>({height}),createElement:()=>({getContext:()=>({createImageData:(w,h)=>({width:w,height:h,data:new Uint8ClampedArray(w*h*4)})})})}};
vm.createContext(ctx);vm.runInContext(fn('initWaterfallBuffer')+'\n'+fn('updateWaterfallRow'),ctx);
ctx.initWaterfallBuffer(3);
function rows(){return Array.from({length:ctx.wfImageData.height},(_,i)=>ctx.wfImageData.data[((ctx.wfRingHead+i)%ctx.wfImageData.height)*12]);}
for(let i=1;i<=30;i++){ctx.spectData=new Uint8Array([i,i,i]);ctx.updateWaterfallRow();assert.deepEqual(rows().slice(0,Math.min(4,i)),Array.from({length:Math.min(4,i)},(_,j)=>i-j));}
height=6;ctx.initWaterfallBuffer(3);assert.deepEqual(rows(),[30,29,28,27,0,0]);
height=2;ctx.initWaterfallBuffer(3);assert.deepEqual(rows(),[30,29]);
console.log('waterfall ring: wrap, order, expansion and shrink passed');
