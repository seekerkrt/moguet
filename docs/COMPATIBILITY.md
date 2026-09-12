# Moguet 互換性と pass-through policy

MoguetはArch Linux向けの **pacman-first wrapper** として扱う。日常的な`pacman` workflowをできるだけ自然に通しつつ、AUR build / installとsource-build preferenceを補助する。MoguetはArch Linux / pacman / AURの公式toolではなく、pacmanや既存AUR helperとの完全互換を宣言しない。

この文書は、利用者がroute差分、pacman / makepkgとの差、pass-through、対応 / 非対応範囲を理解するためのcompatibility summaryである。Issue別production contractの詳細なnormative authorityは[`docs/contracts/`](contracts/README.md)に置き、この文書へ独立した全文contractを重複保持しない。

<a id="compat-route-overview"></a>

## 基本方針

- Moguetが明示的に扱うoperation / optionはMoguet側で解釈する。
- Moguet固有optionはpacman executionやreview前の`.SRCINFO`更新判定へ渡さない。
- pacmanが自然に扱えるoperation / optionは、Moguetがrouteの意味を安全に保てる範囲でpacmanへpass-throughする。
- AUR / source-build経路では、pacman transaction optionを無条件にmakepkg optionへ置き換えない。
- 未対応option、曖昧なselection、authoritative metadataのfailure、安全に意味を保てないpacman optionは黙って無視せず、external mutationより前に停止する。
- `--noconfirm`は「全部yes」ではなく、対応済みpromptの確認を省略する指定である。未解決dependency、provider、conflict / replacement、削除、source selection、local PKGBUILD metadata evaluation、artifact identityの曖昧さを承認しない。
- 値を取るoptionは、値をtargetと誤認しない。値が欠けている場合は停止する。

<a id="compat-general-route-matrix"></a>

## Route matrix

| Operation / route | Moguetのauthorityと動作 | pacman / makepkgとの差 |
| --- | --- | --- |
| plain `-S` Auto | official packageはbinary repository、source preferenceがあるofficial packageはofficial source-build、officialにないtargetはAURへ分類する | pacman単独のbinary repository searchにAUR/source-build分類を補う |
| `-S --aur` | AUR RPC / PackageBase / AUR build planだけを使い、official packageやsource preferenceへfallbackしない | `--aur`はMoguet selectorでありpacmanへ渡さない |
| `-S --repo` | official binary repositoryへ限定し、AUR / source-buildへfallbackしない | selectorを除いたargvをpacmanへ渡す |
| exact target-less `-Syu` Auto | official repository system update成功後にfreshなinstalled foreign inventoryを取得し、normal installed AUR updateを行う。independent devel `RequiresCheck`はattention付きskip、required relationはblocker。saved source-build preferenceはzero-readで適用しない | ordinary AUR-helper updateとしてrepository transactionと後続AUR transactionをsequentialにorchestrateする |
| exact target-less `-Syu --repo` | official repository system updateだけを行い、AUR inventory / RPC、source preference、cache / Git / makepkgへ到達しない | semantic `--repo`を除き、compatibleなpacman argumentをrepository phaseへpass-throughする |
| `-Ss` | official searchとAUR searchを組み合わせる。非対話でprovider / root selectionを開始しない | pacman searchを表示し、MoguetがAUR searchを補完する |
| `-Si` | officialを優先し、見つからない場合だけAUR metadataを表示する。`--aur` / `--repo`はsourceを限定する | AUR infoはpacman infoではなくtyped AUR metadataを表示する |
| remote `build` / local `build --local` / `upgrade` | remote package routeとlocal PKGBUILD production routeを、source preference、BuildPlan、makepkg、artifact validation、`pacman -U`を分離したsource lifecycleで扱う | `makepkg -sic`一括委譲へ戻さない |
| `deps` / `plan` | dependency inspection / plan表示だけを行い、build / install / cloneを開始しない | read-only observationとmutationを分ける |
| `fetch` | 未取得repositoryのclone、既存cloneの`git fetch origin`までに留める | working tree update、pull、merge、reset、build、installを行わない |
| `-G` / `-Gp` | root PackageBaseだけを一時cloneし、exportまたはPKGBUILD表示を行う | dependency plan、makepkg、pacman、sudo、persistent checkoutを行わない |

source routeのselection、preflight、partial completion、failureの詳細は各contractが正本である。routeの結果をpackage nameだけへflattenして別sourceを再推定しない。

<a id="compat-moguet-operations"></a>

## Closed CLI grammar

Moguet-owned operationと、Moguetがinterceptするsource-aware `-S --select`のcanonical
grammarは次のとおりである。

<!-- CLI CANONICAL GRAMMAR BEGIN -->
```text
build <pkg> [V=K...]
build --local <directory> [V=K...]
upgrade
upgrade-aur
upgrade-all
clean
deps [--recursive] <pkg>...
plan <pkg>...
fetch <pkg>...
add-src <item>...
edit-src <pkg>...
list-src
del-src <pkg>...
revert <pkg>...
-G <pkg> [--output-dir=DIR]
-Gp <pkg>
-S --select [--needed] <query>
-Syu [--needed]
-Syu --repo [--needed]
```
<!-- CLI CANONICAL GRAMMAR END -->

remote / local `build`はprimary operandをexactly oneだけ取り、その後には`V=K`
assignmentだけを許すため、extra bare operandを拒否する。`upgrade`、`upgrade-aur`、
`upgrade-all`、`clean`、`list-src`はtarget-lessであり、target operandを拒否する。一方、
`deps`、`plan`、`fetch`と`add-src`、`edit-src`、`del-src`、`revert`は表示どおり
multi-targetを維持する。`add-src`ではpackage itemが後続assignmentのscopeを開始する。

`--local`はlocal `build`、`--recursive`は`deps`、`--select`はplain `-S`だけが所有する。
`-S --select`の`--needed`はroute-owned final installationだけへ作用する。Moguet固有routeに
scope外optionを指定した場合は黙って無視せず停止する。上記以外のpacman operation formは
delegated open grammarであり、この一覧をpacman parserのclosed allowlistとして扱わない。

2つのexact target-less `-Syu` formはMoguet-intercepted semantic routeである。Auto formの
closed compatibility surfaceと、RepoOnly formのcompatibleなdelegated repository tailを
区別する。RepoOnlyのcanonical syntaxに全pacman optionを列挙しないことは、そのopen
pass-throughをclosed allowlistへ縮める意味ではない。

`deps`、`plan`、`fetch`、`-G`、`-Gp`は調査・表示・取得段階であり、build / installを
混ぜない。

<a id="compat-syu-normal-aur-update"></a>

## Exact target-less `-Syu` compatibility

exact canonical tokenかつtargetを持たない`moguet -Syu`だけをordinary AUR-helper updateとして
interceptする。actual phase順は次である。

```text
official repository system update
  -> success
  -> fresh installed foreign inventory / exact AUR metadata
  -> normal AUR plan, provider / relation / artifact preflight
  -> AUR build / install
```

repository transactionが失敗した場合、AUR phaseはtyped `NotAttempted`のまま0-callで終了し、
commandはnon-zeroである。repository成功後にinventory、AUR metadata、plan、provider decision、
preflightをfreshに取得し、repository前またはdry-runのobservation / prepared capabilityをactual
authorityとして再利用しない。repository completion後のAUR blocker / query / preparation /
execution failureはtyped partial failureかつnon-zeroであり、完了済みrepository transactionを
rollbackしない。cleanup failureはinstall resultからflattenせず、inconsistent aggregateはfail
closedとしてsuccessを表示しない。repository package stateのauthoritativeなbefore / after
evidenceを持たないため、AUR `NoUpdates`だけを根拠にoperation全体を`NoOp`と呼ばない。

このnormal AUR phaseはsaved registered / package preferenceをtarget selectionへ使わないだけで
なく、`${XDG_CONFIG_HOME:-$HOME/.config}/moguet/source-build.d/`のsnapshot / 列挙 / strict read、
childからPackageBaseへのfallback read、preference-derived environmentへの適用を行わない。
valid / invalid / unreadable preferenceやinjected read failureが存在してもconsume / `Absent`化せず、
このrouteへ影響させない。一方、`upgrade`、`upgrade-aur`、`upgrade-all`のactual / dry-runは
source-awareなStrict readerとしてsaved preferenceを確認・適用する。`upgrade-aur`のAUR-onlyは
repository system updateを行わないscopeを表し、saved preferenceを無視する意味ではない。

