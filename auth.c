/*
 * Core authentication routines for pam_krb5.
 *
 * The actual authentication work is done here, either via password or via
 * PKINIT.  The only external interface is pamk5_password_auth, which calls
 * the appropriate internal functions.  This interface is used by both the
 * authentication and the password groups.
 *
 * Copyright 2005, 2006, 2007 Russ Allbery <rra@debian.org>
 * Copyright 2005 Andres Salomon <dilinger@debian.org>
 * Copyright 1999, 2000 Frank Cusack <fcusack@fcusack.com>
 * See LICENSE for licensing terms.
 */

#include "config.h"

#include <errno.h>
#include <krb5.h>
#include <pwd.h>
#ifdef HAVE_SECURITY_PAM_APPL_H
# include <security/pam_appl.h>
# include <security/pam_modules.h>
#elif HAVE_PAM_PAM_APPL_H
# include <pam/pam_appl.h>
# include <pam/pam_modules.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef HAVE_HX509_ERR_H
# include <hx509_err.h>
#endif

/*
 * If the PKINIT smart card error statuses aren't defined, define them to 0.
 * This will cause the right thing to happen with the logic around PKINIT.
 */
#ifndef HX509_PKCS11_NO_TOKEN
# define HX509_PKCS11_NO_TOKEN 0
#endif
#ifndef HX509_PKCS11_NO_SLOT
# define HX509_PKCS11_NO_SLOT 0
#endif

#include "internal.h"

/*
 * Fill in ctx->princ from the value of ctx->name or (if configured) from
 * prompting.  If we don't prompt and ctx->name contains an @-sign,
 * canonicalize it to a local account name.  If the canonicalization fails,
 * don't worry about it.  It may be that the application doesn't care.
 */
static krb5_error_code
parse_name(struct pam_args *args)
{
    struct context *ctx = args->ctx;
    krb5_context c = ctx->context;
    char *user = ctx->name;
    char *newuser;
    char kuser[65] = "";        /* MAX_USERNAME == 65 (MIT Kerberos 1.4.1). */
    size_t length;
    krb5_error_code k5_errno;
    int retval;

    /*
     * If configured to prompt for the principal, do that first.  Fall back on
     * using the local username as normal if prompting fails or if the user
     * just presses Enter.
     */
    if (args->prompt_princ) {
        retval = pamk5_conv(args, "Principal: ", PAM_PROMPT_ECHO_ON, &user);
        if (retval != PAM_SUCCESS)
            pamk5_debug_pam(args, "error getting principal", retval);
        if (*user == '\0') {
            free(user);
            user = ctx->name;
        }
    }

    /*
     * We don't just call krb5_parse_name so that we can work around a bug in
     * MIT Kerberos versions prior to 1.4, which store the realm in a static
     * variable inside the library and don't notice changes.  If no realm is
     * specified and a realm is set in our arguments, append the realm to
     * force krb5_parse_name to do the right thing.
     */
    if (args->realm != NULL && strchr(user, '@') == NULL) {
        length = strlen(user) + 1 + strlen(args->realm) + 1;
        newuser = malloc(length);
        if (newuser == NULL)
            return KRB5_CC_NOMEM;
        snprintf(newuser, length, "%s@%s", user, args->realm);
        if (user != ctx->name)
            free(user);
        user = newuser;
    }
    k5_errno = krb5_parse_name(c, user, &ctx->princ);
    if (user != ctx->name)
        free(user);

    /*
     * Now that we have a principal to call krb5_aname_to_localname, we can
     * canonicalize ctx->name to a local name.  We do this even if we were
     * explicitly prompting for a principal, but we use ctx->name to generate
     * the local username, not the principal name.  It's unlikely, and would
     * be rather weird, if the user were to specify a principal name for the
     * username and then enter a different username at the principal prompt,
     * but this behavior seems to make the most sense.
     */
    if (k5_errno == 0 && strchr(ctx->name, '@') != NULL) {
        if (krb5_aname_to_localname(c, ctx->princ, sizeof(kuser), kuser) != 0)
            return 0;
        user = strdup(kuser);
        if (user == NULL) {
            pamk5_error(args, "cannot allocate memory: %s", strerror(errno));
            return 0;
        }
        free(ctx->name);
        ctx->name = user;
    }
    return k5_errno;
}


