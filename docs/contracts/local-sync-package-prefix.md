# Local sync package prefix completion (Issue #253 Slice 1a / 1b)

このcontractはprivate local sync providerと、generated Bash / Zsh / Fishの限定的な接続を定める。
public CLIや`moguet __complete`は追加しない。

## Shared CLI authority

`cli_authority.hpp`の`DELEGATED_OPERATION_EXAMPLES`は既存public exampleのexact tokenと
既存`OperandKind`を対応させる。plain `-S`はPackage、`-Ss`はQueryである。
`-S --select`は既存`SpecialOperationSpec::SyncSelect`のQueryのまま。
runtime contractの`delegated_example`は情報projectionで、validation/dispatchのopen grammarを閉じない。
unknown operation modifier、unknown pacman tail option、`--`ではexact exampleを返さない。
既存authorityを持つ`--needed`だけはtailの例外として認識する。

parserの既存17 value-taking tokenは`PACMAN_VALUE_OPTIONS`へ移した。parser/exporterが共用する。
これはlexical arityであり、route許可、forwarding/effect、suggestion visibilityを定めない。
inline `=value`はfollowing valueを待たず、pending valueはmarkerより先に消費する。
`END_OF_OPTIONS_TOKEN`とhidden marker OptionContractは同じtokenを使用する。
exporterの`OPERAND_CONTEXT` / `LEXICAL_VALUE` / `BOUNDARY`をgeneratorが検証・保持する。
closed formのoperand/selectorは既存FORMを使う。

integrationはpending value、marker後、unknown modifier/tail、Query、`--aur`をproviderへ送らない。
`LEXICAL_VALUE`のalternate-db印は`--config` / `--dbpath` / `--root` / `--sysroot` / `-b` / `-r`を
認識するためだけに使う。これらが現れるinvocationではproviderを抑制し、別DB semanticsを推測しない。

## Metadata API

`RepositoryPackageMetadataSession::query_package_name_prefix(prefix, count, bytes)`はconfigured sync DBの
既存libalpm cacheを読む。default pacman configurationのRootDir/DBPath/repository listをpacman-confへ委ねる。
prefixはliteralな不完全文字列であり、complete package-name validatorへ通さない。
全cache名を既存validatorで検証し、bytewise昇順・dedupした名前だけを返す。
countとnewline込みbytes上限はlexicographic先頭から切り、`truncated`を返す。
同名が複数repoにある場合も1候補。bare nameからsource/AUR identityやinstall可能性を推論しない。
UsageによるInstall/Search可否のfilterは行わない。このAPIはlocal configured inventoryの存在だけを表す。
regex/description/group search、AUR、network refresh、transaction、sudo、build、config/state writeは行わない。

成功0件とfailureはvariantで区別する。session open時のmissing/corrupt DBは既存typed errorを保持する。
不正なmetadata、closed session、invalid limitはfailureであり、空の正常inventoryへ変換しない。

## Private transport

既存`MOGUET_FULL_INTERNAL_EXECUTABLE_DIRECTORY`配下へ以下をinstallする。

- `moguet-repository-prefix-helper PREFIX`: shellが後で呼ぶ唯一のsupervised入口。
- `moguet-repository-prefix-worker PREFIX`: helper専用のlocal metadata worker。

helperは`/proc/self/exe`の実directoryから固定sibling workerを起動する。
PATH、Moguet user config、通常main startupには依存しない。workerを別processにするのは、libalpmのblocking
read全体を既存bounded process utilityで監督するためであり、generic provider frameworkではない。
通常Moguet/mainのobject graphやAUR/transaction/config/state moduleをlinkしない。
既存process utilityがloggingを参照するためlogging objectは必要だが、Loggerを初期化せず、file/state logは作らない。
診断先のみstderrへ設定する。

stdoutはvalidated package name + newlineだけ。exit 0には0件とdeterministic truncationを含む。
failure/budget超過はexit 1、argument数/prefix byte上限違反はexit 2。helperはworkerの成功・protocol検証が
完了してからstdoutへpublishし、failure時にはpartial candidatesを返さない。stderrはmachine dataではない。
workerは単独利用のcompletion入口ではなく、whole-query timeoutはhelperの責務である。

## Enforced budgets and limits

- Prefix: 256 bytes以内。emptyやregex特殊文字もliteral inputとして受け付ける。
- Candidate: 最大256件。
- Candidate output / supervisor capture: newline込み64 KiB以内。
- pacman-conf: 2回、各stdout captureを16 KiBへ制限。overflow/nonzeroはconfiguration failure。
- Worker tree: monotonic 500 ms deadline。SIGTERM後50 ms grace、必要ならgroupへSIGKILL。
  configuration subprocess、libalpm open/cache load、prefix scanを同じworker groupへ含む。
  pacman-conf captureはshell-freeで、独自groupへ逃がさない。

これはwhole workerに対するdeadline enforcementであり、通常metadata API単独のlatency保証ではない。
process utilityはexit/reapとgroup消滅を待つ。scheduler delay、kernelのuninterruptible I/O、意図的setsid escape、
helper起動前のloader、最終stdoutの消費側backpressureについて絶対的なwall-clock return上限は保証しない。
500 ms + 50 msを全状況でのreturn SLAと読み替えない。固定の通常pacman-conf/libalpmとstdoutを読み取るcallerが契約対象。
cache warmthを制御しないfirst/repeated latency観測はmeasurementでありcold-cache guaranteeではない。
persistent cache/daemonを作らない。

## Slice 1b boundary

generatorはshared `OPERAND_CONTEXT`でPackageと確認したplain `-S`だけを、最初のbare operandの
許可contextへ投影する。completed argvはcommand名とoperationだけ、current wordはoptionでなく、
cursor後にtokenがない場合に限る。shell側はこのgenerated許可tokenとshapeを検査するだけで、
pacman grammarを推定しない。追加option、既存operand、global、pending value、`--`、unknown/open
modifier、Query、`--aur`、alternate DB/root（separate/inline/attachedを含む）は全て0 calls。
既存authorityで意味が判る`--needed`も、この最初のvisible Sliceでは許可しない。

Bash/Zshはstatic候補scanの外で1回、Fishは1つのdynamic `complete -a` producerで1回だけhelperを呼ぶ。
current wordはshell quote/escapeを除きliteral prefixとして渡す。Bashはevalせずquote syntaxを除き、
ZshはframeworkのPREFIX、Fishはraw tokenのstring unescapeを使う。候補へdescriptionを混ぜない。
成功exit、newline終端、ASCII package文字、literal prefix、sorted unique、256件/64KiBを検証してから
候補を公開する。NULやpartial/nonzero/stdout不正、missing helperはdynamic候補を全て破棄し、
static候補を保持する。stderrは常に捨てる。Fishはbytewise順序を`/usr/bin/sort -cu`で一度検査し、並べ直さない。
providerのdeadlineは既存supervisorの責務で、adapterに別のtimeout frameworkを設けない。

tracked completionはprefix-neutral置換tokenを持つ。CMakeがresolved
`MOGUET_FULL_INTERNAL_EXECUTABLE_DIRECTORY`をshell別quoteでbindしたbuild-tree版をinstallする。
helper/workerは`repository-prefix`、3 adaptersは`shell-completions` componentである。
source-treeでproviderを使う場合はgeneratorの`--repository-prefix-helper <absolute-path>`でbound版を
stdoutへ生成する。DESTDIRはstagingだけであり、installed adapterのbindingへ混ぜない。
unsupported relative -Dの挙動は再定義しない。`__complete = UNNECESSARY`を維持する。
