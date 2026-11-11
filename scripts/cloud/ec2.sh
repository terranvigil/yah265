#!/bin/bash
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# ec2.sh -- one spot instance for an x86 probe, and everything it needs,
# created and torn down by name. The owner's terms (CLAUDE.md, owner-rules):
# spot only, a price cap per vCPU-hour, instance-initiated shutdown
# terminates, a dead-man shutdown at boot, and nothing left billing.
#
#   ec2.sh up      key pair + security group (ssh from this host's IP) +
#                  one spot instance; writes $STATE
#   ec2.sh ip      the instance's public IP
#   ec2.sh ssh ... ssh to it
#   ec2.sh down    terminate, wait until terminated, delete the key pair
#                  and the security group, list anything left
#
# Environment: TYPE (c7i.4xlarge), MAXPRICE (0.04 x vCPUs), DEADMAN_MIN
# (100), DISK_GB (40), REGION (us-east-2), STATE (a directory for the key
# and ids; default ./ec2-state).
set -euo pipefail

REGION=${REGION:-us-east-2}
TYPE=${TYPE:-c7i.4xlarge}
DEADMAN_MIN=${DEADMAN_MIN:-100}
DISK_GB=${DISK_GB:-40}
STATE=${STATE:-./ec2-state}
NAME=${NAME:-yah265-x86-probe}
AWS=(aws --region "$REGION")

vcpus() {
    "${AWS[@]}" ec2 describe-instance-types --instance-types "$TYPE" \
        --query 'InstanceTypes[0].VCpuInfo.DefaultVCpus' --output text
}

