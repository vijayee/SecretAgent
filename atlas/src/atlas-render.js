/* src/atlas-render.js — Project Atlas diagram renderer (D3).
   Embedded verbatim into atlas.html by build.js (<script id="atlas-render">). Edit here, not there.

   Two views of ONE record set:
     Map  = containment. Nested, content-sized frames; enclosure only, never an arrow.
     DAG  = prerequisites. Every edges.requires is drawn as prerequisite → dependent: the
            prerequisite (`to`) sits above, the dependent (`from`) below, and the arrow head
            lands on the dependent's top edge. Layers run top to bottom so the graph matches a
            landscape canvas; edges from "hub" prerequisites (needed by many slices) are drawn
            faint and named on the dependent card instead of competing with the real chains.

   D3 (vendored, see vendor/PROVENANCE.md) does the SVG data rendering, pan/zoom,
   and the status symbol shapes. D3 ships no graph layerer, so the layered
   placement and the orthogonal routing below are written here: deterministic
   (fixed sweeps, tie-breaks by index then id, no randomness, no force simulation), with
   no ELK / d3-dag / other library.

   Coordinate model: #svg carries a viewBox equal to its own pixel box, so one user unit is
   one CSS pixel at identity transform. d3-zoom owns the transform on <g class="viewport">.
   The diagram's content bounds are published as data-canvas-w/h for geometry tests. */
