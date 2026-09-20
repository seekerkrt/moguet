# Source-build の `--rmdeps` contract

## 文書の位置づけ

この文書はIssue #486のcurrent public contractを所有する。日本語本文をnormative source of truthとする。
後半のIssue #404 / #485記録は歴史資料であり、そこでのpublic unsupported、strict causal proof必須、
makepkg syncdeps NO-GO、nonempty candidate必須という判断は、以下のcurrent contractに置き換わる。
installed helper / transport自身のtransaction outcome、one-shot、retry禁止等の責務は維持する。

## Current public contract — Issue #486 Slice 5

canonical optionはMoguet-owned global `--rmdeps`だけであり、aliasを追加せずpacmanへ転送しない。
positive supportは、repository exact queryで不在を確認してAURへ解決したremote `build <package> --rmdeps`だけ。
`build --local`、repository source build、`-S --aur`、upgrade系、system/source transitionへ広げない。

### Requestとlifecycle

`build_source_target`が要求とoperation resultを所有する。`--rmdeps`未指定なら既存build/installだけを実行し、
cleanup baseline/session、candidate収集、preview、question、fresh revalidation、HoldPkg query、removal、
cleanup status表示を起動しない。

指定時は次の順序を守る。

1. remote source identityと既存option / plan / preparationを検証。
2. collector内でsessionとpre-state baselineを最初のdependency mutationより前に確立。
3. 通常のdependency install、makepkg、artifact検証、root artifact installを実行。
4. current invocation resultの全work item成功を要求し、その後だけpost-state観測とcandidate収集。
5. existing collectorのComplete / eligible集合からpreviewを構築し、explicit interaction。
6. Approvedだけを既存executorへ渡し、fresh revalidationのReady subsetをexact one-shot removal。
7. build/install、interaction、executionを別のtyped resultとして保持し、cleanup outcomeを別に報告。

下位separated build/install ownerは`--rmdeps`を消費しない。public ownerが要求を保持したまま、
そのfieldだけfalseにしたconfigを下位へ渡し、他optionや他routeのrejectを維持する。
build、dependency install、artifact production / install失敗や途中終了ではpreview / question / removalを行わない。
既存workspace cleanup失敗は既存の独立したstaged outcome / typed exceptionを維持し、full invocation successにはしない。
失敗したbuildの部分dependency cleanup、repair、rollbackは行わない。
`--dry-run`はread-only preparation / plan表示までで、cleanup session / interaction / mutationを実行しない。

### Candidate authority

基本はpre absent、post installed、current Dependency reasonに加え、existing BuildPlan role / edge、
PackageBase、actual name / version / architecture、selected provider、policy / shared lifetimeのexact correlation。
pre-existing、Explicit、Unknown、identity mismatch、root / runtime target、protected、still-requiredを除外する。
strict makepkg call-origin / complete causal token proof / hostile same-UID対策をcleanup eligibilityの必須条件にしない。
既存receiptが持つfactual outcomeは保持するが、その欠落だけで通常の候補を全面的にUnknownへ落とさない。

Completeでcandidate 0はNoCandidatesという正常結果であり、prompt / revalidation / removalは0。
Incomplete / unsafeなcandidate universeはBlocked。public callerがsafe subsetを再分類してapprovalへ進めない。
current Absentやauthoritative PreExistingで0になる通常ケースをIncompleteへ戻さない。

### Interactionとremoval

candidateのexact name / versionを先に表示し、`Remove build dependencies? [y/N]`を1回だけ提示する。
Enter / n / noはDeclined、q / quit / cancel / EOFはCancelledで、mutationは0。build/install成功を保持する。
user `--noconfirm`はcleanup approvalではなくInteractionUnavailable(NoConfirm)、non-TTYは
InteractionUnavailable(NonInteractiveInput)。通常build/installは実行できるが、cleanup approvalを推測しない。

explicit Yes後に必ずfresh configuration、installed identity / reason、base-devel policy、effective HoldPkg、
current runtime consumersを再観測し、shared dependency closureを再縮小する。
AlreadyAbsent / IdentityChanged / InstallReasonChanged / Protected / StillRequired / Unknown / Invalidはskip。
HoldPkg matchもProtectedとして報告し、base-develだけが理由であるかのように説明しない。
Readyが0ならNoCandidatesReady、global query failure等はBlocked。

残るexact setだけを1 transactionの`sudo pacman -R --noconfirm -- <names...>`へ渡す。
このinternal `--noconfirm`はexplicit Moguet Yesとfresh checks後のpacman重複prompt抑止だけであり、
user `--noconfirm`をYesへ変換するものではない。
`-Rs` / `-Rns` / cascade / nodeps、`pacman -Qdt*`、broad orphan sweep、autoremove allを追加しない。

### Result / reporting / exit status

`RemoteSourceBuildResult`は`ProductionSourceBuildInvocationResult`とoptional interaction / executionを保持する。
未指定はinteractionなし（NotRequested）で無表示。build failureもcleanupを開始しない。
previewは削除予定候補でありremoved setではない。execution後にremoved setまたはskipped set / reasonを報告する。
RemovalFailedはattempted exact setと既知のexit statusを保持・表示し、実際に消えたpackageを推測しない。
retry / rollbackを行わず、build/install successをcleanup failureで書き換えない。

| build/install | cleanup | command exit |
| --- | --- | --- |
| 既存failure / partial | 未開始 | 既存non-zero |
| Success | NotRequested / NoCandidates / Declined / Cancelled | 0 |
| Success | Removed / NoCandidatesReady（fresh unsafe候補のskip） | 0 |
| Success | Blocked / InteractionUnavailable / RemovalFailed | 1 |

既存のpartial completionをsuccessへ丸めないCLI慣例に従い、要求されたcleanupを実行できない場合はnon-zero。
No / cancelという利用者の判断と、candidateがなくなった正常skipはerrorにしない。

### Route matrix

| Route | `--rmdeps` |
| --- | --- |
| remote `build <AUR package>` | 上記の明示cleanupをsupport |
| repository `build <package>` | exact source解決後、build/install mutation前にreject |
| `build --local` | local root inspection前のrejectを維持 |
| `-S --aur` / source sync / separated lower lifecycle | 既存rejectを維持 |
| `upgrade-aur` / `upgrade-all` | query / mutation前のrejectを維持 |
| registered `upgrade`にsource targetあり | source / system mutation前のrejectを維持 |
| registered `upgrade`にsource targetなし、その他pacman-only compatible route | Moguetがconsumeしno-op、pacmanへforwardしない |

## Historical implementation record — #404 / #485

以下は各Slice当時の記録。current cleanup permission / public supportには上記#486を適用する。
transport自身の独立した安全契約の記録を、public cleanupの再設計に合わせて削除しない。

### Current lifecycle監査（2026-08-27）

current separated source-buildの「build-only」は、source artifactのinstallをmakepkgへ委譲せず、検証済みartifactを後段のtyped `pacman -U` transactionへ渡すという意味である。package database mutationが一切ないという意味ではない。current makepkg build commandは概ね次であり、`-s`によってmakepkg内部からdependency installationが発生し得る。

```text
makepkg -sc
```

`--noconfirm`、rebuild、clean buildのcurrent optionはこのexact baselineへ追加されるが、`-r`は追加されない。Moguetはfresh `PKGDEST`、artifact validation、後段のartifact installを所有する一方、`makepkg -s`内部のdependency transactionをpackage単位のcausal resultとして受け取っていない。

current metadata境界には次の能力と不足がある。

