/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren
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
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <syslog.h>
#include <errno.h>
#define __USE_GNU /* For RTLD_DEFAULT */
#include <dlfcn.h>
#include <dirent.h>
#include <libgen.h>
#include <grp.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <netinet/in.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

/* clicon_cli */
#include "clixon_cli_api.h"
#include "cli_plugin.h"
#include "cli_handle.h"
#include "cli_generate.h"

/*
 * Constants
 */
#define CLI_DEFAULT_PROMPT	"cli> "

/*
 *
 * CLI PLUGIN INTERFACE, INTERNAL SECTION
 *
 */

/*! Find syntax mode named 'mode'. Create if specified
 */
static cli_syntaxmode_t *
syntax_mode_find(cli_syntax_t *stx,
		 const char   *mode,
		 int           create)
{
    cli_syntaxmode_t *csm;

    csm = stx->stx_modes;
    if (csm) {
	do {
	    if (strcmp(csm->csm_name, mode) == 0)
		return csm;
	    csm = NEXTQ(cli_syntaxmode_t *, csm);
	} while (csm && csm != stx->stx_modes);
    }
    
    if (create == 0)
	return  NULL;

   if ((csm = malloc(sizeof(cli_syntaxmode_t))) == NULL) {
       clicon_err(OE_UNIX, errno, "malloc");
       return NULL;
    }
    memset(csm, 0, sizeof(*csm));
    if ((csm->csm_name = strdup(mode)) == NULL){
	clicon_err(OE_UNIX, errno, "strdup");
	return NULL;
    }
    if ((csm->csm_prompt = strdup(CLI_DEFAULT_PROMPT)) == NULL){
	clicon_err(OE_UNIX, errno, "strdup");
	return NULL;
    }
    if ((csm->csm_pt = pt_new()) == NULL){
	clicon_err(OE_UNIX, errno, "pt_new");
	return NULL;
    }
    INSQ(csm, stx->stx_modes);
    stx->stx_nmodes++;

    return csm;
}

/*! Generate parse tree for syntax mode 
 * @param[in]   h     Clicon handle
 * @param[in]   m     Syntax mode struct
 */
static int
gen_parse_tree(clicon_handle     h,
	       cli_syntaxmode_t *m)
{
    int       retval = -1;
    pt_head  *ph;
    
    if ((ph = cligen_ph_add(cli_cligen(h), m->csm_name)) == NULL)
	goto done;
    if (cligen_ph_parsetree_set(ph, m->csm_pt) < 0)
	goto done;
    retval = 0;
 done:
    return retval;
}

/*! Append syntax
 * @param[in]     h       Clicon handle
 */
static int
syntax_append(clicon_handle h,
	      cli_syntax_t *stx,
	      const char   *name, 
	      parse_tree   *pt)
{
    cli_syntaxmode_t *csm;

    if ((csm = syntax_mode_find(stx, name, 1)) == NULL) 
	return -1;

    if (cligen_parsetree_merge(csm->csm_pt, NULL, pt) < 0)
	return -1;
    
    return 0;
}

/*! Remove all cligen syntax modes
 * @param[in]     h       Clicon handle
 */
static int
cli_syntax_unload(clicon_handle h)
{
    cli_syntax_t            *stx = cli_syntax(h);
    cli_syntaxmode_t        *csm;

    if (stx == NULL)
	return 0;

    while (stx->stx_nmodes > 0) {
	csm = stx->stx_modes;
	DELQ(csm, stx->stx_modes, cli_syntaxmode_t *);
	if (csm){
	    if (csm->csm_name)
		free(csm->csm_name);
	    if (csm->csm_prompt)
		free(csm->csm_prompt);
	    free(csm);
	}
	stx->stx_nmodes--;
    }
    return 0;
}

/*! Dynamic linking loader string to function mapper
 *
 * Maps strings from the CLI specification file to real funtions using dlopen 
 * mapping. 
 * First look for function name in local namespace if handle given (given plugin)
 * Then check global namespace, i.e.m lib*.so
 * 
 * @param[in]  name    Name of function
 * @param[in]  handle  Handle to plugin .so module  as returned by dlopen
 * @param[out] error   Static error string, if set indicates error
 * @retval     fn      Function pointer
 * @retval     NULL    Function not found or symbol NULL (check error for proper handling)
 * @see see cli_plugin_load where (optional) handle opened
 * @note the returned function is not type-checked which may result in segv at runtime
 */
