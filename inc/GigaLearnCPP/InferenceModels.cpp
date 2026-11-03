#include <GigaLearnCPP/InferenceModels.h>

#include <GigaLearnCPP/Models.h>
#include "ActionSampling.h"

namespace {

	// Copied/minimized from PPOLearner::InferPolicyProbsFromModels
	torch::Tensor InferPolicyProbsFromModels(
		GGL::ModelSet& models,
		torch::Tensor obs,
		torch::Tensor actionMasks,
		const GGL::ActionSamplingConfig& sampling,
		bool halfPrec,
        GGL::Infer::SamplingDiagnostics* diagnostics
	) {
		if (models["shared_head"])
			obs = models["shared_head"]->Forward(obs, halfPrec);

		auto logits = models["policy"]->Forward(obs, halfPrec)
			.view({ -1, models["policy"]->config.numOutputs });
		return GGL::Infer::FilterActionProbabilities(logits, actionMasks, sampling, diagnostics);
	}

} // anonymous namespace

namespace GGL::Infer {

	void MakeInferenceModels(
		int obsSize,
		int numActions,
		const InferPartialModelConfig& sharedHeadConfig,
		const InferPartialModelConfig& policyConfig,
		torch::Device device,
		ModelSet& outModels
	) {
		// Policy: obs -> logits(numActions)
		ModelConfig fullPolicyConfig = policyConfig;
		fullPolicyConfig.numInputs = obsSize;
		fullPolicyConfig.numOutputs = numActions;

		if (sharedHeadConfig.IsValid()) {
			ModelConfig fullSharedHeadConfig = sharedHeadConfig;
			fullSharedHeadConfig.numInputs = obsSize;
			fullSharedHeadConfig.numOutputs = 0;

			RG_ASSERT(!sharedHeadConfig.addOutputLayer);

			int featSize = fullSharedHeadConfig.layerSizes.back();
			fullPolicyConfig.numInputs = featSize;

			outModels.Add(new Model("shared_head", fullSharedHeadConfig, device));
		}

		outModels.Add(new Model("policy", fullPolicyConfig, device));
	}

	void InferActions(
		ModelSet& models,
		torch::Tensor obs,
		torch::Tensor actionMasks,
		bool deterministic,
		const GGL::ActionSamplingConfig& sampling,
		bool halfPrec,
		torch::Tensor* outActions,
		torch::Tensor* outLogProbs,
        SamplingDiagnostics* diagnostics
	) {
		auto probs = InferPolicyProbsFromModels(models, obs, actionMasks, sampling, halfPrec, diagnostics);

		if (deterministic) {
			auto action = probs.argmax(1);
			if (outActions)  *outActions = action.flatten();
			if (outLogProbs) *outLogProbs = torch::Tensor(); // empty
		}
		else {
			auto action = torch::multinomial(probs, 1, true);
			auto logProb = torch::log(probs).gather(-1, action);

			if (outActions)  *outActions = action.flatten();
			if (outLogProbs) *outLogProbs = logProb.flatten();
		}
	}

} // namespace GGL::Infer
