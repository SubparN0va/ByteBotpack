#include "InferUnit.h"

#include <GigaLearnCPP/Models.h>
#include <GigaLearnCPP/InferenceModels.h>

GGL::InferUnit::InferUnit(
	RLGC::ObsBuilder* obsBuilder, int obsSize, RLGC::ActionParser* actionParser,
	InferPartialModelConfig sharedHeadConfig, InferPartialModelConfig policyConfig,
	std::filesystem::path modelsFolder, bool useGPU) :
	obsBuilder(obsBuilder), obsSize(obsSize), actionParser(actionParser), useGPU(useGPU) {

	this->models = std::make_unique<ModelSet>();

    // Startup catches load failures before connecting to RLBot.
    GGL::Infer::MakeInferenceModels(obsSize, actionParser->GetActionAmount(),
        sharedHeadConfig, policyConfig, useGPU ? torch::kCUDA : torch::kCPU, *models);
    models->Load(modelsFolder, false, false);
    // Run the actual policy once and synchronize so unsupported GPUs fail at startup.
    RG_NO_GRAD;
    const auto options = torch::TensorOptions().device(useGPU ? torch::kCUDA : torch::kCPU);
    auto obs = torch::zeros({1, obsSize}, options);
    auto mask = torch::ones({1, actionParser->GetActionAmount()}, options.dtype(torch::kBool));
    torch::Tensor actions;
    GGL::Infer::InferActions(*models, obs, mask, true, {}, false, &actions, nullptr);
    actions.cpu();
}

RLGC::Action GGL::InferUnit::InferAction(
	const RLGC::Player& player,
	const RLGC::GameState& state,
	bool deterministic,
	const ActionSamplingConfig& sampling,
    Infer::SamplingDiagnostics* diagnostics
) {
	return BatchInferActions({ player }, { state }, deterministic, sampling, diagnostics)[0];
}

std::vector<RLGC::Action> GGL::InferUnit::BatchInferActions(
	const std::vector<RLGC::Player>& players,
	const std::vector<RLGC::GameState>& states,
	bool deterministic,
	const ActionSamplingConfig& sampling,
    Infer::SamplingDiagnostics* diagnostics
) {
	RG_ASSERT(players.size() > 0 && states.size() > 0);
	RG_ASSERT(players.size() == states.size());

	int batchSize = (int)players.size();
	std::vector<float> allObs;
	std::vector<uint8_t> allActionMasks;

	for (int i = 0; i < batchSize; i++) {
		FList curObs = obsBuilder->BuildObs(players[i], states[i]);
		if ((int)curObs.size() != obsSize) {
			RG_ERR_CLOSE(
				"InferUnit: Obs builder produced an obs that differs from the provided size (expected: " << obsSize << ", got: " << curObs.size() << ")\n"
				"Make sure you provided the correct obs size to the InferUnit constructor.\n"
				"Also, make sure there aren't an incorrect number of players (there are " << states[i].players.size() << " in this state)"
			);
		}
		allObs += curObs;
		allActionMasks += actionParser->GetActionMask(players[i], states[i]);
	}

	std::vector<RLGC::Action> results;

	try {
		RG_NO_GRAD;

		auto device = useGPU ? torch::kCUDA : torch::kCPU;

		auto tObs = torch::tensor(allObs).reshape({ (int64_t)batchSize, (int64_t)obsSize }).to(device);
		auto tMasks = torch::tensor(allActionMasks).reshape({ (int64_t)batchSize, (int64_t)actionParser->GetActionAmount() }).to(device);

		torch::Tensor tActions, tLogProbs;

		GGL::Infer::InferActions(
			*models,
			tObs,
			tMasks,
			deterministic,
			sampling,
			/*halfPrec*/ false,
			&tActions,
			&tLogProbs,
            diagnostics
		);

		auto actionIndices = TENSOR_TO_VEC<int>(tActions);

		for (int i = 0; i < batchSize; i++)
			results.push_back(actionParser->ParseAction(actionIndices[i], players[i], states[i]));

	}
	catch (std::exception& e) {
		RG_ERR_CLOSE("InferUnit: Exception when inferring model: " << e.what());
	}

	return results;
}


std::vector<RLGC::Action> GGL::InferUnit::BatchInferActions(
    const std::vector<InferenceRequest>& requests, const RLGC::GameState& state,
    std::vector<Infer::SamplingDiagnostics>* diagnostics) {
    if (requests.empty()) return {};
    RG_NO_GRAD;
    const auto device = useGPU ? torch::kCUDA : torch::kCPU;
    const int64_t count = requests.size();
    std::vector<float> observations;
    std::vector<uint8_t> masks;
    for (const auto& request : requests) {
        const auto& player = state.players.at(request.index);
        const auto obs = obsBuilder->BuildObs(player, state);
        TORCH_CHECK(obs.size() == obsSize, "Unexpected observation size");
        observations.insert(observations.end(), obs.begin(), obs.end());
        const auto mask = actionParser->GetActionMask(player, state);
        TORCH_CHECK(mask.size() == actionParser->GetActionAmount(), "Unexpected action mask size");
        masks.insert(masks.end(), mask.begin(), mask.end());
    }
    auto features = torch::tensor(observations).reshape({count, obsSize}).to(device);
    auto actionMasks = torch::tensor(masks).reshape({count, actionParser->GetActionAmount()}).to(device);
    if ((*models)["shared_head"]) features = (*models)["shared_head"]->Forward(features, false);
    auto logits = (*models)["policy"]->Forward(features, false).view({count, actionParser->GetActionAmount()});
    auto selected = torch::empty({count}, torch::TensorOptions().dtype(torch::kInt64).device(device));
    if (diagnostics) diagnostics->assign(count, {});
    std::vector<bool> processed(count, false);
    for (size_t first = 0; first < requests.size(); ++first) {
        if (processed[first]) continue;
        const auto& a = requests[first];
        std::vector<int64_t> group;
        for (size_t j = first; j < requests.size(); ++j) {
            const auto& b = requests[j];
            if (!processed[j] && a.deterministic == b.deterministic && a.diagnostics == b.diagnostics &&
                a.sampling.temperature == b.sampling.temperature && a.sampling.topK == b.sampling.topK &&
                a.sampling.minRelativeProbability == b.sampling.minRelativeProbability &&
                a.sampling.bestActionProbability == b.sampling.bestActionProbability) {
                processed[j] = true; group.push_back(j);
            }
        }
        const auto rows = torch::tensor(group).to(device);
        std::vector<Infer::SamplingDiagnostics> metrics;
        auto probabilities = Infer::FilterActionProbabilities(logits.index_select(0, rows),
            actionMasks.index_select(0, rows), a.sampling, nullptr,
            diagnostics && a.diagnostics ? &metrics : nullptr);
        auto choices = a.deterministic ? probabilities.argmax(1) : torch::multinomial(probabilities, 1, true).flatten();
        selected.index_copy_(0, rows, choices);
        for (size_t j = 0; j < metrics.size(); ++j) (*diagnostics)[group[j]] = metrics[j];
    }
    // One action readback after every car has been sampled independently.
    const auto indices = TENSOR_TO_VEC<int>(selected);
    std::vector<RLGC::Action> actions;
    for (size_t i = 0; i < requests.size(); ++i)
        actions.push_back(actionParser->ParseAction(indices[i], state.players[requests[i].index], state));
    return actions;
}

GGL::InferUnit::~InferUnit() = default;
