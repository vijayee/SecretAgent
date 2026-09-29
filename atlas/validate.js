// validate.js — structural validation of workflow.json + improvement-proposals.json + atlas.html parity
// Usage: node validate.js
//   Exits 0 on pass, 1 on any validation failure.
//   Reads canonical files only; never mutates or writes.
const fs = require('fs');
const path = require('path');
const { acceptanceFor, findingBlocks } = require('./kit/lifecycle.cjs');

let failed = false;
function check(ok, msg) {
  if (!ok) { failed = true; console.error('  ❌ ' + msg); }
  else console.log('  ✅ ' + msg);
}

// ---- hash discipline -------------------------------------------------------------
// A `*Sha256` field is a DECLARATION that an integrity check exists, so it is honoured on
// PRESENCE, never on truthiness. A present-but-falsey hash (`""`, null, false, 0, a truncated
// string) is a declared-but-broken check and must fail loudly — under `if (x.sha256)` such a
// record silently dropped the check and a tampered copy validated green (the 0106 gate finding,
// reproduced in parent-hash-red.json). Omitting the key entirely is the honest way to record a
// living document and always passes.
const SHA256_HEX = /^[a-f0-9]{64}$/;
const isDeclared = (obj, key) => Object.prototype.hasOwnProperty.call(obj, key);
const shown = v => (typeof v === 'string' ? JSON.stringify(v) : String(v));
// Returns true when a hash was declared. `actual` is the on-disk digest, or null when the file
// could not be read (existence is reported by its own check).
function verifyDeclaredHash(label, holder, key, actual) {
  if (!isDeclared(holder, key)) return false;
  const v = holder[key];
  const shapeOk = typeof v === 'string' && SHA256_HEX.test(v);
  check(shapeOk,
    `${label} declares ${key}, so it must be 64 lowercase hex — got ${shown(v)} ` +
    `(present-but-empty/null/false/0 is a broken declared check, not \"no hash\"; omit the key to record a living document)`);
  if (shapeOk && typeof actual === 'string') {
    check(v === actual, `${label} ${key} matches the file on disk (immutable copy intact)`);
  }
  return true;
}

// Known valid status values
const VALID_STATUSES = new Set([
  'completed', 'planned', 'in-progress-awaiting-review',
  'in-progress-design', 'in-progress', 'deferred', 'superseded'
]);

const projectDir = process.argv.includes('--project')
  ? path.resolve(process.argv[process.argv.indexOf('--project') + 1])
  : __dirname;
if (!process.argv.slice(2).every(a => a === '--check' || !a.startsWith('--') || a === '--project')) {
  console.error('  ❌ Unknown flag'); process.exit(1);
}
if (process.argv.slice(2).some(a => a.startsWith('--') && a !== '--project' && a !== '--check')) {
  console.error('  ❌ Unknown flag'); process.exit(1);
}
const jsonPath = path.join(projectDir, 'workflow.json');
const improvPath = path.join(projectDir, 'improvement-proposals.json');
const atlasPath = path.join(projectDir, 'atlas.html');

console.log('--- workflow.json validation ---');

let jsonData, improvData;
try {
  jsonData = JSON.parse(fs.readFileSync(jsonPath, 'utf-8'));
} catch(e) { console.error('  ❌ workflow.json: invalid JSON'); process.exit(1); }
try {
  improvData = JSON.parse(fs.readFileSync(improvPath, 'utf-8'));
} catch(e) { console.error('  ❌ improvement-proposals.json: invalid JSON'); process.exit(1); }

let rawSource = '';
try { rawSource = fs.readFileSync(jsonPath, 'utf-8'); } catch(e) {}
let rawAtlas = '';
try { rawAtlas = fs.readFileSync(atlasPath, 'utf-8'); } catch(e) {}

// 1. Valid schema
check(jsonData.schema === 'rp036-atlas-v1', 'Schema is rp036-atlas-v1');
check(jsonData.revision && jsonData.lastUpdated, 'Has revision and lastUpdated');
check(jsonData.project && jsonData.project.id && jsonData.project.label && jsonData.project.status,
  'Has project id, label, status');

