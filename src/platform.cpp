#include "platform.hpp"
#include <cerrno>
#include <cstring>
#include <limits>
#ifdef _WIN32
#include <random>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ib {
#ifdef _WIN32
static void windows_error(const std::string &action, const fs::path &path, DWORD error) {
    fail(action + ": " + path.u8string() + ": " +
         std::system_category().message(static_cast<int>(error)));
}
#endif

void set_timestamp(const fs::path &path, uint64_t time, bool link) {
#ifdef _WIN32
    constexpr uint64_t epoch = 11644473600ULL, ticks_per_second = 10000000ULL;
    if (time > uint64_t(INT64_MAX) / ticks_per_second - epoch)
        fail("Timestamp outside supported range");
    uint64_t ticks = (time + epoch) * ticks_per_second;
    FILETIME value{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
    DWORD flags = FILE_FLAG_BACKUP_SEMANTICS;
    if (link)
        flags |= FILE_FLAG_OPEN_REPARSE_POINT;
    HANDLE handle = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, flags, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        windows_error("Cannot open file to set timestamp", path, GetLastError());
    BOOL result = SetFileTime(handle, nullptr, &value, &value);
    DWORD error = GetLastError();
    CloseHandle(handle);
    if (!result)
        windows_error("Cannot set timestamp", path, error);
#else
    if (time > static_cast<uint64_t>(std::numeric_limits<time_t>::max()))
        fail("Timestamp outside supported range");
    timespec values[2]{{static_cast<time_t>(time), 0}, {static_cast<time_t>(time), 0}};
    if (utimensat(AT_FDCWD, path.c_str(), values, link ? AT_SYMLINK_NOFOLLOW : 0))
        fail("Cannot set timestamp: " + path.u8string() + ": " + std::strerror(errno));
#endif
}

void set_mode(const fs::path &path, const std::string &input) {
    unsigned permissions = 0;
    if (input.empty())
        fail("Missing file mode");
    for (char c : input) {
        if (c < '0' || c > '7' || permissions > 0177777)
            fail("Invalid octal file mode");
        permissions = permissions * 8 + (c - '0');
    }
    if (permissions > 0177777)
        fail("Invalid octal file mode");
#ifdef _WIN32
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        windows_error("Cannot read file attributes", path, GetLastError());
    // Windows directories do not use the read-only attribute as a Unix mode.
    if (attributes & FILE_ATTRIBUTE_DIRECTORY)
        return;
    if (permissions & 0222)
        attributes &= ~FILE_ATTRIBUTE_READONLY;
    else
        attributes |= FILE_ATTRIBUTE_READONLY;
    if (!attributes)
        attributes = FILE_ATTRIBUTE_NORMAL;
    if (!SetFileAttributesW(path.c_str(), attributes))
        windows_error("Cannot set permissions", path, GetLastError());
#else
    if (chmod(path.c_str(), permissions & 07777))
        fail("Cannot set permissions: " + path.u8string());
#endif
}

fs::path create_staging(const fs::path &parent) {
#ifdef _WIN32
    std::random_device random;
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        auto path =
            parent / (".ib-unpack-" + std::to_string(random()) + "-" + std::to_string(random()));
        if (CreateDirectoryW(path.c_str(), nullptr))
            return path;
        DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS)
            windows_error("Cannot create staging directory", path, error);
    }
    fail("Cannot create unique staging directory");
#else
    std::string pattern = (parent / ".ib-unpack-XXXXXX").string();
    std::vector<char> temp(pattern.begin(), pattern.end());
    temp.push_back(0);
    if (!mkdtemp(temp.data()))
        fail("Cannot create staging directory");
    return fs::path(temp.data());
#endif
}

void create_link(const fs::path &path, const std::string &target) {
    auto target_path = fs::u8path(target);
#ifdef _WIN32
    DWORD flags =
        fs::is_directory(path.parent_path() / target_path) ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0;
    if (!CreateSymbolicLinkW(path.c_str(), target_path.c_str(),
                             flags | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        DWORD error = GetLastError();
        if (error == ERROR_PRIVILEGE_NOT_HELD)
            fail("Cannot create symbolic link: " + path.u8string() +
                 ". Enable Windows Developer Mode or run as administrator.");
        windows_error("Cannot create symbolic link", path, error);
    }
#else
    fs::create_symlink(target_path, path);
#endif
}

void remove_tree(const fs::path &path, std::error_code &error) {
#ifdef _WIN32
    // Read-only payload files must be writable before Windows can remove them.
    if (fs::exists(path, error)) {
        fs::recursive_directory_iterator iterator(path, error), end;
        while (!error && iterator != end) {
            const auto &entry = *iterator;
            auto status = entry.symlink_status(error);
            if (!error && fs::is_regular_file(status))
                fs::permissions(entry.path(), fs::perms::owner_write, fs::perm_options::add, error);
            if (!error)
                iterator.increment(error);
        }
        if (error)
            return;
    }
#endif
    fs::remove_all(path, error);
}
} // namespace ib
