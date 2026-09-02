#!/bin/sh

#This script is designed to be used with cmake builds.

export PREENY_DEBUG=1
export PREENY_INFO=1
export PREENY_ERROR=1

# Tests that actually assert something report through check(): anything that
# exits nonzero makes this whole script exit nonzero, and 77 means "this
# machine can't run that case" rather than "broken". We deliberately do NOT use
# "set -e" -- several of the eyeball-only tests below already exit nonzero
# (test_realloc under crazyrealloc aborts on purpose), and set -e would stop the
# run right there.
FAILURES=0
if command -v timeout > /dev/null 2>&1; then TIMEOUT="timeout -k 5 60"; else TIMEOUT=""; fi
check() {
	desc="$1"; shift
	$TIMEOUT "$@"
	rc=$?
	if [ $rc -eq 0 ]; then echo "=== PASS: $desc"
	elif [ $rc -eq 77 ]; then echo "=== SKIP: $desc"
	else echo "=== FAIL: $desc (exit $rc)"; FAILURES=$((FAILURES + 1)); fi
}

# Sleep
LD_PRELOAD=lib/libdesleep.so ./bin/test_sleep

# UID
./bin/test_uid
LD_PRELOAD=lib/libdeuid.so FAKE_UID=1337 FAKE_EUID=1338 ./bin/test_uid

# Rand
LD_PRELOAD=lib/libderand.so RAND=1337 ./bin/test_rand

# Realloc
LD_PRELOAD=lib/libcrazyrealloc.so RAND=1337 ./bin/test_realloc

# Sock
echo "TEST" | LD_PRELOAD=lib/libdesock.so ./bin/test_sock

# Canary
LD_PRELOAD=lib/libgetcanary.so ./bin/test_hello

# SetSTDIN
LD_PRELOAD=lib/libsetstdin.so ./bin/test_setstdin_fread
LD_PRELOAD=lib/libsetstdin.so ./bin/test_setstdin_getc
LD_PRELOAD=lib/libsetstdin.so ./bin/test_setstdin_read

# nowrite
check "nowrite: every open() is downgraded to read-only" \
	env LD_PRELOAD=lib/libnowrite.so ./bin/test_nowrite

# pdeathsig
check "pdeathsig: fork()ed descendants die with their ancestor" \
	env LD_PRELOAD=lib/libpdeathsig.so ./bin/test_pdeathsig fork
check "pdeathsig: exec'd descendants die with their ancestor" \
	env LD_PRELOAD=lib/libpdeathsig.so ./bin/test_pdeathsig exec
check "pdeathsig: clone()d descendants die with their ancestor" \
	env LD_PRELOAD=lib/libpdeathsig.so ./bin/test_pdeathsig clone
check "pdeathsig: the kill really is pdeathsig (SIGUSR2, which nothing else sends)" \
	env PREENY_PDEATHSIG=SIGUSR2 LD_PRELOAD=lib/libpdeathsig.so ./bin/test_pdeathsig fork 12
check "pdeathsig: PREENY_PDEATHSIG accepts signal names" \
	env PREENY_PDEATHSIG=SIGUSR2 LD_PRELOAD=lib/libpdeathsig.so ./bin/test_pdeathsig config 12
check "pdeathsig: a bad PREENY_PDEATHSIG falls back to SIGKILL" \
	env PREENY_PDEATHSIG=not-a-signal LD_PRELOAD=lib/libpdeathsig.so ./bin/test_pdeathsig config 9
check "pdeathsig: a child orphaned before it armed kills itself" \
	env LD_PRELOAD=lib/libpdeathsig.so ./bin/test_pdeathsig arm
check "pdeathsig: a child that disarms itself is left alone" \
	env LD_PRELOAD=lib/libpdeathsig.so ./bin/test_pdeathsig disarm 0
check "pdeathsig: a fork()ed child that loses its parent mid-arm still dies" \
	env LD_PRELOAD=lib/libpdeathsig.so ./bin/test_pdeathsig race
check "pdeathsig: a clone(CLONE_THREAD) task is never armed" \
	env LD_PRELOAD=lib/libpdeathsig.so ./bin/test_pdeathsig thread
check "pdeathsig: a clone(CLONE_SETTLS) child reaches its own code" \
	env LD_PRELOAD=lib/libpdeathsig.so ./bin/test_pdeathsig settls
check "pdeathsig: control -- without the module, nothing dies" \
	./bin/test_pdeathsig fork 0

echo "=== $FAILURES failure(s)"
exit $FAILURES