// Additive permanent-reference checks; older registry-free portable records remain valid.
const referenceProblems = require('./references.cjs').referenceErrors(jsonData);
check(referenceProblems.length === 0, 'Permanent references: ' + (referenceProblems.join('; ') || 'unique, covered, reserved, monotonic'));

// 2. Nodes
const nodes = jsonData.nodes || {};
const nodeIds = Object.keys(nodes);
nodeIds.forEach(id => {
  const n = nodes[id];
  check(n.id === id, `Node "${id}" id field matches key`);
  check(n.label, `Node "${id}" has label`);
  check(n.kind, `Node "${id}" has kind`);
  check(['prototype-container','delivery-container','leaf','reference','grilling','checkpoint'].includes(n.kind),
    `Node "${id}" kind "${n.kind}" is valid`);
  check(n.intent, `Node "${id}" has intent`);
  check(n.status, `Node "${id}" has status`);
  check(VALID_STATUSES.has(n.status),
    `Node "${id}" status "${n.status}" is a known valid status`);
});

const uniqueIds = new Set(nodeIds);
check(uniqueIds.size === nodeIds.length, `Unique node IDs (${nodeIds.length})`);

// 3. Edges
const edges = jsonData.edges || {};
const requiresEdges = edges.requires || [];
const containsEdges = edges.contains || [];

requiresEdges.forEach(e => {
  check(nodes[e.from], `requires edge from "${e.from}" exists in nodes`);
  check(nodes[e.to], `requires edge to "${e.to}" exists in nodes`);
});
containsEdges.forEach(e => {
  check(nodes[e.from], `contains edge from "${e.from}" exists in nodes`);
  check(nodes[e.to], `contains edge to "${e.to}" exists in nodes`);
});

// 4a. Containment acyclic
const childrenOf = {};
containsEdges.forEach(e => {
  if (!childrenOf[e.from]) childrenOf[e.from] = [];
  childrenOf[e.from].push(e.to);
});
function hasCycle(id, visited, stack) {
  if (stack.has(id)) return true;
  if (visited.has(id)) return false;
  visited.add(id); stack.add(id);
  const kids = childrenOf[id] || [];
  for (const c of kids) { if (hasCycle(c, visited, stack)) return true; }
  stack.delete(id);
  return false;
}
// Find ALL graph roots and traverse EVERY node — disconnected components,
// self-contained cycles and mutual containment must all be caught.
function containsSelfLoop() {
  return containsEdges.some(e => e.from === e.to);
}
let containmentCyclic = containsSelfLoop();
nodeIds.forEach(rootId => {
  if (hasCycle(rootId, new Set(), new Set())) { containmentCyclic = true; }
});
check(!containmentCyclic, 'Containment is acyclic across every component (incl. disconnected/self-loops)');

// 4b. Requires edges acyclic (requires-cycle check)
const depsOf = {};
requiresEdges.forEach(e => {
  if (!depsOf[e.from]) depsOf[e.from] = [];
  depsOf[e.from].push(e.to);
});
function hasRequiresCycle(id, visited, stack) {
  if (stack.has(id)) return true;
  if (visited.has(id)) return false;
  visited.add(id); stack.add(id);
  const deps = depsOf[id] || [];
  for (const d of deps) { if (hasRequiresCycle(d, visited, stack)) return true; }
  stack.delete(id);
  return false;
}
let requiresCyclic = false;
nodeIds.forEach(id => {
  if (hasRequiresCycle(id, new Set(), new Set())) { requiresCyclic = true; }
});
check(!requiresCyclic, 'Requires edges are acyclic');

// 5. Node requires match edges
Object.keys(nodes).forEach(id => {
  const n = nodes[id];
  if (n.requires && n.requires.length) {
    n.requires.forEach(r => {
      check(requiresEdges.some(e => e.from === id && e.to === r),
        `Node "${id}" declares requires "${r}" and edge exists`);
    });
  }
});

