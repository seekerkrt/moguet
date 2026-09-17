# Exact pinned closure review authority

Issue #564 Slice 6 defines explicit upstream snapshot acceptance in the existing
Slice 4B0 owner of the
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

## Exact snapshot acceptance

The user authorizes this exact upstream snapshot as input to this build. This
is not an audit of all upstream source contents or a claim that they are safe.
Git identity proves which bytes are selected and materialized, not whether the
code is malicious. The PKGBUILD, recipe-local patches, config and other owned
recipe inputs retain their separate full content review semantics.

The presentation includes the selected remote and selector, resolved root X,
each node's commit/tree/object format, every inventory entry's path/mode/object
ID/blob size, all submodule logical names, paths, URLs and exact parent pins,
and the complete root tag count/name/raw-OID/annotated-peeled-OID mapping.
An explicit zero count represents the accepted empty namespace. Raw tag objects
are retained; this presentation does not assert signature verification.
Locators describe transport; parent Gitlinks own child revision authority.
All values are terminal-safe. Each occurrence is shown even for reused children.

Upstream blob bodies are not read or rendered by this owner. Regular binary or
large blobs do not require a viewer, preview or content-specific approval.
The immutable 4A backing already proves hashes, connectivity and inventories;
materialization and common S4 retain their phase-point correlation.

The inventory count is bounded by the existing 4A tree-record budget (262144),
not the recipe content-review entry limit. Rendered identity metadata retains
its 32 MiB bound, including escaping and framing. All metadata is rendered
before any body is written; incomplete/oversized metadata cannot be accepted.
Acquisition, materialization and build resource budgets remain independent.
Symlinks remain unsupported by the current workspace. Invalid topology,
ambiguous identity and unsupported transport remain fail-closed.

## Presentation and explicit acceptance

Successful body writes and flush establish the private presented state.
The producer then calls the existing no-default confirmation primitive with
its own closure-specific question. Prompt output is checked as well; a failed
write/flush cannot mint acceptance. Only explicit `y`/`yes` can succeed.

`ReviewPolicy::Skip` (including `--nodiff` / `review.diff=Skip`), non-TTY and
`--noconfirm` return typed failures. Decline, explicit cancellation, EOF and
input failure remain distinct. No legacy continuation or fallback is produced.

Acceptance does not perform blob reads or reacquire objects, before or after
the human prompt. Object transfer/reproof budgets belong to the 4B1 consumer.

## Ownership and failure

`AcceptedPinnedSubmoduleClosure` is move-only with a private constructor. It
retains the whole 4A owner and the confirmation from this session, preserving
the same selection lineage, object backing, nodes, edges and exact pins.
Observations borrowed from it cannot reconstruct acceptance authority.

Acquisition/read failures remain owned by 4A and cannot reach acceptance.
Presentation, interaction and allocation failures delegate cleanup once and
retain its consequence separately from the primary reason. Accepted ownership
stays live until explicit cleanup or destruction, using the same no-retry policy.

## Remaining scope

The [4B1 workspace producer](pinned-submodule-workspace.md) consumes Accepted
whole ownership into a move-only SourceReady owner, using fresh local derived
Git state and phase-point proof. The immutable 4A backing and same selection
lineage survive. This is not continuous attestation; disposable workspace/cache
is not authority, and persistent user cache is untouched.

4B2 connects this chain to native makepkg/common S4 only through the typed
SourceReady consumer on the exact initial-Missing bootstrap route. Normal
single-root gates remain for inputs without that authority; the S3 recipe
Gitlink gate remains closed. S5/S6 and provenance schema v1/27 keys are unchanged.
Slice 5 supplies split group authority. Slice 6 adds the
[production-representative fixtures](../../tests/fixtures/devel-production-topologies.md).

The focused `test-pinned-submodule-closure-review` target uses real local 4A
fixtures and runs only the review lane. It does not execute the existing 4A,
4A0, bootstrap or artifact transport suites.
