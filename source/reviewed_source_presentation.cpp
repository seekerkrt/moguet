#include "reviewed_source_presentation.hpp"

#include "localization.hpp"
#include "terminal_safe_text.hpp"

#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

constexpr std::string_view GIT_TOOL_NAME = "Git";
constexpr std::string_view PKGBUILD_FILE_NAME = "PKGBUILD";
constexpr std::string_view INSTALL_FILE_SUFFIX = "install";

using SizeCheckResult = std::variant<
    std::uintmax_t,
    ReviewedSourcePresentationFailure>;

ReviewedSourcePresentationFailure inconsistent_failure(
    std::size_t entry_index) {
    return ReviewedSourcePresentationFailure{
        ReviewedSourcePresentationFailureReason::
            InconsistentMaterializedReview,
        entry_index, 0, 0};
}

SizeCheckResult checked_rendered_output_size(
    std::uintmax_t current,
    std::uintmax_t additional,
    std::uintmax_t limit,
    std::size_t entry_index) {
    const bool overflow = additional >
                          std::numeric_limits<std::uintmax_t>::max() - current;
    const std::uintmax_t observed = overflow
                                        ? std::numeric_limits<std::uintmax_t>::max()
                                        : current + additional;
    if(overflow || observed > limit) {
        return ReviewedSourcePresentationFailure{
            ReviewedSourcePresentationFailureReason::
                RenderedOutputLimitExceeded,
            entry_index, observed, limit};
    }
    return observed;
}

class RenderState final {
public:
    explicit RenderState(std::uintmax_t limit) noexcept : limit_(limit) {
    }

    bool append(std::string_view value, std::size_t entry_index = 0) {
        if(failure_.has_value()) return false;
        const SizeCheckResult checked = checked_rendered_output_size(
            output_.size(), value.size(), limit_, entry_index);
        if(const auto* failure =
               std::get_if<ReviewedSourcePresentationFailure>(&checked)) {
            failure_ = *failure;
            return false;
        }
        output_.append(value);
        return true;
    }

    bool append_number(
        std::uintmax_t value, std::size_t entry_index = 0) {
        return append(std::to_string(value), entry_index);
    }

    void fail_inconsistent(std::size_t entry_index) {
        if(!failure_.has_value()) {
            failure_ = inconsistent_failure(entry_index);
        }
    }

    [[nodiscard]] bool failed() const noexcept {
        return failure_.has_value();
    }

    ReviewedSourcePresentationResult finish() {
        if(failure_.has_value()) return *failure_;
        return ReviewedSourceRenderedPresentation{std::move(output_)};
    }

private:
    std::uintmax_t limit_;
    std::string output_;
    std::optional<ReviewedSourcePresentationFailure> failure_;
};

void append_safe_content(
    RenderState& state,
    std::string_view bytes,
    std::size_t entry_index) {
    const bool completed = terminal_safe_text::append_escaped_utf8(
        bytes, [&state, entry_index](std::string_view segment) {
            return state.append(segment, entry_index);
        });
    (void)completed;
}

std::optional<std::string> object_format_display(
    GitObjectFormat format) {
    switch(format) {
        case GitObjectFormat::Sha1:
            return "SHA-1";
        case GitObjectFormat::Sha256:
            return "SHA-256";
    }
    return std::nullopt;
}

std::optional<std::string> mode_display(ReviewedSourceFileMode mode) {
    switch(mode) {
        case ReviewedSourceFileMode::Regular:
            return localization::translate_message("100644 (regular file)");
        case ReviewedSourceFileMode::Executable:
            return localization::translate_message("100755 (executable file)");
        case ReviewedSourceFileMode::SymbolicLink:
            return localization::translate_message("120000 (symbolic link)");
        case ReviewedSourceFileMode::Gitlink:
            return localization::translate_message("160000 (Gitlink)");
    }
    return std::nullopt;
}

std::optional<std::string> classification_display(
    ReviewedSourceFileClassification classification) {
    switch(classification) {
        case ReviewedSourceFileClassification::TrackedSource:
            return localization::translate_message("tracked source");
        case ReviewedSourceFileClassification::GeneratedMetadata:
            return localization::translate_message("generated metadata");
    }
    return std::nullopt;
}

