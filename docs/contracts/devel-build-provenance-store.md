# Devel build provenance store foundation

## 文書の位置づけ

この文書はIssue #476 Slice 2で確立する、将来のactual build/install proofを保存するための
internal XDG current-provenance store contractである。Slice 2終了時点ではproduction lifecycle、
CLI、AUR update assessment、Issue #475 observerのいずれにも接続しない。

## Authority separation

次を同じauthorityへflattenしない。

```text
#411 reviewed-source state record
    != actual built Git revision
    != built artifact evidence
    != installed artifact binding
    != provenance-store generation
```

#411 bindingは、AUR source identityとPackageBase、exact reviewed recipe OID、#411 record
generation、observed raw state documentのSHA-256を保持する。#411 state path、generation leaf、
device、inode、mode、link count、mtime、ctimeはruntime filesystem/CAS proofであり、provenanceの
persistent business identityへ保存しない。将来のconsumerはcurrent #411 store readに対して、
semantic state、generation、raw document digestを再照合できる。

generation ownerは次のとおり分離する。

| Generation | Owner / meaning |
| --- | --- |
| #411 reviewed-state generation | reviewed recipe state lineage |
| provenance generation | low-level immutable-generation storeのfilename/observed token |
| installed record generation | pacman local DB record replacement identity |

provenance payloadはprovenance generationを保存しない。これによりpayload generationとfilename
generationが不一致になる二重authorityを作らない。
#411 generationはlow-level storeと同じ`uint64_t`全域を保持するため、schemaではleading zeroなしの
canonical decimal stringとして保存し、0、符号、overflow、integer representationを拒否する。

## Namespace and lookup

provenanceは#411と共有しない。

```text
${XDG_STATE_HOME:-$HOME/.local/state}/moguet/devel-build-provenance/aur/<PackageBase>/
```

PackageBaseはtyped AUR identityで検証した後にunit leafへ使い、document内のAUR source / PackageBaseを
lookup keyと再照合する。read-only lookupはdirectoryやrecordを作らない。Missing、invalid current
document、corrupt record、source mismatch、PackageBase mismatch、future schema、unsafe history、
authority unavailable、store failureを区別し、Missingへ丸めない。

## Schema and CAS

current schemaは、exact #411 binding、evaluated Git source/selector、actual built Git OID、one-child
artifact evidence、one-child installed bindingだけを決定的なTOML documentへ保存する。cache/cwd/srcdir/
artifact path、build log、localized text、環境snapshot、build historyは保存しない。decoderはboundedな
recordに対してstrict type、known field、enum、package/source identity、SHA-1/SHA-256 OID、SHA-256
digestを検証し、duplicate/unparseable documentをcorruptとして拒否する。

first publicationはexact Missing predecessor、replacementはprior readのexact observed raw/filesystem tokenを
要求する。last-writer-wins、blind overwrite、自動retryは行わない。同一payloadでもexact predecessorを
指定したpublicationはnew generationとし、Slice 2でimplicit idempotenceをauthority化しない。

invalid/corrupt/mismatched current recordはexact raw tokenがあっても自動rebindしない。future schemaは
current schemaとしてparseせず、上書き/downgradeしない。immutable historyのfork、gap、orphan、
unrecognized managed entry、unsafe file/statusは最新らしいrecordを選ばずfail closedする。

no-replace commit後のfile/directory fsync、identity、lineage、history reproofが確定しない場合は
`PublishedUncertain`として保持し、definite `Published`へ丸めない。

## Publication phaseとresource failure

shared generation storeは`PreCommit`、`Committed`、`VerifiedPublished`を区別する。
retained inodeのno-replace `linkat`が成功した直後に、allocation不要のphaseを`Committed`へ固定する。
record/leaf/pathのterminal evidenceは可能な範囲でcommit前に確保し、識別できたcommitted recordは
resource failure時にもmoveで返す。commit前の`bad_alloc`/`length_error`は`ResourceFailure`、
commit後は`PublishedUncertain(ResourceFailure)`であり、例外によってcommit済みrecordを
definite failureへ書き換えない。予期しない内部例外もfilesystem処理開始後はphaseを保持する。
invalid caller configurationの既存programmer-error契約は維持する。

