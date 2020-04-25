/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007      Voltaire. All rights reserved.
 * Copyright (c) 2012      Los Alamos National Security, LLC. All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 *
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020 IBM Corporation. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include <stdlib.h>
#include <string.h>

#include "src/util/argv.h"
#include "src/util/string_copy.h"
#include "constants.h"

#define ARGSIZE 128


/*
 * Append a string to the end of a new or existing argv array.
 */
int prrte_argv_append(int *argc, char ***argv, const char *arg)
{
    int rc;

    /* add the new element */
    if (PRRTE_SUCCESS != (rc = prrte_argv_append_nosize(argv, arg))) {
        return rc;
    }

    *argc = prrte_argv_count(*argv);

    return PRRTE_SUCCESS;
}

int prrte_argv_append_nosize(char ***argv, const char *arg)
{
    int argc;

  /* Create new argv. */

  if (NULL == *argv) {
    *argv = (char**) malloc(2 * sizeof(char *));
    if (NULL == *argv) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }
    argc = 0;
    (*argv)[0] = NULL;
    (*argv)[1] = NULL;
  }

  /* Extend existing argv. */
  else {
        /* count how many entries currently exist */
        argc = prrte_argv_count(*argv);

        *argv = (char**) realloc(*argv, (argc + 2) * sizeof(char *));
        if (NULL == *argv) {
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }
    }

    /* Set the newest element to point to a copy of the arg string */

    (*argv)[argc] = strdup(arg);
    if (NULL == (*argv)[argc]) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    argc = argc + 1;
    (*argv)[argc] = NULL;

    return PRRTE_SUCCESS;
}

int prrte_argv_prepend_nosize(char ***argv, const char *arg)
{
    int argc;
    int i;

    /* Create new argv. */

    if (NULL == *argv) {
        *argv = (char**) malloc(2 * sizeof(char *));
        if (NULL == *argv) {
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }
        (*argv)[0] = strdup(arg);
        (*argv)[1] = NULL;
    } else {
        /* count how many entries currently exist */
        argc = prrte_argv_count(*argv);

        *argv = (char**) realloc(*argv, (argc + 2) * sizeof(char *));
        if (NULL == *argv) {
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }
        (*argv)[argc+1] = NULL;

        /* shift all existing elements down 1 */
        for (i=argc; 0 < i; i--) {
            (*argv)[i] = (*argv)[i-1];
        }
        (*argv)[0] = strdup(arg);
    }

    return PRRTE_SUCCESS;
}

int prrte_argv_append_unique_nosize(char ***argv, const char *arg)
{
    int i;

    /* if the provided array is NULL, then the arg cannot be present,
     * so just go ahead and append
     */
    if (NULL == *argv) {
        return prrte_argv_append_nosize(argv, arg);
    }

    /* see if this arg is already present in the array */
    for (i=0; NULL != (*argv)[i]; i++) {
        if (0 == strcmp(arg, (*argv)[i])) {
            /* already exists */
            return PRRTE_SUCCESS;
        }
    }

    /* we get here if the arg is not in the array - so add it */
    return prrte_argv_append_nosize(argv, arg);
}

/*
 * Free a NULL-terminated argv array.
 */
void prrte_argv_free(char **argv)
{
  char **p;

  if (NULL == argv)
    return;

  for (p = argv; NULL != *p; ++p) {
    free(*p);
  }

  free(argv);
}


/*
 * Split a string into a NULL-terminated argv array.
 */