// 6. No duplicate parent containment
const parentOf = {};
containsEdges.forEach(e => {
  if (!parentOf[e.to]) parentOf[e.to] = [];
  parentOf[e.to].push(e.from);
});
const multiParent = Object.entries(parentOf).filter(([k,v]) => new Set(v).size > 1);
check(multiParent.length === 0,
  'Each node in at most one container' +
  (multiParent.length ? ' (duplicates: ' + multiParent.map(([k,v]) => k).join(', ') + ')' : ''));

// 7. Learning dispositions
const validDispositions = ['project-fix', 'candidate', 'adopted', 'deferred', 'no-generalizable-learning', null, undefined];
Object.keys(nodes).forEach(id => {
  const n = nodes[id];
  if (n.learningDisposition) {
    check(validDispositions.includes(n.learningDisposition),
      `Node "${id}" learningDisposition "${n.learningDisposition}" is valid`);
  }
});

// 8. Review structure
Object.keys(nodes).forEach(id => {
  const n = nodes[id];
  if (n.review) {
    check(n.review.gate, `Node "${id}" review has gate`);
    check(n.review.reviewDir, `Node "${id}" review has reviewDir`);
  }
});

// 9. stableIds: only required when a node is actually awaiting review.
// A legitimate idle/completed project (zero running nodes) must pass.
const awaitingUnits = nodeIds.filter(id => nodes[id].status === 'in-progress-awaiting-review');
if (awaitingUnits.length === 0) {
  console.log('  ✅ No node awaiting review — idle/completed project is valid');
} else {
  awaitingUnits.forEach(id => {
    check(Array.isArray(nodes[id].stableIds) && nodes[id].stableIds.length > 0,
      `Awaiting-review unit "${id}" has stableIds`);
  });
}

// 10. Lifecycle section
check(jsonData.lifecycle && jsonData.lifecycle.phases && jsonData.lifecycle.approvalPolicy,
  'Has lifecycle phases and approval policy');
const approval = jsonData.lifecycle && jsonData.lifecycle.revisionApproval;
const requiresApprovalContract = Object.keys(nodes).some(id => ['authoring-capability-rule', 'continuous-eligibility', 'recovery-resumption', 'parent-integration', 'shared-rollout', 'end-to-end-measurement', 'revision-intake-contracts', 'design-review-decomposition', 'review-integration-successor', 'fresh-agent-continuation-proof'].includes(id));
check(!requiresApprovalContract || (approval && approval.scope === 'whole represented revision' && approval.state &&
  Array.isArray(approval.allowedDecisions) && approval.allowedDecisions.includes('approved') &&
  Array.isArray(approval.requiredBefore) && approval.rule),
  'Has record-visible whole-revision approval contract with explicit blocked branches');
if (approval) {
  check(approval.decision === null || approval.allowedDecisions.includes(approval.decision),
    'Whole-revision approval decision is null or an allowed decision');
  check(approval.state === 'pending-owner-decision' || approval.state === 'approved' || approval.state === 'revise' || approval.state === 'archive',
    'Whole-revision approval state is explicit');
  if (approval.state === 'approved') {
    check(approval.decision === 'approved' && approval.receipt && typeof approval.receipt.id === 'string' && approval.receipt.id.trim() &&
      typeof approval.receipt.sha256 === 'string' && SHA256_HEX.test(approval.receipt.sha256),
      'Approved whole-revision state carries a receipt-bound approval decision');
  }
}
const plannedCapabilityIds = ['authoring-capability-rule', 'continuous-eligibility', 'recovery-resumption',
  'parent-integration', 'shared-rollout', 'end-to-end-measurement', 'revision-intake-contracts',
  'design-review-decomposition', 'review-integration-successor', 'fresh-agent-continuation-proof'];
