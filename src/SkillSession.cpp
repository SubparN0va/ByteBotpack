#include "SkillSession.h"
#include "SkillPersistence.h"
#include <cmath>
#include <cwctype>
#include <iostream>
#include <future>
#include <locale>
#include <sstream>
#include <thread>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace {
    const char* Host() { const auto value=std::getenv("RLBOT_SERVER_IP"); return value?value:"127.0.0.1"; }
    const char* Port() { const auto value=std::getenv("RLBOT_SERVER_PORT"); return value?value:"23234"; }
    // One observer per package/server. Never unlink a flock file: replacing its
    // inode would allow a second controller to take a different lock concurrently.
    class ControllerInstance {
#ifdef _WIN32
        HANDLE handle = nullptr;
#else
        int handle = -1;
#endif
    public:
        bool acquired = false;
        explicit ControllerInstance(const std::filesystem::path& package) {
            const auto path = std::filesystem::weakly_canonical(package);
#ifdef _WIN32
            const auto key = path.wstring() + L"|" + std::filesystem::path(Host()).wstring() + L"|" + std::filesystem::path(Port()).wstring();
#else
            const auto key = path.string() + "|" + Host() + "|" + Port();
#endif
            uint64_t hash = 14695981039346656037ull;
            for (auto ch : key) {
#ifdef _WIN32
                ch = std::towlower(ch);
#endif
                hash ^= uint64_t(ch); hash *= 1099511628211ull;
            }
#ifdef _WIN32
            const auto name = L"Local\\ByteSkillController-" + std::to_wstring(hash);
            handle = CreateMutexW(nullptr, TRUE, name.c_str());
            if (!handle) throw std::runtime_error("Cannot lock the skill controller");
            acquired = GetLastError() != ERROR_ALREADY_EXISTS;
#else
            const auto name = path / ("skill-controller-" + std::to_string(hash) + ".lock");
            handle = open(name.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (handle < 0) throw std::runtime_error("Cannot open skill controller lock: " + std::string(std::strerror(errno)));
            acquired = flock(handle, LOCK_EX | LOCK_NB) == 0;
            if (!acquired && errno != EWOULDBLOCK) {
                const auto error = errno; close(handle); handle = -1;
                throw std::runtime_error("Cannot lock skill controller: " + std::string(std::strerror(error)));
            }
#endif
        }
        ~ControllerInstance() {
#ifdef _WIN32
            if (handle) { if (acquired) ReleaseMutex(handle); CloseHandle(handle); }
#else
            if (handle >= 0) close(handle);
#endif
        }
    };
    bool OwnBot(const rlbot::flat::CustomBotT& bot,const std::filesystem::path& package,const std::string& agentId) {
        std::error_code error;
        return !IsManualRunCommand(bot.run_command) && (bot.agent_id==agentId || bot.agent_id.starts_with(agentId+"/rank/")) &&
            std::filesystem::equivalent(bot.root_dir,package,error) && !error;
    }
}

// Existing observer connections are not guaranteed a new MatchConfiguration
// broadcast. A short read-only connection fetches it after a roster change.
// https://github.com/RLBot/core/blob/master/RLBotCS/Server/ServerMessage/StartMatch.cs
class SkillConfigProbe final : public rlbot::Client {
    SkillSession& owner;
public:
    std::atomic<bool> done=false;
    explicit SkillConfigProbe(SkillSession& session):owner(session) {}
    ~SkillConfigProbe() override {terminate();join();}
    void handleMatchConfiguration(const rlbot::flat::MatchConfiguration* packet) noexcept override {
        owner.handleMatchConfiguration(packet);done=true;
    }
};

