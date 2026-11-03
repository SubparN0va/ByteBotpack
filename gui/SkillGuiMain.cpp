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
        const auto agentId = ReadAgentIdFromBotToml(package / "bot.toml").value_or("SubparNova/MegabyteSkill");
        if (argc >= 2 && std::string_view(argv[1]) == "--controller")
            return RunSkillController(package, agentId, argc == 3 && std::string_view(argv[2]) == "--debug");

        auto choice = ReadSavedChoice(package);
        const auto options = ParseStartupOptions(argc, argv, std::getenv("GGLBOT_SKILL"), choice.Level());
        if (options.explicitSkill) choice = SkillChoice::FromLevel(options.skillLevel);
        if (options.showHelp) {
            std::cout << "Byte Model Selection: --ui-only [--skill 0..23] [--debug]\n";
            return 0;
        }
        if (!options.uiOnly) throw std::invalid_argument("Use the launcher to start the bot, or --ui-only to preview its controls.");
        auto control = std::make_shared<SkillControl>();
        control->saveSelection = [package](SkillChoice selection) { return SaveSkillChoice(package, selection); };
        control->Select(choice);
        control->debug = options.debug;
        SkillWindow window(control, true);
        window.Wait();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Byte Model Selection: " << error.what() << '\n';
        return 1;
    }
}
