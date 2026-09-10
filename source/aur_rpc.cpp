#include "aur_rpc.hpp"

#include "application_identity.hpp"
#include "dependency_spec.hpp"
#include "localization.hpp"
#include "logging.hpp"
#include "package_identifier.hpp"

#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <curl/curl.h>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <set>
#include <string>

namespace {

using json = nlohmann::json;

const std::string AUR_RPC_DEFAULT_BASE_URL = "https://aur.archlinux.org/rpc/";
const std::string USER_AGENT =
    std::string(application_identity::COMMAND_NAME) + "/" +
    std::string(application_identity::VERSION);
const long long AUR_RPC_PROTOCOL_VERSION = 5;
const std::string AUR_RPC_INFO_RESPONSE_TYPE = "multiinfo";
const std::string AUR_RPC_SEARCH_RESPONSE_TYPE = "search";

#ifdef MOGUET_ENABLE_AUR_RPC_TEST_HOOKS
bool g_should_fail_write_append_for_test = false;
std::string g_encode_failure_package_for_test;
std::string g_encode_failure_search_query_for_test;
#endif

void ensure_curl_global_initialized() {
    static CurlGlobal global;
}

// CURL easy handle の確保と解放を 1 request の寿命に束ねる RAII wrapper。
class CurlHandle {
    CURL* curl_;

public:
    CurlHandle() {
        ensure_curl_global_initialized();
        curl_ = curl_easy_init();
        if(!curl_) {
            throw std::runtime_error(localization::format_translated_message(
                "Failed to initialize the {} handle.", "cURL"));
        }
    }
    ~CurlHandle() {
        if(curl_) curl_easy_cleanup(curl_);
    }
    CURL* get() const {
        return curl_;
    }
};

struct CurlEscapedStringReleaser {
    void operator()(char* value) const noexcept {
        if(value != nullptr) curl_free(value);
    }
};

using UniqueCurlEscapedString =
    std::unique_ptr<char, CurlEscapedStringReleaser>;

// AUR parserをmonolithへ逆依存させず、汎用utilityを公開しないためのlocal helper。
std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\n\r");
    if(first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\n\r");
    return str.substr(first, (last - first + 1));
}

bool contains_control_character(const std::string& value) noexcept {
    for(unsigned char character : value) {
        if(std::iscntrl(character) != 0) return true;
    }
    return false;
}

// AUR RPC / JSON解析
std::string aur_rpc_base_url() {
#ifdef MOGUET_ENABLE_TEST_OVERRIDES
    // POLICY: local fixture injection は isolated test binary 限定。production の endpoint は固定する。
    const char* test_base_url = std::getenv("MOGUET_TEST_AUR_RPC_BASE_URL");
    if(test_base_url && test_base_url[0] != '\0') {
        std::string base_url = test_base_url;
        if(base_url.back() != '/') base_url += '/';
        return base_url;
    }
#endif
    return AUR_RPC_DEFAULT_BASE_URL;
}

std::string aur_rpc_search_url() {
    return aur_rpc_base_url() + "v5/search/";
}

std::string aur_rpc_info_url() {
    return aur_rpc_base_url() + "?v=5&type=info&arg%5B%5D=";
}

char* escape_info_many_package_name(
    CURL* handle, const std::string& package_name) {
#ifdef MOGUET_ENABLE_AUR_RPC_TEST_HOOKS
    const char* environment_failure =
        std::getenv("MOGUET_TEST_AUR_RPC_ENCODE_FAILURE_PACKAGE");
    if(package_name == g_encode_failure_package_for_test ||
       (environment_failure != nullptr &&
        package_name == environment_failure)) {
        return nullptr;
    }
#endif
    return curl_easy_escape(
        handle, package_name.c_str(),
        static_cast<int>(package_name.length()));
}

char* escape_strict_search_query(
    CURL* handle, const std::string& query) {
#ifdef MOGUET_ENABLE_AUR_RPC_TEST_HOOKS
    if(query == g_encode_failure_search_query_for_test) return nullptr;
#endif
    return curl_easy_escape(
        handle, query.c_str(), static_cast<int>(query.length()));
}

std::size_t write_callback_failure_result(std::size_t total_size) noexcept {
#ifdef CURL_WRITEFUNC_ERROR
    static_cast<void>(total_size);
    return CURL_WRITEFUNC_ERROR;
#else
    // libcurl treats any value other than the supplied byte count as a write
    // error. Preserve that mismatch even for a zero-byte callback.
    return total_size == 0 ? 1 : 0;
#endif
}

void append_write_response(
    std::string& buffer, char* contents, std::size_t total_size) {
#ifdef MOGUET_ENABLE_AUR_RPC_TEST_HOOKS
    if(g_should_fail_write_append_for_test) {
        // NO_TRANSLATE(Issue #308): Test-hook injection diagnostic, excluded
        // from production builds.
        throw std::runtime_error("Injected AUR response append failure.");
    }
#endif
    buffer.append(contents, total_size);
}

std::size_t WriteCallback(
    void* contents, std::size_t size, std::size_t nmemb,
    void* userp) noexcept {
    const std::size_t total_size = size * nmemb;
    auto* buffer = static_cast<std::string*>(userp);
    try {
        append_write_response(
            *buffer, static_cast<char*>(contents), total_size);
    } catch(...) {
        // C callbacks must not allow any C++ exception to cross libcurl's ABI.
        return write_callback_failure_result(total_size);
    }
    return total_size;
}

json parse_aur_rpc_response(const std::string& response, const std::string& context) {
    json parsed;
    try {
        parsed = json::parse(response);
    } catch(const json::exception& e) {
        throw AurRpcResponseError(
            localization::format_translated_message(
                "{} {} response parse failed for {}: {}",
                "AUR", "RPC", context, e.what()));
    }

    if(!parsed.is_object()) {
        throw AurRpcResponseError(localization::format_translated_message(
            "{} {} response validation failed for {}: expected top-level object, got {}",
            "AUR", "RPC", context, parsed.type_name()));
    }
    return parsed;
}

const json& aur_rpc_results_array(const json& response, const std::string& context) {
    auto results = response.find("results");
    if(results == response.end()) {
        throw AurRpcResponseError(localization::format_translated_message(
            "{} {} response validation failed for {}: field {} expected array, got missing",
            "AUR", "RPC", context, "results"));
    }
    if(!results->is_array()) {
        throw AurRpcResponseError(localization::format_translated_message(
            "{} {} response validation failed for {}: field {} expected array, got {}",
            "AUR", "RPC", context, "results", results->type_name()));
    }
    return *results;
}

[[noreturn]] void throw_aur_rpc_validation_error(
    const std::string& diagnostic) {
    throw AurRpcResponseError(diagnostic);
}

std::string json_value_for_error(const std::string& value) {
    return json(value).dump();
}

std::string aur_rpc_result_context(
    const std::string& context, size_t result_index) {
    // NO_TRANSLATE(Issue #308): Query context and JSON result location are
    // stable diagnostic identities, not human-readable prose.
    return context + " result[" + std::to_string(result_index) + "]";
}

std::string aur_rpc_info_context(const std::string& package_name) {
    // NO_TRANSLATE(Issue #308): Keys and punctuation form a stable diagnostic
    // identity; the quoted value is a package identity.
    return "info[package=" + json_value_for_error(package_name) + "]";
}

std::string aur_rpc_search_context(const std::string& query) {
    // NO_TRANSLATE(Issue #308): Keys and punctuation form a stable diagnostic
    // identity; the quoted value is runtime query data.
    return "search[query=" + json_value_for_error(query) + "]";
}

std::string aur_rpc_provides_context(const std::string& provided_name) {
    // NO_TRANSLATE(Issue #308): Keys and punctuation form a stable diagnostic
    // identity; the quoted value is a package identity.
    return "search[provides=" + json_value_for_error(provided_name) + "]";
}

std::string required_json_string(
    const json& obj, const std::string& key, const std::string& context) {
    auto value = obj.find(key);
    if(value == obj.end()) {
        throw_aur_rpc_validation_error(
            localization::format_translated_message(
                "{} {} response validation failed for {}: field {} expected string, got missing",
                "AUR", "RPC", context, key));
    }
    if(!value->is_string()) {
        throw_aur_rpc_validation_error(
            localization::format_translated_message(
                "{} {} response validation failed for {}: field {} expected string, got {}",
                "AUR", "RPC", context, key,
                value->type_name()));
    }

    std::string result = value->get<std::string>();
    if(trim(result).empty()) {
        throw_aur_rpc_validation_error(
            localization::format_translated_message(
                "{} {} response validation failed for {}: field {} expected non-empty string, got empty or whitespace-only string",
                "AUR", "RPC", context, key));
    }
    return result;
}

std::string optional_json_string(
    const json& obj, const std::string& key, const std::string& context) {
    auto value = obj.find(key);
    if(value == obj.end() || value->is_null()) return "";
    if(!value->is_string()) {
        throw_aur_rpc_validation_error(
            localization::format_translated_message(
                "{} {} response validation failed for {}: field {} expected string or null, got {}",
                "AUR", "RPC", context, key,
                value->type_name()));
    }
    return value->get<std::string>();
}

std::optional<long long> optional_json_integer(
    const json& obj, const std::string& key, const std::string& context) {
    auto value = obj.find(key);
    if(value == obj.end() || value->is_null()) return std::nullopt;
    if(!value->is_number_integer()) {
        throw_aur_rpc_validation_error(
            localization::format_translated_message(
                "{} {} response validation failed for {}: field {} expected integer or null, got {}",
                "AUR", "RPC", context, key,
                value->type_name()));
    }

    if(value->is_number_unsigned()) {
        auto unsigned_value = value->get<unsigned long long>();
        if(unsigned_value > static_cast<unsigned long long>(std::numeric_limits<long long>::max())) {
            throw_aur_rpc_validation_error(
                localization::format_translated_message(
                    "{} {} response validation failed for {}: field {} integer is outside supported range",
                    "AUR", "RPC", context, key));
        }
        return static_cast<long long>(unsigned_value);
    }
    return value->get<long long>();
}

long long required_json_integer(
    const json& obj, const std::string& key, const std::string& context) {
    auto value = obj.find(key);
    if(value == obj.end()) {
        throw_aur_rpc_validation_error(
            localization::format_translated_message(
                "{} {} response validation failed for {}: field {} expected integer, got missing",
                "AUR", "RPC", context, key));
    }
    if(!value->is_number_integer()) {
        throw_aur_rpc_validation_error(
            localization::format_translated_message(
                "{} {} response validation failed for {}: field {} expected integer, got {}",
                "AUR", "RPC", context, key,
                value->type_name()));
    }

    if(value->is_number_unsigned()) {
        auto unsigned_value = value->get<unsigned long long>();
        if(unsigned_value >
           static_cast<unsigned long long>(std::numeric_limits<long long>::max())) {
            throw_aur_rpc_validation_error(
                localization::format_translated_message(
                    "{} {} response validation failed for {}: field {} integer is outside supported range",
                    "AUR", "RPC", context, key));
        }
        return static_cast<long long>(unsigned_value);
    }
    return value->get<long long>();
}

std::vector<std::string> optional_json_string_array(
    const json& obj, const std::string& key, const std::string& context) {
    auto value = obj.find(key);
    if(value == obj.end() || value->is_null()) return {};
    if(!value->is_array()) {
        throw_aur_rpc_validation_error(
            localization::format_translated_message(
                "{} {} response validation failed for {}: field {} expected array or null, got {}",
                "AUR", "RPC", context, key,
                value->type_name()));
    }

    std::vector<std::string> values;
    values.reserve(value->size());
    for(size_t i = 0; i < value->size(); ++i) {
        const json& item = (*value)[i];
        if(!item.is_string()) {
            throw_aur_rpc_validation_error(
                localization::format_translated_message(
                    "{} {} response validation failed for {}: field {}[{}] expected string, got {}",
                    "AUR", "RPC", context, key, i,
                    item.type_name()));
        }
        values.push_back(item.get<std::string>());
    }
    return values;
}

void validate_package_identifier(
    const std::string& value, const std::string& field, const std::string& context) {
    if(!is_valid_package_name(value)) {
        throw_aur_rpc_validation_error(
            localization::format_translated_message(
                "{} {} response validation failed for {}: invalid {} {}",
                "AUR", "RPC", context, field,
                json_value_for_error(value)));
    }
}

void validate_metadata_identifiers(
    const std::vector<std::string>& values, const std::string& field,
    const std::string& context) {
    for(size_t i = 0; i < values.size(); ++i) {
        if(contains_control_character(values[i])) {
            throw_aur_rpc_validation_error(
                localization::format_translated_message(
                    "{} {} response validation failed for {}: field {}[{}] contains a control character",
                    "AUR", "RPC", context, field, i));
        }
        ParsedDependency parsed = parse_dependency_string(values[i]);
        if(!is_valid_package_name(parsed.name)) {
            throw_aur_rpc_validation_error(
                localization::format_translated_message(
                    "{} {} response validation failed for {}: field {}[{}] contains invalid package identifier {}",
                    "AUR", "RPC", context, field, i,
                    json_value_for_error(parsed.name)));
        }
    }
}

void validate_metadata_control_characters(
    const std::vector<std::string>& values, const std::string& field,
    const std::string& context) {
    for(size_t i = 0; i < values.size(); ++i) {
        if(contains_control_character(values[i])) {
            throw_aur_rpc_validation_error(
                localization::format_translated_message(
                    "{} {} response validation failed for {}: field {}[{}] contains a control character",
                    "AUR", "RPC", context, field, i));
        }
    }
}

std::string constraint_metadata_field_name(
    AurConstraintMetadataField field) {
    switch(field) {
        case AurConstraintMetadataField::Depends:
            return "Depends";
        case AurConstraintMetadataField::MakeDepends:
            return "MakeDepends";
        case AurConstraintMetadataField::CheckDepends:
            return "CheckDepends";
        case AurConstraintMetadataField::Provides:
            return "Provides";
        case AurConstraintMetadataField::Conflicts:
            return "Conflicts";
        case AurConstraintMetadataField::Replaces:
            return "Replaces";
    }
    throw std::logic_error("Unknown AUR constraint metadata field.");
}

std::string constraint_metadata_identity_for_error(
    const std::string& specification) {
    const std::size_t operator_position = specification.find_first_of("<>=");
    return trim(operator_position == std::string::npos
                    ? specification
                    : specification.substr(0, operator_position));
}

[[noreturn]] void throw_constraint_metadata_projection_failure(
    const AurConstraintMetadataProjectionFailure& failure,
    const std::string& context) {
    const std::string field = constraint_metadata_field_name(failure.field);
    switch(failure.reason.kind) {
        case DependencyConstraintParseFailureKind::EmptySpecification:
        case DependencyConstraintParseFailureKind::InvalidPackageIdentity:
            throw_aur_rpc_validation_error(
                localization::format_translated_message(
                    "{} {} response validation failed for {}: field {}[{}] contains invalid package identifier {}",
                    "AUR", "RPC", context, field, failure.item_index,
                    json_value_for_error(
                        constraint_metadata_identity_for_error(
                            failure.reason.raw_specification))));
        case DependencyConstraintParseFailureKind::InvalidSonameIdentity:
        case DependencyConstraintParseFailureKind::UnsupportedConsumerOperator:
        case DependencyConstraintParseFailureKind::UnsupportedProviderOperator:
        case DependencyConstraintParseFailureKind::MissingVersion:
        case DependencyConstraintParseFailureKind::InvalidVersion:
            throw_aur_rpc_validation_error(
                localization::format_translated_message(
                    "{} {} response validation failed for {}: field {}[{}] contains an invalid version constraint",
                    "AUR", "RPC", context, field,
                    failure.item_index));
    }
    throw std::logic_error(
        "Unknown AUR constraint metadata projection failure.");
}

AurPackageInfo parse_aur_rpc_package_info(
    const json& pkg, const std::string& context, size_t result_index) {
    std::string entry_context = aur_rpc_result_context(context, result_index);
    if(!pkg.is_object()) {
        throw_aur_rpc_validation_error(
            localization::format_translated_message(
                "{} {} response validation failed for {}: package entry expected object, got {}",
                "AUR", "RPC", entry_context, pkg.type_name()));
    }

    AurPackageInfo info;
    info.Name = required_json_string(pkg, "Name", entry_context);
    validate_package_identifier(info.Name, "Name", entry_context);
    // NO_TRANSLATE(Issue #308): The key and punctuation extend the stable
    // diagnostic identity; Name remains an AUR schema field token.
    entry_context += "[Name=" + json_value_for_error(info.Name) + "]";

    info.PackageBase = required_json_string(pkg, "PackageBase", entry_context);
    validate_package_identifier(info.PackageBase, "PackageBase", entry_context);
    info.Version = required_json_string(pkg, "Version", entry_context);
    if(contains_control_character(info.Version)) {
        throw_aur_rpc_validation_error(
            localization::format_translated_message(
                "{} {} response validation failed for {}: field {} contains a control character",
                "AUR", "RPC", entry_context, "Version"));
    }
    info.Description = optional_json_string(pkg, "Description", entry_context);
    info.Maintainer = optional_json_string(pkg, "Maintainer", entry_context);
    info.OutOfDate = optional_json_integer(pkg, "OutOfDate", entry_context);

    info.Depends = optional_json_string_array(pkg, "Depends", entry_context);
    info.MakeDepends = optional_json_string_array(pkg, "MakeDepends", entry_context);
    info.CheckDepends = optional_json_string_array(pkg, "CheckDepends", entry_context);
    info.OptDepends = optional_json_string_array(pkg, "OptDepends", entry_context);
    info.Provides = optional_json_string_array(pkg, "Provides", entry_context);
    info.Conflicts = optional_json_string_array(pkg, "Conflicts", entry_context);
    info.Replaces = optional_json_string_array(pkg, "Replaces", entry_context);

    // POLICY(#174): OptDepends は `pkg: description` を許すため型だけを検証する。
    validate_metadata_control_characters(
        info.Depends, "Depends", entry_context);
    validate_metadata_control_characters(
        info.MakeDepends, "MakeDepends", entry_context);
    validate_metadata_control_characters(
        info.CheckDepends, "CheckDepends", entry_context);
    validate_metadata_control_characters(
        info.Provides, "Provides", entry_context);
    validate_metadata_identifiers(info.Conflicts, "Conflicts", entry_context);
    validate_metadata_identifiers(info.Replaces, "Replaces", entry_context);

    AurConstraintMetadataProjectionResult constraint_metadata =
        project_aur_constraint_metadata(info);
    if(const auto* failure =
           std::get_if<AurConstraintMetadataProjectionFailure>(
               &constraint_metadata);
       failure != nullptr) {
        throw_constraint_metadata_projection_failure(*failure, entry_context);
    }
    info.constraint_metadata =
        std::get<AurPackageConstraintMetadata>(
            std::move(constraint_metadata));
    return info;
}

std::vector<AurPackageInfo> parse_aur_rpc_package_results(
    const std::string& response, const std::string& context) {
    json parsed = parse_aur_rpc_response(response, context);
    const json& results = aur_rpc_results_array(parsed, context);

    std::vector<AurPackageInfo> packages;
    packages.reserve(results.size());
    for(size_t i = 0; i < results.size(); ++i) {
        packages.push_back(parse_aur_rpc_package_info(results[i], context, i));
    }
    return packages;
}

void require_no_aur_rpc_error(const json& response, const std::string& context) {
    auto error = response.find("error");
    if(error != response.end()) {
        if(!error->is_string()) {
            throw_aur_rpc_validation_error(
                localization::format_translated_message(
                    "{} {} response validation failed for {}: field {} expected string, got {}",
                    "AUR", "RPC", context, "error",
                    error->type_name()));
        }
        throw_aur_rpc_validation_error(
            localization::format_translated_message(
                "{} {} response validation failed for {}: field {} reported {}",
                "AUR", "RPC", context, "error", error->dump()));
    }
}

const json& strict_aur_rpc_results_array(
    const json& response, const std::string& context,
    const std::string& expected_response_type,
    std::optional<std::size_t> maximum_result_count) {
    require_no_aur_rpc_error(response, context);
    long long version = required_json_integer(response, "version", context);
    if(version != AUR_RPC_PROTOCOL_VERSION) {
        throw_aur_rpc_validation_error(
            localization::format_translated_message(
                "{} {} response validation failed for {}: field {} expected {}, got {}",
                "AUR", "RPC", context, "version",
                AUR_RPC_PROTOCOL_VERSION,
                version));
    }

    std::string response_type = required_json_string(response, "type", context);
    if(response_type != expected_response_type) {
        throw_aur_rpc_validation_error(
            localization::format_translated_message(
                "{} {} response validation failed for {}: field {} expected {}, got {}",
                "AUR", "RPC", context, "type",
                json_value_for_error(expected_response_type),
                json_value_for_error(response_type)));
    }

    long long result_count = required_json_integer(response, "resultcount", context);
    if(result_count < 0) {
        throw_aur_rpc_validation_error(
            localization::format_translated_message(
                "{} {} response validation failed for {}: field {} expected non-negative integer, got {}",
                "AUR", "RPC", context, "resultcount",
                result_count));
    }

    const json& results = aur_rpc_results_array(response, context);
    if(static_cast<unsigned long long>(result_count) !=
       static_cast<unsigned long long>(results.size())) {
        throw_aur_rpc_validation_error(
            localization::format_translated_message(
                "{} {} response validation failed for {}: field {} was {} but {} contained {} entries",
                "AUR", "RPC", context, "resultcount",
                result_count, "results", results.size()));
    }
    if(maximum_result_count.has_value() &&
       results.size() > maximum_result_count.value()) {
        throw_aur_rpc_validation_error(
            localization::format_translated_message(
                "{} {} response validation failed for {}: expected zero or one result, got {}",
                "AUR", "RPC", context, results.size()));
    }
    return results;
}

std::vector<AurPackageInfo> parse_strict_aur_rpc_package_results(
    const std::string& response, const std::string& context,
    const std::string& expected_response_type,
    std::optional<std::size_t> maximum_result_count = std::nullopt) {
    json parsed = parse_aur_rpc_response(response, context);
    const json& results = strict_aur_rpc_results_array(
        parsed, context, expected_response_type, maximum_result_count);

    std::vector<AurPackageInfo> packages;
    packages.reserve(results.size());
    for(std::size_t i = 0; i < results.size(); ++i) {
        packages.push_back(parse_aur_rpc_package_info(results[i], context, i));
    }
    return packages;
}

std::optional<AurPackageInfo> parse_single_aur_info_response(
    const std::string& response, const std::string& pkg_name) {
    std::string context = aur_rpc_info_context(pkg_name);
    json parsed = parse_aur_rpc_response(response, context);
    require_no_aur_rpc_error(parsed, context);
    const json& entries = aur_rpc_results_array(parsed, context);
    std::vector<AurPackageInfo> results;
    results.reserve(entries.size());
    for(size_t i = 0; i < entries.size(); ++i) {
        results.push_back(parse_aur_rpc_package_info(entries[i], context, i));
    }
    if(results.empty()) return std::nullopt;
    if(results.size() != 1) {
        throw_aur_rpc_validation_error(
            localization::format_translated_message(
                "{} {} response validation failed for {}: expected zero or one result, got {}",
                "AUR", "RPC", context, results.size()));
    }
    if(results.front().Name != pkg_name) {
        throw_aur_rpc_validation_error(
            localization::format_translated_message(
                "{} {} response validation failed for {}: requested {} but response {} was {}",
                "AUR", "RPC", context, pkg_name, "Name",
                results.front().Name));
    }
    return results.front();
}

std::optional<AurPackageInfo> parse_single_strict_aur_info_response(
    const std::string& response, const std::string& pkg_name) {
    std::string context = aur_rpc_info_context(pkg_name);
    std::vector<AurPackageInfo> results =
        parse_strict_aur_rpc_package_results(
            response, context, AUR_RPC_INFO_RESPONSE_TYPE, 1);
    if(results.empty()) return std::nullopt;
    if(results.front().Name != pkg_name) {
        throw_aur_rpc_validation_error(
            localization::format_translated_message(
                "{} {} response validation failed for {}: requested {} but response {} was {}",
                "AUR", "RPC", context, pkg_name, "Name",
                results.front().Name));
    }
    return results.front();
}

} // namespace

