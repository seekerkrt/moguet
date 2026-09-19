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

// installed-state等を持たない既存のcandidate metadata表示を生成する。
ProviderCandidatePresenter make_default_provider_candidate_presenter(
    PresentationDetail detail = PresentationDetail::Normal);

// fixed metadata labelを保ったcandidate line本体だけを表示する。suffixは
// presentation seamが後ろへ追加し、candidate identityへ戻さない。
void present_provider_candidate_metadata(
    std::ostream& output, std::size_t index,
    const ProvidedDependency& candidate);

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

    bool is_interactive() const noexcept;
    // Raw dependency specifications use the same canonical package-name
    // authority as selection and cancellation storage.
    bool was_cancelled(const std::string& dependency) const;

private:
    std::istream* input_;
    std::ostream* output_;
    bool is_interactive_;
    std::map<std::string, ProvidedDependency> selections_;
    std::set<std::string> cancelled_dependencies_;
};

// production sessionはstdinがTTYかつ--noconfirm未指定の場合だけ入力を読む。
std::shared_ptr<ProviderSelectionSession> make_provider_selection_session(
    bool no_confirm);