Auto combined routeでinitially対応するpacman semantic optionは`--needed`だけで、exact ordered
repository argvへだけ保持し、later AUR installへ伝播しない。他のpacman semantic optionや
unsupported argument formはrepository mutation前にfail closedし、repository phaseだけを実行して
AURを黙ってskipしない。full compatible pacman pass-throughが必要なら
`moguet -Syu --repo`を使う。このRepoOnly formはsemantic selectorをpacmanへforwardせず、
repository upgradeだけを行い、AUR inventory / RPC、source preference、cache、Git、makepkg、
source runnerを呼ばない。`moguet -Syu --aur`はunsupportedであり、AUR-only source-aware
operationのcanonical surfaceは`moguet upgrade-aur`である。`--noconfirm`はprovider ambiguity、
conflict / replacement、required VCS/devel `RequiresCheck`その他のguardを突破せず、independent
`RequiresCheck`をautomatic updateへ昇格させない。

composite化するのは上記exact formだけである。`-Sy`、`-Su`、`-Suy`、`-S -y -u`、
`-S --refresh --sysupgrade`、target-bearing `-Syu <pkg>`、unknown modifier formはcurrent routingを
維持し、installed-AUR sweepへ拡張しない。

<a id="compat-dry-run"></a>

## Unified dry-run compatibility

global `--dry-run`は、Moguet-owned supported `-S` install / system-update、`fetch`、remote `build`、local `build --local`、`upgrade`、`upgrade-aur`、`upgrade-all`だけを統一planとして観測する。nested `dry-run` commandやpacman自身の`--print`への委譲ではない。`deps`、`plan`、`-Ss`、`-Si`、`clean`、`-G` / `-Gp`、source-preference command、未裁定generic pacman pass-throughを含むその他のrouteでは明示的にnon-zeroで拒否する。

観測は各routeの既存production pre-mutation authorityを使い、root discovery、route projection、`BuildPlan`、provider selection、constraint evaluation、read-only local descriptor validation、AUR update preflight、required artifact、repository transaction intentをhuman-readableに表示する。read-only filesystem / network accessと、`pacman -Si` / `pacman -Qm`等のexact allowlist済みread-only discovery queryは実行し得る。statusは既存authorityから得た`Ready` / `NoOp`を終了code 0、`Blocked`をnon-zeroとし、renderer-local completenessから再計算しない。partial source failureやtyped blockerをreadyへflattenしない。

absolute no-mutation boundaryとして、dry-runはstate log / persistent stateのwriteやdirectory作成、cache、workspace、worktree、Git clone / fetch / checkout mutation、`makepkg --printsrcinfo`その他のlocal metadata生成・評価、build output、sudo、pacman transactionの開始・mutation、pacman transaction lock、install、cleanup mutationへ到達しない。local routeは安全な既存descriptorだけを使い、metadata評価が必要ならreadyを推測せず`Blocked`とする。

dry-run observationはapproval token、prepared execution capability、cached provider choiceではない。後続のactual invocationへ渡さず、actual routeはcurrent stateからproduction validationとprovider selectionを再実行する。v2.2.0ではhuman-readable outputだけを提供し、JSON / machine-readable schemaは追加しない。

exact target-less `moguet --dry-run -Syu`はrepository system-update intentとlater normal-AUR
transaction intentを別childとして表示する。AUR assessmentは現在インストールされている状態に
基づき、repository update後のfuture stateを保証しないこと、actual executionではrepository
upgrade成功後にAUR stateをfreshに再評価することを明示する。current AUR assessmentがNoUpdatesでも
repository intentを持つouter operationを`NoOp`へ落とさない。RepoOnlyの
`moguet --dry-run -Syu --repo`はrepository intentだけを表示し、AUR / preference authorityへ
到達しない。どちらもactual capabilityを作成・再利用しない。

Auto dry-runもactualと同じ`SkipIndependentTarget` policyを使う。independent devel
`RequiresCheck`だけならcurrent-state AUR childを`NoOp`、outer operationを`Ready`として
attentionを表示し、unrelated `UpdateAvailable`があればAUR child / outerとも`Ready`を維持する。
required dependency / provider / child relationへ再登場した`RequiresCheck`はAUR child / outerを
`Blocked`にする。RepoOnlyはtyped attentionを含むAUR authorityを一切保持しない。

diagnostic presentationはtyped stateからlocalizeする一方向のprojectionであり、localized / raw
stringからclassificationを逆算しない。English / Japaneseともnormal summary、
attention-required detail、route-owned necessary detailの順を保つ。operation outcomeとpackage
state observation、plan constructionとcompletenessとexecution readiness、severityとblockingと
exit-status effectはそれぞれ独立したdimensionである。successful-unverifiedはrequired actionを
伴うsuccessとして保持し、failureへ丸めない。`Unknown`も`NoOp`へ丸めない。

### Terminal-safe human-readable presentation

unified plan、runtime diagnostic、reviewed-source contentのterminal-facingなopaque textは、共通の
terminal-safe UTF-8 policyを使う。通常のvalid Unicodeは元のUTF-8のまま表示する一方、invalid
UTF-8、C0 / C1 / DEL、backslash、U+2028 / U+2029、Unicode bidi control、U+FEFFは、元byteを
uppercase `\xHH`として可視化する。U+FEFFを削除・正規化したり、invalid UTF-8をreplacement
characterへ置換したりしない。

これはhuman-readable presentationのsecurity hardeningであり、JSON等のmachine-readable schemaを
追加・変更するものではない。escape後のtextをpackage identity、path operation、comparison、
dependency / source判断、subprocess argument、exit statusのauthorityへ逆流させない。localizedな
program-owned textもcontrol-flow authorityにはしない。

<a id="compat-interactive-confirmation"></a>

## Interactive confirmation compatibility

Moguet-owned boolean confirmationは、`[Y/n]`をYes default、`[y/N]`をNo default、`[y/n]`をdefaultなしとして扱う。fixed ASCII / locale-neutral / case-insensitive tokenとして`y` / `yes`、`n` / `no`、`q` / `quit` / `cancel`だけを受理し、日本語response tokenは追加しない。q-familyは全boolean confirmationでformal cancellationであり、question固有のNoへflattenしない。invalid inputとdefaultなしのempty inputはwarning後に再promptする。

`--noconfirm`は宣言済みdefaultだけを利用し、`[y/n]`をapprovalへ変えない。non-TTY stdinは`[Y/n]`のYes defaultを選ばずUnavailableとなり、`[y/N]`だけがsafe Noへ進める。interactive clean EOFはCancelled / EndOfInput、actual stream read failureはInputFailureとして区別する。

Declinedはquestion固有のnegative answer、Cancelledはcurrent Moguet operationの停止であり、actual command / input / internal failureとも区別する。presentation classificationとprocess exit statusは別dimensionで、optional No、normal skip、inspection result等はroute contractに従ってexit 0となり得る一方、required operationを未完了にするDeclined / Cancelled / EOF / Unavailableとactual failureはnon-zeroとなる。cancellationはその時点以降を停止するだけで、既に完了したGit、editor、pacman、system等のphaseをrollbackしない。詳細は[interactive confirmation contract](contracts/interactive-confirmation.md)を正本とする。

<a id="compat-aur-update"></a>

## AUR update operation summary

`upgrade-aur`はinstalled foreign inventoryを起点にする。AUR RPCでexact packageとして解決でき、installed versionより新しいnormal AUR packageは従来どおりupdate candidateである。normal versionが新しくないexact AUR packageでも、PackageBaseまたはinstalled childに`-git`、`-svn`、`-hg`、`-bzr`、`-cvs`、`-darcs`のsuffix根拠があれば、v2.5.0ではsilentな`UpToDate`へ丸めず`RequiresCheck(SuffixCandidateOnly)`として保持する。suffixはcandidate evidenceだけであり、supported VCS、source metadata、tracking readiness、update有無を証明しない。AUR exact packageが存在しないsuffix付きforeign packageは`NonAurForeign`のままで、metadata / version comparison failureも既存failure semanticsを維持する。

