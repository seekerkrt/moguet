# Package/source patch customization — internal consumer / design

## Statusとauthority

**Slice 2のlocal Candidate Consumerは内部実装。public customizationは未実装。**
[Issue #363 current body](https://github.com/seekerkrt/moguet/issues/363)をrequirements SSOTとする。
以下のDesign Gate比較・永続化案と、今回実装した内部consumerのcontractを区別する。
CLI、association、registration、digest persistenceの完成やProduction AC全体の完了を表さない。
[#627 requirements reset](https://github.com/seekerkrt/moguet/issues/627)に従い、過去の
profile / snapshot foundationを要求へ戻さない。requirementsはIssue、具体的な
patch contractはこの文書、上位原則は[decisions](../decisions.md)と[stance](../project-stance.md)が所有する。

## 接続するcurrent authority

### Source patch application authority（product invariant）

recipe-side customizationとsource patch payloadは別責務である。source patch payloadは原則として
`PKGBUILD` / makepkgのsource・prepare lifecycleを通じて適用する。Moguetは展開済みupstream source treeへ
独自にpatchを適用するgeneric ownerにならない。将来のintegrationはrecipe candidateの`source[]`、checksum、
`prepare()`等で行い、modified recipeからmetadata / dependency / build authorityを再評価したうえで、
actual source取得・展開・prepare・buildをmakepkgへ渡す。直接source mutationの具体的要求が生じた場合は
このIssueの暗黙拡張ではなく別design reviewを必要とする。

今回の会話ではDesign GateをSlice 1、以下のCandidate ConsumerをSlice 2と数える。
Issue本文のproduction Slice 1と同じscopeであり、source patch payloadは実装しない。

### Slice 2で実装する内部contract

`prepare_local_recipe_build`はalready-selectedなowning patch bytesのordered vectorを受ける。
external path lookup、digest/association store、登録、saved preference、public dispatchを所有しない。
callerはcandidateでのpre/post二度のmetadata評価を許可した後だけこのmutation-capable seamを呼ぶ。
全materialのPKGBUILD-only shapeをworkspace作成・評価前に検証し、非対応file / binary / mode変更 / 空materialは停止する。
Git unified textのenvelopeだけを検査し、context照合と適用は実Gitへ委譲する。stripは1、入力bytesは同じ
invocation-owned streamからcheck / applyへ渡し、Git config/indexとoriginal checkoutからauthorityを借りない。

early `LocalSourceWorkspace`でfresh prepatch metadataを評価し、original canonical pathとPackageBaseの
known identityを確立する。ordered apply後のfresh metadataについてPackageBaseとordered child namesの不変を確認し、
そのmetadataだけからlocal planを生成する。original/candidateのphysical identity、prepatch/modified recipe snapshot、
effective environmentを保持したmove-only `PreparedLocalRecipeBuild`だけを公開し、temporary pathをsemantic identityにしない。
既存build ownerへのprivate transferで同じworkspaceを消費し、再snapshotやprepatch requestへの差し替えをしない。
dependency executionは既存callerの責務、local build結果は既存artifact/install authorityへ渡せるcapabilityのままとする。

material preflight failureでは全apply outcomeをNotAttemptedとし、rejected entry indexを別に保持する。
apply中のfailureはApplied / Failed / NotAttemptedを保持し、tool non-zeroはConflictを推測せずToolFailureとする。
metadata/identity/plan failureとbuild phase failureを分離し、candidateを公開しない。source cleanupは既存の一度だけの
ownerへ委ね、primary failureとsecondary cleanup failureを保持する。途中で放棄されたprepared candidateは通常RAII cleanup、
build後のsource cleanup failureはinstall可能な成功resultへ変換しない。original PKGBUILD / `.SRCINFO`へ書き戻さない。

`test-local-recipe-candidate`は実Git / makepkg / archive readと既存query stubで、ordered apply、fresh dependency plan、
original保全、identity guard、途中failure、cleanupを確認する。public optionやhelp/man/completionは追加しない。

| Authority | 維持する責任と接続上の制約 |
| --- | --- |
| [#355 identity](source-package-identity.md) | source kind / location / repository、PackageBase、child、revision、releaseを分離する。value equalityやgeneric compatibilityはpatch適用許可ではない |
| [local source](local-pkgbuild.md) | `LocalSourceRoot`の原本identity、`LocalSourceWorkspace`のowned snapshot / cleanup。semantic local pathとtemporary candidate pathを区別する |
| [reviewed AUR source](reviewed-source-state.md) | exact upstream OID、review acceptance、pinned continuation、editor overlay。patch保存や将来の適用許可を所有しない |
| local metadata / plan / request | `LocalSourceBuildMetadata`のrecipe / environment相関、local dependency projection。現行は原本metadata / planの後にsource snapshotを作るため、patch consumerには順序変更が必要 |
| `ArtifactWorkspace` / `ArtifactMakepkgContext` | fresh PKGDEST、packagelist / build、artifact検証。source candidateやdurable materialのownerにはしない |
| [source preference](source-build-preference-xdg.md) / `SourceBuildEnvironment` | `source-build.d`とordered assignments。#362の`--use-preference`、empty値、one-offとの競合規則を再所有しない |
| XDG safety / strict readers | descriptor / named identity、owner / mode、I/O failureを扱う。preference parserのinvalid assignmentをwarningで無視する仕様はpatch recordへ流用しない |

remote AURはRPC由来planをcheckout / editorより先に作る。既存editorへのapply追加だけでは
modified dependency authorityにならない。remoteの`InvocationOwnedRecipeAcquisition`も
devel bootstrap専用であり、汎用candidate ownerとして転用しない。

## Ownership方式の比較と提案

| 判断軸 | A: user directory / reference | B: managed copy / import | C: manual edit → generated diff |
| --- | --- | --- | --- |
| User ownership | bytesは利用者が管理。Moguetはassociationと今回のcopyのみ | originalは利用者、import済みbytesはMoguet管理。二つの版を区別 | 編集前baseline / 編集結果の相関と生成物ownerが必要 |
| Durable persistence / path消失 | associationは残るが原本消失はMissingで停止 | import元消失後も保持できる | 生成方式だけでは保存先を決められずA/Bが別途必要 |
| 再現性 / byte変更 | 保存digestと一致するbytesだけを固定。過去bytesの復元はしない | 保存bytesとdigestを固定できるがbuild全体の再現性とは別 | exact baselineとdiff生成対象の証明が追加で必要 |
| Backup / restore | configとmaterial双方が必要。移設は明示rebind | associationとmanaged dataの整合したbackupが必要 | baseline情報と生成diffのbackupも必要 |
| Update / replace | 利用者が編集後、明示更新でorder / digestを置換。旧digestの自動追随なし | reimport / replace / forget、partial import / publicationを所有 | 再編集、生成失敗、既存patchとの重複・順序も所有 |
| Filesystem safety | 外部root / listed filesのsafe read、race / change検知、owned copy | Aのimport readに加えdurable write / publication / cleanup | editorとbaseline / modified candidateのlifetimeも追加 |
| 実装量 / schema | 最小recordとnarrow reader / adapter。初版のstrict versionのみ | bytes store、整合publication、data resolver等が増える | diff authoring producerと保存方式の両方が必要 |
| 既存authorityとの親和性 | user-owned input → owned candidateと整合 | 可能だがcacheやreviewed stateへbytesを押し込めない | invocation-local editor overlayをpersistent authorityへ昇格させない追加設計が必要 |
| 将来source payload | type / phaseを別に追加できる。今は実装しない | 同様。import自体はprepareへのintegrationを解決しない | 既存のuser-supplied patchesを作り直す理由にならない |
| 今不要な責任 | 自動追随、原本保管、汎用directory管理を持たない | 原本消失後の保管保証とmanaged lifecycle | authoring UI、baseline取得、diff生成 |

**提案はA。** 現在のgoalは保持したassociationとpatch群の更新後reuseであり、original pathの
消失後にもbytesを保管する保証は固定requirementではない。Aでも変更検知と安全なcopyは省略しない。
Bはその保管保証が必要になった場合の次候補、Cはauthoring需要が確認できた場合の追加producerとする。

## Design Gate 6項目の提案

| Gate | Proposed initial contract |
| --- | --- |
| 1. Material ownership | A。user-maintained directoryの明示されたpatch filesを正本とする。buildごとに検証済みbytesをinvocation-owned copyへ固定し、原本を変更・削除しない |
| 2. Association / selection | Known sourceを持つ`PackageBaseIdentity`に1 series。初期はLocal + canonical original path + PackageBase。invocationで保存associationを明示選択する。名前の類似、child名、provider、default / inheritanceから推測しない |
| 3. Series / root / phase | 非空のordered entries（relative material path + SHA-256）。初版typeはGit unified text patch、rootはowned recipe root、phaseはcustom build用metadata再評価前（recipe-side）、stripは1に固定する |
| 4. Apply policy | explicit invocation / selection。登録は将来の自動適用許可ではない。選択済みmaterialのfailureをstock buildへfallbackしない |
| 5. Initial scope | `build --local`相当のlocal routeに接続する、既存regular `PKGBUILD`のtext modificationだけ。remote / official / AUR、他file、source payload、任意source treeは初期非対応 |
| 6. Persistence / XDG | association・order・期待digestは独立したXDG config namespace。material bytesは外部user directory、candidateとmaterial copyはinvocation-owned disposable storage |

### Associationと更新時reuse

source identityはofficial / AUR / localで同じvalue modelを使えるが、producerは異なる。
officialはresolved source-buildのrepository name + Git URL + PackageBase、AURはresolved canonical
Git URL + PackageBase、localはcanonical original path + freshに確認したPackageBaseが必要。
repository rootのpackage nameだけではPackageBaseがなく、complete keyを作れない。
temporary copyのpath、derived canonical keyの逆parse、artifact名をassociation keyにしない。

初期local routeはprepatch candidateのfresh metadataでPackageBase / childrenを確認してからapplyする。
local revisionは`Inapplicable`のまま許可し、Git OIDを要求しない。remoteへの拡張でも#411のexact OIDを
generic projectionへ注入しない。release / OIDはupstream更新で変わるためdurable association keyには
含めず、same associationはapply成功の証明としない。毎回current candidateへの適用を確認する。

canonical pathは「この場所のsource」という利用者の指定であり、同じpathが過去と同じfilesystem object
である保証ではない。各invocationでruntime owner / containmentを検証する。別pathへの移動や
PackageBase変更は明示rebindとし、similar nameへの追随、rename推測、inodeの永続key化は行わない。
postpatchのPackageBaseとordered child identityはprepatchと一致を要求する。version、dependency、
build optionの変更はfresh metadataで扱う。split childrenは既存local selection / install reasonを維持する。

### 最小recordとmaterial read

候補配置は`${XDG_CONFIG_HOME:-$HOME/.config}/moguet/patches.d/`。1 associationにつき1 strict recordで、
version、typed source / PackageBase、absolute material root、ordered relative pathsと各SHA-256を保持する。
storage keyはtyped identityから決定的に導出し、record本文と照合する。named profileや別manifest languageは作らない。
順序は登録時の明示listそのものとし、directory列挙順、glob、filename sort、mtimeを使わない。
初版はroot直下のregular patch filesに限定し、空series、duplicate path、absolute / traversal pathを拒否する。
type / apply root / phase / stripはversion 1の固定値なのでentryごとの自由な設定にしない。
将来の別type / phaseを現版が黙って解釈せず、unknown version / field / type、duplicate key、壊れたrecordは停止する。
古いpatch schemaは存在しないためmigration engineは作らない。

登録・更新はmaterialのsafe readとdigestを利用者が確認したうえでrecordをatomic publicationする。
同じassociationへの置換は明示操作とし、同時更新はcooperative lockと期待recordの照合で拒否する。
forgetはassociationだけを削除し、external bytesを削除しない。material path変更も明示更新とする。
read / list / selected buildはstoreを作らず、未選択の既存routeはpatch storeを読まず現在の挙動を保つ。
未登録は管理操作でのabsenceと、明示選択したbuildのMissing failureを区別する。

configは既存XDG boundaryに沿い、unset / emptyはHOME fallback、明示baseはabsolute・既存・安全を要求する。
managed directory / recordは0700 / 0600、euid ownership、descriptor / named identity、symlink拒否、
atomic write / syncを必要とし、I/O、permission、race、partial publicationをabsenceへ丸めない。
`source-build.d`やreviewed-sources stateに新fieldを混ぜず、root時も別userのcontextを推測しない。

external materialはprivate configと異なり、euid-ownedでgroup / other writableでないdirectory / regular file
（0755 / 0644等）を許可する。rootへのlineageとlisted fileをdescriptor基準で確認し、symlink経由、
unsafe owner / writable component、special file、root escapeを拒否する。原本のchmodはしない。
euid ownershipの要求はmaterial rootとlisted fileに適用する。通常のroot-ownedなsystem ancestorを
一律拒否するものではなく、ancestorの安全性とnamed lineageは別に検証する。
bounded readの前後でnamed / descriptor identityと変更を確認し、全entryのSHA-256一致を確認した同じbytesを
owned copyから使う。apply時にexternal pathを開き直さない。未列挙fileはseriesに追加しない。
missing、changed、unsafe、corrupt、I/O / raceは別reasonで停止し、changedをその場で自動承認・digest更新しない。
snapshot固定後はexternal originalの監視を継続しない。copyと必要な相関情報をconsumerの寿命まで保持する。

Bを採る場合、durable materialはuser dataとしてXDG_DATA_HOME側が自然であり、再生成可能なcacheや
review acceptance stateへ置かない。current resolverはConfig / State / Cacheのみなので、data用resolver / safety、
atomic import、replace / forget、backup整合もその方式のscopeになる。Aではこれらを先行追加しない。
[XDG specification](https://specifications.freedesktop.org/basedir/latest/)のconfig / data / state / cacheの責務分離に従う。

### Apply policyの比較

| 候補 | 初期判断 | 必要になる責任 / 理由 |
| --- | --- | --- |
| every time ask | 条件付き次候補 | 通常routeでassociationを発見しpromptする責任が増える。non-TTY時の扱いも必要。明示選択後のexecution consentとは別 |
| first-use ask then remember | 初期不採用 | remembered authorityのscope、material / identity / upstream変更時の失効を新設する |
| explicit invocation / selection | **推奨** | invocationの意図と必要materialが明確。保存associationだけで既存buildの意味を変えない |
| package-specific automatic apply | 初期不採用 | package名だけでは不足。source選択、失効、missing / changed / conflict、confirmation authorityが必要 |
| defined-condition automatic apply | 初期不採用 | 自動化条件と再評価・失効policyが現在のgoalに不要 |
| never automatic | 初期挙動として採用 | 明示選択なしには適用しない。将来の自動化を永久禁止する仕様にはしない |

patch選択はPKGBUILD実行同意、upstream review acceptance、package transactionの確認を代替しない。
初期localではowned candidate上のpatch前identity評価とpatch後metadata評価を行うことを表示し、
既存localのno-default evaluation consentを維持する。まとめて同意を取る場合も2回の評価範囲を明示する。
`--noconfirm` / non-TTYを評価同意へ昇格させず、dry-runはapply / metadata評価 / store作成を行わない。
必要な評価がないと判定できない場合はBlockedとする。

### Candidate、metadata、outcome

初期consumerの必要順序は次のとおり。既存local route全体を汎用workspace managerへ変更しない。

```text
explicit selection + source/material preflight
→ early owned recipe snapshot / candidate review + evaluation consent
→ fresh prepatch metadata（association identity確認専用）
→ association照合 + material snapshot照合 + ordered recipe apply
→ modified candidate review / fresh postpatch metadata
→ postpatch identity guard / dependency plan / prepared build request
→ existing dependency execution
→ same candidate + same effective settings/environmentでpackagelist / build
→ artifact検証 / selection → source cleanup → install → artifact cleanup（既存ownerの境界を維持）
```

registrationでもsource identityを実証し、reuse時に再確認する。原本PKGBUILDをeditor / evaluation cwdへ
渡す既存local分岐をpatch routeへ流用しない。初期は保存series以外のeditor mutationを合成せず、
追加編集はexternal materialの明示更新と次invocationで扱う。reviewはcandidate内容を判断する境界として残す。
prepatch metadata、original / copied `.SRCINFO`、RPC、prepatch dependency planをcustom buildへ流用しない。
生成`.SRCINFO`相当の出力はinvocation内でparseし、user originalへwrite backしない。

adapterはoriginal semantic identityとphysical candidate identityを別に保持し、selected bytes / series、
modified recipe、fresh metadata、environment、plan / requestが同じcandidate generationに由来することを
評価完了・plan採用・makepkg開始の境界で照合する。既存workspaceのdirectory identity検査だけを
recipe bytes不変の証明にしない。candidate変更の観測後は以前のmetadata / requestを失効させ停止する。
必要なphysical ownerはdependency preparationからbuildまで保持し、後段に必要なsemantic evidenceは別に残す。
既存PKGDEST拒否、environmentの順序とempty policy、artifact / install authorityを弱めない。
localにsaved preferenceを新しく適用せず、#362完了をdependencyにも新たな所有責任にも置かない。

applyはGit unified text patchの既存toolに委譲する。初期はcontext付きの`a/PKGBUILD` → `b/PKGBUILD`の
通常text変更のみで、作成・削除・rename・mode変更・binary・symlink・別path・source phaseをrejectする。
allowlist以外をfilterで捨てて「成功」とせず、全入力がsupportedであることを確認する。
Gitのcheck / applyを同じ固定bytes・root・policyで順番に行い、merge、reverse、reject残し、
context無視、whitespace自動修正へfallbackしない。hunk適用器やshell semantics analyzerは作らない。
[git apply](https://git-scm.com/docs/git-apply)の単patch失敗時の挙動をseries全体のatomicityと取り違えない。
途中失敗時は部分適用candidateをbuildへ公開せず、owned cleanup / diagnostic retentionへ渡す。

outcomeはInvalid、Unsupported、Missing、Changed、Unsafe、NotApplicable / Conflict、ToolFailure、Unknownを
区別できる範囲で保持する。tool exit非zeroやstderrだけからconflictを捏造せず、分類不能ならUnknownで停止する。
apply完了、metadata評価、build、install、cleanupは別outcomeとし、primary failureをcleanup errorで上書きしない。
apply成功はshellの意味やsource安全性の認証ではない。phase間の全same-UID変更監視、sandbox、automatic repairは行わない。

## 初期非採用とSlice案

PKGBUILD-onlyは最初のconsumerの制限であり永久仕様ではない。次の候補はselected recipe-associated text inputs、
その次がuser-supplied source payloadと明示的なrecipe側の`source[]` / checksum / `prepare()` integrationである。
payloadを置いただけでsourceへ適用済みと扱わず、展開sourceへの適用はmakepkgに委ねる。
remote追加時はmodified metadataからのreplanとreviewed upstream / customizationの分離を先に証明する。
arbitrary source tree mutation、B/C同時実装、profile / patch DB / migration foundation、#484変更は初期scope外とする。

最初に公開するproduction consumerはAの登録・明示更新・forget・明示選択からlocal buildまでの一つのjourneyとする。
実装は次の小さいSliceに分け、途中のgeneric foundationや未完成public optionを公開しない。

1. **Candidate consumer:** local early snapshot、PKGBUILDだけのordered apply、pre/post metadataとoriginal / candidate
   identityの相関をnarrow adapterで実証する。apply失敗・metadata変化のfocused regressionを先に閉じる。
2. **Associationとmaterial acquisition:** 初期consumer専用record、strict read / digest snapshot、明示登録・更新・forget。
   configとexternal bytesの非破壊・missing / changed / unsafe / corruptを検証する。record encodingを固定する。
3. **Public end-to-end接続:** 明示selection、consent、postpatch plan / request、既存build / install / cleanupへ接続。
   CLI spellingとhelp / man / completion / docsを同期し、upstream変更後reuseとno stock fallbackをfull-CLIで確認する。

このconsumer完成後の独立Sliceは実例に必要なselected recipe text inputsとし、対象input集合とcandidate generationの
相関を拡張する。source payload integration、remote routeはそれぞれ別の具体的需要とauthorityを確認してから追加する。

最初のSliceでは既存identity / source environment / local workspace / local buildとfull-CLI fixtureへ、
wrong source / PackageBase、material変化・消失・unsafe / corrupt、series途中失敗、patch後dependency変更、
original非変更、cleanup failureを追加する。tool protocolのfailure分類もfocusedに検証する。
fixture側にpatch/buildを再実装しない。実行範囲は[validation policy](../validation.md)へ従う。

## 後続Sliceに残る責任

local Candidate Consumerは今回の実装scopeとして確定した。A reference方式のassociation / strict acquisitionと
persistence、public explicit selection / consentは後続Sliceで接続する。ここにあるstorage候補、CLI spelling、
record encodingは現在のpublic contractではない。material path消失時の停止、明示更新、no stock fallbackを
そのconsumerで証明する。remote、selected recipe text、source payloadはそれぞれ別のauthority確認を要する。
