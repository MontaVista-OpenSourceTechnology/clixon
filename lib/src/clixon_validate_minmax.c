/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren
  Copyright (C) 2017-2019 Olof Hagsand
  Copyright (C) 2020-2022 Olof Hagsand and Rubicon Communications, LLC(Netgate)

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, 
  indicate your decision by deleting the provisions above and replace them with
  the  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****

 *
 * Check YANG validation for min/max-elements and unique
 */
#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <syslog.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/param.h>
#include <netinet/in.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon_map.h"
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_string.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_err.h"
#include "clixon_log.h"
#include "clixon_debug.h"
#include "clixon_data.h"
#include "clixon_netconf_lib.h"
#include "clixon_options.h"
#include "clixon_xml_nsctx.h"
#include "clixon_xml_io.h"
#include "clixon_xpath_ctx.h"
#include "clixon_xpath.h"
#include "clixon_yang_module.h"
#include "clixon_yang_type.h"
#include "clixon_xml_map.h"
#include "clixon_xml_bind.h"
#include "clixon_validate_minmax.h"

/*! New element last in list, check if already exists if so return -1
 *
 * @param[in]  vec   Vector of existing entries (new is last)
 * @param[in]  i1    The new entry is placed at vec[i1]
 * @param[in]  vlen  Length of entry
 * @param[in]  sorted Sorted by system, ie sorted by key, otherwise no assumption
 * @retval     1     Validation OK
 * @retval     0     Validation failed (cbret set)
 * @retval    -1     Error
 * @note This is currently quadratic complexity. It could be improved by inserting new element sorted and binary search.
 */