rlbot::flat::MatchConfigurationT RankRespawnConfiguration(const rlbot::flat::MatchConfigurationT& current,
    const std::map<unsigned,SkillRow>& rows,const std::string& agentId,const std::string& generation) {
    if(rows.empty() || generation.empty()) throw std::invalid_argument("No Kilobyte bots to respawn");
    auto result=current;
    result.existing_match_behavior=rlbot::flat::ExistingMatchBehavior::ContinueAndSpawn;
    for(const auto& [slot,row]:rows) {
        if(slot>=result.player_configurations.size()) throw std::invalid_argument("Match roster changed");
        if(row.pending.mode != SkillChoiceMode::SelectSkill) throw std::invalid_argument("Only Kilobyte ranks can respawn");
        auto& player=*result.player_configurations[slot];
        auto* bot=player.variety.AsCustomBot();
        if(!bot || IsManualRunCommand(bot->run_command) || player.player_id!=row.playerId || player.team!=row.team ||
            !(bot->agent_id==agentId || bot->agent_id.starts_with(agentId+"/rank/")))
            throw std::invalid_argument("Kilobyte roster changed; review the new match first");
        bot->name=ChoiceBotName(row.pending);
        // Core only despawns a car when its type, team or agent ID changes.
        // A new generation forces the rename. Rank-specific IDs separate hive
        // launches while the package profile keeps the policy fixed.
        // https://github.com/RLBot/core/blob/master/RLBotCS/ManagerTools/MatchStarter.cs
        bot->agent_id=agentId+"/rank/"+generation+"/"+std::to_string(row.pending.Level())+"/"+std::to_string(int(row.pending.mode));
        bot->run_command=RunCommandWithChoice(bot->run_command,row.pending);
        bot->hivemind=true;
    }
    return result;
}

SkillSession::SkillSession(std::shared_ptr<SkillControl> shared,std::filesystem::path directory,std::string id,const char* rankOverride)
    :control(std::move(shared)),package(std::move(directory)),agentId(std::move(id)),
     environmentSkill(rankOverride ? std::optional<std::string>(rankOverride) : std::nullopt) {}
SkillSession::~SkillSession() {probe.reset();terminate();join();}

void SkillSession::handleMatchConfiguration(const rlbot::flat::MatchConfiguration* packet) noexcept {
    if(!packet) return;
    try {
        auto next=std::unique_ptr<rlbot::flat::MatchConfigurationT>(packet->UnPack());
        flatbuffers::FlatBufferBuilder encoded;
        encoded.Finish(rlbot::flat::MatchConfiguration::Pack(encoded,next.get()));
        std::vector<uint8_t> bytes(encoded.GetBufferPointer(),encoded.GetBufferPointer()+encoded.GetSize());
        const auto saved=ReadSavedBotSkills(package);
        std::lock_guard lock(control->mutex);
        if(bytes==configurationBytes) return;
        configurationBytes=std::move(bytes);
        std::map<unsigned,SkillRow> rows;
        for(unsigned slot=0;slot<next->player_configurations.size();++slot) {
            const auto& player=next->player_configurations[slot];
            const auto* bot=player->variety.AsCustomBot();
            if(!bot || !OwnBot(*bot,package,agentId)) continue;
            const auto startup = ParseRunCommandOptions(bot->run_command,
                environmentSkill ? environmentSkill->c_str() : nullptr, control->selectedChoice.Level());
            const auto choice=ChoiceForBot(bot->name,startup,control->selectedChoice);
            const auto level=choice.Level();
            SkillRow row{player->player_id,player->team,bot->name,level,choice,choice.mode};
            const auto old=control->rows.find(slot);
            const auto expected=expectedAgents.find(slot);
            // Keep the pending rank through our own respawn and roster refresh.
            if(old!=control->rows.end() && (old->second.playerId==row.playerId ||
                (control->applying && expected!=expectedAgents.end() && bot->agent_id==expected->second)))
                row.pending=old->second.pending;
            else if(!control->applying) {
                const auto remembered=saved.find({slot,row.team});
                if(remembered!=saved.end()) row.pending=remembered->second;
            }
            rows.emplace(slot,std::move(row));
        }
        configuration=std::move(next);control->rows=std::move(rows);control->telemetry.clear();
        needConfiguration=false;
        control->canRespawn=false;
        if(!control->applying) control->status="Choose ranks, then apply & respawn";
    } catch(const std::exception& error) {
        std::lock_guard lock(control->mutex);control->status=error.what();control->canRespawn=false;
    }
}

