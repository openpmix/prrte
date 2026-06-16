# $COPYRIGHT$
# Additional copyrights may follow
# $HEADER$

# These are PMIx client/tool programs used for manual and integration
# testing of attach support under prte/prun. They require a running DVM
# and job environment to exercise meaningfully, so they are built but are
# not part of TESTS - "make check" must not attempt to run them.

# See ../Makefile.am for why CFLAGS is overridden here.
CFLAGS = -g

noinst_PROGRAMS = app tool

app_SOURCES = app.c
tool_SOURCES = tool.c
