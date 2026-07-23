#include "palverify/platform_policy.hpp"
#include "palverify/process_rules.hpp"
#include "palverify/session_guard.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

class TestFailure final : public std::exception {
public:
    explicit TestFailure(std::string message) : message_{std::move(message)}
    {
    }

    [[nodiscard]] auto what() const noexcept -> const char* override
    {
        return message_.c_str();
    }

private:
    std::string message_;
};

void require(bool condition, std::string_view message)
{
    if (!condition) {
        throw TestFailure{std::string{message}};
    }
}

template <typename Actual, typename Expected>
void require_equal(
    const Actual& actual,
    const Expected& expected,
    std::string_view message
)
{
    if (actual != expected) {
        throw TestFailure{std::string{message}};
    }
}

struct TestCase {
    std::string_view name;
    void (*run)();
};

auto config() -> palverify::SessionConfig
{
    return {
        .verification_timeout_seconds = 10.0,
        .heartbeat_timeout_seconds = 15.0,
        .accepted_protocol_versions = {"1"},
    };
}

auto challenges() -> palverify::ChallengeGenerator
{
    return [next = 0]() mutable {
        ++next;
        return "challenge-" + std::to_string(next);
    };
}

void exact_console_hardware_is_exempt()
{
    using enum palverify::ClientPlatform;
    using palverify::VerificationRequirement;

    for (const auto platform : {
             PS5Base,
             PS5Trinity,
             XboxOne,
             XboxOneS,
             XboxOneX,
             XboxSeriesS,
             XboxSeriesX,
         }) {
        require_equal(
            palverify::verification_requirement(platform),
            VerificationRequirement::Exempt,
            "exact console hardware must be exempt"
        );
    }
}

void pc_mac_and_unknown_are_required()
{
    using enum palverify::ClientPlatform;
    using palverify::VerificationRequirement;

    for (const auto platform : {Unknown, Windows, WinGDK, Mac, Linux, PC}) {
        require_equal(
            palverify::verification_requirement(platform),
            VerificationRequirement::Required,
            "PC, Mac, and Unknown must require PalVerify"
        );
    }
}

void runtime_names_do_not_turn_generic_xbox_into_an_exemption()
{
    using palverify::ClientPlatform;

    require_equal(
        palverify::platform_from_runtime_name("WinGDK"),
        std::optional{ClientPlatform::WinGDK},
        "WinGDK mapping"
    );
    require_equal(
        palverify::platform_from_runtime_name("XSS"),
        std::optional{ClientPlatform::XboxSeriesS},
        "Xbox Series S mapping"
    );
    require_equal(
        palverify::platform_from_runtime_name("Xbox"),
        std::optional<ClientPlatform>{},
        "generic Xbox must not become exempt"
    );
}

void current_runtime_names_distinguish_ps5_models_and_linux()
{
    using palverify::VerificationRequirement;

    for (const auto runtime_name : {"PS5Base", "PS5Trinity"}) {
        const auto platform =
            palverify::platform_from_runtime_name(runtime_name);
        require(platform.has_value(), "PS5 model should map");
        require_equal(
            palverify::verification_requirement(*platform),
            VerificationRequirement::Exempt,
            "exact PS5 model should be exempt"
        );
    }

    const auto linux = palverify::platform_from_runtime_name("Linux");
    require(linux.has_value(), "Linux should map");
    require_equal(
        palverify::verification_requirement(*linux),
        VerificationRequirement::Required,
        "Linux client should require PalVerify"
    );

    require_equal(
        palverify::platform_from_runtime_name("PS5"),
        std::optional<palverify::ClientPlatform>{},
        "generic PS5 name must not be treated as a runtime model"
    );
}

void exempt_console_session_is_allowed_without_a_challenge()
{
    palverify::SessionGuard guard{config(), challenges()};
    const auto decision =
        guard.begin(
            "console-player",
            palverify::ClientPlatform::PS5Base,
            100.0
        );

    require_equal(
        decision.kind,
        palverify::DecisionKind::Allow,
        "PS5 should be allowed"
    );
    require_equal(
        decision.reason,
        palverify::DecisionReason::ConsoleExempt,
        "PS5 should use console exemption"
    );
    require(!decision.challenge.has_value(), "console should get no challenge");
    require_equal(
        guard.state("console-player"),
        std::optional{palverify::SessionState::Exempt},
        "console session state"
    );
}

void required_client_receives_a_challenge_and_can_verify()
{
    palverify::SessionGuard guard{config(), challenges()};
    const auto begin =
        guard.begin("pc-player", palverify::ClientPlatform::WinGDK, 100.0);

    require_equal(
        begin.kind,
        palverify::DecisionKind::Challenge,
        "WinGDK should receive challenge"
    );
    require(begin.challenge.has_value(), "challenge should be present");

    const auto verified = guard.submit_proof(
        "pc-player",
        *begin.challenge,
        "1",
        true,
        105.0
    );
    require_equal(
        verified.kind,
        palverify::DecisionKind::Allow,
        "valid proof should allow"
    );
    require_equal(
        guard.state("pc-player"),
        std::optional{palverify::SessionState::Verified},
        "session should be verified"
    );
}

void unsupported_version_is_rejected()
{
    palverify::SessionGuard guard{config(), challenges()};
    const auto begin =
        guard.begin("pc-player", palverify::ClientPlatform::Windows, 0.0);

    const auto rejected = guard.submit_proof(
        "pc-player",
        *begin.challenge,
        "2",
        true,
        1.0
    );
    require_equal(
        rejected.kind,
        palverify::DecisionKind::Kick,
        "unsupported version should kick"
    );
    require_equal(
        rejected.reason,
        palverify::DecisionReason::UnsupportedVersion,
        "unsupported version reason"
    );
}

