#pragma once

namespace GGL {

    /** Restricts candidate actions before applying temperature. **/
    struct ActionSamplingConfig {
        float temperature = 1.f;
        /** Zero disables the candidate-count limit. **/
        int topK = 0;
        /** Minimum probability relative to the best legal action, at temperature 1. **/
        float minRelativeProbability = 0.f;
        /** Negative disables allocation; otherwise reserve this share for the best legal action. **/
        float bestActionProbability = -1.f;
    };
}
