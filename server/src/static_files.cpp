#include <filesystem>
#include <fstream>
#include <sstream>

#include "httplib.h"
#include "sovd/server/routes.hpp"

namespace sovd::server {

namespace fs = std::filesystem;

static const char *mime_for(const std::string &ext) {
    if (ext == ".html") return "text/html; charset=utf-8";
    if (ext == ".js" || ext == ".mjs") return "text/javascript; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".png") return "image/png";
    if (ext == ".json") return "application/json";
    return "application/octet-stream";
}

// Serves `root`/path for a GET. Unknown extension-less paths fall back to
// index.html (SPA routing); anything that would escape root is refused.
bool serve_static_file(const std::string &root, const httplib::Request &req, httplib::Response &res) {
    if (req.method != "GET") return false;
    std::string p = req.path;
    bool root_html = p == "/" && req.get_header_value("Accept").find("text/html") != std::string::npos;
    if (p == "/" && !root_html) return false; // "/" stays the SOVD version-discovery endpoint
    if (p.rfind("/v1", 0) == 0 && (p.size() == 3 || p[3] == '/')) return false;
    if (p.find('\0') != std::string::npos || p.find("..") != std::string::npos) return false;

    std::error_code ec;
    fs::path base = fs::weakly_canonical(root, ec);
    if (ec) return false;
    if (root_html) p = "/index.html";
    fs::path file = fs::weakly_canonical(base / p.substr(1), ec);
    if (ec) return false;
    auto under_base = [&](const fs::path &f) {
        auto rel = f.lexically_relative(base);
        return !rel.empty() && *rel.begin() != "..";
    };
    if (!under_base(file)) return false;
    if (!fs::is_regular_file(file, ec)) {
        if (file.has_extension()) return false;
        file = base / "index.html";
        if (!fs::is_regular_file(file, ec)) return false;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    res.status = 200;
    res.set_content(ss.str(), mime_for(file.extension().string()));
    return true;
}

} // namespace sovd::server
