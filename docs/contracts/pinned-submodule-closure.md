# Parent-pinned recursive closure authority

Issue #564 Slice 4A implements the object-level acquisition foundation used by
the SourceReady bootstrap chain. Issue #589 adds root tag authority. This contract does not authorize
closure review acceptance, a makepkg workspace, source-ready S3, completed S4,
installation, or provenance publication.

## Input and revision authority

`acquire_pinned_submodule_closure(EvaluatedDevelSourceSelection, PresentationDetail)` consumes the
[4A0 evaluated selection](evaluated-devel-source-build-proof.md), retaining its
recipe context, environment and lineage in a private, move-only
`InvocationOwnedPinnedSubmoduleClosure`. Raw URL/selector/OID tuples, copied
`VcsSourceIdentity`, reviewed metadata, completed S4 and decoded provenance
cannot construct this owner. Its opaque backing has no construction friendship.

`PresentationDetail` is an explicit invocation-local command presentation input.
It is passed to the root-tag command owner without being retained in the returned
closure or affecting selection, acquisition, identity, validity or failure policy.
Issue #595 Slice 1 preserves the existing command display and state-log `EXEC`
bytes for both Normal and Detailed.

Only the root uses a remote selector observation: the evaluated default requests
`HEAD`, while an evaluated explicit branch requests its exact `refs/heads/...`.
Exactly one full lowercase SHA-1/SHA-256 OID record for that ref is accepted,
together with the complete advertised `refs/tags/*` mapping in the same bounded
`ls-remote` response (without `--refs`, so HEAD and peeled records survive).
This freezes upstream X; the AUR recipe revision Rr is a separate identity.
Subsequent root acquisition specifies X directly. Remote advancement to Y cannot
change the requested revision; an unavailable X causes failure without fallback.

Every child revision comes exclusively from mode 160000 in its exact parent
commit's tree. `.gitmodules` supplies a name, parent-relative path and HTTPS
locator, never a revision. Child HEAD, branch tips, ambient origin and caches are
not queried to select a child. Sibling occurrences keep distinct edges even when
the locator/OID pair is identical. Initial storage acquires each occurrence
separately. An ancestry recurrence is rejected independently of depth/count limits.

## Exact acquisition and immutable inventory

Each invocation creates a fresh private root with numbered bare repositories.
No checkout, submodule command, index materialization, `.git/modules` layout or
makepkg preparation is involved. Initialization requires an empty repository;
only the expected bare SHA-1/SHA-256 configuration and Git object storage shape
are accepted. Retained directory identities, configuration bytes and contained
filesystem inventory are checked around every Git subprocess.

The existing HTTPS-only acquisition process arguments and complete trusted Git
environment are reused, including global/system configuration isolation,
credential/askpass/hook isolation, redirects disabled, no replacement objects,
fetch/transfer fsck, and existing proxy/absolute-CA exceptions. The less restrictive
S4 managed profile is not used. No ambient repository, alternate, shallow/promisor
store, cache, hook, include, remote override or replacement metadata is accepted.

Fetch uses the complete expected OID with no tags, recursive submodule fetching,
auto maintenance, commit graph or FETCH_HEAD writes. Proof then checks storage
object format, raw object type `commit`, exact unpeeled identity, strict fsck
hash/connectivity, the raw commit's tree header and that tree's object type.
Strict fsck can reject malformed `.gitmodules` during acquisition before the
more specific declaration parser runs; the original Git failure is retained.

Two bounded NUL streams from `ls-tree -r --full-tree --no-abbrev` describe the
same exact tree. The existing tree parser checks modes/types, complete OID width,
framing, sizes, duplicate paths and trailing data. Each inventory entry retains
path, mode/type, object OID and blob size; each node retains commit, format, tree,
locator and parent edge. No worktree HEAD is used. Leaf status comes from this
complete exact-tree inventory.

The exact regular `.gitmodules` blob is read by its tree-bound OID and bounded
size. Every declaration must match one gitlink and every gitlink one declaration,
before any child acquisition starts. Child trees are traversed by the same rules.
Owned object databases retain retrievable backing for all inventoried entries.
`read_blob(node, entry)` can retrieve an inventoried non-gitlink blob, rechecking
backing/configuration and strict fsck with a 64 MiB capture ceiling. Observations
borrowed from the owner cannot reconstruct acquisition authority.

## Root tag authority (Issue #589)

The live owner retains a sorted, unique `PinnedRootTag` list: validated full ref
name, raw `ReviewedSourceObjectId`, and verified terminal peeled OID for an
annotated tag. `^{}` advertisement lines are metadata, never materialized refs.
Parsing rejects duplicate selector/tag/peeled records, orphan peeled records,
foreign namespaces, malformed framing, non-lowercase or wrong-width OIDs and
mixed object formats. Git `check-ref-format` validates full names before they
enter the typed mapping. Empty tag sets are valid.

After the exact root fetch, one bounded fetch requests the distinct observed
raw tag OIDs (excluding X), with the existing
`--no-tags` / no-FETCH_HEAD policy. It never resolves a tag name again. Observed
objects that remain obtainable after remote deletion/retargeting are valid
snapshot backing; unavailable objects or inconsistent observations stop without
fallback. This is one bounded advertisement, not a claim of an atomic remote
transaction or visibility of hidden refs.

