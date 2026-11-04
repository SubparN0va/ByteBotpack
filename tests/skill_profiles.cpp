#include "Package.h"
#include "SkillPersistence.h"
#include "SkillSession.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(GGLBOT_LINUX_GUI)
#include <gtk/gtk.h>
#endif

namespace {
    void Check(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }
    template<class F> void Reject(F action) {
        bool rejected = false;
        try { action(); } catch (const std::invalid_argument&) { rejected = true; }
        Check(rejected, "Invalid profile/rank accepted");
    }
    std::string Read(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }
    void Write(const std::filesystem::path& path, const std::string& text) {
        std::ofstream output(path, std::ios::binary); output << text;
        Check(bool(output), "Cannot write fixture");
    }
    void CheckRanks() {
        for (int rank = 0; rank <= 22; ++rank) {
            const auto choice = SkillChoice::FromLevel(rank);
            Check(choice.mode == SkillChoiceMode::SelectSkill && choice.Level() == rank, "Rank switched model");
            Check(SkillFromBotName(ChoiceBotName(choice)) == rank, "Rank name failed round trip");
            for (int team = 1; team <= 3; ++team) {
                const auto settings = SettingsForChoice(choice, rank ? float(rank) : 14.f, team);
                Check(settings.model == SkillModel::Kilobyte, "Rank uses another policy");
                Check(settings.deterministic == (rank == 22), "SSL deterministic boundary changed");
            }
        }
        Check(SkillBotName(DefaultSkillLevel) == "Kilobyte[Champ I]", "Default rank/name mismatch");
        Check(SkillFromBotName("Kilobyte[Champion I]") == 16, "Legacy Champion name unrecognized");
        Check(SkillBotName(22) == "Kilobyte", "SSL must have no rank suffix");
        Check(SkillBotName(14) == "Kilobyte[Diamond II]", "Reduced name changed");
        Check(SkillFromBotName("Kilobyte (2)") == 22, "Duplicate SSL name unrecognized");
        Check(!SkillFromBotName("Megabyte"), "Megabyte parsed as a rank");
        Check(ChoiceForBot("Megabyte", {}, SkillChoice::FromLevel(14)).rank == 14,
              "Display name switched reduced policy");
        const auto mega = SettingsForChoice({SkillChoiceMode::Megabyte}, 14, 2);
        Check(mega.model == SkillModel::Megabyte && mega.deterministic && mega.decisionTicks == 8,
              "Megabyte must run at full strength");
        AutoSkillController automatic;
        const auto initial = automatic.Update(0, 0, 0, 1, 2);
        Check(automatic.Update(0, 1, 0, 2, 2) < initial, "Auto must weaken after taking the lead");
        Reject([] { SkillChoice::FromLevel(23); });
    }
    void CheckStartup() {
        const char* plain[] = {"GGLBot"};
        const auto kilo = ParseStartupOptions(1, plain, nullptr);
        Check(kilo.showUi && !kilo.manualModel && kilo.skillLevel == 16, "Wrong reduced defaults");
        const auto mega = ParseStartupOptions(1, plain, "invalid legacy environment", 0, true);
        Check(!mega.showUi && mega.manualModel == SkillChoiceMode::Megabyte && mega.skillLevel == 22,
              "Megabyte profile must override saved/environment ranks");
        const char* ranked[] = {"GGLBot", "--skill=22"};
        Check(ParseStartupOptions(2, ranked, nullptr).skillLevel == 22, "SSL CLI rejected");
        Reject([&] { ParseStartupOptions(2, ranked, nullptr, 14, true); });
        const char* manual[] = {"GGLBot", "--manual=Byte"};
        Reject([&] { ParseStartupOptions(2, manual, nullptr); });
        const char* ui[] = {"GGLBot", "--ui-only"};
        Reject([&] { ParseStartupOptions(2, ui, nullptr, 14, true); });
    }
    void CheckRankOverrides(const std::filesystem::path& directory) {
        const auto saved = SkillChoice::FromLevel(14);
        const std::string name = "Kilobyte[Champ I]";
        const char* arguments[] = {"GGLBot", "--skill=1", "--no-ui"};
        const auto direct = ParseStartupOptions(3, arguments, "22", saved.rank);
        Check(ChoiceForBot(name, direct, saved).rank == 1, "Actual startup argv lost to the display name");
        for (const auto* command : {"GGLBot.exe --skill=1 --no-ui", "./GGLBot --skill 1",
                "\"C:\\Bot pack\\GGLBot.exe\" \"--skill=1\"", "'./Bot pack/GGLBot' --skill '1'"}) {
            const auto initial = ParseRunCommandOptions(command, "22", saved.rank);
            Check(ChoiceForBot(name, initial, saved).rank == 1, "CLI rank lost to the name or environment");
            for (int rank : {0, 7, 22}) {
                const auto selected = SkillChoice::FromLevel(rank);
                const auto respawn = RunCommandWithChoice(command, selected);
                const auto options = ParseRunCommandOptions(respawn, "22", saved.rank);
                Check(ChoiceForBot(ChoiceBotName(selected) + " (2)", options, saved).rank == rank,
                      "Apply rank lost to an earlier CLI/environment override");
            }
        }
        const auto environment = ParseRunCommandOptions("GGLBot --no-ui", "0", saved.rank);
        Check(ChoiceForBot(name, environment, saved).rank == 0, "Environment Auto lost to the name");
        const auto ordinary = ParseRunCommandOptions("GGLBot", nullptr, saved.rank);
        Check(ChoiceForBot(name, ordinary, saved).rank == 16, "Existing match rank lost without an override");
        Check(ChoiceForBot("Custom Kilobyte", ordinary, saved).rank == 14, "Saved fallback rank lost");
        Reject([] { ParseRunCommandOptions("GGLBot --skill=23", nullptr); });
        Reject([] { ParseRunCommandOptions("GGLBot --skill", nullptr); });
        Reject([] { ParseRunCommandOptions("GGLBot --skill=1 --skill=2", nullptr); });
        Reject([] { ParseRunCommandOptions("GGLBot --skill='1", nullptr); });

        // Exercise the real observer with different CLI ranks and inherited Auto.
        // Game-packet names remain Champ I, so they must not reset the active ranks.
        rlbot::flat::MatchConfigurationT match;
        for (int index = 0; index < 4; ++index) {
            auto player = std::make_unique<rlbot::flat::PlayerConfigurationT>();
            player->player_id = 200 + index; player->team = index % 2;
            rlbot::flat::CustomBotT bot;
            bot.agent_id = index == 3 ? "SubparNova/Megabyte" : "SubparNova/KilobyteReduced";
            bot.name = index == 3 ? "Megabyte" : name;
            bot.root_dir = directory.string(); bot.hivemind = true;
            bot.run_command = index == 1 ? "GGLBot --skill 1" : index == 2 ? "GGLBot --skill=22" : "GGLBot";
            player->variety.Set(std::move(bot));
            match.player_configurations.push_back(std::move(player));
        }
        auto control = std::make_shared<SkillControl>();
        SkillSession session(control, directory, "SubparNova/KilobyteReduced", "0");
        const auto configure = [&](const rlbot::flat::MatchConfigurationT& configuration) {
            flatbuffers::FlatBufferBuilder builder;
            builder.Finish(rlbot::flat::MatchConfiguration::Pack(builder, &configuration));
            session.handleMatchConfiguration(flatbuffers::GetRoot<rlbot::flat::MatchConfiguration>(builder.GetBufferPointer()));
        };
        configure(match);
        Check(control->rows.size() == 3, "Override roster includes Megabyte or lost Kilobyte");
        const int expected[] = {0, 1, 22};
        rlbot::flat::GamePacketT packet;
        packet.match_info = std::make_unique<rlbot::flat::MatchInfoT>();
        packet.match_info->match_phase = rlbot::flat::MatchPhase::Active;
        for (int index = 0; index < 4; ++index) {
            auto player = std::make_unique<rlbot::flat::PlayerInfoT>();
            player->player_id = 200 + index; player->team = index % 2;
            player->name = index == 3 ? "Megabyte" : name;
            packet.players.push_back(std::move(player));
        }
        flatbuffers::FlatBufferBuilder builder;
        builder.Finish(rlbot::flat::GamePacket::Pack(builder, &packet));
        session.handleGamePacket(flatbuffers::GetRoot<rlbot::flat::GamePacket>(builder.GetBufferPointer()));
        for (unsigned slot = 0; slot < 3; ++slot) {
            const auto& row = control->rows.at(slot);
            Check(row.active == expected[slot] && row.pending.rank == expected[slot],
                  "Observer disagrees with the startup override after a game packet");
        }
        control->rows.at(0).pending = SkillChoice::FromLevel(7);
        control->rows.at(1).pending = SkillChoice::FromLevel(22);
        control->rows.at(2).pending = SkillChoice::FromLevel(0);
        auto next = RankRespawnConfiguration(match, control->rows, "SubparNova/KilobyteReduced", "override-test");
        configure(next);
        for (unsigned slot = 0; slot < 3; ++slot) {
            const auto& row = control->rows.at(slot);
            Check(row.active == row.pending.rank, "Observer did not adopt the per-car Apply selection");
            const auto* bot = next.player_configurations[slot]->variety.AsCustomBot();
            const auto startup = ParseRunCommandOptions(bot->run_command, "0", saved.rank);
            Check(ChoiceForBot(bot->name, startup, saved).rank == row.active, "Respawned inference and observer disagree");
        }
        Check(next.player_configurations[3]->variety.AsCustomBot()->name == "Megabyte", "Apply changed Megabyte");
    }
    void CheckRoster(const std::filesystem::path& directory) {
        rlbot::flat::MatchConfigurationT match;
        for (int index = 0; index < 2; ++index) {
            auto player = std::make_unique<rlbot::flat::PlayerConfigurationT>();
            player->player_id = 100 + index; player->team = index;
            rlbot::flat::CustomBotT bot;
            bot.agent_id = index ? "SubparNova/Megabyte" : "SubparNova/KilobyteReduced";
            bot.name = index ? "Megabyte" : "Kilobyte[Champ I]";
            bot.root_dir = directory.string(); bot.run_command = "GGLBot.exe"; bot.hivemind = true;
            player->variety.Set(std::move(bot));
            match.player_configurations.push_back(std::move(player));
        }
        auto control = std::make_shared<SkillControl>();
        SkillSession session(control, directory, "SubparNova/KilobyteReduced");
        flatbuffers::FlatBufferBuilder builder;
        builder.Finish(rlbot::flat::MatchConfiguration::Pack(builder, &match));
        session.handleMatchConfiguration(flatbuffers::GetRoot<rlbot::flat::MatchConfiguration>(builder.GetBufferPointer()));
        Check(control->rows.size() == 1 && control->rows.contains(0), "GUI roster includes Megabyte");
        control->rows.at(0).pending = SkillChoice::FromLevel(22);
        const auto next = RankRespawnConfiguration(match, control->rows, "SubparNova/KilobyteReduced", "test");
        const auto* kilo = next.player_configurations[0]->variety.AsCustomBot();
        const auto* mega = next.player_configurations[1]->variety.AsCustomBot();
        Check(kilo->name == "Kilobyte" && kilo->agent_id.starts_with("SubparNova/KilobyteReduced/rank/"),
              "SSL respawn name/identity wrong");
        Check(mega->name == "Megabyte" && mega->run_command == "GGLBot.exe" && mega->agent_id == "SubparNova/Megabyte",
              "Reduced GUI changed Megabyte");
        Check(match.player_configurations[0]->variety.AsCustomBot()->name == "Kilobyte[Champ I]",
              "Pending configuration modified live match");
    }
#ifdef _WIN32
    BOOL CALLBACK FindOwnWindow(HWND window, LPARAM data) {
        DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
        wchar_t title[128]{}; GetWindowTextW(window, title, 128);
        if (pid == GetCurrentProcessId() && std::wstring(title) == L"Kilobyte Skill Selection") {
            *reinterpret_cast<HWND*>(data) = window; return FALSE;
        }
        return TRUE;
    }
    BOOL CALLBACK CountCombos(HWND window, LPARAM data) {
        wchar_t name[64]{}; GetClassNameW(window, name, 64);
        if (std::wstring(name) == L"ComboBox") ++*reinterpret_cast<int*>(data);
        return TRUE;
    }
    void CheckGui(const std::filesystem::path& directory) {
        auto control = std::make_shared<SkillControl>();
        control->Select(SkillChoice::FromLevel(14));
        control->saveSelection = [directory](SkillChoice choice) { return SaveSkillChoice(directory, choice); };
        const auto initial = Read(directory / "bot.toml");
        SkillWindow panel(control, true);
        HWND window = nullptr;
        for (int attempt = 0; !window && attempt < 100; ++attempt) {
            EnumWindows(FindOwnWindow, reinterpret_cast<LPARAM>(&window));
            if (!window) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        Check(window != nullptr, "Selector window did not open");
        int combos = 0; EnumChildWindows(window, CountCombos, reinterpret_cast<LPARAM>(&combos));
        Check(combos == 1, "GUI must have only one rank selector");
        const auto combo = GetDlgItem(window, 101);
        Check(SendMessageW(combo, CB_GETCOUNT, 0, 0) == 23, "Wrong rank count in GUI");
        Check(Read(directory / "bot.toml") == initial, "Opening GUI renamed fresh reduced entry");
        SendMessageW(combo, CB_SETCURSEL, 22, 0);
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(101, CBN_SELCHANGE), reinterpret_cast<LPARAM>(combo));
        // Editing stays pending until the user applies/saves the rank.
        Check(Read(directory / "bot.toml") == initial, "Pending edit renamed bot prematurely");
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(1, BN_CLICKED), 0);
        Check(ReadSavedSkill(directory) == 22, "GUI did not save SSL");
        Check(Read(directory / "bot.toml").find("name = \"Kilobyte\"") != std::string::npos,
              "GUI saved SSL with a bracketed rank");
    }
