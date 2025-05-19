PRRTE v4.x series
=================

This file contains all the NEWS updates for the PRRTE v4.x
series, in reverse chronological order.

4.0.0 -- TBD
------------
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

               A few notes:

               (1) Starting with this release, PRRTE requires
               Python >= v3.7 to build a Git clone (ie., not a tarball).
               Certain elements of the code base are constructed at build
               time, with the construction performed by Python script. The
               constructed elements are included in release tarballs.

               (2) PRRTE >= v4.0 is not compatible with PMIx < v6.0 due
               to internal changes (e.g., show-help messages are now
               contained in memory instead of on-disk files). Configure
               will therefore error out if the detected PMIx version
               does not meet this criterion.

A full list of individual changes will not be provided here,
but will commence with the v4.0.1 release.
