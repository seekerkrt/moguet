# Moguet validation policy

## 位置づけ

この文書は、development、logical Slice completion、PR / merge、release candidateの各段階で、
必要なvalidation、approval evidence、evidenceの再利用と無効化、reviewの終了条件を定める
policy authorityである。

C++ build / install graphは`CMakeLists.txt`と`cmake/`、C++ test registration / executionはCTest、
repository validation targetの実際のprerequisiteとrecipeは`Makefile`と`scripts/`、branch / PR /
release操作は[`DEVELOPMENT.md`](DEVELOPMENT.md)を正とする。この文書は、それらの実行段階と証拠の
十分性を所有する。記載と実装がdriftした場合は、対象を十分に見なして続行せず、両者を揃える。

目的はcoverageの削減ではない。変更が壊し得るcontractを先に特定し、そのcontractを
所有するvalidationで証明するrisk-based validationを正式運用とする。

## 前提として維持するmechanism contract

このpolicyはIssue #403の先行Sliceを再設計しない。validation selectionとevidenceの扱いは、
次が成立していることを前提とする。

- Slice 2: producer failureを後段commandのsuccessへ丸めず、`grep` status 0 / 1 / 2+、
  canonical expected status、business failure / infrastructure failureを区別し、primary failureを保持する。
- Slice 4: canonical CMake dependency graphとtarget propertyでstale binaryを防ぎ、target-specific
  compile / link profile、stub exclusion、link firewallを保つ。`release-check-exclusive`、
  `test-host-release`、standalone `release-check`のowner分離を維持する。
- Slice 3: root `VERSION`をversion authorityとし、validation側の不要なmanual synchronizationへ戻さない。
  independent expected oracle、deterministic / live分離、transport-specific fixture authorityを維持する。

## Laneとexisting target authority

| Lane / composite | Authority target | 責務 |
| --- | --- | --- |
| A: pure / unit | `test`の一部 | in-processのvalue、model、utility |
| B: focused component | `test`の一部 | component、stubbed adapter、局所contract |
| C: host/static/tool/filesystem integration | `test`の一部 | host tool、static contract、filesystem、packaging fixture |
| D: deterministic isolated full-CLI integration | `test`の一部 | isolated HOME / XDG、loopback fixture、PTY、full CLI |
| A–D full host | `test` | host regression全体 |
| G: release-only | `release-check-exclusive` | version、license、packaging metadata / payload、tracked Markdown |
| A–D + G | `test-host-release` | PR / mergeのcanonical host gate |
| standalone compatibility | `release-check` | 従来のA–D subsetの後にGを実行。full A–Dではない |
| E: offline/current Arch Docker | `test-container` | image内clean/default build、offline runtimeのA–D + G |
| F: actual provider / AUR / local | `test-container-live` | provider→AUR→localの独立containerを直列・fail-fast実行 |
| security-specific installed ALPM receipt | `test-container-receipt` | networkなしのinstalled root helper、transaction-local hook、actual isolated Install / Upgrade / failure |
| source-artifact installed receipt | `test-container-source-artifact-receipt` | production transport、write-sealed bytes、root-owned staging、fixed `pacman -U`、actual observation / causal evidenceとInstall / Upgrade / reinstall / downgrade / skip / failure |
| installed binding feasibility | `test-container-installed-binding-characterization` | networkless anonymous volume上のephemeral pacman rootでInstall / Upgrade / skip / same-version reinstallとopaque local DB record generationをcharacterizeする。production publicationへは接続しない |
| exact receipt / fresh installed binding | `test-container-exact-installed-binding` | actual Slice 4 proofとinstalled root helper、別purposeのInstall/Upgrade receipt、Post anchor、new ALPM session、raw MTREE、AT_EMPTY_PATH generation、通常userのlive mintとS5-C final proof、publicationなしをanonymous volume DBで確認。Install/Upgrade/reinstall/downgradeを実行し、host DBは共有しない |
| devel provenance store foundation | `test-xdg-generation-store` / `test-devel-build-provenance-store` | production-disconnectedなimmutable-generation/CAS機械層と、別XDG namespaceのstrict provenance codec/storeを確認する |
| trusted devel provenance publication | `test-devel-build-provenance-publication` / `test-devel-build-provenance-publication-result` | deterministic S4/S5 fixtureからschema v1 projection、historical binding、one-shot ownership、no-publication negatives、store fault mappingとlossless resultを確認する。6-C actual/container publication acceptanceとnormal routeを代替しない |
| actual devel provenance publication | `test-container-devel-publication` | S5 actual laneの別modeでproduction publisherを呼ぶ。1 fresh anonymous DB volume上でInstall/Upgrade/reinstall/downgradeを順次実行し、各S5 proof・cleanup、publication Complete、store generation 1→2→3→4、raw bytes SHA-256、schema v1/27 keys、persistent model・historyを確認。installed generationは別authorityとして記録する。S5 publication-noneとnormal routeの未接続を維持する |
| current installed observation / pure Git comparison | `test-current-installed-artifact-binding` / `test-devel-git-revision-comparison` | transactionとは独立したfresh current bindingとtyped OID比較。S5 fresh proof/network/normal assessmentはmint・接続しない |
| read-only devel assessment | `test-devel-package-assessment` | tip-only P/I/R exact後の#475 query、remote taxonomy、成功後のP/I/R one-time recheck、call counts。normal route/build/install/publicationは未接続 |
| reviewed devel execution bridge | `test-reviewed-devel-source-build-execution` | typed reviewed pinから既存S4/S5/S6、install/proof/publication/cleanupの個別結果、one-shot/lifetime。normal routeは未接続 |
| closed cleanup candidate authority | `test-container-cleanup-authority` | production collector、mutation前baseline、actual trusted dependency Install、post-success current / policy、aggregate / classifierとinstalled positive `Eligible` |

