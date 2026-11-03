#pragma once

#include <algorithm>
#include <cmath>

struct TemperatureControllerConfig {
    float initialTemperature = 3.f;
    float minTemperature = 0.01f;
    float maxTemperature = 5.f;

    float deltaInitial = 0.15f;
    float deltaMin = 0.01f;
    float deltaMax = 0.15f;

    float etaPlus = 1.35f;
    float etaMinus = 0.45f;

    // > 1 gives finer control near the high-temperature end
    // which is what you want if 3 -> 4 is too sensitive.
    float curveGamma = 2.2f;
};

class TemperatureController {
public:
    explicit TemperatureController(TemperatureControllerConfig config = {}) noexcept
        : m_config(config)
    {
        Reset();
    }

    void Reset() noexcept
    {
        constexpr float kMinPositive = 1e-6f;

        m_minTemperature = std::max(m_config.minTemperature, kMinPositive);
        m_maxTemperature = std::max(m_config.maxTemperature, m_minTemperature);

        const float initialTemperature =
            std::clamp(std::max(m_config.initialTemperature, kMinPositive), m_minTemperature, m_maxTemperature);

        m_skillPos = SkillPosFromTemperature(initialTemperature);

        m_delta = m_config.deltaInitial;
        m_prevResult = 0;

        RG_LOG("Temperature: " << CurrentTemperature() << "\n");
    }

    float Update(int result) noexcept
    {
        if (m_prevResult != 0) {
            if (result == m_prevResult) {
                m_delta = std::min(m_delta * m_config.etaPlus, m_config.deltaMax);
            }
            else {
                m_delta = std::max(m_delta * m_config.etaMinus, m_config.deltaMin);
            }
        }

        m_prevResult = result;

        // Assumes:
        // result > 0  => increase temperature
        // result < 0  => decrease temperature
        m_skillPos += static_cast<float>(result) * m_delta;
        m_skillPos = std::clamp(m_skillPos, 0.f, 1.f);

        const float currentTemperature = CurrentTemperature();
        RG_LOG("Temperature: " << currentTemperature);

        return currentTemperature;
    }

    float CurrentTemperature() const noexcept
    {
        return TemperatureFromSkillPos(m_skillPos);
    }

private:
    float TemperatureFromSkillPos(float x) const noexcept
    {
        x = std::clamp(x, 0.f, 1.f);

        // gamma > 1 compresses the high end so you get finer control
        // near maxTemperature
        const float curved = 1.f - std::pow(1.f - x, m_config.curveGamma);

        return m_minTemperature + (m_maxTemperature - m_minTemperature) * curved;
    }

    float SkillPosFromTemperature(float temperature) const noexcept
    {
        const float range = m_maxTemperature - m_minTemperature;
        if (range <= 1e-6f)
            return 0.f;

        const float y = std::clamp((temperature - m_minTemperature) / range, 0.f, 1.f);

        // Inverse of:
        // y = 1 - (1 - x)^gamma
        if (m_config.curveGamma <= 1e-6f)
            return y;

        return 1.f - std::pow(1.f - y, 1.f / m_config.curveGamma);
    }

private:
    TemperatureControllerConfig m_config;

    float m_minTemperature = 0.f;
    float m_maxTemperature = 0.f;

    // Normalized controller position in [0, 1]
    float m_skillPos = 0.f;

    float m_delta = 0.0f;
    int m_prevResult = 0;
};