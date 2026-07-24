#include "palverify/windows_process_scan.hpp"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error{message};
    }
}

[[nodiscard]] auto current_executable() -> std::filesystem::path
{
    std::wstring buffer(32768, L'\0');
    const auto length = GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<DWORD>(buffer.size())
    );
    require(length != 0, "current executable path must be available");
    buffer.resize(length);
    return buffer;
}

}  // namespace

int main(int argc, char* argv[])
{
    try {
        if (argc == 2 && std::string_view{argv[1]} == "--scan-processes") {
            const auto started = GetTickCount64();
            const auto scan = palverify::scan_running_processes();
            std::cout << "available=" << scan.available
                      << " first_elapsed_ms="
                      << (GetTickCount64() - started)
                      << '\n';
            for (const auto rule : scan.rules) {
                std::cout << palverify::to_string(rule) << '\n';
            }
            const auto cached_started = GetTickCount64();
            const auto cached_scan = palverify::scan_running_processes();
            std::cout << "cached_available=" << cached_scan.available
                      << " cached_elapsed_ms="
                      << (GetTickCount64() - cached_started)
                      << '\n';
            return scan.available ? EXIT_SUCCESS : EXIT_FAILURE;
        }
        if (argc == 3 && std::string_view{argv[1]} == "--scan-modules") {
            const auto started = GetTickCount64();
            const auto scan =
                palverify::scan_palworld_modules(argv[2]);
            std::cout << "available=" << scan.available
                      << " elapsed_ms=" << (GetTickCount64() - started)
                      << '\n';
            for (const auto rule : scan.rules) {
                std::cout << palverify::to_string(rule) << '\n';
            }
            return scan.available ? EXIT_SUCCESS : EXIT_FAILURE;
        }

        const auto evidence =
            palverify::inspect_process_executable(current_executable());
        require(evidence.has_value(), "fixture executable must be inspectable");
        require(
            evidence->file_description == "PalVerify Identity Fixture",
            "fixture file description"
        );
        require(
            evidence->company_name == "Palworld 3 Mien",
            "fixture company name"
        );
        require(
            evidence->sha256.size() == 64,
            "fixture SHA-256 must contain 64 hexadecimal characters"
        );
        require(
            !evidence->signature_valid,
            "unsigned fixture must not report a trusted signature"
        );

        const std::array modules{
            palverify::ModuleEvidence{
                .image_name = "renamed.dll",
                .sha256 =
                    "4d0597dc7ba65b65106743afbadd70c2045f9e07725bdf4629c0d057a4469bba",
                .signature_valid = false,
                .game_location = false,
                .system_location = true,
            },
        };
        const auto known_module_rules =
            palverify::detect_module_rules(modules);
        require(
            known_module_rules
                == std::vector{
                    palverify::ProcessRuleId::InjectedModuleDetected,
                },
            "known injected DLL hash must survive file renaming"
        );

        const std::array external_modules{
            palverify::ModuleEvidence{
                .image_name = "overlay.dll",
                .sha256 =
                    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                .signature_valid = true,
                .game_location = false,
                .system_location = false,
            },
            palverify::ModuleEvidence{
                .image_name = "unknown.dll",
                .sha256 =
                    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                .signature_valid = false,
                .game_location = false,
                .system_location = false,
            },
        };
        const auto external_rules =
            palverify::detect_module_rules(external_modules);
        require(
            external_rules
                == std::vector{
                    palverify::ProcessRuleId::InjectedModuleDetected,
                },
            "unsigned external DLL must be rejected while signed overlays remain allowed"
        );

        const std::array unrelated_signed_overlay{
            palverify::ModuleEvidence{
                .image_name = "gameoverlayrenderer64.dll",
                .sha256 =
                    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                .signature_valid = true,
                .game_location = false,
                .system_location = false,
                .signer_name = "Valve Corp.",
                .file_description = "Steam Game Overlay Renderer",
                .company_name = "Valve Corp.",
            },
        };
        require(
            palverify::detect_module_rules(unrelated_signed_overlay).empty(),
            "unrelated signed game overlays must remain allowed"
        );

        const std::array wand_modules{
            palverify::ModuleEvidence{
                .image_name = "TrainerLibPlugin_x64.dll",
                .sha256 =
                    "c7a27201a5250d6011aa1d4eef929c99bf4978c0f5d004bcc3af106848a80f8e",
                .signature_valid = true,
                .game_location = false,
                .system_location = false,
                .signer_name = "WeMod LLC",
                .file_description = "TrainerLib Plugin",
                .company_name = "WeMod LLC",
            },
        };
        require(
            palverify::detect_module_rules(wand_modules)
                == std::vector{
                    palverify::ProcessRuleId::InjectedModuleDetected,
                },
            "signed Wand trainer DLL must never be allowed in Palworld"
        );

        const std::array renamed_wand_modules{
            palverify::ModuleEvidence{
                .image_name = "harmless-name.dll",
                .sha256 =
                    "c7a27201a5250d6011aa1d4eef929c99bf4978c0f5d004bcc3af106848a80f8e",
                .signature_valid = true,
                .game_location = true,
                .system_location = false,
                .signer_name = "WeMod LLC",
                .file_description = {},
                .company_name = {},
            },
        };
        require(
            palverify::detect_module_rules(renamed_wand_modules)
                == std::vector{
                    palverify::ProcessRuleId::InjectedModuleDetected,
                },
            "renamed Wand DLL must be rejected by publisher even inside the game root"
        );

        const std::array official_modules{
            palverify::ModuleEvidence{
                .image_name = "tbb12.dll",
                .sha256 =
                    "de25bc26f71c5fa53b585116db11d643dc63373fab094e5d786a2ef7eb50786d",
                .signature_valid = false,
                .game_location = true,
                .system_location = false,
            },
        };
        require(
            palverify::detect_module_rules(official_modules).empty(),
            "exact official Palworld build module hash must remain allowed"
        );

        const std::array current_game_modules{
            palverify::ModuleEvidence{
                .image_name = "GFSDK_Aftermath_Lib.x64.dll",
                .sha256 =
                    "516e14af41a91fe960363728965f354aecc06bcdfaef6bcb45dd3537ede09195",
                .signature_valid = false,
                .game_location = true,
                .system_location = false,
            },
            palverify::ModuleEvidence{
                .image_name = "libcef.dll",
                .sha256 =
                    "e07343732a79a0dcd48a6b813f1469ba0f858f0c3f96b0223ec6aa5468a49cc3",
                .signature_valid = false,
                .game_location = true,
                .system_location = false,
            },
        };
        require(
            palverify::detect_module_rules(current_game_modules).empty(),
            "unsigned DLLs shipped inside the verified Palworld root must not be treated as injected"
        );

        const std::array renamed_known_module_in_game_root{
            palverify::ModuleEvidence{
                .image_name = "harmless-name.dll",
                .sha256 =
                    "4d0597dc7ba65b65106743afbadd70c2045f9e07725bdf4629c0d057a4469bba",
                .signature_valid = false,
                .game_location = true,
                .system_location = false,
            },
        };
        require(
            palverify::detect_module_rules(
                renamed_known_module_in_game_root
            )
                == std::vector{
                    palverify::ProcessRuleId::InjectedModuleDetected,
                },
            "known injected DLL hash must still be rejected from the game root"
        );

        std::array<std::byte, 512> mapped_image{};
        mapped_image[0] = std::byte{'M'};
        mapped_image[1] = std::byte{'Z'};
        const std::uint32_t pe_offset = 0x80;
        std::memcpy(
            mapped_image.data() + 0x3c,
            &pe_offset,
            sizeof(pe_offset)
        );
        IMAGE_NT_HEADERS64 nt_headers{};
        nt_headers.Signature = IMAGE_NT_SIGNATURE;
        nt_headers.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
        nt_headers.FileHeader.NumberOfSections = 3;
        nt_headers.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        nt_headers.OptionalHeader.SizeOfImage = 0x20000;
        std::memcpy(
            mapped_image.data() + pe_offset,
            &nt_headers,
            sizeof(nt_headers)
        );
        require(
            palverify::looks_like_manual_map_candidate(mapped_image),
            "private executable memory with a PE header must be detected"
        );

        std::cout << "PASS Windows process executable evidence\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cout << "FAIL Windows process executable evidence: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
