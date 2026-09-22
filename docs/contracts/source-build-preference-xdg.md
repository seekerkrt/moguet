# Source-build preference XDG authority contract

## 文書の位置づけ

この文書は、source-build preferenceをuser-owned XDG config authorityへ統一するproduction contractである。文書の規範上の正本は日本語本文であり、旧storeとのmigration手順やruntimeの実装詳細を別のauthorityとして作らない。

- Origin Issue: [#335](https://github.com/seekerkrt/moguet/issues/335)
- Related Issues: [#75](https://github.com/seekerkrt/moguet/issues/75)、[#302](https://github.com/seekerkrt/moguet/issues/302)、[#305](https://github.com/seekerkrt/moguet/issues/305)
- Related PRs: #336（#335 source-preference cutover）、#315〜#318（XDG path / directory safety）
- Update history: Issue #373で旧decision 12の本文から安定contractへ分離。
- Related upper decisions: [decision 1](../decisions.md#decision-1)、[decision 2](../decisions.md#decision-2)、[decision 4](../decisions.md#decision-4)、[decision 5](../decisions.md#decision-5)、[decision 6](../decisions.md#decision-6)、[decision 7](../decisions.md#decision-7)

## Contract本文（日本語normative source of truth）

### Canonical authorityとcontext

canonical source-preference rootは次である。

```text
${XDG_CONFIG_HOME:-$HOME/.config}/moguet/source-build.d/<package-name>
```

`add-src`、`edit-src`、`list-src`、`del-src`、`revert`と、build / upgrade側のreaderは、同じXDG authorityだけを使用する。unsetまたはemptyの`XDG_CONFIG_HOME`は`$HOME/.config`へfallbackする。明示した`XDG_CONFIG_HOME`はabsoluteかつ安全で既存のbase directoryでなければfail closedとし、そのbase自体をMoguetが作成しない。

root実行時もroot自身のXDG contextを使い、`SUDO_USER`等から別userの保存先を推測しない。package install / reinstall / uninstallはuser XDG directoryやsource preference fileを作成・削除しない。必要なmanaged directoryは、最初にstorageを必要とするadd / editが既存のXDG directory safety boundaryを通して作成する。

### Filesystem operation

read、list、build、upgradeはdirectoryを作成しない。missingなdelete / revertもdirectoryを作成しない。managed directoryはmode `0700`、entryはmode `0600`で作成する。package name validationはdirectory作成とexternal commandより前に完了する。

preference filesystem操作はdescriptor基準とし、final symlinkを拒否し、write / renameをatomicに行う。missing store / entryだけを正常なabsenceとして扱い、次はhard errorとする。

- invalid entry name
- symlink、non-regular file
- owner / mode違反
- permission、I/O、race
- identityやcontainmentを証明できないreplacement

`list-src`はsnapshot全体を検証してから出力し、不正entryがある場合にpartial listingを表示しない。edit failureで入力中の一時内容を安全に保持できる場合は、そのdiagnostic locationを示す。

同じstoreを使うMoguet process同士はdirectory descriptorのcooperative flockに従う。writerはmutation全体で`LOCK_EX`、strict single-entry readとsnapshot / list readerはread / validation全体で`LOCK_SH`を保持する。正常なreaderはwriterのinternal temporary / tombstoneを観測しない。crash後などに残ったinternal artifactはskipせずhard errorにする。

非協調same-euid processまたはrootがfinal identity checkとpathname syscallの間で行うreplacementまで完全なrace-freeとはしない。ただし、identity mismatchを観測した後は正体を証明できないnameをunlink、exchange、restoreしない。safe cleanupを証明できないartifactは保持し、typed errorで停止する。

source preferenceのfilesystem操作では`sudo`やshell command constructionを使わない。ただし`revert`後のpacman transactionは別責務であり、必要な`sudo`は維持する。

### Legacy storeとmigration

Moguetは旧system-wide storeと`/etc/moguet`をruntimeで作成・参照せず、legacy storeへのfallback、merge、自動copy / rewrite / deleteを行わない。migrationはMigration Guideに従うuserごとの手動操作だけとする。canonical entryとlegacy entryの双方を自動削除してはならない。

v2.0.1でこのauthorityへ揃えることは、v2.0.0で承認済みのuser XDG storage contractから漏れたsource-build preferenceを修正する限定的なPATCH例外である。新しいstorage migrationをPATCHへ許可する一般的precedentではない。v2.0.0 tag、GitHub Release、公開済みrelease bodyはhistorical artifactとして変更しない。

## Non-scope / implementationを固定しない範囲

- legacy storeの自動migration、dual-read / dual-write、legacy directoryの自動削除。
- source preference formatのTOML化、provider selection、interactive root discovery。
- descriptor、lock、temporary file、atomic renameの具体的なAPI実装を恒久固定すること。
- package installがuser configを生成する挙動。

## Compatibility

canonical XDG path、legacy path非参照、sudo境界、directory creation、filesystem safety、migration summaryは、[`compatibility.md`のsource-build preference authority section](../compatibility.md#compat-source-preference-xdg)を参照する。
