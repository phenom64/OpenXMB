#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openxmb::setup {

using Instant = std::chrono::system_clock::time_point;
enum class Page { brand, company, language, time_zone, date_time, user_name, summary, complete };
enum class Command { left, right, up, down, confirm, back, extra };

struct Language { std::string code, label; };
struct Zone {
    std::string id, cities;
    int standard_minutes{};
    bool observes_daylight{}, rules_available{};
    [[nodiscard]] std::string label() const;
};
struct CivilTime {
    int year{2000}, month{1}, day{1}, hour{}, minute{}, second{};
    bool operator==(const CivilTime&) const = default;
};
struct Draft {
    std::string language, region, user_name;
    std::size_t language_index{}, zone_index{};
    bool automatic_daylight{}, automatic_time{true};
    std::chrono::seconds clock_offset{};
};

[[nodiscard]] bool requires_setup(bool saved_profile, std::optional<bool> completed);
[[nodiscard]] bool valid(CivilTime value);
[[nodiscard]] CivilTime adjust_segment(CivilTime value, int segment, int delta);
[[nodiscard]] std::vector<Language> supported_languages();
[[nodiscard]] std::vector<Zone> time_zones();
[[nodiscard]] Draft detect_defaults(const std::vector<Language>& languages, const std::vector<Zone>& zones,
                                    std::string detected_locale = {});
[[nodiscard]] CivilTime local_time(const Zone& zone, Instant now, bool automatic_daylight);
[[nodiscard]] std::optional<Instant> to_instant(const Zone& zone, CivilTime value, bool automatic_daylight);
[[nodiscard]] std::string format_time(CivilTime value, bool seconds = false);

// The draft is committed by the shell only after Summary is accepted.
// Clock and time-zone choices affect OpenXMB, never the host OS.
class Model {
public:
    Model();
    Model(std::vector<Language> languages, std::vector<Zone> zones, Draft draft);
    [[nodiscard]] Page page() const { return page_; }
    [[nodiscard]] const std::vector<Language>& languages() const { return languages_; }
    [[nodiscard]] const std::vector<Zone>& zones() const { return zones_; }
    [[nodiscard]] Draft& draft() { return draft_; }
    [[nodiscard]] const Draft& draft() const { return draft_; }
    [[nodiscard]] bool editing() const { return editing_; }
    [[nodiscard]] int segment() const { return segment_; }
    [[nodiscard]] bool daylight_focused() const { return daylight_focused_; }
    [[nodiscard]] bool confirmation() const { return confirmation_; }
    [[nodiscard]] bool confirmation_yes() const { return confirmation_yes_; }
    [[nodiscard]] const std::string& error() const { return error_; }
    [[nodiscard]] CivilTime current_time(Instant now) const;
    void command(Command command, Instant now = std::chrono::system_clock::now());
    void advance_intro();
    void set_name(std::string name);
    // Deterministic scene selection used by the native capture harness.
    void show_page(Page page);
private:
    std::vector<Language> languages_;
    std::vector<Zone> zones_;
    Draft draft_;
    Page page_{Page::brand};
    bool editing_{}, daylight_focused_{}, confirmation_{}, confirmation_yes_{};
    int segment_{};
    CivilTime edited_{};
    std::string error_;
};

}
