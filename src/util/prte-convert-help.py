#!/usr/bin/env python3
#
# Copyright (c) 2025      Jeffrey M. Squyres.  All rights reserved.
# Copyright (c) 2025-2026 Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

from __future__ import print_function
import os
import sys
import argparse

def parse_cmd_line_options(path, verbose=False):
    # read the file and parse it for options
    options = []
    with open(path) as file:
        for line in file:
            stripped = line.strip()
            if stripped.startswith("#define"):
                # find the first space at end of "define"
                s1 = stripped.find(' ')
                # first word after "#define " is the name of the option
                start = 8
                end = stripped.find(' ', start+1)
                if -1 == end:
                    continue
                name = stripped[start:end]
                # find the first quote mark
                start = stripped.find('"', end)
                if start != -1:
                    # find the ending quote mark
                    end = stripped.find('"', start+1)
                    # everything in-between is the actual option
                    opt = stripped[start+1:end]
                    # track it
                    options.append((name, opt))

    return options

def find_files(root, verbose=False):
    # Check for existence of root directory - otherwise, we just
    # fall through with no error output
    if not os.path.isdir(root):
        print("Root directory " + root + " does not exist\nCannot continue")
        exit(1)

    # Search for help-*.txt files across the source tree, skipping
    # some directories (e.g., .git)
    # Also find and retrieve all source files (.h or .c)
    # so we can search them for entries
    help_files = []
    source_files = []
    tool_help_files = []
    tool_source_files = []
    # skip infrastructure directories
    skip_dirs = ['.git', '.libs', '.deps']
    # there may also be some help files we want to ignore
    skip_files = []
    for root_dir, dirs, files in os.walk(root):
        for sd in skip_dirs:
            if sd in dirs:
                dirs.remove(sd)

        for file in files:
            if file.startswith("help-") and file.endswith(".txt"):
                skipit = False
                for sf in skip_files:
                    if sf == file:
                        skipit = True
                        break
                if not skipit:
                    full_path = os.path.join(root_dir, file)
                    if "schizo/prte" in full_path or "schizo/ompi" in full_path or "tools" in full_path:
                        tool_help_files.append(full_path)
                        if verbose:
                            print("Found tool help: {full_path}".format(full_path=full_path))
                    else:
                        help_files.append(full_path)
                        if verbose:
                            print("Found help: {full_path}".format(full_path=full_path))
            elif file.endswith(".c") or file.endswith(".h"):
                full_path = os.path.join(root_dir, file)
                if "schizo/prte" in full_path or "schizo/ompi" in full_path or "tools" in full_path:
                    tool_source_files.append(full_path)
                else:
                    source_files.append(full_path)
                if verbose:
                    print("Found source: {full_path}".format(full_path=full_path))

    return help_files, source_files, tool_help_files, tool_source_files

def parse_help_files(file_paths, data, citations, verbose=False):
    # Parse INI-style help files, returning a dictionary with filenames as
    # keys.  Don't use the Python configparse module in order to
    # reduce dependencies (i.e., so that we don't have to pip install
    # anything to run this script).
    for file_path in file_paths:
        sections = {}
        current_section = None
        with open(file_path) as file:
            for line in file:
                stripped = line.rstrip()
                if not stripped and current_section is None:
                    continue
                if stripped.startswith("#include"):
                    # if we aren't in a section, then this is an error
                    if current_section is None:
                        sys.stderr.write("ERROR: INCLUDE is specified in ", file_path, " outside of a section")
                        continue
                    # include the line in the section
                    sections[current_section].append(stripped)
                    # this line includes a file/topic from another file
                    # find the '#' at the end of the "include" keyword
                    start = stripped.find('#', 2)
                    if start != -1:
                        # find the '#'' at the end of the filename
                        end = stripped.find('#', start+1)
                        end2 = stripped.find('#', end+1)
                        if end2 == -1:
                            topic = stripped[end+1:]
                            fil = stripped[start+1:end]
                            citations.append((fil, topic))
                        else:
                            topic = stripped[end2+1:]
                            fil = stripped[end+1:end2]
                            citations.append((fil, topic))
                    # if a section contains an include, we treat that
                    # section as having been cited
                    citations.append((os.path.basename(file_path), current_section))
                    continue
                if stripped.startswith('#'):
                    current_section = None
                    continue
                if stripped.startswith('['):
                    # find the end of the section name
                    end = stripped.find(']', 1)
                    if -1 == end:
                        continue
                    current_section = stripped[1:end]
                    sections[current_section] = list()
                elif current_section is not None:
                    sections[current_section].append(stripped)

        if file_path in data:
            sys.stderr.write("ERROR: path ", file_path, " already exists in data dictionary")
        else:
            data[file_path] = sections
        if verbose:
            p = len(sections)
            print("Parsed: {file_path} ({p} sections found)".format(file_path=file_path, p=p))

    return

