#include "malloc.c"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "error.h"


#define USE_PSHM 1
//#define USE_SYSV 1


#if 0
PROFILE_DECLARE();
PROFILE_TIMER(malloc);
PROFILE_TIMER(calloc);
PROFILE_TIMER(free);
PROFILE_TIMER(realloc);
PROFILE_TIMER(memalign);
PROFILE_TIMER(mmap);

void sm_profile_show(void)
{
#if 0
    PROFILE_TIMER_SHOW(malloc);
    PROFILE_TIMER_SHOW(calloc);
    PROFILE_TIMER_SHOW(free);
    PROFILE_TIMER_SHOW(realloc);
    PROFILE_TIMER_SHOW(memalign);
    PROFILE_TIMER_SHOW(mmap);
#endif
}
#endif



#define unlikely(x)     __builtin_expect((x),0)


struct sm_region
{
    intptr_t limit; //End of shared memory region
    intptr_t brk;   //Next available shared memory address.

};

void* sm_lower = NULL;
void* sm_upper = NULL;


static struct sm_region* sm_region = NULL;
static mspace sm_mspace = NULL;


#define TEMP_SIZE (1024 * 1024 * 4L) //Temporary mspace capacity

//Keep this around for use with valgrind.
//static char sm_temp[TEMP_SIZE] = {0};


#define USE_PROC_MAPS 1

#ifdef USE_PROC_MAPS
//Some systems randomize the address returned by mmap(), so it won't be the
// same on all processes.  Instead, parse /proc/<pid>/maps to find a hole and
// hope all processes pick the same hole.

//$ cat /proc/1180/maps
//00400000-005c2000 r-xp 00000000 08:03 76344697                           /usr/bin/vim
//007c1000-007d7000 rw-p 001c1000 08:03 76344697                           /usr/bin/vim
//007d7000-008e9000 rw-p 00000000 00:00 0                                  [heap]
//2aaaaaaab000-2aaaaaacb000 r-xp 00000000 08:03 36945932                   /lib64/ld-2.12.so
//2aaaaaacb000-2aaaaaacc000 r-xp 00000000 00:00 0                          [vdso]
//2aaaaaacc000-2aaaaaacd000 rw-p 00000000 00:00 0 
//2aaaaacca000-2aaaaaccb000 r--p 0001f000 08:03 36945932                   /lib64/ld-2.12.so
//2aaaaaccb000-2aaaaaccc000 rw-p 00020000 08:03 36945932                   /lib64/ld-2.12.so

//Find an address to map a region of size bytes.
//If none is found or some error occurs, aborts.
void* find_map_address(size_t size)
{
    char line[1024];
    char filename[64];
    FILE* map_fd;
    uintptr_t low_addr = 0;
    uintptr_t high_addr= 0;
    uintptr_t prev_addr = 0;

    sprintf(filename, "/proc/%d/maps", getpid());
    map_fd = fopen(filename, "r");
    if(map_fd == NULL) {
        perror("fopen /proc/pid/maps");
        abort();
    }

    while(fgets(line, 1024, map_fd) != NULL) {
        if(sscanf(line, "%lx-%lx", &low_addr, &high_addr)) {
            //printf("%d low %lx high %lx\n", getpid(), low_addr, high_addr);

            if(low_addr - prev_addr >= size) {
                //double dsize = (double)low_addr - (double)prev_addr;
                //dsize = dsize / (1024 * 1024 * 1024);
                //printf("found opening %lx %lx %lf gb\n", prev_addr, low_addr, dsize);
                return (void*)prev_addr;
            }
        }

        prev_addr = high_addr;
    }
#if 0
    while(fscanf(map_fd, "%lx-%lx %*s %*s %*s %*s %*s\n",
                &low_addr, &high_addr) ||
            fscanf(map_fd, "%lx-%lx %*s %*s %*s %*s\n",
                &low_addr, &high_addr) ||
            ) {
        printf("%d low %lx high %lx\n", getpid(), low_addr, high_addr);
        if(low_addr - prev_addr >= size) {
            printf("found opening %lx %lx\n", prev_addr, low_addr);
            //return (void*)low_addr;
        }

        prev_addr = high_addr;
    }
#endif
    ERROR("Did not find large enough whole in mapping for SM region");
    return NULL;
}
#endif


