#pragma once

#include "dependency_plan.hpp"
#include "presentation_detail.hpp"

#include <cstddef>
#include <functional>
#include <iosfwd>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

// invocation内で確定済みだったproviderが、同じ依存の現在候補から消えた状態。
class ProviderSelectionConflict final : public std::runtime_error {
public:
    explicit ProviderSelectionConflict(std::string dependency_name);

    const std::string& dependency_name() const noexcept;

private:
    std::string dependency_name_;
};

// candidate metadataの表示はselection policyから分離し、phase-localな補助表示を
// callbackの外側で接続できるようにする。
using ProviderCandidatePresenter = std::function<void(
    std::ostream& output, std::size_t index,
    const ProvidedDependency& candidate)>;

// selection phaseごとにcandidate presenterを生成する。factory自体はselection
// sessionへstateful metadata lookupやdetail modeを所有させないための外側の接続点。
// modeはAppConfigのinvocation snapshotから渡す。
using ProviderCandidatePresenterFactory =
    std::function<ProviderCandidatePresenter(PresentationDetail)>;

// installed-state等を持たないcandidate表示を生成する。
ProviderCandidatePresenter make_default_provider_candidate_presenter(
    PresentationDetail detail = PresentationDetail::Normal);

// Normal / Detailedのcandidate line本体だけを表示する。suffixは
// presentation seamが後ろへ追加し、candidate identityへ戻さない。
void present_provider_candidate_metadata(
    std::ostream& output, std::size_t index,
    const ProvidedDependency& candidate,
    PresentationDetail detail = PresentationDetail::Detailed);

// 明示選択されたprovider identityを候補順で保持する。空集合は構築できず、
// 同一identityの重複は1件へ正規化する。metadataは選択時の候補snapshot。
class ProviderSelectionSet final {
public:
    // Copy on rvalues too, so a moved-from instance cannot become empty.
    ProviderSelectionSet(const ProviderSelectionSet&) = default;
    ProviderSelectionSet& operator=(const ProviderSelectionSet&) = default;

    static ProviderSelectionSet from_candidate_indices(
        const std::vector<ProvidedDependency>& candidates,
        const std::vector<std::size_t>& one_origin_indices);

    const std::vector<ProvidedDependency>& members() const noexcept;

private:
    explicit ProviderSelectionSet(std::vector<ProvidedDependency> members);

    std::vector<ProvidedDependency> members_;
};

// provider選択をinvocation単位で共有し、CLI入出力とplan callbackを接続する。
class ProviderSelectionSession final {
public:
    ProviderSelectionSession(
        std::istream& input, std::ostream& output,
        bool is_interactive);

    ProviderSelectionSession(const ProviderSelectionSession&) = delete;
    ProviderSelectionSession& operator=(const ProviderSelectionSession&) = delete;

    std::optional<ProvidedDependency> select_provider(
        const std::string& dependency,
        const std::vector<ProvidedDependency>& candidates);

    std::optional<ProvidedDependency> select_provider(
        const std::string& dependency,
        const std::vector<ProvidedDependency>& candidates,
        const ProviderCandidatePresenter& present_candidate);

    // Future multi-provider seam: explicit indices are projected through the
    // current candidate order, then retained for invocation-local reuse.
    ProviderSelectionSet record_provider_selection(
        const std::string& dependency,
        const std::vector<ProvidedDependency>& candidates,
        const std::vector<std::size_t>& one_origin_indices);

    // Reuse all cached identities against current candidates. A missing member
    // throws ProviderSelectionConflict instead of shrinking the selection.
    std::optional<ProviderSelectionSet> reuse_provider_selection(
        const std::string& dependency,
        const std::vector<ProvidedDependency>& candidates) const;

    bool is_interactive() const noexcept;
    // Raw dependency specifications use the same canonical package-name
    // authority as selection and cancellation storage.
    bool was_cancelled(const std::string& dependency) const;

private:
    std::istream* input_;
    std::ostream* output_;
    bool is_interactive_;
    std::map<std::string, ProviderSelectionSet> selections_;
    std::set<std::string> cancelled_dependencies_;
};

// production sessionはstdinがTTYかつ--noconfirm未指定の場合だけ入力を読む。
std::shared_ptr<ProviderSelectionSession> make_provider_selection_session(
    bool no_confirm);