normal AUR `UpdateAvailable`はdevel assessmentより優先し、suffix候補やauthoritative baseline不足を理由に取り消さない。`moguet -Qua`は`RequiresCheck`をpackage、PackageBase / child根拠、reason付きで表示し、installed versionからAUR Versionへのupdate arrowへ偽装しない。check-required自体はquery transport failureではないため、それだけならqueryの終了codeは0である。

`RequiresCheck`はautomatic update / rebuild candidateへ昇格しない。`upgrade-aur`はcurrent all-target contractに従い、1件でもあればoperation全体をAUR mutation前にblockする。dry-runは同じ状態を`Blocked`とnon-zeroへ投影し、`upgrade-all`はsystem、registered source、fresh foreign inventoryまでの完了済みphaseを保持したままfresh AUR phaseをblockする。non-TTYと`--noconfirm`はmanual rebuild promptやimplicit approvalを追加しない。query、recursive plan、provider selection、conflicts / replaces metadata、preparationを全targetについて確認してからexecutionへ進み、blocking targetが1件でもあればcache作成、git checkout、makepkg、`pacman -U`、sudoを開始しない。

v2.5.0のconservative connectionはupstream VCS revisionをquery / 比較せず、provenanceを保存しない。
現在のMoguetはIssue #476のinstalled provenanceとP/I/R revalidationを持ち、7-Bだけが#475を呼ぶ。
normal AUR updateとregistered AUR updateは7-Dからassessmentを使い、GitRevision candidateのexecutionは
reviewed pinから7-C→S4→S5→S6を通す。initial subsetはnon-split、one child/artifact、no overlay、one floating
HTTPS Git、default HEAD/exact branch、architecture-independent sourceに限定する。
full VCS trackingの主張ではない。public contract・identity invariants・schema v1の
no-auto-migration policyは[devel tracking contract](contracts/devel-tracking.md)を参照する。

official repository package、AURに存在しないforeign package、source preferenceだけで選ばれるpackageはautomatic AUR update対象にしない。

exact target-less `-Syu`のnormal AUR phaseも同じexact inventory / AUR query / plan / provider /
conflict / replaces / artifact / `RequiresCheck` safety authorityを使う。ordinary `-Syu`では
independent `RequiresCheck`を`Skipped(IndependentDevelRequiresCheck)`としてwarning / attentionへ
投影し、unrelatedなnormal updateをblockせず、他のhard failureがなければexit 0を許す。ただし
`RequiresCheck`を`UpToDate` / `NoChange`へ変換せず、package-state observationは`Unknown`のまま
保持する。dependency、provider、required artifact childとして必要なtargetは
`Skipped(RequiredDevelRequiresCheck)`とaffected rootのtyped blockerを保持し、current global
preparation barrierに従ってnon-zeroとなる。saved source preference policyは別authorityのIgnoreで
zero-I/Oとする。`upgrade-aur`と`upgrade-all`は`BlockOperation` + Strictのままであり、normal
`-Syu`からsource preference登録packageをsource-build routeへ自動routingしない。

interactiveなexact target-less Auto `-Syu`では、初回P観測が
`ProvenanceMissing` / `Stage::Provenance` / `before=Missing`の独立targetだけに、
明示tracking bootstrapを提示できる。exact AUR recipeのread-only trial観測で、single package・
architecture-independentなone floating HTTPS Git source・checkout overlayなしを確認する。
追加source fileのtracked regular-file identityを確認できない場合など、試行適格性が不明なら
提示せず従来のwarning/skipとする。この観測はS4/S5/S6 proofではない。

default-No確認は対象由来のcache/provider/dependency mutationより前に行う。Yesは開始の許可だけで、
既存review stateがあっても別途full source reviewを要求し、reviewed exact source、対応build、
actual install、fresh installed binding、S6 Completeをすべて要求する。RのMissing偽装やstate削除はしない。
`--noedit`は利用できるが、`--nodiff` / `review.diff=Skip` / `--noconfirm` / non-TTYでは提示しない。
declineはtyped RequiresCheck skip、cancel/EOFはtyped operation cancellation、後続は停止する。
accepted後のreview/build/install/publication failureも既存first-failure policyに従う。
既存valid provenance、通常Version update、required dependency/provider/child blockerは維持する。
invalid/corrupt/future/unsafe/binding mismatchのrepairは行わない。publication FailedとOutcomeUnknownを区別し、
install成功だけをtracking成功へ変換しない。詳細は[normal devel routes](contracts/devel-normal-routes.md)を参照する。

<a id="compat-git-remote-revision-observer"></a>

## Trusted Git remote revision observer foundation compatibility

Issue #475のobserverはproduction buildへ含まれるinternal read-only componentであり、7-B assessmentだけが呼ぶ。raw `ParsedSourceEntry` /
`ParsedSrcinfoSourceMetadata`、bare VCS identity、suffix classificationをnetwork authorityへ昇格せず、
Issue #476 Slice 7-B coordinatorがP/I/R local gates後に作るauthority-approved source capabilityだけをrequest前段として要求する。
normal CLI/AUR update routeは7-Dからこのassessmentへ接続する。observer自身はbuild/install/publicationを呼ばない。

current supported subsetはGit、HTTPS、default HEAD、exact branch、canonical lowercase SHA-1 40 hex / SHA-256
64 hexである。HTTP、SSH、file / local path、`git://`、ext、tag / annotated tag / peeling、fixed commit、
other VCSはunsupportedである。HTTPS remoteはcredential / userinfo、query、fragment、IPv6 zoneを拒否し、
raw Unicode IDNはunsupportedとする。ASCII punycode spelling、IPv6 literal、default `:443`除去、
non-default portはcurrent canonicalization contractに従う。

observerはfixed `/usr/bin/git`、HTTPS-only config / protocol profile、complete environment allowlist、
30 s hard timeout、500 ms termination grace、16 KiB stdout capture、strict full-transcript parserを使う。
`Observed`、`RefNotFound`、`Timeout`、`ProcessFailure`、`GitExitFailure`、`CaptureLimitExceeded`、
`MalformedOutput`、`AmbiguousOutput`を区別し、stderr textから`TransportFailure`を捏造しない。HOME / XDG /
cache / current repository / credential / cookie / traceのno-mutationはloopback HTTPS sentinelで検証する。

SHA-1 / SHA-256はOID bytesとactual fixture capabilityで判定し、Git version stringだけから推測しない。
current Arch hostとoffline/current Arch containerではactual SHA-256 HTTPS fixtureがPASSしているが、Git
2.55.0をproject minimumへ固定したものではない。observer foundationの存在はproduction source metadata、
installed provenance、remote comparison、`UpdateAvailable` / `UpToDate`、automatic rebuildを意味しない。
詳細は[trusted Git remote revision observer contract](contracts/git-remote-revision-observer.md)を正本とする。

`upgrade-all`はsystem upgrade、registered source package、remaining installed AUR packageを`system → registered source → fresh foreign inventory → filtered AUR`のphase順で扱う。single atomic transactionやautomatic rollbackではなく、先行phaseの成功、現在のfailure、後続phaseの`NotAttempted`を区別する。`upgrade-aur`と`upgrade-all`の`--rmdeps`、package target、`--needed`、`--aur`、`--repo`はunsupportedであり、queryやcache mutationより前に停止する。

対象がない場合は成功とするが、query failure、preparation failure、cleanup failure、未実行targetを空の成功結果へ丸めない。partial completionはnon-zeroである。source preferenceで選ばれたPackageBaseとautomatic AUR targetが重複する場合はduplicate exclusion / external satisfactionとして扱い、同じsourceを二重buildしない。

### `upgrade-all` system failure後のversion-lock診断

`moguet upgrade-all`のrepository system upgradeが失敗してsystem phaseで停止した後に限り、Moguetはlocal / sync databaseとexact AUR metadataをread-onlyで追加観測する。installed foreign packageのdirect exact runtime dependencyがinstalled repository package versionでは満たされ、観測したより新しいrepository candidate versionでは満たされない相関を1件以上表示できる場合だけ、repository / AURをまたぐpossible version-lock candidateをsupplemental diagnosticとして表示する。すべてのsystem failureへ表示するものではなく、publicに表示できるcoherentなcandidate correlationがなければ既存failure outputのままである。この非表示自体はcandidate absenceの証明ではない。

