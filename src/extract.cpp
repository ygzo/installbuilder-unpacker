#include "ib.hpp"
#include "platform.hpp"
#include <algorithm>
#include <set>

namespace ib {
struct ManifestItem {
    std::string name, type, arg;
    uint64_t mtime;
    size_t parts;
};
Stats extract(const Options &options) {
    auto source = fs::absolute(options.source).lexically_normal();
    if (fs::is_directory(source))
        source /= fs::path("Contents/Resources/installbuilder");
    if (!fs::is_regular_file(source))
        fail("Installer file not found: " + source.u8string());
    auto destination = fs::absolute(options.destination).lexically_normal();
    if (destination == destination.root_path() || destination == fs::current_path() ||
        destination.filename().empty())
        fail("Unsafe destination directory");
    auto canonical_source = fs::canonical(source);
    auto canonical_destination = fs::weakly_canonical(destination);
    for (auto p = canonical_source; !p.empty(); p = p.parent_path()) {
        if (p == canonical_destination)
            fail("Destination contains the source installer");
        if (p == p.root_path())
            break;
    }
    if (fs::is_symlink(fs::symlink_status(destination)))
        fail("Destination must not be a symbolic link");
    if (fs::exists(destination) && !options.overwrite)
        fail("Destination exists (use -y to overwrite): " + destination.u8string());
    File file(source);
    auto metadata = read_metakit(file);
    Cookfs cookfs(file, metadata.cookfsinfo, options.password);
    auto list = tcl_list(metadata.manifest);
    if (list.size() % 2)
        fail("Manifest has an unmatched path");
    std::vector<ManifestItem> items;
    std::set<std::string> paths, links;
    for (size_t i = 0; i < list.size(); i += 2) {
        auto path = safe_relative(list[i]);
        auto props = tcl_list(list[i + 1]);
        if (props.size() < 3)
            fail("Incomplete manifest properties");
        if (props[0] != "file" && props[0] != "directory" && props[0] != "link")
            fail("Unsupported manifest entry type: " + props[0]);
        std::string name = path.generic_u8string();
        if (!paths.insert(name).second)
            fail("Duplicate manifest path: " + name);
        if (props[0] == "link") {
            if (props[1].find('\0') != std::string::npos)
                fail("Invalid symlink target");
            links.insert(name);
        }
        size_t parts = props.size() > 4 ? tcl_list(props[4]).size() : 0;
        items.push_back({name, props[0], props[1], decimal(props[2]), parts});
    }
    for (const auto &name : paths)
        for (auto p = fs::u8path(name).parent_path(); !p.empty(); p = p.parent_path())
            if (links.count(p.generic_u8string()))
                fail("Manifest writes through a symbolic link: " + name);
    Progress progress;
    std::vector<uint32_t> page_plan;
    progress.total_entries = items.size();
    auto report = [&] {
        if (options.progress)
            options.progress(progress);
    };
    auto count_bytes = [&](const std::string &name) {
        auto entry = cookfs.entries.find(name);
        if (entry == cookfs.entries.end())
            fail("Payload file not found: " + name);
        for (const auto &block : entry->second.blocks) {
            if (block.size)
                page_plan.push_back(block.page);
            if (block.size > UINT64_MAX - progress.total_bytes)
                fail("Payload size exceeds supported range");
            progress.total_bytes += block.size;
        }
    };
    for (const auto &item : items)
        if (item.type == "file") {
            count_bytes(item.name);
            for (size_t j = 1; j < item.parts; ++j)
                count_bytes(item.name + "___bitrockBigFile" + std::to_string(j));
        }
    report();
    cookfs.prepare(page_plan);
    std::function<void(uint64_t)> written;
    if (options.progress)
        written = [&](uint64_t bytes) {
            progress.bytes += bytes;
            report();
        };
    fs::create_directories(destination.parent_path());
    auto staging = create_staging(destination.parent_path());
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            std::error_code ec;
            remove_tree(path, ec);
        }
    } cleanup{staging};
    for (const auto &pair : metadata.extras) {
        auto p = staging / safe_relative(pair.first);
        fs::create_directories(p.parent_path());
        std::ofstream out(p, std::ios::binary);
        if (!pair.second.empty())
            out.write(reinterpret_cast<const char *>(pair.second.data()), pair.second.size());
        if (!out)
            fail("Cannot write metadata file");
    }
    Stats stats;
    auto root = staging / "components";
    for (const auto &item : items) {
        auto p = root / fs::u8path(item.name);
        if (item.type == "directory") {
            fs::create_directories(p);
            ++stats.directories;
        } else if (item.type == "file") {
            fs::create_directories(p.parent_path());
            std::ofstream out(p, std::ios::binary);
            if (!out)
                fail("Cannot create extracted file: " + item.name);
            cookfs.write(item.name, out, written);
            for (size_t j = 1; j < item.parts; ++j)
                cookfs.write(item.name + "___bitrockBigFile" + std::to_string(j), out, written);
            out.close();
            if (!out)
                fail("Cannot finish extracted file: " + item.name);
            set_mode(p, item.arg);
            set_timestamp(p, item.mtime);
            stats.bytes += fs::file_size(p);
            ++stats.files;
        }
        if (item.type != "link") {
            ++progress.entries;
            report();
        }
    }
    for (const auto &item : items)
        if (item.type == "link") {
            auto p = root / fs::u8path(item.name);
            fs::create_directories(p.parent_path());
            create_link(p, item.arg);
            set_timestamp(p, item.mtime, true);
            ++stats.links;
            ++progress.entries;
            report();
        }
    // Set child directories before parents, after all writes and links are complete.
    std::sort(items.begin(), items.end(),
              [](const auto &a, const auto &b) { return a.name.size() > b.name.size(); });
    for (const auto &item : items)
        if (item.type == "directory") {
            auto p = root / fs::u8path(item.name);
            set_timestamp(p, item.mtime);
            set_mode(p, item.arg);
        }
    if (fs::exists(destination)) {
        auto backup = staging;
        backup += ".previous";
        fs::rename(destination, backup);
        try {
            fs::rename(staging, destination);
        } catch (...) {
            fs::rename(backup, destination);
            throw;
        }
        std::error_code ec;
        remove_tree(backup, ec);
        if (ec)
            fail("Extraction completed, but cannot remove previous output: " + backup.u8string());
    } else
        fs::rename(staging, destination);
    progress.finished = true;
    report();
    return stats;
}
} // namespace ib
