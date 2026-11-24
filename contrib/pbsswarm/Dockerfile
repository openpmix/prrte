# The pbsswarm image: contrib/slurmswarm's image with OpenPBS in place of
# SLURM.  Toolchain, OpenPBS built from source, a baked PMIx, a non-root
# job user, SSH wiring.  It does NOT contain PRRTE -- build.sh
# compiles your live tree into the shared volume every node mounts.  See
# AGENTS.md.
#
#     docker build -t prte-pbs-swarm:latest .
#     docker build --build-arg OPENPBS_REF=<commit> -t prte-pbs-swarm:latest .

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# OpenPBS's build and run-time prerequisites for Ubuntu 24.04, as its INSTALL
# lists them, plus what the harness needs.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential gcc g++ make \
        autoconf automake libtool m4 flex bison pkg-config \
        perl python3 python3-dev git curl ca-certificates \
        libevent-dev libhwloc-dev zlib1g-dev libzstd-dev \
        libx11-dev libxt-dev libedit-dev libical-dev libncurses-dev \
        postgresql postgresql-contrib postgresql-server-dev-all \
        tcl-dev tk-dev swig libexpat1-dev libssl-dev libxext-dev libxft-dev \
        libcjson-dev expat libedit2 tcl tk sudo \
        openssh-server openssh-client \
        iproute2 iputils-ping procps less vim \
    && rm -rf /var/lib/apt/lists/*

# ---- OpenPBS, from source ----
# Pinned to a master commit rather than the last release (v23.06.06, 2023),
# whose sources predate Ubuntu 24.04's toolchain; master's INSTALL names
# 24.04 as a target.  OpenPBS's TM API lives in libpbs, and OpenPBS's libpbs
# links no libevent -- so this image cannot reproduce the libevent conflict
# Altair's commercial PBS Pro build has.  See AGENTS.md section 7.
ARG OPENPBS_REPO=https://github.com/openpbs/openpbs.git
ARG OPENPBS_REF=cd7ab5edaf03dcac2f1cb9e2f42d75eb117d468a
RUN git clone "$OPENPBS_REPO" /src/openpbs \
    && cd /src/openpbs \
    && git checkout -q "$OPENPBS_REF" \
    && ./autogen.sh \
    && ./configure --prefix=/opt/pbs \
    && make -j"$(nproc)" \
    && make install \
    && chmod 4755 /opt/pbs/sbin/pbs_iff /opt/pbs/sbin/pbs_rcp \
    && test -f /opt/pbs/include/tm.h \
    && test -x /opt/pbs/bin/pbs_tmrsh \
    && rm -rf /src/openpbs

# ---- baked PMIx (default) ----
# Override with --build-arg, or bind-mount your own via PMIX_SRC at run time
# (see build.sh).
ARG PMIX_REPO=https://github.com/openpmix/openpmix.git
ARG PMIX_REF=master
RUN git clone --recursive --depth=1 -b "$PMIX_REF" "$PMIX_REPO" /src/pmix \
    && cd /src/pmix \
    && ./autogen.pl \
    && ./configure --prefix=/usr/local \
    && make -j"$(nproc)" \
    && make install \
    && ldconfig \
    && rm -rf /src/pmix

# ---- the job user ----
# PBS runs no job as root by default, and PRRTE should be tested as the
# ordinary user it runs as on a cluster.  Its home is a volume shared by
# every node (docker-compose.yml), as a cluster's home file system is: that
# is where PBS returns a job's output files, and where pbs-alloc leaves the
# environment of a held allocation.  The entrypoint gives it its ssh key.
# (the base image's "ubuntu" user holds uid 1000; it goes)
RUN { userdel -r ubuntu 2>/dev/null || true; } \
    && useradd -m -u 1000 -s /bin/bash prte

# ---- the allocation helper ----
COPY pbs-alloc /usr/local/bin/pbs-alloc
RUN chmod 0755 /usr/local/bin/pbs-alloc

# ---- passwordless SSH between the container "nodes" (root) ----
RUN mkdir -p /var/run/sshd /root/.ssh \
    && ssh-keygen -q -t ed25519 -N "" -f /root/.ssh/id_ed25519 \
    && cp /root/.ssh/id_ed25519.pub /root/.ssh/authorized_keys \
    && printf 'Host *\n    StrictHostKeyChecking no\n    UserKnownHostsFile /dev/null\n    LogLevel ERROR\n' \
         > /root/.ssh/config \
    && chmod 600 /root/.ssh/* \
    && sed -i 's/#\?PermitRootLogin.*/PermitRootLogin yes/' /etc/ssh/sshd_config

# make the run-time install discoverable for any login shell (build.sh writes
# /opt/prte/env.sh with PATH/LD_LIBRARY_PATH once it knows the PMIx layout),
# and PBS's own commands
RUN echo '[ -f /opt/prte/env.sh ] && . /opt/prte/env.sh' > /etc/profile.d/prte.sh

# ---- node entrypoint ----
COPY node-entrypoint.sh /usr/local/bin/node-entrypoint.sh
RUN chmod 0755 /usr/local/bin/node-entrypoint.sh

CMD ["/usr/local/bin/node-entrypoint.sh"]