PR / mergeのcanonical host gateは`test-host-release`である。`test`と
`release-check-exclusive`を同一candidateでそれぞれ1回実行したevidenceもcoverageは同等だが、
通常は順序とownerを1つのentryで示せるcompositeを使う。

`release-check`はstandalone互換targetとして維持するが、full host A–Dのapproval evidenceではない。
`test-container`はhost A–D / Gを代替せず、`test-live-contract`はactual Fを代替しない。
`test-container-live`がPASSした場合、同じcandidateに対する3つのindividual live targetの再実行は不要である。
`test-container-receipt`はIssue #404のroot trust boundaryを変更したcandidateで必要な追加evidenceであり、
E / Fやhost gateへ読み替えない。
`test-container-source-artifact-receipt`はIssue #485の別owner boundaryを確認し、selected-provider
`test-container-receipt`を上書きまたは代替しない。
`test-container-cleanup-authority`は同じinstalled source-artifact runnerを使うが、Slice 5のclosed lifecycleを
追加で通す。source-artifact transport laneとselected-provider laneを意味上統合せず、public cleanup executionの
evidenceへ読み替えない。

通常の`make`とpackage consumerは`build/cmake-production` / `BUILD_TESTING=OFF`、developer / host /
release validationは`build/cmake-testing` / `BUILD_TESTING=ON`を使う。developerの`dev-debug` presetは
testing treeへDebugとcompile databaseを明示する。`make cmake-dev-configure`がpreset configure / generateの
exit 0を確認した後だけrepository root linkを生成し、raw preset configureはroot publicationを行わない。
package / release buildへcompile database生成を要求しない。CMake Presets非対応の3.18 runtimeではdirect
configureを使い、preset未実行をCMake project自体のfailureへ読み替えない。

ccacheとmoldにはdefault approval targetを設けない。`CCACHE`はCMakeのcompiler launcherとして
全CMake-owned compile commandへ作用し、link commandへは作用しない。`LDFLAGS=-fuse-ld=mold`は
CMakeが生成するlink commandへ外部inputとして同期される。実行時は対象target、clean / incremental
条件、実際にwrapper / linkerを消費した範囲を記録し、default compiler / linker gateの代替にしない。

