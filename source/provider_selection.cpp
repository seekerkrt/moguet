#include "provider_selection.hpp"

#include "dependency_spec.hpp"
#include "localization.hpp"
#include "package_text_style.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <iostream>
#include <iterator>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace {

std::string trim(std::string value) {
    const auto is_not_space = [](unsigned char character) {
        return std::isspace(character) == 0;
    };

    auto first = std::find_if(value.begin(), value.end(), is_not_space);
    if(first == value.end()) return {};

    auto last = std::find_if(value.rbegin(), value.rend(), is_not_space).base();
    return std::string(first, last);
}

std::string to_lower(std::string value) {
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

std::string metadata_value(const std::string& value) {
    return value.empty() ? "-" : value;
}

std::string metadata_value(const std::optional<std::string>& value) {
    if(!value.has_value() || value->empty()) return "-";
    return value.value();
}

std::string selected_provider_package_identity_conflict_diagnostic(
    const ProvidedDependency& existing,
    const ProvidedDependency& selected) {
    return localization::format_translated_message(
        "Selected providers use incompatible identities for package {}: {} and {}.",
        selected.package_name,
        provider_package_identity_display(existing),
        provider_package_identity_display(selected));
}

void present_candidate_metadata(
    std::ostream& output, std::size_t index,
    const ProvidedDependency& candidate) {
    // NO_TRANSLATE: Provider candidate fields are fixed CLI metadata labels.
    output << index << ") ";
    if(const auto* repository =
           std::get_if<RepositoryProviderOrigin>(&candidate.origin);
       repository != nullptr) {
        output << "source=repository"
               << " package=" << candidate.package_name
               << " repository=" << repository->repository_name;
        if(!candidate.package_base.empty() && candidate.package_base != candidate.package_name) {
            output << " PackageBase=" << candidate.package_base;
        }
    } else {
        output << "source=AUR"
               << " package=" << candidate.package_name
               << " PackageBase=" << metadata_value(candidate.package_base);
    }
    output << " provided="
           << metadata_value(candidate.provided_dependency_name)
           << " provided-specification="
           << metadata_value(candidate.provided_dependency_specification)
           << " version=" << metadata_value(candidate.package_version);
}

void present_compact_candidate(
    std::ostream& output, std::size_t index,
    const ProvidedDependency& candidate) {
    const bool styled = package_text_style::enabled_for(output);
    const auto* repository = std::get_if<RepositoryProviderOrigin>(&candidate.origin);
    output << index << ") ";
    package_text_style::identity(
        output, repository ? repository->repository_name : "aur",
        candidate.package_name, styled);
    output << ' ';
    package_text_style::version(output, metadata_value(candidate.package_version), styled);
    if(repository != nullptr && repository->repository_name == "aur") {
        output << ' ' << localization::translate_message("[repository]");
    }
    if(!candidate.package_base.empty() && candidate.package_base != candidate.package_name) {
        // NO_TRANSLATE: PackageBase is the canonical package build identity term.
        output << " (PackageBase: " << metadata_value(candidate.package_base) << ')';
    }
    // The shared presenter does not own the prompt's dependency context.
    // Keep one capability annotation so versioned or differing capabilities
    // remain comparable, without repeating component + identical specification.
    const std::string capability = candidate.provided_dependency_specification.empty()
                                       ? metadata_value(candidate.provided_dependency_name)
                                       : candidate.provided_dependency_specification;
    output << ' ' << localization::format_translated_message("[provides: {}]", capability);
    if(!candidate.provided_dependency_name.empty() &&
       dependency_package_name(capability) != candidate.provided_dependency_name) {
        output << ' ' << localization::format_translated_message("[component: {}]", candidate.provided_dependency_name);
    }
}

std::optional<std::size_t> parse_candidate_number(
    const std::string& input, std::size_t candidate_count) {
    std::size_t selected = 0;
    const char* first = input.data();
    const char* last = first + input.size();
    auto [end, error] = std::from_chars(first, last, selected);
    if(error != std::errc{} || end != last || selected == 0 ||
       selected > candidate_count) {
        return std::nullopt;
    }
    return selected;
}

} // namespace

