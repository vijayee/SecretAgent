# Review-protection profile

## Ordinary read-only reviewer (enforced capability contract)

Only these tools are granted: `read`, `grep`, `find`, `ls`, and `contact_supervisor`. The shipped `kit/review-protection.cjs` allowlist rejects write/edit/create/patch, shell/exec/run, process-control, unknown and ambient extension capabilities. This is a capability/profile assertion: it does not change harness configuration or OS permissions.

## Separately authorized command verifier

A closure verifier may receive command execution to run named project checks and write external receipts. That authority is distinct from ordinary review and has full host filesystem risk. It is not sandboxed. Prompts, copies, hashes, read-only file attributes and this profile cannot prevent an authorized shell from writing source files. `freeze-manifest.cjs --check` detects added/deleted/changed bytes after a freeze; it does not prevent those writes. Parent integration and acceptance remain separate from raw reviewer verdicts.
