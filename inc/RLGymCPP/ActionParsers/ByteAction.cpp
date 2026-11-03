#include "ByteAction.h"



RLGC::ByteAction::ByteAction()
{
	constexpr float
		R_B[] = { 0, 1 },
		R_F[] = { -1, 0, 1 };

	actions.clear();

	// Ground
	for (float throttle : R_F)
	{
		for (float steer : R_F)
		{
			for (float boost : R_B)
			{
				for (float handbrake : R_B)
				{
					if (boost == 1 && throttle != 1)
						continue;

					actions.emplace_back(
						throttle, steer, 0, steer, 0, 0, boost, handbrake
					);
				}
			}
		}
	}

	const size_t numGroundActions = actions.size();

	// Aerial
	for (float pitch : R_F)
	{
		for (float yaw : R_F)
		{
			for (float roll : R_F)
			{
				for (float jump : R_B)
				{
					for (float boost : R_B)
					{
						if (jump == 1 && yaw != 0)
							continue;

						if (pitch == roll && roll == jump && jump == 0)
							continue;

						float handbrake = (jump == 1) && (pitch != 0 || yaw != 0 || roll != 0);

						actions.emplace_back(
							boost, yaw, pitch, yaw, roll, jump, boost, handbrake
						);
					}
				}
			}
		}
	}

	const size_t n = actions.size();
	groundMask.assign(n, 0);
	airMask.assign(n, 0);
	jumpMask.assign(n, 0);
	boostMask.assign(n, 0);

	for (size_t i = 0; i < n; i++)
	{
		Action& action = actions[i];

		if (action.jump)
			jumpMask[i] = 1;

		if (action.boost)
			boostMask[i] = 1;

		if (i < numGroundActions)
			groundMask[i] = 1;

		// Byte was trained against the legacy DefaultAction mask, which skips
		// the first aerial action while airborne because of this strict boundary.
		if (i > numGroundActions && !action.jump)
			airMask[i] = 1;

		if (i < numGroundActions)
		{
			if (action.throttle == action.boost && (action.yaw != 0) == action.handbrake)
				airMask[i] = 1;
		}
	}
}

std::vector<uint8_t> RLGC::ByteAction::GetActionMask(const Player& player, const GameState& state)
{
	std::vector<uint8_t> result;
	GetActionMaskInto(player, state, result);
	return result;
}

void RLGC::ByteAction::GetActionMaskInto(const Player& player, const GameState& state, std::vector<uint8_t>& result)
{
	(void)state;

	result = player.isOnGround ? groundMask : airMask;

	if (player.boost <= 0.f) {
		for (size_t i = 0; i < actions.size(); i++)
			if (boostMask[i])
				result[i] = 0;
	}

	bool isTurtled = player.worldContact.hasContact && player.worldContact.contactNormal.z > 0.9f;
	if (player.HasFlipOrJump() || isTurtled) {
		for (size_t i = 0; i < actions.size(); i++)
			if (jumpMask[i])
				result[i] = 1;
	}
}
