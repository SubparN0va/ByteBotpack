#include "Package.h"
#include <fstream>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace {
    // Helper functions used to read bot.toml
    inline void ltrim(std::string& s) {
        size_t i = 0;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
        s.erase(0, i);
    }
    inline void rtrim(std::string& s) {
        while (!s.empty()) {
            char c = s.back();
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') s.pop_back();
            else break;
        }
    }
    inline void trim(std::string& s) { ltrim(s); rtrim(s); }

    inline bool starts_with(const std::string& s, const char* prefix) {
        const size_t n = std::char_traits<char>::length(prefix);
        return s.size() >= n && s.compare(0, n, prefix) == 0;
    }

}

// Reads [settings] agent_id = "..." from bot.toml
std::optional<std::string> ReadAgentIdFromBotToml(const std::filesystem::path& botTomlPath)
{
    std::ifstream f(botTomlPath);
    if (!f.is_open())
        return std::nullopt;

    bool inSettings = false;
    std::string line;

    while (std::getline(f, line)) {
        // Strip TOML comments (# ...)
        if (auto hash = line.find('#'); hash != std::string::npos)
            line.erase(hash);

        trim(line);
        if (line.empty()) continue;

        // Section headers
        if (line.front() == '[' && line.back() == ']') {
            inSettings = (line == "[settings]");
            continue;
        }

        if (!inSettings) continue;

        // Look for agent_id = "..."
        // Check key at start after trimming.
        if (starts_with(line, "agent_id")) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;

            std::string rhs = line.substr(eq + 1);
            trim(rhs);

            if (rhs.empty()) continue;

            char quote = rhs.front();
            if (quote != '"' && quote != '\'') continue;

            auto endq = rhs.find(quote, 1);
            if (endq == std::string::npos) continue;

            return rhs.substr(1, endq - 1);
        }
    }

    return std::nullopt;
}

std::filesystem::path ExecutableDirectory(const char* argumentZero) {
    std::filesystem::path executable;
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!size || size >= buffer.size()) throw std::runtime_error("Cannot locate the Byte executable");
    buffer.resize(size);
    executable = buffer;
#else
    std::error_code error;
    executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error) executable = argumentZero ? std::filesystem::absolute(argumentZero) : std::filesystem::current_path() / "GGLBot";
#endif
    return executable.parent_path();
}

std::filesystem::path PackageDirectory(const char* argumentZero) {
    auto directory = ExecutableDirectory(argumentZero);
    if (directory.filename() == "000-runtime") directory = directory.parent_path();
    // bob writes its authoritative configuration at the shared package root,
    // one level above the platform-specific launcher. Local builds are flat.
    if (!std::filesystem::is_regular_file(directory / "bot.toml") &&
        std::filesystem::is_regular_file(directory.parent_path() / "bot.toml"))
        directory = directory.parent_path();
    return directory;
}
