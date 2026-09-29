// build.js — generate atlas.html: embed the vendored D3 bundle, the renderer source and the
// stylesheet, then inject workflow.json and improvement-proposals.json as data.
//
// Usage:
//   node build.js                          # production write
//   node build.js --check                  # non-mutating staleness check (exit 0 if fresh)
//   node build.js --test <alt-json>        # build alternate records into <alt>.html
//   node build.js --test <alt-json> --test-improvements <alt-improvements>
//
// atlas.html is the single self-contained offline artifact: it carries the embedded copies of
// vendor/d3.v7.9.0.min.js, src/atlas.css and src/atlas-render.js inside marked blocks, plus
//   <script id="wd" type="application/json">{}</script>
//   <script id="ipd" type="application/json">{}</script>
// Build replaces each block from its source file. If a source file is not present next to this
// script (a copied fixture carries only atlas.html), the embedded block is kept verbatim and the
// build says so — the output stays self-contained either way.
//
// Unknown flags are rejected with nonzero exit.
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const BASE = __dirname || process.cwd();

function readJSON(filepath) {
  try {
    const raw = fs.readFileSync(filepath, 'utf-8');
    const data = JSON.parse(raw);
    return { data, raw };
  } catch(e) {
    console.error('  ERROR: Cannot read/parse ' + filepath + ': ' + e.message);
    process.exit(1);
  }
}

// Replace everything between an opening marker and the next close marker.
function inject(payload, markerOpen, closeMarker, html, label) {
  const startIdx = html.indexOf(markerOpen);
  if (startIdx === -1) {
    console.error('  ERROR: ' + label + ' open marker not found in atlas.html: ' + markerOpen);
    process.exit(1);
  }
  const tagEnd = startIdx + markerOpen.length;
  const endIdx = html.indexOf(closeMarker, tagEnd);
  if (endIdx === -1) {
    console.error('  ERROR: ' + label + ' close marker not found after ' + markerOpen);
    process.exit(1);
  }
  return html.substring(0, tagEnd) + payload + html.substring(endIdx);
}

function currentBlock(html, markerOpen, closeMarker) {
  const startIdx = html.indexOf(markerOpen);
  if (startIdx === -1) return null;
  const tagEnd = startIdx + markerOpen.length;
  const endIdx = html.indexOf(closeMarker, tagEnd);
  if (endIdx === -1) return null;
  return html.substring(tagEnd, endIdx);
}

// Embed a source file, or keep the embedded copy when the source is not alongside this script.
function embed(html, markerOpen, closeMarker, srcPath, label) {
  if (fs.existsSync(srcPath)) {
    const body = fs.readFileSync(srcPath, 'utf-8');
    if (closeMarker === '</script>' && /<\/script/i.test(body)) {
      console.error('  ERROR: ' + label + ' contains a script close sequence and cannot be embedded: ' + srcPath);
      process.exit(1);
    }
    return { html: inject('\n' + body + '\n', markerOpen, closeMarker, html, label), source: path.relative(BASE, srcPath) };
  }
  if (currentBlock(html, markerOpen, closeMarker) === null) {
    console.error('  ERROR: ' + label + ' marker missing in atlas.html and source absent: ' + srcPath);
    process.exit(1);
  }
  return { html, source: '(embedded copy kept)' };
}

const WORKFLOW_PATH = path.join(BASE, 'workflow.json');
const IMPROV_PATH = path.join(BASE, 'improvement-proposals.json');
const ATLAS_PATH = path.join(BASE, 'atlas.html');
const VENDOR_D3 = path.join(BASE, 'vendor', 'd3.v7.9.0.min.js');
const SRC_CSS = path.join(BASE, 'src', 'atlas.css');
const SRC_RENDER = path.join(BASE, 'src', 'atlas-render.js');

// Parse args for test/check mode
const args = process.argv.slice(2);
let testJsonPath = null;
let testImprovementsPath = null;
let checkMode = false;