## Evidenceの種類と記録

validation resultは次の3種類に分ける。

- **Development feedback**: affected / focused validation。実装中の早期検出に使うが、approval tokenではない。
- **Slice evidence**: focused superset、必要なhost regression、deterministic integrationにより、
  logical Sliceの実装完了を説明する。full gateを含まない限りmerge approvalにはならない。
- **Approval evidence**: 対象candidateと必要laneを全て覆うPASS証拠。PR / mergeと
  release candidateでは別のapproval epochとして扱う。

evidenceは少なくとも次を記録する。

- commit SHAまたは同一contentと証明できるsource tree、およびuncommitted changeの有無
- exact command、target、override、clean / incremental条件
- host / container、toolchain、network、current Arch / AUR等、結果の意味に必要な環境
- exit status、PASS / FAIL / infrastructure failureの区別、実行時刻、必要なlog path
- evidence reuse時は元のrevision、後続delta、変更されたcontract、再実行したtarget

producer failureをconsumerのsuccessで丸めたevidence、`grep` status 2+を0 matchとしたevidence、
canonicalでないstatusをexpected failureとしたevidence、infrastructure failureをbusiness resultとしたevidenceは
PASSとして扱わない。cleanup / diagnostic failureはprimary failureを上書きしない。

## Validation matrix

### 1. Development feedback

対象はimplementation loop、local debugging、small fix、review finding fixである。

- 変更したcontractのownerと、直接consumerを覆うaffected / focused targetを実行する。
- canonical CMake dependency graphを前提としてincremental buildを利用できる。
- static contract、failure injection、deterministic full-CLIでしか見つからないriskは、
  対応するC / D targetを追加する。
- production変更のたびにclean buildやfull suiteを無条件で実行しない。
- PASSは早期feedbackであり、focused / affected resultだけでPR / release approvalにはしない。

### 2. Slice completion

対象はlogical Sliceの完了とPR準備前のimplementation evidenceである。

- development中のfocused setを、変更したownerと直接consumerのsupersetへ広げる。
- production route、CLI、runner、filesystem / tool integrationの変更では、対応するhost regressionと
  deterministic integrationを含める。
- SliceがA–D全体のcompositionや証拠の信頼性を変えた場合は`test`、Gまたは
  merge-ready evidenceも必要なら`test-host-release`を使う。
- Docker Eとactual Fは、Dockerfile / runner / base dependency / network / transaction / fixture等の
  対応boundaryを実際に変更した場合だけ実行する。
- 同じcontractの有効なevidenceを「安心のため」だけで再実行しない。

### 3. PR / merge gate

PR / merge approvalの基本contractは、full host A–Dを1回、release-exclusive Gを1回である。

    env -u MAKEFLAGS -u MFLAGS make -j8 --output-sync=target test-host-release

`test-host-release`のPASSをcanonical host approval evidenceとする。同一candidateに対し、この後へ
`release-check`、`release-check-exclusive`、`test`を安心目的で追加しない。

focused / affected resultだけはPR approval tokenにはならない。ただし、すでに有効なA–D / G
evidenceがあり、後続deltaがそのevidenceを無効化しないとcontract単位で説明できる場合は、
影響を受けたtargetだけを再実行し、evidence chainとしてapprovalへ引き継げる。
説明できない場合はbroader gateを再実行する。

### 4. Release candidate

release candidateは新しいapproval evidence epochである。development中のfocused resultや、
過去のPR / merge evidenceをRC approval tokenとしてそのまま再利用しない。次を同じrelease
candidate revisionで実行する。

