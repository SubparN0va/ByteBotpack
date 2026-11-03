#include "RLBotClient.h"
#include "SkillPersistence.h"
#include "SkillSession.h"

using namespace RLGC;

namespace
{
    Vec ToVec(const rlbot::flat::Vector3 rlbotVec) {
        return Vec(rlbotVec.x(), rlbotVec.y(), rlbotVec.z());
    }

    PhysState ToPhysObj(const rlbot::flat::Physics* phys) {
        PhysState obj = {};
        obj.pos = ToVec(phys->location());

        Angle ang = Angle(phys->rotation().yaw(), phys->rotation().pitch(), phys->rotation().roll());
        obj.rotMat = ang.ToRotMat();

        obj.vel = ToVec(phys->velocity());
        obj.angVel = ToVec(phys->angular_velocity());
        return obj;
    }

    Player ToPlayer(const rlbot::flat::PlayerInfo* playerInfo, float dtSec, PlayerTimingState& timing, int indexInPacket)
    {
        Player pd = {};
        static_cast<PhysState&>(pd) = ToPhysObj(playerInfo->physics());

        pd.index = indexInPacket;
        // Packet indices are unique even when framework player IDs are unset/duplicated.
        pd.carId = static_cast<uint32_t>(indexInPacket) + 1;
        if (!timing.initialized || timing.id != playerInfo->player_id() || timing.team != playerInfo->team())
            timing = {0.f, playerInfo->player_id(), playerInfo->team(), true};
        pd.team = (Team)playerInfo->team();

        pd.boost = playerInfo->boost();

        // --- Raw packet values we may still want ---
        const auto air = playerInfo->air_state();
        const float rawDodgeTimeout = playerInfo->dodge_timeout(); 
        const float rawDemoTimeout = playerInfo->demolished_timeout();

        pd.isOnGround = (air == rlbot::flat::AirState::OnGround);
        pd.isJumping = (air == rlbot::flat::AirState::Jumping);
        pd.isFlipping = (air == rlbot::flat::AirState::Dodging);
        pd.hasJumped = playerInfo->has_jumped();
        pd.hasDoubleJumped = playerInfo->has_double_jumped();
        pd.hasFlipped = playerInfo->has_dodged();
        pd.hasDodged = (pd.hasFlipped || pd.isFlipping);
        pd.isSupersonic = playerInfo->is_supersonic();
        pd.isDemoed = (rawDemoTimeout >= 0.f);
        pd.demoRespawnTimer = pd.isDemoed ? rawDemoTimeout : 0.f;
        pd.dodgeElapsed = playerInfo->dodge_elapsed();
        if (auto dd = playerInfo->dodge_dir()) {
            pd.dodgeDir2D = Vec(dd->x(), dd->y(), 0.f);
        } 
        else {
            pd.dodgeDir2D = Vec{ 0,0,0 };
        }
        pd.flipTime = pd.isFlipping ? std::max(0.f, pd.dodgeElapsed) : 0.f;
        pd.flipRelTorque = pd.isFlipping ? Vec{-pd.dodgeDir2D.y, pd.dodgeDir2D.x, 0.f} : Vec{0,0,0};

        timing.airTime = pd.isOnGround || pd.isDemoed ? 0.f : timing.airTime + dtSec;
        pd.airTime = timing.airTime;
        // The packet retains the correct flip window when joining midair or missing packets.
        if (std::isfinite(rawDodgeTimeout) && rawDodgeTimeout >= 0.f)
            pd.airTimeSinceJump = std::max(0.f, RLConst::DOUBLEJUMP_MAX_DELAY - rawDodgeTimeout);
        else if (pd.isJumping || !pd.hasJumped || pd.isOnGround)
            pd.airTimeSinceJump = 0.f;
        else pd.airTimeSinceJump = RLConst::DOUBLEJUMP_MAX_DELAY;

        // Conservative floor-only self-righting fallback. This is contact, not wheel grounding.
        if (!pd.isDemoed && !pd.isOnGround && pd.rotMat.up.z < -.9f &&
            pd.pos.z >= 0.f && pd.pos.z < 60.f && std::abs(pd.vel.z) < 50.f) {
            pd.worldContact.hasContact = true;
            pd.worldContact.contactNormal = Vec{0,0,1};
        }

        // Training's airState mapping:
        // 0 OnGround, 1 Jumping, 2 DoubleJumping, 3 Dodging, 4 InAir
        pd.airState =
            pd.isOnGround ? 0 :
            pd.isJumping ? 1 :
            pd.hasDoubleJumped ? 2 :
            pd.isFlipping ? 3 :
            4;

        const bool flipAvailable =
            pd.hasJumped &&
            !pd.hasDoubleJumped &&
            !pd.hasDodged &&
            !pd.isOnGround;

        pd.dodgeTimeout = flipAvailable
            ? std::max(0.f, RLConst::DOUBLEJUMP_MAX_DELAY - pd.airTimeSinceJump)
            : 0.f;

        pd.demolishedTimeout = pd.isDemoed ? pd.demoRespawnTimer : 0.f;

        return pd;
    }


