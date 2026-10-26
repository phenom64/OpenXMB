#pragma once
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace openxmb::documents {
enum class Kind { pdf, comic_archive };
struct ArchivePage { std::string name; std::size_t entry{}; std::uintmax_t bytes{}; };
struct Workspace;
struct Document {
    std::filesystem::path path;
    Kind kind{Kind::pdf};
    std::size_t pages{};
    std::vector<ArchivePage> archive_pages;
    std::shared_ptr<Workspace> workspace;
};
std::shared_ptr<Document> open(const std::filesystem::path& path, std::stop_token stop = {});
std::filesystem::path render_page(const Document& document, std::size_t page,
                                  std::stop_token stop = {});
std::string page_label(const Document& document, std::size_t page);
void print(const Document& document, std::optional<std::size_t> page = std::nullopt,
           std::stop_token stop = {});
bool printing_available();
std::string information(const Document& document);
}
