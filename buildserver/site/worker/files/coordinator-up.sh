#!/bin/sh
# Starts the worker site's coordinator unless it runs, and waits until it answers (installed by
# buildserver/site/worker.py `deploy` as /srv/buildserver/bin/coordinator-up.sh). `amb` runs it through
# `ssh worker pct exec 803 -- …` when the coordinator does not answer; the service normally never stops (OpenRC's
# supervise-daemon restarts it), so this is for a crash loop it gave up on, or a service someone stopped.
set -eu
rc-service amb-coordinator status >/dev/null 2>&1 || rc-service amb-coordinator start >/dev/null
i=0
while [ $i -lt 60 ]; do
  if curl -sf -o /dev/null http://127.0.0.1:2207/healthz; then
    echo "running"
    exit 0
  fi
  i=$((i + 1))
  sleep 0.5
done
echo "amb-coordinator did not answer within 30 s; see /srv/buildserver/coordinator/coordinator.log" >&2
exit 1