void SkillSession::handleGamePacket(const rlbot::flat::GamePacket* packet) noexcept {
    if(!packet || !packet->match_info() || !packet->players()) return;
    std::lock_guard lock(control->mutex);
    lastPacket=std::chrono::steady_clock::now();
    const auto phase=packet->match_info()->match_phase();
    std::vector<int> configuredIds,actualIds;
    if(configuration) for(const auto& p:configuration->player_configurations) configuredIds.push_back(p->player_id);
    for(const auto* p:*packet->players()) actualIds.push_back(p->player_id());
    std::sort(configuredIds.begin(),configuredIds.end());std::sort(actualIds.begin(),actualIds.end());
    const bool currentRoster=configuredIds==actualIds;
    if(!currentRoster) needConfiguration=true;
    control->canRespawn=configuration && !control->rows.empty() &&
        currentRoster && phase!=rlbot::flat::MatchPhase::Inactive && phase!=rlbot::flat::MatchPhase::Ended;
    bool confirmed=control->applying && expectedAgents.size()==control->rows.size();
    for(auto& [slot,row]:control->rows) {
        bool found=false;
        for(const auto* player:*packet->players()) if(player->player_id()==row.playerId && player->name()) {
            row.name=player->name()->str();
            // Names can still reflect the packaged default when --skill or the
            // environment overrides it. The match configuration resolved active.
            found=BotNameMatches(row.name,ChoiceBotName(row.pending));break;
        }
        const auto expected=expectedAgents.find(slot);
        const auto* bot=configuration->player_configurations[slot]->variety.AsCustomBot();
        confirmed=confirmed && found && expected!=expectedAgents.end() && bot && bot->agent_id==expected->second;
    }
    if(confirmed) {
        control->applying=false;expectedAgents.clear();control->status="Ranks applied; Kilobyte cars respawned";
    }
}

void SkillSession::Apply() {
    std::lock_guard lock(control->mutex);
    if(control->applying) return;
    if(!connected() || !control->canRespawn || !configuration ||
        std::chrono::steady_clock::now()-lastPacket>std::chrono::seconds(3)) {
        control->status="No active match connection. Nothing was changed.";return;
    }
    try {
        const auto generation=std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        auto next=RankRespawnConfiguration(*configuration,control->rows,agentId,generation);
        expectedAgents.clear();
        for(const auto& [slot,row]:control->rows) expectedAgents[slot]=next.player_configurations[slot]->variety.AsCustomBot()->agent_id;
        control->applying=true;requestedAt=std::chrono::steady_clock::now();
        needConfiguration=true;
        control->status="Respawning Kilobyte cars... waiting for game confirmation";
        sendMatchConfiguration(std::move(next));
        // Remember a useful default for a fresh match. Per-car settings live in
        // the match configuration itself and are restored by each spawned bot.
        SavedBotSkills saved;
        for(const auto& [slot,row]:control->rows) saved[{slot,row.team}]=row.pending;
        control->saveError=SaveBotSkills(package,saved);
        const auto error=SaveSkillChoice(package,control->rows.begin()->second.pending);
        if(!error.empty()) control->saveError+=(control->saveError.empty()?"":"; ")+error;
    } catch(const std::exception& error) {
        control->applying=false;control->status=error.what();
    }
}

