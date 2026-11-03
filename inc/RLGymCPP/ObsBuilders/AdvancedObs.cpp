#include "AdvancedObs.h"
#include <RLGymCPP/GameStates/StateUtil.h>

void RLGC::AdvancedObs::AddPlayerToObs(FList& obs, const Player& player, bool inv, const PhysState& ball) {
	auto phys = InvertPhys(player, inv);

	obs += phys.pos * POS_COEF;
	obs += phys.rotMat.forward;
	obs += phys.rotMat.up;
	obs += phys.vel * VEL_COEF;
	obs += phys.angVel * ANG_VEL_COEF;
	obs += phys.rotMat.Dot(phys.angVel) * ANG_VEL_COEF; // Local ang vel

	// Local ball pos and vel
	obs += phys.rotMat.Dot(ball.pos - phys.pos) * POS_COEF;
	obs += phys.rotMat.Dot(ball.vel - phys.vel) * VEL_COEF;

	obs += player.boost / 100;
	obs += player.isOnGround;
	obs += player.HasFlipOrJump();
	obs += player.isDemoed;
	obs += player.hasJumped; // Allows detecting flip resets
	obs += 1.f; // isActive (1 for real serialized players; padded slots remain 0)
}

RLGC::FList RLGC::AdvancedObs::BuildObs(const Player& player, const GameState& state) {
	FList obs = {};

	bool inv = player.team == Team::ORANGE;

	auto ball = InvertPhys(state.ball, inv);
	auto& pads = state.GetBoostPads(inv);
	auto& padTimers = state.GetBoostPadTimers(inv);

	obs += ball.pos * POS_COEF;
	obs += ball.vel * VEL_COEF;
	obs += ball.angVel * ANG_VEL_COEF;

	for (int i = 0; i < player.prevAction.ELEM_AMOUNT; i++)
		obs += player.prevAction[i];

	for (int i = 0; i < CommonValues::BOOST_LOCATIONS_AMOUNT; i++) {
		// A clever trick that blends the boost pads using their timers
		if (pads[i]) {
			obs += 1.f; // Pad is already available
		}
		else {
			obs += 1.f / (1.f + padTimers[i]); // Approaches 1 as the pad becomes available
		}
	}

	int activeTeammates = 0;
	int activeOpponents = 0;

	for (auto& otherPlayer : state.players) {
		if (otherPlayer.carId == player.carId)
			continue;

		if (otherPlayer.team == player.team)
			activeTeammates++;
		else
			activeOpponents++;
	}

	constexpr float MAX_TEAMMATES = 2.f; // 3v3
	constexpr float MAX_OPPONENTS = 3.f; // 3v3

	//NOTE: Don't change the order of these in the obs, Learner.cpp uses its index
	//to avoid normalizing them
	obs += activeTeammates / MAX_TEAMMATES;
	obs += activeOpponents / MAX_OPPONENTS;

	AddPlayerToObs(obs, player, inv, ball);

	FList teammates = {};
	FList opponents = {};

	for (auto& otherPlayer : state.players) {
		if (otherPlayer.carId == player.carId)
			continue;

		AddPlayerToObs(
			(otherPlayer.team == player.team) ? teammates : opponents,
			otherPlayer, inv, ball
		);
	}

	obs += teammates;
	obs += opponents;

	return obs;
}