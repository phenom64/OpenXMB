#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace app::dialog_text {
inline std::size_t next_boundary(std::string_view value,std::size_t at) {
    if(at>=value.size())return value.size();
    const auto ch=static_cast<unsigned char>(value[at]);
    const std::size_t length=ch<0x80?1:ch>=0xc2&&ch<=0xdf?2:ch>=0xe0&&ch<=0xef?3:ch>=0xf0&&ch<=0xf4?4:1;
    if(at+length>value.size())return at+1;
    for(std::size_t i=1;i<length;++i)if((static_cast<unsigned char>(value[at+i])&0xc0)!=0x80)return at+1;
    return at+length;
}
template<class Measure> std::vector<std::string> wrap(std::string_view value,float width,Measure measure) {
    std::vector<std::string> lines;
    std::size_t paragraph=0;
    do {
        const auto end=value.find_first_of("\r\n",paragraph);
        const auto part=value.substr(paragraph,end==value.npos?value.size()-paragraph:end-paragraph);
        if(part.empty())lines.emplace_back();
        for(std::size_t start=0;start<part.size();) {
            std::size_t fit=start,word=start;
            for(auto next=next_boundary(part,start);next>fit;next=next_boundary(part,next)) {
                if(measure(part.substr(start,next-start))>width&&fit>start)break;
                fit=next;
                if(part[next-1]==' '||part[next-1]=='\t')word=next;
                if(next==part.size()||measure(part.substr(start,next-start))>width)break;
            }
            if(fit<part.size()&&word>start)fit=word;
            lines.emplace_back(part.substr(start,fit-start));
            start=fit;
        }
        if(end==value.npos)break;
        paragraph=end+1;
        if(value[end]=='\r'&&paragraph<value.size()&&value[paragraph]=='\n')++paragraph;
    } while(paragraph<=value.size());
    return lines;
}
template<class Measure> std::string tail(std::string_view value,float width,Measure measure) {
    if(measure(value)<=width)return std::string(value);
    constexpr std::string_view dots="\xE2\x80\xA6";
    if(measure(dots)>width)return {};
    for(std::size_t at=0;at<value.size();at=next_boundary(value,at)) {
        auto result=std::string(dots)+std::string(value.substr(at));
        if(measure(result)<=width)return result;
    }
    return std::string(dots);
}
template<class Measure> std::string ellipsize(std::string_view value,float width,Measure measure) {
    if(measure(value)<=width)return std::string(value);
    std::string result;constexpr std::string_view dots="\xE2\x80\xA6";
    if(measure(dots)>width)return result;
    for(std::size_t at=0;at<value.size();) {
        const auto next=next_boundary(value,at);
        if(measure(std::string(value.substr(0,next))+std::string(dots))>width)break;
        result=value.substr(0,next);at=next;
    }
    return result+std::string(dots);
}
struct Metrics {float ascent,descent;};
inline std::size_t line_capacity(float height,float pitch,float line_height) {
    return static_cast<std::size_t>(std::max(1.0F,1+std::floor((height-line_height)/std::max(1.0F,pitch))));
}
struct MessageLayout {
    std::size_t capacity;float title_last_baseline,body_top,body_first_baseline,body_pitch,body_bottom,choice_first_baseline;
};
inline MessageLayout message_layout(std::size_t,std::size_t body_count,std::size_t choices,Metrics title,Metrics body,Metrics choice,float scale) {
    MessageLayout page{};page.title_last_baseline=382;page.choice_first_baseline=650;
    page.body_top=382+title.descent+24*scale;
    page.body_bottom=choices?650-choice.ascent-20*scale:810;
    page.body_pitch=body.ascent+body.descent+8*scale;
    page.capacity=std::min(std::max(std::size_t{1},body_count),line_capacity(page.body_bottom-page.body_top,page.body_pitch,body.ascent+body.descent));
    const float height=body.ascent+body.descent+(page.capacity-1)*page.body_pitch;
    page.body_first_baseline=page.body_top+(page.body_bottom-page.body_top-height)*0.5F+body.ascent;
    return page;
}
struct Window {std::size_t first,last;};
inline Window choice_window(const std::vector<float>& heights,std::size_t selected,float height,std::size_t first) {
    if(heights.empty())return {0,0};
    selected=std::min(selected,heights.size()-1);first=std::min(first,selected);
    float used=0;for(auto i=first;i<=selected;++i)used+=heights[i];
    while(first<selected&&used>height)used-=heights[first++];
    auto last=first;used=0;
    while(last<heights.size()&&(used+heights[last]<=height||last==first))used+=heights[last++];
    return {first,last};
}
}
