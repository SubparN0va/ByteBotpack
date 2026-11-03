#include "ByteObsPadded.h"

using namespace RLGC;

static uint32_t Mix(uint32_t x) {  // tiny hash
	x ^= x >> 16;
	x *= 0x7feb352d;
	x ^= x >> 15;
	x *= 0x846ca68b;
	x ^= x >> 16;
	return x;
}

void ByteObsPadded::Reset(const GameState& initialState) {
	// Called once per episode start by EnvSet::ResetArena

	episodeCounter++;

	// Deterministic per-episode seed, no wall-clock time.
	// episodeCounter guarantees it changes each reset for this arena.
	uint32_t x = episodeCounter;

	// Optionally fold in something from the arena if available (helps decorrelate across arenas)
	if (initialState.lastArena) {
		x ^= (uint32_t)initialState.lastArena->tickCount;
	}

	episodeSeed = Mix(x);

	// Clear per-episode tie keys (lazily re-filled as carIds appear)
	episodeKey.clear();
}

void ByteObsPadded::AddAirState(FList& obs, int airState) const {
	// RLBot AirState enum values:
	// 0 OnGround, 1 Jumping, 2 DoubleJumping, 3 Dodging, 4 InAir
	for (int i = 0; i < 5; i++) obs += (airState == i) ? 1.f : 0.f;
}

void ByteObsPadded::AddPlayerToObs(FList& obs, const Player& player, bool inv, const PhysState& ballInv) const {
	auto phys = InvertPhys(player, inv);

	// Absolute self
	obs += phys.pos * POS_COEF;
	obs += phys.rotMat.forward;
	obs += phys.rotMat.up;
	obs += phys.vel * VEL_COEF;
	//obs += phys.angVel * ANG_VEL_COEF;
	obs += phys.rotMat.Dot(phys.angVel) * ANG_VEL_COEF;

	// Current ball rel in self-local frame
	obs += phys.rotMat.Dot(ballInv.pos - phys.pos) * POS_COEF;
	obs += phys.rotMat.Dot(ballInv.vel - phys.vel) * VEL_COEF;

	// RLBot-friendly flags/timers (populate these in your Player from packet)
	obs += player.boost / 100.f;

	// Prefer airState if you have it; otherwise derive from obsOnGround
	AddAirState(obs, (int)player.airState);

	obs += player.isSupersonic ? 1.f : 0.f;
	obs += player.hasJumped ? 1.f : 0.f;
	obs += player.hasDoubleJumped ? 1.f : 0.f;
	obs += player.hasDodged ? 1.f : 0.f;

	// Dodge timeout: -1 means N/A; encode (canDodge, timeoutNorm)
	const bool canDodge = player.dodgeTimeout > 0.f;
	obs += canDodge ? 1.f : 0.f;
	obs += canDodge ? Clamp01(player.dodgeTimeout / RLConst::DOUBLEJUMP_MAX_DELAY) : 0.f;

	// Demo timeout: -1 means not demoed
	const bool demoed = player.demolishedTimeout > 0.f;
	obs += demoed ? 1.f : 0.f;
	obs += demoed ? Clamp01(player.demolishedTimeout / RLConst::DEMO_RESPAWN_TIME) : 0.f;

	// Active marker
	obs += 1.f;
}

void ByteObsPadded::EnsurePredTracker(const GameState& state, float maxT) {
	auto* arena = state.lastArena;
	if (!arena) {
		predTracker.reset();
		predArenaSrc = nullptr;
		lastPredArenaTick = UINT64_MAX;
		return;
	}

	// Build tracker sized for maxT
	if (!predTracker || predArenaSrc != arena) {
		const float tickRate = arena->GetTickRate(); // e.g. 120
		const size_t numPredTicks = (size_t)(maxT * tickRate) + 4;
		predTracker = std::make_unique<RocketSim::BallPredTracker>(arena, numPredTicks);
		predArenaSrc = arena;
		lastPredArenaTick = UINT64_MAX;
	}

	if (arena->tickCount != lastPredArenaTick) {
		predTracker->UpdatePredFromArena(arena);
		lastPredArenaTick = arena->tickCount;
	}
}

void ByteObsPadded::AddBallPredictionToObs(
	FList& obs,
	const Player& self,
	const PhysState& selfInv,
	bool inv
) {
	// validity flag
	obs += (predTracker != nullptr) ? 1.f : 0.f;

	// For each horizon: relPosLocal(3) + relVelLocal(3)
	if (!predTracker) {
		for (int i = 0; i < HN; i++) for (int k = 0; k < 6; k++) obs += 0.f;
		return;
	}

	const float tickRate = predArenaSrc->GetTickRate();
	const float dtPolicy = (float)tickSkip / tickRate;

	for (int i = 0; i < HN; i++) {
		const float t = dtPolicy * (float)predStepMult[i];

		auto pb = predTracker->GetBallStateForTime(t);
		auto pbInv = InvertPhys(pb, inv);

		Vec relPosLocal = selfInv.rotMat.Dot(pbInv.pos - selfInv.pos) * POS_COEF;
		Vec relVelLocal = selfInv.rotMat.Dot(pbInv.vel - selfInv.vel) * VEL_COEF;

		obs += relPosLocal;
		obs += relVelLocal;
	}
}