plannedCapabilityIds.forEach(id => {
  const n = nodes[id];
  if (!n) return;
  check(n.learningRequirement && /learningDisposition/.test(n.learningRequirement) && /learningDetail/.test(n.learningRequirement),
    `Planned capability "${id}" declares a learning completion requirement`);
  if (n.status === 'completed') {
    const explicitNoLearning = n.learningDisposition === 'no-generalizable-learning' && typeof n.learningDetail === 'string' && n.learningDetail.trim();
    check((typeof n.learningDisposition === 'string' && typeof n.learningDetail === 'string' && n.learningDetail.trim()) || explicitNoLearning,
      `Completed planned capability "${id}" records learning or explicit no-generalizable-learning`);
  }
  if (n.requiresRevisionApproval) {
    check(approval && approval.requiredBefore.includes(id),
      `Planned capability "${id}" is listed behind the whole-revision approval receipt`);
    check(n.route && n.route.completionEvidence.some(x => /approved whole-revision|whole-revision approval|approval receipt/i.test(x)),
      `Planned capability "${id}" completion evidence consumes the approval receipt`);
  }
});

// 11. Grilling briefs — only real owner-session nodes (kind "grilling") may carry
// one; delivery tasks like skill updates must NOT be conflated with a session.
nodeIds.forEach(id => {
  const n = nodes[id];
  if (n.kind === 'grilling') {
    check(n.grillingBrief, `Grilling node "${id}" carries a grillingBrief`);
  }
  if (n.grillingBrief && n.kind !== 'grilling') {
    check(false, `Node "${id}" is not kind "grilling" but carries a grillingBrief (owner session conflated with delivery task)`);
  }
  const gb = n.grillingBrief || null;
  if (gb) {
    check(gb.purpose, `Grilling brief on "${id}" has purpose`);
    check(gb.questions && gb.questions.length > 0, `Grilling brief on "${id}" has questions`);
    check(gb.evidence && gb.evidence.length > 0, `Grilling brief on "${id}" has evidence`);
    check(gb.affectedSlices && gb.unaffectedSlices, `Grilling brief on "${id}" has affected/unaffected slices`);
    check(gb.outcomeBranches && gb.outcomeBranches.length > 0, `Grilling brief on "${id}" has outcome branches`);
    check(gb.completionOutputs, `Grilling brief on "${id}" has completionOutputs`);
    const refsOk = (list, what) => (list || []).forEach(x => {
      const sid = (x && typeof x === 'object') ? x.sliceId : x;
      check(nodes[sid], `Grilling brief ${what} references existing node "${sid}"`);
      if (x && typeof x === 'object' && list === gb.affectedSlices)
        check(!!x.reason, `Affected slice "${sid}" states a reason`);
    });
    refsOk(gb.affectedSlices, 'affectedSlices');
    refsOk(gb.unaffectedSlices, 'unaffectedSlices');
  }
});

// 11b. Review registry — review records are DATA (WORK.reviews), linked per node.
const reviews = Array.isArray(jsonData.reviews) ? jsonData.reviews : [];
const reviewIds = new Set(reviews.map(r => r.id));
check(Array.isArray(reviews), 'workflow.json carries a review registry array (WORK.reviews); an empty registry is valid for a new project');
check(reviewIds.size === reviews.length,
  `Review IDs are unique (${reviews.length}) — duplicate IDs are an integrity collision, never last-write-wins`);
