/* ============================================================
   POGGED modgui — generic control binder
   --------------------------------------------------------------
   Four kinds of control, all keyed by data-handle == lv2:symbol:

     .pogged-fader  vertical fader   --value  0..1
     .pogged-knob   rotary pot       --knob   0..1
     .pogged-seg    enum switch      one .pogged-seg-btn[data-value] per value
     .pogged-lamp   0/1 toggle       lights when engaged

   Faders and knobs share data-min / data-max / data-scale="log" and go through
   the SAME toPct/toValue: a knob is only a fader with a different shape. That
   is the point of the class-based dispatch — knobs used to be recognised by
   their symbol's "pan_" prefix, with their own hand-rolled linear mapping, so
   nothing but a pan could ever be a knob.

   data-detent="<v>" snaps near a value: the pans and the filter ENV are
   bipolar and want a centre detent, the log pots do not.

   Ranges are hardcoded in the template because MOD-UI's 'start' event ships
   ports as { symbol, value } only.
   ============================================================ */

function (event, funcs) {

    var icon = event.icon

    function clamp01(x) { return x < 0 ? 0 : x > 1 ? 1 : x }

    /* value <-> normalized position. "log": min may be 0 (attack), the log
       floor is then max/2000 so short times keep usable drag travel. */
    function toPct(scale, min, max, v) {
        if (scale === 'log') {
            var lo = Math.max(min, max / 2000)
            return v <= lo ? 0 : clamp01(Math.log(v / lo) / Math.log(max / lo))
        }
        return clamp01((v - min) / (max - min))
    }
    function toValue(scale, min, max, pct) {
        if (scale === 'log') {
            var lo = Math.max(min, max / 2000)
            return pct <= 0 ? min : lo * Math.pow(max / lo, pct)
        }
        return min + pct * (max - min)
    }

    /* min / max / scale / detent off any control element. */
    function spec(el) {
        var d = el.attr('data-detent')
        return {
            sym:    el.attr('data-handle'),
            min:    parseFloat(el.attr('data-min')),
            max:    parseFloat(el.attr('data-max')),
            scale:  el.attr('data-scale'),
            detent: d === undefined ? null : parseFloat(d)
        }
    }

    function formatValue(sym, v) {
        if (sym.indexOf('pan_') === 0) {
            if (Math.abs(v) < 0.005) return 'C'
            return (v < 0 ? 'L' : 'R') + Math.round(Math.abs(v) * 100)
        }
        switch (sym) {
        case 'lp_cutoff':
            return v >= 1000 ? (v / 1000).toFixed(1) + ' kHz' : v.toFixed(0) + ' Hz'
        case 'attack_ms':
        case 'filter_env_a':
        case 'filter_env_d':
            return v >= 1000 ? (v / 1000).toFixed(2) + ' s' : v.toFixed(0) + ' ms'
        case 'detune_cents':
            return v.toFixed(1) + ' ct'
        case 'lp_q':
            return 'Q ' + v.toFixed(2)
        case 'warp_heel':
        case 'warp_toe':
            /* Semitones, signed: the sweep runs heel -> toe, so the sign has to
               be visible or the two ends look interchangeable. */
            return (v >= 0 ? '+' : '') + v.toFixed(1) + ' st'
        case 'filter_env':
            /* Signed depth: centre is off, and which way it sweeps matters. */
            if (Math.abs(v) < 0.005) return 'OFF'
            return (v > 0 ? '+' : '') + (v * 100).toFixed(0) + ' %'
        case 'input_gain':
            return v.toFixed(2) + ' x'
        default:
            return (v * 100).toFixed(0) + ' %'   /* levels: 0..2 -> 0..200 % */
        }
    }

    function readout(sym, v) {
        icon.find('[data-handle-value="' + sym + '"]').text(formatValue(sym, v))
    }

    function setVisual(sym, value) {
        var sel = '[data-handle="' + sym + '"]'

        var lamp = icon.find('.pogged-lamp' + sel)
        if (lamp.length) { lamp.toggleClass('is-on', value > 0.5); return }

        /* Segmented switch: light the button whose value is nearest. The port
           is a float even for an enum, so compare with a tolerance rather than
           ===, or a host sending 1.0000001 would light nothing at all. */
        var seg = icon.find('.pogged-seg' + sel)
        if (seg.length) {
            seg.find('.pogged-seg-btn').each(function () {
                var bv = parseFloat($(this).attr('data-value'))
                $(this).toggleClass('is-on', Math.abs(bv - value) < 0.5)
            })
            return
        }

        var knob = icon.find('.pogged-knob' + sel)
        if (knob.length) {
            var k = spec(knob)
            knob[0].style.setProperty('--knob',
                String(toPct(k.scale, k.min, k.max, value)))
            readout(sym, value)
            return
        }

        var el = icon.find('.pogged-fader' + sel)
        if (!el.length) return
        var f = spec(el)
        el[0].style.setProperty('--value', String(toPct(f.scale, f.min, f.max, value)))
        readout(sym, value)
    }

    if (event.type === 'start') {

        for (var i = 0; i < event.ports.length; i++) {
            setVisual(event.ports[i].symbol, event.ports[i].value)
        }

        var tooltip = icon.find('.pogged-tooltip')[0]
        function showTooltip(ev, text) {
            if (!tooltip) return
            tooltip.textContent = text
            tooltip.style.left = (ev.clientX + 14) + 'px'
            tooltip.style.top  = (ev.clientY - 26) + 'px'
            tooltip.style.display = 'block'
        }
        function hideTooltip() {
            if (tooltip) tooltip.style.display = 'none'
        }

        /* Knobs sit INSIDE a fader's column, so this must be registered first
           and stop propagation, and the fader handler below must ignore events
           originating in one — otherwise dragging a knob would also drive the
           fader underneath it. Relative drag (a knob is only 22px); 150px of
           travel covers the full sweep. */
        icon.on('mousedown.pogged', '.pogged-knob', function (e) {
            if (e.which && e.which !== 1) return
            e.preventDefault()
            e.stopPropagation()

            var knob = $(this)
            var k = spec(knob)
            if (!k.sym || !isFinite(k.min) || !isFinite(k.max)) return

            var startY   = e.pageY
            var startPct = parseFloat(knob[0].style.getPropertyValue('--knob') || '0.5')

            function move(ev) {
                var pct = clamp01(startPct + (startY - ev.pageY) / 150)
                var raw = toValue(k.scale, k.min, k.max, pct)
                if (k.detent !== null &&
                    Math.abs(raw - k.detent) < 0.03 * (k.max - k.min)) {
                    raw = k.detent
                    pct = toPct(k.scale, k.min, k.max, raw)
                }
                knob[0].style.setProperty('--knob', String(pct))
                funcs.set_port_value(k.sym, raw)
                /* 'from-js' isn't echoed back as 'change': refresh readout here */
                readout(k.sym, raw)
                showTooltip(ev, formatValue(k.sym, raw))
            }
            function up() {
                hideTooltip()
                $(document).off('mousemove.pogged mouseup.pogged')
            }
            $(document).on('mousemove.pogged', move)
                       .on('mouseup.pogged', up)
        })

        icon.on('click.pogged', '.pogged-lamp', function (e) {
            e.preventDefault()
            e.stopPropagation()
            var btn = $(this)
            var sym = btn.attr('data-handle')
            if (!sym) return
            var v = btn.hasClass('is-on') ? 0 : 1
            funcs.set_port_value(sym, v)
            setVisual(sym, v)
        })

        icon.on('click.pogged', '.pogged-seg-btn', function (e) {
            e.preventDefault()
            e.stopPropagation()
            var btn = $(this)
            var sym = btn.closest('.pogged-seg').attr('data-handle')
            var v   = parseFloat(btn.attr('data-value'))
            if (!sym || !isFinite(v)) return
            funcs.set_port_value(sym, v)
            setVisual(sym, v)
        })

        icon.on('mousedown.pogged', '.pogged-fader', function (e) {
            if (e.which && e.which !== 1) return
            /* mousedown fires before click, so a knob or lamp press would
               otherwise jump the fader it sits on. */
            if ($(e.target).closest('.pogged-knob').length) return
            if ($(e.target).closest('.pogged-lamp').length) return
            e.preventDefault()
            e.stopPropagation()

            var el = $(this)
            var f  = spec(el)
            var track = el.find('.pogged-fader-track')
            if (!f.sym || !isFinite(f.min) || !isFinite(f.max)) return

            function move(ev) {
                var rect = track[0].getBoundingClientRect()
                if (rect.height <= 0) return
                var pct = 1 - clamp01((ev.pageY - rect.top) / rect.height)
                el[0].style.setProperty('--value', String(pct))
                var raw = toValue(f.scale, f.min, f.max, pct)
                funcs.set_port_value(f.sym, raw)
                readout(f.sym, raw)
                showTooltip(ev, formatValue(f.sym, raw))
            }
            function up() {
                hideTooltip()
                $(document).off('mousemove.pogged mouseup.pogged')
            }
            $(document).on('mousemove.pogged', move)
                       .on('mouseup.pogged', up)
            move(e)
        })
    }
    else if (event.type === 'change') {
        setVisual(event.symbol, event.value)
    }
}
