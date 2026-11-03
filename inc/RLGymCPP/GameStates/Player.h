#pragma once
#include "../Framework.h"
#include "../BasicTypes/Action.h"

namespace RLGC {
	struct PlayerEventState {
		bool goal, save, assist, shot, shotPass, bump, bumped, demo, demoed;

		PlayerEventState() {
			memset(this, 0, sizeof(*this));
		}
	};

	// https://github.com/AechPro/rocket-league-gym-sim/blob/main/rlgym_sim/utils/gamestates/player_data.py
	struct Player : CarState {

		Player* prev = NULL;

		int index = -1; // Index in the gamestate players array
		uint32_t carId;
		Team team;

		PlayerEventState eventState = {};

		bool ballTouchedStep; // True if the player touched the ball during any of tick of the step
		bool ballTouchedTick; // True if the player is touching the ball on the final tick of the step

		Action prevAction = {};

		// RLBot-parity fields (available in RLBot packet; derivable in training)
		uint8_t airState = 0;             // 0..4 (rlbot::flat::AirState)
		bool    hasDodged = false;        // rlbot has_dodged
		float   dodgeTimeout = -1.f;      // rlbot dodge_timeout (seconds), -1 means N/A
		float   demolishedTimeout = -1.f; // rlbot demolished_timeout (seconds), -1 means not demoed

		float   dodgeElapsed = 0.f;       // rlbot dodge_elapsed
		Vec     dodgeDir2D = { 0,0,0 };     // rlbot dodge_dir (x,y), z=0

		// Called before updating to reset the per-step state
		void ResetBeforeStep();

		void UpdateFromCar(Car* car, uint64_t tickCount, int tickSkip, const Action& prevAction, Player* prev);
	};
}