#pragma once
#include <array>
#include <optional>

namespace app {
class ButtonTiming {
public:
    bool press(unsigned button,bool repeatable,double seconds) {
        if(button>=held_.size()||held_[button])return false;
        held_[button]=true;
        if(!repeatable&&seen_[button]&&seconds-last_[button]<0.280)return false;
        seen_[button]=true;last_[button]=seconds;
        if(repeatable){repeating_=button;next_=seconds+0.450;}
        return true;
    }
    void release(unsigned button) {
        if(button<held_.size())held_[button]=false;
        if(repeating_&&*repeating_==button)repeating_.reset();
    }
    std::optional<unsigned> repeat(double seconds,double interval) {
        if(!repeating_||seconds<next_)return {};
        next_=seconds+interval;return repeating_;
    }
    void clear() {held_.fill(false);repeating_.reset();}
private:
    std::array<bool,21> held_{},seen_{};
    std::array<double,21> last_{};
    std::optional<unsigned> repeating_;
    double next_{};
};
}
