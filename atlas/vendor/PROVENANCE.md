# Vendored dependency provenance — D3

| field | value |
| --- | --- |
| package | `d3` |
| version | **7.9.0** (npm `dist-tags.latest` at retrieval time) |
| retrieved | 2026-09-11T04:21Z (UTC) from `https://registry.npmjs.org/d3` → `https://registry.npmjs.org/d3/-/d3-7.9.0.tgz` |
| license | ISC — `LICENSE-d3-ISC.txt` (verbatim `package/LICENSE` from the tarball), © 2010-2023 Mike Bostock |
| vendored file | `d3.v7.9.0.min.js` = `package/dist/d3.min.js` from that tarball, byte-for-byte |
| tarball sha512 (base64) | `e1U46jVP+w7Iut8Jt8ri1YsPOvFpg46k+K8TpCb0P+zjCkjkPnV7WzfDJzMHy1LnA+wj5pLT1wjO901gLXeEhA==` — **matches** the `dist.integrity` the registry declares for 7.9.0 (recomputed locally) |
| tarball sha1 (shasum) | `579e7acb3d749caf8860bd1741ae8d371070cd5d` — matches registry `dist.shasum` |
| tarball sha256 | `7e36605710a2ba54846797c8c6d888911341b215ae53257dc32a49a0b824355e` (233,671 bytes) |
| `d3.v7.9.0.min.js` sha256 | `f2094bbf6141b359722c4fe454eb6c4b0f0e42cc10cc7af921fc158fceb86539` (279,706 bytes) |
| registry metadata | `d3-registry-meta.json` — the version document fields used above (name, version, license, repository, dist, dependencies) |

## How it gets into `atlas.html`

`build.js` reads `d3.v7.9.0.min.js` and replaces the content of
`<script id="d3-embed">…</script>` in `atlas.html` at build time, so the shipped page is one
self-contained offline file. The full package ISC license is embedded separately in a non-executing `d3-license` block. `atlas.html` therefore carries the same bundle; its pinned SHA-256 is verified by `build.js` against the vendored file. The bundle is the UMD browser build; it defines `window.d3` and runs no network requests. It contains no `</script>` sequence, so it is embedded literally.

## Retrieval method (no installer, no lifecycle scripts)

Downloaded with a plain HTTPS GET of the registry packument and the package tarball, then
`tar -xzf … package/LICENSE package/dist/d3.min.js package/package.json`. **Nothing was
installed**: no `npm install`, no package-manager configuration, no dependency tree, no
`postinstall`/prepare script executed, no global tooling. The tarball was expanded only for
those three files. This is a pinned, integrity-checked official artifact rather than a
resolved dependency graph.

## What D3 is used for

`d3-selection` (SVG data rendering), `d3-zoom` (wheel/pinch/drag pan-zoom and programmatic
Fit), and `d3-shape` (`symbol` status keys). The full official D3 bundle includes additional
D3 modules, but this renderer does not use a force simulation or animated layout transitions.
CSS state transitions are disabled under `prefers-reduced-motion`.
D3 provides **no** graph layering, so the layered DAG placement and the elbow routing in
`src/atlas-render.js` are hand-written and deterministic. No ELK, d3-dag, React, UI kit, or any
other library is introduced by this unit.
