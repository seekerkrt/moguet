#include "runtime_diagnostic.hpp"

#include "localization.hpp"
#include "logging.hpp"
#include "terminal_safe_text.hpp"

#include <stdexcept>
#include <string>
#include <string_view>

std::string terminal_safe_runtime_diagnostic_detail(std::string_view value) {
    return terminal_safe_text::escape_utf8(value);
}

std::string diagnostic_class_label(DiagnosticClass classification) {
    switch(classification) {
        case DiagnosticClass::Invalid:
            return localization::translate_message("Invalid");
        case DiagnosticClass::Unsupported:
            return localization::translate_message("Unsupported");
        case DiagnosticClass::Ambiguous:
            return localization::translate_message("Ambiguous");
        case DiagnosticClass::Declined:
            return localization::translate_message("Declined");
        case DiagnosticClass::Cancelled:
            return localization::translate_message("Cancelled");
        case DiagnosticClass::Unavailable:
            return localization::translate_message("Unavailable");
        case DiagnosticClass::InputFailure:
            return localization::translate_message("Input failure");
        case DiagnosticClass::QueryFailure:
            return localization::translate_message("Query failure");
        case DiagnosticClass::MetadataFailure:
            return localization::translate_message("Metadata failure");
        case DiagnosticClass::RequiresCheck:
            return localization::translate_message("Requires check");
        case DiagnosticClass::Blocked:
            return localization::translate_message("Blocked");
        case DiagnosticClass::PartialFailure:
            return localization::translate_message("Partial failure");
        case DiagnosticClass::ExecutionFailure:
            return localization::translate_message("Execution failure");
        case DiagnosticClass::InternalInconsistency:
            return localization::translate_message("Internal inconsistency");
    }
    throw std::logic_error(localization::translate_message(
        "Unknown diagnostic classification."));
}

std::string diagnostic_source_label(DiagnosticSourceKind source_kind) {
    // NO_TRANSLATE(Issue #350): Stable typed source tokens.
    switch(source_kind) {
        case DiagnosticSourceKind::Unspecified:
            return "unspecified";
        case DiagnosticSourceKind::RepositoryBinary:
            return "repository-binary";
        case DiagnosticSourceKind::RepositorySource:
            return "repository-source";
        case DiagnosticSourceKind::Aur:
            return "aur";
        case DiagnosticSourceKind::Local:
            return "local";
        case DiagnosticSourceKind::Pacman:
            return "pacman";
    }
    throw std::logic_error(localization::translate_message(
        "Unknown diagnostic source kind."));
}

std::string diagnostic_identity_suffix(const DiagnosticIdentity& identity) {
    std::string suffix;
    auto append = [&suffix](const std::string& field, const std::string& value) {
        suffix += suffix.empty() ? " [" : ", ";
        suffix += field;
        suffix += "=";
        suffix += value;
    };
    const auto append_opaque = [&append](
                                   const std::string& field,
                                   const std::string& value) {
        append(field, terminal_safe_runtime_diagnostic_detail(value));
    };
    if(identity.source_kind != DiagnosticSourceKind::Unspecified) {
        append(localization::translate_message("source"),
               diagnostic_source_label(identity.source_kind));
    }
    if(identity.repository.has_value()) {
        append_opaque(localization::translate_message("repository"),
                      identity.repository.value());
    }
    if(identity.requested_package.has_value()) {
        append_opaque(localization::translate_message("package"),
                      identity.requested_package.value());
    }
    if(identity.package_base.has_value()) {
        append_opaque("PackageBase", identity.package_base.value());
    }
    if(identity.canonical_source_identity.has_value()) {
        append_opaque(
            localization::translate_message("source identity"),
            identity.canonical_source_identity.value());
    }
    if(identity.local_root.has_value()) {
        append_opaque(localization::translate_message("local root"),
                      identity.local_root->string());
    }
    if(!suffix.empty()) suffix += "]";
    return suffix;
}

void report_runtime_diagnostic(
    const RuntimeDiagnosticPresentation& diagnostic) {
    switch(diagnostic.severity) {
        case DiagnosticSeverity::Info:
            Logger::info(diagnostic.message);
            return;
        case DiagnosticSeverity::Warning:
            Logger::warn(diagnostic.message);
            return;
        case DiagnosticSeverity::Error:
            Logger::error(diagnostic.message);
            return;
    }
    throw std::logic_error("Unknown runtime diagnostic severity.");
}
