#pragma once
#include <RLGymCPP/ObsBuilders/ObsBuilder.h>
//#include <RLGymCPP/GameStates/GameState.h>
#include <RLGymCPP/GameStates/Player.h>
#include <RLGymCPP/GameStates/StateUtil.h>

#include <unordered_map>
#include <random>
#include <vector>
#include <algorithm>

// If your include path differs, adjust this include to wherever BallPredTracker lives in your project.
#include <RocketSim/src/Sim/BallPredTracker/BallPredTracker.h>

namespace RLGC {

	class ByteObsPadded : public ObsBuilder {
	public:
		// 3v3 max: opponents=3, teammates(excluding self)=2
		static constexpr int MAX_OPP = 3;
		static constexpr int MAX_TM = 2;

		constexpr static float
			POS_COEF = 1 / 5000.f,
			VEL_COEF = 1 / 2300.f,
			ANG_VEL_COEF = 1 / 3.f;

		// Decision-step horizons (multipliers of policy dt)
		// 6 horizons: 1,2,4,8,16,32 steps
		static constexpr int HN = 6;
		int predStepMult[HN] = { 1, 2, 4, 8, 16, 32 };

		// Sorting horizon in "policy steps" (mid horizon is stable)
		int sortStepMult = 8;

		explicit ByteObsPadded(int tickSkip = 8)
			: tickSkip(tickSkip) {
		}

		// Call this once at episode start (training reset / RLBot kickoff reset / etc.)
		void Reset(const GameState& initialState) override;

		FList BuildObs(const Player& player, const GameState& state) override;

	private:
		int tickSkip = 8;

		// Per-episode tie-break for ordering (carId -> random key)
		std::unordered_map<uint32_t, uint32_t> episodeKey;
		uint32_t episodeSeed = 0;
		uint32_t episodeCounter = 0;

		// RocketSim ball prediction cache
		std::unique_ptr<RocketSim::BallPredTracker> predTracker;
		RocketSim::Arena* predArenaSrc = nullptr;
		uint64_t lastPredArenaTick = UINT64_MAX;

		void EnsurePredTracker(const GameState& state, float maxT);

		// Helpers
		static inline float Clamp01(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }

		void AddAirState(FList& obs, int airState) const;

		void AddPlayerToObs(FList& obs, const Player& self, bool inv, const PhysState& ballInv) const;

		void AddBallPredictionToObs(
			FList& obs,
			const Player& self,
			const PhysState& selfInv,
			bool inv
		);
	};

} // namespace RLGC