std::optional<std::string> observation_kind_display(
    ReviewedSourceBlobContentKind kind) {
    switch(kind) {
        case ReviewedSourceBlobContentKind::LineReviewable:
            return localization::translate_message("line-reviewable content");
        case ReviewedSourceBlobContentKind::ContainsNul:
            return localization::translate_message("content containing NUL bytes");
        case ReviewedSourceBlobContentKind::Gitlink:
            return localization::translate_message("Gitlink metadata");
    }
    return std::nullopt;
}

std::optional<std::string> representation_display(
    ReviewedSourceReviewRepresentation representation) {
    switch(representation) {
        case ReviewedSourceReviewRepresentation::CompleteTextPatch:
            return localization::translate_message("complete text patch");
        case ReviewedSourceReviewRepresentation::CompleteFullText:
            return localization::translate_message("complete full text");
        case ReviewedSourceReviewRepresentation::NoContentChange:
            return localization::translate_message("no content change");
        case ReviewedSourceReviewRepresentation::ContainsNul:
            return localization::translate_message("content containing NUL bytes");
        case ReviewedSourceReviewRepresentation::GitlinkMetadata:
            return localization::translate_message("Gitlink metadata");
        case ReviewedSourceReviewRepresentation::MixedTextAndNonText:
            return localization::translate_message("mixed text and non-text content");
    }
    return std::nullopt;
}

std::optional<std::string> readiness_display(
    ReviewedSourceReviewReadiness readiness) {
    switch(readiness) {
        case ReviewedSourceReviewReadiness::Complete:
            return localization::translate_message("complete");
        case ReviewedSourceReviewReadiness::ManualInspectionRequired:
            return localization::translate_message("manual inspection required");
        case ReviewedSourceReviewReadiness::SensitiveSourceUnrenderable:
            return localization::translate_message(
                "sensitive source cannot be rendered safely");
    }
    return std::nullopt;
}

std::optional<std::string> emphasis_display(
    ReviewedSourceReviewEmphasis emphasis) {
    switch(emphasis) {
        case ReviewedSourceReviewEmphasis::Ordinary:
            return localization::translate_message("ordinary");
        case ReviewedSourceReviewEmphasis::Sensitive:
            return localization::translate_message("review-sensitive");
    }
    return std::nullopt;
}

enum class ChangeStatus {
    Added,
    Modified,
    Deleted,
    Renamed,
    TypeChanged,
};

std::optional<std::string> change_status_display(ChangeStatus status) {
    switch(status) {
        case ChangeStatus::Added:
            return localization::translate_message("added");
        case ChangeStatus::Modified:
            return localization::translate_message("modified");
        case ChangeStatus::Deleted:
            return localization::translate_message("deleted");
        case ChangeStatus::Renamed:
            return localization::translate_message("renamed");
        case ChangeStatus::TypeChanged:
            return localization::translate_message("type changed");
    }
    return std::nullopt;
}

struct ChangeView {
    const ReviewedSourceFileVersion* old_version = nullptr;
    const ReviewedSourceFileVersion* new_version = nullptr;
    const ReviewedSourceContentChange* content = nullptr;
    ChangeStatus status = ChangeStatus::Modified;
    std::optional<std::uint8_t> rename_similarity;
};

ChangeView change_view(const ReviewedSourceFileChange& change) {
    return std::visit(
        [](const auto& value) -> ChangeView {
            using Change = std::decay_t<decltype(value)>;
            if constexpr(std::is_same_v<Change, ReviewedSourceAdded>) {
                return ChangeView{
                    nullptr, &value.new_version, &value.content,
                    ChangeStatus::Added, std::nullopt};
            } else if constexpr(std::is_same_v<
                                    Change,
                                    ReviewedSourceModified>) {
                return ChangeView{
                    &value.old_version, &value.new_version,
                    &value.content, ChangeStatus::Modified,
                    std::nullopt};
            } else if constexpr(std::is_same_v<
                                    Change,
                                    ReviewedSourceDeleted>) {
                return ChangeView{
                    &value.old_version, nullptr, &value.content,
                    ChangeStatus::Deleted, std::nullopt};
            } else if constexpr(std::is_same_v<
                                    Change,
                                    ReviewedSourceRenamed>) {
                return ChangeView{
                    &value.old_version, &value.new_version,
                    &value.content, ChangeStatus::Renamed,
                    value.similarity};
            } else {
                return ChangeView{
                    &value.old_version, &value.new_version,
                    &value.content, ChangeStatus::TypeChanged,
                    std::nullopt};
            }
        },
        change);
}

