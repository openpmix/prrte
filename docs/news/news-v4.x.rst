PRRTE v4.x series
=================

This file contains all the NEWS updates for the PRRTE v4.x
series, in reverse chronological order.

4.1.0 -- TBD
------------
.. important:: This is the second release in the v4 family. This series
               is based on a forking of the PRRTE master branch. The
               v4.1.0 release contains quite a few bug fixes, plus
               the following significant changes:

               * minimum PMIx required for build and execution is now v6.1.0
               * minimum Python required to build from Git clone is v3.6
               * MPMD jobs can now specify mapping directives for each app
               * Group construct now returns all required info as per Standard
               * MCA parameter support has been extended to include the display
                 and runtime-options and cpus-per-rank cmd line directives
               * fixed debugger daemon rendezvous on remote nodes and debugger
                 connection to launcher spawned by the debugger itself

Commits since branch was forked:
 - PR #2397: Update NEWS for rc1
 - PR #2396: Remove the "compat" macro definitions
 - PR #2394: Purge checks for earlier PMIx versions
 - PR #2392: Require cherry-picks on this release branch
 - PR #2391: Update NEWS and VERSION


Commits from master branch since v4.0.0 include:
 - PR #2389: Initialize variable
 - PR #2388: Correct cflags used for check_compiler_version.m4
 - PR #2387: Initialize variable before use
 - PR #2386: Cleanup prte_info support
 - PR #2383: Pass group info in PMIx server callback
 - PR #2382: Support backward compatibility with PMIx v6.0.0
 - PR #2381: Stop PMIx progress thread during abnormal shutdown
 - PR #2380: Treat a NULL name as indicating "stop all threads"
 - PR #2379: Use session_dir_finalize to preserve output files
 - PR #2378: Stop PMIx progress thread at beginning of finalize
 - PR #2376: Update the directive list in check_multi
 - PR #2374: Provide an MCA param to control hwloc shmem sharing
 - PR #2372: update-my-copyright.py: properly support git workspaces
 - PR #2371: Update handling of log requests
 - PR #2370: Fix typo and add MCA param for default output options
 - PR #2369: Reduce min PMIx version t v6.0.1
 - PR #2368: Silence some Coverity warnings
 - PR #2367: Add MCA param to set personality
 - PR #2365: Initialize ESS before prterun fallback to prun_common()
 - PR #2363: Create a custom signature if asymmetric topology found
 - PR #2361: Cleanup bind output when partial allocations exist
 - PR #2360: Revert back to explicitly setting hwloc support for xml import
 - PR #2357: Update prun to new cmd line option spellings
 - PR #2356: Enable MCA param support for display and runtime-options
 - PR #2353: Don't force map display for donotlaunch if bind display requested
 - PR #2351: Fix hetero node launch
 - PR #2350: Potential use after free (alerts 10-12)
 - PR #2349: Workflow does not contain permissions
 - PR #2348: Correct computation of nprocs for ppr
 - PR #2347: Support multi-app map-by specifications
 - PR #2346: Fix debugger daemon rendezvous on remote nodes
 - PR #2344: Multiple commits
    - Fix ppr mapper
    - Extend existing build check CI
 - PR #2340: Slight mod to the indirect debugger example
 - PR #2338: Correct the nprocs check to support moving to next node
 - PR #2337: Perform sanity check on cmd line
 - PR #2335: Allow setting default cpus/rank
 - PR #2333: Retrieve PMIX_PARENT_ID with wildcard rank
 - PR #2331: Remove stale references to LIKWID mapper
 - PR #2329: Centralize quickmatch to ensure consistency
 - PR #2327: Add the rmaps/lsf component
 - PR #2326: Update ras to allow rank/seq files to define allocation
 - PR #2325: Remove LSF references in rank_file mapper
 - PR #2324: Some cleanup on hostfile parsing
 - PR #2323: Do not override user-specified num procs
 - PR #2322: Correctly set the number of procs for rankfile apps
 - PR #2319: Fix prte_info to output correct version
 - PR #2317: Fix printing of binding ranges
 - PR #2316: Correct ordering of macro variables
 - PR #2315: Provide skeleton for passing allocation requests through RAS
 - PR #2313: Implement support for resource usage monitoring
 - PR #2311: PLM: mark uncached daemons as reported
 - PR #2308: Fix prun tool
 - PR #2307: Cleanup and improve autohandling of hetero nodes
 - PR #2306: Tweak the forwarding of signals
 - PR #2305: Improve hetero node detection a bit
 - PR #2304: Correct show-help message
 - PR #2303: Allow PMIx group construct caller to specify the order of the final membership
 - PR #2301: Fix map-by pe-list when using core CPUs
 - PR #2300: Add launching-apps section to docs
 - PR #2299: Extend timeout to child jobs
 - PR #2298: Replace sprintf with snprintf
 - PR #2297: Fix relative node processing
 - PR #2296: Let seq and rankfile mappers compute their own num-procs
 - PR #2295: Error out when asymmetric topologies cannot support ppr requests
 - PR #2294: Fix printout of binding cpus
 - PR #2293: Do not assign DVM's bookmark to the application job
 - PR #2291: Add some minor verbose debug output
 - PR #2290: Check only for existence of PMIx capability flag
 - PR #2289: Fix the definition checks for tool_connected2 and log2 integration
 - PR #2288: Update with new log2 and tool_connected2 upcalls
 - PR #2287: Clarify help messages
 - PR #2281: Bugfix: inconsistently setting PMIX_JOB_RECOVERABLE
 - PR #2280: Fix display of physical vs logical CPUs
 - PR #2279: Fix precedence ordering on envar operations
 - PR #2278: Fix the colocation algorithm
 - PR #2277: Silence Coverity warnings
 - PR #2276: Silence more Coverity warnings
 - PR #2275: Silence even more Coverity warnings
 - PR #2274: Silence more Coverity warnings
 - PR #2273: Extend testbuild launchers support
 - PR #2272: Silence yet more Coverity warnings
 - PR #2271: Silence more Coverity warnings
 - PR #2270: Silence more Coverity warnings
 - PR #2268: Extend inheritance to app level
 - PR #2266: Inherit env directives if requested
 - PR #2265: Move prte function into librar
 - PR #2264: Silence more Coverity warnings
 - PR #2262: Silence Coverity warnings
 - PR #2261: Ensure we have HNP node aliases
 - PR #2260: Extend control over client connections
 - PR #2257: Resolve Coverity issues
 - PR #2252: Extend support for specifying tool connection parameters
 - PR #2251: Cleanup queries and completely register tools
 - PR #2250: Correct handling of tool-based spawn requests
 - PR #2249: Tool registration updates
 - PR #2248: Include node object when registering tool
 - PR #2246: Properly implement the "abort" operation
 - PR #2243: Adjust top session dir name
 - PR #2241: Add some finer-grained connection support
 - PR #2240: Customize the OMPI "allow-run-as-root" doc snippet
 - PR #2239: Runtime check both min/max PMIx versions
 - PR #2238: Runtime check that PMIx meets min requirement
 - PR #2237: Check for PMIx version too high
 - PR #2234: Add support for client_connected2 server module upcall
 - PR #2233: Declare the process set during registration
 - PR #2231: Provide error message when ssh fails
 - PR #2230: Add some missing command strings for debug output
 - PR #2228: Minor cleanups in tool connection
 - PR #2227: Process deprecated "stop" CLI
 - PR #2226: Update CI
 - PR #2225: Ensure to progress job launch for singleton
 - PR #2224: Properly handle sigterm when started by singleton
 - PR #2221: iof/hnp: correctly handle short write to stdin
 - PR #2217: Update OAC submodule
 - PR #2216: Update child job's fwd environment flag
 - PR #2213: Remove debug output
 - PR #2211: Check for pthread_np.h header
 - PR #2209: src/docs/prrte-rst-content: Add missing file to Makefile.am
 - PR #2208: Preserve formatting in show-help output
 - PR #2207: Support fwd-environment directives for spawned child jobs
 - PR #2206: Preserve source ID across API call
 - PR #2205: Reduce min Python version to 3.6



4.0.0 -- 19 May 2025
--------------------
.. important:: This is the first release in the v4 family. The intent
               for this series is to provide regular "reference tags",
               effectively serving as milestones for any development
               that might occur after the project achieved a stable
               landing zone at the conclusion of the v3 series. It
               is expected, therefore, that releases shall be infrequent
               and rare occurrences, primarily driven by the completion
               of some significant feature or some particularly
               critical bug fix.

               For this initial release, that feature is completion of
               support for the Group family of PMIx APIs. This includes
               support for all three of the group construction modes,
               including the new "bootstrap" method.

               Note that while PRRTE v4 will build and execute against
               PMIx v5, proper execution of the various group construction
               modes requires that your application use PMIx v6 or above.

A full list of individual changes will not be provided here,
but will commence with the v4.0.1 release.
