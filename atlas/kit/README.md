# Project Atlas local kit

This project-local kit provides a reusable offline Atlas setup, update and integrity procedure. It
does not install shared skills, alter global configuration, or act as an OS sandbox.

**Kit version: 1.2.2**

## Initialize a new project

From `ai-config/atlas/` (the Atlas kit directory), create a new project:

```bash
node kit/init.cjs --source . --target ../my-new-project
node validate.js --project ../my-new-project
```

Initialization copies renderer/build/validator assets and the self-contained HTML template, then
creates minimal seed records (one root container + one planned leaf) with an empty review
registry and **immediately builds the target `atlas.html` from those seed records** before
returning. The initialized project is ready-to-open on first command — `node validate.js`
passes without an intermediate `node build.js` caller step. Init never fabricates a pending run
and never leaves another project's history embedded in the seed output.

## Update an existing project

```bash
node kit/init.cjs --source . --target ../existing-project --update
node validate.js --project ../existing-project
```

Update copies only kit-owned assets (renderer, build scripts, templates) and never touches
`workflow.json`, `improvement-proposals.json`, or user-created files. It refuses to overwrite
customized kit assets unless they match the installed digest map in `kit/.installed-assets.json`.
Targets must not contain symlinks; existing projects require `--update`.

## Check source or target integrity

```bash
# Check the kit source (this directory)
node kit/init.cjs --check

# Check an initialized target against its installed manifest
node kit/init.cjs --check --target ../existing-project

# Check an initialized target for staleness relative to the current kit source
node kit/init.cjs --check --target ../existing-project --source .
```

The `--check` flag with no `--target` argument validates the kit source directly — it lists all
expected ASSETS files and their digests. With `--target`, it validates the installed asset
manifest in that target directory. With both `--target` and `--source`, it also checks that the
target is not stale relative to the current source.

## Verify a fresh initialization

```bash
# From the initialized target directory:
node build.js --check       # init-built output is fresh
node validate.js --project . # records and HTML are consistent
```

## Build and validate

From an initialized project directory:

```bash
node build.js               # rebuild atlas.html from workflow.json + improvement-proposals.json
node build.js --check       # non-mutating staleness check (exit 0 if fresh)
node validate.js             # structural validation and HTML-record parity
```

## Freeze and verify a source snapshot

```bash
node kit/freeze-manifest.cjs <external-numbered-manifest.json>
node kit/freeze-manifest.cjs --check <manifest.json> <project-directory>
```

Freeze is exclusive-create — it refuses to overwrite an existing manifest. It records every file's
SHA-256 digest, project identity, and timestamp. Verification reports added, deleted and changed
files.

## Protection limits

Ordinary reviewers receive only read/grep/find/ls/contact_supervisor. `review-protection.cjs`
rejects any write, shell, process-control or ambient capability in that profile. A closure
verifier authorized to run commands retains host filesystem risk. This kit is not an OS sandbox;
prompts, copies, hashes and read-only file attributes do not prevent arbitrary shell writes.
The manifest and post-check comparison detect drift, while the parent remains the acceptance
authority.

## Completion evidence freshness

For a completed legacy-imported leaf, the frozen node fingerprint and review sequence ceiling
preserve the historical import only while the node and linked history remain unchanged. If the
node changes, pre-import reports are history and cannot satisfy acceptance. A newer accepted
reviewer record must carry `reviewedNodeSha256`, computed from the node content while excluding
only review-link and legacy-import metadata. A matching digest detects source drift but cannot
prove arbitrary prose or an unrecorded transition is truthful.

## Limits

- **Windows ancestor junction refusal.** `init.cjs` walks every ancestor of the target to the
  drive root and rejects any directory that is a reparse point (`lstat.isSymbolicLink()` matches
  NTFS junctions). This means targets under `C:\Users\<user>\AppData\Local` or a junctioned
  profile folder (like `~/.agents`) are refused. Workaround: use a physical path
  (e.g. `D:\tmp\` on Windows, or any non-junctioned path). This is conservative by design —
  init cannot know whether the junction owner intends to redirect writes.
- **Coordinated rebaseline.** A node content change paired with an updated legacy baseline entry
  inside `workflow.json` is not detected by the built-in freshness check alone. Only an external
  manifest or independent review process can catch such a coordinated edit. The kit discloses this
  limit as a design constraint, not a secret bypass.
- **Old reviews bind only the bytes they inspected.** `reviewedRevision` and `reportSha256`
  verify that a review ran against a specific snapshot of the project. Reviews do not guarantee
  that newer code satisfies the same properties. Re-verification after source changes is required.
- **Minimal node count.** An initialized project starts with exactly 2 seed nodes (root container
  + one planned leaf). The stale-output parity check enforces that the embedded HTML matches
  `workflow.json`, so a 2-node seed still produces a valid, self-consistent Atlas.