bool append_mapped_value(
    RenderState& state,
    const std::optional<std::string>& value,
    std::size_t entry_index) {
    if(!value.has_value()) {
        state.fail_inconsistent(entry_index);
        return false;
    }
    return state.append(*value, entry_index);
}

void render_revision(
    RenderState& state,
    std::string_view label,
    const SourceRevisionIdentity& revision) {
    const GitObjectFormat* format = revision.git_object_format();
    const std::string* object_id = revision.git_commit();
    if(revision.state() != SourceRevisionState::Known || format == nullptr ||
       object_id == nullptr) {
        state.fail_inconsistent(0);
        return;
    }
    state.append(label);
    append_mapped_value(state, object_format_display(*format), 0);
    state.append(" ");
    state.append(*object_id);
    state.append("\n");
}

void render_version_fields(
    RenderState& state,
    std::string_view side,
    const ReviewedSourceFileVersion* version,
    std::size_t entry_index) {
    state.append("  ", entry_index);
    state.append(localization::format_translated_message(
                     "{} path: ", side),
                 entry_index);
    if(version == nullptr) {
        state.append(localization::translate_message("none\n"), entry_index);
    } else {
        state.append(version->path().escaped_display(), entry_index);
        state.append("\n", entry_index);
    }

    state.append("  ", entry_index);
    state.append(localization::format_translated_message(
                     "{} mode: ", side),
                 entry_index);
    if(version == nullptr) {
        state.append(localization::translate_message("none\n"), entry_index);
    } else {
        append_mapped_value(
            state, mode_display(version->mode()), entry_index);
        state.append("\n", entry_index);
    }

    state.append("  ", entry_index);
    state.append(localization::format_translated_message(
                     "{} object: ", side),
                 entry_index);
    if(version == nullptr) {
        state.append(localization::translate_message("none\n"), entry_index);
    } else {
        append_mapped_value(
            state,
            object_format_display(version->object_id().format()),
            entry_index);
        state.append(" ", entry_index);
        state.append(version->object_id().value(), entry_index);
        state.append("\n", entry_index);
    }

    state.append("  ", entry_index);
    state.append(localization::format_translated_message(
                     "{} size: ", side),
                 entry_index);
    if(version == nullptr) {
        state.append(localization::translate_message("none\n"), entry_index);
    } else if(!version->blob_size().has_value()) {
        state.append(
            localization::translate_message("unavailable\n"),
            entry_index);
    } else {
        state.append(localization::format_translated_message(
                         "{} bytes\n", *version->blob_size()),
                     entry_index);
    }

    state.append("  ", entry_index);
    state.append(localization::format_translated_message(
                     "{} classification: ", side),
                 entry_index);
    if(version == nullptr) {
        state.append(localization::translate_message("none\n"), entry_index);
    } else {
        append_mapped_value(
            state,
            classification_display(version->classification()),
            entry_index);
        state.append("\n", entry_index);
    }
}

void render_observation_kind(
    RenderState& state,
    std::string_view side,
    const std::optional<ReviewedSourceBlobObservation>& observation,
    std::size_t entry_index) {
    state.append("  ", entry_index);
    state.append(localization::format_translated_message(
                     "{} content kind: ", side),
                 entry_index);
    if(!observation.has_value()) {
        state.append(localization::translate_message("none\n"), entry_index);
        return;
    }
    append_mapped_value(
        state, observation_kind_display(observation->kind), entry_index);
    state.append("\n", entry_index);
}

void render_git_marker(
    RenderState& state,
    const ReviewedSourceContentChange& content,
    std::size_t entry_index) {
    state.append(
        localization::format_translated_message(
            "  {} change marker: ", GIT_TOOL_NAME),
        entry_index);
    std::visit(
        [&state, entry_index](const auto& value) {
            using Content = std::decay_t<decltype(value)>;
            if constexpr(std::is_same_v<
                             Content,
                             ReviewedSourceTextChange>) {
                state.append(localization::format_translated_message(
                                 "text (added lines: {}; deleted lines: {})",
                                 value.added_lines,
                                 value.deleted_lines),
                             entry_index);
            } else {
                state.append(
                    localization::translate_message("binary"),
                    entry_index);
            }
        },
        content);
    state.append("\n", entry_index);
}

