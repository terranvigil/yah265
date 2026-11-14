#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""fleet.py -- several spot instances at once, for the cloud parity tables.

ec2.sh holds one probe box. The tables need a media hub and a handful of
timing boxes on two architectures, each launched against a list of
instance types and zones (spot capacity is thin), each recorded in the
EC2 ledger, each torn down by name. The owner's terms are enforced here:
spot only, at most MAXV dollars per vCPU-hour, instance-initiated shutdown
terminates, a dead-man shutdown armed at boot.

    fleet.py init                     key pair + security group (ssh from
                                      this host, everything inside the group)
    fleet.py hub-up                   the media hub: a persistent gp3 volume
                                      and a small spot box that mounts it
    fleet.py up NAME TYPES [--azs a,c] [--disk GB] [--tput MBS]
                                      one worker; TYPES is a comma list tried
                                      in order, each in every zone listed
    fleet.py ip NAME | pip NAME       public / private address
    fleet.py ssh NAME [CMD...]
    fleet.py down NAME                terminate, record the ledger row
    fleet.py status                   every box this fleet holds
    fleet.py teardown                 everything: boxes, the hub volume, the
                                      key pair, the group; lists what's left

State lives in $FLEET_STATE (a directory: the key, ids, launch records).
Every box is named yah265-ct-<NAME> so `teardown` can find strays.
"""
import datetime
import json
import os
import subprocess
import sys
import time

REGION = os.environ.get("REGION", "us-east-2")
STATE = os.environ.get("FLEET_STATE", os.path.expanduser(
    "~/src/yah265/local/cloudtab-2026-11-13/state"))
LEDGER = os.environ.get("FLEET_LEDGER", os.path.expanduser(
    "~/src/yah265/local/ec2-ledger.md"))
PREFIX = "yah265-ct"
MAXV = float(os.environ.get("MAXV", "0.04"))   # dollars per vCPU-hour
DEADMAN_MIN = int(os.environ.get("DEADMAN_MIN", "90"))
AWS = ["aws", "--region", REGION]


def aws(*args, text=True, check=True):
    r = subprocess.run(AWS + list(args), capture_output=True, text=True)
    if check and r.returncode != 0:
        raise RuntimeError(r.stderr.strip())
    return r.stdout.strip() if text else r


def jaws(*args):
    return json.loads(aws(*args, "--output", "json") or "null")


def sp(*p):
    return os.path.join(STATE, *p)


def load(name):
    try:
        with open(sp(f"{name}.json")) as f:
            return json.load(f)
    except OSError:
        return None


def save(name, d):
    os.makedirs(STATE, exist_ok=True)
    with open(sp(f"{name}.json"), "w") as f:
        json.dump(d, f, indent=1)


def now():
    return datetime.datetime.now(datetime.timezone.utc)


def init():
    os.makedirs(STATE, exist_ok=True)
    g = load("_group")
    if g:
        print("already:", g)
        return g
    tag = f"{PREFIX}-{now().strftime('%Y%m%d%H%M%S')}"
    key = aws("ec2", "create-key-pair", "--key-name", tag, "--key-type",
              "ed25519", "--query", "KeyMaterial", "--output", "text")
    with open(sp("key.pem"), "w") as f:
        f.write(key + "\n")
    os.chmod(sp("key.pem"), 0o600)
    vpc = aws("ec2", "describe-vpcs", "--filters", "Name=is-default,Values=true",
              "--query", "Vpcs[0].VpcId", "--output", "text")
    sg = aws("ec2", "create-security-group", "--group-name", tag,
             "--description", "yah265 cloud tables", "--vpc-id", vpc,
             "--query", "GroupId", "--output", "text")
    myip = subprocess.run(["curl", "-s", "https://checkip.amazonaws.com"],
                          capture_output=True, text=True).stdout.strip()
    aws("ec2", "authorize-security-group-ingress", "--group-id", sg,
        "--protocol", "tcp", "--port", "22", "--cidr", f"{myip}/32")
    aws("ec2", "authorize-security-group-ingress", "--group-id", sg,
        "--ip-permissions",
        json.dumps([{"IpProtocol": "-1",
                     "UserIdGroupPairs": [{"GroupId": sg}]}]))
    g = {"key": tag, "sg": sg, "vpc": vpc, "myip": myip}
    save("_group", g)
    print(g)
    return g


def type_info(t):
    d = jaws("ec2", "describe-instance-types", "--instance-types", t)
    it = d["InstanceTypes"][0]
    return (it["VCpuInfo"]["DefaultVCpus"],
            it["ProcessorInfo"]["SupportedArchitectures"][0])


def subnet_of(az):
    return aws("ec2", "describe-subnets", "--filters",
               "Name=default-for-az,Values=true",
               f"Name=availability-zone,Values={REGION}{az}",
               "--query", "Subnets[0].SubnetId", "--output", "text")


def spot_price(t, az):
    s = aws("ec2", "describe-spot-price-history", "--start-time",
            now().strftime("%Y-%m-%dT%H:%M:%S"), "--product-descriptions",
            "Linux/UNIX", "--instance-types", t, "--availability-zone",
            f"{REGION}{az}", "--query", "SpotPriceHistory[0].SpotPrice",
            "--output", "text")
    try:
        return float(s)
    except ValueError:
        return None


def ami(arch):
    deb = "arm64" if arch == "arm64" else "amd64"
    return aws("ssm", "get-parameter", "--name",
               f"/aws/service/canonical/ubuntu/server/24.04/stable/current/"
               f"{deb}/hvm/ebs-gp3/ami-id", "--query", "Parameter.Value",
               "--output", "text")


def launch(name, types, azs, disk, tput, iops, purpose, extra_bdm=None):
    g = load("_group") or sys.exit("fleet: run init first")
    if load(name) and load(name).get("state") == "up":
        sys.exit(f"fleet: {name} is already up")
    tag = f"{PREFIX}-{name}"
    ud = sp(f"{name}.user-data")
    with open(ud, "w") as f:
        f.write(f"#!/bin/sh\nshutdown -h +{DEADMAN_MIN}\n")
    errs = []
    for t in types:
        n, arch = type_info(t)
        maxp = round(MAXV * n, 4)
        image = ami(arch)
        for az in azs:
            price = spot_price(t, az)
            if price is not None and price > maxp:
                errs.append(f"{t} {az}: spot {price} over cap {maxp}")
                continue
            bdm = [{"DeviceName": "/dev/sda1",
                    "Ebs": {"VolumeSize": disk, "VolumeType": "gp3",
                            "Throughput": tput, "Iops": iops,
                            "DeleteOnTermination": True}}]
            r = aws("ec2", "run-instances", "--image-id", image,
                    "--instance-type", t, "--key-name", g["key"],
                    "--security-group-ids", g["sg"],
                    "--subnet-id", subnet_of(az),
                    *(["--private-ip-address", os.environ["PIP"]]
                      if os.environ.get("PIP") else []),
                    "--instance-initiated-shutdown-behavior", "terminate",
                    "--instance-market-options", json.dumps(
                        {"MarketType": "spot", "SpotOptions": {
                            "MaxPrice": str(maxp),
                            "SpotInstanceType": "one-time",
                            "InstanceInterruptionBehavior": "terminate"}}),
                    "--block-device-mappings", json.dumps(bdm),
                    "--user-data", f"file://{ud}",
                    "--tag-specifications",
                    f"ResourceType=instance,Tags=[{{Key=Name,Value={tag}}}]",
                    f"ResourceType=volume,Tags=[{{Key=Name,Value={tag}}}]",
                    "--query", "Instances[0].InstanceId", "--output", "text",
                    check=False, text=False)
            if r.returncode != 0:
                errs.append(f"{t} {az}: {r.stderr.strip().splitlines()[-1][:160]}")
                continue
            iid = r.stdout.strip()
            aws("ec2", "wait", "instance-running", "--instance-ids", iid)
            d = jaws("ec2", "describe-instances", "--instance-ids", iid)
            ins = d["Reservations"][0]["Instances"][0]
            rec = {"name": name, "tag": tag, "iid": iid, "type": t,
                   "vcpus": n, "arch": arch, "az": f"{REGION}{az}",
                   "price": price, "cap": maxp, "purpose": purpose,
                   "start": ins["LaunchTime"],
                   "ip": ins.get("PublicIpAddress"),
                   "pip": ins.get("PrivateIpAddress"),
                   "sir": ins.get("SpotInstanceRequestId"),
                   "disk": disk, "tput": tput, "iops": iops, "state": "up"}
            save(name, rec)
            print(json.dumps(rec))
            return rec
    print("\n".join(errs), file=sys.stderr)
    sys.exit(f"fleet: no capacity for {name} in {types} x {azs}")


def ssh_args(name):
    rec = load(name) or sys.exit(f"fleet: no box {name}")
    return ["ssh", "-i", sp("key.pem"), "-o", "IdentitiesOnly=yes",
            "-o", "StrictHostKeyChecking=accept-new",
            "-o", f"UserKnownHostsFile={sp('known_hosts')}",
            "-o", "ServerAliveInterval=30", "-o", "ConnectTimeout=20",
            f"ubuntu@{rec['ip']}"]


def instance_state(iid):
    try:
        d = jaws("ec2", "describe-instances", "--instance-ids", iid)
        ins = d["Reservations"][0]["Instances"][0]
    except (RuntimeError, IndexError):
        return "terminated", "gone from describe-instances"
    return ins["State"]["Name"], ins.get("StateTransitionReason", "")


def spot_end(rec):
    """(end time, why) from the spot request once the box is gone."""
    try:
        d = jaws("ec2", "describe-spot-instance-requests",
                 "--spot-instance-request-ids", rec["sir"])
        st = d["SpotInstanceRequests"][0]["Status"]
        t = datetime.datetime.fromisoformat(st["UpdateTime"].replace("Z", "+00:00"))
        why = ("spot reclaim" if "no-capacity" in st["Code"] or
               "marked-for" in st["Code"] else st["Code"])
        return t, why
    except (RuntimeError, IndexError, KeyError):
        return None, None


def ledger_row(rec, end, why):
    st = datetime.datetime.fromisoformat(rec["start"].replace("Z", "+00:00"))
    hrs = (end - st).total_seconds() / 3600
    gp3 = rec.get("disk", 0) * 0.08 / 730 + max(0, rec.get("tput", 125) - 125) \
        * 0.04 / 730 + max(0, rec.get("iops", 3000) - 3000) * 0.005 / 730
    cost = hrs * ((rec["price"] or rec["cap"]) + 0.005 + gp3)
    row = (f"| ct-{rec['name']} | {st.date()} | {rec['purpose']} | "
           f"{rec['iid']} | {rec['type']} | {rec['az']} | "
           f"{rec['price']} ({rec['cap']}) | {st.strftime('%H:%M:%S')} | "
           f"{end.strftime('%H:%M:%S')} ({why}) | {hrs:.3f} | {cost:.3f} | |\n")
    with open(LEDGER, "a") as f:
        f.write(row)
    return cost


def down(name, why="terminated by us"):
    rec = load(name) or sys.exit(f"fleet: no box {name}")
    if rec.get("state") != "up":
        print(f"{name}: already {rec.get('state')}")
        return
    st, reason = instance_state(rec["iid"])
    end = None
    if st not in ("terminated", "shutting-down"):
        aws("ec2", "terminate-instances", "--instance-ids", rec["iid"])
        aws("ec2", "wait", "instance-terminated", "--instance-ids",
            rec["iid"])
    else:
        end, w = spot_end(rec)
        if w and w not in ("instance-terminated-by-user", "fulfilled"):
            why = w if "reclaim" in w else (
                "self shutdown" if "shutdown" in reason.lower() else w)
    end = end or now()
    cost = ledger_row(rec, end, why)
    rec.update(state="down", end=end.isoformat(), cost=cost, why=why)
    save(name, rec)
    print(f"{name}: down ({why}), est ${cost:.3f}")


def hub_up():
    g = load("_group") or sys.exit("fleet: run init first")
    h = load("_hubvol")
    az = os.environ.get("HUB_AZ", "c")
    if not h:
        vid = aws("ec2", "create-volume", "--availability-zone",
                  f"{REGION}{az}", "--size", os.environ.get("HUB_GB", "420"),
                  "--volume-type", "gp3", "--throughput", "500", "--iops",
                  "4000", "--tag-specifications",
                  f"ResourceType=volume,Tags=[{{Key=Name,Value={PREFIX}-hubvol}}]",
                  "--query", "VolumeId", "--output", "text")
        aws("ec2", "wait", "volume-available", "--volume-ids", vid)
        h = {"vid": vid, "az": f"{REGION}{az}",
             "created": now().isoformat(), "gb": int(os.environ.get("HUB_GB", "420"))}
        save("_hubvol", h)
    rec = launch("hub", os.environ.get(
        "HUB_TYPES", "c7g.xlarge,m7g.xlarge,c6g.xlarge,c7g.2xlarge").split(","),
        [az], 16, 125, 3000, "media hub (serves the packed media to the "
        "timing boxes)")
    aws("ec2", "attach-volume", "--volume-id", h["vid"], "--instance-id",
        rec["iid"], "--device", "/dev/sdf")
    aws("ec2", "wait", "volume-in-use", "--volume-ids", h["vid"])
    print("hub up; volume attached as /dev/sdf")


def status():
    for fn in sorted(os.listdir(STATE)):
        if fn.endswith(".json") and not fn.startswith("_"):
            r = load(fn[:-5])
            if r.get("state") == "up":
                st, why = instance_state(r["iid"])
                print(f"{r['name']:<10} {r['type']:<13} {r['az']} {st:<10} "
                      f"{r['ip']} {r['pip']} since {r['start']} {why}")


def teardown():
    for fn in sorted(os.listdir(STATE)):
        if fn.endswith(".json") and not fn.startswith("_"):
            r = load(fn[:-5])
            if r.get("state") == "up":
                down(r["name"])
    h = load("_hubvol")
    if h:
        try:
            aws("ec2", "wait", "volume-available", "--volume-ids", h["vid"])
            aws("ec2", "delete-volume", "--volume-id", h["vid"])
            hrs = (now() - datetime.datetime.fromisoformat(h["created"])
                   ).total_seconds() / 3600
            print(f"hub volume {h['vid']} deleted after {hrs:.2f} h, "
                  f"est ${hrs * (h['gb'] * 0.08 + 375 * 0.04 + 1000 * 0.005) / 730:.3f}")
            h["deleted"] = now().isoformat()
            save("_hubvol_deleted", h)
            os.remove(sp("_hubvol.json"))
        except RuntimeError as e:
            print("hub volume:", e)
    g = load("_group")
    if g:
        for _ in range(20):
            try:
                aws("ec2", "delete-security-group", "--group-id", g["sg"])
                break
            except RuntimeError:
                time.sleep(15)
        aws("ec2", "delete-key-pair", "--key-name", g["key"])
        save("_group_deleted", g)
        os.remove(sp("_group.json"))
    leftovers()


def leftovers():
    print("left behind (should be empty):")
    for q in (["ec2", "describe-instances", "--filters",
               f"Name=tag:Name,Values={PREFIX}-*",
               "Name=instance-state-name,Values=pending,running,stopping,"
               "stopped,shutting-down", "--query",
               "Reservations[].Instances[].[InstanceId,State.Name]"],
              ["ec2", "describe-volumes", "--filters",
               f"Name=tag:Name,Values={PREFIX}-*", "--query",
               "Volumes[].[VolumeId,State]"],
              ["ec2", "describe-security-groups", "--filters",
               f"Name=group-name,Values={PREFIX}-*", "--query",
               "SecurityGroups[].GroupId"],
              ["ec2", "describe-key-pairs", "--filters",
               f"Name=key-name,Values={PREFIX}-*", "--query",
               "KeyPairs[].KeyName"]):
        out = aws(*q, "--output", "text")
        print(f"  {q[1]}: {out or '(none)'}")


def main(a):
    if not a:
        sys.exit(__doc__)
    cmd = a[0]
    if cmd == "init":
        init()
    elif cmd == "hub-up":
        hub_up()
    elif cmd == "up":
        import argparse
        p = argparse.ArgumentParser()
        p.add_argument("name")
        p.add_argument("types")
        p.add_argument("--azs", default="c,a,b")
        p.add_argument("--disk", type=int, default=200)
        p.add_argument("--tput", type=int, default=600)
        p.add_argument("--iops", type=int, default=6000)
        p.add_argument("--purpose", default="parity tables")
        o = p.parse_args(a[1:])
        launch(o.name, o.types.split(","), o.azs.split(","), o.disk, o.tput,
               o.iops, o.purpose)
    elif cmd == "ip":
        print(load(a[1])["ip"])
    elif cmd == "pip":
        print(load(a[1])["pip"])
    elif cmd == "ssh":
        os.execvp("ssh", ssh_args(a[1]) + a[2:])
    elif cmd == "sshargs":
        print(" ".join(ssh_args(a[1])))
    elif cmd == "down":
        down(a[1], *(a[2:3]))
    elif cmd == "status":
        status()
    elif cmd == "teardown":
        teardown()
    elif cmd == "leftovers":
        leftovers()
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