ProviderCandidatePresenter make_default_provider_candidate_presenter(
    PresentationDetail detail) {
    switch(detail) {
        case PresentationDetail::Normal:
        case PresentationDetail::Detailed:
            return [detail](std::ostream& output, std::size_t index,
                            const ProvidedDependency& candidate) {
                present_provider_candidate_metadata(output, index, candidate, detail);
                output << '\n';
            };
    }
    throw std::logic_error("Unknown provider presentation detail.");
}

void present_provider_candidate_metadata(
    std::ostream& output, std::size_t index,
    const ProvidedDependency& candidate, PresentationDetail detail) {
    if(detail == PresentationDetail::Normal) {
        present_compact_candidate(output, index, candidate);
    } else {
        present_candidate_metadata(output, index, candidate);
    }
}

ProviderSelectionConflict::ProviderSelectionConflict(
    std::string dependency_name)
    : std::runtime_error(
          localization::format_translated_message(
              "Previously selected provider is no longer a candidate for dependency: {}",
              dependency_name)),
      dependency_name_(std::move(dependency_name)) {
}

const std::string& ProviderSelectionConflict::dependency_name() const noexcept {
    return dependency_name_;
}

ProviderSelectionSession::ProviderSelectionSession(
    std::istream& input, std::ostream& output, bool is_interactive)
    : input_(&input), output_(&output), is_interactive_(is_interactive) {
}

std::optional<ProvidedDependency> ProviderSelectionSession::select_provider(
    const std::string& dependency,
    const std::vector<ProvidedDependency>& candidates) {
    return select_provider(
        dependency, candidates, make_default_provider_candidate_presenter());
}

std::optional<ProviderSelectionSet> ProviderSelectionSession::select_provider_set(
    const std::string& dependency,
    const std::vector<ProvidedDependency>& candidates,
    const ProviderCandidatePresenter& present_candidate) {
    if(const auto cached = reuse_provider_selection(dependency, candidates);
       cached.has_value()) {
        return cached;
    }
    if(!select_provider(dependency, candidates, present_candidate).has_value()) {
        return std::nullopt;
    }
    return reuse_provider_selection(dependency, candidates);
}

std::optional<ProvidedDependency> ProviderSelectionSession::select_provider(
    const std::string& dependency,
    const std::vector<ProvidedDependency>& candidates,
    const ProviderCandidatePresenter& present_candidate) {
    const std::string dependency_name = dependency_package_name(dependency);
    if(dependency_name.empty()) {
        throw std::invalid_argument(
            localization::translate_message(
                "Provider selection requires a non-empty dependency name."));
    }

    if(const auto existing = reuse_provider_selection(dependency_name, candidates);
       existing.has_value()) {
        if(existing->members().size() != 1) {
            throw std::logic_error(
                "Legacy provider selection requires exactly one provider.");
        }
        return existing->members().front();
    }

    if(cancelled_dependencies_.contains(dependency_name))
        return std::nullopt;

    if(!is_interactive_ || candidates.size() < 2) return std::nullopt;
    // NO_TRANSLATE: The ":: " framing, numeric range, and response tokens are
    // fixed provider-selection UI syntax. The complete prompt sentences are
    // translated below.
    *output_ << ":: " << localization::format_translated_message("Choose a provider for {}:", dependency_name) << '\n';
    for(std::size_t index = 0; index < candidates.size(); ++index) {
        present_candidate(*output_, index + 1, candidates[index]);
    }

    const std::string choice_range =
        "1-" + std::to_string(candidates.size());
    for(;;) {
        // TRANSLATORS: The placeholders are the numeric provider-choice range,
        // the literal Enter key, and the fixed q/quit/cancel response tokens.
        *output_ << ":: "
                 << localization::format_translated_message(
                        "Select a provider from [{}], or press {} / enter "
                        "{} to cancel:",
                        choice_range, "Enter", "q/quit/cancel")
                 << " " << std::flush;

        std::string input;
        if(!std::getline(*input_, input)) {
            cancelled_dependencies_.insert(dependency_name);
            return std::nullopt;
        }

        input = to_lower(trim(std::move(input)));
        if(input.empty() || input == "q" || input == "quit" ||
           input == "cancel") {
            cancelled_dependencies_.insert(dependency_name);
            return std::nullopt;
        }

        std::optional<std::size_t> selected =
            parse_candidate_number(input, candidates.size());
        if(!selected.has_value()) {
            // TRANSLATORS: The placeholders are the numeric provider-choice
            // range, the literal Enter key, and the fixed q/quit/cancel
            // response tokens.
            *output_ << ":: "
                     << localization::format_translated_message(
                            "Invalid choice. Enter a number from [{}], or "
                            "press {} / enter {} to cancel.",
                            choice_range, "Enter", "q/quit/cancel")
                     << '\n';
            continue;
        }

        const ProviderSelectionSet selection = record_provider_selection(
            dependency_name, candidates, {selected.value()});
        return selection.members().front();
    }
}

