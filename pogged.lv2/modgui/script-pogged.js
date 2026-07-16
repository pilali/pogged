/* ============================================================
   POGGED modgui — generic vertical-fader binder
   --------------------------------------------------------------
   Every continuous control is a .pogged-fader with:
     data-handle  = lv2:symbol
     data-min / data-max
     data-scale   = "log" (optional) for frequency / time
   The cap + fill positions are driven by the CSS var --value.
   Enums and toggles are .pogged-seg with the same data-handle and
   one .pogged-seg-btn[data-value] per value.
   data ranges are hardcoded in the template because MOD-UI's
   'start' event ships ports as { symbol, value } only.
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

    function isPan(sym) { return sym.indexOf('pan_') === 0 }

    function formatValue(sym, v) {
        if (isPan(sym)) {
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
            /* Semitones, signed: the sweep direction is heel -> toe, so the
               sign has to be visible or the two ends look interchangeable. */
            return (v >= 0 ? '+' : '') + v.toFixed(1) + ' st'
        case 'filter_env':
            /* Signed depth: centre is off, and which way it sweeps matters. */
            if (Math.abs(v) < 0.005) return 'OFF'
            return (v > 0 ? '+' : '') + (v * 100).toFixed(0) + ' %'
        case 'input_gain':
            return v.toFixed(2) + ' x'
        case 'attack_sens':
        case 'filter_sens':
        case 'spread':
        case 'warp':
            return (v * 100).toFixed(0) + ' %'
        default:
            return (v * 100).toFixed(0) + ' %'   /* levels: 0..2 -> 0..200 % */
        }
    }

    function setVisual(sym, value) {
        /* Segmented switch: light the button whose value is nearest. The port
           is a float even for an enum, so compare with a tolerance rather than
           ===, or a host sending 1.0000001 would light nothing at all. */
        var lamp = icon.find('.pogged-lamp[data-handle="' + sym + '"]')
        if (lamp.length) { lamp.toggleClass('is-on', value > 0.5); return }

        var seg = icon.find('.pogged-seg[data-handle="' + sym + '"]')
        if (seg.length) {
            seg.find('.pogged-seg-btn').each(function () {
                var bv = parseFloat($(this).attr('data-value'))
                $(this).toggleClass('is-on', Math.abs(bv - value) < 0.5)
            })
            return
        }
        if (isPan(sym)) {
            var pot = icon.find('.pogged-pan[data-handle="' + sym + '"]')
            if (!pot.length) return
            var pmin = parseFloat(pot.attr('data-min'))
            var pmax = parseFloat(pot.attr('data-max'))
            pot[0].style.setProperty('--pan',
                String(clamp01((value - pmin) / (pmax - pmin))))
            return
        }
        var el = icon.find('.pogged-fader[data-handle="' + sym + '"]')
        if (!el.length) return
        var min   = parseFloat(el.attr('data-min'))
        var max   = parseFloat(el.attr('data-max'))
        var scale = el.attr('data-scale')
        el[0].style.setProperty('--value', String(toPct(scale, min, max, value)))
        icon.find('[data-handle-value="' + sym + '"]').text(formatValue(sym, value))
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

        /* Pan pots sit INSIDE their fader column, so this must be registered
           first and stop propagation, and the fader handler below must ignore
           events originating in a pot — otherwise dragging a pan would also
           drive the fader underneath it. Relative drag (the pot is only 22px);
           150px of travel covers the full L..R sweep. */
        icon.on('mousedown.pogged', '.pogged-pan', function (e) {
            if (e.which && e.which !== 1) return
            e.preventDefault()
            e.stopPropagation()

            var pot = $(this)
            var sym = pot.attr('data-handle')
            var min = parseFloat(pot.attr('data-min'))
            var max = parseFloat(pot.attr('data-max'))
            if (!sym || !isFinite(min) || !isFinite(max)) return

            var startY = e.pageY
            var startV = min + (max - min) *
                parseFloat(pot[0].style.getPropertyValue('--pan') || '0.5')

            function move(ev) {
                var d   = (startY - ev.pageY) / 150
                var raw = Math.max(min, Math.min(max, startV + d * (max - min)))
                if (Math.abs(raw) < 0.03) raw = 0        /* centre detent */
                pot[0].style.setProperty('--pan',
                    String(clamp01((raw - min) / (max - min))))
                funcs.set_port_value(sym, raw)
                showTooltip(ev, formatValue(sym, raw))
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
            setVisual(sym, v)   /* 'from-js' is not echoed back as 'change' */
        })

        icon.on('click.pogged', '.pogged-seg-btn', function (e) {
            e.preventDefault()
            e.stopPropagation()
            var btn = $(this)
            var sym = btn.closest('.pogged-seg').attr('data-handle')
            var v   = parseFloat(btn.attr('data-value'))
            if (!sym || !isFinite(v)) return
            funcs.set_port_value(sym, v)
            setVisual(sym, v)   /* 'from-js' is not echoed back as 'change' */
        })

        icon.on('mousedown.pogged', '.pogged-fader', function (e) {
            if (e.which && e.which !== 1) return
            if ($(e.target).closest('.pogged-pan').length) return   /* pot drag */
            if ($(e.target).closest('.pogged-lamp').length) return  /* DRY lamp */
            e.preventDefault()
            e.stopPropagation()

            var el    = $(this)
            var sym   = el.attr('data-handle')
            var min   = parseFloat(el.attr('data-min'))
            var max   = parseFloat(el.attr('data-max'))
            var scale = el.attr('data-scale')
            var track = el.find('.pogged-fader-track')
            if (!sym || !isFinite(min) || !isFinite(max)) return

            function move(ev) {
                var rect = track[0].getBoundingClientRect()
                if (rect.height <= 0) return
                var pct = 1 - clamp01((ev.pageY - rect.top) / rect.height)
                el[0].style.setProperty('--value', String(pct))
                var raw = toValue(scale, min, max, pct)
                funcs.set_port_value(sym, raw)
                /* 'from-js' isn't echoed back as 'change': refresh readout here */
                icon.find('[data-handle-value="' + sym + '"]').text(formatValue(sym, raw))
                showTooltip(ev, formatValue(sym, raw))
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
