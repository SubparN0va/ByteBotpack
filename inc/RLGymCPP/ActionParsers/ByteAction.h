#pragma once

#include <RLGymCPP/ActionParsers/ActionParser.h>

namespace RLGC
{
	/**
	 * Byte's discrete action parser.
	 * Kept local to the bot so its parser lives beside the agent code.
	 **/
	class ByteAction : public ActionParser
	{
	public:
		std::vector<Action> actions;
		std::vector<uint8_t> groundMask, airMask, jumpMask, boostMask;

		ByteAction();

		Action ParseAction(int index, const Player& player, const GameState& state) override
		{
			return actions[index];
		}

		int GetActionAmount() override
		{
			return actions.size();
		}

		bool UsesActionMask()
		{
			return true;
		}

		std::vector<uint8_t> GetActionMask(const Player& player, const GameState& state) override;
		void GetActionMaskInto(const Player& player, const GameState& state, std::vector<uint8_t>& out);
	};
}