void *
clixon_str2fn(char  *name, 
	      void  *handle, 
	      char **error)
{
    void *fn = NULL;
	
    /* Reset error */
    *error = NULL;
    /* Special check for auto-cli. If the virtual callback is used, it should be overwritten later 
     * by a callback given in the clispec, eg: set @datamodel, cli_set();
     */
    if (strcmp(name, GENERATE_CALLBACK) == 0)
	return NULL;

    /* First check given plugin if any */
    if (handle) {
	dlerror();	/* Clear any existing error */
	fn = dlsym(handle, name);
	if ((*error = (char*)dlerror()) == NULL)
	    return fn;  /* If no error we found the address of the callback */
    }

    /* Now check global namespace which includes any shared object loaded
     * into the global namespace. I.e. all lib*.so as well as the 
     * master plugin if it exists 
     */
    dlerror();	/* Clear any existing error */
    /* RTLD_DEFAULT instead of NULL for linux + FreeBSD:
     * Use default search algorithm. Thanks jdl@netgate.com */
    fn = dlsym(RTLD_DEFAULT, name);
    if ((*error = (char*)dlerror()) == NULL)
	return fn;  /* If no error we found the address of the callback */

    /* Return value not really relevant here as the error string is set to
     * signal an error. However, just checking the function pointer for NULL
     * should work in most cases, although it's not 100% correct. 
     */
   return NULL; 
}

/*! Load a file containing syntax and append to specified modes, also load C plugin
 *
 * First load CLIgen file, 
 * Then find which .so to load by looking in the "CLICON_PLUGIN" variable in that file.
 * Make a lookup of plugins already loaded and resolve callbacks from cligen trees to
 * dl symbols in the plugin.
 * @param[in]  h        Clixon handle
 * @param[in]  filename	Name of file where syntax is specified (in syntax-group dir)
 * @param[in]  dir	Name of dir, or NULL
 * @param[out] ptall    Universal CLIgen parse tree: apply to all modes
 * @see clixon_plugins_load  Where .so plugin code has been loaded prior to this
 */