const reviewSequences = new Set();
reviews.forEach((r, reviewIndex) => {
  check(Number.isInteger(r.sequence) && r.sequence === reviewIndex + 1 && !reviewSequences.has(r.sequence),
    `Review "${r.id}" has immutable canonical sequence ${reviewIndex + 1}`);
  reviewSequences.add(r.sequence);
  check(r.id && r.run && r.rawVerdict && r.scope && r.disposition,
    `Review "${r.id}" has id/run/rawVerdict/scope/disposition`);
  check(r.parentDisposition, `Review "${r.id}" records parent disposition separately from raw verdict`);
  check(Array.isArray(r.findings), `Review "${r.id}" has a findings array`);
  (r.findings || []).forEach(f => check(f && f.severity && f.text,
    `Review "${r.id}" finding has severity + text`));
  if (r.pending) {
    check(!r.report, `Pending review "${r.id}" links no file (not a broken link)`);
    check(!isDeclared(r, 'reportSha256'),
      `Pending review "${r.id}" declares no reportSha256 (there is no copy to hash yet)`);
  } else {
    check(!!r.report, `Review "${r.id}" has a report path`);
    check(isDeclared(r, 'reportSha256'),
      `Review "${r.id}" records reportSha256 — a real gate/parent run's report copy always carries its integrity check`);
    if (r.report) {
      const abs = path.isAbsolute(r.report) ? r.report : path.join(projectDir, r.report);
      let ok = false, sha = null;
      try {
        const buf = fs.readFileSync(abs);
        ok = buf.length > 0;
        sha = require('crypto').createHash('sha256').update(buf).digest('hex');
      } catch (e) {}
      check(ok, `Review "${r.id}" report file exists and is non-empty (${r.report})`);
      verifyDeclaredHash(`Review "${r.id}"`, r, 'reportSha256', sha);
      check(!!r.sourceReport, `Review "${r.id}" records the original source path`);
    }
  }
  // Provenance — the bytes a reviewer READ and the bytes a parent later DELIVERED are different
  // things once any post-gate correction happens. Where the reviewed bytes were never hashed the
  // gap is declared, never smuggled in as an empty or falsey hash.
  if (r.provenance) {
    const pv = r.provenance;
    ['reviewedRevision', 'deliveredRevision', 'modelProvenanceObservedBy'].forEach(k =>
      check(typeof pv[k] === 'string' && pv[k].length > 0,
        `Review "${r.id}" provenance.${k} is a non-empty string`));
    check(typeof pv.reviewedEqualsDelivered === 'boolean',
      `Review "${r.id}" provenance.reviewedEqualsDelivered is a boolean`);
    const hashDeclared = verifyDeclaredHash(`Review "${r.id}" provenance`, pv, 'reviewedRevisionSha256', null);
    check(hashDeclared
        ? !isDeclared(pv, 'reviewedRevisionUnknown')
        : (typeof pv.reviewedRevisionUnknown === 'string' && pv.reviewedRevisionUnknown.length > 0),
      hashDeclared
        ? `Review "${r.id}" provenance records a reviewed-revision hash, so reviewedRevisionUnknown must be omitted`
        : `Review "${r.id}" provenance omits the reviewed-revision hash, so reviewedRevisionUnknown must state why those bytes are unrecoverable`);
    if (isDeclared(pv, 'modelProvenanceSource')) {
      check(typeof pv.modelProvenanceSource === 'string' && pv.modelProvenanceSource.length > 0,
        `Review "${r.id}" provenance.modelProvenanceSource is a non-empty string`);
    }
  }
});

// 11b1. Unresolved history — a fact about past work whose report cannot be recovered does NOT go
// into reviews[] (that registry holds real gate/parent runs with a real report, or one honest
// pending record). It goes here: stated, dated, with the evidence that was searched for.
const unresolved = jsonData.unresolvedHistory;
if (unresolved !== undefined) {
  check(Array.isArray(unresolved), 'unresolvedHistory is an array when recorded');
  const histIds = new Set();
  (Array.isArray(unresolved) ? unresolved : []).forEach(h => {
    if (!h || typeof h !== 'object') { check(false, 'unresolvedHistory entry is an object'); return; }
    check(typeof h.id === 'string' && /^[A-Za-z0-9][A-Za-z0-9._:-]*$/.test(h.id),
      `unresolvedHistory entry has a selector-safe id (got ${shown(h.id)})`);
    if (h.id) { check(!histIds.has(h.id), `unresolvedHistory id "${h.id}" is unique`); histIds.add(h.id); }
    ['topic', 'why', 'evidenceSearched'].forEach(k =>
      check(typeof h[k] === 'string' && h[k].length > 0,
        `unresolvedHistory "${h.id}" has a non-empty ${k}`));
    check(h.status === 'unresolved',
      `unresolvedHistory "${h.id}" status is the literal "unresolved" (got ${shown(h.status)})`);
    ['rawVerdict', 'report', 'reportSha256', 'disposition', 'pending', 'tier']
      .forEach(k => check(!isDeclared(h, k),
        `unresolvedHistory "${h.id}" must not carry reviews[] field "${k}" — unresolved history is neither a verdict nor a pending gate`));
    (h.relatedNodes || []).forEach(nid =>
      check(!!nodes[nid], `unresolvedHistory "${h.id}" relatedNodes -> "${nid}" exists in nodes`));
    (h.relatedReviewIds || []).forEach(rid =>
      check(reviewIds.has(rid), `unresolvedHistory "${h.id}" relatedReviewIds -> "${rid}" exists in the review registry`));
  });
}
nodeIds.forEach(id => {
  const n = nodes[id];
  check(!n.review, `Node "${id}" has no stale embedded review object (registry is data) `);
  (n.reviewRunIds || []).forEach(rid =>
    check(reviewIds.has(rid), `Node "${id}" reviewRunIds → "${rid}" exists in registry`));
});

