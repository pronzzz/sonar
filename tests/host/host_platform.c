/* Host (macOS/Linux) implementation of the AI engine platform layer, so the
 * exact engine code that runs in the kernel can be tested at full speed. */
#include "platform.h"

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void *ai_alloc(size_t n) { return malloc(n ? n : 1); }
void *ai_calloc(size_t n, size_t s) { return calloc(n ? n : 1, s ? s : 1); }
void *ai_realloc(void *p, size_t n) { return realloc(p, n); }
void ai_free(void *p) { free(p); }

void *ai_alloc_large(size_t n)
{
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

void ai_free_large(void *p, size_t n)
{
    if (p && n)
        munmap(p, n);
}

void ai_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

u64 ai_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
}

bool ai_should_abort(void) { return false; }

#if defined(__APPLE__)
#include <sys/sysctl.h>
bool ai_cpu_has_dotprod(void)
{
    int v = 0;
    size_t n = sizeof(v);
    return sysctlbyname("hw.optional.arm.FEAT_DotProd", &v, &n, NULL, 0) == 0 && v;
}
#elif defined(__linux__) && defined(__aarch64__)
#include <sys/auxv.h>
bool ai_cpu_has_dotprod(void) { return (getauxval(AT_HWCAP) & (1UL << 20)) != 0; /* ASIMDDP */ }
#else
bool ai_cpu_has_dotprod(void) { return false; }
#endif

/* ---- thread pool mirroring the kernel's parallel_for semantics ----
 * Workers spin briefly for the next job before sleeping, like the kernel's
 * WFE-based pool, so host timings reflect the engine rather than futexes. */
static int nthreads = 1;
static pthread_t threads[64];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static struct {
    ai_par_fn fn;
    void *ctx;
    int n, grain;
    int next, pending;
} job;
static u64 job_gen;

static void run_chunks(void)
{
    for (;;) {
        int s = __atomic_fetch_add(&job.next, job.grain, __ATOMIC_RELAXED);
        if (s >= job.n)
            break;
        int e = s + job.grain < job.n ? s + job.grain : job.n;
        job.fn(job.ctx, s, e);
    }
}

static void *worker(void *arg)
{
    (void)arg;
    u64 seen = 0;
    for (;;) {
        u64 g;
        int spins = 0;
        while ((g = __atomic_load_n(&job_gen, __ATOMIC_ACQUIRE)) == seen && spins < 200000) {
            spins++;
#ifdef __aarch64__
            __asm__ volatile("yield");
#endif
        }
        if (g == seen) {
            pthread_mutex_lock(&mu);
            while ((g = __atomic_load_n(&job_gen, __ATOMIC_ACQUIRE)) == seen)
                pthread_cond_wait(&cv, &mu);
            pthread_mutex_unlock(&mu);
        }
        seen = g;
        run_chunks();
        __atomic_fetch_sub(&job.pending, 1, __ATOMIC_RELEASE);
    }
    return NULL;
}

void host_threads_init(int n)
{
    if (n < 1)
        n = 1;
    if (n > 64)
        n = 64;
    nthreads = n;
    for (int i = 1; i < n; i++)
        pthread_create(&threads[i], NULL, worker, NULL);
}

int ai_num_threads(void) { return nthreads; }

void ai_parallel_for(int n, int grain, ai_par_fn fn, void *ctx)
{
    if (n <= 0)
        return;
    if (grain < 1)
        grain = 1;
    if (nthreads <= 1 || n <= grain) {
        fn(ctx, 0, n);
        return;
    }
    job.fn = fn;
    job.ctx = ctx;
    job.n = n;
    job.grain = grain;
    job.next = 0;
    job.pending = nthreads - 1;
    pthread_mutex_lock(&mu);
    __atomic_add_fetch(&job_gen, 1, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    run_chunks();
    while (__atomic_load_n(&job.pending, __ATOMIC_ACQUIRE) != 0)
        ;
}

/* ---- files ---- */
struct host_file {
    int fd;
};

static bool host_read(void *ctx, u64 off, void *buf, u64 len)
{
    struct host_file *h = ctx;
    u8 *p = buf;
    while (len) {
        ssize_t r = pread(h->fd, p, len > (1 << 30) ? (1 << 30) : len, (off_t)off);
        if (r <= 0)
            return false;
        p += r;
        off += (u64)r;
        len -= (u64)r;
    }
    return true;
}

bool host_open(const char *path, struct ai_file *f, bool map)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    struct stat st;
    fstat(fd, &st);
    struct host_file *h = malloc(sizeof(*h));
    h->fd = fd;
    f->ctx = h;
    f->size = (u64)st.st_size;
    f->read = host_read;
    f->mapped = NULL;
    if (map) {
        void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p != MAP_FAILED)
            f->mapped = p;
    }
    return true;
}

bool ai_large_dma_ok(const void *p)
{
    (void)p;
    return true;
}
