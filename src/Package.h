#pragma once

#include <filesystem>
#include <optional>
#include <string>

enum class BotProfile { KilobyteReduced, Megabyte };

/** Build-time identity is independent of package files and launch commands. **/
BotProfile CompiledBotProfile() noexcept;

/** Finds the bot package beside the launcher, independent of the working directory. **/
std::filesystem::path PackageDirectory(const char* argumentZero);

/** Directory containing the launcher and models, including Bob platform packages. **/
std::filesystem::path LauncherDirectory(const char* argumentZero);

/** Reads the base agent identity shared by the inference core and GUI observer. **/
std::optional<std::string> ReadAgentIdFromBotToml(const std::filesystem::path& path);

/** Directory containing this running core or GUI executable. **/
std::filesystem::path ExecutableDirectory(const char* argumentZero);
