#!/bin/bash
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Start one node of the PBS test cluster: name resolution, the job user's
# ssh key, and the PBS services - the server, scheduler, comm and a
# mom on node1, a mom alone everywhere else.  See AGENTS.md section 8 for
# why each container-specific step is here.

set -u
log() { printf '[entrypoint] %s\n' "$*"; }

# PRRTE's install in the shared volume, for every process on this node
for d in /opt/prte/prte/lib /opt/prte/pmix/lib; do
    [ -d "$d" ] && echo "$d" >> /etc/ld.so.conf.d/prte.conf
done
ldconfig
for b in /opt/prte/prte/bin/*; do
    [ -e "$b" ] && ln -sf "$b" /usr/local/bin/
done 2>/dev/null

# ---- name resolution ----
# PBS identifies a node by the name its address resolves BACK to.  Docker's
# embedded DNS answers a reverse lookup with the container name and the
# network ("prtepbs-node2.prtepbsswarm_dvm"), not the hostname, so without
# this the job's $PBS_NODEFILE names nodes nobody can reach by those names
# and pbs_tmrsh refuses every one of them as "not a node in job".  So every
# node's address goes in /etc/hosts under its hostname, which the resolver
# consults first.  The other containers may still be starting: wait for
# each to appear in DNS.
nodes="${PBS_SWARM_NODES:-node1 node2 node3 node4 node5 node6 node7 node8 node9 node10}"
me="$(hostname)"
for h in $nodes; do
    [ "$h" = "$me" ] && continue
    ip=""
    for _ in $(seq 120); do
        ip="$(getent ahostsv4 "$h" | awk 'NR==1{print $1}')"
        [ -n "$ip" ] && break
        sleep 0.5
    done
    if [ -z "$ip" ]; then
        log "ERROR: $h never appeared in DNS; PBS will not know it by name"
        continue
    fi
    grep -q "[[:space:]]$h\$" /etc/hosts || echo "$ip $h" >> /etc/hosts
done

# ---- the job user's home, shared by every node ----
# The volume starts empty; whichever node gets there first gives it the
# skeleton and an ssh key, which is all the others need.
if [ ! -f /home/prte/.ssh/id_ed25519 ]; then
    mkdir -p /home/prte/.ssh
    cp -rn /etc/skel/. /home/prte/ 2>/dev/null
    ssh-keygen -q -t ed25519 -N "" -f /home/prte/.ssh/id_ed25519 2>/dev/null
    cp /home/prte/.ssh/id_ed25519.pub /home/prte/.ssh/authorized_keys
    printf 'Host *\n    StrictHostKeyChecking no\n    UserKnownHostsFile /dev/null\n    LogLevel ERROR\n' \
        > /home/prte/.ssh/config
    chown -R prte:prte /home/prte
    chmod 700 /home/prte/.ssh
    chmod 600 /home/prte/.ssh/*
fi

# ---- PBS ----
role=execution
[ "$me" = node1 ] && role=server
{
    echo "PBS_SERVER=node1"
    echo "PBS_EXEC=/opt/pbs"
    echo "PBS_HOME=/var/spool/pbs"
    echo "PBS_CORE_LIMIT=unlimited"
    echo "PBS_SCP=/usr/bin/scp"
    if [ "$role" = server ]; then
        printf 'PBS_START_SERVER=1\nPBS_START_SCHED=1\nPBS_START_COMM=1\nPBS_START_MOM=1\n'
    else
        printf 'PBS_START_SERVER=0\nPBS_START_SCHED=0\nPBS_START_COMM=0\nPBS_START_MOM=1\n'
        echo "PBS_LEAF_ROUTERS=node1"
    fi
} > /etc/pbs.conf
/opt/pbs/libexec/pbs_postinstall "$role" > /var/log/pbs-postinstall.log 2>&1
# The mom samples a job's resource use only every so often, and a sister
# reports its share on the same cycle, so with the defaults a job of a few
# seconds is charged for a fraction of what it used - and the accounting
# cases here run jobs of a few seconds.  $usecp: the home is shared, so a
# job's output files are copied, not scp'd, back to it.
grep -q '^\$min_check_poll' /var/spool/pbs/mom_priv/config 2>/dev/null ||
    cat >> /var/spool/pbs/mom_priv/config <<'EOC'
$min_check_poll 1
$max_check_poll 2
$usecp *:/home/ /home/
EOC
if ! /opt/pbs/libexec/pbs_init.d start > /var/log/pbs-start.log 2>&1; then
    log "ERROR: PBS did not start - see /var/log/pbs-start.log"
fi

if [ "$role" = server ]; then
    # PBS's profile script reads MANPATH unguarded
    set +u
    . /etc/profile.d/pbs.sh
    set -u
    for _ in $(seq 120); do
        qstat -B >/dev/null 2>&1 && break
        sleep 0.5
    done
    for h in $nodes; do
        qmgr -c "create node $h" >/dev/null 2>&1
    done
    # one uid space across the cluster; job history, so qstat -x answers
    # for a finished job - which is where its resources_used are read
    qmgr -c "set server flatuid = true"
    qmgr -c "set server job_history_enable = true"
fi

touch /var/run/pbsswarm.ready
log "node $me up ($role)"
exec /usr/sbin/sshd -D -e