// 11b2. Artifact links — generic node.artifacts[] field (evidence documents and immutable
// project-local report copies). An artifact that cannot be opened is a broken claim, so ids,
// path safety, file existence and any recorded hash are all checked here.
const SAFE_ARTIFACT_PATH = /^[A-Za-z0-9][A-Za-z0-9._ -]*(\/[A-Za-z0-9][A-Za-z0-9._ -]*)*$/;
function artifactPathIsSafe(p) {
  if (typeof p !== 'string' || p.length === 0) return false;
  if (!SAFE_ARTIFACT_PATH.test(p)) return false;          // no scheme, no leading/duplicate/dot segments
  if (p.split('/').some(s => s === '.' || s === '..')) return false;
  const abs = path.resolve(projectDir, p);
  return abs.startsWith(path.resolve(projectDir) + path.sep);
}
const artifactIds = new Set();
nodeIds.forEach(id => {
  const list = (nodes[id] || {}).artifacts;
  if (list === undefined) return;
  check(Array.isArray(list), `Node "${id}" artifacts is an array`);
  list.forEach(a => {
    if (!a || typeof a !== 'object') { check(false, `Node "${id}" artifact entry is an object`); return; }
    check(typeof a.id === 'string' && /^[A-Za-z0-9][A-Za-z0-9._:-]*$/.test(a.id),
      `Artifact on "${id}" has a selector-safe id`);
    if (a.id) {
      check(!artifactIds.has(a.id), `Artifact id "${a.id}" is unique project-wide`);
      artifactIds.add(a.id);
    }
    check(typeof a.label === 'string' && a.label.length > 0, `Artifact "${a.id}" has a label`);
    check(artifactPathIsSafe(a.path),
      `Artifact "${a.id}" path is project-relative and safe (${typeof a.path === 'string' ? a.path : JSON.stringify(a.path)})`);
    let actual = null;
    if (artifactPathIsSafe(a.path)) {
      const abs = path.resolve(projectDir, a.path);   // never opened unless it is inside the project
      let buf = null;
      try { buf = fs.readFileSync(abs); } catch (e) {}
      check(!!buf && buf.length > 0, `Artifact "${a.id}" target exists and is non-empty (${a.path})`);
      if (buf && buf.length > 0) actual = require('crypto').createHash('sha256').update(buf).digest('hex');
    }
    // Presence, not truthiness: a declared-but-falsey hash fails even beside an unsafe path.
    verifyDeclaredHash(`Artifact "${a.id}"`, a, 'sha256', actual);
    // An immutability claim is a separate claim and must be a boolean when made.
    if (isDeclared(a, 'immutable')) {
      check(typeof a.immutable === 'boolean',
        `Artifact "${a.id}" immutable is a boolean when recorded (got ${shown(a.immutable)})`);
    }
    ['source', 'kind'].forEach(field => {
      if (a[field] !== undefined) check(typeof a[field] === 'string' && a[field].length > 0,
        `Artifact "${a.id}" ${field} is a non-empty string when recorded`);
    });
  });
});

