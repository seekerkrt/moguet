# Pinned submodule source-ready workspace

Issue #564 Slice 4B1の唯一のinput authorityは、liveな
[`AcceptedPinnedSubmoduleClosure`](pinned-submodule-closure-review.md)である。
`materialize_pinned_submodule_workspace()`はwhole ownerをmoveし、
`SourceReadyPinnedSubmoduleWorkspace`へ移す。同じevaluated selection、recipe context、
acceptance、nodes、edges、pins、raw tag identityを保持する。acquisition object backingは全copyと
SourceReady final proof成功まで保持し、その直後にprivateな一度限りのcleanupで解放する。selection release、raw path、
raw metadata、decoded provenanceからのmintは公開しない。

## Local materialization

既存invocation-owned builddirのworking recipeと並ぶ専用leafをfreshに作成する。
materialization自体ではSRCDESTを変更しない。既存leafを発見した場合、所有を推測して
採用・repair・削除せずtyped failureとする。

private transferが各accepted nodeのbare backingをnative
`git clone --local --no-hardlinks --no-checkout --template=`でcopyする。
4Aとは独立したphase deadlineで、copy前後にsourceのretained identity、config exact bytes、
object-store shapeを照合する。immutableな4A backingへrefs、HEAD、worktreeやmodules metadataを
書き込まない。hardlink、alternates、ambient object cacheを使わず、derived storeは独立させる。
derived originはaccepted locatorへ設定するが、revisionを選ぶauthorityにはしない。

Gitはlocal copyでもfile protocol許可を要求するため、private source/targetを指定するcloneの
invocation argumentsだけでfileを許可し、HTTPSを無効にする。その他の4B1 Git commandでは
全protocolを無効にする。local URL overrideやworktree `.gitmodules`の書換えは行わない。

root Xと各child occurrenceをaccepted full OIDでcheckoutする。ls-remote、fetch、remote HEAD /
branch lookup、submodule update、remote fallbackは呼ばない。Moguet自身のGitは既存trustedな
fixed executable/environment primitiveを使う。これはlocal-only code pathであり、OS network
sandboxの保証ではない。

## Root tag projection / reproof (Issue #589)

accepted closureのroot tag mappingを、exact detached root checkout後に
`git update-ref --no-deref --stdin -z`の単一create transactionでfresh作成する。
full ref名とraw OIDはvalidated `PinnedRootTag`からだけ渡し、partial batch updateは使わない。
transactionのmemfdはWRITE/GROW/SHRINK/SEALの4 sealsを設定・確認し、seal後のsize・全bytes・EOFを
validated transactionと再照合してからoffsetを0へ戻してGit stdinへ渡す。seal/reproof失敗はfail-closedで、
tempfile/path fallbackは使わない。
update-ref attemptでのFailureは元のGit/process/cancellation情報のまま再送出し、cleanup refusalを設定する。
seal/prove前のcreate collisionでもpreexisting tag metadataをgeneric cleanupへ採用しない。
namespaceへ触れる前のmemfd準備失敗だけでは、このrefusalは設定しない。
annotated tagをpeeled commitへ変換せず、nested/non-reachable tag objectsも保持する。
そのrootからprivate native mirrorを作り、既存selected branch / derived HEAD adapterを維持する。

local transfer後のSourceReady mint前、native mirror構築後のmakepkg preparation前、
prepared metadata/packagelist後、post-build S4 mint前に、root workspaceと（存在する場合）mirrorの
`refs/tags/*`全logical集合をaccepted mappingと比較する。診断をcaptureした`for-each-ref`と、explicit object引数なしの`fsck --strict`で、
broken/invalid refの黙示的省略を成功扱いにしない。後者はdefault ref rootsも検査し、
`for-each-ref`や`refs verify`が省略するdangling symbolic refも拒否する。
loose/packedの物理表現はauthorityではない。追加・削除・retarget・symbolic tag・同じpeeled
commitへの別raw annotated objectは`TagNamespaceDrift`で拒否する。各storeのformatとraw objectの
strict fsck/hash/connectivity（dangling symbolic refを含む）は`TagObjectInvalid`で拒否し、process infrastructure/cancelは元の分類を保つ。

semantic driftはownership replacementの`UnsafeFilesystem`と区別する。
cleanupは既存sealed inventory/retained identityの契約を維持する。tag proofを完了できない場合は、
root/mirrorの未知metadata subtreeをgeneric cleanupへ採用させず、primary tag/process failureとは別に
workspace cleanup refusalとcontextの`UnprovenCleanupContent`を保持する。これは実際のinode交換を
断定するものではなく、未証明metadataを削除しないための保守的な拒否である。
承認後のMoguet Git acquisitionはなく、native makepkgもaccepted private mirrorを読む。
任意PKGBUILD内部のnetwork accessの一般的な禁止はこの契約に含めない。

## Native topology

child worktreeはparent worktree / edge.pathとする。native `git submodule init`と
`git submodule absorbgitdirs`にgitfile、modules layout、逆方向のcore.worktree生成を任せる。
4B1用の新mirror managerは作らない。

