#pragma once

namespace GGL::Infer {

    /** Diagnostics before and after filtering, for one car or a requested batch mean. **/
    struct SamplingDiagnostics {
        float policyBestProbability = 0.f;
        float normalizedEntropy = 0.f;
        float retainedActions = 0.f;
        float finalBestProbability = 0.f;
    };
}
