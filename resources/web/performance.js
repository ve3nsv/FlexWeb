/* Station performance logging; ?metrics=1 also enables local JSON export.
 * Summaries go only to this station server, never a third-party service. */
(function () {
    'use strict';
    const visibleDiagnostics = new URLSearchParams(location.search).get('metrics') === '1';
    let enabled = false, observing = false;
    const history = [], maxSamples = 1800;
    let received = {audio: 0, spectrum: 0, json: 0, other: 0};
    let server = null, dsp = null, frames = [], lastFrame = 0, longTasks = 0, longTaskMs = 0;
    let observer = null;
    try {
        observer = new PerformanceObserver(list => {
            for (const e of list.getEntries()) { longTasks++; longTaskMs += e.duration; }
        });
        // Enabled only while the station is collecting measurements.
    } catch (_) { /* Unsupported browsers report unavailable, not zero. */ }
    function frame(t) {
        if (!enabled) { lastFrame=0; return; }
        if (!document.hidden && lastFrame) {
            frames.push(t - lastFrame);
            if (frames.length > 1000) frames.shift();
        }
        lastFrame = document.hidden ? 0 : t;
        requestAnimationFrame(frame);
    }
    if(document.addEventListener) document.addEventListener('visibilitychange',()=>{lastFrame=0;frames=[];});
    let previous = performance.now();
    window.flexMetrics = {
        json(message) {
            if (message.type === 'serverLoad') {
                const next=visibleDiagnostics || !!(message.performanceLog && message.performanceLog.enabled);
                if(next && !enabled) { previous=performance.now(); received={audio:0,spectrum:0,json:0,other:0}; requestAnimationFrame(frame); if(observer && !observing) { try {observer.observe({type:'longtask',buffered:false}); observing=true;} catch (_) {observer=null;} } }
                if(!next && observing) {observer.disconnect();observing=false;}
                enabled=next;
            }
            if (!enabled) return;
            received.json += new TextEncoder().encode(JSON.stringify(message)).length;
            if (message.type === 'serverLoad') server = message;
            if (message.type === 'flexDsp') dsp = {queueMs:message.rxDspQueueMs, processingUs:message.rxDspProcessingUs, droppedBlocks:message.rxDspDroppedBlocks};
        },
        binary(buffer) {
            if (!enabled) return;
            const type = new Uint8Array(buffer)[0];
            const key = type === 1 ? 'spectrum' : [2,5].includes(type) ? 'audio' : 'other';
            received[key] += buffer.byteLength;
        },
        export() {
            const report = {schema: 1, recordedAt: new Date().toISOString(), userAgent: navigator.userAgent,
                viewport: [innerWidth, innerHeight], devicePixelRatio, samples: history,
                notes: 'Payload bytes only; frame gaps include scheduling. JS heap is not browser process RAM. Use DevTools/OS profiling for CPU and total RAM.'};
            const url = URL.createObjectURL(new Blob([JSON.stringify(report, null, 2)], {type:'application/json'}));
            const a = document.createElement('a'); a.href = url; a.download = 'flexweb-performance.json'; a.click();
            setTimeout(() => URL.revokeObjectURL(url), 1000);
        }
    };
    const panel = document.createElement('button'); panel.type = 'button';
    panel.style.cssText = 'position:fixed;bottom:40px;right:8px;z-index:99999;padding:8px;background:#172536;color:white;border:1px solid #789';
    panel.title = 'Download diagnostics (maximum one hour). No audio or passwords are recorded.';
    panel.onclick = () => window.flexMetrics.export(); if(visibleDiagnostics)document.body.appendChild(panel);
    setInterval(() => {
        if (!enabled) return;
        const now = performance.now(), seconds = (now - previous) / 1000; previous = now;
        const sorted = frames.slice().sort((a,b) => a-b);
        const sample = {at: new Date().toISOString(), elapsedSeconds: seconds, hidden: document.hidden,
            payloadBytes: received, frameP95Ms: sorted.length ? sorted[Math.floor((sorted.length-1)*.95)] : null,
            longTasks: observer ? longTasks : null, longTaskMs: observer ? longTaskMs : null,
            jsHeapBytes: performance.memory ? performance.memory.usedJSHeapSize : null,
            server: server, dsp: dsp};
        if(visibleDiagnostics) {history.push(sample); if (history.length > maxSamples) history.shift();}
        if(server && server.performanceLog && server.performanceLog.enabled && window.wsTransport && window.wsTransport.isOpen()) {
            window.wsTransport.sendCommand({cmd:'performanceSample',sample:{hidden:sample.hidden,elapsedSeconds:seconds,
                frameP95Ms:sample.frameP95Ms,longTasks:sample.longTasks,longTaskMs:sample.longTaskMs,jsHeapBytes:sample.jsHeapBytes,
                audioBufferMs: window.audioJbStats ? window.audioJbStats.bufMs : null,
                audioTargetMs: window.audioJbStats ? window.audioJbStats.targetMs : null,
                audioUnderruns: window.audioJbStats ? window.audioJbStats.underruns : null,
                audioSkips: window.audioJbStats ? window.audioJbStats.skips : null,
                payloadBytes:received}});
        }
        const rate = Object.values(received).reduce((a,b) => a+b,0) / seconds;
        panel.textContent = 'Metrics: ' + (rate/1024).toFixed(1) + ' KiB/s · Export';
        received = {audio:0,spectrum:0,json:0,other:0}; frames = []; longTasks = 0; longTaskMs = 0;
    }, 2000);
})();
