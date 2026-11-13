#!/bin/bash
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# setup_vmaf.sh HUB -- runs ON a ladder box after setup_tables.sh: libvmaf
# at the commit the Mac's v1 binary reports (0f9912e), float features on as
# the Mac's build has them, and the Mac's vmaf_v1.0.16 models from the hub.
set -euxo pipefail
HUB=$1
cd ~/t
sudo apt-get install -y -q --no-install-recommends git xxd
rm -rf vmaf && git clone -q https://github.com/Netflix/vmaf.git
git -C vmaf checkout -q 0f9912e
(cd vmaf/libvmaf && meson setup build -Dbuildtype=release -Denable_float=true \
    -Denable_avx512=true -Dbuilt_in_models=true && ninja -C build)
mkdir -p models
rsync -a -e "ssh -i $HOME/.ssh/hub.pem -o StrictHostKeyChecking=no" ubuntu@$HUB:/data/src/v1models.tgz .
tar -xzf v1models.tgz -C models
{ echo "vmaf: $(git -C vmaf log --oneline -1)"; ./vmaf/libvmaf/build/tools/vmaf --version; } >> env.txt
