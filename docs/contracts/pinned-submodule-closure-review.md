# Exact pinned closure review authority

Issue #564 Slice 4B0 adds a separate, invocation-local review of the
[4A complete object closure](pinned-submodule-closure.md). Recipe acceptance
is not closure acceptance. Neither migration Yes nor recipe review Yes can
substitute for the closure producer's own explicit confirmation.

## Input and ordering

The only input authority is a live `InvocationOwnedPinnedSubmoduleClosure`.
`review_pinned_submodule_closure()` consumes that whole owner. It cannot accept
raw nodes, pins, manifests, cache paths, decoded provenance, completed S4, or
an externally obtained confirmation token.

The order remains recipe review and exact pin → S3 → actual initial source
evaluation → evaluated selection → 4A exact acquisition → closure review.
No second initial evaluation, worktree materialization or makepkg phase is
introduced by the review producer.

## Small complete text subset

Every regular/executable blob occurrence, including exact `.gitmodules`, is
read from the owned 4A backing. NUL/binary content and other modes, including
symlinks, are unsupported. Gitlinks are presented as edges to the exact child
node whose complete contents are included in the same review; the existing
recipe review's metadata-only Gitlink rejection is unchanged.

The existing review constants bound 4096 inventory entries, 8 MiB per text
blob, 32 MiB aggregate text, 1 MiB per logical line, and 32 MiB rendered output.
Occurrences are counted separately without content deduplication. The rendered
bound includes framing and escaped metadata/content. A limit failure stops;
there is no truncation, pagination acceptance or manual-inspection bypass.

All required blobs are collected and classified, and the complete body is
rendered, before any review body is written. Root X, each node commit/tree and
object format, closure path, parent edge, logical name, path, locator and pin
are shown with terminal-safe escaping. Locators are transport metadata, not
revision authority. Missing final newlines and executable modes remain visible.

## Presentation and explicit acceptance

Successful body writes and flush establish the private presented state.
The producer then calls the existing no-default confirmation primitive with
its own closure-specific question. Prompt output is checked as well; a failed
write/flush cannot mint acceptance. Only explicit `y`/`yes` can succeed.

`ReviewPolicy::Skip` (including `--nodiff` / `review.diff=Skip`), non-TTY and
`--noconfirm` return typed failures. Decline, explicit cancellation, EOF and
input failure remain distinct. No legacy continuation or fallback is produced.

All object reads precede the human prompt. Acceptance does not read objects
again after a potentially long human wait or extend 4A's acquisition/read
deadline. Object transfer/reproof budgets belong to the future 4B1 consumer.

## Ownership and failure

`AcceptedPinnedSubmoduleClosure` is move-only with a private constructor. It
retains the whole 4A owner and the confirmation from this session, preserving
the same selection lineage, object backing, nodes, edges and exact pins.
Observations borrowed from it cannot reconstruct acceptance authority.

Read failure preserves the original `PinnedClosureFailure`, including process
outcome, parent cancellation, cleanup and any abandoned-root diagnostic.
Because 4A already cleans up on read failure, the review layer does not repeat
that cleanup. Other review failures explicitly delegate cleanup once and keep
its consequence separately from the primary reason. Accepted ownership stays
live until explicit cleanup or destruction, using the same 4A no-retry policy.

## Remaining scope

The [4B1 workspace producer](pinned-submodule-workspace.md) consumes Accepted
whole ownership into a move-only SourceReady owner, using fresh local derived
Git state and phase-point proof. The immutable 4A backing and same selection
lineage survive. This is not continuous attestation; disposable workspace/cache
is not authority, and persistent user cache is untouched.

This remains a production-disconnected foundation. All production gitfile, `.git/modules`,
`.gitmodules`, workspace-cardinality and S3 recipe Gitlink gates remain closed.
4B2 native makepkg/common S4 integration remains pending. S5/S6,
provenance schema v1/27 keys, cache policy and CLI routing are unchanged.

The focused `test-pinned-submodule-closure-review` target uses real local 4A
fixtures and runs only the review lane. It does not execute the existing 4A,
4A0, bootstrap or artifact transport suites.
