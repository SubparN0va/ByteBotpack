#pragma once
#include "AdvancedObs.h"

namespace RLGC {
	// Version of AdvancedObs that supports a varying number of players with fixed obs size
	// Pads teammate/opponent slots up to the configured maximum and shuffles to avoid slot bias
	class AdvancedObsPadded : public AdvancedObs {
	public:
		int maxPlayers;

		AdvancedObsPadded(int maxPlayers) : maxPlayers(maxPlayers) {}

		virtual FList BuildObs(const Player& player, const GameState& state) override;
	};
}
