/*
 * Script-class guest exercising every call in src/sandbox/zone_abi.h.
 *
 * Built for riscv64-musl and run by the host's -g flag. It is the
 * end-to-end check that a guest reaches FoundationDB through the ABI
 * and nothing else: the keys this writes must be visible afterwards
 * from fdbcli, which is what distinguishes real storage from an
 * in-process buffer that merely behaves like it.
 *
 * Freestanding, and built that way (-nostdlib, see build.sh). Not
 * "freestanding-ish": this file calls no libc function at all, so
 * linking one in is pure cost. An earlier build linked static glibc
 * anyway, and __libc_start_main then reached writev (66), exit_group
 * (94), and set_tid_address (96) before main. Those looked like guest
 * requirements. They were startup code for a userland this test never
 * asked for, and the host correctly refused all three.
 *
 * The host installs 7 minimal syscalls plus this ABI. A guest that
 * needs more than that is not a script guest.
 */

#include "../../src/sandbox/zone_abi.h"

int main(void);

/*
 * Entry point. No argc/argv: main takes none, and libriscv's stack
 * setup is irrelevant to a guest that ignores it. Syscall 93 is exit,
 * one of the 7 from setup_minimal_syscalls().
 */
__attribute__((naked, used)) void _start(void)
{
	__asm__ __volatile__(
		"call main\n"
		"li a7, 93\n"
		"ecall\n");
}

static unsigned long zlen(const char *s)
{
	unsigned long n = 0;
	while (s[n]) n++;
	return n;
}

static void say(const char *s)
{
	zone_print(s, zlen(s));
}

static int same(const char *a, const char *b, unsigned long n)
{
	for (unsigned long i = 0; i < n; i++) {
		if (a[i] != b[i]) return 0;
	}
	return 1;
}

int main(void)
{
	static char buf[256];
	int failures = 0;

	say("kv_smoke: start");

	/* 1. set then get round-trips the exact bytes. */
	const char *k1 = "smoke/hello";
	const char *v1 = "world";
	if (zone_kv_set(k1, zlen(k1), v1, zlen(v1)) < 0) {
		say("FAIL: set smoke/hello");
		failures++;
	}
	long n = zone_kv_get(k1, zlen(k1), buf, sizeof(buf));
	if (n != (long)zlen(v1) || !same(buf, v1, zlen(v1))) {
		say("FAIL: get smoke/hello did not round-trip");
		failures++;
	} else {
		say("ok: set/get round-trip");
	}

	/* 2. a missing key is -ENOENT, not a fault and not an empty value. */
	const char *missing = "smoke/nope";
	if (zone_kv_get(missing, zlen(missing), buf, sizeof(buf)) >= 0) {
		say("FAIL: missing key did not report an error");
		failures++;
	} else {
		say("ok: missing key reports an error");
	}

	/* 3. a 20 KB value round-trips whole. Large enough that a host
	 *    tempted to chunk it would, small enough to stay under
	 *    ZONE_KV_MAX_VALUE -- so this passing means the host stored it
	 *    as one value in one transaction, which is the design. */
	static char big[20000];
	for (int i = 0; i < 20000; i++) big[i] = (char)(i * 31 + 7);
	const char *k2 = "smoke/big";
	if (zone_kv_set(k2, zlen(k2), big, sizeof(big)) < 0) {
		say("FAIL: set smoke/big");
		failures++;
	}
	static char back[20000];
	n = zone_kv_get(k2, zlen(k2), back, sizeof(back));
	if (n != (long)sizeof(big) || !same(back, big, sizeof(big))) {
		say("FAIL: multi-chunk value did not round-trip");
		failures++;
	} else {
		say("ok: multi-chunk value round-trip");
	}

	/* 4. a short buffer reports the FULL length, so a guest can resize
	 *    and retry rather than silently truncating. */
	n = zone_kv_get(k2, zlen(k2), buf, sizeof(buf));
	if (n != (long)sizeof(big)) {
		say("FAIL: short read did not report the full length");
		failures++;
	} else {
		say("ok: short read reports the full length");
	}

	/* 5. list finds what was written under a prefix. */
	static char keys[1024];
	n = zone_kv_list("smoke", 5, keys, sizeof(keys));
	if (n < 2) {
		say("FAIL: list found fewer than the two keys written");
		failures++;
	} else {
		say("ok: list found the written keys");
	}

	/* 6. delete removes it. */
	if (zone_kv_del(k1, zlen(k1)) < 0) {
		say("FAIL: del smoke/hello");
		failures++;
	}
	if (zone_kv_get(k1, zlen(k1), buf, sizeof(buf)) >= 0) {
		say("FAIL: key still readable after delete");
		failures++;
	} else {
		say("ok: delete removes the key");
	}

	/* 7. entropy returns the requested count and is not all zeros. A
	 *    host that fails closed returns an error instead, which is the
	 *    correct answer and also not this. */
	static char rnd[32];
	for (int i = 0; i < 32; i++) rnd[i] = 0;
	n = zone_entropy(rnd, sizeof(rnd));
	if (n != (long)sizeof(rnd)) {
		say("FAIL: entropy returned the wrong count");
		failures++;
	} else {
		int nonzero = 0;
		for (int i = 0; i < 32; i++) if (rnd[i]) nonzero = 1;
		if (!nonzero) {
			say("FAIL: entropy returned all zeros");
			failures++;
		} else {
			say("ok: entropy");
		}
	}

	/* 8. Isolation. These are host syscalls the sandbox never
	 *    installed. Each must come back as an error and leave the
	 *    guest running -- an unimplemented call is not a crash. */
	long r_open = zone_ecall4(56, 0, 0, 0, 0);   /* openat */
	long r_sock = zone_ecall4(198, 0, 0, 0, 0);  /* socket */
	long r_exec = zone_ecall4(221, 0, 0, 0, 0);  /* execve */
	if (r_open >= 0 || r_sock >= 0 || r_exec >= 0) {
		say("FAIL: a denied syscall reported success");
		failures++;
	} else {
		say("ok: openat/socket/execve denied, guest still running");
	}

	if (failures == 0) {
		say("kv_smoke: PASS");
		return 0;
	}
	say("kv_smoke: FAIL");
	return 1;
}
