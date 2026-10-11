#include "ib.hpp"
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <set>
#include <thread>

namespace ib {
class PageDecoder {
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::packaged_task<Bytes()>> tasks_;
    bool stopping_ = false;
    std::vector<std::thread> workers_;

    void stop() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            tasks_.clear();
        }
        ready_.notify_all();
        for (auto &worker : workers_)
            worker.join();
    }

  public:
    explicit PageDecoder(unsigned count) {
        try {
            for (unsigned i = 0; i < count; ++i)
                workers_.emplace_back([this] {
                    for (;;) {
                        std::packaged_task<Bytes()> task;
                        {
                            std::unique_lock<std::mutex> lock(mutex_);
                            ready_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
                            if (stopping_)
                                return;
                            task = std::move(tasks_.front());
                            tasks_.pop_front();
                        }
                        task(); // Decoder exceptions travel through the future.
                    }
                });
        } catch (...) {
            stop();
            throw;
        }
    }
    ~PageDecoder() { stop(); }
    std::future<Bytes> submit(std::function<Bytes()> decode) {
        std::packaged_task<Bytes()> task(std::move(decode));
        auto result = task.get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.push_back(std::move(task));
        }
        ready_.notify_one();
        return result;
    }
};

Cookfs::~Cookfs() = default;

void Cookfs::prepare(const std::vector<uint32_t> &pages) {
    std::set<uint32_t> seen;
    for (auto index : pages) {
        if (index >= sizes_.size())
            fail("Cookfs page number outside bounds");
        if (seen.insert(index).second)
            page_plan_.push_back(index);
    }
    if (page_plan_.size() > 1 && std::thread::hardware_concurrency() > 1) {
        decoder_ = std::make_unique<PageDecoder>(2);
        prefetch();
    }
}

uint64_t Cookfs::page_memory(uint32_t index) {
    // Reserve for input copies, decoder state and output allocation. Unknown
    // or encrypted formats use their maximum supported sizes and run alone.
    constexpr uint64_t mib = 1024 * 1024;
    uint64_t memory = 4ULL * sizes_[index] + 2ULL * max_buffer + 512 * mib + 16 * mib;
    if (!encrypted_ && codec_ == "lzma" && sizes_[index] >= 14) {
        auto header = file_.read(offsets_[index], 14);
        if (header[0] == 255) {
            uint64_t dictionary = 0, length = 0;
            for (unsigned i = 0; i < 4; ++i)
                dictionary |= uint64_t(header[2 + i]) << (8 * i);
            for (unsigned i = 0; i < 8; ++i)
                length |= uint64_t(header[6 + i]) << (8 * i);
            uint64_t output = length <= max_buffer ? length : 2ULL * max_buffer;
            uint64_t state = std::min<uint64_t>(512 * mib, dictionary * 2 + 16 * mib);
            memory = 2ULL * sizes_[index] + output + state + 16 * mib;
        }
    }
    return memory;
}

void Cookfs::prefetch() {
    constexpr uint64_t budget = 1024ULL * 1024 * 1024;
    while (decoder_ && pending_.size() < 4 && next_page_ < page_plan_.size()) {
        auto index = page_plan_[next_page_];
        if (cache_.count(index) || pending_.count(index)) {
            ++next_page_;
            continue;
        }
        auto memory = page_memory(index);
        if (!pending_.empty() && pending_bytes_ + memory > budget)
            break;
        // Only the main thread touches File's seekable stream.
        auto input = file_.read(offsets_[index], sizes_[index]);
        auto result = decoder_->submit([this, input = std::move(input)] { return decode(input); });
        pending_.emplace(index, PendingPage{std::move(result), memory});
        pending_bytes_ += memory;
        ++next_page_;
    }
}