- 個別installed package queryはownedなname、version、`Explicit` / `Dependency` / `Unknown` install reasonを返す。
- Slice 3のfull local package state snapshotは、1つの`PackageMetadataSession`がpreloadしたlocal DB cacheを1回だけ走査し、全entryのownedなname、version、`Explicit` / `Dependency` / `Unknown` install reasonを同じread phaseとして保持する。invalid / empty name、empty version、duplicate name、invalid cache entryはsnapshot failureであり、skipやlast-write-winsにしない。
- full snapshot成功時にkeyが存在しない場合だけ、そのread phaseでのconfirmed absenceである。session open、local DB、cache、query、metadata validationのfailureはtyped failureであり、empty inventoryやabsenceへ変換しない。
- production source-build invocationは、このsnapshotをmakepkg dependency transaction前後でcaptureしたり、transaction ownershipへ接続したりしていない。Slice 3 adapter APIもproduction routeから未使用である。
- metadata query failure、`PackageNotFound`、`Unknown` reasonは区別されるが、その区別だけでinvocation ownershipは証明されない。

利用者が明示選択したrepository providerは、invocation全体でdeduplicateしたexact `pacman -S [--asdeps] --needed` transactionへ別phaseとして渡される。ただしcurrent resultはactual package state changeを`Unknown`として保持し、cleanup inventoryを作らない。Moguetがphaseを開始した事実と、削除可能なpackage単位のcausal ownership proofは別である。

複数root / PackageBaseのsource-build work itemはorderedに実行され、BuildPlan上のrole、PackageBase、provider identityは保持される。しかしcurrent lifecycleには、makepkgが実際に導入したpackageを各edgeへcorrelateし、後続work itemからの共有利用が終わったことを証明するcleanup lifetime modelはない。package名や実行順だけから共有終了を推測してはならない。

### `NewlyObserved != InvocationOwned`

pre/post observationで次を確認できても、cleanup ownershipの証明としては不十分である。

```text
pre:  package absent
post: package present, reason=Dependency
```

この差分は、そのpackageがread phaseの間に新しく観測されたという`NewlyObserved`のevidenceに留まり、current invocationが導入を所有する`InvocationOwned`ではない。pre/post snapshot差分だけでは、少なくとも次を区別できない。

- `makepkg -s`が開始したdependency transaction。
- 並行するpacman transaction。
- invocation外で行われたpackage install。
- invocation外で行われたinstall reason変更。

したがって、旧#269が「ownershipを証明できない」とした問題を、snapshot fieldや観測回数を増やしただけで解決済みとしてはならない。現在orphanであること、package名がdependency edgeに現れること、makepkgが導入したように見えることもcausal proofではない。

Issue #404の後続Sliceでは、少なくとも次を別dimensionのtyped stateとして保持する。

- pre-existing / newly-observed。
- `Explicit` / `Dependency` / `Unknown` install reason。
- dependency role、root、PackageBase、selected providerとのcorrelation。
- current invocationがauthoritativeに因果関係を所有できるか。
- shared / still-required state。
- verified / protected / unknown / invalid classification。

cleanup eligibleへ昇格できるのは、Moguetがauthoritativeに所有するdependency transactionまたは同等のcausal proofへcorrelateでき、pre-existingではなく、`Dependency` reason、identity、installed stateを削除直前まで再validationでき、後続build unitから共有利用されないpackageだけである。proof、metadata、identity、reason、shared stateのいずれかを確定できないpackageは`Protected` / `Unknown`側へ倒し、cleanup candidateへ含めない。

Slice 1では、このcausal proofやtyped modelの具体的方式を実装・固定しない。Slice 2はfilesystem / pacman mutationへ未接続のpure model、Slice 3はmetadata / lifecycle correlation adapterであり、snapshot差分単独をownership authorityにしてはならない。

### Slice 2 pure cleanup classification authority（production未接続）

Slice 2のcleanup modelは、filesystem、environment、libalpm session、pacman、makepkg、process、sudo、prompt、cleanup executorを参照しないpure value / pure classifierである。production source-build lifecycleからは呼び出さず、cleanup candidateやremoval capabilityをcurrent runtimeへ公開しない。

入力evidenceは少なくとも次を独立したtyped dimensionとして保持する。

- baseline observation: `PreExisting` / `NewlyObserved` / `Unknown`。
- current installed state: `Present` / `Absent` / `Unknown`。current metadataが得られた場合も、既存の`Explicit` / `Dependency` / `Unknown` install reasonを別stateとして保持する。source-aware candidateのpackage versionは`Known` / `Unknown` / `Unavailable`を保持し、`Present`なcurrent metadataのversionとknown valueが異なる場合だけ明白なidentity contradictionとする。`Unknown` / `Unavailable`を不一致へ変換しない。
- causal ownership: authoritativeな`InvocationOwned` / known `NotInvocationOwned` / `Unknown`。baseline observationとは別型であり、`NewlyObserved`から変換しない。
- role / correlation: requested root、source-aware package child、PackageBase、BuildPlan dependency edge identity、typed dependency requirement、provider identityとresolution、`Root` / `RuntimeDependency` / `BuildDependency` / `CheckDependency`を保持する。同一packageの複数role、root、PackageBase、edgeを単一roleやpackage nameへflattenしない。
- correlation-set coverage: `Complete` / `Incomplete` / `Unknown`。これはcandidateに関係する全root、PackageBase、role、BuildPlan dependency edgeをcleanup classificationに必要な範囲でauthoritativeにprojectできたかを表すcandidate-level authorityである。
- shared lifetime: `StillRequired` / `NoLongerRequired` / `Unknown`。correlation数や実行順だけからshared lifetimeを推測しない。
- evidence quality: current package evidenceと各correlationを`Verified` / `Unverified`として保持する。per-correlation `Verified`は、その1件についてrole、edge、typed requirement、selected / resolved providerとprovided dependency identityのassociationをadapterがauthoritative inputへ照合済みであることを表す。別のroot、role、edgeが存在しないことやcorrelation集合全体の完全性は証明しない。これはcleanup classificationでも、Slice 4のmutation直前revalidationでもない。
- policy protection: package名heuristicではなく、adapter / policyが与えるtyped `Protected` / `NotProtected` / `Unknown`を保持できる。Slice 2は`base-devel` group queryを行わない。

classificationは`Eligible` / `Protected` / `Unknown` / `Invalid`を返し、次のprecedenceを固定する。

1. typed stateの範囲外、known candidate versionと`Present`なcurrent metadata versionの不一致、correlation package / provider identityの不一致、consumer requirementとdirect packageまたはproviderのprovided dependency identityの明白な不一致等、structurally malformedな入力は`Invalid`。
2. `PreExisting`、current `Absent`、`Explicit`、known `NotInvocationOwned`、root、runtime dependency、`StillRequired`、typed policy protectionのいずれかがあれば`Protected`。
3. positive protectionがなく、baseline、current installed state / reason、package version authority、causal ownership、identity / edge correlation verification、correlation-set coverage、build / check role correlation、shared lifetime、policy evidenceの必要なproofが不足すれば`Unknown`。coverage `Incomplete` / `Unknown`もこのarmであり、verifiedな1 edgeから残りのedge不在を推測しない。
4. すべてのproofが揃った場合だけ`Eligible`。

`Eligible`には、少なくとも`NewlyObserved`、current `Present`、`Dependency` reason、current metadataと一致するknown package version、authoritative `InvocationOwned`、verified current identity、verifiedなbuild / check dependency edge correlation、correlation-set coverage `Complete`、runtime / root roleなし、`NoLongerRequired`、追加policy protectionなしがすべて必要である。selected providerであるという事実自体はclassificationを変えない。selected providerがruntime roleを持てば`Protected`であり、build / checkだけで他のproofも揃う場合に限り`Eligible`となり得る。

Slice 3 adapterは、BuildPlan自体のcompletenessと、このcandidateに到達する全root、PackageBase、role、dependency edgeのprojectionを確認できる場合だけcoverage `Complete`を生成できる。partial BuildPlan、root subset、edge observation failure、completeness authority不在のいずれかがあれば`Complete`を生成してはならず、判明している範囲に応じて`Incomplete`または`Unknown`を使う。consumer requirementとprovider capabilityのversion constraintやSONAME semanticsはSlice 2 pure classifierで再評価せず、adapterがassociationを証明できないcorrelationは`Unverified`として`Unknown`へ倒す。pure classifierがtyped identity同士から直接確認できる明白なname contradictionだけは`Invalid`とする。