bool all_present_versions_are_generated_metadata(
    const ChangeView& view) noexcept {
    if(view.old_version != nullptr &&
       view.old_version->classification() !=
           ReviewedSourceFileClassification::GeneratedMetadata) {
        return false;
    }
    if(view.new_version != nullptr &&
       view.new_version->classification() !=
           ReviewedSourceFileClassification::GeneratedMetadata) {
        return false;
    }
    return view.old_version != nullptr || view.new_version != nullptr;
}

void render_status_summary(
    RenderState& state,
    const ChangeView& view,
    std::size_t entry_index) {
    state.append(localization::translate_message("  change status: "),
                 entry_index);
    append_mapped_value(
        state, change_status_display(view.status), entry_index);
    if(view.status == ChangeStatus::Renamed && view.old_version != nullptr &&
       view.new_version != nullptr) {
        state.append(" ", entry_index);
        state.append(
            view.old_version->path().escaped_display(), entry_index);
        state.append(" -> ", entry_index);
        state.append(
            view.new_version->path().escaped_display(), entry_index);
    } else if(view.status == ChangeStatus::TypeChanged &&
              view.old_version != nullptr && view.new_version != nullptr) {
        state.append(" ", entry_index);
        append_mapped_value(
            state, mode_display(view.old_version->mode()), entry_index);
        state.append(" -> ", entry_index);
        append_mapped_value(
            state, mode_display(view.new_version->mode()), entry_index);
    }
    state.append("\n", entry_index);
}

void render_readiness_diagnostic(
    RenderState& state,
    ReviewedSourceReviewReadiness readiness,
    std::size_t entry_index) {
    state.append(
        localization::translate_message("  review readiness detail: "),
        entry_index);
    switch(readiness) {
        case ReviewedSourceReviewReadiness::Complete:
            state.append(
                localization::translate_message(
                    "complete terminal-safe content presentation\n"),
                entry_index);
            return;
        case ReviewedSourceReviewReadiness::ManualInspectionRequired:
            state.append(
                localization::translate_message(
                    "WARNING: manual inspection is required; metadata alone is not a complete content review\n"),
                entry_index);
            return;
        case ReviewedSourceReviewReadiness::SensitiveSourceUnrenderable:
            state.append(
                localization::translate_message(
                    "WARNING: review-sensitive source content cannot be rendered safely; metadata does not complete the review\n"),
                entry_index);
            return;
    }
    state.fail_inconsistent(entry_index);
}

const ReviewedSourceTextContent* line_content(
    RenderState& state,
    const std::optional<ReviewedSourceBlobObservation>& observation,
    std::size_t entry_index) {
    if(!observation.has_value() ||
       observation->kind != ReviewedSourceBlobContentKind::LineReviewable) {
        return nullptr;
    }
    if(!observation->text) {
        state.fail_inconsistent(entry_index);
        return nullptr;
    }
    return observation->text.get();
}

void render_text_content(
    RenderState& state,
    const ReviewedSourceTextContent& content,
    char prefix,
    std::size_t entry_index) {
    if(content.lines.empty()) {
        state.append(
            localization::translate_message("    (empty)\n"),
            entry_index);
        return;
    }
    for(const ReviewedSourceTextLine& line : content.lines) {
        if(state.failed()) return;
        state.append("    ", entry_index);
        const char prefix_text[1]{prefix};
        state.append(
            std::string_view(prefix_text, sizeof(prefix_text)),
            entry_index);
        append_safe_content(state, line.bytes, entry_index);
        state.append("\n", entry_index);
        if(!line.has_newline) {
            state.append(
                localization::translate_message(
                    "    \\ No newline at end of file\n"),
                entry_index);
        }
    }
}

void render_complete_full_text(
    RenderState& state,
    const ReviewedSourceReviewEntry& entry,
    std::size_t entry_index) {
    const ReviewedSourceTextContent* old_text =
        line_content(state, entry.old_observation, entry_index);
    const ReviewedSourceTextContent* new_text =
        line_content(state, entry.new_observation, entry_index);
    if(state.failed()) return;
    const std::size_t content_count =
        static_cast<std::size_t>(old_text != nullptr) +
        static_cast<std::size_t>(new_text != nullptr);
    if(content_count != 1) {
        state.fail_inconsistent(entry_index);
        return;
    }
    if(old_text != nullptr) {
        state.append(
            localization::translate_message("  old full text:\n"),
            entry_index);
        render_text_content(state, *old_text, '-', entry_index);
    } else {
        state.append(
            localization::translate_message("  new full text:\n"),
            entry_index);
        render_text_content(state, *new_text, '+', entry_index);
    }
}