/*
 * Set initial credential options based on our configuration information, and
 * using the Heimdal call to set initial credential options if it's available.
 * This function is used both for regular password authentication and for
 * PKINIT.
 *
 * Takes a flag indicating whether we're getting tickets for a specific
 * service.  If so, we don't try to get forwardable, renewable, or proxiable
 * tickets.
 */
static void
set_credential_options(struct pam_args *args, krb5_get_init_creds_opt *opts,
                       int service)
{
#ifdef HAVE_KRB5_GET_INIT_CREDS_OPT_SET_DEFAULT_FLAGS
    krb5_get_init_creds_opt_set_default_flags(args->ctx->context, "pam",
                                              args->realm_data, opts);
#endif
    if (!service) {
        if (args->forwardable)
            krb5_get_init_creds_opt_set_forwardable(opts, 1);
        if (args->lifetime != 0)
            krb5_get_init_creds_opt_set_tkt_life(opts, args->lifetime);
        if (args->renew_lifetime != 0)
            krb5_get_init_creds_opt_set_renew_life(opts, args->renew_lifetime);
    } else {
        krb5_get_init_creds_opt_set_forwardable(opts, 0);
        krb5_get_init_creds_opt_set_proxiable(opts, 0);
        krb5_get_init_creds_opt_set_renew_life(opts, 0);
    }
#ifdef HAVE_KRB5_GET_INIT_CREDS_OPT_SET_PA
    if (args->try_pkinit) {
        if (args->pkinit_user != NULL)
            krb5_get_init_creds_opt_set_pa(args->ctx->context, opts,
                "X509_user_identity", args->pkinit_user);
        if (args->pkinit_anchors != NULL)
            krb5_get_init_creds_opt_set_pa(args->ctx->context, opts,
                "X509_anchors", args->pkinit_anchors);
        if (args->preauth_opt != NULL && args->preauth_opt_count > 0) {
            int i;
            char *name, *value;
            char save;

            for (i = 0; i < args->preauth_opt_count; i++) {
                name = args->preauth_opt[i];
                if (name == NULL)
                    continue;
                value = strchr(name, '=');
                if (value != NULL) {
                    save = *value;
                    *value = '\0';
                    value++;
                }
                krb5_get_init_creds_opt_set_pa(args->ctx->context, opts,
                    name, (value != NULL) ? value : "yes");
                if (value != NULL)
                    value[-1] = save;
            }
        }
    }
#endif /* HAVE_KRB5_GET_INIT_CREDS_OPT_SET_PA */
}


/*
 * Used to support trying each principal in the .k5login file.  Read through
 * each line that parses correctly as a principal and use the provided
 * password to try to authenticate as that user.  If at any point we succeed,
 * fill out creds, set princ to the successful principal in the context, and
 * return PAM_SUCCESS.  Otherwise, return PAM_AUTH_ERR for a general
 * authentication error or PAM_SERVICE_ERR for a system error.  If
 * PAM_AUTH_ERR is returned, retval will be filled in with the Kerberos error
 * if available, 0 otherwise.
 */
