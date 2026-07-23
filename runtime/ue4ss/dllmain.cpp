#include <Mod/CppUserModBase.hpp>

namespace palverify::ue4ss {

class PalVerifyMod final : public RC::CppUserModBase {
public:
    PalVerifyMod()
    {
        ModName = STR("PalVerify");
        ModVersion = STR("0.1.0-dev");
        ModAuthors = STR("Palworld 3 Mien");
        ModDescription =
            STR("PalVerify native lifecycle scaffold; enforcement disabled");
    }
};

}  // namespace palverify::ue4ss

#define PALVERIFY_API __declspec(dllexport)

extern "C" {

PALVERIFY_API auto start_mod() -> RC::CppUserModBase*
{
    return new palverify::ue4ss::PalVerifyMod();
}

PALVERIFY_API void uninstall_mod(RC::CppUserModBase* mod)
{
    delete mod;
}

}