結果reasonはuser-facing proseではないtyped valueであり、選択されたprecedence armのreasonだけをcanonical enum orderで返す。`Invalid`は情報不足の別名にせず、情報不足は`Unknown`へ倒す。Slice 2のverificationはcurrent evidenceのqualityまでであり、Slice 4が所有するmutation直前revalidationを表したり代替したりしない。

### Slice 3 metadata / lifecycle adapter authority（production未接続）

Slice 3 adapterは、install-reason付きbaseline/current snapshot、typed `BuildPlan`、source-aware resolved candidate、prepared production work item、work-item outcome、selected repository provider transaction resultをread-onlyで受け取り、Slice 2の`InvocationOwnedCleanupCandidate`へprojectする。pacman、makepkg、filesystem、sudo、prompt、removeを呼ばず、production source-build / local build / CLI routeからは呼ばない。

baseline/current projectionは次を固定する。

- baseline snapshot成功 + package存在は`PreExisting`。
- baseline snapshot成功 + package不在は`NewlyObserved`。これはownershipではない。
- baseline snapshot failureは`Unknown`。
- current snapshot成功 + package存在は`Present`とexact current metadata。
- current snapshot成功 + package不在は`Absent`。
- current snapshot failureは`Unknown`かつmetadata authorityなし。

source-aware candidateは`ResolvedDependencyCandidate`またはrepository exact observation等のtyped authorityからのみ構成し、installed name/versionやpackage名からsource / PackageBaseを補完しない。Slice 4以降のrepository providerはstrict `RepositoryExactPackage.package_base`をprovider identityへ保持する。empty PackageBaseをchild nameで補完せず、requirement / provided capability、actual provider package、PackageBaseを別identityとして相関する。authoritative candidate identity自体を構成できない場合はcandidateを捏造せずtyped projection failureを返す。known candidate versionとcurrent installed versionの不一致はSlice 2 classifierの`Invalid`へ渡し、`Unknown` / `Unavailable` versionは推測でknownへ変換しない。

correlation coverage `Complete`は少なくとも次の全条件が揃う場合だけ生成する。

- `project_build_plan_state()`がconstructed / completeであり、provider decisionがuniqueまたはselectedとして確定している。
- required artifact target projectionが成功し、全root、PackageBase、package target、role、execution orderが相互に整合する。
- prepared work itemがBuildPlan orderと1対1で対応し、各PackageBaseのtyped source identityとrequired targetを保持する。
- candidateに関係する全dependency edgeがtyped requirement、resolved source-aware candidate、successful constraint evaluation、requiring package、root attribution、provider associationを保持する。

resolution failureやprovider candidate observation failureで全集合自体を断言できない場合は`Unknown`、unresolved / ambiguous / cancelled provider、missing source context、missing requirement / resolved identity、repository provider PackageBase不足等の既知gapは`Incomplete`へ倒す。verifiedな1 edge、vector件数、package名一致だけから`Complete`を作らない。baseline/current metadata failureはcorrelation集合の列挙可否とは別dimensionであり、集合を列挙できても各correlationとcurrent package evidenceを`Unverified`へ倒してclassifierの`Eligible`を禁止する。

shared lifetimeはtyped lifecycle boundaryを`BeforeBuildCompletion` / `AfterWorkItem` / `AfterSuccessfulInvocation` / `Unknown`として区別する。後続work itemのverified edgeがcandidateを必要とする場合とroot / runtime roleは`StillRequired`、全work itemのbuild / install success、coverage `Complete`、build / check-only、後続利用なしをすべて確認できる場合だけ`NoLongerRequired`とする。それ以外は`Unknown`である。local routeはcurrent prepared remote dependency invocationがlocal root unitの完全集合を所有しないため、Slice 3ではcoverage / shared lifetimeを安全側へ倒す。

causal ownershipはbaselineやshared lifetimeとは独立してprojectする。current `makepkg -s` outcome、`SelectedRepositoryProviderTransactionResult::Succeeded`、`PackageStateChange::Unknown`、command exit 0、または将来のtransaction-level aggregate `Changed`からpackage単位`InvocationOwned`を生成しない。authoritativeにpre-existingと確認したpackageもcausal stateを推測せず、baseline `PreExisting`によって`Protected`とし、causal ownershipは`Unknown`を保つ。

Slice 3にはgroup等のcomplete policy inventory authorityがないため、policy protectionは`Unknown`である。このためcurrent adapterはproduction evidenceから`Eligible`を生成しない。

### Slice 3.5 causal transaction receipt authority（production transport未接続）

Slice 3.5は、snapshot、command success、transaction targetをcausal proofへ昇格させず、package manager transactionから得るmachine receiptの必要条件をpure typed contractとして固定する。

pacman 7.1.0 / libalpm 16.0.1のinstalled manualと、host package databaseを共有しないisolated Arch transaction fixtureで、ALPM hookの次のsemanticsを確認した。

- `Operation = Install`はtransaction開始時点で存在しないpackageに一致する。versionが同じか新しいかにかかわらず、既に存在するpackageは`Upgrade`であり`Install`ではない。
- `Type = Package`、`Target = *`、`NeedsTargets`はmatched package nameをstdinへ渡す。localized pacman prose、stdout / stderr、pacman logではない。
- `PostTransaction` hookはtransaction commit failure時に実行されない。
- `pacman --hookdir`はabsoluteな追加hook directoryであり、複数指定時は後のdirectoryが同名hookをoverrideする。custom hookを指定したpacman invocationへだけ追加できるが、directoryとhook executableのtrustはpacmanをrootで実行する前に別途証明する必要がある。

pure receipt modelは少なくとも次を別dimensionとして保持する。

- `InvocationDependencyTransactionOwner`: selected repository provider、source artifact install、makepkg syncdeps、unknown。
- `InvocationDependencyTransactionCommandOutcome`: not attempted、succeeded、failed、unknown。
- 64 lowercase hexのtransaction token。token generationやtransportはこのpure modelの責務ではない。
- `PacmanTransactionReceiptState`: unavailable、incomplete、complete、invalid。
- package単位の`Install` / `Upgrade` operation。`Remove` / unknown operation、invalid package name、duplicate package name、token / owner mismatchはinvalidである。
- `InvocationDependencyTransactionLedger`: requested package identity、command outcome、receiptをtransaction順に保持する。requested targetとactual `Install` packageを同一視せず、solver-introduced packageもreceipt evidenceとして保持できる。後続transactionの`Upgrade`やreceipt failureで、先行transactionの`Install` evidenceを上書きしない。

receipt completenessは「hookらしい出力があった」ことではない。exact transaction tokenとownerが一致し、machine protocolがexplicitなfinal recordへ到達し、全recordがvalidかつduplicateなしの場合だけ`Complete`である。missing、partial / truncated、unexpected pre-existing data、malformed record、identity mismatchは`Unavailable` / `Incomplete` / `Invalid`として保持し、packageをnewly installedへ公開しない。command outcomeはreceipt completenessと独立しており、command failureではcomplete-looking receiptがあってもownershipを認めない。

Slice 3 adapterは、次の全条件が揃う場合だけcausal dimensionを`InvocationOwned`へprojectできる。

- baselineが`NewlyObserved`であり、これ自体はownership proofとして使用しない。
- current packageが`Present`かつverifiedで、candidate nameおよびknown versionと矛盾しない。
- ledger entryのcommand outcomeが`Succeeded`である。
- ledger token、owner、requested package identityがstructurally validである。
- receiptがそのexact token / ownerに対して`Complete`である。
- candidate packageがactual `Install` recordに存在する。

