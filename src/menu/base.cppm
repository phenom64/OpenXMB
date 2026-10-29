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
#include <algorithm>
#include <concepts>
#include <ranges>
#include <stdexcept>
#include <functional>
#include <memory>
#include <string>
#include <vector>

export module openxmb.app:menu_base;
import auroreengine;
import openxmb.utils;

export namespace menu {

class menu_entry {
    public:
        virtual ~menu_entry() = default;

        virtual std::string_view get_name() const = 0;
        virtual std::string_view get_description() const = 0;
        virtual const auroreengine::texture& get_icon() const = 0;
        virtual const auroreengine::texture* get_glass_icon() const {
            return nullptr;
        }
        virtual bool preserve_icon_colour() const { return false; }
        virtual result activate(action action) {
            return result::unsupported;
        }
};

class menu : public menu_entry {
    public:
        virtual void prepare_icons() {}
        virtual unsigned int get_submenus_count() const {
            return 0;
        }
        virtual unsigned int get_selected_submenu() const {
            return 0;
        }
        virtual void select_submenu(unsigned int index) {
        }
        virtual menu_entry& get_submenu(unsigned int index) const {
            throw std::out_of_range("Index out of range");
        }
        virtual void on_open() {
        }
        virtual void on_close() {
        }
        virtual void get_button_actions(std::vector<std::pair<action, std::string>>& v) {

        }
};

// Adds contextual rows without changing the wrapped menu's scan or lifecycle.
class prefixed_menu final : public menu {
    std::unique_ptr<menu> body;
    std::vector<std::unique_ptr<menu_entry>> prefix;
    unsigned int selected{};
    bool opened{};
public:
    prefixed_menu(std::unique_ptr<menu> value, std::vector<std::unique_ptr<menu_entry>> rows)
        : body(std::move(value)), prefix(std::move(rows)) {}
    std::string_view get_name() const override { return body->get_name(); }
    std::string_view get_description() const override { return body->get_description(); }
    const auroreengine::texture& get_icon() const override { return body->get_icon(); }
    const auroreengine::texture* get_glass_icon() const override { return body->get_glass_icon(); }
    unsigned int get_submenus_count() const override { return static_cast<unsigned int>(prefix.size())+body->get_submenus_count(); }
    unsigned int get_selected_submenu() const override { const auto count=get_submenus_count(); return count?std::min(selected,count-1):0; }
    void select_submenu(unsigned int index) override { selected=index; if(index>=prefix.size()) body->select_submenu(index-static_cast<unsigned int>(prefix.size())); }
    menu_entry& get_submenu(unsigned int index) const override { return index<prefix.size()?*prefix.at(index):body->get_submenu(index-static_cast<unsigned int>(prefix.size())); }
    void on_open() override { opened=true; body->on_open(); }
    void on_close() override { opened=false; body->on_close(); }
    void prepare_icons() override { body->prepare_icons(); }
    void get_button_actions(std::vector<std::pair<action,std::string>>& actions) override { if(selected>=prefix.size()) body->get_button_actions(actions); }
    result activate(action input) override {
        if(!opened) return result::submenu;
        if(selected<prefix.size()) return prefix.at(selected)->activate(input);
        return body->activate(input);
    }
};

template<typename T>
class simple : public T {
    public:
        using icon_type = auroreengine::texture;

        simple(std::string name, icon_type&& icon, std::string description = "") :
            name(std::move(name)), icon(std::move(icon)), description(std::move(description)) {}
        ~simple() override = default;

        std::string_view get_name() const override {
            return name;
        }
        std::string_view get_description() const override {
            return description;
        }
        const auroreengine::texture& get_icon() const override {
            return icon;
        }
        const auroreengine::texture* get_glass_icon() const override {
            return glass_icon.get();
        }
        auroreengine::texture& get_icon() {
            return icon;
        }
        auroreengine::texture& emplace_glass_icon(auroreengine::texture&& texture) {
            glass_icon = std::make_unique<auroreengine::texture>(std::move(texture));
            return *glass_icon;
        }
    private:
        std::string name;
        std::string description;
        icon_type icon;
        std::unique_ptr<auroreengine::texture> glass_icon;
};

template<typename T>
class simple_shared : public T {
    public:
        using icon_type = std::shared_ptr<auroreengine::texture>;

