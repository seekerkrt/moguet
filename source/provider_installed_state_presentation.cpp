#include "provider_installed_state_presentation.hpp"

#include "localization.hpp"
#include "package_text_style.hpp"

#include <memory>
#include <ostream>
#include <set>
#include <string>
#include <utility>

namespace {

bool is_session_level_failure(PackageMetadataErrorCode code) {
    switch(code) {
        case PackageMetadataErrorCode::ConfigurationUnavailable:
        case PackageMetadataErrorCode::ConfigurationMalformed:
        case PackageMetadataErrorCode::InitializationFailed:
        case PackageMetadataErrorCode::LocalDatabaseUnavailable:
            return true;
        case PackageMetadataErrorCode::InvalidPackageName:
        case PackageMetadataErrorCode::QueryFailed:
        case PackageMetadataErrorCode::MalformedMetadata:
        case PackageMetadataErrorCode::SyncDatabaseUnavailable:
        case PackageMetadataErrorCode::RepositoryNotConfigured:
            return false;
    }
    return false;
}

class ProviderInstalledStateCandidatePresenter final {
public:
    explicit ProviderInstalledStateCandidatePresenter(
        ProviderInstalledStateLookup& lookup, PresentationDetail detail)
        : lookup_(lookup), detail_(detail) {
    }

    void present(
        std::ostream& output, std::size_t index,
        const ProvidedDependency& candidate) {
        present_provider_candidate_metadata(output, index, candidate, detail_);

        ProviderInstalledStateObservation observation =
            lookup_.query(candidate.package_name);
        const PackageMetadataFailure* failure = nullptr;
        switch(observation.state()) {
            case ProviderInstalledState::Installed:
                output << ' ';
                package_text_style::installed(
                    output, localization::translate_message("[installed]"),
                    detail_ == PresentationDetail::Normal &&
                        package_text_style::enabled_for(output));
                break;
            case ProviderInstalledState::NotInstalled:
                break;
            case ProviderInstalledState::Unknown:
                output << ' '
                       << localization::translate_message(
                              // TRANSLATORS: This tag means Moguet could not
                              // determine whether the provider package is
                              // installed. It does not reject a valid
                              // numbered choice and is distinct from a
                              // not-installed package.
                              "[installed state unknown]");
                failure = &observation.failure();
                break;
        }
        output << '\n';
        if(failure != nullptr) {
            report_unknown_state(output, candidate.package_name, *failure);
        }
    }

private:
    void report_unknown_state(
        std::ostream& output, const std::string& package_name,
        const PackageMetadataFailure& failure) {
        if(is_session_level_failure(failure.code)) {
            if(session_failure_reported_) return;
            session_failure_reported_ = true;
            output << ":: " << localization::format_translated_message(
                                   // TRANSLATORS: The placeholder is a diagnostic from the
                                   // read-only local package database query. This warning is
                                   // emitted once for one provider-selection phase, not once
                                   // for every candidate.
                                   "Warning: installed state is unavailable for provider candidates: {}.", failure.diagnostic)
                   << '\n';
            return;
        }

        if(!reported_package_failures_.insert(package_name).second) return;
        output << ":: " << localization::format_translated_message(
                               // TRANSLATORS: The first placeholder is a provider package
                               // name and the second is a diagnostic from the read-only
                               // local package database query. This warning does not make
                               // the candidate invalid.
                               "Warning: installed state is unavailable for provider candidate {}: {}.", package_name, failure.diagnostic)
               << '\n';
    }

    ProviderInstalledStateLookup& lookup_;
    PresentationDetail detail_;
    bool session_failure_reported_ = false;
    std::set<std::string> reported_package_failures_;
};

} // namespace

ProviderCandidatePresenter make_provider_installed_state_candidate_presenter(
    ProviderInstalledStateLookup& lookup, PresentationDetail detail) {
    // Detail belongs to the phase-local presenter, never the lookup/session.
    switch(detail) {
        case PresentationDetail::Normal:
        case PresentationDetail::Detailed: {
            auto presenter = std::make_shared<ProviderInstalledStateCandidatePresenter>(
                lookup, detail);
            return [presenter = std::move(presenter)](
                       std::ostream& output, std::size_t index,
                       const ProvidedDependency& candidate) {
                presenter->present(output, index, candidate);
            };
        }
    }
    throw std::logic_error("Unknown provider presentation detail.");
}

ProviderCandidatePresenterFactory
make_provider_installed_state_candidate_presenter_factory() {
    return [](PresentationDetail detail) {
        // POLICY(#388): lookupはselection sessionではなく、このcallback phaseの
        // presentation seamが所有する。queryは候補listを実際に表示するまで行わない。
        auto lookup = std::make_shared<ProviderInstalledStateLookup>();
        ProviderCandidatePresenter presenter =
            make_provider_installed_state_candidate_presenter(*lookup, detail);
        return [lookup = std::move(lookup), presenter = std::move(presenter)](
                   std::ostream& output, std::size_t index,
                   const ProvidedDependency& candidate) {
            presenter(output, index, candidate);
        };
    };
}
