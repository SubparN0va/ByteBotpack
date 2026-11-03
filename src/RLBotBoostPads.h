#pragma once

#include <rlbot/Bot.h>
#include <RLGymCPP/GameStates/GameState.h>
#include <array>
#include <cmath>

/** Maps packet pads to the canonical soccer order used by Byte's observations. **/
class RLBotBoostPads {
public:
    RLBotBoostPads() {
        for (int i = 0; i < Count; ++i) mapping[i] = i;
    }

    void SetFieldInfo(const rlbot::flat::FieldInfo* field) {
        if (!field) return;
        mapping.fill(-1);
        const auto* pads = field->boost_pads();
        if (!pads || pads->size() != Count) return;
        std::array<int, Count> candidate;
        std::array<bool, Count> used{};
        for (int i = 0; i < Count; ++i) {
            int closest = -1;
            float distance = 64.f;
            for (int j = 0; j < Count; ++j) {
                const auto* pos = pads->Get(j)->location();
                if (!pos || used[j]) continue;
                const RocketSim::Vec point(pos->x(), pos->y(), pos->z());
                const float squared = point.DistSq2D(RLGC::CommonValues::BOOST_LOCATIONS[i]);
                if (squared < distance) { closest = j; distance = squared; }
            }
            if (closest < 0) return;
            used[closest] = true;
            candidate[i] = closest;
        }
        mapping = candidate;
    }

    void Apply(RLGC::GameState& state, const rlbot::flat::GamePacket& packet) const {
        const auto* pads = packet.boost_pads();
        for (int i = 0; i < Count; ++i) {
            const auto* pad = pads && pads->size() == Count && mapping[i] >= 0 ? pads->Get(mapping[i]) : nullptr;
            const bool active = pad && pad->is_active();
            const float duration = RLGC::CommonValues::BOOST_LOCATIONS[i].z > 72.f ? 10.f : 4.f;
            const float reported = pad ? pad->timer() : -1.f;
            // NOTE: Preserve valid raw timer semantics and the trained observation inversion.
            // Unknown cooldowns must not encode an unavailable pad as immediately available.
            const float timer = active ? 0.f :
                std::isfinite(reported) && reported > 0.f && reported <= duration + .05f ? reported : duration;
            state.boostPads[i] = state.boostPadsInv[Count - 1 - i] = active;
            state.boostPadTimers[i] = state.boostPadTimersInv[Count - 1 - i] = timer;
        }
    }

private:
    static constexpr int Count = RLGC::CommonValues::BOOST_LOCATIONS_AMOUNT;
    std::array<int, Count> mapping;
};
