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
  use your version of this file under the terms of Apache License version 2, indicate
  your decision by deleting the provisions above and replace them with the 
  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****

 *
 * Access functions for clixon data. 
 * Free-typed values for runtime getting and setting.
 *            Accessed with clicon_data(h).
 * @see clixon_option.[ch] for clixon options
 */
#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/socket.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon_map.h"
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_err.h"
#include "clixon_log.h"
#include "clixon_debug.h"
#include "clixon_xml_sort.h"
#include "clixon_yang_module.h"
#include "clixon_options.h"
#include "clixon_yang_module.h"
#include "clixon_plugin.h"
#include "clixon_xpath_ctx.h"
#include "clixon_xpath.h"
#include "clixon_data.h"

/*! Get generic clixon data on the form <name>=<val> where <val> is string
 *
 * @param[in]  h    Clixon handle
 * @param[in]  name Data name
 * @param[out] val  Data value as string
 * @retval     0    OK
 * @retval    -1    Not found (or error)
 * @see clicon_option_str
 */
int
clicon_data_get(clixon_handle h,
                const char   *name,
                char        **val)
{
    clicon_hash_t *cdat = clicon_data(h);

    if (clicon_hash_lookup(cdat, (char*)name) == NULL)
        return -1;
    if (val)
        *val = clicon_hash_value(cdat, (char*)name, NULL);
    return 0;
}

/*! Set generic clixon data on the form <name>=<val> where <val> is string
 *
 * @param[in]  h    Clixon handle
 * @param[in]  name Data name
 * @param[in]  val  Data value as null-terminated string (copied)
 * @retval     0    OK
 * @retval    -1    Error
 * @see clicon_option_str_set
 */
int
clicon_data_set(clixon_handle h,
                const char   *name,
                char         *val)
{
    clicon_hash_t  *cdat = clicon_data(h);

    return clicon_hash_add(cdat, (char*)name, val, strlen(val)+1)==NULL?-1:0;
}

/*! Delete generic clixon data
 *
 * @param[in]  h    Clixon handle
 * @param[in]  name Data name
 * @retval     0    OK
 * @retval    -1    Error
 * @see clicon_option_del
 */
int
clicon_data_del(clixon_handle h,
                const char   *name)
{
    clicon_hash_t  *cdat = clicon_data(h);

    return clicon_hash_del(cdat, (char*)name);
}

/*! Get generic clixon data on the form <name>=<ptr> where <ptr> is void*
 *
 * @param[in]  h    Clixon handle
 * @param[in]  name Data name
 * @param[out] ptr  Pointer
 * @retval     0    OK
 * @retval    -1    Not found (or error)
 * @see clicon_option_str
 */
int
clicon_ptr_get(clixon_handle h,
                const char  *name,
                void       **ptr)
{
    clicon_hash_t *cdat = clicon_data(h);
    void          *p;
    size_t         vlen;

    if (clicon_hash_lookup(cdat, (char*)name) == NULL)
        return -1;
    if (ptr){
        p = clicon_hash_value(cdat, (char*)name, &vlen);
        memcpy(ptr, p, vlen);
    }
    return 0;
}

/*! Set generic clixon data on the form <name>=<ptr> where <ptr> is void*
 *
 * @param[in]  h    Clixon handle
 * @param[in]  name Data name
 * @param[in]  ptr  Pointer
 * @retval     0    OK
 * @retval    -1    Error
 * @see clicon_option_str_set
 */
int
clicon_ptr_set(clixon_handle h,
               const char   *name,
               void         *ptr)
{
    clicon_hash_t  *cdat = clicon_data(h);

    return clicon_hash_add(cdat, (char*)name, &ptr, sizeof(ptr))==NULL?-1:0;
}

/*! Delete generic clixon data
 *
 * @param[in]  h    Clixon handle
 * @param[in]  name Data name
 * @retval     0    OK
 * @retval    -1    Error
 * @see clicon_option_del
 */
int
clicon_ptr_del(clixon_handle h,
               const char   *name)
{
    clicon_hash_t  *cdat = clicon_data(h);

    return clicon_hash_del(cdat, (char*)name);
}

/*! Get generic cligen variable vector (cvv) on the form <name>=<val> where <val> is cvv
 *
 * @param[in]  h     Clixon handle
 * @param[in]  name  Data name
 * @retval     cvv   Data value as cvv
 * @retval     NULL  Not found (or error)
 * @code
 *   cvec *cvv = NULL;
 *   if (clicon_data_cvec_get(h, "mycvv", &cvv) < 0)
 *     err;
 * @endcode                                     
 */
