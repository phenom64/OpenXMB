#include "openxmb/integrations/desktop.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

int main() {
  using namespace openxmb::integrations;
  const auto root = std::filesystem::temp_directory_path() /
    ("openxmb-browser-protocol-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(root);
  const auto require = [](bool value, const char* message) { if (!value) throw std::runtime_error(message); };
  try {
    BrowserSession session;
    session.embedded_view = true;
    session.url = "https://example.org/initial";
    session.state_file = root / "page-state.json";
    require(bool(browser_view_command(session, "hide")), "first command write failed");
    require(bool(browser_view_command(session, "show")), "replacing an existing Windows command failed");
    auto read_command = [&] { std::ifstream file(root / "page-command.json"); return nlohmann::json::parse(file); };
    auto message = read_command();
    require(message.at("command") == "show", "new command did not replace old command");
    const auto sequence = message.at("sequence").get<std::int64_t>();
    require(bool(browser_view_command(session, "navigate", "https://example.org/next?q=a&b=c")), "valid navigation rejected");
    message = read_command();
    require(message.at("sequence").get<std::int64_t>() > sequence, "command sequence did not increase");
    require(session.url == "https://example.org/next?q=a&b=c", "successful navigation did not update URL");
    require(!browser_view_command(session, "navigate", "javascript:alert(1)"), "unsafe navigation accepted");
    require(!browser_view_command(session, "unknown"), "unknown command accepted");
    require(browser_address_url("cats & dogs", "duckduckgo") == "https://duckduckgo.com/?q=cats%20%26%20dogs", "search query not encoded");
    require(browser_address_url("example.org/path", "google") == "https://example.org/path", "bare domain was not resolved");
    require(browser_address_url("https://example.org/", "bing") == "https://example.org/", "explicit URL changed");
    require(browser_address_url("hello", "bing") == "https://www.bing.com/search?q=hello", "search engine choice ignored");
    require(browser_address_url("hello", "Google") == "https://www.google.com/search?q=hello", "Settings engine display name ignored");
    require(!browser_view_command(session,"text",std::string(1,static_cast<char>(0xff))),"invalid UTF-8 accepted");
    require(bool(browser_view_command(session, "text", "Unicode \xe2\x9c\x93 \"quoted\"\n")), "Unicode text rejected");
    require(read_command().at("value") == "Unicode \xe2\x9c\x93 \"quoted\"\n", "text was not preserved");
    std::ofstream(session.state_file) << R"({"uri":"https://example.org/next","title":"Page title","hover":"https://example.org/link","loading":true,"address_requested":true,"back":true,"forward":true,"error":"Runtime test error","ready":true})";
    const auto page = browser_page(session);
    require(page.title == "Page title" && page.url == "https://example.org/next" && page.loading && page.address_requested, "page state not decoded");
    require(page.back && page.forward && page.ready && page.error == "Runtime test error", "browser capability/error state lost");
#ifdef _WIN32
    require(bool(browser_view_command(session, "configure", R"({"zoom":125,"downloads":""})")), "browser configuration command rejected");
    require(bool(browser_view_command(session, "clear_site_data")), "site-data command rejected");
    message=read_command();
    require(message.at("commands").size() >= 2, "commands between helper ticks were lost");
    std::ofstream(session.state_file) << nlohmann::json{{"command_sequence",message.at("sequence")}};
    require(bool(browser_view_command(session,"show")),"command after acknowledgement failed");
    require(read_command().at("commands").size()==1,"acknowledged commands were replayed");
#endif
    // Malformed / oversized state must never throw into the shell frame loop.
    std::ofstream(session.state_file) << "{";
    require(browser_page(session).url == session.url, "malformed state lost fallback URL");
    std::ofstream(session.state_file) << nlohmann::json{{"title",std::string(70000,'x')}};
    require(browser_page(session).title.empty(),"oversized state was accepted");
    const auto previous = session.url;
    session.state_file = root / "missing" / "page-state.json";
    require(!browser_view_command(session, "navigate", "https://example.org/failure"), "unwritable command accepted");
    require(session.url == previous, "failed navigation changed session URL");
    std::filesystem::remove_all(root);
    std::cout << "browser_protocol_tests: replacement, sequence, URL validation, Unicode, state and failed-write checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::filesystem::remove_all(root);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
