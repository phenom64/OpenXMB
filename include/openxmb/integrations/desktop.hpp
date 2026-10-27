#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace openxmb::integrations {

struct Result {
  bool success{};
  std::string message;
  explicit operator bool() const noexcept { return success; }
};

struct ProcessResult {
  bool success{};
  std::int64_t process_id{-1};
  std::string message;
  explicit operator bool() const noexcept { return success; }
};

// Arguments are passed directly to the program; no command shell is involved.
[[nodiscard]] ProcessResult launch_process(const std::vector<std::string>& arguments,
                                           const std::filesystem::path& working_directory = {});
[[nodiscard]] Result stop_process(std::int64_t process_id);
[[nodiscard]] std::optional<int> process_exit_status(std::int64_t process_id);
[[nodiscard]] std::optional<std::filesystem::path> find_executable(std::string_view name);
[[nodiscard]] std::filesystem::path executable_path();
[[nodiscard]] std::filesystem::path user_data_directory();

[[nodiscard]] Result validate_browser_url(std::string_view url);
// Resolve an address/search entry without invoking a shell or external browser.
[[nodiscard]] std::string browser_address_url(std::string_view text,std::string_view search_engine="duckduckgo");
struct BrowserSession {
  std::int64_t process_id{-1};
  bool dedicated_window{};
  std::string url;
  std::uintptr_t return_window{};
  std::shared_ptr<void> native_input;
  bool embedded_view{};
  std::filesystem::path state_file;
  // Keeps the Windows helper handle alive until its exit has been observed.
  std::shared_ptr<void> native_process;
};
struct BrowserPage {
  std::string title,url,hover;
  bool loading{},address_requested{};
  bool back{},forward{},ready{};
  std::string error,status;
};
[[nodiscard]] BrowserPage browser_page(const BrowserSession& session);
[[nodiscard]] Result browser_view_command(BrowserSession& session,std::string_view command,std::string_view value={});
enum class BrowserControl {
  pointer, click, context_click, scroll, tab, reverse_tab, back, forward,
  confirm, escape, address_bar, text, return_to_shell
};
struct BrowserInput {
  BrowserControl control{BrowserControl::pointer};
  // Pointer coordinates are normalized to the browser client area. Scroll uses y.
  double x{0.5};
  double y{0.5};
  std::string text;
};
[[nodiscard]] Result browser_input(BrowserSession& session, const BrowserInput& input);
[[nodiscard]] Result open_browser(std::string_view url, BrowserSession& session,
                                  const std::filesystem::path& profile_directory = {});
[[nodiscard]] Result close_browser(BrowserSession& session);
[[nodiscard]] Result capture_screenshot(const std::filesystem::path& destination);
[[nodiscard]] Result set_autostart(bool enabled, const std::filesystem::path& executable,
                                  const std::filesystem::path& configuration_root = {});
[[nodiscard]] bool autostart_enabled(const std::filesystem::path& configuration_root = {});

struct Preferences {
  std::string browser_url{"https://www.wikipedia.org/"};
  std::filesystem::path core;
  std::filesystem::path content;
};
[[nodiscard]] Preferences load_preferences(const std::filesystem::path& directory = {});
[[nodiscard]] Result save_preferences(const Preferences& preferences,
                                      const std::filesystem::path& directory = {});
[[nodiscard]] std::vector<std::filesystem::path> discover_libretro_cores();
[[nodiscard]] ProcessResult launch_libretro(const std::filesystem::path& core,
                                           const std::filesystem::path& content = {});

} // namespace openxmb::integrations
