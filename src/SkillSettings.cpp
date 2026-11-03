#include "SkillSettings.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace {
    int ParseSkill(std::string_view text) {
        int level = -1;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), level);
        if (text.empty() || error != std::errc{} || end != text.data() + text.size() || level < 0 || level > MaxSkillLevel)
            throw std::invalid_argument("Skill must be an integer from 0 (Auto) to 23 (SSL+).");
        return level;
    }
}

int SkillChoice::Level() const {
    if(rank<0 || rank>MaxSelectableRank) throw std::invalid_argument("Invalid selected rank");
    switch(mode) {
    case SkillChoiceMode::Byte: return 21;
    case SkillChoiceMode::Kilobyte: return 22;
    case SkillChoiceMode::Megabyte: return 23;
    case SkillChoiceMode::SelectSkill: return rank;
    }
    throw std::invalid_argument("Invalid model choice");
}

SkillChoice SkillChoice::FromLevel(int level) {
    if(level<0 || level>MaxSkillLevel) throw std::invalid_argument("Invalid skill level");
    if(level>=21) return {static_cast<SkillChoiceMode>(level-21),14};
    return {SkillChoiceMode::SelectSkill,level};
}

const char* SkillChoiceLabel(SkillChoiceMode mode) {
    switch(mode) {
    case SkillChoiceMode::Byte: return "Byte";
    case SkillChoiceMode::Kilobyte: return "Kilobyte";
    case SkillChoiceMode::Megabyte: return "Megabyte";
    case SkillChoiceMode::SelectSkill: return "Select Skill";
    }
    throw std::invalid_argument("Invalid model choice");
}

std::string SkillLabel(int level) {
    if (level == 0) return "Auto - try to keep the score even";
    if (level < 0 || level > MaxSkillLevel) throw std::invalid_argument("Invalid skill level");
    if (level == 22) return "Supersonic Legend";
    if (level == MaxSkillLevel) return "SSL+ (Megabyte)";
    static constexpr const char* ranks[] = {"Bronze", "Silver", "Gold", "Platinum", "Diamond", "Champion", "Grand Champion"};
    static constexpr const char* roman[] = {"I", "II", "III"};
    return std::string(ranks[(level - 1) / 3]) + " " + roman[(level - 1) % 3];
}

const char* SkillModelName(SkillModel model) {
    switch (model) {
    case SkillModel::Megabyte: return "Megabyte";
    case SkillModel::Byte95: return "Byte (95B)";
    case SkillModel::Kilobyte: return "Kilobyte";
    }
    throw std::invalid_argument("Invalid skill model");
}

const char* SkillModelFolder(SkillModel model) {
    switch (model) {
    case SkillModel::Megabyte: return "models/megabyte";
    case SkillModel::Byte95: return "models/byte";
    case SkillModel::Kilobyte: return "models/kilobyte";
    }
    throw std::invalid_argument("Invalid skill model");
}