static int
cli_load_syntax_file(clicon_handle h,
		     const char   *filename,
		     const char   *dir,
		     parse_tree   *ptall)
{
    void          *handle = NULL;  /* Handle to plugin .so module */
    char          *mode = NULL;    /* Name of syntax mode to append new syntax */
    parse_tree    *pt = NULL;
    int            retval = -1;
    FILE          *f;
    char           filepath[MAXPATHLEN];
    cvec          *cvv = NULL;
    char          *prompt = NULL;
    char         **vec = NULL;
    int            i, nvec;
    char          *plgnam;
#ifndef CLIXON_STATIC_PLUGINS
    clixon_plugin_t *cp;
#endif

    if ((pt = pt_new()) == NULL){
	clicon_err(OE_UNIX, errno, "pt_new");
	goto done;
    }
    if (dir)
	snprintf(filepath, MAXPATHLEN-1, "%s/%s", dir, filename);
    else
	snprintf(filepath, MAXPATHLEN-1, "%s", filename);
    if ((cvv = cvec_new(0)) == NULL){
	clicon_err(OE_PLUGIN, errno, "cvec_new");
	goto done;
    }
    /* Build parse tree from syntax spec. */
    if ((f = fopen(filepath, "r")) == NULL){
	clicon_err(OE_PLUGIN, errno, "fopen %s", filepath);
	goto done;
    }

    /* Assuming this plugin is first in queue */
    if (cli_parse_file(h, f, filepath, pt, cvv) < 0){
	clicon_err(OE_PLUGIN, 0, "failed to parse cli file %s", filepath);
	fclose(f);
	goto done;
    }
    fclose(f);
    /* Get CLICON specific global variables:
     *  CLICON_MODE: which mode(s) this syntax applies to
     *  CLICON_PROMPT: Cli prompt in this mode
     *  CLICON_PLUGIN: Name of C API plugin
     * Note: the base case is that it is:
     *   (1) a single mode or 
     *   (2) "*" all modes or "m1:m2" - a list of modes
     * but for (2), prompt and plgnam may have unclear semantics
     */
    mode = cvec_find_str(cvv, "CLICON_MODE");
    prompt = cvec_find_str(cvv, "CLICON_PROMPT");
    plgnam = cvec_find_str(cvv, "CLICON_PLUGIN");

#ifndef CLIXON_STATIC_PLUGINS
    if (plgnam != NULL) { /* Find plugin for callback resolving */
	if ((cp = clixon_plugin_find(h, plgnam)) != NULL)
	    handle = clixon_plugin_handle_get(cp);
	if (handle == NULL){
	    clicon_err(OE_PLUGIN, 0, "CLICON_PLUGIN set to '%s' in %s but plugin %s.so not found in %s", 
		       plgnam, filename, plgnam, 
		       clicon_cli_dir(h));
	    goto done;
	}
    }
#endif
    /* Resolve callback names to function pointers. */
    if (cligen_callbackv_str2fn(pt, (cgv_str2fn_t*)clixon_str2fn, handle) < 0){     
	clicon_err(OE_PLUGIN, 0, "Mismatch between CLIgen file '%s' and CLI plugin file '%s'. Some possible errors:\n\t1. A function given in the CLIgen file does not exist in the plugin (ie link error)\n\t2. The CLIgen spec does not point to the correct plugin .so file (CLICON_PLUGIN=\"%s\" is wrong)", 
		   filename, plgnam, plgnam);
	goto done;
    }
     if (cligen_expandv_str2fn(pt, (expandv_str2fn_t*)clixon_str2fn, handle) < 0)     
	 goto done;
     /* Variable translation functions */
     if (cligen_translate_str2fn(pt, (translate_str2fn_t*)clixon_str2fn, handle) < 0)     
	 goto done;

    /* Make sure we have a syntax mode specified */
    if (mode == NULL || strlen(mode) < 1) { /* may be null if not given in file */
	mode = clicon_cli_mode(h);
	if (mode == NULL || strlen(mode) < 1) { /* may be null if not given in file */	
	    clicon_err(OE_PLUGIN, 0, "No syntax mode specified in %s", filepath);
	    goto done;
	}
    }
    /* Find all modes in CLICON_MODE string: where to append the pt syntax tree */
    if ((vec = clicon_strsep(mode, ":", &nvec)) == NULL) 
	goto done;

    if (nvec == 1 && strcmp(vec[0], "*") == 0){
 	/* Special case: Add this to all modes. Add to special "universal" syntax
	 * and add to all syntaxes after all files have been loaded. At this point
	 * all modes may not be known (not yet loaded)
	 */
	if (cligen_parsetree_merge(ptall, NULL, pt) < 0)
	    return -1;
    }
    else {
	for (i = 0; i < nvec; i++) {
	    if (syntax_append(h,
			      cli_syntax(h),
			      vec[i],
			      pt) < 0) { 
		goto done;
	    }
	    if (prompt)
		cli_set_prompt(h, vec[i], prompt);
	}
    }

    cligen_parsetree_free(pt, 1);
    retval = 0;
    
done:
    if (cvv)
	cvec_free(cvv);
    if (vec)
	free(vec);
    return retval;
}

/*! CLIgen spec syntax files and create CLIgen trees to drive the CLI syntax generator
 *
 * CLI .so plugins have been loaded: syntax table in place.
 * Now load cligen syntax files and create cligen pt trees.
 * @param[in]     h       Clicon handle
 */