表示できる根拠は、observed repository candidateと同名のinstalled package / version、observed repository candidate / version、installed foreign package / version、そのinstalled direct exact dependency requirement、observed AUR replacement candidate、およびreplacementのdirect runtime requirementである。`foreign`は、installed packageのexact nameが現在設定されているrepository metadataに存在しないという観測に基づくもので、historical AUR provenanceを証明しない。

observed repository candidateはread-only repository metadataであり、pacmanが実際に選択したtransaction targetではない。possible candidateはconfirmed pacman blockerでもsystem failureの特定済み原因でもない。`CompatibleReplacement`も、observed AUR replacement candidateのdirect runtime requirementがobserved repository candidate versionで満たされるというmetadata correlationだけを表す。pacmanがそのversionを選択したこと、installed foreign packageがfailure原因だったこと、またはsafeなcoordinated updateが許可・証明されたことを意味しない。

replacement assessmentはcompatible、incompatible、matching candidate not found、compatibility unknown、AUR query failure、ambiguous evidenceを区別する。query failureをreplacement missingへ変換せず、candidate observationの`Partial` / `Failed`もabsenceへ丸めない。`Partial` observationからcandidateを表示する場合はsupplemental observationがincompleteであることを明示し、coherentなcandidateがなければsupplemental outputを追加しない。

このdiagnosticは元のpacman / sudo output、既存Moguet failure result、failure exit behaviorを置換しないため、candidateが表示されてもcommandはfailureのままである。Moguetが行うのはpossible candidateとversion / dependency constraintを示してmanual reviewを求めるところまでであり、repository / AURのautomatic coordinated update、automatic remove / reinstall、rollback、retry、partial upgrade、dependency bypassは行わない。

<a id="compat-aur-export"></a>

## AUR PKGBUILD export summary

`-G <pkg>`と`-Gp <pkg>`はexactly oneのAUR root PackageBaseだけを扱う。official repository probe、source preference、repository fallback、dependency plan、dependency repository、makepkg、pacman、sudo、editor、build / installは行わない。

`-G`は`--output-dir`未指定時、command開始時current directory直下の`./<PackageBase>`をdestinationとする。`--output-dir=DIR`は`-G`専用のoperation-local attached-value optionであり、指定した既存parent直下の`<PackageBase>`へexportする。relative valueはcommand開始時current directory基準で解決し、parentを自動作成せず、指定pathのfinal / intermediate symlink componentをfollowしない。Moguet独自のtilde expansionは行わないため、HOME配下には`--output-dir="$HOME/..."`またはabsolute pathを使う。space-separated value、empty value、duplicate、`-Gp`、他operationでは拒否する。

export parentはdirectory fdと`st_dev` / `st_ino` identityでanchorし、publish直前にnamed pathをnofollowで再openして同じfilesystem objectであることを確認する。rename、replacement、symlink replacement、identity driftはfail closedとし、replacement側へredirectしない。destination leafはvalidated PackageBaseのdirect childへ固定する。既存のdirectory、git repository、regular file、symlink、special fileがあればfail closedし、preflight後にdestinationが現れた場合も`renameat2(..., RENAME_NOREPLACE)`相当で置換しない。clone、PKGBUILD、`.git`、remote URL、containment、temporary checkout identityを検証してからpublishする。

`-Gp`はtemporary cloneからregular non-symlink PKGBUILD bytesだけをstdoutへ出し、通常成功・failureでpersistent checkoutやMoguet cacheを変更しない。`--output-dir=DIR`を受理せず、stdout lifecycleも変更しない。identity replacementを証明できないtemporary artifactは手動確認用に保持し得る。

<a id="compat-conflicts-replaces"></a>

## AUR conflicts / replaces summary

AUR RPC、`.SRCINFO`等から得た`Conflicts` / `Replaces`宣言は、dependency resolutionとは分離したtyped metadataとして保持する。Moguetはread-onlyなinstalled package databaseとplanned targetを観測し、package name、PackageBase、source / root attribution、version、provided componentを保ったままversion付きrelationをtransaction前に分類する。public diagnostic、build / install readiness、execution preflightはこのtyped assessmentを共通authorityとし、rendererやrouteごとにraw declarationを再parse・再判定しない。

classificationは、installed packageとのconfirmed conflict、planned targetとのconfirmed conflict、reviewが必要なpotential replacement impact、judgment不能な`Unknown`、invalid metadata / observation、complete observationによるconfirmed no matching current / planned targetを区別する。replacement matchはautomatic replacementの予告や許可ではない。`Unknown`とinvalid result、およびdeclarationはあるがassessment未完了のfallbackはfail closedとし、「一致対象なし」へ丸めない。completeな観測がpackageとprovided componentのいずれにも一致しないと確認した場合だけrelation guardを解除する。この結果もdeclaration自体が存在しないという意味ではない。

`-Si`はsource metadataとして`Conflicts` / `Replaces`を表示し、installed / planned stateを必要とする判定はplan / build preflightへ延期したことを明示する。`deps` / `plan`はtyped assessmentとreadinessをread-onlyに表示し、既存inspection commandのstatus contractへ新しいexit codeを追加しない。standalone `fetch`はfetch readinessが満たされる限りsource取得だけを行えるが、relation assessmentはBuild / Install readinessを許可しない。blocking conflict、potential replacement、`Unknown`、invalid、assessment未完了の各resultは、source install、local source route、AUR updateを含むexecutionをmakepkg、sudo、pacman install transactionより前に停止する。dry-run / unified planは同じblocking truthを`Blocked`とnon-zero statusへ投影する。complete no-matchだけならrelationを理由にblockせず、他のguardがなければ`Ready` / successを維持する。

Moguetが所有するのはmetadata observation、typed classification、pre-transaction diagnostic、safety stopまでである。automatic package removal、automatic replacement、automatic conflict resolution、replacement targetやproviderのimplicit selection、full dependency / conflict solverの置換、libalpm transaction prepare / commitは行わない。`pacman` / libalpmが最終transaction authorityであり、Moguetのpreflight successはtransaction successを意味しない。`--noconfirm`もrelation guardをbypassせず、自動削除・自動置換を許可しない。

<a id="compat-plan-size"></a>

## Planのofficial package size summary

`plan <pkg>...`で表示するofficial repository dependencyのpackage sizeはpresentation metadataであり、BuildPlanのgraph safety、AUR build unitのsize、dependency resolution、provider selection、transactionを変更しない。configured repository orderとread-only sync metadataをauthorityとし、package absence、query failure、malformed metadata、configuration failure、0 bytesを区別する。size metadataが取得できなくても、既存のplan本文を表示できる場合はgraph statusやexit codeを不必要に変えない。

dependency edgeはmetadata trust boundaryで構成したtyped requirement、installed / configured repository / AUR / local / providerのsource-aware candidate、`ConstraintEvaluation`を保持し、production downstreamでraw constraintを再parseしない。`deps`は`Satisfied` / `Unconstrained`を通常表示し、`Unsatisfied` / `Unknown`をresult / reason付きwarningとして継続する。`plan`は同じ2状態をincompleteとする。`Invalid` / `Conflicting`はread-only plan constructionでもfail-closedとする。`fetch`、build、install、upgrade、local buildは`Unsatisfied` / `Unknown`を含め、成功を証明できないconstraint resultをclone、fetch、source mutation、build、sudo、pacman、transaction開始前に拒否する。preflight successはtransaction successを意味しない。

<a id="compat-aur-status"></a>

## AUR status display summary

`-Ss`は軽いsearch / discovery表示として、AUR resultの状態tagを`[installed]`、`[out-of-date]`、`[orphaned]`の順に表示する。`-Si`はAUR metadataの`Maintainer`、`Installed`、`Orphaned`、`Out of Date`を表示する。repository packageの`-Si`はpacmanへ委譲し、AUR metadata表示と混ぜない。status表示はselectionやbuild executionを開始しない。

<a id="compat-split-package"></a>

## Remote source-build PackageBase summary