real Git focused fixtureで次のshapeをcharacterizeする。

```text
root/.git/modules/logical/A/
root/deps/a/.git -> gitdir: ../../.git/modules/logical/A
root/.git/modules/logical/A/modules/leaf-name/
root/deps/a/nested/b/.git -> gitdir: ../../../../.git/modules/logical/A/modules/leaf-name
```

logical nameとdeclaration pathは別の名前である。nested childはparent gitdirのmodules namespaceを
使う。同じlocator/pinのsiblingも別repository/worktree occurrenceとして保持する。
sibling logical nameがprefixで重なりmetadata namespaceが衝突する場合は、workspace作成前に拒否する。

supported gitfileはnative canonical relative formだけとする。必要な`..`は許可するが、absolute /
wrong target、余分なbytes、symlinked metadata、workspace escapeは拒否する。
root `.git`はdirectoryとし、linked worktreeは扱わない。root、worktrees、gitdirs、gitfiles、
independent object directoriesをretained descriptorへbindする。fresh native config bytesをsealし、
core.worktreeもexpected reverse mappingと照合する。全Git config keyやhostile environmentの
汎用scannerは追加しない。

## Source-ready phase-point proof

mint直前とexplicit `reprove()`で次を証明する。

- gitfile/modules occurrenceがexpected setと完全一致し、unexpected repositoryがない。
- retained filesystem identitiesとnative gitfile/gitdir/worktree mappingが一致する。
- root HEAD = accepted X、各child HEAD = accepted pin。raw commit type、object format、
  independent backingのstrict fsckも確認する。
- 各parent indexのwritten tree = accepted tree。全mode160000 path/pinを含めて一致させ、
  child HEADだけではparent gitlink proofにしない。
- `.gitmodules`がaccepted exact bytesと一致し、4Aで証明済みのname/path/locator/gitlink対応を維持する。
- untracked/ignored entriesとsubmodulesを含めてGit source stateがcleanである。

通常のGit attributes checkout semanticsを維持する。全blobとraw worktree bytesを再比較する
新契約は追加しない。このproofはprepare前である。4B2 consumerのprepared/post-build proofは通常contentの変換を許可する。

保証は**phase-point proofでありcontinuous attestationではない**。観測できたdriftではfail-closedとし、
workspaceをconsumeする。same-UID userが検証の間に意図的に改変して元へ戻す操作はthreat model外。
background watcher、general-purpose sandbox、all-descendant mediationは実装しない。

## Derived state / cache / cleanup

cache/workspaceは**disposable derived state**でありrevision authorityではない。
persistent user cacheはauthorityに使わず、触らず、削除しない。このproducerは常にfresh stateを
作るため、stale cache repair経路を持たない。失敗したowned stateを安全にdiscardできた後のretryは、
fresh invocation/acceptanceから開始する。unknownまたはidentityが交換されたstateはtyped failureとする。

move-only success ownerがderived workspaceとAccepted whole ownerを保持する。
SourceReadyを返す前に、全nodeの独立copy、tag projection、final proofを完了し、元の4A rootと
bare repositoriesだけを既存のbounded removalで削除する。元repository、root、parentのFDも閉じるが、
selection/context、confirmation、nodes/edges/tagsは同じownerに残す。これはFD closeだけではなく
失敗し得るfilesystem cleanupであり、拒否時は`Cleanup` stageのfailureに元の4A consequenceと
`abandoned_root`を保持し、SourceReadyを返さずmakepkgへ進まない。成功・失敗のどちらも試行済みとし、
後段cleanup/destructorは物理削除を再試行しない。残るworkspace/contextは通常のcleanupを一度行う。
copy途中やfinal proof失敗時にはこの早期releaseへ入らず、既存のfailure unwindでcleanupする。
explicit cleanupはretained workspace bindingsとsealed inventoryを照合してから、既存invocation
contextのbounded removalへ委譲する。unsafe cleanupでは既存contextのunproven-content refusalも設定し、
親のgeneric scanが交換されたsubtreeを再採用しないようにする。materialization途中でinventoryが
未sealでも、取得済みbindingのidentityは照合する。
primary failure、workspace refusal、元の4A process/failure、object/context cleanup consequenceを区別し、
destructorから試行済みcleanupをretryしない。

materialization/reproof各phaseは10分・Git process 4096回に制限する。filesystem observationは
262144 entries・128階層を上限とし、immutable / pre-execution観測にはregular fileの`st_size`合計1 GiBも適用する。
SourceReady seal/reproof、移設前proof、初期SRCDEST inventory、実行前のNativePreparationはこのbyte上限を維持する。
Prepared/PostBuildのmutable structural traversalと実行開始済みownerのcleanupでは、通常の実行生成物の
regular-file合計sizeをsource authorityの条件にしない。root inventoryとnative-src追加scanのどちらも
全path/statを列挙し、生成directoryを除外せず、entry/depth/deadline、device/UID/type、no-follow/containment、
retained identity、Git/module/tag authorityの各検証を維持する。負のregular-file sizeは両modeで拒否する。
個別authority content read、Git owner、artifact ownerはそれぞれ既存の制限を維持し、
proof commandのcaptureは最大1 MiBとする。
cleanup ownership preflightは独立した5秒budgetを使い、その後に既存contextのbounded cleanupを行う。
これらは観測budgetでありdisk quotaやsandbox保証ではない。

