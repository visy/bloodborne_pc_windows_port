/* libkernel/libScePosix process services: clocks, sleeping, errno mapping,
 * pthread once/keys, signal bookkeeping and a narrow sysctl. Guest values
 * use FreeBSD numbering; host errno values never reach the guest directly. */
#define _GNU_SOURCE
#define _CRT_RAND_S
#include "runtime.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <x86intrin.h>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <pthread.h>
#include <sched.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <dlfcn.h>
#include <dirent.h>
#include <ucontext.h>
#include <signal.h>
#include <sys/uio.h>
#endif

#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define PAGE 16384

typedef struct { int64_t sec, nsec; } GuestTimespec;
typedef struct { int64_t sec, usec; } GuestTimeval;

int32_t runtime_guest_errno(int e) {
    switch (e) {
    case 0: return 0;
    case EPERM: return 1; case ENOENT: return 2; case ESRCH: return 3; case EINTR: return 4;
    case EIO: return 5; case ENXIO: return 6; case E2BIG: return 7; case ENOEXEC: return 8;
    case EBADF: return 9; case ECHILD: return 10; case EDEADLK: return 11; case ENOMEM: return 12;
    case EACCES: return 13; case EFAULT: return 14; case EBUSY: return 16; case EEXIST: return 17;
    case EXDEV: return 18; case ENODEV: return 19; case ENOTDIR: return 20; case EISDIR: return 21;
    case EINVAL: return 22; case ENFILE: return 23; case EMFILE: return 24; case ENOTTY: return 25;
    case EFBIG: return 27; case ENOSPC: return 28; case ESPIPE: return 29; case EROFS: return 30;
    case EMLINK: return 31; case EPIPE: return 32; case ERANGE: return 34; case EAGAIN: return 35;
    case ENAMETOOLONG: return 63; case ENOTEMPTY: return 66; case ETIMEDOUT: return 60;
    case ELOOP: return 62; case ENOSYS: return 78; case EOVERFLOW: return 84; case ECANCELED: return 85;
    default: return 5; /* EIO: unmapped host error */
    }
}
static int32_t fail_posix(int e) { *runtime_errno()=runtime_guest_errno(e); return -1; }

/* ---- clocks and sleeping ---- */
static uint64_t process_start;
__attribute__((constructor)) static void remember_start(void) { process_start=host_monotonic_ns(); }

#ifdef _WIN32
typedef enum { REALTIME, MONOTONIC, THREAD_CPU, PROCESS_CPU } HostClock;
#define HOST_CLOCK(linux_id, windows_id) windows_id
#else
typedef clockid_t HostClock;
#define HOST_CLOCK(linux_id, windows_id) linux_id
#endif

static int host_clock(uint32_t id,HostClock *out) {
    switch (id) {
    case 0: case 9: case 10: *out=HOST_CLOCK(CLOCK_REALTIME,REALTIME); return 1;  /* REALTIME(_PRECISE/_FAST) */
    case 4: case 11: case 12: case 5: case 7: case 8: *out=HOST_CLOCK(CLOCK_MONOTONIC,MONOTONIC); return 1; /* MONOTONIC/UPTIME */
    case 13: *out=HOST_CLOCK(CLOCK_REALTIME_COARSE,REALTIME); return 1;          /* SECOND */
    case 14: *out=HOST_CLOCK(CLOCK_THREAD_CPUTIME_ID,THREAD_CPU); return 1;
    case 2: case 15: *out=HOST_CLOCK(CLOCK_PROCESS_CPUTIME_ID,PROCESS_CPU); return 1; /* PROF/PROCTIME */
    case 16: case 17: case 18: case 19: *out=HOST_CLOCK(CLOCK_MONOTONIC,MONOTONIC); return 1; /* PS4 network clocks */
    default: return 0;
    }
}

static int clock_read(uint32_t id,GuestTimespec *ts) {
    HostClock host;
    if (!ts || !host_clock(id,&host)) return EINVAL;
#ifdef _WIN32
    uint64_t ns;
    if (host==REALTIME) ns=host_realtime_ns();
    else if (host==MONOTONIC) ns=host_monotonic_ns();
    else { int64_t user,system; runtime_cpu_times(host==THREAD_CPU,&user,&system); ns=(uint64_t)(user+system)*1000; }
    ts->sec=(int64_t)(ns/1000000000); ts->nsec=id==13 ? 0 : (int64_t)(ns%1000000000);
    return 0;
#else
    struct timespec t;
    if (clock_gettime(host,&t)) return errno;
    if (id==13) t.tv_nsec=0;
    ts->sec=t.tv_sec; ts->nsec=t.tv_nsec; return 0;
#endif
}

