set(PALVERIFY_UE4SS_TARGET PalVerifyNative)

add_library(${PALVERIFY_UE4SS_TARGET} SHARED
    dllmain.cpp
)

target_link_libraries(
    ${PALVERIFY_UE4SS_TARGET}
    PRIVATE
        UE4SS
        palverify_core
)

target_compile_features(${PALVERIFY_UE4SS_TARGET} PRIVATE cxx_std_20)
set_target_properties(
    ${PALVERIFY_UE4SS_TARGET}
    PROPERTIES
        OUTPUT_NAME "main"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out/server/dlls"
)
