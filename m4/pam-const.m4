dnl pam-const.m4 -- Determine whether PAM uses const in prototypes.
dnl
dnl Linux marks several PAM arguments const, including the argument to
dnl pam_get_item and some arguments to conversation functions, which Solaris
dnl doesn't.  This test tries to determine which style is in use to select
dnl whether to declare variables const in order to avoid compiler warnings.
dnl
dnl Since this is just for compiler warnings, it's not horribly important if
dnl we guess wrong.  This test is ugly, but it seems to work.
dnl
dnl Contributed by Markus Moeller.
dnl
dnl Copyright 2007 Russ Allbery <rra@debian.org>
dnl Copyright 2007 Markus Moeller
dnl See LICENSE for licensing terms.

AC_DEFUN([RRA_HEADER_PAM_CONST],
[AC_CACHE_CHECK([whether PAM prefers const], [rra_cv_header_pam_const],
    [AC_EGREP_HEADER([const void \*\* *item], [security/pam_appl.h],
        [rra_cv_header_pam_const=yes], [rra_cv_header_pam_const=no])])
AS_IF([test x"$rra_cv_header_pam_const" = xyes],
    [rra_header_pam_const=const], [rra_header_pam_const=])
AC_DEFINE_UNQUOTED([PAM_CONST], [$rra_header_pam_const],
    [Define to const if PAM uses const in pam_get_item, empty otherwise.])])
