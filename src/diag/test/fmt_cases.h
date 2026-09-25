/* One list of cases, expanded twice: once against the host's printf to produce
 * the reference, once against out_fmt on the target. Anything a handler's
 * format string uses belongs here.
 *
 * Keep the arguments' types exactly as the handlers pass them --
 * a %llu fed an int is undefined for both implementations, so a disagreement
 * there would say nothing. */
#define FMT_CASES(X) \
	X("%s|", "abc") \
	X("%-13s|", "abc") \
	X("%-13s|", "a much longer string than the field") \
	/* Over 71 characters. %s used to be built in a 72-byte scratch buffer
	 * sized for the widest NUMERIC conversion, so anything longer was
	 * silently cut -- it truncated 19 of the 67 pairs in a real U-Boot
	 * environment. The longest case here was 34 characters, which is why
	 * this suite passed throughout. */ \
	X("%s|", "setenv bootargs ${bootargs_base} ${mtdparts0} ${rst2dfl_flg}; bootm ${img0_kernel}") \
	X("%s|", "if itest.s ${sw_tryactive} == 0;then setenv bootargs ${bootargs_base} ${mtdparts0} ${rst2dfl_flg};bootm ${img0_kernel};else setenv bootargs ${bootargs_base} ${mtdparts1} ${rst2dfl_flg};bootm ${img1_kernel};fi") \
	X("%.20s|", "a string truncated deliberately by a precision, not by a buffer") \
	X("%-100s|", "left justified past the old buffer size, padded to a hundred") \
	X("%9s|", "abc") \
	X("%-2d|", 7) \
	X("port:%2d  rate:%d\n", 2, 4194296) \
	X("         queue:%2d  apr-index:%2d\n", 0, 16) \
	X("         queue:%2d  apr-index:%2d\n", 7, -8) \
	X("%8d|", -1234) \
	X("%08d|", 1234) \
	X("%d %d %d|", 0, -1, 2147483647) \
	X("%u|", 4294967295u) \
	X("%-6u|", 42u) \
	X("%x|", 0u) \
	X("%x|", 3735928559u) \
	X("%02x|", 5u) \
	X("%02x|", 255u) \
	X("%04x|", 4660u) \
	X("%08x|", 305419896u) \
	X("%08X|", 3735928559u) \
	X("%2.2x|", 7u) \
	X("%4.4x|", 7u) \
	X("%01x|", 10u) \
	X("%llu|", (unsigned long long)0) \
	X("%llu|", (unsigned long long)18446744073709551615ull) \
	X("%25llu|", (unsigned long long)1234567890123456789ull) \
	X("%lld|", (long long)-9007199254740993ll) \
	X("%c%c|", 'o', 'k') \
	X("100%%|") \
	X("Error (0x%x): %s\n", 7u, "NULL argument") \
	X("Index %d: Meter: %d|", 3, 11) \
	X("%-11s%-12s%-14s|", "a", "bb", "ccc") \
	X("port:%-4d %s|", 0, "Disable")