// 11c. Completion routes — executable slices carry concrete, checkable fields.
nodeIds.forEach(id => {
  const n = nodes[id];
  if (n.kind === 'leaf' && n.intent === 'delivery' && n.status === 'planned') {
    check(!!n.route && Array.isArray(n.route.completionEvidence) && n.route.completionEvidence.length >= 1,
      `Planned leaf "${id}" route has concrete completionEvidence checks`);
    check(n.route && Array.isArray(n.route.next), `Planned leaf "${id}" route declares next node IDs`);
    check(n.route && !!n.route.onRejection, `Planned leaf "${id}" route has a rejection/needs-discussion path`);
    check(n.route && !!n.route.reviewPlanned, `Planned leaf "${id}" route names its review`);
  }
  if (n.route) {
    (n.route.next || []).forEach(nid => check(!!nodes[nid], `Route on "${id}" next → "${nid}" exists`));
  }
  if (n.kind === 'leaf' && n.status === 'completed') {
    check(!!n.completedOutcome, `Completed leaf "${id}" shows its actual completed outcome`);
  }
  // route/registry references from awaiting units must resolve
  (n.stableIds || []).forEach(sid => check(typeof sid === 'string' && sid.length > 0,
    `Node "${id}" stable ID non-empty`));
});
// 11d. Completion acceptance contract. New records require a reviewer-authorized,
// parent-separated acceptance; literal PASS/length alone is never enough. A narrow
// explicit legacy import list preserves pre-hardening history without exempting future records.
const completionPolicy = jsonData.lifecycle && jsonData.lifecycle.completionAcceptance || {};
Object.keys(nodes).filter(id => nodes[id].kind === 'leaf' && nodes[id].status === 'completed').forEach(id => {
  const n = nodes[id];
  const result = acceptanceFor(n, reviews, completionPolicy);
  check(result.ok, `Completed leaf "${id}" has explicit completion acceptance (${result.reason})`);
  if (n.completedOutcome) check(n.completedOutcome.length >= 20,
    `Completed leaf "${id}" completedOutcome is substantive (schema cannot prove prose truth)`);
});

const childSet = new Set();
containsEdges.forEach(e => { childSet.add(e.to); });

console.log('\n--- improvement-proposals.json validation ---');

check(improvData.schema === 'rp036-improvement-proposals-v1', 'Schema is rp036-improvement-proposals-v1');
const proposals = improvData.proposals || [];
const proposalIds = proposals.map(p => p.id);
const uniquePIds = new Set(proposalIds);
check(uniquePIds.size === proposals.length, `Unique proposal IDs (${proposals.length})`);

proposals.forEach(p => {
  check(p.id && p.title && p.status && p.category && p.sourceSliceId,
    `Proposal "${p.id}" has required fields`);
  check(nodes[p.sourceSliceId], `Proposal "${p.id}" sourceSliceId "${p.sourceSliceId}" exists in workflow`);
  check(['candidate','rejected-local','adopted','local','needs-validation'].includes(p.status),
    `Proposal "${p.id}" status "${p.status}" is valid`);
  // Candidate must have whyNonObvious and whyHighLeverage
  if (p.status === 'candidate') {
    check(p.whyNonObvious, `Candidate "${p.id}" has whyNonObvious`);
    check(p.whyHighLeverage, `Candidate "${p.id}" has whyHighLeverage`);
    check(p.minimalChange, `Candidate "${p.id}" has minimalChange`);
    check(p.failureReplayAndControl, `Candidate "${p.id}" has failureReplayAndControl`);
  }
  // Rejected-local must have whyLocal
  if (p.status === 'rejected-local') {
    check(p.whyLocal, `Local fix "${p.id}" has whyLocal`);
  }
  // Non-blocking: pending proposals don't block
  if (p.status === 'candidate' || p.status === 'needs-validation') {
    check(p.validationState && ['needs-validation','needs-review','validated','adopted'].includes(p.validationState),
      `Proposal "${p.id}" has valid validationState`);
  }
  // Adopted proposals must have ownerReview (non-null)
  if (p.status === 'adopted') {
    check(p.ownerReview !== null && p.ownerReview !== undefined,
      `Proposal "${p.id}" status is adopted but ownerReview is null`);
    check(p.validationState === 'adopted',
      `Proposal "${p.id}" status is adopted but validationState is not 'adopted'`);
  }
});

