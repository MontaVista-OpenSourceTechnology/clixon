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
 * XML default values
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
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_string.h"
#include "clixon_handle.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_err.h"
#include "clixon_log.h"
#include "clixon_debug.h"
#include "clixon_xpath_ctx.h"
#include "clixon_xpath.h"
#include "clixon_data.h"
#include "clixon_xml_sort.h"
#include "clixon_xml_nsctx.h"
#include "clixon_xml_map.h"
#include "clixon_xml_default.h"

/* Forward */
static int xml_default(yang_stmt *yt, cxobj *xt, int state);

/*!
 */
static int
xml_default_create1(yang_stmt *y,
                    cxobj     *xt,
                    cxobj    **xcp)
{
    int        retval = -1;
    char      *namespace;
    char      *prefix;
    int        ret;
    cxobj     *xc = NULL;

    if ((xc = xml_new(yang_argument_get(y), NULL, CX_ELMNT)) == NULL)
        goto done;
    xml_spec_set(xc, y);
    /* assign right prefix */
    if ((namespace = yang_find_mynamespace(y)) != NULL){
        prefix = NULL;
        if ((ret = xml2prefix(xt, namespace, &prefix)) < 0)
            goto done;
        if (ret){ /* Namespace found, prefix returned in prefix */
            if (xml_prefix_set(xc, prefix) < 0)
                goto done;
        }
        else{ /* Namespace does not exist in target, must add it w xmlns attr.
                 use source prefix */
            if (xml_add_namespace(xc, xc, prefix, namespace) < 0)
                goto done;
            /* Add prefix to x, if any */
            if (prefix && xml_prefix_set(xc, prefix) < 0)
                goto done;
        }
    }
    if (xml_addsub(xt, xc) < 0)
        goto done;
    *xcp = xc;
    retval = 0;
 done:
    return retval;
}

/*! Create leaf from default value
 *
 * @param[in]   y       Yang spec
 * @param[in]   xt      XML tree
 * @param[in]   top     Use default namespace (if you create xmlns statement)
 * @retval      0       OK
 * @retval     -1       Error
 */
static int
xml_default_create(yang_stmt *y,
                   cxobj     *xt,
                   int        top)
{
    int        retval = -1;
    cxobj     *xc = NULL;
    cxobj     *xb;
    char      *str;
    cg_var    *cv;

    if (xml_default_create1(y, xt, &xc) < 0)
        goto done;
    xml_flag_set(xc, XML_FLAG_DEFAULT);
    if ((xb = xml_new("body", xc, CX_BODY)) == NULL)
        goto done;
    if ((cv = yang_cv_get(y)) == NULL){
        clixon_err(OE_UNIX, ENOENT, "No yang cv of %s", yang_argument_get(y));
        goto done;
    }
    if ((str = cv2str_dup(cv)) == NULL){
        clixon_err(OE_UNIX, errno, "cv2str_dup");
        goto done;
    }
    if (xml_value_set(xb, str) < 0)
        goto done;
    free(str);
    retval = 0;
 done:
    return retval;
}

/*! Traverse a choice
 *
 * From RFC7950 Sec 7.9.3
 * 1. Default case,  the default if no child nodes from any of the choice's cases exist
 * 2. Default for child nodes under a case are only used if one of the nodes under that case
 *   is present 
 */
static int
xml_default_choice(yang_stmt *yc,
                   cxobj     *xt,
                   int        state)
{
    int        retval = -1;
    cxobj     *x = NULL;
    cxobj     *x0 = NULL;
    yang_stmt *y;
    yang_stmt *ych = NULL;
    yang_stmt *yca = NULL;
    yang_stmt *ydef;

    clixon_debug(CLIXON_DBG_XML | CLIXON_DBG_DETAIL, "");
    /* 1. Is there a default case and no child under this choice?
     */
    x = NULL;
    while ((x = xml_child_each(xt, x, CX_ELMNT)) != NULL) {
        if ((y = xml_spec(x)) == NULL)
            continue;
        /* Check if this child is a child of yc */
        yca = ych = NULL;
        if (yang_choice_case_get(y, &yca, &ych) == 1 &&
            ych == yc){
            x0 = x;
            break;
        }
    }
    if (x0 == NULL){ /* case 1: no child nodes of any of the choice's cases */
        if ((ydef = yang_find(yc, Y_DEFAULT, NULL)) != NULL)
            yca = yang_find(yc, Y_CASE, yang_argument_get(ydef));
        else
            yca = NULL;
    }
    if (yca)
        if (xml_default(yca, xt, state) < 0)
            goto done;
    retval = 0;
 done:
    return retval;
}

