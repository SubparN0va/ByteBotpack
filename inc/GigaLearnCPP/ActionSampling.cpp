#include "ActionSampling.h"

#include <algorithm>
#include <cmath>
#include <limits>

torch::Tensor GGL::Infer::FilterActionProbabilities(
    const torch::Tensor& logits,
    const torch::Tensor& actionMasks,
    const ActionSamplingConfig& config,
    SamplingDiagnostics* diagnostics,
    std::vector<SamplingDiagnostics>* rowDiagnostics) {
    TORCH_CHECK(std::isfinite(config.temperature) && config.temperature > 0.f,
        "Action temperature must be finite and positive");
    TORCH_CHECK(std::isfinite(config.bestActionProbability) &&
        (config.bestActionProbability == -1.f || (config.bestActionProbability >= 0.f && config.bestActionProbability <= 1.f)),
        "Best-action probability must be -1 (disabled) or in [0, 1]");
    TORCH_CHECK(config.topK >= 0, "Action topK must be nonnegative");
    TORCH_CHECK(std::isfinite(config.minRelativeProbability) &&
        config.minRelativeProbability >= 0.f && config.minRelativeProbability <= 1.f,
        "Minimum relative probability must be in [0, 1]");
    TORCH_CHECK(logits.dim() == 2 && logits.size(0) > 0 && logits.size(1) > 0 &&
        logits.sizes() == actionMasks.sizes(), "Expected matching [players, actions] tensors");

    auto legal = actionMasks.to(torch::kBool);
    TORCH_CHECK(legal.any(-1).all().item<bool>(), "Every player needs a legal action");
    TORCH_CHECK((torch::isfinite(logits) | legal.logical_not()).all().item<bool>(),
        "Legal action logits must be finite");

    const float negativeInfinity = -std::numeric_limits<float>::infinity();
    auto maskedLogits = logits.to(torch::kFloat32).masked_fill(legal.logical_not(), negativeInfinity);
    auto bestLogit = std::get<0>(maskedLogits.max(-1, true));
    auto bestIndices = maskedLogits.argmax(-1, true);
    auto eligible = legal.clone();

    // p(action) / p(best) = exp(logit(action) - logit(best)) at temperature 1.
    // Filter before temperature so extra randomness cannot revive implausible actions.
    if (config.minRelativeProbability > 0.f) {
        eligible &= (maskedLogits - bestLogit) >= std::log(config.minRelativeProbability);
    }

    if (config.topK > 0) {
        const auto count = std::min<int64_t>(config.topK, logits.size(1));
        auto topMask = torch::zeros_like(legal).scatter_(-1, bestIndices, true);
        if (count > 1) {
            auto others = maskedLogits.clone().scatter_(-1, bestIndices, negativeInfinity);
            auto indices = std::get<1>(others.topk(count - 1, -1));
            topMask.scatter_(-1, indices, true);
        }
        eligible &= topMask;
    }

    // The best legal action always survives both filters. Center first for stability.
    auto scaled = ((maskedLogits - bestLogit) / config.temperature)
        .masked_fill(eligible.logical_not(), negativeInfinity);
    auto probabilities = torch::softmax(scaled, -1);
    if (config.bestActionProbability >= 0.f) {
        // A fixed best-action share bounds uncertainty-driven differences between modes.
        // Normalize alternatives in log space so very confident rows do not underflow.
        auto alternatives = eligible.clone().scatter_(-1, bestIndices, false);
        auto hasAlternative = alternatives.any(-1, true);
        auto alternativeLogits = scaled.masked_fill(alternatives.logical_not(), negativeInfinity);
        alternativeLogits = torch::where(hasAlternative, alternativeLogits, torch::zeros_like(scaled));
        auto otherProbs = torch::softmax(alternativeLogits, -1) * hasAlternative;
        auto bestShare = torch::where(hasAlternative,
            torch::full_like(bestLogit, config.bestActionProbability), torch::ones_like(bestLogit));
        probabilities = otherProbs * (1.f - config.bestActionProbability);
        probabilities.scatter_(-1, bestIndices, bestShare);
    }
    if (diagnostics || rowDiagnostics) {
        auto original = torch::softmax(maskedLogits - bestLogit, -1);
        auto entropy = -(original * original.clamp_min(1e-30f).log()).sum(-1);
        auto maximumEntropy = legal.sum(-1).to(torch::kFloat32).clamp_min(2).log();
        // Transfer all four metrics together, retaining per-car values in batches.
        auto metrics = torch::stack({original.gather(-1, bestIndices).squeeze(-1),
            entropy / maximumEntropy, eligible.sum(-1).to(torch::kFloat32),
            probabilities.gather(-1, bestIndices).squeeze(-1)}, 1).cpu().contiguous();
        const auto rows = metrics.accessor<float, 2>();
        if (rowDiagnostics) rowDiagnostics->resize(logits.size(0));
        SamplingDiagnostics average{};
        for (int64_t i = 0; i < logits.size(0); ++i) {
            SamplingDiagnostics row{rows[i][0], rows[i][1], rows[i][2], rows[i][3]};
            if (rowDiagnostics) (*rowDiagnostics)[i] = row;
            average.policyBestProbability += row.policyBestProbability / logits.size(0);
            average.normalizedEntropy += row.normalizedEntropy / logits.size(0);
            average.retainedActions += row.retainedActions / logits.size(0);
            average.finalBestProbability += row.finalBestProbability / logits.size(0);
        }
        if (diagnostics) *diagnostics = average;
    }
    return probabilities;
}