#elif defined(GGLBOT_LINUX_GUI)
    GtkWidget* FindWidget(GtkWidget* root, const char* name) {
        if (std::string(gtk_widget_get_name(root)) == name) return root;
        if (!GTK_IS_CONTAINER(root)) return nullptr;
        auto children = gtk_container_get_children(GTK_CONTAINER(root));
        GtkWidget* found = nullptr;
        for (auto child = children; child && !found; child = child->next)
            found = FindWidget(GTK_WIDGET(child->data), name);
        g_list_free(children); return found;
    }
    GtkWidget* FindPanel() {
        auto windows = gtk_window_list_toplevels();
        GtkWidget* panel = nullptr;
        for (auto item = windows; item; item = item->next)
            if (std::string(gtk_widget_get_name(GTK_WIDGET(item->data))) == "kilobyte-panel") panel = GTK_WIDGET(item->data);
        g_list_free(windows); return panel;
    }
    void CheckGui(const std::filesystem::path& directory) {
        auto control = std::make_shared<SkillControl>();
        control->Select(SkillChoice::FromLevel(14));
        control->saveSelection = [directory](SkillChoice choice) { return SaveSkillChoice(directory, choice); };
        const auto initial = Read(directory / "bot.toml");
        {
            SkillWindow panel(control, true); Check(panel.Pump(), "Linux selector did not open");
            const auto window = FindPanel(); Check(window, "Cannot find GTK panel");
            const auto combo = FindWidget(window, "rank-0"); Check(combo && GTK_IS_COMBO_BOX_TEXT(combo), "Missing rank dropdown");
            const auto model = gtk_combo_box_get_model(GTK_COMBO_BOX(combo));
            Check(gtk_tree_model_iter_n_children(model, nullptr) == 23, "Wrong GTK rank count");
            Check(Read(directory / "bot.toml") == initial, "Opening GTK panel changed default name");
            gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 22);
            Check(Read(directory / "bot.toml") == initial, "GTK pending edit saved prematurely");
            gtk_button_clicked(GTK_BUTTON(FindWidget(window, "apply-ranks")));
            Check(ReadSavedSkill(directory) == 22 && Read(directory / "bot.toml").find("name = \"Kilobyte\"") != std::string::npos,
                  "GTK SSL name/persistence incorrect");
            for (int i = 0; i < 10; ++i) { panel.Pump(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
            auto pixels = gdk_pixbuf_get_from_window(gtk_widget_get_window(window), 0, 0,
                gtk_widget_get_allocated_width(window), gtk_widget_get_allocated_height(window));
            Check(pixels, "Cannot capture GTK window");
            Check(gdk_pixbuf_save(pixels, (directory / "linux-gui.png").c_str(), "png", nullptr, nullptr), "Cannot save GTK screenshot");
            g_object_unref(pixels);
            gtk_widget_destroy(window); Check(!panel.Pump(), "Closed GTK window remained active");
        }
        control->rows.clear();
        control->rows[4] = {104, 1, "Kilobyte", 22, SkillChoice::FromLevel(22)};
        control->canRespawn = true;
        bool applied = false;
        control->applyPending = [&] { applied = control->rows.at(4).pending.rank == 7; };
        SkillWindow live(control); live.Pump();
        const auto window = FindPanel();
        const auto combo = FindWidget(window, "rank-4");
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 7);
        Check(control->rows.at(4).active == 22 && !applied, "Editing GTK live rank affected active bot");
        gtk_button_clicked(GTK_BUTTON(FindWidget(window, "apply-ranks")));
        Check(applied, "GTK Apply did not reach the shared respawn handler");
    }
#endif
}

int main(int argc, char** argv) {
    try {
        Check(argc == 2, "Pass a scratch output directory");
        const auto directory = std::filesystem::path(argv[1]);
        std::filesystem::create_directories(directory);
        CheckRanks(); CheckStartup(); CheckRankOverrides(directory); CheckRoster(directory);
        Write(directory / "bot.toml", "[settings]\r\nname = \"Kilobyte[Champ I]\" # keep comment");
#if defined(_WIN32) || defined(GGLBOT_LINUX_GUI)
        CheckGui(directory);
#endif
        Check(SaveSkillSelection(directory, 14).empty(), "Saving reduced rank failed");
        const auto text = Read(directory / "bot.toml");
        Check(text == "[settings]\r\nname = \"Kilobyte[Diamond II]\" # keep comment", "Config formatting changed");
        Check(SaveSkillSelection(directory, 22).empty() && ReadSavedChoice(directory).rank == 22, "SSL persistence failed");
        Check(!SaveSkillChoice(directory, {SkillChoiceMode::Megabyte}).empty(), "Saved a model selection in reduced package");
        std::cout << "PASS: fixed profiles, startup overrides, per-car respawns, all ranks, SSL naming, Auto, mixed roster, persistence and GUI\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