#ifdef MOGUET_ENABLE_AUR_RPC_TEST_HOOKS
void set_aur_rpc_write_append_failure_for_test(bool should_fail) noexcept {
    g_should_fail_write_append_for_test = should_fail;
}

void set_aur_rpc_encode_failure_package_for_test(
    const std::string& package_name) {
    g_encode_failure_package_for_test = package_name;
}

void set_aur_rpc_encode_failure_search_query_for_test(
    const std::string& query) {
    g_encode_failure_search_query_for_test = query;
}

std::size_t invoke_aur_rpc_write_callback_for_test(
    char* contents, std::size_t size, std::size_t nmemb,
    std::string& buffer) noexcept {
    return WriteCallback(contents, size, nmemb, &buffer);
}
#endif

CurlGlobal::CurlGlobal() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

CurlGlobal::~CurlGlobal() {
    curl_global_cleanup();
}

namespace {

std::string get_url_strict(const std::string& url) {
    CurlHandle handle;
    std::string readBuffer;
    char errorBuffer[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(handle.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &readBuffer);
    curl_easy_setopt(handle.get(), CURLOPT_USERAGENT, USER_AGENT.c_str());
    curl_easy_setopt(handle.get(), CURLOPT_ERRORBUFFER, errorBuffer);
    curl_easy_setopt(handle.get(), CURLOPT_FAILONERROR, 1L);
    CURLcode res = curl_easy_perform(handle.get());
    if(res != CURLE_OK) {
        std::string error = errorBuffer[0] ? errorBuffer : curl_easy_strerror(res);
        throw std::runtime_error(localization::format_translated_message(
            "{} request failed: {}", "AUR", error));
    }

    long response_code = 0;
    curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &response_code);
    if(response_code < 200 || response_code >= 300) {
        throw std::runtime_error(localization::format_translated_message(
            "{} request failed with {} status {}.",
            "AUR", "HTTP", response_code));
    }
    if(readBuffer.empty()) {
        throw std::runtime_error(localization::format_translated_message(
            "{} request returned an empty response.", "AUR"));
    }
    return readBuffer;
}

std::string get_url(const std::string& url) {
    CurlHandle handle;
    std::string readBuffer;
    char errorBuffer[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(handle.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &readBuffer);
    curl_easy_setopt(handle.get(), CURLOPT_USERAGENT, USER_AGENT.c_str());
    curl_easy_setopt(handle.get(), CURLOPT_ERRORBUFFER, errorBuffer);
    CURLcode res = curl_easy_perform(handle.get());
    if(res != CURLE_OK) {
        // NOTE: 呼び出し側は空 response を「取得不能/未検出」として扱い、CLI 境界で文脈付きに変換する。
        std::string error = errorBuffer[0] ? errorBuffer : curl_easy_strerror(res);
        Logger::warn(localization::format_translated_message(
            "{} request failed: {}", "AUR", error));
        return "";
    }
    return readBuffer;
}

} // namespace

std::vector<AurPackageInfo> AurClient::search(const std::string& query) {
    std::vector<AurPackageInfo> packages;
    CurlHandle handle;
    char* escaped = curl_easy_escape(handle.get(), query.c_str(), static_cast<int>(query.length()));
    if(!escaped) return packages;
    std::string url = aur_rpc_search_url() + escaped;
    curl_free(escaped);

    std::string response = get_url(url);
    if(response.empty()) return packages;
    return parse_aur_rpc_package_results(
        response, aur_rpc_search_context(query));
}

std::vector<AurPackageInfo> AurClient::search_strict(
    const std::string& query) {
    CurlHandle handle;
    UniqueCurlEscapedString escaped(
        escape_strict_search_query(handle.get(), query));
    if(!escaped) {
        throw std::runtime_error(localization::format_translated_message(
            "Failed to encode {} search query: {}", "AUR", query));
    }
    std::string url = aur_rpc_search_url() + escaped.get();

    std::string response = get_url_strict(url);
    return parse_strict_aur_rpc_package_results(
        response, aur_rpc_search_context(query),
        AUR_RPC_SEARCH_RESPONSE_TYPE);
}

std::vector<std::string> AurClient::search_names_by_provides(const std::string& provided_name) {
    std::vector<std::string> names;
    CurlHandle handle;
    char* escaped = curl_easy_escape(handle.get(), provided_name.c_str(), static_cast<int>(provided_name.length()));
    if(!escaped) return names;
    std::string url = aur_rpc_search_url() + escaped + "?by=provides";
    curl_free(escaped);

    std::string response = get_url(url);
    if(response.empty()) return names;

    std::vector<AurPackageInfo> results =
        parse_aur_rpc_package_results(
            response, aur_rpc_provides_context(provided_name));
    for(const auto& info : results) {
        names.push_back(info.Name);
    }
    return names;
}

std::vector<std::string> AurClient::search_names_by_provides_strict(
    const std::string& provided_name) {
    std::vector<std::string> names;
    CurlHandle handle;
    char* escaped = curl_easy_escape(
        handle.get(), provided_name.c_str(),
        static_cast<int>(provided_name.length()));
    if(!escaped) {
        throw std::runtime_error(localization::format_translated_message(
            "Failed to encode {} provided name: {}",
            "AUR", provided_name));
    }
    std::string url = aur_rpc_search_url() + escaped + "?by=provides";
    curl_free(escaped);

    std::string response = get_url_strict(url);
    std::vector<AurPackageInfo> results =
        parse_strict_aur_rpc_package_results(
            response, aur_rpc_provides_context(provided_name),
            AUR_RPC_SEARCH_RESPONSE_TYPE);
    for(const auto& info : results)
        names.push_back(info.Name);
    return names;
}

std::optional<AurPackageInfo> AurClient::info(const std::string& pkg_name) {
    CurlHandle handle;
    char* escaped = curl_easy_escape(handle.get(), pkg_name.c_str(), static_cast<int>(pkg_name.length()));
    if(!escaped) {
        throw std::runtime_error(localization::format_translated_message(
            "Failed to encode {} package name: {}", "AUR", pkg_name));
    }
    std::string url = aur_rpc_info_url() + escaped;
    curl_free(escaped);

    std::string response = get_url_strict(url);

    return parse_single_aur_info_response(response, pkg_name);
}

std::optional<AurPackageInfo> AurClient::info_strict(const std::string& pkg_name) {
    CurlHandle handle;
    char* escaped = curl_easy_escape(
        handle.get(), pkg_name.c_str(), static_cast<int>(pkg_name.length()));
    if(!escaped) {
        throw std::runtime_error(localization::format_translated_message(
            "Failed to encode {} package name: {}", "AUR", pkg_name));
    }
    std::string url = aur_rpc_info_url() + escaped;
    curl_free(escaped);

    std::string response = get_url_strict(url);
    return parse_single_strict_aur_info_response(response, pkg_name);
}

std::map<std::string, AurPackageInfo> AurClient::info_many(const std::vector<std::string>& pkg_names) {
    std::map<std::string, AurPackageInfo> results;
    if(pkg_names.empty()) return results;

    std::set<std::string> requested_names;
    for(const auto& pkg_name : pkg_names) {
        require_valid_package_name(pkg_name);
        requested_names.insert(pkg_name);
    }

    CurlHandle handle;
    std::string url = aur_rpc_base_url() + "?v=5&type=info";
    for(size_t i = 0; i < pkg_names.size(); ++i) {
        char* escaped = escape_info_many_package_name(
            handle.get(), pkg_names[i]);
        if(!escaped) {
            throw std::runtime_error(localization::format_translated_message(
                "Failed to encode {} package name: {}",
                "AUR", pkg_names[i]));
        }
        url += "&";
        url += "arg%5B%5D=";
        url += escaped;
        curl_free(escaped);
    }

    std::string response = get_url(url);
    if(response.empty()) return results;

    std::string context = "multiinfo";
    std::vector<AurPackageInfo> aur_results = parse_aur_rpc_package_results(response, context);
    for(const auto& pkg_info : aur_results) {
        if(!requested_names.contains(pkg_info.Name)) {
            throw_aur_rpc_validation_error(
                localization::format_translated_message(
                    "{} {} response validation failed for {}: response {} {} was not requested",
                    "AUR", "RPC", context, "Name",
                    pkg_info.Name));
        }
        if(!results.emplace(pkg_info.Name, pkg_info).second) {
            throw_aur_rpc_validation_error(
                localization::format_translated_message(
                    "{} {} response validation failed for {}: duplicate response {} {}",
                    "AUR", "RPC", context, "Name",
                    pkg_info.Name));
        }
    }
    return results;
}