PackageBaseはclone / fetch / build repositoryの単位であり、package nameはinstall targetである。official repositoryでは、requested childとPackageBaseの対応をconfigured repository順のstrict libalpm exact snapshotから取得する。`Present`だけをrepository sourceとして採用し、confirmed `NotFound`だけをAUR fallbackへ渡す。query / config / metadata failureをabsenceへflattenせず停止し、requested name、filename、URL、artifact pathからPackageBaseを推測しない。

`deps` / `plan` / `fetch` / `-G` / `-Gp`はPackageBaseとrequested packageの違いを表示・取得のidentityとして保持するだけで、splitであることだけを理由にincomplete扱いしたり全artifactをinstallしたりしない。build / install routeはsource-build upper projectionが確定したrequired childとartifact metadata identityがexactly one一致する場合だけselected childを渡し、sibling / debug outputを暗黙にinstallしない。official repositoryのstandalone / registered routeではrequested `Explicit` childだけをinstallし、全unselected sibling / debugをresultへ保持する。詳細なselection、transaction、partial completionは[PackageBase contract](contracts/packagebase-child-selection.md)を参照する。

<a id="compat-contract-summary"></a>

## Production contract summary

各contract本文の日本語がnormative source of truthである。ここでは利用者がroute差分を判断するための要約だけを示す。

| Behavior / safety contract | User-visible compatibility summary | Normative contract |
| --- | --- | --- |
| common source-aware identity | package child、PackageBase、source、revision、release、architectureを別fieldで保持するinternal foundation。既存routeを置換せず、incomplete evidenceをcomplete identityへ推測しない | [source-aware package identity](contracts/source-package-identity.md) |
| reviewed AUR source state | AUR PackageBaseごとにexplicit accept済みexact revisionを保持し、previous reviewed revisionからexact targetまでをreviewする。skipではstateを進めず、accepted targetだけをpinned build authorityにする | [reviewed AUR source state](contracts/reviewed-source-state.md) |
| trusted Git remote revision observer | authority-approved sourceだけを受けるHTTPS Git read-only observer foundation。default HEAD / exact branchとstrict SHA-1 / SHA-256 resultに限定し、7-B coordinatorだけがproduction comparisonに使用 | [trusted Git remote revision observer](contracts/git-remote-revision-observer.md) |
| PackageBase / child selection | PackageBase単位でbuildするが、installするのはsource-build upper projectionが要求しmetadata identityで選択したchildだけ。sibling / debugは暗黙installしない | [PackageBase / required-child selection](contracts/packagebase-child-selection.md) |
| separated source-build `--rmdeps` | source-buildではownershipを証明できないためmutation前に拒否。pacman-onlyではMoguetが消費するが作用させず、pacmanへ転送しない | [source-build `--rmdeps`](contracts/source-build-rmdeps.md) |
| XDG cache cutover | trusted root、filesystem identity、symlink、root escape、legacy cache非変更を守る。implementation moduleは固定しない | [XDG cache safety](contracts/xdg-cache-safety.md) |
| source-build preference | `${XDG_CONFIG_HOME:-$HOME/.config}/moguet/source-build.d/`をreader / writer共通のauthorityとする。legacy storeへfallbackしない | [source-build preference XDG authority](contracts/source-build-preference-xdg.md) |
| interactive confirmation | `[Y/n]` / `[y/N]` / `[y/n]`、fixed yes / no / cancel token、non-TTY / `--noconfirm` gate、Declined / Cancelled / failure、route-owned exit、non-rollbackを統一する | [interactive confirmation](contracts/interactive-confirmation.md) |
| ambiguous provider | exact / unique provider以外は候補順で選ばず、interactive TTYの明示selectionだけを受け付ける。non-TTY / `--noconfirm`は停止 | [ambiguous provider selection](contracts/ambiguous-provider-selection.md) |
| root package selection | `-S --select`だけが対話root selection。`-Ss`は非対話search。候補が1件でもdefault選択せず、source identityを保持する | [root package selection](contracts/root-package-selection.md) |
| local PKGBUILD | `build --local <directory>`を明示入口とし、local treeをAUR rootへfallbackせず、metadata / source identity / artifactをfail closedで検証するproduction接続済みroute | [local PKGBUILD](contracts/local-pkgbuild.md) |

<a id="compat-reviewed-source-state"></a>

## Reviewed AUR source state compatibility

AUR Git source-buildでは、最後に利用者が明示acceptしたcomplete commit OIDをPackageBase単位で
XDG stateへ保持する。fetch / clone後にexact targetをpinし、reviewed continuationのreview、
acceptance、detached checkout、state publication、build continuationへ同じOIDを渡す。cache
checkoutのHEAD、branch、`origin/<branch>`等のmutable refをreview baselineやreviewed build
authorityとして再利用しない。

reviewed stateがない場合はempty treeからexact targetまでのinitial full review、valid baselineが
targetと異なる場合はprevious reviewed revisionからexact targetまでのupdate review、同じ場合は
prompt / rewriteなしのalready-reviewed continuationとなる。baseline objectが利用不能ならcache HEADへ
fallbackせずfull rebaseline reviewを行う。current-schemaのinvalid / corrupted / source-mismatched
stateはreason付きfull rebind reviewを要求し、unsupported future schema、unsafe history、store /
observation failureはoverwriteせずfail closedとする。

review inventoryはAUR Git treeのtracked file全体であり、added / modified / deleted / renamed /
type-changedを保持する。root `PKGBUILD`とtop-level `*.install`をreview-sensitiveとして強調するが、
patch、service unit、helper script、config / local source、binary / non-text change等をextensionで除外
しない。`.SRCINFO`はgenerated metadataとして表示に残すが、source-review authorityにはしない。

review表示とacceptanceは別eventである。defaultなしのinteractive promptへ明示入力した`y` /
`yes`だけがstate advancementを許可する。`--nodiff`、review decline、safe default No、
`--noconfirm`、non-TTYはreviewed authorityを作らず、compatibility buildを継続し得る場合もstateを
進めない。q-family、EOF、input failure、materialization / presentation failure、manual inspection
required、review-sensitive source unrenderable等のstop outcomeではbuildへ進まない。unsafe / future /
inconsistent stateをcompatibility routeで迂回しない。

compatibility-only buildはreviewed authorityを持たず、legacy checkout continuationをreview済みまたは
pinned reviewed buildとして表示しない。この経路からreviewed revision provenanceやstate publicationを
作らない。

state publicationはreview開始時のexact record identityとraw contentsをguardにするCAS semanticsを
持つ。並行processが別targetへ進めたstateをstale writerが上書きせず、same exact targetだけを
idempotent successとして扱う。acceptance後もexact checkout、editor boundary、既存preflightを
完了してからmakepkg前にpublishする。正常にpublishしたreviewed revisionは、後続build / install /
cleanup failureでrollbackしない。reviewed、built、installed outcomeは別々に表示する。

`--edit` / `--noedit`と`review.pkgbuild`はinvocation-localなPKGBUILD / detected top-level
`*.install` editor policyであり、upstream reviewed-source acceptanceではない。editor changeは
reviewed exact commit上のoverlayとしてcurrent buildにだけ作用し、persistent reviewed revision、
将来invocationのpatch、generic source identityへ昇格しない。

Issue #411より前から存在するAUR cacheにmanual migrationは不要である。reviewed stateが本当に
存在しない状態だけをnormalな`Missing`として扱い、最初に対象となるPackageBaseを一度full review
する。legacy checkout HEAD、branch、remote ref、artifactからreviewed revisionを捏造しない。
Invalid / Corrupted / SourceMismatch / UnsupportedFutureやunsafe stateをMissingへ丸めない。

official repository source-buildとlocal PKGBUILD routeはreviewed-source stateをread / writeしない。
official routeはconfigured repository / libalpm snapshot、local routeはuser-owned filesystem /
content provenanceをそれぞれ維持する。詳細は
[reviewed AUR source state contract](contracts/reviewed-source-state.md)を正本とする。

<a id="compat-common-source-identity"></a>

## Common source-aware identity compatibility

Issue #355のcommon identityは、後続profile / snapshot / patch workflow向けのinternal foundationであり、現時点のpublic CLIやproduction selection / build / install semanticsを変更しない。package child、PackageBase、repository / AUR / local source、source location、source revision、package release、architectureを別fieldで保持し、package名またはderived string keyへflattenしない。