SkillSettings SettingsForSkill(float level, int teamSize) {
    if (!std::isfinite(level) || level < 1.f || level > MaxSkillLevel || teamSize < 1 || teamSize > 3)
        throw std::invalid_argument("Effective skill must be in [1, 23] and team size in [1, 3]");
    SkillSettings settings;
    if (level >= 22.f) {
        // SSL is full Kilobyte. Level 23 remains a legacy CLI/benchmark endpoint
        // for Megabyte, but cannot be selected as a custom rank or reached by Auto.
        settings.model = level < 23.f ? SkillModel::Kilobyte : SkillModel::Megabyte;
        settings.deterministic = true;
        settings.sampling.temperature = 1.f;
        settings.sampling.topK = 1;
        settings.sampling.minRelativeProbability = 0.f;
        settings.sampling.bestActionProbability = 1.f;
        return settings;
    }
    struct Anchor { float level, temperature, topK, minimum, bestShare, ticks; };
    // Provisional 2v2 rank scale: Necto = Diamond III; Nexto = GC I.
    // Each mode is calibrated independently against those reference bots.
    // Ordinary play compensates for the scripted/policy kickoff boundary.
    static constexpr Anchor modeAnchors[3][22] = {
        { // 1v1
            { 1, 2.5f, 13, 0.0124999993f, 0.114999995f, 40 },
            { 2, 2.5f, 13, 0.0132082021f, 0.11924921f, 38 },
            { 3, 2.5f, 13, 0.013680337f, 0.122082025f, 36 },
            { 4, 2.5f, 12, 0.0141595751f, 0.124957442f, 35 },
            { 5, 2.5f, 12, 0.0151690515f, 0.130170569f, 32 },
            { 6, 2.5f, 12, 0.0162615087f, 0.134986877f, 30 },
            { 7, 2.5f, 12, 0.0164062493f, 0.135625005f, 30 },
            { 8, 2.5f, 12, 0.0174547061f, 0.139818832f, 29 },
            { 9, 2.5f, 12, 0.018081829f, 0.142327324f, 28 },
            { 10, 2.47593904f, 12, 0.0212365296f, 0.16124931f, 26 },
            { 11, 2.45570183f, 12, 0.0222765524f, 0.170710891f, 26 },
            { 12, 2.41522717f, 12, 0.0243565999f, 0.189634055f, 25 },
            { 13, 2.25899267f, 11, 0.0323857106f, 0.262678683f, 24 },
            { 14, 2.14216042f, 11, 0.0383898951f, 0.317301571f, 23 },
            { 15, 2.0541265f, 11, 0.043143101f, 0.356408358f, 22 },
            { 16, 1.90480304f, 10, 0.0515905581f, 0.419291377f, 20 },
            { 17, 1.83549213f, 10, 0.056389004f, 0.4406178f, 18 },
            { 18, 1.74000001f, 9, 0.063000001f, 0.469999999f, 16 },
            { 19, 1.30744743f, 6, 0.113324396f, 0.672059596f, 14 },
            { 20, 1.11836004f, 3, 0.0615472198f, 0.853233576f, 11 },
            { 21, 1.01183605f, 2, 0.00615472207f, 0.970000029f, 9 },
            { 22, 1.0f, 1, 0.0f, 1.0f, 8 },
        },
        { // 2v2
            { 1, 2.5f, 12, 0.0101359673f, 0.110543787f, 38 },
            { 2, 2.5f, 12, 0.0130681647f, 0.122272618f, 34 },
            { 3, 2.5f, 12, 0.0140982633f, 0.126393035f, 33 },
            { 4, 2.5f, 12, 0.0155727742f, 0.132291093f, 31 },
            { 5, 2.5f, 12, 0.0160414409f, 0.134165764f, 30 },
            { 6, 2.5f, 12, 0.0172215626f, 0.138886258f, 29 },
            { 7, 2.5f, 12, 0.0180968866f, 0.142387554f, 28 },
            { 8, 2.47557116f, 12, 0.0209895559f, 0.160357803f, 26 },
            { 9, 2.46900582f, 12, 0.0215928257f, 0.164490715f, 26 },
            { 10, 2.37223768f, 12, 0.0265658945f, 0.209733069f, 25 },
            { 11, 2.24006987f, 11, 0.0333581865f, 0.271525741f, 24 },
            { 12, 2.22426844f, 11, 0.0341702513f, 0.278913498f, 24 },
            { 13, 2.17696166f, 11, 0.0366014168f, 0.301030964f, 23 },
            { 14, 2.05439544f, 10, 0.0429002643f, 0.358334541f, 22 },
            { 15, 1.90176308f, 10, 0.0518010147f, 0.420226753f, 20 },
            { 16, 1.88736463f, 10, 0.0527978279f, 0.424657017f, 19 },
            { 17, 1.76831234f, 9, 0.0610399172f, 0.461288512f, 17 },
            { 18, 1.49184775f, 7, 0.0944104567f, 0.581472218f, 16 },
            { 19, 1.29083145f, 6, 0.123466969f, 0.672034144f, 15 },
            { 20, 1.15206063f, 4, 0.0790714994f, 0.811444879f, 12 },
            { 21, 1.07709026f, 3, 0.0400869548f, 0.904408038f, 10 },
            { 22, 1.0f, 1, 0.0f, 1.0f, 8 },
        },
        { // 3v3
            { 1, 2.5f, 12, 0.0134084933f, 0.120450951f, 37 },
            { 2, 2.5f, 12, 0.0138003873f, 0.122802317f, 36 },
            { 3, 2.5f, 12, 0.0145551972f, 0.127331182f, 33 },
            { 4, 2.5f, 12, 0.0162499994f, 0.13499999f, 31 },
            { 5, 2.5f, 12, 0.0175190866f, 0.140076354f, 29 },
            { 6, 2.5f, 12, 0.0192957371f, 0.147182956f, 27 },
            { 7, 2.4951067f, 12, 0.020065764f, 0.151544973f, 26 },
            { 8, 2.45846462f, 12, 0.0221345648f, 0.169419169f, 26 },
            { 9, 2.43771935f, 12, 0.0232006907f, 0.179118216f, 25 },
            { 10, 2.36794496f, 12, 0.026786495f, 0.211739972f, 25 },
            { 11, 2.25315976f, 11, 0.0326854885f, 0.265405893f, 24 },
            { 12, 2.16107702f, 11, 0.0374177508f, 0.308457524f, 23 },
            { 13, 2.10875082f, 10, 0.0401068702f, 0.332921743f, 22 },
            { 14, 1.96099997f, 10, 0.0476999991f, 0.40200001f, 21 },
            { 15, 1.92117572f, 10, 0.0504570603f, 0.414253622f, 20 },
            { 16, 1.82930064f, 10, 0.0568176471f, 0.442522854f, 18 },
            { 17, 1.7840817f, 9, 0.059948191f, 0.456436396f, 17 },
            { 18, 1.45000005f, 7, 0.0960000008f, 0.600000024f, 16 },
            { 19, 1.29999995f, 6, 0.122000001f, 0.667999983f, 15 },
            { 20, 1.2157526f, 5, 0.112191349f, 0.732466757f, 13 },
            { 21, 1.10752773f, 3, 0.0559144169f, 0.866665602f, 11 },
            { 22, 1.0f, 1, 0.0f, 1.0f, 8 },
        },
    };
    const auto& anchors = modeAnchors[teamSize - 1];
    const Anchor* upper = anchors + 1;
    while (upper->level < level) ++upper;
    const auto& lower = *(upper - 1);
    const float weight = (level - lower.level) / (upper->level - lower.level);
    const auto blend = [weight](float a, float b) { return weight <= 0.f ? a : weight >= 1.f ? b : a + weight * (b - a); };
    settings.decisionTicks = static_cast<int>(std::lround(blend(lower.ticks, upper->ticks)));
    settings.sampling.temperature = blend(lower.temperature, upper->temperature);
    settings.sampling.topK = static_cast<int>(std::lround(blend(lower.topK, upper->topK)));
    settings.sampling.minRelativeProbability = blend(lower.minimum, upper->minimum);
    // Share, rather than raw temperature alone, controls the chance of deviating
    // from the best action in every mode. Confident single-candidate rows stay safe.
    settings.sampling.bestActionProbability = blend(lower.bestShare, upper->bestShare);
    return settings;
}

