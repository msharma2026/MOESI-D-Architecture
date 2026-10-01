/* Seeded nonlinear-hash sketch UPDATE microbenchmark, not a validated
 * telemetry service. Final cells are checked against a serial stream replay.
 * Queries, network parsing, application decisions and freshness SLAs are not
 * timed here. PRIVATIZE includes an output-partitioned parallel final merge.
 * See docs/EVALUATION.md for the matched-ISA placement/persistence ablations. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "dstate_ops.h"
#include <string.h>
#include <pthread.h>

#if !defined(MODE_NAIVE) && !defined(MODE_DSTATE) && \
    !defined(MODE_PRIVATIZE) && !defined(MODE_PRIVSTREAM)
#error "define one of MODE_NAIVE / MODE_DSTATE / MODE_PRIVATIZE / MODE_PRIVSTREAM"
#endif

#define D_ROWS        4          /* CMS depth (independent hash rows)            */
#define W_COLS        4096       /* CMS width per row                           */
#define NCELLS        (D_ROWS * W_COLS)   /* 16384 int32 = 64 KB sketch         */
#define HOT_WIDTH     8          /* # of hot keys (attack targets) at a time     */
#define SHIFT_PERIOD  128        /* attack target shifts this often              */
#define CACHELINE     128

static inline void atomic_add_noret(int32_t *p, int32_t d)
{ dstate_add32(p, (uint32_t)d); }
static inline int32_t atomic_fetch_add(int32_t *p, int32_t d)
{ return conventional_add32(p, d); }

static int32_t sketch[NCELLS] __attribute__((aligned(CACHELINE)));
static int g_threads, g_packets, g_window = 0;
#if defined(MODE_PRIVATIZE)
static int32_t *private_rows[64];
#endif

static inline uint32_t xrng(uint32_t *s){ uint32_t x=*s; x^=x<<13; x^=x>>17; x^=x<<5; *s=x; return x; }

static double zipf_cdf[HOT_WIDTH];
static void zipf_init(void){ double h=0,a=0; for(int k=1;k<=HOT_WIDTH;k++) h+=1.0/k;
    for(int k=1;k<=HOT_WIDTH;k++){ a+=(1.0/k)/h; zipf_cdf[k-1]=a; } }
static inline int zipf_rank(uint32_t *s){ double u=(double)(xrng(s)&0xFFFFFF)/(double)0x1000000;
    for(int k=0;k<HOT_WIDTH;k++) if(u<zipf_cdf[k]) return k; return HOT_WIDTH-1; }

/* shifting DDoS target: hot key window advances over time; all threads share the
 * schedule keyed on packet index j -> they contend on the same hot cells. */
static inline uint32_t pick_key(int j, uint32_t *s){
    uint32_t base = (uint32_t)((j / SHIFT_PERIOD) * HOT_WIDTH);
    return base + (uint32_t)zipf_rank(s);
}
static const uint32_t HC[D_ROWS] = {0x9E3779B1u, 0x85EBCA77u, 0xC2B2AE3Du, 0x27D4EB2Fu};
static inline int cell(int r, uint32_t key){
    uint32_t x = key ^ HC[r];
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15;
    x *= 0x846ca68bu; x ^= x >> 16;
    return r * W_COLS + (int)(x % W_COLS);
}

/* sense-reversing barrier */
static volatile int bar_c=0, bar_s=0;
static void barrier(int *ls){ *ls=!*ls; int a=__atomic_add_fetch(&bar_c,1,__ATOMIC_ACQ_REL);
    if(a==g_threads){ __atomic_store_n(&bar_c,0,__ATOMIC_RELAXED); __atomic_store_n(&bar_s,*ls,__ATOMIC_RELEASE); }
    else while(__atomic_load_n(&bar_s,__ATOMIC_ACQUIRE)!=*ls); }

typedef struct { int tid; } arg_t;