complete receiptでcandidateが省略された場合、`Upgrade`だけが存在する場合、receiptがmissing / incomplete / invalidの場合、command failure、token / owner mismatch、legacy `PackageStateChange::Changed`の場合は`NotInvocationOwned`へ推測せず`Unknown`へ倒す。pre-existing packageと`Install` receiptが矛盾する場合も`InvocationOwned`へ上げず、既存classifierの`PreExisting` protectionを維持する。

#### Slice 3.5時点のprivilege / transport boundary

ALPM hook actionはpacman transaction内でrootとして実行され得る。Slice 3.5時点のMoguet packageは、causal receipt専用のroot-owned helper、root-owned hook directory、privileged IPC、root-owned `/run` stateをinstallしていなかった。このためSlice 3.5ではproduction hook transportを接続しなかった。

次は安全な代替ではなく禁止する。

- user-writable temporary hook fileまたはhook directoryを`sudo pacman --hookdir`へ渡すこと。pacmanが読む前の差し替えによりarbitrary root executableを起動できる。
- current running Moguet path、build treeの`./build/moguet`、user-writable wrapper / helperをhook `Exec`にすること。
- root hookがuser指定path、predictable `/tmp` path、symlink-following pathをopen / truncate / createすること。
- stdout / stderr、pacman log、timestamp、pre/post snapshotへreceiptを混在させ、parserで拾うこと。
- `sh -c`、`tee`、shell quotingによってarbitrary root writeを安全化したとみなすこと。

安全なproduction transportを追加するには、少なくともroot-owned installed hook / dedicated helperのprovenance、arbitrary pathを受けないfenced protocol、transaction tokenをexact pacman invocationへ束縛するprivileged transport、symlink / owner / mode / inode replacement防止、partial write検出、install / uninstall payload contract、actual installed-package validationが必要である。Slice 3.6は次節の限定scopeでこの不足を解決する。開発treeのhelperをroot実行するtestでは代替しない。

### Slice 3.6 trusted ALPM receipt transport

#### Threat modelとhelper provenance

Slice 3.6は、unprivileged userによるhook / state差し替え、symlink substitution、arbitrary root write、stale receipt reuse、token replay、別transaction receiptの誤帰属、partial / malformed / duplicate receipt、wrong owner、path / executable / shell injection、development-tree executableのroot実行を防ぐ。Moguetがuser inputを任意root capabilityへ変換しないことをboundaryとする。root権限そのものを取得したactorや、同じ利用者が既存sudo authorityを意図的に別pacman invocationへ悪用することは、新しいsecurity boundaryとして扱わない。

packageはinternal executable `moguet-alpm-receipt-helper`を`${CMAKE_INSTALL_LIBEXECDIR}/moguet/`へinstallする。canonical Arch layoutは`/usr/libexec/moguet/moguet-alpm-receipt-helper`、mode `0755`、package install時のownerはrootである。public command、PATH lookup対象、man page対象ではない。production Moguetと生成hookはconfigure時に確定した同じabsolute installed pathだけを使い、source tree、build tree、cwd、home、`/tmp`、caller指定executableをroot実行しない。receipt-capable transportはhelper、`/usr/bin/sudo`、`/usr/bin/pacman`についてroot ownership、regular executable、group / world non-writableなancestor / file identityをnofollow descriptor walkで確認する。rootによる置換は上記threat model外だが、unprivileged置換を許容しない。

#### Token、root-owned state、filesystem policy

unprivileged transportがLinux `getrandom(2)`から256 bitを直接取得し、64文字のlowercase hexへencodeする。`rand()`、timestamp、counter、`std::random_device` fallbackは使わず、generation failureはtransaction開始前にfail closedする。

runtime stateはboot-localな次の固定hierarchyだけを使う。

```text
/run/moguet/                              root:root 0700
  alpm-receipts/                          root:root 0700
    active/                               root:root 0700
      <64-lowercase-hex-token>/           root:root 0700
        prepared                          root:root 0600
        hooks/                            root:root 0700
          moguet-install-<token>.hook     root:root 0600
        receipt                           root:root 0600, Complete時だけ
    used/                                 root:root 0700
      <token>/                            root:root 0700, empty tombstone
```

`/tmp`、`TMPDIR`、XDG runtime、home、build directory、caller指定pathは使わない。全操作はnofollow-openしたdirectory descriptorから`openat` / `mkdirat` / `fstatat(AT_SYMLINK_NOFOLLOW)`等で相対解決し、expected uid / type / exact modeとnamed entryのdevice / inode identityを確認する。file作成は`O_CREAT|O_EXCL|O_NOFOLLOW`相当、transaction publicationとreceipt finalizationは`renameat2(RENAME_NOREPLACE)`、file / directoryは`fsync`してから公開する。kernel / filesystemがno-replace publicationを提供できない場合はfallbackせずfail closedする。

PREPAREはfinal token directoryを直接組み立てず、private staging directoryへprepared stateとhookを完成させてからactive tokenへatomic publishする。既存active / preparing / used token、symlink、unexpected objectはreuseしない。CONSUME / ABORTはactive tokenをusedへatomic retireしてからexact known childrenだけをcleanupし、empty tombstoneをboot終了まで残す。これによりconsume / abort済みtokenは再prepareできない。`/run`はboot開始時にclearされるephemeral authorityであり、background sweeperやbroad directory sweepは追加しない。unexpected stateやexact cleanup failureは隠さずnonzeroとし、root-owned stateを推測で再帰削除しない。

#### PREPARE、hook、RECORD、CONSUME、ABORT

helper CLIは次の4つのfixed verbだけを持つ。owner stringは`selected-repository-provider`だけを受理する。

```text
prepare <token> selected-repository-provider -- <requested-package>...
record  <token> selected-repository-provider
consume <token> selected-repository-provider
abort   <token> selected-repository-provider
```

output path、hook path、state root、executable、command、environmentを指定する引数は存在しない。PREPAREはtoken、owner、nonempty / unique / validなexact requested package nameを検証する。requested setはprepared audit stateでありcausal proofやreceipt filterではない。

PREPAREが作るunique hook filenameにはtokenを含め、system / later hook directoryとの実用上のname collisionを避ける。hookのdynamic dataはvalidated tokenとfixed ownerだけで、package name一覧を`Exec`へ埋めない。

```ini
[Trigger]
Operation = Install
Type = Package
Target = *

[Action]
When = PostTransaction
Exec = /usr/libexec/moguet/moguet-alpm-receipt-helper record <token> selected-repository-provider
NeedsTargets
```

actual configured helper pathはinstall prefixに対応するabsolute pathである。shell、`sh -c`、PATH lookup、quoting、user-writable configを介さない。`Operation = Install`だけを対象とし、Upgradeのfull inventoryや「Installがない」というnegative proofは作らない。PostTransactionが実行されないfailureやInstall non-matchはpositive proof unavailableである。

RECORDはrootで、exact prepared state / hook identityを再検証してからNeedsTargets stdinをEOFまで読む。全recordをlocale-neutralなpackage grammarで検証し、duplicate、control character、missing final newline、empty input、100000件超、1 package 4096 bytes超、protocol全体16 MiB超を拒否する。正常なInstall setだけを`receipt.partial`へwrite + fsyncし、final `receipt`が存在しないことをno-replace renameで保証してComplete publishする。duplicate RECORD、partial file、pre-existing receipt、unexpected extra stateはfail closedである。

CONSUMEはexact token / owner / prepared state / hook / receipt owner・mode・type・inodeを再検証する。Complete receiptがなければvalidな`Missing` responseを返すが、これをNotInvocationOwned証明へ使わない。Complete / Missingのresponse bytesを構成後、active stateをone-shot retire / cleanupしてからstdoutへ固定machine protocolだけを返す。diagnosticはstderrでありmachine stdoutへ混在させない。ABORTはpacman nonzero / exec failure時にexact token stateだけをretire / cleanupし、complete-looking receiptがあってもcommand failureをownershipへ変換しない。

#### Machine protocol

