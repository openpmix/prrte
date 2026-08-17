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

EXTRA_DIST = test-topo.xml test-topo2.xml turin-4gpu.xml turin-4gpu-nvml.xml