int
cli_syntax_load(clicon_handle h)
{
    int                retval = -1;
    char              *clispec_dir = NULL;
    char              *clispec_file = NULL;
    int                ndp;
    int                i;
    struct dirent     *dp = NULL;
    cli_syntax_t      *stx;
    cli_syntaxmode_t  *m;
    cligen_susp_cb_t  *fns = NULL;
    cligen_interrupt_cb_t *fni = NULL;
    clixon_plugin_t     *cp;
    parse_tree        *ptall = NULL; /* Universal CLIgen parse tree all modes */

    /* Syntax already loaded.  XXX should we re-load?? */
    if ((stx = cli_syntax(h)) != NULL)
	return 0;
    if ((ptall = pt_new()) == NULL){
	clicon_err(OE_UNIX, errno, "pt_new");
	goto done;
    }

    /* Format plugin directory path */
    clispec_dir = clicon_clispec_dir(h);
    clispec_file = clicon_option_str(h, "CLICON_CLISPEC_FILE");

    /* Allocate plugin group object */
    if ((stx = malloc(sizeof(*stx))) == NULL) {
	clicon_err(OE_UNIX, errno, "malloc");
	goto done;
    }
    memset(stx, 0, sizeof(*stx));	/* Zero out all */

    cli_syntax_set(h, stx);

    /* Load single specific clispec file */
    if (clispec_file){
	if (cli_load_syntax_file(h, clispec_file, NULL, ptall) < 0)
	    goto done;
    }
    /* Load all clispec .cli files in directory */
    if (clispec_dir){
	/* Get directory list of files */
	if ((ndp = clicon_file_dirent(clispec_dir, &dp, "(.cli)$", S_IFREG)) < 0)
	    goto done;
	/* Load the syntax parse trees into cli_syntax stx structure */
	for (i = 0; i < ndp; i++) {
	    clicon_debug(1, "DEBUG: Loading syntax '%.*s'", 
			 (int)strlen(dp[i].d_name)-4, dp[i].d_name);
	    if (cli_load_syntax_file(h, dp[i].d_name, clispec_dir, ptall) < 0)
		goto done;
	}
    }
    /* Were any syntax modes successfully loaded? If not, leave */
    if (stx->stx_nmodes <= 0) {
	retval = 0;
	goto done;
    }	
    
    /* Go thorugh all modes and :
     * 1) Add the universal syntax 
     * 2) add syntax tree (of those modes - "activate" syntax from stx to CLIgen)
     */
    m = stx->stx_modes;
    do {
	if (cligen_parsetree_merge(m->csm_pt, NULL, ptall) < 0)
	    return -1;
	if (gen_parse_tree(h, m) != 0)
	    goto done;
	m = NEXTQ(cli_syntaxmode_t *, m);
    } while (m && m != stx->stx_modes);

    /* Set susp and interrupt callbacks into  CLIgen */
    cp = NULL;
    while ((cp = clixon_plugin_each(h, cp)) != NULL) {
	if (fns==NULL && (fns = clixon_plugin_api_get(cp)->ca_suspend) != NULL)
	    if (cli_susp_hook(h, fns) < 0)
		goto done;
	if (fni==NULL && (fni = clixon_plugin_api_get(cp)->ca_interrupt) != NULL)
	    if (cli_interrupt_hook(h, fni) < 0)
		goto done;
    }

    /* All good. We can now proudly return a new group */
    retval = 0;
done:
    if (retval != 0) {
	cli_syntax_unload(h);
	cli_syntax_set(h, NULL);
    }
    cligen_parsetree_free(ptall, 1);
    if (dp)
	free(dp);
    return retval;
}

/*! Remove syntax modes and remove syntax
 * @param[in]     h       Clicon handle
 */
int
cli_plugin_finish(clicon_handle h)
{
    /* Remove all cligen syntax modes */
    cli_syntax_unload(h);
    cli_syntax_set(h, NULL);
    return 0;
}

/*! Help function to print a meaningful error string. 
 * Sometimes the libraries specify an error string, if so print that.
 * Otherwise just print 'command error'.
 * @param[in]  f   File handler to write error to.
 */
int 
cli_handler_err(FILE *f)
{
    if (clicon_errno){
	fprintf(f,  "%s: %s", clicon_strerror(clicon_errno), clicon_err_reason);
	if (clicon_suberrno)
	    fprintf(f, ": %s", strerror(clicon_suberrno));
	fprintf(f,  "\n");
    }
    else
	fprintf(f, "CLI command error\n");
    return 0;
}

/*! Given a command string, parse and if match single command, eval it.
 * Parse and evaluate the string according to
 * the syntax parse tree of the syntax mode specified by *mode.
 * If there is no match in the tree for the command, the parse hook 
 * will be called to see if another mode should be evaluated. If a
 * match is found in another mode, the mode variable is updated to point at 
 * the new mode string.
 *
 * @param[in]     h           Clicon handle
 * @param[in]     cmd	      Command string
 * @param[in,out] modenamep   Pointer to the mode string pointer
 * @param[out]    result      CLIgen match result, < 0: errors, >=0 number of matches
 * @param[out]    evalres     Evaluation result if result=1
 * @retval  0     OK
 * @retval  -1    Error
 */
