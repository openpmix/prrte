# NAME

prte - Establish a PRRTE Distributed Virtual Machine (DVM).

# SYNOPSIS

Persistent DVM mode:
```
prte [ options ]
```

Single-Use DVM mode:
```
prte [ options ] <program> [ <args> ]
```

Invoking `prte` via an absolute path name is equivalent to
specifying the `--prefix` option with a `<dir>` value equivalent to
the directory where `prte` resides, minus its last subdirectory.
For example:

```
$ /usr/local/bin/prte ...
```

is equivalent to

```
$ prte --prefix /usr/local ...
```

# QUICK SUMMARY

`prte` can be invoked in one of two DVM modes, namely, Persistent and Single-Use
modes.

In Persistent mode, `prte` will establish a Distributed Virtual Machine (DVM)
that can be used to execute subsequent applications. Use of `prte` in this mode
can be advantageous, for example, when you want to execute a number of
short-lived tasks (e.g., in a workflow scenario). In such cases, the time
required to start the PRRTE DVM can be a significant fraction of the time to
execute the overall application. Thus, creating a persistent PRRTE DVM can
speed the overall execution. In addition, a persistent DVM will support
executing multiple parallel applications while maintaining separation between
their respective cores.

In Single-Use mode, `prte` will establish a Distributed Virtual Machine (DVM),
run the specified `<program>`, then shutdown the PRRTE DVM. Use of `prte` in
this mode can streamline applications that only want to execute a single
application. In this mode `prte` accepts all of the `prun` command line options
for starting the application.

# OPTIONS

`prte` accepts all of the command line options from `prun`. Some of which are
only meaningful when `prte` is used in the Single-Use mode. Below are frequently
used options and those options unique to `prte`.

`-h, --help`

:   Display help for this command

`-V, --version`

:   Print version number. If no other arguments are given, this will
    also cause `prte` to exit.

`--daemonize`

:   Daemonize the DVM daemons into the background

`--no-ready-msg`

:  Do not print a "DVM ready" message

`--report-pid <channel>`

:   Print out `prte`'s PID during startup. The `<channel>` must be
    either a '-' to indicate that the URI is to be output to stdout, a
    '+' to indicate that the URI is to be output to stderr, or a
    filename to which the URI is to be written.

`--report-uri <channel>`

:   Print out `prte`'s URI during startup. The `<channel>` must be
    either a '-' to indicate that the URI is to be output to stdout, a
    '+' to indicate that the URI is to be output to stderr, or a
    filename to which the URI is to be written.

`--system-server`

:   Start the DVM as the system server

`--set-sid`

:   Direct the DVM daemons to separate from the current session

`--max-vm-size <arg0>`

:   The number of DVM daemons to start

`--launch-agent <arg0>`

:   Name of DVM daemon executable used to start processes on remote nodes.
    Default: `prted`

## Debugging Options

The following options are useful for developers; they are not generally
useful to most PRRTE users:

`-d, --debug-devel`

:   Enable debugging of the PRRTE layer.

`--debug`

:   Top-level PRRTE debug switch (default: false)

`--debug-daemons`

:   Enable debugging of the PRRTE daemons in the DVM, output to the terminal.

`--debug-daemons-file`

:   Enable debugging of the PRRTE daemons in the DVM, storing output in
    files.

`--debug-verbose <arg0>`

:  Verbosity level for PRRTE debug messages (default: `1`)

`--leave-session-attached`

:  Do not discard stdout/stderr of remote PRRTE daemons

`--test-suicide <arg0>`

:  Suicide instead of clean abort after delay

There may be other options listed with `prte --help`.


# DESCRIPTION

`prte` starts a Distributed Virtual Machine (DVM) by launching a
daemon on each node of the allocation, as modified or specified by the
`--host` and `--hostfile` options.

In the Persistent mode, applications can subsequently be executed using the
`prun` command. In this mode, the DVM remains in operation until receiving the
`pterm` command.

In the Single-Use mode, applications are executed immediately after `prte`
establishes the DVM, and the DVM is cleaned up when the application terminates.

# RETURN VALUE

In the Persistent mode, `prte` returns 0 if no abnormal daemon failure occurs
during the life of the DVM, and non-zero otherwise.

In the Single-Use mode, `prte` returns the value that `prun` would have returned
for that application. See the `prun` man page for details.