protocolはstrict line format version 1で、field順、tab separator、final newline、`END` markerを固定する。unknown / reordered / duplicate fieldを許容しない。PREPARE responseはheader、token、owner、derived `HOOKDIR`、`END`、receipt responseはheader、token、owner、`STATE`、0件以上の`INSTALL`、`END`を持つ。`STATE=Complete`は1件以上のunique valid `INSTALL`を必要とし、`STATE=Missing`は`INSTALL`を持たない。stored Complete receiptとCONSUME stdoutはpacman localized stdout / stderr / logではなく、root-owned helper protocolである。unprivileged sideはbounded raw stdoutをstrict parseし、その後にSlice 3.5 validatorへtoken / owner / `Install` observationとして渡す。

#### Exact transaction bindingとsudo argv

bindingは次のchainで成立する。

```text
getrandom token
  -> root PREPARE state + unique root-owned hookdir
  -> same selected-provider pacman argvの --hookdir
  -> hook内fixed token / owner
  -> same execution resultのcommand outcome
  -> exact token / owner CONSUME
  -> same InvocationDependencyTransactionLedger entry
```

helperとpacmanは`ExplicitProcessInvocation`のargv-vectorで`execve(2)`へ渡し、executableはabsolute `/usr/bin/sudo`、sudoのtargetもabsolute helper / `/usr/bin/pacman`とする。child environmentは`PATH=/usr/bin`と`LC_ALL=C`のfixed setだけで、caller environmentをroot boundaryへpreserveしない。shell executionはない。表示用にargvをquoteすることは実行authorityではない。

PID、`/proc` start time、pidfd等の脆い追加bindingは採用しない。unrelated pacman transactionはunique `--hookdir`を持たないためreceiptへ入らない。same userがtoken / hookdirを観測して別のsudo pacmanへ意図的に与える行為は、既存root authorizationの意図的悪用であり今回の境界外である。

#### selected-provider integrationとfailure semantics

既存の`execute_selected_repository_provider_transaction(invocation, config)`は変更せず、helper / `/run` / hookへ依存しない。別のtyped `SelectedRepositoryProviderTrustedReceiptRequest` overloadだけがtrusted transportを要求する。current public `--rmdeps` routeはこのcapabilityを生成せず、通常buildのargv / failure contractを変えない。

receipt-capable pathはtrusted PREPARE failureならpacman前にblockする。pacman nonzeroではABORTし、ledger command outcomeを`Failed`とする。pacman success後のmissing / malformed / consume failureはcompleted package transactionをfailureへ書き換えず、receipt dimensionをUnavailable / Incomplete / Invalid側へ倒してcausal proofだけを禁止する。requested targetとreceipt actual Install setは別vectorであり、solver-introduced Installもそのままledgerへ保持する。

#### Mutation pathごとの結果

- selected repository provider: Moguet-owned `sudo pacman -S [--asdeps] --needed`に対するproduction-capable typed receipt pathが成立する。baseline absent、current verified Present / Dependency、command Succeeded、exact token / owner、Complete actual Install receiptが揃えばSlice 3.5 ledgerからpackage単位`CleanupCausalOwnership::InvocationOwned`を生成できる。legacy result successやaggregate Changedだけからは生成しない。
- typed artifact install: singular / PackageBase setのexact artifact identity、desired reason、`--needed` policy、transaction resultは保持するが、pre-transaction installed observationとpacman successだけではexternal transaction raceを排除できない。trusted receipt transportなしでは`pacman -U`の`Installed` outcomeをnewly installed ownershipへ読み替えない。
- makepkg syncdeps: current `makepkg -sc`内部の`pacman -S --asdeps`へcustom hookを安全にattachするproduction pathはない。`PACMAN` / `PACMAN_AUTH` override、wrapper、injected configをuser-writableな形で追加しない。

makepkg `-s`分離もSlice 3.5では行わない。current BuildPlanはdepends / makedepends / checkdepends、repository / AUR / local candidate、typed constraint、provider、PackageBase / split child、multi-root、local architectureを広く保持するが、remote AUR RPC projectionはmakepkgのcurrent PKGBUILD / architecture-qualified evaluationそのものではなく、direct repository dependency transaction全体もMoguet-owned phaseへ移されていない。pacman solverを再実装せず、already-satisfied state、provider、version constraint、split / architecture semanticsをmakepkgと同等に保つ証明がないため、`makepkg -s`を外さない。

`makepkg -r` / `-scr`へ戻すこともIssue #404のseparated artifact install、preview、explicit confirmation、phase separationを満たさないため採用しない。

### Slice 3.5 causal authority readiness（historical）: NO-GO

pure typed receiptとadapter projectionは、authoritative inputを与えるtest上でpackage単位の`CleanupCausalOwnership::InvocationOwned`を生成できる。しかしcurrent production lifecycleからその`Complete` receiptを安全に構成するtransportはなく、実在production evidence pathからは`InvocationOwned`を1件も生成できない。したがってcausal authority readinessはNO-GOである。

Slice 4 removalへ進む前に、少なくとも次のいずれかを提供する別Slice / redesignが必要である。

- package単位のmachine-readable changed-setとtransaction result。
- external transaction raceを排除できるtransaction ownership tokenまたはpackage-manager lock / transaction authority。
- makepkg syncdepsをMoguet-owned dependency install phaseへ分離し、そのexact inputとactual changed packageを相関するlifecycle。

stdout / stderr、localized output、pacman log、timestamp近接、orphan state、BuildPlan上のpackage名はこの不足を埋めるauthorityではない。

### Slice 3.6 readiness

- trusted transport readiness: **GO**。installed helper、root-owned state、transaction-local Install hook、exact token / owner、atomic Complete receipt、one-shot consume / abort、security negative、networkなしのinstalled Arch transaction fixtureが成立する。
- selected-provider causal readiness: **GO**。production-capable selected-provider typed pathからSlice 3.5 ledgerを構成し、actual Install packageを`InvocationOwned`へprojectできる。
- overall causal authority: **PARTIAL**。`pacman -U` / source artifact installと`makepkg -s` syncdepsはUnknownのままである。
- Slice 4 readiness: **NO-GO**。policy protection、route completeness、shared lifetime、mutation直前revalidation等が独立blockerとして残る。

### Slice 3.7 makepkg syncdeps causal authority audit（2026-08-28）

current Moguetは、validated checkoutをcurrent directoryとして、custom `V=K`をfirst-seen orderで渡し、invocation-ownedなabsolute `PKGDEST`を最後のassignmentとして追加した上で、次のbuild-only baselineを実行する。

```text
makepkg -sc
```

`--noconfirm`、`-f`、`-C`は対応するcurrent optionが有効な場合だけこの順で追加する。`-c`はsuccessful build後のwork file cleanupであり、`-C` / `--cleanbuild`や`-r` / `--rmdeps`ではない。Moguetはpacman用`--config` / `--hookdir`、makepkg用`--config`、`PACMAN`、`PACMAN_AUTH`を通常buildへ追加しない。makepkgは既定ではsystem makepkg config、drop-in、user configを読み、明示された`MAKEPKG_CONF`等のcurrent environment authorityも保持する。

pacman 7.1.0 / makepkg 7.1.0のpublic manual、installed makepkg implementation、host package databaseを使わないfake pacman / auth characterizationから、current syncdeps flowを次のとおり確認した。

- makepkg自身がPKGBUILDをsourceし、current `CARCH`の`depends_<arch>`、`makedepends_<arch>`、`checkdepends_<arch>`をglobal arrayへmergeする。
- runtime `depends`を先に`pacman -T`で確認し、不足時だけ`pacman -S --asdeps`へ渡す。その後、`makedepends`と、有効な`check()`に対応する`checkdepends`を同様に確認する。したがって1回のmakepkg buildはsync dependency install transactionを0〜2回開始し得る。
- current Moguetは`--check` / `--nocheck`を追加しないため、`check()` / `checkdepends`の有効性はmakepkg configとPKGBUILDが所有する。characterizationではdefault check有効時に`makedepends + checkdepends`が同じ2回目のtransactionへ入り、`--nocheck`では`checkdepends`だけが除外された。
- already-satisfied dependencyは`pacman -T`後にinstall transactionを開始しない。version constraint、installed provider / versioned providesのsatisfactionと、不足時のrepository provider選択、solver-introduced dependency、conflict等はpacman / libalpmが所有する。
- split package固有の`package_<name>()`内`depends`はsyncdeps対象ではなく、buildに必要なdependencyはglobal `depends` / `makedepends`へ置くというupstream contractを維持する。
- dependency transaction failureはmakepkg failureとしてbuild前に停止する。command failureとreceipt completenessは別dimensionのままである。