up() {
    mkdir -p "$STATE"
    [ -f "$STATE/instance" ] && { echo "already up: $(cat "$STATE/instance")"; exit 1; }
    local n; n=$(vcpus)
    local maxp=${MAXPRICE:-$(python3 -c "print(f'{0.04*$n:.3f}')")}
    local ami
    ami=$("${AWS[@]}" ssm get-parameter --name \
        /aws/service/canonical/ubuntu/server/24.04/stable/current/amd64/hvm/ebs-gp3/ami-id \
        --query Parameter.Value --output text)
    local myip; myip=$(curl -s https://checkip.amazonaws.com)
    local tag sg
    # a launch refused for capacity leaves the key and group; reuse them
    if [ -f "$STATE/keyname" ] && [ -f "$STATE/sg" ]; then
        tag=$(cat "$STATE/keyname"); sg=$(cat "$STATE/sg")
    else
        tag="$NAME-$(date +%Y%m%d%H%M%S)"
        "${AWS[@]}" ec2 create-key-pair --key-name "$tag" --key-type ed25519 \
            --query KeyMaterial --output text > "$STATE/key.pem"
        chmod 600 "$STATE/key.pem"
        echo "$tag" > "$STATE/keyname"
        sg=$("${AWS[@]}" ec2 create-security-group --group-name "$tag" \
            --description "x86 probe ssh" --query GroupId --output text)
        echo "$sg" > "$STATE/sg"
        "${AWS[@]}" ec2 authorize-security-group-ingress --group-id "$sg" \
            --protocol tcp --port 22 --cidr "$myip/32" >/dev/null
    fi

    # the dead-man switch runs before anything else the instance does
    local ud="$STATE/user-data.sh"
    printf '#!/bin/sh\nshutdown -h +%s\n' "$DEADMAN_MIN" > "$ud"

    local iid="" subnet
    for subnet in $("${AWS[@]}" ec2 describe-subnets --filters \
            Name=default-for-az,Values=true \
            "Name=availability-zone,Values=${AZS:-*}" \
            --query 'Subnets[].SubnetId' --output text); do
    iid=$("${AWS[@]}" ec2 run-instances --image-id "$ami" \
        --instance-type "$TYPE" --key-name "$tag" \
        --security-group-ids "$sg" --subnet-id "$subnet" \
        --instance-initiated-shutdown-behavior terminate \
        --instance-market-options "{\"MarketType\":\"spot\",\"SpotOptions\":{\"MaxPrice\":\"$maxp\",\"SpotInstanceType\":\"one-time\",\"InstanceInterruptionBehavior\":\"terminate\"}}" \
        --block-device-mappings "[{\"DeviceName\":\"/dev/sda1\",\"Ebs\":{\"VolumeSize\":$DISK_GB,\"VolumeType\":\"gp3\",\"DeleteOnTermination\":true}}]" \
        --user-data "file://$ud" \
        --tag-specifications "ResourceType=instance,Tags=[{Key=Name,Value=$tag}]" \
            "ResourceType=volume,Tags=[{Key=Name,Value=$tag}]" \
        --query 'Instances[0].InstanceId' --output text) && break
    echo "no spot capacity in $subnet" >&2
    done
    [ -n "$iid" ] || { echo "no capacity in any AZ" >&2; exit 1; }
    echo "$iid" > "$STATE/instance"
    echo "launched $iid ($TYPE, spot max \$$maxp/h, ami $ami)"
    "${AWS[@]}" ec2 wait instance-running --instance-ids "$iid"
    "${AWS[@]}" ec2 describe-instances --instance-ids "$iid" --query \
        'Reservations[0].Instances[0].[LaunchTime,Placement.AvailabilityZone,PublicIpAddress,SpotInstanceRequestId]' \
        --output text | tee "$STATE/launch"
}

ip() {
    "${AWS[@]}" ec2 describe-instances --instance-ids "$(cat "$STATE/instance")" \
        --query 'Reservations[0].Instances[0].PublicIpAddress' --output text
}

ssh_() {
    ssh -i "$STATE/key.pem" -o StrictHostKeyChecking=accept-new \
        -o UserKnownHostsFile="$STATE/known_hosts" -o ServerAliveInterval=30 \
        "ubuntu@$(ip)" "$@"
}

down() {
    if [ -f "$STATE/instance" ]; then
        local iid; iid=$(cat "$STATE/instance")
        "${AWS[@]}" ec2 terminate-instances --instance-ids "$iid" >/dev/null || true
        "${AWS[@]}" ec2 wait instance-terminated --instance-ids "$iid"
        "${AWS[@]}" ec2 describe-instances --instance-ids "$iid" --query \
            'Reservations[0].Instances[0].[InstanceId,State.Name,StateTransitionReason]' \
            --output text
    fi
    if [ -f "$STATE/sg" ]; then
        "${AWS[@]}" ec2 delete-security-group --group-id "$(cat "$STATE/sg")" && rm "$STATE/sg"
    fi
    if [ -f "$STATE/keyname" ]; then
        "${AWS[@]}" ec2 delete-key-pair --key-name "$(cat "$STATE/keyname")" && rm "$STATE/keyname"
        rm -f "$STATE/key.pem"
    fi
    echo "left behind (should be empty):"
    "${AWS[@]}" ec2 describe-instances --filters "Name=tag:Name,Values=$NAME-*" \
        "Name=instance-state-name,Values=pending,running,stopping,stopped,shutting-down" \
        --query 'Reservations[].Instances[].[InstanceId,State.Name]' --output text
    "${AWS[@]}" ec2 describe-volumes --filters "Name=tag:Name,Values=$NAME-*" \
        --query 'Volumes[].[VolumeId,State]' --output text
    "${AWS[@]}" ec2 describe-security-groups --filters "Name=group-name,Values=$NAME-*" \
        --query 'SecurityGroups[].GroupId' --output text
    "${AWS[@]}" ec2 describe-key-pairs --filters "Name=key-name,Values=$NAME-*" \
        --query 'KeyPairs[].KeyName' --output text
    "${AWS[@]}" ec2 describe-spot-instance-requests --filters \
        "Name=state,Values=open,active" --query \
        'SpotInstanceRequests[].[SpotInstanceRequestId,State,InstanceId]' --output text
}

case "${1:-}" in
    up) up ;;
    ip) ip ;;
    ssh) shift; ssh_ "$@" ;;
    down) down ;;
    *) echo "usage: ec2.sh up|ip|ssh|down" >&2; exit 2 ;;
esac
