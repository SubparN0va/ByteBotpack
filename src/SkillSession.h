#pragma once

#include "SkillControl.h"
#include <rlbot/Client.h>
#include <chrono>
#include <filesystem>
class SkillConfigProbe;

/** Builds a continued match with new identities only for this package's Kilobyte cars. **/
rlbot::flat::MatchConfigurationT RankRespawnConfiguration(
    const rlbot::flat::MatchConfigurationT& current, const std::map<unsigned, SkillRow>& rows,
    const std::string& agentId, const std::string& generation);

/** Observer connection shared by both teams; it never owns a car or runs inference. **/
class SkillSession final : public rlbot::Client {
public:
    SkillSession(std::shared_ptr<SkillControl> control, std::filesystem::path package, std::string agentId, const char* environmentSkill = nullptr);
    ~SkillSession() override;
    void Apply();
    void Tick();
    // Public for serialized protocol tests without connecting to a live game.
    void handleMatchConfiguration(const rlbot::flat::MatchConfiguration* packet) noexcept override;
    void handleGamePacket(const rlbot::flat::GamePacket* packet) noexcept override;
    void handleMatchComm(const rlbot::flat::MatchComm* packet) noexcept override;
private:
    std::shared_ptr<SkillControl> control;
    std::filesystem::path package;
    std::string agentId;
    /** Snapshot of the controller's inherited rank override, independent of saved defaults. **/
    std::optional<std::string> environmentSkill;
    std::unique_ptr<rlbot::flat::MatchConfigurationT> configuration;
    std::map<unsigned, std::string> expectedAgents;
    std::chrono::steady_clock::time_point lastPacket{}, requestedAt{};
    std::chrono::steady_clock::time_point lastProbe{};
    std::unique_ptr<SkillConfigProbe> probe;
    std::vector<uint8_t> configurationBytes;
    bool needConfiguration = false;
};

/** Telemetry is sent over RLBot match comms with no quick-chat display text. **/
std::string EncodeSkillTelemetry(int playerId, const SkillTelemetry& telemetry);
bool DecodeSkillTelemetry(const std::string& message, int& playerId, SkillTelemetry& telemetry);

/** Starts a singleton observer window, without interrupting a match. **/
void LaunchSkillController(const std::filesystem::path& executable, bool debug);
int RunSkillController(const std::filesystem::path& package, const std::string& agentId, bool debug);