def parse_src_files(source_files, citations, verbose=False):
    # search the source code for show_help references
    for src in source_files:
        cont_topic = False
        cont_filename = False
        with open(src) as file:
            for line in file:
                line = line.strip()
                # skip the obvious comment lines
                if line.startswith("//") or line.startswith("/*") or line.startswith("* ") or line.startswith("PRTE_EXPORT"):
                    continue

                if cont_topic:
                    # the topic for the prior filename should be on this line
                    if verbose:
                        print("CONT TOPIC LINE: ", line)
                    start = line.find('"')
                    if start != -1:
                        end = line.find('"', start+1, -1)
                        if end != -1:
                            topic = line[start+1:end]
                            citations.append((filename,topic))
                            if verbose:
                                print("Found topic: ", filename, topic)
                            cont_topic = False
                            continue
                elif cont_filename:
                    # the filename should be on this line
                    start = line.find('"')
                    if start != -1:
                        end = line.find('"', start+1, -1)
                        if end != -1:
                            filename = line[start+1:end]
                            # see if the topic is on the same line
                            start = line.find('"', end+1, -1)
                            if start != -1:
                                # find the end of it
                                end = line.find('"', start+1, -1)
                                if end != -1:
                                    topic = line[start+1:end]
                                    citations.append((filename,topic))
                            else:
                                # topic must be on next line
                                cont_topic = True
                                if verbose:
                                    print("Found filename: ", filename)
                            cont_filename = False
                            continue
                        else:
                            cont_filename = False
                            sys.stderr.write("ERROR: Missing end of filename")
                            continue

                if "pmix_show_help(" in line or "pmix_show_help_string(" in line or "send_error_show_help" in line:
                    cont_topic = False
                    cont_filename = False
                    # line contains call to show-help - try to extract
                    # filename/topic
                    start = line.find('"')
                    if start != -1:
                        end = line.find('"', start+1, -1)
                        if end != -1:
                            filename = line[start+1:end]
                            # see if the topic is on this line
                            start = line.find('"', end+1, -1)
                            if start != -1:
                                # find the end of it
                                end = line.find('"', start+1, -1)
                                if end != -1:
                                    topic = line[start+1:end]
                                    citations.append((filename,topic))
                            else:
                                # topic must be on next line
                                cont_topic = True
                                continue
                    else:
                        # the filename must be on next line
                        cont_filename = True
                        continue

    return

