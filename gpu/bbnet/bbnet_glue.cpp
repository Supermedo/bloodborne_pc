// SPDX-License-Identifier: GPL-2.0-or-later
// bbport co-op: what the network and NP libraries from shadp2p need from the rest of
// shadPS4, implemented on top of the bbport runtime, and their registration.

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "common/elf_info.h"
#include "common/types.h"
#include "core/emulator_settings.h"
#include "core/bloodborne_re.h"
#include "core/debugger.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/memory.h"
#include "core/libraries/kernel/threads/pthread.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/libs.h"
#include "core/libraries/system/systemservice.h"
#include "core/libraries/system/userservice.h"
#include "core/user_manager.h"
#include "imgui/invitation_prompt_layer.h"
#include "imgui/shadnet_notifications_layer.h"

#include "core/libraries/network/http.h"
#include "core/libraries/network/http2.h"
#include "core/libraries/network/net.h"
#include "core/libraries/network/netctl.h"
#include "core/libraries/network/ssl.h"
#include "core/libraries/network/ssl2.h"
#include "core/libraries/np/np_auth.h"
#include "core/libraries/np/np_common.h"
#include "core/libraries/np/np_manager.h"
#include "core/libraries/np/np_matching2/np_matching2.h"
#include "core/libraries/np/np_partner.h"
#include "core/libraries/np/np_party.h"
#include "core/libraries/np/np_score/np_score.h"
#include "core/libraries/np/np_signaling/np_signaling.h"
#include "core/libraries/np/np_tus.h"
#include "core/libraries/np/np_web_api/np_web_api.h"
#include "core/libraries/np/np_web_api2/np_web_api2.h"

extern "C" {
int32_t runtime_thread_spawn(void** out, void* PS4_SYSV_ABI (*entry)(void*), void* argument,
                             uint64_t stack, const char* name);
int32_t runtime_thread_join_spawned(void* thread, void** result);
}

// ---- Kernel ---------------------------------------------------------------------------------