ProviderSelectionSet ProviderSelectionSession::record_provider_selection(
    const std::string& dependency,
    const std::vector<ProvidedDependency>& candidates,
    const std::vector<std::size_t>& one_origin_indices) {
    const std::string dependency_name = dependency_package_name(dependency);
    if(dependency_name.empty()) {
        throw std::invalid_argument(
            localization::translate_message(
                "Provider selection requires a non-empty dependency name."));
    }
    if(selections_.contains(dependency_name) ||
       cancelled_dependencies_.contains(dependency_name)) {
        throw std::logic_error("Provider selection was already decided.");
    }

    ProviderSelectionSet selection = ProviderSelectionSet::from_candidate_indices(
        candidates, one_origin_indices);
    for(const auto& [other_dependency, existing] : selections_) {
        static_cast<void>(other_dependency);
        for(const ProvidedDependency& selected : selection.members()) {
            const auto conflict = std::find_if(
                existing.members().begin(), existing.members().end(),
                [&selected](const ProvidedDependency& previous) {
                    return has_incompatible_provider_package_identity(previous, selected);
                });
            if(conflict != existing.members().end()) {
                throw std::runtime_error(
                    selected_provider_package_identity_conflict_diagnostic(
                        *conflict, selected));
            }
        }
    }
    selections_.emplace(dependency_name, selection);
    return selection;
}

std::optional<ProviderSelectionSet> ProviderSelectionSession::reuse_provider_selection(
    const std::string& dependency,
    const std::vector<ProvidedDependency>& candidates) const {
    const std::string dependency_name = dependency_package_name(dependency);
    if(dependency_name.empty()) {
        throw std::invalid_argument(
            localization::translate_message(
                "Provider selection requires a non-empty dependency name."));
    }
    const auto existing = selections_.find(dependency_name);
    if(existing == selections_.end()) return std::nullopt;

    std::vector<std::size_t> current_indices;
    for(const ProvidedDependency& previous : existing->second.members()) {
        const auto current = std::find_if(
            candidates.begin(), candidates.end(),
            [&previous](const ProvidedDependency& candidate) {
                return same_provider_identity(candidate, previous);
            });
        if(current == candidates.end()) {
            throw ProviderSelectionConflict(dependency_name);
        }
        current_indices.push_back(
            static_cast<std::size_t>(std::distance(candidates.begin(), current)) + 1);
    }
    // Rebuild from current candidates: cached metadata never becomes authority.
    return ProviderSelectionSet::from_candidate_indices(candidates, current_indices);
}

bool ProviderSelectionSession::is_interactive() const noexcept {
    return is_interactive_;
}

bool ProviderSelectionSession::was_cancelled(
    const std::string& dependency) const {
    const std::string dependency_name = dependency_package_name(dependency);
    return !dependency_name.empty() &&
           cancelled_dependencies_.contains(dependency_name);
}

std::shared_ptr<ProviderSelectionSession> make_provider_selection_session(
    bool no_confirm) {
    const bool is_interactive =
        !no_confirm && isatty(STDIN_FILENO) != 0;
    return std::make_shared<ProviderSelectionSession>(
        std::cin, std::cout, is_interactive);
}