#### Instrumentation candidate result

- Candidate A（dedicated pacman config / HookDir）: **NO-GO**。pacmanの`--config` / `--hookdir`と`pacman.conf`の`HookDir`はpublic contractだが、makepkgには内部pacmanへこれらを渡すpublic pass-throughがない。global hook directoryへdynamic hookを置く方式もunrelated pacman transactionからexact tokenを分離できない。internal `PACMAN_OPTS`等のcurrent shell implementationへ依存しない。
- Candidate B（`PACMAN` / `PACMAN_AUTH`）: user-writable wrapper / config / build treeをroot pathへ到達させる形は**NO-GO**。両者はpublic overrideだがhook専用authorityではなく、`PACMAN`はpacman互換command全体、`PACMAN_AUTH`はroot command prefix全体を置き換える。PKGBUILDも同じshell contextでsourceされ、characterizationではPKGBUILD側`PACMAN_AUTH`がactual dependency install prefixへ伝播した。package-installed root-owned dedicated adapterなら理論上はfail-closedに構成できるが、transactionごとのtoken生成、0〜2回のinternal transaction集約、root-owned multi-receipt state、parent makepkg outcomeとのcorrelation、owner拡張、adapter bypass時のMissing処理が必要であり、独立security Sliceへ**DEFER**する。
- Candidate C（`makepkg -s`を外してMoguetがsyncdeps）: **NO-GO**。current BuildPlanはcurrent checkoutのPKGBUILD evaluation、effective makepkg config、architecture merge、check enablement、split-package syncdeps rule、already-satisfied/provider/version semanticsをmakepkgと同一authorityで再現しない。`makepkg --printsrcinfo`等の追加評価だけでもcurrent flow全体の代替proofにはならない。makepkg / pacman semanticsをMoguetへ複製しない。
- Candidate D（pre/post snapshot + selected-provider receipt）: **NO-GO**。selected-provider receiptが証明するのはそのdirect transactionだけであり、makepkg内部transactionのsnapshot差分を`InvocationOwned`へ昇格しない。
- Candidate E（pacman log / stdout parser）: **NO-GO**。localized prose、log、timestampはtransaction-local causal receiptではない。

Slice 3.5のpure ledgerと`InvocationDependencyTransactionOwner::MakepkgSyncDependencies`は将来adapterから再利用できる。一方、Slice 3.6 protocol / helper / transportはownerをselected repository providerへ固定し、1 token / 1 Complete receiptをone-shotで扱う。makepkg内部の複数transactionへそのまま接続できず、概念の再利用とproduction transportの小変更を同一視しない。

#### Slice 3.7 readiness / return-home decision

- makepkg syncdeps causal readiness: **DEFER**。安全なroot-owned adapterの方向は存在するが、限定的なintegrationではなくSlice 3.6同等以上のprivileged adapter / IPC redesignを必要とする。
- selected-provider causal readiness: **GO**のまま維持する。
- overall causal authority: **PARTIAL**。makepkg syncdepsとtyped dependency artifactの`pacman -U`はUnknownのままである。
- Slice 4 production cleanup readiness: **NO-GO**。causal authorityに加えてpolicy protection、route / correlation completeness、shared lifetime、mutation直前revalidationが未成立である。
- Issue #404 continuation recommendation: **RETURN-HOME**。v2.5.0では成立済みのmodel / adapter / selected-provider trusted receipt foundationを保持し、public source-build `--rmdeps`はunsupported / fail-closedのままとする。makepkg syncdeps authority、残るcleanup candidate authority、preview / confirmation / mutation executorは最大3件のfollow-up候補へ分離し、Issue #404内で新しいsecurity subsystemを開始しない。

### Issue #485 Slice 1 source-artifact causal evidence（production未接続）

actual package archive metadataは、validated path provenanceとは別にread-only `alpm_pkg_load()`と`alpm_pkg_get_name()` / `alpm_pkg_get_version()` / `alpm_pkg_get_base()` / `alpm_pkg_get_arch()`から取得する。`ArtifactPackageIdentity`はchild、exact full version、actual PackageBase、actual architectureを保持し、PackageBase / architectureの`Known` / `Missing` / `Malformed` / `Unavailable`を区別する。missing等をBuildPlanやupper source contextの値で補完せず、known expected child / version / PackageBase / architectureとのexact correlationだけをcompleteとする。`any`とknown other architectureもactual valueとして保持し、cleanup側でcompatibility solverを再実装しない。

artifact path provenance、archive metadata identity、future actual ALPM Install receiptは別dimensionである。stable artifact indexはvalidated aggregate内のpathへ解決できるが、それ自体はpath capabilityでもreceiptでもない。同じinodeのcontent mutationをpath identityだけでは排除できないため、future transport / candidate integrationでもactual transaction receiptとcurrent installed identityを省略しない。

`SourceArtifactInstall` evidenceは次を一つのclosed valueへ束縛する。

- invocation-local identity、exact work-item index、PackageBase、requested root set。
- selected artifact exact set。各artifactのstable index、expected source-aware child / version / PackageBase / exact architecture、actual archive identity、build / check role、`Dependency` desired reason、root attributionを保持する。
- exact transaction token、hard-coded `InvocationDependencyTransactionOwner::SourceArtifactInstall`、requested package set、command outcome、receipt state / operation、actual Install setを保持する。

positive causal capabilityは、bindingとselected setがexact、command `Succeeded`、receipt `Complete`、same token / fixed owner、全selected childがactual `Install`の場合だけ発行する。actual Install setのsolver-introduced / unselected entryはfactual resultへ残るがselected artifactへ昇格しない。`Upgrade`はversionの大小や同一性を問わず、normal upgrade、same-version reinstall、downgradeのすべてでpositive不可である。Root / Runtime role、Explicit desired reason、missing / incomplete / invalid receipt、command failure、token / owner / invocation / work item / PackageBase / selected set / name / version / architecture mismatchもpositive不可とする。

generic `InvocationDependencyTransactionLedger`はfactual modelとして残すが、production candidate adapterはraw ledgerを受け取らない。raw ledgerからの旧causal projectionはreceipt model regression用test hookだけに限定し、selected-provider transport / helper / fixed owner / `/run` stateは変更しない。Slice 1時点の`SourceArtifactInstallReceiptObservation`にはproduction constructorがなく、次節のSlice 2 transportだけがproduction observationを構築する。

### Issue #485 Slice 2 source-artifact trusted receipt transport（candidate未接続）

SourceArtifactInstallはselected-provider helperをgeneric化せず、package-installedな
`/usr/libexec/moguet/moguet-source-artifact-install-helper`、fixed owner
`source-artifact-install`、fixed verb grammar、専用
`/run/moguet/source-artifact-installs/{active,used}`を使用する。source helperのCLIはowner、state root、
executable、hookdir、destination、pacman config / root / dbpathをcallerから受け取らない。selected-provider
tokenとsource-artifact tokenは相互のstate namespaceでconsumeできない。

