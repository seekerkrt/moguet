# Trusted Git remote revision observer contract

## 文書の位置づけ

この文書は、authority-approvedなGit source identityを入力として、remote revisionだけを
read-onlyに観測するinternal observer foundationのnormative production contractである。
規範上の正本は日本語本文である。

- Origin Issue: [#475](https://github.com/seekerkrt/moguet/issues/475)
- Split from: [#270](https://github.com/seekerkrt/moguet/issues/270)
- Reused foundations: [#355](https://github.com/seekerkrt/moguet/issues/355)、[#411](https://github.com/seekerkrt/moguet/issues/411)
- Follow-up authority / comparison owner: [#476](https://github.com/seekerkrt/moguet/issues/476)
- Related contracts: [source-aware package identity](source-package-identity.md)、[reviewed AUR source state](reviewed-source-state.md)
- Related upper decisions: [decision 1](../DECISIONS.md#decision-1)、[decision 2](../DECISIONS.md#decision-2)、[decision 3](../DECISIONS.md#decision-3)、[decision 4](../DECISIONS.md#decision-4)、[decision 5](../DECISIONS.md#decision-5)、[decision 6](../DECISIONS.md#decision-6)、[decision 7](../DECISIONS.md#decision-7)

このcontractが完成させるのはobserver foundationだけである。

```text
implemented by #475:
  sealed validated request
  fixed trusted HTTPS Git execution
  bounded process lifecycle
  strict remote transcript parser
  typed observation result

NOT implemented by #475:
  authoritative upstream source metadata producer
  production observer caller
  installed/build provenance
  remote-to-installed comparison
  UpdateAvailable / UpToDate
  AUR update connection
```

従って、#475の完成を「VCS/devel update tracking完成」と表現しない。current production buildには
observer implementationが含まれる。#476 Slice 7-Bの[read-only assessment coordinator](devel-package-assessment.md)だけが
P/I/R local gates後にauthority-approved requestを生成し、このobserverを呼ぶ。
normal CLI/AUR update routeは未接続であり、raw metadataをnetwork authorityへ昇格させない。

## Purpose

observerは、前段の別authorityが承認済みのGit sourceについて、working tree、repository、
Moguetのconfig / state / cacheを変更せず、指定されたremote refのcomplete object IDだけを
観測する。Git transport、refname grammar、remote advertisementはGitへ委ね、Moguetは次を所有する。

- networkへ渡せるrequestのsealed authority boundary
- HTTPS remoteのbounded validationとcanonical representation
- `DefaultHead | ExactBranch`だけのclosed selector
- shellを通さないfixed Git commandとcomplete environment
- timeout、capture、child tree cleanupのresource boundary
- stdout全体を対象とするstrict grammar
- observation、absence、process failure、Git exit、malformed / ambiguous outputの分離

observerはmetadata trust、installed baseline、provenance、update decision、build、install、transactionを
所有しない。

## Authority boundary

### Normative flow

```text
raw source syntax / metadata
  ParsedSourceEntry
  ParsedSrcinfoSourceMetadata
        │
        │ direct promotion forbidden
        ▼
future reviewed/evaluated source authority (#476)
        ↓
AuthorityApprovedGitSourceIdentity
        ↓
ValidatedHttpsGitRemote
+
ValidatedGitRemoteSelector
        ↓
ValidatedGitRemoteRevisionRequest
        ↓
Git remote revision observer
        ↓
GitRemoteRevisionObservationResult
```

`AuthorityApprovedGitSourceIdentity`は、前段ownerが一つのeffective Git sourceをremote observationへ
使用してよいと承認済みであることを表すsealed capabilityである。#475にはこのcapabilityの
production factory / friend producerがなく、test-only fixtureだけがcompile definitionで隔離される。

次の型や値は、それ自体ではnetwork authorityではない。

- `ParsedSourceEntry`: makepkg source syntaxのparse結果であり、transport policy、trust、network accessを承認しない。
- `ParsedSrcinfoSourceMetadata`: untrusted `.SRCINFO` bytesのpure parse結果である。
- bare `VcsSourceIdentity`: VCS kind、broad location token、parsed selector roleを保持するが、HTTPS canonicalizationやGit refname validationを証明しない。
- `TrustedDevelSourceMetadata`: classification inputであり、public factoryがbare `VcsSourceIdentity`を受ける。名称の`Trusted`だけではprovenanceやnetwork authorityにならない。
- `SourceAwarePackageIdentity`: generic identity foundationであり、authority-approved upstream source connectionではない。current generic repository / AUR projectionのrevision `Unknown`をobserved revisionへ昇格させない。
- `AurRecipeRevision`、reviewed source state、accepted AUR recipe commit: AUR recipe authorityであり、PKGBUILD内のupstream Git revision authorityではない。

requestは上記raw / generic型からconstructibleでもconvertibleでもない。#476がreview済み / 評価済みの
effective source metadata producerを追加するまで、このfirewallをconvenience adapterで迂回しない。

## Supported subset

current production subsetは次へ閉じる。

| Dimension | Supported value |
| --- | --- |
| VCS | Git only |
| Transport | HTTPS only |
| Selector | `DefaultHead`、`ExactBranch` |
| Object ID | canonical lowercase SHA-1 40 hex、canonical lowercase SHA-256 64 hex |

supportは「requestとobserverがこのsubsetを正しく表現・実行できる」という意味である。
production authority producerやpublic update workflowへの接続を意味しない。

## Request model

| Type | Contract |
| --- | --- |
| `AuthorityApprovedGitSourceIdentity` | separate authority ownerが承認済みのGit source capability。#475 production producerなし |
| `ValidatedHttpsGitRemote` | libcurl-backed canonical HTTPS URLだけを保持。raw spellingをnetwork sidecarとして保持しない |
| `ValidatedExactGitBranch` | fixed `/usr/bin/git check-ref-format --branch`が受理し、stdoutもexact一致したbranch |
| `ValidatedGitRemoteSelector` | payloadなし`DefaultHead`、または`ValidatedExactGitBranch`を持つ`ExactBranch` |
| `GitRemoteRevisionObservationKey` | canonical HTTPS remote + validated selectorのstructural value |
| `ValidatedGitRemoteRevisionRequest` | authority-approved source + 同じremote / selectorを持つobservation key |

request factoryはapproved sourceとkeyを再照合する。source kindはGit、source locationを同じHTTPS
canonicalizationへ通した値はkey remoteと一致し、source selectorはkeyの`DefaultHead`またはexact
branch bytesと一致しなければならない。tag、fixed revision、unsupported / unrecognized selectorは
requestにならない。

successはfull requestとsource-bound `UpstreamGitRevision`を同じ`ObservedGitRemoteRevision`へ保持する。
OID textだけをsource identityから切り離したsuccessとして公開しない。

## HTTPS remote validation

`ValidatedHttpsGitRemote::make`はnetwork authorityを生成するfactoryではなく、sealed requestを構成する
一要素のtransport validatorである。current contractは次のとおりである。

- inputは8192 bytes以下。8192 bytesは受理し、8193 bytes以上を拒否する。
- raw spellingにcase-insensitiveなexplicit `https://` authorityを要求する。libcurlがrepairし得る
  `https:///host`を受理しない。
- hostは必須。
- user、password、URL optionsを拒否する。
- literal `?` / `#`を、empty query / fragmentを含めてparse前に拒否し、URL partとしても再確認する。
- embedded NUL、C0、DEL、ASCII whitespaceを拒否する。
- HTTPS以外のscheme、scp-like syntax、local pathを拒否する。
- canonical URLだけを保持し、callerのraw spellingをGit argvへ戻さない。
- explicit default port `:443`を除去し、non-default portを保持する。
- IPv6 literalを受理してcanonical lowercaseを保持し、IPv6 zone IDを拒否する。
- raw Unicode IDN hostはunsupported / fail-closedである。ASCII punycode spellingは受理し、
  ASCII lowercaseへcanonicalizeする。
- 独自RFC URL parser / IDN normalizerを実装せず、libcurl URL APIを利用する。

current libcurl 8.21.0でfreezeしたcanonicalizationは次のとおりである。このversionはproject minimumを
定めるものではなく、library upgrade時にfixture failureをexpected値の機械的更新で処理しない。

| Input characteristic | Canonical behavior |
| --- | --- |
| scheme / DNS host case | lowercase |
| `HTTPS://EXAMPLE.COM:443/a/../repo.git` | `https://example.com/repo.git` |
| `/a/./repo.git` | `/a/repo.git` |
| `/%2e/repo.git` | `/repo.git` |
| `/%2E%2E/a/../repo.git` | `/repo.git` |
| ordinary `/%72epo.git` | percent spellingをheuristic decodeせず保持 |
| empty path、`/` | `https://host/`へ正規化 |

## Selector model

selectorは次の2 armだけである。

```text
DefaultHead
ExactBranch(ValidatedExactGitBranch)
```

`DefaultHead`をempty stringやnullable branchで表さない。tag、annotated tag、peeling、fixed commit、
revision queryをselector armへ追加しない。

Exact branch inputは4096 bytes以下で、emptyとembedded NULをresource preflightで拒否する。
MoguetはGit refname grammarを再実装しない。次のfixed processへ判定を委ねる。

```text
/usr/bin/git
  <observer fixed global profile>
  check-ref-format
  --branch
  <input as one exact argv element>
```

validatorもshellを通さず、complete trusted envp、cwd `/`、stdin `/dev/null`、stderr `/dev/null`、
30 s hard timeout、500 ms termination graceを使う。status 0でもstdoutがexactly次でなければsuccessにしない。

```text
<input exact bytes><LF>
```

different spelling、extra line、CRLF、control byte、partial final lineを拒否する。Git normal nonzeroは
stderr textを読まず`InvalidExactGitBranch`とし、launch / setup / I/O / signal / timeout / captureは
validation process failureとして区別する。validator stdout captureは4096-byte input + final LFの
4097 bytesへ閉じる。

## Observation key and dedup ownership

`GitRemoteRevisionObservationKey`は次だけから成る。

```text
ValidatedHttpsGitRemote
+
ValidatedGitRemoteSelector
```

source capability、target package、TTL、cache state、scheduler stateはkeyへ含めない。#475 observerは
global / static / persistent cache、batch API、TTL、same-invocation dedup、concurrency scheduler、aggregate
network budgetを所有しない。

- per-observation timeout / capture policy: #475 observer owner
- same-invocation dedup / aggregate invocation budget / target attribution: future orchestrator / #476 owner

## Process execution boundary

observerが利用するbounded processのsecurity contractは次である。これはgeneric process module全体の
内部設計を恒久固定するものではないが、同等以下へ弱めてはならない。

- executableはproductionでfixed `/usr/bin/git`。`PATH` lookupやcaller overrideを使わない。
- shell、`sh -c`、display commandの再実行を使わず`execve`へargv / complete envpを渡す。
- cwdとstdinをdescriptorで明示し、observerはtrusted `/` directoryとO_RDONLY `/dev/null`を使う。
- stdout pipeのparent read endをnonblockingにし、activityで延長されないmonotonic absolute deadlineを使う。
- childをdedicated process groupへ置き、parent / child双方の`setpgid` raceを処理する。
- timeout、capture overflow、lingering same-group descendantをprocess-group `SIGTERM`、bounded grace、
  必要時`SIGKILL`でcleanupする。
- root childを必ずwait / reapし、subreaperへadoptされたsame-group descendantをreapし、group absenceを
  return前に確認する。
- childへparent-death `SIGKILL`を設定し、parent PID raceを確認する。
- exec-status CLOEXEC pipeでchild setup / `execve` failureとactual executable exit 127を分ける。
- stdout capture limitを超えた最初のbyteでcaptureを閉じ、EOFまでunbounded drainしない。
- stdioとexec-status descriptor以外の不要なfdをexec前に閉じる。
- callerで元からblockされていないSIGINT / SIGQUIT / SIGHUP / SIGTERMをdedicated child groupへforwardする。
- observer stderrは`/dev/null`へ送り、classification inputとしてcaptureしない。

cleanup infrastructure自身が失敗した場合は、timeout / overflowを偽装せずtyped `ProcessFailure`を保持し得る。
fixed `/usr/bin/git`と通常のsame-group Git descendantがthreat modelであり、意図的に`setsid()`してgroupを
escapeするarbitrary malicious executableの回収はcontract外である。production executable overrideはない。

## Resource limits

current production policyは次である。

| Resource | Limit | Owner |
| --- | ---: | --- |
| one observer hard timeout | 30 s | #475 observer |
| process-group SIGTERM grace | 500 ms | #475 observer |
| observer stdout capture | 16 KiB | #475 observer |
| HTTPS URL input | 8 KiB / 8192 bytes | `ValidatedHttpsGitRemote` |
| exact branch input | 4 KiB / 4096 bytes | exact branch validator |
| exact branch validation stdout | 4097 bytes | exact branch validator |

これらはcaller parameterやuser configで弱められない。test-only compile definitionはprocess mechanicsを
検証するためdeadline / executable / argvだけを限定的に差し替え、trusted env / cwd / stdin / stderrと
production 16 KiB observer captureを維持する。aggregate invocation budgetは#475へ追加しない。

## Git environment, config, and protocol isolation

### Complete environment baseline

observer childはparent environmentへの追加ではなく、次のcomplete envpを受ける。

```text
PATH=/usr/bin:/bin
LC_ALL=C
LANG=C
GIT_CONFIG_NOSYSTEM=1
GIT_CONFIG_SYSTEM=/dev/null
GIT_CONFIG_GLOBAL=/dev/null
GIT_TERMINAL_PROMPT=0
GIT_ASKPASS=/bin/false
SSH_ASKPASS=/bin/false
GIT_PAGER=cat
PAGER=cat
GIT_ATTR_NOSYSTEM=1
GIT_OPTIONAL_LOCKS=0
```

これにcurrent proxy 8変数とcustom CA 4変数のallowlistだけを追加できる。少なくとも次のambient
stateをauthorityとして継承しない。

```text
HOME
XDG_*
GIT_DIR
GIT_WORK_TREE
GIT_CONFIG*
GIT_EXEC_PATH
GIT_SSH*
GIT_TRACE*
GIT_PROXY_COMMAND
GIT_SSL_NO_VERIFY
CURL_CA_BUNDLE
LD_PRELOAD
LD_LIBRARY_PATH
BASH_ENV / ENV / SHELL and shell startup state
unknown parent variables
```

complete envpで閉じるため、個別denylistの網羅性をsecurity authorityにしない。

### Proxy / CA routing exception

credential isolationとnetwork routing policyを混同しない。current proxy allowlistは次のexact 8変数である。

```text
http_proxy
https_proxy
all_proxy
no_proxy
HTTP_PROXY
HTTPS_PROXY
ALL_PROXY
NO_PROXY
```

proxy valueはexisting trusted routing policy上opaqueであり、proxy URL内にproxy credentialを含み得る。
これはremote Git credential helper、HOME credential state、askpass、interactive promptを許可するものではない。

custom CA allowlistは次のexact 4変数である。

```text
SSL_CERT_FILE
SSL_CERT_DIR
GIT_SSL_CAINFO
GIT_SSL_CAPATH
```

empty CA valueはomitしてdefault trust storeを維持する。`SSL_CERT_DIR`はcolon-separatedな全component、
その他3変数はvalue全体がabsolute pathでなければならない。invalid valueをdiagnosticへ含めない。
`CURL_CA_BUNDLE`はallowlistに含めない。`http.proxy`や`http.sslCAInfo`をempty configで上書きせず、
allowlisted routingとdefault trust storeを維持する。

### Fixed Git profile

current observer global profileは次である。

```text
--no-pager
--git-dir=/dev/null

-c core.hooksPath=/dev/null
-c core.fsmonitor=false
-c core.sshCommand=/bin/false

-c credential.helper=
-c credential.interactive=false
-c credential.username=
-c core.askPass=/bin/false

-c http.emptyAuth=false
-c http.proactiveAuth=none
-c http.delegation=none
-c http.extraHeader=
-c http.cookieFile=
-c http.saveCookies=false
-c http.followRedirects=false
-c http.sslVerify=true

-c protocol.allow=never
-c protocol.https.allow=always
-c protocol.http.allow=never
-c protocol.file.allow=never
-c protocol.ext.allow=never
-c protocol.ssh.allow=never
-c protocol.git.allow=never

-c submodule.recurse=false
```

`--git-dir=/dev/null`はcwd repositoryのlocal config discoveryを遮断する。credential helper、interactive
auth、askpass、ambient username / header / cookieを無効化し、redirectを拒否してTLS verificationを
有効にする。protocol defaultはdeny、HTTPSだけをallowし、HTTP / file / ext / SSH / `git://`を
explicit denyする。unspecified protocolもdenyである。

上記のsecurity invariantはnormativeである。argvの内部生成方法やhelper function名は恒久APIではないが、
profile項目を削除・緩和する変更はcontract changeとしてreview / regression evidenceを必要とする。

## Command shapes

### Default HEAD

```text
/usr/bin/git
  <observer fixed global profile>
  ls-remote
  --quiet
  --symref
  --exit-code
  <canonical HTTPS remote>
  HEAD
```

accepted status 0 transcriptは次の2形式だけである。

symbolic HEAD:

```text
ref: refs/heads/<nonempty target><TAB>HEAD<LF>
<canonical oid><TAB>HEAD<LF>
```

direct / detached HEAD:

```text
<canonical oid><TAB>HEAD<LF>
```

symbolic targetは`refs/heads/` prefix、nonempty tail、ASCII whitespaceなしを要求し、success payloadへ
保存しない。unborn / missing HEADはexit 2 + stdout exactly emptyだけを`RefNotFound`とする。

### Exact branch

```text
/usr/bin/git
  <observer fixed global profile>
  ls-remote
  --quiet
  --refs
  --branches
  --exit-code
  <canonical HTTPS remote>
  refs/heads/<validated exact branch>
```

accepted status 0 transcriptはexactly 1 recordである。

```text
<canonical oid><TAB>refs/heads/<validated exact branch><LF>
```

`git ls-remote`のpattern matchingはexact matchではない。`--branches`は別namespaceを減らすが、branch
namespace内のtail matchまでauthorityとして除去しない。parserがexpected refとのbyte equalityを
最終authorityとする。command optionだけ、first match、first line、last lineの採用でexactnessを
代替しない。

## Strict output grammar

normal exitのstatusとbounded stdout全体だけをpure parserへ渡す。stderr parameterは存在しない。

| Status / stdout | Result |
| --- | --- |
| status 0 + accepted exact grammar | `Observed` |
| status 0 + exactly empty | `MalformedOutput` |
| status 2 + exactly empty | `RefNotFound` |
| status 2 + nonempty | `MalformedOutput` |
| other normal nonzero | stdoutをparseせず`GitExitFailure` |

status 0 transcriptはさらに次を満たす。

- final LF必須。partial final lineを拒否する。
- CRLFを拒否する。
- delimiterとして期待するTAB / LF以外のC0、DEL、NULを拒否する。
- recordはexactly 1 TABを持つ。
- expected refをbyte-exactに照合し、wrong refを拒否する。
- OIDはcomplete canonical lowercase SHA-1 40 hexまたはSHA-256 64 hexだけを受理する。
- uppercase、abbreviated、wrong-width、non-hex OIDを拒否する。
- expected OID recordのduplicateを`AmbiguousOutput`とする。
- expected OIDとextra well-formed OID recordの併存を`AmbiguousOutput`とする。
- wrong refだけ、unexpected symref、bad delimiter / OID / orderを`MalformedOutput`とする。
- extra / duplicateが同じOIDでもsilent successにしない。

raw stdout bytesをresultやdiagnosticへ保持しない。

## Failure model

`GitRemoteRevisionObservationResult`は次の8 top-level armだけを持つ。

| Result arm | Meaning |
| --- | --- |
| `Observed` | source-bound complete upstream Git revisionを観測した |
| `RefNotFound` | exit 2 + exactly-empty stdoutでselected ref不在を観測した |
| `Timeout` | bounded process hard deadlineに到達しcleanupした |
| `ProcessFailure` | signal、launch / setup、I/O / wait / cleanupのmechanical failure |
| `GitExitFailure` | Gitが2以外のnormal nonzeroで終了した。exit codeを保持する |
| `CaptureLimitExceeded` | stdoutが16 KiBを超え、child treeを停止した |
| `MalformedOutput` | accepted grammarを満たさない非ambiguous transcript |
| `AmbiguousOutput` | duplicate expected OIDまたはexpected + extra OID record |

`TransportFailure` armは存在しない。Git exit statusとstderr wordingだけからTLS、DNS、authentication、
protocol denial、remote failure、Git internal / usage failureをlosslessに区別できないためである。
`GitExitFailure`をroot-cause transport証明として表示せず、stderr textをcontrol-flow authorityにしない。

## Mutation-free contract

observerは少なくとも次をcreate、write、rewrite、remove、checkout、fetch、resetしない。

```text
HOME
XDG config / state / cache / data
current working-directory repository
refs
objects
index
tracked / untracked worktree
Git config
FETCH_HEAD
packed-refs
Git logs / lock files
credential-helper / askpass markers
cookie target
trace target
```

この保証を「`ls-remote`だからread-only」という推測だけに置かない。complete environment、
`--git-dir=/dev/null`、credential / cookie / trace isolation、protocol profile、bounded child cleanupと、
inotify + before/after structural/hash/semantic sentinelを組み合わせる。

approval fixtureは各caseを独立sentinel下で実行し、observer return後も250 ms監視する。次のterminal
outcomeでfilesystem snapshot、inotify event、cwd repository status / HEAD / refs / index / objectsがcleanである。

```text
success
RefNotFound
GitExitFailure
Timeout
CaptureLimitExceeded
MalformedOutput
AmbiguousOutput
401 credential failure
redirect failure
```

401ではcredential helper / `GIT_ASKPASS` / `SSH_ASKPASS` markerが作られず、redirect targetへ接続せず、
ambient Authorization header / cookieを送信せず、cookie / trace targetを変更しない。bounded processの
root / group / descendant absence testとpost-return sentinelの両方でdelayed mutation absenceを検証する。

## SHA-1 / SHA-256 behavior

parserはGit version stringからobject formatを推測しない。OID bytesをexisting
`SourceRevisionIdentity::git_commit`へ渡し、40 lowercase hexをSHA-1、64 lowercase hexをSHA-256として
typedに保持する。それ以外をknown revisionにしない。

actual observationにはruntime Gitとremote側のobject-format capabilityも必要である。current Arch hostと
offline/current Arch Docker `--network=none` laneでは、`git init --object-format=sha256`で実repositoryを
作り、loopback HTTPSからproduction observer APIが64-hex OIDを取得するend-to-end fixtureがPASSしている。
SHA-256をGit version表示だけから推測せず、unsupported environmentではapproval fixtureをsilent skipせず
明示failureとする。

このcontractはGit 2.55.0をproject minimumに固定しない。package/runtime authorityは引き続きArchの
runtime `git` dependencyであり、environment capabilityはactual fixtureで判定する。older / different Gitで
SHA-256 remote observationが成立しない場合、64-hex parser supportをそのenvironmentのsuccessful
end-to-end supportと読み替えず、typed Git / process failureまたはvalidation failureとして扱う。

## Dedup ownership

observerは一つのvalidated requestを一回観測するadapterである。observation keyはfuture orchestratorが
same-invocation dedupに利用できるが、#475は次を追加しない。

- global / static cache
- persistent cache / state directory
- TTL / invalidation policy
- batch scheduler / concurrency policy
- aggregate network deadline
- package target attribution

これらはreviewed/evaluated source authorityとprovenance-bound comparisonを所有する#476側で裁定する。

## Relationship to #270 / #355 / #411 / #476

- **#270**: v2.5.0の完成点はconventional suffix evidenceをfalse `UpToDate`へ丸めず
  `RequiresCheck`にするconservative connectionである。#475 observerをv2.5.0 update workflowへ
  retroactively接続しない。
- **#355**: `SourceRevisionIdentity`のcomplete SHA-1 / SHA-256 validation等、low-level valueを再利用する。
  generic source-aware projectionは引き続きrevision `Unknown`であり、observer resultを暗黙注入しない。
- **#411**: reviewed AUR recipe revision、explicit acceptance、pinned build authorityを維持する。ただし
  reviewed recipe commitはupstream source commitではなく、review済みeffective source metadata producerも
  #411には存在しない。
- **#476**: reviewed/evaluated source metadataから`AuthorityApprovedGitSourceIdentity`を作るproduction
  producer、installed/build provenance、same-invocation orchestration、remote comparison、
  `UpdateAvailable` / `UpToDate`、AUR update connectionを所有するfuture follow-upである。

## Validation evidence

deterministic validationは責務を一つのfixtureへ混ぜず、次の層で構成する。

1. pure transcript / value tests
   - HTTPS canonicalization、authority firewall、selector / key / request、40 / 64 OID、status mapping、
     malformed / ambiguous / wrong / duplicate / control / partial transcript
2. bounded process tests
   - normal / signal / actual exit 127 / exec failure、absolute timeout、TERM -> KILL、overflow、held-open
     stdout descendant、fd hygiene、stdin EOF、signal forwarding、root / group / waitable child absence
3. loopback HTTPS + mutation integration
   - actual fixed Git、generated local CA、SHA-1 / SHA-256、symbolic / direct / unborn HEAD、exact branch、
     branch advance、missing ref、401、redirect、malicious HOME / XDG / local config、sentinel

current focused entryは`test-git-remote-revision-observer`でunit/compositionとloopback HTTPS integrationの
両方を実行し、bounded mechanicsは`test-bounded-process`でも独立検証する。fixtureはpublic Internetを
使わず、HTTPS serverはephemeral `127.0.0.1` port、runtime containerは`--network=none`である。

## Explicit non-scope

次はこのcontractが実装済みとする範囲に含めない。

- #476 production authority producer
- reviewed `.SRCINFO` acquisition、raw metadataからのauthority-approved connection
- `makepkg --printsrcinfo` evaluation authority
- `aur_update_plan` / AUR update assessment connection
- installed baseline、actual build source proof、build / installed artifact provenance
- provenance store、schema、migration
- `UpdateAvailable` / `UpToDate`
- persistent cache、same-invocation dedup scheduler、aggregate invocation budget
- public CLI、user-facing observer command / output / exit code
- automatic rebuild、fetch / build / install lifecycle、package transaction
- HTTP、SSH、file、local path、`git://`、ext transport
- tag、annotated tag、peeling、fixed commit、arbitrary revision query
- Git以外のVCS
- release operation、branch merge、Issue close

## Compatibility

このfoundation自体はcurrent CLI、AUR update result、output、exit status、config / state / cache layout、
build / install / transactionを変更しない。利用者向けの対応subsetと未接続境界は
[`COMPATIBILITY.md`のobserver foundation section](../COMPATIBILITY.md#compat-git-remote-revision-observer)
を参照する。
