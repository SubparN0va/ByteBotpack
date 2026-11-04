#include "Package.h"
#include "SkillControl.h"
#include "SkillPersistence.h"
#include "SkillSession.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    try {
        const auto package = PackageDirectory(argc ? argv[0] : nullptr);
        if (CompiledBotProfile() != BotProfile::KilobyteReduced)
            throw std::invalid_argument("Megabyte has no skill selector");
        const auto agentId = ReadAgentIdFromBotToml(package / "bot.toml").value_or("SubparNova/KilobyteReduced");
        if (argc >= 2 && std::string_view(argv[1]) == "--controller")
            return RunSkillController(package, agentId, argc == 3 && std::string_view(argv[2]) == "--debug");

        auto choice = ReadSavedChoice(package);
        const auto options = ParseStartupOptions(argc, argv, std::getenv("GGLBOT_SKILL"), choice.Level());
        if (options.explicitSkill) choice = SkillChoice::FromLevel(options.skillLevel);
        if (options.showHelp) {
            std::cout << "Kilobyte Skill Selection: --ui-only [--skill 0..22] [--debug]\n";
            return 0;
        }
        if (!options.uiOnly) throw std::invalid_argument("Use the launcher to start the bot, or --ui-only to preview its controls.");
        auto control = std::make_shared<SkillControl>();
        control->Select(choice);
        control->saveSelection = [package](SkillChoice selection) { return SaveSkillChoice(package, selection); };
        control->debug = options.debug;
        SkillWindow window(control, true);
        window.Wait();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Kilobyte Skill Selection: " << error.what() << '\n';
        return 1;
    }
}
