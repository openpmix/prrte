# NAME

prterun - Execute serial and parallel jobs with the PMIx Reference Runtime (PRTE).

# SYNOPSIS

`prterun` does **not** require a running `prte` Distributed Virtual Machine
(DVM). It will start the `prte` DVM, run a single job, and shutdown the DVM.

Single Process Multiple Data (SPMD) Model:

```
prterun [ options ] <program> [ <args> ]
```

Multiple Instruction Multiple Data (MIMD) Model:

```
prterun [ global_options ] \
        [ local_options1 ] <program1> [ <args1> ] : \
        [ local_options2 ] <program2> [ <args2> ] : \
        ... : \
        [ local_optionsN ] <programN> [ <argsN> ]
```

Note that in both models, invoking `prterun` via an absolute path name is
equivalent to specifying the `--prefix` option with a `<dir>` value
equivalent to the directory where `prterun` resides, minus its last
subdirectory. For example:

```
$ /usr/local/bin/prterun ...
```

is equivalent to

```
$ prterun --prefix /usr/local
```

# QUICK SUMMARY

If you are simply looking for how to run an application, you probably
want to use a command line of the following form:

```
$ prterun [ -np X ] [ --hostfile <filename> ] <program>
```

This will run `X` copies of `<program>` in your current run-time environment
over the set of hosts specified by `<filename>`, scheduling (by default)
in a round-robin fashion by CPU slot. If running under a supported resource
manager a hostfile is usually not required unless the caller wishes to further
restrict the set of resources used for that job.

# OPTIONS

This section includes many commonly used options. There may be other options
listed with `prterun --help`.

`prterun` will send the name of the directory where it was invoked on the
local node to each of the remote nodes, and attempt to change to that
directory. See the "Current Working Directory" section below for
further details.

`<program>`

:   The program executable. This is identified as the first
    non-recognized argument to `prterun`.

`<args>`

:   Pass these run-time arguments to every new process. These must
    always be the last arguments to `prterun` after the `<program>`.
    If an app context file is used, `<args>` will be ignored.

`-h, --help`

:   Display help for this command

`-V, --version`

:   Print version number. If no other arguments are given, this will
    also cause `prterun` to exit.

Since `prterun` combines both `prte` and `prun` it accepts all of the command
line arguments from both of these tools. See prte(1) and prun(1) for details
on the command line options. See prte-map(1) for more details on mapping,
ranking, and binding options.

# DESCRIPTION

One invocation of `prterun` starts the PRTE DVM (i.e., `prte`), runs a single
job (similar to `prun`), then terminates the DVM (similar to `pterm`). If
the application is single process multiple data (SPMD), the application
can be specified on the `prterun` command line.

If the application is multiple instruction multiple data (MIMD),
comprising of multiple programs, the set of programs and argument can be
specified in one of two ways: Extended Command Line Arguments, and
Application Context.

An application context describes the MIMD program set including all
arguments in a separate file. This file essentially contains multiple
`prterun` command lines, less the command name itself. The ability to
specify different options for different instantiations of a program is
another reason to use an application context.

Extended command line arguments allow for the description of the
application layout on the command line using colons (`:`) to separate
the specification of programs and arguments. Some options are globally
set across all specified programs (e.g. `--hostfile`), while others are
specific to a single program (e.g. `--np`).

# RETURN VALUE

See prun(1) for details.
