/* memprobe: read or write physical pages through /dev/mem. Static, tiny.
 *   memprobe r ADDR LEN         hexdump of the first 64 bytes + 32-bit sum of LEN bytes
 *   memprobe w ADDR LEN TAG     fill LEN bytes with TAG repeated (TAG up to 60 chars),
 *                               the page offset written in every 64-byte line
 *   memprobe d ADDR LEN         raw bytes to stdout
 *   memprobe set ADDR VAL       write one 32-bit register, then read it back
 *   memprobe reg ADDR           read one 32-bit register (uncached KSEG1 address
 *                               given as its physical address)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

static void *map(unsigned long phys, unsigned long len, int *fd)
{
	unsigned long pg = phys & ~4095UL, off = phys - pg;
	*fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (*fd < 0) { perror("/dev/mem"); exit(2); }
	void *p = mmap(0, off + len, PROT_READ | PROT_WRITE, MAP_SHARED, *fd, pg);
	if (p == MAP_FAILED) { perror("mmap"); exit(2); }
	return (char *)p + off;
}

int main(int argc, char **argv)
{
	int fd;
	if (argc < 3) { fprintf(stderr, "usage: memprobe r|w|reg ADDR [LEN [TAG]]\n"); return 1; }
	unsigned long addr = strtoul(argv[2], 0, 16);
	if (!strcmp(argv[1], "set")) {
		volatile unsigned int *r = map(addr, 4, &fd);
		unsigned int v = strtoul(argc > 3 ? argv[3] : "0", 0, 16);
		*r = v;
		printf("%08lx <= %08x, reads back %08x\n", addr, v, *r);
		return 0;
	}
	if (argv[1][0] == 'g' || !strcmp(argv[1], "reg")) {
		volatile unsigned int *r = map(addr, 4, &fd);
		printf("%08lx = %08x\n", addr, *r);
		return 0;
	}
	unsigned long len = argc > 3 ? strtoul(argv[3], 0, 16) : 4096;
	volatile unsigned char *m = map(addr, len, &fd);
	if (argv[1][0] == 'd') {
		for (unsigned long o = 0; o < len; o++) putchar(m[o]);
		return 0;
	}
	if (argv[1][0] == 'w') {
		const char *tag = argc > 4 ? argv[4] : "MEMPROBE";
		char line[64];
		for (unsigned long o = 0; o < len; o += 64) {
			memset(line, '.', 64);
			int n = snprintf(line, 64, "%s@%08lx+%06lx", tag, addr, o);
			if (n < 63) line[n] = ' ';
			line[63] = '\n';
			for (int i = 0; i < 64 && o + i < len; i++) m[o + i] = line[i];
		}
		printf("wrote %lx bytes at %08lx\n", len, addr);
	}
	unsigned int sum = 0;
	for (unsigned long o = 0; o < len; o++) sum = sum * 31 + m[o];
	printf("%08lx len %lx sum %08x |", addr, len, sum);
	for (int i = 0; i < 64 && (unsigned long)i < len; i++) {
		unsigned char c = m[i];
		putchar(c >= 32 && c < 127 ? c : '.');
	}
	putchar('\n');
	return 0;
}
