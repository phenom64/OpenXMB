#include "openxmb/setup/model.hpp"
#include "../localization/language_options.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <format>
#include <stdexcept>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace openxmb::setup {
namespace {
using namespace std::chrono;
int wrap(int value, int minimum, int maximum) {
    const int count=maximum-minimum+1;
    return minimum+((value-minimum)%count+count)%count;
}
int month_days(int year, int month) {
    return static_cast<unsigned>((year_month_day_last{std::chrono::year{year},month_day_last{std::chrono::month{static_cast<unsigned>(month)}}}).day());
}
CivilTime civil(sys_seconds value) {
    const auto date=floor<days>(value);
    const year_month_day ymd{date};
    const hh_mm_ss clock{value-date};
    return {int(ymd.year()),int(unsigned(ymd.month())),int(unsigned(ymd.day())),int(clock.hours().count()),int(clock.minutes().count()),int(clock.seconds().count())};
}
sys_seconds civil_seconds(CivilTime value) {
    return sys_days{year{value.year}/month{static_cast<unsigned>(value.month)}/day{static_cast<unsigned>(value.day)}}+
        hours{value.hour}+minutes{value.minute}+seconds{value.second};
}
std::string host_locale() {
#if defined(_WIN32)
    std::array<wchar_t,LOCALE_NAME_MAX_LENGTH> name{};
    if(GetUserDefaultLocaleName(name.data(),static_cast<int>(name.size()))) {
        std::string result;
        for(const auto character:name) {if(character==0)break; result.push_back(static_cast<char>(character));}
        return result;
    }
#endif
    for(const auto* variable:{"LC_ALL","LC_MESSAGES","LANG"})
        if(const auto* value=std::getenv(variable);value&&*value)return value;
    return "en_GB";
}
std::string cleaned_name(std::string name) {
    std::erase_if(name,[](unsigned char character){return character<32||character==127;});
    const auto first=name.find_first_not_of(" \t\r\n");
    if(first==std::string::npos)return {};
    name=name.substr(first,name.find_last_not_of(" \t\r\n")-first+1);
    // Bound by complete UTF-8 code points, without cutting a multibyte character.
    if(name.size()>128) {std::size_t end=128;while(end>0&&(static_cast<unsigned char>(name[end])&0xc0)==0x80)--end;name.resize(end);}
    return name;
}
int offset_minutes(const Zone& zone, Instant now, bool automatic_daylight) {
#if defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
    if(automatic_daylight&&zone.rules_available) {
        try {return static_cast<int>(duration_cast<minutes>(locate_zone(zone.id)->get_info(now).offset).count());}
        catch(const std::exception&) {}
    }
#else
    static_cast<void>(now);static_cast<void>(automatic_daylight);
#endif
    return zone.standard_minutes;
}
}

