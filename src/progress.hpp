#pragma once
#include "ib.hpp"
#include <chrono>
#include <ostream>

class ProgressBar {
    bool enabled_, active_ = false;
    std::ostream &output_;
    std::chrono::steady_clock::time_point last_{};

  public:
    explicit ProgressBar(bool enabled, std::ostream &output);
    ~ProgressBar();
    void update(const ib::Progress &progress);
    static bool terminal();
};