Issue #355のgeneric current repository / AUR modelはexact source commitを保持しないためrevisionは`Unknown`であり、known commitとして推測しない。Issue #411のreviewed-source lifecycleはAUR review / build用のexact OIDを別のpersistent / capability authorityとして保持するが、そのOIDをgeneric `source_package_identity_projection`へ注入して`Known`へ昇格させない。generic source identity projectionとreviewed-source persistent / build authorityは同じものではない。current local routeはGit repositoryをauthorityにせずfilesystem / content provenanceを使うためrevisionは`Inapplicable`である。`Unknown`、`Absent`、`Unavailable`、`Inapplicable`をknown matchやabsenceへ丸めない。

repository root candidateはPackageBaseを保持しない。actual artifactはarchive内のPackageBase / architectureをread-only libalpm metadataとして保持するが、source / revisionは保持しない。internal read-only adapterはactual child / version / PackageBase / architectureと既存typed contextをexactに相関できる場合だけcomplete common identityを返し、`Missing` / `Malformed` / `Unavailable`なactual metadataをupper contextで補完しない。filename、package name、URL leaf、canonical source keyから不足fieldを逆算しない。PackageBaseを持たないrepository root、source contextを持たないartifact、installed-only dependency等はtyped failureであり、partial identityを公開しない。

internal compatibility evaluatorはsource、PackageBase、child、revision、release、architectureをdimension別に判定し、`ExactMatch`、`SamePackageChild`、`SamePackageBase`、`Incompatible`、`Indeterminate`を区別する。structurally equalな`Unknown` / `Absent` / `Unavailable` / `Inapplicable`も`ExactMatch`へ昇格しない。

read-only projectionの一部はIssue #485のinternal production pathに限定接続されている。`project_dependency_source_package_identity()`は`SourceArtifactInstall`のtrusted bindingとinvocation-owned cleanup correlation / evidenceに、`project_artifact_source_package_identity()`はtrusted bindingのartifact整合とreceipt evidenceに利用される。このprojectionは既存のtrusted ownerへtyped identity / correlation evidenceを渡すだけであり、source-build routing、artifact identity、`SourceArtifactInstall` trusted transport、invocation-owned cleanupのauthorityをcommon modelへ移さず、既存routeを置換しない。

generic compatibility evaluatorは引き続きpublic production workflow / routing decisionへ未接続である。public profile workflow、patch / revision authority、generic compatibility-driven routing、v3 source-build / profile architectureはcurrent featureではない。詳細なstate、equality、compatibility、projection contractは[source-aware package identity contract](contracts/source-package-identity.md)を正本とする。

<a id="compat-packagebase-child-selection"></a>

## PackageBase / required-child compatibility

PackageBaseはrepository / build / workspace / package transactionの単位、required childはinstall-selectionの単位である。1 PackageBaseを1 fresh workspaceで1回buildし、`makepkg --packagelist`のexpected aggregateとbuild後package metadata identityを照合する。required childがexactly one選択できない場合、filename、先頭artifact、PackageBase名、`--noconfirm`で補わずfail closedする。

remote source-buildのcurrent route matrixは次である。

| Route | PackageBase lifecycle / preparation |
| --- | --- |
| standalone repository | `PackageBaseSet` |
| registered repository | `OnlyIfUpdated` preparation後の`PackageBaseSet` |
| sync repository | `SingularCompatibility` + existing `--needed` |
| registered AUR | `SingularCompatibility` + existing provider / split guard |

official repositoryのrequested child / PackageBase authorityはstrict libalpm exact snapshotである。confirmed `NotFound`だけがAUR fallbackを許し、metadata query / config / malformed identityは停止する。standalone / registered repository routeはsingle / multiple outputに関係なくrequested `Explicit` childだけをinstallし、sibling / debug artifactはunselectedかつnot installedとして保持する。sync / registered AUR routeの既存capabilityを、このSet対応から推測して拡張しない。

selected childだけがinstall input、install reason、installed / skipped-as-needed outcome、target attributionを持つ。unselected sibling / debug artifactはresult dataとして保持する。transaction failureでchild successを推測せず、cleanup failureはcompleted childを保持するpartial successとして扱う。詳細は[contract](contracts/packagebase-child-selection.md)を参照する。

<a id="compat-rmdeps"></a>

## `--rmdeps` compatibility

`--rmdeps`はpacman optionではなく、makepkg由来のMoguet global optionである。separated source-buildでは、今回のinvocationが導入したdependency集合をMoguetがauthoritativeに所有できないため、意味のあるcleanup要求をsilent ignoreせず、mutation前にfail closedする。current build-only commandは概ね`makepkg -sc`であり、`-s`によるdependency installが発生し得る。pre/post installed package差分だけではmakepkg内部または並行するtransaction、invocation外のinstall / reason変更を安全に区別できず、新しく観測された`NewlyObserved` packageを`InvocationOwned`へ昇格できない。

source-build routeでは`makepkg -r`、`pacman -Rns`、`pacman -Qdt`、独自orphan cleanup、automatic rollbackへ変換しない。`--noconfirm`でも拒否を突破しない。

current internal treeには、validated source artifact bytesをwrite-sealed snapshotからroot-owned stagingへ移し、
actual `pacman -U`のInstall-only receiptを取得する`SourceArtifactInstall`専用transportがある。Issue #485 Slice 5は、
trusted executor-issued selected-provider evidence、non-reconstructible invocation session、owner/work-item別token inventory、
全BuildPlan edgeのexhaustive classification、nonempty completeness、exact current PackageBase / architecture、
phase-bound baseline/current/policyを、remote AUR専用の単一closed production collector内部でfinal candidate assessmentまで
一方向に接続した。candidate originはowner-specific actual selected `Install`だけであり、solver-introduced package、
snapshot差分、orphan state、makepkg syncdepsはcandidate化しない。authoritative installed fixtureはcompleteな1 candidateだけが
`Eligible`へ到達することと、各authorityを崩したnegativeがnon-Eligibleであることを確認する。

このinternal completionはpublic `--rmdeps` supportではない。assessmentをpreview、prompt、confirmation、removeへ
公開せず、public source-buildは引き続きexternal mutation前に`--rmdeps`を拒否する。makepkg syncdeps authorityは
Issue #484 / #501、mutation直前revalidationとremovalは#486の独立scopeであり、Issue #485だけでcleanup executionをGOにしない。

pacman-only routeでは、Moguetがmakepkg dependency installation lifecycleを実行しない。そのためcleanup対象となるinvocation-owned dependency集合自体が発生せず、Moguetはoptionを消費するが作用させず、pacmanへ転送しない。このno-opはsource-build routeで意味のあるcleanupを黙って無視することとは異なる。pacman-onlyでは安全に作用させるcleanup lifecycleが存在しないからである。decision 1の「黙って無視せず、意味を安全に維持できない場合は停止する」とも矛盾しない。

| Route | `--rmdeps` contract |
| --- | --- |
| 明示的なAUR / source-build install、`build` | source resolutionより前、またはroute probe後でcheckout mutation、workspace、makepkg、metadata query、pacman / sudoより前に拒否 |
| local `build --local` | local root inspectionより前にoperation-local parserで拒否 |
| singular / PackageBase separated lifecycle | workspace / process / metadata / transactionより前に拒否。既存preflight orderを維持 |
| `upgrade-aur` | update query、default log / cache初期化より前に拒否。target 0件でもno-op成功へ変換しない |
| `upgrade-all` | log / cache、source preparation、system upgrade、foreign inventory、AUR queryより前に拒否 |
| registered `upgrade`にvalid source targetがある | source preparation、source mutation、system mutationより前に拒否 |
| registered `upgrade`にsource targetがなくpacman-onlyへ縮退 | Moguetが消費するが作用させず、system `pacman -Syu`へ転送しない |
| その他のpacman-only route | Moguetが消費するが作用させず、pacmanへ転送しない |

`--rmdeps`のauthority、source-build fail-closed、pacman-only no-opの理由は[専用contract](contracts/source-build-rmdeps.md)を正本とする。

<a id="compat-xdg-cache-safety"></a>

## XDG cache compatibility