bool requires_setup(bool saved_profile,std::optional<bool> completed) {
    return completed?!*completed:!saved_profile;
}
bool valid(CivilTime value) {
    return value.year>=1900&&value.year<=2100&&value.month>=1&&value.month<=12&&value.day>=1&&
        value.day<=month_days(value.year,value.month)&&value.hour>=0&&value.hour<24&&
        value.minute>=0&&value.minute<60&&value.second>=0&&value.second<60;
}
CivilTime adjust_segment(CivilTime value,int segment,int delta) {
    switch(segment) {
    case 0:value.day=wrap(value.day+delta,1,month_days(value.year,value.month));break;
    case 1:value.month=wrap(value.month+delta,1,12);break;
    case 2:value.year=wrap(value.year+delta,1900,2100);break;
    case 3:value.hour=wrap(value.hour+delta,0,23);break;
    case 4:value.minute=wrap(value.minute+delta,0,59);break;
    default:break;
    }
    value.day=std::min(value.day,month_days(value.year,value.month));
    return value;
}
std::string Zone::label() const {
    const auto magnitude=std::abs(standard_minutes);
    return std::format("GMT{}{:02}:{:02} {}",standard_minutes<0?'-':'+',magnitude/60,magnitude%60,cities);
}
std::vector<Language> supported_languages() {
    std::vector<Language> result;
    for(const auto& [code,label]:localization::shell_locale::languages) {
        if(code=="auto")continue;
        result.push_back({std::string(code),std::string(label)});
    }
    return result;
}
std::vector<Zone> time_zones() {
    std::vector<Zone> result{
        {"Pacific/Honolulu","Hawaii",-600}, {"America/Anchorage","Alaska",-540},
        {"America/Los_Angeles","Pacific Time (US & Canada)",-480}, {"America/Denver","Mountain Time (US & Canada)",-420},
        {"America/Phoenix","Arizona",-420}, {"America/Chicago","Central Time (US & Canada)",-360},
        {"America/New_York","Eastern Time (US & Canada)",-300}, {"America/Halifax","Atlantic Time (Canada)",-240},
        {"America/St_Johns","Newfoundland",-210}, {"America/Sao_Paulo","Brasilia",-180},
        {"Atlantic/Azores","Azores",-60}, {"Etc/UTC","Coordinated Universal Time",0},
        {"Europe/London","Dublin, Edinburgh, Lisbon, London",0},
        {"Europe/Amsterdam","Amsterdam",60}, {"Europe/Berlin","Berlin",60}, {"Europe/Paris","Paris",60},
        {"Europe/Rome","Rome",60}, {"Europe/Zurich","Zurich",60},
        {"Europe/Athens","Athens, Bucharest, Helsinki",120}, {"Africa/Johannesburg","Pretoria",120},
        {"Europe/Moscow","Moscow, St. Petersburg",180}, {"Asia/Dubai","Abu Dhabi, Muscat",240},
        {"Asia/Kabul","Kabul",270}, {"Asia/Karachi","Islamabad, Karachi",300},
        {"Asia/Kolkata","Bombay, Calcutta, Madras, New Delhi",330}, {"Asia/Kathmandu","Kathmandu",345},
        {"Asia/Dhaka","Dhaka",360}, {"Asia/Yangon","Rangoon",390}, {"Asia/Bangkok","Bangkok, Hanoi, Jakarta",420},
        {"Asia/Shanghai","Beijing, Chongqing, Hong Kong, Urumqi",480}, {"Asia/Singapore","Singapore",480},
        {"Asia/Tokyo","Osaka, Sapporo, Tokyo",540}, {"Asia/Seoul","Seoul",540},
        {"Australia/Adelaide","Adelaide",570}, {"Australia/Darwin","Darwin",570},
        {"Australia/Sydney","Canberra, Melbourne, Sydney",600}, {"Australia/Brisbane","Brisbane",600},
        {"Pacific/Noumea","New Caledonia",660}, {"Pacific/Auckland","Auckland, Wellington",720}
    };
#if defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
    const auto year_value=year_month_day{floor<days>(system_clock::now())}.year();
    for(auto& zone:result) {
        try {
            const auto* rules=locate_zone(zone.id);
            const auto january=rules->get_info(sys_days{year_value/January/15});
            const auto july=rules->get_info(sys_days{year_value/July/15});
            zone.standard_minutes=static_cast<int>(duration_cast<minutes>(std::min(january.offset,july.offset)).count());
            zone.observes_daylight=january.offset!=july.offset;
            zone.rules_available=true;
        } catch(const std::exception&) {}
    }
#endif
    return result;
}
Draft detect_defaults(const std::vector<Language>& languages,const std::vector<Zone>& zones,std::string detected_locale) {
    if(languages.empty()||zones.empty())throw std::invalid_argument("Setup requires languages and time zones");
    Draft result;
    auto locale=detected_locale.empty()?host_locale():std::move(detected_locale);
    std::replace(locale.begin(),locale.end(),'-','_');
    const auto suffix=locale.find_first_of(".@:");
    const auto locale_tag=locale.substr(0,suffix);
    const auto region_start=locale_tag.find_last_of('_');
    if(region_start!=std::string::npos) {
        auto region=locale_tag.substr(region_start+1);
        if(region.size()==2||region.size()==3) {
            std::transform(region.begin(),region.end(),region.begin(),[](unsigned char c){return static_cast<char>(std::toupper(c));});
            result.region=std::move(region);
        }
    }
    const auto code=localization::shell_locale::normalize(locale);
    auto language=std::find_if(languages.begin(),languages.end(),[&](const auto& item){return item.code==code;});
    if(language==languages.end())language=languages.begin();
    result.language_index=static_cast<std::size_t>(language-languages.begin());result.language=language->code;
    std::string zone_id;
#if defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
    try {zone_id=current_zone()->name();}catch(const std::exception&) {}
#endif
    auto zone=std::find_if(zones.begin(),zones.end(),[&](const auto& item){return item.id==zone_id;});
    if(zone==zones.end()) {
        const auto raw=system_clock::to_time_t(system_clock::now());std::tm local{},utc{};
#if defined(_WIN32)
        localtime_s(&local,&raw);gmtime_s(&utc,&raw);
#else
        localtime_r(&raw,&local);gmtime_r(&raw,&utc);
#endif
        const auto local_value=civil_seconds({local.tm_year+1900,local.tm_mon+1,local.tm_mday,local.tm_hour,local.tm_min,local.tm_sec});
        const auto utc_value=civil_seconds({utc.tm_year+1900,utc.tm_mon+1,utc.tm_mday,utc.tm_hour,utc.tm_min,utc.tm_sec});
        const auto offset=duration_cast<minutes>(local_value-utc_value).count();
        zone=std::find_if(zones.begin(),zones.end(),[&](const auto& item){return offset_minutes(item,system_clock::now(),true)==offset;});
    }
    if(zone==zones.end())zone=std::find_if(zones.begin(),zones.end(),[](const auto& item){return item.id=="Etc/UTC";});
    if(zone==zones.end())zone=zones.begin();
    result.zone_index=static_cast<std::size_t>(zone-zones.begin());
    result.automatic_daylight=zone->observes_daylight&&zone->rules_available;
    for(const auto* key:{"USERNAME","USER"})if(const auto* user=std::getenv(key);user&&*user){result.user_name=cleaned_name(user);break;}
    if(result.user_name.empty())result.user_name="User 1";
    return result;
}
CivilTime local_time(const Zone& zone,Instant now,bool automatic_daylight) {
    return civil(floor<seconds>(now)+minutes{offset_minutes(zone,now,automatic_daylight)});
}
std::optional<Instant> to_instant(const Zone& zone,CivilTime value,bool automatic_daylight) {
    if(!valid(value))return {};
    const auto seconds_value=civil_seconds(value);
#if defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
    if(automatic_daylight&&zone.rules_available) {
        try {
            const auto* rules=locate_zone(zone.id);
            const local_seconds wall{seconds_value.time_since_epoch()};
            if(rules->get_info(wall).result!=local_info::unique)return {};
            return rules->to_sys(wall);
        }catch(const std::exception&){return {};}
    }
#else
    static_cast<void>(automatic_daylight);
#endif
    return seconds_value-minutes{zone.standard_minutes};
}
std::string format_time(CivilTime value,bool seconds) {
    auto text=std::format("{:02}/{:02}/{:04}  {:02}:{:02}",value.day,value.month,value.year,value.hour,value.minute);
    if(seconds)text+=std::format(":{:02}",value.second);
    return text;
}