namespace Libraries::Kernel {

struct BbNetMutex {
    std::recursive_mutex mutex;
    bool recursive{};
    int depth{};
};
struct BbNetMutexAttr {
    PthreadMutexType type{PthreadMutexType::ErrorCheck};
};
struct BbNetCond {
    std::condition_variable_any cond;
};
struct BbNetCondAttr {};
struct BbNetThreadAttr {
    u64 stack{};
};

s32 ErrnoToSceKernelError(s32 e) {
    return e > 0 && e <= 0xffff ? s32(0x80020000u | u32(e)) : e;
}

s32 PS4_SYSV_ABI sceKernelGetSystemSwVersion(SwVersionStruct* ret) {
    if (!ret) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    std::memset(ret->text_representation, 0, sizeof(ret->text_representation));
    std::snprintf(ret->text_representation, sizeof(ret->text_representation), "%s", "11.000.000");
    ret->hex_representation = 0x11000001;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelClockGettime(u32 clock_id, OrbisKernelTimespec* tp) {
    if (!tp) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    s64 ns;
    if (clock_id == ORBIS_CLOCK_REALTIME || clock_id == ORBIS_CLOCK_REALTIME_PRECISE ||
        clock_id == ORBIS_CLOCK_REALTIME_FAST || clock_id == ORBIS_CLOCK_SECOND) {
        ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
                 .count();
    } else {
        ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                 std::chrono::steady_clock::now().time_since_epoch())
                 .count();
    }
    tp->tv_sec = ns / 1000000000;
    tp->tv_nsec = ns % 1000000000;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelMapNamedFlexibleMemory(void** addr_in_out, u64 len, s32, s32,
                                                 const char*) {
    if (!addr_in_out || !len) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    void* memory = std::calloc(1, len);
    if (!memory) {
        return ORBIS_KERNEL_ERROR_ENOMEM;
    }
    *addr_in_out = memory;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelMunmap(void* addr, u64) {
    std::free(addr);
    return ORBIS_OK;
}

int posix_pthread_mutexattr_init(PthreadMutexAttrT* attr) {
    if (!attr) return 22;
    *attr = new BbNetMutexAttr;
    return 0;
}
int posix_pthread_mutexattr_settype(PthreadMutexAttrT* attr, PthreadMutexType type) {
    if (!attr || !*attr) return 22;
    (*attr)->type = type;
    return 0;
}
int posix_pthread_mutexattr_destroy(PthreadMutexAttrT* attr) {
    if (!attr) return 22;
    delete *attr;
    *attr = nullptr;
    return 0;
}
int scePthreadMutexInit(PthreadMutexT* mutex, const PthreadMutexAttrT* attr, const char*) {
    if (!mutex) return 22;
    *mutex = new BbNetMutex;
    (*mutex)->recursive = attr && *attr && (*attr)->type == PthreadMutexType::Recursive;
    return 0;
}
int posix_pthread_mutex_destroy(PthreadMutexT* mutex) {
    if (!mutex || !*mutex) return 22;
    delete *mutex;
    *mutex = nullptr;
    return 0;
}
int posix_pthread_mutex_lock(PthreadMutexT* mutex) {
    if (!mutex || !*mutex) return 22;
    (*mutex)->mutex.lock();
    if (++(*mutex)->depth > 1 && !(*mutex)->recursive) {
        --(*mutex)->depth;
        (*mutex)->mutex.unlock();
        return 11; // EDEADLK
    }
    return 0;
}
int posix_pthread_mutex_trylock(PthreadMutexT* mutex) {
    if (!mutex || !*mutex) return 22;
    if (!(*mutex)->mutex.try_lock()) return 16; // EBUSY
    if (++(*mutex)->depth > 1 && !(*mutex)->recursive) {
        --(*mutex)->depth;
        (*mutex)->mutex.unlock();
        return 16;
    }
    return 0;
}
int posix_pthread_mutex_unlock(PthreadMutexT* mutex) {
    if (!mutex || !*mutex) return 22;
    --(*mutex)->depth;
    (*mutex)->mutex.unlock();
    return 0;
}

int posix_pthread_condattr_init(PthreadCondAttrT* attr) {
    if (!attr) return 22;
    *attr = new BbNetCondAttr;
    return 0;
}
int posix_pthread_condattr_destroy(PthreadCondAttrT* attr) {
    if (!attr) return 22;
    delete *attr;
    *attr = nullptr;
    return 0;
}
int scePthreadCondInit(PthreadCondT* cond, const PthreadCondAttrT*, const char*) {
    if (!cond) return 22;
    *cond = new BbNetCond;
    return 0;
}
int posix_pthread_cond_destroy(PthreadCondT* cond) {
    if (!cond || !*cond) return 22;
    delete *cond;
    *cond = nullptr;
    return 0;
}
int posix_pthread_cond_signal(PthreadCondT* cond) {
    if (!cond || !*cond) return 22;
    (*cond)->cond.notify_one();
    return 0;
}
namespace {
// The caller holds the mutex once; the wait releases and retakes it like pthread_cond_wait.
struct HeldMutex {
    BbNetMutex* mutex;
    void lock() {
        mutex->mutex.lock();
        ++mutex->depth;
    }
    void unlock() {
        --mutex->depth;
        mutex->mutex.unlock();
    }
};
} // namespace
int posix_pthread_cond_wait(PthreadCondT* cond, PthreadMutexT* mutex) {
    if (!cond || !*cond || !mutex || !*mutex) return 22;
    HeldMutex held{*mutex};
    (*cond)->cond.wait(held);
    return 0;
}
int posix_pthread_cond_reltimedwait_np(PthreadCondT* cond, PthreadMutexT* mutex, u64 usec) {
    if (!cond || !*cond || !mutex || !*mutex) return 22;
    HeldMutex held{*mutex};
    const auto status = (*cond)->cond.wait_for(held, std::chrono::microseconds(usec));
    return status == std::cv_status::timeout ? 60 : 0; // ETIMEDOUT
}

int posix_pthread_attr_init(PthreadAttrT* attr) {
    if (!attr) return 22;
    *attr = new BbNetThreadAttr;
    return 0;
}
int posix_pthread_attr_destroy(PthreadAttrT* attr) {
    if (!attr) return 22;
    delete *attr;
    *attr = nullptr;
    return 0;
}
int posix_pthread_attr_setstacksize(PthreadAttrT* attr, size_t stack_size) {
    if (!attr || !*attr) return 22;
    (*attr)->stack = stack_size;
    return 0;
}
int posix_pthread_attr_setinheritsched(PthreadAttrT*, int) {
    return 0;
}
int posix_pthread_attr_setschedpolicy(PthreadAttrT*, SchedPolicy) {
    return 0;
}
int posix_pthread_attr_setschedparam(PthreadAttrT*, const SchedParam*) {
    return 0;
}
int scePthreadAttrSetaffinity(PthreadAttrT*, u64) {
    return 0;
}
int posix_pthread_create_name_np(PthreadT* thread, const PthreadAttrT* attr,
                                 PthreadEntryFunc start, void* arg, const char* name) {
    if (!thread || !start) return 22;
    const u64 stack = attr && *attr ? (*attr)->stack : 0;
    return runtime_thread_spawn(thread, start, arg, stack, name ? name : "np");
}
int posix_pthread_join(PthreadT thread, void** ret) {
    return runtime_thread_join_spawned(thread, ret);
}

} // namespace Libraries::Kernel

// ---- User and system service, debugger, Bloodborne helpers -------------------------------------

namespace Libraries::UserService {
s32 PS4_SYSV_ABI sceUserServiceGetInitialUser(int* user_id) {
    if (!user_id) {
        return s32(0x80960009); // SCE_USER_SERVICE_ERROR_INVALID_ARGUMENT
    }
    *user_id = UserManager::RuntimeUserId;
    return ORBIS_OK;
}
} // namespace Libraries::UserService

namespace Libraries::SystemService {
void PushSystemServiceEvent(const OrbisSystemServiceEvent& event) {
    std::printf("Co-op: system service event %#x not delivered (invitations are not ported)\n",
                unsigned(event.event_type));
}
} // namespace Libraries::SystemService

namespace Core::Debugger {
s32 GetCurrentPid() {
#ifdef _WIN32
    return s32(GetCurrentProcessId());
#else
    return s32(getpid());
#endif
}
} // namespace Core::Debugger

namespace Core::Bloodborne {
namespace {
std::mutex placement_mutex;
std::optional<std::string> placement;
} // namespace
std::optional<std::string> GetSeamlessHostPlacementHeader() {
    std::scoped_lock lock{placement_mutex};
    return placement;
}
bool SetSeamlessHostPlacementHeader(std::string_view value) {
    if (value.empty() || value.size() > 4096) {
        return false;
    }
    std::scoped_lock lock{placement_mutex};
    placement = std::string{value};
    return true;
}
void ClearSeamlessHostPlacementHeader() {
    std::scoped_lock lock{placement_mutex};
    placement.reset();
}
void TraceMatching2LeaveRoom(std::uintptr_t, std::uint64_t) {}
} // namespace Core::Bloodborne

// ---- Notifications (log only for now) ------------------------------------------------------------

namespace ImGui::ShadNetNotify {
namespace {
std::mutex history_mutex;
std::vector<HistoryEntry> history;
std::size_t unread{};
} // namespace
KindDisplay DisplayOf(Kind kind) {
    switch (kind) {
    case Kind::FriendRequest:
        return {"Friend request", {0.9f, 0.8f, 0.3f, 1.0f}};
    case Kind::FriendNew:
        return {"Friend", {0.4f, 0.9f, 0.4f, 1.0f}};
    case Kind::FriendLost:
        return {"Friend", {0.9f, 0.4f, 0.4f, 1.0f}};
    case Kind::Online:
        return {"Online", {0.4f, 0.8f, 0.9f, 1.0f}};
    default:
        return {"Info", {0.8f, 0.8f, 0.8f, 1.0f}};
    }
}
void Push(Kind kind, std::string text) {
    std::printf("Co-op: %s: %s\n", DisplayOf(kind).tag, text.c_str());
    std::scoped_lock lock{history_mutex};
    history.push_back({kind, std::move(text), {}});
    ++unread;
}
std::vector<HistoryEntry> GetHistory() {
    std::scoped_lock lock{history_mutex};
    return history;
}
void ClearHistory() {
    std::scoped_lock lock{history_mutex};
    history.clear();
    unread = 0;
}
std::size_t UnreadCount() {
    std::scoped_lock lock{history_mutex};
    return unread;
}
void MarkRead() {
    std::scoped_lock lock{history_mutex};
    unread = 0;
}
void Register() {}
void Unregister() {}
} // namespace ImGui::ShadNetNotify

namespace ImGui::InvitationPrompt {
void Push(s32, std::string invitation_id, std::string, std::string from_npid) {
    std::printf("Co-op: invitation %s from %s (invitations are not supported yet)\n",
                invitation_id.c_str(), from_npid.c_str());
}
void Dismiss(const std::string&) {}
void Register() {}
void Unregister() {}
} // namespace ImGui::InvitationPrompt

// ---- Game identity -------------------------------------------------------------------------------

// ElfInfo lets shadPS4's Core::Emulator fill it in; this port has no emulator class, so the
// same name is used here for that one job.
namespace Core {
class Emulator {
public:
    static void SetGame(const char* app0, const char* serial, const char* title,
                        const char* app_ver) {
        auto& info = Common::ElfInfo::Instance();
        info.game_serial = serial ? serial : "";
        info.title = title ? title : "";
        info.app_ver = app_ver ? app_ver : "";
        info.game_folder = app0 ? app0 : "";
        info.initialized = true;
        info.npCommIds = ReadNpCommIds(info.game_folder / "sce_sys" / "npbind.dat");
    }

private:
    // npbind.dat: a 0x80-byte header, then tagged records (big-endian u16 tag and length);
    // tag 0x10 holds an NP communication id such as NPWR05818_00.
    static std::vector<std::string> ReadNpCommIds(const std::filesystem::path& path) {
        std::vector<std::string> ids;
        std::ifstream file{path, std::ios::binary};
        const std::vector<u8> data{std::istreambuf_iterator<char>(file), {}};
        for (size_t at = 0x80; at + 4 <= data.size();) {
            const u16 tag = u16(data[at] << 8 | data[at + 1]);
            const u16 size = u16(data[at + 2] << 8 | data[at + 3]);
            at += 4;
            if (at + size > data.size()) {
                break;
            }
            if (tag == 0x10 && size) {
                std::string id(reinterpret_cast<const char*>(&data[at]), size);
                id.resize(std::strlen(id.c_str()));
                ids.push_back(id);
            }
            at += size;
        }
        return ids;
    }
};
} // namespace Core

extern "C" void bbnet_configure(const char* app0, const char* serial, const char* title,
                                const char* app_ver) {
    Core::Emulator::SetGame(app0, serial, title, app_ver);
    const auto& ids = Common::ElfInfo::Instance().GetNpCommIds();
    std::printf("Co-op: %s, NP communication id %s%s\n", serial ? serial : "?",
                ids.empty() ? "missing" : ids.front().c_str(),
                EmulatorSettings.IsShadNetEnabled() ? "; online play on" : "");
}

// ---- Registration ------------------------------------------------------------------------------

extern "C" void bbnet_register(void) {
    Core::Loader::SymbolsResolver resolver;
    Core::Loader::SymbolsResolver* sym = &resolver;
    Libraries::Net::RegisterLib(sym);
    Libraries::NetCtl::RegisterLib(sym);
    Libraries::Http::RegisterLib(sym);
    Libraries::Http2::RegisterLib(sym);
    Libraries::Ssl::RegisterLib(sym);
    Libraries::Ssl2::RegisterLib(sym);
    Libraries::Np::NpAuth::RegisterLib(sym);
    Libraries::Np::NpCommon::RegisterLib(sym);
    Libraries::Np::NpManager::RegisterLib(sym);
    Libraries::Np::NpMatching2::RegisterLib(sym);
    Libraries::Np::NpPartner::RegisterLib(sym);
    Libraries::Np::NpParty::RegisterLib(sym);
    Libraries::Np::NpScore::RegisterLib(sym);
    Libraries::Np::NpSignaling::RegisterLib(sym);
    Libraries::Np::NpTus::RegisterLib(sym);
    Libraries::Np::NpWebApi::RegisterLib(sym);
    Libraries::Np::NpWebApi2::RegisterLib(sym);
}