#ifdef USE_PSHM

//Set up new default parameters, and use env vars to override.
#ifdef __bg__

//Total shared memory space to mmap.
#define DEFAULT_TOTAL_SIZE ((1024L*1024L*1024L * 12))

//How many pieces the available SM memory should be divided into.
// Each rank/process will get one piece.
#define DEFAULT_RANK_DIVIDER (64)

#else

//Total shared memory space to mmap.
#define DEFAULT_TOTAL_SIZE ((1024L*1024L * 1024L * 12))

//How many pieces the available SM memory should be divided into.
// Each rank/process will get one piece.
#define DEFAULT_RANK_DIVIDER (12)

#endif

static char* sm_filename = "hmpismfile59";


static void __sm_destroy(void)
{
    shm_unlink(sm_filename);
}


static int __sm_init_region(void* map_addr, size_t size)
{
    int do_init = 1; //Whether to do initialization

    //Open the SM region file.
    int fd = shm_open(sm_filename, O_RDWR|O_CREAT|O_EXCL|O_TRUNC, S_IRUSR|S_IWUSR); 
    if(fd == -1) {
        do_init = 0;

        if(errno == EEXIST) {
            //Another process has already created the file.
            fd = shm_open(sm_filename, O_RDWR, S_IRUSR|S_IWUSR);
        } 
        
        if(fd == -1) {
            perror("shm_open");
            fflush(stderr);
            abort();
        }
    }


    if(ftruncate(fd, size) == -1) {
        perror("ftruncate");
        fflush(stderr);
        abort();
    }

    int flags = MAP_SHARED;
    if(map_addr != NULL) {
        flags |= MAP_FIXED;
    }

    //Map the SM region.
    //sm_region = mmap((void *)0X300616000000, size, PROT_READ|PROT_WRITE, flags, fd, 0);
    sm_region = mmap(map_addr, size, PROT_READ|PROT_WRITE, flags, fd, 0);
	printf("found address = %lx\n", map_addr);
    if(sm_region == (void*)MAP_FAILED) {
        perror("mmap");
        fflush(stderr);
        abort();
    }

    close(fd);

    return do_init;
}

#endif

#ifdef USE_SYSV
#error "not updated"
//MMAP and SYSV have different space capabilities.
//LC machines are configured for 12gb of sysv memory (768mb for 16 ranks)
#define MSPACE_SIZE (1024L * 1024L * 700L)
//#define DEFAULT_SIZE (MSPACE_SIZE * 16L + (long)getpagesize()) //Default shared heap size
#define DEFAULT_SIZE (1024L * 1024L * 12200L)


static int sm_shmid = -1;


static void __sm_destroy(void)
{
    shmctl(sm_shmid, IPC_RMID, NULL);
}


static int __sm_init_region(void)
{
    int do_init = 1; //Whether to do initialization

    //Use the PWD for an ftok file -- we don't have argv[0] here,
    // and "_" points to srun under slurm.
    char* pwd = getenv("PWD");
    if(pwd == NULL) {
        abort();
    }

    key_t key = ftok(pwd, 'S' << 1);


    sm_shmid = shmget(key, DEFAULT_SIZE, 0600 | IPC_CREAT | IPC_EXCL);
    if(sm_shmid == -1) {

        if(errno == EEXIST) {
            //SM region exists, try again -- we won't initialize.
            sm_shmid = shmget(key, DEFAULT_SIZE, 0600 | IPC_CREAT);
            do_init = 0;
        }


            printf("DEFAULT_SIZE %ld %d\n", DEFAULT_SIZE, errno);
            fflush(stdout);
        //Abort if both tries failed.
        if(sm_shmid == -1) {
            abort();
        }
    }


    sm_region = shmat(sm_shmid, NULL, 0);
    if(sm_region == (void*)-1) {
        abort();
    }

    return do_init;
}

