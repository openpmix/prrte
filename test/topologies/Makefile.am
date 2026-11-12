# $COPYRIGHT$
# Additional copyrights may follow
# $HEADER$

# Shared synthetic hwloc topologies used to exercise map/rank/bind
# patterns without a live DVM.  These XML files are consumed both by the
# rmaps C unit tests (test/unit/rmaps) and by the offline mapping test
# harness (test/offline), which auto-discovers every *.xml placed here.
# See the "Testing the mapper without launching" section in AGENTS.md.

# turin-4gpu.xml and turin-4gpu-nvml.xml are the same machine (Open MPI
# issue #14169) seen by two different hwloc builds, which is the whole
# point of keeping both. turin-4gpu came from the distro hwloc: its GPUs
# appear as DRM card and render nodes carrying no vendor identity, so
# --map-by device=gpu is refused against it. turin-4gpu-nvml came from an
# hwloc built with the CUDA and NVML backends: each GPU function there
# exposes cuda0, opencl0d0 and nvml0, with NVIDIAUUID on nvml0, and the
# same directive is accepted. Neither is synthetic, and the pair is the
# only thing in the tree that exercises both arms of that decision.

# slingshot.xml is an HPE Cray EX235n node read by hwloc 2.10: four A100s and
# four Slingshot (Cassini) NICs, one of each under every PCI host bridge,
# plus a 1 GbE management port under one of them. That hwloc had no vendor
# GPU backend and the node no render nodes, so the A100s carry no OS
# devices at all, and it predates the cxiN devices hwloc 2.14 records
# beside each hsnN. The host name, MAC addresses and cgroup path were
# replaced before it was checked in.

# mi350-bcm57608.xml is a real node read by hwloc 2.15 with the RSMI
# backend: eight AMD MI350s (PCI class 0x1200, a processing accelerator,
# each named rsmiN with its AMDUUID) and eight dual-port Broadcom BCM57608
# RoCE adapters, one GPU and one adapter under every PCI host bridge. The
# adapters' RDMA devices carry rdma-core's PCI-path names (rocep6s0f0)
# rather than bnxt_reN. Its host name, cgroup path, board and BIOS
# strings, MAC addresses and the GUIDs and GIDs derived from them, GPU and
# disk serials, GPU uuids and XGMI hive id were replaced with synthetic
# values before it was checked in.

EXTRA_DIST = test-topo.xml test-topo2.xml turin-4gpu.xml turin-4gpu-nvml.xml \
             slingshot.xml mi350-bcm57608.xml