/*! Try to see if intermediate nodes are necessary for default values, create if so
 *
 * @param[in]   yt      Yang container (no-presence)
 * @param[in]   state   Set if global state, otherwise config
 * @param[out]  createp Need to create XML container
 * @retval      0       OK
 * @retval     -1       Error
 */
static int
xml_nopresence_try(yang_stmt *yt,
                   int        state,
                   int       *createp)
{
    int        retval = -1;
    yang_stmt *y;
    yang_stmt *ydef;

    if (yt == NULL || yang_keyword_get(yt) != Y_CONTAINER){
        clixon_err(OE_XML, EINVAL, "yt argument is not container");
        goto done;
    }
    *createp = 0;
    y = NULL;
    while ((y = yn_each(yt, y)) != NULL) {
        switch (yang_keyword_get(y)){
        case Y_LEAF:
            /* Default value exists */
            if (!cv_flag(yang_cv_get(y), V_UNSET)){
                /* Want to add state defaults, but this is config */
                if (state && yang_config_ancestor(y))
                    ;
                else
                    /* Need to create container */
                    *createp = 1;
                goto ok;
            }
            break;
        case Y_CONTAINER:
            if (yang_find(y, Y_PRESENCE, NULL) == NULL){
                /* If this is non-presence, (and it does not exist in xt) call recursively 
                 * and create nodes if any default value exist first. Then continue and populate?
                 */
                if (xml_nopresence_try(y, state, createp) < 0)
                    goto done;
                if (*createp)
                    goto ok;
            }
            break;
        case Y_CHOICE:
            if ((ydef = yang_find(y, Y_DEFAULT, NULL)) != NULL &&
                yang_find(y, Y_CASE, yang_argument_get(ydef)))
                 *createp = 1;
            break;
        default:
            break;
        } /* switch */
    }
 ok:
    retval = 0;
 done:
    return retval;
}

/*! Ensure default values are set on (children of) one single xml node
 *
 * Not recursive, except in one case with one or several non-presence containers, in which case
 * XML containers may be created to host default values. That code may be a little too recursive.
 * @param[in]   yt      Yang spec, usually spec of xt but always (eg Y_CASE)
 * @param[in]   xt      XML tree (with yt as spec of xt, informally)
 * @param[in]   state   Set if global state, otherwise config
 * @retval      0       OK
 * @retval     -1       Error
 * XXX If state, should not add config defaults             
 *      if (state && yang_config(yc)) 
 */
static int
xml_default(yang_stmt *yt,
            cxobj     *xt,
            int        state)
{
    int        retval = -1;
    yang_stmt *yc;
    cxobj     *xc;
    int        top = 0; /* Top symbol (set default namespace) */
    int        create = 0;
    char      *xpath;
    int        nr = 0;
    int        hit = 0;
    cg_var    *cv;

    if (xt == NULL){ /* No xml */
        clixon_err(OE_XML, EINVAL, "No XML argument");
        goto done;
    }
    switch (yang_keyword_get(yt)){
    case Y_MODULE:
    case Y_SUBMODULE:
        top++;
    case Y_CONTAINER: /* XXX maybe check for non-presence here as well */
    case Y_LIST:
    case Y_INPUT:
    case Y_OUTPUT:
    case Y_CASE:
        yc = NULL;
        while ((yc = yn_each(yt, yc)) != NULL) {
            // XXX consider only data nodes for optimization?
            /* If config parameter and local is config false */
            if (!state && !yang_config(yc))
                continue;
            /* Want to add state defaults, but this is config */
            if (state && yang_config_ancestor(yc))
                continue;
            switch (yang_keyword_get(yc)){
            case Y_LEAF:
                if ((cv = yang_cv_get(yc)) == NULL){
                    clixon_err(OE_YANG,0, "Internal error: yang leaf %s not populated with cv as it should",
                               yang_argument_get(yc));
                    goto done;
                }
                if (!cv_flag(cv, V_UNSET)){  /* Default value exists */
                    /* Check when condition */
                    if (yang_check_when_xpath(NULL, xt, yc, &hit, &nr, &xpath) < 0)
                        goto done;
                    if (hit && nr == 0)
                        break; /* Do not create default if xpath fails */
                    if (xml_find_type(xt, NULL, yang_argument_get(yc), CX_ELMNT) == NULL){
                        /* No such child exist, create this leaf */
                        if (xml_default_create(yc, xt, top) < 0)
                            goto done;
                        xml_sort(xt);
                    }
                }
                break;
            case Y_CONTAINER:
                if (yang_find(yc, Y_PRESENCE, NULL) == NULL){
                    /* Check when condition */
                    if (yang_check_when_xpath(NULL, xt, yc, &hit, &nr, &xpath) < 0)
                        goto done;
                    if (hit && nr == 0)
                        break; /* Do not create default if xpath fails */
                    /* If this is non-presence, (and it does not exist in xt) call 
                     * recursively and create nodes if any default value exist first. 
                     * Then continue and populate?
                     * Also this code expands some "when" statements that have nothing to do with 
                     * defaults.
                     */
                    if (xml_find_type(xt, NULL, yang_argument_get(yc), CX_ELMNT) == NULL){
                        /* No such container exist, recursively try if needed */
                        if (xml_nopresence_try(yc, state, &create) < 0)
                            goto done;
                        if (create){
                            /* Retval shows there is a default value need to create the
                             * container */
                            if (xml_default_create1(yc, xt, &xc) < 0)
                                goto done;
                            xml_sort(xt);
                            /* Then call it recursively */
                            if (xml_default(yc, xc, state) < 0)
                                goto done;
                        }
                    }
                }
                break;
            case Y_CHOICE:{
                if (xml_default_choice(yc, xt, state) < 0)
                    goto done;
                break;
            }
            default:
                break;
            }
        }
        break;
    default:
        break;
    } /* switch */
    retval = 0;
 done:
    return retval;
}

