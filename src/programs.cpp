module;

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <map>

module openxmb.app;

import :programs;

namespace programs {

file_info::file_info(const std::filesystem::path& path) {
    name = path.filename().string();
    display_name = name;
    
    // Determine MIME type based on file extension
    std::string ext = path.extension().string();
    if (ext.empty()) {
        mime_type = "application/octet-stream";
        content_type = "unknown";
        fast_content_type = "unknown";
    } else {
        // Common MIME type mappings
        static const std::map<std::string, std::string> mime_types = {
            {".txt", "text/plain"},
            {".md", "text/markdown"},
            {".html", "text/html"},
            {".htm", "text/html"},
            {".css", "text/css"},
            {".js", "application/javascript"},
            {".json", "application/json"},
            {".xml", "application/xml"},
            {".pdf", "application/pdf"},
            {".jpg", "image/jpeg"},
            {".jpeg", "image/jpeg"},
            {".png", "image/png"},
            {".gif", "image/gif"},
            {".bmp", "image/bmp"},
            {".svg", "image/svg+xml"},
            {".ico", "image/x-icon"},
            {".mp3", "audio/mpeg"},
            {".wav", "audio/wav"},
            {".ogg", "audio/ogg"},
            {".mp4", "video/mp4"},
            {".avi", "video/x-msvideo"},
            {".mkv", "video/x-matroska"},
            {".mov", "video/quicktime"},
            {".zip", "application/zip"},
            {".tar", "application/x-tar"},
            {".gz", "application/gzip"},
            {".7z", "application/x-7z-compressed"},
            {".exe", "application/x-executable"},
            {".deb", "application/vnd.debian.binary-package"},
            {".rpm", "application/x-rpm"},
            {".app", "application/x-executable"},
            {".dmg", "application/x-apple-diskimage"}
        };
        
        auto it = mime_types.find(ext);
        if (it != mime_types.end()) {
            mime_type = it->second;
            content_type = it->second;
            fast_content_type = it->second;
        } else {
            mime_type = "application/octet-stream";
            content_type = "unknown";
            fast_content_type = "unknown";
        }
    }
    
    // Try to determine icon name based on file type
    if (mime_type.find("text/") == 0) {
        icon_name = "text-x-generic";
    } else if (mime_type.find("image/") == 0) {
        icon_name = "image-x-generic";
    } else if (mime_type.find("audio/") == 0) {
        icon_name = "audio-x-generic";
    } else if (mime_type.find("video/") == 0) {
        icon_name = "video-x-generic";
    } else if (mime_type.find("application/") == 0) {
        icon_name = "application-x-generic";
    } else {
        icon_name = "text-x-generic";
    }
    
    content_type_string = mime_type;
}

}
