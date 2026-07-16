// modgui_measure — the panel's geometry, measured rather than squinted at.
//
// Every track in the board must start and end on the same line and the
// readouts must share a baseline. A column whose fixed content above or below
// the track differs by even a little silently changes that track's length —
// this has slipped past the eye three times (missing pan spacer, missing lamp
// spacer, and a keycap 1px shorter than its own spacer, which no one would
// ever spot in a screenshot).
//
// Run from the repo root after any modgui layout change:
//   node tools/modgui_measure.js
// Exits non-zero on a misalignment, so it can gate a change.
const fs = require('fs'), path = require('path');
const { chromium } = require('playwright-core');
const CHROMIUM_BIN = process.env.CHROMIUM_BIN || '/usr/bin/chromium-browser';
const REPO = '/home/pilal/dev/Pogged';
const JQUERY = fs.readFileSync(
  path.join(path.dirname(require.resolve('jquery')), 'jquery.min.js'), 'utf8');

const dir = path.join(REPO, 'pogged.lv2', 'modgui');
let html = fs.readFileSync(path.join(dir, 'icon-pogged.html'), 'utf8');
let css  = fs.readFileSync(path.join(dir, 'stylesheet-pogged.css'), 'utf8');
html = html.replace(/\{\{\{cns\}\}\}/g, '').split('<!-- Audio I/O jacks')[0];
css  = css.replace(/\{\{\{cns\}\}\}/g, '');