static ABI int32_t kernel_clock_gettime(uint32_t id,GuestTimespec *ts) {
    int e=clock_read(id,ts); return e ? ERR(runtime_guest_errno(e)) : 0;
}
static ABI int32_t posix_clock_gettime(uint32_t id,GuestTimespec *ts) {
    int e=clock_read(id,ts); return e ? fail_posix(e) : 0;
}
static ABI int32_t posix_clock_getres(uint32_t id,GuestTimespec *ts) {
    HostClock host;
    if (!host_clock(id,&host)) return fail_posix(EINVAL);
#ifdef _WIN32
    if (ts) { ts->sec=0; ts->nsec=100; } /* FILETIME and QPC units */
#else
    struct timespec t;
    if (clock_getres(host,&t)) return fail_posix(errno);
    if (ts) { ts->sec=t.tv_sec; ts->nsec=t.tv_nsec; }
#endif
    return 0;
}

static ABI uint64_t process_time(void) { return (host_monotonic_ns()-process_start)/1000; }
static ABI uint64_t process_time_counter(void) { return host_monotonic_ns()-process_start; }
static ABI uint64_t process_time_frequency(void) { return 1000000000; }
static ABI uint64_t read_tsc(void) { return __rdtsc(); }
static uint64_t tsc_hz;
static ABI uint64_t tsc_frequency(void) {
    if (!tsc_hz) {
        uint64_t a=host_monotonic_ns(), t0=__rdtsc();
        host_sleep_ns(20000000);
        uint64_t ns=host_monotonic_ns()-a, t1=__rdtsc();
        tsc_hz=(t1-t0)*1000000000/(ns ? ns : 1);
    }
    return tsc_hz;
}
uint64_t runtime_process_time_us(void) { return process_time(); }
uint64_t runtime_process_time_counter(void) { return process_time_counter(); }
uint64_t runtime_tsc_frequency(void) { return tsc_frequency(); }

/* bbport (frame stats): how often and how long the game sleeps (it polls GPU labels that way). */
static _Atomic uint64_t sleep_calls, sleep_total_ns;
void runtime_sleep_stats(uint64_t *calls, uint64_t *ns) {
    *calls=atomic_exchange(&sleep_calls,0); *ns=atomic_exchange(&sleep_total_ns,0);
}
static int sleep_ns(uint64_t ns) {
    const uint64_t a=host_monotonic_ns();
    host_sleep_ns(ns);
    const uint64_t slept=host_monotonic_ns()-a;
    atomic_fetch_add(&sleep_calls,1);
    atomic_fetch_add(&sleep_total_ns,slept);
    runtime_wait_note(3,slept);
    return 0;
}
static ABI int32_t kernel_usleep(uint32_t usec) { sleep_ns((uint64_t)usec*1000); return 0; }
static ABI int32_t posix_usleep(uint32_t usec) { sleep_ns((uint64_t)usec*1000); return 0; }
static ABI uint32_t posix_sleep(uint32_t seconds) { sleep_ns((uint64_t)seconds*1000000000); return 0; }
static ABI int32_t kernel_nanosleep(const GuestTimespec *rq,GuestTimespec *rem) {
    if (!rq || rq->nsec<0 || rq->nsec>=1000000000 || rq->sec<0) return ERR(22);
    (void)rem;
    sleep_ns((uint64_t)rq->sec*1000000000+(uint64_t)rq->nsec);
    return 0;
}
static ABI int32_t posix_nanosleep(const GuestTimespec *rq,GuestTimespec *rem) {
    int32_t r=kernel_nanosleep(rq,rem); return r ? fail_posix(r & 0xffff) : 0;
}

typedef struct { int32_t minuteswest, dsttime; } GuestTimezone;
static ABI int32_t kernel_gettimezone(GuestTimezone *tz) {
    if (!tz) return ERR(22);
#ifdef _WIN32
    tz->minuteswest=(int32_t)(-runtime_utc_offset(time(NULL))/60); tz->dsttime=0;
#else
    time_t now=time(NULL); struct tm local; localtime_r(&now,&local);
    tz->minuteswest=(int32_t)(-local.tm_gmtoff/60); tz->dsttime=0;
#endif
    return 0;
}
static ABI int32_t posix_gettimeofday(GuestTimeval *tv,GuestTimezone *tz) {
    uint64_t now=host_realtime_ns();
    if (tv) { tv->sec=(int64_t)(now/1000000000); tv->usec=(int64_t)(now%1000000000/1000); }
    if (tz) kernel_gettimezone(tz);
    return 0;
}
static ABI int64_t posix_time(int64_t *out) { int64_t t=(int64_t)time(NULL); if (out) *out=t; return t; }

