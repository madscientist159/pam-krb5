/*
 * Support for FAST (Flexible Authentication Secure Tunneling).
 *
 * FAST is a mechanism to protect Kerberos against password guessing attacks
 * and provide other security improvements.  It requires existing credentials
 * to protect the initial preauthentication exchange.  These can come either
 * from a ticket cache for another principal or via anonymous PKINIT.
 *
 * Written by Russ Allbery <rra@stanford.edu>
 * Contributions from Sam Hartman and Yair Yarom
 * Copyright 2010, 2012
 *     The Board of Trustees of the Leland Stanford Junior University
 *
 * See LICENSE for licensing terms.
 */

#include <config.h>
#include <portable/krb5.h>
#include <portable/system.h>

#include <errno.h>

#include <internal.h>
#include <pam-util/args.h>
#include <pam-util/logging.h>


/*
 * Initialize an internal anonymous ticket cache with a random name and store
 * the resulting ticket cache in the ccache argument.  Returns a Kerberos
 * error code.
 */
#ifndef HAVE_KRB5_GET_INIT_CREDS_OPT_SET_ANONYMOUS

static krb5_error_code
cache_init_anonymous(struct pam_args *args, krb5_ccache *ccache UNUSED)
{
    putil_debug(args, "not built with anonymous FAST support");
    return KRB5KDC_ERR_BADOPTION;
}

#else /* HAVE_KRB5_GET_INIT_CREDS_OPT_SET_ANONYMOUS */

static krb5_error_code
cache_init_anonymous(struct pam_args *args, krb5_ccache *ccache)
{
    krb5_context c = args->config->ctx->context;
    krb5_error_code retval;
    krb5_principal princ = NULL;
    const char *dir;
    char *realm;
    char *path = NULL;
    int status;
    krb5_creds creds;
    bool creds_valid = false;
    krb5_get_init_creds_opt *opts = NULL;

    *ccache = NULL;
    memset(&creds, 0, sizeof(creds));

    /* Construct the anonymous principal name. */
    retval = krb5_get_default_realm(c, &realm);
    if (retval != 0) {
        putil_debug_krb5(args, retval, "cannot find realm for anonymous FAST");
        return retval;
    }
    retval = krb5_build_principal_ext(c, &princ, strlen(realm), realm,
                 strlen(KRB5_WELLKNOWN_NAME), KRB5_WELLKNOWN_NAME,
                 strlen(KRB5_ANON_NAME), KRB5_ANON_NAME, NULL);
    if (retval != 0) {
        krb5_free_default_realm(c, realm);
        putil_debug_krb5(args, retval, "cannot create anonymous principal");
        return retval;
    }
    krb5_free_default_realm(c, realm);

    /* Set up the credential cache the anonymous credentials. */
    dir = args->config->ccache_dir;
    if (strncmp("FILE:", args->config->ccache_dir, strlen("FILE:")) == 0)
        dir += strlen("FILE:");
    if (asprintf(&path, "%s/krb5cc_pam_armor_XXXXXX", dir) < 0) {
        putil_crit(args, "malloc failure: %s", strerror(errno));
        retval = errno;
        goto done;
    }
    status = pamk5_cache_mkstemp(args, path);
    if (status != PAM_SUCCESS) {
        retval = errno;
        goto done;
    }
    retval = krb5_cc_resolve(c, path, ccache);
    if (retval != 0) {
        putil_err_krb5(args, retval, "cannot create anonymous FAST ccache %s",
                       path);
        goto done;
    }

    /* Obtain the credentials. */
    retval = krb5_get_init_creds_opt_alloc(c, &opts);
    if (retval != 0) {
        putil_err_krb5(args, retval, "cannot create FAST credential options");
        goto done;
    }
    krb5_get_init_creds_opt_set_anonymous(opts, 1);
    krb5_get_init_creds_opt_set_tkt_life(opts, 60);
# ifdef HAVE_KRB5_GET_INIT_CREDS_OPT_SET_OUT_CCACHE
    krb5_get_init_creds_opt_set_out_ccache(c, opts, *ccache);
# endif
    retval = krb5_get_init_creds_password(c, &creds, princ, NULL, NULL, NULL,
                                          0, NULL, opts);
    if (retval != 0) {
        putil_debug_krb5(args, retval, "cannot obtain anonymous credentials"
                         " for FAST");
        goto done;
    }
    creds_valid = true;

    /*
     * If set_out_ccache was available, we're done.  Otherwise, we have to
     * manually set up the ticket cache.  Use the principal from the acquired
     * credentials when initializing the ticket cache, since the realm will
     * not match the realm of our input principal.
     */
# ifndef HAVE_KRB5_GET_INIT_CREDS_OPT_SET_OUT_CCACHE
    retval = krb5_cc_initialize(c, *ccache, creds.client);
    if (retval != 0) {
        putil_err_krb5(args, retval, "cannot initialize FAST ticket cache");
        goto done;
    }
    retval = krb5_cc_store_cred(c, *ccache, &creds);
    if (retval != 0) {
        putil_err_krb5(args, retval, "cannot store FAST credentials");
        goto done;
    }
# endif /* !HAVE_KRB5_GET_INIT_CREDS_OPT_SET_OUT_CCACHE */

 done:
    if (retval != 0 && *ccache != NULL) {
        krb5_cc_destroy(c, *ccache);
        *ccache = NULL;
    }
    if (princ != NULL)
        krb5_free_principal(c, princ);
    if (path != NULL)
        free(path);
    if (opts != NULL)
        krb5_get_init_creds_opt_free(c, opts);
    if (creds_valid)
        krb5_free_cred_contents(c, &creds);
    return retval;
}
#endif /* HAVE_KRB5_GET_INIT_CREDS_OPT_SET_ANONYMOUS */


