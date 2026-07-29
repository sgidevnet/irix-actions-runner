#!/usr/sgug/bin/bash
# Guest side of the /job contract: run one job, then signal completion.
LD_LIBRARYN32_PATH=/usr/sgug/lib32; export LD_LIBRARYN32_PATH
SSL_CERT_FILE=/usr/local/runner/cert.pem; export SSL_CERT_FILE

/usr/local/runner/runner execjob --message /job/job.json \
    --name "${SGUG_RUNNER_NAME:-irix-worker}" --work /var/tmp/work \
    --cancel-file /job/cancel
rc=$?
[ "$rc" -eq 0 ] && : > /job/complete
exit "$rc"