static char **prrte_argv_split_inter(const char *src_string, int delimiter,
        int include_empty)
{
  char arg[ARGSIZE];
  char **argv = NULL;
  const char *p;
  char *argtemp;
  int argc = 0;
  size_t arglen;

  while (src_string && *src_string) {
    p = src_string;
    arglen = 0;

    while (('\0' != *p) && (*p != delimiter)) {
      ++p;
      ++arglen;
    }

    /* zero length argument, skip */

    if (src_string == p) {
      if (include_empty) {
        arg[0] = '\0';
        if (PRRTE_SUCCESS != prrte_argv_append(&argc, &argv, arg))
          return NULL;
      }
    }

    /* tail argument, add straight from the original string */

    else if ('\0' == *p) {
      if (PRRTE_SUCCESS != prrte_argv_append(&argc, &argv, src_string))
	return NULL;
      src_string = p;
      continue;
    }

    /* long argument, malloc buffer, copy and add */

    else if (arglen > (ARGSIZE - 1)) {
        argtemp = (char*) malloc(arglen + 1);
      if (NULL == argtemp)
	return NULL;

      prrte_string_copy(argtemp, src_string, arglen + 1);
      argtemp[arglen] = '\0';

      if (PRRTE_SUCCESS != prrte_argv_append(&argc, &argv, argtemp)) {
	free(argtemp);
	return NULL;
      }

      free(argtemp);
    }

    /* short argument, copy to buffer and add */

    else {
      prrte_string_copy(arg, src_string, arglen + 1);
      arg[arglen] = '\0';

      if (PRRTE_SUCCESS != prrte_argv_append(&argc, &argv, arg))
	return NULL;
    }

    src_string = p + 1;
  }

  /* All done */

  return argv;
}

char **prrte_argv_split(const char *src_string, int delimiter)
{
    return prrte_argv_split_inter(src_string, delimiter, 0);
}

char **prrte_argv_split_with_empty(const char *src_string, int delimiter)
{
    return prrte_argv_split_inter(src_string, delimiter, 1);
}

/*
 * Return the length of a NULL-terminated argv array.
 */
int prrte_argv_count(char **argv)
{
  char **p;
  int i;

  if (NULL == argv)
    return 0;

  for (i = 0, p = argv; *p; i++, p++)
    continue;

  return i;
}


/*
 * Join all the elements of an argv array into a single
 * newly-allocated string.
 */
char *prrte_argv_join(char **argv, int delimiter)
{
  char **p;
  char *pp;
  char *str;
  size_t str_len = 0;
  size_t i;

  /* Bozo case */

  if (NULL == argv || NULL == argv[0]) {
      return strdup("");
  }

  /* Find the total string length in argv including delimiters.  The
     last delimiter is replaced by the NULL character. */

  for (p = argv; *p; ++p) {
    str_len += strlen(*p) + 1;
  }

  /* Allocate the string. */

  if (NULL == (str = (char*) malloc(str_len)))
    return NULL;

  /* Loop filling in the string. */

  str[--str_len] = '\0';
  p = argv;
  pp = *p;

  for (i = 0; i < str_len; ++i) {
    if ('\0' == *pp) {

      /* End of a string, fill in a delimiter and go to the next
         string. */

      str[i] = (char) delimiter;
      ++p;
      pp = *p;
    } else {
      str[i] = *pp++;
    }
  }

  /* All done */

  return str;
}


/*
 * Join all the elements of an argv array from within a
 * specified range into a single newly-allocated string.
 */
char *prrte_argv_join_range(char **argv, size_t start, size_t end, int delimiter)
{
    char **p;
    char *pp;
    char *str;
    size_t str_len = 0;
    size_t i;

    /* Bozo case */

    if (NULL == argv || NULL == argv[0] || (int)start > prrte_argv_count(argv)) {
        return strdup("");
    }

    /* Find the total string length in argv including delimiters.  The
     last delimiter is replaced by the NULL character. */

    for (p = &argv[start], i=start; *p && i < end; ++p, ++i) {
        str_len += strlen(*p) + 1;
    }

    /* Allocate the string. */

    if (NULL == (str = (char*) malloc(str_len)))
        return NULL;

    /* Loop filling in the string. */

    str[--str_len] = '\0';
    p = &argv[start];
    pp = *p;

    for (i = 0; i < str_len; ++i) {
        if ('\0' == *pp) {

            /* End of a string, fill in a delimiter and go to the next
             string. */

            str[i] = (char) delimiter;
            ++p;
            pp = *p;
        } else {
            str[i] = *pp++;
        }
    }

    /* All done */

    return str;
}


/*
 * Return the number of bytes consumed by an argv array.
 */
