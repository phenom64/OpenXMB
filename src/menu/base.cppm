module;

#include <string>
#include <stdexcept>
#include <functional>
#include <memory>

export module shell.app:menu_base;
import dreamrender;
import shell.utils;

export namespace menu {

class menu_entry {
    public:
        virtual ~menu_entry() = default;

        virtual std::string_view get_name() const = 0;
        virtual std::string_view get_description() const = 0;
        virtual const dreamrender::texture& get_icon() const = 0;
        virtual result activate(action action) {
            return result::unsupported;
        }
};

class menu : public menu_entry {
    public:
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

template<typename T>
class simple : public T {
    public:
        using icon_type = dreamrender::texture;

        simple(std::string name, icon_type&& icon, std::string description = "") :
            name(std::move(name)), icon(std::move(icon)), description(std::move(description)) {}
        ~simple() override = default;

        std::string_view get_name() const override {
            return name;
        }
        std::string_view get_description() const override {
            return description;
        }
        const dreamrender::texture& get_icon() const override {
            return icon;
        }
        dreamrender::texture& get_icon() {
            return icon;
        }
    private:
        std::string name;
        std::string description;
        icon_type icon;
};

template<typename T>
class simple_shared : public T {
    public:
        using icon_type = std::shared_ptr<dreamrender::texture>;

        simple_shared(std::string name, icon_type&& icon, std::string description = "") :
            name(std::move(name)), icon(std::move(icon)), description(std::move(description)) {}
        ~simple_shared() override = default;

        std::string_view get_name() const override {
            return name;
        }
        std::string_view get_description() const override {
            return description;
        }
        const dreamrender::texture& get_icon() const override {
            return *icon;
        }
        dreamrender::texture& get_icon() {
            return *icon;
        }
    private:
        std::string name;
        std::string description;
        icon_type icon;
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
            std::string description = ""
        ) :
            Base(std::move(name), std::move(icon), std::move(description)), on_activate(std::move(on_activate)), on_action(std::move(on_action)) {}
        ~action_menu_entry_generic() override = default;

        result activate(action action) override {
            if(on_action) {
                return on_action(action);
            } else if(action != action::ok) {
                return result::unsupported;
            }
            return on_activate();
        }
    private:
        std::function<result()> on_activate;
        std::function<result(action)> on_action;
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