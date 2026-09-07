# Evaluated devel source build proof contract

## 位置づけ

この文書はIssue #476 Slice 4で確立する、reviewed AUR recipeからactual fresh package archiveまでの
production-compiled / production-disconnected proof contractを定める。Slice 4が証明するのは
**何を実際にbuildしたか**であり、何をinstallしたかではない。

責務の後続境界は次のとおり分離する。

- Slice 4: evaluated source、actual makepkg workspace / Git OID、dynamic version、fresh artifact identity
- Slice 5: exact artifactのtrusted `Install` / `Upgrade` receiptとfresh installed binding
- Slice 6: successful install後のprovenance publication
- Slice 7: #475 remote observationとのauthoritative comparison

current CLI、normal source-build / install、AUR assessment、upgrade routeはこのcapabilityを生成または消費しない。

## Input authorityとlineage

唯一のproduction mintは、同じ`InvocationOwnedSourceBuildContext`が生成した
`InvocationOwnedMakepkgEnvironment`と、そのcontext自体をmoveで消費する。callerはrecipe cwd、
`PKGDEST`、`BUILDDIR`、`SRCDEST`、makepkg path / FD、artifact path、Git OIDを個別に渡せない。

Slice 3のimmutable `recipe/`はexact reviewed Git-tree snapshotとして保持する。makepkg 7.1.0はdynamic
`pkgver()`の結果をwritable PKGBUILDへ反映して後続invocationへ渡すため、Slice 4は同じcontextのprivate
`BUILDDIR`内へdescriptor-relativeにexact working recipeを複製する。working copyでは最初の
`--nobuild`中だけPKGBUILDをowner-writableにし、他entryのidentity/contentを維持したまま直後にread-onlyへ
sealする。build直前に同じfileだけを再びwritableにし、build後のdynamic bytesがpost-preparation proofとexactに
一致する場合だけ再sealする。再評価された`pkgver()`がdriftした場合は古いpackagelist/versionを採用せず停止する。
persistent AUR checkoutとimmutable reviewed snapshotはmakepkg cwdにならない。

makepkgはSlice 3が保持するfixed `/usr/bin/makepkg` descriptorを、child-local non-CLOEXEC duplicateから
`execveat(AT_EMPTY_PATH)`で起動する。PATH lookup、shell alias/function、pathname再resolutionへ戻さない。
shebang interpreter hand-offのために必要なduplicateはchild内だけで作り、parentのretained identityを変更しない。

## Evaluated source subset

exact reviewed `.SRCINFO`と、working recipeで実行した最初の`makepkg --printsrcinfo`を同じstrict parserへ渡し、
source projectionがstructuralに一致する場合だけ`EvaluatedDevelSourceProjection`を作る。

initial authoritative subsetは次に限定する。

- AUR PackageBase、valid exact #411 binding、editor overlayなし
- exactly one package child
- exactly one floating Git source
- HTTPS transport
- default HEADまたはGitが`check-ref-format --branch`で受理するexact branch
- architecture-independent source declaration
- 追加sourceはexact reviewed snapshot内のcontained regular local fileのみ

source substitution、conditional mutation、追加/削除、architecture-qualified source、複数VCS、non-Git、
tag / commit / query / signed selector、remote non-VCS source、generated/untracked local sourceはtyped failureである。

## Makepkg phase protocol

Slice 1 characterizationとmakepkg owner contractに従い、同じworking recipe / roots / environmentで次を実行する。

1. initial `--printsrcinfo`とraw/evaluated source一致
2. empty private `PKGDEST` reproof
3. `--nobuild --nodeps --noconfirm`
4. dynamic PKGBUILD seal、post-preparation `--printsrcinfo`と`--packagelist`
5. private mirror + actual worktree Git proof
6. `--noextract --nodeps --noconfirm`（`-c`なし）
7. post-build Git reproof
8. exact fresh `PKGDEST` inventory
9. retained-FD archive metadata、archive SHA-256、raw ALPM-MTREE SHA-256

pre-preparation `--packagelist`はfinal identityへ入れない。post-preparation package metadata / packagelistとactual
archive metadataのchild、PackageBase、full version、architectureが一致することを要求する。

## Git proof

private `SRCDEST`直下のexactly one bare mirrorと、private `BUILDDIR`配下をbounded / descriptor-relativeに走査して
得たexactly one `.git` directory worktreeを保持する。directory traversalはowner、mode、device、symlink / mount
escape、entry/depth limitを検証する。

