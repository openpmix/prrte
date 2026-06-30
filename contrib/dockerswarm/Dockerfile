# Image for the PRRTE elastic-DVM test "swarm" (see README.md).
#
# Builds PMIx (cloned from git) and PRRTE (supplied in the build context by
# build.sh as a clean git-archive of the local committed tree) into /usr/local,
# plus the `elastic` test client and passwordless SSH between the container
# "nodes".  Build this with ./build.sh, not a bare `docker build` -- build.sh
# prepares the PRRTE source the COPY below expects.

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

# ---- build PMIx ----
# Cloned with its submodules so autogen.pl runs normally.  PMIx master is the
# default because the DVM size-change event codes (PMIX_DVM_IS_READY /
# PMIX_ERR_DVM_MOD) the elastic client registers for may not be in a release
# yet; override with --build-arg if you need a specific PMIx.
# Do NOT pass --with-libevent=/usr: it forces /usr/lib and misses the multiarch
# dir (/usr/lib/<triplet>); let configure find the system libevent-dev/hwloc-dev.
ARG PMIX_REPO=https://github.com/openpmix/openpmix.git
ARG PMIX_REF=master
RUN git clone --recursive --depth=1 -b "$PMIX_REF" "$PMIX_REPO" /src/pmix \
    && cd /src/pmix \
    && ./autogen.pl \
    && ./configure --prefix=/usr/local \
    && make -j"$(nproc)" \
    && make install \
    && ldconfig

# ---- build PRRTE (this repo's committed tree, from the build context) ----
# build.sh git-archives the local tree into ./prrte with config/oac populated
# and .gitmodules removed, so autogen.pl skips its git-only submodule check and
# runs from the VERSION file.  The archived tree has no .git, so configure does
# NOT imply --enable-devel-check (warnings-as-errors); --enable-debug still
# gives us debug symbols and assertions.
COPY prrte /src/prrte
RUN cd /src/prrte \
    && ./autogen.pl \
    && ./configure --prefix=/usr/local --with-pmix=/usr/local --enable-debug \
    && make -j"$(nproc)" \
    && make install \
    && ldconfig

# ---- elastic test client ----
COPY elastic.c /src/elastic.c
RUN gcc -O0 -g -o /usr/local/bin/elastic /src/elastic.c \
        -I/usr/local/include -L/usr/local/lib -lpmix \
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

# make the install discoverable for any login shell too
RUN echo 'export PATH=/usr/local/bin:$PATH' > /etc/profile.d/prte.sh

EXPOSE 22
CMD ["/usr/sbin/sshd", "-D", "-e"]