/* ---- process ---- */
static ABI int32_t get_pagesize(void) { return PAGE; }
static ABI int32_t get_pid(void) { return 1000; }
static ABI int32_t yield(void) { host_yield(); return 0; }
static ABI __attribute__((noreturn)) void hard_exit(int status) {
    printf("Runtime: guest requested _exit(%d)\n",status);
    runtime_report();
    exit(status);
}

typedef struct { uint32_t bits[4]; } GuestSigset;
static _Thread_local GuestSigset signal_mask;
static ABI int32_t guest_sigprocmask(int how,const GuestSigset *set,GuestSigset *old) {
    if (old) *old=signal_mask;
    if (!set) return 0;
    for (int i=0;i<4;++i) {
        if (how==1) signal_mask.bits[i]|=set->bits[i];        /* SIG_BLOCK */
        else if (how==2) signal_mask.bits[i]&=~set->bits[i];  /* SIG_UNBLOCK */
        else if (how==3) signal_mask.bits[i]=set->bits[i];    /* SIG_SETMASK */
        else return fail_posix(EINVAL);
    }
    return 0;
}
static ABI int32_t guest_sigfillset(GuestSigset *set) { if (!set) return fail_posix(EINVAL); memset(set,0xff,sizeof(*set)); return 0; }
static ABI int32_t guest_sigemptyset(GuestSigset *set) { if (!set) return fail_posix(EINVAL); memset(set,0,sizeof(*set)); return 0; }
typedef struct { GuestTimeval utime, stime; int64_t rest[14]; } GuestRusage;
static ABI int32_t guest_getrusage(int who,GuestRusage *out) {
    if (!out || (who!=0 && who!=1)) return fail_posix(EINVAL);
    memset(out,0,sizeof(*out));
#ifdef _WIN32
    int64_t user,system;
    runtime_cpu_times(who==1,&user,&system);
    out->utime=(GuestTimeval){user/1000000,user%1000000};
    out->stime=(GuestTimeval){system/1000000,system%1000000};
#else
    struct rusage r;
    getrusage(who==0 ? RUSAGE_SELF : RUSAGE_THREAD,&r);
    out->utime=(GuestTimeval){r.ru_utime.tv_sec,r.ru_utime.tv_usec};
    out->stime=(GuestTimeval){r.ru_stime.tv_sec,r.ru_stime.tv_usec};
#endif
    return 0;
}

static int32_t get_host_ncpu(void) {
    const char *env = getenv("BB_NCPU");
    if (env && *env) {
        int v = atoi(env);
        if (v > 0) return (int32_t)v;
    }
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? (int32_t)si.dwNumberOfProcessors : 4;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int32_t)n : 4;
#endif
}

static ABI int32_t guest_sysctl(const int32_t *name,uint32_t namelen,void *old,uint64_t *oldlen,const void *new_value,uint64_t newlen) {
    (void)newlen;
    if (!name || namelen<2 || new_value) return fail_posix(EINVAL);
    if (name[0]==1 && name[1]==37) { /* kern.arandom */
        if (!old || !oldlen) return fail_posix(EINVAL);
#ifdef _WIN32
        if (runtime_random(old,(size_t)*oldlen)<0) return fail_posix(errno);
#else
        if (getrandom(old,(size_t)*oldlen,0)<0) return fail_posix(errno);
#endif
        return 0;
    }
    if (name[0]==6 && (name[1]==7 || name[1]==3)) { /* hw.pagesize / hw.ncpu */
        if (!oldlen) return fail_posix(EINVAL);
        int32_t value = (name[1] == 7) ? PAGE : get_host_ncpu();
        if (old) { if (*oldlen<4) return fail_posix(ENOMEM); memcpy(old,&value,4); }
        *oldlen=4; return 0;
    }
    fprintf(stderr,"STOP: unsupported sysctl mib");
    for (uint32_t i=0;i<namelen && i<8;++i) fprintf(stderr," %d",name[i]);
    fputc('\n',stderr);
    exit(21);
}

