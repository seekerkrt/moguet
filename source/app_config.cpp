#include "app_config.hpp"

#include <cstdlib>
#include <string>
#include <utility>

namespace {

// POLICY(#65): editor selection stays environment-owned and is snapshotted
// once with the other invocation settings.
std::string resolve_editor_from_environment() {
    const char* visual = std::getenv("VISUAL");
    if(visual != nullptr && visual[0] != '\0') return visual;

    const char* editor = std::getenv("EDITOR");
    if(editor != nullptr && editor[0] != '\0') return editor;

    return "nano";
}

} // namespace

AppConfig make_app_config(
    UserConfig final_user_config, bool no_confirm, bool rm_deps,
    PresentationDetail presentation_detail) {
    return AppConfig{
        std::move(final_user_config),
        no_confirm,
        rm_deps,
        resolve_editor_from_environment(),
        make_provider_selection_session(no_confirm),
        {},
        presentation_detail};
}

ProviderSelectionCallback provider_selection_callback(const AppConfig& config) {
    if(!config.provider_selection) return {};

    std::shared_ptr<ProviderSelectionSession> session = config.provider_selection;
    ProviderCandidatePresenter presenter =
        config.provider_candidate_presenter_factory
            ? config.provider_candidate_presenter_factory()
            : make_default_provider_candidate_presenter();
    return [session = std::move(session), presenter = std::move(presenter)](
               const std::string& dependency,
               const std::vector<ProvidedDependency>& candidates) {
        return session->select_provider(dependency, candidates, presenter);
    };
}
