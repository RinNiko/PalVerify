#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace palverify::client_ui {

[[nodiscard]] auto confirm_verification(std::string_view code) -> bool;

[[nodiscard]] auto prompt_giftcode(std::string_view initial_value)
    -> std::optional<std::string>;

[[nodiscard]] auto open_default_browser(std::string_view url) -> bool;

}  // namespace palverify::client_ui