static int
unique_search_xpath(cxobj  *x,
                    char   *xpath,
                    cvec   *nsc,
                    char ***svec,
                    size_t *slen)
{
    int     retval = -1;
    cxobj **xvec = NULL;
    size_t  xveclen;
    int     i;
    int     s;
    cxobj  *xi;
    char   *bi;

    /* Collect tuples */
    if (xpath_vec(x, nsc, "%s", &xvec, &xveclen, xpath) < 0)
        goto done;
    for (i=0; i<xveclen; i++){
        xi = xvec[i];
        if ((bi = xml_body(xi)) == NULL)
            break;
        /* Check if bi is duplicate? 
         * XXX: sort svec?
         */
        for (s=0; s<(*slen); s++){
            if (strcmp(bi, (*svec)[s]) == 0){
                goto fail;
            }
        }
        (*slen) ++;
        if (((*svec) = realloc((*svec), (*slen)*sizeof(char*))) == NULL){
            clixon_err(OE_UNIX, errno, "realloc");
            goto done;
        }
        (*svec)[(*slen)-1] = bi;
    } /* i search results */
    retval = 1;
 done:
    if (xvec)
        free(xvec);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! New element last in list, return error if already exists 
 *
 * @param[in]  vec    Vector of existing entries (new is last)
 * @param[in]  i1     The new entry is placed at vec[i1]
 * @param[in]  vlen   Length of vec
 * @param[in]  sorted Sorted by system, ie sorted by key, otherwise no assumption
 * @param[out] dupl   Index of duplicated element (if retval = -1)
 * @retval     0      OK, entry is unique
 * @retval    -1      Duplicate detected
 * @note This is currently quadratic complexity. It could be improved by inserting new element sorted and binary search.
 */
static int
check_insert_duplicate(char **vec,
                       int    i1,
                       int    vlen,
                       int    sorted,
                       int   *dupl)
{
    int i;
    int v;
    char *b;

    if (sorted){
        /* Just go look at previous element to see if it is duplicate (sorted by system) */
        if (i1 == 0)
            return 0;
        i = i1-1;
        for (v=0; v<vlen; v++){
            b = vec[i*vlen+v];
            if (b == NULL || strcmp(b, vec[i1*vlen+v]))
                return 0;
        }
        /* here we have passed thru all keys of previous element and they are all equal */
        if (dupl)
            *dupl = i;
        return -1;
    }
    else{
        for (i=0; i<i1; i++){
            for (v=0; v<vlen; v++){
                b = vec[i*vlen+v];
                if (b == NULL || strcmp(b, vec[i1*vlen+v]))
                    break;
            }
            if (v==vlen) /* duplicate */
                break;
        }
        if (i<i1){
            if (dupl)
                *dupl = i;
            return -1;
        }
        else
            return 0;
    }
}

/*! Given a list with unique constraint, detect duplicates
 *
 * @param[in]  x     The first element in the list (on return the last)
 * @param[in]  xt    The parent of x (a list)
 * @param[in]  y     Its yang spec (Y_LIST)
 * @param[in]  yu    A yang unique (Y_UNIQUE) for unique schema node ids or (Y_LIST) for list keys
 * @param[in]  mark  If set, a flag mask to mark (prior) duplicate object with
 * @param[out] xret  Error XML tree. Free with xml_free after use
 * @retval     1     Validation OK
 * @retval     0     Validation failed (cbret set)
 * @retval    -1     Error
 * Discussion: the RFC 7950 Sec 7.8.3: "constraints on valid list entries"
 * The arguments are "descendant schema node identifiers". A direct interpretation is that
 * this is for "direct" descendants, but it does not rule out transient descendants.
 * The implementation supports two variants:
 * 1) list of direct descendants, eg "a b"
 * 2) single transient schema node identifier, eg "a/b"
 * The problem with combining (1) and (2) is that (2) results in a potential set of results, what
 * would unique "a/b c/d" mean if both a/b and c/d returns a set?
 * For (1):
 * All key leafs MUST be present for all list entries.
 * The combined values of all the leafs specified in the key are used to
 * uniquely identify a list entry.  All key leafs MUST be given values
 * when a list entry is created.
 */
static int
check_unique_list_direct(cxobj     *x,
                         cxobj     *xt,
                         yang_stmt *y,
                         yang_stmt *yu,
                         int        mark,
                         cxobj    **xret)
{
    int       retval = -1;
    cg_var   *cvi; /* unique node name */
    cxobj    *xi;
    char    **vec = NULL; /* 2xmatrix */
    cxobj   **xvec = NULL;
    int       clen;
    int       i;
    int       v;
    char     *bi;
    int       sorted;
    char     *str;
    cvec     *cvk;
    int       dupl;

    /* If list and is sorted by system, then it is assumed elements are in key-order which is optimized
     * Other cases are "unique" constraint or list sorted by user which is quadratic in complexity
     * This second case COULD be optimized if binary insert is made on the vec vector.
     */
    sorted = (yang_keyword_get(yu) == Y_LIST &&
              yang_find(y, Y_ORDERED_BY, "user") == NULL);
    cvk = yang_cvec_get(yu);
    /* nr of unique elements to check */
    if ((clen = cvec_len(cvk)) == 0){
        /* No keys: no checks necessary */
        goto ok;
    }
    /* x need not be child 0, which could make the vector larger than necessary */
    if ((vec = calloc(clen*xml_child_nr(xt), sizeof(char*))) == NULL){
        clixon_err(OE_UNIX, errno, "calloc");
        goto done;
    }
    if ((xvec = calloc(xml_child_nr(xt), sizeof(cxobj*))) == NULL){
        clixon_err(OE_UNIX, errno, "calloc");
        goto done;
    }
    /* A vector is built with key-values, for each iteration check "backward" in the vector
     * for duplicates
     */
    i = 0; /* x element index */
    do {
        xvec[i] = x;
        cvi = NULL;
        v = 0; /* index in each tuple */
        /* XXX Quadratic if clen > 1 */
        while ((cvi = cvec_each(cvk, cvi)) != NULL){
            /* RFC7950: Sec 7.8.3.1: entries that do not have value for all
             * referenced leafs are not taken into account */
            str = cv_string_get(cvi);
            if (index(str, '/') != NULL){
                clixon_err(OE_YANG, 0, "Multiple descendant nodes not allowed (w /)");
                goto done;
            }
            if ((xi = xml_find(x, str)) == NULL)
                break;
            if ((bi = xml_body(xi)) == NULL)
                break;
            vec[i*clen + v++] = bi;
        }
        if (cvi==NULL){
            /* Last element (i) is newly inserted, see if it is already there */
            if (check_insert_duplicate(vec, i, clen, sorted, &dupl) < 0){
                if (mark)
                    xml_flag_set(xvec[dupl], mark);
                if (xret && netconf_data_not_unique_xml(xret, x, cvk) < 0)
                    goto done;
                goto fail;
            }
        }
        x = xml_child_each(xt, x, CX_ELMNT);
        i++;
    } while (x && y == xml_spec(x));  /* stop if list ends, others may follow */
 ok:
    /* It would be possible to cache vec here as an optimization */
    retval = 1;
 done:
    if (xvec)
        free(xvec);
    if (vec)
        free(vec);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Given a list with unique constraint, detect duplicates
 *
 * @param[in]  x     The first element in the list (on return the last)
 * @param[in]  xt    The parent of x (a list)
 * @param[in]  y     Its yang spec (Y_LIST)
 * @param[in]  yu    A yang unique (Y_UNIQUE) for unique schema node ids or (Y_LIST) for list keys
 * @param[in]  mark  If set, a flag mask to mark (prior) duplicate object with
 * @param[out] xret  Error XML tree. Free with xml_free after use
 * @retval     1     Validation OK
 * @retval     0     Validation failed (xret set)
 * @retval    -1     Error
 * Discussion: the RFC 7950 Sec 7.8.3: "constraints on valid list entries"
 * The arguments are "descendant schema node identifiers". A direct interpretation is that
 * this is for "direct" descendants, but it does not rule out transient descendants.
 * The implementation supports two variants:
 * 1) list of direct descendants, eg "a b"
 * 2) single transient schema node identifier, eg "a/b"
 * The problem with combining (1) and (2) is that (2) results in a potential set of results, what
 * would unique "a/b c/d" mean if both a/b and c/d returns a set?
 * For (1):
 * All key leafs MUST be present for all list entries.
 * The combined values of all the leafs specified in the key are used to
 * uniquely identify a list entry.  All key leafs MUST be given values
 * when a list entry is created.
 */
