# TRIS_PROG_EGREP
# -------------
m4_ifndef([TRIS_PROG_EGREP], [AC_DEFUN([TRIS_PROG_EGREP],
[AC_CACHE_CHECK([for egrep], [ac_cv_prog_egrep],
   [if echo a | (grep -E '(a|b)') >/dev/null 2>&1
    then ac_cv_prog_egrep='grep -E'
    else ac_cv_prog_egrep='egrep'
    fi])
 EGREP=$ac_cv_prog_egrep
 AC_SUBST([EGREP])
])]) # TRIS_PROG_EGREP
