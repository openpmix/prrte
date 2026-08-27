#!/usr/bin/env perl
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# A static initializer that names the object it initializes must name the
# RIGHT object.
#
# PMIX_LIST_STATIC_INIT takes the list it is initializing, because an
# empty list is one whose sentinel points at itself and there is no other
# way to spell that address at compile time. Handing it a different list
# of the same type compiles perfectly and produces a list whose sentinel
# belongs to somebody else: appends land on the wrong list, walks find
# the wrong items, and destructing either one leaves the other pointing
# into freed memory. Nothing downstream catches it, which is why this
# exists.
#
# Nothing here is maintained by hand, and there is no list of macros to
# keep in step. The set is derived two ways, because a tree may use a
# self-naming initializer it does not define -- PRRTE gets
# PMIX_LIST_STATIC_INIT from an installed PMIx header, outside anything
# it can scan:
#
#   by definition, when the macro is declared in this tree: its parameter
#     is used as an object -- "(p)." -- rather than as a type or a class,
#     which is what separates PMIX_LIST_STATIC_INIT(l) from
#     PMIX_OBJ_STATIC_INIT(pmix_object_t);
#
#   by use, otherwise: the argument names a member ("obj.field") or
#     repeats the object being initialized ("var = NAME(var)"). A macro
#     taking a type never looks like either.
#
# The second rule is what lets one copy of this script serve both trees.
# It classifies on the *shape* of the argument, not on whether the
# argument is correct, so a use that names the wrong object still marks
# the macro as self-naming and is then caught below. Its only blind spot
# is a macro whose every use in the tree is wrong, which would have to be
# introduced that way from the start.

use strict;
use warnings;
use File::Basename;
use File::Find;

# Top of the source tree, from this script's own location, so that a
# VPATH build and an in-tree build both work without being told.
my $top = dirname(dirname(dirname(File::Spec->rel2abs($0))));
$top = $ENV{srcdir} . "/../.." if (!-d "$top/src" && defined $ENV{srcdir});
if (!-d "$top/src") {
    print "check_static_init: cannot locate the source tree from $0\n";
    exit 77;    # skip rather than fail: this is not a defect in the tree
}

# Not every tree has every one of these, and a missing directory is not
# something to fail over.
my @roots = grep { -d $_ } ("$top/src", "$top/test", "$top/examples");
my @files;
find(sub { push @files, $File::Find::name if (/\.[ch]$/); }, @roots) if (@roots);
@files = sort @files;

# ---------------------------------------------------------------- #
# 1. which *_STATIC_INIT macros name an object?
# ---------------------------------------------------------------- #
my %selfnaming;

# 1a. by definition, for the ones declared here
foreach my $f (@files) {
    next unless ($f =~ /\.h$/);
    open(my $fh, '<', $f) or next;
    my @lines = <$fh>;
    close($fh);
    for (my $i = 0; $i < @lines; $i++) {
        next unless ($lines[$i] =~ /^\s*#\s*define\s+(\w+_STATIC_INIT)\s*\(\s*(\w+)\s*\)/);
        my ($name, $param) = ($1, $2);
        # gather the whole definition, following backslash continuations
        my $body = $lines[$i];
        while ($body =~ /\\\s*$/ && $i + 1 < @lines) {
            $body .= $lines[++$i];
        }
        # the parameter used as an object - "(p)." - not as a type name
        $selfnaming{$name} = 1 if ($body =~ /\(\s*\Q$param\E\s*\)\s*\./);
    }
}
# 1b. by use, for the ones that come from somewhere else
foreach my $f (@files) {
    open(my $fh, '<', $f) or next;
    while (my $line = <$fh>) {
        # NOTE: capture into lexicals before matching again - a nested
        # match resets $1..$N, so testing $arg first and reading $2
        # afterwards silently reads the wrong match.
        if ($line =~ /\.(\w+)\s*=\s*(\w+_STATIC_INIT)\s*\(\s*(.*?)\s*\)\s*(?=[,;}]|\\?\s*$)/) {
            my ($macro, $arg) = ($2, $3);
            $selfnaming{$macro} = 1 if ($arg =~ /\.\w+$/);
            next;
        }
        if ($line =~ /\b(\w+)\s*=\s*(\w+_STATIC_INIT)\s*\(\s*(.*?)\s*\)\s*(?=[,;}]|\\?\s*$)/) {
            my ($var, $macro, $arg) = ($1, $2, $3);
            $selfnaming{$macro} = 1 if ($arg eq $var);
        }
    }
    close($fh);
}

if (!keys %selfnaming) {
    print "check_static_init: found no self-naming initializers to check\n";
    exit 77;
}
my $macros = join('|', map { quotemeta } sort keys %selfnaming);

# ---------------------------------------------------------------- #
# 2. every use must name the object it is initializing
# ---------------------------------------------------------------- #
my ($checked, @bad) = (0);
foreach my $f (@files) {
    open(my $fh, '<', $f) or next;
    my $lno = 0;
    while (my $line = <$fh>) {
        $lno++;
        my $rel = $f;
        $rel =~ s/^\Q$top\E\///;

        # a struct member:   .field = NAME(obj.field)
        #
        # The argument may itself contain parentheses - a composed
        # initializer passes "(e).actives" down - so it runs to the ")"
        # that closes the macro, which is the one followed by a comma, a
        # brace, a semicolon, or the end of the line (a line continuation
        # included).
        if ($line =~ /\.(\w+)\s*=\s*($macros)\s*\(\s*(.*?)\s*\)\s*(?=[,;}]|\\?\s*$)/) {
            my ($field, $macro, $arg) = ($1, $2, $3);
            $checked++;
            next if ($arg =~ /\.\Q$field\E$/);
            push @bad, sprintf("%s:%d: .%s is initialized by %s(%s)\n"
                             . "    the argument must name this member, i.e. end in \".%s\"",
                               $rel, $lno, $field, $macro, $arg, $field);
            next;
        }

        # a whole object:    pmix_list_t foo = NAME(foo)
        if ($line =~ /\b(\w+)\s*=\s*($macros)\s*\(\s*(.*?)\s*\)\s*(?=[,;}]|\\?\s*$)/) {
            my ($var, $macro, $arg) = ($1, $2, $3);
            $checked++;
            next if ($arg eq $var);
            push @bad, sprintf("%s:%d: %s is initialized by %s(%s)\n"
                             . "    the argument must name the object itself, i.e. \"%s\"",
                               $rel, $lno, $var, $macro, $arg, $var);
        }
    }
    close($fh);
}

printf("checked %d use%s of %d self-naming initializer%s (%s)\n",
       $checked, ($checked == 1 ? "" : "s"),
       scalar(keys %selfnaming), (keys %selfnaming == 1 ? "" : "s"),
       join(", ", sort keys %selfnaming));

if (@bad) {
    print "\nFAILED - a static initializer names the wrong object:\n\n";
    print "  $_\n\n" foreach (@bad);
    exit 1;
}
print "all of them name the object they initialize\n";
exit 0;