void SkillSession::Tick() {
    const auto now=std::chrono::steady_clock::now();
    if(probe && (probe->done || now-lastProbe>std::chrono::seconds(3))) probe.reset();
    bool refresh=false;
    {
        std::lock_guard lock(control->mutex);
        if(!connected() || now-lastPacket>std::chrono::seconds(3)) control->canRespawn=false;
        if(control->applying && now-requestedAt>std::chrono::seconds(20)) {
            control->applying=false;expectedAgents.clear();
            control->status="Respawn not confirmed. Check RLBot; ranks follow the actual cars.";
        }
        refresh=(needConfiguration || control->applying) && !probe && now-lastProbe>std::chrono::seconds(2);
    }
    if(refresh && connected()) {
        lastProbe=now;probe=std::make_unique<SkillConfigProbe>(*this);
        if(probe->connect(Host(),Port()))
            probe->sendConnectionSettings({.agent_id="",.wants_ball_predictions=false,.wants_comms=false,.close_between_matches=false});
        else probe.reset();
    }
}

std::string EncodeSkillTelemetry(int playerId,const SkillTelemetry& t) {
    std::ostringstream s;s.imbue(std::locale::classic());
    s<<"BYTE_SKILL_1 "<<playerId<<' '<<t.effectiveLevel<<' '<<t.kickoff<<' '
        <<int(t.settings.model)<<' '<<t.settings.deterministic<<' '<<t.settings.decisionTicks<<' '
        <<t.settings.sampling.temperature<<' '<<t.settings.sampling.topK<<' '<<t.settings.sampling.minRelativeProbability<<' '
        <<t.settings.sampling.bestActionProbability<<' '<<t.sampling.finalBestProbability<<' '
        <<t.sampling.policyBestProbability<<' '<<t.sampling.normalizedEntropy<<' '<<t.sampling.retainedActions<<' '
        <<t.teammates<<' '<<t.opponents<<' '<<t.teamScore<<' '<<t.opponentScore<<' '<<t.scriptedKickoff;
    return s.str();
}
bool DecodeSkillTelemetry(const std::string& message,int& playerId,SkillTelemetry& t) {
    if(message.size()>2048) return false;
    std::istringstream s(message);s.imbue(std::locale::classic());std::string magic;int model=-1;
    if(!(s>>magic>>playerId>>t.effectiveLevel>>t.kickoff>>model>>t.settings.deterministic>>t.settings.decisionTicks
        >>t.settings.sampling.temperature>>t.settings.sampling.topK>>t.settings.sampling.minRelativeProbability
        >>t.settings.sampling.bestActionProbability>>t.sampling.finalBestProbability>>t.sampling.policyBestProbability
        >>t.sampling.normalizedEntropy>>t.sampling.retainedActions>>t.teammates>>t.opponents>>t.teamScore>>t.opponentScore)) return false;
    if(magic!="BYTE_SKILL_1" || model<0 || model>1 || !std::isfinite(t.effectiveLevel) ||
        t.effectiveLevel<1 || t.effectiveLevel>MaxSelectableRank) return false;
    // The optional trailing flag keeps telemetry from older bot processes readable.
    t.scriptedKickoff=false;
    s>>std::ws;
    if(!s.eof() && !(s>>t.scriptedKickoff)) return false;
    t.settings.model=static_cast<SkillModel>(model);return true;
}
void SkillSession::handleMatchComm(const rlbot::flat::MatchComm* packet) noexcept {
    if(!packet || !packet->content()) return;
    SkillTelemetry t;int id=0;
    const auto bytes=packet->content();
    if(bytes->size()>2048 || !DecodeSkillTelemetry(std::string(bytes->begin(),bytes->end()),id,t)) return;
    std::lock_guard lock(control->mutex);
    for(const auto& [slot,row]:control->rows) if(row.playerId==id && row.team==packet->team()) {
        t.name=row.name;t.team=row.team;control->telemetry[slot]=std::move(t);break;
    }
}

