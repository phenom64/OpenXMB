/* XMBShell, a console-like desktop shell
 * Copyright (C) 2025 - JCM
 *
 * This file (or substantial portions of it) is derived from XMBShell:
 *   https://github.com/JnCrMx/xmbshell
 *
 * Modified by Syndromatic Ltd for OpenXMB.
 * Portions Copyright (C) 2025 Syndromatic Ltd, Kavish Krishnakumar.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

module;

#include <array>
#include <cstdlib>
#include <cerrno>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

export module openxmb.app:menu_utils;
import dreamrender;
import :menu_base;

#if !defined(_WIN32)
extern "C" char **environ;
#endif

export namespace menu {

inline bool command_available(std::string_view command)
{
    if(command.empty()) {
        return false;
    }

    std::filesystem::path command_path{std::string(command)};
    if(command_path.has_parent_path()) {
        std::error_code ec;
        if(!std::filesystem::exists(command_path, ec) || ec) {
            return false;
        }
#if defined(_WIN32)
        return true;
#else
        return access(command_path.c_str(), X_OK) == 0;
#endif
    }

    const char* path_env = std::getenv("PATH");
    if(path_env == nullptr) {
        return false;
    }

    std::string paths{path_env};
#if defined(_WIN32)
    constexpr char separator = ';';
#else
    constexpr char separator = ':';
#endif

    std::size_t start = 0;
    while(start <= paths.size()) {
        auto end = paths.find(separator, start);
        auto directory = paths.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if(directory.empty()) {
            directory = ".";
        }
        std::filesystem::path candidate = std::filesystem::path(directory) / std::string(command);
        std::error_code ec;
        if(std::filesystem::exists(candidate, ec) && !ec) {
#if defined(_WIN32)
            return true;
#else
            if(access(candidate.c_str(), X_OK) == 0) {
                return true;
            }
#endif
        }
        if(end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    return false;
}

inline std::filesystem::path icon_or_fallback(std::filesystem::path icon_path)
{
    std::error_code ec;
    if(std::filesystem::exists(icon_path, ec) && !ec) {
        return icon_path;
    }

    auto icon_dir = icon_path.parent_path();
    constexpr std::array fallback_names{
        "icon_category_settings.png",
        "icon_settings_personalization.png",
        "icon_category_application.png",
    };
    for(const auto* fallback_name : fallback_names) {
        auto fallback = icon_dir / fallback_name;
        ec.clear();
        if(std::filesystem::exists(fallback, ec) && !ec) {
            return fallback;
        }
    }

    return icon_path;
}

inline bool launch_detached(const std::vector<std::string>& args)
{
    if(args.empty() || args.front().empty()) {
        return false;
    }

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for(const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

#if defined(_WIN32)
    auto pid = _spawnvp(_P_NOWAIT, args.front().c_str(), argv.data());
    return pid != -1;
#else
    pid_t pid{};
    int rc = posix_spawnp(&pid, args.front().c_str(), nullptr, nullptr, argv.data(), environ);
    if(rc != 0) {
        return false;
    }

    std::thread([pid]() {
        int status{};
        while(waitpid(pid, &status, 0) == -1 && errno == EINTR) {
        }
    }).detach();
    return true;
#endif
}

inline std::vector<std::string> terminal_command(std::vector<std::string> command)
{
    if(command.empty()) {
        return {};
    }

#if defined(_WIN32)
    std::vector<std::string> args{"cmd", "/c", "start", ""};
    args.insert(args.end(), command.begin(), command.end());
    return args;
#elif defined(__APPLE__)
    std::string script = "tell application \"Terminal\" to do script ";
    std::string shell_line;
    for(const auto& arg : command) {
        shell_line += "'";
        for(char c : arg) {
            if(c == '\'') {
                shell_line += "'\\''";
            } else {
                shell_line += c;
            }
        }
        shell_line += "' ";
    }
    std::string escaped_shell_line;
    escaped_shell_line.reserve(shell_line.size());
    for(char c : shell_line) {
        if(c == '"' || c == '\\') {
            escaped_shell_line.push_back('\\');
        }
        escaped_shell_line.push_back(c);
    }
    script += "\"" + escaped_shell_line + "\"";
    return {"osascript", "-e", script};
#else
    if(const char* terminal = std::getenv("TERMINAL"); terminal != nullptr && command_available(terminal)) {
        std::vector<std::string> args{terminal, "-e"};
        args.insert(args.end(), command.begin(), command.end());
        return args;
    }

    const std::vector<std::pair<std::string, std::string>> terminals{
        {"x-terminal-emulator", "-e"},
        {"kgx", "--"},
        {"gnome-terminal", "--"},
        {"konsole", "-e"},
        {"xfce4-terminal", "-x"},
        {"xterm", "-e"},
    };
    for(const auto& [terminal, flag] : terminals) {
        if(command_available(terminal)) {
            std::vector<std::string> args{terminal, flag};
            args.insert(args.end(), command.begin(), command.end());
            return args;
        }
    }
    return {};
#endif
}

template<typename Menu, typename... Args>
std::unique_ptr<Menu> make_simple(std::string name, std::filesystem::path icon_path,
    dreamrender::resource_loader& loader,
    Args&&... args)
{
    auto menu = std::make_unique<Menu>(std::move(name), dreamrender::texture(loader.getDevice(), loader.getAllocator()), std::forward<Args>(args)...);
    loader.loadTexture(&menu->get_icon(), icon_or_fallback(std::move(icon_path)));
    return menu;
}

template<typename Menu, typename... Args>
std::unique_ptr<simple<Menu>> make_simple_of(std::string name, std::filesystem::path icon_path,
    dreamrender::resource_loader& loader,
    Args&&... args)
{
    return make_simple<simple<Menu>>(std::move(name), std::move(icon_path), loader, std::forward<Args>(args)...);
}

}