static int
k5login_password_auth(struct pam_args *args, krb5_creds *creds,
                      krb5_get_init_creds_opt *opts, const char *service,
                      char *pass, int *retval)
{
    struct context *ctx = args->ctx;
    char *filename = NULL;
    char line[BUFSIZ];
    size_t len;
    FILE *k5login;
    struct passwd *pwd;
    struct stat st;
    int k5_errno;
    krb5_principal princ;

    /*
     * C sucks at string manipulation.  Generate the filename for the user's
     * .k5login file.  If the user doesn't exist, the .k5login file doesn't
     * exist, or the .k5login file cannot be read, fall back on the easy way
     * and assume ctx->princ is already set properly.
     */
    pwd = pamk5_compat_getpwnam(args, ctx->name);
    if (pwd != NULL) {
        len = strlen(pwd->pw_dir) + strlen("/.k5login");
        filename = malloc(len + 1);
    }
    if (filename != NULL) {
        strncpy(filename, pwd->pw_dir, len);
        filename[len] = '\0';
        strncat(filename, "/.k5login", len - strlen(pwd->pw_dir));
    }
    if (pwd == NULL || filename == NULL || access(filename, R_OK) != 0) {
        *retval = krb5_get_init_creds_password(ctx->context, creds,
                     ctx->princ, pass, pamk5_prompter_krb5, args, 0,
                     (char *) service, opts);
        return (*retval == 0) ? PAM_SUCCESS : PAM_AUTH_ERR;
    }

    /*
     * Make sure the ownership on .k5login is okay.  The user must own their
     * own .k5login or it must be owned by root.  If that fails, set the
     * Kerberos error code to errno.
     */
    k5login = fopen(filename, "r");
    free(filename);
    if (k5login == NULL) {
        *retval = errno;
        return PAM_AUTH_ERR;
    }
    if (fstat(fileno(k5login), &st) != 0) {
        *retval = errno;
        goto fail;
    }
    if (st.st_uid != 0 && (st.st_uid != pwd->pw_uid)) {
        *retval = errno;
        goto fail;
    }

    /*
     * Parse the .k5login file and attempt authentication for each principal.
     * Ignore any lines that are too long or that don't parse into a Kerberos
     * principal.  Assume an invalid password error if there are no valid
     * lines in .k5login.
     */
    *retval = KRB5KRB_AP_ERR_BAD_INTEGRITY;
    while (fgets(line, BUFSIZ, k5login) != NULL) {
        len = strlen(line);
        if (line[len - 1] != '\n') {
            while (fgets(line, BUFSIZ, k5login) != NULL) {
                len = strlen(line);
                if (line[len - 1] == '\n')
                    break;
            }
            continue;
        }
        line[len - 1] = '\0';
        k5_errno = krb5_parse_name(ctx->context, line, &princ);
        if (k5_errno != 0)
            continue;

        /* Now, attempt to authenticate as that user. */
        *retval = krb5_get_init_creds_password(ctx->context, creds,
                     princ, pass, pamk5_prompter_krb5, args, 0,
                     (char *) service, opts);

        /*
         * If that worked, update ctx->princ and return success.  Otherwise,
         * continue on to the next line.
         */
        if (*retval == 0) {
            if (ctx->princ)
                krb5_free_principal(ctx->context, ctx->princ);
            ctx->princ = princ;
            fclose(k5login);
            return PAM_SUCCESS;
        }
        krb5_free_principal(ctx->context, princ);
    }

fail:
    fclose(k5login);
    return PAM_AUTH_ERR;
}


#if HAVE_KRB5_HEIMDAL && HAVE_KRB5_GET_INIT_CREDS_OPT_SET_PKINIT
/*
 * Attempt authentication via PKINIT.  Currently, this uses an API specific to
 * Heimdal.  Once MIT Kerberos supports PKINIT, some of the details may need
 * to move into the compat layer.
 *
 * Some smart card readers require the user to enter the PIN at the keyboard
 * after inserting the smart card.  Others have a pad on the card and no
 * prompting by PAM is required.  The Kerberos library prompting functions
 * should be able to work out which is required.
 *
 * PKINIT is just one of many pre-authentication mechanisms that could be
 * used.  It's handled separately because of possible smart card interactions
 * and the possibility that some users may be authenticated via PKINIT and
 * others may not.
 *
 * Takes the same arguments as pamk5_password_auth and returns a
 * krb5_error_code.  If successful, the credentials will be stored in creds.
 */
