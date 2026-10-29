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
    // Separate press/release thresholds reject analog jitter around a hold.
    std::optional<bool> trigger(unsigned index,int value) {
        if(index>=triggers_.size())return {};
        auto& held=triggers_[index];
        if(!held&&value>=16384){held=true;return true;}
        if(held&&value<=8192){held=false;return false;}
        return {};
    }
    void clear() {held_.fill(false);triggers_.fill(false);repeating_.reset();}
private:
    std::array<bool,23> held_{},seen_{};
    std::array<double,23> last_{};
    std::array<bool,2> triggers_{};
    std::optional<unsigned> repeating_;
    double next_{};
};
}
