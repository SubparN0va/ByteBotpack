#include "AdvancedObsPadded.h"
#include <RLGymCPP/GameStates/StateUtil.h>
#include <algorithm>

using namespace RLGC;

FList AdvancedObsPadded::BuildObs(const Player& player, const GameState& state) {
	FList obs = {};

	bool inv = player.team == Team::ORANGE;

	auto ball = InvertPhys(state.ball, inv);
	auto& pads = state.GetBoostPads(inv);
	auto& padTimers = state.GetBoostPadTimers(inv);

	// Ball
	obs += ball.pos * POS_COEF;
	obs += ball.vel * VEL_COEF;
	obs += ball.angVel * ANG_VEL_COEF;

	// Previous action
	for (int i = 0; i < player.prevAction.ELEM_AMOUNT; i++)
		obs += player.prevAction[i];

	// Boost pads blended by timers
	for (int i = 0; i < CommonValues::BOOST_LOCATIONS_AMOUNT; i++) {
		if (pads[i]) {
			obs += 1.f;
		} else {
			obs += 1.f / (1.f + padTimers[i]);
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

	obs += activeTeammates / MAX_TEAMMATES;
	obs += activeOpponents / MAX_OPPONENTS;

	// Self
	FList selfObs = {};
	AddPlayerToObs(selfObs, player, inv, ball);
	obs += selfObs;
	int playerObsSize = selfObs.size();

	std::vector<FList> teammates = {}, opponents = {};

	for (auto& otherPlayer : state.players) {
		if (otherPlayer.carId == player.carId)
			continue;

		FList pObs = {};
		AddPlayerToObs(pObs, otherPlayer, inv, ball);
		((otherPlayer.team == player.team) ? teammates : opponents).push_back(pObs);
	}

	// Validate capacity
	if ((int)teammates.size() > maxPlayers - 1)
		RG_ERR_CLOSE("AdvancedObsPadded: Too many teammates for Obs, maximum is " << (maxPlayers - 1));
	if ((int)opponents.size() > maxPlayers)
		RG_ERR_CLOSE("AdvancedObsPadded: Too many opponents for Obs, maximum is " << maxPlayers);

	// Pad up to target counts: teammates to (maxPlayers-1), opponents to maxPlayers
	for (int i = 0; i < 2; i++) {
		auto& list = i ? teammates : opponents;
		int target = i ? maxPlayers - 1 : maxPlayers;
		while ((int)list.size() < target) {
			FList pad(playerObsSize);
			list.push_back(pad);
		}
	}

	// Shuffle to avoid slot bias
	std::shuffle(teammates.begin(), teammates.end(), ::Math::GetRandEngine());
	std::shuffle(opponents.begin(), opponents.end(), ::Math::GetRandEngine());

	// Append lists
	for (auto& t : teammates) obs += t;
	for (auto& o : opponents) obs += o;

	return obs;
}
