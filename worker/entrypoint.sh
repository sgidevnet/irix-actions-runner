#!/bin/sh
# Cold boot rather than snapshot restore; see README.md.
set -u

export IRIS_SOCKET=/tmp/iris.sock
CI=/opt/iris/iris-ci
BOOT_TIMEOUT=${BOOT_TIMEOUT:-600}
NAME=${SGUG_RUNNER_NAME:-irix-worker}

# The container is removed on the way out, so anything not printed here is lost.
fail() {
	echo "$1" >&2
	tail -40 /tmp/console.log /tmp/iris.log >&2 2>/dev/null
	exit 1
}

case $NAME in
*[!A-Za-z0-9._-]*) fail "runner name must be alphanumeric, dot, dash or underscore" ;;
esac

test -f /job/job.json || fail "no /job/job.json"

cd /opt/iris
./iris --config worker.toml >/tmp/iris.log 2>&1 &
IRIS_PID=$!

# The socket appears before the machine is started.
n=0
while [ ! -S "$IRIS_SOCKET" ] && [ $n -lt 60 ]; do sleep 1; n=$((n + 1)); done
[ -S "$IRIS_SOCKET" ] || fail "iris never opened its control socket"

$CI start -q || fail "machine would not start"

# The saved NVRAM autoboots, so the maintenance menu never appears.
# Timeouts here are seconds, not milliseconds.
$CI serial-wait --timeout "$BOOT_TIMEOUT" -q "console login:" ||
	fail "guest did not reach a login prompt"

$CI login root -q || fail "login failed"

# --shell sh on every run: iris-ci defaults to csh and scrapes $status, while
# root's shell is bash. noac because the guest polls cancel for an mtime
# change, and IRIX would otherwise cache the attributes for up to a minute.
$CI run --shell sh --timeout 120 -q \
	"mount -o vers=3,noac 192.168.0.1:/ /job" >/dev/null 2>&1
$CI run --shell sh --timeout 60 -q "test -f /job/job.json" ||
	fail "guest could not read the job export"

$CI run --shell sh --timeout "${JOB_TIMEOUT:-14000}" \
	"SGUG_RUNNER_NAME='$NAME' /usr/local/runner/runjob.sh"
rc=$?

$CI quit >/dev/null 2>&1
kill $IRIS_PID 2>/dev/null
exit $rc
