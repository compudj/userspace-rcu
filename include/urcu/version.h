#ifndef _URCU_VERSION_H
#define _URCU_VERSION_H

/*
 * Imported from glibc 2.27.9000 include/libc-symbols.h.
 *
 * Copyright (C) 1995-2018 Free Software Foundation, Inc.
 * Copyright (C) 2018 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * The GNU C Library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
*
 * The GNU C Library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
*
 * You should have received a copy of the GNU Lesser General Public
 * License along with the GNU C Library; if not, see
 * <http://www.gnu.org/licenses/>.
 */

/* TODO: require binutils 2.22+ for powerpc64 (no dot symbol). */

#define URCU_SYM_EVAL1(x)	x
#define URCU_SYM_EVAL(x)	URCU_SYM_EVAL1(x)

#define URCU_SYM_STR1(x)	#x
#define URCU_SYM_STR(x)		URCU_SYM_STR1(x)

#ifndef __ASSEMBLER__

/*
 * GCC understands weak symbols and aliases; use its interface where
 * possible, instead of embedded assembly language.
 */

/* Define ALIASNAME as a strong alias for NAME.  */
# define urcu_strong_alias(name, aliasname) _urcu_strong_alias(name, aliasname)
# define _urcu_strong_alias(name, aliasname) \
  extern __typeof (name) aliasname __attribute__ ((alias (#name)));

#endif

/*
 * Use urcu_symbol_version_reference to specify the version a symbol
 * reference should link to.  Use urcu_symbol_version or
 * urcu_default_symbol_version for the definition of a versioned symbol.
 * The difference is that the latter is a no-op in non-shared builds.
 */

#ifdef __ASSEMBLER__
# define urcu_symbol_version_reference(real, name, version) \
     .symver URCU_SYM_EVAL(real), URCU_SYM_EVAL(name)##@URCU_##URCU_SYM_EVAL(version)
#else  /* !__ASSEMBLER__ */
# define urcu_symbol_version_reference(real, name, version) \
  __asm__ (".symver " URCU_SYM_STR(real) "," URCU_SYM_STR(name) "@URCU_" URCU_SYM_STR(version))
#endif

#ifdef SHARED
# define urcu_symbol_version(real, name, version) \
  urcu_symbol_version_reference(real, name, version)
# define urcu_default_symbol_version(real, name, version) \
     _urcu_default_symbol_version(real, name, version)
# ifdef __ASSEMBLER__
#  define _urcu_default_symbol_version(real, name, version) \
     .symver URCU_SYM_EVAL(real), URCU_SYM_EVAL(name)##@##@URCU_##URCU_SYM_EVAL(version)
# else
#  define _urcu_default_symbol_version(real, name, version) \
     __asm__ (".symver " URCU_SYM_STR(real) "," URCU_SYM_STR(name) "@@URCU_" URCU_SYM_STR(version))
# endif
#else
# define urcu_symbol_version(real, name, version)
# define urcu_default_symbol_version(real, name, version) \
  urcu_strong_alias(real, name)
#endif

#endif /* _URCU_VERSION_H */
