#pragma once

#include <filesystem>
#include <optional>
#include <string>

/** Finds the bot package beside the launcher, independent of the working directory. **/
std::filesystem::path PackageDirectory(const char* argumentZero);

/** Reads the base agent identity shared by the inference core and GUI observer. **/
std::optional<std::string> ReadAgentIdFromBotToml(const std::filesystem::path& path);

/** Directory containing this running core or GUI executable. **/
std::filesystem::path ExecutableDirectory(const char* argumentZero);
