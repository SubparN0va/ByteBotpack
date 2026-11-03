#include "SkillPersistence.h"
#include "SkillSettings.h"

#include <charconv>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <regex>
#include <stdexcept>
#include <sstream>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace {
    // Team matches can start several processes from the same package. Serialize
    // preference reads/writes across them without creating disposable lock files.
    class SelectionLock {
#ifdef _WIN32
        HANDLE handle = nullptr;
#else
        int handle = -1;
#endif
    public:
        explicit SelectionLock(const std::filesystem::path& package) {
#ifdef _WIN32
            const auto path = std::filesystem::weakly_canonical(package).wstring();
            uint64_t hash = 14695981039346656037ull;
            for (auto ch : path) { hash ^= uint64_t(std::towlower(ch)); hash *= 1099511628211ull; }
            const auto name = L"Local\\GGLBotSkill-" + std::to_wstring(hash);
            handle = CreateMutexW(nullptr, FALSE, name.c_str());
            if (!handle) throw std::runtime_error("Cannot lock skill preferences");
            const auto status = WaitForSingleObject(handle, 5000);
            if (status != WAIT_OBJECT_0 && status != WAIT_ABANDONED) {
                CloseHandle(handle); handle = nullptr;
                throw std::runtime_error("Skill preferences are busy");
            }
#else
            handle = open(package.c_str(), O_RDONLY);
            if (handle < 0) throw std::runtime_error("Cannot open skill preference directory");
            if (flock(handle, LOCK_EX) != 0) {
                close(handle); handle = -1;
                throw std::runtime_error("Cannot lock skill preferences");
            }
#endif
        }
        ~SelectionLock() {
#ifdef _WIN32
            if (handle) { ReleaseMutex(handle); CloseHandle(handle); }
#else
            if (handle >= 0) { flock(handle, LOCK_UN); close(handle); }
#endif
        }
        SelectionLock(const SelectionLock&) = delete;
        SelectionLock& operator=(const SelectionLock&) = delete;
    };

    std::string ReadText(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("Cannot read " + path.string());
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    void WriteText(const std::filesystem::path& path, const std::string& text) {
        if (std::filesystem::exists(path) && ReadText(path) == text) return;
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << text;
        output.close();
        if (!output) throw std::runtime_error("Cannot save " + path.string());
    }
    std::optional<int> ReadSavedSkillFile(const std::filesystem::path& package) {
        const auto text = ReadText(package / "skill-level.txt");
        const auto first = text.find_first_not_of(" \t\r\n");
        const auto last = text.find_last_not_of(" \t\r\n");
        if (first == std::string::npos) return {};
        int level = -1;
        const auto end = text.data() + last + 1;
        const auto result = std::from_chars(text.data() + first, end, level);
        if (result.ec != std::errc{} || result.ptr != end || level < 0 || level > MaxSkillLevel) return {};
        return level;
    }
}

std::optional<int> ReadSavedSkill(const std::filesystem::path& package) {
    try {
        SelectionLock lock(package);
        return ReadSavedSkillFile(package);
    } catch (const std::exception&) { return {}; }
}

std::string SkillBotName(int level) {
    if (level == 0) return "Byte [Auto]";
    if (level == 21) return "Byte [GC III]";
    if (level == 22) return "Byte [SSL]";
    if (level == 23) return "Byte [SSL+]";
    return "Byte [" + SkillLabel(level) + "]";
}

std::string ChoiceBotName(SkillChoice choice) {
    const int level=choice.Level();
    return choice.mode==SkillChoiceMode::SelectSkill?SkillBotName(level):SkillChoiceLabel(choice.mode);
}

bool BotNameMatches(const std::string& actual,const std::string& expected) {
    if(actual==expected) return true;
    const auto prefix=expected+" (";
    if(!actual.starts_with(prefix) || actual.back()!=')') return false;
    unsigned duplicate=0;
    const auto end=actual.data()+actual.size()-1;
    const auto result=std::from_chars(actual.data()+prefix.size(),end,duplicate);
    return result.ec==std::errc{} && result.ptr==end && duplicate>=2;
}