cvec *
clicon_data_cvec_get(clixon_handle h,
                     const char   *name)
{
    cvec *cvv = NULL;

    if (clicon_ptr_get(h, name, (void**)&cvv) < 0)
        return NULL;
    return cvv;
}

/*! Set generic cligen variable vector (cvv) on the form <name>=<val> where <val> is cvv
 *
 * @param[in]  h     Clixon handle
 * @param[in]  name  Name
 * @param[in]  cvv   CLIgen variable vector (cvv) (malloced)
 */
int
clicon_data_cvec_set(clixon_handle h,
                     const char   *name,
                     cvec         *cvv)
{
    cvec *cvv0 = NULL;

    clicon_ptr_get(h, name, (void**)&cvv0);
    if (cvv0)
        cvec_free(cvv0);
    return clicon_ptr_set(h, name, cvv);
}

/*! Delete generic cligen variable vector (cvv)
 *
 * @param[in]  h     Clixon handle
 * @param[in]  name  Name
 */
int
clicon_data_cvec_del(clixon_handle h,
                     const char   *name)
{
    cvec *cvv = NULL;

    clicon_ptr_get(h, name, (void**)&cvv);
    if (cvv)
        cvec_free(cvv);
    return clicon_ptr_del(h, name);
}

/*! Get data option as integer but store as string
 *
 * @param[in] h       clixon_handle
 * @param[in] name    option name
 * @retval    int     An integer as a result of atoi
 * @retval   -1       If option does not exist
 * Note that -1 can be both error and value.
 * @see clicon_option_int   for values in clixon config file
 */
int
clicon_data_int_get(clixon_handle h,
                    const char   *name)
{
    clicon_hash_t *cdat = clicon_data(h);
    char          *s;

    if (clicon_hash_lookup(cdat, (char*)name) == NULL)
        return -1;
    s = clicon_hash_value(cdat, (char*)name, NULL);
    return atoi(s);
}

/*! Set a single string data via handle
 *
 * @param[in] h       clixon_handle
 * @param[in] name    option name
 * @param[in] val     option value, must be null-terminated string
 * @retval    0       OK
 * @retval   -1       Error
 */
int
clicon_data_int_set(clixon_handle h,
                    const char   *name,
                    int           val)
{
    clicon_hash_t *cdat = clicon_data(h);
    char           s[64];

    if (snprintf(s, sizeof(s)-1, "%u", val) < 0)
        return -1;
    return clicon_hash_add(cdat, (char*)name, s, strlen(s)+1)==NULL?-1:0;
}

/*! Delete single int data via handle 
 *
 * @param[in] h       clixon_handle
 * @param[in] name    option name
 * @retval    0       OK
 * @retval   -1       Error
 */
int
clicon_data_int_del(clixon_handle h,
                    const char   *name)
{
    clicon_hash_t *cdat = clicon_data(h);

    return clicon_hash_del(cdat, (char*)name);
}

/*! Get top-level yang mounts
 *
 * @param[in]  h     Clixon handle
 * @retval     ymnts Yang mounts
 */
yang_stmt *
clixon_yang_mounts_get(clixon_handle h)
{
    yang_stmt *ys = NULL;

    if (clicon_ptr_get(h, "yang-mounts", (void**)&ys) < 0)
        return NULL;
    return ys;
}

/*! Set top-level yang mounts
 *
 * @param[in]  h     Clixon handle
 * @param[in]  yspec Yang spec (malloced pointer)
 */
int
clixon_yang_mounts_set(clixon_handle h,
                       yang_stmt    *ys)
{
    return clicon_ptr_set(h, "yang-mounts", ys);
}

/*! Get data yangspec, yspec
 *
 * @param[in]  h     Clixon handle
 * @retval     yspec Yang spec
 * @see clicon_config_yang  for the configuration yang
 */
yang_stmt *
clicon_dbspec_yang(clixon_handle h)
{
    yang_stmt *ys = NULL;
    yang_stmt *ymounts = NULL;

    if ((ymounts = clixon_yang_mounts_get(h)) != NULL)
        ys = yang_find(ymounts, Y_SPEC, YANG_DATA_TOP);
    return ys;
}

