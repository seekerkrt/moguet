#include "reviewed_source_presentation.hpp"
#include "terminal_safe_text_cases.hpp"

#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

static_assert(!std::is_aggregate_v<
              ReviewedSourceVerifiedMaterializedReview>);
static_assert(!std::is_constructible_v<
              ReviewedSourceVerifiedMaterializedReview,
              ReviewedSourceMaterializedReview>);
static_assert(std::is_const_v<std::remove_reference_t<decltype(std::declval<ReviewedSourceVerifiedMaterializedReview&>()
                                                                   .review())>>);

constexpr const char* SHA1_A =
    "1111111111111111111111111111111111111111";
constexpr const char* SHA1_B =
    "2222222222222222222222222222222222222222";
constexpr const char* SHA1_C =
    "3333333333333333333333333333333333333333";
constexpr const char* SHA1_D =
    "4444444444444444444444444444444444444444";
constexpr const char* SHA256_A =
    "1111111111111111111111111111111111111111111111111111111111111111";
constexpr const char* SHA256_B =
    "2222222222222222222222222222222222222222222222222222222222222222";

void require(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

template <typename Expected, typename Variant>
const Expected& require_arm(
    const Variant& value, std::string_view message) {
    const Expected* result = std::get_if<Expected>(&value);
    if(result == nullptr) throw std::runtime_error(std::string(message));
    return *result;
}

ReviewedSourceFileVersion version(
    std::string path,
    ReviewedSourceFileMode mode,
    const char* object_id,
    std::uintmax_t size = 0) {
    return ReviewedSourceFileVersion::make(
        ReviewedSourcePath::make(std::move(path)), mode,
        ReviewedSourceObjectId::make(object_id),
        mode == ReviewedSourceFileMode::Gitlink
            ? std::nullopt
            : std::optional<std::uintmax_t>(size));
}

ReviewedSourceBlobObservation text_observation(
    std::vector<ReviewedSourceTextLine> lines) {
    auto content = std::make_shared<ReviewedSourceTextContent>();
    content->lines = std::move(lines);
    return ReviewedSourceBlobObservation{
        ReviewedSourceBlobContentKind::LineReviewable,
        std::move(content)};
}

ReviewedSourceBlobObservation one_line_observation(
    std::string bytes, bool has_newline = true) {
    return text_observation({ReviewedSourceTextLine{
        std::move(bytes), has_newline}});
}

ReviewedSourceBlobObservation non_text_observation(
    ReviewedSourceBlobContentKind kind) {
    return ReviewedSourceBlobObservation{kind, nullptr};
}

const ReviewedSourceFileVersion* old_version(
    const ReviewedSourceFileChange& change) {
    return std::visit(
        [](const auto& value) -> const ReviewedSourceFileVersion* {
            using Change = std::decay_t<decltype(value)>;
            if constexpr(std::is_same_v<Change, ReviewedSourceAdded>) {
                return nullptr;
            } else {
                return &value.old_version;
            }
        },
        change);
}

const ReviewedSourceFileVersion* new_version(
    const ReviewedSourceFileChange& change) {
    return std::visit(
        [](const auto& value) -> const ReviewedSourceFileVersion* {
            using Change = std::decay_t<decltype(value)>;
            if constexpr(std::is_same_v<Change, ReviewedSourceDeleted>) {
                return nullptr;
            } else {
                return &value.new_version;
            }
        },
        change);
}

ReviewedSourceReviewEmphasis change_emphasis(
    const ReviewedSourceFileChange& change) {
    const ReviewedSourceFileVersion* old_file = old_version(change);
    const ReviewedSourceFileVersion* new_file = new_version(change);
    if((old_file != nullptr &&
        reviewed_source_review_emphasis(old_file->path()) ==
            ReviewedSourceReviewEmphasis::Sensitive) ||
       (new_file != nullptr &&
        reviewed_source_review_emphasis(new_file->path()) ==
            ReviewedSourceReviewEmphasis::Sensitive)) {
        return ReviewedSourceReviewEmphasis::Sensitive;
    }
    return ReviewedSourceReviewEmphasis::Ordinary;
}

ReviewedSourceReviewEntry entry(
    ReviewedSourceFileChange change,
    ReviewedSourceReviewRepresentation representation,
    ReviewedSourceReviewReadiness readiness,
    std::optional<ReviewedSourceBlobObservation> old_observation =
        std::nullopt,
    std::optional<ReviewedSourceBlobObservation> new_observation =
        std::nullopt,
    std::optional<ReviewedSourceTextPatch> patch = std::nullopt) {
    const ReviewedSourceReviewEmphasis emphasis = change_emphasis(change);
    return ReviewedSourceReviewEntry{
        std::move(change), emphasis,
        std::move(old_observation), std::move(new_observation),
        representation, readiness, std::move(patch)};
}

ReviewedSourceReviewReadiness aggregate_readiness(
    const std::vector<ReviewedSourceReviewEntry>& entries) {
    ReviewedSourceReviewReadiness readiness =
        ReviewedSourceReviewReadiness::Complete;
    for(const ReviewedSourceReviewEntry& value : entries) {
        if(value.readiness ==
           ReviewedSourceReviewReadiness::SensitiveSourceUnrenderable) {
            return value.readiness;
        }
        if(value.readiness ==
           ReviewedSourceReviewReadiness::ManualInspectionRequired) {
            readiness = value.readiness;
        }
    }
    return readiness;
}

ReviewedSourceMaterializedReview update_review(
    std::vector<ReviewedSourceReviewEntry> entries,
    const char* baseline = SHA1_A,
    const char* target = SHA1_B,
    ReviewedSourceHistoryRelation relation =
        ReviewedSourceHistoryRelation::Ancestor) {
    const ReviewedSourceReviewReadiness readiness =
        aggregate_readiness(entries);
    return ReviewedSourceMaterializedUpdateReview{
        SourceRevisionIdentity::git_commit(baseline),
        SourceRevisionIdentity::git_commit(target), relation,
        ReviewedSourceReviewBody{readiness, std::move(entries)}};
}

ReviewedSourceVerifiedMaterializedReview seal_for_render(
    ReviewedSourceMaterializedReview review) {
    return seal_reviewed_source_materialized_review_for_test(
        std::move(review));
}

std::string render_success(ReviewedSourceMaterializedReview review) {
    const ReviewedSourceVerifiedMaterializedReview verified =
        seal_for_render(std::move(review));
    ReviewedSourcePresentationResult result =
        render_reviewed_source_presentation(verified);
    auto* rendered = std::get_if<ReviewedSourceRenderedPresentation>(&result);
    if(rendered == nullptr) {
        throw std::runtime_error("Reviewed source presentation failed");
    }
    return std::move(rendered->text);
}

ReviewedSourcePresentationResult render_result(
    ReviewedSourceMaterializedReview review) {
    const ReviewedSourceVerifiedMaterializedReview verified =
        seal_for_render(std::move(review));
    return render_reviewed_source_presentation(verified);
}

void require_inconsistent(
    ReviewedSourceMaterializedReview review,
    std::string_view message) {
    const ReviewedSourcePresentationResult result =
        render_result(std::move(review));
    const auto& failure = require_arm<ReviewedSourcePresentationFailure>(
        result, message);
    require(failure.reason ==
                    ReviewedSourcePresentationFailureReason::
                        InconsistentMaterializedReview &&
                failure.observed == 0 && failure.limit == 0,
            std::string(message));
}

std::string patch_header(
    const std::string& old_oid,
    const std::string& new_oid) {
    return "diff --git " + old_oid + " " + new_oid + "\n" +
           "index " + old_oid + ".." + new_oid + " 100644\n" +
           "--- " + old_oid + "\n" +
           "+++ " + new_oid + "\n";
}

ReviewedSourceTextPatch one_line_patch(
    const char* old_oid,
    const char* new_oid,
    std::string_view old_line,
    std::string_view new_line,
    bool old_has_newline = true,
    bool new_has_newline = true) {
    std::string raw = patch_header(old_oid, new_oid);
    raw += "@@ -1 +1 @@\n-" + std::string(old_line) + "\n";
    if(!old_has_newline) raw += "\\ No newline at end of file\n";
    raw += "+" + std::string(new_line) + "\n";
    if(!new_has_newline) raw += "\\ No newline at end of file\n";
    ReviewedSourcePatchParseResult parsed =
        parse_and_verify_reviewed_source_patch(
            raw,
            ReviewedSourceObjectId::make(old_oid),
            ReviewedSourceObjectId::make(new_oid),
            std::string(old_line) +
                (old_has_newline ? "\n" : ""),
            std::string(new_line) +
                (new_has_newline ? "\n" : ""));
    const auto* patch = std::get_if<ReviewedSourceTextPatch>(&parsed);
    if(patch == nullptr) {
        throw std::runtime_error("Strict patch fixture did not parse");
    }
    return *patch;
}

void require_contains(
    std::string_view text,
    std::string_view expected,
    std::string_view message) {
    require(text.find(expected) != std::string_view::npos,
            std::string(message));
}

void require_not_contains(
    std::string_view text,
    std::string_view unexpected,
    std::string_view message) {
    require(text.find(unexpected) == std::string_view::npos,
            std::string(message));
}

void test_change_types_and_sha1_metadata() {
    std::vector<ReviewedSourceReviewEntry> entries;
    entries.push_back(entry(
        ReviewedSourceAdded{
            version("PKGBUILD", ReviewedSourceFileMode::Regular,
                    SHA1_A, 13),
            ReviewedSourceBinaryChange{}},
        ReviewedSourceReviewRepresentation::CompleteFullText,
        ReviewedSourceReviewReadiness::Complete,
        std::nullopt,
        one_line_observation("pkgname=demo")));
    entries.push_back(entry(
        ReviewedSourceModified{
            version("mode-only", ReviewedSourceFileMode::Regular,
                    SHA1_A, 5),
            version("mode-only", ReviewedSourceFileMode::Executable,
                    SHA1_A, 5),
            ReviewedSourceTextChange{0, 0}},
        ReviewedSourceReviewRepresentation::NoContentChange,
        ReviewedSourceReviewReadiness::Complete,
        one_line_observation("same"),
        one_line_observation("same")));
    entries.push_back(entry(
        ReviewedSourceDeleted{
            version("deleted", ReviewedSourceFileMode::Regular,
                    SHA1_B, 8),
            ReviewedSourceTextChange{0, 1}},
        ReviewedSourceReviewRepresentation::CompleteFullText,
        ReviewedSourceReviewReadiness::Complete,
        one_line_observation("deleted")));
    entries.push_back(entry(
        ReviewedSourceRenamed{
            version("rename-old", ReviewedSourceFileMode::Regular,
                    SHA1_B, 5),
            version("rename-new", ReviewedSourceFileMode::Regular,
                    SHA1_B, 5),
            100, ReviewedSourceTextChange{0, 0}},
        ReviewedSourceReviewRepresentation::NoContentChange,
        ReviewedSourceReviewReadiness::Complete,
        one_line_observation("same"),
        one_line_observation("same")));
    entries.push_back(entry(
        ReviewedSourceRenamed{
            version("patched-old", ReviewedSourceFileMode::Regular,
                    SHA1_A, 7),
            version("patched-new", ReviewedSourceFileMode::Regular,
                    SHA1_C, 6),
            75, ReviewedSourceTextChange{1, 1}},
        ReviewedSourceReviewRepresentation::CompleteTextPatch,
        ReviewedSourceReviewReadiness::Complete,
        one_line_observation("before"),
        one_line_observation("after"),
        one_line_patch(SHA1_A, SHA1_C, "before", "after")));
    entries.push_back(entry(
        ReviewedSourceTypeChanged{
            version("type", ReviewedSourceFileMode::Regular,
                    SHA1_C, 11),
            version("type", ReviewedSourceFileMode::SymbolicLink,
                    SHA1_D, 11),
            ReviewedSourceTextChange{1, 1}},
        ReviewedSourceReviewRepresentation::CompleteTextPatch,
        ReviewedSourceReviewReadiness::Complete,
        one_line_observation("target-old"),
        one_line_observation("target-new"),
        one_line_patch(
            SHA1_C, SHA1_D, "target-old", "target-new")));

    const std::string output = render_success(update_review(std::move(entries)));
    for(const std::string_view status : {
            "change status: added", "change status: modified",
            "change status: deleted", "change status: renamed",
            "change status: type changed"}) {
        require_contains(output, status, "A change status was lost");
    }
    require_contains(
        output,
        "change status: renamed \"rename-old\" -> \"rename-new\"",
        "Exact rename paths were not retained");
    require_contains(
        output,
        "change status: type changed 100644 (regular file) -> 120000 (symbolic link)",
        "TypeChanged modes were not retained");
    require_contains(
        output, "rename similarity: 100%",
        "Exact rename similarity was not rendered");
    require_contains(
        output, "rename similarity: 75%",
        "Modified rename similarity was not rendered");
    require_contains(
        output, "metadata change: rename only (content unchanged)",
        "Exact rename was flattened to no change");
    require_contains(
        output, "metadata change: mode changed (content unchanged)",
        "Mode-only change was flattened to no change");
    require_contains(
        output, "content representation: complete text patch",
        "Text patch representation was not rendered");
    require_contains(
        output, "@@ -1,1 +1,1 @@",
        "Parsed patch hunk coordinates were not rendered");
    require_contains(
        output, "Git change marker: binary",
        "Git binary marker was hidden");
    require_contains(
        output, "new content kind: line-reviewable content",
        "Actual content kind was hidden by the Git marker");
    require_contains(
        output, std::string("SHA-1 ") + SHA1_A,
        "Full SHA-1 object metadata was not rendered");
    require_contains(
        output, "new size: 13 bytes",
        "Blob size was not rendered");
}

void test_sha256_metadata_and_patch() {
    const ReviewedSourceReviewEntry modified = entry(
        ReviewedSourceModified{
            version("PKGBUILD", ReviewedSourceFileMode::Regular,
                    SHA256_A, 7),
            version("PKGBUILD", ReviewedSourceFileMode::Regular,
                    SHA256_B, 6),
            ReviewedSourceBinaryChange{}},
        ReviewedSourceReviewRepresentation::CompleteTextPatch,
        ReviewedSourceReviewReadiness::Complete,
        one_line_observation("before"),
        one_line_observation("after"),
        one_line_patch(SHA256_A, SHA256_B, "before", "after"));
    const std::string output = render_success(update_review(
        {modified}, SHA256_A, SHA256_B,
        ReviewedSourceHistoryRelation::NonAncestor));
    require_contains(
        output, std::string("baseline revision: SHA-256 ") + SHA256_A,
        "SHA-256 baseline was not rendered");
    require_contains(
        output, std::string("target revision: SHA-256 ") + SHA256_B,
        "SHA-256 target was not rendered");
    require_contains(
        output, std::string("old object: SHA-256 ") + SHA256_A,
        "Full old SHA-256 blob OID was not rendered");
    require_contains(
        output, std::string("new object: SHA-256 ") + SHA256_B,
        "Full new SHA-256 blob OID was not rendered");
    require_contains(
        output,
        "history relation: reviewed baseline is not an ancestor of the target",
        "Non-ancestor history relation was flattened");
    require_contains(
        output, "Git change marker: binary",
        "SHA-256 Git marker was lost");
    require_contains(
        output, "content representation: complete text patch",
        "SHA-256 patch representation was lost");
}

void append_bytes(
    std::string& output,
    std::initializer_list<unsigned int> bytes) {
    for(const unsigned int byte : bytes) {
        output.push_back(static_cast<char>(byte));
    }
}

void test_terminal_safe_content_encoding() {
    std::string unsafe = "ASCII 日本語 tab";
    unsafe.push_back('\t');
    unsafe += "cr";
    unsafe.push_back('\r');
    unsafe += "esc";
    unsafe.push_back('\x1b');
    unsafe += "c0";
    unsafe.push_back('\x01');
    unsafe += "del";
    unsafe.push_back('\x7f');
    unsafe += "raw-c1";
    unsafe.push_back(static_cast<char>(0x85));
    unsafe += "invalid";
    unsafe.push_back(static_cast<char>(0xff));
    unsafe += " literal \\x41 ";
    append_bytes(unsafe, {0xc2, 0x85});
    append_bytes(unsafe, {0xd8, 0x9c});
    append_bytes(unsafe, {0xe2, 0x80, 0x8e});
    append_bytes(unsafe, {0xe2, 0x80, 0x8f});
    for(unsigned int last = 0xaa; last <= 0xae; ++last) {
        append_bytes(unsafe, {0xe2, 0x80, last});
    }
    for(unsigned int last = 0xa6; last <= 0xa9; ++last) {
        append_bytes(unsafe, {0xe2, 0x81, last});
    }
    append_bytes(unsafe, {0xe2, 0x80, 0xa8});
    append_bytes(unsafe, {0xe2, 0x80, 0xa9});
    append_bytes(unsafe, {0xef, 0xbb, 0xbf});

    const ReviewedSourceReviewEntry added = entry(
        ReviewedSourceAdded{
            version("content", ReviewedSourceFileMode::Regular,
                    SHA1_A, unsafe.size()),
            ReviewedSourceTextChange{1, 0}},
        ReviewedSourceReviewRepresentation::CompleteFullText,
        ReviewedSourceReviewReadiness::Complete,
        std::nullopt,
        one_line_observation(unsafe, false));
    const std::string output = render_success(update_review({added}));

    require_contains(output, "ASCII 日本語", "Printable UTF-8 was escaped");
    for(const std::string_view escaped : {
            "\\x09", "\\x0D", "\\x1B", "\\x01", "\\x7F",
            "\\x85", "\\xFF", "\\xC2\\x85", "\\xD8\\x9C",
            "\\xE2\\x80\\x8E", "\\xE2\\x80\\x8F",
            "\\xE2\\x80\\xAA", "\\xE2\\x80\\xAB",
            "\\xE2\\x80\\xAC", "\\xE2\\x80\\xAD",
            "\\xE2\\x80\\xAE", "\\xE2\\x81\\xA6",
            "\\xE2\\x81\\xA7", "\\xE2\\x81\\xA8",
            "\\xE2\\x81\\xA9", "\\xE2\\x80\\xA8",
            "\\xE2\\x80\\xA9", "\\xEF\\xBB\\xBF"}) {
        require_contains(output, escaped, "Unsafe content was not escaped");
    }
    require_contains(
        output, "literal \\x5Cx41",
        "Escape-looking literal source was ambiguous");
    require_contains(
        output, "\\ No newline at end of file",
        "No-final-LF structure was lost");
    require_not_contains(
        output, std::string_view("\x1b", 1),
        "Raw ANSI escape reached presentation");
    require_not_contains(output, "\t", "Raw TAB reached presentation");
    require_not_contains(output, "\r", "Raw CR reached presentation");
    require(output.find(static_cast<char>(0x7f)) == std::string::npos,
            "Raw DEL reached presentation");
    require(output.find(static_cast<char>(0xff)) == std::string::npos,
            "Invalid UTF-8 reached presentation");
    require_not_contains(
        output, std::string_view("\xc2\x85", 2),
        "Valid UTF-8 C1 control reached presentation");

    const std::string long_line(REVIEWED_SOURCE_LOGICAL_LINE_LIMIT, 'z');
    const ReviewedSourceReviewEntry long_added = entry(
        ReviewedSourceAdded{
            version("long", ReviewedSourceFileMode::Regular,
                    SHA1_B, long_line.size() + 1),
            ReviewedSourceTextChange{1, 0}},
        ReviewedSourceReviewRepresentation::CompleteFullText,
        ReviewedSourceReviewReadiness::Complete,
        std::nullopt,
        one_line_observation(long_line));
    const std::string long_output = render_success(update_review({long_added}));
    require_contains(
        long_output, long_line,
        "Exact-limit long safe line was not preserved");
}

ReviewedSourceReviewEntry full_text_entry(
    std::string path,
    std::string content) {
    const std::size_t content_size = content.size();
    return entry(
        ReviewedSourceAdded{
            version(
                std::move(path), ReviewedSourceFileMode::Regular,
                SHA1_A, content_size),
            ReviewedSourceTextChange{1, 0}},
        ReviewedSourceReviewRepresentation::CompleteFullText,
        ReviewedSourceReviewReadiness::Complete, std::nullopt,
        one_line_observation(std::move(content), false));
}

void test_shared_terminal_safe_content_corpus() {
    for(const auto& test_case : terminal_safe_text_test_cases::cases()) {
        // NUL content is withheld by the reviewed-source model and LF is a
        // structural line boundary, so neither can enter a verified line.
        if(test_case.input.find('\0') != std::string::npos ||
           test_case.input.find('\n') != std::string::npos) {
            continue;
        }
        const std::string output = render_success(update_review(
            {full_text_entry("corpus", test_case.input)}));
        require_contains(
            output, "    +" + test_case.expected + "\n",
            std::string("Reviewed-source corpus differs for ") +
                std::string(test_case.label));
    }

    using terminal_safe_text_test_cases::bytes;
    std::string patch_content = "patch-";
    patch_content += bytes(
        {0xe2, 0x80, 0xae, 0xef, 0xbb, 0xbf, 0xff});
    const ReviewedSourceReviewEntry modified = entry(
        ReviewedSourceModified{
            version(
                "patch-corpus", ReviewedSourceFileMode::Regular,
                SHA1_A, 4),
            version(
                "patch-corpus", ReviewedSourceFileMode::Regular,
                SHA1_B, patch_content.size() + 1),
            ReviewedSourceTextChange{1, 1}},
        ReviewedSourceReviewRepresentation::CompleteTextPatch,
        ReviewedSourceReviewReadiness::Complete,
        one_line_observation("old"),
        one_line_observation(patch_content),
        one_line_patch(SHA1_A, SHA1_B, "old", patch_content));
    const std::string patch_output =
        render_success(update_review({modified}));
    require_contains(
        patch_output,
        "    +patch-\\xE2\\x80\\xAE\\xEF\\xBB\\xBF\\xFF\n",
        "Reviewed-source patch did not use the canonical policy");
}

void test_representation_and_readiness_diagnostics() {
    const ReviewedSourceReviewEntry full = entry(
        ReviewedSourceAdded{
            version("full", ReviewedSourceFileMode::Regular,
                    SHA1_A, 5),
            ReviewedSourceTextChange{1, 0}},
        ReviewedSourceReviewRepresentation::CompleteFullText,
        ReviewedSourceReviewReadiness::Complete,
        std::nullopt, one_line_observation("full"));
    const ReviewedSourceReviewEntry patch = entry(
        ReviewedSourceModified{
            version("patch", ReviewedSourceFileMode::Regular,
                    SHA1_A, 4),
            version("patch", ReviewedSourceFileMode::Regular,
                    SHA1_B, 4),
            ReviewedSourceTextChange{1, 1}},
        ReviewedSourceReviewRepresentation::CompleteTextPatch,
        ReviewedSourceReviewReadiness::Complete,
        one_line_observation("old"), one_line_observation("new"),
        one_line_patch(SHA1_A, SHA1_B, "old", "new"));
    const ReviewedSourceReviewEntry unchanged = entry(
        ReviewedSourceModified{
            version("unchanged", ReviewedSourceFileMode::Regular,
                    SHA1_A, 5),
            version("unchanged", ReviewedSourceFileMode::Executable,
                    SHA1_A, 5),
            ReviewedSourceTextChange{0, 0}},
        ReviewedSourceReviewRepresentation::NoContentChange,
        ReviewedSourceReviewReadiness::Complete,
        one_line_observation("same"), one_line_observation("same"));
    const ReviewedSourceReviewEntry binary = entry(
        ReviewedSourceAdded{
            version("binary", ReviewedSourceFileMode::Regular,
                    SHA1_B, 8),
            ReviewedSourceTextChange{1, 0}},
        ReviewedSourceReviewRepresentation::ContainsNul,
        ReviewedSourceReviewReadiness::ManualInspectionRequired,
        std::nullopt,
        non_text_observation(ReviewedSourceBlobContentKind::ContainsNul));
    const ReviewedSourceReviewEntry gitlink = entry(
        ReviewedSourceAdded{
            version("submodule", ReviewedSourceFileMode::Gitlink,
                    SHA1_C),
            ReviewedSourceBinaryChange{}},
        ReviewedSourceReviewRepresentation::GitlinkMetadata,
        ReviewedSourceReviewReadiness::ManualInspectionRequired,
        std::nullopt,
        non_text_observation(ReviewedSourceBlobContentKind::Gitlink));
    const ReviewedSourceReviewEntry mixed = entry(
        ReviewedSourceTypeChanged{
            version("mixed", ReviewedSourceFileMode::Regular,
                    SHA1_A, 5),
            version("mixed", ReviewedSourceFileMode::SymbolicLink,
                    SHA1_D, 8),
            ReviewedSourceBinaryChange{}},
        ReviewedSourceReviewRepresentation::MixedTextAndNonText,
        ReviewedSourceReviewReadiness::ManualInspectionRequired,
        one_line_observation("text"),
        non_text_observation(ReviewedSourceBlobContentKind::ContainsNul));
    const ReviewedSourceReviewEntry sensitive = entry(
        ReviewedSourceAdded{
            version("PKGBUILD", ReviewedSourceFileMode::Regular,
                    SHA1_D, 8),
            ReviewedSourceBinaryChange{}},
        ReviewedSourceReviewRepresentation::ContainsNul,
        ReviewedSourceReviewReadiness::SensitiveSourceUnrenderable,
        std::nullopt,
        non_text_observation(ReviewedSourceBlobContentKind::ContainsNul));

    const std::string output = render_success(update_review(
        {full, patch, unchanged, binary, gitlink, mixed, sensitive}));
    for(const std::string_view representation : {
            "complete text patch", "complete full text",
            "no content change", "content containing NUL bytes",
            "Gitlink metadata", "mixed text and non-text content"}) {
        require_contains(
            output, std::string("content representation: ") + std::string(representation),
            "A representation was not rendered");
    }
    for(const std::string_view readiness : {
            "complete", "manual inspection required",
            "sensitive source cannot be rendered safely"}) {
        require_contains(
            output, std::string("review readiness: ") + std::string(readiness),
            "A readiness state was not rendered");
    }
    require_contains(
        output, "binary/non-line-reviewable content; raw bytes withheld",
        "ContainsNul diagnostic was not fixed and safe");
    require_contains(
        output,
        "Gitlink/submodule metadata only; recursive review not performed",
        "Gitlink diagnostic was not fixed and safe");
    require_contains(
        output,
        "mixed text and non-line-reviewable content; raw bytes withheld",
        "Mixed representation diagnostic was not fixed and safe");
    require_contains(
        output, "line-reviewable old content:",
        "Safe side of mixed content was not rendered");
    require_contains(
        output, "WARNING: manual inspection is required",
        "Manual readiness was visually flattened to Complete");
    require_contains(
        output,
        "WARNING: review-sensitive source content cannot be rendered safely",
        "Sensitive unrenderable readiness was not stronger");
    require_contains(
        output,
        "overall review readiness: sensitive source cannot be rendered safely",
        "Aggregate readiness was not retained");
    require_contains(
        output, "new size: unavailable",
        "Gitlink size semantics were lost");
}

std::string render_single_path(std::string path) {
    const ReviewedSourceReviewEntry added = entry(
        ReviewedSourceAdded{
            version(std::move(path), ReviewedSourceFileMode::Regular,
                    SHA1_A, 2),
            ReviewedSourceTextChange{1, 0}},
        ReviewedSourceReviewRepresentation::CompleteFullText,
        ReviewedSourceReviewReadiness::Complete,
        std::nullopt, one_line_observation("x"));
    return render_success(update_review({added}));
}

void test_path_and_emphasis_policy() {
    for(const std::string_view path : {"PKGBUILD", "foo.install"}) {
        const std::string output = render_single_path(std::string(path));
        require_contains(
            output, "review emphasis: review-sensitive",
            "Root sensitive source was not emphasized");
        require_contains(
            output, "presentation priority: review-sensitive",
            "Root sensitive priority was not rendered");
    }
    for(const std::string_view path : {
            "nested/PKGBUILD", "nested/foo.install", ".install",
            "ordinary.patch"}) {
        const std::string output = render_single_path(std::string(path));
        require_contains(
            output, "review emphasis: ordinary",
            "Generic tracked path was over-emphasized");
        require_contains(
            output, "presentation priority: ordinary",
            "Generic tracked priority drifted");
    }

    const std::string srcinfo = render_single_path(".SRCINFO");
    require_contains(
        srcinfo, "new classification: generated metadata",
        ".SRCINFO classification was lost");
    require_contains(
        srcinfo, "presentation priority: lower-priority generated metadata",
        ".SRCINFO was not lower-emphasis metadata");
    require_contains(
        srcinfo, "not source-review authority",
        ".SRCINFO was presented as source-review authority");

    std::string arbitrary = "odd";
    arbitrary.push_back('\t');
    arbitrary.push_back(static_cast<char>(0xff));
    const std::string arbitrary_output = render_single_path(arbitrary);
    require_contains(
        arbitrary_output, "\"odd\\t\\xFF\"",
        "ReviewedSourcePath::escaped_display was not reused");
    require_not_contains(
        arbitrary_output, "\t",
        "Raw path TAB reached terminal presentation");
    require(arbitrary_output.find(static_cast<char>(0xff)) ==
                std::string::npos,
            "Raw invalid path byte reached terminal presentation");
}

std::string render_exact_rename(
    std::string old_path, std::string new_path) {
    const ReviewedSourceReviewEntry renamed = entry(
        ReviewedSourceRenamed{
            version(std::move(old_path),
                    ReviewedSourceFileMode::Regular, SHA1_A, 5),
            version(std::move(new_path),
                    ReviewedSourceFileMode::Regular, SHA1_A, 5),
            100, ReviewedSourceTextChange{0, 0}},
        ReviewedSourceReviewRepresentation::NoContentChange,
        ReviewedSourceReviewReadiness::Complete,
        one_line_observation("same"), one_line_observation("same"));
    return render_success(update_review({renamed}));
}

void test_classification_crossing_priority() {
    for(const auto& [old_path, new_path] : {
            std::pair<std::string_view, std::string_view>{
                ".SRCINFO", "helper.sh"},
            {"helper.sh", ".SRCINFO"}}) {
        const std::string output = render_exact_rename(
            std::string(old_path), std::string(new_path));
        require_contains(
            output, "presentation priority: ordinary",
            "TrackedSource side did not retain ordinary priority");
        require_not_contains(
            output,
            "presentation priority: lower-priority generated metadata",
            "GeneratedMetadata side lowered a tracked-source rename");
    }

    const std::string generated_only = render_single_path(".SRCINFO");
    require_contains(
        generated_only,
        "presentation priority: lower-priority generated metadata",
        "GeneratedMetadata-only entry lost lower priority");

    const std::string sensitive =
        render_exact_rename(".SRCINFO", "PKGBUILD");
    require_contains(
        sensitive, "presentation priority: review-sensitive",
        "Sensitive side did not win classification-crossing priority");
}

void test_lifecycle_presentation() {
    const ReviewedSourceMaterializedReview initial =
        ReviewedSourceMaterializedInitialFullReview{
            SourceRevisionIdentity::git_commit(SHA1_A),
            ReviewedSourceReviewBody{
                ReviewedSourceReviewReadiness::Complete, {}}};
    const std::string initial_output = render_success(initial);
    require_contains(
        initial_output, "review type: initial full review",
        "Initial review lifecycle was lost");

    const ReviewedSourceMaterializedReview already =
        ReviewedSourceMaterializedAlreadyReviewed{
            SourceRevisionIdentity::git_commit(SHA1_A)};
    const std::string already_output = render_success(already);
    require_contains(
        already_output, "review type: already reviewed",
        "AlreadyReviewed lifecycle was lost");
    require_contains(
        already_output, "review result: already reviewed",
        "AlreadyReviewed result was not explicit");

    const ReviewedSourceMaterializedReview rebaseline =
        ReviewedSourceMaterializedRebaselineFullReview{
            SourceRevisionIdentity::git_commit(SHA1_A),
            SourceRevisionIdentity::git_commit(SHA1_B),
            ReviewedSourceBaselineUnavailableReason::MissingOrNotCommit,
            ReviewedSourceReviewBody{
                ReviewedSourceReviewReadiness::Complete, {}}};
    const std::string rebaseline_output = render_success(rebaseline);
    require_contains(
        rebaseline_output, "review type: explicit full rebaseline",
        "Rebaseline lifecycle was lost");
    require_contains(
        rebaseline_output,
        "baseline unavailable reason: baseline object is missing or is not a commit",
        "Rebaseline reason was lost");
}

void test_verified_boundary_rejects_forged_models() {
    const std::string actual_old = "actual-old";
    const std::string actual_new = "DANGEROUS-ACTUAL-NEW";
    ReviewedSourceTextPatch forged_patch{
        ReviewedSourceObjectId::make(SHA1_A),
        ReviewedSourceObjectId::make(SHA1_B),
        {ReviewedSourcePatchHunk{
            1, 1, 1, 1, {
                            ReviewedSourcePatchLine{ReviewedSourcePatchLineKind::Removed, ReviewedSourceTextLine{"fabricated-old", true}},
                            ReviewedSourcePatchLine{ReviewedSourcePatchLineKind::Added, ReviewedSourceTextLine{"benign-reviewed", true}},
                        }}}};
    const ReviewedSourceReviewEntry forged_patch_entry = entry(
        ReviewedSourceModified{
            version("helper.sh", ReviewedSourceFileMode::Regular,
                    SHA1_A, actual_old.size() + 1),
            version("helper.sh", ReviewedSourceFileMode::Regular,
                    SHA1_B, actual_new.size() + 1),
            ReviewedSourceTextChange{1, 1}},
        ReviewedSourceReviewRepresentation::CompleteTextPatch,
        ReviewedSourceReviewReadiness::Complete,
        one_line_observation(actual_old),
        one_line_observation(actual_new), std::move(forged_patch));
    require_inconsistent(
        update_review({forged_patch_entry}),
        "Matching-OID forged patch bypassed strict replay provenance");

    for(const std::string_view path : {"PKGBUILD", "pkg.install"}) {
        ReviewedSourceReviewEntry sensitive = entry(
            ReviewedSourceAdded{
                version(std::string(path),
                        ReviewedSourceFileMode::Regular, SHA1_A, 8),
                ReviewedSourceBinaryChange{}},
            ReviewedSourceReviewRepresentation::ContainsNul,
            ReviewedSourceReviewReadiness::Complete,
            std::nullopt,
            non_text_observation(
                ReviewedSourceBlobContentKind::ContainsNul));
        sensitive.emphasis = ReviewedSourceReviewEmphasis::Ordinary;
        require_inconsistent(
            update_review({std::move(sensitive)}),
            "Sensitive NUL source accepted forged Ordinary/Complete state");
    }

    const ReviewedSourceReviewEntry false_unchanged = entry(
        ReviewedSourceModified{
            version("changed", ReviewedSourceFileMode::Regular,
                    SHA1_A, 4),
            version("changed", ReviewedSourceFileMode::Regular,
                    SHA1_B, 4),
            ReviewedSourceTextChange{1, 1}},
        ReviewedSourceReviewRepresentation::NoContentChange,
        ReviewedSourceReviewReadiness::Complete,
        one_line_observation("old"), one_line_observation("new"));
    require_inconsistent(
        update_review({false_unchanged}),
        "Different-OID content was accepted as NoContentChange");

    const ReviewedSourceReviewEntry wrong_side_full = entry(
        ReviewedSourceAdded{
            version("wrong-side", ReviewedSourceFileMode::Regular,
                    SHA1_A, 4),
            ReviewedSourceTextChange{1, 0}},
        ReviewedSourceReviewRepresentation::CompleteFullText,
        ReviewedSourceReviewReadiness::Complete,
        one_line_observation("old"), std::nullopt);
    require_inconsistent(
        update_review({wrong_side_full}),
        "Wrong-side CompleteFullText was accepted");

    const ReviewedSourceReviewEntry invalid_mixed = entry(
        ReviewedSourceModified{
            version("mixed-forge", ReviewedSourceFileMode::Regular,
                    SHA1_A, 4),
            version("mixed-forge", ReviewedSourceFileMode::Regular,
                    SHA1_B, 4),
            ReviewedSourceTextChange{1, 1}},
        ReviewedSourceReviewRepresentation::MixedTextAndNonText,
        ReviewedSourceReviewReadiness::ManualInspectionRequired,
        one_line_observation("old"), one_line_observation("new"));
    require_inconsistent(
        update_review({invalid_mixed}),
        "Line-reviewable pair was accepted as MixedTextAndNonText");

    const ReviewedSourceReviewEntry invalid_nul_readiness = entry(
        ReviewedSourceAdded{
            version("binary-forge", ReviewedSourceFileMode::Regular,
                    SHA1_A, 8),
            ReviewedSourceBinaryChange{}},
        ReviewedSourceReviewRepresentation::ContainsNul,
        ReviewedSourceReviewReadiness::Complete,
        std::nullopt,
        non_text_observation(
            ReviewedSourceBlobContentKind::ContainsNul));
    require_inconsistent(
        update_review({invalid_nul_readiness}),
        "ContainsNul accepted Complete readiness");

    const ReviewedSourceReviewEntry invalid_gitlink_readiness = entry(
        ReviewedSourceAdded{
            version("submodule-forge",
                    ReviewedSourceFileMode::Gitlink, SHA1_A),
            ReviewedSourceBinaryChange{}},
        ReviewedSourceReviewRepresentation::GitlinkMetadata,
        ReviewedSourceReviewReadiness::Complete,
        std::nullopt,
        non_text_observation(ReviewedSourceBlobContentKind::Gitlink));
    require_inconsistent(
        update_review({invalid_gitlink_readiness}),
        "GitlinkMetadata accepted Complete readiness");

    const ReviewedSourceReviewEntry manual_entry = entry(
        ReviewedSourceAdded{
            version("aggregate-forge",
                    ReviewedSourceFileMode::Regular, SHA1_A, 8),
            ReviewedSourceBinaryChange{}},
        ReviewedSourceReviewRepresentation::ContainsNul,
        ReviewedSourceReviewReadiness::ManualInspectionRequired,
        std::nullopt,
        non_text_observation(
            ReviewedSourceBlobContentKind::ContainsNul));
    require_inconsistent(
        ReviewedSourceMaterializedUpdateReview{
            SourceRevisionIdentity::git_commit(SHA1_A),
            SourceRevisionIdentity::git_commit(SHA1_B),
            ReviewedSourceHistoryRelation::Ancestor,
            ReviewedSourceReviewBody{
                ReviewedSourceReviewReadiness::Complete,
                {manual_entry}}},
        "Aggregate readiness was weakened below an entry");

    ReviewedSourceReviewPreparationResult prepared =
        prepare_reviewed_source_review(
            ReviewedSourceUpdateReview{
                SourceRevisionIdentity::git_commit(SHA1_A),
                SourceRevisionIdentity::git_commit(SHA1_B),
                ReviewedSourceHistoryRelation::Ancestor,
                {}},
            {});
    auto* preparation =
        std::get_if<ReviewedSourceReviewPreparation>(&prepared);
    require(preparation != nullptr,
            "Genuine 3B1 preparation fixture failed");
    ReviewedSourceReviewFinalizationResult finalized =
        finalize_reviewed_source_review(std::move(*preparation), {});
    const auto* materialized =
        std::get_if<ReviewedSourceMaterializedReview>(&finalized);
    require(materialized != nullptr,
            "Genuine 3B1 finalizer fixture failed");
    require_contains(
        render_success(*materialized), "overall review readiness: complete",
        "Genuine 3B1 finalizer result did not render");
}

ReviewedSourceMaterializedReview large_render_review(
    std::string final_line) {
    constexpr std::size_t CHUNK_COUNT = 7;
    std::vector<ReviewedSourceTextLine> lines;
    lines.reserve(CHUNK_COUNT + 1);
    std::uintmax_t blob_size = 0;
    for(std::size_t index = 0; index < CHUNK_COUNT; ++index) {
        lines.push_back(ReviewedSourceTextLine{
            std::string(REVIEWED_SOURCE_LOGICAL_LINE_LIMIT, '\t'), true});
        blob_size += REVIEWED_SOURCE_LOGICAL_LINE_LIMIT + 1;
    }
    blob_size += final_line.size() + 1;
    lines.push_back(ReviewedSourceTextLine{std::move(final_line), true});
    const ReviewedSourceReviewEntry added = entry(
        ReviewedSourceAdded{
            version("large", ReviewedSourceFileMode::Regular,
                    SHA1_A, blob_size),
            ReviewedSourceTextChange{CHUNK_COUNT + 1, 0}},
        ReviewedSourceReviewRepresentation::CompleteFullText,
        ReviewedSourceReviewReadiness::Complete,
        std::nullopt, text_observation(std::move(lines)));
    return ReviewedSourceMaterializedInitialFullReview{
        SourceRevisionIdentity::git_commit(SHA1_A),
        ReviewedSourceReviewBody{
            ReviewedSourceReviewReadiness::Complete, {added}}};
}

void test_rendered_output_resource_contract() {
    const auto exact_size =
        checked_reviewed_source_presentation_size_for_test(
            0, REVIEWED_SOURCE_RENDERED_OUTPUT_LIMIT,
            REVIEWED_SOURCE_RENDERED_OUTPUT_LIMIT);
    require(require_arm<std::uintmax_t>(
                exact_size, "Exact rendered size was rejected") ==
                REVIEWED_SOURCE_RENDERED_OUTPUT_LIMIT,
            "Exact rendered size accounting drifted");
    const auto one_over_size =
        checked_reviewed_source_presentation_size_for_test(
            REVIEWED_SOURCE_RENDERED_OUTPUT_LIMIT, 1,
            REVIEWED_SOURCE_RENDERED_OUTPUT_LIMIT);
    const auto& one_over_failure =
        require_arm<ReviewedSourcePresentationFailure>(
            one_over_size,
            "One-over rendered size accounting was accepted");
    require(one_over_failure.reason ==
                    ReviewedSourcePresentationFailureReason::
                        RenderedOutputLimitExceeded &&
                one_over_failure.observed ==
                    REVIEWED_SOURCE_RENDERED_OUTPUT_LIMIT + 1 &&
                one_over_failure.limit ==
                    REVIEWED_SOURCE_RENDERED_OUTPUT_LIMIT,
            "One-over rendered size failure detail drifted");

    const auto overflow =
        checked_reviewed_source_presentation_size_for_test(
            std::numeric_limits<std::uintmax_t>::max(), 1,
            REVIEWED_SOURCE_RENDERED_OUTPUT_LIMIT);
    const auto& overflow_failure =
        require_arm<ReviewedSourcePresentationFailure>(
            overflow, "Rendered size accounting overflow wrapped");
    require(overflow_failure.reason ==
                    ReviewedSourcePresentationFailureReason::
                        RenderedOutputLimitExceeded &&
                overflow_failure.observed ==
                    std::numeric_limits<std::uintmax_t>::max(),
            "Rendered size overflow was not safely saturated");

    std::string final_line;
    {
        const std::string baseline =
            render_success(large_render_review({}));
        require(baseline.size() < REVIEWED_SOURCE_RENDERED_OUTPUT_LIMIT,
                "Large exact-limit fixture baseline was already oversized");
        const std::size_t missing = static_cast<std::size_t>(
            REVIEWED_SOURCE_RENDERED_OUTPUT_LIMIT - baseline.size());
        final_line.assign(missing / 4, '\t');
        final_line.append(missing % 4, 'b');
    }
    require(final_line.size() <= REVIEWED_SOURCE_LOGICAL_LINE_LIMIT,
            "Exact rendered-limit fixture exceeded logical-line authority");
    {
        const std::string exact =
            render_success(large_render_review(final_line));
        require(exact.size() == REVIEWED_SOURCE_RENDERED_OUTPUT_LIMIT,
                "Actual final rendered output did not pass at exact 32 MiB");
    }
    std::string one_over_line = final_line;
    one_over_line.push_back('b');
    const ReviewedSourcePresentationResult one_over =
        render_result(large_render_review(std::move(one_over_line)));
    const auto& render_failure =
        require_arm<ReviewedSourcePresentationFailure>(
            one_over,
            "Actual final rendered output accepted one-over 32 MiB");
    require(render_failure.reason ==
                    ReviewedSourcePresentationFailureReason::
                        RenderedOutputLimitExceeded &&
                render_failure.observed ==
                    REVIEWED_SOURCE_RENDERED_OUTPUT_LIMIT + 1 &&
                render_failure.limit ==
                    REVIEWED_SOURCE_RENDERED_OUTPUT_LIMIT,
            "Actual one-over render failure detail drifted");

    ReviewedSourceReviewEntry inconsistent = entry(
        ReviewedSourceModified{
            version("inconsistent", ReviewedSourceFileMode::Regular,
                    SHA1_A, 4),
            version("inconsistent", ReviewedSourceFileMode::Regular,
                    SHA1_B, 4),
            ReviewedSourceTextChange{1, 1}},
        ReviewedSourceReviewRepresentation::CompleteTextPatch,
        ReviewedSourceReviewReadiness::Complete,
        one_line_observation("old"), one_line_observation("new"));
    const ReviewedSourcePresentationResult inconsistent_result =
        render_result(update_review({std::move(inconsistent)}));
    const auto& inconsistent_failure =
        require_arm<ReviewedSourcePresentationFailure>(
            inconsistent_result,
            "Inconsistent model published a partial presentation");
    require(inconsistent_failure.reason ==
                    ReviewedSourcePresentationFailureReason::
                        InconsistentMaterializedReview &&
                inconsistent_failure.entry_index == 0 &&
                inconsistent_failure.observed == 0 &&
                inconsistent_failure.limit == 0,
            "Inconsistent presentation failure retained unsafe detail");

    const ReviewedSourceMaterializedReview small =
        ReviewedSourceMaterializedAlreadyReviewed{
            SourceRevisionIdentity::git_commit(SHA1_A)};
    const std::string small_text = render_success(small);
    const ReviewedSourceVerifiedMaterializedReview verified_small =
        seal_for_render(small);
    require(std::holds_alternative<ReviewedSourceRenderedPresentation>(
                render_reviewed_source_presentation_with_limit_for_test(
                    verified_small, small_text.size())),
            "Custom exact limit was rejected");
    require(std::holds_alternative<ReviewedSourcePresentationFailure>(
                render_reviewed_source_presentation_with_limit_for_test(
                    verified_small, small_text.size() - 1)),
            "Custom one-over render published a truncated success");

    using terminal_safe_text_test_cases::bytes;
    const ReviewedSourceMaterializedReview three_byte_review =
        update_review(
            {full_text_entry("safe-prefix", "safe"),
             full_text_entry(
                 "unsafe-three-byte",
                 std::string{"before"} +
                     bytes({0xe2, 0x80, 0xae}) + "after")});
    const std::string three_byte_output =
        render_success(three_byte_review);
    const std::size_t three_byte_offset =
        three_byte_output.find("\\xE2\\x80\\xAE");
    require(
        three_byte_offset != std::string::npos,
        "Three-byte expansion fixture was not escaped");
    const std::uintmax_t three_byte_limit = three_byte_offset + 4;
    const ReviewedSourcePresentationResult three_byte_limited =
        render_reviewed_source_presentation_with_limit_for_test(
            seal_for_render(three_byte_review), three_byte_limit);
    const auto& three_byte_failure =
        require_arm<ReviewedSourcePresentationFailure>(
            three_byte_limited,
            "Three-byte escape expansion crossed the limit with partial success");
    require(
        three_byte_failure.reason ==
                ReviewedSourcePresentationFailureReason::
                    RenderedOutputLimitExceeded &&
            three_byte_failure.entry_index == 1 &&
            three_byte_failure.observed == three_byte_limit + 4 &&
            three_byte_failure.limit == three_byte_limit,
        "Three-byte escape expansion failure attribution drifted");

    const ReviewedSourceMaterializedReview four_byte_review =
        update_review({full_text_entry(
            "unsafe-four-byte",
            std::string{"before"} +
                bytes({0xf4, 0x90, 0x80, 0x80}) + "after")});
    const std::string four_byte_output = render_success(four_byte_review);
    const std::size_t four_byte_offset =
        four_byte_output.find("\\xF4\\x90\\x80\\x80");
    require(
        four_byte_offset != std::string::npos,
        "Invalid four-byte expansion fixture was not escaped");
    const std::uintmax_t four_byte_limit = four_byte_offset + 4;
    const ReviewedSourcePresentationResult four_byte_limited =
        render_reviewed_source_presentation_with_limit_for_test(
            seal_for_render(four_byte_review), four_byte_limit);
    const auto& four_byte_failure =
        require_arm<ReviewedSourcePresentationFailure>(
            four_byte_limited,
            "Invalid four-byte escape expansion crossed the limit with partial success");
    require(
        four_byte_failure.reason ==
                ReviewedSourcePresentationFailureReason::
                    RenderedOutputLimitExceeded &&
            four_byte_failure.entry_index == 0 &&
            four_byte_failure.observed == four_byte_limit + 4 &&
            four_byte_failure.limit == four_byte_limit,
        "Invalid four-byte escape expansion failure attribution drifted");
}

void run_tests() {
    test_change_types_and_sha1_metadata();
    test_sha256_metadata_and_patch();
    test_terminal_safe_content_encoding();
    test_shared_terminal_safe_content_corpus();
    test_representation_and_readiness_diagnostics();
    test_path_and_emphasis_policy();
    test_classification_crossing_priority();
    test_lifecycle_presentation();
    test_verified_boundary_rejects_forged_models();
    test_rendered_output_resource_contract();
}

} // namespace

int main() {
    try {
        run_tests();
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "reviewed source presentation tests: all checks passed\n";
    return 0;
}
