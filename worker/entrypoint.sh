#!/bin/sh
# Boots the emulated Indy and hands it one job over the /job contract.
#
# Cold boot rather than snapshot restore: restoring a snapshot panics this
# guest when an NFS mount is live, and leaves the serial console dead when it
# is not. Boot is about five minutes, which is small against an IRIX build.
set -u

export IRIS_SOCKET=/tmp/iris.sock
CI=/opt/iris/iris-ci
BOOT_TIMEOUT=${BOOT_TIMEOUT:-600}
JOB_TIMEOUT=${JOB_TIMEOUT:-14400}
NAME=${SGUG_RUNNER_NAME:-irix-worker}

test -f /job/job.json || { echo "no /job/job.json" >&2; exit 1; }

cd /opt/iris
./iris --config worker.toml >/tmp/iris.log 2>&1 &
IRIS_PID=$!

# The socket appears before the machine is started.
n=0
while [ ! -S "$IRIS_SOCKET" ] && [ $n -lt 60 ]; do sleep 1; n=$((n + 1)); done
[ -S "$IRIS_SOCKET" ] || { echo "iris never opened its control socket" >&2; exit 1; }

$CI start -q || exit 1

# The saved NVRAM autoboots, so the maintenance menu never appears.
# Timeouts here are seconds, not milliseconds.
$CI serial-wait --timeout "$BOOT_TIMEOUT" -q "console login:" || {
	echo "guest did not reach a login prompt" >&2; exit 1; }

$CI login root -q || { echo "login failed" >&2; exit 1; }

# root's shell is bash, so $status would never be set. mount's own status is
# not trustworthy through the serial scrape, so check the result instead.
$CI run --shell sh --timeout 120 -q "mount -o vers=3 192.168.0.1:/ /job" \
	>/dev/null 2>&1
$CI run --shell sh --timeout 60 -q "test -f /job/job.json" || {
	echo "guest could not read the job export" >&2; exit 1; }

$CI run --shell sh --timeout "$JOB_TIMEOUT" \
	"SGUG_RUNNER_NAME=$NAME /usr/local/runner/runjob.sh"
rc=$?

$CI quit >/dev/null 2>&1
wait $IRIS_PID 2>/dev/null
exit $rc
