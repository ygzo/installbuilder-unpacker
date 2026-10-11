#include "ib.hpp"
#include <cctype>
#include <limits>
#include <stdexcept>

namespace ib {
[[noreturn]] void fail(const std::string &message) { throw std::runtime_error(message); }
static bool space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}
static void utf8(std::string &out, uint32_t cp) {
    if (cp < 0x80)
        out += char(cp);
    else if (cp < 0x800) {
        out += char(0xc0 | (cp >> 6));
        out += char(0x80 | (cp & 63));
    } else {
        out += char(0xe0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 63));
        out += char(0x80 | (cp & 63));
    }
}
static void escape(const std::string &s, size_t &p, std::string &out) {
    if (++p == s.size()) {
        out += '\\';
        return;
    }
    char c = s[p++];
    switch (c) {
    case 'a':
        out += '\a';
        return;
    case 'b':
        out += '\b';
        return;
    case 'f':
        out += '\f';
        return;
    case 'n':
        out += '\n';
        return;
    case 'r':
        out += '\r';
        return;
    case 't':
        out += '\t';
        return;
    case 'v':
        out += '\v';
        return;
    case '\n':
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t'))
            ++p;
        out += ' ';
        return;
    default:
        break;
    }
    if (c == 'x' || c == 'u') {
        uint32_t v = 0;
        size_t begin = p, limit = c == 'x' ? 2 : 4;
        while (p < s.size() && p - begin < limit &&
               std::isxdigit(static_cast<unsigned char>(s[p]))) {
            char h = s[p++];
            v = (v << 4) | (h <= '9' ? h - '0' : (h | 32) - 'a' + 10);
        }
        if (p == begin)
            out += c;
        else
            utf8(out, v);
    } else if (c >= '0' && c <= '7') {
        unsigned v = c - '0', n = 1;
        while (p < s.size() && n < 3 && s[p] >= '0' && s[p] <= '7' && (n < 2 || v < 32)) {
            v = (v << 3) + (s[p++] - '0');
            ++n;
        }
        utf8(out, v);
    } else
        out += c;
}
std::vector<std::string> tcl_list(const std::string &s) {
    std::vector<std::string> result;
    size_t p = 0;
    while (p < s.size()) {
        while (p < s.size() && space(s[p]))
            ++p;
        if (p == s.size())
            break;
        std::string word;
        if (s[p] == '{') {
            unsigned depth = 1;
            ++p;
            while (p < s.size() && depth) {
                char c = s[p++];
                if (c == '\\' && p < s.size()) {
                    if (s[p] == '\n') {
                        ++p;
                        while (p < s.size() && (s[p] == ' ' || s[p] == '\t'))
                            ++p;
                        word += ' ';
                    } else {
                        word += c;
                        word += s[p++];
                    }
                } else if (c == '{') {
                    if (++depth > 256)
                        fail("Tcl list nesting limit exceeded");
                    word += c;
                } else if (c == '}') {
                    if (--depth)
                        word += c;
                } else
                    word += c;
            }
            if (depth)
                fail("Unclosed brace in Tcl list");
            if (p < s.size() && !space(s[p]))
                fail("Characters after braced Tcl list element");
        } else if (s[p] == '"') {
            ++p;
            bool closed = false;
            while (p < s.size()) {
                if (s[p] == '"') {
                    ++p;
                    closed = true;
                    break;
                }
                if (s[p] == '\\')
                    escape(s, p, word);
                else
                    word += s[p++];
            }
            if (!closed)
                fail("Unclosed quote in Tcl list");
            if (p < s.size() && !space(s[p]))
                fail("Characters after quoted Tcl list element");
        } else {
            while (p < s.size() && !space(s[p])) {
                if (s[p] == '\\')
                    escape(s, p, word);
                else
                    word += s[p++];
            }
        }
        result.push_back(std::move(word));
    }
    return result;
}
uint64_t decimal(const std::string &s) {
    if (s.empty())
        fail("Missing integer");
    uint64_t n = 0;
    for (char c : s) {
        if (c < '0' || c > '9' || n > (UINT64_MAX - uint64_t(c - '0')) / 10)
            fail("Invalid integer: " + s);
        n = n * 10 + (c - '0');
    }
    return n;
}
fs::path safe_relative(const std::string &s) {
    if (s.empty() || s.find('\0') != std::string::npos || s.find('\\') != std::string::npos)
        fail("Unsafe archive path");
    fs::path p = fs::u8path(s);
    if (p.is_absolute() || p.has_root_name())
        fail("Absolute archive path: " + s);
    for (const auto &part : p) {
        if (part == ".." || part == ".")
            fail("Unsafe archive path: " + s);
#ifdef _WIN32
        auto name = part.u8string();
        if (name.empty() || name.back() == ' ' || name.back() == '.')
            fail("Filename cannot be represented on Windows: " + s);
        for (unsigned char c : name)
            if (c < 32 || std::string(":*?\"<>|").find(c) != std::string::npos)
                fail("Filename cannot be represented on Windows: " + s);
        auto base = name.substr(0, name.find('.'));
        for (auto &c : base)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" || base == "CONIN$" ||
            base == "CONOUT$" ||
            (base.size() == 4 && (base.substr(0, 3) == "COM" || base.substr(0, 3) == "LPT") &&
             base[3] >= '1' && base[3] <= '9'))
            fail("Reserved Windows filename: " + s);
#endif
    }
    return p;
}
} // namespace ib
