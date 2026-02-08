#pragma once

#ifdef _WIN32
/* ================= WINDOWS ================= */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <stdlib.h>

/* ---- pthread compat ---- */
typedef CRITICAL_SECTION pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER {0}

typedef HANDLE pthread_t;

static inline void pthread_mutex_init(pthread_mutex_t *m, void *a){
    (void)a;
    InitializeCriticalSection(m);
}
static inline void pthread_mutex_lock(pthread_mutex_t *m){
    EnterCriticalSection(m);
}
static inline void pthread_mutex_unlock(pthread_mutex_t *m){
    LeaveCriticalSection(m);
}
static inline void pthread_mutex_destroy(pthread_mutex_t *m){
    DeleteCriticalSection(m);
}

typedef struct {
    void *(*start_routine)(void *);
    void *arg;
} pthread_start_ctx_t;

static unsigned __stdcall pthread_start_thunk(void *raw){
    pthread_start_ctx_t *ctx = (pthread_start_ctx_t *)raw;
    void *(*fn)(void *) = ctx->start_routine;
    void *arg = ctx->arg;
    free(ctx);
    (void)fn(arg);
    return 0;
}

static inline int pthread_create(pthread_t *thread, void *attr, void *(*start_routine)(void *), void *arg){
    (void)attr;
    pthread_start_ctx_t *ctx = (pthread_start_ctx_t *)malloc(sizeof(*ctx));
    if (!ctx) return -1;
    ctx->start_routine = start_routine;
    ctx->arg = arg;
    uintptr_t h = _beginthreadex(NULL, 0, pthread_start_thunk, ctx, 0, NULL);
    if (h == 0) {
        free(ctx);
        return -1;
    }
    *thread = (HANDLE)h;
    return 0;
}

static inline int pthread_detach(pthread_t thread){
    return CloseHandle(thread) ? 0 : -1;
}

/* ---- semaphore ---- */
typedef HANDLE sem_t;
static inline int sem_init(sem_t *s,int p,unsigned v){
    (void)p;
    *s = CreateSemaphoreA(NULL, v, 32767, NULL);
    return *s ? 0 : -1;
}
static inline int sem_wait(sem_t *s){
    return WaitForSingleObject(*s, INFINITE) == WAIT_OBJECT_0 ? 0 : -1;
}
static inline int sem_trywait(sem_t *s){
    DWORD rc = WaitForSingleObject(*s, 0);
    if (rc == WAIT_OBJECT_0) return 0;
    if (rc == WAIT_TIMEOUT) {
        errno = EAGAIN;
        return -1;
    }
    return -1;
}
static inline int sem_post(sem_t *s){
    return ReleaseSemaphore(*s, 1, NULL) ? 0 : -1;
}
static inline int sem_destroy(sem_t *s){
    return CloseHandle(*s) ? 0 : -1;
}

/* pipe */
#ifndef _O_BINARY
#define _O_BINARY 0
#endif

static inline int pipe(int fd[2]){
    return _pipe(fd, 4096, _O_BINARY);
}

/* fcntl (stub) */
#define O_NONBLOCK 0
#define F_SETFL 0
static inline int fcntl(int fd,int c,int f){
    (void)fd; (void)c; (void)f;
    return 0;
}

static inline int usleep(unsigned int usec){
    DWORD msec = (usec + 999U) / 1000U;
    Sleep(msec);
    return 0;
}

/* gmtime_r (NÃO usar gmtime_s para evitar conflito Zig/MinGW) */
static inline struct tm *gmtime_r(const time_t *t, struct tm *out){
    struct tm *tmp = gmtime(t);
    if (!tmp) return NULL;
    *out = *tmp;
    return out;
}

/* realpath */
static inline char *realpath(const char *p,char *o){
    return _fullpath(o, p, 32768);
}

/* gettimeofday (struct timeval já existe no Windows) */
static inline int gettimeofday(struct timeval *tv, void *tz){
    (void)tz;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    u.QuadPart -= 116444736000000000ULL;
    tv->tv_sec  = (long)(u.QuadPart / 10000000ULL);
    tv->tv_usec = (long)((u.QuadPart % 10000000ULL) / 10);
    return 0;
}

#else
/* ================= POSIX ================= */
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#endif