(function () {
  'use strict';
  var d3 = window.d3;
  if (!d3 || !d3.select || !d3.zoom) {
    var warn = document.getElementById('atlas-info');
    if (warn) warn.textContent = 'D3 bundle missing — run node build.js to embed vendor/d3 into atlas.html.';
    return;
  }

  var WORK = JSON.parse(document.getElementById('wd').textContent);
  var IMPROV = JSON.parse(document.getElementById('ipd').textContent);

  /* ------------------------------------------------------------------ records */
  var NODES = {};
  var EDGES = { requires: [], contains: [] };
  (function initData() {
    if (!WORK.nodes) return;
    var arr = Array.isArray(WORK.nodes) ? WORK.nodes : Object.keys(WORK.nodes).map(function (k) { return WORK.nodes[k]; });
    arr.forEach(function (n) { if (n && n.id) NODES[n.id] = n; });
    if (WORK.edges) {
      EDGES.requires = (WORK.edges.requires || []).filter(function (e) { return e && NODES[e.from] && NODES[e.to]; });
      EDGES.contains = (WORK.edges.contains || []).filter(function (e) { return e && NODES[e.from] && NODES[e.to]; });
    }
  })();

  // The node's declared `contains` order is the product-facing containment order. Edges remain
  // the compatibility fallback for older records, but they must not silently reorder a chart.
  var KIDS = {};
  Object.keys(NODES).forEach(function (id) {
    var declared = NODES[id] && NODES[id].contains;
    if (Array.isArray(declared) && declared.length) {
      KIDS[id] = declared.filter(function (child, i, list) {
        return NODES[child] && list.indexOf(child) === i;
      });
    }
  });
  EDGES.contains.forEach(function (e) {
    if (!KIDS[e.from] || !KIDS[e.from].length) (KIDS[e.from] = []).push(e.to);
    else if (KIDS[e.from].indexOf(e.to) === -1) KIDS[e.from].push(e.to);
  });
  var CONTAINS = {};                                  // childId -> [parentId]
  Object.keys(KIDS).forEach(function (parent) {
    KIDS[parent].forEach(function (child) { if (!CONTAINS[child]) CONTAINS[child] = []; CONTAINS[child].push(parent); });
  });
  function childIds(id) { return (KIDS[id] || []).slice(); }
  function isContainer(id) { return childIds(id).length > 0; }
  function descendants(id) {                          // containment is acyclic (validate.js); guarded anyway
    var seen = {}, out = [];
    (function walk(cur) {
      childIds(cur).forEach(function (c) { if (!seen[c] && NODES[c]) { seen[c] = 1; out.push(c); walk(c); } });
    })(id);
    return out;
  }
  var CHILD_SET = {};
  Object.keys(KIDS).forEach(function (parent) { KIDS[parent].forEach(function (child) { CHILD_SET[child] = 1; }); });
  var declaredOrder = [];
  var declaredSeen = {};
  function appendDeclared(id) {
    if (declaredSeen[id] || !NODES[id]) return;
    declaredSeen[id] = 1; declaredOrder.push(id);
    childIds(id).forEach(appendDeclared);
  }
  var sourceIds = Object.keys(NODES);
  sourceIds.filter(function (id) { return !CHILD_SET[id]; }).forEach(appendDeclared);
  sourceIds.forEach(appendDeclared);
  var ALL_IDS = declaredOrder;
  var ROOT = ALL_IDS.filter(function (id) { return !CHILD_SET[id]; })[0] || ALL_IDS[0] || 'root';
  var ROOT_DESC = descendants(ROOT);
  // Anything containment never reaches still has to be painted, so it hangs off the plate.
  var ORPHANS = ALL_IDS.filter(function (id) { return id !== ROOT && ROOT_DESC.indexOf(id) === -1; });

  // Prerequisite fan-in. A "hub" is a prerequisite that at least a third of the slices need:
  // its edges stay on the record and in the DOM, but are painted faint and named on the card.
  var REQ_BY = {};                                     // prerequisiteId -> [dependentId]
  EDGES.requires.forEach(function (e) { (REQ_BY[e.to] = REQ_BY[e.to] || []).push(e.from); });
  var HUB_MIN = Math.max(4, Math.ceil(ALL_IDS.length / 3));
  var HUBS = {};
  Object.keys(REQ_BY).forEach(function (id) { if (REQ_BY[id].length >= HUB_MIN) HUBS[id] = REQ_BY[id].length; });
  function isHubEdge(e) { return !!HUBS[e.to]; }

  // Rollup: how much of the work a container encloses is complete. Containers carry a recorded
  // status for validation, but readers get the count, never a single word that contradicts it.
  function rollup(id) {
    var kids = descendants(id).filter(function (c) { return !isContainer(c); });
    var done = kids.filter(function (c) { return NODES[c].status === 'completed'; }).length;
    return { done: done, total: kids.length, word: kids.length ? done + ' of ' + kids.length + ' complete' : 'no slices' };
  }

  function esc(s) { if (s === null || s === undefined) return ''; return String(s).replace(/[<>&]/g, function (c) { return { '<': '&lt;', '>': '&gt;', '&': '&amp;' }[c]; }); }
  function escAttr(s) { if (s === null || s === undefined) return ''; return String(s).replace(/[<>&"]/g, function (c) { return { '<': '&lt;', '>': '&gt;', '&': '&amp;', '"': '&quot;' }[c]; }); }

  // Artifact links are pure data (node.artifacts). Only a project-relative path with no URI
  // scheme, no leading separator, no backslash and no '.'/'..' segment may become a clickable
  // href; anything else renders as inert text. validate.js enforces the same rule on records.
  function safeArtifactPath(p) {
    if (typeof p !== 'string' || p.length === 0) return false;
    if (/^[A-Za-z][A-Za-z0-9+.-]*:/.test(p)) return false;
    if (p.charAt(0) === '/' || p.charAt(0) === '\\') return false;
    if (p.indexOf('\\') !== -1) return false;
    var segs = p.split('/');
    for (var i = 0; i < segs.length; i++) { if (segs[i] === '' || segs[i] === '.' || segs[i] === '..') return false; }
    return true;
  }

  /* ------------------------------------------------------------ visual tokens */
  var SANS = '"Segoe UI",system-ui,-apple-system,"Helvetica Neue",Arial,sans-serif';
  var F_META = '12px ' + SANS;                          // 12px floor: never smaller at 100%
  // Selected titles are drawn bold (see .node.selected in the sheet), so every width and wrap
  // decision is measured against the bold metrics — selection can never overflow a card.
  var F_TITLE_B = '600 14px ' + SANS, F_GTITLE_B = '600 15px ' + SANS;
  var PAD_X = 14, PAD_Y = 13, LH_TITLE = 19, LH_GTITLE = 21, LH_META = 16, LH_OUTCOME = 15;
  var CARD_MIN = 206, CARD_MAX = 272;
  var GUTTER = 20, GROUP_PAD = 20, GROUP_MIN_W = 288, HEAD_MIN_H = 76, PROGRESS_H = 5;
  var LAYER_GAP = 46, COL_GAP = 24;                    // DAG spacing: rows of layers, cards across
  var MARGIN = { x: 20, y: 16 };                       // content inset inside the canvas box
  var LOD_BELOW = 0.8;                                 // under this zoom the cards show title + glyph only

  var MEAS = document.createElement('canvas').getContext('2d');
  function textW(s, font) { MEAS.font = font; return MEAS.measureText(s).width; }
  function clamp(lo, v, hi) { return Math.max(lo, Math.min(hi, v)); }
  function maxOf(list, f) { return list.reduce(function (m, x) { return Math.max(m, f(x)); }, 0); }
  // Greedy word wrap. Never truncates and never splits a word, so a card's text nodes still
  // read back as the whole recorded label (qa-test joins them with single spaces).
  function wrap(text, font, maxW) {
    var words = String(text === null || text === undefined ? '' : text).split(/\s+/).filter(function (w) { return w.length; });
    if (!words.length) return [''];
    var lines = [], line = words[0];
    for (var i = 1; i < words.length; i++) {
      var cand = line + ' ' + words[i];
      if (textW(cand, font) <= maxW) line = cand;
      else { lines.push(line); line = words[i]; }
    }
    lines.push(line);
    return lines;
  }

  function statusMeta(s) {
    if (s === 'completed') return { word: 'COMPLETED', cls: 'stat-completed', sym: 'completed', key: d3.symbolSquare, size: 40 };
    if (s === 'in-progress-awaiting-review') return { word: 'AWAITING REVIEW', cls: 'stat-awaiting', sym: 'awaiting', key: d3.symbolDiamond, size: 52 };
    if (s === 'in-progress' || s === 'in-progress-design') return { word: 'IN PROGRESS', cls: 'stat-current', sym: 'active', key: d3.symbolTriangle, size: 52 };
    if (s === 'deferred') return { word: 'DEFERRED', cls: 'stat-deferred', sym: 'hold', key: d3.symbolCross, size: 44 };
    if (s === 'superseded') return { word: 'SUPERSEDED', cls: 'stat-deferred', sym: 'hold', key: d3.symbolCross, size: 44 };
    return { word: (s || 'PLANNED').toUpperCase(), cls: 'stat-planned', sym: 'planned', key: d3.symbolCircle, size: 32 };
  }
  function statusNodeClass(s) {
    if (s === 'completed') return 'status-completed';
    if (s === 'in-progress-awaiting-review' || s === 'in-progress' || s === 'in-progress-design') return 'status-active';
    if (s === 'deferred') return 'status-deferred';
    if (s === 'superseded') return 'status-superseded';
    return 'status-planned';
  }
  function nodeStyle(n) {
    if (!n) return { fill: '#FFFFFF', stroke: '#C6D2DF', cls: 'pill-del' };
    switch (n.kind) {
      case 'prototype-container': return { fill: '#F2F3FB', stroke: '#4B4FA8', cls: 'pill-ptl' };
      case 'delivery-container': return { fill: '#F1F7F6', stroke: '#0F7A6C', cls: 'pill-del' };
      case 'leaf': return { fill: '#FFFFFF', stroke: '#C6D2DF', cls: 'pill-del' };
      case 'reference': return { fill: '#F4F6F9', stroke: '#7C8BA0', cls: 'pill-ref' };
      case 'grilling': return { fill: '#FBF5EC', stroke: '#A15C0B', cls: 'pill-dis' };
      case 'checkpoint': return { fill: '#FBF5EC', stroke: '#A15C0B', cls: 'pill-dis' };
      default: return { fill: '#FFFFFF', stroke: '#C6D2DF', cls: 'pill-del' };
    }
  }
  var SYMBOL = d3.symbol();
  function symbolPath(st) { return SYMBOL.type(st.key).size(st.size)(); }

  /* ---------------------------------------------------------------- app state */
  var EMPTY_INSPECTOR = document.getElementById('insp').innerHTML;
  var SEL = null;              // selected slice id
  var PREVIOUS = null;         // one-step inspector return, not browser history
  var HOVER_NODE = null, HOVER_EDGE = null, EDGE_SAMPLES = [];
  function shortRef(id) { return (WORK.references && WORK.references.byId[id]) || ''; }
  function qualifiedRef(id) { return ((WORK.project || {}).id || 'Project') + '/' + (shortRef(id) || id); }
  function namedRef(id) { return (shortRef(id) ? shortRef(id) + ' · ' : '') + (NODES[id] ? NODES[id].label : id); }
  var CUR_VIEW = 'map';
  var COLLAPSED = {};          // groupId -> truthy while its subtree is folded away
  var FOCUS = null;            // groupId the canvas is narrowed to (null = whole project)
  var OPENING_DAG_FOCUS = null; // reversible default narrative focus, never a selection
  var BOX = { map: { w: 900, h: 600 }, dag: { w: 900, h: 600 } };
  var MAP = { plate: null, extras: [] };
  var DAG = { pos: {}, cards: {}, routes: [] };

  /* ============================================================ card models */
  function cardModel(id, opt) {
    var n = NODES[id], st = statusMeta(n.status), s = nodeStyle(n);
    var avail = CARD_MAX - PAD_X * 2;
    // One meta line: reference, then the status word (or the rollup for a container), then the
    // most useful extra bits while they fit. Tertiary counts never get their own smaller line.
    var container = isContainer(id);
    var bits = [container ? rollup(id).word : st.word];
    if (n.kind === 'grilling') bits.push('owner session');
    // In the DAG a dimmed hub edge is only honest if the card names the hub, so "needs S0xx"
    // outranks the optional bits (slice count, intent) when the line runs out of room — for
    // container cards as much as for leaves.
    if (opt.groupView) {
      var hubRefs = (n.requires || []).filter(function (r) { return HUBS[r] && shortRef(r); }).map(shortRef);
      if (hubRefs.length) bits.push('needs ' + hubRefs.join(', '));     // one bit, so two hubs never lose the second
    }
    if (opt.groupView && container) {
      var children = childIds(id), childGroups = children.length > 0 && children.every(isContainer);
      bits.push(children.length + (childGroups ? (children.length > 1 ? ' groups' : ' group') : (children.length > 1 ? ' slices' : ' slice')));
      // DAG cards need to carry the beneficiary context that Map headers inherit visibly.
      if (n.role) bits.push('for ' + n.role);
    }
    else if (!container && n.role) bits.push('for ' + n.role);
    if (opt.groupView && !container && n.intent) bits.push(n.intent);
    if (!opt.groupView && n.reviewRunIds && n.reviewRunIds.length) bits.push(n.reviewRunIds.length + (n.reviewRunIds.length > 1 ? ' reviews' : ' review'));
    var metaText = (shortRef(id) ? shortRef(id) + ' · ' : '') + bits[0], GLYPH_W = 16;
    for (var b = 1; b < bits.length; b++) {
      var cand = metaText + ' · ' + bits[b];
      if (textW(cand, F_META) + GLYPH_W <= avail) metaText = cand;
    }
    var longestWord = String(n.label || '').split(/\s+/).reduce(function (m, w) { return Math.max(m, textW(w, F_TITLE_B)); }, 0);
    var want = Math.max(
      Math.min(textW(n.label, F_TITLE_B), CARD_MAX) + PAD_X * 2,
      textW(metaText, F_META) + GLYPH_W + PAD_X * 2,
      longestWord + PAD_X * 2 + 2,
      CARD_MIN
    );
    var w = Math.max(clamp(CARD_MIN, want, CARD_MAX), longestWord + PAD_X * 2 + 2);
    var lines = wrap(n.label, F_TITLE_B, w - PAD_X * 2);
    // New capability records may opt into one concise chart-facing outcome. Keep legacy
    // records unchanged while making role/value legible without opening the inspector.
    var outcome = n.visibleOutcome ? String(n.visibleOutcome) : '';
    var outcomeLines = outcome ? wrap(outcome, F_META, w - PAD_X * 2 - 4) : [];
    var outcomeLine = outcomeLines[0] || '';
    // Cut impact is a first-glance consequence, not inspector-only metadata. Prefer an
    // authored concise chart line, while retaining the full source text in the inspector.
    var cut = n.visibleCutImpact ? String(n.visibleCutImpact) : (n.cutImpact ? String(n.cutImpact) : '');
    var cutLines = cut ? wrap('Cut: ' + cut, F_META, w - PAD_X * 2 - 4) : [];
    var outcomeBlock = outcomeLines.length ? outcomeLines.length * LH_OUTCOME + 2 : 0;
    var cutBlock = cutLines.length ? cutLines.length * LH_OUTCOME + 2 : 0;
    var h = PAD_Y * 2 + lines.length * LH_TITLE + outcomeBlock + cutBlock + 4 + LH_META;
    if (opt.groupView) {
      // Ports spread across the top (incoming) and bottom (outgoing) edges: keep room for them.
      var ports = Math.max(EDGES.requires.filter(function (e) { return e.from === id; }).length,
        EDGES.requires.filter(function (e) { return e.to === id; }).length);
      w = Math.max(w, Math.min(CARD_MAX, ports * 14 + 40));
    }
    return {
      id: id, node: n, kind: 'card', x: 0, y: 0, w: w, h: h, lines: lines,
      outcomeLine: outcomeLine, outcomeLines: outcomeLines, cutLines: cutLines, metaText: metaText, st: st, style: s, group: container,
      cls: statusNodeClass(n.status)
    };
  }

  /* =============================================================== MAP layout */
  function packRows(items, budget) {
    var rows = [], row = [], rowW = 0;
    items.forEach(function (it) {
      var need = it.w + (row.length ? GUTTER : 0);
      if (row.length && rowW + need > budget) { rows.push(row); row = [it]; rowW = it.w; }
      else { row.push(it); rowW += need; }
    });
    if (row.length) rows.push(row);
    return rows;
  }
  function rowWidth(r) { return r.reduce(function (a, it) { return a + it.w; }, 0) + GUTTER * (r.length - 1); }
  function rowHeight(r) { return maxOf(r, function (it) { return it.h; }); }

  function buildGroup(id, depth) {
    var n = NODES[id], head = { id: id, title: n.label, cls: statusNodeClass(n.status) };
    var kids = childIds(id).filter(function (c) { return NODES[c]; })
      .map(function (c) { return isContainer(c) ? buildGroup(c, depth + 1) : cardModel(c, { foot: true, groupView: false }); });
    var collapsed = !!COLLAPSED[id];
    var rows = (collapsed || !kids.length) ? [] : packRows(kids, (function () {
      var ws = kids.map(function (k) { return k.w; });
      var total = ws.reduce(function (a, b) { return a + b; }, 0) + GUTTER * (ws.length - 1);
      var widest = Math.max.apply(null, ws);
      // Bound row width instead of adding the three widest nested groups together.
      // A focused phone view uses one readable column; the full overview stays stable.
      var budget = window.innerWidth <= 700 ? Math.max(280, document.getElementById('ms').clientWidth - 80) : (depth === 0 ? 1200 : 860);
      return Math.max(widest, Math.min(total, budget));
    })());

    var toolsW = 0; // Group actions live in the inspector, outside the zoom transform.
    // The header states the rollup, never the recorded container status word: a group that is
    // "planned" while six of its eight slices are complete would otherwise contradict itself.
    var roll = rollup(id);
    var metaBits = [];
    if (shortRef(id)) metaBits.push(shortRef(id));
    metaBits.push(roll.word);
    if (n.role) metaBits.push('for ' + n.role);
    if (n.intent) metaBits.push(n.intent);
    if (collapsed) metaBits.push('+' + descendants(id).length + ' hidden');
    head.metaText = metaBits.join(' · ');
    head.outcomeLine = n.visibleOutcome ? (textW(String(n.visibleOutcome), F_META) > GROUP_MIN_W - PAD_X * 2
      ? wrap(String(n.visibleOutcome), F_META, GROUP_MIN_W - PAD_X * 2)[0] : String(n.visibleOutcome)) : '';
    head.progress = roll;

    var contentW = rows.length ? Math.max.apply(null, rows.map(rowWidth)) : 0;
    var contentH = rows.length ? rows.reduce(function (a, r) { return a + rowHeight(r); }, 0) + GUTTER * (rows.length - 1) : 0;
    var floorW = Math.max(GROUP_MIN_W, contentW + GROUP_PAD * 2, PAD_X * 2 + textW(head.metaText, F_META) + toolsW + 18);
    head.titleLines = wrap(head.title, F_GTITLE_B, floorW - PAD_X * 2 - toolsW - 18);
    head.h = Math.max(HEAD_MIN_H, 11 + head.titleLines.length * LH_GTITLE + 3 + (head.outcomeLine ? LH_OUTCOME + 2 : 0) + LH_META + 6 + PROGRESS_H + 9);

    var g = {
      id: id, node: n, kind: 'group', cls: statusNodeClass(n.status), x: 0, y: 0,
      w: floorW, h: head.h + (contentH ? GROUP_PAD - 4 + contentH + GROUP_PAD : 12),
      head: head, depth: depth, collapsed: collapsed, hidden: descendants(id).length,
      rows: rows, style: nodeStyle(n)
    };
    if (collapsed) g.h = head.h + 12;
    var cy = head.h + GROUP_PAD - 4;
    rows.forEach(function (r) {
      var rh = rowHeight(r), cx = GROUP_PAD;
      r.forEach(function (it) { it.x = cx; it.y = cy; cx += it.w + GUTTER; });
      cy += rh + GUTTER;
    });
    return g;
  }

  // Children carry group-local coordinates; make them absolute for painting.
  function placeAbsolute(item, ox, oy) {
    item.x += ox; item.y += oy;
    if (item.kind === 'group' && !item.collapsed) {
      item.rows.forEach(function (r) { r.forEach(function (it) { placeAbsolute(it, item.x, item.y); }); });
    }
    return item;
  }

  function layoutMap() {
    var topId = (FOCUS && NODES[FOCUS]) ? FOCUS : ROOT;
    var plate = placeAbsolute(isContainer(topId) ? buildGroup(topId, 0) : cardModel(topId, { foot: true, groupView: false }), MARGIN.x, MARGIN.y + 22);
    var extras = [];
    if (topId === ROOT) ORPHANS.forEach(function (id) { extras.push(cardModel(id, { foot: true, groupView: false })); });
    var ey = plate.y + plate.h + (extras.length ? GUTTER + 6 : 0);
    var ex = plate.x;
    extras.forEach(function (it) {
      it.x = ex; it.y = ey; ex += it.w + GUTTER;
      if (ex > MARGIN.x + 1300) { ex = MARGIN.x; ey += it.h + GUTTER; }
    });
    var w = Math.max(plate.x + plate.w, maxOf(extras, function (it) { return it.x + it.w; })) + MARGIN.x;
    var h = Math.max(plate.y + plate.h, maxOf(extras, function (it) { return it.y + it.h; })) + MARGIN.y;
    MAP.plate = plate; MAP.extras = extras;
    BOX.map = { w: w, h: h };
  }

  function flattenMap(item, out) {
    out = out || [];
    if (item.kind === 'group') {
      out.push(item);
      if (!item.collapsed) item.rows.forEach(function (r) { r.forEach(function (it) { flattenMap(it, out); }); });
    } else out.push(item);
    return out;
  }

  /* =============================================================== DAG layout */
  function dagLayers() {
    var layer = {};
    ALL_IDS.forEach(function (id) { layer[id] = 0; });
    for (var pass = 0; pass < ALL_IDS.length + 2; pass++) {
      var changed = false;
      EDGES.requires.forEach(function (e) {
        if (layer[e.from] < layer[e.to] + 1) { layer[e.from] = layer[e.to] + 1; changed = true; }
      });
      if (!changed) break;
    }
    var maxL = 0;
    ALL_IDS.forEach(function (id) { layer[id] = Math.max(0, layer[id] | 0); maxL = Math.max(maxL, layer[id]); });
    return { layer: layer, maxL: maxL };
  }

  // Layers are ROWS: prerequisites above, dependents below. Within a row, cards sit side by
  // side. Long edges reserve a virtual lane in every row they pass through; hub edges instead
  // travel down the left gutter so seven near-identical fans never widen the rows.
  function layoutDag() {
    DAG.pos = {}; DAG.cards = {}; DAG.routes = []; DAG.vlane = {};
    var lay = dagLayers(), layer = lay.layer, maxL = lay.maxL;
    ALL_IDS.forEach(function (id) { DAG.cards[id] = cardModel(id, { groupView: true }); });

    // vertices per row: real cards plus virtual lane markers for non-hub edges spanning >1 row
    var cols = [];
    for (var l = 0; l <= maxL; l++) cols[l] = [];
    ALL_IDS.forEach(function (id) {
      var c = DAG.cards[id];
      cols[layer[id]].push({ id: id, real: true, w: c.w, h: c.h, layer: layer[id] });
    });
    var vseq = 0;
    EDGES.requires.forEach(function (e) {
      if (isHubEdge(e)) return;
      for (var k = layer[e.to] + 1; k < layer[e.from]; k++) {
        cols[k].push({ id: 'v' + (vseq++), real: false, w: 12, h: 10, layer: k, edge: e.from + '>' + e.to });
      }
    });
    function edgeEnds(v) {
      if (!v.edge) return [];
      var p = v.edge.split('>');
      return [p[0], p[1]];
    }
    function neighbourIds(v) {
      if (!v.real) return edgeEnds(v);
      var out = [];
      EDGES.requires.forEach(function (e) { if (e.from === v.id) out.push(e.to); if (e.to === v.id) out.push(e.from); });
      // Containment is visual context in the DAG as well as enclosure in the Map. Include
      // adjacent parents/children only in ordering, never in the prerequisite edge set.
      (CONTAINS[v.id] || []).forEach(function (parent) { out.push(parent); });
      childIds(v.id).forEach(function (child) { out.push(child); });
      return out;
    }
    function indexOf() {
      var idx = {};
      cols.forEach(function (col) { col.forEach(function (v, i) { idx[v.id] = i; }); });
      return idx;
    }

    // 1. ordering: barycenter sweeps over real + virtual vertices
    cols.forEach(function (col) { col.sort(function (a, b) { return a.id < b.id ? -1 : a.id > b.id ? 1 : 0; }); });
    for (var sweep = 0; sweep < 6; sweep++) {
      var idx = indexOf(), down = sweep % 2 === 0, order = [];
      for (var s = 0; s <= maxL; s++) order.push(down ? s : maxL - s);
      order.forEach(function (li) {
        var col = cols[li];
        col.forEach(function (v) {
          var nb = neighbourIds(v).map(function (id) { return idx[id]; }).filter(function (x) { return x !== undefined; });
          v.bary = nb.length ? nb.reduce(function (a, b) { return a + b; }, 0) / nb.length : idx[v.id];
        });
        col.sort(function (a, b) { return a.bary - b.bary || (a.id < b.id ? -1 : a.id > b.id ? 1 : 0); });
        col.forEach(function (v, i) { idx[v.id] = i; });
      });
    }

    // 2. rows: height from the real cards, separated by a routing corridor
    var GUTTER_W = 30;                            // left gutter for hub edges, outside every card
    var rowH = [], rowY = [], y = MARGIN.y + 12;
    for (var li2 = 0; li2 <= maxL; li2++) {
      var hs = cols[li2].filter(function (v) { return v.real; }).map(function (v) { return v.h; });
      rowH[li2] = hs.length ? Math.max.apply(null, hs) : 10;
      rowY[li2] = y; y += rowH[li2] + LAYER_GAP;
    }
    var canvasBottom = y - LAYER_GAP;

    // 3. across each row: stack, then pull each vertex toward the mean x of its neighbours
    var left0 = MARGIN.x + GUTTER_W;
    cols.forEach(function (col) {
      var x = left0;
      col.forEach(function (v) { v.x = x; x += v.w + COL_GAP; });
    });
    function centreOf(id) {
      if (DAG.pos[id]) return DAG.pos[id].cx;
      if (DAG.vlane[id] !== undefined) return DAG.vlane[id];
      return null;
    }
    for (var pass2 = 0; pass2 < 4; pass2++) {
      var seq = [];
      for (var s2 = 0; s2 <= maxL; s2++) seq.push(pass2 % 2 === 0 ? s2 : maxL - s2);
      seq.forEach(function (li) {
        var col = cols[li];
        col.forEach(function (v) {
          var xs = neighbourIds(v).map(centreOf).filter(function (x) { return x !== null && x !== undefined; });
          v.want = xs.length ? xs.reduce(function (a, b) { return a + b; }, 0) / xs.length : v.x + v.w / 2;
        });
        col.sort(function (a, b) { return a.want - b.want || (a.id < b.id ? -1 : 1); });
        var cursor = left0;
        col.forEach(function (v) {
          v.x = Math.max(cursor, v.want - v.w / 2);
          cursor = v.x + v.w + COL_GAP;
        });
        var leftmost = col.length ? Math.min.apply(null, col.map(function (v) { return v.x; })) : cursor;
        col.forEach(function (v) { v.x = left0 + (v.x - leftmost); });
        col.forEach(function (v) {
          var top = rowY[li] + (rowH[li] - v.h) / 2;
          if (v.real) DAG.pos[v.id] = { x: v.x, y: top, w: v.w, h: v.h, cx: v.x + v.w / 2, cy: top + v.h / 2, layer: li };
          else DAG.vlane[v.id] = v.x + v.w / 2;
        });
      });
    }
    // virtual lanes keyed by edge + row, for the router
    cols.forEach(function (col) { col.forEach(function (v) { if (!v.real) DAG.vlane[v.edge + '@' + v.layer] = v.x + v.w / 2; }); });
    var canvasRight = 0;
    cols.forEach(function (col) { col.forEach(function (v) { canvasRight = Math.max(canvasRight, v.x + v.w); }); });
    DAG.rowY = rowY; DAG.rowH = rowH; DAG.layer = layer; DAG.maxL = maxL; DAG.gutterX = MARGIN.x + 8;
    BOX.dag = { w: canvasRight + MARGIN.x, h: canvasBottom + MARGIN.y + 12 };
    DAG.routes = routeDag();
  }

  function corridorY(k) {                       // empty channel between row k-1 and row k
    var above = DAG.rowY[k - 1] + DAG.rowH[k - 1];
    return above + (DAG.rowY[k] - above) / 2;
  }

  // Orthogonal routes, prerequisite → dependent: leave the prerequisite's bottom edge, travel
  // corridors and reserved lanes, arrive at the dependent's top edge where the arrowhead lands.
  // Hub edges go down the left gutter, which by construction contains no card.
  function routeDag() {
    var edges = EDGES.requires.slice().sort(function (a, b) {
      return (a.from + '>' + a.to) < (b.from + '>' + b.to) ? -1 : 1;
    });
    var lanes = {}, gutterLanes = 0;
    function rankWithin(list, e, side) {
      var same = list.filter(function (o) { return o[side] === e[side]; });
      same.sort(function (a, b) {
        var other = side === 'from' ? 'to' : 'from';
        var ka = DAG.pos[a[other]].cx, kb = DAG.pos[b[other]].cx;
        return ka - kb || ((a.from + '>' + a.to) < (b.from + '>' + b.to) ? -1 : 1);
      });
      return { i: same.indexOf(e), n: same.length };
    }
    function port(e, side, rank) {
      var p = DAG.pos[side === 'top' ? e.from : e.to];
      var span = Math.max(0, (p.w - 28) / 2);
      var off = rank.n > 1 ? (rank.i - (rank.n - 1) / 2) * (span * 2 / Math.max(1, rank.n - 1)) : 0;
      return { x: p.cx + off, y: side === 'top' ? p.y - 4 : p.y + p.h + 3 };
    }
    return edges.map(function (e) {
      var a = DAG.pos[e.from], b = DAG.pos[e.to];      // a = dependent (below), b = prerequisite (above)
      var start = port(e, 'bottom', rankWithin(edges, e, 'to'));
      var end = port(e, 'top', rankWithin(edges, e, 'from'));
      var pts = [[start.x, start.y]];
      function laneY(k) { var i = (lanes[k] = (lanes[k] || 0) + 1) - 1; return corridorY(k) + ((i % 5) - 2) * 4; }
      if (isHubEdge(e) && a.layer - b.layer > 1) {
        var gx = DAG.gutterX - (gutterLanes++ % 3) * 4;
        var y0 = laneY(b.layer + 1), y1 = laneY(a.layer);
        pts.push([start.x, y0], [gx, y0], [gx, y1], [end.x, y1]);
      } else {
        var y = laneY(b.layer + 1);
        pts.push([start.x, y]);
        for (var k = b.layer + 1; k < a.layer; k++) {
          var lx = DAG.vlane[e.from + '>' + e.to + '@' + k];
          if (lx === undefined) lx = start.x;
          pts.push([lx, y]);
          y = laneY(k + 1);
          pts.push([lx, y]);
        }
        pts.push([end.x, y]);
      }
      pts.push([end.x, end.y]);
      return { key: e.from + '>' + e.to, from: e.from, to: e.to, hub: isHubEdge(e), pts: dedupe(pts) };
    });
  }
  function dedupe(pts) {
    var out = [];
    pts.forEach(function (p) {
      var last = out[out.length - 1];
      if (!last || Math.abs(last[0] - p[0]) > 0.5 || Math.abs(last[1] - p[1]) > 0.5) out.push([p[0], p[1]]);
    });
    return out;
  }
  // Rounded-elbow path from an axis-aligned point list.
  function elbowPath(pts, r) {
    if (pts.length < 2) return '';
    var d = 'M' + pts[0][0].toFixed(1) + ',' + pts[0][1].toFixed(1);
    for (var i = 1; i < pts.length - 1; i++) {
      var p = pts[i], a = pts[i - 1], b = pts[i + 1];
      var l1 = Math.sqrt((p[0] - a[0]) * (p[0] - a[0]) + (p[1] - a[1]) * (p[1] - a[1]));
      var l2 = Math.sqrt((b[0] - p[0]) * (b[0] - p[0]) + (b[1] - p[1]) * (b[1] - p[1]));
      if (!l1 || !l2) { d += 'L' + p[0].toFixed(1) + ',' + p[1].toFixed(1); continue; }
      var rr = Math.min(r, l1 / 2, l2 / 2);
      if (rr < 1) { d += 'L' + p[0].toFixed(1) + ',' + p[1].toFixed(1); continue; }
      var ux = (a[0] - p[0]) / l1, uy = (a[1] - p[1]) / l1, vx = (b[0] - p[0]) / l2, vy = (b[1] - p[1]) / l2;
      d += 'L' + (p[0] + ux * rr).toFixed(1) + ',' + (p[1] + uy * rr).toFixed(1) +
        'Q' + p[0].toFixed(1) + ',' + p[1].toFixed(1) + ' ' + (p[0] + vx * rr).toFixed(1) + ',' + (p[1] + vy * rr).toFixed(1);
    }
    var last = pts[pts.length - 1];
    return d + 'L' + last[0].toFixed(1) + ',' + last[1].toFixed(1);
  }

  /* ================================================================ rendering */
  var svgEl = document.getElementById('svg');
  var svg = d3.select(svgEl);
  var viewport = null, scene = null;

  function ensureDefs() {
    var defs = svg.selectAll('defs').data([0]).join('defs');
    var marks = defs.selectAll('marker').data([{ id: 'ah', fill: '#0F7A6C' }, { id: 'ah-hl', fill: '#16283E' }], function (d) { return d.id; })
      .join(function (en) {
        var m = en.append('marker').attr('id', function (d) { return d.id; })
          .attr('viewBox', '0 0 10 10').attr('refX', 9.2).attr('refY', 5)
          .attr('markerWidth', 5.4).attr('markerHeight', 5.4).attr('markerUnits', 'strokeWidth').attr('orient', 'auto');
        m.append('path').attr('d', 'M0,0.6 L10,5 L0,9.4 L2.4,5 Z');
        return m;
      });
    marks.select('path').attr('fill', function (d) { return d.fill; });
    defs.selectAll('pattern#grid').data([0]).join(function (en) {
      var p = en.append('pattern').attr('id', 'grid').attr('width', 28).attr('height', 28).attr('patternUnits', 'userSpaceOnUse');
      p.append('path').attr('d', 'M28,0 L0,0 L0,28').attr('fill', 'none').attr('stroke', '#EDF1F6').attr('stroke-width', 1);
      return p;
    });
  }

  function isFaded(id) {
    if (!FOCUS || !NODES[FOCUS]) return false;
    return id !== FOCUS && descendants(FOCUS).indexOf(id) === -1;
  }

  function paintCards(sel, cards) {
    var neigh = neighboursOfSel();
    var g = sel.selectAll('g.node.card-g').data(cards, function (d) { return d.id; })
      .join(function (en) {
        var gg = en.append('g').attr('class', 'node card-g').attr('tabindex', -1).attr('role', 'button');
        gg.append('rect').attr('class', 'card nodeBg').attr('rx', 8);
        gg.append('rect').attr('class', 'nodeHit').attr('rx', 8).attr('fill', 'transparent').style('pointer-events', 'all');
        gg.append('rect').attr('class', 'hover-ring').attr('rx', 11);
        gg.append('path').attr('class', 'glyph');
        gg.append('text').attr('class', 'card-title');
        gg.append('text').attr('class', 'card-outcome');
        gg.append('text').attr('class', 'card-cut');
        gg.append('text').attr('class', 'card-meta');
        return gg;
      });
    g.attr('class', function (d) {
        var c = 'node card-g ' + d.cls + (d.group ? ' group-card' : '');
        if (d.id === SEL) c += ' selected'; else if (neigh[d.id]) c += ' adj';
        if (isFaded(d.id)) c += ' faded';
        return c;
      })
      .attr('data-id', function (d) { return d.id; })
      .attr('data-primary-capability', function (d) { return d.node && d.node.role && d.node.beneficiary && d.node.intent === 'delivery' && d.node.visibleOutcome ? 'true' : null; })
      .attr('aria-pressed', function (d) { return d.id === SEL ? 'true' : 'false'; })
      .attr('aria-label', function (d) { return namedRef(d.id) + ', ' + d.st.word; })
      .attr('transform', function (d) { return 'translate(' + d.x + ',' + d.y + ')'; });
    g.select('rect.nodeBg').attr('width', function (d) { return d.w; }).attr('height', function (d) { return d.h; });
    g.select('rect.nodeHit').attr('width', function (d) { return d.w; }).attr('height', function (d) { return d.h; });
    g.select('rect.hover-ring').attr('x', -4).attr('y', -4).attr('width', function (d) { return d.w + 8; }).attr('height', function (d) { return d.h + 8; });
    g.select('path.glyph')
      .attr('class', function (d) { return 'glyph glyph-' + d.st.sym; })
      .attr('transform', function (d) { return 'translate(' + (PAD_X + 4) + ',' + (PAD_Y + d.lines.length * LH_TITLE + (d.outcomeLines.length ? d.outcomeLines.length * LH_OUTCOME + 2 : 0) + (d.cutLines.length ? d.cutLines.length * LH_OUTCOME + 2 : 0) + 4 + LH_META / 2) + ')'; })
      .attr('d', function (d) { return symbolPath(d.st); });
    g.select('text.card-title').attr('x', PAD_X).attr('y', PAD_Y + LH_TITLE - 4)
      .selectAll('tspan').data(function (d) { return d.lines; }).join('tspan')
      .attr('x', PAD_X).attr('dy', function (dd, i) { return i ? LH_TITLE : 0; })
      .text(function (dd) { return dd; });
    g.select('text.card-outcome').attr('x', PAD_X).attr('y', function (d) {
      return PAD_Y + d.lines.length * LH_TITLE + LH_OUTCOME - 2;
    }).selectAll('tspan').data(function (d) { return d.outcomeLines; }).join('tspan')
      .attr('x', PAD_X).attr('dy', function (dd, i) { return i ? LH_OUTCOME : 0; })
      .text(function (dd) { return dd; });
    g.select('text.card-cut').attr('x', PAD_X).attr('y', function (d) {
      return PAD_Y + d.lines.length * LH_TITLE + (d.outcomeLines.length ? d.outcomeLines.length * LH_OUTCOME + 2 : 0) + LH_OUTCOME - 2;
    }).selectAll('tspan').data(function (d) { return d.cutLines; }).join('tspan')
      .attr('x', PAD_X).attr('dy', function (dd, i) { return i ? LH_OUTCOME : 0; })
      .text(function (dd) { return dd; });
    g.select('text.card-meta').attr('x', PAD_X + 16)
      .attr('y', function (d) { return PAD_Y + d.lines.length * LH_TITLE + (d.outcomeLines.length ? d.outcomeLines.length * LH_OUTCOME + 2 : 0) + (d.cutLines.length ? d.cutLines.length * LH_OUTCOME + 2 : 0) + 4 + LH_META - 3; })
      .text(function (d) { return d.metaText; });
    return g;
  }

  // Roving tabindex: the canvas is one Tab stop and the current slice is the only focusable
  // node; arrow keys move between nodes (see the keydown handler in init).
  function tabStopId() {
    if (SEL && document.querySelector('#svg .node[data-id="' + cssId(SEL) + '"]')) return SEL;
    var first = document.querySelector('#svg .node');
    return first ? first.dataset.id : null;
  }
  function cssId(id) { return String(id).replace(/["\\]/g, '\\$&'); }
  function applyTabStops() {
    var stop = tabStopId();
    document.querySelectorAll('#svg .node').forEach(function (el) { el.setAttribute('tabindex', el.dataset.id === stop ? '0' : '-1'); });
  }
  // Nearest node in a direction, measured on painted centres: what a sighted keyboard user expects.
  function nodeInDirection(fromId, key) {
    var cur = document.querySelector('#svg .node[data-id="' + cssId(fromId) + '"]');
    if (!cur) return null;
    var c = cur.getBoundingClientRect(), cx = (c.left + c.right) / 2, cy = (c.top + c.bottom) / 2;
    var best = null, bestD = Infinity, fallback = null, fallbackD = Infinity;
    document.querySelectorAll('#svg .node').forEach(function (el) {
      if (el === cur) return;
      var r = el.getBoundingClientRect(), x = (r.left + r.right) / 2 - cx, y = (r.top + r.bottom) / 2 - cy;
      var along = key === 'ArrowRight' ? x : key === 'ArrowLeft' ? -x : key === 'ArrowDown' ? y : -y;
      var across = (key === 'ArrowRight' || key === 'ArrowLeft') ? Math.abs(y) : Math.abs(x);
      var distance = Math.hypot(x, y) + across * 2.5;
      if (distance < fallbackD) { fallbackD = distance; fallback = el.dataset.id; }
      if (along <= 4) return;
      var d = along + across * 2.5;
      if (d < bestD) { bestD = d; best = el.dataset.id; }
    });
    // At a diagram edge, keep arrow-key traversal within the canvas rather than trapping focus.
    return best || fallback;
  }

  function paintGrid(sel, box) {
    sel.selectAll('rect.canvas-grid').data([0]).join('rect')
      .attr('class', 'canvas-grid')
      .attr('x', -box.w).attr('y', -box.h).attr('width', box.w * 3).attr('height', box.h * 3)
      .attr('fill', 'url(#grid)');
  }

  function drawMap(sc) {
    layoutMap();
    paintGrid(sc, BOX.map);
    var items = flattenMap(MAP.plate).concat(MAP.extras);
    var frames = items.filter(function (it) { return it.kind === 'group'; });

    sc.selectAll('g.frame').data(frames, function (d) { return d.id; })
      .join(function (en) { return en.append('g').attr('class', 'frame').style('pointer-events', 'none'); })
      .attr('data-id', function (d) { return d.id; })
      .each(function (d) {
        var s = d3.select(this);
        s.selectAll('rect.groupFrame').data([d]).join('rect')
          .attr('class', 'groupFrame').attr('x', d.x - 4).attr('y', d.y + d.head.h - 10)
          .attr('width', d.w + 8).attr('height', Math.max(14, d.h - d.head.h + 12))
          .attr('rx', 11)
          .attr('fill', function (dd) { return dd.depth === 0 ? '#FFFFFF' : nodeStyle(dd.node).fill; })
          .attr('stroke', function (dd) { return nodeStyle(dd.node).stroke; })
          .attr('opacity', function (dd) { return dd.depth === 0 ? 1 : 0.9; });
      });

    paintCards(sc, items.filter(function (it) { return it.kind === 'card'; }));

    var hg = sc.selectAll('g.node.head-g').data(frames, function (d) { return d.id; })
      .join(function (en) {
        var g = en.append('g').attr('class', 'node head-g').attr('tabindex', -1).attr('role', 'button');
        g.append('rect').attr('class', 'groupHeadBg containerHit nodeBg').attr('rx', 10);
        g.append('rect').attr('class', 'hover-ring').attr('rx', 13);
        g.append('text').attr('class', 'group-title');
        g.append('text').attr('class', 'group-outcome');
        g.append('text').attr('class', 'group-meta');
        g.append('rect').attr('class', 'progress-track').attr('rx', 2.5);
        g.append('rect').attr('class', 'progress-fill').attr('rx', 2.5);
        return g;
      });
    var neigh = neighboursOfSel();
    hg.attr('class', function (d) {
        var c = 'node head-g ' + d.cls;
        if (d.id === SEL) c += ' selected'; else if (neigh[d.id]) c += ' adj';
        if (isFaded(d.id)) c += ' faded';
        return c;
      })
      .attr('data-id', function (d) { return d.id; })
      .attr('data-primary-capability', function (d) { return d.node && d.node.role && d.node.beneficiary && d.node.intent === 'delivery' && d.node.visibleOutcome ? 'true' : null; })
      .attr('aria-pressed', function (d) { return d.id === SEL ? 'true' : 'false'; })
      .attr('aria-label', function (d) { return 'Inspect ' + namedRef(d.id) + ', ' + statusMeta(d.node.status).word; })
      .attr('transform', function (d) { return 'translate(' + d.x + ',' + d.y + ')'; });
    hg.select('rect.containerHit')
      .attr('class', function (d) { return 'groupHeadBg containerHit nodeBg depth' + d.depth; })
      .attr('x', 0).attr('y', 0).attr('width', function (d) { return d.w; }).attr('height', function (d) { return d.head.h; });
    hg.select('rect.hover-ring').attr('x', -4).attr('y', -4).attr('width', function (d) { return d.w + 8; }).attr('height', function (d) { return d.head.h + 8; });
    hg.select('text.group-title').attr('x', PAD_X).attr('y', 11 + 14)
      .selectAll('tspan').data(function (d) { return d.head.titleLines; }).join('tspan')
      .attr('x', PAD_X).attr('dy', function (dd, i) { return i ? LH_GTITLE : 0; })
      .text(function (dd) { return dd; });
    hg.select('text.group-outcome').attr('x', PAD_X)
      .attr('y', function (d) { return 11 + d.head.titleLines.length * LH_GTITLE + (d.head.outcomeLine ? LH_OUTCOME + 1 : 0); })
      .text(function (d) { return d.head.outcomeLine || ''; });
    hg.select('text.group-meta').attr('x', PAD_X)
      .attr('y', function (d) { return 11 + d.head.titleLines.length * LH_GTITLE + 3 + (d.head.outcomeLine ? LH_OUTCOME + 2 : 0) + LH_META - 3; })
      .text(function (d) { return d.head.metaText; });
    // rollup bar: painted width is the completed share of enclosed slices
    function barY(d) { return 11 + d.head.titleLines.length * LH_GTITLE + 3 + (d.head.outcomeLine ? LH_OUTCOME + 2 : 0) + LH_META + 6; }
    function barW(d) { return Math.max(60, Math.min(160, d.w - PAD_X * 2)); }
    hg.select('rect.progress-track').attr('x', PAD_X).attr('y', barY).attr('height', PROGRESS_H).attr('width', barW);
    hg.select('rect.progress-fill').attr('x', PAD_X).attr('y', barY).attr('height', PROGRESS_H)
      .attr('class', function (d) { return 'progress-fill' + (d.head.progress.total ? '' : ' progress-empty'); })
      .attr('width', function (d) { var p = d.head.progress; return p.total ? Math.round(barW(d) * p.done / p.total) : 0; });
  }

  function drawDag(sc) {
    layoutDag();
    paintGrid(sc, BOX.dag);
    var eg = sc.selectAll('g.edgeLayer').data([0]).join('g').attr('class', 'edgeLayer');
    eg.selectAll('path.dagEdge').data(DAG.routes, function (d) { return d.key; })
      .join(function (en) { return en.append('path'); })
      .attr('class', function (d) {
        var c = 'dagEdge' + (d.hub ? ' hub-edge' : '');
        if (SEL && (d.from === SEL || d.to === SEL)) c += ' hl';
        else if (SEL) c += ' dim';
        if (isFaded(d.from) && isFaded(d.to)) c += ' faded';
        return c;
      })
      .attr('data-from', function (d) { return d.from; })
      .attr('data-to', function (d) { return d.to; })
      .attr('marker-end', function (d) { return SEL && (d.from === SEL || d.to === SEL) ? 'url(#ah-hl)' : 'url(#ah)'; })
      .attr('d', function (d) { return elbowPath(d.pts, 9); });
    // Cache actual painted curves once per layout; closest-route picking is independent
    // of SVG paint order and does not add fat hit paths over the cards.
    EDGE_SAMPLES = [];
    eg.selectAll('path.dagEdge').each(function (d) {
      var len = this.getTotalLength(), count = Math.max(1, Math.ceil(len / 4)), pts = [];
      for (var i = 0; i <= count; i++) pts.push(this.getPointAtLength(len * i / count));
      EDGE_SAMPLES.push({el: this, edge: d, pts: pts, step: len / count, len: len});
    });
    paintCards(sc, ALL_IDS.map(function (id) {
      var p = DAG.pos[id], c = DAG.cards[id];
      c.x = p.x; c.y = p.y;
      return c;
    }));
  }

  function neighboursOfSel() {
    var out = {};
    if (!SEL) return out;
    EDGES.requires.forEach(function (e) { if (e.from === SEL) out[e.to] = 1; if (e.to === SEL) out[e.from] = 1; });
    return out;
  }

  // Selection and highlight are class-level state: repaint them without re-laying out.
  function refreshStates() {
    if (!scene) return;
    var sc = d3.select(scene.node());
    if (CUR_VIEW === 'map') {
      var neighM = neighboursOfSel();
      sc.selectAll('g.node').each(function (d) {
        var c = d.kind === 'group' ? 'node head-g ' : 'node card-g ';
        c += d.cls + (d.kind === 'card' && d.group ? ' group-card' : '');
        if (d.id === SEL) c += ' selected'; else if (neighM[d.id]) c += ' adj';
        if (isFaded(d.id)) c += ' faded';
        this.setAttribute('class', c);
        this.setAttribute('aria-pressed', d.id === SEL ? 'true' : 'false');
      });
    } else {
      var neighD = neighboursOfSel();
      sc.selectAll('g.node.card-g').each(function (d) {
        var c = 'node card-g ' + d.cls + (d.group ? ' group-card' : '');
        if (d.id === SEL) c += ' selected'; else if (neighD[d.id]) c += ' adj';
        if (isFaded(d.id)) c += ' faded';
        this.setAttribute('class', c);
        this.setAttribute('aria-pressed', d.id === SEL ? 'true' : 'false');
      });
      sc.selectAll('path.dagEdge').each(function (d) {
        var c = 'dagEdge' + (d.hub ? ' hub-edge' : '');
        if (SEL && (d.from === SEL || d.to === SEL)) c += ' hl'; else if (SEL) c += ' dim';
        if (isFaded(d.from) && isFaded(d.to)) c += ' faded';
        this.setAttribute('class', c);
        this.setAttribute('marker-end', SEL && (d.from === SEL || d.to === SEL) ? 'url(#ah-hl)' : 'url(#ah)');
      });
    }
    applyTabStops();
    renderReadiness();
    updateMinimap();
  }

  // Transient feedback has its own ring: never overwrite status, selection or focus.
  function paintHover() {
    if (!scene) return;
    scene.selectAll('.node').classed('pointed', function (d) { return d.id === HOVER_NODE; })
      .classed('hover-endpoint', function (d) { return !!HOVER_EDGE && (d.id === HOVER_EDGE.from || d.id === HOVER_EDGE.to); });
    scene.selectAll('.dagEdge').classed('pointed-edge', function (d) { return !!HOVER_EDGE && d.key === HOVER_EDGE.key; });
    var readout = document.getElementById('pointing-readout');
    if (readout) {
      readout.textContent = HOVER_EDGE ? namedRef(HOVER_EDGE.from) + ' requires ' + namedRef(HOVER_EDGE.to)
        : HOVER_NODE ? namedRef(HOVER_NODE) + ' — ' + statusMeta(NODES[HOVER_NODE].status).word : '';
      readout.hidden = !HOVER_EDGE && !HOVER_NODE;
    }
  }
  function clearHover() { HOVER_NODE = null; HOVER_EDGE = null; paintHover(); }
  function pointAt(ev) {
    if (ev.pointerType === 'touch' || ev.buttons) { clearHover(); return; }
    var node = ev.target.closest('.node');
    var id = node ? node.dataset.id : null, picked = null;
    if (!id && CUR_VIEW === 'dag' && EDGE_SAMPLES.length) {
      var matrix = EDGE_SAMPLES[0].el.getScreenCTM();
      if (!matrix) return;
      var local = new DOMPoint(ev.clientX, ev.clientY).matrixTransform(matrix.inverse());
      var scale = Math.hypot(matrix.a, matrix.b), best = 8 / scale;
      EDGE_SAMPLES.forEach(function (sample) {
        var pts = sample.pts, distance = Infinity, nearest = 0;
        for (var i = 1; i < pts.length; i++) {
          var a = pts[i-1], b = pts[i], dx = b.x-a.x, dy = b.y-a.y;
          var t = clamp(0, ((local.x-a.x)*dx+(local.y-a.y)*dy)/(dx*dx+dy*dy || 1), 1);
          var dist = Math.hypot(local.x-a.x-t*dx, local.y-a.y-t*dy);
          if (dist < distance) { distance = dist; nearest = i; }
        }
        if (distance > best + 1) return;
        // Refine on the actual rounded SVG curve, including routes <1px apart.
        var lo = Math.max(0, (nearest-2)*sample.step), hi = Math.min(sample.len, (nearest+1)*sample.step);
        function at(s) { var p = sample.el.getPointAtLength(s); return Math.hypot(local.x-p.x, local.y-p.y); }
        for (var j = 0; j < 18; j++) { var l = lo+(hi-lo)/3, r = hi-(hi-lo)/3; if (at(l) < at(r)) hi=r; else lo=l; }
        distance = at((lo+hi)/2);
        if (distance < best) { best = distance; picked = sample.edge; }
      });
    }
    if (id === HOVER_NODE && picked === HOVER_EDGE) return;
    HOVER_NODE = id; HOVER_EDGE = picked; paintHover();
  }

  function syncCanvasBox() {
    var vp = document.getElementById('ms');
    var box = CUR_VIEW === 'dag' ? BOX.dag : BOX.map;
    svgEl.setAttribute('viewBox', '0 0 ' + Math.max(1, vp.clientWidth) + ' ' + Math.max(1, vp.clientHeight));
    svgEl.setAttribute('data-canvas-w', Math.round(box.w));
    svgEl.setAttribute('data-canvas-h', Math.round(box.h));
  }

  function render() {
    clearHover();
    EDGE_SAMPLES = [];
    ensureDefs();
    viewport = svg.selectAll('g.viewport').data([0]).join('g').attr('class', 'viewport');
    scene = viewport.selectAll('g.scene').data([CUR_VIEW], String)
      .join(function (en) { return en.append('g').attr('class', function (d) { return 'scene scene-' + d; }); });
    scene.each(function (view) {
      var sc = d3.select(this);
      if (view === 'dag') drawDag(sc); else drawMap(sc);
    });
    syncCanvasBox();
    updateBar();
    updateLegend();
    updateInfo();
    applyTabStops();
    renderReadiness();
    readZoom();
    updateMinimap();
  }

  function updateInfo() {
    var el = document.getElementById('atlas-info-text');
    if (!el) return;
    el.textContent = CUR_VIEW === 'dag'
      ? 'Prerequisites flow top to bottom: an arrow points at the work that needs it. Drag to pan · Ctrl+scroll or pinch to zoom · Fit shows everything.'
      : 'Groups enclose their slices; nothing here is an arrow. Drag to pan · Ctrl+scroll or pinch to zoom · Esc leaves a focused group · / searches.';
  }

  /* --------------------------------------------------------------------- zoom */
  var zoom = d3.zoom().scaleExtent([0.01, 3])
    .filter(function (ev) {
      if (ev.type === 'wheel') return !!(ev.ctrlKey || ev.metaKey);   // plain wheel keeps scrolling the page
      if (ev.type === 'dblclick') return false;
      return ev.button === undefined || ev.button === 0;
    })
    .on('start', clearHover)
    .on('zoom', function (ev) {
      if (viewport) viewport.attr('transform', ev.transform.toString());
      readZoom(ev.transform.k);
      svgEl.classList.toggle('lod-overview', ev.transform.k < LOD_BELOW);
      updateMinimap();
    });

  /* ------------------------------------------------------------------ minimap */
  // The whole diagram in a corner, with the current viewport drawn on it. Click to move there.
  function updateMinimap() {
    var mm = document.getElementById('minimap'), vp = document.getElementById('ms');
    if (!mm || !vp) return;
    var box = CUR_VIEW === 'dag' ? BOX.dag : BOX.map;
    var W = mm.clientWidth || 172, H = mm.clientHeight || 108, pad = 6;
    var k = Math.min((W - pad * 2) / box.w, (H - pad * 2) / box.h);
    var ox = (W - box.w * k) / 2, oy = (H - box.h * k) / 2;
    var t = d3.zoomTransform(svgEl);
    var whole = t.k > 0 && (vp.clientWidth / t.k >= box.w - 1 && vp.clientHeight / t.k >= box.h - 1 && -t.x / t.k <= 0.5 && -t.y / t.k <= 0.5);
    // Keep the navigator discoverable on phones even when the current transform happens to
    // show the whole scene; it remains the touch/keyboard viewport affordance.
    mm.classList.toggle('hidden', whole && window.innerWidth > 700);
    mm.setAttribute('viewBox', '0 0 ' + W + ' ' + H);
    var items = CUR_VIEW === 'dag'
      ? ALL_IDS.map(function (id) { var p = DAG.pos[id]; return p ? { id: id, x: p.x, y: p.y, w: p.w, h: p.h } : null; }).filter(Boolean)
      : (MAP.plate ? flattenMap(MAP.plate).concat(MAP.extras) : []).map(function (it) { return { id: it.id, x: it.x, y: it.y, w: it.w, h: it.kind === 'group' ? it.head.h : it.h }; });
    var s = d3.select(mm);
    s.selectAll('rect.mm-card').data(items, function (d) { return d.id; }).join('rect')
      .attr('class', function (d) {
        var st = statusNodeClass(NODES[d.id].status);
        return 'mm-card' + (d.id === SEL ? ' mm-selected' : st === 'status-completed' ? ' mm-done' : st === 'status-active' ? ' mm-active' : (st === 'status-deferred' || st === 'status-superseded') ? ' mm-hold' : '');
      })
      .attr('x', function (d) { return ox + d.x * k; }).attr('y', function (d) { return oy + d.y * k; })
      .attr('width', function (d) { return Math.max(2, d.w * k); }).attr('height', function (d) { return Math.max(2, d.h * k); }).attr('rx', 1);
    var vx = -t.x / t.k, vy = -t.y / t.k, vw = vp.clientWidth / t.k, vh = vp.clientHeight / t.k;
    s.selectAll('rect.mm-view').data([0]).join('rect').attr('class', 'mm-view').attr('rx', 2)
      .attr('x', ox + clamp(0, vx, box.w) * k).attr('y', oy + clamp(0, vy, box.h) * k)
      .attr('width', Math.max(4, Math.min(vw, box.w - clamp(0, vx, box.w)) * k))
      .attr('height', Math.max(4, Math.min(vh, box.h - clamp(0, vy, box.h)) * k));
    mm.__map = { k: k, ox: ox, oy: oy };
  }
  function minimapJump(ev) {
    var mm = document.getElementById('minimap'), vp = document.getElementById('ms');
    var m = mm && mm.__map; if (!m) return;
    var r = mm.getBoundingClientRect(), t = d3.zoomTransform(svgEl);
    var cx = (ev.clientX - r.left - m.ox) / m.k, cy = (ev.clientY - r.top - m.oy) / m.k;
    svg.interrupt().call(zoom.transform, d3.zoomIdentity.translate(vp.clientWidth / 2 - cx * t.k, vp.clientHeight / 2 - cy * t.k).scale(t.k));
  }

  function readZoom(k) {
    var el = document.getElementById('zoom-readout');
    if (!el) return;
    var scale = (k === undefined || k === null) ? d3.zoomTransform(svgEl).k : k;
    el.textContent = Math.round(scale * 100) + '%';
  }
  function zoomBy(factor) {
    svg.interrupt().call(zoom.scaleBy, factor);
  }
  // Fit is an overview, not a reading scale. Width changes refit; height-only browser
  // chrome changes preserve the user's zoom instead of repeatedly undoing it.
  function fitDiagram() {
    var vp = document.getElementById('ms'), box = CUR_VIEW === 'dag' ? BOX.dag : BOX.map;
    // On a phone the zoom controls float over the canvas: Fit keeps the diagram clear of that
    // column, so "Fit shows everything" stays true.
    var overlay = vp.querySelector('.map-controls.overlay');
    var inset = overlay ? Math.ceil(overlay.getBoundingClientRect().width) + 14 : 0;
    var k = clamp(0.01, Math.min((vp.clientWidth - 24 - inset) / box.w, (vp.clientHeight - 24) / box.h), 1);
    var tx = (vp.clientWidth - inset - box.w * k) / 2;
    var ty = Math.max(12, (vp.clientHeight - box.h * k) / 2);
    svg.interrupt().call(zoom.transform, d3.zoomIdentity.translate(tx, ty).scale(k));
    readZoom(k);
  }
  function readableView() {
    var vp = document.getElementById('ms'), box = CUR_VIEW === 'dag' ? BOX.dag : BOX.map;
    var selected = document.querySelector('#svg .node.selected .nodeBg');
    var cx = Math.min(box.w / 2, vp.clientWidth / 2 - 20), cy = Math.min(box.h / 2, vp.clientHeight / 2 - 20);
    if (selected) {
      var t = d3.zoomTransform(svgEl), r = selected.getBoundingClientRect(), v = vp.getBoundingClientRect();
      cx = ((r.left + r.right) / 2 - v.left - t.x) / t.k;
      cy = ((r.top + r.bottom) / 2 - v.top - t.y) / t.k;
    }
    svg.interrupt().call(zoom.transform, d3.zoomIdentity.translate(vp.clientWidth / 2 - cx, vp.clientHeight / 2 - cy));
  }
  function openingCapabilityId() {
    var first = null;
    for (var i = 0; i < ALL_IDS.length; i++) {
      var id = ALL_IDS[i], n = NODES[id];
      if (!(n && n.kind === 'leaf' && n.role && n.beneficiary && n.intent === 'delivery' && n.visibleOutcome)) continue;
      if (!first) first = id;
      // Prefer a capability with a capability prerequisite so the opening frame contains
      // multiple useful increments, while retaining the declared order as the fallback.
      if (CUR_VIEW === 'dag' && (n.requires || []).some(function (req) {
        var p = NODES[req]; return p && p.kind === 'leaf' && p.role && p.beneficiary && p.intent === 'delivery' && p.visibleOutcome;
      })) return id;
    }
    return first;
  }
  function openingNarrativeGroup() {
    var id = openingCapabilityId();
    return id && (CONTAINS[id] || []).find(function (candidate) {
      return NODES[candidate] && NODES[candidate].role && NODES[candidate].visibleOutcome;
    });
  }
  // Center the first declared capability in the prerequisite view without selecting it. This
  // keeps a fresh DAG load useful while preserving the user's real selection/navigation state.
  function anchorOpeningCapability() {
    var id = openingCapabilityId(), el = id && document.querySelector('#svg .node[data-id="' + cssId(id) + '"]');
    var vp = document.getElementById('ms');
    if (!el || !vp) return false;
    var v = vp.getBoundingClientRect(), t = d3.zoomTransform(svgEl), ids = [id];
    // In the DAG, keep the capability's nearest role-bearing container in the same frame.
    // This makes the inherited beneficiary context visible without hiding prerequisites.
    var parent = (CONTAINS[id] || []).find(function (candidate) {
      return NODES[candidate] && NODES[candidate].role && NODES[candidate].visibleOutcome;
    });
    if (CUR_VIEW === 'map') {
      (CONTAINS[id] || []).slice(0, 2).forEach(function (ancestor) { if (ids.indexOf(ancestor) === -1) ids.push(ancestor); });
    } else if (parent && window.innerWidth > 700) ids.push(parent);
    if (window.innerWidth <= 700 && CUR_VIEW === 'dag') {
      var next = ALL_IDS.find(function (candidate) {
        var n = NODES[candidate];
        return n && n.kind === 'leaf' && n.role && n.beneficiary && n.intent === 'delivery' &&
          (n.requires || []).indexOf(id) !== -1;
      });
      if (next) ids.push(next);
    }
    var boxes = ids.map(function (nodeId) {
      var node = document.querySelector('#svg .node[data-id="' + cssId(nodeId) + '"]');
      if (!node) return null;
      var r = node.querySelector('.nodeBg').getBoundingClientRect();
      return { left: (r.left - v.left - t.x) / t.k, right: (r.right - v.left - t.x) / t.k,
        top: (r.top - v.top - t.y) / t.k, bottom: (r.bottom - v.top - t.y) / t.k };
    }).filter(Boolean);
    if (!boxes.length) return false;
    var bounds = boxes.reduce(function (out, b) {
      out.left = Math.min(out.left, b.left); out.right = Math.max(out.right, b.right);
      out.top = Math.min(out.top, b.top); out.bottom = Math.max(out.bottom, b.bottom); return out;
    }, { left: Infinity, right: -Infinity, top: Infinity, bottom: -Infinity });
    var k = t.k, margin = 12;
    var tx = vp.clientWidth / 2 - k * ((bounds.left + bounds.right) / 2);
    var ty = vp.clientHeight / 2 - k * ((bounds.top + bounds.bottom) / 2);
    // Clamp the context bounds to the canvas edges so titles are never cut by the first frame.
    var txMin = vp.clientWidth - margin - k * bounds.right, txMax = margin - k * bounds.left;
    var tyMin = vp.clientHeight - margin - k * bounds.bottom, tyMax = margin - k * bounds.top;
    if (txMin <= txMax) tx = clamp(txMin, tx, txMax);
    if (tyMin <= tyMax) ty = clamp(tyMin, ty, tyMax);
    svg.interrupt().call(zoom.transform, d3.zoomIdentity.translate(tx, ty).scale(k));
    return true;
  }
  // The first view is a reading view, on every screen: whole diagram if it fits at 100%,
  // otherwise 100% with declared capability content in the opening frame. Fit stays one click away.
  function openingView() {
    var vp = document.getElementById('ms'), box = CUR_VIEW === 'dag' ? BOX.dag : BOX.map;
    if (CUR_VIEW === 'dag' && !FOCUS) {
      var narrativeGroup = openingNarrativeGroup();
      if (narrativeGroup) {
        // Supporting history remains painted and navigable, but the default frame clearly
        // discloses a reversible capability focus instead of presenting legacy codes as the story.
        FOCUS = narrativeGroup;
        OPENING_DAG_FOCUS = narrativeGroup;
        render();
        box = BOX.dag;
        vp = document.getElementById('ms');
      }
    }
    if (box.w <= vp.clientWidth - 24 && box.h <= vp.clientHeight - 24) { fitDiagram(); return; }
    if (document.querySelector('#svg .node.selected')) { readableView(); return; }
    svg.interrupt().call(zoom.transform, d3.zoomIdentity.translate(12, 12));
    anchorOpeningCapability();
  }
  // Keyboard focus must never land on something off-canvas.
  function revealNode(id) {
    var el = document.querySelector('.node[data-id="' + String(id).replace(/["\\]/g, '\\$&') + '"]');
    var vp = document.getElementById('ms');
    if (!el || !vp) return;
    var b = el.getBoundingClientRect(), v = vp.getBoundingClientRect();
    if (b.left >= v.left + 4 && b.right <= v.right - 4 && b.top >= v.top + 4 && b.bottom <= v.bottom - 4) return;
    var t = d3.zoomTransform(svgEl);
    var cx = (b.left + b.right) / 2 - v.left - v.width / 2;
    var cy = (b.top + b.bottom) / 2 - v.top - v.height / 2;
    svg.interrupt().call(zoom.transform,
      d3.zoomIdentity.translate(t.x - cx, t.y - cy).scale(t.k));
  }

  /* -------------------------------------------------------- selection / views */
  function selectNode(id) {
    if (!NODES[id]) return;
    if (SEL && SEL !== id) PREVIOUS = SEL;
    SEL = id;
    clearHover();
    var changed = false;
    if (CUR_VIEW === 'map') {
      if (FOCUS && id !== FOCUS && descendants(FOCUS).indexOf(id) === -1) { FOCUS = null; changed = true; }
      Object.keys(COLLAPSED).forEach(function (parent) {
        if (descendants(parent).indexOf(id) !== -1) { delete COLLAPSED[parent]; changed = true; }
      });
    }
    if (changed) { render(); openingView(); } else refreshStates();
    refreshSearchCount();
    showInsp(id);
    renderReviews(id);
  }
  // A selection made on the canvas retires the last search: the box no longer claims a match
  // that has nothing to do with what is selected.
  var refreshSearchCount = function () {};
  var resetSearch = function () {};
  function toggleCollapse(id) {
    if (!isContainer(id)) return;
    if (COLLAPSED[id]) delete COLLAPSED[id]; else COLLAPSED[id] = 1;
    // When hiding the selected child, select its visible container explicitly.
    if (COLLAPSED[id] && descendants(id).indexOf(SEL) !== -1) SEL = id;
    render();
    fitDiagram();
    if (SEL) showInsp(SEL);
  }
  function setFocus(id) {
    FOCUS = (id && NODES[id]) ? id : null;
    if (FOCUS) { delete COLLAPSED[FOCUS]; SEL = FOCUS; }
    render();
    fitDiagram();
    if (FOCUS && CUR_VIEW === 'map' && window.innerWidth <= 700) {
      var vp = document.getElementById('ms'), k = Math.min(1, (vp.clientWidth - 24) / BOX.map.w);
      svg.call(zoom.transform, d3.zoomIdentity.translate(12, 12).scale(k));
    }
    if (SEL) { refreshSearchCount(); showInsp(SEL); renderReviews(SEL); }
    window.requestAnimationFrame(function () { document.getElementById('map-panel').scrollIntoView({ block: 'start' }); });
  }
  function updateBar() {
    var bar = document.getElementById('canvas-bar');
    var crumb = document.getElementById('crumb');
    var note = document.getElementById('bar-note');
    if (!bar) return;
    if (!FOCUS || !NODES[FOCUS]) {
      bar.classList.remove('visible');
      crumb.textContent = '';
      if (note) note.textContent = '';
      return;
    }
    bar.classList.add('visible');
    var chain = [], cur = FOCUS, guard = 0;
    while (cur && NODES[cur] && guard++ < 40) { chain.unshift(NODES[cur].label); cur = (CONTAINS[cur] || [])[0]; }
    if (chain.length > 1) chain.shift();
    crumb.innerHTML = chain.map(function (c) { return '<span class="crumb">' + esc(c) + '</span>'; }).join('<span class="crumb-sep"> / </span>');
    if (note) note.textContent = CUR_VIEW === 'dag'
      ? 'Group highlighted. Every prerequisite remains visible.'
      : 'Focused group. Return to see all project work.';
  }
  function switchView(view) {
    if (view === 'map' && OPENING_DAG_FOCUS && FOCUS === OPENING_DAG_FOCUS) {
      FOCUS = null;
      OPENING_DAG_FOCUS = null;
    }
    CUR_VIEW = view;
    document.getElementById('view-map').classList.toggle('active', view === 'map');
    document.getElementById('view-dag').classList.toggle('active', view === 'dag');
    document.getElementById('view-map').setAttribute('aria-pressed', String(view === 'map'));
    document.getElementById('view-dag').setAttribute('aria-pressed', String(view === 'dag'));
    render();
    openingView();
    if (SEL && NODES[SEL]) { showInsp(SEL); renderReviews(SEL); }
    else {
      SEL = null;
      var insp = document.getElementById('insp');
      if (insp) { insp.innerHTML = EMPTY_INSPECTOR; placeControls(); }
    }
  }

  /* ------------------------------------------------------------------- legend */
  // The status key is drawn with the same d3 symbols the canvas uses, so the legend and the
  // painted shapes cannot drift apart.
  function symbolSwatch(st) {
    return '<i class="sw"><svg width="12" height="12" viewBox="-7 -7 14 14" aria-hidden="true">' +
      '<path class="glyph-' + st.sym + '" d="' + symbolPath(st) + '"/></svg></i>';
  }
  function updateLegend() {
    var status = [
      { st: statusMeta('completed'), t: 'Completed' }, { st: statusMeta('in-progress'), t: 'In progress' },
      { st: statusMeta('in-progress-awaiting-review'), t: 'Awaiting review' }, { st: statusMeta('planned'), t: 'Planned' },
      { st: statusMeta('deferred'), t: 'Deferred / superseded' }];
    var items = CUR_VIEW === 'dag'
      ? [{ ln: true, t: 'Prerequisite → the work that needs it' }].concat(status,
         Object.keys(HUBS).length ? [{ sep: true }, { faint: true, t: 'Faint: a prerequisite many slices share (named on the card)' }] : [])
      : status.concat([{ sep: true },
         { c: '#F2F3FB', s: '#4B4FA8', t: 'Prototype group', tip: 'A container whose children explore or prototype' },
         { c: '#F1F7F6', s: '#0F7A6C', t: 'Delivery group', tip: 'A container of deliverable work' },
         { c: '#FFFFFF', s: '#C6D2DF', t: 'Work slice', tip: 'One unit of work' },
         { c: '#F4F6F9', s: '#7C8BA0', t: 'Reference', tip: 'Existing material the work depends on' },
         { c: '#FBF5EC', s: '#A15C0B', t: 'Owner session', tip: 'A decision the owner makes, not a delivery' },
         { note: true, t: 'Groups enclose; they never point' }]);
    document.getElementById('map-legend').innerHTML = items.map(function (it) {
      if (it.sep) return '<span class="legend-sep" aria-hidden="true"></span>';
      if (it.ln) return '<span><i class="ln"></i>' + esc(it.t) + '</span>';
      if (it.faint) return '<span><i class="ln" style="opacity:.32"></i>' + esc(it.t) + '</span>';
      if (it.st) return '<span>' + symbolSwatch(it.st) + esc(it.t) + '</span>';
      if (it.note) return '<span class="legend-note">' + esc(it.t) + '</span>';
      var style = 'background:' + (it.c || '#FFFFFF') + ';border-color:' + (it.s || '#7C8BA0');
      return '<span title="' + escAttr(it.tip || '') + '"><i style="' + style + '"></i>' + esc(it.t) + '</span>';
    }).join('');
  }

  /* -------------------------------------------------------------- readiness */
  // Ready now / in progress / blocked / on hold / done, computed from requires + status.
  // Containers are enclosure, not work, so only leaves, references and owner sessions count.
  function readiness() {
    var out = { ready: [], active: [], blocked: [], hold: [], done: [] };
    ALL_IDS.filter(function (id) { return !isContainer(id); }).forEach(function (id) {
      var n = NODES[id], s = n.status;
      if (s === 'completed') { out.done.push({ id: id }); return; }
      if (s === 'deferred' || s === 'superseded') { out.hold.push({ id: id }); return; }
      if (s === 'in-progress' || s === 'in-progress-design' || s === 'in-progress-awaiting-review') { out.active.push({ id: id }); return; }
      var unmet = (n.requires || []).filter(function (r) { return NODES[r] && NODES[r].status !== 'completed'; });
      if (unmet.length) out.blocked.push({ id: id, by: unmet[0], more: unmet.length - 1 });
      else out.ready.push({ id: id });
    });
    return out;
  }
  var readinessExpanded = {};
  var readinessOpen = false;
  function renderReadiness() {
    var el = document.getElementById('ready-strip');
    if (!el) return;
    var r = readiness();
    function chip(it) {
      return '<button type="button" class="ready-chip' + (it.id === SEL ? ' selected' : '') + '" data-id="' + escAttr(it.id) + '">' +
        (shortRef(it.id) ? '<span class="chip-ref">' + esc(shortRef(it.id)) + '</span>' : '') + esc(NODES[it.id].label) +
        (it.by ? '<span class="chip-because">needs ' + esc(shortRef(it.by) || NODES[it.by].label) + (it.more ? ' +' + it.more : '') + '</span>' : '') + '</button>';
    }
    function group(cls, label, list, max) {
      var limit = readinessExpanded[cls] ? list.length : max;
      var shown = list.slice(0, limit), rest = list.length - shown.length;
      return '<span class="ready-group ' + cls + '"><span class="ready-label">' + esc(label) + '</span><span class="ready-count">' + list.length + '</span>' +
        shown.map(chip).join('') + (max > 0 && rest > 0 ? '<button type="button" class="ready-more" data-ready-more="' + escAttr(cls) + '" aria-label="Show all ' + escAttr(label.toLowerCase()) + ' slices">+' + rest + ' more</button>' : '') + '</span>';
    }
    var h = r.ready.length ? group('ready-now', 'Ready now', r.ready, 4)
      : '<span class="ready-group ready-now"><span class="ready-label">Ready now</span><span class="ready-count">0</span><span class="ready-empty">nothing can start until a review or prerequisite clears</span></span>';
    if (r.active.length) h += group('ready-active', 'In progress', r.active, 3);
    if (r.blocked.length) h += group('ready-blocked', 'Blocked', r.blocked, 3);
    if (r.hold.length) h += group('ready-hold', 'On hold', r.hold, 2);
    h += group('ready-done', 'Done', r.done, 0);
    var counts = ['ready-now', 'ready-active', 'ready-blocked', 'ready-hold', 'ready-done'].map(function (cls) {
      var key = cls === 'ready-now' ? 'ready' : cls.replace('ready-', '');
      var list = r[key] || [];
      var label = { 'ready-now': 'Ready now', 'ready-active': 'In progress', 'ready-blocked': 'Blocked', 'ready-hold': 'On hold', 'ready-done': 'Done' }[cls];
      return label + ' ' + list.length;
    }).join(' · ');
    el.innerHTML = '<button type="button" class="ready-toggle" aria-expanded="' + readinessOpen + '">Readiness: ' + esc(counts) + ' · Show technical status</button>' +
      '<div class="ready-details"' + (readinessOpen ? '' : ' hidden') + '>' + h + '</div>';
    el.querySelector('.ready-toggle').addEventListener('click', function () {
      readinessOpen = !readinessOpen;
      renderReadiness();
    });
    el.querySelectorAll('.ready-chip').forEach(function (b) {
      b.addEventListener('click', function () { selectNode(b.dataset.id); revealNode(b.dataset.id); resetSearch(); });
    });
    el.querySelectorAll('.ready-more').forEach(function (b) {
      b.addEventListener('click', function () { readinessExpanded[b.dataset.readyMore] = true; renderReadiness(); });
    });
  }

  /* ---------------------------------------------------------------- inspector */
  function showInsp(id) {
    var insp = document.getElementById('insp');
    var n = NODES[id];
    if (!id || !n) { insp.innerHTML = ''; return; }
    insp.hidden = false;
    insp.removeAttribute('aria-hidden');
    var s = nodeStyle(n);
    var meta = statusMeta(n.status);
    var roll = isContainer(id) ? rollup(id) : null;
    var statusPill = roll
      ? '<span class="pill-stat stat-rollup rollup" title="Recorded container status: ' + escAttr(meta.word) + '">' + esc(roll.word) +
        '<span class="rollup-bar" aria-hidden="true"><i style="width:' + (roll.total ? Math.round(100 * roll.done / roll.total) : 0) + '%"></i></span></span>'
      : '<span class="pill-stat ' + meta.cls + '">' + esc(meta.word) + '</span>';
    var h = '<div class="meta">' +
      '<span class="pill ' + s.cls + '">' + esc(({leaf:'Work slice','prototype-container':'Prototype group','delivery-container':'Work group',reference:'Reference',grilling:'Owner decision',checkpoint:'Checkpoint'})[n.kind] || n.kind || 'Work') + '</span>' +
      statusPill +
      '</div><div class="identity-row"><strong class="slice-reference" title="Permanent reference, not a position">' + esc(shortRef(id) || id) + '</strong>' +
      '<button type="button" class="copy-reference" data-reference="' + escAttr(qualifiedRef(id)) + '">Copy reference</button>' +
      '</div><span class="copy-status" role="status"></span><h2 class="slice-title">' + esc(n.label) + '</h2>' +
      (n.usefulOutcome ? '<div class="outcome-lead"><h3>Useful outcome</h3><p>' + esc(n.usefulOutcome) + '</p></div>' : '') +
      ((Array.isArray(n.acceptanceEvidence) && n.acceptanceEvidence.length) ? '<div class="outcome-lead"><h3>Acceptance proof</h3><ul>' + n.acceptanceEvidence.map(function (x) { return '<li>' + esc(x) + '</li>'; }).join('') + '</ul></div>' : '') +
      (n.cutImpact ? '<div class="outcome-lead"><h3>If cut</h3><p>' + esc(n.cutImpact) + '</p></div>' : '') +
      '<div class="mobile-inspector-actions" aria-label="Mobile detail controls">' +
      '<button type="button" class="mobile-detail-hide">Hide details</button>' +
      '<button type="button" class="mobile-detail-return">Return to diagram</button>' +
      '</div>';

    var groupId = isContainer(id) ? id : (CONTAINS[id] || [])[0];
    if (PREVIOUS && PREVIOUS !== id) h += '<button type="button" class="inspect-back" data-id="' + escAttr(PREVIOUS) + '">← Back: ' + esc(NODES[PREVIOUS].label) + (shortRef(PREVIOUS) ? ' (' + esc(shortRef(PREVIOUS)) + ')' : '') + '</button>';
    if (groupId) {
      h += '<div class="group-actions"><button type="button" class="group-focus" data-group="' + escAttr(groupId) + '">Focus ' + (groupId === id ? 'this group' : 'parent group') + '</button>';
      if (isContainer(id) && id !== ROOT) h += '<button type="button" class="group-collapse" aria-expanded="' + (!COLLAPSED[id]) + '">' + (COLLAPSED[id] ? 'Expand group' : 'Collapse group') + '</button>';
      h += '</div>';
      if (COLLAPSED[id]) h += '<p class="muted">' + descendants(id).length + ' slices hidden in the Map. Expand the group to see them.</p>';
    }

    function fld(label, content) {
      var technical = ['Machine ID','Recorded status','Description','Evidence / output','Input','Expected output','Verification','Status line','Completed outcome (history)','Completion route','Review history','Learning','Stable IDs','Execution intent design','Lifecycle','Approval policy'].indexOf(label) !== -1;
      return content ? '<div class="field' + (technical ? ' technical-field' : '') + '"><h3>' + esc(label) + '</h3>' + content + '</div>' : '';
    }
    function lnk(rid) { return '<a href class="nl" data-id="' + escAttr(rid) + '">' + esc(namedRef(rid)) + '</a>'; }

    h += fld('Description', esc(n.description || ''));
    h += fld('Machine ID', '<code>' + esc(id) + '</code>');
    if (roll) h += fld('Recorded status', esc(meta.word) + ' <span class="muted">(container record; the rollup above counts enclosed slices)</span>');
    h += fld('Evidence / output', esc(n.evidence || ''));
    h += fld('Input', esc(n.input || ''));
    h += fld('Expected output', esc(n.expectedOutput || ''));
    h += fld('Verification', esc(n.verification || ''));

    if (n.requires && n.requires.length) h += fld('Requires', '<ul>' + n.requires.map(function (r) { return '<li>' + lnk(r) + '</li>'; }).join('') + '</ul>');
    var reqBy = [];
    EDGES.requires.forEach(function (e) { if (e.to === id) reqBy.push(e.from); });
    if (reqBy.length) h += fld('Required by', '<ul>' + reqBy.map(function (r) { return '<li>' + lnk(r) + '</li>'; }).join('') + '</ul>');
    var parents = CONTAINS[id] || [];
    if (parents.length) h += fld('Container', '<ul>' + parents.map(function (p) { return '<li>' + lnk(p) + '</li>'; }).join('') + '</ul>');
    if (n.contains && n.contains.length) h += fld('Contains', '<ul>' + n.contains.map(function (c) { return '<li>' + lnk(c) + '</li>'; }).join('') + '</ul>');

    if (n.completionRoute) h += fld('Status line', esc(n.completionRoute));
    if (n.completedOutcome) h += fld('Completed outcome (history)', esc(n.completedOutcome));

    if (n.route) {
      var rt = n.route, rth = '';
      if (rt.completionEvidence && rt.completionEvidence.length) rth += '<strong>Completion evidence / checks:</strong><ul>' + rt.completionEvidence.map(function (x) { return '<li>' + esc(x) + '</li>'; }).join('') + '</ul>';
      if (rt.reviewPlanned) rth += '<strong>Review:</strong> ' + esc(rt.reviewPlanned) + '<br>';
      if (rt.next && rt.next.length) rth += '<strong>Candidate successors (eligibility NOT proven):</strong> <ul>' + rt.next.map(function (x) { return '<li>' + lnk(x) + '</li>'; }).join('') + '</ul>' + '<span class="muted">Candidates only — prerequisites, active holds and separate authorization decide eligibility. A listed successor whose prerequisite is still planned, or whose change needs its own owner authorization, is NOT eligible yet.</span>';
      if (rt.onRejection) rth += '<strong>If rejected / needs discussion:</strong> ' + esc(rt.onRejection);
      h += fld('Completion route', rth);
    }

    // Review rounds stay with the slice: dated, in order, newest marked, raw verdict and parent
    // decision on the same row. This is the summary view; the global panel is for browsing.
    var nodeReviews = (WORK.reviews || []).filter(function (r) { return (r.nodeIds || []).indexOf(id) >= 0; });
    if (nodeReviews.length) h += fld('Review rounds', '<ul class="round-list">' + roundRowsHtml(nodeReviews) + '</ul>');

    // Generic artifact links — built from records only, never from a per-node code path.
    if (n.artifacts && n.artifacts.length) {
      var ath = '<ul>' + n.artifacts.map(function (a) {
        a = a || {};
        var item = '<li data-artifact-id="' + escAttr(a.id) + '">';
        if (safeArtifactPath(a.path)) {
          item += '<a href="' + escAttr(a.path) + '" target="_blank" rel="noopener">' + esc(a.label || a.path) + '</a>';
        } else {
          item += '<em>' + esc(a.label || a.path || '(unnamed)') + '</em> <span class="muted">unsafe or absolute path — not linkable</span>';
        }
        if (a.kind) item += '<div class="muted technical-field">' + esc(a.kind) + '</div>';
        if (a.source) item += '<div class="muted technical-field">source: <code>' + esc(a.source) + '</code></div>';
        if (a.sha256) item += '<div class="muted technical-field">sha256 <code>' + esc(a.sha256) + '</code></div>';
        return item + '</li>';
      }).join('') + '</ul>';
      h += fld('Artifacts', ath);
    }

    if (n.learningDisposition) {
      var ld = '<strong>' + esc(n.learningDisposition) + '</strong>';
      if (n.learningDetail) ld += '<br>' + esc(n.learningDetail);
      h += fld('Learning', ld);
    }
    if (n.stableIds && n.stableIds.length) h += fld('Stable IDs', n.stableIds.map(function (x) { return '<code>' + esc(x) + '</code>'; }).join('<br>'));
    if (n.executionIntentDesign) {
      var ei = n.executionIntentDesign;
      var eih = '<strong>' + esc(ei.note || 'Design requirement') + '</strong><br>';
      if (ei.interactiveVsUnattended) eih += esc(ei.interactiveVsUnattended) + '<br>';
      if (ei.evaluateEndToEnd) eih += esc(ei.evaluateEndToEnd) + '<br>';
      if (ei.inputs && ei.inputs.length) eih += '<strong>Inputs:</strong> ' + esc(ei.inputs.join(', ')) + '<br>';
      if (ei.measurement) eih += esc(ei.measurement) + '<br>';
      if (ei.constraints && ei.constraints.length) eih += '<strong>Constraints:</strong><ul>' + ei.constraints.map(function (c) { return '<li>' + esc(c) + '</li>'; }).join('') + '</ul>';
      h += fld('Execution intent design', eih);
    }
    if (n.grillingBrief) {
      var gb = n.grillingBrief;
      function sliceRefs(list) {
        return '<ul>' + list.map(function (x) {
          if (x && typeof x === 'object') return '<li>' + lnk(x.sliceId) + ' — ' + esc(x.reason || '') + '</li>';
          return '<li>' + lnk(x) + '</li>';
        }).join('') + '</ul>';
      }
      var gbh = '<strong>Single decision:</strong> ' + esc(gb.purpose || '') + '<br>';
      if (gb.decisionBoundaries) gbh += '<strong>Boundaries:</strong> ' + esc(gb.decisionBoundaries) + '<br>';
      if (gb.questions) gbh += '<strong>Concrete questions:</strong><ul>' + gb.questions.map(function (q) { return '<li>' + esc(q) + '</li>'; }).join('') + '</ul>';
      if (gb.evidence && gb.evidence.length) gbh += '<strong>Evidence to bring:</strong><ul>' + gb.evidence.map(function (ev) { return '<li>' + esc(ev) + '</li>'; }).join('') + '</ul>';
      if (gb.affectedSlices) gbh += '<strong>Affected slices (with reasons):</strong>' + sliceRefs(gb.affectedSlices);
      if (gb.unaffectedSlices) gbh += '<strong>Explicitly unaffected (with reasons):</strong>' + sliceRefs(gb.unaffectedSlices);
      if (gb.outcomeBranches) gbh += '<strong>Outcome branches:</strong><ul>' + gb.outcomeBranches.map(function (o) { return '<li>' + esc(o) + '</li>'; }).join('') + '</ul>';
      if (gb.completionOutputs) gbh += '<strong>Completion outputs:</strong> ' + esc(gb.completionOutputs);
      h += fld('Planned owner grilling session', gbh);
    }
    if (id === ROOT && WORK.lifecycle) {
      var lyc = WORK.lifecycle;
      h += fld('Lifecycle', esc((lyc.phases || []).join('; ')));
      if (lyc.approvalPolicy) h += fld('Approval policy', esc(lyc.approvalPolicy));
    }

    if (nodeReviews.length) h += '<button type="button" class="inspect-reviews">Browse all review history (' + nodeReviews.length + (nodeReviews.length === 1 ? ' record' : ' records') + ' for this slice)</button>';
    insp.innerHTML = h;
    // Promote recorded files and the history shortcut above relationships, without
    // inferring which arbitrary artifact is an accepted deliverable or deleting fields.
    var firstField = insp.querySelector('.field');
    var files = Array.from(insp.querySelectorAll('.field')).find(function (f) { return f.querySelector('h3').textContent === 'Artifacts'; });
    if (files) {
      files.classList.add('files-lead'); insp.insertBefore(files, firstField);
      var historyRows = Array.from(files.querySelectorAll('li[data-artifact-id]')).filter(function (li) {
        var a = li.querySelector('a'); return a && /(^|\/)reviews\//.test(a.getAttribute('href'));
      });
      if (historyRows.length) {
        var details = document.createElement('details'); details.className = 'artifact-history';
        var summary = document.createElement('summary'); summary.textContent = 'Review files and provenance (' + historyRows.length + ')';
        var list = document.createElement('ul'); historyRows.forEach(function (li) { list.appendChild(li); });
        details.append(summary, list); files.appendChild(details);
      }
    }
    var navigation = insp.querySelector('.group-actions');
    var mobileActions = insp.querySelector('.mobile-inspector-actions');
    var heading = document.querySelector('.details-heading');
    if (window.innerWidth <= 700 && heading) {
      // On a phone the sheet actions live in the sheet's heading row, so the body keeps the
      // outcome and the first useful file in view without scrolling.
      var stale = heading.querySelector('.mobile-inspector-actions'); if (stale) stale.remove();
      heading.appendChild(mobileActions);
    } else if (navigation) navigation.appendChild(mobileActions);
    if (navigation) {
      var prior = insp.querySelector('.inspect-back'); if (prior) navigation.prepend(prior);
      // Outcome → useful files → navigation: the files stay in the first screenful on a phone.
      if (files) files.parentNode.insertBefore(navigation, files.nextSibling);
    }
    // The inline rounds lead the fields; the global-panel shortcut follows them.
    var roundsField = Array.from(insp.querySelectorAll('.field')).find(function (f) { return f.querySelector('h3').textContent === 'Review rounds'; });
    if (roundsField) insp.insertBefore(roundsField, firstField);
    var historyShortcut = insp.querySelector('.inspect-reviews');
    if (historyShortcut) insp.insertBefore(historyShortcut, roundsField ? roundsField.nextSibling : firstField);
    var copy = insp.querySelector('.copy-reference');
    copy.addEventListener('click', function () {
      var value = copy.dataset.reference, status = insp.querySelector('.copy-status');
      function fallback() {
        var input = document.createElement('input'); input.value = value; input.readOnly = true;
        input.className = 'reference-fallback'; input.setAttribute('aria-label', 'Reference to copy');
        status.replaceChildren(input); input.focus(); input.select();
        var ok = false; try { ok = document.execCommand('copy'); } catch (_) {}
        if (ok) { status.textContent = 'Copied ' + value; copy.focus(); }
        else { var hint = document.createElement('span'); hint.textContent = 'Select and copy this reference.'; status.appendChild(hint); }
      }
      if (navigator.clipboard && navigator.clipboard.writeText) navigator.clipboard.writeText(value).then(function () { status.textContent = 'Copied ' + value; }, fallback);
      else fallback();
    });
    var back = insp.querySelector('.inspect-back');
    if (back) back.addEventListener('click', function () { selectNode(back.dataset.id); readableView(); });
    insp.scrollTop = 0; // A newly selected slice starts at its title, not the previous slice's scroll offset.
    var reviewButton = insp.querySelector('.inspect-reviews');
    if (reviewButton) reviewButton.addEventListener('click', function () {
      var panel = document.getElementById('reviews-panel');
      panel.open = true;
      panel.scrollIntoView({ block: 'start' });
      document.getElementById('reviews-toggle').focus({ preventScroll: true });
    });
    insp.querySelectorAll('.nl').forEach(function (a) {
      a.addEventListener('click', function (ev) { ev.preventDefault(); selectNode(a.dataset.id); revealNode(a.dataset.id); });
    });
    var focusButton = insp.querySelector('.group-focus');
    var collapseButton = insp.querySelector('.group-collapse');
    if (focusButton) focusButton.addEventListener('click', function () { setFocus(focusButton.dataset.group); });
    if (collapseButton) collapseButton.addEventListener('click', function () {
      toggleCollapse(id);
      var replacement = insp.querySelector('.group-collapse');
      if (replacement) replacement.focus({ preventScroll: true });
    });
    var reopen = document.getElementById('mobile-detail-reopen');
    var hideButton = mobileActions.querySelector('.mobile-detail-hide');
    var returnButton = mobileActions.querySelector('.mobile-detail-return');
    if (hideButton) hideButton.addEventListener('click', function () {
      insp.hidden = true;
      insp.setAttribute('aria-hidden', 'true');
      if (reopen) reopen.hidden = false;
    });
    if (returnButton) returnButton.addEventListener('click', function () {
      document.getElementById('map-panel').scrollIntoView({ block: 'start', behavior: 'auto' });
    });
    if (reopen) reopen.hidden = true;
    if (window.innerWidth <= 700) {
      // The phone inspector is a fixed bottom sheet: nothing scrolls away, the diagram stays above it.
      insp.hidden = false;
      insp.removeAttribute('aria-hidden');
    }
  }
  // Review rows shared by the inspector and the global panel.
  function reviewDate(r) {
    var m = String(r.date || r.sourceReport || r.gateDir || r.report || '').match(/(\d{4})-(\d{2})-(\d{2})/);
    if (!m) return null;
    var months = ['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'];
    return { iso: m[0], text: Number(m[3]) + ' ' + months[Number(m[2]) - 1] + ' ' + m[1] };
  }
  function verdictClass(r) {
    if (r.pending) return 'verdict-pending';
    if (reviewIsParent(r)) return 'verdict-parent';
    return /^PASS/.test(r.rawVerdict || '') ? 'verdict-pass' : /FAIL/.test(r.rawVerdict || '') ? 'verdict-fail' : 'verdict-parent';
  }
  function roundRowsHtml(list) {
    var rounds = 0, latest = null;
    list.forEach(function (r) { if (!r.pending) latest = r; });
    return list.map(function (r) {
      var parent = reviewIsParent(r), d = reviewDate(r);
      var label = r.pending ? 'Pending' : parent ? 'Parent decision' : 'Round ' + (++rounds);
      var verdict = r.pending ? 'awaiting a report' : esc(r.rawVerdict);
      var link = r.pending || !r.report ? '<span class="round-verdict ' + verdictClass(r) + '">' + verdict + '</span>'
        : '<a class="round-verdict ' + verdictClass(r) + '" href="' + escAttr(r.report) + '" target="_blank" rel="noopener" title="verbatim copy of ' + escAttr(r.sourceReport || '') + '">' + verdict + '</a>';
      return '<li class="round-row" data-review-id="' + escAttr(r.id) + '"><span class="round-n">' + label + '</span>' + link +
        '<span class="round-date">' + (d ? esc(d.text) : 'date not recorded') + '</span>' +
        (r.tier && !parent ? '<span class="round-tier">' + esc(r.tier) + '</span>' : '') +
        (r === latest ? '<span class="round-latest">latest</span>' : '') +
        (r.disposition ? '<span class="rev-disposition ' + (r.disposition === 'addressed' || r.disposition === 'accepted' ? 'rev-addressed' : r.disposition === 'superseded' ? 'rev-superseded' : 'rev-open') + '">' + esc(r.disposition) + '</span>' : '') +
        (r.findings || []).map(function (f) { return '<span class="rev-finding">[' + esc(f.severity) + '] ' + esc(f.text) + '</span>'; }).join('') +
        (r.parentDisposition ? '<span class="round-parent">Parent: ' + esc(r.parentDisposition) + '</span>' : '') +
        '<span class="round-run">' + esc(r.run) + (r.pending ? ' · report pending — no file linked' : '') + '</span></li>';
    }).join('');
  }

  /* ---------------------------------------------------------- review registry */
  var REVIEW_BY_ID = {};
  (WORK.reviews || []).forEach(function (r) {
    // Validation rejects duplicates; retaining the first here prevents a copied malformed fixture
    // from silently changing which authoritative run the inspector resolves.
    if (!Object.prototype.hasOwnProperty.call(REVIEW_BY_ID, r.id)) REVIEW_BY_ID[r.id] = r;
  });
  function reviewIsParent(r) { return !!r && (r.tier === 'parent' || /parent/i.test(r.id || '') || /^parent[- ]/i.test(r.rawVerdict || '')); }
  function reviewScopeKey(r) { var ids = (r.nodeIds || []).slice().sort(); return ids.length ? ids.join('|') : 'unscoped'; }
  function reviewScopeLabel(key) {
    if (key === 'unscoped') return 'Unscoped review history';
    return key.split('|').map(function (id) { return NODES[id] ? NODES[id].label : id; }).join(' · ');
  }
  function reviewRecordHtml(r) {
    var dispCls = r.disposition === 'addressed' || r.disposition === 'accepted' ? 'rev-addressed' : (r.disposition === 'superseded' ? 'rev-superseded' : 'rev-open');
    var run = r.pending ? '<em>' + esc(r.run) + '</em>' : '<a href="' + escAttr(r.report) + '" target="_blank" rel="noopener" title="verbatim copy of ' + escAttr(r.sourceReport || '') + '">' + esc(r.run) + '</a>';
    var d = reviewDate(r);
    var hh = '<div class="review-round"><div class="rev-verdict">' + run + ' <strong>' + esc(r.rawVerdict) + '</strong> <span class="' + dispCls + ' rev-disposition">' + esc(r.disposition) + '</span>' + (d ? ' <span class="rev-meta">' + esc(d.text) + '</span>' : '') + '</div>';
    hh += '<div class="rev-meta">' + esc(r.scope || '') + (r.tier ? ' · ' + esc(r.tier) : '') + (r.pending ? ' · report pending — no file linked' : '') + '</div>';
    (r.findings || []).forEach(function (f) { hh += '<span class="rev-finding">[' + esc(f.severity) + '] ' + esc(f.text) + '</span>'; });
    if (r.parentDisposition) hh += '<div class="rev-meta">Parent disposition: ' + esc(r.parentDisposition) + '</div>';
    return hh + '</div>';
  }
  function renderReviews(selectedId) {
    var body = document.getElementById('reviews-body');
    var recs = WORK.reviews || [];
    if (!recs.length) { body.innerHTML = '<div class="inbox-empty">No review runs recorded.</div>'; return; }
    var groups = {};
    recs.forEach(function (r) { var key = reviewScopeKey(r); if (!groups[key]) groups[key] = []; groups[key].push(r); });
    var html = '';
    // Selected slice first, then most recently reviewed, then name: the reader's order, not the key's.
    function latestIso(rs) { return rs.reduce(function (m, r) { var d = reviewDate(r); return d && d.iso > m ? d.iso : m; }, ''); }
    var keys = Object.keys(groups).sort(function (a, b) {
      var sa = selectedId && groups[a].some(function (r) { return (r.nodeIds || []).indexOf(selectedId) >= 0; }) ? 1 : 0;
      var sb = selectedId && groups[b].some(function (r) { return (r.nodeIds || []).indexOf(selectedId) >= 0; }) ? 1 : 0;
      if (sa !== sb) return sb - sa;
      var da = latestIso(groups[a]), db = latestIso(groups[b]);
      if (da !== db) return da < db ? 1 : -1;
      return reviewScopeLabel(a) < reviewScopeLabel(b) ? -1 : 1;
    });
    keys.forEach(function (key) {
      var rs = groups[key], rounds = rs.filter(function (r) { return !r.pending && !reviewIsParent(r); });
      var pending = rs.filter(function (r) { return !!r.pending; }), par = rs.filter(reviewIsParent);
      var selected = selectedId && rs.some(function (r) { return (r.nodeIds || []).indexOf(selectedId) >= 0; });
      var last = latestIso(rs);
      html += '<details class="review-group' + (selected ? ' selected-group' : '') + '" data-review-scope="' + escAttr(key) + '"' + (selected ? ' open' : '') + '><summary>' + esc(reviewScopeLabel(key));
      html += '<span class="review-count">' + rounds.length + ' completed round' + (rounds.length === 1 ? '' : 's') + '</span>';
      if (pending.length) html += '<span class="review-count pending">' + pending.length + ' pending</span>';
      if (par.length) html += '<span class="review-count parent">' + par.length + ' parent decision' + (par.length === 1 ? '' : 's') + '</span>';
      if (last) html += '<span class="review-count latest">last ' + esc(last) + '</span>';
      html += '</summary><div class="review-group-body">';
      if (rounds.length) html += '<section class="review-section"><h4>Review rounds</h4>' + rounds.map(reviewRecordHtml).join('') + '</section>';
      if (pending.length) html += '<section class="review-section"><h4>Pending reviews</h4>' + pending.map(reviewRecordHtml).join('') + '</section>';
      if (par.length) html += '<section class="review-section"><h4>Parent adjudications</h4>' + par.map(reviewRecordHtml).join('') + '</section>';
      html += '</div></details>';
    });
    body.innerHTML = html;
  }

  function renderInbox() {
    var body = document.getElementById('inbox-body');
    if (!IMPROV.proposals || IMPROV.proposals.length === 0) {
      body.innerHTML = '<div class="inbox-empty">No improvement proposals yet. Per-slice learnings that are ordinary local fixes stay local.</div>';
      return;
    }
    var html = '';
    IMPROV.proposals.forEach(function (p) {
      var cl = 'label-' + p.status;
      if (p.status === 'candidate') cl = 'label-candidate';
      else if (p.status === 'rejected-local') cl = 'label-rejected';
      else if (p.status === 'adopted') cl = 'label-adopted';
      else cl = 'label-local';
      html += '<div class="proposal-card"><h4><span class="label ' + cl + '">' + esc(p.status) + '</span> ' + esc(p.title) + '</h4>';
      if (p.problem) html += '<p>' + esc(p.problem) + '</p>';
      if (p.minimalChange) html += '<p><strong>Suggested change:</strong> ' + esc(p.minimalChange) + '</p>';
      html += '<p><strong>Validation:</strong> ' + esc(p.validationState || 'Not recorded') + (p.ownerReview ? ' · owner reviewed' : ' · awaiting owner review') + '</p>';
      html += '<details class="proposal-details"><summary>Evidence and scope</summary>';
      if (p.category) html += '<p><strong>Category:</strong> ' + esc(p.category) + '</p>';
      html += '<p><strong>Source:</strong> <code>' + esc(p.sourceSliceId) + '</code></p>';

      if (p.whyNonObvious) html += '<p><strong>Why non-obvious:</strong> ' + esc(p.whyNonObvious) + '</p>';
      if (p.whyHighLeverage) html += '<p><strong>High-leverage because:</strong> ' + esc(p.whyHighLeverage) + '</p>';

      if (p.failureReplayAndControl) {
        html += '<p><strong>Failure replay:</strong> ' + esc(p.failureReplayAndControl.replay || '') + '</p>';
        html += '<p><strong>Positive control:</strong> ' + esc(p.failureReplayAndControl.control || '') + '</p>';
      }

      if (p.applicableFutureSliceIds && p.applicableFutureSliceIds.length) {
        html += '<p><strong>Applies to future:</strong> ' + p.applicableFutureSliceIds.map(function (s) { return '<code>' + esc(s) + '</code>'; }).join(', ') + '</p>';
      }
      html += '</details></div>';
    });
    body.innerHTML = html;
  }

  function initSearch() {
    var input = document.getElementById('slice-search'), results = document.getElementById('search-results');
    var count = document.getElementById('search-count');
    function hide() { results.hidden = true; }
    function show() {
      var q = input.value.trim().toLowerCase();
      if (!q) { hide(); count.textContent = ALL_IDS.length + ' items'; return; }
      var referenceQuery = q.match(/^(?:s|slice\s+)(\d+)$/);
      var hits = ALL_IDS.filter(function (id) {
        if (referenceQuery) return !!shortRef(id) && Number(shortRef(id).slice(1)) === Number(referenceQuery[1]);
        var n = NODES[id];
        var searchable = [id, n.label, qualifiedRef(id), n.description, n.usefulOutcome].concat(n.sourceSections || [], (n.artifacts || []).map(function (a) { return a.label + ' ' + a.path; })).join(' ');
        return searchable.toLowerCase().indexOf(q) !== -1;
      });
      count.textContent = hits.length ? hits.length + (hits.length === 1 ? ' match' : ' matches') : 'No matching slices';
      results.innerHTML = hits.map(function (id) { return '<li><button type="button" data-id="' + escAttr(id) + '">' + esc(namedRef(id)) + '<small>' + esc(statusMeta(NODES[id].status).word) + (isContainer(id) ? ' · group' : '') + '</small></button></li>'; }).join('');
      results.hidden = hits.length === 0;
    }
    function choose(button) { hide(); selectNode(button.dataset.id); readableView(); }
    refreshSearchCount = function () {
      count.textContent = SEL && shortRef(SEL) ? 'Selected ' + shortRef(SEL) : ALL_IDS.length + ' items';
    };
    resetSearch = function () {
      if (!input.value) { refreshSearchCount(); return; }
      input.value = '';
      hide();
      refreshSearchCount();
    };
    input.addEventListener('input', show);
    input.addEventListener('focus', show);
    input.addEventListener('keydown', function (e) {
      var first = results.querySelector('button');
      if (e.key === 'ArrowDown' && first && !results.hidden) { e.preventDefault(); first.focus(); }
      if (e.key === 'Enter' && first && !results.hidden) { e.preventDefault(); choose(first); }
      if (e.key === 'Escape') { e.stopPropagation(); hide(); input.value = ''; count.textContent = ALL_IDS.length + ' items'; }
    });
    results.addEventListener('click', function (e) { var b = e.target.closest('button'); if (b) choose(b); });
    results.addEventListener('keydown', function (e) {
      var bs = Array.from(results.querySelectorAll('button')), i = bs.indexOf(document.activeElement);
      if (e.key === 'ArrowDown' || e.key === 'ArrowUp') { e.preventDefault(); bs[(i + (e.key === 'ArrowDown' ? 1 : bs.length - 1)) % bs.length].focus(); }
      if (e.key === 'Escape') { e.stopPropagation(); input.focus(); hide(); }
    });
    document.addEventListener('click', function (e) { if (!e.target.closest('.search-area')) hide(); });
    document.addEventListener('keydown', function (e) {
      if (e.key === '/' && !e.ctrlKey && !e.metaKey && !e.altKey && !/^(INPUT|TEXTAREA|SELECT)$/.test(e.target.tagName) && !e.target.isContentEditable) { e.preventDefault(); input.focus(); }
    });
    show();
  }

  // Keep phone zoom controls in the header rather than over the diagram or details sheet.
  // This costs one compact header row but leaves both the canvas and selected details reachable.
  function placeControls() {
    var controls = document.querySelector('.map-controls'), header = document.querySelector('.map-header');
    if (!controls || !header) return;
    var phone = window.innerWidth <= 700;
    if (controls.parentElement !== header) { header.appendChild(controls); controls.classList.remove('overlay'); }
    var insp = document.getElementById('insp');
    if (phone && !SEL) { insp.hidden = true; insp.setAttribute('aria-hidden', 'true'); }
  }

  /* --------------------------------------------------------------------- init */
  document.addEventListener('DOMContentLoaded', function () {
    var titleEl = document.getElementById('atlas-title');
    if (WORK.project) { titleEl.textContent = 'Project Atlas — ' + WORK.project.id; titleEl.title = WORK.project.label; }
    var revEl = document.getElementById('atlas-revision');
    if (WORK.revision) { revEl.textContent = 'rev ' + WORK.revision; revEl.title = WORK.lastUpdated || ''; }

    var pointing = document.createElement('div');
    pointing.id = 'pointing-readout'; pointing.hidden = true; pointing.setAttribute('aria-hidden', 'true');
    document.getElementById('ms').appendChild(pointing);
    svgEl.addEventListener('pointermove', pointAt);
    svgEl.addEventListener('pointerleave', clearHover);
    svgEl.addEventListener('pointercancel', clearHover);
    window.addEventListener('blur', clearHover);
    svg.call(zoom).on('dblclick.zoom', null);
    render();
    renderInbox();
    renderReviews();
    initSearch();
    var reviewCount = (WORK.reviews || []).length;
    var proposalCount = (IMPROV.proposals || []).length;
    document.getElementById('review-total').textContent = reviewCount + (reviewCount === 1 ? ' record' : ' records');
    document.getElementById('proposal-total').textContent = proposalCount + (proposalCount === 1 ? ' suggestion' : ' suggestions');
    // "All fields" is a switch with a visible pressed state; its label names the state, not the action.
    document.getElementById('detail-depth').addEventListener('click', function () {
      var all = document.getElementById('insp').classList.toggle('show-technical');
      this.setAttribute('aria-pressed', String(all));
    });
    // The phone sheet wrapper follows the inspector's hidden state wherever that state changes.
    var inspEl = document.getElementById('insp'), sidePanel = document.querySelector('.side-panel');
    function syncSheet() { if (sidePanel) sidePanel.classList.toggle('sheet-closed', !!inspEl.hidden); }
    new MutationObserver(syncSheet).observe(inspEl, { attributes: true, attributeFilter: ['hidden'] });
    placeControls();
    syncSheet();
    openingView();

    if (IMPROV.proposals && IMPROV.proposals.length > 0) document.getElementById('inbox-section').classList.add('visible');

    document.getElementById('view-map').addEventListener('click', function () { switchView('map'); });
    document.getElementById('view-dag').addEventListener('click', function () { switchView('dag'); });
    document.getElementById('btn-return').addEventListener('click', function () { setFocus(null); });
    // The inbox is a disclosure like Review history; the summary mirrors the open state.
    var inboxSection = document.getElementById('inbox-section');
    inboxSection.addEventListener('toggle', function () {
      document.getElementById('inbox-toggle').setAttribute('aria-expanded', String(inboxSection.open));
    });
    var hintToggle = document.getElementById('hint-toggle'), hintBody = document.getElementById('hint-body');
    if (hintToggle && hintBody) hintToggle.addEventListener('click', function () {
      hintBody.hidden = !hintBody.hidden;
      hintToggle.setAttribute('aria-expanded', String(!hintBody.hidden));
    });
    document.getElementById('mobile-detail-reopen').addEventListener('click', function () {
      var insp = document.getElementById('insp');
      insp.hidden = false;
      insp.removeAttribute('aria-hidden');
      this.hidden = true;
    });
    var minimap = document.getElementById('minimap');
    minimap.addEventListener('click', minimapJump);
    minimap.addEventListener('keydown', function (ev) {
      if (ev.key !== 'Enter' && ev.key !== ' ') return;
      ev.preventDefault();
      var r = minimap.getBoundingClientRect();
      minimapJump({ clientX: r.left + r.width / 2, clientY: r.top + r.height / 2 });
    });

    svgEl.addEventListener('click', function (e) {
      var n = e.target.closest('.node');
      if (n && n.dataset && n.dataset.id) { selectNode(n.dataset.id); resetSearch(); }
    });
    svgEl.addEventListener('keydown', function (e) {
      var n = e.target.closest('.node');
      if (!n || !n.dataset || !n.dataset.id) return;
      if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); selectNode(n.dataset.id); resetSearch(); return; }
      if (/^Arrow(Left|Right|Up|Down)$/.test(e.key)) {
        var next = nodeInDirection(n.dataset.id, e.key);
        if (!next) return;
        e.preventDefault(); e.stopPropagation();
        var el = document.querySelector('#svg .node[data-id="' + cssId(next) + '"]');
        document.querySelectorAll('#svg .node').forEach(function (x) { x.setAttribute('tabindex', x === el ? '0' : '-1'); });
        el.focus();
      }
    });
    svgEl.addEventListener('focusin', function (e) {
      clearHover();
      var node = e.target.closest ? e.target.closest('.node') : null;
      if (node) revealNode(node.dataset.id);
    });
    document.addEventListener('keydown', function (e) {
      if (e.key === 'Escape' && FOCUS) setFocus(null);
    });
    document.getElementById('ms').addEventListener('keydown', function (e) {
      if (e.target.closest && e.target.closest('.node')) return;   // node focus: arrows move between slices
      var delta = { ArrowLeft: [64, 0], ArrowRight: [-64, 0], ArrowUp: [0, 64], ArrowDown: [0, -64] }[e.key];
      if (delta) {
        e.preventDefault();
        var k = d3.zoomTransform(svgEl).k;
        svg.call(zoom.translateBy, delta[0] / k, delta[1] / k);
      } else if (e.key === 'Home') { e.preventDefault(); fitDiagram(); }
    });

    // Navigation hooks for the browser suites (pan/zoom only — never selection, which stays a real click).
    window.atlas = { reveal: revealNode, fit: fitDiagram, readable: readableView };
    document.getElementById('z-in').addEventListener('click', function () { zoomBy(1.25); });
    document.getElementById('z-out').addEventListener('click', function () { zoomBy(1 / 1.25); });
    document.getElementById('z-fit').addEventListener('click', function () { fitDiagram(); });
    document.getElementById('z-read').addEventListener('click', function () { readableView(); });
    var lastWidth = window.innerWidth;
    window.addEventListener('resize', function () {
      var changedWidth = window.innerWidth !== lastWidth;
      lastWidth = window.innerWidth;
      placeControls();
      if (changedWidth) { render(); fitDiagram(); }
      else { syncCanvasBox(); updateMinimap(); }
      if (window.innerWidth > 700) {
        var insp = document.getElementById('insp');
        insp.hidden = false;
        insp.removeAttribute('aria-hidden');
        document.getElementById('mobile-detail-reopen').hidden = true;
      }
    });
  });
})();
