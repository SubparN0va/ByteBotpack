#include "RLBotClient.h"
#include "SkillPersistence.h"
#include "SkillSession.h"
#include "Package.h"
#if GGLBOT_USE_CUDA
#include <torch/cuda.h>
#endif

#include <rlbot/BotManager.h>
#include <ATen/Parallel.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{
    std::shared_ptr<const SharedBotContext>& SpawnContext() noexcept
    {
        static std::shared_ptr<const SharedBotContext> ctx;
        return ctx;
    }

    void SetSpawnContext(std::shared_ptr<const SharedBotContext> ctx) noexcept
    {
        SpawnContext() = std::move(ctx);
    }

    std::unique_ptr<rlbot::Bot> SpawnBot(std::unordered_set<unsigned> indices,
        unsigned team,
        std::string name) noexcept
    {
        auto ctx = SpawnContext();
        if (!ctx)
            return {};

        return std::make_unique<RLBotBot>(std::move(indices), team, std::move(name), std::move(ctx));
    }

    class RLBotBotManager final : public rlbot::BotManagerBase
    {
    public:
        explicit RLBotBotManager(bool batchHivemind = false) noexcept
            : rlbot::BotManagerBase(batchHivemind, SpawnBot)
        {
        }
    };
} // anonymous namespace


