#!/bin/bash
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# restart_box.sh NAME PLAN.json -- stop a box's runner and every encode it
# started, copy the current cloud scripts and plan, and start the runner
# again; it resumes from the records it holds.
set -euo pipefail
NAME=$1; PLAN=$2
D=$(cd "$(dirname "$0")" && pwd)
ST=${FLEET_STATE:-$HOME/src/yah265/local/cloudtab-2026-11-13/state}
IP=$(python3 -c "import json;print(json.load(open('$ST/$NAME.json'))['ip'])")
O=(-i "$ST/key.pem" -o IdentitiesOnly=yes -o UserKnownHostsFile="$ST/known_hosts" -o StrictHostKeyChecking=accept-new -o ConnectTimeout=20)
ssh "${O[@]}" ubuntu@$IP 'pkill -f "[t]ables_run.py"; pkill -f "[l]adder_run.py"; pkill -f "[x]265build/x265"; pkill -f "[b]uild-rel/n265"; pkill -f "[f]eeder.py"; pkill -f "^cat /home/ubuntu/t/wor[k]"; true'
scp "${O[@]}" -q "$D"/*.py "$D"/*.sh "$PLAN" ubuntu@$IP:t/cloud/
P=$(basename "$PLAN")
RUNNER=${RUNNER:-tables_run.py}
ssh -n -f "${O[@]}" ubuntu@$IP "cd ~/t && cp cloud/*.py src/scripts/cloud/ && nohup python3 cloud/$RUNNER cloud/$P >> run.log 2>&1 < /dev/null &"
echo "restarted $NAME with $P"
