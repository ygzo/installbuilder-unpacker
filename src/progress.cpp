#include "progress.hpp"
#include <algorithm>
#include <cstdio>
#include <iomanip>
#include <sstream>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

ProgressBar::ProgressBar(bool enabled, std::ostream &output) : enabled_(enabled), output_(output) {}

ProgressBar::~ProgressBar() {
    if (active_)
        output_ << '\n' << std::flush;
}

bool ProgressBar::terminal() {
#ifdef _WIN32
    return _isatty(_fileno(stderr)) != 0;
#else
    return isatty(fileno(stderr)) != 0;
#endif
}

void ProgressBar::update(const ib::Progress &progress) {
    if (!enabled_)
        return;
    auto now = std::chrono::steady_clock::now();
    if (active_ && !progress.finished && now - last_ < std::chrono::milliseconds(100))
        return;
    last_ = now;
    uint64_t completed = progress.total_bytes ? progress.bytes : progress.entries;
    uint64_t total = progress.total_bytes ? progress.total_bytes : progress.total_entries;
    unsigned percent = total ? static_cast<unsigned>(100.0L * completed / total) : 0;
    percent = progress.finished ? 100 : std::min(percent, 99u);
    constexpr unsigned width = 24;
    unsigned filled = percent * width / 100;
    std::ostringstream line;
    line << "\rUnpacking [" << std::string(filled, '=') << std::string(width - filled, ' ') << "] "
         << std::setw(3) << percent << "%  " << progress.entries << '/' << progress.total_entries
         << " entries";
    if (progress.total_bytes)
        line << "  " << std::fixed << std::setprecision(1) << progress.bytes / (1024.0 * 1024.0)
             << '/' << progress.total_bytes / (1024.0 * 1024.0) << " MiB";
    output_ << line.str();
    if (progress.finished)
        output_ << '\n';
    output_.flush();
    active_ = !progress.finished;
}
