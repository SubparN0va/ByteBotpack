#pragma once

#include <GigaLearnCPP/ActionSamplingConfig.h>
#include <string>
#include <optional>

inline constexpr int MaxSkillLevel = 22;
inline constexpr int MaxSelectableRank = 22;
inline constexpr int DefaultSkillLevel = 16;

/** Megabyte is a package identity; only SelectSkill is exposed by the Kilobyte GUI. **/
enum class SkillChoiceMode { Megabyte = 2, SelectSkill = 3 };
struct SkillChoice {
    SkillChoiceMode mode = SkillChoiceMode::SelectSkill;
    int rank = DefaultSkillLevel;
    /** Returns the selected Kilobyte rank, or the full-strength endpoint for Megabyte. **/
    int Level() const;
    static SkillChoice FromLevel(int level);
    bool operator==(const SkillChoice&) const = default;
};
const char* SkillChoiceLabel(SkillChoiceMode mode);

enum class SkillModel { Megabyte, Kilobyte };
/** Display name for each packaged policy. **/
const char* SkillModelName(SkillModel model);

struct SkillSettings {
    SkillModel model = SkillModel::Kilobyte;
    bool deterministic = false;
    int decisionTicks = 8;
    GGL::ActionSamplingConfig sampling;
};

struct StartupOptions {
    int skillLevel = DefaultSkillLevel;
    bool explicitSkill = false;
    /** Apply & respawn stores a per-car choice in the match name. **/
    bool matchRank = false;
    /** Set by the Megabyte package profile, never by the rank selector. **/
    std::optional<SkillChoiceMode> manualModel;
    bool showHelp = false;
    bool showUi = true;
    bool debug = false;
    bool uiOnly = false;
    bool checkModels = false;
};

/** 0 selects Auto; 1..21 cover Bronze I through GC III; 22 is full-strength Kilobyte (SSL). **/
std::string SkillLabel(int level);
/** Returns the provisional rank profile for an effective, non-Auto level. **/
SkillSettings SettingsForSkill(float level, int teamSize = 1);
/** Full-strength presets bypass rank weakening; Select Skill always uses Kilobyte. **/
SkillSettings SettingsForChoice(SkillChoice choice, float effectiveLevel, int teamSize = 1);
/** Restricts kickoff noise; Gold keeps a gentler guard than higher ranks. **/
SkillSettings GuardKickoff(SkillSettings settings, float effectiveLevel = 10.f);
/** CLI overrides GGLBOT_SKILL, then the saved choice; invalid explicit values throw. **/
StartupOptions ParseStartupOptions(int argc, const char* const* argv, const char* environmentSkill, int savedSkill = DefaultSkillLevel, bool fullStrengthMegabyte = false);

/** Parses the executable and quoted arguments from an RLBot launch command. **/
StartupOptions ParseRunCommandOptions(const std::string& command, const char* environmentSkill, int savedSkill = DefaultSkillLevel);

/** Score feedback is local to each bot/team and never changes the user's selection. **/
class AutoSkillController {
public:
    float Update(int selectedLevel, int teamScore, int opponentScore, float gameSeconds, int teamSize);
    float EffectiveLevel() const { return level; }
private:
    float level = 14.f;
    float lastSeconds = -1.f;
    int lastTeamScore = -1, lastOpponentScore = -1;
    int lastSelection = 0, lastMode = 0;
    int direction = 0;
    float step = 1.f;
};