def parse_tool_files(help_files, citations, verbose=False):
    # cycle through the tool help files
    for hlp in help_files:
        topics = []
        options = []
        inUsage = False
        # scan the help file to find cmd line options that
        # are referenced in the help file itself
        with open(hlp) as file:
            continuation = False
            for line in file:
                line = line.rstrip()
                if not line:
                    continue
                if line.startswith('#'):
                    inUsage = False
                if line.startswith('[') and line.endswith(']'):
                    # topic - save it
                    start = line.find('[') + 1
                    end = line.find(']')
                    topic = line[start:end]
                    topics.append(topic)
                    if "usage" == topic:
                        inUsage = True
                    continue
                if not inUsage:
                    continue
                if not line.startswith('|'):
                    continue
                # look for end of option
                entryend = line.find('|', 2)
                # if this is a continuation line, then just take the remaining option words
                if continuation:
                    optend = line.find(' ', 2, entryend)
                    # if the last character is a quote, then back up
                    if line[optend-1] == '"':
                        optend = optend - 1
                    opt = optstart + line[2:optend]
                    options.append(opt)
                    continuation = False
                    continue
                # scan the line for the dash(es) that indicates a cmd line option
                opt = line.find('-', 2, entryend)
                if -1 == opt:
                    continue
                # check for single-dash option
                if '-' != line[opt+1]:
                    options.append(line[opt+1])
                    # adjust the entryend value as there will be a double-dash version
                    entryend = line.find('|', opt+6)
                # there could be a double-dash version as well
                opt = line.find("--", 2, entryend)
                if -1 == opt:
                    continue
                # move past the double-dashes
                opt += 2
                optend = line.find(' ', opt+1, entryend)
                if line[optend-1] == '"':
                    optend = optend - 1
                # if the last character is a "-", then the option continues on next line
                if line[optend-1] == '-':
                    optstart = line[opt:optend]
                    continuation = True
                    continue
                option = line[opt:optend]
                # skip the help, verbose, and version standard options
                if option == "help" or option == "version" or option == "verbose":
                    continue
                # track the option
                options.append(option)
        # scan the options
        for option in options:
            # scan the topics for a match
            for topic in topics:
                if option == topic:
                    if verbose:
                        print("CITED ", os.path.basename(hlp), option)
                    citations.append((os.path.basename(hlp), option))
                    break



def purge(parsed_data, citations):
    special_topics = ["help",
                      "version",
                      "usage",
                      "placement",
                      "placement-examples",
                      "placement-rankfiles",
                      "placement-deprecated",
                      "placement-diagnostics",
                      "placement-fundamentals",
                      "placement-limits"]
    result_data = {}
    errorFound = False
    for filename in parsed_data:
        sections = parsed_data[filename]
        result_sections = {}
        for section in sections:
            content_list = sections[section]
            # check for duplicate entries
            content = '\n'.join(content_list)
            # search all other entries for a matching section
            for (file2, sec) in parsed_data.items():
                if file2 == filename:
                    continue
                for (sec2, cl) in sec.items():
                    cnt = '\n'.join(cl)
                    if sec == section:
                        if content == cnt:
                            # these are the same
                            sys.stderr.write("DUPLICATE FOUND:\n    SECTION: " + section + "\n    FILES: " + \
                                             filename + "\n           " + file2 + "\n")
                            errorFound = True
                        else:
                            # same topic, different content
                            sys.stderr.write("DUPLICATE SECTION WITH DIFFERENT CONTENT:\n    SECTION: " + \
                                             section + "\n    FILES: " + filename + "\n           " + file2 + "\n")
                            errorFound = True
            # search code files for usage
            # protect special values
            if section in special_topics:
                result_sections[section] = content_list
                continue
            for entry in citations:
                used = False
                file2 = entry[0]
                sec = entry[1]
                if file2 == os.path.basename(filename) and sec == section:
                    # this is a used entry
                    result_sections[section] = content_list
                    used = True
                    break;
            if not used:
                sys.stderr.write("** WARNING: Unused help topic\n")
                sys.stderr.write("    File: " + filename + "\n")
                sys.stderr.write("    Section: " + section + "\n")
                errorFound = True
        # see if anything is left
        if 0 < len(result_sections):
            # don't retain the file if no citations for it are left
            result_data[filename] = result_sections
        else:
            sys.stderr.write("File " + filename + " has no used topics - omitting\n")
            errorFound = True
        if errorFound:
            exit(1)

    if errorFound:
        exit(1)
    return result_data

