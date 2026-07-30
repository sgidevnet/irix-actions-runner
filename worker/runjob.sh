#!/usr/sgug/bin/bash
# Runs in the guest, against the NFS export. See src/serve/exec.h.

# Guest local: the contract forbids anything but the three files under /job.
/usr/local/runner/runner execjob --message /job/job.json \
    --name "${SGUG_RUNNER_NAME:-irix-worker}" --work /var/tmp/work \
    --cancel-file /job/cancel
rc=$?

[ "$rc" -eq 0 ] && : > /job/complete
exit "$rc"
