// Render the Pogged modgui with the real HTML/CSS/JS and take the store
// screenshot + thumbnail at the default port values (fader positions are
// reproduced exactly as MOD-UI would).
//
// Requirements: node with `playwright` resolvable (a preinstalled Chromium
// works via PLAYWRIGHT_BROWSERS_PATH) and `jquery` installed somewhere
// require() can find it (`npm i jquery` at the repo root is enough).
//
// Run from the repo root after any modgui change:
//   node tools/modgui_screenshot.js
const fs = require('fs');
const path = require('path');
// playwright-core + the system Chromium (no browser download needed);
// override with CHROMIUM_BIN if yours lives elsewhere.
const { chromium } = require('playwright-core');
const CHROMIUM_BIN = process.env.CHROMIUM_BIN || '/usr/bin/chromium-browser';

const REPO = process.cwd();
// jquery v4 no longer exports dist paths: resolve the package root instead.
const JQUERY = fs.readFileSync(
  path.join(path.dirname(require.resolve('jquery')), 'jquery.min.js'), 'utf8');

// LV2 defaults (from pogged.ttl).
const PORTS = {
  dry_level: 1.0, sub1_level: 0.8, sub2_level: 0.0,
  up1_level: 0.8, up2_level: 0.0, up5_level: 0.0,
  detune_cents: 0.0, attack_ms: 0.0, attack_sens: 0.35,
  lp_cutoff: 20000, lp_q: 0.707, out_level: 1.0,
  pan_dry: 0.0, pan_sub1: 0.0, pan_sub2: 0.0,
  pan_up5: 0.0, pan_up1: 0.0, pan_up2: 0.0,
  spread: 0.0,
  filter_mode: 0, filter_env: 0.0, filter_env_a: 50, filter_env_d: 200,
  filter_sens: 0.5, range_mode: 0,
};

const PANEL_W = 760, PANEL_H = 380;

function buildPage() {
  const dir = path.join(REPO, 'pogged.lv2', 'modgui');
  let html = fs.readFileSync(path.join(dir, 'icon-pogged.html'), 'utf8');
  let css = fs.readFileSync(path.join(dir, 'stylesheet-pogged.css'), 'utf8');
  const gui = fs.readFileSync(path.join(dir, 'script-pogged.js'), 'utf8');

  // Strip the mustache bits: template class + the audio I/O jack blocks
  // (they are siblings of the panel and need MOD-UI core to render/position).
  // The panel closes itself before the jacks, so nothing to re-close here.
  html = html.replace(/\{\{\{cns\}\}\}/g, '');
  html = html.split('<!-- Audio I/O jacks')[0];
  css = css.replace(/\{\{\{cns\}\}\}/g, '');

  return `<!DOCTYPE html><html><head><meta charset="utf-8">
<style>
  html, body { margin: 0; padding: 0; background: transparent; }
  #stage { position: relative; }
  ${css}
</style></head>
<body><div id="stage">${html}</div>
<script>${JQUERY}<\/script>
<script>var poggedGui = ${gui};<\/script>
</body></html>`;
}

(async () => {
  const browser = await chromium.launch({ executablePath: CHROMIUM_BIN });
  const dir = path.join(REPO, 'pogged.lv2', 'modgui');
  const outputs = [];

  for (const { file, dsf } of [
    { file: path.join(dir, 'screenshot-pogged.png'), dsf: 1 },
    { file: path.join(dir, 'thumbnail-pogged.png'),  dsf: 0.35 },
  ]) {
    const ctx = await browser.newContext({
      viewport: { width: PANEL_W + 40, height: PANEL_H + 40 },
      deviceScaleFactor: dsf,
    });
    const page = await ctx.newPage();
    page.on('pageerror', e => console.error('pageerror:', e.message));
    page.on('console', m => { if (m.type() === 'error') console.error('console:', m.text()); });

    await page.setContent(buildPage(), { waitUntil: 'load' });

    // Every fader must get a value. A port missing from PORTS renders at the
    // CSS fallback instead of its default, which looks plausible and is easy
    // to miss by eye — so fail loudly rather than ship a wrong screenshot.
    const unset = await page.evaluate((known) =>
      [...document.querySelectorAll('.pogged-fader, .pogged-pan')]
        .map(f => f.dataset.handle)
        .filter(h => !known.includes(h)), Object.keys(PORTS));
    if (unset.length) {
      console.error('PORTS is missing defaults for:', unset.join(', '));
      process.exit(1);
    }

    await page.evaluate((ports) => {
      const icon = $('.pogged-panel');
      const portList = Object.keys(ports).map(s => ({ symbol: s, value: ports[s] }));
      // Drive the real modgui script exactly like MOD-UI does on 'start'.
      poggedGui({ type: 'start', icon: icon, data: {}, ports: portList },
                { set_port_value: function () {} });
      // MOD-UI core normally lights the bypass LED.
      icon.find('.pogged-logo-light').addClass('on');
    }, PORTS);

    await page.waitForTimeout(150);
    const buf = await page.locator('.pogged-panel').screenshot({ omitBackground: true });
    fs.writeFileSync(file, buf);
    outputs.push(file);
    await ctx.close();
  }

  await browser.close();
  console.log('rendered:', outputs.join(' '));
})();