int main(int argc, char** argv)
{
    const auto exeDir = PackageDirectory(argc ? argv[0] : nullptr);
    const auto profile = CompiledBotProfile();
    const bool megabyte = profile == BotProfile::Megabyte;
    const auto baseAgentId=ReadAgentIdFromBotToml(exeDir/"bot.toml").value_or(
        megabyte ? "SubparNova/Megabyte" : "SubparNova/KilobyteReduced");
    if(!megabyte && argc>=2 && std::string_view(argv[1])=="--controller")
        return RunSkillController(exeDir,baseAgentId,argc>=3 && std::string_view(argv[2])=="--debug");
    StartupOptions startup;
    auto choice=megabyte ? SkillChoice{SkillChoiceMode::Megabyte} : ReadSavedChoice(exeDir);
    try {
        startup = ParseStartupOptions(argc, argv, std::getenv("GGLBOT_SKILL"), choice.Level(), megabyte);
        if(startup.manualModel) choice={*startup.manualModel};
        else if(startup.explicitSkill) choice=SkillChoice::FromLevel(startup.skillLevel);
    }
    catch (const std::invalid_argument& error) {
        std::cerr << error.what() << "\nUsage: GGLBot.exe [--skill 0..22] [--no-ui] [--debug] [--ui-only] [--check-models]\n";
        return EXIT_FAILURE;
    }
    if (startup.showHelp) {
        std::cout << "Usage: GGLBot.exe [--skill 0..22] [--no-ui] [--debug] [--ui-only] [--check-models]\n"
            "Kilobyte: 0 = Auto; 1..21 = Bronze I through Grand Champion III; 22 = full-strength SSL.\n"
            "Megabyte packages always run at full strength without the GUI.\n"
            "--check-models checks the package policy without connecting to RLBot.\n"
            "One shared rank window opens for Kilobyte. --ui-only sets the next-match default.\n";
        return EXIT_SUCCESS;
    }

    auto ctx = std::make_shared<SharedBotContext>();
    ctx->control = std::make_shared<SkillControl>();
    ctx->startup = startup;
    if(startup.manualModel) ctx->manualChoice=choice;
    // Initial startup preserves Kilobyte[Champ I]; only an explicit selection saves a rank name.
    ctx->control->Select(choice);
    if (!startup.checkModels && !megabyte)
        ctx->control->saveSelection = [exeDir](SkillChoice selection) { return SaveSkillChoice(exeDir, selection); };
    ctx->control->debug.store(startup.debug);
    std::unique_ptr<SkillWindow> skillWindow;
    if (startup.uiOnly) skillWindow = std::make_unique<SkillWindow>(ctx->control, true);
    if (startup.uiOnly) { skillWindow->Wait(); return EXIT_SUCCESS; }
    if(startup.manualModel) std::cout << "Package model: " << SkillChoiceLabel(*startup.manualModel) << " (full strength, GUI disabled)\n";
    else std::cout << "Kilobyte default skill: " << startup.skillLevel << " - " << SkillLabel(startup.skillLevel) << "\n";

    // Live inference uses tiny batches. Avoid competing CPU thread pools when
    // several bot processes run together in a team match.
    at::set_num_threads(1);
    at::set_num_interop_threads(1);

    // ------------------------------------------------------------------------
    // Set the following to match the configuration your model was trained with
    // ------------------------------------------------------------------------
    ctx->obs = std::make_shared<RLGC::ByteObs>(3);
    ctx->act = std::make_shared<RLGC::ByteAction>();

    ctx->params.tickSkip = 8;
    ctx->params.actionDelay = ctx->params.tickSkip - 1;

    int obsSize = 233; // You can find this from the console when running training

    // Shared head config
    GGL::InferPartialModelConfig sharedHeadCfg;
    sharedHeadCfg.layerSizes = { 1024, 1024};
    sharedHeadCfg.addLayerNorm = true;
    sharedHeadCfg.activationType = GGL::ModelActivationType::RELU;
    sharedHeadCfg.addOutputLayer = false; // <- leave this false

    // Policy config
    GGL::InferPartialModelConfig policyCfg;
    policyCfg.layerSizes = { 1024, 1024, 512, 512 };
    policyCfg.addLayerNorm = true;
    policyCfg.activationType = GGL::ModelActivationType::RELU;
    policyCfg.addOutputLayer = true;

    // ------------------------------------------
    // Everything below can usually be left as is
    // ------------------------------------------
    constexpr bool useGPU = GGLBOT_USE_CUDA != 0;
    try {
#if GGLBOT_USE_CUDA
        if (!torch::cuda::is_available())
            throw std::runtime_error("CUDA is unavailable; check the NVIDIA GPU, driver, and shared LibTorch runtime.");
#endif

    const auto modelRoot = LauncherDirectory(argc ? argv[0] : nullptr);
    for (SkillModel model : {megabyte ? SkillModel::Megabyte : SkillModel::Kilobyte}) {
        // Each package contains exactly one policy pair beside its launcher.
        const auto& folder = modelRoot;
        std::cout << "Loading " << SkillModelName(model) << " from " << folder.string() << '\n';
        ctx->inferUnits[static_cast<size_t>(model)] = std::make_shared<GGL::InferUnit>(
            ctx->obs.get(), obsSize, ctx->act.get(), sharedHeadCfg, policyCfg, folder, useGPU);
    }

    } catch (const std::exception& error) {
        std::cerr << (megabyte ? "Megabyte: " : "Kilobyte: ") << (useGPU ? "GPU" : "CPU") << " initialization failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "Bot: using " << (useGPU ? "GPU" : "CPU") << " for " << (megabyte ? "Megabyte" : "Kilobyte") << "\n" << std::flush;
    if (startup.checkModels) return EXIT_SUCCESS;

    SetSpawnContext(ctx);

    auto const serverHost = []() -> char const* {
        auto const env = std::getenv("RLBOT_SERVER_IP");
        return env ? env : "127.0.0.1";
        }();

    auto const serverPort = []() -> char const* {
        auto const env = std::getenv("RLBOT_SERVER_PORT");
        return env ? env : "23234";
        }();

    // Read agent_id from bot.toml next to the exe
    const std::filesystem::path botTomlPath = exeDir / "bot.toml";
    std::string agentIdStr = "GigaLearn/GGLBot"; // fallback default
    if (auto maybeId = ReadAgentIdFromBotToml(botTomlPath)) {
        agentIdStr = *maybeId;
    }

    // Respawns use a generation-specific agent ID to force RLBot to replace
    // the cars. RLBot passes that identity to the new process in the environment.
    if(const auto assigned=std::getenv("RLBOT_AGENT_ID"); assigned && *assigned) agentIdStr=assigned;
    RLBotBotManager manager(true);

    if (!manager.connect(serverHost, serverPort, agentIdStr.c_str(), false)) {
        return EXIT_FAILURE;
    }
    if(startup.showUi) {
#ifdef _WIN32
        const auto controller = "ByteSkillUI.exe";
#else
        const auto controller = "ByteSkillUI";
#endif
        LaunchSkillController(ExecutableDirectory(argv[0])/controller,startup.debug);
    }

    return 0;
}
