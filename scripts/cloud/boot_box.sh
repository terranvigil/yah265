#!/bin/bash
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# boot_box.sh NAME PLAN.json -- from the Mac: give a fleet box the hub key,
# the cloud scripts and its plan, then run setup_tables.sh and tables_run.py
# on it, detached (nohup), logging to ~/t/run.log.
set -euo pipefail
NAME=$1; PLAN=$2
D=$(cd "$(dirname "$0")" && pwd)
ST=${FLEET_STATE:-$HOME/src/yah265/local/cloudtab-2026-11-12/state}
IP=$(python3 -c "import json;print(json.load(open('$ST/$NAME.json'))['ip'])")
HUB=$(python3 -c "import json;print(json.load(open('$PLAN'))['hub'])")
O=(-i "$ST/key.pem" -o IdentitiesOnly=yes -o UserKnownHostsFile="$ST/known_hosts" -o StrictHostKeyChecking=accept-new -o ConnectTimeout=20)
for i in $(seq 30); do ssh "${O[@]}" ubuntu@$IP true 2>/dev/null && break; sleep 10; done
ssh "${O[@]}" ubuntu@$IP 'mkdir -p ~/.ssh ~/t/cloud'
scp "${O[@]}" -q "$ST/key.pem" ubuntu@$IP:.ssh/hub.pem
scp "${O[@]}" -q "$D"/*.py "$D"/*.sh "$PLAN" ubuntu@$IP:t/cloud/
FILES=$(python3 -c "import json;print(' '.join(sorted({c['src']['file'] for c in json.load(open('$PLAN'))['cells']})))")
P=$(basename "$PLAN")
ssh -n -f "${O[@]}" ubuntu@$IP "chmod 600 ~/.ssh/hub.pem; cd ~/t && nohup bash -c 'bash cloud/setup_tables.sh $HUB $FILES > setup.log 2>&1 && mkdir -p src/scripts/cloud && cp cloud/*.py src/scripts/cloud/ && ${PRE:-true} && python3 cloud/${RUNNER:-tables_run.py} cloud/$P' > run.log 2>&1 < /dev/null &"
echo "booted $NAME ($IP) with $P; files: $FILES"
