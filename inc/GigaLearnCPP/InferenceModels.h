#pragma once

#include <torch/torch.h>
#include "ActionSamplingConfig.h"
#include "ActionSampling.h"
#include <GigaLearnCPP/InferenceModelConfig.h>

namespace GGL {
	class ModelSet;
}

namespace GGL::Infer {

	// Builds only what inference needs: shared_head + policy
	void MakeInferenceModels(
		int obsSize,
		int numActions,
		const InferPartialModelConfig& sharedHeadConfig,
		const InferPartialModelConfig& policyConfig,
		torch::Device device,
		ModelSet& outModels
	);

	void InferActions(
		ModelSet& models,
		torch::Tensor obs,
		torch::Tensor actionMasks,
		bool deterministic,
		const ActionSamplingConfig& sampling,
		bool halfPrec,
		torch::Tensor* outActions,
		torch::Tensor* outLogProbs,
        SamplingDiagnostics* diagnostics = nullptr
	);

} // namespace GGL::Infer