#endif


static void __attribute__((noinline)) __sm_init(void)
{
    char* tmp;
    size_t total_size;
    size_t rank_divider;
    size_t pagesize = (size_t)getpagesize();
    int do_init; //Whether to do initialization

    //Set up a temporary area on the stack for malloc() calls during our
    // initialization process.

    uint64_t* temp_space = alloca(TEMP_SIZE);
    sm_mspace = create_mspace_with_base(temp_space, TEMP_SIZE, 0);

    //Keep this for use with valgrind.
    //sm_mspace = create_mspace_with_base(sm_temp, TEMP_SIZE, 0);

    //sm_region->limit = (intptr_t)sm_region + TEMP_SIZE;


    //Query environment variables to figure out how much size is available.
    //The value of SM_SIZE is always expected to be megabytes.
    tmp = getenv("SM_SIZE");
    if(tmp == NULL) {
        //On BGQ, the size var MUST be set.
        //If it is not, there probably is only enough shared memory for the
        // system reservation.  Can't assume there's usable SM, so abort.
#ifdef __bg__
        ERROR("SM_SIZE env var not set (make sure BG_SHAREDMEMSIZE is set too");
#else
        total_size = DEFAULT_TOTAL_SIZE;
#endif
    } else {
        total_size = atol(tmp) * 1024L * 1024L;
    }

    //SM_RANKS and DEFAULT_RANK_DIVIDER indicate how many regions to break the
    //SM region into -- one region per rank/process.
    tmp = getenv("SM_RANKS");
    if(tmp == NULL) {
        rank_divider = DEFAULT_RANK_DIVIDER;
    } else {
        rank_divider = atol(tmp);
    }


    //TESTING: output /proc/maps info.
//#ifndef __bg__
#if 0
    {
        char line[1024];
        char map_path[256];
        FILE* map_fd;
        int pid = getpid();

        sprintf(map_path, "/proc/%d/maps", pid);
        map_fd = fopen(map_path, "r");
        if(map_fd == NULL) {
            perror("fopen /proc/pid/maps");
            fflush(stdout);
            abort();
        }

        while(fgets(line, 1024, map_fd) != NULL) {
            printf("%d: %s", pid, line);
        }

        fclose(map_fd);
    }
#endif

    //offset is the size taken by sm_region at the beginning of the space.
    size_t offset = ((sizeof(struct sm_region) / pagesize) + 1) * pagesize;

#ifdef USE_PROC_MAPS
    void* map_addr = find_map_address(total_size + offset);
#else
    void* map_addr = NULL;
#endif

    //Set up the SM region using one of mmap/sysv/pshm
    do_init = __sm_init_region(map_addr, total_size + offset);


    //Only the process creating the file should initialize.
    if(do_init) {
        //Only the initializing process registers the shutdown handler.
        atexit(__sm_destroy);

        sm_region->limit = (intptr_t)sm_region + total_size + offset;

#ifdef __bg__
        //Ensure everything above is set before brk below:
        // setting brk is the synchronization signal.
        __lwsync();
#endif

        sm_region->brk = (intptr_t)sm_region + offset;
    } else {
        //Wait for another process to finish initialization.
        void* volatile * brk_ptr = (void**)&sm_region->brk;

        while(*brk_ptr == NULL);

        //Ensure none of the following loads occur during/before the spin loop.
#ifdef __bg__
        __lwsync();
#endif
    }

   printf("brk = %p\n", sm_region->brk);
    //Check that this process' region is mapped to the same address as the
    //process that initialized the region.
    if(sm_region->limit != (intptr_t)sm_region + total_size + offset) {
        ERROR("sm_region limit %lx doesn't match computed limit %lx",
                sm_region->limit, (intptr_t)sm_region + total_size + offset);
    }

    sm_lower = sm_region;
    sm_upper = (void*)sm_region->limit;
    //Create my own mspace.
    size_t local_size = total_size / rank_divider;

    //void* base = sm_morecore(local_size);
    void* base = (void*)__sync_fetch_and_add(&sm_region->brk, local_size);
    if(base < sm_lower || base >= sm_upper) {
        ERROR("Got local base %p outside of range %p -> %p",
                base, sm_lower, sm_upper);
    }

    //Clearing the memory seems to avoid some bugs and
    // forces out subtle OOM issues here instead of later.
    //memset(base, 0, local_size);

    //WARNING("%d sm_region %p base %p total_size %lx local_size %lx\n",
    //        getpid(), sm_region, base, total_size, local_size);

    //Careful to subtract off space for the local data.
    sm_mspace = create_mspace_with_base(base, local_size, 1);

    //This should go last so it can use proper malloc and friends.
    //PROFILE_INIT();

}