void LaunchSkillController(const std::filesystem::path& executable,bool debug) {
#ifdef _WIN32
    std::wstring command=L"\""+executable.wstring()+L"\" --controller"+(debug?L" --debug":L"");
    STARTUPINFOW startup={sizeof(startup)};PROCESS_INFORMATION process{};
    // Windows can override the first ShowWindow call with this launch setting.
    // https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-showwindow
    startup.dwFlags=STARTF_USESHOWWINDOW|STARTF_FORCEOFFFEEDBACK;startup.wShowWindow=SW_SHOWNOACTIVATE;
    DWORD flags=CREATE_NO_WINDOW;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job{};
    // Leave our launcher's cleanup job, so a core restart cannot close the shared GUI.
    if(QueryInformationJobObject(nullptr,JobObjectExtendedLimitInformation,&job,sizeof(job),nullptr) &&
        (job.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_BREAKAWAY_OK)) flags|=CREATE_BREAKAWAY_FROM_JOB;
    if(CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,flags,
        nullptr,executable.parent_path().c_str(),&startup,&process)) {CloseHandle(process.hThread);CloseHandle(process.hProcess);}
    else std::cerr<<"Could not open Kilobyte controls (Windows error "<<GetLastError()<<").\n";
#else
    // Spawn without a shell and in its own session so an inference respawn does
    // not kill the controller. Do not retain RLBot sockets or captured stdio
    // pipes, which could otherwise keep a retired inference launch alive.
    // https://man7.org/linux/man-pages/man3/posix_spawn.3.html
    auto path = executable.string();
    char controller[] = "--controller", diagnostic[] = "--debug";
    char* arguments[] = {path.data(), controller, debug ? diagnostic : nullptr, nullptr};
    posix_spawnattr_t attributes;
    posix_spawn_file_actions_t actions;
    int error = posix_spawnattr_init(&attributes);
    if (error) { std::cerr << "Cannot initialize controller launch: " << std::strerror(error) << '\n'; return; }
    error = posix_spawn_file_actions_init(&actions);
    if (error) { posix_spawnattr_destroy(&attributes); std::cerr << "Cannot initialize controller launch: " << std::strerror(error) << '\n'; return; }
    error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
    if (!error) error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (!error) error = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    if (!error) error = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    if (!error) error = posix_spawn_file_actions_addclosefrom_np(&actions, 3);
    pid_t child = -1;
    if (!error) error = posix_spawn(&child, path.c_str(), &actions, &attributes, arguments, environ);
    posix_spawn_file_actions_destroy(&actions); posix_spawnattr_destroy(&attributes);
    if (error) { std::cerr << "Could not open Kilobyte controls: " << std::strerror(error) << '\n'; return; }
    std::thread([child] {
        int status = 0;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
            std::cerr << "Kilobyte controls exited with status " << WEXITSTATUS(status) << ". Check the desktop session and GTK 3 runtime.\n";
    }).detach();
#endif
}

int RunSkillController(const std::filesystem::path& package,const std::string& agentId,bool debug) {
    ControllerInstance singleton(package);
    if (!singleton.acquired) return 0;
    auto control=std::make_shared<SkillControl>();control->debug=debug;
    control->selectedChoice=ReadSavedChoice(package);
    control->selectedLevel=control->selectedChoice.Level();
    SkillSession session(control,package,agentId,std::getenv("GGLBOT_SKILL"));
    if(!session.connect(Host(),Port())) return 1;
    session.sendConnectionSettings({.agent_id="",.wants_ball_predictions=false,.wants_comms=true,.close_between_matches=false});
    session.sendInitComplete({});
    control->applyPending=[&session] {session.Apply();};
    // Client::connected() stays true until join() completes in this interface.
    // Wait on the service thread separately so socket loss closes the observer.
    auto stopped=std::async(std::launch::async,[&session] {session.join();});
    try {
        SkillWindow window(control);
        while(stopped.wait_for(std::chrono::milliseconds(20))!=std::future_status::ready) {
            session.Tick();
            if (!window.Pump()) { session.terminate(); break; }
        }
    } catch (...) {
        session.terminate(); stopped.get(); control->applyPending = {}; throw;
    }
    stopped.get();control->applyPending={};
    return 0;
}
