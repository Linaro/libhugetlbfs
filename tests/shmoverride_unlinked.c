/*
 * libhugetlbfs - Easy use of Linux hugepages
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */
#include <sys/types.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <hugetlbfs.h>
#include "hugetests.h"

/*
 * Test Scenario:
 *
 * libhugetlbfs_shmoverride can be used to force shmget() to use the
 * SHM_HUGETLB flag. This test ensures that the flag is correctly used
 * based on the value of the environment variable. The assumption is
 * made that the library is being preloaded.
 */

extern int errno;

/* Global test configuration */
#define DYNAMIC_SYSCTL "/proc/sys/vm/nr_overcommit_hugepages"
static long saved_nr_hugepages = -1;
static long hpage_size, bpage_size;

/* Required pool size for test */
#define POOL_SIZE 4

/* State arrays for our mmaps */
#define NR_SLOTS	1
#define SL_TEST		0
static int map_id[NR_SLOTS];
static char *map_addr[NR_SLOTS];
static size_t map_size[NR_SLOTS];

/* Only ia64 requires this */
#ifdef __ia64__
#define ADDR (void *)(0x8000000000000000UL)
#define SHMAT_FLAGS (SHM_RND)
#else
#define ADDR (void *)(0x0UL)
#define SHMAT_FLAGS (0)
#endif

void _shmmap(int s, int hpages, int bpages, int line)
{
	map_size[s] = hpages * hpage_size + bpages * bpage_size;
	map_id[s] = shmget(IPC_PRIVATE, map_size[s], IPC_CREAT | SHM_R | SHM_W);
	if (map_id[s] < 0)
		FAIL("shmget failed size %zd from line %d: %s",
			map_size[s], line, strerror(errno));

	map_addr[s] = shmat(map_id[s], ADDR, SHMAT_FLAGS);
	if (map_addr[s] == (char *)-1)
		FAIL("shmmat failed from line %d: %s", line, strerror(errno));
}
#define shmmap(s, h, b) _shmmap(s, h, b, __LINE__)

void _shmunmap(int s, int line)
{
	if (shmdt((const void *)map_addr[s]) != 0) {
		FAIL("shmdt failed from line %d: %s", line, strerror(errno));
		return;
	}

	if (shmctl(map_id[s], IPC_RMID, NULL) == -1)
		FAIL("shmctl failed from line %d: %s", line, strerror(errno));

	map_id[s] = -1;
	map_addr[s] = NULL;
	map_size[s] = 0;
}
#define shmunmap(s) _shmunmap(s, __LINE__)

void set_nr_hugepages(unsigned long count)
{
	FILE *fd;
	unsigned long poolsize;
	count += read_meminfo("HugePages_Rsvd:");
	fd = fopen("/proc/sys/vm/nr_hugepages", "w");
	if (!fd)
		CONFIG("Cannot open nr_hugepages for writing\n");
	fprintf(fd, "%lu", count);
	fclose(fd);

	/* Confirm the resize worked */
	poolsize = read_meminfo("HugePages_Total:");
	if (poolsize != count)
		FAIL("Failed to resize pool to %lu pages. Got %lu instead\n",
			count, poolsize);
}

void run_test(char *desc, int hpages, int bpages, int pool_nr, int expect_diff)
{
	long resv_before, resv_after;
	verbose_printf("%s...\n", desc);
	set_nr_hugepages(pool_nr);

	/* untouched, shared mmap */
	resv_before = read_meminfo("HugePages_Rsvd:");
	shmmap(SL_TEST, hpages, bpages);
	resv_after = read_meminfo("HugePages_Rsvd:");
	memset(map_addr[SL_TEST], 0, map_size[SL_TEST]);
	shmunmap(SL_TEST);

	if (resv_after - resv_before != expect_diff)
		FAIL("%s: Reserve page count did not adjust by %d page. "
			"Expected %li reserved pages but got %li pages",
				desc, expect_diff,
				resv_before + expect_diff, resv_after);
}

void cleanup(void)
{
	if (saved_nr_hugepages >= 0)
		set_nr_hugepages(saved_nr_hugepages);
}

int main(int argc, char **argv)
{
	test_init(argc, argv);
	check_must_be_root();
	check_free_huge_pages(POOL_SIZE);
	saved_nr_hugepages = read_meminfo("HugePages_Total:");

	/*
	 * We cannot call check_hugepagesize because we are not linked to
	 * libhugetlbfs. This is a bit hacky but we are depending on earlier
	 * tests failing to catch when this wouldn't work
	 */
	hpage_size = read_meminfo("Hugepagesize:") * 1024;
	bpage_size = getpagesize();

	/* Run the test with small pages */
	setenv("HUGETLB_SHM", "no", 1);
	run_test("override-not-requested-aligned", 1, 0, POOL_SIZE, 0);

	/* Run the test with large pages */
	setenv("HUGETLB_SHM", "yes", 1);
	run_test("override-requested-aligned", 1, 0, POOL_SIZE, 1);

	/* Run the test with large pages but with an unaligned size */
	setenv("HUGETLB_SHM", "yes", 1);
	run_test("override-requested-unaligned", 1, 1, POOL_SIZE, 2);

	/* Run the test with no pool but requested large pages */
	set_nr_hugepages(0);
	run_test("override-requested-aligned-nopool", 1, 0, 0, 0);

	PASS();
}
