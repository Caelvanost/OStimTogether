#include "PCH.h"
#include "PPAIntegration.h"

#include <array>
#include <cstring>

namespace OStimTogether
{
    namespace
    {
        constexpr std::uintptr_t kGetAnimationTaggerRVA = 0x0004B9B0;
        constexpr std::uintptr_t kSetInteractionRVA = 0x00057D80;
        constexpr std::uintptr_t kSetTargetRVA = 0x00058070;
        constexpr std::size_t kAbsoluteJumpBytes = 14;

        // Exact whole-instruction prefixes from the verified
        // AccuratePenetration.dll build. Both are long enough to replace the
        // entry with a 14-byte RIP-indirect absolute jump without splitting an
        // instruction.
        constexpr std::array<std::uint8_t, 16> kSetInteractionStolenBytes{
            0x40, 0x55, 0x53, 0x56, 0x57,
            0x41, 0x54,
            0x41, 0x56,
            0x41, 0x57,
            0x48, 0x8D, 0x6C, 0x24, 0xE1
        };

        constexpr std::array<std::uint8_t, 18> kSetTargetStolenBytes{
            0x40, 0x55, 0x53, 0x56, 0x57,
            0x41, 0x54,
            0x41, 0x55,
            0x41, 0x56,
            0x41, 0x57,
            0x48, 0x8D, 0x6C, 0x24, 0xE9
        };

        void* g_absoluteStubPage{ nullptr };

        void WriteAbsoluteJump(
            std::uint8_t* destination,
            std::uintptr_t target)
        {
            // jmp qword ptr [rip+0]
            // dq target
            // No register is clobbered and there is no +/-2 GiB restriction.
            constexpr std::array<std::uint8_t, 6> kPrefix{
                0xFF, 0x25, 0x00, 0x00, 0x00, 0x00
            };

            std::memcpy(
                destination,
                kPrefix.data(),
                kPrefix.size());
            std::memcpy(
                destination + kPrefix.size(),
                std::addressof(target),
                sizeof(target));
        }

        bool WriteExecutableBytes(
            std::uintptr_t destination,
            const void* source,
            std::size_t size)
        {
            if (!destination || !source || size == 0) {
                return false;
            }

            DWORD oldProtect = 0;
            if (!VirtualProtect(
                    reinterpret_cast<void*>(destination),
                    size,
                    PAGE_EXECUTE_READWRITE,
                    &oldProtect)) {
                SKSE::log::error(
                    "OSTNET PPA ABS64 VirtualProtect failed address=0x{:X} size={} error={}",
                    destination,
                    size,
                    GetLastError());
                return false;
            }

            std::memcpy(
                reinterpret_cast<void*>(destination),
                source,
                size);

            const bool flushed =
                FlushInstructionCache(
                    GetCurrentProcess(),
                    reinterpret_cast<void*>(destination),
                    size) != 0;

            DWORD ignored = 0;
            const bool restored =
                VirtualProtect(
                    reinterpret_cast<void*>(destination),
                    size,
                    oldProtect,
                    &ignored) != 0;

            if (!flushed || !restored) {
                SKSE::log::warn(
                    "OSTNET PPA ABS64 patch post-write warning address=0x{:X} flushed={} protectionRestored={} error={}",
                    destination,
                    flushed ? 1 : 0,
                    restored ? 1 : 0,
                    GetLastError());
            }

            return flushed;
        }

