# Base image for the PRRTE real-SLURM test swarm (see AGENTS.md).
#
# This is contrib/dockerswarm's image plus an actual SLURM installation.  Like
# that one it does NOT contain PRRTE: PRRTE (and, optionally, PMIx) is built at
# run time from your *live* working tree, which build.sh bind-mounts into a
# builder container and compiles out-of-tree (VPATH) into a shared volume
# mounted at /opt/prte.  This image provides:
#
#   * the build toolchain (so the builder container can compile PRRTE/PMIx),
#   * a baked PMIx in /usr/local as the default (overridden when you bind-mount
#     an openpmix checkout via PMIX_SRC),
#   * SLURM (slurmctld/slurmd/client) and munge, with a shared munge key and a
#     slurm.conf naming all ten nodes,
#   * passwordless SSH between the "nodes" -- plm/ssh is what launches when
#     there is no allocation, and this harness covers both launchers, and
#   * an entrypoint that starts munge and the SLURM daemons and makes the
#     shared-volume libraries loadable.
#
# Build it with ./build.sh (which also drives the PRRTE build); a bare
# `docker build` just produces this base.
#
# WHY SLURM IS BUILT FROM SOURCE HERE, AND NOT INSTALLED FROM THE DISTRIBUTION.
# It is not about JSON *support*: Ubuntu 24.04's slurm-wlm does ship
# serializer_json.so, and `scontrol show job --json` works.  It is about the
# JSON *schema*.  Ubuntu 24.04 carries SLURM 23.11, whose newest data parser
# is v0.0.40, and in that schema a job's resources look like
#
#     "job_resources": { "nodes": "node[1-3]", "allocated_nodes": [ ... ] }
#
# PRRTE's parser (ras_slurm_jansson.c) reads the shape SLURM adopted in
# data_parser v0.0.41, which ships in SLURM 24.05 and is the default output
# from 24.05 on:
#
#     "job_resources": { "nodes": { "count": 3, "list": "node[1-3]",
#                                   "allocation": [ ... ] } }
#
# Against 23.11 every extend fails at once with "Failed to parse input JSON in
# ras_slurm_jansson.c" and no coverage is obtained at all -- which is exactly
# what this harness was built to find out, and which it did find out.  So the
# version is pinned here, and it is a build-arg so that the suite can be run
# against another one:
#
#     docker build --build-arg SLURM_VERSION=25.05.3 -t prte-slurm-swarm:latest .
#
# 24.05 is the floor.  run-tests.sh checks the schema at preflight rather than
# the version number, because what matters is what the scheduler emits.

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential gcc g++ make \
        autoconf automake libtool m4 flex \
        perl python3 git curl bzip2 \
        libevent-dev libhwloc-dev zlib1g-dev \
        libjansson-dev \
        munge libmunge-dev libjson-c-dev libyaml-dev \
        openssh-server openssh-client \
        iproute2 iputils-ping procps less vim ca-certificates \
        valgrind \
    && rm -rf /var/lib/apt/lists/*

# ---- SLURM, from source ----
# --with-json is the argument that matters: without libjson-c the serializer
# plugin is not built, `scontrol show job --json` fails outright, and
# ras/slurm's entire modify surface has nothing to parse.  configure does not
# fail when it is missing, it just quietly builds less, so the plugin is
# checked for after install rather than assumed.
ARG SLURM_VERSION=24.11.6
ARG SLURM_URL=https://download.schedmd.com/slurm
RUN groupadd -r slurm && useradd -r -g slurm -d /var/lib/slurm -s /usr/sbin/nologin slurm \
    && curl -fsSL "$SLURM_URL/slurm-$SLURM_VERSION.tar.bz2" -o /tmp/slurm.tar.bz2 \
    && mkdir -p /src/slurm && tar xjf /tmp/slurm.tar.bz2 -C /src/slurm --strip-components=1 \
    && cd /src/slurm \
    && ./configure --prefix=/usr/local --sysconfdir=/etc/slurm \
                   --with-munge --with-json --without-hwloc \
    && make -j"$(nproc)" \
    && make install \
    && ldconfig \
    && test -f /usr/local/lib/slurm/serializer_json.so \
    && rm -rf /src/slurm /tmp/slurm.tar.bz2

# ---- baked PMIx (default) ----
# NOTE the order: SLURM is configured and built ABOVE this, with no PMIx on
# the system, so it builds no mpi/pmix plugin.  That is deliberate.  Nothing
# here wants one -- PRRTE is launched by srun as a plain task
# (MpiDefault=none) and brings its own PMIx -- and a SLURM plugin linked
# against this image's PMIx would tie the scheduler's ABI to the library the
# harness exists to vary.  Moving this block above the SLURM build would
# silently create that coupling.
# Cloned with submodules so autogen.pl runs normally.  PMIx master is the
# default because the DVM size-change event codes (PMIX_DVM_IS_READY /
# PMIX_ERR_DVM_MOD) the elastic client registers for may not be in a release
# yet; override with --build-arg, or bind-mount your own via PMIX_SRC at run
# time (see build.sh), in which case this baked copy is ignored.
# Do NOT pass --with-libevent=/usr: it forces /usr/lib and misses the multiarch
# dir; let configure find the system libevent-dev/hwloc-dev.
ARG PMIX_REPO=https://github.com/openpmix/openpmix.git
ARG PMIX_REF=master
RUN git clone --recursive --depth=1 -b "$PMIX_REF" "$PMIX_REPO" /src/pmix \
    && cd /src/pmix \
    && ./autogen.pl \
    && ./configure --prefix=/usr/local \
    && make -j"$(nproc)" \
    && make install \
    && ldconfig

# ---- munge: one key for the whole cluster ----
# Every node runs the same image, so baking the key here is what makes the ten
# containers members of one authentication domain.  It is generated at build
# time rather than checked in, so two people's swarms do not share a
# credential -- but it is still a key sitting in a local image, which is fine
# for a private bridge network and is not fine anywhere else.
RUN install -d -o munge -g munge -m 0700 /etc/munge /var/lib/munge /var/log/munge \
    && dd if=/dev/urandom of=/etc/munge/munge.key bs=1024 count=1 2>/dev/null \
    && chown munge:munge /etc/munge/munge.key \
    && chmod 0400 /etc/munge/munge.key

# ---- SLURM configuration ----
# One file, identical on every node, which is what SLURM requires and what a
# swarm of containers from one image gets for free.  See slurm.conf for why
# each container-specific setting is there.
COPY slurm.conf /etc/slurm/slurm.conf
COPY cgroup.conf /etc/slurm/cgroup.conf
RUN chmod 0644 /etc/slurm/slurm.conf /etc/slurm/cgroup.conf \
    && install -d -o slurm -g slurm -m 0755 /var/spool/slurmctld /var/log/slurm \
    && install -d -m 0755 /var/spool/slurmd /var/lib/slurm

# ---- the allocation helper ----
# Creates and describes real SLURM allocations, and prints the SLURM_* export
# lines that a `salloc`-with-a-shell would have set.  The harness holds an
# allocation across many `docker exec` invocations, so it cannot simply run
# everything under one salloc.  See AGENTS.md section 11.
COPY slurm-alloc.py /usr/local/bin/slurm-alloc
RUN chmod 0755 /usr/local/bin/slurm-alloc

# ---- passwordless SSH between the container "nodes" ----
RUN mkdir -p /var/run/sshd /root/.ssh \
    && ssh-keygen -t ed25519 -N "" -f /root/.ssh/id_ed25519 \
    && cp /root/.ssh/id_ed25519.pub /root/.ssh/authorized_keys \
    && printf 'Host *\n    StrictHostKeyChecking no\n    UserKnownHostsFile /dev/null\n    LogLevel ERROR\n' \
         > /root/.ssh/config \
    && chmod 600 /root/.ssh/* \
    && sed -i 's/#\?PermitRootLogin.*/PermitRootLogin yes/' /etc/ssh/sshd_config \
    && printf '\nAcceptEnv *\n' >> /etc/ssh/sshd_config

# make the run-time install discoverable for any login shell (build.sh writes
# /opt/prte/env.sh with PATH/LD_LIBRARY_PATH once it knows the PMIx layout)
RUN echo '[ -f /opt/prte/env.sh ] && . /opt/prte/env.sh' > /etc/profile.d/prte.sh

# ---- node entrypoint ----
COPY node-entrypoint.sh /usr/local/bin/node-entrypoint.sh
RUN chmod 0755 /usr/local/bin/node-entrypoint.sh

EXPOSE 22 6817 6818
CMD ["/usr/local/bin/node-entrypoint.sh"]
