module;

#include <array>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

export module openxmb.app:text_entry;
import :component;
import openxmb.utils;
import dreamrender;
import glm;

export namespace app {

class text_entry_overlay : public component, public event_receiver {
public:
    float typography_scale() const override { return 2.5F; }

    text_entry_overlay(std::string title, std::string initial,
                       std::function<void(std::string)> accept)
        : title_(std::move(title)), value_(std::move(initial)), accept_(std::move(accept)) {}

    result on_event(const event& e) override {
        if (const auto* key = e.get<events::key_up>(); key && (key->keycode==225 || key->keycode==229)) { upper_=false; return result::success; }
        if (const auto* key = e.get<events::key_down>()) {
            const unsigned code = key->keycode;
            if (code == 40 || code == 88) return submit();
            if (code == 42) { erase(); return result::success; }
            if (code == 225 || code == 229) { upper_ = true; return result::success; }
            char ch = 0;
            if (code >= 4 && code <= 29) ch = static_cast<char>((upper_ ? 'A' : 'a') + code - 4);
            else if (code >= 30 && code <= 38) ch = upper_ ? std::string_view("!@#$%^&*(")[code-30] : static_cast<char>('1' + code - 30);
            else if (code == 39) ch = upper_ ? ')' : '0';
            else if (code == 44) ch = ' ';
            else if (code >= 45 && code <= 56) {
                constexpr std::array<char, 12> punctuation{'-', '=', '[', ']', '\\', '\\', ';', '\'', '`', ',', '.', '/'};
                constexpr std::string_view shifted = "_+{}||:\"~<>?";
                ch = upper_ ? shifted[code-45] : punctuation[code - 45];
            }
            if (ch) { if (value_.size() < 4096) value_ += ch; return result::success; }
        }
        switch (e.action) {
        case action::cancel: return result::close | result::back_sound;
        case action::up: row_ = (row_ + 5) % 6; break;
        case action::down: row_ = (row_ + 1) % 6; break;
        case action::left: col_ = (col_ + 9) % 10; break;
        case action::right: col_ = (col_ + 1) % 10; break;
        case action::extra: erase(); break;
        case action::options: return submit();
        case action::ok:
            if (row_ == 5) {
                if (col_ < 2) erase();
                else if (col_ < 5) { if (value_.size() < 4096) value_ += ' '; }
                else if (col_ < 7) upper_ = !upper_;
                else return submit();
            } else if (value_.size() < 4096) {
                char ch = rows_[row_][col_];
                if (upper_ && ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
                value_ += ch;
            }
            break;
        default: return result::success;
        }
        return result::success;
    }

    void render(dreamrender::gui_renderer& r, shell*) override {
        r.draw_rect({0.08F, 0.12F}, {0.84F, 0.76F}, {0.025F, 0.035F, 0.055F, 0.96F});
        r.draw_text(title_, 0.12F, 0.19F, 0.05F);
        const auto shown = value_.size() > 70 ? "..." + value_.substr(value_.size()-70) : value_;
        r.draw_text(shown + "_", 0.12F, 0.28F, 0.035F);
        for (int row = 0; row < 5; ++row) for (int col = 0; col < 10; ++col) {
            const float x = 0.14F + col * 0.073F, y = 0.38F + row * 0.065F;
            if (row == row_ && col == col_) r.draw_rect({x-0.008F, y-0.027F}, {0.047F, 0.055F}, {0.28F, 0.48F, 0.70F, 0.9F});
            char ch = rows_[row][col];
            if (upper_ && ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
            r.draw_text(std::string(1, ch), x, y, 0.04F);
        }
        const std::array<std::string_view, 4> labels{"Delete", "Space", "Shift", "Done"};
        for (int i = 0; i < 4; ++i) {
            const bool selected = row_ == 5 && (col_ < 2 ? 0 : col_ < 5 ? 1 : col_ < 7 ? 2 : 3) == i;
            r.draw_text(labels[i], 0.14F + i * 0.19F, 0.74F, 0.037F,
                        selected ? glm::vec4(0.5F, 0.8F, 1.0F, 1.0F) : glm::vec4(1.0F));
        }
        r.draw_text("Enter / Options: save    Cancel: back    Extra: delete", 0.14F, 0.82F, 0.026F);
    }
    bool is_opaque() const override { return false; }
private:
    void erase() { if (!value_.empty()) { auto start=value_.size()-1; while(start>0 && (static_cast<unsigned char>(value_[start]) & 0xc0)==0x80) --start; value_.resize(start); } }
    result submit() { accept_(value_); return result::close | result::confirm_sound; }
    static constexpr std::array<std::string_view, 5> rows_{"1234567890", "qwertyuiop", "asdfghjkl:", "zxcvbnm./-", "_@+?=\\()[]"};
    std::string title_, value_;
    std::function<void(std::string)> accept_;
    int row_ = 0, col_ = 0;
    bool upper_ = false;
};
}