cacheのdestructive operationはtrusted root内へ限定し、symlink / root escapeをfollowせず、identity replacement、ownership不明、preflight不足をfail closedとする。cache cleanupは全targetのpreflight前に開始しない。legacy cacheを自動read / migrate / modify / deleteしない。Git executionも親processの危険なroutingやconfig environmentを暗黙継承しない。

このsectionはuser-visibleな安全要約であり、filesystem identity、rollback、implementation proportionalityの正本は[XDG cache safety contract](contracts/xdg-cache-safety.md)である。

v2.8.0では、trusted cache recursive cleanupの作業FDをentry総数ではなくtree depthに応じた量へ制限する。
preflightからconsumeまでのfilesystem object generationを安全に証明できないtargetは、filesystem / kernelの
capability不足も含めて拒否する。従来の小さいcacheでの成功を、当該runtimeでの継続的なcleanup対応とは扱わない。
全targetの事前検証とpacman / confirmationの順序は維持する。cooperative leaseは対象subtreeの検証・削除中に
保持し、全targetをcommand終了まで予約しない。途中で対象がbusyになった場合もskipせずincomplete / non-zeroで停止し、
既に削除したcacheは復元しない。

<a id="compat-source-preference-xdg"></a>

## Source-build preference authority compatibility

canonical rootは次である。

```text
${XDG_CONFIG_HOME:-$HOME/.config}/moguet/source-build.d/<package-name>
```

unset / emptyの`XDG_CONFIG_HOME`は`$HOME/.config`へfallbackし、明示値はabsoluteかつ安全で既存base directoryでなければfail closedとする。root実行時もroot自身のXDG contextを使い、`SUDO_USER`から別userを推測しない。add / editだけが必要なdirectoryをsafe creation boundary経由で作成し、read / list / build / upgrade / missing delete / revertはdirectoryを作成しない。

explicit `upgrade` / `upgrade-aur` / `upgrade-all`とsource-aware buildはこのauthorityをStrictに
扱う。exact target-less `-Syu`のactual / dry-runはnormal AUR routeのIgnore policyを使い、
directory snapshot、strict read、PackageBase fallback read、preference-derived environment適用を
すべて行わない。preference read failureをconsumeしてmissingへ変換することもしない。

`/etc/jpacker`と`/etc/moguet`をruntimeで作成・参照せず、legacy storeへのfallback、merge、自動copy / rewrite / deleteを行わない。source preference filesystem操作はsudoを使わず、revert後のpacman transactionのsudoとは分離する。詳細は[source-build preference contract](contracts/source-build-preference-xdg.md)を参照する。

<a id="compat-ambiguous-provider"></a>

## Dependency provider compatibility

official exact、AUR exact、unique providerを先に扱い、複数providerはambiguousとして扱う。候補identityはsource kind、package、repositoryまたはPackageBase、provided dependency、available constraint metadataを保持する。interactive TTYの番号選択以外ではdefaultを設けない。

non-TTY、`--noconfirm`、cancel、EOFではpromptや自動選択を開始しない。choiceはinvocation-localであり、config / cacheへ保存しない。selected repository providerはexact `repository/package`のofficial dependency、selected AUR providerはPackageBase build unitとして扱う。selectionとstatic preflight前にclone、build、pacman、sudoを開始しない。詳細は[ambiguous provider contract](contracts/ambiguous-provider-selection.md)を参照する。

constraint resultはcandidateのfilter、sort、番号、default、recommend、auto-selection、choice reuseを変更しない。`Unsatisfied` / `Unknown`はprompt上のpresentation-only warningであり、`Invalid` / `Conflicting`だけをprompt前にfail-closedとする。constraintによるrepository / AUR / local source fallbackは行わない。provider metadata refresh後はcurrent matching capabilityで再評価し、古いprovided version / resultを再利用しない。

interactive candidate listには、read-only local package databaseにcandidateの`package_name`と同名packageがある場合だけlocalizedな`[installed]`を末尾へ付ける。authoritativeなabsenceはsuffixなし、configuration / local DB / query / malformed metadata failureはlocalizedな`[installed state unknown]`と別warningで表示する。これはname-only observationであり、source provenance、PackageBase、version / constraint、install reasonを証明しない。state表示はcandidate identity、順序、番号、選択、choice reuse、BuildPlan、routingを変更せず、non-TTY、`--noconfirm`、candidate数1以下、reuse、cancelled dependencyではlookupを開始しない。

<a id="compat-root-package-selection"></a>

## Root package selection compatibility

正式入口は`moguet -S --select [--needed] <query>`であり、`-Ss`は非対話search / presentationのままである。repository / AUR candidateはsource identityを保持し、同名packageでもsourceが違えば別候補とする。official searchはread-only libalpm metadata、AUR searchはtyped AUR responseをauthorityとし、pacmanのhuman-readable search outputをparseしない。

interactive stdinで番号、複数番号、inclusive range、表示済みofficial groupの`@group` selectorを扱う。empty、cancel、EOF、non-TTY、`--noconfirm`はnon-zeroで停止し、invalid lineはatomically retryする。selection、identity validation、全static preflightが終わるまでpacman、sudo、clone、build、install、cache / workspace mutationを開始しない。selected repository rootとAUR rootは明示routeへprojectし、package nameからsourceを再推定しない。詳細は[root package selection contract](contracts/root-package-selection.md)を参照する。

<a id="compat-local-pkgbuild"></a>

## Local PKGBUILD compatibility（production接続済み）

正式入口は`moguet build --local <directory> [V=K...]`であり、`build <pkg>`はremote package routeとして維持する。local PKGBUILD routeはproduction CLIへ接続済みである。local directory、root `PKGBUILD`、`.SRCINFO`のfilesystem identity、owner、mode、containmentをdescriptor-firstで検証し、unsafe stateはfail closedとする。local rootをAUR RPCへqueryせず、metadata failureをAUR absenceやempty dependencyへfallbackしない。

safe `.SRCINFO`をread-only authorityの第一候補とし、missing / invalid / known-staleとPKGBUILD evaluationを区別する。`--noedit`はevaluation consentではなく、`--noconfirm`、non-TTY、cancel、EOFはevaluationを自動承認しない。local source treeをreset、clean、overwrite、deleteせず、local rootはExplicit、dependency artifactsはDependencyとして扱い、existing Explicitを降格しない。artifactはPackageBase / required-child contractへ接続する。Issue #271 Slice 2〜5でmetadata、dependency plan、source workspace、artifact / install、public surfaceを揃え、production CLIへ接続済みである。詳細なfilesystem、execution、cleanup contractは[local PKGBUILD contract](contracts/local-pkgbuild.md)を参照する。

<a id="compat-source-selection"></a>

## Package source selection policy

source selectionは排他的な3状態である。

- Auto: selector未指定時のdefault。既存の分類を維持する。
- AurOnly (`--aur`): root targetをAURへ限定し、officialへfallbackしない。
- RepoOnly (`--repo`): targetをofficial binary repositoryへ限定し、AUR / source-buildへfallbackしない。

exact target-less `-Syu`では、この一般selector taxonomyを専用semantic routeへ投影する。Autoは
repository + normal AURかつsaved preference Ignore、RepoOnlyはrepository-onlyかつfull compatible
pacman pass-throughである。AurOnlyはinvalidであり、`-Syu --aur`をAUR-only aliasとして追加しない。

`--aur`と`--repo`の同時指定はconflictとして、pacman、sudo、AUR RPC、git、makepkg、cache mutationより前に停止する。scope外のoperationでselectorを認識した場合も黙って無視しない。selectorはpacman option value待ち、`--`後のopaque operand、`--` markerより優先されず、通常位置のtokenだけを消費する。

## Package metadata compatibility probes

repository packageの分類はtyped exact metadataを使用し、confirmed `NotFound`だけをAUR fallback条件とする。
Auto `-Si`はmetadata failure対象をdiagnostic付きで失敗扱いにし、AURへ照会せず、後段pacman operandからも除外する。
独立targetの表示とqualified target、option value、operand順序、refresh barrierは維持する。

`revert`は対象ごとにrepository metadataを確認してからsource preferenceを削除する。metadata failure対象は
preferenceを保持し、binary reinstallへ含めず、最終non-zeroにする。独立成功targetの継続、grouped reinstall、
preference削除failure時の継続、pacman failureの診断優先順位を維持し、rollbackは追加しない。