static krb5_error_code
pkinit_auth(struct pam_args *args, const char *service, krb5_creds **creds)
{
    struct context *ctx = args->ctx;
    krb5_get_init_creds_opt *opts = NULL;
    krb5_error_code retval;
    char *dummy = NULL;

    /*
     * We may not be able to dive directly into the PKINIT functions because
     * the user may not have a chance to enter the smart card.  For example,
     * gnome-screensaver jumps into PAM as soon as the mouse is moved and
     * expects to be prompted for a password, which may not happen if the
     * smart card is the type that has a pad for the PIN on the card.
     *
     * Allow the user to set pkinit_prompt as an option.  If set, we tell the
     * user they need to insert the card.
     *
     * We always ignore the input.  If the user wants to use a password
     * instead, they'll be prompted later when the PKINIT code discovers that
     * no smart card is available.
     */
    if (args->pkinit_prompt) {
        pamk5_conv(args,
                   args->use_pkinit
                       ? "Insert smart card and press Enter:"
                       : "Insert smart card if desired, then press Enter:",
                   PAM_PROMPT_ECHO_OFF, &dummy);
    }

    /*
     * Set credential options.  We have to use the allocated version of the
     * credential option struct to store the PKINIT options.
     */
    *creds = calloc(1, sizeof(krb5_creds));
    if (*creds == NULL)
        return ENOMEM;
    retval = krb5_get_init_creds_opt_alloc(ctx->context, &opts);
    if (retval != 0)
        return retval;
    set_credential_options(args, opts, service != NULL);
#ifdef HAVE_KRB5_GET_INIT_CREDS_OPT_SET_PKINIT_11_ARGS
    retval = krb5_get_init_creds_opt_set_pkinit(ctx->context, opts,
                  ctx->princ, args->pkinit_user, args->pkinit_anchors, NULL,
                  NULL, 0, pamk5_prompter_krb5, args, NULL);
#else
    retval = krb5_get_init_creds_opt_set_pkinit(ctx->context, opts,
                  ctx->princ, args->pkinit_user, args->pkinit_anchors, 0,
                  pamk5_prompter_krb5, args, NULL);
#endif
    if (retval != 0)
        goto done;

    /* Finally, do the actual work and return the results. */
    retval = krb5_get_init_creds_password(ctx->context, *creds, ctx->princ,
                 NULL, pamk5_prompter_krb5, args, 0, (char *) service, opts);

done:
    pamk5_compat_opt_free(ctx->context, opts);
    if (retval != 0) {
        krb5_free_cred_contents(ctx->context, *creds);
        free(*creds);
        *creds = NULL;
    }
    return retval;
}
#endif /* HAVE_KRB5_HEIMDAL && HAVE_KRB5_GET_INIT_CREDS_OPT_SET_PKINIT */


/*
 * Try to verify credentials by obtaining and checking a service ticket.  This
 * is required to verify that no one is spoofing the KDC, but requires read
 * access to a keytab with a valid key.  By default, the Kerberos library will
 * silently succeed if no verification keys are available, but the user can
 * change this by setting verify_ap_req_nofail in [libdefaults] in
 * /etc/krb5.conf.
 *
 * The MIT Kerberos implementation of krb5_verify_init_creds hardwires the
 * host key for the local system as the desired principal if no principal is
 * given.  If we have an explicitly configured keytab, instead read that
 * keytab, find the first principal in that keytab, and use that.
 *
 * Returns a Kerberos status code (0 for success).
 */
