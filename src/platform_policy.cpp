#include "palverify/platform_policy.hpp"

#include <array>
#include <utility>

namespace palverify {

auto verification_requirement(ClientPlatform platform) -> VerificationRequirement
{
    switch (platform) {
    case ClientPlatform::PS5Base:
    case ClientPlatform::PS5Trinity:
    case ClientPlatform::XboxOne:
    case ClientPlatform::XboxOneS:
    case ClientPlatform::XboxOneX:
    case ClientPlatform::XboxSeriesS:
    case ClientPlatform::XboxSeriesX:
        return VerificationRequirement::Exempt;
    case ClientPlatform::Unknown:
    case ClientPlatform::Windows:
    case ClientPlatform::WinGDK:
    case ClientPlatform::Mac:
    case ClientPlatform::Linux:
    case ClientPlatform::PC:
        return VerificationRequirement::Required;
    }

    return VerificationRequirement::Required;
}

auto platform_from_runtime_name(std::string_view runtime_name)
    -> std::optional<ClientPlatform>
{
    using Mapping = std::pair<std::string_view, ClientPlatform>;
    constexpr std::array mappings{
        Mapping{"Unknown", ClientPlatform::Unknown},
        Mapping{"Windows", ClientPlatform::Windows},
        Mapping{"WinGDK", ClientPlatform::WinGDK},
        Mapping{"Mac", ClientPlatform::Mac},
        Mapping{"Linux", ClientPlatform::Linux},
        Mapping{"PC", ClientPlatform::PC},
        Mapping{"PS5Base", ClientPlatform::PS5Base},
        Mapping{"PS5Trinity", ClientPlatform::PS5Trinity},
        Mapping{"XB1", ClientPlatform::XboxOne},
        Mapping{"XB1S", ClientPlatform::XboxOneS},
        Mapping{"XB1X", ClientPlatform::XboxOneX},
        Mapping{"XSS", ClientPlatform::XboxSeriesS},
        Mapping{"XSX", ClientPlatform::XboxSeriesX},
    };

    for (const auto& [name, platform] : mappings) {
        if (runtime_name == name) {
            return platform;
        }
    }

    return std::nullopt;
}

auto to_string(ClientPlatform platform) -> std::string_view
{
    switch (platform) {
    case ClientPlatform::Unknown:
        return "Unknown";
    case ClientPlatform::Windows:
        return "Windows";
    case ClientPlatform::WinGDK:
        return "WinGDK";
    case ClientPlatform::Mac:
        return "Mac";
    case ClientPlatform::Linux:
        return "Linux";
    case ClientPlatform::PC:
        return "PC";
    case ClientPlatform::PS5Base:
        return "PS5Base";
    case ClientPlatform::PS5Trinity:
        return "PS5Trinity";
    case ClientPlatform::XboxOne:
        return "XB1";
    case ClientPlatform::XboxOneS:
        return "XB1S";
    case ClientPlatform::XboxOneX:
        return "XB1X";
    case ClientPlatform::XboxSeriesS:
        return "XSS";
    case ClientPlatform::XboxSeriesX:
        return "XSX";
    }

    return "Unknown";
}

}  // namespace palverify
