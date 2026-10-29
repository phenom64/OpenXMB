#pragma once
#ifdef __linux__
#include <array>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace openxmb::integrations::linux_detail {
// /proc starttime is in kernel ticks since boot. It is an identity token,
// never converted into wall time. Only live processes belonging to this user.
inline std::optional<std::uint64_t> process_creation(std::int64_t pid) {
  if(pid<=0||pid>std::numeric_limits<pid_t>::max())return {};
  const auto directory="/proc/"+std::to_string(pid);
  struct stat info{};if(::stat(directory.c_str(),&info)||info.st_uid!=geteuid())return {};
  std::ifstream input(directory+"/stat",std::ios::binary);
  std::array<char,8193> buffer{};input.read(buffer.data(),buffer.size());
  const auto count=input.gcount();if(input.bad()||count<=0||count>=static_cast<std::streamsize>(buffer.size()))return {};
  std::string_view text(buffer.data(),static_cast<std::size_t>(count));
  const auto end=text.rfind(')');if(end==text.npos||end+2>=text.size())return {};
  text.remove_prefix(end+2);
  for(int field=3;field<=22;++field) {
    const auto space=text.find(' ');const auto token=text.substr(0,space);
    if(field==3&&(token=="Z"||token=="X"||token=="x"))return {};
    if(field==22){std::uint64_t value{};const auto parsed=std::from_chars(token.data(),token.data()+token.size(),value);return parsed.ec==std::errc{}&&parsed.ptr==token.data()+token.size()&&value?std::optional{value}:std::nullopt;}
    if(space==text.npos)return {};text.remove_prefix(space+1);
  }
  return {};
}
}
#endif
