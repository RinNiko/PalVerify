#pragma once

#include <optional>
#include <string_view>

namespace palverify {

enum class ClientPlatform {
    Unknown,
    Windows,
    WinGDK,
    Mac,
    Linux,
    PC,
    PS5Base,
    PS5Trinity,
    XboxOne,
    XboxOneS,
    XboxOneX,
    XboxSeriesS,
    XboxSeriesX,
};

enum class VerificationRequirement {
    Required,
    Exempt,
};

[[nodiscard]] auto verification_requirement(ClientPlatform platform)
    -> VerificationRequirement;

[[nodiscard]] auto platform_from_runtime_name(std::string_view runtime_name)
    -> std::optional<ClientPlatform>;

[[nodiscard]] auto to_string(ClientPlatform platform) -> std::string_view;

}  // namespace palverify
