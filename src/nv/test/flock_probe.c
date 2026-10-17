/* Run under qemu-user (src/nv/Makefile `test`): the flock(2) numbers in lock.h
 * against a real Linux kernel. Two open file descriptions of one file conflict
 * even inside one process, so: the first lock is granted, the second is
 * refused with EAGAIN, and it is granted once the first descriptor is closed.
 * That is exactly the sequence env_lock in main.c depends on. */
#include "sys.h"
#include "io.h"
#include "lock.h"

int main(void)
{
	const char *path = "/tmp/nv-flock-probe";
	long a = sys_create(path, O_RDWR | O_CREAT, 0600);
	long b = sys_open(path, O_RDONLY);
	long r1, r2, r3;

	if (a < 0 || b < 0) {
		out("FAIL  cannot open the probe file\n");
		return 1;
	}
	r1 = syscall3(NV_NR_FLOCK, a, NV_LOCK_EX | NV_LOCK_NB, 0);
	r2 = syscall3(NV_NR_FLOCK, b, NV_LOCK_EX | NV_LOCK_NB, 0);
	sys_close((int)a);
	r3 = syscall3(NV_NR_FLOCK, b, NV_LOCK_EX | NV_LOCK_NB, 0);
	sys_unlink(path);
	if (r1 != 0) { out_fmt("FAIL  first flock returned %d\n", r1); return 1; }
	if (r2 != -NV_EAGAIN) { out_fmt("FAIL  second flock returned %d, want -EAGAIN\n", r2); return 1; }
	if (r3 != 0) { out_fmt("FAIL  flock after release returned %d\n", r3); return 1; }
	out("ok    flock: granted, refused with EAGAIN while held, granted after release\n");
	return 0;
}