SkillSettings SettingsForChoice(SkillChoice choice,float effectiveLevel,int teamSize) {
    choice.Level();
    if(choice.mode==SkillChoiceMode::SelectSkill) {
        if(effectiveLevel>MaxSelectableRank) throw std::invalid_argument("Select Skill ends at SSL");
        return SettingsForSkill(effectiveLevel,teamSize);
    }
    auto settings=SettingsForSkill(22,teamSize);
    settings.model=choice.mode==SkillChoiceMode::Byte?SkillModel::Byte95:
        choice.mode==SkillChoiceMode::Kilobyte?SkillModel::Kilobyte:SkillModel::Megabyte;
    return settings;
}

SkillSettings GuardKickoff(SkillSettings settings, float effectiveLevel) {
    // Gold remains protected from low-probability flailing, but reacts a little
    // less often and follows the best action less consistently than higher ranks.
    const bool gold = effectiveLevel >= 7.f && effectiveLevel < 10.f;
    settings.decisionTicks = gold ? 10 : 8;
    settings.sampling.temperature = std::min(settings.sampling.temperature, gold ? 1.3f : 1.2f);
    settings.sampling.topK = std::min(settings.sampling.topK, 4);
    settings.sampling.minRelativeProbability = std::max(settings.sampling.minRelativeProbability, 0.12f);
    settings.sampling.bestActionProbability = std::max(settings.sampling.bestActionProbability, gold ? 0.6f : 0.7f);
    return settings;
}