std::optional<char> patch_line_prefix(
    ReviewedSourcePatchLineKind kind) noexcept {
    switch(kind) {
        case ReviewedSourcePatchLineKind::Context:
            return ' ';
        case ReviewedSourcePatchLineKind::Removed:
            return '-';
        case ReviewedSourcePatchLineKind::Added:
            return '+';
    }
    return std::nullopt;
}

void render_patch(
    RenderState& state,
    const ReviewedSourceReviewEntry& entry,
    const ChangeView& view,
    std::size_t entry_index) {
    if(!entry.patch.has_value() || entry.patch->hunks.empty() ||
       view.old_version == nullptr || view.new_version == nullptr ||
       entry.patch->old_object_id != view.old_version->object_id() ||
       entry.patch->new_object_id != view.new_version->object_id()) {
        state.fail_inconsistent(entry_index);
        return;
    }
    state.append(
        localization::translate_message("  verified text patch:\n"),
        entry_index);
    for(const ReviewedSourcePatchHunk& hunk : entry.patch->hunks) {
        if(state.failed()) return;
        state.append("    @@ -", entry_index);
        state.append_number(hunk.old_start, entry_index);
        state.append(",", entry_index);
        state.append_number(hunk.old_count, entry_index);
        state.append(" +", entry_index);
        state.append_number(hunk.new_start, entry_index);
        state.append(",", entry_index);
        state.append_number(hunk.new_count, entry_index);
        state.append(" @@\n", entry_index);
        for(const ReviewedSourcePatchLine& line : hunk.lines) {
            const auto prefix = patch_line_prefix(line.kind);
            if(!prefix.has_value()) {
                state.fail_inconsistent(entry_index);
                return;
            }
            state.append("    ", entry_index);
            const char prefix_text[1]{*prefix};
            state.append(
                std::string_view(prefix_text, sizeof(prefix_text)),
                entry_index);
            append_safe_content(state, line.line.bytes, entry_index);
            state.append("\n", entry_index);
            if(!line.line.has_newline) {
                state.append(
                    localization::translate_message(
                        "    \\ No newline at end of file\n"),
                    entry_index);
            }
        }
    }
}

void render_no_content_change(
    RenderState& state,
    const ChangeView& view,
    std::size_t entry_index) {
    state.append(
        localization::translate_message(
            "  content: content unchanged\n"),
        entry_index);
    if(view.status == ChangeStatus::Renamed) {
        state.append(
            localization::translate_message(
                "  metadata change: rename only (content unchanged)\n"),
            entry_index);
    }
    if(view.old_version != nullptr && view.new_version != nullptr &&
       view.old_version->mode() != view.new_version->mode()) {
        state.append(
            localization::translate_message(
                "  metadata change: mode changed (content unchanged)\n"),
            entry_index);
    }
    if(view.status != ChangeStatus::Renamed && view.old_version != nullptr &&
       view.new_version != nullptr &&
       view.old_version->mode() == view.new_version->mode()) {
        state.append(
            localization::translate_message(
                "  metadata change: change type retained despite unchanged content\n"),
            entry_index);
    }
}

void render_mixed_text(
    RenderState& state,
    const ReviewedSourceReviewEntry& entry,
    std::size_t entry_index) {
    state.append(
        localization::translate_message(
            "  content: mixed text and non-line-reviewable content; raw bytes withheld\n"),
        entry_index);
    const ReviewedSourceTextContent* old_text =
        line_content(state, entry.old_observation, entry_index);
    const ReviewedSourceTextContent* new_text =
        line_content(state, entry.new_observation, entry_index);
    if(state.failed()) return;
    if(old_text != nullptr) {
        state.append(
            localization::translate_message(
                "  line-reviewable old content:\n"),
            entry_index);
        render_text_content(state, *old_text, '-', entry_index);
    }
    if(new_text != nullptr) {
        state.append(
            localization::translate_message(
                "  line-reviewable new content:\n"),
            entry_index);
        render_text_content(state, *new_text, '+', entry_index);
    }
}

