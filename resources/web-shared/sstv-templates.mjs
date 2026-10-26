// Normalized positions keep saved layouts usable in every SSTV image mode.
export const SSTV_TEMPLATES = [
    {name:'CQ', layers:[{text:'CQ SSTV %m',x:0.04,y:0.14,size:0.1,color:'#ffffff'}, {text:'%l  %t UTC',x:0.04,y:0.94,size:0.07,color:'#ffffff'}]},
    {name:'Reply / QSL', layers:[{text:'%c DE %m',x:0.04,y:0.12,size:0.08,color:'#ffffff'}, {text:'%i',x:0.04,y:0.2,w:0.53,h:0.6}, {text:'TNX %o',x:0.6,y:0.36,size:0.065,color:'#ffffff'}, {text:'RSV %r',x:0.6,y:0.52,size:0.065,color:'#ffffff'}, {text:'73!',x:0.6,y:0.7,size:0.12,color:'#ffff00'}, {text:'%l  %d',x:0.04,y:0.95,size:0.06,color:'#ffffff'}]},
    {name:'73',layers:[{text:'73 %c',x:0.04,y:0.2,size:0.14,color:'#ffff00'}, {text:'DE %m  %l',x:0.04,y:0.88,size:0.08,color:'#ffffff'}, {text:'%x',x:0.04,y:0.5,size:0.07,color:'#ffffff'}]}
];
export function expandSstvMacros(text, values) {
    return String(text).replace(/%([mlfncortdxyz%])/g, (all, key) => key === '%' ? '%' : String(values[key] ?? '').slice(0,160));
}
export function validateSstvTemplates(templates) {
    if (!Array.isArray(templates) || templates.length > 12) return false;
    return templates.every(t => typeof t.name === 'string' && t.name.trim().length > 0 && t.name.length <= 32 && Array.isArray(t.layers) && t.layers.length <= 16 && t.layers.every(l => {
        const n = (key,min,max) => typeof l[key] === 'number' && Number.isFinite(l[key]) && l[key] >= min && l[key] <= max;
        return typeof l.text === 'string' && l.text.length <= 160 && n('x',0,1) && n('y',0,1) && (l.text === '%i' ? n('w',0.05,1) && n('h',0.05,1) : n('size',0.02,0.3) && /^#[0-9a-f]{6}$/i.test(l.color));
    }));
}
export function drawSstvTemplate(g, template, values, inset) {
    const {width:w,height:h} = g.canvas;
    g.save();
    for (const l of template.layers) {
        if (l.text === '%i') {
            if (!inset) continue;
            const iw = inset.naturalWidth || inset.width, ih = inset.naturalHeight || inset.height;
            const k = Math.min(l.w*w/iw, l.h*h/ih);
            g.drawImage(inset,l.x*w,l.y*h,iw*k,ih*k);
        } else {
            const size = Math.max(6,Math.round(l.size*h));
            g.font = `bold ${size}px sans-serif`; g.textAlign = 'left'; g.textBaseline='alphabetic';
            g.strokeStyle = '#000000'; g.fillStyle = l.color; g.lineWidth = Math.max(2,size/7);
            const text = expandSstvMacros(l.text,values);
            g.strokeText(text,l.x*w,l.y*h,w*(1-l.x)); g.fillText(text,l.x*w,l.y*h,w*(1-l.x));
        }
    }
    g.restore();
}
