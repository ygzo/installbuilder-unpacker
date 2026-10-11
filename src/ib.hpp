#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ib {
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;
constexpr size_t max_buffer = 256 * 1024 * 1024;
[[noreturn]] void fail(const std::string &message);
std::vector<std::string> tcl_list(const std::string &input);
uint64_t decimal(const std::string &input);
fs::path safe_relative(const std::string &name);

class File {
    std::ifstream stream_;

  public:
    uint64_t size;
    explicit File(const fs::path &path);
    Bytes read(uint64_t offset, size_t length);
};
struct Cursor {
    const Bytes &data;
    size_t pos = 0;
    uint8_t byte();
    uint32_t u32();
    uint64_t u64();
    Bytes take(size_t count);
    std::string text(size_t count);
};
uint32_t be32(const uint8_t *p);
std::string text(const Bytes &data);
Bytes bytes(const std::string &data);

struct Metadata {
    std::string manifest;
    std::string cookfsinfo;
    std::map<std::string, Bytes> extras;
};
Metadata read_metakit(File &file);
Bytes inflate(const Bytes &data, bool raw);
Bytes decode_lzma(const Bytes &data);
Bytes decode_lzham(const Bytes &data);
Bytes sha256(const Bytes &data);
Bytes twofish(const Bytes &key, const Bytes &input, Bytes &iv, bool encrypt);

class Crypto {
    Bytes key_, ivs_;

  public:
    void initialize(const std::string &password, const Bytes &info);
    Bytes decrypt(const Bytes &input) const;
};
struct Block {
    uint32_t page, offset, size;
};
struct Entry {
    uint64_t mtime;
    std::vector<Block> blocks;
};
class PageDecoder;
class Cookfs {
    File &file_;
    std::string codec_;
    bool encrypted_ = false;
    Crypto crypto_;
    std::vector<uint64_t> offsets_;
    std::vector<uint32_t> sizes_;
    struct CachedPage {
        Bytes data;
        std::list<uint32_t>::iterator position;
    };
    std::map<uint32_t, CachedPage> cache_;
    std::list<uint32_t> recent_pages_;
    size_t cache_bytes_ = 0;
    std::vector<uint32_t> page_plan_;
    size_t next_page_ = 0;
    struct PendingPage {
        std::future<Bytes> result;
        uint64_t memory;
    };
    std::map<uint32_t, PendingPage> pending_;
    uint64_t pending_bytes_ = 0;
    std::unique_ptr<PageDecoder> decoder_;
    Bytes decode(const Bytes &data);
    uint64_t page_memory(uint32_t index);
    void prefetch();
    void read_tree(Cursor &cursor, const std::string &parent, unsigned depth);

  public:
    std::map<std::string, Entry> entries;
    std::map<std::string, Bytes> metadata;
    Cookfs(File &file, const std::string &options, const std::string &password);
    ~Cookfs();
    void prepare(const std::vector<uint32_t> &pages);
    const Bytes &page(uint32_t index);
    void write(const std::string &name, std::ostream &out,
               const std::function<void(uint64_t)> &written = {});
};
struct Progress {
    uint64_t bytes = 0, total_bytes = 0, entries = 0, total_entries = 0;
    bool finished = false;
};
struct Options {
    fs::path source, destination;
    std::string password;
    bool overwrite = false;
    std::function<void(const Progress &)> progress;
};
struct Stats {
    uint64_t files = 0, directories = 0, links = 0, bytes = 0;
};
Stats extract(const Options &options);
} // namespace ib
