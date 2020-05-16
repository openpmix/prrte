# NAME

pterm - Terminate an instance of the PMIx Reference RTE (PRTE)

# SYNOPSIS

```
pterm JMS write some more here
```

# DESCRIPTION

`pterm` terminates an instance of the PMIx reference run time
environment (PRTE).

JMS write more here

# OPTIONS

JMS `pterm --help` shows this:

```
pterm (PRTE) 2.0.0a1

Usage: pterm [OPTION]...
Terminate an instance of the PMIx Reference RTE (PRTE)

/*****      General Options      *****/

-h|--help                            This help message
-v|--verbose                         Be verbose
-V|--version                         Print version and exit



/*****    DVM-Specific Options   *****/

   --dvm-uri <arg0>                  Specify the URI of the DVM master, or the name of the file (specified as
                                     file:filename) that contains that info
   --num-connect-retries             Max number of times to try to connect
   --pid <arg0>                      PID of the session-level daemon to which we should connect
   --system-server-first             First look for a system server and connect to it if found
   --system-server-only              Connect only to a system-level server
   --wait-to-connect                 Delay specified number of seconds before trying to connect
```

# AUTHORS

The PRTE maintainers -- see
[https://github.com/openpmix/prte](https://github.com/openpmix/prte)
or the file `AUTHORS`.
