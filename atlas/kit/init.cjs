// Explicit SOURCE -> TARGET Atlas kit initialization/update. No git or global-state dependency.
'use strict';
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');

const KIT_VERSION = '1.2.2';
// Portable assets are the only files copied into a new or updated target.
// Source-project checks (freeze, protection, regression) are not consumer assets.
const ASSETS = [
  'build.js', 'validate.js', 'references.cjs',
  'src/atlas-render.js', 'src/atlas.css', 'vendor/d3.v7.9.0.min.js',
  'vendor/LICENSE-d3-ISC.txt', 'vendor/PROVENANCE.md', 'vendor/d3-registry-meta.json',
  'kit/README.md', 'kit/freeze-manifest.cjs', 'kit/review-protection.cjs',
  'kit/review-protection-profile.md', 'kit/lifecycle.cjs', 'kit/init.cjs', 'package.json'
];
const hash = bytes => crypto.createHash('sha256').update(bytes).digest('hex');
const root = () => path.resolve(__dirname, '..');
function real(p) { return fs.existsSync(p) ? fs.realpathSync(p) : path.resolve(p); }
function within(p, base) {
  const a = p.toLowerCase(), b = base.toLowerCase();
  return a === b || a.startsWith(b + path.sep.toLowerCase());
}
function assertNoSymlinks(dir) {
  if (!fs.existsSync(dir)) return;
  const stat = fs.lstatSync(dir);
  if (stat.isSymbolicLink()) throw new Error('symlink destination is unsafe: ' + dir);
  if (!stat.isDirectory()) throw new Error('target must be a directory: ' + dir);
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name);
    const child = fs.lstatSync(full);
    if (child.isSymbolicLink()) throw new Error('symlink destination is unsafe: ' + full);
    if (child.isDirectory()) assertNoSymlinks(full);
    else if (!child.isFile()) throw new Error('unreadable/non-regular target entry: ' + full);
  }
}
function assertAncestorsSafe(file, boundary) {
  let current = path.resolve(file);
  const base = path.resolve(boundary);
  const relative = path.relative(base, current);
  if (relative.startsWith('..') || path.isAbsolute(relative)) throw new Error('destination escapes target: ' + file);
  while (current !== base) {
    if (fs.existsSync(current) && fs.lstatSync(current).isSymbolicLink())
      throw new Error('symlink destination is unsafe: ' + current);
    current = path.dirname(current);
  }
}
function safeAssetPath(source, relative) {
  const base = real(source);
  const full = path.resolve(source, relative);
  if (!within(full, base)) throw new Error('asset escapes source: ' + relative);
  assertAncestorsSafe(full, base);
  if (!fs.existsSync(full) || !fs.lstatSync(full).isFile() || fs.lstatSync(full).isSymbolicLink())
    throw new Error('missing or unsafe source asset: ' + relative);
  return full;
}
function ensureSafeTarget(target, source, allowExisting) {
  const t = path.resolve(target), s = real(source);
  assertAncestorsSafe(t, path.parse(t).root);
  if (fs.existsSync(t) && fs.lstatSync(t).isSymbolicLink()) throw new Error('target symlink is unsafe');
  const tr = real(t);
  if (within(tr, s) || within(s, tr)) throw new Error('source and target overlap');
  if (fs.existsSync(t) && !fs.lstatSync(t).isDirectory()) throw new Error('target must be a directory');
  if (fs.existsSync(t) && !allowExisting && fs.readdirSync(t).length) throw new Error('target must be absent or empty');
  if (fs.existsSync(t) && allowExisting) assertNoSymlinks(t);
  return t;
}
function copyAtomic(src, dst, boundary) {
  assertAncestorsSafe(dst, boundary);
  fs.mkdirSync(path.dirname(dst), { recursive: true });
  assertAncestorsSafe(dst, boundary);
  const temp = dst + '.atlas-temp-' + crypto.randomBytes(16).toString('hex');
  if (fs.existsSync(temp)) throw new Error('temporary collision');
  try {
    fs.copyFileSync(src, temp);
    fs.renameSync(temp, dst);
  } finally {
    if (fs.existsSync(temp)) fs.rmSync(temp, { force: true });
  }
}
function assetMap(source) {
  const m = {};
  for (const file of ASSETS) {
    const p = safeAssetPath(source, file);
    m[file] = hash(fs.readFileSync(p));
  }
  return m;
}
function sourceVersion(source) {
  const versionPath = safeAssetPath(source, 'kit/VERSION');
  return fs.readFileSync(versionPath, 'utf8').trim();
}
function validateSource(source) {
  const version = sourceVersion(source);
  if (version !== KIT_VERSION) throw new Error('source kit/VERSION is ' + version + ', expected ' + KIT_VERSION);
  return assetMap(source);
}
function readInstalled(target) {
  const manifestPath = safeAssetPath(target, 'kit/.installed-assets.json');
  let installed;
  try { installed = JSON.parse(fs.readFileSync(manifestPath, 'utf8')); }
  catch (e) { throw new Error('installed asset manifest is invalid'); }
  if (!installed || installed.version !== KIT_VERSION || !installed.assets || typeof installed.assets !== 'object')
    throw new Error('installed asset manifest is stale or invalid');
  const expected = ASSETS.slice().sort();
  const actual = Object.keys(installed.assets).sort();
  if (JSON.stringify(actual) !== JSON.stringify(expected)) throw new Error('installed asset manifest has unexpected asset entries');
  for (const file of ASSETS) {
    if (typeof installed.assets[file] !== 'string' || !/^[a-f0-9]{64}$/.test(installed.assets[file]))
      throw new Error('installed asset manifest lacks valid digest: ' + file);
  }
  return installed;
}
function currentTargetAssets(target, installed) {
  const current = assetMap(target);
  for (const file of ASSETS) {
    if (current[file] !== installed.assets[file]) throw new Error('owned asset customized; refusing partial update: ' + file);
  }
  const version = fs.readFileSync(safeAssetPath(target, 'kit/VERSION'), 'utf8').trim();
  if (version !== installed.version) throw new Error('installed kit/VERSION does not match its asset manifest');
  return current;
}
function minimalRecords() {
  return {
    schema: 'rp036-atlas-v1', revision: 1, lastUpdated: '1970-01-01T00:00:00.000Z',
    project: { id: 'ATLAS-PROJECT', label: 'New Atlas project', purpose: 'A self-contained project map.', status: 'in-progress-design' },
    nodes: {
      'atlas-root': {
        id: 'atlas-root', label: 'New Atlas project', kind: 'prototype-container', intent: 'learning-delivery',
        status: 'in-progress-design', description: 'Replace this seed record with project-authoritative work.',
        contains: ['first-slice'], requires: [], completionRoute: 'Define and verify project slices.'
      },
      'first-slice': {
        id: 'first-slice', label: 'Define first slice', kind: 'leaf', intent: 'delivery', status: 'planned',
        description: 'Replace this seed slice with the first bounded deliverable.', contains: [], requires: [],
        route: {
          completionEvidence: ['A delivered artifact and independent review'],
          reviewPlanned: 'Create a native review record before completion.', next: [],
          onRejection: 'Keep planned and correct only within scope.'
        }
      }
    },
    edges: { contains: [{ from: 'atlas-root', to: 'first-slice' }], requires: [] },
    references: { next: 3, byId: { 'atlas-root': 'S001', 'first-slice': 'S002' } },
    lifecycle: {
      phases: ['plan', 'deliver', 'verify', 'review'],
      approvalPolicy: 'Owner authorization is separate from reviewer evidence.',
      completionAcceptance: { schema: 'atlas-completion-acceptance-v1', policy: 'fresh-review-required', legacy: { cutoffRevision: 0, nodeIds: [], baseline: {}, reason: 'No legacy records in a new project.' } }
    },
    reviews: [], unresolvedHistory: []
  };
}
function proposals() { return { schema: 'rp036-improvement-proposals-v1', revision: 1, proposals: [] }; }
function writeText(file, text, boundary) {
  assertAncestorsSafe(file, boundary);
  fs.writeFileSync(file, text);
}
function writeNew(source, target) {
  const files = validateSource(source);
  copyAtomic(safeAssetPath(source, 'atlas.html'), path.join(target, 'atlas.html'), target);
  for (const file of ASSETS) copyAtomic(path.join(source, file), path.join(target, file), target);
  writeText(path.join(target, 'workflow.json'), JSON.stringify(minimalRecords(), null, 2) + '\n', target);
  writeText(path.join(target, 'improvement-proposals.json'), JSON.stringify(proposals(), null, 2) + '\n', target);
  writeText(path.join(target, 'kit', 'VERSION'), KIT_VERSION + '\n', target);
  writeText(path.join(target, 'kit', '.installed-assets.json'), JSON.stringify({ version: KIT_VERSION, source: 'local Atlas kit', assets: files }, null, 2) + '\n', target);
  // Build the target atlas.html from seed workflow/proposals so it embeds clean records,
  // not the source project's history. Must succeed before returning.
  const { spawnSync } = require('node:child_process');
  const build = spawnSync(process.execPath, ['build.js'], { cwd: target, encoding: 'utf-8', timeout: 30000 });
  if (build.error || build.status !== 0) {
    const msg = build.error ? build.error.message : (build.stderr || 'build.js exited with code ' + build.status);
    throw new Error('init: build of target atlas.html failed after seed write: ' + msg);
  }
  return files;
}
function update(source, target) {
  if (!fs.existsSync(path.join(target, 'workflow.json')) || !fs.existsSync(path.join(target, 'kit', '.installed-assets.json')))
    throw new Error('target is not an initialized Atlas project');
  assertNoSymlinks(target);
  const installed = readInstalled(target);
  currentTargetAssets(target, installed);
  const sourceHashes = validateSource(source);
  for (const file of ASSETS) copyAtomic(path.join(source, file), path.join(target, file), target);
  writeText(path.join(target, 'kit', 'VERSION'), KIT_VERSION + '\n', target);
  writeText(path.join(target, 'kit', '.installed-assets.json'), JSON.stringify({ version: KIT_VERSION, source: 'local Atlas kit', assets: sourceHashes }, null, 2) + '\n', target);
  return sourceHashes;
}
function checkTarget(target, source) {
  ensureSafeTarget(target, source || root(), true);
  const installed = readInstalled(target);
  currentTargetAssets(target, installed);
  if (source) {
    const sourceHashes = validateSource(source);
    const stale = ASSETS.filter(file => sourceHashes[file] !== installed.assets[file]);
    if (stale.length) throw new Error('target is stale relative to source: ' + stale.join(', '));
  }
  return { ok: true, version: installed.version, assetCount: ASSETS.length, fresh: true };
}
function parseArgs(args) {
  const out = { check: false, update: false, source: null, target: null };
  const valueFlags = new Set(['--source', '--target']);
  for (let i = 0; i < args.length; i++) {
    const flag = args[i];
    if (flag === '--check') { if (out.check) throw new Error('duplicate --check'); out.check = true; continue; }
    if (flag === '--update') { if (out.update) throw new Error('duplicate --update'); out.update = true; continue; }
    if (valueFlags.has(flag)) {
      if (i + 1 >= args.length || args[i + 1].startsWith('--')) throw new Error(flag + ' requires a path');
      if (out[flag.slice(2)]) throw new Error('duplicate ' + flag);
      out[flag.slice(2)] = args[++i]; continue;
    }
    throw new Error('unknown argument: ' + flag);
  }
  return out;
}
function main() {
  try {
    const opts = parseArgs(process.argv.slice(2));
    if (opts.check) {
      if (opts.update) throw new Error('--check cannot combine with --update');
      if (opts.target) console.log(JSON.stringify(checkTarget(path.resolve(opts.target), opts.source ? path.resolve(opts.source) : null)));
      else console.log(JSON.stringify({ ok: true, version: KIT_VERSION, assetCount: ASSETS.length, source: path.resolve(opts.source || root()), assets: Object.keys(validateSource(path.resolve(opts.source || root()))).length }));
      return;
    }
    if (!opts.source || !opts.target) throw new Error('Both --source and --target are required');
    const source = real(opts.source);
    const target = ensureSafeTarget(opts.target, source, opts.update);
    if (opts.update) update(source, target);
    else {
      if (fs.existsSync(path.join(target, 'workflow.json'))) throw new Error('target already contains a project; init refuses clobber');
      fs.mkdirSync(target, { recursive: true });
      assertNoSymlinks(target);
      writeNew(source, target);
    }
    console.log('Atlas ' + (opts.update ? 'update' : 'init') + ' complete: ' + target + ' version ' + KIT_VERSION);
  } catch (e) { console.error('ERROR: ' + e.message); process.exitCode = 1; }
}
if (require.main === module) main();
module.exports = { KIT_VERSION, ASSETS, minimalRecords, writeNew, update, checkTarget, validateSource };