/* ---- pthread_once and thread-specific data ---- */
static ABI int32_t thread_once(int32_t *once,void (ABI *routine)(void)) {
    if (!once || !routine) return ERR(22);
    for (;;) {
        int32_t state=__atomic_load_n(once,__ATOMIC_ACQUIRE);
        if (state==1) return 0;
        if (state==0 && __atomic_compare_exchange_n(once,&state,2,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
            routine();
            __atomic_store_n(once,1,__ATOMIC_RELEASE);
            return 0;
        }
        host_yield();
    }
}
static ABI int32_t posix_once(int32_t *once,void (ABI *routine)(void)) { return thread_once(once,routine) ? 22 : 0; }
#define KEYS 256
typedef void (ABI *KeyDestructor)(void *);
static struct { int used; KeyDestructor destructor; } keys[KEYS];
static HostMutex key_lock = HOST_MUTEX_INIT;
static _Thread_local void *key_values[KEYS];

static ABI int32_t key_create(uint32_t *key,KeyDestructor destructor) {
    if (!key) return ERR(22);
    host_lock(&key_lock);
    for (uint32_t i=1;i<KEYS;++i) if (!keys[i].used) {
        keys[i].used=1; keys[i].destructor=destructor;
        host_unlock(&key_lock);
        *key=i; return 0;
    }
    host_unlock(&key_lock);
    return ERR(35);
}
static ABI int32_t key_delete(uint32_t key) {
    if (!key || key>=KEYS) return ERR(22);
    host_lock(&key_lock);
    int32_t r=keys[key].used ? 0 : ERR(22);
    keys[key].used=0; keys[key].destructor=NULL;
    host_unlock(&key_lock);
    return r;
}
static ABI int32_t key_set(uint32_t key,void *value) {
    if (!key || key>=KEYS || !keys[key].used) return ERR(22);
    key_values[key]=value; return 0;
}
static ABI void *key_get(uint32_t key) { return (!key || key>=KEYS || !keys[key].used) ? NULL : key_values[key]; }
static ABI int32_t posix_key_create(uint32_t *key,KeyDestructor destructor) {
    int32_t r=key_create(key,destructor); return r ? (int32_t)(r & 0xffff) : 0;
}
static ABI int32_t posix_key_delete(uint32_t key) {
    int32_t r=key_delete(key); return r ? (int32_t)(r & 0xffff) : 0;
}
static ABI int32_t posix_key_set(uint32_t key,void *value) {
    int32_t r=key_set(key,value); return r ? (int32_t)(r & 0xffff) : 0;
}

void runtime_thread_keys_cleanup(void) {
    for (int iter=0;iter<4;++iter) {
        int found=0;
        for (uint32_t i=1;i<KEYS;++i) {
            void *v=key_values[i];
            if (!v) continue;
            key_values[i]=NULL;
            KeyDestructor d=keys[i].used ? keys[i].destructor : NULL;
            if (d) { d(v); found=1; }
        }
        if (!found) break;
    }
}

static const RuntimeExport exports[]={
    /* Bloodborne NIDs */
    {"QBi7HCK03hw#p#J",kernel_clock_gettime},
    {"1jfXLRVzisc#p#J",kernel_usleep},
    {"-2IRUCO--PM#p#J",read_tsc},
    {"4J2sUJmuHZQ#p#J",process_time},
    {"kOcnerypnQA#p#J",kernel_gettimezone},
    {"yS8U2TGCe1A#p#J",kernel_nanosleep},
    {"6XG4B33N09g#p#J",yield},
    {"T72hz6ffq08#p#J",yield},
    {"6Z83sYWFlA8#p#J",hard_exit},
    {"3kg7rT0NQIs#p#J",hard_exit},
    {"ejekcaNQNq0#p#J",posix_gettimeofday},
    {"lLMT9vJAck0#p#J",posix_clock_gettime},
    {"n88vx3C5nW8#p#J",posix_gettimeofday},
    {"n88vx3C5nW8#I#J",posix_gettimeofday},
    {"wLlFkwG9UcQ#q#q",posix_time},
    {"QcteRwbsnV0#I#J",posix_usleep},
    {"0wu33hunNdE#I#J",posix_sleep},
    {"FJrT5LuUBAU#I#J",hard_exit},
    {"DFmMT80xcNI#p#J",guest_sysctl},

    /* Alternate NIDs & plain symbol names */
    {"k5cZ6eB0GjU#p#J",kernel_clock_gettime},
    {"qZ7nU89W8s4#p#J",posix_clock_gettime},
    {"Hq79wG3lF8I#p#J",posix_clock_getres},
    {"8gC35w1n37s#p#J",process_time},
    {"K84gq3HqC44#p#J",process_time_counter},
    {"6q5j7uQ+6g4#p#J",process_time_frequency},
    {"7J7CqZ-X-Z0#p#J",read_tsc},
    {"t+993k+39C0#p#J",tsc_frequency},
    {"-GqRms9mF9Q#p#J",kernel_usleep},
    {"7sNqP8Z-k8I#p#J",posix_usleep},
    {"Gq6nQ9e2zGg#p#J",posix_sleep},
    {"v6Y4zB2uM-g#p#J",kernel_nanosleep},
    {"ZgE5+j47X1g#p#J",posix_nanosleep},
    {"4y4hJj5M81A#p#J",kernel_gettimezone},
    {"k1mP8Y-f1vQ#p#J",posix_gettimeofday},
    {"6Q9j3fL6X8A#p#J",posix_time},
    {"1q6u7B7G33k#p#J",get_pagesize},
    {"e2X73+L7-i4#p#J",get_pid},
    {"3u2F0q3wYgI#p#J",yield},
    {"u4K7G8-B8hA#p#J",hard_exit},
    {"2N6s4+z25eY#p#J",guest_sigprocmask},
    {"5z8k3hP9P8s#p#J",guest_sigfillset},
    {"1F3m-X-k88I#p#J",guest_sigemptyset},
    {"X8K43+7wP-s#p#J",guest_getrusage},
    {"1gK2F-X58+4#p#J",guest_sysctl},
    {"7b4h-48Kq3w#p#J",thread_once},
    {"0W-jP5E2KjQ#p#J",posix_once},
    {"9k2j0+T7-N4#p#J",key_create},
    {"2B8q1pB1H+I#p#J",key_delete},
    {"E97h4n4w7z8#p#J",key_set},
    {"f+8F99p7v5g#p#J",key_get},

    /* Symbol names */
    {"sceKernelClockGettime",kernel_clock_gettime},
    {"clock_gettime",posix_clock_gettime},
    {"sceKernelReadTsc",read_tsc},
    {"sceKernelUsleep",kernel_usleep},
    {"usleep",posix_usleep},
    {"sleep",posix_sleep},
    {"nanosleep",posix_nanosleep},
    {"sched_yield",yield},
    {"scePthreadYield",yield},
    {"gettimeofday",posix_gettimeofday},
    {"time",posix_time},
    {"sysctl",guest_sysctl},
    {"pthread_key_create",posix_key_create},
    {"pthread_key_delete",posix_key_delete},
    {"pthread_setspecific",posix_key_set},
    {"pthread_getspecific",key_get},
};

uintptr_t runtime_kernel_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }

