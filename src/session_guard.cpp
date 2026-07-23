#include "palverify/session_guard.hpp"

#include <algorithm>
#include <utility>

namespace palverify {

SessionGuard::SessionGuard(SessionConfig config, ChallengeGenerator generator)
    : config_{std::move(config)}, challenge_generator_{std::move(generator)}
{
}

auto SessionGuard::begin(
    std::string player_id,
    ClientPlatform platform,
    double now_seconds
) -> Decision
{
    if (verification_requirement(platform) == VerificationRequirement::Exempt) {
        sessions_.insert_or_assign(
            player_id,
            Session{
                .platform = platform,
                .state = SessionState::Exempt,
                .challenge = {},
                .deadline_seconds = now_seconds,
                .last_heartbeat_seconds = now_seconds,
                .highest_sequence = 0,
            }
        );
        return {
            .kind = DecisionKind::Allow,
            .reason = DecisionReason::ConsoleExempt,
            .challenge = std::nullopt,
        };
    }

    auto challenge = challenge_generator_();
    sessions_.insert_or_assign(
        player_id,
        Session{
            .platform = platform,
            .state = SessionState::AwaitingVerification,
            .challenge = challenge,
            .deadline_seconds =
                now_seconds + config_.verification_timeout_seconds,
            .last_heartbeat_seconds = now_seconds,
            .highest_sequence = 0,
        }
    );
    return {
        .kind = DecisionKind::Challenge,
        .reason = DecisionReason::ChallengeIssued,
        .challenge = std::move(challenge),
    };
}

auto SessionGuard::submit_proof(
    std::string_view player_id,
    std::string_view challenge,
    std::string_view protocol_version,
    bool proof_valid,
    double now_seconds
) -> Decision
{
    const auto found = sessions_.find(std::string{player_id});
    if (found == sessions_.end()) {
        return {
            .kind = DecisionKind::Kick,
            .reason = DecisionReason::UnknownSession,
            .challenge = std::nullopt,
        };
    }

    auto& session = found->second;
    if (session.state == SessionState::Exempt) {
        return {
            .kind = DecisionKind::Allow,
            .reason = DecisionReason::ConsoleExempt,
            .challenge = std::nullopt,
        };
    }
    if (session.state != SessionState::AwaitingVerification) {
        return {
            .kind = DecisionKind::Kick,
            .reason = DecisionReason::ReplayDetected,
            .challenge = std::nullopt,
        };
    }
    if (now_seconds >= session.deadline_seconds) {
        return {
            .kind = DecisionKind::Kick,
            .reason = DecisionReason::VerificationTimeout,
            .challenge = std::nullopt,
        };
    }
    if (!accepts_version(protocol_version)) {
        return {
            .kind = DecisionKind::Kick,
            .reason = DecisionReason::UnsupportedVersion,
            .challenge = std::nullopt,
        };
    }
    if (challenge != session.challenge) {
        return {
            .kind = DecisionKind::Kick,
            .reason = DecisionReason::InvalidChallenge,
            .challenge = std::nullopt,
        };
    }
    if (!proof_valid) {
        return {
            .kind = DecisionKind::Kick,
            .reason = DecisionReason::InvalidProof,
            .challenge = std::nullopt,
        };
    }

    session.state = SessionState::Verified;
    session.challenge.clear();
    session.last_heartbeat_seconds = now_seconds;
    session.highest_sequence = 0;
    return {
        .kind = DecisionKind::Allow,
        .reason = DecisionReason::Verified,
        .challenge = std::nullopt,
    };
}

auto SessionGuard::heartbeat(
    std::string_view player_id,
    std::uint64_t sequence,
    std::string_view protocol_version,
    bool proof_valid,
    double now_seconds
) -> Decision
{
    const auto found = sessions_.find(std::string{player_id});
    if (found == sessions_.end()) {
        return {
            .kind = DecisionKind::Kick,
            .reason = DecisionReason::UnknownSession,
            .challenge = std::nullopt,
        };
    }

    auto& session = found->second;
    if (session.state != SessionState::Verified) {
        return {
            .kind = DecisionKind::Kick,
            .reason = DecisionReason::InvalidProof,
            .challenge = std::nullopt,
        };
    }
    if (now_seconds
        >= session.last_heartbeat_seconds
            + config_.heartbeat_timeout_seconds) {
        return {
            .kind = DecisionKind::Kick,
            .reason = DecisionReason::HeartbeatTimeout,
            .challenge = std::nullopt,
        };
    }
    if (!accepts_version(protocol_version)) {
        return {
            .kind = DecisionKind::Kick,
            .reason = DecisionReason::UnsupportedVersion,
            .challenge = std::nullopt,
        };
    }
    if (!proof_valid) {
        return {
            .kind = DecisionKind::Kick,
            .reason = DecisionReason::InvalidProof,
            .challenge = std::nullopt,
        };
    }
    if (sequence <= session.highest_sequence) {
        return {
            .kind = DecisionKind::Kick,
            .reason = DecisionReason::ReplayDetected,
            .challenge = std::nullopt,
        };
    }

    session.highest_sequence = sequence;
    session.last_heartbeat_seconds = now_seconds;
    return {
        .kind = DecisionKind::Allow,
        .reason = DecisionReason::HeartbeatAccepted,
        .challenge = std::nullopt,
    };
}

auto SessionGuard::evaluate(double now_seconds) const
    -> std::vector<KickDecision>
{
    std::vector<KickDecision> kicks;
    for (const auto& [player_id, session] : sessions_) {
        if (session.state == SessionState::AwaitingVerification
            && now_seconds >= session.deadline_seconds) {
            kicks.push_back({
                .player_id = player_id,
                .reason = DecisionReason::VerificationTimeout,
            });
        } else if (
            session.state == SessionState::Verified
            && now_seconds
                >= session.last_heartbeat_seconds
                    + config_.heartbeat_timeout_seconds
        ) {
            kicks.push_back({
                .player_id = player_id,
                .reason = DecisionReason::HeartbeatTimeout,
            });
        }
    }

    std::ranges::sort(kicks, {}, &KickDecision::player_id);
    return kicks;
}

void SessionGuard::disconnect(std::string_view player_id)
{
    sessions_.erase(std::string{player_id});
}

auto SessionGuard::state(std::string_view player_id) const
    -> std::optional<SessionState>
{
    const auto found = sessions_.find(std::string{player_id});
    if (found == sessions_.end()) {
        return std::nullopt;
    }
    return found->second.state;
}

auto SessionGuard::accepts_version(std::string_view protocol_version) const
    -> bool
{
    return std::ranges::find(
               config_.accepted_protocol_versions,
               protocol_version
           )
        != config_.accepted_protocol_versions.end();
}

}  // namespace palverify
