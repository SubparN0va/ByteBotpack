#pragma once

#include "SkillSettings.h"

#include <filesystem>
#include <optional>
#include <string>
#include <map>

/** Package-local preference; missing or invalid files have no saved selection. **/
std::optional<int> ReadSavedSkill(const std::filesystem::path& package);
/** Restores both selectors, or migrates a legacy rank; a fresh package uses Megabyte. **/
SkillChoice ReadSavedChoice(const std::filesystem::path& package);
std::string SaveSkillChoice(const std::filesystem::path& package, SkillChoice choice);
/** Bracketed custom-rank name; legacy SSL+ is retained for reading old matches. **/
std::string SkillBotName(int level);
/** Preset names have no rank suffix; Select Skill uses Byte [rank]. **/
std::string ChoiceBotName(SkillChoice choice);
/** Matches an exact name or the duplicate suffix added by RLBot. **/
bool BotNameMatches(const std::string& actual, const std::string& expected);
/** Recognizes preset and rank names, including RLBot duplicate-name suffixes. **/
std::optional<int> SkillFromBotName(const std::string& name);
/** Retains policy identity when a full-strength preset and custom rank share a name. **/
SkillChoice ChoiceForBot(const std::string& name, const std::string& runCommand, SkillChoice fallback);
/** Identifies explicit manual launches so shared GUI controls leave them alone. **/
bool IsManualRunCommand(const std::string& runCommand);
/** Adds one model-mode marker while preserving the rest of the launch command. **/
std::string RunCommandWithChoice(const std::string& runCommand, SkillChoice choice);
/** Changes only [settings].name, preserving comments, other fields and file endings. **/
std::string ConfigWithSkillName(const std::string& config, int level);
std::string ConfigWithSkillName(const std::string& config, SkillChoice choice);
/** Updates only the configured name for a full-strength manual model. **/
std::string SyncManualModelName(const std::filesystem::path& package, SkillChoiceMode model);
/** Saves the selection and next-match name; returns an error message on failure. **/
std::string SaveSkillSelection(const std::filesystem::path& package, int level);
/** Per-slot/team choices return as pending selections in a newly configured match. **/
using SavedBotSkills = std::map<std::pair<unsigned, unsigned>, SkillChoice>;
SavedBotSkills ReadSavedBotSkills(const std::filesystem::path& package);
std::string SaveBotSkills(const std::filesystem::path& package, const SavedBotSkills& levels);