void missing_initial_proof_times_out()
{
    palverify::SessionGuard guard{config(), challenges()};
    static_cast<void>(
        guard.begin("pc-player", palverify::ClientPlatform::Mac, 50.0)
    );

    require(guard.evaluate(59.9).empty(), "grace period should remain active");
    const auto kicks = guard.evaluate(60.0);
    require_equal(kicks.size(), std::size_t{1}, "one timed-out session");
    require_equal(
        kicks.front().reason,
        palverify::DecisionReason::VerificationTimeout,
        "initial timeout reason"
    );
}

void heartbeat_refreshes_a_verified_session_and_replay_is_rejected()
{
    palverify::SessionGuard guard{config(), challenges()};
    const auto begin =
        guard.begin("pc-player", palverify::ClientPlatform::Windows, 0.0);
    static_cast<void>(
        guard.submit_proof("pc-player", *begin.challenge, "1", true, 1.0)
    );

    const auto accepted = guard.heartbeat("pc-player", 1, "1", true, 10.0);
    require_equal(
        accepted.reason,
        palverify::DecisionReason::HeartbeatAccepted,
        "fresh heartbeat should be accepted"
    );
    require(guard.evaluate(24.9).empty(), "heartbeat should extend session");

    const auto replayed = guard.heartbeat("pc-player", 1, "1", true, 11.0);
    require_equal(
        replayed.kind,
        palverify::DecisionKind::Kick,
        "duplicate heartbeat sequence should kick"
    );
    require_equal(
        replayed.reason,
        palverify::DecisionReason::ReplayDetected,
        "duplicate sequence reason"
    );
}

void verified_session_times_out_without_heartbeat()
{
    palverify::SessionGuard guard{config(), challenges()};
    const auto begin =
        guard.begin("pc-player", palverify::ClientPlatform::Windows, 0.0);
    static_cast<void>(
        guard.submit_proof("pc-player", *begin.challenge, "1", true, 2.0)
    );

    require(guard.evaluate(16.9).empty(), "heartbeat grace remains active");
    const auto kicks = guard.evaluate(17.0);
    require_equal(kicks.size(), std::size_t{1}, "one heartbeat timeout");
    require_equal(
        kicks.front().reason,
        palverify::DecisionReason::HeartbeatTimeout,
        "heartbeat timeout reason"
    );
}

void disconnect_removes_session_state()
{
    palverify::SessionGuard guard{config(), challenges()};
    static_cast<void>(
        guard.begin("pc-player", palverify::ClientPlatform::Windows, 0.0)
    );

    guard.disconnect("pc-player");
    require(
        !guard.state("pc-player").has_value(),
        "disconnect must erase session"
    );
}

void known_cheat_processes_emit_minimized_rule_ids()
{
    const std::vector<std::string_view> process_images{
        "Palworld-Win64-Shipping.exe",
        "WeMod.exe",
        "CHEATENGINE-X86_64.EXE",
        "cheatengine-i386.exe",
    };

    const auto rules = palverify::detect_process_rules(process_images);
    require_equal(rules.size(), std::size_t{2}, "two unique process rules");
    require_equal(
        rules[0],
        palverify::ProcessRuleId::CheatEngineRunning,
        "Cheat Engine rule"
    );
    require_equal(
        rules[1],
        palverify::ProcessRuleId::WeModRunning,
        "WeMod rule"
    );
}

void process_rules_use_exact_names_to_avoid_substring_false_positives()
{
    const std::vector<std::string_view> process_images{
        "MyCheatEngineNotes.exe",
        "WeModInstaller.exe",
        "not-wemod.exe",
    };

    require(
        palverify::detect_process_rules(process_images).empty(),
        "unrelated substring matches must not trigger"
    );
}

}  // namespace

auto main() -> int
{
    const std::vector<TestCase> tests{
        {"exact console hardware is exempt", exact_console_hardware_is_exempt},
        {"PC, Mac, and Unknown are required", pc_mac_and_unknown_are_required},
        {
            "runtime names keep generic Xbox untrusted",
            runtime_names_do_not_turn_generic_xbox_into_an_exemption,
        },
        {
            "current runtime names distinguish PS5 models and Linux",
            current_runtime_names_distinguish_ps5_models_and_linux,
        },
        {
            "console session is allowed without challenge",
            exempt_console_session_is_allowed_without_a_challenge,
        },
        {
            "required client can verify",
            required_client_receives_a_challenge_and_can_verify,
        },
        {"unsupported version is rejected", unsupported_version_is_rejected},
        {"missing initial proof times out", missing_initial_proof_times_out},
        {
            "heartbeat refreshes and rejects replay",
            heartbeat_refreshes_a_verified_session_and_replay_is_rejected,
        },
        {
            "verified session heartbeat times out",
            verified_session_times_out_without_heartbeat,
        },
        {"disconnect removes state", disconnect_removes_session_state},
        {
            "known cheat processes emit minimized rule IDs",
            known_cheat_processes_emit_minimized_rule_ids,
        },
        {
            "process rules avoid substring false positives",
            process_rules_use_exact_names_to_avoid_substring_false_positives,
        },
    };

    std::size_t passed = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            ++passed;
            std::cout << "PASS " << test.name << '\n';
        } catch (const std::exception& error) {
            std::cout << "FAIL " << test.name << ": " << error.what() << '\n';
        }
    }

    std::cout << passed << " passed, " << tests.size() - passed << " failed\n";
    return passed == tests.size() ? EXIT_SUCCESS : EXIT_FAILURE;
}