console.log('\n--- atlas.html parity check ---');

try {
  const atlas = fs.readFileSync(atlasPath, 'utf-8');
  const wdMatch = atlas.match(/<script id="wd" type="application\/json">([\s\S]*?)<\/script>/);
  check(wdMatch !== null, 'atlas.html has embedded workflow script tag');
  if (wdMatch) {
    try {
      const embed = JSON.parse(wdMatch[1]);
      const embedNodes = embed.nodes || {};
      const embedIds = Object.keys(embedNodes).filter(k => k !== '0').length;
      check(embedIds >= 1, `Embedded has at least one node (${embedIds})`);
      // Schema match
      check(embed.schema === jsonData.schema,
        `Embedded schema "${embed.schema}" matches source`);
    } catch(e) {
      check(false, 'Embedded workflow JSON is valid: ' + e.message);
    }
  }

  const ipdMatch = atlas.match(/<script id="ipd" type="application\/json">([\s\S]*?)<\/script>/);
  check(ipdMatch !== null, 'atlas.html has embedded proposals script tag');
  if (ipdMatch) {
    try {
      const embed = JSON.parse(ipdMatch[1]);
      check(embed.schema === improvData.schema,
        `Embedded proposal schema matches source`);
    } catch(e) {
      check(false, 'Embedded proposals JSON is valid: ' + e.message);
    }
  }
} catch(e) {
  check(false, 'atlas.html can be read: ' + e.message);
}

// Stale-output check: embedded workflow data must exactly match source data
// Uses stable JSON.stringify (no whitespace variation) to compare
const sourceWorkflowStr = JSON.stringify(jsonData);
const sourceImprovStr = JSON.stringify(improvData);
try {
  const atlas = fs.readFileSync(atlasPath, 'utf-8');
  const wdMatch = atlas.match(/<script id="wd" type="application\/json">([\s\S]*?)<\/script>/);
  if (wdMatch) {
    try {
      const embedStr = wdMatch[1].trim();
      check(embedStr === sourceWorkflowStr,
        'Embedded workflow JSON matches source data (stale-output check)');
    } catch(e) {
      check(false, 'Embedded workflow JSON stale check: ' + e.message);
    }
  }
  const ipdMatch = atlas.match(/<script id="ipd" type="application\/json">([\s\S]*?)<\/script>/);
  if (ipdMatch) {
    try {
      const embedStr = ipdMatch[1].trim();
      check(embedStr === sourceImprovStr,
        'Embedded proposals JSON matches source data (stale-output check)');
    } catch(e) {
      check(false, 'Embedded proposals JSON stale check: ' + e.message);
    }
  }
} catch(e) {
  check(false, 'Stale-output check: ' + e.message);
}

// 12. Required fields on all non-root nodes
Object.keys(nodes).forEach(id => {
  const n = nodes[id];
  if (id === jsonData.project.id || id === 'rp036-root' || !n) return;
  if (n.kind === 'leaf') {
    check(n.intent, `Leaf "${id}" has intent`);
    const okRoute = n.route || n.completedOutcome || n.completionRoute ||
      (n.intent === 'reference') || n.status === 'deferred';
    check(!!okRoute, `Leaf "${id}" has a truthful route record (structured route, completed outcome, or explicit reference/deferral)`);
  }
});

// 13. Proposal slice references resolve
proposals.forEach(p => {
  (p.applicableFutureSliceIds || []).forEach(sid =>
    check(!!nodes[sid], `Proposal "${p.id}" applicableFutureSliceIds → "${sid}" exists`));
});

console.log('');
if (failed) {
  console.log('❌ VALIDATION FAILED — examine errors above');
  process.exit(1);
} else {
  console.log('✅ ALL VALIDATIONS PASSED');
}