/* ==========================================================================
   Wizard meter face (wizard-look branch): an analog S-meter on an arc with
   the wizard behind it. He is not there while the signal is below S9; past
   S9 he fades in and grows more intense with the signal - brighter, eyes
   glowing, the lightning flickering harder - and is at full power at +60 dB.
   On TX the same face reads Po against the radio's rated power and he follows
   the power.

   WizardMeter.draw(canvas, p) with
     p.tx       transmitting
     p.s        RX level in dB over S9 (S0 = -54, S9 = 0, +60), animated
     p.peak     RX peak hold, same units
     p.po       TX power in watts, p.poFull the rated power
     p.auxLabel / p.auxText   the selected TX reading (SWR, ALC, ...)
   The host calls draw() on every reading; the face runs its own animation
   (the lightning, the needle's spring, his fade) only while something moves.
   ========================================================================== */
(function () {
    'use strict';

    var img = new Image(), bolts = new Image();
    var imgOk = false, boltsOk = false;
    img.onload = function () { imgOk = true; kick(); };
    bolts.onload = function () { boltsOk = true; kick(); };
    img.src = 'wizard.jpg';
    bolts.src = 'wizard-bolts.jpg';
    // Where his eyes are in wizard.jpg (1320 x 439).
    var IMG_W = 1320, IMG_H = 439, EYES = [[638, 259], [699, 259]], FACE_Y = 0.6;

    var canvas = null, p = null, raf = 0, lastTs = 0;
    var needle = null, needleV = 0;   // needle position as a scale fraction, its velocity
    var power = 0;                    // his shown intensity, 0..1
    var flick = 0, flash = 0;         // lightning flicker / occasional flash

    // RX scale: S units linear up to S9, then the dB over S9 compressed as
    // on an analog meter (the spacing of the mock-up: +10 and +20 wider than
    // +40 and +60).
    var S9F = 0.583;
    var DB_PTS = [[0, S9F], [10, 0.688], [20, 0.789], [40, 0.895], [60, 1.0]];
    function rxFrac(db) {
        if (db <= 0) return Math.max(0, 0.026 + (db + 54) / 6 * 0.0619);
        for (var i = 1; i < DB_PTS.length; i++) {
            if (db <= DB_PTS[i][0]) {
                var a = DB_PTS[i - 1], b = DB_PTS[i];
                return a[1] + (db - a[0]) / (b[0] - a[0]) * (b[1] - a[1]);
            }
        }
        return 1;
    }
    // His target intensity: nothing below S9, a fade in over the first few
    // dB, then rising to full at +60 (on TX: with the power).
    function target() {
        if (!p) return 0;
        if (p.tx) {
            var f = p.poFull > 0 ? p.po / p.poFull : 0;
            return f <= 0.02 ? 0 : Math.min(1, 0.25 + 0.75 * f);
        }
        var d = p.s;
        if (d <= 0) return 0;
        return Math.min(1, d / 4) * Math.min(1, 0.22 + 0.78 * Math.min(1, d / 60));
    }
    function wantFrac() {
        if (!p) return 0;
        if (p.tx) return Math.max(0, Math.min(1.02, p.poFull > 0 ? p.po / p.poFull : 0));
        return rxFrac(p.s);
    }

    function draw(c, params) {
        canvas = c; p = params;
        kick();
    }
    function kick() {
        if (!raf && canvas) { lastTs = performance.now(); raf = requestAnimationFrame(frame); }
    }
    function stop() {
        if (raf) cancelAnimationFrame(raf);
        raf = 0; canvas = null;
    }

    function frame(now) {
        raf = 0;
        if (!canvas || !p) return;
        var dt = Math.max(0.001, Math.min(0.1, (now - lastTs) / 1000));
        lastTs = now;
        // Needle: a stiff, slightly under-damped spring so it swings like a
        // real movement but settles fast.
        var want = wantFrac();
        if (needle === null) needle = want;
        var k = 260, damp = 26;
        needleV += (k * (want - needle) - damp * needleV) * dt;
        needle += needleV * dt;
        if (Math.abs(want - needle) < 0.0005 && Math.abs(needleV) < 0.001) { needle = want; needleV = 0; }
        // His intensity: quick to come, slow to leave.
        var t = target();
        power += (t - power) * (1 - Math.exp(-dt / (t > power ? 0.12 : 0.7)));
        if (power < 0.004 && t === 0) power = 0;
        // Lightning: a noisy flicker, and at high power the odd full flash.
        flick += ((Math.random() * 2 - 1) - flick) * Math.min(1, dt * 18);
        if (flash > 0) flash = Math.max(0, flash - dt * 5);
        if (power > 0.45 && Math.random() < dt * (power - 0.4) * 5) flash = 0.6 + Math.random() * 0.4;

        paint(now);
        canvas.parentElement && canvas.parentElement.style.setProperty('--wz-power', power.toFixed(3));
        var moving = needle !== want || needleV !== 0 || power > 0 || Math.abs(t - power) > 0.001;
        if (moving) raf = requestAnimationFrame(frame);
    }

    function paint(now) {
        var dpr = window.devicePixelRatio || 1;
        var w = canvas.clientWidth, h = canvas.clientHeight;
        if (!w || !h) return;
        if (canvas.width !== Math.round(w * dpr) || canvas.height !== Math.round(h * dpr)) {
            canvas.width = Math.round(w * dpr);
            canvas.height = Math.round(h * dpr);
        }
        var ctx = canvas.getContext('2d');
        ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
        ctx.clearRect(0, 0, w, h);

        // Ground: deep navy, a little lighter behind the scale.
        var g = ctx.createRadialGradient(w / 2, h * 0.9, 10, w / 2, h * 0.7, w * 0.6);
        g.addColorStop(0, '#0d1a33');
        g.addColorStop(1, '#050913');
        ctx.fillStyle = g;
        ctx.fillRect(0, 0, w, h);

        // The wizard, cover-fit on the width, his face a little above centre.
        if (power > 0 && imgOk) {
            var sc = Math.max(w / IMG_W, h / IMG_H);
            var dw = IMG_W * sc, dh = IMG_H * sc;
            var dx = (w - dw) / 2, dy = Math.min(0, Math.max(h - dh, h * 0.62 - dh * FACE_Y));
            var shake = power > 0.85 ? (power - 0.85) * 6 : 0;
            if (shake) { dx += (Math.random() - 0.5) * shake; dy += (Math.random() - 0.5) * shake; }
            ctx.save();
            ctx.globalAlpha = Math.min(1, 0.15 + 0.95 * power);
            ctx.filter = 'brightness(' + (0.45 + 0.75 * power).toFixed(2) + ') saturate(' + (0.8 + 0.6 * power).toFixed(2) + ')';
            ctx.drawImage(img, dx, dy, dw, dh);
            ctx.restore();
            if (boltsOk) {
                ctx.save();
                ctx.globalCompositeOperation = 'lighter';
                ctx.globalAlpha = Math.max(0, Math.min(1, power * (0.45 + 0.35 * flick) + flash * power));
                ctx.drawImage(bolts, dx, dy, dw, dh);
                ctx.restore();
            }
            // Eyes: a glow that grows with him.
            ctx.save();
            ctx.globalCompositeOperation = 'lighter';
            for (var e = 0; e < EYES.length; e++) {
                var ex = dx + EYES[e][0] * sc, ey = dy + EYES[e][1] * sc;
                var er = (5 + 22 * power + 6 * flash) * Math.max(0.6, sc * 2);
                var eg = ctx.createRadialGradient(ex, ey, 0, ex, ey, er);
                eg.addColorStop(0, 'rgba(220,240,255,' + Math.min(1, 0.4 + power).toFixed(2) + ')');
                eg.addColorStop(0.25, 'rgba(90,170,255,' + (0.7 * power).toFixed(2) + ')');
                eg.addColorStop(1, 'rgba(40,80,255,0)');
                ctx.fillStyle = eg;
                ctx.fillRect(ex - er, ey - er, er * 2, er * 2);
            }
            ctx.restore();
        }
        // Keep the scale readable over him: darken the top and the bottom.
        var v = ctx.createLinearGradient(0, 0, 0, h);
        v.addColorStop(0, 'rgba(4,8,18,0.55)');
        v.addColorStop(0.45, 'rgba(4,8,18,0.0)');
        v.addColorStop(1, 'rgba(4,8,18,0.35)');
        ctx.fillStyle = v;
        ctx.fillRect(0, 0, w, h);

        drawScale(ctx, w, h);
    }

    function drawScale(ctx, w, h) {
        var m = Math.max(28, w * 0.055);
        var yEnd = h * 0.68, yTop = h * 0.34;
        var c = w / 2 - m, s = yEnd - yTop;
        var R = (c * c + s * s) / (2 * s);
        var cx = w / 2, cy = yTop + R;
        var tMax = Math.asin(Math.min(1, c / R));
        function ang(f) { return -tMax + 2 * tMax * f; }
        function pt(f, r) { var a = ang(f); return [cx + r * Math.sin(a), cy - r * Math.cos(a)]; }
        var fs = Math.max(10, Math.min(16, Math.round(h / 10)));
        var tx = !!p.tx;
        var redFrom = tx ? 0.9 : S9F;

        // The arc: cyan to the red line, red past it, with a glow.
        function arc(f0, f1, col, glow) {
            ctx.save();
            ctx.beginPath();
            ctx.arc(cx, cy, R, -Math.PI / 2 + ang(f0), -Math.PI / 2 + ang(f1));
            ctx.strokeStyle = col;
            ctx.lineWidth = Math.max(3, h / 36);
            ctx.lineCap = 'round';
            ctx.shadowColor = glow;
            ctx.shadowBlur = 10;
            ctx.stroke();
            ctx.restore();
        }
        arc(0, redFrom, '#3fd0ff', 'rgba(0,190,255,0.9)');
        arc(redFrom, 1, '#ff4a5a', 'rgba(255,50,70,0.9)');

        // Ticks and labels.
        var marks = [];
        if (tx) {
            var full = p.poFull > 0 ? p.poFull : 100;
            for (var q = 0; q <= 8; q++) {
                var f = q / 8;
                marks.push({ f: f, major: q % 2 === 0, text: q % 2 === 0 ? String(Math.round(full * f)) + (q === 8 ? 'W' : '') : '' });
            }
        } else {
            marks.push({ f: rxFrac(-54), major: true, text: 'S', big: true });
            for (var su = 1; su <= 9; su++)
                marks.push({ f: rxFrac((su - 9) * 6), major: su % 2 === 1, text: su % 2 === 1 ? String(su) : '' });
            [[10, '+10'], [20, '+20'], [30, ''], [40, '+40'], [50, ''], [60, '+60']].forEach(function (d) {
                marks.push({ f: rxFrac(d[0]), major: !!d[1], text: d[1] });
            });
        }
        ctx.save();
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        for (var i = 0; i < marks.length; i++) {
            var mk = marks[i];
            var red = mk.f > redFrom + 0.001;
            var a = pt(mk.f, R + (mk.major ? 7 : 4)), b = pt(mk.f, R - (mk.major ? 5 : 3));
            ctx.strokeStyle = red ? '#ff6070' : '#bfefff';
            ctx.lineWidth = mk.major ? 2 : 1;
            ctx.beginPath(); ctx.moveTo(a[0], a[1]); ctx.lineTo(b[0], b[1]); ctx.stroke();
            if (mk.text) {
                var l = pt(mk.f, R + fs * 1.15);
                ctx.font = (mk.big ? 'bold ' + Math.round(fs * 1.35) : 'bold ' + fs) + 'px "Segoe UI", Inter, system-ui, sans-serif';
                ctx.fillStyle = red ? '#ff5a66' : '#eaf6ff';
                ctx.shadowColor = red ? 'rgba(255,40,60,0.8)' : 'rgba(0,170,255,0.7)';
                ctx.shadowBlur = 6;
                // The S at the start sits below the arc's end, as on the mock-up.
                if (mk.big) { var sp = pt(mk.f, R - fs * 1.6); ctx.fillText(mk.text, sp[0] - 2, sp[1] + fs * 0.6); }
                else ctx.fillText(mk.text, l[0], l[1]);
            }
        }
        ctx.restore();

        // Peak hold (RX): a small red mark on the arc.
        if (!tx && p.peak !== undefined && p.peak > p.s + 0.5) {
            var pk = rxFrac(p.peak), p1 = pt(pk, R + 8), p2 = pt(pk, R - 8);
            ctx.save();
            ctx.strokeStyle = '#ffd04a';
            ctx.lineWidth = 2;
            ctx.beginPath(); ctx.moveTo(p1[0], p1[1]); ctx.lineTo(p2[0], p2[1]); ctx.stroke();
            ctx.restore();
        }

        // Readouts: level on the left, dBm (or the TX reading) on the right.
        var left, right, title;
        if (tx) {
            left = 'Po ' + Math.round(p.po || 0) + ' W';
            right = p.auxLabel ? p.auxLabel + ' ' + p.auxText : '';
            title = 'TX METER';
        } else {
            left = sText(p.s);
            right = Math.round(p.s - 73) + ' dBm';
            title = 'S-METER';
        }
        var big = Math.max(13, Math.min(22, Math.round(h / 7.5)));
        ctx.save();
        ctx.textBaseline = 'top';
        ctx.font = 'bold ' + big + 'px "Segoe UI", Inter, system-ui, sans-serif';
        ctx.shadowColor = 'rgba(0,180,255,0.8)';
        ctx.shadowBlur = 8;
        ctx.fillStyle = '#7fe3ff';
        ctx.textAlign = 'left';
        ctx.fillText(left, Math.round(w * 0.03), Math.round(h * 0.07));
        ctx.textAlign = 'right';
        ctx.fillText(right, Math.round(w * 0.97), Math.round(h * 0.07));
        ctx.font = '600 ' + Math.max(10, Math.round(big * 0.7)) + 'px "Segoe UI", Inter, system-ui, sans-serif';
        ctx.fillStyle = '#d6e8f5';
        ctx.shadowBlur = 4;
        ctx.textAlign = 'center';
        ctx.fillText(title, w / 2, Math.round(h * 0.08));
        ctx.restore();

        // Needle: from the pivot at the bottom centre to the printed value.
        var nf = Math.max(-0.02, Math.min(1.03, needle));
        var px = w / 2, py = h * 0.93;
        var tip = pt(nf, R - 2);
        ctx.save();
        ctx.lineCap = 'round';
        ctx.shadowColor = 'rgba(180,230,255,0.9)';
        ctx.shadowBlur = 8;
        ctx.strokeStyle = '#ffffff';
        ctx.lineWidth = Math.max(2, h / 60);
        ctx.beginPath(); ctx.moveTo(px, py); ctx.lineTo(tip[0], tip[1]); ctx.stroke();
        ctx.restore();
        // Pivot: a polished knob.
        var kr = Math.max(7, h / 15);
        var kg = ctx.createRadialGradient(px - kr * 0.35, py - kr * 0.35, 1, px, py, kr);
        kg.addColorStop(0, '#ffffff');
        kg.addColorStop(0.45, '#aab6c4');
        kg.addColorStop(1, '#3b4656');
        ctx.save();
        ctx.shadowColor = 'rgba(150,210,255,0.8)';
        ctx.shadowBlur = 8;
        ctx.fillStyle = kg;
        ctx.beginPath(); ctx.arc(px, py, kr, 0, Math.PI * 2); ctx.fill();
        ctx.restore();
    }

    function sText(db) {
        if (db <= 0) return 'S' + Math.max(0, Math.round((db + 54) / 6));
        return 'S9 +' + Math.round(db) + ' dB';
    }

    window.WizardMeter = { draw: draw, stop: stop, rxFrac: rxFrac };
})();