/*! Get YANG specification for clixon config (separate from application yangs)
 *
 * @param[in]  h     Clixon handle
 * @retval     yspec Yang spec
 * @see clicon_dbspec_yang  for the application specs
 */
yang_stmt *
clicon_config_yang(clixon_handle h)
{
    yang_stmt *ymounts = NULL;
    yang_stmt *ys = NULL;

    if ((ymounts = clixon_yang_mounts_get(h)) != NULL)
        ys = yang_find(ymounts, Y_SPEC, YANG_CONFIG_TOP);
    return ys;
}

/*! Get YANG specification for external NACM (separate from application yangs)
 *
 * @param[in]  h     Clixon handle
 * @retval     yspec Yang spec
 * @see clicon_nacm_ext  for external NACM XML
 */
yang_stmt *
clicon_nacm_ext_yang(clixon_handle h)
{
    yang_stmt *ymounts = NULL;
    yang_stmt *ys = NULL;

    if ((ymounts = clixon_yang_mounts_get(h)) != NULL)
        ys = yang_find(ymounts, Y_SPEC, YANG_NACM_TOP);
    return ys;
}

/*! Get Global "canonical" namespace context
 *
 * Canonical: use prefix and namespace specified in the yang modules.
 * @param[in]  h     Clixon handle
 * @retval     nsctx Namespace context (malloced)
 * @code
 *   cvec *nsctx;
 *   nsctx = clicon_nsctx_global_get(h);
 * @endcode                                     
 */
cvec *
clicon_nsctx_global_get(clixon_handle h)
{
    cvec *cvv = NULL;

    if (clicon_ptr_get(h, "nsctx_global", (void**)&cvv) < 0)
        return NULL;
    return cvv;
}

/*! Set global "canonical" namespace context
 *
 * Canonical: use prefix and namespace specified in the yang modules.
 * @param[in]  h     Clixon handle
 * @param[in]  nsctx Namespace context (malloced)
 */
int
clicon_nsctx_global_set(clixon_handle h,
                        cvec         *nsctx)
{
    return clicon_ptr_set(h, "nsctx_global", nsctx);
}

/*! Get NACM (rfc 8341) XML parse tree if external not in std xml config
 *
 * @param[in]  h    Clixon handle
 * @retval     xn   XML NACM tree, or NULL
 * @note only used if config option CLICON_NACM_MODE is external
 * @see clicon_nacm_ext_set
 */
cxobj *
clicon_nacm_ext(clixon_handle h)
{
    cxobj *x = NULL;

    if (clicon_ptr_get(h, "nacm_xml", (void**)&x) < 0)
        return NULL;
    return x;
}

/*! Set NACM (rfc 8341) external XML parse tree, free old if any
 *
 * @param[in]  h   Clixon handle
 * @param[in]  xn  XML Nacm tree
 * @note only used if config option CLICON_NACM_MODE is external
 * @see clicon_nacm_ext
 */
int
clicon_nacm_ext_set(clixon_handle h,
                     cxobj        *x)
{
    cxobj *x0 = NULL;

    if ((x0 = clicon_nacm_ext(h)) != NULL)
        xml_free(x0);
    return clicon_ptr_set(h, "nacm_xml", x);
}

/*! Get NACM (rfc 8341) XML parse tree cache
 *
 * @param[in]  h    Clixon handle
 * @retval     xn   XML NACM tree, or NULL. Direct pointer, no copying
 * @note  Use with caution, only valid on a stack, direct pointer freed on function return
 * @see from_client_msg
 */
cxobj *
clicon_nacm_cache(clixon_handle h)
{
    cxobj *x = NULL;

    if (clicon_ptr_get(h, "nacm_cache", (void**)&x) < 0)
        return NULL;
    return x;
}

/*! Set NACM (rfc 8341) external XML parse tree cache
 *
 * @param[in]  h   Clixon handle
 * @param[in]  xn  XML Nacm tree direct pointer, no copying
 * @note  Use with caution, only valid on a stack, direct pointer freed on function return
 * @see from_client_msg
 */
int
clicon_nacm_cache_set(clixon_handle h,
                      cxobj        *xn)
{
    return clicon_ptr_set(h, "nacm_cache", xn);
}

/*! Get YANG specification for Clixon system options and features
 *
 * Must use hash functions directly since they are not strings.
 * Example: features are typically accessed directly in the config tree.
 * @code
 *   cxobj *x = NULL;
 *   while ((x = xml_child_each(clicon_conf_xml(h), x, CX_ELMNT)) != NULL) {
 *      if (strcmp(xml_name(x), "CLICON_YANG_DIR") != 0)
 *         break;
 *   }
 * @endcode
 */