Strict fsck proves raw hashes and connectivity with root X and all raw tag OIDs
as roots. Bounded raw tag traversal checks every object type, format and terminal
OID against the advertisement. Lightweight, annotated, nested annotated,
non-reachable, and tree/blob tags retain their original objects. Tagger metadata,
embedded names and signature bytes are preserved; signature trust/verification
is not asserted. Source selector and child gitlink commit-only rules are unchanged.

Tag admission limits are 1024 names, 4096 bytes/name, a 1 MiB advertisement,
64 annotated links per tag, and 256 KiB per raw tag. All reads/processes also
consume the existing aggregate metadata, process, deadline and storage budgets.
A limit failure never accepts a partial snapshot. Object backing remains
ref-free; only the accepted workspace consumer creates derived tag refs.
Child tags, other ref namespaces, original symbolic HEAD names, `describe --all`,
new source selectors and persistent tag provenance/update detection are outside
this contract. The list is invocation-local build authority, not a generic
all-ref snapshot or reproducible-build framework.

## Initial declaration subset

Supported declarations use literal `[submodule "name"]` sections, required unique
`path` and `url`, absolute canonical HTTPS URLs, and optional `update = checkout`.
Logical name and path are separate namespaces. Quoted values, escaped quote and
backslash, blank lines and ordinary comments are accepted. Repeated sections or
keys, implicit values, legacy section syntax, continuations, other escapes and
unknown keys are rejected. `branch` is rejected as a closed-subset policy; Git's
ordinary pinned update does not become floating merely because a branch key exists.
Relative/HTTP/SSH/scp/file/ext/git locators and merge/rebase/none/custom updates
are rejected. No `--remote` entry point exists.

Paths and literal logical names reject absolute/empty components, dot/dotdot,
`.git`, backslash/control bytes and excessive lengths. Duplicate paths and
ancestor/descendant declaration overlap are rejected. These are logical source
paths, not authorization to materialize filesystem worktrees; 4B must establish
its own namespace, symlink, gitfile and modules correlation.

## Bounds, failures and lifetime

| Bound | Initial value |
| --- | --- |
| Child depth (root is 0) | 16 |
| Edge occurrences | 128 |
| Aggregate recursive tree records | 262144 |
| Path/name bytes; component bytes | 4096; 255 |
| Single `.gitmodules`; aggregate declarations | 256 KiB; 1 MiB |
| Aggregate captured metadata | 64 MiB |
| Single tree metadata/path stream | 32 MiB each |
| Raw commit capture | 1 MiB |
| Git process count | 4096 |
| Total invocation acquisition/read deadline | 10 minutes |
| Process termination grace | 200 milliseconds |
| Independent object cleanup deadline | 5 seconds |
| Observed stored files/directories; storage bytes | 262144; 1 GiB |
| Object-database filesystem depth | 64 |

These are bounded observation/admission limits, not hard network-byte, disk or
expanded-object quotas, performance SLOs, or universal upstream compatibility.
Tests exercise small real Git topologies and shorten individual limits around
boundaries; they do not claim a maximum-sized live repository benchmark.

All Git calls pass one runner. Parent cancellation is checked before child
outcome, so SIGINT plus child exit zero cannot mint success. Typed stage/reason
and original bounded process outcome/cancellation survive first-failure stop.
An ordinary nonzero exact fetch reports `PinnedObjectUnavailable`; init failures
and launch, timeout, signal, capture or process I/O failures report
`GitProcessFailed` with the original process outcome retained.
There is no retry with HEAD, another revision, protocol or cache.

Cleanup consumes authority even on failure. Object cleanup uses only the
invocation-created root and retained descriptor identities, a complete bounded
removal plan and rechecked ancestors. It never derives removal targets from
locators or declarations. Selection cleanup is separately retained alongside the
primary failure and object cleanup consequence. Explicit cleanup failure is not
silently discarded: producer and `read_blob` failures retain an `abandoned_root`
when object cleanup fails. Cleanup is not
retried by destruction. Same-UID concurrent mutation, SIGKILL and power loss are
not an atomic sandbox or guaranteed residue-free execution contract.

## Slice 4B0 review transition

The separate [exact closure review](pinned-submodule-closure-review.md)
producer consumes this live whole owner, presents the complete bounded
identity/topology metadata, and obtains its own explicit snapshot acceptance
(as redefined in Slice 6). Upstream blob contents are not rendered. The resulting
`AcceptedPinnedSubmoduleClosure` retains this owner, including the selection
and backing. Recipe acceptance and migration Yes do not authorize this step.
Binary/large regular blobs do not alone prevent acceptance; invalid identity,
unsupported topology and resource failures still stop. The
[workspace consumer](pinned-submodule-workspace.md) retains Accepted whole
ownership and immutable backing through native makepkg/common S4 (4B2).
Production activation is limited to the existing typed initial-Missing
bootstrap intent on exact target-less ordinary Auto `-Syu` / `-Su`.

## Phase-point / remaining scope

SourceReady, prepared and post-build closure proofs are phase-point evidence,
not continuous attestation or a general sandbox. Only the typed SourceReady
path replaces the normal single-root gates. S3 recipe Gitlink refusal,
S5/S6 and provenance schema v1/27 keys remain unchanged.
Issue #564 Slice 5 supplies split group authority; Slice 6 adds representative
production coverage without changing this acquisition owner.
