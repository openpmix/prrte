# Base image for the PRRTE DVM test "swarm" (see README.md).
#
# Unlike the old harness, this image does NOT contain PRRTE.  PRRTE (and,
# optionally, PMIx) is built at run time from your *live* working tree, which
# build.sh bind-mounts into a builder container and compiles out-of-tree
# (VPATH) into a shared volume mounted at /opt/prte.  This image only provides:
#
#   * the build toolchain (so the builder container can compile PRRTE/PMIx),
#   * a baked PMIx in /usr/local as the default (overridden when you bind-mount
#     an openpmix checkout via PMIX_SRC),
#   * passwordless SSH between the "nodes", and
#   * an entrypoint that makes the shared-volume libraries loadable.
#
# Build it with ./build.sh (which also drives the PRRTE build); a bare
# `docker build` just produces this base.

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential gcc g++ make \
        autoconf automake libtool m4 flex \
        perl python3 git \
        libevent-dev libhwloc-dev zlib1g-dev \
        openssh-server openssh-client \
        iproute2 iputils-ping procps less vim ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# ---- baked PMIx (default) ----
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
# The PRRTE/PMIx install lives in the shared /opt/prte volume, which is not on
# the image's default PATH or ld.so search path.  Before starting sshd:
#   * register the install's lib dirs with ldconfig so libprrte/libpmix load,
#     and
#   * symlink the install's binaries onto the default PATH (/usr/local/bin) so
#     that `prted` resolves in the non-login shell that plm/ssh uses to launch
#     daemons on the other nodes -- without this, a multi-node launch fails with
#     "prted: command not found".
RUN printf '#!/bin/sh\n\
for d in /opt/prte/prte/lib /opt/prte/pmix/lib; do\n\
    [ -d "$d" ] && echo "$d" >> /etc/ld.so.conf.d/prte.conf\n\
done\n\
ldconfig\n\
for b in /opt/prte/prte/bin/*; do\n\
    [ -e "$b" ] && ln -sf "$b" /usr/local/bin/\n\
done 2>/dev/null\n\
exec /usr/sbin/sshd -D -e\n' > /usr/local/bin/node-entrypoint.sh \
    && chmod +x /usr/local/bin/node-entrypoint.sh

EXPOSE 22
CMD ["/usr/local/bin/node-entrypoint.sh"]
