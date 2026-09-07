#include "exact_artifact_transaction_protocol.hpp"

#include "xdg_generation_store.hpp"

#include <algorithm>
#include <charconv>
#include <new>
#include <stdexcept>

namespace {

std::string_view operation_name(ExactArtifactTransactionOperation operation) {
    switch(operation) {
        case ExactArtifactTransactionOperation::Install: return "Install";
        case ExactArtifactTransactionOperation::Upgrade: return "Upgrade";
    }
    throw std::invalid_argument("unknown exact transaction operation");
}

std::string fragment_prefix(const SourceArtifactInstallRootPrepareRequest& manifest,
                            std::string_view stage, ExactArtifactTransactionOperation operation) {
    if(!is_valid_source_artifact_install_root_request(manifest) ||
       manifest.purpose != SourceArtifactInstallTrustedPurpose::ExactInstalledBinding ||
       !is_valid_source_artifact_install_sha256(stage))
        throw std::invalid_argument("invalid exact transaction authority");
    return "MOGUET-EXACT-ARTIFACT-OPERATIONS\t1\nOWNER\tsource-artifact-install\n"
           "PURPOSE\tExactInstalledBinding\nTOKEN\t" +
           manifest.transaction_token +
           "\nMANIFEST\t" + xdg_generation_store_raw_contents_sha256(serialize_source_artifact_install_root_prepared_state(manifest)) +
           "\nSTAGE\t" + std::string(stage) + "\nOPERATION\t" + std::string(operation_name(operation)) + "\n";
}

std::string artifact_record(const SourceArtifactInstallRootArtifactExpectation& artifact) {
    return "SELECTED\t" + std::to_string(artifact.artifact_index) + "\t" + artifact.package_name +
           "\t" + artifact.archive_sha256 + "\t" + artifact.full_version + "\t" + artifact.package_base +
           "\t" + artifact.architecture + "\t" + artifact.raw_mtree_sha256 + "\n";
}

std::size_t size_value(std::string_view text) {
    std::size_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if(text.empty() || (text.size() > 1 && text.front() == '0') || result.ec != std::errc{} ||
       result.ptr != text.data() + text.size() || value > SOURCE_ARTIFACT_INSTALL_MAXIMUM_PROTOCOL_BYTES)
        throw std::invalid_argument("invalid exact protocol length");
    return value;
}

std::string_view take_line(std::string_view& text) {
    const auto end = text.find('\n');
    if(end == std::string_view::npos) throw std::invalid_argument("truncated exact protocol line");
    const auto line = text.substr(0, end);
    text.remove_prefix(end + 1);
    return line;
}

void append_frame(std::string& protocol, std::string_view key, std::string_view payload) {
    protocol.append(key).append("\t").append(std::to_string(payload.size())).append("\n").append(payload);
    if(protocol.size() > SOURCE_ARTIFACT_INSTALL_MAXIMUM_PROTOCOL_BYTES)
        throw std::invalid_argument("oversized exact protocol frame");
}

std::string_view take_frame(std::string_view& protocol, std::string_view key) {
    const auto line = take_line(protocol);
    const auto prefix = std::string(key) + "\t";
    if(!line.starts_with(prefix)) throw std::invalid_argument("unexpected exact protocol field");
    const auto size = size_value(line.substr(prefix.size()));
    if(size > protocol.size()) throw std::invalid_argument("truncated exact protocol frame");
    const auto payload = protocol.substr(0, size);
    protocol.remove_prefix(size);
    return payload;
}

std::string evidence_prefix(const SourceArtifactInstallRootPrepareRequest& manifest, std::string_view stage) {
    static_cast<void>(fragment_prefix(manifest, stage, ExactArtifactTransactionOperation::Install));
    return "MOGUET-EXACT-ARTIFACT-EVIDENCE\t1\nOWNER\tsource-artifact-install\nPURPOSE\tExactInstalledBinding\nTOKEN\t" +
           manifest.transaction_token + "\nMANIFEST\t" +
           xdg_generation_store_raw_contents_sha256(serialize_source_artifact_install_root_prepared_state(manifest)) +
           "\nSTAGE\t" + std::string(stage) + "\n";
}

std::string serialize_record_result(const ExactArtifactRecordObservationsResult& result) {
    if(const auto* issue = std::get_if<InstalledRecordObservationIssue>(&result))
        return "MOGUET-EXACT-RECORD-OBSERVATIONS\t1\nFAILURE\n" + serialize_installed_package_record_observation(*issue);
    return serialize_exact_artifact_record_observations(std::get<ExactArtifactRecordObservations>(result));
}

} // namespace

