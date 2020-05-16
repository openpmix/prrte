# NAME

prte_info - Display information about the PRTE installation

# SYNOPSIS

```
prte-info [options]
```

# DESCRIPTION

`prte_info` provides detailed information about the PRTE
installation. It can be useful for at least three common scenarios:

1. Checking local configuration and seeing how PRTE was installed.
1. Submitting bug reports / help requests to the PRTE community (see
   [https://github.com/openpmix/prte/issues](https://github.com/openpmix/prte/issues)).
1. Seeing a list of installed PRTE plugins and querying what MCA
   parameters they support.

# OPTIONS

`prte_info` accepts the following options:

`-a|--all`

:   Show all configuration options and MCA parameters

`--arch`

:   Show architecture PRTE was compiled on

`-c|--config`

:   Show configuration options

`-gmca|--gmca <param> <value>`

:   Pass global MCA parameters that are applicable to all contexts.

`-h|--help`

:   Shows help / usage message

`--hostname`

:   Show the hostname on which PRTE was configured and built.

`--internal`

:   Show internal MCA parameters (not meant to be modified by users)

`-mca|--mca <param> <value>`

:   Pass context-specific MCA parameters; they are considered global if
    `--gmca` is not used and only one context is specified.

`--param <type> <component>`

:   Show MCA parameters. The first parameter is the type of the
    component to display; the second parameter is the specific component
    to display (or the keyword `all`, meaning "display all components
    of this type").

`--parsable`

:   When used in conjunction with other parameters, the output is
`   displayed in a machine-parsable format

`--parseable`

:   Synonym for `--parsable`.

`--path <type>`

:   Show paths that PRTE was configured with. Accepts the following
    parameters: `prefix`, `bindir`, `libdir`, `incdir`, `pkglibdir`,
    `sysconfdir`.

`--pretty`

:   When used in conjunction with other parameters, the output is
    displayed in 'prettyprint' format (default).

`-v|--version <component> <scope>`

:   Show version of PRTE or a component. <component> can be the
    keywords `ompi` or `all`, the name of a framework (e.g., `coll`
    shows all components in the `coll` framework), or the name of a
    specific component (e.g., `pls:rsh` shows the information from the
    rsh PLS component). <scope> can be one of: `full`, `major`,
    `minor`, `release`, `greek`, `git`.

# EXAMPLES

`prte_info`

:   Show the default output of options and listing of installed
    components in a human-readable / prettyprint format.

`prte_info --parsable`

:   Show the default output of options and listing of installed
    components in a machine-parsable format.

`prte_info --param rmcast udp`

:   Show the MCA parameters of the "udp" RMCAST component in a
    human-readable / prettyprint format.

`prte_info --param rmcast udp --parsable`

:   Show the MCA parameters of the "udp" RMCAST component in a
    machine-parsable format.

`prte_info --path bindir`

:   Show the "bindir" that PRTE was configured with.

`prte_info --version prte full --parsable`

:   Show the full version numbers of PRTE (including the PRTE version
    number) in a machine-readable format.

`prte_info --version rmcast major`

:   Show the major version number of all RMCAST components in a
    prettyprint format.

`prte_info --version rmcast:tcp minor`

:   Show the minor version number of the TCP RMCAST component in a
    prettyprint format.

`prte_info --all`

:   Show `all` information about the PRTE installation, including all
    components that can be found, the MCA parameters that they support,
    versions of PRTE and the components, etc.

# AUTHORS

The PRTE maintainers -- see
[https://github.com/openpmix/prte](https://github.com/openpmix/prte)
or the file `AUTHORS`.

This manual page was originally contributed by Dirk Eddelbuettel
<edd@debian.org>, one of the Debian GNU/Linux maintainers for Open
MPI, and may be used by others.