        template <class Fn, class Hook, std::size_t N>
        Fn InstallAbsoluteEntryDetour(
            std::uint8_t*& stubCursor,
            std::uintptr_t source,
            Hook hook,
            const std::array<std::uint8_t, N>& stolenBytes)
        {
            static_assert(N >= kAbsoluteJumpBytes);

            if (!stubCursor || !source || !hook) {
                return nullptr;
            }

            auto* originalStub = stubCursor;
            stubCursor += 64;

            // Callable original: replay all whole instructions displaced from
            // PPA, then jump absolutely to the first untouched instruction.
            std::memcpy(
                originalStub,
                stolenBytes.data(),
                stolenBytes.size());
            WriteAbsoluteJump(
                originalStub + stolenBytes.size(),
                source + stolenBytes.size());

            // Function entry: direct absolute jump to OStimTogether.dll. Fill
            // any bytes beyond the 14-byte jump with NOPs so the entire stolen
            // instruction window is replaced deterministically.
            std::array<std::uint8_t, N> patch{};
            patch.fill(0x90);
            WriteAbsoluteJump(
                patch.data(),
                reinterpret_cast<std::uintptr_t>(hook));

            if (!WriteExecutableBytes(
                    source,
                    patch.data(),
                    patch.size())) {
                return nullptr;
            }

            return reinterpret_cast<Fn>(originalStub);
        }
    }

    bool PPAIntegration::PrepareAbsoluteHooks()
    {
        if (_hooksInstalled) {
            return true;
        }

        // Do not modify PPA merely because it is installed. The FOMOD marker
        // remains the user's explicit opt-in for this exact-build integration.
        // Return true here so main.cpp does not emit a scary hook-failure warning
        // when the optional component was intentionally left unchecked.
        if (!IsOptionalIntegrationInstalled()) {
            SKSE::log::info(
                "OSTNET PPA ABS64 skipped: optional FOMOD component not installed");
            return true;
        }

        const auto module = GetModuleHandleW(
            AccuratePenetration::API::kPluginDLL);
        if (!module) {
            SKSE::log::warn(
                "OSTNET PPA ABS64 unavailable: AccuratePenetration.dll is not loaded");
            return false;
        }

        // Reuse the exact timestamp/image/signature validation already owned by
        // PPAIntegration. Unsupported future PPA builds remain fail-closed.
        if (!ValidateExactPPABuild(module)) {
            return false;
        }

        const auto base = reinterpret_cast<std::uintptr_t>(module);
        _getAnimationTagger = reinterpret_cast<GetAnimationTaggerFn>(
            base + kGetAnimationTaggerRVA);

        if (!g_absoluteStubPage) {
            g_absoluteStubPage = VirtualAlloc(
                nullptr,
                4096,
                MEM_COMMIT | MEM_RESERVE,
                PAGE_EXECUTE_READWRITE);
        }

        if (!g_absoluteStubPage) {
            SKSE::log::error(
                "OSTNET PPA ABS64 stub allocation failed error={} action=disable-no-hook",
                GetLastError());
            return false;
        }

        auto* cursor = static_cast<std::uint8_t*>(g_absoluteStubPage);

        const auto interactionOriginal =
            InstallAbsoluteEntryDetour<SetInteractionFn>(
                cursor,
                base + kSetInteractionRVA,
                &PPAIntegration::SetInteractionHook,
                kSetInteractionStolenBytes);
        if (!interactionOriginal) {
            SKSE::log::error(
                "OSTNET PPA ABS64 setInteraction install failed action=disable-integration");
            return false;
        }
        _setInteractionOriginal = interactionOriginal;

        const auto targetOriginal =
            InstallAbsoluteEntryDetour<SetTargetFn>(
                cursor,
                base + kSetTargetRVA,
                &PPAIntegration::SetTargetHook,
                kSetTargetStolenBytes);
        if (!targetOriginal) {
            SKSE::log::error(
                "OSTNET PPA ABS64 setTarget install failed action=disable-integration");
            return false;
        }
        _setTargetOriginal = targetOriginal;

        _hooksInstalled = true;

        SKSE::log::info(
            "OSTNET PPA ABS64 READY getterRva=0x{:X} setInteractionRva=0x{:X} setTargetRva=0x{:X} stolenInteraction={} stolenTarget={} detour=direct-rip-indirect-abs64 rel32=0 exactBuild=1 targetWrite=1",
            kGetAnimationTaggerRVA,
            kSetInteractionRVA,
            kSetTargetRVA,
            kSetInteractionStolenBytes.size(),
            kSetTargetStolenBytes.size());

        return true;
    }
}