StartupOptions ParseStartupOptions(int argc, const char* const* argv, const char* environmentSkill, int savedSkill) {
    StartupOptions options;
    std::optional<std::string_view> requestedSkill;
    bool manualSpecified=false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") options.showHelp = true;
        else if (arg == "--no-ui") options.showUi = false;
        else if (arg == "--manual" || arg.starts_with("--manual=")) {
            if(manualSpecified) throw std::invalid_argument("Specify --manual only once.");
            manualSpecified=true;
            std::string model="Megabyte";
            if(arg.starts_with("--manual=")) model=arg.substr(9);
            else if(i+1<argc && !std::string_view(argv[i+1]).starts_with("--")) model=argv[++i];
            std::transform(model.begin(),model.end(),model.begin(),[](unsigned char c) {return char(std::tolower(c));});
            if(model=="off") options.manualModel.reset();
            else if(model=="byte") options.manualModel=SkillChoiceMode::Byte;
            else if(model=="kilobyte") options.manualModel=SkillChoiceMode::Kilobyte;
            else if(model=="megabyte") options.manualModel=SkillChoiceMode::Megabyte;
            else throw std::invalid_argument("--manual accepts Off, Byte, Kilobyte or Megabyte.");
            if(options.manualModel) options.showUi=false;
        }
        else if (arg == "--debug") options.debug = true;
        else if (arg == "--ui-only") options.uiOnly = true;
        else if (arg == "--check-models") options.checkModels = true;
        else if (arg.starts_with("--byte-skill-mode=")) {
            // Match-specific metadata also separates same-name hive launches.
            // The actual per-car choice is read from its match configuration.
            const auto value=arg.substr(std::string_view("--byte-skill-mode=").size());
            if(value.size()!=1 || value[0]<'0' || value[0]>'3') throw std::invalid_argument("Invalid Byte model mode");
        }
        else if (arg == "--skill" || arg.starts_with("--skill=")) {
            if (requestedSkill) throw std::invalid_argument("Specify --skill only once.");
            if (arg == "--skill") {
                if (++i == argc) throw std::invalid_argument("--skill requires an integer from 0 to 23.");
                requestedSkill = argv[i];
            } else requestedSkill = arg.substr(8);
        } else throw std::invalid_argument("Unknown argument. Use --help for startup options.");
    }
    if (options.uiOnly && !options.showUi) throw std::invalid_argument("--ui-only cannot be combined with --no-ui.");
    if(options.manualModel) {
        if(requestedSkill) throw std::invalid_argument("--manual cannot be combined with --skill.");
        options.skillLevel=SkillChoice{*options.manualModel}.Level();
        return options;
    }
    if (!options.showHelp) {
        options.explicitSkill = requestedSkill.has_value() || environmentSkill;
        if (requestedSkill) options.skillLevel = ParseSkill(*requestedSkill);
        else if (environmentSkill) options.skillLevel = ParseSkill(environmentSkill);
        else options.skillLevel = savedSkill >= 0 && savedSkill <= MaxSkillLevel ? savedSkill : DefaultSkillLevel;
    }
    return options;
}

float AutoSkillController::Update(int selectedLevel, int teamScore, int opponentScore, float gameSeconds, int teamSize) {
    if (selectedLevel < 0 || selectedLevel > MaxSkillLevel) throw std::invalid_argument("Invalid selected skill");
    const bool newMatch = lastSeconds >= 0.f && (gameSeconds < lastSeconds - 1.f ||
        teamScore < lastTeamScore || opponentScore < lastOpponentScore);
    const bool baseline = newMatch || lastTeamScore < 0 || teamSize != lastMode || selectedLevel != lastSelection;
    if (newMatch) { level = 14.f; direction = 0; step = 1.f; }
    if (selectedLevel != 0) level = static_cast<float>(selectedLevel);
    else level=std::clamp(level,1.f,float(MaxSelectableRank));
    if (selectedLevel == 0 && !baseline && (teamScore != lastTeamScore || opponentScore != lastOpponentScore)) {
        // React once per score update. A lead lowers strength; a deficit raises it.
        const int difference = teamScore - opponentScore;
        const int goalChange = (teamScore - lastTeamScore) - (opponentScore - lastOpponentScore);
        const int nextDirection = difference ? (difference > 0 ? -1 : 1) : (goalChange > 0 ? -1 : goalChange < 0 ? 1 : 0);
        if (nextDirection) {
            step = direction == nextDirection ? std::min(3.f, step * 1.25f) : std::max(0.5f, step * 0.6f);
            const float pressure = 1.f + 0.15f * std::min(std::abs(difference), 4);
            level = std::clamp(level + nextDirection * step * pressure, 1.f, float(MaxSelectableRank));
            direction = nextDirection;
        }
    }
    if (selectedLevel != lastSelection) { direction = 0; step = 1.f; }
    lastTeamScore = teamScore; lastOpponentScore = opponentScore; lastSeconds = gameSeconds;
    lastSelection = selectedLevel; lastMode = teamSize;
    return level;
}
