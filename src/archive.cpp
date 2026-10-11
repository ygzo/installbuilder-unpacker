#include "ib.hpp"
#include <cstring>
#include <limits>
#include <mk4.h>

namespace ib {
uint32_t be32(const uint8_t *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
std::string text(const Bytes &b) { return std::string(b.begin(), b.end()); }
Bytes bytes(const std::string &s) { return Bytes(s.begin(), s.end()); }
File::File(const fs::path &path) : stream_(path, std::ios::binary), size(0) {
    if (!stream_)
        fail("Cannot open input: " + path.u8string());
    stream_.seekg(0, std::ios::end);
    auto end = stream_.tellg();
    if (end < 0)
        fail("Cannot determine input size");
    size = uint64_t(end);
}
Bytes File::read(uint64_t offset, size_t count) {
    if (count > max_buffer || offset > size || count > size - offset)
        fail("Archive read outside bounds or buffer limit");
    Bytes result(count);
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(offset));
    if (count &&
        !stream_.read(reinterpret_cast<char *>(result.data()), static_cast<std::streamsize>(count)))
        fail("Truncated input");
    return result;
}
uint8_t Cursor::byte() {
    if (pos >= data.size())
        fail("Truncated archive record");
    return data[pos++];
}
uint32_t Cursor::u32() {
    auto b = take(4);
    return be32(b.data());
}
uint64_t Cursor::u64() {
    uint64_t a = u32();
    return (a << 32) | u32();
}
Bytes Cursor::take(size_t n) {
    if (pos > data.size() || n > data.size() - pos)
        fail("Truncated archive record");
    Bytes out(data.begin() + pos, data.begin() + pos + n);
    pos += n;
    return out;
}
std::string Cursor::text(size_t n) { return ib::text(take(n)); }

class InputStream : public c4_Stream {
    File &file_;
    uint64_t offset_, left_;

  public:
    InputStream(File &f, uint64_t offset, uint64_t count)
        : file_(f), offset_(offset), left_(count) {}
    int Read(void *dest, int length) override {
        if (length < 0)
            fail("Invalid Metakit read");
        size_t n = static_cast<size_t>(std::min<uint64_t>(uint64_t(length), left_));
        auto b = file_.read(offset_, n);
        if (n)
            std::memcpy(dest, b.data(), n);
        offset_ += n;
        left_ -= n;
        return int(n);
    }
    bool Write(const void *, int) override { return false; }
};
static std::unique_ptr<c4_Storage> load(File &file) {
    auto tail = file.read(file.size - std::min<uint64_t>(4096, file.size),
                          static_cast<size_t>(std::min<uint64_t>(4096, file.size)));
    uint64_t end = file.size;
    auto marker = text(tail).find("bitrock-lzma-4.0");
    if (marker != std::string::npos)
        end = file.size - tail.size() + marker;
    uint64_t start = 0;
    if (end >= 16) {
        auto trailer = file.read(end - 16, 16);
        if (be32(trailer.data()) == 0x80000000 && (be32(trailer.data() + 8) >> 24) == 0x80) {
            uint64_t n = be32(trailer.data() + 4);
            if (n > end - 16)
                fail("Invalid Metakit trailer offset");
            start = end - 16 - n;
        }
    }
    if (end - start > max_buffer)
        fail("Metakit region exceeds 256 MiB limit");
    auto magic = file.read(start, 2);
    if (magic != bytes("JL") && magic != bytes("LJ"))
        fail("Embedded Metakit archive not found");
    InputStream input(file, start, end - start);
    auto db = std::make_unique<c4_Storage>();
    if (!db->LoadFrom(input))
        fail("Unable to load Metakit archive");
    if (db->View("dirs").GetSize() == 0)
        fail("Metakit filesystem schema missing");
    return db;
}
static std::vector<std::string> directory_paths(c4_View dirs) {
    c4_StringProp name("name");
    c4_IntProp parent("parent");
    std::vector<std::string> paths(dirs.GetSize());
    std::vector<uint8_t> states(dirs.GetSize());
    auto visit = [&](auto &&self, int i, unsigned depth) -> std::string {
        if (i < 0 || i >= dirs.GetSize() || depth > 256)
            fail("Invalid Metakit directory parent");
        if (states[i] == 1)
            fail("Metakit directory cycle");
        if (states[i] == 2)
            return paths[i];
        states[i] = 1;
        int p = parent(dirs[i]);
        if (p >= 0) {
            std::string part = static_cast<const char *>(name(dirs[i]));
            safe_relative(part);
            auto root = self(self, p, depth + 1);
            paths[i] = root.empty() ? part : root + "/" + part;
        }
        states[i] = 2;
        return paths[i];
    };
    for (int i = 0; i < dirs.GetSize(); ++i)
        visit(visit, i, 0);
    return paths;
}
static Bytes content(c4_RowRef row) {
    c4_IntProp size("size");
    c4_BytesProp data("contents");
    c4_Bytes b = data(row);
    if (size(row) < 0 || size_t(size(row)) > max_buffer)
        fail("Invalid Metakit file size");
    Bytes value;
    if (b.Size())
        value.assign(b.Contents(), b.Contents() + b.Size());
    if (value.size() != size_t(size(row))) {
        // Tcl's mk4 VFS uses a zlib-wrapped stream for compressed contents.
        value = inflate(value, false);
        if (value.size() != size_t(size(row)))
            fail("Metakit file size mismatch");
    }
    return value;
}
Metadata read_metakit(File &file) {
    auto db = load(file);
    c4_View dirs = db->View("dirs");
    auto paths = directory_paths(dirs);
    c4_ViewProp files("files");
    c4_StringProp name("name");
    Metadata out;
    bool manifest = false;
    for (int i = 0; i < dirs.GetSize(); ++i) {
        c4_View rows = files(dirs[i]);
        for (int j = 0; j < rows.GetSize(); ++j) {
            std::string n = static_cast<const char *>(name(rows[j]));
            safe_relative(n);
            std::string path = paths[i].empty() ? n : paths[i] + "/" + n;
            if (path == "manifest.txt") {
                out.manifest = text(content(rows[j]));
                manifest = true;
            } else if (path == "cookfsinfo.txt")
                out.cookfsinfo = text(content(rows[j]));
            else if (path == "project.xml" || path.rfind("images/", 0) == 0)
                out.extras.emplace(path, content(rows[j]));
        }
    }
    if (!manifest)
        fail("manifest.txt not found in Metakit archive");
    return out;
}
} // namespace ib
