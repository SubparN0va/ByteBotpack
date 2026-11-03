#pragma once
#include <RLGymCPP/GameStates/GameState.h>
#include <cmath>

/** One-car kickoff selection, latched handoff and drive controls. **/
namespace KickoffControl {
    inline bool Centered(const RLGC::GameState& state) {
        return std::abs(state.ball.pos.x) < 1.f && std::abs(state.ball.pos.y) < 1.f
            && state.ball.pos.z > 70.f && state.ball.pos.z < 120.f
            && state.ball.vel.Length() < 10.f;
    }

    /** Chooses among all teammates, including humans and other bot families. **/
    inline int Taker(const RLGC::GameState& state, unsigned team) {
        int best = -1;
        float distance = 0.f, left = 0.f;
        for (int i = 0; i < static_cast<int>(state.players.size()); ++i) {
            const auto& player = state.players[i];
            if (unsigned(player.team) != team || player.isDemoed) continue;
            const float candidate = (player.pos - state.ball.pos).Length();
            const float candidateLeft = team == 0 ? player.pos.x : -player.pos.x;
            if (best < 0 || candidate < distance - 1.f ||
                (std::abs(candidate - distance) <= 1.f && candidateLeft > left)) {
                best = i; distance = candidate; left = candidateLeft;
            }
        }
        return best;
    }

    /** One-shot handoff; rearmed only by an explicit new kickoff/episode. **/
    struct Script {
        int taker = -1;
        bool finished = true;
        void Begin(const RLGC::GameState& state, unsigned team) {
            taker = Taker(state, team);
            finished = taker < 0;
        }
        bool Update(const RLGC::GameState& state, bool touched) {
            if (!finished && (taker >= static_cast<int>(state.players.size()) || touched || !Centered(state)
                || state.players[taker].isDemoed || (state.players[taker].pos - state.ball.pos).Length() <= 400.f))
                finished = true;
            return !finished;
        }
    };

    inline RLGC::Action Drive(const RLGC::Player& player, const RocketSim::Vec& ball) {
        const auto target = (ball - player.pos).To2D();
        const float angle = std::atan2(target.Dot(player.rotMat.right), target.Dot(player.rotMat.forward));
        RLGC::Action action{};
        action.throttle = 1.f;
        action.steer = std::clamp(angle * 3.f, -1.f, 1.f);
        action.boost = player.isOnGround && !player.isDemoed && player.boost > 0.f
            && !player.isSupersonic && player.vel.Length2D() < RocketSim::RLConst::SUPERSONIC_START_SPEED
            && std::abs(angle) < .35f;
        return action;
    }
}
