#pragma once

#include "SkillSettings.h"

#include <filesystem>
#include <optional>
#include <string>
#include <map>

/** Package-local preference; missing or invalid files have no saved selection. **/
std::optional<int> ReadSavedSkill(const std::filesystem::path& package);
/** Restores a Kilobyte rank; a fresh package starts with the default reduced strength. **/
SkillChoice ReadSavedChoice(const std::filesystem::path& package);
std::string SaveSkillChoice(const std::filesystem::path& package, SkillChoice choice);
/** Kilobyte with a bracketed rank, or plain Kilobyte at SSL. **/
std::string SkillBotName(int level);
/** Megabyte has a fixed name; Kilobyte names follow the selected rank. **/
std::string ChoiceBotName(SkillChoice choice);
/** Matches an exact name or the duplicate suffix added by RLBot. **/
bool BotNameMatches(const std::string& actual, const std::string& expected);
/** Recognizes Kilobyte rank names, including RLBot duplicate-name suffixes. **/
std::optional<int> SkillFromBotName(const std::string& name);
/** Explicit startup ranks override names; Apply & respawn's per-car choice takes precedence. **/
SkillChoice ChoiceForBot(const std::string& name, const StartupOptions& startup, SkillChoice fallback);
/** Identifies explicit manual launches so shared GUI controls leave them alone. **/
bool IsManualRunCommand(const std::string& runCommand);
/** Adds the Kilobyte rank-mode marker while preserving the rest of the launch command. **/
std::string RunCommandWithChoice(const std::string& runCommand, SkillChoice choice);
/** Changes only [settings].name, preserving comments, other fields and file endings. **/
std::string ConfigWithSkillName(const std::string& config, int level);
std::string ConfigWithSkillName(const std::string& config, SkillChoice choice);
/** Saves the selection and next-match name; returns an error message on failure. **/
std::string SaveSkillSelection(const std::filesystem::path& package, int level);
/** Per-slot/team choices return as pending selections in a newly configured match. **/
using SavedBotSkills = std::map<std::pair<unsigned, unsigned>, SkillChoice>;
SavedBotSkills ReadSavedBotSkills(const std::filesystem::path& package);
std::string SaveBotSkills(const std::filesystem::path& package, const SavedBotSkills& levels);
