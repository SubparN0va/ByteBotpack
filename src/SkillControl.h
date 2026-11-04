#pragma once

#include "SkillSettings.h"
#include <GigaLearnCPP/SamplingDiagnostics.h>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <functional>

struct SkillTelemetry {
    std::string name;
    unsigned team = 0;
    int teammates = 1, opponents = 1;
    int teamScore = 0, opponentScore = 0;
    float effectiveLevel = 14.f;
    bool kickoff = false;
    SkillSettings settings;
    GGL::Infer::SamplingDiagnostics sampling;
    /** Sampling diagnostics do not apply while the drive-only kickoff is active. **/
    bool scriptedKickoff = false;
};

/** A match-config slot, not a game-packet index; editing never changes active skill. **/
struct SkillRow {
    int playerId = 0;
    unsigned team = 0;
    std::string name;
    int active = DefaultSkillLevel;
    SkillChoice pending;
    SkillChoiceMode activeMode = SkillChoiceMode::SelectSkill;
};

/** Shared control channel; the UI never touches models or game-state objects. **/
struct SkillControl {
    std::atomic<int> selectedLevel {DefaultSkillLevel};
    SkillChoice selectedChoice;
    std::atomic<bool> debug {false};
    std::mutex mutex;
    std::map<unsigned, SkillTelemetry> telemetry;
    std::map<unsigned, SkillRow> rows;
    bool canRespawn = false, applying = false;
    std::string status = "Waiting for a match";
    std::function<void()> applyPending;
    /** Called by the selector, never by the game thread or Auto score feedback. **/
    std::function<std::string(SkillChoice)> saveSelection;
    std::string saveError;
    void Select(SkillChoice choice);
    void Publish(unsigned player, SkillTelemetry value);
};

/** Native rank panel. Closing the panel leaves a running bot alone. **/
class SkillWindow {
public:
    SkillWindow(std::shared_ptr<SkillControl> control, bool standalone = false);
    ~SkillWindow();
    void Wait();
    /** Pumps the Linux window on the main thread; false once it has closed. **/
    bool Pump();
    SkillWindow(const SkillWindow&) = delete;
    SkillWindow& operator=(const SkillWindow&) = delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