AUR infoのinstalled表示はtyped local metadataを使い、成功時のyes / noを維持する。取得不能時は
`unavailable`とwarningを表示する。Auto searchの`[installed]`は正常なforeign inventoryへの所属だけで付け、
正常0件では省略する。inventory failureではpartial resultを採用せず、取得不能のwarningとともにannotationを省略する。
これらはoptional presentationであり、そのfailureだけではsearch / info本体の成功規則を変更しない。
AurOnly searchはこのmetadata queryを開始しない。表示用の継続policyをplanning / executionのabsence判定へ流用しない。

## pacman由来 operationのpass-through

MoguetがAUR / source-buildへ介入しない場合、次のoperationは基本的にpacmanへ委譲する。

- `-S`系（sourceへ分岐しない場合）
- `-R`系
- `-Q`系
- `-U`系
- `-D`系
- `-F`系
- `-T`系

`-Sc`は`sudo pacman -Sc`へ委譲し、Moguet build/cacheは削除しない。Moguetのbuild/cacheをcleanしたい場合は`clean`を使う。exact target-less `-Syu`だけは上記combined semantic routeまたは明示RepoOnly routeとしてinterceptする。`-Sy`、`-Su`、alternate / separated modifier、target-bearing form等はcurrent delegated routingを維持する。`-Syu`からregistered source preferenceを走査・適用せず、source-awareな全体走査は明示的な`upgrade*` commandへ分離する。

read-only queryのpacman標準出力・標準エラーはできるだけ保ち、Moguetが主要なexternal commandを実行する場合はcommandを実行前に表示する。pacmanのtransaction ownerはpacman、source artifact build ownerはmakepkg、source repository retrieval ownerはgitである。

<a id="compat-pacman-options"></a>

## pacman / makepkg由来 option

pacmanへ直接委譲する経路では、Moguetが明示的に消費しないpacman-compatible optionをpacmanへ渡す。AUR / source-build経路では、pacman optionをそのままmakepkg optionとはみなさない。

### Moguet固有 option

- `--edit` / `--noedit`: PKGBUILD / detected top-level `.install`のinvocation-local editor prompt policyを選ぶ。upstream reviewed-source acceptanceではない。
- `--diff`: previous reviewed revisionからexact targetまでのreviewed source change、またはinitial / rebaseline full reviewのprompt policyを選ぶ。state advancementには別のexplicit acceptanceが必要である。
- `--nodiff`: reviewed source changeのreview / acceptance導線を省略し、reviewed stateを進めない。
- `--rebuild`: build-only makepkgの`-f`へ変換する。
- `--cleanbuild`: build-only makepkgの`-C`へ変換する。
- `--rmdeps`: source-buildでは下記contractに従い拒否し、pacman-onlyでは消費する。
- `--aur` / `--repo`: Moguetのsource selectorであり、pacman / makepkgへ渡さない。
- `--output-dir=DIR`: `-G`だけが消費するoperation-local export parentであり、configやpacman / makepkgへ渡さない。

### `--noconfirm`

pacman-onlyではpacmanへ、separated source-buildではbuild-only makepkgとtyped `sudo pacman -U`へ渡す。ただし、未解決依存、循環依存、ambiguous provider、root candidate、unknown / duplicate artifact identity、mixed install reason、conflicts / replaces、未指定のrebuild / cleanbuild、危険な削除 / reset、PKGBUILD evaluationを自動承認しない。

### `--needed`

pacman-onlyではordered pacman argvへ保持する。対応済み`-S`でsource routeが生じる場合は、build skipではなく検証済みartifactへ渡すtyped `pacman -U`のinstall-only policyとして1回だけ扱い、makepkgへ渡さない。selection、metadata、provider、artifact、safety guardは省略しない。

Auto combined `-Syu`ではinitially唯一対応するpacman semantic optionであり、repository phaseの
exact ordered argvへだけ保持する。later normal-AUR transactionへ`needed` policyを伝播しない。
RepoOnly `-Syu --repo`ではcompatible pacman pass-throughの一部としてrepository phaseへ保持する。

### AUR / source-buildで単純pass-throughできないoption

`--asdeps`、`--asexplicit`、`--ignore`、`--ignoregroup`、`--overwrite`、`--config`、`--dbpath`、`--root`、`--sysroot`、`--cachedir`、`--gpgdir`、`--hookdir`、`--logfile`、`--print-format`、`--nodeps` / `--assume-installed`、`--dbonly` / `--noscriptlet`、`--downloadonly`、`--print`は、source-buildで同じ意味を保てないため、対応範囲外ではmutation前に停止する。pacmanが見ているworldとMoguetのmetadata / installed state / cacheがずれるoptionを黙って変換しない。

### 値を取るoption

`--arch`、`--assume-installed`、`--cachedir`、`--color`、`--config`、`--dbpath`、`--gpgdir`、`--hookdir`、`--ignore`、`--ignoregroup`、`--logfile`、`--overwrite`、`--print-format`、`--root`、`--sysroot`、および`-b`、`-r`はvalueとの組として扱う。option value待ちのtokenや`--`後のopaque operandをMoguet global optionとして再解釈しない。

## Exit code、partial completion、failure

- pacmanへ直接委譲したcommandはpacmanの終了codeを返す。
- AURの単一package infoはquery failureと正常応答による不存在を区別する。transport / configuration / HTTP / 空応答 / parse / RPC errorはcallerへ例外として伝え、`-Si`のAurOnly / Autoやsource buildでpackage not foundへ変換しない。不存在とfailureはいずれも既存のnon-zero終了を維持する。`info_strict`の追加envelope検証とsearchのmatch policyは変更しない。
- exact target-less Auto `-Syu`はrepository + normal AUR aggregateのtyped resultを使い、complete successは0とする。independent devel `RequiresCheck`だけはwarning / attention付きsuccessを許すが、repository failure、required `RequiresCheck`を含むrepository完了後のAUR blocker / failure、inconsistent resultはnon-zeroとする。
- repository failureではAURをnot attemptedとして示す。repository完了後のAUR failureではrepository completionを隠さずpartial completionとして示し、rollbackを行わない。
- AUR install resultとcleanup failureをflattenせず、AUR `NoUpdates`だけをwhole-operation `NoOp`へ昇格しない。
- integrated searchはofficialまたはAURのmatchを表示できた場合に成功とするが、query failureをempty resultへflattenしない。
- `plan` / inspectionのwarningと、plan作成自体のfailureを区別する。
- build / install / cleanup、package transaction、source phaseのfailureはpartial completionとunattempted targetを保持し、successへ丸めない。
- 先行phaseの成功をautomatic rollbackや後続phaseの成功と解釈しない。

## Out of scope

この方針はpacman完全互換、provider choiceの永続化、arbitrary multiple-outputの全自動install、debug package default install、conflicts / replacesの自動解決、dependency solver強化、pacman database write、package verificationの独自再実装を宣言しない。詳細なproduction safety contractは[`docs/contracts/`](contracts/README.md)と[`DECISIONS.md`](DECISIONS.md)へ分離している。

## Authoritative devel update routes (#476 Slice 7-D)

normal AUR RPC version-newerを優先し、same/olderだけを7-Bのtip-only P/I/R + remote assessmentで補完する。
GitRevision updateはpackage version増加と別basisであり、-Qua/unified planは架空のversion arrowを作らない。
ordinary -Syuのindependent RequiresCheck skip、required re-entry block、strict AUR routeのnonzeroを維持する。
Unknownはremote observation failureとしてbuildせず、RequiresCheckとは別reasonを保持する。
registered AUR OnlyIfUpdatedは共通coreをversion-only shortcutより先に使い、RequiresCheckはdefault-Noの明示rebuild確認を要求する。
dry-runは同じcurrent read-only producerを使い、source/build/transaction/publicationを実行しない。
actual prior-phase後のfuture stateと同一とは主張しない。explicit reviewed buildだけが7-Cからbaselineをbootstrapできる。
詳細は[normal devel routes](contracts/devel-normal-routes.md)、identity・schema/migration・compatibility matrixは
[devel tracking](contracts/devel-tracking.md)を正とする。この対応範囲と安全境界を現行契約とする。
