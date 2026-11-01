// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the actual applier without calling guest code or starting its thread.
#include "../gpu/shim/bbport_pc_menu.cpp"
#include <stdexcept>

extern "C" void runtime_restart() { throw std::runtime_error("unexpected restart"); }

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    try {
        using namespace BbSettings;
        auto& s = Get();
        const auto start = Clock::time_point{};
        for (const int provider : {UpscalerFsr4, UpscalerFsr411, UpscalerFsr412}) {
            SelectUpscaler(UpscalerFsr3);
            Require(Apply(Line::Upscaler, provider), "changed selection must request a save");
            Require(s.upscaler == provider, "PC menu selection did not reach shared settings");
            UpscalerFailure(provider);
            const auto request = s.upscaler_request.load();
            applied[Line::Upscaler] = provider;
            SetValue(Line::Upscaler, provider);
            int32_t seen = provider;
            auto since = start;
            // Re-selecting an unchanged value cannot bypass the applier's filter.
            Require(!ApplySettled(Line::Upscaler, provider, start + Settle, seen, since) &&
                    s.UpscalerFallbackActive(), "unchanged choice retried implicitly");
            // Browse past retry and cancel before settling: do not recreate a provider.
            ApplySettled(Line::Upscaler, RetryUpscaler, start + Settle, seen, since);
            ApplySettled(Line::Upscaler, provider, start + Settle + Settle / 2, seen, since);
            Require(s.upscaler_request == request && s.UpscalerFallbackActive(),
                    "browsing or cancelling retried a provider");
            const auto retry = start + Settle * 3;
            ApplySettled(Line::Upscaler, RetryUpscaler, retry, seen, since);
            ApplySettled(Line::Upscaler, RetryUpscaler, retry + Settle - std::chrono::milliseconds(1), seen, since);
            Require(s.UpscalerFallbackActive(), "retry bypassed settle delay");
            Require(!ApplySettled(Line::Upscaler, RetryUpscaler, retry + Settle, seen, since),
                    "retry action must not save a sentinel value");
            Require(s.upscaler == provider && !s.UpscalerFallbackActive() &&
                    s.upscaler_request == request + 1, "retry did not preserve/re-request the saved selection");
            Require(Value(Line::Upscaler) == provider && applied[Line::Upscaler] == provider,
                    "retry sentinel was not removed from the menu state");
            ApplySettled(Line::Upscaler, Value(Line::Upscaler), retry + Settle * 2, seen, since);
            Require(s.upscaler_request == request + 1, "retry repeated without user action");
            // A stale open menu may retain the action after another frontend recovers.
            Apply(Line::Upscaler, RetryUpscaler);
            Require(s.upscaler_request == request + 1 && s.upscaler == provider,
                    "stale retry overwrote a working selection");
        }
        SelectUpscaler(UpscalerFsr3);
        Apply(Line::Upscaler, RetryUpscaler);
        Require(s.upscaler == UpscalerFsr3 && !s.UpscalerFallbackActive(),
                "stale retry changed a non-FSR4 selection");
        std::puts("PASS: PC-menu shared selection, explicit retry, settle/cancel and sentinel cleanup");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