int
clicon_parse(clicon_handle  h, 
	     char          *cmd, 
	     char         **modenamep, 
	     cligen_result *result,	     
	     int           *evalres)
{
    int               retval = -1;
    char             *modename;
    int               ret;
    cli_syntax_t     *stx = NULL;
    cli_syntaxmode_t *csm;
    parse_tree       *pt;     /* Orig */
    cg_obj           *match_obj = NULL;
    cvec             *cvv = NULL;
    cg_callback      *callbacks = NULL;
    FILE             *f;
    char             *reason = NULL;
    cligen_handle     ch;
    
    ch = cli_cligen(h);
    if (clicon_get_logflags()&CLICON_LOG_STDOUT)
	f = stdout;
    else
	f = stderr;
    stx = cli_syntax(h);
    if ((modename = *modenamep) == NULL) {
	csm = stx->stx_active_mode;
	modename = csm->csm_name;
    }
    else {
	if ((csm = syntax_mode_find(stx, modename, 0)) == NULL) {
	    fprintf(f, "Can't find syntax mode '%s'\n", modename);
	    goto done;
	}
    }
    if (csm != NULL){
	if (cligen_ph_active_set_byname(ch, modename) < 0){
	    fprintf(stderr, "No such parse-tree registered: %s\n", modename);
	    goto done;
	}
	if ((pt = cligen_pt_active_get(ch)) == NULL){
	    fprintf(stderr, "No such parse-tree registered: %s\n", modename);
	    goto done;
	}
	if (cliread_parse(ch, cmd, pt, &match_obj, &cvv, &callbacks, result, &reason) < 0)
	    goto done;
	/* Debug command and result code */
	clicon_debug(1, "%s result:%d command: \"%s\"", __FUNCTION__, *result, cmd);
	switch (*result) {
	case CG_EOF: /* eof */
	case CG_ERROR:
	    fprintf(f, "CLI parse error: %s\n", cmd);
	    break;
	case CG_NOMATCH: /* no match */
	    fprintf(f, "CLI syntax error: \"%s\": %s\n", cmd, reason);
	    break;
	case CG_MATCH:
	    if (strcmp(modename, *modenamep)){	/* Command in different mode */
		*modenamep = modename;
		cli_set_syntax_mode(h, modename);
	    }
	    cli_output_reset();
	    if (!cligen_exiting(ch)) {	
		clicon_err_reset();
		if ((ret = cligen_eval(ch, match_obj, cvv, callbacks)) < 0) 
		    cli_handler_err(stdout);
		
	    }
	    else
		ret = 0;
	    if (evalres)
		*evalres = ret;
	    break;
	default:
	    fprintf(f, "CLI syntax error: \"%s\" is ambiguous\n", cmd);
	    break;
	} /* switch result */
    }
    retval = 0;
done:
    if (callbacks)
	co_callbacks_free(&callbacks);
    if (reason)
	free(reason);
    if (cvv)
	cvec_free(cvv);
    if (match_obj)
	co_free(match_obj, 0);
    return retval;
}

/*! Return a malloced expanded prompt string from printf-like format
 * @param[in]   h        Clixon handle
 * @param[in]   fmt      Format string, using %H, %
 * @retval      prompt   Malloced string, free after use
 * @retval      NULL     Error
 */
static char *
cli_prompt_get(clicon_handle h,
	       char         *fmt)
{
    char   *s = fmt;
    char    hname[1024];
    char    tty[32];
    char   *tmp;
    cbuf   *cb = NULL;
    char   *path = NULL;
    char   *promptstr = NULL;
    char   *str0;

    if ((cb = cbuf_new()) == NULL){
	clicon_err(OE_XML, errno, "cbuf_new");
	goto done;
    }
    /* Start with empty string */
    while(*s) {
	if (*s == '%' && *++s) {
	    switch(*s) {
	    case 'H': /* Hostname */
		if (gethostname(hname, sizeof(hname)) != 0)
		    strncpy(hname, "unknown", sizeof(hname)-1);
		cprintf(cb, "%s", hname);
		break;
	    case 'U': /* Username */
		tmp = getenv("USER");
		cprintf(cb, "%s", tmp?tmp:"nobody");
		break;
	    case 'T': /* TTY */
		if(ttyname_r(fileno(stdin), tty, sizeof(tty)-1) < 0)
		    strcpy(tty, "notty");
		cprintf(cb, "%s", tty);
		break;
	    case 'W': /* working edit path */
		if (clicon_data_get(h, "cli-edit-mode", &path) == 0 &&
		    strlen(path))
		    cprintf(cb, "%s", path);
		else
		    cprintf(cb, "/");
		break;
	    default:
		cprintf(cb, "%%");
		cprintf(cb, "%c", *s);
	    }
	}
	else if (*s == '\\' && *++s) {
	    switch(*s) {
	    case 'n':
		cprintf(cb, "\n");
		break;
	    default:
		cprintf(cb, "\\");
		cprintf(cb, "%c", *s);
	    }
	}
	else 
	    cprintf(cb, "%c", *s);
	s++;
    }
    str0 = cbuf_len(cb) ? cbuf_get(cb) : CLI_DEFAULT_PROMPT;
    if ((promptstr = strdup(str0)) == NULL){
	clicon_err(OE_UNIX, errno, "strdup");
	goto done;
    }
 done:
    if (cb)
	cbuf_free(cb);
    return promptstr;
}

