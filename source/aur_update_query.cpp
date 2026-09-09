#include "aur_update_query.hpp"

#include "aur_rpc.hpp"
#include "aur_devel_update.hpp"
#include "localization.hpp"
#include "logging.hpp"
#include "package_metadata.hpp"
#include "process.hpp"
#include "shell_words.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// installed foreign package inventoryから、read-only AUR queryとversion比較を経て
// pureなAurUpdatePlanを組み立てるconsumer。
namespace {

constexpr std::size_t AUR_INFO_BATCH_SIZE = 100;

AurVersionRelation compare_aur_versions(
    const std::string& aur_version, const std::string& installed_version) {
    std::string comparison_command =
        "vercmp " + shell_words::quote(aur_version) + " " +
        shell_words::quote(installed_version);
    std::string comparison_output = exec_command(comparison_command.c_str());

    try {
        std::size_t consumed_characters = 0;
        int comparison = std::stoi(comparison_output, &consumed_characters);
        if(consumed_characters != comparison_output.size()) {
            return AurVersionRelation::Unavailable;
        }
        if(comparison > 0) return AurVersionRelation::NewerThanInstalled;
        if(comparison < 0) return AurVersionRelation::OlderThanInstalled;
        return AurVersionRelation::SameAsInstalled;
    } catch(const std::invalid_argument&) {
        return AurVersionRelation::Unavailable;
    } catch(const std::out_of_range&) {
        return AurVersionRelation::Unavailable;
    }
}

std::vector<AurUpdatePlanInput> make_plan_inputs(
    const ForeignPackageInventory& installed_packages,
    const std::map<std::string, AurPackageInfo>& aur_packages,
    const std::set<std::string>& metadata_unavailable_packages) {
    std::vector<AurUpdatePlanInput> inputs;
    inputs.reserve(installed_packages.size());

    // POLICY(#266): lookup containerのkey順ではなく、inventory順をplanの正本にする。
    for(const auto& installed_package : installed_packages) {
        auto aur_package = aur_packages.find(installed_package.name);
        AurUpdateMetadataResult aur_metadata = AurUpdateMetadataNotFound{};
        if(metadata_unavailable_packages.contains(installed_package.name)) {
            aur_metadata = AurUpdateMetadataUnavailable{};
        } else if(aur_package != aur_packages.end()) {
            aur_metadata = AurUpdateRemotePackage{
                aur_package->second.Name,
                aur_package->second.PackageBase,
                aur_package->second.Version,
                compare_aur_versions(
                    aur_package->second.Version,
                    installed_package.version)};
        }

        inputs.push_back(AurUpdatePlanInput{
            installed_package.name,
            installed_package.version,
            installed_package.reason,
            std::move(aur_metadata)});
    }

    return inputs;
}

} // namespace

AurUpdateQueryResult query_installed_aur_updates() {
    PacmanRepositoryConfiguration configuration =
        resolve_pacman_repository_configuration();
    ForeignPackageInventoryResult inventory_result =
        query_foreign_package_inventory(configuration);
    if(const auto* failure =
           std::get_if<PackageMetadataFailure>(&inventory_result)) {
        // POLICY(#266): inventory failureは正常なempty inventoryへ落とさない。
        // AUR RPC開始前にfatal errorとしてCLI境界へ伝播する。
        throw PackageMetadataError(*failure);
    }

    ForeignPackageInventory installed_packages =
        std::get<ForeignPackageInventory>(std::move(inventory_result));
    return query_aur_updates_for_foreign_inventory(
        std::move(installed_packages));
}

AurUpdateQueryResult query_aur_updates_for_foreign_inventory(
    ForeignPackageInventory installed_packages) {
    if(installed_packages.empty()) return {};

    std::vector<std::string> package_names;
    package_names.reserve(installed_packages.size());
    for(const auto& installed_package : installed_packages) {
        package_names.push_back(installed_package.name);
    }

    Logger::info(localization::format_translated_message(
        // TRANSLATORS: The placeholders are the AUR project identity and a package count.
        "Checking {} updates for {} foreign packages...", "AUR",
        installed_packages.size()));

    std::map<std::string, AurPackageInfo> aur_packages;
    std::set<std::string> metadata_unavailable_packages;
    std::vector<AurUpdateQueryFailure> recoverable_failures;
    for(std::size_t offset = 0; offset < package_names.size();
        offset += AUR_INFO_BATCH_SIZE) {
        std::size_t end =
            std::min(offset + AUR_INFO_BATCH_SIZE, package_names.size());
        Logger::info(localization::format_translated_message(
            // TRANSLATORS: The placeholders are the AUR project identity,
            // first package number, last package number, and total package count.
            "Fetching {} info for packages {}-{} of {}...", "AUR",
            offset + 1, end, package_names.size()));

        std::vector<std::string> batch(
            package_names.begin() + offset, package_names.begin() + end);
        try {
            std::map<std::string, AurPackageInfo> batch_results =
                AurClient::info_many(batch);
            if(batch_results.empty()) {
                // POLICY(#266): non-empty partial responseは追加queryせず正本とする。
                // 全件emptyの場合だけstrict queryでabsenceとtransport failureを分ける。
                Logger::warn(localization::format_translated_message(
                    // TRANSLATORS: AUR is a runtime project identity.
                    "Bulk {} info returned no results. Falling back to per-package checks for this batch.",
                    "AUR"));
                for(const auto& package_name : batch) {
                    std::optional<AurPackageInfo> aur_package =
                        AurClient::info_strict(package_name);
                    if(aur_package.has_value()) {
                        batch_results[aur_package->Name] = aur_package.value();
                    }
                }
            }
            aur_packages.insert(batch_results.begin(), batch_results.end());
        } catch(const AurRpcResponseError&) {
            // schema/semantic violationはrecoverableなtransport failureへ落とさない。
            throw;
        } catch(const std::exception& error) {
            Logger::error(localization::format_translated_message(
                // TRANSLATORS: The placeholders are the AUR project identity and an upstream failure detail.
                "Failed to fetch {} info: {}", "AUR", error.what()));
            metadata_unavailable_packages.insert(batch.begin(), batch.end());
            recoverable_failures.push_back(
                AurUpdateQueryFailure{batch, error.what()});
        }
    }

    std::vector<AurUpdatePlanInput> inputs = make_plan_inputs(
        installed_packages, aur_packages, metadata_unavailable_packages);
    AurUpdateQueryResult result{make_aur_update_plan(inputs), std::move(recoverable_failures), {}};
    result.devel_observations = refine_aur_devel_updates(result.plan);
    for(const auto& entry : result.plan.entries) {
        if(aur_update_basis(entry) == AurUpdateBasis::GitRevision)
            Logger::info(localization::format_translated_message("Git revision update available: {} (package-version decision unchanged).", entry.installed_name));
    }
    return result;
}