static int
check_unique_list(cxobj     *x,
                  cxobj     *xt,
                  yang_stmt *y,
                  yang_stmt *yu,
                  uint16_t   mark,
                  cxobj    **xret)
{
    int       retval = -1;
    cg_var    *cvi; /* unique node name */
    char     **svec = NULL; /* vector of search results */
    size_t     slen = 0;
    char      *xpath0 = NULL;
    char      *xpath1 = NULL;
    int        ret;
    cvec      *cvk;
    cvec      *nsc0 = NULL;
    cvec      *nsc1 = NULL;

    /* Check if multiple direct children */
    cvk = yang_cvec_get(yu);
    if (cvec_len(cvk) > 1){
        retval = check_unique_list_direct(x, xt, y, yu, mark, xret);
        goto done;
    }
    cvi = cvec_i(cvk, 0);
    if (cvi == NULL || (xpath0 = cv_string_get(cvi)) == NULL){
        clixon_err(OE_YANG, 0, "No descendant schemanode");
        goto done;
    }
    /* Check if direct schmeanode-id , ie not xpath */
    if (index(xpath0, '/') == NULL){
        retval = check_unique_list_direct(x, xt, y, yu, mark, xret);
        goto done;
    }
    /* Here proper xpath with at least one slash (can there be a descendant schemanodeid w/o slash?) */
    if (xml_nsctx_yang(yu, &nsc0) < 0)
        goto done;
    if ((ret = xpath2canonical(xpath0, nsc0, ys_spec(y),
                               &xpath1, &nsc1, NULL)) < 0)
        goto done;
    if (ret == 0)
        goto fail; // XXX set xret
    do {
        /* Collect search results from one */
        if ((ret = unique_search_xpath(x, xpath1, nsc1, &svec, &slen)) < 0)
            goto done;
        if (ret == 0){
            if (xret && netconf_data_not_unique_xml(xret, x, cvk) < 0)
                goto done;
            goto fail;
        }
        x = xml_child_each(xt, x, CX_ELMNT);
    } while (x && y == xml_spec(x));  /* stop if list ends, others may follow */
    // ok:
    /* It would be possible to cache vec here as an optimization */
    retval = 1;
 done:
    if (nsc0)
        cvec_free(nsc0);
    if (nsc1)
        cvec_free(nsc1);
    if (xpath1)
        free(xpath1);
    if (svec)
        free(svec);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Given a list or leaf-list, check if any min/max-elemants constraints apply
 *
 * @param[in]  xp    Parent of the xml list there are too few/many (for error)
 * @param[in]  y     Yang spec of the failing list
 * @param[in]  nr    Number of elements (like x) in the list
 * @param[out] xret  Error XML tree. Free with xml_free after use
 * @retval     1     Validation OK
 * @retval     0     Validation failed (cbret set)
 * @retval    -1     Error
 * @see RFC7950 7.7.5
 * @note No recurse for non-presence container is made, see eg xml_yang_minmax
 */
static int
check_minmax(cxobj     *xp,
             yang_stmt *y,
             int        nr,
             cxobj     **xret)
{
    int         retval = -1;
    yang_stmt  *ymin; /* yang min */
    yang_stmt  *ymax; /* yang max */
    cg_var     *cv;

    if ((ymin = yang_find(y, Y_MIN_ELEMENTS, NULL)) != NULL){
        cv = yang_cv_get(ymin);
        if (nr < cv_uint32_get(cv)){
            if (xret && netconf_minmax_elements_xml(xret, xp, yang_argument_get(y), 0) < 0)
                goto done;
            goto fail;
        }
    }
    if ((ymax = yang_find(y, Y_MAX_ELEMENTS, NULL)) != NULL){
        cv = yang_cv_get(ymax);
        if (cv_uint32_get(cv) > 0 && /* 0 means unbounded */
            nr > cv_uint32_get(cv)){
            if (xret && netconf_minmax_elements_xml(xret, xp, yang_argument_get(y), 1) < 0)
                goto done;
            goto fail;
        }
    }
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Check if there is any empty list (no x elements) and check min-elements
 *
 * @param[in]  xt    XML node
 * @param[in]  yt    YANG node
 * @param[out] xret  Error XML tree. Free with xml_free after use
 * @retval     1     Validation OK
 * @retval     0     Validation failed (xret set)
 * @retval    -1     Error
 * @note recurse for non-presence container 
 */
static int
check_empty_list_minmax(cxobj     *xt,
                        yang_stmt *ye,
                        cxobj    **xret)
{
    int        retval = -1;
    int        ret;
    yang_stmt *yprev = NULL;
    int        inext;

    if (yang_config(ye) == 1){
        if(yang_keyword_get(ye) == Y_CONTAINER &&
           yang_find(ye, Y_PRESENCE, NULL) == NULL){
            inext = 0;
            while ((yprev = yn_iter(ye, &inext)) != NULL) {
                if ((ret = check_empty_list_minmax(xt, yprev, xret)) < 0)
                    goto done;
                if (ret == 0)
                    goto fail;
            }
        }
        else if (yang_keyword_get(ye) == Y_LIST ||
                 yang_keyword_get(ye) == Y_LEAF_LIST){
            /* Check if the list length violates min/max */
            if ((ret = check_minmax(xt, ye, 0, xret)) < 0)
                goto done;
            if (ret == 0)
                goto fail;
        }
    }
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Check duplicates/unique in list
 *
 * @param[in]  x     XML LIST node
 * @param[in]  xt    XML parent
 * @param[in]  y     YANG of x
 * @param[in]  mark  If set, a flag mask to mark (prior) duplicate object with
 * @param[out] xret  Error as XML if ret = 0
 * @retval     1     Validation OK
 * @retval     0     Validation failed (xret set)
 * @retval    -1     Error
 */
static int
xml_yang_minmax_new_list(cxobj     *x,
                         cxobj     *xt,
                         yang_stmt *y,
                         int        mark,
                         cxobj    **xret)
{
    int        retval = -1;
    yang_stmt *yu;
    int        inext;
    int        ret;

    /* Here new (first element) of lists only
     * First check unique keys direct children
     */
    if ((ret = check_unique_list_direct(x, xt, y, y, mark, xret)) < 0)
        goto done;
    if (ret == 0)
        goto fail;
    /* Check if there is a unique constraint on the list
     */
    inext = 0;
    while ((yu = yn_iter(y, &inext)) != NULL) {
        if (yang_keyword_get(yu) != Y_UNIQUE)
            continue;
        /* Here is a list w unique constraints identified by:
         * its first element x, its yang spec y, its parent xt, and 
         * a unique yang spec yu,
         * Two cases: 
         * 1) multiple direct children (no prefixes), eg "a b"
         * 2) single xpath with canonical prefixes, eg "/ex:a/ex:b"
         */
        if ((ret = check_unique_list(x, xt, y, yu, 0, xret)) < 0)
            goto done;
        if (ret == 0)
            goto fail;
    }
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Check duplicates in leaf-list
 *
 * @param[in]  x0    XML LIST node
 * @param[in]  xt    XML parent
 * @param[in]  y0    YANG of x0
 * @param[in]  mark  If set, a flag mask to mark (prior) duplicate object with
 * @param[out] xret  Error as XML if ret = 0
 * @retval     1     Validation OK
 * @retval     0     Validation failed (xret set)
 * @retval    -1     Error
 * @note works for both ordered-by user and system. But worst case quadratic
 */
static int
xml_yang_minmax_new_leaf_list(cxobj     *x0,
                              cxobj     *xt,
                              yang_stmt *y0,
                              int        mark,
                              cxobj    **xret)
{
    int        retval = -1;
    cxobj     *xi;
    cxobj     *xj;
    char      *bi;
    char      *bj;
    cvec      *cvv = NULL;

    xi = x0;
    do {
        if ((bi = xml_body(xi)) == NULL)
            continue;
        xj = xi;
        while ((xj = xml_child_each(xt, xj, CX_ELMNT)) != NULL &&
               xml_spec(xj) == y0) {
            if ((bj = xml_body(xj)) == NULL)
                continue;
            if (bi && bj && strcmp(bi, bj) == 0){
                if (mark)
                    xml_flag_set(xi, mark);
                if ((cvv = cvec_new(0)) == NULL){
                    clixon_err(OE_UNIX, errno, "cvec_new");
                    goto done;
                }
                cvec_add_string(cvv, "name", bi);
                if (xret && netconf_data_not_unique_xml(xret, xi, cvv) < 0)
                    goto done;
                goto fail;
            }
        }
    }
    while ((xi = xml_child_each(xt, xi, CX_ELMNT)) != NULL &&
           xml_spec(xi) == y0);
    retval = 1;
 done:
    if (cvv)
        cvec_free(cvv);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Perform gap analysis in a child-vector interval [ye,y]
 *
 * Gap analysis here meaning if there is a list x with min-element constraint but there are no
 * x elements in an interval of the children of xt. 
 * For example, assume the yang of xt is yt and is defined as:
 *   yt {
 *     list a;
 *     list x{ min-elements 1;}; // potential gap
 *     list b;
 *   }
 * Further assume that xt is:
 *   <xt><a><a><b><b></xt>
 * Then a call to this function could be ye=a, y=b. 
 * By iterating over the children of yt in the interval [a,b] it will find "x" with a min-element 
 * constraint.
 */
static int
xml_yang_minmax_gap_analysis(cxobj      *xt,
                             yang_stmt  *y,
                             yang_stmt  *yt,
                             int        *inext,
                             yang_stmt **yep,
                             cxobj     **xret)
{
    int        retval = -1;
    yang_stmt *ye;
    int        ret;
    yang_stmt *ych = NULL;

    ye = *yep;
    if (y && (ych = yang_choice(y)) == NULL)
        ych = y;
    /* Gap analysis: Check if there is any empty list between y and yprevlist
     * Note, does not detect empty choice list (too complicated)
     */
    if (yt != NULL && ych != ye){
        /* Skip analysis if Yang spec is unknown OR
         * if we are still iterating the same Y_CASE w multiple lists
         */
        ye = yn_iter(yt, inext);
        if (ye && ych != ye)
            do {
                if ((ret = check_empty_list_minmax(xt, ye, xret)) < 0)
                    goto done;
                if (ret == 0)
                    goto fail;
                ye = yn_iter(yt, inext);
            } while(ye != NULL && /* to avoid livelock (shouldnt happen) */
                    ye != ych);
    }
    *yep = ye;
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! YANG Minmax check, no recursion
 *
 * Assume xt:s children are sorted and yang populated.
 * The function does two different things of the children of an XML node:
 * (1) Check min/max element constraints
 * (2) Check unique constraints
 *
 * The routine uses a node traversing mechanism as the following example, where
 * two lists [x1,..] and [x2,..] are embedded:
 *   xt:  {a, b, [x1, x1, x1], d, e, f, [x2, x2, x2], g}
 * The function does this using a single iteration and uses the fact that the
 * xml symbols share yang symbols: ie [x1..] has yang y1 and d has yd.
 *
 * Unique constraints:
 * Lists are identified, then check_unique_list is called on each list.
 * Example, x has an associated yang list node with list of unique constraints
 *         y-list->y-unique - "a"
 *      xt->x ->  ab
 *          x ->  bc
 *          x ->  ab
 *
 * Min-max constraints: 
 * Find upper and lower bound of existing lists and report violations
 * Somewhat tricky to find violation of min-elements of empty
 * lists, but this is done by a "gap-detection" mechanism, which detects
 * gaps in the xml nodes given the ancestor Yang structure. 
 * But no gap analysis is done if the yang spec of the top-level xml is unknown
 * Example: 
 *   Yang structure: y1, y2, y3,
 *   XML structure: [x1, x1], [x3, x3] where [x2] list is missing
 * @note min-element constraints on empty lists are not detected on top-level.
 * Or more specifically, if no yang spec if associated with the top-level
 * XML node. This may not be a large problem since it would mean empty configs
 * are not allowed.
 * RFC 7950 7.7.5: regarding min-max elements check
 * The behavior of the constraint depends on the type of the 
 * leaf-list's or list's closest ancestor node in the schema tree 
 * that is not a non-presence container (see Section 7.5.1):
 * o If no such ancestor exists in the schema tree, the constraint
 * is enforced.
 * o Otherwise, if this ancestor is a case node, the constraint is
 * enforced if any other node from the case exists.
 * o  Otherwise, it is enforced if the ancestor node exists.
 *
 * Special handling: y / yprev
 * nr  yprev            y       eq?   action
 *-------------------------------------
 * 1   list             list    eq    nr++
 * 2   list             list    neq   gap analysis; yprev: check-minmax; new: list key check
 * 3   null             list    neq   gap analysis, new: list key check
 * 4   nolist           list    neq   gap analysis, new: list key check
 * 5   nolist           nolist  eq    error
 * 6   nolist           nolist  neq   gap analysis; nopresence-check; 
 * 7   null             nolist  neq   gap analysis; nopresence-check; 
 * 8   list             nolist  neq   gap analysis; yprev: check-minmax; nopresence-check; 
 * @param[in]  xt      XML parent (may have lists w unique constraints as child)
 * @param[in]  presence Set if called in a recursive loop (the caller will recurse anyway),
 *                     otherwise non-presence containers will be traversed
 * @param[out] xret    Error XML tree. Free with xml_free after use
 * @retval     1       Validation OK
 * @retval     0       Validation failed (xret set)
 * @retval    -1       Error
 * Note that many checks, ie all except gap analysis, may be unnecessary if this fn
 * is called in a recursive environment, since the recursion being made here will
 * be made in that environment anyway and thus leading to double checks.
 * @see xml_yang_validate_unique  Sunset of unique tests
 */
int
xml_yang_validate_minmax(cxobj  *xt,
                         int     presence,
                         cxobj **xret)
{
    int           retval = -1;
    cxobj        *x = NULL;
    yang_stmt    *y;
    yang_stmt    *yprev = NULL;
    yang_stmt    *ye = NULL;    /* yang each list to catch emtpy */
    enum rfc_6020 keyw;
    int           nr = 0;
    int           ret;
    yang_stmt    *yt;
    int           inext = 0;

    yt = xml_spec(xt); /* If yt == NULL, then no gap-analysis is done */
    while ((x = xml_child_each(xt, x, CX_ELMNT)) != NULL){
        if ((y = xml_spec(x)) == NULL)
            continue;
        keyw = yang_keyword_get(y);
        if (keyw == Y_LIST || keyw == Y_LEAF_LIST){
            /* equal: just continue*/
            if (y == yprev){
                nr++;
                continue;
            }
            /* gap analysis */
            if ((ret = xml_yang_minmax_gap_analysis(xt, y, yt, &inext, &ye, xret)) < 0)
                goto done;
            /* check-minmax of previous list */
            if (ret &&
                yprev &&
                (yang_keyword_get(yprev) == Y_LIST || yang_keyword_get(yprev) == Y_LEAF_LIST)){
                /* Check if the list length violates min/max */
                if ((ret = check_minmax(xt, yprev, nr, xret)) < 0)
                    goto done;
            }
            nr=1;
            /* new list check */
            if (ret){
                if (keyw == Y_LIST){
                    if ((ret = xml_yang_minmax_new_list(x, xt, y, 0, xret)) < 0)
                        goto done;
                }
#ifdef NOTYET /* XXX This is enforced in xml_yang_validate_unique instead */
                else if (keyw == Y_LEAF_LIST){
                    if ((ret = xml_yang_minmax_new_leaf_list(x, xt, y, xret)) < 0)
                        goto done;
                }
#endif
            }
            if (ret == 0)
                goto fail;
            yprev = y;
        }
        else{
            /* equal: error */
            if (y == yprev){
                /* Only lists and leaf-lists are allowed to be more than one */
                if (xret && netconf_minmax_elements_xml(xret, xml_parent(x), xml_name(x), 1) < 0)
                    goto done;
                goto fail;
            }
            /* gap analysis */
            if ((ret = xml_yang_minmax_gap_analysis(xt, y, yt, &inext, &ye, xret)) < 0)
                goto done;
            /* check-minmax of previous list */
            if (ret &&
                yprev &&
                (yang_keyword_get(yprev) == Y_LIST || yang_keyword_get(yprev) == Y_LEAF_LIST)){
                /* Check if the list length violates min/max */
                if ((ret = check_minmax(xt, yprev, nr, xret)) < 0)
                    goto done;
                nr = 0;
            }
            if (ret == 0)
                goto fail;
            if (presence && keyw == Y_CONTAINER &&
                yang_find(y, Y_PRESENCE, NULL) == NULL){
                if ((ret = xml_yang_validate_minmax(x, presence, xret)) < 0)
                    goto done;
                if (ret == 0)
                    goto fail;
            }
            yprev = y;
        }
    }
    /* After traversal checks;
       gap analysis */
#if 1
    /* Variant of gap analysis, does not use ych
     * XXX: try to unify with xml_yang_minmax_gap_analysis()
     */
    if ((ye = yn_iter(yt, &inext)) != NULL){
        do {
            if ((ret = check_empty_list_minmax(xt, ye, xret)) < 0)
                goto done;
            if (ret == 0)
                goto fail;
        } while ((ye = yn_iter(yt, &inext)) != NULL);
    }
    ret = 1;
#else
    if ((ret = xml_yang_minmax_gap_analysis(xt, NULL, yt, &inext, &ye, xret)) < 0)
        goto done;
#endif
    /* check-minmax of previous list */
    if (ret &&
        yprev &&
        (yang_keyword_get(yprev) == Y_LEAF || yang_keyword_get(yprev) == Y_LEAF_LIST)){
        /* Check if the list length violates min/max */
        if ((ret = check_minmax(xt, yprev, nr, xret)) < 0)
            goto done;
    }
    if (ret == 0)
        goto fail;
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Recursive minmax check
 *
 * Callback function type for xml_apply
 * @param[in]  x    XML node
 * @param[in]  arg  General-purpose argument
 * @retval     2    Locally abort this subtree, continue with others
 * @retval     1    Abort, dont continue with others, return 1 to end user
 * @retval     0    OK, continue
 * @retval    -1    Error, aborted at first error encounter, return -1 to end user
 */
static int
xml_yang_minmax_apply(cxobj *x,
                      void  *arg)
{
    int     ret;
    cxobj **xret = (cxobj **)arg;

    if ((ret = xml_yang_validate_minmax(x, 1, xret)) < 0)
        return -1;
     if (ret == 0){ /* Validation failed (xret set) */
        return 1; /* Abort dont continue */
    }
    return 0;
}

/*! Recursive YANG Minmax check
 *
 * @param[in]  xt      XML parent (may have lists w unique constraints as child)
 * @param[out] xret    Error XML tree. Free with xml_free after use
 * @retval     1       Validation OK
 * @retval     0       Validation failed (xret set)
 * @retval    -1       Error
 */
int
xml_yang_validate_minmax_recurse(cxobj  *xt,
                                 cxobj **xret)
{
    int retval = -1;
    int ret;

    if ((ret = xml_apply0(xt, CX_ELMNT, xml_yang_minmax_apply, xret)) < 0)
        goto done;
    if (ret == 1)
        goto fail;
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! YANG unique check, no recursion
 *
 * Assume xt:s children are sorted and yang populated.
 * @param[in]  xt      XML parent (may have lists w unique constraints as child)
 * @param[out] xret    Error XML tree. Free with xml_free after use
 * @retval     1       Validation OK
 * @retval     0       Validation failed (xret set)
 * @retval    -1       Error
 * @see xml_yang_validate_minmax  which include these unique tests
 */
int
xml_yang_validate_unique(cxobj  *xt,
                         cxobj **xret)
{
    int           retval = -1;
    cxobj        *x = NULL;
    yang_stmt    *y;
    yang_stmt    *yprev = NULL;
    enum rfc_6020 keyw;
    int           ret;

    while ((x = xml_child_each(xt, x, CX_ELMNT)) != NULL){
        if ((y = xml_spec(x)) == NULL)
            continue;
        keyw = yang_keyword_get(y);
        if (keyw == Y_LIST || keyw == Y_LEAF_LIST){
            /* equal: just continue*/
            if (y == yprev){
                continue;
            }
            /* new list check */
            switch (keyw){
            case Y_LIST:
                if ((ret = xml_yang_minmax_new_list(x, xt, y, 0, xret)) < 0)
                    goto done;
                if (ret == 0)
                    goto fail;
                break;
            case Y_LEAF_LIST:
                if ((ret = xml_yang_minmax_new_leaf_list(x, xt, y, 0, xret)) < 0)
                    goto done;
                if (ret == 0)
                    goto fail;
                break;
            default:
                break;
            }
            yprev = y;
        }
    }
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Recursive unique check
 *
 * Callback function type for xml_apply
 * @param[in]  x    XML node
 * @param[in]  arg  General-purpose argument
 * @retval     2    Locally abort this subtree, continue with others
 * @retval     1    Abort, dont continue with others, return 1 to end user
 * @retval     0    OK, continue
 * @retval    -1    Error, aborted at first error encounter, return -1 to end user
 */
static int
xml_yang_unique_apply(cxobj *x,
                      void  *arg)
{
    int     ret;
    cxobj **xret = (cxobj **)arg;

    if ((ret = xml_yang_validate_unique(x, xret)) < 0)
        return -1;
     if (ret == 0){ /* Validation failed (xret set) */
        return 1; /* Abort dont continue */
    }
    return 0;
}

/*! Recursive YANG unique check
 *
 * @param[in]  xt      XML parent (may have lists w unique constraints as child)
 * @param[out] xret    Error XML tree. Free with xml_free after use
 * @retval     1       Validation OK
 * @retval     0       Validation failed (xret set)
 * @retval    -1       Error
 */
int
xml_yang_validate_unique_recurse(cxobj  *xt,
                                 cxobj **xret)
{
    int retval = -1;
    int ret;

    if ((ret = xml_apply0(xt, CX_ELMNT, xml_yang_unique_apply, xret)) < 0)
        goto done;
    if (ret == 1)
        goto fail;
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! YANG unique check and remove duplicates, keep last
 *
 * Assume xt:s children are sorted and yang populated.
 * @param[in]  xt      XML parent (may have lists w unique constraints as child)
 * @param[out] xret    Error XML tree. Free with xml_free after use
 * @retval     2       Locally abort this subtree, continue with others
 * @retval     1       Abort, dont continue with others, return 1 to end user
 * @retval     0       OK, continue
 * @retval    -1       Error, aborted at first error encounter, return -1 to end user
 * @see xml_yang_validate_minmax  which include these unique tests
 */
static int
xml_duplicate_remove(cxobj *xt,
                     void  *arg)
{
    int           retval = -1;
    cxobj       **xret = (cxobj **)arg;
    cxobj        *x;
    yang_stmt    *y;
    yang_stmt    *yprev;
    enum rfc_6020 keyw;
    int           again;
    int           ret;

    again = 1;
    while (again){
        again = 0;
        yprev = NULL;
        x = NULL;
        while ((x = xml_child_each(xt, x, CX_ELMNT)) != NULL){
            if ((y = xml_spec(x)) == NULL)
                continue;
            keyw = yang_keyword_get(y);
            if (keyw == Y_LIST || keyw == Y_LEAF_LIST){
                if (y == yprev){                 /* equal: continue, assume list check does look-forward */
                    continue;
                }
                /* new list check */
                switch (keyw){
                case Y_LIST:
                    if ((ret = xml_yang_minmax_new_list(x, xt, y, XML_FLAG_DEL, xret)) < 0)
                        goto done;
                    if (ret == 0){
                        if (xml_tree_prune_flags1(xt, XML_FLAG_DEL, XML_FLAG_DEL, 0, &again) < 0)
                            goto done;
                        if (again){
                            if (xret && *xret){
                                xml_free(*xret);
                                *xret = NULL;
                            }
                            break;
                        }
                        goto fail;
                    }
                    break;
                case Y_LEAF_LIST:
                    if ((ret = xml_yang_minmax_new_leaf_list(x, xt, y, XML_FLAG_DEL, xret)) < 0)
                        goto done;
                    if (ret == 0){
                        if (xml_tree_prune_flags1(xt, XML_FLAG_DEL, XML_FLAG_DEL, 0, &again) < 0)
                            goto done;
                        if (again){
                            if (xret && *xret){
                                xml_free(*xret);
                                *xret = NULL;
                            }
                            break;
                        }
                        goto fail;
                    }
                    break;
                default:
                    break;
                }
                if (again)
                    break;
                yprev = y;
            }
        }
    }
    retval = 0;
 done:
    return retval;
 fail:
    retval = 1;
    goto done;
}

/*! Recursive YANG unique check and remove duplicates, keep last
 *
 * @param[in]  xt      XML parent (may have lists w unique constraints as child)
 * @param[out] xret    Error XML tree. Free with xml_free after use
 * @retval     1       Validation OK
 * @retval     0       Validation failed (xret set)
 * @retval    -1       Error
 * @see xml_yang_validate_unique_recurse
 */
int
xml_duplicate_remove_recurse(cxobj  *xt,
                             cxobj **xret)
{
    int retval = -1;
    int ret;

    if ((ret = xml_apply0(xt, CX_ELMNT, xml_duplicate_remove, xret)) < 0)
        goto done;
    if (ret == 1)
        goto fail;
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}