cxobj *
clicon_conf_xml(clixon_handle h)
{
    cxobj *x = NULL;

    if (clicon_ptr_get(h, "clixon_conf", (void**)&x) < 0)
        return NULL;
    return x;
}

/*! Set YANG specification for Clixon system options and features
 *
 * ys must be a malloced pointer
 */
int
clicon_conf_xml_set(clixon_handle h,
                    cxobj        *x)
{
    return clicon_ptr_set(h, "clixon_conf", x);
}

/*! Get local YANG specification for Clixon-restconf.yang tree
 *
 * That is, get the XML of clixon-config/restconf container of clixon-config.yang
 * @param[in]  h  Clixon handle
 * @retval     x  XML tree containing restconf xml node from clixon-restconf.yang
 * @code
 *    cxobj *xrestconf = clicon_conf_restconf(h);
 * @endcode
 * @note The clixon-restconf.yang instance can also be a part of the running datastore if 
 *       CLICON_BACKEND_RESTCONF_PROCESS is true
 */
cxobj *
clicon_conf_restconf(clixon_handle h)
{
    cxobj  *xconfig = NULL;

    if ((xconfig = clicon_conf_xml(h)) != NULL)          /* Get local config */
        return xpath_first(xconfig, NULL, "restconf");
    return NULL;
}

/*! Get clixon-autocli.yang part of the clixon config tree
 *
 * That is, get the XML of clixon-config/autocli container of clixon-config.yang
 * @param[in]  h  Clixon handle
 * @retval     x  XML tree containing clispec xml node from clixon-autoclu.yang
 * @code
 *    cxobj *xautocli = clicon_conf_autocli(h);
 * @endcode
 */
cxobj *
clicon_conf_autocli(clixon_handle h)
{
    cxobj  *xconfig = NULL;

    if ((xconfig = clicon_conf_xml(h)) != NULL){          /* Get local config */
        return xml_find_type(xconfig, NULL, "autocli", CX_ELMNT);
    }
    return NULL;
}

/*! Get authorized user name
 *
 * @param[in]  h   Clixon handle
 * @retval     username
 */
char *
clicon_username_get(clixon_handle h)
{
    clicon_hash_t  *cdat = clicon_data(h);

    return (char*)clicon_hash_value(cdat, "username", NULL);
}

/*! Set authorized user name
 *
 * @param[in]  h   Clixon handle
 * @param[in]  username
 * @note Just keep note of it, dont allocate it or so.
 */
int
clicon_username_set(clixon_handle h,
                    void         *username)
{
    clicon_hash_t  *cdat = clicon_data(h);

    if (username == NULL)
        return clicon_hash_del(cdat, "username");
    return clicon_hash_add(cdat, "username", username, strlen(username)+1)==NULL?-1:0;
}

/*! Get backend daemon startup status
 *
 * @param[in]  h      Clixon handle
 * @retval     status Startup status
 */
enum startup_status
clicon_startup_status_get(clixon_handle h)
{
    clicon_hash_t *cdat = clicon_data(h);
    void           *p;

    if ((p = clicon_hash_value(cdat, "startup_status", NULL)) != NULL)
        return *(enum startup_status *)p;
    return STARTUP_ERR;
}

/*! Set backend daemon startup status
 *
 * @param[in]  h      Clixon handle
 * @param[in]  status Startup status
 * @retval     0      OK
 * @retval    -1      Error (when setting value)
 */
int
clicon_startup_status_set(clixon_handle       h,
                          enum startup_status status)
{
    clicon_hash_t  *cdat = clicon_data(h);
    if (clicon_hash_add(cdat, "startup_status", &status, sizeof(status))==NULL)
        return -1;
    return 0;
}

/*! Get server socket fd (ie backend server socket / restconf fcgi socket)
 *
 * @param[in]  h   Clixon handle
 * @retval     s   Socket
 * @retval    -1   No open socket
 */
int
clicon_socket_get(clixon_handle h)
{
    clicon_hash_t *cdat = clicon_data(h);
    void           *p;

    if ((p = clicon_hash_value(cdat, "socket", NULL)) == NULL)
        return -1;
    return *(int*)p;
}