let i = 0;
while (i < args.length) {
  if (args[i] === '--check') {
    checkMode = true;
    i++;
  } else if (args[i] === '--test' && i + 1 < args.length) {
    testJsonPath = path.resolve(args[i + 1]);
    i += 2;
  } else if (args[i] === '--test-improvements' && i + 1 < args.length) {
    testImprovementsPath = path.resolve(args[i + 1]);
    i += 2;
  } else if (args[i].startsWith('--')) {
    console.error('  ERROR: Unknown flag ' + args[i]);
    process.exit(1);
  } else {
    console.error('  ERROR: Unexpected argument ' + args[i]);
    process.exit(1);
  }
}

if (testJsonPath) {
  // Node resolves __dirname through Windows junctions; compare real paths on both sides.
  const inputPath = fs.existsSync(testJsonPath) ? fs.realpathSync(testJsonPath) : path.resolve(testJsonPath);
  const relative = path.relative(fs.realpathSync(BASE), inputPath).toLowerCase();
  if (!/\.json$/i.test(testJsonPath) || (!relative.startsWith('..' + path.sep) && !path.isAbsolute(relative))) {
    console.error('  ERROR: --test requires an external .json fixture, never a project source path');
    process.exit(1);
  }
}
const jsonPath = testJsonPath || WORKFLOW_PATH;
const improvPath = testImprovementsPath || IMPROV_PATH;

const { data: workflowData } = readJSON(jsonPath);
const { data: improvData } = readJSON(improvPath);

let html = fs.readFileSync(ATLAS_PATH, 'utf-8');

// ---- embed dependency + source blocks (deterministic: same bytes in, same bytes out) ----
const STYLE_OPEN = '<style id="atlas-style">';
const D3_OPEN = '<script id="d3-embed">';
const RENDER_OPEN = '<script id="atlas-render">';
const SCR_CLOSE = '</script>';

let embedded = [];
let step = embed(html, STYLE_OPEN, '</style>', SRC_CSS, 'stylesheet');
html = step.html; embedded.push(['css', step.source]);
step = embed(html, D3_OPEN, SCR_CLOSE, VENDOR_D3, 'd3 bundle');
html = step.html; embedded.push(['d3', step.source]);
html = embed(html, '<script id="d3-license" type="text/plain">', SCR_CLOSE,
  path.join(BASE, 'vendor', 'LICENSE-d3-ISC.txt'), 'D3 license').html;
const pinnedD3 = 'f2094bbf6141b359722c4fe454eb6c4b0f0e42cc10cc7af921fc158fceb86539';
const bundledD3 = currentBlock(html, D3_OPEN, SCR_CLOSE);
// Embedding adds exactly one boundary newline on either side; don't normalize dependency bytes.
const actualD3 = crypto.createHash('sha256').update(bundledD3.slice(1, -1)).digest('hex');
if (actualD3 !== pinnedD3) {
  console.error('  ERROR: D3 bundle differs from the pinned official artifact');
  process.exit(1);
}
step = embed(html, RENDER_OPEN, SCR_CLOSE, SRC_RENDER, 'renderer');
html = step.html; embedded.push(['renderer', step.source]);

// ---- inject records ----
// Minify JSON — same data always produces the same string
const minWorkflow = JSON.stringify(workflowData);
const minImprov = JSON.stringify(improvData);

const wdOpen = '<script id="wd" type="application/json">';
const ipdOpen = '<script id="ipd" type="application/json">';

// Update the HTML <title> from the workflow project ID so init creates correct tab labels.
// Use a function replacement: project IDs are user data, so `$&`, `$`` and `$'` must not
// be interpreted as String.replace replacement tokens. Escape ampersand before markup chars.
const projectId = (workflowData.project && workflowData.project.id) || 'Project';
const safeId = projectId.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
html = html.replace(new RegExp('<title>[^<]*<\\/title>'), () => '<title>Project Atlas — ' + safeId + '<\/title>');

// Preserve the validator's exact serialization contract; reject unsafe script terminators
// before writing instead of silently altering canonical data or permitting markup injection.
if (/<\/script[\s/>]/i.test(minWorkflow) || /<\/script[\s/>]/i.test(minImprov)) {
  console.error('  ERROR: Record text contains an unsafe HTML script terminator');
  process.exit(1);
}
html = inject(minWorkflow, wdOpen, SCR_CLOSE, html, 'workflow data');
html = inject(minImprov, ipdOpen, SCR_CLOSE, html, 'proposals data');