1. exact candidate commit、またはそのcommitとcontent-identicalになるcandidate treeを固定し、unrelated changeを混ぜない。
2. optional wrapper / linker overrideのないclean/default host production buildを実行する。
3. `test-host-release`でfull A–DとGを1回ずつ実行する。
4. `test-container`でoffline/current Arch Docker Eを実行する。
5. `test-container-live`でactual provider / AUR / local Fをすべて実行する。
6. Gのversion、license、packaging metadata / payload、tracked Markdownでrelease metadataの整合を確認する。
7. `sh scripts/extract-release-notes.sh`のcurrent `VERSION` sectionを確認し、release notes payloadを固定する。
8. ccache / mold parityがそのreleaseに必要な場合は、default gateの後にexact scopeを記録して追加する。

default host buildの例は次のとおり。`CCACHE`や`LDFLAGS`等の意図的なoverrideがある場合は
先に除くか、defaultでないことを明示する。

    env -u MAKEFLAGS -u MFLAGS make clean
    env -u MAKEFLAGS -u MFLAGS make -j8 --output-sync=target
    env -u MAKEFLAGS -u MFLAGS make -j8 --output-sync=target test-host-release
    env -u MAKEFLAGS -u MFLAGS make test-container
    env -u MAKEFLAGS -u MFLAGS make test-container-live

`test-host-release`内でGがPASSした後、metadataを変更していなければGを別に再実行しない。
metadataだけを後から変更した場合は`release-check-exclusive`だけを再実行できる。
Release公開時の`RELEASE_NOTES.md`からのpayload抽出と目視確認は`DEVELOPMENT.md`のrelease flowを維持する。

## Contract-based risk classification

次の表はpathで自動決定するmatrixではない。まず変更したowner / contractを特定し、
pathとtargetはその根拠として使う。

| 変更したcontract | Development / Sliceのhigh-signal evidence | 原則として無効化されるevidence |
| --- | --- | --- |
| Production C++ semantics、CLI、出力、exit status | ownerのA / B target、直接consumer、対応するDのfull-CLI scenario | A–D。payload / version / install surfaceも変わるならG / E / Fも対象 |
| CMake / CTest graph、Make frontend、dependency、compile / link profile、target composition | production / testing configure policy、affected targetのrebuild、inventory / link firewall、focused alias composition、必要ならUnix Makefiles / Ninja parity | clean/default buildとA–D。container / package consumer graphも変えたらE / package validation |
| Test runner、status helper、capture / normalization、expected status | `test-validation-status`と全ての直接consumer、必要なfault injection | helperを消費するlane。host / E / Fのうち実際に影響する範囲 |
| Fixture authority、expected oracle、transport-specific projection | `test-fixture-authority`、affected deterministic scenario、`test-live-contract` | fixtureのconsumerが属するD / E / F。他transportの独立authorityは自動で無効化しない |
| Offline Dockerfile / runner / current Arch dependency | `test-live-contract`のstatic boundaryと、semanticsを変えた場合の`test-container` | E。shared host targetも変えた場合だけA–D / G |
| Live provider / AUR / local Dockerfile、runner、gateway、transaction fixture | `test-live-contract`、affected individual live target。shared / aggregate orderの変更は`test-container-live` | affected F lane。offline Eは自動で無効化しない |
| Packaging / release metadata、root `VERSION`、license / payload authority | affected checker、通常は`release-check-exclusive` | G。build artifact / Docker payloadの意味も変わるならclean build / Eも対象 |
| Docs、gettext source / generated projectionのみ | `test-markdown-links`、`test-public-documentation`、`check-pot`、`check-catalogs`、localizationのaffected target | tracked Markdown / release metadataならG、localization ownerなら対応A–D target。production semantics evidenceは自動で無効化しない |
| Review findingのminimal fix | findingのacceptance criteriaと直接regressionを覆うaffected target | fixが変更したcontractだけ。unrelated full evidenceは引き継げる |

root `VERSION`はSlice 3で固定したversion authorityであり、generated projectionとして扱わない。
fixtureのindependent expected oracleとdeterministic / live、transport-specific authorityの分離を維持する。

## Evidence reuse / invalidation