void* sm_morecore(intptr_t increment)
{
    abort();
#if 0
    void* oldbrk = (void*)__sync_fetch_and_add(&sm_region->brk, increment);

/*    printf("%d sm_morecore incr %ld brk %p limit %p\n",
            getpid(), increment, oldbrk, sm_region->limit);
    fflush(stdout);*/

    if((uintptr_t)oldbrk + increment > (uintptr_t)sm_region->limit) {
        errno = ENOMEM;
        return (void*)-1;
    }

    //memset(oldbrk, 0, increment);
    return oldbrk;
#endif
}


#if 0
void* sm_mmap(void* addr, size_t len, int prot, int flags, int fildes, off_t off)
{
    //PROFILE_START(mmap);
    void* ptr = sm_morecore(len);
    //PROFILE_STOP(mmap);
    return ptr;
}


int sm_munmap(void* addr, size_t len)
{
    //For now, just move the break back if possible.

    //Clear this so MMAP_CLEARS works right -- free mem is always clear.
    memset(addr, 0, len);

    /*int success =*/ __sync_bool_compare_and_swap(&sm_region->brk,
            (intptr_t)addr + len, addr);

    //if(success) {
    //    printf("munmap returned break %lx\n", len);
    //} else {
    //    printf("munmap leaking mem %p len %lx (%p) brk 0x%lx\n",
    //            addr, len, (void*)((uintptr_t)addr + len), sm_region->brk);
    //}
    //fflush(stdout);

    return 0;
}
#endif


int is_sm_buf(void* mem) {
    //if(sm_region == NULL) __sm_init();

    return (intptr_t)mem >= (intptr_t)sm_region &&
        (intptr_t)mem < sm_region->limit;
}


void* malloc(size_t bytes) {
    if(unlikely(sm_mspace == NULL)) __sm_init();
    //PROFILE_START(malloc);

    void* ptr = mspace_malloc(sm_mspace, bytes);

    //PROFILE_STOP(malloc);
    return ptr;
}

void free(void* mem) {
    //if(unlikely(sm_region == NULL)) __sm_init();

    //PROFILE_START(free);

    if(mem < sm_lower || mem >= sm_upper) {
        return;
    }

    if(unlikely(sm_mspace == NULL)) return;

    mspace_free(sm_mspace, mem);
    //PROFILE_STOP(free);
}

void* realloc(void* mem, size_t newsize) {
    if(unlikely(sm_mspace == NULL)) __sm_init();

    //PROFILE_START(realloc);
    void* ptr = mspace_realloc(sm_mspace, mem, newsize);
    //PROFILE_STOP(realloc);

    return ptr;
}

void* calloc(size_t n_elements, size_t elem_size) {
    if(unlikely(sm_mspace == NULL)) __sm_init();

    //PROFILE_START(calloc);
    void* ptr = mspace_calloc(sm_mspace, n_elements, elem_size);
    //PROFILE_STOP(calloc);

    return ptr;
}

void* memalign(size_t alignment, size_t bytes) {
    if(unlikely(sm_mspace == NULL)) __sm_init();

    //PROFILE_START(memalign);
    void* ptr = mspace_memalign(sm_mspace, alignment, bytes);
    //PROFILE_STOP(memalign);

    return ptr;
}

