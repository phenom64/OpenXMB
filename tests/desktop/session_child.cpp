#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace fs = std::filesystem;
int child(const std::vector<std::string>& arguments) {
  if (arguments.size() < 4) return 90;
  const auto text = fs::current_path().u8string();
  std::ofstream("arguments.json", std::ios::binary) << nlohmann::json{{"cwd", std::string(text.begin(), text.end())}, {"arguments", arguments}};
  if (arguments[1] == "wait") {
    std::ofstream(fs::path(std::u8string(arguments[2].begin(), arguments[2].end()))) << "ready";
    const auto release = fs::path(std::u8string(arguments[3].begin(), arguments[3].end()));
    for (unsigned i = 0; i < 6000 && !fs::exists(release); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (!fs::exists(release)) return 91;
    std::ofstream("completed") << "completed";
    return 0;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(std::stoi(arguments[3])));
  return std::stoi(arguments[2]);
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
  std::vector<std::string> arguments;
  for (int i = 0; i < argc; ++i) {
    const auto size = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
    std::string text(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, text.data(), size, nullptr, nullptr);
    text.pop_back(); arguments.push_back(std::move(text));
  }
  return child(arguments);
}
#else
int main(int argc, char** argv) { return child({argv, argv + argc}); }
#endif