## Native makepkg / common S4 consumer（4B2）

唯一のproduction inputは`resume_evaluated_devel_source(SourceReadyPinnedSubmoduleWorkspace)`。
whole owner内のselection/contextをprivateに借用し、成功時はS4 resultへwhole ownerをmoveする。
同じselectionを再評価せず、closureの再取得・再review・workspace再materialize・remote再観測を行わない。

private adapterはretained workspaceを`BUILDDIR/<PackageBase>/src/<evaluated source name>`へ一度relocateする。
childのrelative gitfile/core.worktree bindingとinodeを保ち、native親directoriesもretainする。
同じworkspaceのobject storeから`SRCDEST/<source name>`へindependent local mirrorを作り、accepted Xの
local refをnative selectorへbindする。このnative mirrorはSourceReady後の派生物であり、既に解放済みの
4A backingを参照しない。SRCDESTのpreexisting contentやnative leaf collisionは
採用せずfail-closedとし、ambient/persistent cacheを使わない。

Issue #591では、prepared metadata/packagelist後の既存PreparedReproofが成功した場合だけ、private bridgeから
このretained native mirrorのdescriptorとSRCDEST相対名をcommon S4へ借用する。S4はdescriptorを複製し、
同じcontextのSRCDEST内でnamed entryとretained identityの一致、およびdevice/owner/mode/containmentを確認して
既存Git proofへ渡す。prepareがSRCDEST内へ作る補助cache等のsiblingはmirror候補・選択authorityではなく、
SRCDEST全entry数からmirrorを再発見しない。保持済みmirrorのmissing/replacement/symlinkやconfig/tag等のdriftの拒否と、
未知metadataのcleanup refusalを維持する。通常selection入力のlocatorは変更しない。

準備はnative `--nobuild --nodeps --noconfirm --holdver`を使う。`--holdver`はsource mirrorのremote更新を止める。
native extractionの`git fetch`はinvocation内のmirrorだけを読む。review済みrecipeの
`git submodule update --init --recursive`は既に存在するaccepted childを使える。
native Gitが生成する`objects/info/commit-graph`、`objects/info/commit-graphs/`等の派生indexは
外部object backingではなく、通常のfilesystem検証とGit proofの対象として許容する。
`objects/info/alternates`と`objects/info/http-alternates`、shallow、promisor、grafts、replaceの拒否は維持する。
`--noextract`はprepare()も省略するため準備には使わず、既存build phaseだけで使う。
[makepkgのoption契約](https://man.archlinux.org/man/makepkg.8.en)とactual native fixtureを根拠とする。

prepared metadata/packagelist後とbuild/check/package後に、共通validatorでroot X、各child pin、parent gitlinks、
exact `.gitmodules`、gitfile/gitdir/worktree mapping、retained identity、expected module setを再証明する。
通常contentの変更・生成は比較せず、source-ready clean proofとは区別する。
makepkg phaseへ移った後のcleanupも通常contentのinode集合一致を要求せず、retained metadata/親identityと
既存contextのbounded cleanupを使う。retained replacementは削除せずrefuseする。

通常のS4 gatesはSourceReadyを持たないpathで維持する。S3のAUR recipe snapshotのGitlink拒否は別契約として維持する。
root ActualBuiltGitRevision mint、artifact、install transport、S5/S6は既存実装を共有する。
child pinsはwhole owner内のinvocation-local evidenceでありprovenance v1 / 27 keysを変更しない。

## Remaining scope / validation

production activationはexact target-less ordinary `-Syu` / `-Su` Auto + initial ProvenanceMissing + current typed bootstrap intentだけ。
package名や`-git` suffixでこのbranchを選択しない。通常のvalid-provenance / non-devel経路を変更しない。
Slice 5はsplit PackageBase groupを、Slice 6は代表3topologyのdeterministic coverageを接続する。
[fixtureの範囲](../../tests/fixtures/devel-production-topologies.md)を参照する。live Cargo取得、一般sandbox、
persistent cache manager、continuous監視を今回の対応に含めない。

`test-pinned-submodule-s4-integration`は既存bootstrap fixtureのproduction ownerを使い、新しいrecursive casesだけを
実行する。native prepareによる正当な変更、childを実際に利用したartifact、S6 Complete、root/child remoteの
steady-state、prepared/post-build drift、cancellation/build failure/cleanup refusalを確認する。
new consumer negative compileはborrowed owner、foreign context、private selection extractionを拒否する。
4A、closure acceptance、bootstrap、transport、canonical等の他suiteを前置きで起動しない。