size_t prrte_argv_len(char **argv)
{
  char **p;
  size_t length;

  if (NULL == argv)
    return (size_t) 0;

  length = sizeof(char *);

  for (p = argv; *p; ++p) {
    length += strlen(*p) + 1 + sizeof(char *);
  }

  return length;
}


/*
 * Copy a NULL-terminated argv array.
 */
char **prrte_argv_copy(char **argv)
{
  char **dupv = NULL;
  int dupc = 0;

  if (NULL == argv)
    return NULL;

  /* create an "empty" list, so that we return something valid if we
     were passed a valid list with no contained elements */
  dupv = (char**) malloc(sizeof(char*));
  dupv[0] = NULL;

  while (NULL != *argv) {
    if (PRRTE_SUCCESS != prrte_argv_append(&dupc, &dupv, *argv)) {
      prrte_argv_free(dupv);
      return NULL;
    }

    ++argv;
  }

  /* All done */

  return dupv;
}


int prrte_argv_delete(int *argc, char ***argv, int start, int num_to_delete)
{
    int i;
    int count;
    int suffix_count;
    char **tmp;

    /* Check for the bozo cases */
    if (NULL == argv || NULL == *argv || 0 == num_to_delete) {
        return PRRTE_SUCCESS;
    }
    count = prrte_argv_count(*argv);
    if (start > count) {
        return PRRTE_SUCCESS;
    } else if (start < 0 || num_to_delete < 0) {
        return PRRTE_ERR_BAD_PARAM;
    }

    /* Ok, we have some tokens to delete.  Calculate the new length of
       the argv array. */

    suffix_count = count - (start + num_to_delete);
    if (suffix_count < 0) {
        suffix_count = 0;
    }

    /* Free all items that are being deleted */

    for (i = start; i < count && i < start + num_to_delete; ++i) {
        free((*argv)[i]);
    }

    /* Copy the suffix over the deleted items */

    for (i = start; i < start + suffix_count; ++i) {
        (*argv)[i] = (*argv)[i + num_to_delete];
    }

    /* Add the trailing NULL */

    (*argv)[i] = NULL;

    /* adjust the argv array */
    tmp = (char**)realloc(*argv, sizeof(char*) * (i + 1));
    if (NULL != tmp) *argv = tmp;

    /* adjust the argc */
    if (NULL != argc) {
        (*argc) = prrte_argv_count(*argv);
    }

    return PRRTE_SUCCESS;
}


int prrte_argv_insert(char ***target, int start, char **source)
{
    int i, source_count, target_count;
    int suffix_count;

    /* Check for the bozo cases */

    if (NULL == target || NULL == *target || start < 0) {
        return PRRTE_ERR_BAD_PARAM;
    } else if (NULL == source) {
        return PRRTE_SUCCESS;
    }

    /* Easy case: appending to the end */

    target_count = prrte_argv_count(*target);
    source_count = prrte_argv_count(source);
    if (start > target_count) {
        for (i = 0; i < source_count; ++i) {
            prrte_argv_append(&target_count, target, source[i]);
        }
    }

    /* Harder: insertting into the middle */

    else {

        /* Alloc out new space */

        *target = (char**) realloc(*target,
                                   sizeof(char *) * (target_count + source_count + 1));

        /* Move suffix items down to the end */

        suffix_count = target_count - start;
        for (i = suffix_count - 1; i >= 0; --i) {
            (*target)[start + source_count + i] =
                (*target)[start + i];
        }
        (*target)[start + suffix_count + source_count] = NULL;

        /* Strdup in the source argv */

        for (i = start; i < start + source_count; ++i) {
            (*target)[i] = strdup(source[i - start]);
        }
    }

    /* All done */

    return PRRTE_SUCCESS;
}