/*! Set server socket fd (ie backend server socket / restconf fcgi socket)
 *
 * @param[in] h   Clixon handle
 * @param[in] s   Open socket (or -1 to close)
 * @retval    0   OK
 * @retval   -1   Error
 */
int
clicon_socket_set(clixon_handle h,
                  int           s)
{
    clicon_hash_t  *cdat = clicon_data(h);

    if (s == -1)
        return clicon_hash_del(cdat, "socket");
    return clicon_hash_add(cdat, "socket", &s, sizeof(int))==NULL?-1:0;
}

/*! Get client socket fd (ie client cli / netconf / restconf / client-api socket
 *
 * @param[in]  h   Clixon handle
 * @retval     s   Socket
 * @retval    -1   No open socket
 */
int
clicon_client_socket_get(clixon_handle h)
{
    clicon_hash_t *cdat = clicon_data(h);
    void           *p;

    if ((p = clicon_hash_value(cdat, "client-socket", NULL)) == NULL)
        return -1;
    return *(int*)p;
}

/*! Set client socket fd (ie client cli / netconf / restconf / client-api socket
 *
 * @param[in]  h   Clixon handle
 * @param[in]  s   Open socket (or -1 to close)
 * @retval     0   OK
 * @retval    -1   Error
 */
int
clicon_client_socket_set(clixon_handle h,
                         int           s)
{
    clicon_hash_t  *cdat = clicon_data(h);

    if (s == -1)
        return clicon_hash_del(cdat, "client-socket");
    return clicon_hash_add(cdat, "client-socket", &s, sizeof(int))==NULL?-1:0;
}

/*! Get module state cache
 *
 * @param[in]  h     Clixon handle
 * @param[in]  brief 0: Full module state tree, 1: Brief tree (datastore)
 * @retval     xms   Module state cache XML tree
 * xms is on the form: <modules-state>...
 */
cxobj *
clicon_modst_cache_get(clixon_handle h,
                       int           brief)
{
    clicon_hash_t *cdat = clicon_data(h);
    void           *p;

    if ((p = clicon_hash_value(cdat, brief?"modst_brief":"modst_full", NULL)) != NULL)
        return *(cxobj **)p;
    return NULL;
}

/*! Set module state cache
 *
 * @param[in] h     Clixon handle
 * @param[in] brief 0: Full module state tree, 1: Brief tree (datastore)
 * @param[in] xms   Module state cache XML tree
 * @retval    0     OK
 * @retval   -1     Error
 */
int
clicon_modst_cache_set(clixon_handle h,
                       int           brief,
                        cxobj        *xms)
{
    clicon_hash_t  *cdat = clicon_data(h);
    cxobj          *x;

    if ((x = clicon_modst_cache_get(h, brief)) != NULL)
        xml_free(x);
    if (xms == NULL)
        goto ok;
    if ((x = xml_dup(xms)) == NULL)
        return -1;
    if (clicon_hash_add(cdat, brief?"modst_brief":"modst_full", &x, sizeof(x))==NULL)
        return -1;
 ok:
    return 0;
}

/*! Get yang module changelog
 *
 * @param[in]  h    Clixon handle
 * @retval     xch  Module revision changelog XML tree
 * @see draft-wang-netmod-module-revision-management-01
 */
cxobj *
clicon_xml_changelog_get(clixon_handle h)
{
    clicon_hash_t *cdat = clicon_data(h);
    void          *p;

    if ((p = clicon_hash_value(cdat, "xml-changelog", NULL)) != NULL)
        return *(cxobj **)p;
    return NULL;
}

/*! Set xml module changelog
 *
 * @param[in] h   Clixon handle
 * @param[in] s   Module revision changelog XML tree
 * @retval    0   OK
 * @retval   -1   Error
 * @see draft-wang-netmod-module-revision-management-01
 */
int
clicon_xml_changelog_set(clixon_handle h,
                         cxobj        *xchlog)
{
    clicon_hash_t  *cdat = clicon_data(h);

    if (clicon_hash_add(cdat, "xml-changelog", &xchlog, sizeof(xchlog))==NULL)
        return -1;
    return 0;
}

/*! Get user clicon command-line options argv, argc (after --)
 *
 * @param[in]  h    Clixon handle
 * @param[out] argc
 * @param[out] argv
 * @retval     0    OK 
 * @retval    -1    Error
 */