unprivileged transportはvalidated aggregateが保持するartifact / signature descriptorからbytesをcopyし、
size、mtime、ctime、device、inode、ownerをcopy前後で再確認する。copy先はLinux memfdで、
`F_SEAL_WRITE` / `F_SEAL_GROW` / `F_SEAL_SHRINK` / `F_SEAL_SEAL`をすべて付与する。root helperはこの
write-sealed regular fileだけをstdinとして受理し、caller pathを受け取らない。root helperはboundedな
exact byte countをprivate preparing directoryへcopyし、root:root `0600`のgenerated basenameだけを作る。
各root-owned staged archiveをread-only libalpmで再読し、child name、full version、actual PackageBase、
actual architectureがprepared expectationとbyte-exactに一致した後だけ、state全体をactive tokenへatomic
publishする。signatureが存在する場合も対応するgenerated sidecarへstageする。

transportが実行するtransaction argvはabsolute `/usr/bin/sudo`と`/usr/bin/pacman`のshell-free argvで、
次のclosed subsetだけである。

```text
/usr/bin/pacman -U [--needed] [--asdeps] [--noconfirm]
  --hookdir <root-owned exact transaction hookdir>
  -- <root-owned exact staged artifacts...>
```

全selected artifactのdesired reasonは`Dependency`、roleはbuild / checkだけに限定する。existing Dependencyを
保持するtransactionではdirectiveなしを許すが、new installのpositive pathは`--asdeps`である。
`--needed` successでもInstall receiptがなければpositiveにしない。hookは`Operation = Install`、
`PostTransaction`、`NeedsTargets`だけを記録し、Upgrade、same-version reinstall、downgradeはMissing receiptに
留まる。command failure、consume failure、malformed / incomplete / missing receiptをactual Installへ推測しない。

root prepare / run / consume / abortは256-bit CSPRNG tokenをone-shot retirementへ束縛する。activeからusedへ
先にatomic retireし、prepared state、hook、staged archive / signature、receiptというknown exact childrenだけを
cleanupしてempty tombstoneを残す。unknown shapeをrecursive deleteせず、crash後のactive / used stateやstaged
bytesを別tokenのauthorityへ再利用しない。

trusted resultはexact invocation、work-item index、PackageBase、requested roots、selected stable index /
identity / role / reasonとtransaction-local ledgerを同じ`SourceArtifactInstallReceiptObservation`へ保持する。
requested selected setとactual Install setは別vectorであり、solver-introduced Installをfactual setへ保持しても
selected cleanup authorityへ自動昇格しない。transport resultからSlice 1のclosed receipt evidenceとcausal
capabilityを作れるが、`project_invocation_owned_cleanup_candidate()`、policy、shared lifetime、route completeness、
public `--rmdeps`へは接続しない。

### Issue #485 Slice 3 protected build environment policy（candidate未接続）

initial protected build environment policyはexact `base-devel`だけである。current Archでは`base-devel`は
groupではなくmeta packageであり、installed exact meta-package dependency metadataをprimary、完全な
configured sync exact meta-package metadataを次のauthorityとする。両方がauthoritatively absentで、全configured
sync databaseとexact group inventoryがcompleteな場合だけ、exact `base-devel` groupをcompatibility fallbackとして
使う。arbitrary group、all groups、設定追加、`gcc` / `make`等のhard-coded member listは導入しない。

candidateはread-only local libalpm metadataからexact name / version / provides / groupsをowned snapshotへ保持する。
meta dependencyとのsatisfactionは`alpm_pkg_get_depends()`と`alpm_find_satisfier()`を使い、dependency parser / solverを
Moguet側へ追加しない。local-only packageもlocal objectのname / versioned providesがdependencyをsatisfyする場合は
`Protected`であり、repository identityをpackage名から補完しない。group fallbackのmembershipもcandidate local metadataの
exact group declarationを使い、sync group memberとのpackage-name-only相関をprovenanceへ昇格しない。

factual `CleanupPolicyProtectionEvidence`はlocal DB、candidate metadata、installed meta、configured sync meta、group
inventory、satisfier evaluation、source consistency、failureを別stateで保持する。pure
`project_cleanup_policy_protection()`は次を固定する。

- selected authorityのcomplete positive satisfier / exact group evidenceだけを`Protected`へprojectする。
- `NotProtected`はcandidate、selected protected authority、inventory、satisfier evaluationがすべてcompleteで、failure / contradictionがない場合だけ生成する。
- local DB / required sync DB failure、candidate incomplete、meta / group malformed、partial inventory、evaluation failure、authority unresolved、contradictionは`Unknown`とする。
- missing exact group、package-name listへの非membership、query failureを`NotProtected`へ変換しない。
- installed metaはsync / groupより、sync metaはgroupより優先する。group fallbackはexact metaがpresentまたはunresolvedなら選択しない。

standalone query / reducerはproduction buildへ含まれるが、current
`project_invocation_owned_cleanup_candidate()`はSlice 4のinvocation-wide aggregateをまだ消費しないlegacy projectionであり、
production candidate collector未接続のためpolicyを`Unknown`に保つ。Explicit、PreExisting、Root、RuntimeDependency、shared lifetime、receipt、routeはpolicy reducerへ
混ぜず、既存classifierの独立dimensionを維持する。public behavior、cleanup candidate collector、preview / prompt /
removalは変更しない。

### Issue #485 Slice 4 correlation / shared lifetime / route authority（candidate未接続）

Slice 4は、repository providerのauthoritative PackageBaseをstrict repository observationからprovider identityへlosslessに保持する。selected repository provider correlationは、invocation identity、exact BuildPlan edge、user-selected decision、configured repository order、actual provider package、PackageBase、provided capability / requirement、current package identity、paired trusted execution result / capture、token、Install receiptを同じclosed evidenceへ束縛する。raw receipt、transaction success、solver-introduced Install単独をselected provider edgeへ昇格しない。

`SourceArtifactInstallCausalEvidence`は、invocation identity、ordered work-item index、PackageBase、selected artifact、dependency role、requested rootに加えてexact BuildPlan edge indexを保持する。route-specific correlationはprepared work itemとcomplete BuildPlanへ再照合し、same child / versionでもinvocation、work item、PackageBase、root、edge、roleのどれかが異なれば`Complete`にしない。

invocation-wide evidenceはcomplete BuildPlan、全root、ordered work item、全PackageBase identity、全dependency edge、全work-item outcome、全source-artifact correlation、全selected-provider correlationを一つのowned aggregateへ保持する。shared lifetimeはcandidate countやfirst consumer completionでは決めない。Root / Runtime consumerまたはverified later consumerは`StillRequired`、failed / unattempted final lifecycleとincomplete evidenceは`Unknown`、supported invocation全体が成功し全consumerがbuild / check-onlyで後続requirementがない場合だけ`NoLongerRequired`である。

routeとevidenceは独立dimensionである。`CleanupRouteAuthority`は`Complete / Unsupported / Unknown`、`CleanupEvidenceCompleteness`は`Complete / Incomplete / Unknown`を保持する。必要evidenceが全て揃ったremote AUR source buildだけがinitial positive `Complete` routeである。local build、`upgrade`、`upgrade-aur`、`upgrade-all`は`Unsupported`、standalone repository sourceとmakepkg syncdepsは`Unknown`であり、empty candidate setを`Complete` proofにしない。

Slice 4はこのaggregateを`project_invocation_owned_cleanup_candidate()`、baseline/current collector、policy query、classifier、public routeへ接続しない。causal ownership、policy protection、shared lifetime、route authority、correlation completenessは独立したままである。production candidate collectorはIssue #485 Slice 5、preview / prompt / mutation-time revalidation / removalは後続authorityのscopeである。

### Issue #485 Slice 4.5 closed producer / freshness / exhaustive completeness（candidate未接続）

cleanup correlationのinvocation authorityは、callerが文字列から再構成するidentityではなく、1つのremote AUR preparationをmove ownershipする`CleanupInvocationSession`である。session authorityはprocess内のopaque state identityであり、PID、timestamp、package名、PackageBase、root、edge、同一textから復元できない。trusted request、phase observation、correlation、aggregateは同じauthorityを保持し、別sessionの値が同一でもevidenceをreuseしない。production buildにはcallerがraw `PreparedRemoteSourceBuild`からsessionをmintするfactoryを公開せず、session test factoryはfocused targetのcompile macro内だけである。future Slice 5 collectorだけがprivate session constructionを所有する。

