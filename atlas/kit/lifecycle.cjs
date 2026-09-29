// Shared completion lifecycle rules. This is a validator, not an acceptance authority.
'use strict';
const crypto = require('node:crypto');
const ACCEPTED_RAW = new Set(['PASS', 'PASS_WITH_NOTES']);
const LEGACY_REVIEW = /^legacy-imported$/;
function findingBlocks(f) { return f && (f.severity === 'blocker' || f.severity === 'major') && f.resolution !== 'addressed'; }
function canonical(value) {
  if (Array.isArray(value)) return '[' + value.map(canonical).join(',') + ']';
  if (value && typeof value === 'object') return '{' + Object.keys(value).sort().map(k => JSON.stringify(k) + ':' + canonical(value[k])).join(',') + '}';
  return JSON.stringify(value);
}
function nodeFingerprint(node) {
  const copy = { ...node };
  delete copy.acceptancePolicy;
  return crypto.createHash('sha256').update(canonical(copy)).digest('hex');
}
// A reviewer digest covers the delivered node content, but not the review link that
// records the reviewer. Excluding the link avoids a circular digest while still binding
// outcome, requirements, dependencies and other node fields to the review evidence.
function reviewedNodeFingerprint(node) {
  const copy = { ...node };
  delete copy.acceptancePolicy;
  delete copy.reviewRunIds;
  delete copy.legacyCompletion;
  delete copy.legacyImportedRevision;
  return crypto.createHash('sha256').update(canonical(copy)).digest('hex');
}
function sequenceOf(review, fallback) { return Number.isInteger(review && review.sequence) ? review.sequence : fallback + 1; }
function reviewInvalid(review) {
  return review && (review.pending || review.disposition === 'rejected' || review.disposition === 'withheld' || review.rawVerdict === 'FAIL' || (review.findings || []).some(findingBlocks));
}
function linkedReviews(node, reviews) {
  const ids = new Set(node.reviewRunIds || []);
  return reviews.filter(r => r && ids.has(r.id)).map((r, i) => ({ review: r, sequence: sequenceOf(r, i) })).sort((a, b) => a.sequence - b.sequence);
}
function acceptanceFor(node, reviews, policy = {}) {
  if (!node || node.kind !== 'leaf' || node.status !== 'completed') return { ok: true, reason: 'not-completed-leaf' };
  const linked = linkedReviews(node, reviews);
  const legacyPolicy = policy.legacy || {};
  const baseline = legacyPolicy.baseline && legacyPolicy.baseline[node.id];
  const legacyTracked = Array.isArray(legacyPolicy.nodeIds) && legacyPolicy.nodeIds.includes(node.id) &&
    LEGACY_REVIEW.test(node.acceptancePolicy || '') && !!baseline;
  // Imported records retain their old reports only while their frozen content and
  // pre-import review ceiling are unchanged. Once changed, old reports are history,
  // never fresh evidence for the changed node.
  const fresh = linked.filter((entry, index) => {
    const r = entry.review;
    const afterImportBoundary = !legacyTracked || entry.sequence > baseline.reviewSequence;
    return afterImportBoundary && !r.pending && r.tier !== 'parent' && Array.isArray(r.nodeIds) && r.nodeIds.includes(node.id) &&
      ACCEPTED_RAW.has(r.rawVerdict) && r.disposition === 'accepted' && !(r.findings || []).some(findingBlocks) &&
      typeof r.parentDisposition === 'string' && r.parentDisposition.trim() &&
      r.reviewedNodeSha256 === reviewedNodeFingerprint(node) &&
      !linked.slice(index + 1).some(next => reviewInvalid(next.review));
  });
  if (fresh.length) return { ok: true, reason: 'fresh-accepted-review', reviewIds: fresh.map(entry => entry.review.id) };

  const baselineAccepted = legacyTracked && linked.some(entry => {
    const r = entry.review;
    return entry.sequence <= baseline.reviewSequence && r.tier !== 'parent' &&
      ACCEPTED_RAW.has(r.rawVerdict) && ['accepted', 'addressed'].includes(r.disposition) &&
      !(r.findings || []).some(findingBlocks) && typeof r.parentDisposition === 'string' && r.parentDisposition.trim();
  });
  const legacy = legacyTracked && baselineAccepted &&
    node.status === 'completed' && node.legacyCompletion === baseline.completionState &&
    node.legacyImportedRevision === legacyPolicy.cutoffRevision && baseline.importedRevision === legacyPolicy.cutoffRevision &&
    nodeFingerprint(node) === baseline.nodeSha256 &&
    linked.every(entry => entry.sequence <= baseline.reviewSequence);
  if (legacy) return { ok: true, reason: 'explicit-legacy-import', reviewIds: linked.map(entry => entry.review.id) };

  const reasons = [];
  if (!linked.length) reasons.push('no-linked-review');
  if (linked.some(entry => entry.review.pending)) reasons.push('pending-review');
  if (linked.some(entry => reviewInvalid(entry.review))) reasons.push('rejected-or-failed-review');
  if (linked.some(entry => entry.review.rawVerdict && !ACCEPTED_RAW.has(entry.review.rawVerdict) && entry.review.tier !== 'parent')) reasons.push('unknown-or-withheld-verdict');
  if (linked.every(entry => entry.review.tier === 'parent')) reasons.push('parent-is-not-reviewer-authority');
  if (baseline && legacyPolicy.nodeIds && legacyPolicy.nodeIds.includes(node.id)) reasons.push('legacy-baseline-or-transition-changed');
  return { ok: false, reason: reasons.join(',') || 'no-explicit-accepted-review' };
}
module.exports = { ACCEPTED_RAW, acceptanceFor, findingBlocks, nodeFingerprint, reviewedNodeFingerprint, reviewInvalid, sequenceOf };
