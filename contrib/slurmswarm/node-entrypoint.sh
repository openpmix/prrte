#!/bin/bash
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Entrypoint for a node of the PRRTE real-SLURM swarm.
#
# Brings up, in order: munge (the credential service everything else
# authenticates through), slurmctld (on the controller node only), slurmd
# (everywhere), and finally sshd in the foreground as PID 1's child so the
# container stays up.
#
# It also does what contrib/dockerswarm's entrypoint does: register the
# shared-volume library directories with ld.so and symlink the install's
# binaries onto the default PATH.  That second one is not cosmetic here --
# SLURM launches `prted` through srun, in an environment that has none of the
# login shell's PATH, so an unlinked prted is a launch that fails with
# "prted: command not found" and no other diagnostic.

set -u

log() { printf '[entrypoint] %s\n' "$*"; }

# ---------------------------------------------------------------------------
# the PRRTE install in the shared volume
# ---------------------------------------------------------------------------
for d in /opt/prte/prte/lib /opt/prte/pmix/lib; do
    [ -d "$d" ] && echo "$d" >> /etc/ld.so.conf.d/prte.conf
done
ldconfig
for b in /opt/prte/prte/bin/*; do
    [ -e "$b" ] && ln -sf "$b" /usr/local/bin/
done 2>/dev/null

# ---------------------------------------------------------------------------
# munge
# ---------------------------------------------------------------------------
# The key is baked into the image, so every node already agrees.  What does
# NOT survive the image is the runtime state: /run is a fresh tmpfs in every
# container, and munged refuses to start if it cannot create its socket
# directory with the right ownership.
mkdir -p /run/munge /var/log/munge /var/lib/munge
chown -R munge:munge /run/munge /var/log/munge /var/lib/munge
chmod 0755 /run/munge
rm -f /run/munge/munge.socket.2
log "starting munged"
runuser -u munge -- /usr/sbin/munged --force

# Do not proceed to the SLURM daemons until munge actually answers: a slurmd
# that starts against a dead munged does not retry, it exits, and the node
# then shows up as DOWN for the life of the swarm with nothing in the SLURM
# logs pointing at the credential service.
for _ in $(seq 20); do
    munge -n >/dev/null 2>&1 && break
    sleep 0.5
done
if ! munge -n >/dev/null 2>&1; then
    log "ERROR: munged is not answering; SLURM cannot authenticate"
fi

# ---------------------------------------------------------------------------
# SLURM
# ---------------------------------------------------------------------------
mkdir -p /var/spool/slurmctld /var/spool/slurmd /var/log/slurm
chown slurm:slurm /var/spool/slurmctld /var/log/slurm

# Which node is the controller is read out of slurm.conf rather than
# hardcoded here, so changing SlurmctldHost is a one-file edit.
ctld_host="$(sed -n 's/^SlurmctldHost=\([^ ]*\).*/\1/p' /etc/slurm/slurm.conf | head -1)"
if [ "$(hostname -s)" = "$ctld_host" ]; then
    log "starting slurmctld (this node is the controller)"
    slurmctld
fi

# Every node runs a slurmd, controller included: the head node is part of the
# cluster here, exactly as it is in the dockerswarm harness, so a DVM can be
# allocated node1 and PRRTE's "allocation excludes the head node" case has
# something to exclude.
log "starting slurmd"
slurmd

# ---------------------------------------------------------------------------
# sshd
# ---------------------------------------------------------------------------
# Kept even though SLURM is the launcher: plm/ssh is what runs when there is
# no allocation, and the harness deliberately covers both (a DVM inside an
# allocation goes out over srun; the same swarm with no SLURM_JOBID set goes
# out over ssh).  It is also how a case reaches a node to look at it.
exec /usr/sbin/sshd -D -e
