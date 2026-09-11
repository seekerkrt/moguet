#include "source_artifact_install_trusted_protocol.hpp"

#include "package_identifier.hpp"
#include "trusted_alpm_receipt_protocol.hpp"

#include <algorithm>
#include <iterator>
#include <charconv>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

constexpr std::string_view OWNER = "source-artifact-install";
constexpr std::string_view PREPARED_HEADER =
    "MOGUET-SOURCE-ARTIFACT-PREPARED\t2";
constexpr std::string_view LEGACY_PREPARED_HEADER = "MOGUET-LEGACY-ARTIFACT-PREPARED\t1";
constexpr std::string_view PREPARE_RESPONSE_HEADER =
    "MOGUET-SOURCE-ARTIFACT-PREPARE-RESPONSE\t2";
constexpr std::string_view RECEIPT_HEADER =
    "MOGUET-SOURCE-ARTIFACT-RECEIPT\t2";
constexpr std::string_view EXACT_PREPARED_PREFIX =
    "MOGUET-EXACT-ARTIFACT-PREPARED\t1\nPURPOSE\tExactInstalledBinding\n";
constexpr std::string_view EXACT_RESPONSE_PREFIX =
    "MOGUET-EXACT-ARTIFACT-PREPARE-RESPONSE\t1\nPURPOSE\tExactInstalledBinding\n";
constexpr std::string_view TOKEN_PREFIX = "TOKEN\t";
constexpr std::string_view OWNER_PREFIX = "OWNER\t";
constexpr std::string_view PACKAGE_BASE_PREFIX = "PACKAGEBASE\t";
constexpr std::string_view DIRECTIVE_PREFIX = "DIRECTIVE\t";
constexpr std::string_view NEEDED_PREFIX = "NEEDED\t";
constexpr std::string_view NO_CONFIRM_PREFIX = "NOCONFIRM\t";
constexpr std::string_view ARTIFACT_PREFIX = "ARTIFACT\t";
constexpr std::string_view HOOK_DIRECTORY_PREFIX = "HOOKDIR\t";
constexpr std::string_view STAGED_PREFIX = "STAGED\t";
constexpr std::string_view STATE_PREFIX = "STATE\t";
constexpr std::string_view INSTALL_PREFIX = "INSTALL\t";
constexpr std::string_view END_RECORD = "END";

template <typename Success>
std::variant<Success, SourceArtifactInstallTrustedProtocolFailure> fail(
    SourceArtifactInstallTrustedProtocolIssueKind issue) {
    return SourceArtifactInstallTrustedProtocolFailure{issue};
}

bool metadata_value_is_valid(
    std::string_view value, std::size_t maximum_bytes) noexcept {
    if(value.empty() || value.size() > maximum_bytes) return false;
    return std::all_of(
        value.begin(), value.end(), [](unsigned char character) {
            return character > 0x20 && character != 0x7f;
        });
}