FList ByteObsPadded::BuildObs(const Player& player, const GameState& state) {
	FList obs = {};

	const bool inv = player.team == Team::ORANGE;

	// Ball (absolute, inverted)
	auto ballInv = InvertPhys(state.ball, inv);

	obs += ballInv.pos * POS_COEF;
	obs += ballInv.vel * VEL_COEF;
	obs += ballInv.angVel * ANG_VEL_COEF;

	// Prediction setup
	// max horizon time = dtPolicy * 32
	float maxT = 0.f;
	if (state.lastArena) {
		float tickRate = state.lastArena->GetTickRate();
		float dtPolicy = (float)tickSkip / tickRate;
		maxT = dtPolicy * 32.f;
	}
	EnsurePredTracker(state, maxT);

	// Precompute selfInv for local projections
	auto selfInv = InvertPhys(player, inv);

	// Ball prediction (self-local)
	AddBallPredictionToObs(obs, player, selfInv, inv);

	// Previous action (keep if you’re still using it)
	for (int i = 0; i < player.prevAction.ELEM_AMOUNT; i++)
		obs += player.prevAction[i];

	// Boost pads (same encoding you already like)
	auto& pads = state.GetBoostPads(inv);
	auto& padTimers = state.GetBoostPadTimers(inv);
	for (int i = 0; i < CommonValues::BOOST_LOCATIONS_AMOUNT; i++) {
		if (pads[i]) obs += 1.f;
		else         obs += 1.f / (1.f + padTimers[i]);
	}

	// Active counts (normalized)
	int activeTm = 0, activeOp = 0;
	for (auto& p : state.players) {
		if (p.carId == player.carId) continue;
		if (p.team == player.team) activeTm++;
		else activeOp++;
	}
	obs += (float)activeTm / 2.f; // max teammates excluding self in 3v3
	obs += (float)activeOp / 3.f; // max opponents in 3v3

	// Self block
	FList selfBlock = {};
	AddPlayerToObs(selfBlock, player, inv, ballInv);
	const int playerSize = (int)selfBlock.size();
	obs += selfBlock;

	// Goal vectors in self-local frame (inverted already via phys)
	const bool isOrange = (player.team == Team::ORANGE);
	const Vec ownGoal = isOrange ? CommonValues::ORANGE_GOAL_CENTER : CommonValues::BLUE_GOAL_CENTER;
	const Vec oppGoal = isOrange ? CommonValues::BLUE_GOAL_CENTER : CommonValues::ORANGE_GOAL_CENTER;

	// toOwnGoal, toOppGoal (self-local)
	obs += selfInv.rotMat.Dot(ownGoal - selfInv.pos) * POS_COEF;
	obs += selfInv.rotMat.Dot(oppGoal - selfInv.pos) * POS_COEF;

	// Build other player blocks
	struct Slot {
		FList data;
		float key;       // distance to predicted ball at sort horizon
		uint32_t tie;    // per-episode tie-break
	};

	std::vector<Slot> tms;
	std::vector<Slot> ops;

	// Sorting ball reference (predicted at sortStepMult)
	PhysState sortBall = ballInv;
	if (predTracker && state.lastArena) {
		float tickRate = state.lastArena->GetTickRate();
		float dtPolicy = (float)tickSkip / tickRate;
		float t = dtPolicy * (float)sortStepMult;
		auto pb = predTracker->GetBallStateForTime(t);
		sortBall = InvertPhys(pb, inv);
	}

	auto getTie = [&](uint32_t carId) -> uint32_t {
		auto it = episodeKey.find(carId);
		if (it != episodeKey.end()) return it->second;
		uint32_t v = Mix(carId ^ episodeSeed);
		episodeKey[carId] = v;
		return v;
		};

	for (auto& p : state.players) {
		if (p.carId == player.carId) continue;

		FList b = {};
		AddPlayerToObs(b, p, inv, ballInv);

		auto pInv = InvertPhys(p, inv);
		float distSqToSortBall = pInv.pos.DistSq(sortBall.pos);

		Slot s{ std::move(b), distSqToSortBall, getTie(p.carId) };
		if (p.team == player.team) tms.push_back(std::move(s));
		else                       ops.push_back(std::move(s));
	}

	auto slotSort = [](const Slot& a, const Slot& b) {
		if (a.key != b.key) return a.key < b.key;
		return a.tie < b.tie;
		};

	std::sort(tms.begin(), tms.end(), slotSort);
	std::sort(ops.begin(), ops.end(), slotSort);

	// Pad teammates to 2, opponents to 3
	auto padTo = [&](std::vector<Slot>& list, int target) {
		while ((int)list.size() < target) {
			Slot s;
			s.data = FList(playerSize);
			std::fill(s.data.begin(), s.data.end(), 0.f);
			s.key = 0.f;
			s.tie = 0;
			list.push_back(std::move(s));
		}
		};

	padTo(tms, MAX_TM);
	padTo(ops, MAX_OPP);

	for (auto& s : tms) obs += s.data;
	for (auto& s : ops) obs += s.data;

	return obs;
}