Model::Model():languages_(supported_languages()),zones_(time_zones()),draft_(detect_defaults(languages_,zones_)) {}
Model::Model(std::vector<Language> languages,std::vector<Zone> zones,Draft draft)
    :languages_(std::move(languages)),zones_(std::move(zones)),draft_(std::move(draft)) {
    if(languages_.empty()||zones_.empty()||draft_.language_index>=languages_.size()||draft_.zone_index>=zones_.size())
        throw std::invalid_argument("Invalid initial setup choices");
}
CivilTime Model::current_time(Instant now) const {
    if(editing_)return edited_;
    return local_time(zones_[draft_.zone_index],now+(draft_.automatic_time?std::chrono::seconds{}:draft_.clock_offset),draft_.automatic_daylight);
}
void Model::advance_intro() {if(page_==Page::brand)page_=Page::company;else if(page_==Page::company)page_=Page::language;}
void Model::show_page(Page page) {page_=page;editing_=false;confirmation_=false;error_.clear();}
void Model::set_name(std::string name) {name=cleaned_name(std::move(name));if(!name.empty())draft_.user_name=std::move(name);}
void Model::command(Command command,Instant now) {
    error_.clear();
    if(confirmation_) {
        if(command==Command::back)confirmation_=false;
        else if(command==Command::left||command==Command::right)confirmation_yes_=!confirmation_yes_;
        else if(command==Command::confirm){confirmation_=false;if(confirmation_yes_)page_=Page::complete;}
        return;
    }
    if(page_==Page::brand||page_==Page::company){if(command==Command::confirm||command==Command::right)advance_intro();return;}
    if(editing_) {
        if(command==Command::left)segment_=wrap(segment_-1,0,4);
        else if(command==Command::right)segment_=wrap(segment_+1,0,4);
        else if(command==Command::up||command==Command::down)edited_=adjust_segment(edited_,segment_,command==Command::up?1:-1);
        else if(command==Command::back)editing_=false;
        else if(command==Command::confirm) {
            if(const auto instant=to_instant(zones_[draft_.zone_index],edited_,draft_.automatic_daylight)) {
                draft_.clock_offset=duration_cast<seconds>(*instant-now);draft_.automatic_time=false;editing_=false;
            }else error_="This local time is skipped or repeated by daylight saving. Choose a different time or turn off automatic daylight saving.";
        }
        return;
    }
    if(page_==Page::language&&(command==Command::up||command==Command::down)) {
        draft_.language_index=static_cast<std::size_t>(wrap(static_cast<int>(draft_.language_index)+(command==Command::up?-1:1),0,static_cast<int>(languages_.size())-1));
        draft_.language=languages_[draft_.language_index].code;
    }else if(page_==Page::time_zone) {
        if(command==Command::extra)daylight_focused_=!daylight_focused_;
        else if(daylight_focused_&&command==Command::confirm) {
            if(zones_[draft_.zone_index].observes_daylight&&zones_[draft_.zone_index].rules_available)draft_.automatic_daylight=!draft_.automatic_daylight;
        }else if(!daylight_focused_&&(command==Command::up||command==Command::down)) {
            draft_.zone_index=static_cast<std::size_t>(wrap(static_cast<int>(draft_.zone_index)+(command==Command::up?-1:1),0,static_cast<int>(zones_.size())-1));
            const auto& zone=zones_[draft_.zone_index];draft_.automatic_daylight=zone.observes_daylight&&zone.rules_available;
        }
    }else if(page_==Page::date_time) {
        if(command==Command::confirm){edited_=current_time(now);editing_=true;segment_=0;return;}
        if(command==Command::extra){draft_.automatic_time=true;draft_.clock_offset=std::chrono::seconds{};}
    }else if(page_==Page::summary&&command==Command::confirm){confirmation_=true;confirmation_yes_=false;return;}
    if(command==Command::right&&page_>=Page::language&&page_<Page::summary) {
        page_=static_cast<Page>(static_cast<int>(page_)+1);daylight_focused_=false;
    }else if((command==Command::left||command==Command::back)&&page_>Page::language&&page_<=Page::summary) {
        page_=static_cast<Page>(static_cast<int>(page_)-1);daylight_focused_=false;
    }
}
}
