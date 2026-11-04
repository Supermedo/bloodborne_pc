/* PS4 rwlocks behind the pointer-to-handle ABI, on the host's reader-writer locks (SRW locks on
   Windows). Lock records come from a pool that is never freed, so a handle from the game is
   checked without a global lock: 0 is the static initializer, 1 a destroyed lock, anything else
   must point at a live record. Each thread keeps the list of locks it holds (EDEADLK, EPERM),
   and a thread that already reads a lock reads it again without asking the host lock, so a
   writer waiting in between can't deadlock it (FreeBSD's libthr allows nested reads too). */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif
/* A cache line each: threads using different locks don't slow each other down. */
typedef struct Rwlock {
#ifdef _WIN32
    _Alignas(64) SRWLOCK native;
#else
    _Alignas(64) pthread_rwlock_t native;
#endif
    unsigned users;  /* threads holding the lock or waiting for it; DEAD while the record is free */
    struct Rwlock *next_free;
} Rwlock;
#define DEAD 0x80000000u
#ifdef _WIN32
#define chunk_alloc(size) _aligned_malloc(size,64)
#else
#define chunk_alloc(size) aligned_alloc(64,size)
#endif
enum { CHUNK=1024, CHUNKS=4096 };
typedef struct { int64_t seconds, nanoseconds; } GuestTime;
/* Making and destroying locks; using them takes no global lock. */
static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER;
static Rwlock *chunks[CHUNKS];
static unsigned chunk_count, next_unused;
static Rwlock *free_records;
static size_t created, reads, writes, unlocks;
/* The locks this thread holds: a thread rarely holds more than a few at once. */
typedef struct { Rwlock *lock; unsigned readers, writer; } Held;
static _Thread_local Held *held;
static _Thread_local unsigned held_count, held_size;
static int32_t error(int e) {
    if (!e) return 0;
    unsigned code;
    switch(e) {
    case EPERM: code=1; break;
    case EDEADLK: code=11; break;
    case ENOMEM: code=12; break;
    case EBUSY: code=16; break;
    case EINVAL: code=22; break;
    case EAGAIN: code=35; break;
    case ETIMEDOUT: code=60; break;
    default: fprintf(stderr,"STOP: unmapped rwlock error %d\n",e); exit(21);
    }
    return (int32_t)(UINT32_C(0x80020000)|code);
}
/* The live record a handle points at, or NULL. Reads only pool memory, which stays mapped. */
static Rwlock *live(Rwlock *handle) {
    uintptr_t p=(uintptr_t)handle;
    unsigned n=__atomic_load_n(&chunk_count,__ATOMIC_ACQUIRE);
    for (unsigned i=0;i<n;++i) {
        uintptr_t base=(uintptr_t)chunks[i];
        if (p<base || p-base>=CHUNK*sizeof(Rwlock)) continue;
        if ((p-base)%sizeof(Rwlock)) return NULL;
        return __atomic_load_n(&handle->users,__ATOMIC_ACQUIRE)&DEAD ? NULL : handle;
    }
    return NULL;
}
/* Requires pool_lock. */
static int make_locked(Rwlock **out) {
    Rwlock *r=free_records;
    if (r) free_records=r->next_free;
    else {
        unsigned n=chunk_count;
        if (!n || next_unused==CHUNK) {
            if (n==CHUNKS) return ENOMEM;
            Rwlock *chunk=chunk_alloc(CHUNK*sizeof(*chunk)); /* never freed */
            if (!chunk) return ENOMEM;
            memset(chunk,0,CHUNK*sizeof(*chunk));
            for (unsigned i=0;i<CHUNK;++i) chunk[i].users=DEAD;
            chunks[n++]=chunk; next_unused=0;
            __atomic_store_n(&chunk_count,n,__ATOMIC_RELEASE);
        }
        r=&chunks[n-1][next_unused++];
    }
#ifdef _WIN32
    InitializeSRWLock(&r->native);
#else
    int e=pthread_rwlock_init(&r->native,NULL);
    if (e) { r->next_free=free_records; free_records=r; return e; }
#endif
    /* Clears only DEAD: a late call with a stale handle may still be counting itself in. */
    __atomic_fetch_and(&r->users,~DEAD,__ATOMIC_RELEASE);
    *out=r; ++created; return 0;
}
/* The record behind a handle; a statically initialized (0) handle gets one on first use. */
static int resolve(Rwlock **handle, Rwlock **out) {
    Rwlock *h=__atomic_load_n(handle,__ATOMIC_ACQUIRE);
    if (!h) {
        pthread_mutex_lock(&pool_lock);
        int e=0;
        if (!(h=*handle)) {
            e=make_locked(&h);
            if (!e) __atomic_store_n(handle,h,__ATOMIC_RELEASE);
        }
        pthread_mutex_unlock(&pool_lock);
        if (e) return e;
    }
    return (*out=live(h)) ? 0 : EINVAL;
}
static Held *holding(const Rwlock *r) {
    for (unsigned i=0;i<held_count;++i) if (held[i].lock==r) return &held[i];
    return NULL;
}
#ifdef _WIN32
static int take(Rwlock *r, int writer, int attempt) {
    if (attempt) return (writer ? TryAcquireSRWLockExclusive(&r->native) : TryAcquireSRWLockShared(&r->native)) ? 0 : EBUSY;
    if (writer) AcquireSRWLockExclusive(&r->native); else AcquireSRWLockShared(&r->native);
    return 0;
}
static void give(Rwlock *r, int writer) {
    if (writer) ReleaseSRWLockExclusive(&r->native); else ReleaseSRWLockShared(&r->native);
}
/* SRW locks have no timed wait: try again until the deadline, yielding first, then in 1 ms naps. */
static int take_until(Rwlock *r, int writer, const struct timespec *deadline) {
    for (unsigned tries=0;;++tries) {
        if (!take(r,writer,1)) return 0;
        struct timespec now;
        clock_gettime(CLOCK_REALTIME,&now);
        if (now.tv_sec>deadline->tv_sec || (now.tv_sec==deadline->tv_sec && now.tv_nsec>=deadline->tv_nsec)) return ETIMEDOUT;
        if (tries<64) SwitchToThread(); else Sleep(1);
    }
}
#else
static int take(Rwlock *r, int writer, int attempt) {
    if (attempt) return writer ? pthread_rwlock_trywrlock(&r->native) : pthread_rwlock_tryrdlock(&r->native);
    return writer ? pthread_rwlock_wrlock(&r->native) : pthread_rwlock_rdlock(&r->native);
}
static void give(Rwlock *r, int writer) { (void)writer; pthread_rwlock_unlock(&r->native); }
static int take_until(Rwlock *r, int writer, const struct timespec *deadline) {
    return writer ? pthread_rwlock_timedwrlock(&r->native,deadline) : pthread_rwlock_timedrdlock(&r->native,deadline);
}
#endif
static ABI int32_t rw_init(Rwlock **out, void *const *attr, const char *name) {
    (void)name;
    if (!out || (attr && !*attr)) return error(EINVAL);
    if (attr) { fputs("STOP: non-default PS4 rwlock attributes are not implemented\n",stderr); exit(21); }
    pthread_mutex_lock(&pool_lock);
    /* POSIX allows uninitialized storage, so never read *out during init. */
    Rwlock *r=NULL;
    int e=make_locked(&r);
    pthread_mutex_unlock(&pool_lock);
    if (!e) *out=r;
    return error(e);
}
enum Operation { READ, WRITE, TRY_READ, TRY_WRITE, TIMED_READ, TIMED_WRITE };
static int32_t acquire(Rwlock **handle, enum Operation op, const GuestTime *time) {
    if (!handle) return error(EINVAL);
    int writer=op==WRITE || op==TRY_WRITE || op==TIMED_WRITE;
    int attempt=op==TRY_READ || op==TRY_WRITE;
    int timed=op==TIMED_READ || op==TIMED_WRITE;
    Rwlock *r;
    int e=resolve(handle,&r);
    if (e) return error(e);
    Held *h=holding(r);
    if (h) {
        if (h->writer || writer) return error(attempt ? EBUSY : EDEADLK);
        if (h->readers==UINT_MAX) return error(EAGAIN);
        ++h->readers; RUNTIME_COUNT(reads); return 0;
    }
    if (held_count==held_size) {
        unsigned size=held_size ? held_size*2 : 8;
        Held *grown=realloc(held,size*sizeof(*grown));
        if (!grown) return error(ENOMEM);
        held=grown; held_size=size;
    }
    if (__atomic_fetch_add(&r->users,1,__ATOMIC_ACQ_REL)&DEAD) e=EINVAL; /* destroyed meanwhile */
    else {
        /* Timed operations first try the lock, matching POSIX timeout validation. */
        e=take(r,writer,attempt || timed);
        if (timed && e==EBUSY) {
            if (!time || time->nanoseconds<0 || time->nanoseconds>=1000000000) e=EINVAL;
            else {
                struct timespec deadline={.tv_sec=(time_t)time->seconds,.tv_nsec=(long)time->nanoseconds};
                e=take_until(r,writer,&deadline);
            }
        }
    }
    if (e) { __atomic_fetch_sub(&r->users,1,__ATOMIC_ACQ_REL); return error(e); }
    held[held_count++]=(Held){r,!writer,writer};
    if (writer) RUNTIME_COUNT(writes); else RUNTIME_COUNT(reads);
    return 0;
}
static ABI int32_t rw_read(Rwlock **r) { return acquire(r,READ,NULL); }
static ABI int32_t rw_write(Rwlock **r) { return acquire(r,WRITE,NULL); }
static ABI int32_t rw_tryread(Rwlock **r) { return acquire(r,TRY_READ,NULL); }
static ABI int32_t rw_trywrite(Rwlock **r) { return acquire(r,TRY_WRITE,NULL); }
static ABI int32_t rw_timedread(Rwlock **r,const GuestTime *t) { return acquire(r,TIMED_READ,t); }
static ABI int32_t rw_timedwrite(Rwlock **r,const GuestTime *t) { return acquire(r,TIMED_WRITE,t); }
static ABI int32_t rw_unlock(Rwlock **handle) {
    if (!handle) return error(EINVAL);
    Held *h=holding(*handle);
    if (!h) return error(live(*handle) ? EPERM : EINVAL);
    RUNTIME_COUNT(unlocks);
    if (!h->writer && --h->readers) return 0; /* this thread still reads it */
    Rwlock *r=h->lock;
    give(r,h->writer);
    *h=held[--held_count];
    __atomic_fetch_sub(&r->users,1,__ATOMIC_ACQ_REL);
    return 0;
}
static ABI int32_t rw_destroy(Rwlock **handle) {
    if (!handle) return error(EINVAL);
    pthread_mutex_lock(&pool_lock);
    if (!*handle) { pthread_mutex_unlock(&pool_lock); return 0; }
    Rwlock *r=live(*handle);
    if (!r) { pthread_mutex_unlock(&pool_lock); return error(EINVAL); }
    /* Only an unused lock is destroyed; from then on every check of its handle fails. */
    unsigned unused=0;
    if (!__atomic_compare_exchange_n(&r->users,&unused,DEAD,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
        pthread_mutex_unlock(&pool_lock); return error(EBUSY);
    }
#ifndef _WIN32
    int e=pthread_rwlock_destroy(&r->native);
    if (e) { __atomic_store_n(&r->users,0,__ATOMIC_RELEASE); pthread_mutex_unlock(&pool_lock); return error(e); }
#endif
    r->next_free=free_records; free_records=r;
    *handle=(Rwlock *)(uintptr_t)1;
    pthread_mutex_unlock(&pool_lock); return 0;
}
uintptr_t runtime_rwlock_resolve(const char *name) {
    if (!strcmp(name,"6ULAa0fq4jA#p#J")) return (uintptr_t)rw_init;
    if (!strcmp(name,"BB+kb08Tl9A#p#J")) return (uintptr_t)rw_destroy;
    if (!strcmp(name,"Ox9i0c7L5w0#p#J")) return (uintptr_t)rw_read;
    if (!strcmp(name,"mqdNorrB+gI#p#J")) return (uintptr_t)rw_write;
    if (!strcmp(name,"XD3mDeybCnk#p#J")) return (uintptr_t)rw_tryread;
    if (!strcmp(name,"bIHoZCTomsI#p#J")) return (uintptr_t)rw_trywrite;
    if (!strcmp(name,"+L98PIbGttk#p#J")) return (uintptr_t)rw_unlock;
    if (!strcmp(name,"iPtZRWICjrM#p#J")) return (uintptr_t)rw_timedread;
    if (!strcmp(name,"adh--6nIqTk#p#J")) return (uintptr_t)rw_timedwrite;
    return 0;
}
void runtime_rwlock_report(void) {
    pthread_mutex_lock(&pool_lock);
    printf("Runtime: rwlocks created=%zu, reads=%zu, writes=%zu, unlocks=%zu\n",created,
           __atomic_load_n(&reads,__ATOMIC_RELAXED),__atomic_load_n(&writes,__ATOMIC_RELAXED),
           __atomic_load_n(&unlocks,__ATOMIC_RELAXED));
    pthread_mutex_unlock(&pool_lock);
}
