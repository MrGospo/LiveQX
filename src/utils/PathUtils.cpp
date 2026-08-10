#include "utils/PathUtils.h"

#include <fstream>
#include <stdexcept>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

namespace PathUtils {

std::string sanitizeForPath(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c < 0x20)             { out.push_back('_'); continue; }
        if (c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            out.push_back('_'); continue;
        }
        out.push_back(static_cast<char>(c));
    }
    size_t a = 0;
    while (a < out.size() && (out[a] == '.' || out[a] == ' ')) ++a;
    size_t b = out.size();
    while (b > a && (out[b - 1] == ' ' || out[b - 1] == '\t')) --b;
    return out.substr(a, b - a);
}

void atomicWriteFile(const std::filesystem::path& target,
                     const std::string& payload) {
    namespace fs = std::filesystem;
    const auto tmp = target.string() + ".tmp";

    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("cannot open " + tmp);
        f << payload;
        f.flush();
        if (!f) throw std::runtime_error("write failed: " + tmp);
    }

    {
        const int fd = ::open(tmp.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            ::fsync(fd);
            ::close(fd);
        }
    }

    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec) {
        // Cross-FS rename returns EXDEV — fall back to copy + remove. Not
        // atomic, but the old target is preserved if copy_file throws.
        fs::copy_file(tmp, target, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp);
        if (ec) throw std::runtime_error("rename/copy failed: " + ec.message());
    }

    {
        const int dfd = ::open(target.parent_path().c_str(),
                               O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dfd >= 0) {
            ::fsync(dfd);
            ::close(dfd);
        }
    }
}

}  // namespace PathUtils
