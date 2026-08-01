.. _label-version-numbers:

Software Version Numbers
========================

PRRTE's version numbers are the union of several different values:
major, minor, release, and an optional quantifier.

* Major: The major number is the first integer in the version string
  (e.g., v1.2.3). Changes in the major number typically indicate a
  significant change in the code base and/or end-user
  functionality. The major number is always included in the version
  number.

* Minor: The minor number is the second integer in the version
  string (e.g., v1.2.3). Changes in the minor number typically
  indicate an incremental change in the code base and/or end-user
  functionality. The minor number is always included in the version
  number:

* Release: The release number is the third integer in the version
  string (e.g., v1.2.3). Changes in the release number typically
  indicate a bug fix in the code base and/or end-user
  functionality.

* Quantifier: PRRTE version numbers sometimes have an arbitrary
  string affixed to the end of the version number. Common strings
  include:

  * ``aX``: Indicates an alpha release. X is an integer indicating
    the number of the alpha release (e.g., v1.2.3a5 indicates the
    5th alpha release of version 1.2.3).
  * ``bX``: Indicates a beta release. X is an integer indicating
    the number of the beta release (e.g., v1.2.3b3 indicates the 3rd
    beta release of version 1.2.3).
  * ``rcX``: Indicates a release candidate. X is an integer
    indicating the number of the release candidate (e.g., v1.2.3rc4
    indicates the 4th release candidate of version 1.2.3).

Although the major, minor, and release values (and optional
quantifiers) are reported in PRRTE snapshot tarballs, the
filenames of these snapshot tarballs follow a slightly different
convention.

Specifically, the snapshot tarball filename contains three distinct
values:

* Most recent Git tag name on the branch from which the tarball was
  created.

* An integer indicating how many Git commits have occurred since
  that Git tag.

* The Git hash of the tip of the branch.

For example, a snapshot tarball filename of
``prrte-v1.0.2-57-gb9f1fd9.tar.bz2`` indicates that this tarball was
created from the v1.0 branch, 57 Git commits after the ``v1.0.2`` tag,
specifically at Git hash gb9f1fd9.

PRRTE's Git master branch contains a single ``dev`` tag.  For example,
``prrte-dev-8-gf21c349.tar.bz2`` represents a snapshot tarball created
from the master branch, 8 Git commits after the "dev" tag,
specifically at Git hash gf21c349.

The exact value of the "number of Git commits past a tag" integer is
fairly meaningless; its sole purpose is to provide an easy,
human-recognizable ordering for snapshot tarballs.


Version Compatibility Within a DVM
==================================

**Every process in a DVM must come from the same PRRTE build.** Mixing
versions — a ``prte`` of one version with ``prted`` or ``prun`` of another,
or daemons of differing versions across the nodes of one DVM — is strictly
forbidden and is not supported in any form.

This is not a statement about how far apart the versions are: there is no
"close enough" range and no compatibility window. The messages PRRTE's
processes exchange (the launch message, the daemon rollup, the nidmap, the
collectives) carry no format version and no self-description; both ends are
hand-written mirrors of each other, compiled from the same source. A field
added, removed, or retyped on one side and not the other does not produce a
diagnosable error. It silently produces wrong values, or desynchronizes
everything that follows it in the buffer.

So a mixed-version DVM does not fail in a way that points at the mismatch.
It fails as corrupt data somewhere else entirely — a job mapped onto the
wrong nodes, a rank bound to nothing, a daemon that crashes with an
unrelated-looking backtrace. If you are running one, the first step in
diagnosing *any* misbehavior is to stop running one.

Install one build, and make sure every node reaches that same installation.