function check(label, ok) {
  if (ok) console.log('  ✅ ' + label);
  else { console.error('  ❌ ' + label); process.exit(1); }
}

const outputPath = testJsonPath ? testJsonPath.replace(/\.json$/, '.html') : ATLAS_PATH;
const ctx = testJsonPath ? '(test mode)' : '(production)';

// --check mode: compare generated HTML with existing file, no write
if (checkMode) {
  const generatedHtml = html;
  let existingHtml = '';
  try { existingHtml = fs.readFileSync(outputPath, 'utf-8'); } catch(e) {}
  if (generatedHtml === existingHtml) {
    embedded.forEach(function (e) { if (String(e[1]).includes('embedded copy')) console.log('  note: ' + e[0] + ' source absent, kept embedded copy'); });
    console.log('  ✅ ' + outputPath + ' is up to date — no changes needed');
    process.exit(0);
  } else {
    console.error('  ❌ ' + outputPath + ' STALE — generated output differs from existing file');
    process.exit(1);
  }
}

fs.writeFileSync(outputPath, html, 'utf-8');
console.log('  ' + ctx + ' atlas.html updated: records from workflow.json + improvement-proposals.json, ' +
  'css from ' + embedded[0][1] + ', d3 from ' + embedded[1][1] + ', renderer from ' + embedded[2][1]);

// Verify embedded data
const embeddedMatch = html.match(/<script id="wd" type="application\/json">([\s\S]*?)<\/script>/);
if (embeddedMatch) {
  try {
    const parsed = JSON.parse(embeddedMatch[1]);
    const nodeArr = Array.isArray(parsed.nodes) ? parsed.nodes : Object.keys(parsed.nodes || {}).map(k => parsed.nodes[k]);
    const nodeCount = nodeArr.length;
    const edgeCount = parsed.edges && parsed.edges.requires ? parsed.edges.requires.length : 0;
    console.log('  ' + ctx + ' embedded: ' + nodeCount + ' nodes, ' + edgeCount + ' requires edges');
    // Stable-output check: re-parse embedded and compare serialization
    const reSerialized = JSON.stringify(parsed);
    check('Embedded workflow JSON round-trips identically', minWorkflow === reSerialized);
    const ipdMatch = html.match(/<script id="ipd" type="application\/json">([\s\S]*?)<\/script>/);
    if (ipdMatch) {
      const ipdParsed = JSON.parse(ipdMatch[1]);
      const propCount = ipdParsed.proposals ? ipdParsed.proposals.length : 0;
      console.log('  ' + ctx + ' improvement proposals: ' + propCount + ' items');
      check('Embedded proposals JSON round-trips identically', minImprov === JSON.stringify(ipdParsed));
    }
  } catch(e) {
    console.error('  ERROR: Embedded JSON parse failed: ' + e.message);
    process.exit(1);
  }
} else {
  console.error('  ERROR: Could not verify embedded data in output');
  process.exit(1);
}

// Verify the dependency block really is the pinned vendor bundle (provenance, not vibes)
const d3Block = currentBlock(html, D3_OPEN, SCR_CLOSE) || '';
if (fs.existsSync(VENDOR_D3)) {
  const vendor = fs.readFileSync(VENDOR_D3, 'utf-8');
  check('Embedded D3 block equals vendor/d3.v7.9.0.min.js', d3Block.trim() === vendor.trim());
  const digest = crypto.createHash('sha256').update(fs.readFileSync(VENDOR_D3)).digest('hex');
  console.log('  ' + ctx + ' d3 bundle sha256 ' + digest);
} else {
  check('Embedded D3 block is present and non-trivial', d3Block.length > 100000);
}
check('Renderer block is present', (currentBlock(html, RENDER_OPEN, SCR_CLOSE) || '').length > 5000);
check('Stylesheet block is present', (currentBlock(html, STYLE_OPEN, '</style>') || '').length > 2000);
check('No runtime network references in output', !/<(script|link)[^>]+(src|href)\s*=\s*["']?(https?:)?\/\//i.test(html));

console.log('  ' + ctx + ' Done.');
