#pragma once

#include <rlbot/Bot.h>
#include <RLGymCPP/ObsBuilders/ByteObs.h>
#include <RLGymCPP/ActionParsers/ByteAction.h>
#include <GigaLearnCPP/InferUnit.h>
#include "SkillSettings.h"
#include "KickoffControl.h"
#include "SkillControl.h"
#include "RLBotBoostPads.h"

#include <memory>
#include <array>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GGL { class InferUnit; }

struct RLBotParams {
    int tickSkip;
    int actionDelay;
};

struct SharedBotContext {
    std::shared_ptr<RLGC::ObsBuilder> obs;
    std::shared_ptr<RLGC::ActionParser> act;
    /** Loaded once so rank changes never read model files on the game thread. **/
    std::array<std::shared_ptr<GGL::InferUnit>, 3> inferUnits;
    RLBotParams params;
    /** Locks every controlled car to the explicit full-strength startup model. **/
    std::optional<SkillChoice> manualChoice;
    std::shared_ptr<SkillControl> control;
};

struct PlayerTimingState {
    float airTime = 0.f;
    int id = 0;
    unsigned team = 0;
    bool initialized = false;
};

class RLBotBot : public rlbot::Bot {
public:
    struct PerBotState {
        bool initialized = false;
        /** Tracks the handoff so queued model actions cannot interrupt the kickoff. **/
        bool scriptedKickoff = false;
        int64_t nextDecisionTick = -1, applyTick = -1;
        int64_t lastTelemetryTick = -120, lastDiagnosticTick = -120;
        GGL::Infer::SamplingDiagnostics diagnostics;
        AutoSkillController autoSkill;

        // Queued action and current action
        RLGC::Action
            action = {},
            controls = {};
    };
    

    RLBotBot() noexcept = delete;
    ~RLBotBot() noexcept override;

    RLBotBot(std::unordered_set<unsigned> indices_,
        unsigned team_,
        std::string name_,
        std::shared_ptr<const SharedBotContext> ctx_) noexcept;

    void update(rlbot::flat::GamePacket const* packet_,
        rlbot::flat::BallPrediction const* ballPrediction_) noexcept override;
    void initialize(const rlbot::flat::ControllableTeamInfo* teamInfo,
        const rlbot::flat::FieldInfo* field, const rlbot::flat::MatchConfiguration* match) noexcept override;

private:
    std::shared_ptr<const SharedBotContext> ctx_;
    std::unordered_map<unsigned, SkillChoice> configuredChoices;
    std::string displayName_;
    std::unordered_map<unsigned, PerBotState> m_botState;
    std::vector<PlayerTimingState> m_playerTiming;
    KickoffControl::Script kickoffScript;
    RLBotBoostPads boostPads;
    bool haveFrame = false;
    uint32_t previousFrame = 0;
    float previousSeconds = 0.f;
    int64_t elapsedTicks = 0;
    bool seenKickoffPacket = false, wasKickoffPhase = false, kickoffResetPending = false;
    float kickoffTouchBaseline = -1.f;
    int kickoffScore = -1;

};
