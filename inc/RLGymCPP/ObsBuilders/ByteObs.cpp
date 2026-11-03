#include "ByteObs.h"


#include <RLGymCPP/GameStates/StateUtil.h>

#include <algorithm>
#include <random>

using namespace RLGC;



namespace
{
	void KeepClosestToBall(std::vector<const Player*>& players, int maxCount, const Vec& ballPos)
	{
		if ((int)players.size() <= maxCount)
			return;

		std::partial_sort(
			players.begin(),
			players.begin() + maxCount,
			players.end(),
			[&ballPos](const Player* lhs, const Player* rhs)
			{
				const float lhsDistance = lhs->pos.DistSq(ballPos);
				const float rhsDistance = rhs->pos.DistSq(ballPos);
				if (lhsDistance != rhsDistance)
					return lhsDistance < rhsDistance;
				return lhs->carId < rhs->carId;
			});
		players.resize(maxCount);
	}
}

void ByteObs::AddPlayerToObs(FList& obs, const Player& player, bool inv, const PhysState& ball)
{
	auto phys = InvertPhys(player, inv);

	obs += phys.pos * POS_COEF;
	obs += phys.rotMat.forward;
	obs += phys.rotMat.up;
	obs += phys.vel * VEL_COEF;
	obs += phys.angVel * ANG_VEL_COEF;
	obs += phys.rotMat.Dot(phys.angVel) * ANG_VEL_COEF;

	// Local ball pos and vel
	obs += phys.rotMat.Dot(ball.pos - phys.pos) * POS_COEF;
	obs += phys.rotMat.Dot(ball.vel - phys.vel) * VEL_COEF;

	obs += player.boost / 100;
	obs += player.isOnGround;
	obs += player.HasFlipOrJump();
	obs += player.isDemoed;
	obs += player.hasJumped;
	obs += 1.f;
}

FList ByteObs::BuildObs(const Player& player, const GameState& state)
{
	FList obs = {};
	BuildObsInto(player, state, obs);
	return obs;
}

void ByteObs::BuildObsInto(const Player& player, const GameState& state, FList& obs)
{
	obs.clear();
	obs.reserve(82 + 29 * (2 * maxPlayers));
	bool inv = player.team == Team::ORANGE;

	auto ball = InvertPhys(state.ball, inv);
	auto& pads = state.GetBoostPads(inv);
	// NOTE: Existing Byte policies require the opposite timer orientation. The
	// corrected builder keeps the timers aligned with the player-relative pads.
	auto& padTimers = state.GetBoostPadTimers(useCorrectBoostPadTimers ? inv : !inv);

	obs += ball.pos * POS_COEF;
	obs += ball.vel * VEL_COEF;
	obs += ball.angVel * ANG_VEL_COEF;

	for (int i = 0; i < player.prevAction.ELEM_AMOUNT; i++)
		obs += player.prevAction[i];

	for (int i = 0; i < CommonValues::BOOST_LOCATIONS_AMOUNT; i++)
	{
		if (pads[i])
		{
			obs += 1.f;
		}
		else
		{
			obs += 1.f / (1.f + padTimers[i]);
		}
	}

	const int teammateSlots = std::max(0, maxPlayers - 1);
	const int opponentSlots = std::max(0, maxPlayers);
	std::vector<const Player*> teammates = {}, opponents = {};
	teammates.reserve(teammateSlots);
	opponents.reserve(opponentSlots);

	for (auto& otherPlayer : state.players)
	{
		if (otherPlayer.carId == player.carId)
			continue;

		if (otherPlayer.team == player.team)
			teammates.push_back(&otherPlayer);
		else
			opponents.push_back(&otherPlayer);
	}

	// Keep the trained 3v3 layout in oversized RLBot matches by dropping the cars
	// farthest from the ball. Self always retains its dedicated observation slot.
	KeepClosestToBall(teammates, teammateSlots, state.ball.pos);
	KeepClosestToBall(opponents, opponentSlots, state.ball.pos);

	constexpr float MAX_TEAMMATES = 2.f;
	constexpr float MAX_OPPONENTS = 3.f;

	// NOTE: Learner.cpp relies on these counts staying in this exact position when normalization is on.
	obs += teammates.size() / MAX_TEAMMATES;
	obs += opponents.size() / MAX_OPPONENTS;

	FList selfObs = {};
	selfObs.reserve(32);
	AddPlayerToObs(selfObs, player, inv, ball);
	obs += selfObs;
	int playerObsSize = selfObs.size();

	std::vector<FList> teammateObs = {}, opponentObs = {};
	teammateObs.reserve(teammateSlots);
	opponentObs.reserve(opponentSlots);

	for (const Player* otherPlayer : teammates)
	{
		FList otherObs = {};
		otherObs.reserve(playerObsSize);
		AddPlayerToObs(otherObs, *otherPlayer, inv, ball);
		teammateObs.push_back(std::move(otherObs));
	}

	for (const Player* otherPlayer : opponents)
	{
		FList otherObs = {};
		otherObs.reserve(playerObsSize);
		AddPlayerToObs(otherObs, *otherPlayer, inv, ball);
		opponentObs.push_back(std::move(otherObs));
	}

	for (int i = 0; i < 2; i++)
	{
		auto& players = i ? teammateObs : opponentObs;
		int targetCount = i ? teammateSlots : opponentSlots;

		while ((int)players.size() < targetCount)
		{
			players.emplace_back(playerObsSize, 0.0f);
		}
	}

	if (seededEvaluation) {
		// Keep the trained slot-shuffle distribution, but key it to this game,
		// relative tick and player so worker scheduling cannot change evaluation.
		const uint64_t tick = state.lastTickCount - evaluationStartTick;
		std::seed_seq seed { uint32_t(evaluationSeed), uint32_t(evaluationSeed >> 32),
			uint32_t(tick), uint32_t(tick >> 32), uint32_t(player.carId) };
		std::mt19937 engine(seed);
		std::shuffle(teammateObs.begin(), teammateObs.end(), engine);
		std::shuffle(opponentObs.begin(), opponentObs.end(), engine);
	} else {
		// GetRandEngine owns thread-local state, so workers can shuffle independently.
		std::shuffle(teammateObs.begin(), teammateObs.end(), ::Math::GetRandEngine());
		std::shuffle(opponentObs.begin(), opponentObs.end(), ::Math::GetRandEngine());
	}

	for (auto& teammate : teammateObs)
		obs += teammate;
	for (auto& opponent : opponentObs)
		obs += opponent;
}
