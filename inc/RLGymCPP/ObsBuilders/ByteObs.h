#pragma once

#include <RLGymCPP/ObsBuilders/AdvancedObs.h>

namespace RLGC
{
	/**
	 * Byte's legacy padded observation layout.
	 * Preserves the boost pad timer orientation used by existing policies.
	 **/
	class ByteObs : public AdvancedObs
	{
	public:
		int maxPlayers;
		void SetEvaluationSeed(uint64_t seed) { evaluationSeed = seed; seededEvaluation = true; }
		void Reset(const GameState& state) override { evaluationStartTick = state.lastTickCount; }

		explicit ByteObs(int maxPlayers) : ByteObs(maxPlayers, false) {}

		FList BuildObs(const Player& player, const GameState& state) override;
		void BuildObsInto(const Player& player, const GameState& state, FList& out);

		void AddPlayerToObs(FList& obs, const Player& player, bool inv, const PhysState& ball) override;

	protected:
		ByteObs(int maxPlayers, bool useCorrectBoostPadTimers)
			: maxPlayers(maxPlayers), useCorrectBoostPadTimers(useCorrectBoostPadTimers) {}

	private:
		bool useCorrectBoostPadTimers;
		bool seededEvaluation = false;
		uint64_t evaluationSeed = 0, evaluationStartTick = 0;
	};

	/**
	 * Byte's observation layout for new training.
	 * Boost pad timers use the same player-relative inversion as pad activity.
	 **/
	class ByteObsCorrected final : public ByteObs
	{
	public:
		explicit ByteObsCorrected(int maxPlayers) : ByteObs(maxPlayers, true) {}
	};
}
