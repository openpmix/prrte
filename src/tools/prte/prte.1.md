# NAME

prte - Establish a PRTE Distributed Virtual Machine (DVM).

# SYNOPSIS

```
prte [ options ]
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

`prte` will establish a DVM that can be used to execute subsequent
applications. Use of `prte` can be advantageous, for example, when
you want to execute a number of short-lived tasks (e.g., in a workflow
scenario). In such cases, the time required to start the PRTE DVM can be a
significant fraction of the time to execute the overall application. Thus,
creating a persistent PRTE DVM can speed the overall execution. In addition, a
persistent PRTE DVM will support executing multiple parallel applications while
maintaining separation between their respective cores.

# OPTIONS

This section includes many commonly used options. There may be other options
listed with `prte --help`.

`-h, --help`

:   Display help for this command

`-V, --version`

:   Print version number. If no other arguments are given, this will
    also cause `prte` to exit.

`--daemonize`

:   Daemonize the DVM daemons into the background

`--no-ready-msg`

:   Do not print a DVM ready message

`--report-pid <arg0>`

:   Print out `prte`'s PID during startup. The `<arg0>` must be a `-` to
    indicate that the PID is to be output to `stdout`, a `+` to indicate that
    the PID is to be output to `stderr`, or a filename to which the PID is to
    be written.

`--report-uri <arg0>`

:   Print out `prte`'s URI during startup. The `<arg0>` must be a `-` to
    indicate that the URI is to be output to `stdout`, a `+` to indicate that
    the URI is to be output to `stderr`, or a filename to which the URI is to
    be written.

`--system-server`

:   Start the DVM as the system server

`--prefix <dir>`

:   Prefix directory that will be used to set the `PATH` and
    `LD_LIBRARY_PATH` on the remote node before invoking the PRTE
    daemon.

Use one of the following options to specify which hosts (nodes) of the
cluster to use for the DVM. See prte-map(1) for more details.

`-H, --host <host1,host2,...,hostN>`

:   List of hosts for the DVM.

`--hostfile <hostfile>`

:   Provide a hostfile to use.

`--machinefile <machinefile>`

:   Synonym for `-hostfile`.

Setting MCA parameters:

`--gpmixmca <key> <value>`

:   Pass global PMIx MCA parameters that are applicable to all application
    contexts. `<key>` is the parameter name; `<value>` is the parameter value.

`--mca <key> <value>`

:   Send arguments to various MCA modules. See the "MCA" section,
    below.

`--pmixmca <key> <value>`

:   Send arguments to various PMIx MCA modules. See the "MCA" section,
    below.

`--prtemca <key> <value>`

:   Send arguments to various PRTE MCA modules. See the "MCA" section,
    below.

`--pmixam <arg0>`

:   Aggregate PMIx MCA parameter set file list. The `arg0` argument is a
    comma-separated list of tuning files. Each file containing MCA parameter
    sets for this application context.

The following options are useful for developers; they are not generally
useful to most PRTE users:

`-d, --debug-devel`

:   Enable debugging of the PRTE layer.

`--debug-daemons-file`

:   Enable debugging of the PRTE daemons in the DVM, storing output in
    files.


# DESCRIPTION

`prte` starts a Distributed Virtual Machine (DVM) by launching a
daemon on each node of the allocation, as modified or specified by the
`--host` and `--hostfile` options (See prte-map(1) for more details).
Applications can subsequently be executed using the `prun` command. The DVM
remains in operation until receiving the `pterm` command.

When starting the Distributed Virtual Machine (DVM), `prte` will prefer to use
the process starter provided by a supported resource manager to start the
`prted` daemons on the allocated compute nodes. If a supported resource manager
or process starter is not available then `rsh` or `ssh` are used with a
corresponding hostfile, or if no hostfile is provided then all `X` copies are
run on the `localhost`.

# RETURN VALUE

`prte` returns `0` if no abnormal daemon failure occurs during the life of the
DVM, and non-zero otherwise.