        simple_shared(std::string name, icon_type&& icon, std::string description = "") :
            name(std::move(name)), icon(std::move(icon)), description(std::move(description)) {}
        ~simple_shared() override = default;

        std::string_view get_name() const override {
            return name;
        }
        std::string_view get_description() const override {
            return description;
        }
        const auroreengine::texture& get_icon() const override {
            return *icon;
        }
        const auroreengine::texture* get_glass_icon() const override {
            return glass_icon.get();
        }
        auroreengine::texture& get_icon() {
            return *icon;
        }
        auroreengine::texture& emplace_glass_icon(auroreengine::texture&& texture) {
            glass_icon = std::make_unique<auroreengine::texture>(std::move(texture));
            return *glass_icon;
        }
    private:
        std::string name;
        std::string description;
        icon_type icon;
        std::unique_ptr<auroreengine::texture> glass_icon;
};

using simple_menu_shallow = simple<menu>;
using simple_menu_shallow_shared = simple_shared<menu>;
using simple_menu_entry = simple<menu_entry>;
using simple_menu_entry_shared = simple_shared<menu_entry>;

template<typename Base>
class action_menu_entry_generic : public Base {
    public:
        action_menu_entry_generic(
            std::string name, Base::icon_type&& icon,
            std::function<result()> on_activate, std::function<result(action)> on_action = {},
            std::string description = "", bool preserve_colour = false
        ) :
            Base(std::move(name), std::move(icon), std::move(description)), on_activate(std::move(on_activate)), on_action(std::move(on_action)), preserve_colour(preserve_colour) {}
        ~action_menu_entry_generic() override = default;
        bool preserve_icon_colour() const override { return preserve_colour; }

        result activate(action action) override {
            if(on_action) {
                return on_action(action);
            } else if(action != action::ok) {
                return result::unsupported;
            }
            return on_activate ? on_activate() : result::success;
        }
    private:
        std::function<result()> on_activate;
        std::function<result(action)> on_action;
        bool preserve_colour{};
};
using action_menu_entry = action_menu_entry_generic<simple_menu_entry>;
using action_menu_entry_shared = action_menu_entry_generic<simple_menu_entry_shared>;

template<typename Base>
class simple_menu_generic : public Base {
    public:
        simple_menu_generic(std::string name, Base::icon_type&& icon, std::string description = "") :
            Base(std::move(name), std::move(icon), std::move(description)) {}

        template<std::derived_from<menu_entry> T, std::size_t N>
        simple_menu_generic(const std::string& name, Base::icon_type&& icon, std::array<std::unique_ptr<T>, N>&& entries, std::string description = "") :
            Base(name, std::move(icon), std::move(description))
        {
            for(auto& entry : std::move(entries)) {
                this->entries.push_back(std::move(entry));
            }
        }
        simple_menu_generic(const std::string& name, Base::icon_type&& icon, std::ranges::range auto&& entries, std::string description = "") :
            Base(name, std::move(icon), std::move(description))
        {
            for(auto& entry : std::move(entries)) {
                this->entries.push_back(std::move(entry));
            }
        }
        ~simple_menu_generic() override = default;

        unsigned int get_submenus_count() const override {
            return entries.size();
        }
        menu_entry& get_submenu(unsigned int index) const override {
            return *entries.at(index);
        }
        unsigned int get_selected_submenu() const override {
            return selected_submenu;
        }
        void select_submenu(unsigned int index) override {
            selected_submenu = index;
        }
        result activate(action action) override {
            if(!is_open) {
                return result::submenu;
            }
            if(selected_submenu < entries.size()) {
                return entries.at(selected_submenu)->activate(action);
            }
            return result::unsupported;
        }
        void on_open() override {
            is_open = true;
        }
        void on_close() override {
            is_open = false;
        }
    protected:
        bool is_open = false;
        std::vector<std::unique_ptr<menu_entry>> entries;
        unsigned int selected_submenu = 0;
};
using simple_menu = simple_menu_generic<simple_menu_shallow>;
using simple_menu_shared = simple_menu_generic<simple_menu_shallow_shared>;

}
