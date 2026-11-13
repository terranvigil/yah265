#!/bin/bash
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# chain_box.sh NAME PLAN.json CURRENT_PLAN.json -- queue a second plan on a box: once its
# current runner ends, cancel the halt it scheduled, pull the new plan's
# media from the hub, and run the new plan (which halts the box at its end).
# Chains on one box hold chain.lock in turn, so two never run at once.
set -euo pipefail
NAME=$1; PLAN=$2; CUR=$3
D=$(cd "$(dirname "$0")" && pwd)
ST=${FLEET_STATE:-$HOME/src/yah265/local/cloudtab-2026-11-12/state}
IP=$(python3 -c "import json;print(json.load(open('$ST/$NAME.json'))['ip'])")
HUB=$(python3 -c "import json;print(json.load(open('$PLAN'))['hub'])")
O=(-i "$ST/key.pem" -o IdentitiesOnly=yes -o UserKnownHostsFile="$ST/known_hosts" -o StrictHostKeyChecking=accept-new -o ConnectTimeout=20)
scp "${O[@]}" -q "$D"/*.py "$D"/*.sh "$PLAN" ubuntu@$IP:t/cloud/
P=$(basename "$PLAN")
FILES=$(python3 -c "import json;print(' '.join(sorted({c['src']['file'] for c in json.load(open('$PLAN'))['cells']})))")
ssh -n -f "${O[@]}" ubuntu@$IP "cd ~/t && nohup flock chain.lock bash -c 'busy() { pgrep -f \"[s]etup_tables.sh\" >/dev/null || pgrep -f \"python3 cloud/[t]ables_run.py cloud/$CUR\" >/dev/null; }; while busy || { sleep 20; busy; }; do sleep 20; done; sudo shutdown -c; sudo shutdown -h +60; for f in $FILES; do mkdir -p /data/\$(dirname \$f); until rsync -a -e \"ssh -i \$HOME/.ssh/hub.pem -o StrictHostKeyChecking=no\" ubuntu@$HUB:/data/\$f/ /data/\$f/ 2>/dev/null || rsync -a -e \"ssh -i \$HOME/.ssh/hub.pem -o StrictHostKeyChecking=no\" ubuntu@$HUB:/data/\$f /data/\$f; do sleep 30; done; done; cp cloud/*.py src/scripts/cloud/; python3 cloud/tables_run.py cloud/$P' > chain_$P.log 2>&1 < /dev/null &"
echo "chained $P on $NAME"
