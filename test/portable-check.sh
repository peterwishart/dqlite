#!/bin/sh
# portable-check.sh -- exercise the portable code paths that a default
# `make check` leaves dead, by re-running the relevant test binaries with the
# environment switches that gate them:
#
#   DQLITE_IO_BACKEND=threadpool  portable libuv-threadpool write backend
#                                 (src/raft/uv_writer.c, uvWriterWorkCbPortable)
#   DQLITE_VFS_NO_MREMAP=1        no-mremap shared-memory remap fallback
#                                 (src/vfs.c, mmap MAP_FIXED branches)
#   DQLITE_IO_NO_DIRECT=1         forced-buffered (no O_DIRECT) I/O path
#                                 (src/raft/uv_fs.c)
#
# Usage: sh test/portable-check.sh [-s] [BUILD_DIR]
#
# BUILD_DIR (default: .) is an EXISTING build directory with the test binaries
# at its top level; both an automake build tree (after `make check-norun`) and
# a CMake build directory work. Each switch prints a one-shot
# "dqlite: WARNING: ..." line on stderr per process: that is by design (the
# switches must never be silent) and is NOT a failure. Note that the munit
# runner captures each test's stderr and replays it only when the test fails
# (or when --show-stderr is passed), so in a fully passing run the warnings
# are usually not visible; run a binary with --no-fork --show-stderr to see
# the warning exactly once.
#
# -s additionally runs the slower integration cluster/ and fsm/ suites with
#    threadpool + no-mremap combined; the default run stays bounded.

set -u

STRESS=0
BUILD_DIR=.

while [ $# -gt 0 ]; do
	case "$1" in
	-s) STRESS=1 ;;
	-*)
		echo "usage: sh $0 [-s] [BUILD_DIR]" >&2
		exit 2
		;;
	*) BUILD_DIR=$1 ;;
	esac
	shift
done

find_bin() {
	if [ -x "$BUILD_DIR/$1" ]; then
		echo "$BUILD_DIR/$1"
	elif [ -x "$BUILD_DIR/$1.exe" ]; then
		echo "$BUILD_DIR/$1.exe"
	else
		echo "portable-check: $1 not found in '$BUILD_DIR'." >&2
		echo "Build the tests first: 'make check-norun' (automake)" \
		     "or build the CMake test targets, then pass the build" \
		     "directory as the first argument." >&2
		exit 1
	fi
}

UNIT=$(find_bin unit-test) || exit 1
RAFT_UV_UNIT=$(find_bin raft-uv-unit-test) || exit 1
RAFT_UV_INTEGRATION=$(find_bin raft-uv-integration-test) || exit 1
if [ "$STRESS" -eq 1 ]; then
	INTEGRATION=$(find_bin integration-test) || exit 1
fi

PASSES=0

banner() {
	PASSES=$((PASSES + 1))
	echo ""
	echo "==== portable-check pass $PASSES: $1"
	echo "==== env: $2"
}

run() {
	echo "---- running: $*"
	echo "---- (one-shot 'dqlite: WARNING: ...' stderr lines are expected;"
	echo "----  the munit runner replays them only on failure or --show-stderr)"
	"$@" || {
		status=$?
		echo "portable-check: FAILED (exit $status): $*" >&2
		exit "$status"
	}
}

banner "threadpool write backend" "DQLITE_IO_BACKEND=threadpool"
run env DQLITE_IO_BACKEND=threadpool "$RAFT_UV_UNIT"
run env DQLITE_IO_BACKEND=threadpool "$RAFT_UV_INTEGRATION"

banner "no-mremap VFS fallback" "DQLITE_VFS_NO_MREMAP=1"
run env DQLITE_VFS_NO_MREMAP=1 "$UNIT"

banner "forced-buffered I/O (no O_DIRECT)" "DQLITE_IO_NO_DIRECT=1"
run env DQLITE_IO_NO_DIRECT=1 "$RAFT_UV_UNIT"
run env DQLITE_IO_NO_DIRECT=1 "$RAFT_UV_INTEGRATION"

if [ "$STRESS" -eq 1 ]; then
	banner "combined stress (integration cluster/ + fsm/ suites)" \
	       "DQLITE_IO_BACKEND=threadpool DQLITE_VFS_NO_MREMAP=1"
	run env DQLITE_IO_BACKEND=threadpool DQLITE_VFS_NO_MREMAP=1 \
	    "$INTEGRATION" cluster/ fsm/
fi

echo ""
echo "==== portable-check: all $PASSES passes OK"
