// SPDX-License-Identifier: GPL-2.0-or-later
#include "sdk24_fg.h"
#include <cassert>
#include <cstdio>
#include <cstring>

namespace {
int mode{};
int configured{};
ffxReturnCode_t Configure(ffxContext* context, const ffxConfigureDescHeader* desc) {
    assert(!context && desc->type==FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG);
    const auto* debug=reinterpret_cast<const ffxConfigureDescGlobalDebug*>(desc);
    assert(debug->effectId==FFX_API_EFFECT_ID_FRAMEGENERATION && debug->fpMessage);
    assert(debug->debugLevel==(FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_ERRORS|FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS));
    ++configured;
    return mode==7 ? FFX_API_RETURN_ERROR : FFX_API_RETURN_OK;
}
ffxReturnCode_t Query(ffxContext* context, ffxQueryDescHeader* desc) {
    if (mode == 6) return FFX_API_RETURN_ERROR;
    if (context) {
        if (desc->type==FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE) {
            if (mode==8) return FFX_API_RETURN_ERROR;
            auto* query=reinterpret_cast<ffxQueryDescFrameGenerationGetGPUMemoryUsage*>(desc);
            query->gpuMemoryUsageFrameGeneration->totalUsageInBytes=410000000;
            query->gpuMemoryUsageFrameGeneration->aliasableUsageInBytes=360000000;
            return FFX_API_RETURN_OK;
        }
        auto* value = reinterpret_cast<ffxQueryGetProviderVersion*>(desc);
        value->versionId = mode == 3 ? 800 : 702;
        value->versionName = mode == 4 ? "4.0.1" : "4.0.2";
        return FFX_API_RETURN_OK;
    }
    auto* value = reinterpret_cast<ffxQueryDescGetVersions*>(desc);
    assert(value->createDescType == FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION);
    if (mode == 5) { *value->outputCount = 65; return FFX_API_RETURN_OK; }
    if (!value->versionIds) { *value->outputCount = 2; return FFX_API_RETURN_OK; }
    value->versionIds[0] = 501; value->versionNames[0] = "4.0.1";
    value->versionIds[1] = 702; value->versionNames[1] = mode == 1 ? "3.1.6" : "4.0.2";
    if (mode == 2) *value->outputCount = 3;
    return FFX_API_RETURN_OK;
}
template<class F> void Reject(F operation) {
    bool rejected = false;
    try { operation(); } catch (const FgSdk24::Error&) { rejected = true; }
    assert(rejected);
}
}
int main(int argc, char** argv) {
    _set_error_mode(_OUT_TO_STDERR);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    if (argc == 3) {
        HMODULE legacy{};
        const bool legacy_first = std::strcmp(argv[1], "legacy-first") == 0;
        const auto path = std::filesystem::absolute(argv[2]);
        if (legacy_first) legacy = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        const auto& sdk = FgSdk24::Runtime();
        if (!legacy_first) legacy = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        assert(legacy && sdk.module != legacy);
        assert(GetProcAddress(legacy, "ffxCreateContext") != reinterpret_cast<FARPROC>(sdk.functions.CreateContext));
        FreeLibrary(legacy);
        std::puts("PASS: SDK 4.0.2 and legacy 4.0.1 use independent modules");
        return 0;
    }
    FgSdk24::Api api{}; api.functions.Query = Query; api.functions.Configure=Configure;
    ffxCreateContextDescFrameGeneration create{};
    create.flags=FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT;
    _putenv_s("BB_FSR24_DEBUG","");
    FgSdk24::ConfigureDiagnostics(api,create);
    assert(create.flags==FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT && configured==1);
    _putenv_s("BB_FSR24_DEBUG","true");
    FgSdk24::ConfigureDiagnostics(api,create);
    assert(!(create.flags&FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING));
    _putenv_s("BB_FSR24_DEBUG","1");
    FgSdk24::ConfigureDiagnostics(api,create);
    assert(create.flags&FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING);
    _putenv_s("BB_FSR24_DEBUG","");
    mode=7;FgSdk24::ConfigureDiagnostics(api,create);mode=0; // Optional diagnostics never reject FG.
    assert(FgSdk24::Is402("4.0.2") && FgSdk24::Is402("4.0.2 *"));
    assert(!FgSdk24::Is402(nullptr) && !FgSdk24::Is402("4.0.20") && !FgSdk24::Is402("4.0.1"));
    const auto selected = FgSdk24::Select(api, nullptr);
    assert(selected.id == 702 && selected.name == "4.0.2");
    ffxContext context = reinterpret_cast<void*>(1);
    FgSdk24::LogMemory(api,&context);
    mode=8;FgSdk24::LogMemory(api,&context);mode=0;
    FgSdk24::Verify(api, &context, selected);
    for (mode = 1; mode <= 2; ++mode) Reject([&]{ FgSdk24::Select(api, nullptr); });
    for (mode = 3; mode <= 4; ++mode) Reject([&]{ FgSdk24::Verify(api, &context, selected); });
    for (mode = 5; mode <= 6; ++mode) Reject([&]{ FgSdk24::Select(api, nullptr); });
    std::puts("PASS: SDK FG enumeration, bounded counts, exact identity and rejection of other versions");
}