/*! Read command from CLIgen's cliread() using current syntax mode.
 * @param[in]  h       Clicon handle
 * @param[out] stringp Pointer to command buffer or NULL on EOF
 * @retval     0       OK
 * @retval    -1       Error
 */
int
clicon_cliread(clicon_handle h,
	       char        **stringp)
{
    int               retval = -1;
    char             *pfmt = NULL;
    cli_syntaxmode_t *mode;
    cli_syntax_t     *stx;
    cli_prompthook_t *fn;
    clixon_plugin_t  *cp;
    char             *promptstr;
    
    stx = cli_syntax(h);
    mode = stx->stx_active_mode;
    /* Get prompt from plugin callback? */
    cp = NULL;
    while ((cp = clixon_plugin_each(h, cp)) != NULL) {
	if ((fn = clixon_plugin_api_get(cp)->ca_prompt) == NULL)
	    continue;
	pfmt = fn(h, mode->csm_name);
	break;
    }
    if (clicon_quiet_mode(h))
	cli_prompt_set(h, "");
    else{
	if ((promptstr = cli_prompt_get(h, pfmt ? pfmt : mode->csm_prompt)) == NULL)
	    goto done;
	cli_prompt_set(h, promptstr);
	free(promptstr);
    }
    cligen_ph_active_set_byname(cli_cligen(h), mode->csm_name);

    if (cliread(cli_cligen(h), stringp) < 0){
	clicon_err(OE_FATAL, errno, "CLIgen");
	goto done;
    }
    retval = 0;
 done:
    if (pfmt)
	free(pfmt);
    return retval;
}

/*
 *
 * CLI PLUGIN INTERFACE, PUBLIC SECTION
 *
 */

/*! Set syntax mode mode for existing current plugin group.
 * @param[in]     h       Clicon handle
 */
int
cli_set_syntax_mode(clicon_handle h,
		    const char   *name)
{
    cli_syntaxmode_t *mode;
    
    if ((mode = syntax_mode_find(cli_syntax(h), name, 1)) == NULL)
	return 0;
    
    cli_syntax(h)->stx_active_mode = mode;
    return 1;
}

/*! Get syntax mode name
 * @param[in]     h       Clicon handle
 */
char *
cli_syntax_mode(clicon_handle h)
{
    cli_syntaxmode_t *csm;

    if ((csm = cli_syntax(h)->stx_active_mode) == NULL)
	return NULL;
    return csm->csm_name;
}

/*! Callback from cli_set_prompt(). Set prompt format for syntax mode
 * @param[in]  h       Clicon handle
 * @param[in]  name    Name of syntax mode 
 * @param[in]  prompt  Prompt format
 * @retval     0       OK
 * @retval    -1       Error
 */
int
cli_set_prompt(clicon_handle h,
	       const char   *name,
	       const char   *prompt)
{
    cli_syntaxmode_t *csm;

    if ((csm = syntax_mode_find(cli_syntax(h), name, 1)) == NULL)
	return -1;
    
    if (csm->csm_prompt != NULL){
	free(csm->csm_prompt);
	csm->csm_prompt = NULL;
    }
    if ((csm->csm_prompt = strdup(prompt)) == NULL){
	clicon_err(OE_UNIX, errno, "strdup");
	return -1;
    }
    return 0;
}

