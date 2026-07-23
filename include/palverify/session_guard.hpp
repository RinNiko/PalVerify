#pragma once

#include "palverify/platform_policy.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace palverify {

enum class SessionState {
    AwaitingVerification,
    Verified,
    Exempt,
};

enum class DecisionKind {
    Challenge,
    Allow,
    Kick,
};

enum class DecisionReason {
    ChallengeIssued,
    ConsoleExempt,
    Verified,
    HeartbeatAccepted,
    UnsupportedVersion,
    InvalidProof,
    InvalidChallenge,
    ReplayDetected,
    VerificationTimeout,
    HeartbeatTimeout,
    UnknownSession,
};

struct SessionConfig {
    double verification_timeout_seconds{10.0};
    double heartbeat_timeout_seconds{15.0};
    std::vector<std::string> accepted_protocol_versions;
};

struct Decision {
    DecisionKind kind;
    DecisionReason reason;
    std::optional<std::string> challenge;
};

struct KickDecision {
    std::string player_id;
    DecisionReason reason;
};

using ChallengeGenerator = std::function<std::string()>;

class SessionGuard {
public:
    SessionGuard(SessionConfig config, ChallengeGenerator challenge_generator);

    [[nodiscard]] auto begin(
        std::string player_id,
        ClientPlatform platform,
        double now_seconds
    ) -> Decision;

    [[nodiscard]] auto submit_proof(
        std::string_view player_id,
        std::string_view challenge,
        std::string_view protocol_version,
        bool proof_valid,
        double now_seconds
    ) -> Decision;

    [[nodiscard]] auto heartbeat(
        std::string_view player_id,
        std::uint64_t sequence,
        std::string_view protocol_version,
        bool proof_valid,
        double now_seconds
    ) -> Decision;

    [[nodiscard]] auto evaluate(double now_seconds) const
        -> std::vector<KickDecision>;

    void disconnect(std::string_view player_id);

    [[nodiscard]] auto state(std::string_view player_id) const
        -> std::optional<SessionState>;

private:
    struct Session {
        ClientPlatform platform;
        SessionState state;
        std::string challenge;
        double deadline_seconds;
        double last_heartbeat_seconds;
        std::uint64_t highest_sequence;
    };

    [[nodiscard]] auto accepts_version(std::string_view protocol_version) const
        -> bool;

    SessionConfig config_;
    ChallengeGenerator challenge_generator_;
    std::unordered_map<std::string, Session> sessions_;
};

}  // namespace palverify
