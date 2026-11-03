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
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {
    const char* Host() { const auto value=std::getenv("RLBOT_SERVER_IP"); return value?value:"127.0.0.1"; }
    const char* Port() { const auto value=std::getenv("RLBOT_SERVER_PORT"); return value?value:"23234"; }
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
    if(rows.empty() || generation.empty()) throw std::invalid_argument("No Byte bots to respawn");
    auto result=current;
    result.existing_match_behavior=rlbot::flat::ExistingMatchBehavior::ContinueAndSpawn;
    for(const auto& [slot,row]:rows) {
        if(slot>=result.player_configurations.size()) throw std::invalid_argument("Match roster changed");
        auto& player=*result.player_configurations[slot];
        auto* bot=player.variety.AsCustomBot();
        if(!bot || IsManualRunCommand(bot->run_command) || player.player_id!=row.playerId || player.team!=row.team ||
            !(bot->agent_id==agentId || bot->agent_id.starts_with(agentId+"/rank/")))
            throw std::invalid_argument("Byte roster changed; review the new match first");
        bot->name=ChoiceBotName(row.pending);
        // Core only despawns a car when its type, team or agent ID changes.
        // A new generation forces the rename. Mode-specific run commands and
        // IDs preserve the selected policy independently of display names.
        // https://github.com/RLBot/core/blob/master/RLBotCS/ManagerTools/MatchStarter.cs
        bot->agent_id=agentId+"/rank/"+generation+"/"+std::to_string(row.pending.Level())+"/"+std::to_string(int(row.pending.mode));
        bot->run_command=RunCommandWithChoice(bot->run_command,row.pending);
        bot->hivemind=true;
    }
    return result;
}

SkillSession::SkillSession(std::shared_ptr<SkillControl> shared,std::filesystem::path directory,std::string id)
    :control(std::move(shared)),package(std::move(directory)),agentId(std::move(id)) {}
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
            const auto choice=ChoiceForBot(bot->name,bot->run_command,control->selectedChoice);
            const auto level=choice.Level();
            SkillRow row{player->player_id,player->team,bot->name,level,choice,choice.mode};
            const auto old=control->rows.find(slot);
            const auto expected=expectedAgents.find(slot);
            // Keep the two-selector choice through our own respawn, including
            // Select Skill at an endpoint and the inactive custom-rank value.
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
            row.active=SkillFromBotName(row.name).value_or(row.active);
            found=BotNameMatches(row.name,ChoiceBotName(row.pending));break;
        }
        const auto expected=expectedAgents.find(slot);
        const auto* bot=configuration->player_configurations[slot]->variety.AsCustomBot();
        confirmed=confirmed && found && expected!=expectedAgents.end() && bot && bot->agent_id==expected->second;
    }
    if(confirmed) {
        control->applying=false;expectedAgents.clear();control->status="Ranks applied; Byte cars respawned";
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
        control->status="Respawning Byte cars... waiting for game confirmation";
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
    if(magic!="BYTE_SKILL_1" || model<0 || model>2 || !std::isfinite(t.effectiveLevel) ||
        t.effectiveLevel<1 || t.effectiveLevel>23) return false;
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
    else std::cerr<<"Could not open Byte controls (Windows error "<<GetLastError()<<").\n";
#endif
}

int RunSkillController(const std::filesystem::path& package,const std::string& agentId,bool debug) {
#ifdef _WIN32
    const auto key=std::filesystem::weakly_canonical(package).wstring()+L"|"+
        std::filesystem::path(Host()).wstring()+L"|"+std::filesystem::path(Port()).wstring();
    uint64_t hash=14695981039346656037ull;
    for(auto ch:key) {hash^=uint64_t(std::towlower(ch));hash*=1099511628211ull;}
    const auto name=L"Local\\ByteSkillController-"+std::to_wstring(hash);
    HANDLE singleton=CreateMutexW(nullptr,TRUE,name.c_str());
    if(!singleton) return 1;
    if(GetLastError()==ERROR_ALREADY_EXISTS) {CloseHandle(singleton);return 0;}
    auto control=std::make_shared<SkillControl>();control->debug=debug;
    control->selectedChoice=ReadSavedChoice(package);
    control->selectedLevel=control->selectedChoice.Level();
    SkillSession session(control,package,agentId);
    if(!session.connect(Host(),Port())) {ReleaseMutex(singleton);CloseHandle(singleton);return 1;}
    session.sendConnectionSettings({.agent_id="",.wants_ball_predictions=false,.wants_comms=true,.close_between_matches=false});
    session.sendInitComplete({});
    control->applyPending=[&session] {session.Apply();};
    // Client::connected() stays true until join() completes in this interface.
    // Wait on the service thread separately so socket loss closes the observer.
    auto stopped=std::async(std::launch::async,[&session] {session.join();});
    {
        SkillWindow window(control);
        while(stopped.wait_for(std::chrono::milliseconds(250))!=std::future_status::ready) session.Tick();
    }
    stopped.get();control->applyPending={};
    ReleaseMutex(singleton);CloseHandle(singleton);return 0;
#else
    std::cerr<<"The shared skill window requires Windows.\n";return 1;
#endif
}
