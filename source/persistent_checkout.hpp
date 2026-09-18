#pragma once

#include "trusted_cache.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// Existing checkoutの.gitが、redirectではないregular directoryならtrueを返す。
// Missingはfalse、unsafe descendantは例外としてconsumerの既存判断へ返す。
bool has_safe_persistent_checkout_git_directory(const ValidatedCachePath& checkout);

// Pinned-tree review does not inspect worktree artifacts. Keep the retained
// checkout identity and recursive .git safety proof as its narrow boundary.
void require_safe_persistent_checkout_git_metadata(
    const ValidatedCachePath& checkout);

// Acquisition-only bounded companion. A caller-owned checkpoint can throw
// before every directory read and metadata visit; normal review callers keep
// the original validation policy and failure taxonomy.
void require_safe_persistent_checkout_git_metadata(
    const ValidatedCachePath& checkout, const std::function<void()>& checkpoint);

// A missing-object result is authoritative only while every regular .git
// metadata file remains readable through the same descriptor-safe traversal.
void require_readable_persistent_checkout_git_metadata(
    const ValidatedCachePath& checkout);

// .git / PKGBUILD / root直下の*.installをnofollow検証する。
// 戻り値はsort済みのrelative install-script filenameで、review開始時のsnapshotとして使う。
std::vector<std::filesystem::path> require_safe_persistent_checkout_descendants(
    const ValidatedCachePath& checkout);

struct PersistentCheckoutReviewOverrides {
    bool has_attributes = false;
    bool has_grafts = false;

    bool operator==(const PersistentCheckoutReviewOverrides&) const = default;
};

// Review projection must not inherit repository-local attribute or history
// overrides that are outside the pinned commit trees.
PersistentCheckoutReviewOverrides observe_persistent_checkout_review_overrides(
    const ValidatedCachePath& checkout);

// 現在のdescendantsに加え、review開始時に存在したinstall scriptも再検証する。
void require_safe_persistent_checkout_review_targets(
    const ValidatedCachePath& checkout,
    const std::vector<std::filesystem::path>& install_scripts);

// URL normalizationを行わず、既存契約どおり周辺空白を除いたexact matchだけを許可する。
bool remote_url_matches_expected(
    const std::string& current_url, const std::string& expected_url);