1. 同じsource tree、command、profile、必要な環境のPASSを、同じstageで再実行しない。
2. 後続deltaがある場合は、変更pathの数ではなく、変更したcontractとconsumerで無効化を判断する。
3. 無効化されないlaneの既存PASSは引き継げる。元revisionからcurrent revisionまでの
   delta分類と、current revisionで再実行したaffected targetを一緒に記録する。
4. A–D PASSの後にG-onlyのcontractを変更した場合は、`release-check-exclusive`だけを
   再実行する。G PASSの後にA–D-onlyを変更した場合は、`test`だけを再実行する。
5. target composition、test discovery、status propagation、fixture owner等「何が実行されたか」または
   「PASSを信頼できるか」を変えた場合は、そのmechanismに依存するevidenceを無効化する。
6. current Arch、public AUR、actual transactionのF evidenceは、その時点のexternal stateを含む。
   deterministic Dやoffline Eに読み替えず、RCではactual Fを新しくevidenceとして取得する。
7. 影響を十分に分類できない、evidenceのsource treeが不明、またはlog / statusが不完全な
   場合は再利用せず、必要なbroader gateを再実行する。
8. RCは常に新しいepochとし、上記のdelta reuseをPR evidenceからRC approvalへ跨がせない。

## Review stop rule / closure rule

### Initial review

- Sliceのactual base…head diffと、変更が触れるauthority / safety boundaryを広くreviewできる。
- correctness、authority drift、safety、compatibility、regression、evidence信頼性のblockerを探す。
- blocking findingには、対象contract、根拠、再現またはcounterexample、明示的なacceptance
  criteria、必要なvalidationをできる限り付ける。

### Finding fix

- acceptance criteriaを満たすminimal fixに限定する。findingと無関係なhardeningを同じfixへ混ぜない。
- affected validationを実行する。fixが既存full evidenceのcontractを変えた場合だけ、
  対応するgateを再実行する。

### Re-review

- previous blocking findingのacceptance criteriaが閉じたか、fixが直接regressionを生んでいないかに
  scopeを限定する。
- re-reviewのたびにbase…headの無関係な一般hardening探索を再開しない。
- fixがarchitecture、authority、safety boundaryを実質的に変えた場合は、その新しい変更範囲に
  対してのinitial reviewが必要であると明示する。小さなfixを理由に全体reviewを黙ってリセットしない。

### Closure criterion

findingは、次をすべて満たしたとき`CLOSED`とする。

1. acceptance criteriaが各々PASSで説明されている。
2. affected validationがPASSしている。
3. fix起因の直接的なblocking regressionがない。
4. fixが無効化したevidenceだけが更新されている。

その時点のcontractを満たしており「さらに強くできる」内容は、non-blocking follow-upまたは
別Issueとする。すべてのblocking findingがCLOSEDで、fix起因のblockerがなければreviewを終了する。

ただし、closure確認中に実際のCritical / High correctnessまたはsafety bugが具体的根拠とともに
見つかった場合は無視しない。current Sliceのblockerか、独立Issueへ分離すべきかを明示し、
安全にclosureできない間はREADYとしない。

## Issue #403 timing outcome

以下は2026-08-10〜11のIssue #403で得た単発measurementであり、将来の絶対SLOではない。
Slice 1は84 host target、Slice 2の`test-validation-status`追加後のSlice 4は85 host targetであるため、
coverage数の差をperformance gainとして数えない。

| Evidence | Slice 1 baseline | Slice 4 result | 意味 |
| --- | ---: | ---: | --- |
| host A–D + G | `test` 41.75 s + `release-check` 39.42 s = 81.17 s | `test-host-release` 41.38 s | Gとhost prerequisiteの重複を1回化 |
| G-only | 4 checker合計1.63 s | `release-check-exclusive` 1.81 s | coverage同等、差は単発変動 |
| Docker E | 587.38 s | 457.12 s | runtime production rebuildとhost / release重複を削減 |

hostは約39.79秒、Docker Eは約130.26秒の重複costを削減した。coverage削減、lane統合、
actual Fのdeterministicへの読み替えは行っていない。