(async () => {
  const browser = await chromium.launch({ executablePath: CHROMIUM_BIN });
  const page = await browser.newPage({ viewport: { width: 700, height: 560 } });
  await page.setContent(`<!DOCTYPE html><html><head><meta charset="utf-8">
<style>html,body{margin:0;padding:0}#stage{position:relative}${css}</style></head>
<body><div id="stage">${html}</div><script>${JQUERY}<\/script></body></html>`);

  // The panel is overflow:hidden, so anything past its edge is SILENTLY cut —
  // the same mechanism that once made the I/O jacks invisible and the plugin
  // impossible to wire. Check every control against the panel's box.
  const spills = await page.evaluate(() => {
    const panel = document.querySelector('.pogged-panel').getBoundingClientRect();
    const out = [];
    document.querySelectorAll(
      '.pogged-panel [data-handle], .pogged-panel .pogged-knob-label,' +
      '.pogged-panel .pogged-fader-label, .pogged-panel .pogged-seg-label'
    ).forEach(el => {
      const r = el.getBoundingClientRect();
      if (r.width === 0 && r.height === 0) return;
      if (r.left < panel.left - 0.5 || r.right > panel.right + 0.5 ||
          r.top < panel.top - 0.5 || r.bottom > panel.bottom + 0.5)
        out.push((el.getAttribute('data-handle') || el.className) +
                 ` [${Math.round(r.left)},${Math.round(r.right)}]`);
    });
    return out;
  });

  // Every knob rides the same line, whatever sits under it. MASTER reads as
  // higher than the pans because its column carries no L..R marks and no
  // keycap, so the eye centres the cluster on a void — an illusion, but only
  // measuring can tell that from a real 7px slip, and the tool used to check
  // tracks and readouts while never looking at a knob at all.
  const knobs = await page.evaluate(() =>
    [...document.querySelectorAll('.pogged-board .pogged-knob')].map(k => ({
      sym: k.getAttribute('data-handle'),
      top: Math.round(k.getBoundingClientRect().top) })));

  const rows = await page.evaluate(() => {
    const out = [];
    document.querySelectorAll('.pogged-board .pogged-fader').forEach(f => {
      const t = f.querySelector('.pogged-fader-track').getBoundingClientRect();
      const v = f.querySelector('.pogged-fader-value').getBoundingClientRect();
      out.push({ sym: f.getAttribute('data-handle'),
                 x: Math.round(f.getBoundingClientRect().x),
                 top: Math.round(t.top), bot: Math.round(t.bottom),
                 val: Math.round(v.top) });
    });
    return out;
  });

  // A bracket must span exactly the columns it groups — FOCUS the voices (the
  // columns wearing a keycap), DRY the effects (the columns wearing a lamp).
  // Expected edges are read back off those columns rather than copied from the
  // stylesheet, so this disagrees with the CSS instead of echoing it. The
  // hardcoded pair it replaced predated the wider FILTER column: the DRY
  // bracket cut through the ATTACK lamp and overhung the right by 42px.
  const brackets = await page.evaluate(() => {
    const span = sel => {
      const cols = [...document.querySelectorAll('.pogged-board .pogged-fader')]
        .filter(f => f.querySelector(sel));
      const r = cols.map(c => c.getBoundingClientRect());
      return { l: Math.min(...r.map(b => b.left)), r: Math.max(...r.map(b => b.right)) };
    };
    const of = k => {
      const b = document.querySelector('.pogged-bracket-' + k).getBoundingClientRect();
      return { l: b.left, r: b.right };
    };
    return { focus: { want: span('.pogged-key'),  got: of('focus') },
             dry:   { want: span('.pogged-lamp'), got: of('dry')   } };
  });

  // The cap is centred on the value, so at 0 it hangs half its height BELOW the
  // track — straight through the readout unless the gap clears it. Every fader
  // shipped struck through and the eye let it pass on both panels.
  const covered = await page.evaluate(() => {
    const out = [];
    document.querySelectorAll('.pogged-board .pogged-fader').forEach(f => {
      f.style.setProperty('--value', '0');            // worst case: bottom of travel
      const cap = f.querySelector('.pogged-fader-cap').getBoundingClientRect();
      const val = f.querySelector('.pogged-fader-value').getBoundingClientRect();
      if (cap.bottom > val.top + 0.5)
        out.push(`${f.getAttribute('data-handle')} (cap ${cap.bottom.toFixed(0)} ` +
                 `> readout ${val.top.toFixed(0)})`);
      f.style.removeProperty('--value');
    });
    return out;
  });
  await browser.close();

  console.log('sym'.padEnd(14), 'x'.padStart(5), 'track_top'.padStart(10),
              'track_bot'.padStart(10), 'value_y'.padStart(8));
  for (const r of rows)
    console.log(r.sym.padEnd(14), String(r.x).padStart(5),
                String(r.top).padStart(10), String(r.bot).padStart(10),
                String(r.val).padStart(8));

  const uniq = k => [...new Set(rows.map(r => r[k]))];
  const bad = [];
  for (const k of ['top', 'bot', 'val'])
    if (uniq(k).length > 1) bad.push(`${k}: ${uniq(k).join(', ')}`);
  if (spills.length) bad.push('spills outside the panel: ' + spills.join(', '));
  if (covered.length) bad.push('cap covers the readout: ' + covered.join(', '));

  const knobTops = [...new Set(knobs.map(k => k.top))];
  console.log(`\n${knobs.length} knobs, top y = ${knobTops.join(', ')}` +
              (knobTops.length > 1 ? '  MISALIGNED' : '  ok'));
  if (knobTops.length > 1)
    bad.push('knobs off the line: ' +
             knobs.map(k => `${k.sym}@${k.top}`).join(', '));

  console.log('');
  for (const [k, b] of Object.entries(brackets)) {
    const off = Math.abs(b.got.l - b.want.l) > 1 || Math.abs(b.got.r - b.want.r) > 1;
    console.log(`bracket ${k.padEnd(5)} [${b.got.l.toFixed(0)},${b.got.r.toFixed(0)}]` +
                ` vs its columns [${b.want.l.toFixed(0)},${b.want.r.toFixed(0)}]` +
                (off ? '  MISALIGNED' : '  ok'));
    if (off) bad.push(`bracket ${k} is off its columns`);
  }

  // Pitch between neighbouring columns (the FILTER column is deliberately wider).
  const pitch = rows.slice(1).map((r, i) => r.x - rows[i].x);
  console.log('\ncolumn pitch:', pitch.join(', '));
  console.log(bad.length ? '\nMISALIGNED -> ' + bad.join(' | ')
                         : '\nall tracks and readouts share a line: ok');
  process.exit(bad.length ? 1 : 0);
})();