std::string serialize_exact_artifact_record_observations(const ExactArtifactRecordObservations& observations) {
    if(observations.size() > SOURCE_ARTIFACT_INSTALL_MAXIMUM_ARTIFACTS)
        throw std::invalid_argument("oversized exact record observations");
    std::string protocol = "MOGUET-EXACT-RECORD-OBSERVATIONS\t1\n";
    for(const auto& record : observations)
        append_frame(protocol, "RECORD\t" + std::to_string(record.artifact_index) + "\t" + record.package_name,
                     serialize_installed_package_record_observation(record.observation));
    return protocol + "END\n";
}

ExactArtifactRecordObservationsResult parse_exact_artifact_record_observations(
    std::string_view protocol, const SourceArtifactInstallRootPrepareRequest& manifest) noexcept {
    using Issue = InstalledRecordObservationIssue;
    try {
        if(protocol.size() > SOURCE_ARTIFACT_INSTALL_MAXIMUM_PROTOCOL_BYTES ||
           take_line(protocol) != "MOGUET-EXACT-RECORD-OBSERVATIONS\t1") return Issue::MalformedMetadata;
        if(protocol.starts_with("FAILURE\n")) {
            auto parsed = parse_installed_package_record_observation(protocol.substr(8));
            if(const auto* issue = std::get_if<Issue>(&parsed)) return *issue;
            return Issue::MalformedMetadata;
        }
        ExactArtifactRecordObservations records;
        for(const auto& artifact : manifest.artifacts) {
            const auto key = "RECORD\t" + std::to_string(artifact.artifact_index) + "\t" + artifact.package_name;
            if(!protocol.starts_with(key + "\t")) continue;
            const auto payload = take_frame(protocol, key);
            records.push_back({artifact.artifact_index, artifact.package_name, parse_installed_package_record_observation(payload)});
        }
        if(protocol != "END\n") return Issue::MalformedMetadata;
        return records;
    } catch(const std::bad_alloc&) {
        return Issue::ResourceFailure;
    } catch(...) {
        return Issue::MalformedMetadata;
    }
}

std::string serialize_exact_artifact_root_evidence(
    const ExactArtifactRootEvidence& evidence, const SourceArtifactInstallRootPrepareRequest& manifest, std::string_view stage) {
    auto protocol = evidence_prefix(manifest, stage);
    append_frame(protocol, "WORLD", serialize_installed_database_world(evidence.world));
    append_frame(protocol, "BASELINE", serialize_record_result(evidence.baseline));
    append_frame(protocol, "INSTALL", evidence.installs.value_or(""));
    append_frame(protocol, "UPGRADE", evidence.upgrades.value_or(""));
    append_frame(protocol, "INSTALL-ANCHORS", serialize_record_result(evidence.install_anchors));
    append_frame(protocol, "UPGRADE-ANCHORS", serialize_record_result(evidence.upgrade_anchors));
    return protocol + "END\n";
}

std::variant<ExactArtifactRootEvidence, ExactArtifactReceiptIssue> parse_exact_artifact_root_evidence(
    std::string_view protocol, const SourceArtifactInstallRootPrepareRequest& manifest, std::string_view stage) noexcept {
    try {
        const auto prefix = evidence_prefix(manifest, stage);
        if(protocol.size() > SOURCE_ARTIFACT_INSTALL_MAXIMUM_PROTOCOL_BYTES || !protocol.starts_with(prefix))
            return ExactArtifactReceiptIssue::Invalid;
        protocol.remove_prefix(prefix.size());
        ExactArtifactRootEvidence evidence;
        evidence.world = parse_installed_database_world(take_frame(protocol, "WORLD"));
        evidence.baseline = parse_exact_artifact_record_observations(take_frame(protocol, "BASELINE"), manifest);
        const auto installs = take_frame(protocol, "INSTALL");
        if(!installs.empty()) evidence.installs.emplace(installs);
        const auto upgrades = take_frame(protocol, "UPGRADE");
        if(!upgrades.empty()) evidence.upgrades.emplace(upgrades);
        evidence.install_anchors = parse_exact_artifact_record_observations(take_frame(protocol, "INSTALL-ANCHORS"), manifest);
        evidence.upgrade_anchors = parse_exact_artifact_record_observations(take_frame(protocol, "UPGRADE-ANCHORS"), manifest);
        if(protocol != "END\n") return ExactArtifactReceiptIssue::Invalid;
        return evidence;
    } catch(const std::bad_alloc&) {
        return ExactArtifactReceiptIssue::ResourceFailure;
    } catch(...) {
        return ExactArtifactReceiptIssue::Invalid;
    }
}

