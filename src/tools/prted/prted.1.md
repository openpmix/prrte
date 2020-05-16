# NAME

prted - Start an PRTE User-Level Daemon

# SYNOPSIS

```
prted [options]
```

# DESCRIPTION

`prted` starts an PRTE daemon for the PRTE system.

# NOTE

The `prted` command is _not intended to be manually invoked by end
users._ It is part of the PRTE architecture and is invoked
automatically as necessary. This man page is mainly intended for those
adventerous end users and system administrators who have noticed an
`prted` process and wondered what it is.

As such, the command line options accepted by the `prted` are not
listed below because they are considered internal and are therefore
subject to change between versions without warning. Running `prted`
with the `--help` command line option will show all available options.

# AUTHORS

The PRTE maintainers -- see
[https://github.com/openpmix/prte](https://github.com/openpmix/prte)
or the file `AUTHORS`.

This manual page was originally contributed by Dirk Eddelbuettel
<edd@debian.org>, one of the Debian GNU/Linux maintainers for Open
MPI, and may be used by others.
