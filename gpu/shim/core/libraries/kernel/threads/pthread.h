// bbport co-op: the pthread calls the NP libraries make (NpCommon threads, mutexes and
// condition variables). Threads are real guest threads from the C runtime, because the
// matching and signaling threads call the game's callbacks; mutexes and condition variables
// are only used by the host code here, so they are plain host objects.
#pragma once
#include "common/types.h"

namespace Libraries::Kernel {

struct BbNetMutex;
struct BbNetMutexAttr;
struct BbNetCond;
struct BbNetCondAttr;
struct BbNetThreadAttr;

using PthreadT = void*; // the runtime's guest thread handle
using PthreadMutexT = BbNetMutex*;
using PthreadMutexAttrT = BbNetMutexAttr*;
using PthreadCondT = BbNetCond*;
using PthreadCondAttrT = BbNetCondAttr*;
using PthreadAttrT = BbNetThreadAttr*;
using PthreadEntryFunc = void* PS4_SYSV_ABI (*)(void*);

enum class PthreadMutexType : u32 { ErrorCheck = 1, Recursive = 2, Normal = 3, AdaptiveNp = 4 };
enum class SchedPolicy : u32 { Fifo = 1, Other = 2, RoundRobin = 3 };
struct SchedParam {
    int sched_priority;
};

constexpr int ORBIS_KERNEL_PRIO_FIFO_DEFAULT = 700;
constexpr int ORBIS_KERNEL_PRIO_FIFO_HIGHEST = 256;
constexpr int ORBIS_KERNEL_PRIO_FIFO_LOWEST = 767;

// All return 0 or a POSIX errno value, as shadPS4's posix_* functions do.
int posix_pthread_mutexattr_init(PthreadMutexAttrT* attr);
int posix_pthread_mutexattr_settype(PthreadMutexAttrT* attr, PthreadMutexType type);
int posix_pthread_mutexattr_destroy(PthreadMutexAttrT* attr);
int scePthreadMutexInit(PthreadMutexT* mutex, const PthreadMutexAttrT* attr, const char* name);
int posix_pthread_mutex_destroy(PthreadMutexT* mutex);
int posix_pthread_mutex_lock(PthreadMutexT* mutex);
int posix_pthread_mutex_trylock(PthreadMutexT* mutex);
int posix_pthread_mutex_unlock(PthreadMutexT* mutex);

int posix_pthread_condattr_init(PthreadCondAttrT* attr);
int posix_pthread_condattr_destroy(PthreadCondAttrT* attr);
int scePthreadCondInit(PthreadCondT* cond, const PthreadCondAttrT* attr, const char* name);
int posix_pthread_cond_destroy(PthreadCondT* cond);
int posix_pthread_cond_signal(PthreadCondT* cond);
int posix_pthread_cond_wait(PthreadCondT* cond, PthreadMutexT* mutex);
int posix_pthread_cond_reltimedwait_np(PthreadCondT* cond, PthreadMutexT* mutex, u64 usec);

int posix_pthread_attr_init(PthreadAttrT* attr);
int posix_pthread_attr_destroy(PthreadAttrT* attr);
int posix_pthread_attr_setstacksize(PthreadAttrT* attr, size_t stack_size);
int posix_pthread_attr_setinheritsched(PthreadAttrT* attr, int inherit);
int posix_pthread_attr_setschedpolicy(PthreadAttrT* attr, SchedPolicy policy);
int posix_pthread_attr_setschedparam(PthreadAttrT* attr, const SchedParam* param);
int scePthreadAttrSetaffinity(PthreadAttrT* attr, u64 mask);
int posix_pthread_create_name_np(PthreadT* thread, const PthreadAttrT* attr,
                                 PthreadEntryFunc start, void* arg, const char* name);
int posix_pthread_join(PthreadT thread, void** ret);

} // namespace Libraries::Kernel
