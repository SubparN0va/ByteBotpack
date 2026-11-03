#pragma once

#include "ActionSamplingConfig.h"
#include "SamplingDiagnostics.h"
#include <torch/types.h>
#include <vector>

namespace GGL::Infer {

    /**
     * Produces normalized probabilities for batched [players, actions] logits.
     * Illegal and filtered actions retain exactly zero probability.
     **/
    torch::Tensor FilterActionProbabilities(
        const torch::Tensor& logits,
        const torch::Tensor& actionMasks,
        const ActionSamplingConfig& config,
        SamplingDiagnostics* diagnostics = nullptr,
        std::vector<SamplingDiagnostics>* rowDiagnostics = nullptr);
}