void render_representation(
    RenderState& state,
    const ReviewedSourceReviewEntry& entry,
    const ChangeView& view,
    std::size_t entry_index) {
    const bool expects_patch = entry.representation ==
                               ReviewedSourceReviewRepresentation::CompleteTextPatch;
    if(expects_patch != entry.patch.has_value()) {
        state.fail_inconsistent(entry_index);
        return;
    }
    switch(entry.representation) {
        case ReviewedSourceReviewRepresentation::CompleteTextPatch:
            render_patch(state, entry, view, entry_index);
            return;
        case ReviewedSourceReviewRepresentation::CompleteFullText:
            render_complete_full_text(state, entry, entry_index);
            return;
        case ReviewedSourceReviewRepresentation::NoContentChange:
            render_no_content_change(state, view, entry_index);
            return;
        case ReviewedSourceReviewRepresentation::ContainsNul:
            state.append(
                localization::translate_message(
                    "  content: binary/non-line-reviewable content; raw bytes withheld\n"),
                entry_index);
            return;
        case ReviewedSourceReviewRepresentation::GitlinkMetadata:
            state.append(
                localization::translate_message(
                    "  content: Gitlink/submodule metadata only; recursive review not performed\n"),
                entry_index);
            return;
        case ReviewedSourceReviewRepresentation::MixedTextAndNonText:
            render_mixed_text(state, entry, entry_index);
            return;
    }
    state.fail_inconsistent(entry_index);
}

void render_entry(
    RenderState& state,
    const ReviewedSourceReviewEntry& entry,
    std::size_t entry_index) {
    const ChangeView view = change_view(entry.change);
    if(view.content == nullptr) {
        state.fail_inconsistent(entry_index);
        return;
    }

    state.append(localization::format_translated_message(
                     "review entry {}:\n", entry_index + 1),
                 entry_index);
    render_status_summary(state, view, entry_index);
    render_version_fields(
        state, localization::translate_message("old"),
        view.old_version, entry_index);
    render_version_fields(
        state, localization::translate_message("new"),
        view.new_version, entry_index);
    state.append(
        localization::translate_message("  rename similarity: "),
        entry_index);
    if(view.rename_similarity.has_value()) {
        state.append_number(*view.rename_similarity, entry_index);
        state.append("%\n", entry_index);
    } else {
        state.append(localization::translate_message("none\n"), entry_index);
    }
    render_git_marker(state, *view.content, entry_index);
    render_observation_kind(
        state, localization::translate_message("old"),
        entry.old_observation, entry_index);
    render_observation_kind(
        state, localization::translate_message("new"),
        entry.new_observation, entry_index);

    state.append(
        localization::translate_message("  review emphasis: "),
        entry_index);
    append_mapped_value(
        state, emphasis_display(entry.emphasis), entry_index);
    state.append("\n", entry_index);
    state.append(
        localization::translate_message("  presentation priority: "),
        entry_index);
    if(entry.emphasis == ReviewedSourceReviewEmphasis::Sensitive) {
        state.append(
            localization::translate_message("review-sensitive\n"),
            entry_index);
        state.append(
            localization::format_translated_message(
                "  review-sensitive guidance: explicitly inspect root {}/{} source content\n",
                PKGBUILD_FILE_NAME, INSTALL_FILE_SUFFIX),
            entry_index);
    } else if(all_present_versions_are_generated_metadata(view)) {
        state.append(
            localization::translate_message(
                "lower-priority generated metadata\n"),
            entry_index);
        state.append(
            localization::translate_message(
                "  generated metadata guidance: lower emphasis; not source-review authority\n"),
            entry_index);
    } else {
        state.append(localization::translate_message("ordinary\n"),
                     entry_index);
    }

    state.append(
        localization::translate_message("  content representation: "),
        entry_index);
    append_mapped_value(
        state,
        representation_display(entry.representation),
        entry_index);
    state.append("\n", entry_index);
    state.append(
        localization::translate_message("  review readiness: "),
        entry_index);
    append_mapped_value(
        state, readiness_display(entry.readiness), entry_index);
    state.append("\n", entry_index);
    render_readiness_diagnostic(state, entry.readiness, entry_index);
    if(state.failed()) return;
    render_representation(state, entry, view, entry_index);
}

void render_review_body(
    RenderState& state,
    const ReviewedSourceReviewBody& body) {
    state.append(localization::translate_message("overall review readiness: "));
    append_mapped_value(state, readiness_display(body.readiness), 0);
    state.append(localization::format_translated_message(
        "\nreview entry count: {}\nreview entries:\n",
        body.entries.size()));
    if(body.entries.empty()) {
        state.append(localization::translate_message("  (none)\n"));
        return;
    }
    for(std::size_t index = 0;
        index < body.entries.size() && !state.failed(); ++index) {
        render_entry(state, body.entries[index], index);
    }
}

