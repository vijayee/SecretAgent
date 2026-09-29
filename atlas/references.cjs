// Permanent conversational references. Rendering NEVER allocates identities.
// node references.cjs = read-only allocation preview; --write explicitly persists to this
// project's workflow.json. Rebuild separately. Keep byId entries when retiring nodes.
'use strict';
const fs = require('node:fs');
const path = require('node:path');
const own = (o, k) => Object.prototype.hasOwnProperty.call(o, k);
const format = n => 'S' + String(n).padStart(3, '0');
const ids = w => Object.values(w.nodes || {}).map(n => n.id);
function referenceErrors(w, {coverage = true} = {}) {
  // Older portable projects may have no registry. Once present it must be complete.
  if (!own(w, 'references')) return [];
  const r = w.references, errors = [];
  if (!r || typeof r !== 'object' || Array.isArray(r) || !r.byId || typeof r.byId !== 'object' || Array.isArray(r.byId)) return ['references needs a byId registry'];
  if (!Number.isSafeInteger(r.next) || r.next < 1) errors.push('reference next must be a positive safe integer');
  const seen = new Set();
  for (const [id, ref] of Object.entries(r.byId)) {
    const n = typeof ref === 'string' && /^S\d{3,}$/.test(ref) ? Number(ref.slice(1)) : NaN;
    if (!id || !Number.isSafeInteger(n) || n < 1 || format(n) !== ref) errors.push('invalid reference: ' + id);
    if (seen.has(ref)) errors.push('duplicate/reserved reference: ' + ref);
    seen.add(ref);
    if (n >= r.next) errors.push('reference next must exceed every issued reference (including retired IDs)');
  }
  if (coverage) for (const id of ids(w)) if (!own(r.byId, id)) errors.push('missing reference: ' + id);
  return errors;
}
function allocateReferences(input) {
  const w = structuredClone(input);
  if (!own(w, 'references')) w.references = {next: 1, byId: {}};
  const errors = referenceErrors(w, {coverage: false});
  if (errors.length) throw new Error(errors.join('; '));
  for (const id of ids(w).filter(id => !own(w.references.byId, id)).sort()) {
    if (w.references.next >= Number.MAX_SAFE_INTEGER) throw new Error('reference namespace exhausted');
    Object.defineProperty(w.references.byId, id, {value: format(w.references.next++), enumerable: true, configurable: true, writable: true});
  }
  return w;
}
// Historical stability is a transition property, not provable from a lone edited file.
// Use this oracle against a saved baseline when reviewing record changes.
function assertStableReferences(before, after) {
  for (const [id, ref] of Object.entries(before.references?.byId || {})) {
    if (!own(after.references?.byId || {}, id) || after.references.byId[id] !== ref) throw new Error('reference changed or reservation removed: ' + id);
  }
  if (before.references && after.references.next < before.references.next) throw new Error('reference counter rewound');
}
module.exports = {referenceErrors, allocateReferences, assertStableReferences};
if (require.main === module) {
  if (process.argv.slice(2).some(a => a !== '--write')) throw new Error('Usage: node references.cjs [--write]');
  const file = path.join(__dirname, 'workflow.json');
  const before = JSON.parse(fs.readFileSync(file, 'utf8')), after = allocateReferences(before);
  assertStableReferences(before, after);
  if (process.argv.includes('--write')) fs.writeFileSync(file, JSON.stringify(after, null, 2) + '\n');
  console.log(JSON.stringify(after.references, null, 2));
}