emergency resultは追加allocationを必要としない。最初のallocationで失敗した場合のdiagnostic pathは
空になり得る。commit後でもdescriptor identityの取得前ならobserved recordを推定して付けない。
これはrollback/unlink、retry、readbackによる成功adoptの許可ではない。

semantic wrapperは成功/uncertain armが所有するsemantic valueをlow-level write前にcopyする。
low-levelのverified `Published`/`PublishedUncertain`取得後はnothrow moveで結果を保持する。
この境界は共通foundationを使う#411 reviewed storeでも維持する。live final proofのpublisherや
Slice 6 aggregateを追加するものではない。

## Managed namespaceのdurability

publicationではunitのcontaining store directoryをfsyncした後、resolver-owned managed entriesの
containing parentsを最深部からexisting anchorまで同期し、record `linkat`より前に完了させる。
同期範囲はprepared capabilityが保持するmanaged component数で固定し、filesystem rootやanchorより上へ広げない。
readable descriptorはretained lineage FDに対する`openat(".")`から取得し、diagnostic pathnameを再openしない。
descriptor/named lineageを同期前後に再検証する。

| Resolver | commit前のdirectory sync対象 |
| --- | --- |
| explicit XDG_STATE_HOME | `<anchor>/moguet/devel-build-provenance/aur`、その上のmanaged parents、existing `<anchor>` |
| HOME fallback | `<HOME>/.local/state/moguet/devel-build-provenance/aur`、その上のmanaged parents、existing HOME |

最初のstore-directory syncはPackageBase unitのentryを、それより上のsyncは各managed directory entryを
永続化する。record自身はfile fsync、recordのentryはcommit後のunit-directory fsyncが担当する。
同期は`created_this_invocation`に限定しない。前回のmkdir後のfailureで残ったunit/namespaceを採用する場合も
同じbounded chainを同期する。親のsync failureはprecommitのtyped failureで、new generationをpublishしない。
directory residueは残り得るが、named cleanupやglobal `sync`/`syncfs`は行わない。

read-no-createは不変であり、lookup/prepare capabilityの取得だけでこの同期を実行しない。
既存の`test-xdg-generation-store`、`test-devel-build-provenance-store`、`test-reviewed-source-state-store`、
`test-xdg-directory-safety`でresource phase、namespace同期、residueと既存CAS/chain契約を検証する。

## Construction authority

`InstalledArtifactBinding`全体、actual built revision、persistent #411 bindingのraw decoder constructionは
codec専用のprivate accessに閉じる。test fixture mintはcompile definition付きtest targetだけに存在する。
S5-Aではcycle-freeな`devel_build_provenance_decoder_authority.hpp`をfriend付与元のnarrow headerと
codec headerで共有する。installed binding headerだけをincludeしたconsumerによる同名decoder定義も
compileで拒否する。正規persistent decodeは従来どおりhistorical valueを復元し、fresh live authorityを与えない。
S5-Bの[live installed observation producer](exact-installed-artifact-binding.md)は別authorityであり、
complete private declarationを共有する。persistent decodeからfresh live bindingへ昇格する入口はない。
[S5-C final proof](installed-devel-source-build-proof.md)もin-memory capabilityであり、token、receipt lifetime、
fresh binding capabilityをpersistent schemaへ入れない。将来のSlice 6がsemantic valueをprojectionする。
このstoreのschema、publicationと通常routeへの未接続は変更しない。

## Production-disconnected boundary

Slice 2で追加するstore/modelはproduction binaryへcompileできるが、normal invocationからlookup、directory
creation、publicationを呼ぶconsumerは存在しない。source-build phase、makepkg、Git workspace observation、
install receipt、AUR comparison、CLI output/exit statusは変更しない。

## Non-scope

- private build workspace、actual makepkg phase変更、actual Git workspace observation
- installed binding query、Install/Upgrade receipt、post-install publication
- Issue #475 observer connection、AUR update comparison、CLI integration
- migration/rebind command、build history database、split PackageBase provenance