int prrte_argv_insert_element(char ***target, int location, char *source)
{
    int i, target_count;
    int suffix_count;

    /* Check for the bozo cases */

    if (NULL == target || NULL == *target || location < 0) {
        return PRRTE_ERR_BAD_PARAM;
    } else if (NULL == source) {
        return PRRTE_SUCCESS;
    }

    /* Easy case: appending to the end */
    target_count = prrte_argv_count(*target);
    if (location > target_count) {
        prrte_argv_append(&target_count, target, source);
        return PRRTE_SUCCESS;
    }

    /* Alloc out new space */
    *target = (char**) realloc(*target,
                               sizeof(char*) * (target_count + 2));

    /* Move suffix items down to the end */
    suffix_count = target_count - location;
    for (i = suffix_count - 1; i >= 0; --i) {
        (*target)[location + 1 + i] =
        (*target)[location + i];
    }
    (*target)[location + suffix_count + 1] = NULL;

    /* Strdup in the source */
    (*target)[location] = strdup(source);

    /* All done */
    return PRRTE_SUCCESS;
}

// This is used to parse the argument in cases like
//   --map-by core:pe=2,PE-LIST=4-63
// Here the input arg is "core:pe=2,PE-LIST=4-63"
//
// The list_item might be "pe" in which case want the output
// to be *str = strdup of "2"
//
// The list_item_separators string specifies where the list
// begins, and what separates items.
//
// Also though I don't think it's allowed elsewhere, here we'll
// allow --map-by pe=2 for example, so if arg starts with the
// string being searched for, it will accept it without requiring
// that it be after an expected initial colon.
//
// returns 1 if it found the setting listed, 0 if it didn't

int
prrte_parse_arg_for_a_listed_setting(
    char *arg,
    char *list_item,
    char *list_item_separators,
    char **str)
{
    *str = NULL;
    if (!arg) { return 0; }
    if (!list_item) { return 0; }

    char start_char = list_item_separators[0];
    char separator_char = list_item_separators[1];

    // See if the list_item is at the very front of the string first
    int found = 0;
    char *p = arg;
    char *p2;
    if (0 == strncmp(p, list_item, strlen(list_item))) {
        p2 = p + strlen(list_item);
        if (*p2 == '\0' || *p2 == '=' || *p2 == separator_char) {
            found = 1;
        }
    }

    // if not yet found, advance p beyond the first start_char (probably :)
    if (!found) {
        while (*p && *p != start_char) { ++p; }
        if (*p) { ++p; } // past the :
    }

    // Now loop similarly to the first check
    while (!found && *p) {
        if (0 == strncmp(p, list_item, strlen(list_item))) {
            p2 = p + strlen(list_item);
            if (*p2 == '\0' || *p2 == '=' || *p2 == separator_char) {
                found = 1;
            }
        }
        // if not found, advance p beyond the next separator_char
        if (!found) {
            while (*p && *p != separator_char) { ++p; }
            if (*p) { ++p; } // past the ,
        }
    }

    if (!found) { return 0; }

    // At this point p would be on an item like
    //   pe=2                 from --map-by socket:pe=2
    //   PE-LIST=4-63,68-127  from --map-by hwthread:PE-LIST=4-63,68-127
    //   REPORT               from --bind-to hwthread:REPORT
    //
    // If there's an equal sign (before the next separator), then
    // the thing following it is the value we return
    // up through the null or a separator char.
    // If no equal, then the value we return is just "1"

    char *pval = strchr(p, '=');
    char *psep = strchr(p, separator_char);
    if (psep && pval && psep < pval) {
        // eg if we're looking for REPORT in --bind-to hwthread:REPORT,var=val
        // p is at REPORT:var=val
        // pval would find the "val"
        // psep would find the ",var=val"
        // and that would let us know the pval wasn't for this portion
        pval = NULL;
    }
    if (!pval) {
        // option without a val, like --bind-to hwthread:REPORT
        *str = strdup("1"); // this tells the caller to set MCA_something=1
    } else {
        ++pval; // the string after the '='
        if (!*pval) { return 0; }
        *str = strdup(pval);
        // put a null after the separator char, eg for a case like
        // --map-by hwthread:pe=2,otherstuff
        // we just strduped "2,otherstuff" so we're about to put a null where
        // the comma is.
        //
        // But hard-code an exception for PE-LIST where we skip past a
        // comma separated list of numbers first, then do the same thing
        p = *str;
        if (0 == strcmp(list_item, "PE-LIST")) {
            p += strspn(p, "0123456789-,");
        }
        while (*p && *p != separator_char) { ++p; }
        *p = '\0';
    }
    return 1;
}