    GameState ToGameState(rlbot::flat::GamePacket const* packet, float dtSec, 
                        std::vector<PlayerTimingState>& playerTiming, 
                        const RLBotBoostPads& boostPads)
    {
        GameState gs = {};
        gs.deltaTime = dtSec;

        auto players = packet->players();
        if (players) {
            const int n = (int)players->size();
            if ((int)playerTiming.size() < n)
                playerTiming.resize(n);

            gs.players.reserve(n);
            for (int i = 0; i < n; i++) {
                gs.players.push_back(ToPlayer(players->Get(i), dtSec, playerTiming[i], i));
            }
        }

        static_cast<PhysState&>(gs.ball) = ToPhysObj(packet->balls()->Get(0)->physics());

        boostPads.Apply(gs, *packet);

        return gs;
    }
} // anonymous namespace

RLBotBot::RLBotBot(std::unordered_set<unsigned> indices_,
    unsigned const team_,
    std::string name_,
    std::shared_ptr<const SharedBotContext> ctx) noexcept
    : rlbot::Bot(std::move(indices_), team_, name_)
    , ctx_(std::move(ctx))
    , displayName_(name_)
{
    std::set<unsigned> sorted(std::begin(indices), std::end(indices));
    for (auto const& index : sorted)
        std::printf("Team %u Index %u: %s created\n", team_, index, name_.c_str());
}

RLBotBot::~RLBotBot() {
    std::lock_guard lock(ctx_->control->mutex);
    for (unsigned index : indices) ctx_->control->telemetry.erase(index);
}

void RLBotBot::initialize(const rlbot::flat::ControllableTeamInfo* teamInfo,
    const rlbot::flat::FieldInfo* field,const rlbot::flat::MatchConfiguration* match) noexcept {
    boostPads.SetFieldInfo(field);
    if(ctx_->manualChoice) return;
    if(!teamInfo || !teamInfo->controllables() || !match || !match->player_configurations()) return;
    SkillChoice fallback;
    {std::lock_guard lock(ctx_->control->mutex);fallback=ctx_->control->selectedChoice;}
    for(const auto* car:*teamInfo->controllables()) {
        for(const auto* player:*match->player_configurations()) if(player->player_id()==car->identifier()) {
            if(const auto* bot=player->variety_as_CustomBot(); bot && bot->name())
                configuredChoices[car->index()]=ChoiceForBot(bot->name()->str(),bot->run_command()?bot->run_command()->str():"",fallback);
        }
    }
}