std::string serialize_exact_artifact_operation_fragment(
    const SourceArtifactInstallRootPrepareRequest& manifest,
    std::string_view staged_identity_sha256, ExactArtifactTransactionOperation operation,
    const std::vector<std::string>& targets) {
    std::string protocol = fragment_prefix(manifest, staged_identity_sha256, operation);
    if(targets.empty() || targets.size() > manifest.artifacts.size())
        throw std::invalid_argument("empty or oversized exact operation targets");
    for(std::size_t index = 0; index < targets.size(); ++index) {
        if(std::find(targets.begin(), targets.begin() + static_cast<std::ptrdiff_t>(index), targets[index]) !=
               targets.begin() + static_cast<std::ptrdiff_t>(index) ||
           std::none_of(manifest.artifacts.begin(), manifest.artifacts.end(),
                        [&](const auto& artifact) { return artifact.package_name == targets[index]; }))
            throw std::invalid_argument("duplicate or unselected exact operation target");
    }
    // NeedsTargets arrival order has no identity authority.
    for(const auto& artifact : manifest.artifacts)
        if(std::find(targets.begin(), targets.end(), artifact.package_name) != targets.end())
            protocol += artifact_record(artifact);
    protocol += "END\n";
    if(protocol.size() > SOURCE_ARTIFACT_INSTALL_MAXIMUM_PROTOCOL_BYTES)
        throw std::invalid_argument("oversized exact operation fragment");
    return protocol;
}

ExactArtifactOperationRecordsResult parse_exact_artifact_operation_fragment(
    std::string_view fragment, const SourceArtifactInstallRootPrepareRequest& manifest,
    std::string_view staged_identity_sha256, ExactArtifactTransactionOperation fixed_operation) {
    try {
        const auto prefix = fragment_prefix(manifest, staged_identity_sha256, fixed_operation);
        if(fragment.size() > SOURCE_ARTIFACT_INSTALL_MAXIMUM_PROTOCOL_BYTES || !fragment.starts_with(prefix))
            return ExactArtifactReceiptIssue::Invalid;
        fragment.remove_prefix(prefix.size());
        ExactArtifactOperationRecords records;
        for(const auto& artifact : manifest.artifacts) {
            const auto expected = artifact_record(artifact);
            if(fragment.starts_with(expected)) {
                records.push_back({fixed_operation, artifact});
                fragment.remove_prefix(expected.size());
            }
        }
        if(records.empty() || fragment != "END\n") return ExactArtifactReceiptIssue::Invalid;
        return records;
    } catch(const std::bad_alloc&) {
        return ExactArtifactReceiptIssue::ResourceFailure;
    } catch(const std::exception&) {
        return ExactArtifactReceiptIssue::Invalid;
    }
}

ExactArtifactOperationRecordsResult join_exact_artifact_operation_fragments(
    const std::optional<std::string>& installs, const std::optional<std::string>& upgrades,
    const SourceArtifactInstallRootPrepareRequest& manifest, std::string_view stage) {
    try {
        // Validate even an entirely missing receipt against a closed purpose.
        static_cast<void>(fragment_prefix(manifest, stage, ExactArtifactTransactionOperation::Install));
        ExactArtifactOperationRecords records;
        for(const auto kind : {ExactArtifactTransactionOperation::Install, ExactArtifactTransactionOperation::Upgrade}) {
            const auto& fragment = kind == ExactArtifactTransactionOperation::Install ? installs : upgrades;
            if(!fragment) continue;
            auto parsed = parse_exact_artifact_operation_fragment(*fragment, manifest, stage, kind);
            if(const auto* issue = std::get_if<ExactArtifactReceiptIssue>(&parsed)) return *issue;
            for(auto& record : std::get<ExactArtifactOperationRecords>(parsed)) {
                if(std::any_of(records.begin(), records.end(), [&](const auto& prior) {
                       return prior.artifact.artifact_index == record.artifact.artifact_index ||
                              prior.artifact.package_name == record.artifact.package_name;
                   })) return ExactArtifactReceiptIssue::Invalid;
                records.push_back(std::move(record));
            }
        }
        if(records.size() != manifest.artifacts.size()) return ExactArtifactReceiptIssue::Missing;
        ExactArtifactOperationRecords canonical;
        canonical.reserve(records.size());
        for(const auto& artifact : manifest.artifacts) {
            const auto found = std::find_if(records.begin(), records.end(), [&](const auto& record) {
                return record.artifact == artifact;
            });
            if(found == records.end()) return ExactArtifactReceiptIssue::Invalid;
            canonical.push_back(*found);
        }
        return canonical;
    } catch(const std::bad_alloc&) {
        return ExactArtifactReceiptIssue::ResourceFailure;
    } catch(const std::exception&) {
        return ExactArtifactReceiptIssue::Invalid;
    }
}