bool artifact_is_valid(
    const SourceArtifactInstallRootArtifactExpectation& artifact,
    std::string_view expected_package_base, bool legacy = false) noexcept {
    return artifact.artifact_index <
               SOURCE_ARTIFACT_INSTALL_MAXIMUM_ARTIFACTS &&
           is_valid_package_name(artifact.package_name) &&
           is_valid_trusted_alpm_receipt_package_name(
               artifact.package_name) &&
           metadata_value_is_valid(artifact.full_version, 4096) &&
           (legacy ? artifact.package_base == "-" && artifact.architecture == "-"
                   : is_valid_package_name(artifact.package_base) &&
                         artifact.package_base == expected_package_base &&
                         metadata_value_is_valid(artifact.architecture, 256)) &&
           artifact.artifact_size > 0 &&
           artifact.artifact_size <=
               SOURCE_ARTIFACT_INSTALL_MAXIMUM_ARTIFACT_BYTES &&
           artifact.signature_size <=
               SOURCE_ARTIFACT_INSTALL_MAXIMUM_SIGNATURE_BYTES &&
           is_valid_source_artifact_install_sha256(artifact.archive_sha256) &&
           (artifact.signature_size == 0 ? (artifact.signature_sha256 == "-" ||
                                            (legacy && artifact.signature_sha256 == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"))
                                         : is_valid_source_artifact_install_sha256(artifact.signature_sha256));
}

bool checked_add(
    std::uint64_t value, std::uint64_t& aggregate) noexcept {
    if(value > SOURCE_ARTIFACT_INSTALL_MAXIMUM_TRANSACTION_BYTES -
                   aggregate) {
        return false;
    }
    aggregate += value;
    return true;
}

std::optional<std::string_view> record_value(
    std::string_view record, std::string_view prefix) noexcept {
    if(!record.starts_with(prefix)) return std::nullopt;
    return record.substr(prefix.size());
}

std::variant<
    std::vector<std::string_view>,
    SourceArtifactInstallTrustedProtocolFailure>
split_protocol_lines(std::string_view protocol) {
    if(protocol.size() >
       SOURCE_ARTIFACT_INSTALL_MAXIMUM_PROTOCOL_BYTES) {
        return SourceArtifactInstallTrustedProtocolFailure{
            SourceArtifactInstallTrustedProtocolIssueKind::InputTooLarge};
    }
    if(protocol.empty() || protocol.back() != '\n') {
        return SourceArtifactInstallTrustedProtocolFailure{
            SourceArtifactInstallTrustedProtocolIssueKind::
                MissingFinalNewline};
    }

    std::vector<std::string_view> lines;
    std::size_t offset = 0;
    while(offset < protocol.size()) {
        const std::size_t newline = protocol.find('\n', offset);
        if(newline == std::string_view::npos) {
            return SourceArtifactInstallTrustedProtocolFailure{
                SourceArtifactInstallTrustedProtocolIssueKind::
                    TruncatedProtocol};
        }
        lines.push_back(protocol.substr(offset, newline - offset));
        if(lines.size() >
           TRUSTED_ALPM_RECEIPT_MAXIMUM_PACKAGES + 16) {
            return SourceArtifactInstallTrustedProtocolFailure{
                SourceArtifactInstallTrustedProtocolIssueKind::
                    TooManyArtifacts};
        }
        offset = newline + 1;
    }
    return lines;
}

std::vector<std::string_view> split_tabs(std::string_view line) {
    std::vector<std::string_view> fields;
    std::size_t offset = 0;
    while(true) {
        const std::size_t separator = line.find('\t', offset);
        if(separator == std::string_view::npos) {
            fields.push_back(line.substr(offset));
            return fields;
        }
        fields.push_back(line.substr(offset, separator - offset));
        offset = separator + 1;
    }
}

template <typename Integer>
std::optional<Integer> parse_canonical_unsigned(std::string_view value) {
    if(value.empty() ||
       (value.size() > 1 && value.front() == '0')) {
        return std::nullopt;
    }
    Integer parsed = 0;
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if(result.ec != std::errc{} ||
       result.ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<bool> parse_boolean(std::string_view value) noexcept {
    if(value == "0") return false;
    if(value == "1") return true;
    return std::nullopt;
}

std::optional<SourceArtifactInstallTrustedDirective> parse_directive(
    std::string_view value) noexcept {
    if(value == "PreserveExistingReason") {
        return SourceArtifactInstallTrustedDirective::
            PreserveExistingReason;
    }
    if(value == "AsDependency") {
        return SourceArtifactInstallTrustedDirective::AsDependency;
    }
    if(value == "AsExplicit") return SourceArtifactInstallTrustedDirective::AsExplicit;
    return std::nullopt;
}

std::string_view serialize_directive(
    SourceArtifactInstallTrustedDirective directive) {
    switch(directive) {
        case SourceArtifactInstallTrustedDirective::AsExplicit: return "AsExplicit";
        case SourceArtifactInstallTrustedDirective::
            PreserveExistingReason:
            return "PreserveExistingReason";
        case SourceArtifactInstallTrustedDirective::AsDependency:
            return "AsDependency";
    }
    throw std::invalid_argument(
        "invalid source-artifact install directive");
}

std::optional<SourceArtifactInstallRootArtifactExpectation>
parse_artifact_fields(
    const std::vector<std::string_view>& fields,
    std::size_t offset,
    std::string_view expected_package_base, bool legacy = false) {
    if(fields.size() < offset + 9) return std::nullopt;
    const auto artifact_index =
        parse_canonical_unsigned<std::size_t>(fields[offset]);
    const auto artifact_size =
        parse_canonical_unsigned<std::uint64_t>(fields[offset + 5]);
    const auto signature_size =
        parse_canonical_unsigned<std::uint64_t>(fields[offset + 6]);
    if(!artifact_index.has_value() || !artifact_size.has_value() ||
       !signature_size.has_value()) {
        return std::nullopt;
    }
    SourceArtifactInstallRootArtifactExpectation artifact{
        *artifact_index,
        std::string(fields[offset + 1]),
        std::string(fields[offset + 2]),
        std::string(fields[offset + 3]),
        std::string(fields[offset + 4]),
        *artifact_size,
        *signature_size, std::string(fields[offset + 7]),
        std::string(fields[offset + 8])};
    if(!artifact_is_valid(artifact, expected_package_base, legacy)) {
        return std::nullopt;
    }
    return artifact;
}

void append_artifact_record(
    std::string& protocol,
    const SourceArtifactInstallRootArtifactExpectation& artifact) {
    protocol.append(ARTIFACT_PREFIX)
        .append(std::to_string(artifact.artifact_index))
        .push_back('\t');
    protocol.append(artifact.package_name).push_back('\t');
    protocol.append(artifact.full_version).push_back('\t');
    protocol.append(artifact.package_base).push_back('\t');
    protocol.append(artifact.architecture).push_back('\t');
    protocol.append(std::to_string(artifact.artifact_size)).push_back('\t');
    protocol.append(std::to_string(artifact.signature_size)).push_back('\t');
    protocol.append(artifact.archive_sha256).push_back('\t');
    protocol.append(artifact.signature_sha256).push_back('\n');
}

} // namespace

std::string_view source_artifact_install_trusted_owner() noexcept {
    return OWNER;
}

bool is_valid_source_artifact_install_root_request(
    const SourceArtifactInstallRootPrepareRequest& request) noexcept {
    if((request.purpose != SourceArtifactInstallTrustedPurpose::CleanupInstallOnly &&
        request.purpose != SourceArtifactInstallTrustedPurpose::ExactInstalledBinding &&
        request.purpose != SourceArtifactInstallTrustedPurpose::LegacyArtifactInstall) ||
       !is_valid_trusted_alpm_receipt_token(request.transaction_token) ||
       !is_valid_package_name(request.package_base) ||
       request.artifacts.empty() ||
       request.artifacts.size() >
           SOURCE_ARTIFACT_INSTALL_MAXIMUM_ARTIFACTS) {
        return false;
    }
    const bool legacy = request.purpose == SourceArtifactInstallTrustedPurpose::LegacyArtifactInstall;
    switch(request.directive) {
        case SourceArtifactInstallTrustedDirective::AsExplicit:
            if(!legacy) return false;
            break;
        case SourceArtifactInstallTrustedDirective::
            PreserveExistingReason:
        case SourceArtifactInstallTrustedDirective::AsDependency:
            break;
        default:
            return false;
    }

    std::uint64_t aggregate_size = 0;
    std::size_t protocol_size = 256;
    for(std::size_t index = 0; index < request.artifacts.size(); ++index) {
        const auto& artifact = request.artifacts[index];
        if((request.purpose == SourceArtifactInstallTrustedPurpose::ExactInstalledBinding
                ? !is_valid_source_artifact_install_sha256(artifact.raw_mtree_sha256)
                : artifact.raw_mtree_sha256 != "-") ||
           !artifact_is_valid(artifact, request.package_base, legacy) ||
           !checked_add(artifact.artifact_size, aggregate_size) ||
           !checked_add(artifact.signature_size, aggregate_size)) {
            return false;
        }
        const std::size_t artifact_protocol_size =
            artifact.package_name.size() + artifact.full_version.size() +
            artifact.package_base.size() + artifact.architecture.size() +
            288;
        if(protocol_size >
               SOURCE_ARTIFACT_INSTALL_MAXIMUM_PROTOCOL_BYTES ||
           artifact_protocol_size >
               SOURCE_ARTIFACT_INSTALL_MAXIMUM_PROTOCOL_BYTES -
                   protocol_size) {
            return false;
        }
        protocol_size += artifact_protocol_size;
        for(std::size_t prior = 0; prior < index; ++prior) {
            if(request.artifacts[prior].artifact_index ==
                   artifact.artifact_index ||
               request.artifacts[prior].package_name ==
                   artifact.package_name) {
                return false;
            }
        }
    }
    return true;
}

SourceArtifactInstallTrustedHelperInvocationResult
parse_source_artifact_install_trusted_helper_arguments(
    const std::vector<std::string>& input_arguments) {
    auto arguments = input_arguments;
    std::string legacy_input_path;
    if(!arguments.empty() && arguments[0] == "install-legacy") {
        if(arguments.size() < 3) return fail<SourceArtifactInstallTrustedHelperInvocation>(SourceArtifactInstallTrustedProtocolIssueKind::InvalidArgumentCount);
        const auto pid = parse_canonical_unsigned<unsigned int>(arguments[1]);
        const auto fd = parse_canonical_unsigned<unsigned int>(arguments[2]);
        if(!pid || *pid == 0 || !fd || *pid > 2147483647U || *fd > 2147483647U)
            return fail<SourceArtifactInstallTrustedHelperInvocation>(SourceArtifactInstallTrustedProtocolIssueKind::UnexpectedRecord);
        legacy_input_path = "/proc/" + arguments[1] + "/fd/" + arguments[2];
        arguments.erase(arguments.begin() + 1, arguments.begin() + 3);
    }
    if(arguments.empty()) {
        return fail<SourceArtifactInstallTrustedHelperInvocation>(
            SourceArtifactInstallTrustedProtocolIssueKind::
                InvalidArgumentCount);
    }

    SourceArtifactInstallTrustedHelperCommand command;
    if(arguments[0] == "install-legacy") {
        command = SourceArtifactInstallTrustedHelperCommand::InstallLegacy;
    } else if(arguments[0] == "prepare") {
        command = SourceArtifactInstallTrustedHelperCommand::Prepare;
    } else if(arguments[0] == "prepare-exact") {
        command = SourceArtifactInstallTrustedHelperCommand::PrepareExact;
    } else if(arguments[0] == "execute") {
        command = SourceArtifactInstallTrustedHelperCommand::Execute;
    } else if(arguments[0] == "execution-status") {
        command = SourceArtifactInstallTrustedHelperCommand::ExecutionStatus;
    } else if(arguments[0] == "observe-execution") {
        command = SourceArtifactInstallTrustedHelperCommand::ObserveExecution;
    } else if(arguments[0] == "record") {
        command = SourceArtifactInstallTrustedHelperCommand::Record;
    } else if(arguments[0] == "record-install") {
        command = SourceArtifactInstallTrustedHelperCommand::RecordInstall;
    } else if(arguments[0] == "record-upgrade") {
        command = SourceArtifactInstallTrustedHelperCommand::RecordUpgrade;
    } else if(arguments[0] == "consume-exact") {
        command = SourceArtifactInstallTrustedHelperCommand::ConsumeExact;
    } else if(arguments[0] == "consume") {
        command = SourceArtifactInstallTrustedHelperCommand::Consume;
    } else if(arguments[0] == "abort") {
        command = SourceArtifactInstallTrustedHelperCommand::Abort;
    } else {
        return fail<SourceArtifactInstallTrustedHelperInvocation>(
            SourceArtifactInstallTrustedProtocolIssueKind::InvalidCommand);
    }

    if(arguments.size() < 2 ||
       !is_valid_trusted_alpm_receipt_token(arguments[1])) {
        return fail<SourceArtifactInstallTrustedHelperInvocation>(
            arguments.size() < 2
                ? SourceArtifactInstallTrustedProtocolIssueKind::
                      InvalidArgumentCount
                : SourceArtifactInstallTrustedProtocolIssueKind::
                      InvalidTransactionToken);
    }
    const bool legacy = command == SourceArtifactInstallTrustedHelperCommand::InstallLegacy;
    const bool exact = command == SourceArtifactInstallTrustedHelperCommand::PrepareExact;
    if(command != SourceArtifactInstallTrustedHelperCommand::Prepare && !exact && !legacy) {
        if(arguments.size() != 2) {
            return fail<SourceArtifactInstallTrustedHelperInvocation>(
                SourceArtifactInstallTrustedProtocolIssueKind::
                    InvalidArgumentCount);
        }
        return SourceArtifactInstallTrustedHelperInvocation{
            command, arguments[1], {}, {}, SourceArtifactInstallTrustedDirective::PreserveExistingReason, false, false};
    }

    if(arguments.size() < 16) {
        return fail<SourceArtifactInstallTrustedHelperInvocation>(
            SourceArtifactInstallTrustedProtocolIssueKind::EmptyArtifactSet);
    }
    if(!is_valid_package_name(arguments[2])) {
        return fail<SourceArtifactInstallTrustedHelperInvocation>(
            SourceArtifactInstallTrustedProtocolIssueKind::
                InvalidPackageBase);
    }
    const auto directive = parse_directive(arguments[3]);
    if(!directive.has_value() || (!legacy && *directive == SourceArtifactInstallTrustedDirective::AsExplicit)) {
        return fail<SourceArtifactInstallTrustedHelperInvocation>(
            SourceArtifactInstallTrustedProtocolIssueKind::InvalidDirective);
    }
    const auto needed = parse_boolean(arguments[4]);
    const auto no_confirm = parse_boolean(arguments[5]);
    if(!needed.has_value() || !no_confirm.has_value()) {
        return fail<SourceArtifactInstallTrustedHelperInvocation>(
            SourceArtifactInstallTrustedProtocolIssueKind::InvalidBoolean);
    }
    if(arguments[6] != "--") {
        return fail<SourceArtifactInstallTrustedHelperInvocation>(
            SourceArtifactInstallTrustedProtocolIssueKind::
                MissingArgumentSeparator);
    }
    const std::size_t stride = exact ? 10 : 9;
    if((arguments.size() - 7) % stride != 0) {
        return fail<SourceArtifactInstallTrustedHelperInvocation>(
            SourceArtifactInstallTrustedProtocolIssueKind::
                InvalidArgumentCount);
    }
    const std::size_t artifact_count = (arguments.size() - 7) / stride;
    if(artifact_count == 0) {
        return fail<SourceArtifactInstallTrustedHelperInvocation>(
            SourceArtifactInstallTrustedProtocolIssueKind::EmptyArtifactSet);
    }
    if(artifact_count > SOURCE_ARTIFACT_INSTALL_MAXIMUM_ARTIFACTS) {
        return fail<SourceArtifactInstallTrustedHelperInvocation>(
            SourceArtifactInstallTrustedProtocolIssueKind::TooManyArtifacts);
    }

    std::vector<std::string_view> fields;
    fields.reserve(arguments.size() - 7);
    for(std::size_t index = 7; index < arguments.size(); ++index) {
        fields.emplace_back(arguments[index]);
    }
    std::vector<SourceArtifactInstallRootArtifactExpectation> artifacts;
    artifacts.reserve(artifact_count);
    for(std::size_t index = 0; index < artifact_count; ++index) {
        if(!is_valid_source_artifact_install_sha256(fields[index * stride + 7]) ||
           (fields[index * stride + 8] != "-" &&
            !is_valid_source_artifact_install_sha256(fields[index * stride + 8]))) {
            return fail<SourceArtifactInstallTrustedHelperInvocation>(
                SourceArtifactInstallTrustedProtocolIssueKind::InvalidDigest);
        }
        auto artifact = parse_artifact_fields(
            fields, index * stride, arguments[2], legacy);
        if(!artifact.has_value()) {
            return fail<SourceArtifactInstallTrustedHelperInvocation>(
                SourceArtifactInstallTrustedProtocolIssueKind::
                    UnexpectedRecord);
        }
        if(exact) artifact->raw_mtree_sha256 = std::string(fields[index * stride + 9]);
        for(const auto& prior : artifacts) {
            if(prior.artifact_index == artifact->artifact_index)
                return fail<SourceArtifactInstallTrustedHelperInvocation>(
                    SourceArtifactInstallTrustedProtocolIssueKind::DuplicateArtifactIndex);
            if(prior.package_name == artifact->package_name)
                return fail<SourceArtifactInstallTrustedHelperInvocation>(
                    SourceArtifactInstallTrustedProtocolIssueKind::DuplicatePackageName);
        }
        artifacts.push_back(*artifact);
    }
    SourceArtifactInstallRootPrepareRequest request{
        arguments[1], arguments[2], *directive, *needed, *no_confirm,
        artifacts};
    if(legacy) request.purpose = SourceArtifactInstallTrustedPurpose::LegacyArtifactInstall;
    if(exact) request.purpose = SourceArtifactInstallTrustedPurpose::ExactInstalledBinding;
    if(!is_valid_source_artifact_install_root_request(request)) {
        return fail<SourceArtifactInstallTrustedHelperInvocation>(
            SourceArtifactInstallTrustedProtocolIssueKind::
                UnexpectedRecord);
    }
    return SourceArtifactInstallTrustedHelperInvocation{
        command, request.transaction_token, std::move(artifacts),
        request.package_base, request.directive, request.needed,
        request.no_confirm, legacy_input_path};
}

std::string serialize_source_artifact_install_root_prepared_state(
    const SourceArtifactInstallRootPrepareRequest& request) {
    if(!is_valid_source_artifact_install_root_request(request)) {
        throw std::invalid_argument(
            "invalid source-artifact prepared request");
    }
    if(request.purpose == SourceArtifactInstallTrustedPurpose::ExactInstalledBinding) {
        // The enclosed v2 projection is explicitly legacy-only. Exact state
        // requires its own purpose and every raw MTREE hash; decoding never
        // supplies missing authority or upgrades a legacy document.
        auto cleanup_projection = request;
        cleanup_projection.purpose = SourceArtifactInstallTrustedPurpose::CleanupInstallOnly;
        for(auto& artifact : cleanup_projection.artifacts)
            artifact.raw_mtree_sha256 = "-";
        std::string protocol(EXACT_PREPARED_PREFIX);
        for(const auto& artifact : request.artifacts)
            protocol += "MTREE\t" + std::to_string(artifact.artifact_index) + "\t" + artifact.raw_mtree_sha256 + "\n";
        protocol += serialize_source_artifact_install_root_prepared_state(cleanup_projection);
        if(protocol.size() > SOURCE_ARTIFACT_INSTALL_MAXIMUM_PROTOCOL_BYTES)
            throw std::invalid_argument("oversized exact prepared protocol");
        return protocol;
    }
    std::string protocol;
    protocol.reserve(256 + request.artifacts.size() * 128);
    protocol.append(request.purpose == SourceArtifactInstallTrustedPurpose::LegacyArtifactInstall ? LEGACY_PREPARED_HEADER : PREPARED_HEADER).push_back('\n');
    protocol.append(TOKEN_PREFIX).append(request.transaction_token).push_back('\n');
    protocol.append(OWNER_PREFIX).append(OWNER).push_back('\n');
    protocol.append(PACKAGE_BASE_PREFIX).append(request.package_base).push_back('\n');
    protocol.append(DIRECTIVE_PREFIX)
        .append(serialize_directive(request.directive))
        .push_back('\n');
    protocol.append(NEEDED_PREFIX)
        .append(request.needed ? "1" : "0")
        .push_back('\n');
    protocol.append(NO_CONFIRM_PREFIX)
        .append(request.no_confirm ? "1" : "0")
        .push_back('\n');
    for(const auto& artifact : request.artifacts) {
        append_artifact_record(protocol, artifact);
    }
    protocol.append(END_RECORD).push_back('\n');
    if(protocol.size() >
       SOURCE_ARTIFACT_INSTALL_MAXIMUM_PROTOCOL_BYTES) {
        throw std::invalid_argument(
            "oversized source-artifact prepared protocol");
    }
    return protocol;
}

SourceArtifactInstallRootPreparedStateResult
parse_source_artifact_install_root_prepared_state(
    std::string_view protocol) {
    if(protocol.starts_with(EXACT_PREPARED_PREFIX)) {
        if(protocol.size() > SOURCE_ARTIFACT_INSTALL_MAXIMUM_PROTOCOL_BYTES)
            return fail<SourceArtifactInstallRootPrepareRequest>(SourceArtifactInstallTrustedProtocolIssueKind::InputTooLarge);
        const auto legacy_offset = protocol.find(PREPARED_HEADER, EXACT_PREPARED_PREFIX.size());
        if(legacy_offset == std::string_view::npos)
            return fail<SourceArtifactInstallRootPrepareRequest>(SourceArtifactInstallTrustedProtocolIssueKind::TruncatedProtocol);
        auto parsed = parse_source_artifact_install_root_prepared_state(protocol.substr(legacy_offset));
        auto* request = std::get_if<SourceArtifactInstallRootPrepareRequest>(&parsed);
        if(!request) return parsed;
        std::size_t offset = EXACT_PREPARED_PREFIX.size();
        for(auto& artifact : request->artifacts) {
            const auto end = protocol.find('\n', offset);
            if(end == std::string_view::npos || end >= legacy_offset)
                return fail<SourceArtifactInstallRootPrepareRequest>(SourceArtifactInstallTrustedProtocolIssueKind::TruncatedProtocol);
            const auto prefix = "MTREE\t" + std::to_string(artifact.artifact_index) + "\t";
            const auto value = record_value(protocol.substr(offset, end - offset), prefix);
            if(!value || !is_valid_source_artifact_install_sha256(*value))
                return fail<SourceArtifactInstallRootPrepareRequest>(SourceArtifactInstallTrustedProtocolIssueKind::InvalidDigest);
            artifact.raw_mtree_sha256 = std::string(*value);
            offset = end + 1;
        }
        if(offset != legacy_offset)
            return fail<SourceArtifactInstallRootPrepareRequest>(SourceArtifactInstallTrustedProtocolIssueKind::UnexpectedRecord);
        request->purpose = SourceArtifactInstallTrustedPurpose::ExactInstalledBinding;
        if(serialize_source_artifact_install_root_prepared_state(*request) != protocol)
            return fail<SourceArtifactInstallRootPrepareRequest>(SourceArtifactInstallTrustedProtocolIssueKind::UnexpectedRecord);
        return parsed;
    }
    const auto split = split_protocol_lines(protocol);
    if(const auto* failure =
           std::get_if<SourceArtifactInstallTrustedProtocolFailure>(&split);
       failure != nullptr) {
        return *failure;
    }
    const auto& lines = std::get<std::vector<std::string_view>>(split);
    if(lines.size() < 9) {
        return fail<SourceArtifactInstallRootPrepareRequest>(
            SourceArtifactInstallTrustedProtocolIssueKind::
                TruncatedProtocol);
    }
    const bool legacy = lines[0] == LEGACY_PREPARED_HEADER;
    if((lines[0] != PREPARED_HEADER && !legacy) || lines.back() != END_RECORD) {
        return fail<SourceArtifactInstallRootPrepareRequest>(
            lines[0] != PREPARED_HEADER
                ? SourceArtifactInstallTrustedProtocolIssueKind::
                      InvalidHeader
                : SourceArtifactInstallTrustedProtocolIssueKind::
                      TruncatedProtocol);
    }
    const auto token = record_value(lines[1], TOKEN_PREFIX);
    const auto owner = record_value(lines[2], OWNER_PREFIX);
    const auto package_base = record_value(lines[3], PACKAGE_BASE_PREFIX);
    const auto directive_value = record_value(lines[4], DIRECTIVE_PREFIX);
    const auto needed_value = record_value(lines[5], NEEDED_PREFIX);
    const auto no_confirm_value = record_value(lines[6], NO_CONFIRM_PREFIX);
    if(!token.has_value() ||
       !is_valid_trusted_alpm_receipt_token(*token) ||
       !owner.has_value() || *owner != OWNER ||
       !package_base.has_value() ||
       !is_valid_package_name(std::string(*package_base)) ||
       !directive_value.has_value() || !needed_value.has_value() ||
       !no_confirm_value.has_value()) {
        return fail<SourceArtifactInstallRootPrepareRequest>(
            SourceArtifactInstallTrustedProtocolIssueKind::
                UnexpectedRecord);
    }
    const auto directive = parse_directive(*directive_value);
    const auto needed = parse_boolean(*needed_value);
    const auto no_confirm = parse_boolean(*no_confirm_value);
    if(!directive.has_value() || !needed.has_value() ||
       !no_confirm.has_value()) {
        return fail<SourceArtifactInstallRootPrepareRequest>(
            SourceArtifactInstallTrustedProtocolIssueKind::
                UnexpectedRecord);
    }

    std::vector<SourceArtifactInstallRootArtifactExpectation> artifacts;
    artifacts.reserve(lines.size() - 8);
    for(std::size_t index = 7; index + 1 < lines.size(); ++index) {
        const auto fields = split_tabs(lines[index]);
        if(fields.size() != 10 || fields[0] != "ARTIFACT") {
            return fail<SourceArtifactInstallRootPrepareRequest>(
                SourceArtifactInstallTrustedProtocolIssueKind::
                    UnexpectedRecord);
        }
        if(!is_valid_source_artifact_install_sha256(fields[8]) ||
           (fields[9] != "-" && !is_valid_source_artifact_install_sha256(fields[9]))) {
            return fail<SourceArtifactInstallRootPrepareRequest>(
                SourceArtifactInstallTrustedProtocolIssueKind::InvalidDigest);
        }
        const auto artifact = parse_artifact_fields(
            fields, 1, *package_base, legacy);
        if(!artifact.has_value()) {
            return fail<SourceArtifactInstallRootPrepareRequest>(
                SourceArtifactInstallTrustedProtocolIssueKind::
                    UnexpectedRecord);
        }
        artifacts.push_back(*artifact);
    }
    SourceArtifactInstallRootPrepareRequest request{
        std::string(*token), std::string(*package_base), *directive,
        *needed, *no_confirm, std::move(artifacts)};
    if(legacy) request.purpose = SourceArtifactInstallTrustedPurpose::LegacyArtifactInstall;
    if(!is_valid_source_artifact_install_root_request(request)) {
        return fail<SourceArtifactInstallRootPrepareRequest>(
            SourceArtifactInstallTrustedProtocolIssueKind::
                UnexpectedRecord);
    }
    return request;
}

std::string source_artifact_install_hook_directory(
    std::string_view transaction_token) {
    if(!is_valid_trusted_alpm_receipt_token(transaction_token)) {
        throw std::invalid_argument(
            "invalid source-artifact transaction token");
    }
    return "/run/moguet/source-artifact-installs/active/" +
           std::string(transaction_token) + "/hooks";
}

std::string source_artifact_install_hook_filename(
    std::string_view transaction_token) {
    if(!is_valid_trusted_alpm_receipt_token(transaction_token)) {
        throw std::invalid_argument(
            "invalid source-artifact transaction token");
    }
    return "moguet-source-install-" + std::string(transaction_token) +
           ".hook";
}

std::string source_artifact_install_staged_artifact_path(
    std::string_view transaction_token, std::size_t ordinal) {
    if(!is_valid_trusted_alpm_receipt_token(transaction_token) ||
       ordinal >= SOURCE_ARTIFACT_INSTALL_MAXIMUM_ARTIFACTS) {
        throw std::invalid_argument(
            "invalid source-artifact staging identity");
    }
    return "/run/moguet/source-artifact-installs/active/" +
           std::string(transaction_token) + "/artifacts/artifact-" +
           std::to_string(ordinal) + ".pkg.tar.zst";
}

std::string source_artifact_install_staged_signature_path(
    std::string_view transaction_token, std::size_t ordinal) {
    return source_artifact_install_staged_artifact_path(
               transaction_token, ordinal) +
           ".sig";
}

std::string serialize_source_artifact_install_root_prepare_response(
    const SourceArtifactInstallRootPrepareResponse& response,
    const SourceArtifactInstallRootPrepareRequest& request) {
    if(!is_valid_source_artifact_install_root_request(request) ||
       response.transaction_token != request.transaction_token ||
       response.hook_directory != source_artifact_install_hook_directory(
                                      request.transaction_token) ||
       response.artifacts.size() != request.artifacts.size()) {
        throw std::invalid_argument(
            "invalid source-artifact prepare response");
    }
    std::string protocol;
    if(request.purpose == SourceArtifactInstallTrustedPurpose::ExactInstalledBinding) {
        if(!is_valid_source_artifact_install_sha256(response.staged_identity_sha256))
            throw std::invalid_argument("exact prepare response lacks its staged identity");
        protocol.append(EXACT_RESPONSE_PREFIX).append("STAGE\t").append(response.staged_identity_sha256).push_back('\n');
    } else if(!response.staged_identity_sha256.empty()) {
        throw std::invalid_argument("cleanup response contains exact authority");
    }
    protocol.append(PREPARE_RESPONSE_HEADER).push_back('\n');
    protocol.append(TOKEN_PREFIX).append(response.transaction_token).push_back('\n');
    protocol.append(OWNER_PREFIX).append(OWNER).push_back('\n');
    protocol.append(HOOK_DIRECTORY_PREFIX)
        .append(response.hook_directory)
        .push_back('\n');
    for(std::size_t index = 0; index < response.artifacts.size(); ++index) {
        const auto& staged = response.artifacts[index];
        if(staged.artifact_index != request.artifacts[index].artifact_index ||
           staged.path != source_artifact_install_staged_artifact_path(
                              request.transaction_token, index)) {
            throw std::invalid_argument(
                "invalid source-artifact staged response");
        }
        protocol.append(STAGED_PREFIX)
            .append(std::to_string(staged.artifact_index))
            .push_back('\t');
        protocol.append(staged.path).push_back('\n');
    }
    protocol.append(END_RECORD).push_back('\n');
    return protocol;
}

SourceArtifactInstallRootPrepareResponseResult
parse_source_artifact_install_root_prepare_response(
    std::string_view protocol,
    const SourceArtifactInstallRootPrepareRequest& expected_request) {
    if(!is_valid_source_artifact_install_root_request(expected_request)) {
        return fail<SourceArtifactInstallRootPrepareResponse>(
            SourceArtifactInstallTrustedProtocolIssueKind::
                InvalidTransactionToken);
    }
    if(expected_request.purpose == SourceArtifactInstallTrustedPurpose::ExactInstalledBinding) {
        const auto prefix = std::string(EXACT_RESPONSE_PREFIX) + "STAGE\t";
        if(protocol.size() > SOURCE_ARTIFACT_INSTALL_MAXIMUM_PROTOCOL_BYTES ||
           !protocol.starts_with(prefix) || protocol.size() <= prefix.size() + 64 ||
           protocol[prefix.size() + 64] != '\n' ||
           !is_valid_source_artifact_install_sha256(protocol.substr(prefix.size(), 64)))
            return fail<SourceArtifactInstallRootPrepareResponse>(SourceArtifactInstallTrustedProtocolIssueKind::UnexpectedRecord);
        auto cleanup_projection = expected_request;
        cleanup_projection.purpose = SourceArtifactInstallTrustedPurpose::CleanupInstallOnly;
        for(auto& artifact : cleanup_projection.artifacts)
            artifact.raw_mtree_sha256 = "-";
        auto parsed = parse_source_artifact_install_root_prepare_response(protocol.substr(prefix.size() + 65), cleanup_projection);
        if(auto* response = std::get_if<SourceArtifactInstallRootPrepareResponse>(&parsed))
            response->staged_identity_sha256 = std::string(protocol.substr(prefix.size(), 64));
        return parsed;
    }
    const auto split = split_protocol_lines(protocol);
    if(const auto* failure =
           std::get_if<SourceArtifactInstallTrustedProtocolFailure>(&split);
       failure != nullptr) {
        return *failure;
    }
    const auto& lines = std::get<std::vector<std::string_view>>(split);
    const std::size_t expected_lines =
        expected_request.artifacts.size() + 5;
    if(lines.size() != expected_lines ||
       lines[0] != PREPARE_RESPONSE_HEADER ||
       lines.back() != END_RECORD) {
        return fail<SourceArtifactInstallRootPrepareResponse>(
            lines.size() < expected_lines
                ? SourceArtifactInstallTrustedProtocolIssueKind::
                      TruncatedProtocol
                : SourceArtifactInstallTrustedProtocolIssueKind::
                      UnexpectedRecord);
    }
    const auto token = record_value(lines[1], TOKEN_PREFIX);
    const auto owner = record_value(lines[2], OWNER_PREFIX);
    const auto hook = record_value(lines[3], HOOK_DIRECTORY_PREFIX);
    const std::string expected_hook =
        source_artifact_install_hook_directory(
            expected_request.transaction_token);
    if(!token.has_value() ||
       *token != expected_request.transaction_token ||
       !owner.has_value() || *owner != OWNER || !hook.has_value() ||
       *hook != expected_hook) {
        return fail<SourceArtifactInstallRootPrepareResponse>(
            SourceArtifactInstallTrustedProtocolIssueKind::
                UnexpectedRecord);
    }

    std::vector<SourceArtifactInstallStagedArtifact> staged_artifacts;
    staged_artifacts.reserve(expected_request.artifacts.size());
    for(std::size_t index = 0;
        index < expected_request.artifacts.size(); ++index) {
        const auto fields = split_tabs(lines[index + 4]);
        if(fields.size() != 3 || fields[0] != "STAGED") {
            return fail<SourceArtifactInstallRootPrepareResponse>(
                SourceArtifactInstallTrustedProtocolIssueKind::
                    UnexpectedRecord);
        }
        const auto artifact_index =
            parse_canonical_unsigned<std::size_t>(fields[1]);
        const std::string expected_path =
            source_artifact_install_staged_artifact_path(
                expected_request.transaction_token, index);
        if(!artifact_index.has_value() ||
           *artifact_index !=
               expected_request.artifacts[index].artifact_index ||
           fields[2] != expected_path) {
            return fail<SourceArtifactInstallRootPrepareResponse>(
                SourceArtifactInstallTrustedProtocolIssueKind::
                    InvalidStagedArtifactPath);
        }
        staged_artifacts.push_back(
            SourceArtifactInstallStagedArtifact{
                *artifact_index, expected_path});
    }
    return SourceArtifactInstallRootPrepareResponse{
        std::string(*token), std::string(*hook),
        std::move(staged_artifacts)};
}

std::string serialize_source_artifact_install_root_receipt(
    const SourceArtifactInstallRootReceipt& receipt) {
    if(!is_valid_trusted_alpm_receipt_token(receipt.transaction_token) ||
       (receipt.state == SourceArtifactInstallRootReceiptState::Missing &&
        !receipt.installed_package_names.empty()) ||
       (receipt.state == SourceArtifactInstallRootReceiptState::Complete &&
        receipt.installed_package_names.empty())) {
        throw std::invalid_argument(
            "invalid source-artifact receipt");
    }
    for(std::size_t index = 0;
        index < receipt.installed_package_names.size(); ++index) {
        const std::string& package_name =
            receipt.installed_package_names[index];
        if(!is_valid_trusted_alpm_receipt_package_name(package_name)) {
            throw std::invalid_argument(
                "invalid source-artifact receipt package");
        }
        if(std::find(
               receipt.installed_package_names.begin(),
               receipt.installed_package_names.begin() +
                   static_cast<std::ptrdiff_t>(index),
               package_name) !=
           receipt.installed_package_names.begin() +
               static_cast<std::ptrdiff_t>(index)) {
            throw std::invalid_argument(
                "duplicate source-artifact receipt package");
        }
    }
    std::string protocol;
    protocol.append(RECEIPT_HEADER).push_back('\n');
    protocol.append(TOKEN_PREFIX).append(receipt.transaction_token).push_back('\n');
    protocol.append(OWNER_PREFIX).append(OWNER).push_back('\n');
    protocol.append(STATE_PREFIX)
        .append(
            receipt.state == SourceArtifactInstallRootReceiptState::Complete
                ? "Complete"
                : "Missing")
        .push_back('\n');
    for(const std::string& package_name :
        receipt.installed_package_names) {
        protocol.append(INSTALL_PREFIX).append(package_name).push_back('\n');
    }
    protocol.append(END_RECORD).push_back('\n');
    if(protocol.size() >
       SOURCE_ARTIFACT_INSTALL_MAXIMUM_PROTOCOL_BYTES) {
        throw std::invalid_argument(
            "oversized source-artifact receipt protocol");
    }
    return protocol;
}

SourceArtifactInstallRootReceiptResult
parse_source_artifact_install_root_receipt(std::string_view protocol) {
    const auto split = split_protocol_lines(protocol);
    if(const auto* failure =
           std::get_if<SourceArtifactInstallTrustedProtocolFailure>(&split);
       failure != nullptr) {
        return *failure;
    }
    const auto& lines = std::get<std::vector<std::string_view>>(split);
    if(lines.size() < 5 || lines[0] != RECEIPT_HEADER ||
       lines.back() != END_RECORD) {
        return fail<SourceArtifactInstallRootReceipt>(
            lines.size() < 5
                ? SourceArtifactInstallTrustedProtocolIssueKind::
                      TruncatedProtocol
                : SourceArtifactInstallTrustedProtocolIssueKind::
                      InvalidHeader);
    }
    const auto token = record_value(lines[1], TOKEN_PREFIX);
    const auto owner = record_value(lines[2], OWNER_PREFIX);
    const auto state_value = record_value(lines[3], STATE_PREFIX);
    if(!token.has_value() ||
       !is_valid_trusted_alpm_receipt_token(*token) ||
       !owner.has_value() || *owner != OWNER ||
       !state_value.has_value()) {
        return fail<SourceArtifactInstallRootReceipt>(
            SourceArtifactInstallTrustedProtocolIssueKind::
                UnexpectedRecord);
    }
    SourceArtifactInstallRootReceiptState state;
    if(*state_value == "Complete") {
        state = SourceArtifactInstallRootReceiptState::Complete;
    } else if(*state_value == "Missing") {
        state = SourceArtifactInstallRootReceiptState::Missing;
    } else {
        return fail<SourceArtifactInstallRootReceipt>(
            SourceArtifactInstallTrustedProtocolIssueKind::
                InvalidReceiptState);
    }

    std::vector<std::string> packages;
    packages.reserve(lines.size() - 5);
    for(std::size_t index = 4; index + 1 < lines.size(); ++index) {
        const auto package = record_value(lines[index], INSTALL_PREFIX);
        if(!package.has_value() ||
           !is_valid_trusted_alpm_receipt_package_name(*package) ||
           std::find(packages.begin(), packages.end(), *package) !=
               packages.end()) {
            return fail<SourceArtifactInstallRootReceipt>(
                SourceArtifactInstallTrustedProtocolIssueKind::
                    UnexpectedRecord);
        }
        packages.emplace_back(*package);
    }
    if((state == SourceArtifactInstallRootReceiptState::Complete &&
        packages.empty()) ||
       (state == SourceArtifactInstallRootReceiptState::Missing &&
        !packages.empty())) {
        return fail<SourceArtifactInstallRootReceipt>(
            SourceArtifactInstallTrustedProtocolIssueKind::
                InvalidReceiptState);
    }
    return SourceArtifactInstallRootReceipt{
        state, std::string(*token), std::move(packages)};
}

bool is_valid_source_artifact_install_sha256(std::string_view digest) noexcept {
    return digest.size() == 64 && std::all_of(digest.begin(), digest.end(),
                                              [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); });
}

namespace {
constexpr std::string_view EXECUTION_HEADER = "MOGUET-SOURCE-ARTIFACT-EXECUTION\t3";
constexpr std::string_view EXECUTION_EVIDENCE[] = {"Unobserved", "PreTransaction", "PostTransaction"};
constexpr std::string_view SEALING_REASONS[] = {
    "StagedArtifactDigestMismatch", "StagedArtifactGenerationMismatch",
    "StagedArtifactReplacement", "StagedArtifactRevalidationFailure",
    "SignatureDigestMismatch", "TrustedTransportProtocolMismatch",
    "ExecutableLaunchFailure", "TransactionLifetimeBusy"};
} // namespace

std::string serialize_source_artifact_install_execution_observation(
    const SourceArtifactInstallExecutionObservation& observation) {
    const auto evidence_index = static_cast<std::size_t>(observation.execution_evidence);
    if(!is_valid_trusted_alpm_receipt_token(observation.transaction_token) ||
       evidence_index >= std::size(EXECUTION_EVIDENCE) ||
       (observation.execution_evidence != SourceArtifactInstallExecutionEvidence::Unobserved &&
        (!observation.authorized || observation.refusal))) {
        throw std::invalid_argument("invalid execution observation token");
    }
    std::string reason = "None";
    int error_number = 0;
    if(observation.refusal) {
        const auto index = static_cast<std::size_t>(observation.refusal->reason);
        if(index >= std::size(SEALING_REASONS) || observation.refusal->error_number < 0) {
            throw std::invalid_argument("invalid sealing refusal");
        }
        reason = SEALING_REASONS[index];
        error_number = observation.refusal->error_number;
    }
    return std::string(EXECUTION_HEADER) + "\nTOKEN\t" + observation.transaction_token +
           "\nOWNER\t" + std::string(OWNER) + "\nAUTHORIZED\t" +
           (observation.authorized ? "1" : "0") + "\nEXECUTION\t" + std::string(EXECUTION_EVIDENCE[evidence_index]) +
           "\nREASON\t" + reason +
           "\nERRNO\t" + std::to_string(error_number) + "\nEND\n";
}

std::variant<SourceArtifactInstallExecutionObservation, SourceArtifactInstallTrustedProtocolFailure>
parse_source_artifact_install_execution_observation(std::string_view protocol) {
    const auto split = split_protocol_lines(protocol);
    if(const auto* failure = std::get_if<SourceArtifactInstallTrustedProtocolFailure>(&split)) return *failure;
    const auto& lines = std::get<std::vector<std::string_view>>(split);
    const auto malformed = SourceArtifactInstallTrustedProtocolFailure{
        SourceArtifactInstallTrustedProtocolIssueKind::UnexpectedRecord};
    if(lines.size() != 8 || lines[0] != EXECUTION_HEADER || lines[7] != END_RECORD) return malformed;
    const auto token = record_value(lines[1], TOKEN_PREFIX);
    const auto owner = record_value(lines[2], OWNER_PREFIX);
    const auto authorized_text = record_value(lines[3], "AUTHORIZED\t");
    const auto evidence_text = record_value(lines[4], "EXECUTION\t");
    const auto reason_text = record_value(lines[5], "REASON\t");
    const auto errno_text = record_value(lines[6], "ERRNO\t");
    if(!token || !is_valid_trusted_alpm_receipt_token(*token) ||
       !owner || *owner != OWNER || !authorized_text || !evidence_text || !reason_text || !errno_text) return malformed;
    const auto authorized = parse_boolean(*authorized_text);
    const auto error_number = parse_canonical_unsigned<unsigned>(*errno_text);
    if(!authorized || !error_number || *error_number > static_cast<unsigned>(std::numeric_limits<int>::max())) return malformed;
    SourceArtifactInstallExecutionObservation result{std::string(*token), *authorized, std::nullopt};
    const auto evidence = std::find(std::begin(EXECUTION_EVIDENCE), std::end(EXECUTION_EVIDENCE), *evidence_text);
    if(evidence == std::end(EXECUTION_EVIDENCE)) return malformed;
    result.execution_evidence = static_cast<SourceArtifactInstallExecutionEvidence>(evidence - std::begin(EXECUTION_EVIDENCE));
    if(result.execution_evidence != SourceArtifactInstallExecutionEvidence::Unobserved &&
       (!result.authorized || *reason_text != "None")) return malformed;
    if(*reason_text == "None") {
        if(*error_number != 0) return malformed;
        return result;
    }
    for(std::size_t i = 0; i < std::size(SEALING_REASONS); ++i) {
        if(*reason_text == SEALING_REASONS[i]) {
            result.refusal = SourceArtifactInstallSealingRefusal{
                static_cast<SourceArtifactInstallSealingFailure>(i), static_cast<int>(*error_number)};
            return result;
        }
    }
    return malformed;
}
