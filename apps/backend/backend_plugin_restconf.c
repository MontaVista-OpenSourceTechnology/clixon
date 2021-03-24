/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2017-2019 Olof Hagsand
  Copyright (C) 2020-2021 Olof Hagsand and Rubicon Communications, LLC(Netgate)

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
  * Pseudo backend plugin for starting restconf daemon
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include "clixon_backend_transaction.h"
#include "backend_plugin_restconf.h"

/*---------------------------------------------------------------------
 * Restconf process pseudo plugin
 */

#define RESTCONF_PROCESS "restconf"

/*! Process rpc callback function 
 * - if RPC op is start, if enable is true, start the service, if false, error or ignore it
 * - if RPC op is stop, stop the service 
 * These rules give that if RPC op is start and enable is false -> change op to none
 */
int
restconf_rpc_wrapper(clicon_handle    h,
		     process_entry_t *pe,
		     proc_operation  *operation)
{
    int    retval = -1;
    cxobj *xt = NULL;
    
    clicon_debug(1, "%s", __FUNCTION__);
    switch (*operation){
    case PROC_OP_STOP:
	/* if RPC op is stop, stop the service */
	break;
    case PROC_OP_START:
	/* RPC op is start & enable is true, then start the service, 
                           & enable is false, error or ignore it */
	if (xmldb_get(h, "running", NULL,  "/restconf", &xt) < 0)
	    goto done;
	if (xt != NULL &&
	    xpath_first(xt, NULL, "/restconf[enable='false']") != NULL) {
	    *operation = PROC_OP_NONE;
	}
	break;
    default:
	break;
    }
    retval = 0;
 done:
    if (xt)
	xml_free(xt);
    return retval;
}

/*! Enable process-control of restconf daemon, ie start/stop restconf by registering restconf process
 * @param[in]  h  Clicon handle
 * @note Could also look in clixon-restconf and start process if enable is true, but that needs to 
 *       be in start callback using a pseudo plugin.
 */
static int
restconf_pseudo_process_control(clicon_handle h)
{
    int    retval = -1;
    char **argv = NULL;
    int    i;
    int    nr;
    cbuf  *cb = NULL;
    cbuf  *cbdbg = NULL;

    nr = 4;
    if (clicon_debug_get() != 0)
	nr += 2;
    if ((argv = calloc(nr, sizeof(char *))) == NULL){
	clicon_err(OE_UNIX, errno, "calloc");
	goto done;
    }
    i = 0;
    if ((cb = cbuf_new()) == NULL){
	clicon_err(OE_UNIX, errno, "cbuf_new");
	goto done;
    }
    cprintf(cb, "%s/clixon_restconf", clicon_option_str(h, "CLICON_WWWDIR"));
    argv[i++] = cbuf_get(cb);
    argv[i++] = "-f";
    argv[i++] = clicon_option_str(h, "CLICON_CONFIGFILE");
    /* Add debug if backend has debug. 
     * There is also a debug flag in clixon-restconf.yang but it kicks in after it starts
     */
    if (clicon_debug_get() != 0){
	argv[i++] = "-D";
	if ((cbdbg = cbuf_new()) == NULL){ /* Cant use cb since it would overwrite it */
	    clicon_err(OE_UNIX, errno, "cbuf_new");
	    goto done;
	}
	cprintf(cbdbg, "%d", clicon_debug_get());
	argv[i++] = cbuf_get(cbdbg);
    }
    argv[i++] = NULL;
    assert(i==nr);
    if (clixon_process_register(h, RESTCONF_PROCESS,
				"Clixon RESTCONF process",
				NULL /* XXX network namespace */,
				restconf_rpc_wrapper,
				argv, nr) < 0)
	goto done;
    if (argv != NULL)
	free(argv);
    retval = 0;
 done:
    if (cb)
	cbuf_free(cb);
    if (cbdbg)
	cbuf_free(cbdbg);
    return retval;
}

/*! Restconf pseduo-plugin process validate
 */
static int
restconf_pseudo_process_validate(clicon_handle    h,
				 transaction_data td)
{
    int    retval = -1;
    cxobj *xtarget;

    clicon_debug(1, "%s", __FUNCTION__);
    xtarget = transaction_target(td);
    /* If ssl-enable is true and (at least a) socket has ssl,
     * then server-cert-path and server-key-path must exist */
    if (xpath_first(xtarget, NULL, "restconf/enable[.='true']") &&
	xpath_first(xtarget, NULL, "restconf/socket[ssl='true']")){
	/* Should filepath be checked? One could claim this is a runtime system,... */
	if (xpath_first(xtarget, 0, "restconf/server-cert-path") == NULL){
	    clicon_err(OE_CFG, 0, "SSL enabled but server-cert-path not set");
	    return -1; /* induce fail */
	}
	if (xpath_first(xtarget, 0, "restconf/server-key-path") == NULL){
	    clicon_err(OE_CFG, 0, "SSL enabled but server-key-path not set");
	    return -1; /* induce fail */
	}
    }
    retval = 0;
    return retval;
}

/*! Restconf pseduo-plugin process commit
 */
static int
restconf_pseudo_process_commit(clicon_handle    h,
			       transaction_data td)
{
    int    retval = -1;
    cxobj *xtarget;
    cxobj *cx;
    int    enabled = 0;

    clicon_debug(1, "%s", __FUNCTION__);
    xtarget = transaction_target(td);
    if (xpath_first(xtarget, NULL, "/restconf[enable='true']") != NULL)
	enabled++;
    /* Toggle start/stop if enable flag changed */
    if ((cx = xpath_first(xtarget, NULL, "/restconf/enable")) != NULL &&
	xml_flag(cx, XML_FLAG_CHANGE|XML_FLAG_ADD)){
	if (clixon_process_operation(h, RESTCONF_PROCESS,
				     enabled?PROC_OP_START:PROC_OP_STOP, 0) < 0)
	    goto done;
    }
    else if (enabled){     /* If something changed and running, restart process */
	if (transaction_dlen(xtarget) != 0 ||
	    transaction_alen(xtarget) != 0 ||
	    transaction_clen(xtarget) != 0){
	    if ((cx = xpath_first(xtarget, NULL, "/restconf")) != NULL &&
		xml_flag(cx, XML_FLAG_CHANGE|XML_FLAG_ADD)){
		/* A restart can terminate a restconf connection (cut the tree limb you are sitting on)
		 * Specifically, the socket is terminated where the reply is sent, which will
		 * cause the curl to fail.
		 */
		if (clixon_process_operation(h, RESTCONF_PROCESS, PROC_OP_RESTART, 0) < 0)
		    goto done;
	    }
	}
    }
    retval = 0;
 done:
    return retval;
}

/*! Register start/stop restconf RPC and create pseudo-plugin to monitor enable flag
 * @param[in]  h  Clixon handle
 */
int
backend_plugin_restconf_register(clicon_handle h,
				 yang_stmt    *yspec)
{
    int            retval = -1;
    clixon_plugin *cp = NULL;

    if (clixon_pseudo_plugin(h, "restconf pseudo plugin", &cp) < 0)
	goto done;

    cp->cp_api.ca_trans_validate = restconf_pseudo_process_validate;
    cp->cp_api.ca_trans_commit = restconf_pseudo_process_commit;

    /* Register generic process-control of restconf daemon, ie start/stop restconf */
    if (restconf_pseudo_process_control(h) < 0)
	goto done;
    retval = 0;
 done:
    return retval;
}
