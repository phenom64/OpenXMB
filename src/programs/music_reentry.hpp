#pragma once
#include "openxmb/music/player.hpp"
#include <map>
#include <string>
#include <utility>

// Private UI entry policy, shared by the browser and viewer constructor.
namespace openxmb::music::reentry {
inline bool same_track(const std::filesystem::path &first, const std::filesystem::path &second) {
    std::error_code error;
    if (std::filesystem::equivalent(first, second, error))
        return true;
    // A decoder can remain live after its file was moved or removed. Do not
    // restart that stream just because filesystem identity is unavailable.
    error.clear();
    const auto absolute_first = std::filesystem::absolute(first, error).lexically_normal();
    if (error)
        return first.lexically_normal() == second.lexically_normal();
    const auto absolute_second = std::filesystem::absolute(second, error).lexically_normal();
    return !error && absolute_first == absolute_second;
}

template <class Start>
bool start_new(Player &player, const std::map<std::string, std::string> &preferences, Start &&start) {
    const auto shuffle = preferences.find("music.shuffle"), repeat = preferences.find("music.repeat");
    player.set_shuffle(shuffle != preferences.end() && shuffle->second == "On");
    player.set_repeat(repeat == preferences.end() ? Repeat::off
                      : repeat->second == "One"   ? Repeat::one
                      : repeat->second == "All"   ? Repeat::all
                                                  : Repeat::off);
    return std::forward<Start>(start)();
}

template <class Start>
bool open(Player &player, const std::filesystem::path &path, const std::map<std::string, std::string> &preferences,
          Start &&start) {
    const auto status = player.snapshot().status;
    const auto *current = player.queue().current();
    if ((status == Status::playing || status == Status::paused) && current && same_track(current->path, path))
        return true;
    return start_new(player, preferences, std::forward<Start>(start));
}
} // namespace openxmb::music::reentry