def generate_c_code(parsed_data):
    # Generate C code with an array of filenames and their
    # corresponding INI sections.
    c_code = "// THIS FILE IS GENERATED AUTOMATICALLY! EDITS WILL BE LOST!\n" + \
             "// This file generated by " + os.path.basename(sys.argv[0]) + "\n\n"

    c_code += "#include \"src/include/prte_config.h\"\n" + \
              "#include <stdio.h>\n" + \
              "#include <string.h>\n\n" + \
              "#include \"src/include/pmix_globals.h\"\n\n"

    ini_arrays = []
    file_entries = []

    for idx, (fil, sections) in enumerate(sorted(parsed_data.items())):
        filename = os.path.basename(fil)
        var_name = filename.replace('-', '_').replace('.', '_')

        ini_entries = []
        for section, content_list in sections.items():
            entry = "    { .topic = \"" + section + "\",\n      .content = (const char *[]){\n"
            for content in content_list:
                c_content = content.replace('"','\\"').replace("\n", '\\n"\n"').replace("\\;", ";")
                entry += "                 \"" + c_content + "\",\n"
            entry += "                 NULL}\n    },\n"
            ini_entries.append(entry)
        ini_entries.append("    { .topic = NULL, .content = NULL }")

        ini_array_name = "ini_entries_" + str(idx)
        ini_arrays.append("static pmix_show_help_entry_t " + ini_array_name + "[] = {\n" + "\n".join(ini_entries) + "\n};\n")
        file_entries.append("    { \"" + filename + "\", " + ini_array_name + "}")
    file_entries.append("    { NULL, NULL }")

    c_code += "\n".join(ini_arrays) + "\n"
    c_code += "pmix_show_help_file_t prte_show_help_data[] = {\n" + ",\n".join(file_entries) + "\n};\n"

    return c_code

#-------------------------------

def main():
    parser = argparse.ArgumentParser(description="Generate C code from help text INI files.")
    parser.add_argument("--root",
                        required=True,
                        help="Root directory to search for help-*.txt files")
    parser.add_argument("--out",
                        required=True,
                        help="Output C file")
    parser.add_argument("--verbose",
                        action="store_true",
                        help="Enable verbose output")
    parser.add_argument("--purge",
                        action="store_true",
                        help="Purge duplicates, update topic pointers as required")
    parser.add_argument("--dryrun",
                        action="store_true",
                        help="Do not write out resulting ini_array_name")

    args = parser.parse_args()

    parsed_data = {}
    citations = []
    cli_options = []
    outdata = {}
    rootdir = os.path.join(args.root, "src")

    # the options are in root/util/prte_cmd_line.h
    path = os.path.join(rootdir, "util", "prte_cmd_line.h")
    if not os.path.exists(path):
        sys.stderr.write("File " + path + " does not exist\nCannot continue")
        exit(1)
    # obtain a list of (option name, string) tuples
    cli_options = parse_cmd_line_options(path, args.verbose)

    help_files, source_files, tool_help_files, tool_source_files = find_files(rootdir, args.verbose)
    parse_help_files(help_files, parsed_data, citations, args.verbose)
    parse_help_files(tool_help_files, parsed_data, citations, args.verbose)
    parse_src_files(source_files, citations, args.verbose)
    parse_src_files(tool_source_files, citations, args.verbose)
    parse_tool_files(tool_help_files, citations, args.verbose)
    if args.purge:
        outdata = purge(parsed_data, citations)
    else:
        outdata = parsed_data

    if not args.dryrun:
        c_code = generate_c_code(outdata)

        with open(args.out, "w") as f:
            f.write(c_code)

        if args.verbose:
            print("Generated C code written to ", args.out)

if __name__ == "__main__":
    main()