Cookfs::Cookfs(File &file, const std::string &options, const std::string &password) : file_(file) {
    uint64_t end = file.size;
    bool explicit_end = false;
    auto args = tcl_list(options);
    if (args.size() % 2)
        fail("Malformed Cookfs options");
    for (size_t i = 0; i < args.size(); i += 2) {
        if (args[i] == "-endoffset" && !args[i + 1].empty()) {
            end = decimal(args[i + 1]);
            explicit_end = true;
        } else if (args[i] == "-decompresscommand") {
            auto command = tcl_list(args[i + 1]);
            if (command.empty())
                continue;
            if (command.size() == 1 && command[0] == "lzmadec")
                codec_ = "lzma";
            else if (command.size() == 1 && (command[0] == "tcllzham::decompress" ||
                                             command[0] == "::tcllzham::decompress"))
                codec_ = "lzham";
            else if (command.size() == 2 &&
                     (command[0] == "::maui::util::MI_oJ" || command[0] == "maui::util::MI_oJ")) {
                encrypted_ = true;
                codec_ = command[1];
                if (codec_ == "0")
                    codec_ = "zip";
                if (codec_ == "1")
                    codec_ = "lzma";
                if (codec_ == "lzma-ultra")
                    codec_ = "lzma";
                if (codec_ == "lzham-ultra")
                    codec_ = "lzham";
                if (codec_ != "zip" && codec_ != "lzma" && codec_ != "lzham")
                    fail("Unsupported encrypted payload codec: " + codec_);
            } else
                fail("Unsupported Cookfs decompressor: " + args[i + 1]);
        } else if (args[i] == "-pagecachesize" || args[i] == "-pagesize" ||
                   args[i] == "-compression") { /* writer/performance hints */
        } else if (args[i] == "-endoffset") {   /* empty offset means trailer discovery */
        } else
            fail("Unsupported Cookfs option: " + args[i]);
    }
    if (!explicit_end) {
        auto n = std::min<uint64_t>(file.size, 4096);
        auto tail = file.read(file.size - n, size_t(n));
        auto found = text(tail).rfind("CFS0002");
        if (found == std::string::npos)
            fail("Cookfs footer not found");
        end = file.size - n + found + 7;
    }
    if (end < 16 || end > file.size)
        fail("Invalid Cookfs end offset");
    auto footer = file.read(end - 16, 16);
    if (text(Bytes(footer.begin() + 9, footer.end())) != "CFS0002")
        fail("Unsupported Cookfs signature");
    uint32_t index_size = be32(footer.data()), count = be32(footer.data() + 4);
    uint64_t table_size = uint64_t(count) * 20;
    if (index_size > max_buffer || table_size > max_buffer ||
        uint64_t(index_size) + table_size > end - 16)
        fail("Invalid Cookfs index bounds");
    uint64_t table_start = end - 16 - index_size - table_size;
    auto table = file.read(table_start + uint64_t(count) * 16, size_t(count) * 4);
    uint64_t total = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t size = be32(table.data() + size_t(i) * 4);
        if (size > max_buffer)
            fail("Cookfs page exceeds limit");
        sizes_.push_back(size);
        total += size;
    }
    if (total > table_start)
        fail("Cookfs pages exceed archive bounds");
    uint64_t offset = table_start - total;
    for (auto size : sizes_) {
        offsets_.push_back(offset);
        offset += size;
    }
    auto index = decode(file.read(end - 16 - index_size, index_size));
    Cursor c{index};
    if (c.text(8) != "CFS2.200")
        fail("Unsupported Cookfs filesystem index");
    read_tree(c, "", 0);
    if (c.pos < index.size()) {
        uint32_t n = c.u32();
        if (n > (index.size() - c.pos) / 4)
            fail("Invalid metadata count");
        for (uint32_t i = 0; i < n; ++i) {
            auto record = c.take(c.u32());
            auto split = std::find(record.begin(), record.end(), uint8_t(0));
            if (split == record.end())
                fail("Malformed Cookfs metadata");
            std::string key(record.begin(), split);
            if (!metadata.emplace(key, Bytes(split + 1, record.end())).second)
                fail("Duplicate Cookfs metadata");
        }
    }
    if (c.pos != index.size())
        fail("Trailing Cookfs filesystem index data");
    auto info = metadata.find("installbuilder.payloadinfo");
    if (info != metadata.end()) {
        if (password.empty())
            fail("This installer needs a password to unpack");
        crypto_.initialize(password, info->second);
    } else if (encrypted_)
        fail("Encrypted installer is missing password metadata");
}
Bytes Cookfs::decode(const Bytes &input) {
    if (input.empty())
        return {};
    uint8_t type = input[0];
    Bytes data(input.begin() + 1, input.end());
    switch (type) {
    case 0:
        return data;
    case 1:
        return inflate(data, true);
    case 255:
        if (encrypted_)
            data = crypto_.decrypt(data);
        if (codec_ == "lzma")
            return decode_lzma(data);
        if (codec_ == "lzham")
            return decode_lzham(data);
        if (codec_ == "zip")
            return inflate(data, true);
        fail("Custom-compressed Cookfs page has no decompressor");
    default:
        fail("Unsupported Cookfs page compression ID: " + std::to_string(type));
    }
}
void Cookfs::read_tree(Cursor &c, const std::string &parent, unsigned depth) {
    if (depth > 256)
        fail("Cookfs directory nesting limit exceeded");
    uint32_t count = c.u32();
    if (count > (c.data.size() - c.pos) / 14)
        fail("Invalid Cookfs directory entry count");
    for (uint32_t i = 0; i < count; ++i) {
        auto name = c.text(c.byte());
        if (c.byte() != 0)
            fail("Cookfs filename terminator missing");
        auto component = safe_relative(name);
        if (component.has_parent_path())
            fail("Invalid Cookfs filename component");
        std::string path = parent.empty() ? name : parent + "/" + name;
        uint64_t mtime = c.u64();
        uint32_t n = c.u32();
        if (n == UINT32_MAX) {
            read_tree(c, path, depth + 1);
            continue;
        }
        if (n > (c.data.size() - c.pos) / 12)
            fail("Invalid Cookfs file block count");
        Entry entry{mtime, {}};
        for (uint32_t j = 0; j < n; ++j) {
            Block b{c.u32(), c.u32(), c.u32()};
            if ((b.size && b.page >= sizes_.size()) || uint64_t(b.offset) + b.size > max_buffer)
                fail("Invalid Cookfs file block");
            entry.blocks.push_back(b);
        }
        if (!entries.emplace(path, std::move(entry)).second)
            fail("Duplicate Cookfs file path: " + path);
    }
}
const Bytes &Cookfs::page(uint32_t index) {
    if (index >= sizes_.size())
        fail("Cookfs page number outside bounds");
    auto found = cache_.find(index);
    if (found != cache_.end()) {
        recent_pages_.splice(recent_pages_.end(), recent_pages_, found->second.position);
        return found->second.data;
    }
    Bytes decoded;
    auto pending = pending_.find(index);
    if (pending != pending_.end()) {
        decoded = pending->second.result.get();
        pending_bytes_ -= pending->second.memory;
        pending_.erase(pending);
    } else {
        // A previously used page may have been evicted. Release speculative
        // work before decoding it, keeping the memory bound on this path too.
        bool parallel = bool(decoder_);
        decoder_.reset();
        pending_.clear();
        pending_bytes_ = 0;
        decoded = decode(file_.read(offsets_[index], sizes_[index]));
        if (parallel)
            decoder_ = std::make_unique<PageDecoder>(2);
    }
    // Large installers commonly share 80 MiB pages across many files. Keep
    // recently used pages instead of discarding the entire cache on a miss.
    constexpr size_t cache_limit = 128 * 1024 * 1024;
    while (!cache_.empty() &&
           (cache_bytes_ + decoded.size() > cache_limit || cache_.size() >= 128)) {
        auto oldest = cache_.find(recent_pages_.front());
        cache_bytes_ -= oldest->second.data.size();
        cache_.erase(oldest);
        recent_pages_.pop_front();
    }
    cache_bytes_ += decoded.size();
    recent_pages_.push_back(index);
    auto inserted =
        cache_.emplace(index, CachedPage{std::move(decoded), std::prev(recent_pages_.end())});
    prefetch();
    return inserted.first->second.data;
}
void Cookfs::write(const std::string &name, std::ostream &out,
                   const std::function<void(uint64_t)> &written) {
    auto found = entries.find(name);
    if (found == entries.end())
        fail("Payload file not found: " + name);
    for (const auto &b : found->second.blocks) {
        if (!b.size)
            continue; // Empty files use a sentinel page number in Cookfs 1.4.
        const auto &data = page(b.page);
        if (b.offset > data.size() || b.size > data.size() - b.offset)
            fail("Cookfs block outside decoded page");
        if (b.size)
            out.write(reinterpret_cast<const char *>(data.data() + b.offset), b.size);
        if (!out)
            fail("Cannot write extracted file: " + name);
        if (written)
            written(b.size);
    }
}
} // namespace ib
