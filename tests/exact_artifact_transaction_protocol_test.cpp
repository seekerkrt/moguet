#include "exact_artifact_transaction_protocol.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>

namespace {

using Operation = ExactArtifactTransactionOperation;
using Issue = ExactArtifactReceiptIssue;
const std::string TOKEN(64, 'a');
const std::string STAGE(64, 'b');

SourceArtifactInstallRootPrepareRequest manifest() {
    return {TOKEN, "base", SourceArtifactInstallTrustedDirective::PreserveExistingReason, false, true, {{1, "one", "1:2-3", "base", "any", 12, 0, std::string(64, 'c'), "-", std::string(64, 'd')}, {4, "four", "1:2-3", "base", "any", 14, 0, std::string(64, 'e'), "-", std::string(64, 'f')}}, SourceArtifactInstallTrustedPurpose::ExactInstalledBinding};
}

template <typename Function>
void rejects(Function function) {
    bool rejected = false;
    try {
        function();
    } catch(const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}

std::string replace(std::string text, const std::string& from, const std::string& to) {
    const auto position = text.find(from);
    assert(position != std::string::npos);
    text.replace(position, from.size(), to);
    return text;
}

void protocol_matrix() {
    const auto expected = manifest();
    const auto install = serialize_exact_artifact_operation_fragment(expected, STAGE, Operation::Install, {"one", "four"});
    assert(install == serialize_exact_artifact_operation_fragment(expected, STAGE, Operation::Install, {"four", "one"}));
    const auto upgrade = serialize_exact_artifact_operation_fragment(expected, STAGE, Operation::Upgrade, {"one", "four"});
    for(const auto operation : {Operation::Install, Operation::Upgrade}) {
        const auto& bytes = operation == Operation::Install ? install : upgrade;
        const auto parsed = parse_exact_artifact_operation_fragment(bytes, expected, STAGE, operation);
        const auto& records = std::get<ExactArtifactOperationRecords>(parsed);
        assert(records.size() == 2 && records[0].artifact.artifact_index == 1 && records[1].artifact.artifact_index == 4);
        assert(records[0].operation == operation && records[1].operation == operation);
    }
    const auto one = serialize_exact_artifact_operation_fragment(expected, STAGE, Operation::Install, {"one"});
    const auto four = serialize_exact_artifact_operation_fragment(expected, STAGE, Operation::Upgrade, {"four"});
    const auto mixed = join_exact_artifact_operation_fragments(one, four, expected, STAGE);
    const auto& records = std::get<ExactArtifactOperationRecords>(mixed);
    assert(records[0].artifact.artifact_index == 1 && records[0].operation == Operation::Install);
    assert(records[1].artifact.artifact_index == 4 && records[1].operation == Operation::Upgrade);
    assert(std::holds_alternative<ExactArtifactOperationRecords>(join_exact_artifact_operation_fragments(install, {}, expected, STAGE)));
    assert(std::holds_alternative<ExactArtifactOperationRecords>(join_exact_artifact_operation_fragments({}, upgrade, expected, STAGE)));
    assert(std::get<Issue>(join_exact_artifact_operation_fragments(one, {}, expected, STAGE)) == Issue::Missing);
    assert(std::get<Issue>(join_exact_artifact_operation_fragments({}, {}, expected, STAGE)) == Issue::Missing);
    assert(std::get<Issue>(join_exact_artifact_operation_fragments(install, four, expected, STAGE)) == Issue::Invalid);
    assert(std::get<Issue>(parse_exact_artifact_operation_fragment(install, expected, STAGE, Operation::Upgrade)) == Issue::Invalid);

    const std::vector<std::string> invalid = {
        replace(install, "OPERATIONS\t1", "OPERATIONS\t0"),
        replace(install, "OPERATIONS\t1", "OPERATIONS\t2"),
        replace(install, "MOGUET-EXACT-ARTIFACT-OPERATIONS\t1", "MOGUET-SOURCE-ARTIFACT-RECEIPT\t2"),
        replace(install, "source-artifact-install", "other-owner"),
        replace(install, "ExactInstalledBinding", "CleanupInstallOnly"),
        replace(install, TOKEN, std::string(32, 'b')),
        replace(install, "MANIFEST\t", "MANIFEST\t0"),
        replace(install, STAGE, std::string(64, 'a')),
        replace(install, "OPERATION\tInstall", "OPERATION\tReinstall"),
        replace(install, "OPERATION\tInstall", "OPERATION\tDowngrade"),
        replace(install, "SELECTED\t4\tfour", "SELECTED\t1\tfour"),
        replace(install, "SELECTED\t4\tfour", "SELECTED\t4\tone"),
        replace(install, "SELECTED\t4\tfour", "SELECTED\t0\tfour"),
        replace(install, std::string(64, 'c'), std::string(64, 'a')),
        replace(install, "1:2-3", "2-3"), replace(install, "\tbase\tany", "\tother\tany"),
        replace(install, "\tbase\tany", "\tbase\tx86_64"),
        replace(install, "OPERATION\tInstall\n", ""),
        replace(install, "OPERATION\tInstall\n", "OPERATION\tInstall\nOPERATION\tInstall\n"),
        install + "END\n", install.substr(0, install.size() - 4), install + std::string(1, '\0')};
    for(const auto& bytes : invalid)
        assert(std::get<Issue>(parse_exact_artifact_operation_fragment(bytes, expected, STAGE, Operation::Install)) == Issue::Invalid);
    rejects([&] { static_cast<void>(serialize_exact_artifact_operation_fragment(expected, STAGE, static_cast<Operation>(99), {"one"})); });
    rejects([&] { static_cast<void>(serialize_exact_artifact_operation_fragment(expected, STAGE, Operation::Install, {"one", "one"})); });
    rejects([&] { static_cast<void>(serialize_exact_artifact_operation_fragment(expected, STAGE, Operation::Install, {"unselected"})); });
}

void prepared_purpose_matrix() {
    auto request = manifest();
    const auto exact = serialize_source_artifact_install_root_prepared_state(request);
    assert(std::get<SourceArtifactInstallRootPrepareRequest>(parse_source_artifact_install_root_prepared_state(exact)) == request);
    for(const auto& bytes : {replace(exact, "PREPARED\t1", "PREPARED\t0"),
                             replace(exact, "PREPARED\t1", "PREPARED\t2"),
                             replace(exact, "PURPOSE\tExactInstalledBinding\n", ""),
                             replace(exact, "ExactInstalledBinding", "CleanupInstallOnly"),
                             replace(exact, "MTREE\t4", "MTREE\t1"), exact + "END\n"})
        assert(std::holds_alternative<SourceArtifactInstallTrustedProtocolFailure>(parse_source_artifact_install_root_prepared_state(bytes)));
    auto duplicate = request;
    duplicate.artifacts[1].artifact_index = 1;
    assert(!is_valid_source_artifact_install_root_request(duplicate));
    duplicate = request;
    duplicate.artifacts[1].package_name = "one";
    assert(!is_valid_source_artifact_install_root_request(duplicate));
    request.purpose = SourceArtifactInstallTrustedPurpose::CleanupInstallOnly;
    for(auto& artifact : request.artifacts)
        artifact.raw_mtree_sha256 = "-";
    const auto legacy = serialize_source_artifact_install_root_prepared_state(request);
    assert(legacy.starts_with("MOGUET-SOURCE-ARTIFACT-PREPARED\t2\n"));
    assert(std::get<SourceArtifactInstallRootPrepareRequest>(parse_source_artifact_install_root_prepared_state(legacy)).purpose ==
           SourceArtifactInstallTrustedPurpose::CleanupInstallOnly);
    assert(std::get<Issue>(parse_exact_artifact_operation_fragment(legacy, request, STAGE, Operation::Install)) == Issue::Invalid);
    for(const auto& command : {"record-install", "record-upgrade", "consume-exact"}) {
        assert(std::holds_alternative<SourceArtifactInstallTrustedHelperInvocation>(parse_source_artifact_install_trusted_helper_arguments({command, TOKEN})));
        assert(std::holds_alternative<SourceArtifactInstallTrustedProtocolFailure>(parse_source_artifact_install_trusted_helper_arguments({command, TOKEN, "Install"})));
    }
}

} // namespace

int main() {
    protocol_matrix();
    prepared_purpose_matrix();
    std::cout << "exact-artifact-transaction-protocol: all checks passed\n";
}