static void sample_start(void);
static void sample_report(void);

/* bbport (frame stats): time guest threads spend blocked in the runtime (condition variables,
 * mutexes, semaphores, sleeps), by thread and guest call site: the first return address into the
 * game's code on the stack. Shows where the game waits for the GPU (labels, frame pacing). */
typedef struct { _Atomic uint64_t key; uint64_t site, site2, site3; int tid, kind; char name[16]; _Atomic uint64_t count, ns; } WaitSite;
static WaitSite wait_sites[512];
static _Thread_local int wait_tid;
void runtime_guest_call_sites(uint64_t out[3]);
/* The first three return addresses into the game's code on this thread's stack (guest offsets). */
static void guest_call_sites(uint64_t out[3]) {
    const uintptr_t text_lo=0x800000000ull, text_hi=0x800000000ull+0x50d96dcull;
    static _Thread_local uintptr_t stack_hi;
    if (!stack_hi) {
#ifdef _WIN32
        ULONG_PTR low=0, high=0;
        GetCurrentThreadStackLimits(&low,&high);
        stack_hi=(uintptr_t)high;
#else
        pthread_attr_t attr; void *base=NULL; size_t size=0;
        if (!pthread_getattr_np(pthread_self(),&attr)) { pthread_attr_getstack(&attr,&base,&size); pthread_attr_destroy(&attr); }
        stack_hi=(uintptr_t)base+size;
#endif
    }
    const uintptr_t *sp=(const uintptr_t *)__builtin_frame_address(0);
    int found=0;
    out[0]=out[1]=out[2]=0;
    for (int i=0;i<400 && found<3 && (uintptr_t)(sp+i+1)<=stack_hi;++i) if (sp[i]>=text_lo && sp[i]<text_hi) out[found++]=sp[i]-text_lo;
}
void runtime_wait_note(int kind, uint64_t ns) {
    static int enabled=-1;
    if (enabled<0) enabled=getenv("BB_FRAME_STATS")!=NULL;
    if (!enabled) return;
#ifdef _WIN32
    if (!wait_tid) wait_tid=(int)GetCurrentThreadId();
#else
    if (!wait_tid) wait_tid=(int)syscall(SYS_gettid);
#endif
    uint64_t sites[3];
    guest_call_sites(sites);
    const uint64_t key=((sites[0]<<20)^(sites[1]*0x9E3779B1ull)^(sites[2]<<7)^((uint64_t)wait_tid<<4)^(uint64_t)kind)|1;
    for (uint64_t i=0,slot=(key*0x9E3779B97F4A7C15ull)>>55;i<512;++i) {
        WaitSite *w=&wait_sites[(slot+i)%512];
        uint64_t expected=0;
        if (atomic_load(&w->key)==key || atomic_compare_exchange_strong(&w->key,&expected,key)) {
            if (!w->tid) {
                w->site=sites[0]; w->site2=sites[1]; w->site3=sites[2]; w->kind=kind;
#ifdef _WIN32
                runtime_win_thread_name(0,w->name,sizeof(w->name));
#else
                pthread_getname_np(pthread_self(),w->name,sizeof(w->name));
#endif
                w->tid=wait_tid;
            }
            atomic_fetch_add(&w->count,1); atomic_fetch_add(&w->ns,ns);
            return;
        }
    }
}
void runtime_wait_report(double frames) {
    sample_start();
    sample_report();
    static const char *kinds[]={"cond","mutex","sema","sleep"};
    struct { WaitSite *w; uint64_t ns, count; } rows[512]; int n=0;
    for (int i=0;i<512;++i) {
        uint64_t ns=atomic_exchange(&wait_sites[i].ns,0), count=atomic_exchange(&wait_sites[i].count,0);
        if (count) { rows[n].w=&wait_sites[i]; rows[n].ns=ns; rows[n].count=count; ++n; }
    }
    if (!n || frames<=0) return;
    static FILE *dump; static int dump_checked;
    if (!dump_checked) { const char *path=getenv("BB_WAIT_LOG"); if (path && *path) dump=fopen(path,"w"); dump_checked=1; }
    if (dump) {
        for (int i=0;i<n;++i)
            fprintf(dump,"%s %d %s %#llx %#llx %#llx %.2f %.3f\n", rows[i].w->name, rows[i].w->tid, kinds[rows[i].w->kind&3],
                    (unsigned long long)rows[i].w->site, (unsigned long long)rows[i].w->site2, (unsigned long long)rows[i].w->site3,
                    rows[i].count/frames, rows[i].ns/(frames*1e6));
        fprintf(dump,"--\n"); fflush(dump);
    }
    for (int i=1;i<n;++i) for (int j=i;j>0 && rows[j].ns>rows[j-1].ns;--j) { __typeof__(rows[0]) t=rows[j]; rows[j]=rows[j-1]; rows[j-1]=t; }
    printf("Guest waits per frame (thread kind +site):");
    for (int i=0;i<n && i<12;++i) if (rows[i].ns/rows[i].count<5000000)
        printf("%s %s %s +%#llx<+%#llx<+%#llx %.1fx %.2f ms", i ? ";" : "", rows[i].w->name, kinds[rows[i].w->kind&3],
               (unsigned long long)rows[i].w->site, (unsigned long long)rows[i].w->site2, (unsigned long long)rows[i].w->site3,
               rows[i].count/frames, rows[i].ns/(frames*1e6));
    printf("\n");
}

