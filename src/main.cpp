#include "ib.hpp"
#include "progress.hpp"
#include "version.h"
#include <iostream>
#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <shellapi.h>
// clang-format on
#endif

static void usage() {
    std::cout << "Usage: ib-unpack [-y] [--no-progress] [-p PASSWORD] INSTALLER [DESTINATION]\n"
                 "       ib-unpack -v | --version\n";
}
int main(int argc, char **argv) {
    try {
#ifdef _WIN32
        int wide_count = 0;
        auto wide = CommandLineToArgvW(GetCommandLineW(), &wide_count);
        if (!wide)
            ib::fail("Cannot read command-line arguments");
        struct FreeArguments {
            LPWSTR *value;
            ~FreeArguments() { LocalFree(value); }
        } free_arguments{wide};
        std::vector<std::string> arguments;
        for (int i = 0; i < wide_count; ++i) {
            int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, nullptr, 0,
                                           nullptr, nullptr);
            if (!size)
                ib::fail("Cannot convert command-line argument to UTF-8");
            std::string argument(size, '\0');
            if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, argument.data(),
                                     size, nullptr, nullptr))
                ib::fail("Cannot convert command-line argument to UTF-8");
            argument.pop_back();
            arguments.push_back(std::move(argument));
        }
        std::vector<char *> pointers;
        for (auto &argument : arguments)
            pointers.push_back(argument.data());
        argc = wide_count;
        argv = pointers.data();
#endif
        ib::Options options;
        std::vector<std::string> positional;
        bool flags = true;
        bool show_progress = ProgressBar::terminal();
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (flags && a == "--")
                flags = false;
            else if (flags && (a == "-h" || a == "--help")) {
                usage();
                return 0;
            } else if (flags && (a == "--version" || a == "-v" || a == "-V")) {
                std::cout << "ib-unpack " << IB_VERSION << "\n";
                return 0;
            } else if (flags && (a == "-y" || a == "--yes"))
                options.overwrite = true;
            else if (flags && a == "--no-progress")
                show_progress = false;
            else if (flags && (a == "-p" || a == "--password")) {
                if (++i == argc)
                    ib::fail("Missing password argument");
                options.password = argv[i];
            } else if (flags && !a.empty() && a[0] == '-')
                ib::fail("Unknown option: " + a);
            else
                positional.push_back(a);
        }
        if (positional.empty() || positional.size() > 2) {
            usage();
            return 2;
        }
        options.source = ib::fs::u8path(positional[0]);
        options.destination = positional.size() == 2 ? ib::fs::u8path(positional[1])
                                                     : ib::fs::u8path(positional[0] + ".unpacked");
        std::cout << "Extracting " << options.source.u8string() << " to "
                  << options.destination.u8string() << std::endl;
        ProgressBar bar(show_progress, std::cerr);
        if (show_progress)
            options.progress = [&](const ib::Progress &progress) { bar.update(progress); };
        auto stats = ib::extract(options);
        std::cout << "Done: " << stats.files << " files, " << stats.directories << " directories, "
                  << stats.links << " links, " << stats.bytes << " bytes.\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Error: " << error.what() << "\n";
        return 1;
    }
}