std::optional<std::string> history_relation_display(
    ReviewedSourceHistoryRelation relation) {
    switch(relation) {
        case ReviewedSourceHistoryRelation::Ancestor:
            return localization::translate_message(
                "reviewed baseline is an ancestor of the target");
        case ReviewedSourceHistoryRelation::NonAncestor:
            return localization::translate_message(
                "reviewed baseline is not an ancestor of the target");
    }
    return std::nullopt;
}

std::optional<std::string> baseline_reason_display(
    ReviewedSourceBaselineUnavailableReason reason) {
    switch(reason) {
        case ReviewedSourceBaselineUnavailableReason::MissingOrNotCommit:
            return localization::translate_message(
                "baseline object is missing or is not a commit");
    }
    return std::nullopt;
}

ReviewedSourcePresentationResult render_with_limit(
    const ReviewedSourceVerifiedMaterializedReview& verified_review,
    std::uintmax_t limit) {
    const ReviewedSourceMaterializedReview& review = verified_review.review();
    const auto inconsistent_entry =
        reviewed_source_materialized_review_inconsistent_entry(review);
    if(inconsistent_entry.has_value()) {
        return inconsistent_failure(*inconsistent_entry);
    }
    RenderState state(limit);
    std::visit(
        [&state](const auto& value) {
            using Review = std::decay_t<decltype(value)>;
            if constexpr(std::is_same_v<
                             Review,
                             ReviewedSourceMaterializedInitialFullReview>) {
                state.append(localization::translate_message(
                    "review type: initial full review\n"));
                render_revision(
                    state,
                    localization::translate_message(
                        "target revision: "),
                    value.target);
                render_review_body(state, value.review);
            } else if constexpr(std::is_same_v<
                                    Review,
                                    ReviewedSourceMaterializedAlreadyReviewed>) {
                state.append(localization::translate_message(
                    "review type: already reviewed\n"));
                render_revision(
                    state,
                    localization::translate_message(
                        "reviewed revision: "),
                    value.revision);
                state.append(localization::translate_message(
                    "review result: already reviewed; no acceptance prompt is required\n"));
            } else if constexpr(std::is_same_v<
                                    Review,
                                    ReviewedSourceMaterializedUpdateReview>) {
                state.append(localization::translate_message(
                    "review type: update review\n"));
                render_revision(
                    state,
                    localization::translate_message(
                        "baseline revision: "),
                    value.baseline);
                render_revision(
                    state,
                    localization::translate_message(
                        "target revision: "),
                    value.target);
                state.append(localization::translate_message(
                    "history relation: "));
                append_mapped_value(
                    state,
                    history_relation_display(value.relation), 0);
                state.append("\n");
                render_review_body(state, value.review);
            } else {
                state.append(localization::translate_message(
                    "review type: explicit full rebaseline\n"));
                render_revision(
                    state,
                    localization::translate_message(
                        "unavailable baseline: "),
                    value.unavailable_baseline);
                render_revision(
                    state,
                    localization::translate_message(
                        "target revision: "),
                    value.target);
                state.append(localization::translate_message(
                    "baseline unavailable reason: "));
                append_mapped_value(
                    state,
                    baseline_reason_display(value.reason), 0);
                state.append("\n");
                render_review_body(state, value.review);
            }
        },
        review);
    return state.finish();
}

} // namespace

ReviewedSourcePresentationResult render_reviewed_source_presentation(
    const ReviewedSourceVerifiedMaterializedReview& review) {
    return render_with_limit(review, REVIEWED_SOURCE_RENDERED_OUTPUT_LIMIT);
}

#ifdef MOGUET_ENABLE_REVIEWED_SOURCE_PRESENTATION_TEST_HOOKS
ReviewedSourcePresentationSizeCheckResult
checked_reviewed_source_presentation_size_for_test(
    std::uintmax_t current,
    std::uintmax_t additional,
    std::uintmax_t limit) {
    return checked_rendered_output_size(current, additional, limit, 0);
}

ReviewedSourcePresentationResult
render_reviewed_source_presentation_with_limit_for_test(
    const ReviewedSourceVerifiedMaterializedReview& review,
    std::uintmax_t limit) {
    return render_with_limit(review, limit);
}
#endif