void RLBotBot::update(rlbot::flat::GamePacket const* packet,
    rlbot::flat::BallPrediction const* ballPrediction_) noexcept
{
    if (!packet || !packet->match_info() || !packet->players() || !packet->balls() || packet->balls()->size() == 0 || !packet->balls()->Get(0)->physics()) {
        return;
    }

    const float curTime = packet->match_info()->seconds_elapsed();
    if (!std::isfinite(curTime)) return;
    for (const auto* player : *packet->players()) if (!player || !player->physics()) return;
    const uint32_t frame = packet->match_info()->frame_num();
    const uint32_t elapsed = haveFrame ? frame - previousFrame : 0;
    const bool clockReset = haveFrame && (curTime < previousSeconds || elapsed > 0x80000000u);
    if (haveFrame && !clockReset && elapsed == 0) return;
    if (clockReset) {
        m_botState.clear(); m_playerTiming.clear(); seenKickoffPacket = false;
        kickoffResetPending = false; kickoffScript = {}; elapsedTicks = 0;
    }
    const float deltaTime = !haveFrame || clockReset ? 0.f : elapsed / 120.f;
    elapsedTicks += !haveFrame || clockReset ? 0 : elapsed;
    haveFrame = true; previousFrame = frame; previousSeconds = curTime;
    const int64_t currentTick = elapsedTicks;
    GameState gs = ToGameState(packet, deltaTime, m_playerTiming, boostPads);
    gs.lastTickCount = frame;
    int ownScore = 0, otherScore = 0, teammates = 0, opponents = 0;
    for (const auto& player : gs.players) (unsigned(player.team) == team ? teammates : opponents)++;
    if (auto scores = packet->teams()) {
        for (const auto* score : *scores) {
            if (score->team_index() == team) ownScore = static_cast<int>(score->score());
            else otherScore += static_cast<int>(score->score());
        }
    }
    const int mode = std::clamp(std::max(teammates, opponents), 1, 3);
    const bool kickoff = KickoffControl::Centered(gs);
    const auto phase = packet->match_info()->match_phase();
    const bool kickoffPhase = phase == rlbot::flat::MatchPhase::Countdown || phase == rlbot::flat::MatchPhase::Kickoff;
    float latestTouch = -1.f;
    for (const auto* player : *packet->players())
        if (const auto* touch = player->latest_touch(); touch && touch->ball_index() == 0)
            latestTouch = std::max(latestTouch, touch->game_seconds());
    // Latch the taker at the reset, not on each distance update. A centered ball
    // later in open play must not restart a completed script.
    if (!seenKickoffPacket || (kickoffPhase && !wasKickoffPhase) || kickoffScore != ownScore + otherScore)
        kickoffResetPending = kickoffPhase || kickoff;
    if (kickoff && kickoffResetPending) {
        kickoffScript.Begin(gs, team);
        kickoffTouchBaseline = latestTouch;
        kickoffResetPending = false;
    }
    kickoffScore = ownScore + otherScore;
    seenKickoffPacket = true;
    wasKickoffPhase = kickoffPhase;
    // During countdown the ball falls from its spawn height onto the field.
    // Cars are frozen: do not consume the one-shot script on that settling motion.
    const bool scriptAvailable = phase == rlbot::flat::MatchPhase::Countdown
        ? !kickoffScript.finished : kickoffScript.Update(gs, latestTouch > kickoffTouchBaseline);

    struct PendingDecision {
        unsigned index;
        float effective;
        SkillSettings settings;
        bool scripted, kickoffGuard, kickoffChanged, sampleDiagnostics;
    };
    std::vector<PendingDecision> pending;
    std::array<std::vector<GGL::InferenceRequest>, 3> batches;
    const std::set<unsigned> ordered(indices.begin(), indices.end());
    for (const auto index : ordered)
    {
        if (index >= gs.players.size()) continue;
        auto& st = m_botState[index];
        SkillChoice choice;
        if(ctx_->manualChoice) choice=*ctx_->manualChoice;
        else if(configuredChoices.contains(index)) choice=configuredChoices.at(index);
        else {
            std::lock_guard lock(ctx_->control->mutex);
            const int selected=ctx_->control->selectedLevel.load();
            choice=ctx_->control->selectedChoice.Level()==selected?ctx_->control->selectedChoice:SkillChoice::FromLevel(selected);
        }
        const float effective=st.autoSkill.Update(choice.Level(),ownScore,otherScore,curTime,mode);
        auto settings=SettingsForChoice(choice,effective,mode);
        const bool lowRank = choice.mode == SkillChoiceMode::SelectSkill && effective < 7.f;
        const bool scriptedKickoff = lowRank && scriptAvailable && int(index) == kickoffScript.taker;
        // Low ranks always use ordinary settings outside their one-car script,
        // including before the first touch and during the delayed handoff.
        const bool kickoffGuard = kickoff && !lowRank;
        if (kickoffGuard) settings = GuardKickoff(settings, effective);
        if (!st.initialized) {
            st.initialized = true;

            st.action = RLGC::Action{};
            st.controls = RLGC::Action{};
        }

        auto& localPlayer = gs.players[index];
        localPlayer.prevAction = st.controls;

        const bool kickoffChanged = st.scriptedKickoff != scriptedKickoff;
        if (kickoffChanged) {
            // Drop any pre-kickoff pending action. On exit, infer immediately
            // using the scripted controls as prevAction, with the normal delay.
            st.applyTick = -1;
            st.nextDecisionTick = currentTick;
        }
        st.scriptedKickoff = scriptedKickoff;
        if (scriptedKickoff) {
            st.action = st.controls = KickoffControl::Drive(localPlayer, gs.ball.pos);
            st.applyTick = -1;
            settings.decisionTicks = 1;
            localPlayer.prevAction = st.controls;
        } else if (st.applyTick >= 0 && currentTick >= st.applyTick) {
            st.controls = st.action;
            st.applyTick = -1;
            localPlayer.prevAction = st.controls;
        }
        if (scriptedKickoff || currentTick >= st.nextDecisionTick) {
            const bool sampleDiagnostics = !scriptedKickoff &&
                (kickoffChanged || currentTick - st.lastDiagnosticTick >= 60);
            pending.push_back({index, effective, settings, scriptedKickoff, kickoffGuard, kickoffChanged, sampleDiagnostics});
            if (!scriptedKickoff) batches[static_cast<size_t>(settings.model)].push_back(
                {index, settings.deterministic, settings.sampling, sampleDiagnostics});
        }
    }

    // All controls due on this packet are applied before constructing any observations.
    // Batch the policy forward pass while preserving each car's mask, rank and cadence.
    for (size_t model = 0; model < batches.size(); ++model) {
        if (batches[model].empty()) continue;
        std::vector<GGL::Infer::SamplingDiagnostics> diagnostics;
        const auto actions = ctx_->inferUnits[model]->BatchInferActions(batches[model], gs, &diagnostics);
        for (size_t i = 0; i < actions.size(); ++i) {
            auto& st = m_botState[batches[model][i].index];
            st.action = actions[i];
            if (batches[model][i].diagnostics) {
                st.diagnostics = diagnostics[i]; st.lastDiagnosticTick = currentTick;
            }
        }
    }
    for (const auto& decision : pending) {
        const unsigned index = decision.index;
        auto& st = m_botState[index];
        if (!decision.scripted) {
            st.nextDecisionTick = currentTick + decision.settings.decisionTicks;
            st.applyTick = currentTick + ctx_->params.actionDelay;
        }
        const auto* packetPlayer = packet->players()->Get(index);
        SkillTelemetry telemetry{packetPlayer->name() ? packetPlayer->name()->str() : displayName_,
            team, teammates, opponents, ownScore, otherScore, decision.effective,
            decision.kickoffGuard || decision.scripted, decision.settings,
            decision.scripted ? GGL::Infer::SamplingDiagnostics{} : st.diagnostics, decision.scripted};
        ctx_->control->Publish(index, telemetry);
        if (decision.kickoffChanged || currentTick - st.lastTelemetryTick >= 60) {
            const auto message = EncodeSkillTelemetry(packetPlayer->player_id(), telemetry);
            sendMatchComm(index, "", {message.begin(),message.end()}, false);
            st.lastTelemetryTick = currentTick;
        }
    }
    for (const auto index : ordered) {
        if (index >= gs.players.size()) continue;
        auto& st = m_botState[index];
        if (st.applyTick >= 0 && currentTick >= st.applyTick) {
            st.controls = st.action;
            st.applyTick = -1;
        }

        const auto& c = st.controls;
        setOutput(index, {
            c.throttle,
            c.steer,
            c.pitch,
            c.yaw,
            c.roll,
            c.jump > 0.5f,
            c.boost > 0.5f,
            c.handbrake > 0.5f,
            false,
            });
    }

}
