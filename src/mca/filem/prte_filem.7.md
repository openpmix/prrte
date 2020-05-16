# NAME

PRTE_FILEM - PRTE MCA File Management (FileM) Framework: Overview of
PRTE's FileM framework, and selected modules.

# DESCRIPTION

FileM is a utility framework used by PRTE for a variety of purposes,
including the transport of checkpoint files, preloading user binaries,
and preloading of user files.

# AVAILABLE COMPONENTS

PRTE currently ships with one FileM component: `rsh`.

The following MCA parameters apply to all components:

`filem_base_verbose`

:   Set the verbosity level for all components. Default is 0, or silent
    except on error.

## rsh FileM Component

The `rsh` component uses `rcp` or `scp` to do its file transfers. This
component requires the use of passwordless `rsh` or `ssh` between all
nodes.

The `rsh` component has the following MCA parameters:

`filem_rsh_priority`

:   The component's priority to use when selecting the most appropriate
    component for a run.

`filem_rsh_verbose`

:   Set the verbosity level for this component. Default is 0, or silent
    except on error.

`filem_rsh_rcp`

:   The program to use to copy files. Generally will be `rcp` or `scp`.

`filem_rsh_rsh`

:   The program used to remotely log into a given machine and remove
    files. Generally will be `rsh` or `ssh`.

## none FileM Component

The `none` component simply selects no FileM component. All of the FileM
function calls return immediately with PRTE_SUCCESS.

This component is the last component to be selected by default. This
means that if another component is available, and the `none` component
was not explicity requested then PRTE will attempt to activate all of
the available components before falling back to this component.

# SEE ALSO

prte-checkpoint(1), prte-restart(1), prte-checkpoint(1),
prte-restart(1), prte_snapc(7), prte_crs(7)