std::optional<int> SkillFromBotName(const std::string& name) {
    for(int mode=0;mode<3;++mode)
        if(BotNameMatches(name,SkillChoiceLabel(static_cast<SkillChoiceMode>(mode)))) return 21+mode;
    // RLBot adds duplicate-name suffixes, e.g. Byte [Gold II] (2).
    const auto start = name.find('['), end = name.find(']', start);
    if (start == std::string::npos || end == std::string::npos) return {};
    const auto rank = name.substr(start, end - start + 1);
    for (int level = 0; level <= MaxSkillLevel; ++level) {
        const auto expected = SkillBotName(level);
        if (expected.substr(expected.find('[')) == rank) return level;
    }
    return {};
}

bool IsManualRunCommand(const std::string& runCommand) {
    static const std::regex manual(R"arg((^|[ 	])"?--manual(?:=([^ 	"]*)"?|"?(?:[ 	]+"?([^ 	"]+)"?)?)($|[ 	]))arg");
    std::smatch match;
    if(!std::regex_search(runCommand,match,manual)) return false;
    auto model=match[2].matched?match.str(2):match.str(3);
    std::transform(model.begin(),model.end(),model.begin(),[](unsigned char c) {return char(std::tolower(c));});
    return model!="off";
}

SkillChoice ChoiceForBot(const std::string& name,const std::string& runCommand,SkillChoice fallback) {
    for(int mode=0;mode<3;++mode) {
        const auto preset=static_cast<SkillChoiceMode>(mode);
        if(BotNameMatches(name,SkillChoiceLabel(preset))) return {preset,fallback.rank};
    }
    const auto level=SkillFromBotName(name).value_or(fallback.Level());
    const std::regex marker(R"((?:^|\s)--byte-skill-mode=([0-3])(?=\s|$))");
    std::smatch match;
    if(std::regex_search(runCommand,match,marker)) {
        const auto mode=static_cast<SkillChoiceMode>(match.str(1)[0]-'0');
        SkillChoice choice{mode,mode==SkillChoiceMode::SelectSkill?level:fallback.rank};
        if(choice.rank<=MaxSelectableRank && choice.Level()==level) return choice;
    }
    return fallback.Level()==level?fallback:SkillChoice::FromLevel(level);
}

std::string RunCommandWithChoice(const std::string& runCommand,SkillChoice choice) {
    choice.Level();
    const std::regex marker(R"((?:^|\s)--byte-skill-mode=[0-3](?=\s|$))");
    auto command=std::regex_replace(runCommand,marker,"");
    return command+" --byte-skill-mode="+std::to_string(int(choice.mode));
}

std::string ConfigWithSkillName(const std::string& config, int level) {
    return ConfigWithSkillName(config,SkillChoice::FromLevel(level));
}

std::string ConfigWithSkillName(const std::string& config, SkillChoice choice) {
    const auto name = ChoiceBotName(choice);
    const std::regex section(R"(^\s*\[settings\]\s*(#.*)?$)");
    const std::regex field(R"re(^([ \t]*name[ \t]*=[ \t]*)("([^"\\]|\\.)*"|'[^']*')([ \t]*(#.*)?\r?$))re");
    bool settings = false;
    size_t valueStart = std::string::npos, valueSize = 0;
    for (size_t start = 0; start < config.size();) {
        const auto end = config.find('\n', start);
        const auto line = config.substr(start, end == std::string::npos ? end : end - start);
        const auto first = line.find_first_not_of(" \t\r");
        if (first != std::string::npos && line[first] == '[')
            settings = std::regex_match(line, section);
        std::smatch match;
        if (settings && std::regex_match(line, match, field)) {
            if (valueStart != std::string::npos) throw std::runtime_error("Duplicate [settings].name in bot.toml");
            valueStart = start + match.position(2); valueSize = match.length(2);
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (valueStart == std::string::npos) throw std::runtime_error("Cannot find [settings].name in bot.toml");
    auto result = config;
    result.replace(valueStart, valueSize, '"' + name + '"');
    return result;
}

SkillChoice ReadSavedChoice(const std::filesystem::path& package) {
    int level=DefaultSkillLevel;
    try {
        SelectionLock lock(package);
        // Both files describe one choice. Releasing the lock between reads can
        // combine an old level with a different process's newly saved model.
        try { level=ReadSavedSkillFile(package).value_or(DefaultSkillLevel); }
        catch(const std::exception&) {}
        std::istringstream input(ReadText(package/"skill-choice.txt"));
        int mode,rank;
        if(input>>mode>>rank && (input>>std::ws).eof()) {
            const int previousRank=rank;
            if(rank==23) rank=MaxSelectableRank;
            SkillChoice choice{static_cast<SkillChoiceMode>(mode),rank};
            // Legacy launchers may update only skill-level.txt. Never restore
            // selector metadata that disagrees with the authoritative level.
            if(choice.Level()==level || (mode==int(SkillChoiceMode::SelectSkill) && previousRank==23 && level==23)) return choice;
        }
    } catch(const std::exception&) {}
    return SkillChoice::FromLevel(level);
}

std::string SyncManualModelName(const std::filesystem::path& package, SkillChoiceMode model) {
    try {
        if(model!=SkillChoiceMode::Byte && model!=SkillChoiceMode::Kilobyte && model!=SkillChoiceMode::Megabyte)
            throw std::invalid_argument("Manual mode requires a named model");
        SelectionLock lock(package);
        const auto path=package/"bot.toml";
        // Preserve GUI preferences and avoid rewriting an already matching file.
        WriteText(path,ConfigWithSkillName(ReadText(path),SkillChoice{model}));
    } catch(const std::exception& error) {return error.what();}
    return {};
}

std::string SaveSkillSelection(const std::filesystem::path& package, int level) {
    try {return SaveSkillChoice(package,SkillChoice::FromLevel(level));}
    catch(const std::exception& error) {return error.what();}
}

std::string SaveSkillChoice(const std::filesystem::path& package, SkillChoice choice) {
    try {
        const auto level=choice.Level();
        SelectionLock lock(package);
        // Only explicit choices are persisted. Auto's changing effective level
        // never reaches this function, so an Auto selection stays Auto next match.
        ChoiceBotName(choice);
        WriteText(package / "skill-level.txt", std::to_string(level));
        WriteText(package / "skill-choice.txt", std::to_string(int(choice.mode))+" "+std::to_string(choice.rank));
        try {
            const auto path = package / "bot.toml";
            WriteText(path, ConfigWithSkillName(ReadText(path), choice));
        } catch (const std::exception& error) {
            return "Skill saved, but next-match name was not updated: " + std::string(error.what());
        }
    } catch (const std::exception& error) { return error.what(); }
    return {};
}

SavedBotSkills ReadSavedBotSkills(const std::filesystem::path& package) {
    try {
        SelectionLock lock(package);std::istringstream input(ReadText(package/"skill-roster.txt"));
        std::string header;input>>header;
        const bool selectors=header=="BYTE_SKILL_ROSTER_2";
        if(!selectors) {input.clear();input.seekg(0);}
        SavedBotSkills levels;
        while(input>>std::ws && !input.eof()) {
            unsigned slot,team;int value;
            if(!(input>>slot>>team>>value) || slot>63 || team>1) return {};
            SkillChoice choice;
            if(selectors) {
                int rank;if(!(input>>rank)) return {};
                if(rank==23) rank=MaxSelectableRank;
                choice={static_cast<SkillChoiceMode>(value),rank};
            } else choice=SkillChoice::FromLevel(value);
            choice.Level();
            if(!levels.emplace(std::make_pair(slot,team),choice).second) return {};
        }
        return levels;
    } catch(const std::exception&) {return {};}
}

std::string SaveBotSkills(const std::filesystem::path& package,const SavedBotSkills& levels) {
    try {
        SelectionLock lock(package);std::ostringstream text;text<<"BYTE_SKILL_ROSTER_2\n";
        for(const auto& [key,choice]:levels) {
            choice.Level();
            if(key.first>63 || key.second>1) throw std::invalid_argument("Invalid per-bot rank");
            text<<key.first<<' '<<key.second<<' '<<int(choice.mode)<<' '<<choice.rank<<'\n';
        }
        WriteText(package/"skill-roster.txt",text.str());return {};
    } catch(const std::exception& error) {return error.what();}
}
