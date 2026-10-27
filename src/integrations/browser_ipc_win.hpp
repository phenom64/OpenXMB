#pragma once
#include <windows.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <string>

namespace openxmb::integrations::detail {
// CRT streams do not share delete access on Windows. A concurrent atomic
// replacement must be allowed while the reader still has the old file open.
inline nlohmann::json read_browser_json(const std::filesystem::path& path,DWORD limit=262144) {
  const auto file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
    nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(file==INVALID_HANDLE_VALUE) return nullptr;
  LARGE_INTEGER size{};
  if(!GetFileSizeEx(file,&size)||size.QuadPart<0||size.QuadPart>limit) {CloseHandle(file);return nullptr;}
  std::string bytes(static_cast<std::size_t>(size.QuadPart),'\0');DWORD read{};
  const bool success=ReadFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)!=FALSE;
  CloseHandle(file);
  if(!success||read!=bytes.size()) return nullptr;
  return nlohmann::json::parse(bytes,nullptr,false);
}
inline bool replace_browser_file(const std::filesystem::path& source,const std::filesystem::path& destination) {
  for(int attempt=0;attempt<8;++attempt) {
    if(MoveFileExW(source.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) return true;
    const auto error=GetLastError();
    if(error!=ERROR_SHARING_VIOLATION&&error!=ERROR_ACCESS_DENIED) return false;
    Sleep(2);
    SetLastError(error);
  }
  return false;
}
}
