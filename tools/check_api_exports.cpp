#include <cctype>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

struct ManifestEntry {
    std::size_t line_number;
    std::string target;
    std::string symbol;
};

std::string trim(const std::string& value) {
    std::size_t first = 0;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first])) != 0) {
        ++first;
    }

    std::size_t last = value.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(value[last - 1])) != 0) {
        --last;
    }

    return value.substr(first, last - first);
}

std::string library_name_for_target(const std::string& target) {
#ifdef _WIN32
    return target + ".dll";
#elif defined(__APPLE__)
    return "lib" + target + ".dylib";
#else
    return "lib" + target + ".so";
#endif
}

std::string join_path(const std::string& directory, const std::string& file_name) {
    if (directory.empty()) {
        return file_name;
    }

    const char last = directory[directory.size() - 1];
    if (last == '/' || last == '\\') {
        return directory + file_name;
    }

#ifdef _WIN32
    return directory + "\\" + file_name;
#else
    return directory + "/" + file_name;
#endif
}

bool read_manifest(const std::string& path, std::vector<ManifestEntry>* entries) {
    std::ifstream input(path.c_str());
    if (!input) {
        std::cerr << "error: failed to open manifest: " << path << '\n';
        return false;
    }

    bool ok = true;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;

        const std::size_t comment = line.find('#');
        if (comment != std::string::npos) {
            line.erase(comment);
        }

        line = trim(line);
        if (line.empty()) {
            continue;
        }

        const std::size_t separator = line.find(':');
        if (separator == std::string::npos ||
            line.find(':', separator + 1) != std::string::npos) {
            std::cerr << "error: invalid manifest line " << line_number
                      << ": expected <target>:<symbol>\n";
            ok = false;
            continue;
        }

        ManifestEntry entry;
        entry.line_number = line_number;
        entry.target = trim(line.substr(0, separator));
        entry.symbol = trim(line.substr(separator + 1));

        if (entry.target.empty() || entry.symbol.empty()) {
            std::cerr << "error: invalid manifest line " << line_number
                      << ": target and symbol must be non-empty\n";
            ok = false;
            continue;
        }

        entries->push_back(entry);
    }

    if (ok && entries->empty()) {
        std::cerr << "error: manifest contains no API export entries: " << path << '\n';
        return false;
    }

    return ok;
}

#ifdef _WIN32

std::string format_windows_error(DWORD error_code) {
    LPSTR message = nullptr;
    const DWORD size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error_code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&message),
        0,
        nullptr);

    if (size == 0 || message == nullptr) {
        return "Windows error " + std::to_string(error_code);
    }

    std::string result(message, size);
    LocalFree(message);
    return trim(result);
}

bool check_target_exports(const std::string& target,
                          const std::vector<ManifestEntry>& entries,
                          const std::string& library_dir) {
    const std::string path = join_path(library_dir, library_name_for_target(target));
    HMODULE library = LoadLibraryA(path.c_str());
    if (library == nullptr) {
        std::cerr << "error: failed to load library for target '" << target
                  << "': " << path << ": " << format_windows_error(GetLastError())
                  << '\n';
        return false;
    }

    bool ok = true;
    for (std::vector<ManifestEntry>::const_iterator it = entries.begin();
         it != entries.end();
         ++it) {
        FARPROC symbol = GetProcAddress(library, it->symbol.c_str());
        if (symbol == nullptr) {
            std::cerr << "error: missing export '" << it->symbol << "' in target '"
                      << target << "' (" << path << "), manifest line "
                      << it->line_number << '\n';
            ok = false;
        }
    }

    FreeLibrary(library);
    return ok;
}

#else

bool check_target_exports(const std::string& target,
                          const std::vector<ManifestEntry>& entries,
                          const std::string& library_dir) {
    const std::string path = join_path(library_dir, library_name_for_target(target));
    void* library = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (library == nullptr) {
        const char* error = dlerror();
        std::cerr << "error: failed to load library for target '" << target
                  << "': " << path << ": "
                  << (error != nullptr ? error : "unknown dlopen error") << '\n';
        return false;
    }

    bool ok = true;
    for (std::vector<ManifestEntry>::const_iterator it = entries.begin();
         it != entries.end();
         ++it) {
        dlerror();
        (void)dlsym(library, it->symbol.c_str());
        const char* error = dlerror();
        if (error != nullptr) {
            std::cerr << "error: missing export '" << it->symbol << "' in target '"
                      << target << "' (" << path << "), manifest line "
                      << it->line_number << ": " << error << '\n';
            ok = false;
        }
    }

    dlclose(library);
    return ok;
}

#endif

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: check_api_exports <manifest> <library_dir>\n";
        return 2;
    }

    std::vector<ManifestEntry> entries;
    if (!read_manifest(argv[1], &entries)) {
        return 1;
    }

    std::map<std::string, std::vector<ManifestEntry> > entries_by_target;
    std::vector<std::string> target_order;
    for (std::vector<ManifestEntry>::const_iterator it = entries.begin();
         it != entries.end();
         ++it) {
        if (entries_by_target.find(it->target) == entries_by_target.end()) {
            target_order.push_back(it->target);
        }
        entries_by_target[it->target].push_back(*it);
    }

    bool ok = true;
    for (std::vector<std::string>::const_iterator it = target_order.begin();
         it != target_order.end();
         ++it) {
        if (!check_target_exports(*it, entries_by_target[*it], argv[2])) {
            ok = false;
        }
    }

    return ok ? 0 : 1;
}
