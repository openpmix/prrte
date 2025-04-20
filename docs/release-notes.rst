Release Notes
=============

- Systems that have been tested are:

  - Linux (various flavors/distros), 64 bit (x86 and ARM), with gcc
  - OS X (14.0 and above), 64 bit (x86_64 and ARM), with gcc and clang

- Launch environment testing status:

  - ssh: fully tested
  - slurm: compiled, no testing
  - lsf: compiled using a shim header, no testing
  - pals: compiled, no testing
  - tm (Torque): compiled using a shim header, no testing

- PRRTE has taken some steps towards Reproducible Builds
  (https://reproducible-builds.org/).  Specifically, PRRTE's
  ``configure`` and ``make`` process, by default, records the build
  date and some system-specific information such as the hostname where
  PRRTE was built and the username who built it.  If you desire a
  Reproducible Build, set the ``$SOURCE_DATE_EPOCH``, ``$USER`` and
  ``$HOSTNAME`` environment variables before invoking ``configure``
  and ``make``, and PRRTE will use those values instead of invoking
  ``whoami`` and/or ``hostname``, respectively.  See
  https://reproducible-builds.org/docs/source-date-epoch/ for
  information on the expected format and content of the
  ``$SOURCE_DATE_EPOCH`` variable.