static krb5_error_code
verify_creds(struct pam_args *args, krb5_creds *creds)
{
    krb5_verify_init_creds_opt opts;
    krb5_keytab keytab = NULL;
    krb5_kt_cursor cursor;
    int cursor_valid = 0;
    krb5_keytab_entry entry;
    krb5_principal princ = NULL;
    const char *message;
    krb5_error_code retval;
    krb5_context c = args->ctx->context;

    memset(&entry, 0, sizeof(entry));
    krb5_verify_init_creds_opt_init(&opts);
    if (args->keytab) {
        retval = krb5_kt_resolve(c, args->keytab, &keytab);
        if (retval != 0) {
            message = pamk5_compat_get_error(c, retval);
            pamk5_error(args, "cannot open keytab %s: %s", args->keytab,
                        message);
            pamk5_compat_free_error(c, message);
            keytab = NULL;
        }
        if (retval == 0)
            retval = krb5_kt_start_seq_get(c, keytab, &cursor);
        if (retval == 0) {
            cursor_valid = 1;
            retval = krb5_kt_next_entry(c, keytab, &entry, &cursor);
        }
        if (retval == 0)
            retval = krb5_copy_principal(c, entry.principal, &princ);
        if (retval != 0) {
            message = pamk5_compat_get_error(c, retval);
            pamk5_error(args, "error reading keytab %s: %s", args->keytab,
                        message);
            pamk5_compat_free_error(c, message);
        }
        if (entry.principal != NULL)
            pamk5_compat_free_keytab_contents(c, &entry);
        if (cursor_valid)
            krb5_kt_end_seq_get(c, keytab, &cursor);
    }
    retval = krb5_verify_init_creds(c, creds, princ, keytab, NULL, &opts);
    if (retval != 0)
        pamk5_error_krb5(args, "credential verification failed", retval);
    if (princ != NULL)
        krb5_free_principal(c, princ);
    if (keytab != NULL)
        krb5_kt_close(c, keytab);
    return retval;
}


/*
 * Prompt the user for a password and authenticate the password with the KDC.
 * If correct, fill in creds with the obtained TGT or ticket.  service, if
 * non-NULL, specifies the service to get tickets for; the only interesting
 * non-null case is kadmin/changepw for changing passwords.  Therefore, if it
 * is non-null, we look for the password in PAM_OLDAUTHOK and save it there
 * instead of using PAM_AUTHTOK.
 */