/*! Selectively recursively fill in default values in an XML tree using flags
 *
 * Skip nodes that are not either CHANGE or "flag" (typically ADD|DEL)
 * When ADD is encountered process all children.
 * This will process all nodes that lead to ADD nodes and skip others.
 * @param[in]   xt      XML tree
 * @param[in]   state   If set expand defaults also for state data, otherwise only config
 * @param[in]   flag    If set only traverse nodes marked with flag (or CHANGE)
 * @retval      0       OK
 * @retval     -1       Error
 * @see xml_default_recurse
 */
int
xml_default_recurse(cxobj *xn,
                    int    state,
                    int    flag)
{
    int        retval = -1;
    yang_stmt *yn;
    cxobj     *x;
    yang_stmt *y;

    if (flag){
        if (xml_flag(xn, XML_FLAG_CHANGE) != 0)
            ; /* continue */
        else if (xml_flag(xn, flag) != 0){
            flag = 0x0; /* Pass all */
        }
        else
            goto skip;
    }
    if ((yn = (yang_stmt*)xml_spec(xn)) != NULL){
        if (xml_default(yn, xn, state) < 0)
            goto done;
    }
    x = NULL;
    while ((x = xml_child_each(xn, x, CX_ELMNT)) != NULL) {
        if ((y = (yang_stmt*)xml_spec(x)) != NULL){
            if (!state && !yang_config(y))
                continue;
        }
        if (xml_default_recurse(x, state, flag) < 0)
            goto done;
    }
 skip:
    retval = 0;
 done:
    return retval;
}

/*! Expand and set default values of global top-level on XML tree
 *
 * Not recursive, except in one case with one or several non-presence containers
 * @param[in]   xt      XML tree
 * @param[in]   yspec   Top-level YANG specification tree, all modules
 * @param[in]   state  p Set if global state, otherwise config
 * @retval      0       OK
 * @retval     -1       Error
 */
static int
xml_global_defaults_create(cxobj     *xt,
                           yang_stmt *yspec,
                           int        state)
{
    int        retval = -1;
    yang_stmt *ymod = NULL;

    if (yspec == NULL || yang_keyword_get(yspec) != Y_SPEC){
        clixon_err(OE_XML, EINVAL, "yspec argument is not yang spec");
        goto done;
    }
    while ((ymod = yn_each(yspec, ymod)) != NULL)
        if (xml_default(ymod, xt, state) < 0)
            goto done;
    retval = 0;
 done:
    return retval;
}

