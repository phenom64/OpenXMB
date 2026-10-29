#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace app::dialog_input::completion {
// A bounded vocabulary of actual local text, not a language prediction service.
// User buffers are never written to disk or shared with another editor.
inline constexpr std::size_t max_words=8192,max_word_bytes=96,max_source_bytes=2*1024*1024;
inline char lower(char c) {return c>='A'&&c<='Z'?static_cast<char>(c+'a'-'A'):c;}
inline std::string folded(std::string_view word) {
    std::string result(word);for(auto& c:result)c=lower(c);return result;
}
inline bool folded_less(std::string_view a,std::string_view b) {
    return std::lexicographical_compare(a.begin(),a.end(),b.begin(),b.end(),[](char x,char y){
        return static_cast<unsigned char>(lower(x))<static_cast<unsigned char>(lower(y));
    });
}
inline bool prefix_matches(std::string_view word,std::string_view prefix) {
    return word.size()>=prefix.size()&&std::equal(prefix.begin(),prefix.end(),word.begin(),[](char a,char b){return lower(a)==lower(b);});
}
inline std::size_t decode(std::string_view text,std::size_t at,std::uint32_t& cp) {
    if(at>=text.size())return at;
    const auto lead=static_cast<unsigned char>(text[at]);
    const unsigned bytes=lead<0x80?1:lead>=0xc2&&lead<=0xdf?2:lead>=0xe0&&lead<=0xef?3:lead>=0xf0&&lead<=0xf4?4:0;
    if(!bytes||at+bytes>text.size())return at;
    cp=lead&static_cast<unsigned char>(bytes==1?0x7f:(1u<<(7-bytes))-1);
    for(unsigned i=1;i<bytes;++i) {
        const auto c=static_cast<unsigned char>(text[at+i]);
        if((c&0xc0)!=0x80)return at;
        cp=(cp<<6)|(c&0x3f);
    }
    if((bytes==2&&cp<0x80)||(bytes==3&&cp<0x800)||(bytes==4&&cp<0x10000)||
       cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff))return at;
    return at+bytes;
}
inline bool letter(std::uint32_t cp) {
    return (cp>='a'&&cp<='z')||(cp>='A'&&cp<='Z')||
        (cp>=0xc0&&cp<=0x2af&&cp!=0xd7&&cp!=0xf7)||
        (cp>=0x300&&cp<=0x52f)||(cp>=0x590&&cp<=0x8ff)||
        (cp>=0x900&&cp<=0x1fff)||(cp>=0x3040&&cp<=0x30ff)||
        (cp>=0x3400&&cp<=0x9fff)||(cp>=0xac00&&cp<=0xd7af)||
        (cp>=0xf900&&cp<=0xfaff)||(cp>=0x20000&&cp<=0x3134f);
}
inline bool space(char c) {return c==' '||c=='\t'||c=='\r'||c=='\n';}
inline bool safe_segment(std::string_view text) {
    return text.find_first_of("/\\@{}%_=<>0123456789")==text.npos;
}
inline void collect(std::string_view source,std::vector<std::string>& words,std::unordered_set<std::string>& seen) {
    for(std::size_t at=0;at<source.size()&&words.size()<max_words;) {
        while(at<source.size()&&space(source[at]))++at;
        const auto begin=at;while(at<source.size()&&!space(source[at]))++at;
        const auto segment=source.substr(begin,at-begin);
        if(!safe_segment(segment))continue;
        std::size_t start=0,position=0;
        const auto add=[&](std::size_t end) {
            const auto word=segment.substr(start,end-start);
            if(word.size()>=2&&word.size()<=max_word_bytes&&words.size()<max_words&&seen.insert(folded(word)).second)
                words.emplace_back(word);
        };
        while(position<segment.size()) {
            std::uint32_t cp{};const auto next=decode(segment,position,cp);
            if(next==position) {start=segment.size();break;}
            if(!letter(cp)) {add(position);start=next;}
            position=next;
        }
        if(start<segment.size())add(segment.size());
    }
}
inline std::vector<std::string> vocabulary(const std::vector<std::string>& sources) {
    std::vector<std::string> result;std::unordered_set<std::string> seen;std::size_t bytes=0;
    for(const auto& source:sources) {
        if(bytes+source.size()>max_source_bytes||result.size()>=max_words)break;
        bytes+=source.size();collect(source,result,seen);
    }
    std::sort(result.begin(),result.end(),folded_less);
    return result;
}
struct Token {std::size_t begin,end;std::string prefix;};
inline std::optional<Token> token(std::string_view value,std::size_t caret) {
    if(!caret||caret>value.size()||(caret<value.size()&&(static_cast<unsigned char>(value[caret])&0xc0)==0x80))return {};
    auto first=caret;while(first&&!space(value[first-1]))--first;
    auto last=caret;while(last<value.size()&&!space(value[last]))++last;
    if(!safe_segment(value.substr(first,last-first)))return {};
    auto start=first,end=caret;
    for(auto position=first;position<caret;) {
        std::uint32_t cp{};const auto next=decode(value,position,cp);
        if(next==position||next>caret)return {};
        if(!letter(cp))start=next;
        position=next;
    }
    if(start==caret||caret-start>max_word_bytes)return {};
    while(end<last) {
        std::uint32_t cp{};const auto next=decode(value,end,cp);
        if(next==end||!letter(cp))break;
        end=next;
    }
    return Token{start,end,std::string(value.substr(start,caret-start))};
}
inline std::vector<std::string> candidates(const std::vector<std::string>& vocabulary,std::string_view value,std::size_t caret) {
    const auto current=token(value,caret);if(!current)return {};
    const std::string_view prefix=current->prefix;
    std::vector<std::string> result;result.reserve(6);
    // Vocabulary is immutable and sorted once at opening. A keystroke only
    // visits its prefix range, copying at most the six visible candidates.
    auto word=std::lower_bound(vocabulary.begin(),vocabulary.end(),prefix,folded_less);
    for(;word!=vocabulary.end()&&prefix_matches(*word,prefix)&&result.size()<6;++word)
        if(word->size()>prefix.size())result.push_back(*word);
    std::vector<std::string> buffer_words;std::unordered_set<std::string> seen;
    collect(value.substr(0,4096),buffer_words,seen);
    for(const auto& candidate:buffer_words)if(candidate.size()>prefix.size()&&prefix_matches(candidate,prefix)) {
        const auto position=std::lower_bound(result.begin(),result.end(),candidate,folded_less);
        if(position!=result.end()&&!folded_less(candidate,*position))continue;
        if(result.size()==6&&position==result.end())continue;
        result.insert(position,candidate);if(result.size()>6)result.pop_back();
    }
    return result;
}
inline std::string match_case(std::string word,std::string_view prefix) {
    const bool uppercase=std::all_of(prefix.begin(),prefix.end(),[](char c){return c<'a'||c>'z';})&&
                         std::any_of(prefix.begin(),prefix.end(),[](char c){return c>='A'&&c<='Z';});
    for(auto& c:word)c=lower(c);
    if(uppercase)for(auto& c:word) {if(c>='a'&&c<='z')c=static_cast<char>(c+'A'-'a');}
    else if(!prefix.empty()&&prefix[0]>='A'&&prefix[0]<='Z'&&!word.empty()&&word[0]>='a'&&word[0]<='z')word[0]=static_cast<char>(word[0]+'A'-'a');
    return word;
}
}