/* bbport BB_SAMPLE_THREAD=<name> (with frame stats): where that thread runs, sampled every 0.5 ms
 * (Linux: SIGPROF; Windows: suspended and its context read); reported with the wait profile. A
 * thread that never blocks but waits for the GPU shows up spinning in its polling loop. */
static _Atomic uint64_t sample_keys[1024], sample_counts[1024];
static uint64_t sample_rips[1024], sample_callers[1024];
static _Atomic uint64_t samples_total;
/* rip: where the thread was; stack: words from its stack pointer up (count of them). */
static void sample_record(uint64_t rip, const uint64_t *stack, size_t count) {
    const uint64_t text_lo=0x800000000ull, text_hi=0x800000000ull+0x50d96dcull;
    /* Host code: keyed with its guest caller, the first return address into the game on the stack. */
    uint64_t caller=0;
    if (rip<text_lo || rip>=text_hi)
        for (size_t i=0;i<count;++i) if (stack[i]>=text_lo && stack[i]<text_hi) { caller=stack[i]-text_lo; break; }
    const uint64_t key=(rip*0x9E3779B97F4A7C15ull)^caller^1;
    atomic_fetch_add(&samples_total,1);
    for (uint64_t i=0,slot=(key*0x9E3779B97F4A7C15ull)>>54;i<1024;++i) {
        _Atomic uint64_t *k=&sample_keys[(slot+i)%1024];
        uint64_t expected=0;
        if (atomic_load(k)==key || atomic_compare_exchange_strong(k,&expected,key)) {
            sample_rips[(slot+i)%1024]=rip; sample_callers[(slot+i)%1024]=caller;
            atomic_fetch_add(&sample_counts[(slot+i)%1024],1);
            return;
        }
    }
}
/* A host address as module, symbol (NULL when unknown) and the base its offset is from. */
typedef struct { const char *module, *symbol; uint64_t base; } HostSymbol;
static HostSymbol host_symbol(uint64_t address, char *buffer, size_t size) {
    HostSymbol s={NULL,NULL,0};
#ifdef _WIN32
    HMODULE module=NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)address,&module) && module) {
        s.base=(uint64_t)(uintptr_t)module;
        if (GetModuleFileNameA(module,buffer,(DWORD)size)) s.module=buffer;
    }