/*! Expand and set default values of global top-level on XML tree
 *
 * Not recursive, except in one case with one or several non-presence containers
 * @param[in]   h       Clixon handle
 * @param[in]   xt      XML tree, assume already filtered with xpath
 * @param[in]   xpath   Filter global defaults with this and merge with xt
 * @param[in]   yspec   Top-level YANG specification tree, all modules
 * @param[in]   state   Set if global state, otherwise config
 * @param[in]   flags   Only traverse nodes where flag is set
 * @retval      0       OK
 * @retval     -1       Error
 * Uses cache?
 * @see xml_default_recurse
 */
int
xml_global_defaults(clixon_handle h,
                    cxobj        *xt,
                    cvec         *nsc,
                    const char   *xpath,
                    yang_stmt    *yspec,
                    int           state)
{
    int        retval = -1;
    db_elmnt   de0 = {0,};
    db_elmnt  *de = NULL;
    cxobj     *xcache = NULL;
    cxobj     *xpart = NULL;
    cxobj    **xvec = NULL;
    size_t     xlen;
    int        i;
    cxobj     *x0;
    int        ret;
    char      *key;

    /* Use different keys for config and state */
    key = state ? "global-defaults-state" : "global-defaults-config";
    /* First get or compute global xml tree cache */
    if ((de = clicon_db_elmnt_get(h, key)) == NULL){
        /* Create it */
        if ((xcache = xml_new(DATASTORE_TOP_SYMBOL, NULL, CX_ELMNT)) == NULL)
            goto done;
        if (xml_global_defaults_create(xcache, yspec, state) < 0)
            goto done;
        de0.de_xml = xcache;
        clicon_db_elmnt_set(h, key, &de0);
    }
    else
        xcache = de->de_xml;

    /* Here xcache has all global defaults. Now find the matching nodes 
     * XXX: nsc as 2nd argument
     */
    if (xpath_vec(xcache, nsc, "%s", &xvec, &xlen, xpath?xpath:"/") < 0)
        goto done;
    /* Iterate through match vector
     * For every node found in x0, mark the tree up to t1
     */
    for (i=0; i<xlen; i++){
        x0 = xvec[i];
        xml_flag_set(x0, XML_FLAG_MARK);
        xml_apply_ancestor(x0, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_CHANGE);
    }
    /* Create a new tree and copy over the parts from the cache that matches xpath */
    if ((xpart = xml_new(DATASTORE_TOP_SYMBOL, NULL, CX_ELMNT)) == NULL)
        goto done;
    if (xml_copy_marked(xcache, xpart) < 0) /* config */
        goto done;
    if (xml_apply(xcache, CX_ELMNT, (xml_applyfn_t*)xml_flag_reset, (void*)(XML_FLAG_MARK|XML_FLAG_CHANGE)) < 0)
        goto done;
    if (xml_apply(xpart, CX_ELMNT, (xml_applyfn_t*)xml_flag_reset, (void*)(XML_FLAG_MARK|XML_FLAG_CHANGE)) < 0)
        goto done;
    /* Merge global pruned tree with xt */
    if ((ret = xml_merge(xt, xpart, yspec, NULL)) < 1) /* XXX reason */
        goto done;
    retval = 0;
 done:
    if (xpart)
        xml_free(xpart);
    if (xvec)
        free(xvec);
    return retval;
}

/*! Recursively find empty nopresence containers and default leaves, optionally purge
 *
 * Semantics of mode parameter somewhat complex
 * @param[in] xn       XML tree
 * @param[in] mode     0: Dont remove any nodes
 *                     1: Remove config sub-nodes that are empty non-presence container or default leaf
 *                     2: Remove all sub-nodes that are empty non-presence container or default leaf
 *                     3: Remove all sub-nodes that are empty non-presence containers
 * @param[in] flag     If set only traverse nodes marked with flag (or CHANGE)
 * @retval    1        Node is an (recursive) empty non-presence container or default leaf
 * @retval    0        Other node
 * @retval   -1        Error
 * @note xn is not itself removed if mode
 * @note for mode=1 are removed only if config or no yang spec(!)
 */