sessionはbaseline observationより前のtrusted transaction token登録を拒否する。selected repository providerとSourceArtifactInstallのtrusted producerだけが、fixed owner、exact transaction token、work-item集合をsession inventoryへ1回登録し、trusted transaction完了後に同じentryをcompleteへ進められる。current / policy observationはcapture時のcompleted transaction countを保持し、final inventoryと一致しないpre-transaction / mid-invocation snapshotをpost-success authorityにしない。aggregateはclosed evidenceとinventoryをtoken、owner、work itemでbijection確認し、missing、foreign、duplicate、wrong owner、previous invocation token、unused inventoryを`Incomplete`へ倒す。root helperのone-shot retirementとは独立して、copy済みin-memory evidenceのcross-invocation reuseもauthority mismatchとなる。

selected repository providerのpositive inputは、trusted executorだけがprivate constructionするtransaction-level `SelectedRepositoryProviderTrustedExecutionEvidence`である。raw receipt observation、raw ledger、`TrustedAlpmReceiptCaptureResult`、public diagnostic execution result、caller-created current evidenceはpositive correlator inputではない。1 transaction tokenは1 closed evidenceに1回だけ保持し、その内部に複数edge / work-item bindingを持つ。selected decision、edge、resolved candidate、prepared invocation、execution bindingは、repository / configured order、actual provider package、PackageBase、architecture、typed requirement、provided dependency name、raw specification、package/provided version metadata、constraint result、resolutionをfull equalityで再照合する。SourceArtifactのtoken/nameをraw selected resultへ再包装してもclosed evidenceを構成できない。

baselineは`BeforeFirstDependencyMutation`、current installed snapshotとpolicy queryは`AfterFullSupportedInvocationSuccess`へfixedし、same sessionへbindする。current installed metadataはlibalpm local package objectからactual name、version、PackageBase、architecture、reasonを同じread phaseで保持する。PackageBase / architectureの`Known` / `Missing` / `Malformed` / `Unavailable` / `Unknown`を区別し、candidate source、archive、package名から補完しない。repository providerのexpected architectureはstrict configured sync libalpm observationからlosslessに保持し、`any`を含むexact literal matchだけを使う。独自architecture compatibility solverは追加しない。

remote aggregateは全BuildPlan dependency edgeをexactly one回、`SupportedOwnerSpecificReceipt`、`AuthoritativelyPreExistingOrIrrelevant`、`UnsupportedOrUnowned`、`InvalidOrUnknown`へ分類する。Local、Unknown、AmbiguousProvider、invalid enum、raw/untyped、missing requirement / resolution / constraintはsilent skipしない。dependency edge集合とcleanup-relevant supported edge集合はnonemptyを要求し、missing / duplicate / extra correlationやwork-item reattributionをcurrent plan / current work-item vectorへ再照合する。empty setの`all_of`から`Complete`を作らない。shared `NoLongerRequired`もcandidateに対するexact relevant edge集合がnonemptyの場合だけであり、Root / Runtime evidenceは従来どおり`StillRequired`を優先する。

work-item outcomeはSucceeded / Failed / NotAttemptedごとのexhaustive shape validatorを通す。invalid status / staged enum / role / dependency kind / provider resolution、Succeededとfailure markerの同居、Failedのrequired failure shape不足、NotAttemptedとsuccess/failure stateの同居は`Incomplete / Unknown`である。

Slice 4.5はsession-bound factual transportとaggregateをfreezeするだけで、production candidate collector、classifier lifecycle、public `--rmdeps`、preview、prompt、confirmation、mutation直前revalidation、removalを接続しない。mutation直前のinstalled identity / reason / policy / shared state再確認はIssue #486のauthorityである。

### Issue #485 Slice 5 closed production lifecycle / candidate authority

supported remote AUR source buildは、`PreparedRemoteSourceBuild`をmove ownershipでconsumeする単一の`RemoteAurCleanupCandidateCollector`相当だけがproduction sessionをmintする。callerへraw session factory、phase observation factory、positive enum、raw `InvocationOwnedCleanupCandidate`、aggregateを渡さない。one invocation / one collector / one sessionであり、session authorityとaggregateはpublic approval tokenや後続mutation capabilityとして返さない。

collectorは最初のselected-provider / source-artifact dependency mutationより前に、session-owned pacman DB pathからbaseline local package snapshotを取得する。query failureやname / version / PackageBase / architecture / reasonの不完全さはtyped `Unknown`を維持する。baseline absenceは`NewlyObserved`に留まり、causal ownershipはowner-specific trusted actual `Install` receiptからだけ得る。

selected repository providerはidentity-bearing trusted executorへ接続し、exact invocation、work item、edge、decision、repository、actual package、PackageBase、architecture、requirement / capabilityを閉じたevidenceとしてretainする。source dependency PackageBaseは、全selected childが`Dependency` reasonかつBuild / Check roleへexact attributionされた場合だけ`SourceArtifactInstall` trusted transportを使う。trusted transportを開始できないpre-mutation compatibility caseでは既存operation transactionへfallbackできるが、そのraw fallbackをcausal ownershipへ再利用せずcleanup authorityはIncompleteとする。pacman success後のreceipt missing / malformed / consume failureでもoperation resultはSucceededのまま保持し、cleanup evidenceだけをIncompleteへ倒す。automatic rollbackは行わない。

全ordered work itemのoutcomeが成功した後だけ、same session / same DB authorityでcurrent installed snapshotとcandidateごとのprotected-policy queryを実行する。current identityはactual name、version、PackageBase、architecture、reasonを要求し、receiptやarchive contextから補完しない。policy `NotProtected`はcompleteなquery/reducer resultからだけ得る。

candidate集合は、closed SourceArtifact causal evidenceのselected dependency childと、closed selected-provider executionのactual selected packageだけから列挙する。solver-introduced `Install`はfactual setへ残すがcandidate化せず、その存在はuncorrelated evidenceとしてaggregateをIncompleteにする。snapshot diff、orphan state、全installed dependency scan、makepkg `-s/-sc` syncdepsをcandidate originにしない。

candidateごとにbaseline、current exact identity、causal ownership、policy、shared requirement、route / correlation completenessをinternal `InvocationOwnedCleanupCandidate`へ一方向にprojectし、pure classifierへ渡す。`Eligible`は`NewlyObserved`、exact current `Present` / `Dependency`、Build / Check only、InvocationOwned、NotProtected、NoLongerRequired、route / correlation / evidence Complete、full invocation successがすべて揃う場合だけである。duplicate/conflicting identity、pre-existing、Explicit、wrong version / PackageBase / architecture、query failure、Protected / Unknown policy、Root / Runtime、failed / unattempted later work、unsupported route、missing/foreign/cross-session evidence、Upgrade / reinstall / downgrade / `--needed` skipは`Eligible`にならない。

networkless disposable Arch fixtureはsynthetic remote AUR rootとMoguet-owned source dependency transactionを用い、collector session mint、actual sealed-bytes `pacman -U` Install receipt、actual post-success metadata / policy query、correlation、aggregate、projection、classifierまでを通して`Eligible`を確認する。host package databaseは変更しない。同じfixture familyのnegative matrixと既存installed receipt matrixは、各authorityを1つずつ崩した場合にnon-Eligibleであることを確認する。

Slice 5のassessmentはinternal resultであり、public output、preview、prompt、confirmation、removalへ接続しない。`pacman -R*`、`pacman -Qdt*`、broad orphan scan、automatic rollbackは追加しない。makepkg syncdeps authorityは#484 / #501、mutation直前revalidationとremovalは#486の独立scopeである。Issue #485のcandidate authority completionだけで#486全体をGOとしない。


## Compatibility

利用者向け互換性は[compatibility](../compatibility.md#compat-rmdeps)も参照。