/*
 * Set initial credential options for FAST if support is available.  For
 * non-anonymous FAST, we open the ticket cache and read the principal from it
 * first to ensure that the cache exists and contains credentials, and skip
 * setting the FAST cache if we cannot do that.
 */
#ifndef HAVE_KRB5_GET_INIT_CREDS_OPT_SET_FAST_CCACHE_NAME

void
pamk5_fast_setup(struct pam_args *args UNUSED,
                 krb5_get_init_creds_opt *opts UNUSED)
{
}

#else /* HAVE_KRB5_GET_INIT_CREDS_OPT_SET_FAST_CCACHE_NAME */

void
pamk5_fast_setup(struct pam_args *args, krb5_get_init_creds_opt *opts)
{
    krb5_context c = args->config->ctx->context;
    const char *cache = args->config->fast_ccache;
    krb5_error_code retval;
    krb5_principal princ;
    krb5_ccache ccache;
    bool valid = false;
    char *anon_cache = NULL;

    /*
     * Obtain the credential cache.  We may generate a new anonymous ticket
     * cache or we may use an existing ticket cache.  Try to use an existing
     * one first, and fall back on anonymous if that was configured.
     */
    if (cache != NULL) {
        retval = krb5_cc_resolve(c, cache, &ccache);
        if (retval != 0)
            putil_debug_krb5(args, retval, "failed resolving FAST ccache %s",
                             cache);
        else {
            retval = krb5_cc_get_principal(c, ccache, &princ);
            if (retval != 0)
                putil_debug_krb5(args, retval, "failed to get principal from"
                                 " FAST ccache %s", cache);
            else {
                valid = true;
                krb5_free_principal(c, princ);
            }
            krb5_cc_close(c, ccache);
        }
    }
    if (!valid && args->config->anon_fast) {
        retval = cache_init_anonymous(args, &ccache);
        if (retval != 0) {
            putil_debug_krb5(args, retval, "skipping anonymous FAST");
            return;
        }
        if (args->config->ctx->anon_fast_ccache != NULL)
            krb5_cc_destroy(c, args->config->ctx->anon_fast_ccache);
        args->config->ctx->anon_fast_ccache = ccache;
        retval = krb5_cc_get_full_name(c, ccache, &anon_cache);
        if (retval != 0)
            putil_debug_krb5(args, retval, "cannot get name of anonymous FAST"
                             " credential cache");
        else {
            valid = true;
            cache = anon_cache;
        }
    }
    if (!valid)
        goto done;

    /* We have a valid FAST ticket cache.  Set the option. */
    retval = krb5_get_init_creds_opt_set_fast_ccache_name(c, opts, cache);
    if (retval != 0)
        putil_err_krb5(args, retval, "failed to set FAST ccache");
    else if (anon_cache != NULL)
        putil_debug(args, "setting anonymous FAST credential cache to %s",
                    anon_cache);
    else
        putil_debug(args, "setting FAST credential cache to %s", cache);

done:
    if (anon_cache != NULL)
        krb5_free_string(c, anon_cache);
}

#endif /* HAVE_KRB5_GET_INIT_CREDS_OPT_SET_FAST_CCACHE_NAME */