int
clicon_argv_get(clixon_handle h,
                int          *argc,
                char       ***argv)
{
    clicon_hash_t *cdat = clicon_data(h);
    void          *p;

    if (argc){
        if ((p = clicon_hash_value(cdat, "argc", NULL)) == NULL)
            return -1;
        *argc = *(int*)p;
    }
    if (argv){
        if ((p = clicon_hash_value(cdat, "argv", NULL)) == NULL)
            return -1;
        *argv = (char**)p;
    }
    return 0;
}

/*! Set clicon user command-line options argv, argc (after --)
 *
 * @param[in] h     Clixon handle
 * @param[in] prog  argv[0] - the program name
 * @param[in] argc  Length of argv
 * @param[in] argv  Array of command-line options or NULL
 * @retval    0     OK
 * @retval   -1     Error
 * @note If argv=NULL deallocate allocated argv vector if exists.
 */
int
clicon_argv_set(clixon_handle h,
                char         *prgm,
                int           argc,
                char        **argv)
{
    int             retval = -1;
    clicon_hash_t  *cdat = clicon_data(h);
    char          **argvv = NULL;
    size_t          len;

    /* add space for null-termination and argv[0] program name */
    len = argc+2;
    if ((argvv = calloc(len, sizeof(char*))) == NULL){
        clixon_err(OE_UNIX, errno, "calloc");
        goto done;
    }
    memcpy(argvv+1, argv, argc*sizeof(char*));
    argvv[0] = prgm;
    /* Note the value is the argv vector (which is copied) */
    if (clicon_hash_add(cdat, "argv", argvv, len*sizeof(char*))==NULL)
        goto done;
    argc += 1;
    if (clicon_hash_add(cdat, "argc", &argc, sizeof(argc))==NULL)
        goto done;
    retval = 0;
 done:
    if (argvv)
        free(argvv);
    return retval;
}

/*! Get session id
 *
 * @param[in]  h    Clixon handle
 * @param[out] sid  Session identifier
 * @retval     0    OK
 * @retval    -1    Session id not set
 * Session-ids survive TCP sessions that are created for each message sent to the backend.
 * The backend assigns session-id for clients: backend assigns, clients get it from backend.
 * @note a client will get the currrent session-id of that client, BUT
 *       a backend will get the next session-id to be assigned
 *       A backend getting a session-id of an ongoing session should use ce->ce_id
 */
int
clicon_session_id_get(clixon_handle h,
                      uint32_t     *id)
{
    clicon_hash_t *cdat = clicon_data(h);
    void           *p;

    if ((p = clicon_hash_value(cdat, "session-id", NULL)) == NULL)
        return -1;
    *id = *(uint32_t*)p;
    return 0;
}

/*! Delete session id
 */
int
clicon_session_id_del(clixon_handle h)
{
    clicon_hash_t *cdat = clicon_data(h);

    return clicon_hash_del(cdat, (char*)"session-id");
}

/*! Set session id
 *
 * @param[in]  h   Clixon handle
 * @param[in]  id  Session id (in range 1..max uint32)
 * @retval     0   OK
 * @retval    -1   Error
 * Session-ids survive TCP sessions that are created for each message sent to the backend.
 */
int
clicon_session_id_set(clixon_handle h,
                      uint32_t      id)
{
    clicon_hash_t  *cdat = clicon_data(h);

    clicon_hash_add(cdat, "session-id", &id, sizeof(uint32_t));
    return 0;
}

/*! Get quit-after-upgrade flag
 *
 * @param[in]  h    Clixon handle
 * @retval     1    Flag set: quit startup directly after upgrade
 * @retval     0    Flag not set
 * If set, quit startup directly after upgrade
 */
int
clicon_quit_upgrade_get(clixon_handle h)
{
    clicon_hash_t *cdat = clicon_data(h);
    void           *p;

    if ((p = clicon_hash_value(cdat, "quit-after-upgrade", NULL)) == NULL)
        return 0;
    return *(int*)p;
}

/*! Set quit-after-upgrade flag
 *
 * @param[in]  h   Clixon handle
 * @param[in]  val  Set or reset flag
 * @retval     0   OK
 * @retval    -1   Error
 * If set, quit startup directly after upgrade
 */
int
clicon_quit_upgrade_set(clixon_handle h,
                        int           val)
{
    clicon_hash_t  *cdat = clicon_data(h);

    clicon_hash_add(cdat, "quit-after-upgrade", &val, sizeof(int));
    return 0;
}