static void *worker(void *a){
    int tid=((arg_t*)a)->tid; int ls=0;
    uint32_t s=0x9E3779B9u ^ (uint32_t)(tid*2654435761u); if(!s) s=1;
#if defined(MODE_PRIVATIZE) || defined(MODE_PRIVSTREAM)
    int32_t *loc=bench_calloc(NCELLS,sizeof(int32_t));
#endif
#if defined(MODE_PRIVSTREAM)
    int *dirty=bench_calloc(NCELLS, sizeof(int)); uint32_t *dmark=bench_calloc(NCELLS,sizeof(uint32_t));
    int ndirty=0; uint32_t epoch=1;
#endif
    for(int j=0;j<g_packets;j++){
        uint32_t key=pick_key(j,&s);
        for(int r=0;r<D_ROWS;r++){
            int ix=cell(r,key);
#if defined(MODE_NAIVE)
            (void)atomic_fetch_add(&sketch[ix],1);
#elif defined(MODE_DSTATE)
            atomic_add_noret(&sketch[ix],1);
#elif defined(MODE_PRIVATIZE)
            loc[ix]+=1;
#elif defined(MODE_PRIVSTREAM)
            loc[ix]+=1;
            if(dmark[ix]!=epoch){ dmark[ix]=epoch; dirty[ndirty++]=ix; }
#endif
        }
#if defined(MODE_PRIVSTREAM)
        if(g_window>0 && ((j+1)%g_window)==0){
            for(int d=0;d<ndirty;d++){ int ix=dirty[d]; (void)atomic_fetch_add(&sketch[ix],loc[ix]); loc[ix]=0; }
            ndirty=0; epoch++;
        }
#endif
    }
#if defined(MODE_PRIVSTREAM)
    for(int d=0;d<ndirty;d++){ int ix=dirty[d]; (void)atomic_fetch_add(&sketch[ix],loc[ix]); loc[ix]=0; }
    free(loc); free(dirty); free(dmark);
#endif
    #if defined(MODE_PRIVATIZE)
    private_rows[tid] = loc;
    #endif
    barrier(&ls);
#if defined(MODE_PRIVATIZE)
    int chunk = ((NCELLS + g_threads - 1) / g_threads + 31) / 32 * 32;
    int end = (tid + 1) * chunk;
    if (end > NCELLS) end = NCELLS;
    for (int k = tid * chunk; k < end; ++k)
        for (int t = 0; t < g_threads; ++t) sketch[k] += private_rows[t][k];
    barrier(&ls);
    free(loc);
#endif
    return NULL;
}

int main(int argc,char**argv){
    if(argc<3){ fprintf(stderr,"Usage: %s <threads> <packets/thread> [flush_window]\n",argv[0]); return 1; }
    g_threads=bench_int(argv[1],1,64); g_packets=bench_int(argv[2],1,INT_MAX/g_threads);
    if(argc>=4) g_window=bench_int(argv[3],0,INT_MAX);
    if(g_threads<1||g_threads>64){ fprintf(stderr,"threads 1..64\n"); return 1; }
    memset(sketch,0,sizeof(sketch)); zipf_init();
#if defined(MODE_NAIVE)
    const char*mode="NAIVE(lock-xadd)";
#elif defined(MODE_DSTATE)
    const char*mode="DSTATE(explicit-add32)";
#elif defined(MODE_PRIVATIZE)
    const char*mode="PRIVATIZE(per-core,batch-merge)";
#elif defined(MODE_PRIVSTREAM)
    const char*mode="PRIVSTREAM(per-core,flush)";
#endif
    long total=(long)g_threads*g_packets;
    printf("cms_bench: threads=%d packets/thread=%d mode=%s window=%d\n",g_threads,g_packets,mode,g_window);
    printf("sketch=%dx%d(%d cells,%dKB) total_packets=%ld hot_targets=%d\n",
           D_ROWS,W_COLS,NCELLS,(int)(NCELLS*sizeof(int32_t)/1024),total,HOT_WIDTH);
    bench_roi_begin();
    pthread_t th[64]; arg_t ar[64];
    for(int t=0;t<g_threads;t++) ar[t].tid=t;
    for(int t=1;t<g_threads;t++) bench_thread_check(pthread_create(&th[t],NULL,worker,&ar[t]));
    worker(&ar[0]);
    for(int t=1;t<g_threads;t++) bench_thread_check(pthread_join(th[t],NULL));
    bench_roi_end();
    int64_t sum=0; for(int k=0;k<NCELLS;k++) sum+=sketch[k];
    int64_t expect=(int64_t)total*D_ROWS;
    int ok=(sum==expect);
    int32_t *oracle = bench_calloc(NCELLS, sizeof(*oracle));
    for (int t = 0; t < g_threads; ++t) {
        uint32_t seed = 0x9E3779B9u ^ (uint32_t)(t * 2654435761u);
        if (!seed) seed = 1;
        for (int j = 0; j < g_packets; ++j) {
            uint32_t key = pick_key(j, &seed);
            for (int r = 0; r < D_ROWS; ++r) oracle[cell(r, key)]++;
        }
    }
    for (int k = 0; k < NCELLS; ++k) ok &= sketch[k] == oracle[k];
    free(oracle);
    printf("checksum=%lld expected=%lld %s\n",(long long)sum,(long long)expect, ok?"CORRECT":"WRONG");
    return ok?0:2;
}
