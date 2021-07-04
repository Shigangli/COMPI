#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>


#define USE_PSHM 1

#define DEFAULT_SIZE 4096

struct sm_region
{
	int lock;
};

static struct sm_region* sm_region = NULL;


#define LOCK_ACQUIRE() \
	while(__sync_lock_test_and_set(&sm_region->lock, 1) != 0)

#define LOCK_RELEASE() \
	__sync_lock_release(&sm_region->lock)

#ifdef USE_PSHM

static char* sm_filename = "/hmpismfile";


static void __sm_destroy(void)
{
	shm_unlink(sm_filename);
}


static int __sm_init_region(void)
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
			abort();
		}
	}

	if(ftruncate(fd, DEFAULT_SIZE) == -1) {
		abort();
	}

	//Map the SM region.
	sm_region = mmap(NULL, DEFAULT_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
	if(sm_region == (void*)MAP_FAILED) {
		abort();
	}

	close(fd);

	return do_init;
}

#endif


static void* (*__old_malloc_hook)(size_t size, const void* caller) = NULL;
static void (*__old_free_hook)(void* ptr, const void* caller) = NULL;
static void* (*__old_realloc_hook)(void* ptr, size_t size, const void* caller) = NULL;
static void* (*__old_memalign_hook)(size_t alignment, size_t size, const void* caller) = NULL;


static void* __my_malloc_hook(size_t size, const void* caller)
{
	__malloc_hook = __old_malloc_hook;
	LOCK_ACQUIRE();

	void* ptr = malloc(size);

	LOCK_RELEASE();
	__malloc_hook = __my_malloc_hook;

	return ptr;
}


static void __my_free_hook(void* ptr, const void* caller)
{
	__free_hook = __old_free_hook;
	LOCK_ACQUIRE();

	free(ptr);

	LOCK_RELEASE();
	__free_hook = __my_free_hook;
}


static void* __my_realloc_hook(void* ptr, size_t size, const void* caller)
{
	__realloc_hook = __old_realloc_hook;
	LOCK_ACQUIRE();

	void* rptr = realloc(ptr, size);

	LOCK_RELEASE();

	__realloc_hook = __my_realloc_hook;

	return rptr;
}


static void* __my_memalign_hook(size_t alignment, size_t size, const void* caller)
{
	__memalign_hook = __old_memalign_hook;
	LOCK_ACQUIRE();

	void* ptr = memalign(alignment, size);

	LOCK_RELEASE();
	__memalign_hook = __my_memalign_hook;

	return ptr;
}


static void __attribute__((noinline)) __sm_init(void)
{
	int do_init; //Whether to do initialization

	do_init = __sm_init_region();

	if(do_init) {
		atexit(__sm_destroy);

		sm_region->lock = 0;
	}

	__old_malloc_hook = __malloc_hook;
	__old_free_hook = __free_hook;
	__old_realloc_hook = __realloc_hook;
	__old_memalign_hook = __memalign_hook;

	__malloc_hook = __my_malloc_hook;
	__free_hook = __my_free_hook;
	__realloc_hook = __my_realloc_hook;
	__memalign_hook = __my_memalign_hook;
}


void (*__malloc_initialize_hook)(void) = __sm_init;