#else
    (void)buffer; (void)size;
    Dl_info dl={0};
    dladdr((void *)address,&dl);
    s.module=dl.dli_fname; s.symbol=dl.dli_sname;
    s.base=(uint64_t)(uintptr_t)(dl.dli_sname ? dl.dli_saddr : dl.dli_fbase);
#endif
    return s;
}
#ifdef _WIN32
static DWORD WINAPI sampler_main(void *arg) {
    const char *name=(const char *)arg;
    const DWORD self=GetCurrentProcessId();
    DWORD tid=0;
    while (!tid) {
        HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
        THREADENTRY32 entry={.dwSize=sizeof(entry)};
        if (snapshot!=INVALID_HANDLE_VALUE && Thread32First(snapshot,&entry)) do {
            char comm[64];
            if (entry.th32OwnerProcessID==self && runtime_win_thread_name(entry.th32ThreadID,comm,sizeof(comm)) &&
                !strncmp(comm,name,strlen(name))) tid=entry.th32ThreadID;
        } while (!tid && Thread32Next(snapshot,&entry));
        if (snapshot!=INVALID_HANDLE_VALUE) CloseHandle(snapshot);
        if (!tid) Sleep(1000);
    }
    HANDLE thread=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,tid);
    if (!thread) return 0;
    printf("Runtime: sampling thread %s (%lu)\n",name,(unsigned long)tid);
    for (;;) {
        /* Nothing that may take a lock while the thread is suspended (it may hold it). */
        if (SuspendThread(thread)==(DWORD)-1) break;
        CONTEXT context; memset(&context,0,sizeof(context));
        context.ContextFlags=CONTEXT_CONTROL;
        uint64_t stack[256]; SIZE_T got=0;
        const BOOL ok=GetThreadContext(thread,&context);
        if (ok && !ReadProcessMemory(GetCurrentProcess(),(const void *)(uintptr_t)context.Rsp,stack,sizeof(stack),&got)) got=0;
        ResumeThread(thread);
        if (!ok) break;
        sample_record(context.Rip,stack,got/8);
        host_sleep_ns(500000);
    }
    CloseHandle(thread);
    return 0;
}
#else
static void sample_handler(int sig, siginfo_t *info, void *context) {
    (void)sig; (void)info;
    const ucontext_t *uc=(const ucontext_t *)context;
    uint64_t rip=(uint64_t)uc->uc_mcontext.gregs[REG_RIP];
    const uint64_t text_lo=0x800000000ull, text_hi=0x800000000ull+0x50d96dcull;
    uint64_t stack[256]={0};
    ssize_t got=0;
    if (rip<text_lo || rip>=text_hi) {
        struct iovec local={stack,sizeof(stack)}, remote={(void *)uc->uc_mcontext.gregs[REG_RSP],sizeof(stack)};
        got=process_vm_readv(getpid(),&local,1,&remote,1,0);
    }
    sample_record(rip,stack,got>0 ? (size_t)got/8 : 0);
}
static void *sampler_main(void *arg) {
    const char *name=(const char *)arg;
    pid_t tid=0;
    while (!tid) {
        DIR *dir=opendir("/proc/self/task");
        struct dirent *e;
        while (dir && (e=readdir(dir))) {
            char path[300], comm[32]={0};
            snprintf(path,sizeof(path),"/proc/self/task/%s/comm",e->d_name);
            FILE *f=fopen(path,"r");
            if (!f) continue;
            if (fgets(comm,sizeof(comm),f) && !strncmp(comm,name,strlen(name))) tid=(pid_t)atoi(e->d_name);
            fclose(f);
        }
        if (dir) closedir(dir);
        if (!tid) sleep(1);
    }
    printf("Runtime: sampling thread %s (%d)\n",name,(int)tid);
    for (;;) {
        if (syscall(SYS_tgkill,getpid(),tid,SIGPROF)) break;
        struct timespec t={0,500000};
        nanosleep(&t,NULL);
    }
    return NULL;
}
#endif
static void sample_start(void) {
    static int started;
    const char *name=getenv("BB_SAMPLE_THREAD");
    if (started || !name || !*name) return;
    started=1;
#ifdef _WIN32
    HANDLE thread=CreateThread(NULL,0,sampler_main,(void *)name,0,NULL);
    if (thread) CloseHandle(thread);
#else
    struct sigaction action={0};
    action.sa_sigaction=sample_handler;
    action.sa_flags=SA_SIGINFO|SA_RESTART;
    sigemptyset(&action.sa_mask);
    sigaction(SIGPROF,&action,NULL);
    pthread_t thread;
    pthread_create(&thread,NULL,sampler_main,(void *)name);
    pthread_detach(thread);
#endif
}
static void sample_report(void) {
    static int got_dumped;
    char module_path[512];
    if (!got_dumped) {
        got_dumped=1;
        const char *got=getenv("BB_DUMP_GOT"); /* a guest GOT slot (offset): which host function it calls */
        if (got && *got) {
            const uint64_t target=*(const uint64_t *)(0x800000000ull+strtoull(got,NULL,0));
            const HostSymbol s=host_symbol(target,module_path,sizeof(module_path));
            printf("Runtime: GOT %s -> %#llx %s:%s+%#llx\n",got,(unsigned long long)target,s.module ? s.module : "?",
                   s.symbol ? s.symbol : "?",(unsigned long long)(target-s.base));
        }
    }
    const uint64_t total=atomic_exchange(&samples_total,0);
    if (!total) return;
    struct { uint64_t rip, caller, count; } rows[1024]; int n=0;
    for (int i=0;i<1024;++i) { uint64_t c=atomic_exchange(&sample_counts[i],0); if (c) { rows[n].rip=sample_rips[i]; rows[n].caller=sample_callers[i]; rows[n].count=c; ++n; } }
    for (int i=1;i<n;++i) for (int j=i;j>0 && rows[j].count>rows[j-1].count;--j) { __typeof__(rows[0]) t=rows[j]; rows[j]=rows[j-1]; rows[j-1]=t; }
    printf("Thread samples (%llu):",(unsigned long long)total);
    for (int i=0;i<n && i<16;++i) {
        const uint64_t text_lo=0x800000000ull, text_hi=0x800000000ull+0x50d96dcull;
        if (rows[i].rip>=text_lo && rows[i].rip<text_hi) {
            printf("%s +%#llx %.1f%%",i?";":"",(unsigned long long)(rows[i].rip-text_lo),100.0*rows[i].count/total);
            continue;
        }
        const HostSymbol s=host_symbol(rows[i].rip,module_path,sizeof(module_path));
        const char *module=s.module ? strrchr(s.module,'/') : NULL;
        const char *back=s.module ? strrchr(s.module,'\\') : NULL;
        if (back && (!module || back>module)) module=back;
        printf("%s host:%s%s%s+%#llx (from +%#llx) %.1f%%",i?";":"",module ? module+1 : "?",s.symbol ? ":" : "",
               s.symbol ? s.symbol : "",(unsigned long long)(rows[i].rip-s.base),
               (unsigned long long)rows[i].caller,100.0*rows[i].count/total);
    }
    printf("\n");
}
/* The first three return addresses into the game's code on the calling thread's stack. */
void runtime_guest_call_sites(uint64_t out[3]) { guest_call_sites(out); }
