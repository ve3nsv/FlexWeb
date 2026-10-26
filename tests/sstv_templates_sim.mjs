import assert from 'node:assert/strict';
import {SSTV_TEMPLATES,expandSstvMacros,validateSstvTemplates,drawSstvTemplate} from '../resources/web-shared/sstv-templates.mjs';
assert(validateSstvTemplates(SSTV_TEMPLATES));
assert.equal(expandSstvMacros('%c DE %m %% %q',{c:'VE3NSV',m:'W1AW'}),'VE3NSV DE W1AW % %q');
assert.equal(expandSstvMacros('%m',{m:'%c'}),'%c'); // single pass; user text cannot inject macros
for(const invalid of [null,{},Array(13).fill(SSTV_TEMPLATES[0])]) assert(!validateSstvTemplates(invalid));
for(const [key,value] of [['x',-1],['size',0],['color','red'],['text','x'.repeat(161)]]) {
 const t=structuredClone(SSTV_TEMPLATES);t[0].layers[0][key]=value;assert(!validateSstvTemplates(t));
}
const text=[],images=[];
const ctx={canvas:{width:320,height:256},save(){},restore(){},strokeText(){},fillText(...args){text.push(args);},drawImage(...args){images.push(args);}};
drawSstvTemplate(ctx,SSTV_TEMPLATES[1],{m:'W1AW',c:'VE3NSV',r:'595'},{width:320,height:256});
assert(text.some(t=>t[0]==='VE3NSV DE W1AW'));
assert(text.some(t=>t[0]==='RSV 595'));
assert.equal(images.length,1);assert(images[0][3]<=320*.53);assert(images[0][4]<=256*.6);
console.log('PASS SSTV template validation, macro expansion and image fitting');
