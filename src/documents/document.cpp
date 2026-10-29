/* This file is a part of the OpenXMB desktop experience project.
 * Copyright (C) 2025-2026 Syndromatic Ltd. All rights reserved
 * Designed by Kavish Krishnakumar in Manchester.
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

#include <openxmb/documents/document.hpp>
#include <openxmb/photos/photo_library.hpp>
#include <archive.h>
#include <archive_entry.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace openxmb::documents {
struct Workspace {
    std::filesystem::path directory;
    ~Workspace() { std::error_code ec; std::filesystem::remove_all(directory,ec); }
};
namespace {
constexpr std::uintmax_t max_document_bytes=512ULL*1024*1024;
constexpr std::uintmax_t max_page_bytes=128ULL*1024*1024;
constexpr std::size_t max_pages=5000,max_entries=20000;
constexpr std::uintmax_t max_uncompressed_archive_bytes=1024ULL*1024*1024;
std::atomic_uint64_t serial{};
void check_stop(std::stop_token stop) { if(stop.stop_requested()) throw std::runtime_error("Document operation cancelled"); }
std::filesystem::path executable(std::string_view name) {
    const auto* environment=std::getenv("PATH");
    if(!environment) return {};
    std::string paths(environment);
#if defined(_WIN32)
    constexpr char separator=';';
#else
    constexpr char separator=':';
#endif
    for(std::size_t start=0;start<=paths.size();) {
        const auto end=paths.find(separator,start);
        auto directory=paths.substr(start,end==std::string::npos?std::string::npos:end-start);
        auto candidate=std::filesystem::path(directory.empty()?".":directory)/std::string(name);
#if defined(_WIN32)
        if(!candidate.has_extension()) candidate+=".exe";
        if(std::filesystem::is_regular_file(candidate)) return std::filesystem::absolute(candidate);
#else
        if(access(candidate.c_str(),X_OK)==0 && std::filesystem::is_regular_file(candidate)) return std::filesystem::absolute(candidate);
#endif
        if(end==std::string::npos) break;
        start=end+1;
    }
    return {};
}
std::shared_ptr<Workspace> workspace() {
    auto result=std::make_shared<Workspace>();
    for(int n=0;n<100;++n) {
        auto candidate=std::filesystem::temp_directory_path()/("openxmb-document-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(serial.fetch_add(1)));
        std::error_code ec;
        if(std::filesystem::create_directory(candidate,ec)) {
            std::filesystem::permissions(candidate,std::filesystem::perms::owner_all,std::filesystem::perm_options::replace,ec);
            result->directory=std::move(candidate); return result;
        }
        if(ec && ec!=std::errc::file_exists) throw std::filesystem::filesystem_error("Cannot create document cache",candidate,ec);
    }
    throw std::runtime_error("Cannot create a private document cache");
}
std::string log_excerpt(const std::filesystem::path& log) {
    std::ifstream input(log,std::ios::binary); std::string text(4096,'\0'); input.read(text.data(),text.size()); text.resize(static_cast<std::size_t>(input.gcount()));
    return text;
}
#if defined(_WIN32)
std::wstring quote_windows(const std::wstring& arg) {
    std::wstring result=L"\""; std::size_t slashes=0;
    for(wchar_t c:arg) {
        if(c==L'\\') { ++slashes; continue; }
        result.append(c==L'"'?slashes*2+1:slashes,L'\\'); slashes=0; result+=c;
    }
    result.append(slashes*2,L'\\'); result+=L'"'; return result;
}
std::wstring utf16(const std::string& value) {
    if(value.empty()) return {};
    const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0);
    if(count<=0) throw std::runtime_error("Invalid UTF-8 document tool argument");
    std::wstring output(count,0); MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),output.data(),count); return output;
}
#endif
void run(std::vector<std::string> arguments,const std::filesystem::path& log,std::stop_token stop,std::chrono::seconds timeout=std::chrono::seconds(60)) {
    check_stop(stop);
    auto binary=executable(arguments.front());
    if(binary.empty()) throw std::runtime_error("Required program is unavailable: "+arguments.front());
    const auto deadline=std::chrono::steady_clock::now()+timeout;
    int status=-1;
#if defined(_WIN32)
    SECURITY_ATTRIBUTES attributes{sizeof(attributes),nullptr,TRUE};
    struct ChildResources {
        HANDLE output{INVALID_HANDLE_VALUE},input{INVALID_HANDLE_VALUE};
        PROCESS_INFORMATION process{};LPPROC_THREAD_ATTRIBUTE_LIST list{};
        std::vector<unsigned char> storage;
        void close_stdio()noexcept{if(output!=INVALID_HANDLE_VALUE)CloseHandle(output);if(input!=INVALID_HANDLE_VALUE)CloseHandle(input);output=input=INVALID_HANDLE_VALUE;}
        ~ChildResources(){if(list)DeleteProcThreadAttributeList(list);close_stdio();if(process.hThread)CloseHandle(process.hThread);if(process.hProcess)CloseHandle(process.hProcess);}
    } child;
    child.output=CreateFileW(log.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&attributes,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(child.output==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create document tool log");
    child.input=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&attributes,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(child.input==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot open document tool input");
    SIZE_T attribute_bytes{};
    if(InitializeProcThreadAttributeList(nullptr,1,0,&attribute_bytes)||GetLastError()!=ERROR_INSUFFICIENT_BUFFER||!attribute_bytes)throw std::runtime_error("Cannot size document tool handle list");
    child.storage.resize(attribute_bytes);const auto list=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(child.storage.data());
    if(!InitializeProcThreadAttributeList(list,1,0,&attribute_bytes))throw std::runtime_error("Cannot initialize document tool handle list");
    child.list=list;
    std::array<HANDLE,2> handles{child.output,child.input};
    if(!UpdateProcThreadAttribute(child.list,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles.data(),sizeof(handles),nullptr,nullptr))throw std::runtime_error("Cannot restrict document tool inherited handles");
    STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.lpAttributeList=child.list;
    startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;startup.StartupInfo.hStdOutput=child.output;startup.StartupInfo.hStdError=child.output;startup.StartupInfo.hStdInput=child.input;
    std::wstring command;
    for(const auto& arg:arguments) { if(!command.empty()) command+=L' '; command+=quote_windows(utf16(arg)); }
    auto& process=child.process;
    const bool launched=CreateProcessW(binary.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,nullptr,nullptr,&startup.StartupInfo,&process)!=0;
    child.close_stdio();
    if(!launched) throw std::runtime_error("Cannot start "+arguments.front());
    bool cancelled=false;
    while(WaitForSingleObject(process.hProcess,20)==WAIT_TIMEOUT) {
        if(stop.stop_requested() || std::chrono::steady_clock::now()>deadline) { TerminateProcess(process.hProcess,1); cancelled=true; break; }
    }
    WaitForSingleObject(process.hProcess,INFINITE); DWORD code{}; GetExitCodeProcess(process.hProcess,&code); status=static_cast<int>(code);
    if(cancelled) throw std::runtime_error(stop.stop_requested()?"Document operation cancelled":"Document tool timed out");
#else
    std::vector<char*> argv; for(auto& arg:arguments) argv.push_back(arg.data()); argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    if(posix_spawn_file_actions_init(&actions)!=0) throw std::runtime_error("Cannot prepare document tool");
    posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,log.c_str(),O_WRONLY|O_CREAT|O_TRUNC,0600);
    posix_spawn_file_actions_adddup2(&actions,STDOUT_FILENO,STDERR_FILENO);
    posix_spawn_file_actions_addopen(&actions,STDIN_FILENO,"/dev/null",O_RDONLY,0);
    pid_t child{};
    const int launched=posix_spawn(&child,binary.c_str(),&actions,nullptr,argv.data(),environ);
    posix_spawn_file_actions_destroy(&actions);
    if(launched!=0) throw std::runtime_error("Cannot start "+arguments.front()+": "+std::to_string(launched));
    int wait_status{};
    for(;;) {
        const auto waited=waitpid(child,&wait_status,WNOHANG);
        if(waited==child) { status=WIFEXITED(wait_status)?WEXITSTATUS(wait_status):128; break; }
        if(waited<0 && errno!=EINTR) throw std::runtime_error("Cannot wait for document tool");
        if(stop.stop_requested() || std::chrono::steady_clock::now()>deadline) {
            kill(child,SIGTERM);
            for(int attempt=0;attempt<10;++attempt) { if(waitpid(child,&wait_status,WNOHANG)==child) { child=-1; break; } std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
            if(child>0) { kill(child,SIGKILL); while(waitpid(child,&wait_status,0)<0 && errno==EINTR) {} }
            throw std::runtime_error(stop.stop_requested()?"Document operation cancelled":"Document tool timed out");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
#endif
    if(status!=0) throw std::runtime_error(arguments.front()+" failed: "+log_excerpt(log));
}
using Reader=std::unique_ptr<archive,decltype(&archive_read_free)>;
Reader reader(const std::filesystem::path& path) {
    Reader result{archive_read_new(),archive_read_free};
    if(!result) throw std::runtime_error("Cannot initialize comic archive reader");
    archive_read_support_filter_all(result.get()); archive_read_support_format_zip(result.get()); archive_read_support_format_tar(result.get());
    if(archive_read_open_filename(result.get(),path.string().c_str(),65536)!=ARCHIVE_OK) throw std::runtime_error(std::string("Cannot open comic archive: ")+archive_error_string(result.get()));
    return result;
}
bool natural_less(std::string_view a,std::string_view b) {
    std::size_t x=0,y=0;
    while(x<a.size() && y<b.size()) {
        const auto ca=static_cast<unsigned char>(a[x]),cb=static_cast<unsigned char>(b[y]);
        if(std::isdigit(ca) && std::isdigit(cb)) {
            auto ax=x,by=y;
            while(ax<a.size() && a[ax]=='0') ++ax;
            while(by<b.size() && b[by]=='0') ++by;
            auto ae=ax,be=by;
            while(ae<a.size() && std::isdigit(static_cast<unsigned char>(a[ae]))) ++ae;
            while(be<b.size() && std::isdigit(static_cast<unsigned char>(b[be]))) ++be;
            if(ae-ax!=be-by) return ae-ax<be-by;
            if(auto result=a.substr(ax,ae-ax).compare(b.substr(by,be-by));result!=0) return result<0;
            x=ae;y=be; continue;
        }
        if(std::tolower(ca)!=std::tolower(cb)) return std::tolower(ca)<std::tolower(cb);
        ++x;++y;
    }
    return x==a.size() && y<b.size();
}
}
std::shared_ptr<Document> open(const std::filesystem::path& input,std::stop_token stop) {
    auto document=std::make_shared<Document>(); document->path=std::filesystem::absolute(input).lexically_normal();
    if(!std::filesystem::is_regular_file(document->path) || std::filesystem::file_size(document->path)>max_document_bytes) throw std::runtime_error("Document must be a regular file of at most 512 MiB");
    document->workspace=workspace();
    std::string extension=document->path.extension().string(); std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return std::tolower(c);});
    if(extension==".pdf") {
        document->kind=Kind::pdf;
        if(executable("pdfinfo").empty() || executable("pdftoppm").empty()) throw std::runtime_error("PDF viewing requires Poppler tools: install pdfinfo and pdftoppm and add them to PATH");
        auto log=document->workspace->directory/"pdfinfo.txt";
        run({"pdfinfo",document->path.string()},log,stop);
        std::ifstream info(log); std::string line;
        while(std::getline(info,line)) if(line.starts_with("Pages:")) {
            std::istringstream value(line.substr(6)); value>>document->pages;
        }
        if(document->pages==0 || document->pages>max_pages) throw std::runtime_error("PDF page count is invalid or exceeds 5000 pages");
    } else if(extension==".cbz" || extension==".cbt") {
        document->kind=Kind::comic_archive;
        auto archive=reader(document->path); archive_entry* entry{}; int result{}; std::size_t index=0; std::uintmax_t declared_bytes=0;
        while((result=archive_read_next_header(archive.get(),&entry))==ARCHIVE_OK) {
            check_stop(stop);
            if(index>=max_entries) throw std::runtime_error("Comic archive exceeds the 20000 entry limit");
            const char* raw=archive_entry_pathname_utf8(entry); if(!raw) raw=archive_entry_pathname(entry);
            const std::string name=raw?raw:"";
            const auto size=archive_entry_size(entry);
            if(name.size()>8192 || size<0 || static_cast<std::uintmax_t>(size)>max_uncompressed_archive_bytes-declared_bytes) throw std::runtime_error("Comic archive exceeds its expanded data limit");
            declared_bytes+=static_cast<std::uintmax_t>(size);
            if(archive_entry_filetype(entry)==AE_IFREG && !archive_entry_symlink(entry) && !archive_entry_hardlink(entry) && openxmb::photos::supported_image(name)) {
                if(size<0 || static_cast<std::uintmax_t>(size)>max_page_bytes) throw std::runtime_error("Comic page exceeds the 128 MiB limit");
                if(archive_entry_is_encrypted(entry)) throw std::runtime_error("Encrypted comic archives are not supported");
                document->archive_pages.push_back({name,index,static_cast<std::uintmax_t>(size)});
                if(document->archive_pages.size()>max_pages) throw std::runtime_error("Comic archive exceeds 5000 pages");
            }
            archive_read_data_skip(archive.get()); ++index;
        }
        if(result!=ARCHIVE_EOF) throw std::runtime_error(std::string("Cannot read comic archive: ")+archive_error_string(archive.get()));
        std::stable_sort(document->archive_pages.begin(),document->archive_pages.end(),[](const auto& a,const auto& b){return natural_less(a.name,b.name);});
        document->pages=document->archive_pages.size();
        if(document->pages==0) throw std::runtime_error("Comic archive contains no supported image pages");
    } else throw std::runtime_error("Supported documents are PDF, CBZ and CBT");
    return document;
}
std::filesystem::path render_page(const Document& document,std::size_t page,std::stop_token stop) {
    check_stop(stop);
    if(page>=document.pages) throw std::out_of_range("Document page is out of range");
    // Keep only the currently requested raster, so a large book cannot fill disk.
    const auto output=document.workspace->directory/"current-page.png";
    if(document.kind==Kind::pdf) {
        const auto prefix=document.workspace->directory/"raster";
        const auto number=std::to_string(page+1);
        run({"pdftoppm","-f",number,"-l",number,"-singlefile","-scale-to","1600","-png",document.path.string(),prefix.string()},document.workspace->directory/"render.log",stop);
        const auto raster=prefix; std::filesystem::path png=raster; png+=".png";
        std::error_code ec; std::filesystem::remove(output,ec); std::filesystem::rename(png,output);
    } else {
        const auto& selected=document.archive_pages.at(page); auto archive=reader(document.path); archive_entry* entry{}; std::size_t index=0; int status{}; bool found=false;
        while((status=archive_read_next_header(archive.get(),&entry))==ARCHIVE_OK) {
            check_stop(stop);
            if(index++!=selected.entry) { archive_read_data_skip(archive.get()); continue; }
            const char* current_name=archive_entry_pathname_utf8(entry);if(!current_name) current_name=archive_entry_pathname(entry);
            if(!current_name || selected.name!=current_name || archive_entry_size(entry)<0 || static_cast<std::uintmax_t>(archive_entry_size(entry))!=selected.bytes || archive_entry_filetype(entry)!=AE_IFREG || archive_entry_symlink(entry) || archive_entry_hardlink(entry)) throw std::runtime_error("Comic archive changed while viewing");
            std::ofstream destination(output,std::ios::binary|std::ios::trunc); if(!destination) throw std::runtime_error("Cannot create comic page cache");
            std::array<char,65536> buffer{}; std::uintmax_t total=0; la_ssize_t read{};
            while((read=archive_read_data(archive.get(),buffer.data(),buffer.size()))>0) {
                check_stop(stop); total+=static_cast<std::uintmax_t>(read);
                if(total>max_page_bytes || total>selected.bytes) throw std::runtime_error("Comic page exceeds its declared size");
                destination.write(buffer.data(),read); if(!destination) throw std::runtime_error("Cannot write comic page cache");
            }
            if(read<0 || total!=selected.bytes) throw std::runtime_error("Comic page is truncated or cannot be decompressed");
            found=true; break;
        }
        if(!found) throw std::runtime_error("Comic archive page is unavailable");
    }
    if(std::filesystem::file_size(output)>max_page_bytes) throw std::runtime_error("Rendered page exceeds the cache limit");
    check_stop(stop); return output;
}
std::string page_label(const Document& document,std::size_t page) {
    return document.kind==Kind::comic_archive?document.archive_pages.at(page).name:"Page "+std::to_string(page+1);
}
bool printing_available() { return !executable("lp").empty() || !executable("lpr").empty(); }
void print(const Document& document,std::optional<std::size_t> page,std::stop_token stop) {
    auto printer=!executable("lp").empty()?"lp":"lpr";
    if(executable(printer).empty()) throw std::runtime_error("Printing requires a configured CUPS printer and lp or lpr in PATH");
    if(page && *page>=document.pages) throw std::out_of_range("Print page is out of range");
    std::vector<std::string> arguments{printer};
    if(document.kind==Kind::pdf) {
        if(page) { arguments.push_back("-o"); arguments.push_back("page-ranges="+std::to_string(*page+1)); }
        arguments.push_back(document.path.string());
    } else {
        if(!page) throw std::runtime_error("Choose a comic page to print");
        arguments.push_back(render_page(document,*page,stop).string());
    }
    run(std::move(arguments),document.workspace->directory/"print.log",stop,std::chrono::seconds(20));
}
std::string information(const Document& document) {
    std::ostringstream output; output<<document.path.filename().string()<<"\n"<<document.pages<<" pages\n"<<(document.kind==Kind::pdf?"PDF":"Comic archive")<<"\n"<<std::fixed<<std::setprecision(1)<<static_cast<double>(std::filesystem::file_size(document.path))/(1024.0*1024.0)<<" MiB\n"<<document.path.string(); return output.str();
}
}