int
xml_default_nopresence(cxobj *xn,
                       int    mode,
                       int    flag)
{
    int           retval = -1;
    cxobj        *x;
    cxobj        *xprev;
    yang_stmt    *yn;
    yang_stmt    *y;
    int           rmx = 0; /* If set, remove this xn */
    int           ret;
    enum rfc_6020 keyw;
    int           config = 1;

    if (flag){
        if (xml_flag(xn, XML_FLAG_CHANGE) != 0)
            ; /* continue */
        else if (xml_flag(xn, flag) != 0){
            flag = 0x0; /* Pass all */
        }
        else{
            retval = 0;
            goto done;
        }
    }
    if ((yn = xml_spec(xn)) != NULL){
        keyw = yang_keyword_get(yn);
        if (keyw == Y_CONTAINER &&
            yang_find(yn, Y_PRESENCE, NULL) == NULL)
            rmx = 1;
        else if (keyw == Y_LEAF &&
                 xml_flag(xn, XML_FLAG_DEFAULT) &&
                 mode != 3)
            rmx = 1;
        config = yang_config_ancestor(yn);
    }
    /* Loop thru children */
    x = NULL;
    xprev = NULL;
    while ((x = xml_child_each(xn, x, CX_ELMNT)) != NULL) {
        /* 1: node is empty non-presence or default leaf (eg rmx) */
        if ((ret = xml_default_nopresence(x, mode, flag)) < 0)
            goto done;
        if (ret == 1){
            switch (mode){
            case 1: /* config nodes only */
                if (!config)
                    break;
                if ((y = xml_spec(x)) != NULL &&
                    !yang_config(y))
                    break;
                /* fall thru */
            case 2: /* purge all nodes */
            case 3:
                if (xml_purge(x) < 0)
                    goto done;
                x = xprev;
                break;
            default:
                break;
            }
        }
        else if (rmx)
            /* May switch an empty non-presence container (rmx=1) to non-empty non-presence container (rmx=0) */
            rmx = 0;
    }
    retval = rmx;
 done:
    return retval;
}

/*! Add default attribute to node with default value.
 *
 * Used in with-default code for report-all-tagged
 * @param[in]   x       XML node
 * @param[in]   flags   Flags indicatiing default nodes
 * @retval      0       OK
 * @retval     -1       Error
 */
int
xml_add_default_tag(cxobj *x,
                    uint16_t flags)
{
    int retval = -1;

    if (xml_flag(x, flags)) {
        /* set default attribute */
        if (xml_add_attr(x, "default", "true", IETF_NETCONF_WITH_DEFAULTS_ATTR_PREFIX, NULL) == NULL)
            goto done;
    }
    retval = 0;
 done:
    return retval;
}

/*! Set flag on node having schema default value. (non-config)
 *
 * Used in with-default code for trim/report-all-tagged
 * @param[in]   x       XML node
 * @param[in]   flag    Flag to be used
 * @retval      0       OK
 * @see xml_flag_default_value for config default value
 */
int
xml_flag_state_default_value(cxobj   *x,
                             uint16_t flag)
{
    yang_stmt   *y;
    cg_var      *cv;
    char        *yv;
    char        *xv;

    xml_flag_reset(x, flag); /* Assume not default value */
    if ((xv = xml_body(x)) == NULL)
        goto done;
    if ((y = xml_spec(x)) == NULL)
        goto done;
    if (yang_config_ancestor(y) == 1)
        goto done;
    if ((cv = yang_cv_get(y)) == NULL)
        goto done;
    if (cv_name_get(cv) == NULL)
        goto done;
    if ((yv = cv2str_dup(cv)) == NULL)
        goto done;
    if (strcmp(xv, yv) == 0)
        xml_flag_set(x, flag); /* Actual value same as default value */
    free(yv);
  done:
    return 0;
}

/*! Set flag on node having schema default value. (config)
 *
 * Used in with-default code for trim and report-all-tagged
 * @param[in]   x       XML node
 * @param[in]   flag    Flag to be used
 * @retval      0       OK
 * @see xml_flag_state_default_value for non-config default value
 */
int
xml_flag_default_value(cxobj   *x,
                       uint16_t flag)
{
    yang_stmt   *y;
    cg_var      *cv;
    char        *yv;
    char        *xv;

    xml_flag_reset(x, flag); /* Assume not default value */
    if ((xv = xml_body(x)) == NULL)
        goto done;
    if ((y = xml_spec(x)) == NULL)
        goto done;
    if ((cv = yang_cv_get(y)) == NULL)
        goto done;
    if ((cv = yang_cv_get(y)) == NULL)
        goto done;
    if (cv_name_get(cv) == NULL)
        goto done;
    if ((yv = cv2str_dup(cv)) == NULL)
        goto done;
    if (strcmp(xv, yv) == 0)
        xml_flag_set(x, flag); /* Actual value same as default value */
    free(yv);
  done:
    return 0;
}