mirror `origin`はevaluated canonical HTTPS remoteと一致し、worktree `origin`はそのexact private mirrorを指す。
default HEADはmirror `HEAD` / worktree `origin/HEAD`、branchはmirror `refs/heads/<branch>` / worktree
`refs/remotes/origin/<branch>`を照合する。worktree `HEAD`のraw commit OIDはselected refおよびmirrorと一致し、
preparation後とbuild後で同じcomplete lowercase SHA-1 / SHA-256 OIDでなければならない。
両proof pointの最初のGit observationより前と最後に、loose / packed / reftableの`refs/replace`および
`info/grafts`の存在を拒否する。すべてのactual-proof Git commandは`--no-replace-objects`を使用し、
selected ref / HEADをpeelせず取得したOIDのraw typeが`commit`で、repositoryのstorage object formatとも
一致することを確認する。replacement無効化はmetadata存在のfail-closed検査を代替しない。refs inventoryはGit自身へ問い合わせ、
filesystem上のloose ref directoryの不在だけからreplacement metadataの不在を推定しない。

makepkgのshared cloneが作るalternateはexact private mirror `objects` 1件だけを許す。linked worktree、submodule、
external alternateは拒否する。`prepare()`やbuildがtracked worktree bytesを変更することは許すが、dirty stateを
upstream commit OIDへflattenしない。

## Artifact proofとownership

private `PKGDEST`はbuild前にempty、build後にpost-preparation packagelistと同じleafのregular file 1件だけを
許す。symlink、hardlink、foreign owner、group/other writable file、別device、zero/oversized file、signatureを含む
追加entryを拒否する。

inventory後は`O_NOFOLLOW` descriptorを保持し、named path、descriptor、size、mode、owner、link count、mtime、
ctimeをmetadata/hash/MTREE読取の後にも再証明する。libalpm metadataは`/proc/<self>/fd/<retained-fd>`から
同じopen fileを読む。archive SHA-256とraw `.MTREE` bytes SHA-256は既存XDG generation-store SHA-256 helperを
再利用する。archive inventory、member path/count、`.PKGINFO`、`.MTREE`、process captureはそれぞれ固定上限を
持ち、libalpm queryより前にexactly one `.PKGINFO` / `.MTREE`とbounded metadata extractionを確認する。

成功時の`EvaluatedDevelSourceBuildProof`は次を一体でmove所有する。

- Slice 3 contextと#411 reviewed binding / recipe tree identity
- evaluated source projection
- `ActualBuiltGitRevision`
- retained artifact descriptor、`PackageChildIdentity`、`BuiltPackageArtifactEvidence`

artifact pathはdiagnostic/presentation valueでありauthorityではない。proof破棄または明示cleanupまでcontextと
artifactを保持する。S5-Aの`EvaluatedDevelSourceArtifactTransport`はproof全体をmoveでconsumeし、
保存archive digestとの再照合を経てoriginal retained FDを既存sealed transportへ渡す。
同ownerのS5-B `execute_exact`は[exact receiptとfresh installed binding](exact-installed-artifact-binding.md)までを
内部producerとして実装する。元proofとこれらをまとめるS5-C final proof、通常routeとSlice 6 publicationは未接続である。
接続の契約は[trusted transport](trusted-source-artifact-transport.md)を正とする。
Slice 4 producer自身にはinstall呼出を追加せず、normal routeは未接続のままである。

## Failureとcleanup

phase、reason、existing parser/context/process/revision causeをtyped failureとして保持する。失敗時はcontextの
descriptor-relative cleanupを明示実行し、cleanupも失敗した場合はprimary failureを置換せず
`cleanup_consequence`へ別に保持する。retained archive queryで発生したruntime failureは狭いquery境界で
`ArtifactMetadata / ArtifactMetadataQueryFailure`へ翻訳し、元diagnosticを保持する。

artifact inventory拒否、replacement、ambiguous / unsafe workspace、またはpackage build開始後のfailureでは、
context全体を`UnprovenCleanupContent`として保持する。失敗後の再走査で未証明entryを削除対象へ採用しない。
cleanup consequenceはprimary failureとは別にtyped reasonと`retained_root`を返す。この拒否はcontextの寿命中
解除せず、destructorもgeneric cleanupを再試行しない。build開始前のfailureでも`PKGDEST`をemptyと
証明できなければpartial / unproven outputとして同じ拒否を保持する。

generic context cleanupはroot自身を除く合計65536 entries / root配下depth512を上限とする。
全体のplan構築とvalidationを削除開始前に完了し、validation / removalはその有限planと各directoryの
固定inventoryだけを走査する。budget超過は`CleanupResourceLimitExceeded`としてrootを保持し、再試行しない。
他のtransient cleanup failureは明示retryが可能だが、destructorは一度試行済みのcleanupを繰り返さない。

friendを与える各narrow headerは循環依存のないcomplete `EvaluatedDevelSourceBuildAuthority`宣言をincludeし、
callerによる同名class定義やraw observation mintをcompilerのredefinition / access controlで拒否する。

Slice 4はpacman、sudo、installed local DB、installed binding、provenance store publication、
`observe_git_remote_revision()`を呼ばない。

Refs #476