int
pamk5_password_auth(struct pam_args *args, const char *service,
                    krb5_creds **creds)
{
    struct context *ctx;
    krb5_get_init_creds_opt *opts = NULL;
    int retval, retry, success, creds_valid;
    char *pass = NULL;
    int authtok = (service == NULL) ? PAM_AUTHTOK : PAM_OLDAUTHTOK;

    /* Sanity check and initialization. */
    if (args->ctx == NULL)
        return PAM_SERVICE_ERR;
    ctx = args->ctx;

    /* Fill in the principal to authenticate as. */
    retval = parse_name(args);
    if (retval != 0) {
        pamk5_debug_krb5(args, "krb5_parse_name", retval);
        return PAM_SERVICE_ERR;
    }

    /* Log the principal we're attempting to authenticate as. */
    if (args->debug && !args->search_k5login) {
        char *principal;

        retval = krb5_unparse_name(ctx->context, ctx->princ, &principal);
        if (retval != 0)
            pamk5_debug_krb5(args, "krb5_unparse_name", retval);
        else {
            pamk5_debug(args, "attempting authentication as %s", principal);
            free(principal);
        }
    }

    /*
     * If PKINIT is available and we were configured to attempt it, try
     * authenticating with PKINIT first.  Otherwise, fail all authentication
     * if PKINIT is not available and use_pkinit was set.  Fake an error code
     * that gives an approximately correct error message.
     */
#if HAVE_KRB5_HEIMDAL && HAVE_KRB5_GET_INIT_CREDS_OPT_SET_PKINIT
    if (args->use_pkinit || args->try_pkinit) {
        retval = pkinit_auth(args, service, creds);
        if (retval == 0)
            goto done;
        if (retval != HX509_PKCS11_NO_TOKEN && retval != HX509_PKCS11_NO_SLOT)
            goto done;
        if (retval != 0 && args->use_pkinit)
            goto done;
    }
#else
    if (args->use_pkinit) {
        retval = KRB5_KDC_UNREACH;
        goto done;
    }
#endif

    /* Allocate cred structure and set credential options. */
    *creds = calloc(1, sizeof(krb5_creds));
    if (*creds == NULL) {
        pamk5_error(args, "cannot allocate memory: %s", strerror(errno));
        return PAM_SERVICE_ERR;
    }
    creds_valid = 0;
    retval = pamk5_compat_opt_alloc(ctx->context, &opts);
    if (retval != 0) {
        pamk5_error_krb5(args, "cannot allocate credential options", retval);
        goto done;
    }
    set_credential_options(args, opts, service != NULL);

    /*
     * If try_first_pass or use_first_pass is set, grab the old password (if
     * set) instead of prompting.  If try_first_pass is set, and the old
     * password doesn't work, prompt for the password (loop).
     *
     * pass is a char * so &pass is a char **, which due to the stupid C type
     * rules isn't convertable to a const void **.  If we cast directly to a
     * const void **, gcc complains about type-punned pointers, even though
     * void and char shouldn't worry about that rule.  So cast it to a void *
     * to turn off type-checking entirely.
     */
    retry = args->try_first_pass ? 1 : 0;
    if (args->try_first_pass || args->use_first_pass || args->use_authtok)
        retval = pam_get_item(args->pamh, authtok, (void *) &pass);
    if (args->use_authtok && (retval != PAM_SUCCESS || pass == NULL)) {
        pamk5_debug_pam(args, "no stored password", retval);
        retval = PAM_SERVICE_ERR;
        goto done;
    }
    do {
        if (pass == NULL) {
            const char *prompt = (service == NULL) ? NULL : "Current";

            retry = 0;
            retval = pamk5_get_password(args, prompt, &pass);
            if (retval != PAM_SUCCESS) {
                pamk5_debug_pam(args, "error getting password", retval);
                retval = PAM_SERVICE_ERR;
                goto done;
            }

            /* Set this for the next PAM module's try_first_pass. */
            retval = pam_set_item(args->pamh, authtok, pass);
            memset(pass, 0, strlen(pass));
            free(pass);
            if (retval != PAM_SUCCESS) {
                pamk5_debug_pam(args, "error storing password", retval);
                retval = PAM_SERVICE_ERR;
                goto done;
            }
            pam_get_item(args->pamh, authtok, (void *) &pass);
        }

        /* Get a TGT */
        if (args->search_k5login) {
            success = k5login_password_auth(args, *creds, opts, service,
                          pass, &retval);
        } else {
            retval = krb5_get_init_creds_password(ctx->context, *creds,
                          ctx->princ, pass, pamk5_prompter_krb5, args, 0,
                          (char *) service, opts);
            success = (retval == 0) ? PAM_SUCCESS : PAM_AUTH_ERR;
        }
        if (success == PAM_SUCCESS) {
            if (retval != 0)
                goto done;
            break;
        }
        pass = NULL;
    } while (retry && retval == KRB5KRB_AP_ERR_BAD_INTEGRITY);
    if (retval != 0)
        pamk5_debug_krb5(args, "krb5_get_init_creds_password", retval);
    else
        creds_valid = 1;

done:
    /*
     * If we think we succeeded, whether through the regular path or via
     * PKINIT, try to verify the credentials.  Don't do this if we're
     * authenticating for password changes (or any other case where we're not
     * getting a TGT).  We can't get a service ticket from a kadmin/changepw
     * ticket.
     */
    if (retval == 0 && service == NULL)
        retval = verify_creds(args, *creds);

    /*
     * If we failed, free any credentials we have sitting around and return
     * the appropriate PAM error code.
     */ 
    if (retval == 0)
        retval = PAM_SUCCESS;
    else {
        if (*creds != NULL) {
            if (creds_valid)
                krb5_free_cred_contents(ctx->context, *creds);
            free(*creds);
            *creds = NULL;
        }
        switch (retval) {
        case KRB5KDC_ERR_C_PRINCIPAL_UNKNOWN:
            retval = PAM_USER_UNKNOWN;
            break;
        case KRB5KDC_ERR_KEY_EXP:
            retval = PAM_NEW_AUTHTOK_REQD;
            break;
        case KRB5KDC_ERR_NAME_EXP:
            retval = PAM_ACCT_EXPIRED;
            break;
        case KRB5_KDC_UNREACH:
        case KRB5_REALM_CANT_RESOLVE:
            retval = PAM_AUTHINFO_UNAVAIL;
            break;
        default:
            retval = PAM_AUTH_ERR;
            break;
        }
    }
    if (opts != NULL)
        pamk5_compat_opt_free(ctx->context, opts);
    return retval;
}