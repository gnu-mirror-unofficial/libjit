/*
 * <jit/jit-elf.h> - Routines to read and write ELF-format binaries.
 *
 * Copyright (C) 2004  Southern Storm Software, Pty Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef	_JIT_ELF_H
#define	_JIT_ELF_H

#include <jit/jit-common.h>

#ifdef	__cplusplus
extern	"C" {
#endif

/*
 * Opaque types that represent a loaded ELF binary in read or write mode.
 */
typedef struct jit_readelf *jit_readelf_t;
typedef struct jit_writeelf *jit_writeelf_t;

/*
 * Flags for "jit_readelf_open".
 */
#define	JIT_READELF_FLAG_FORCE	(1 << 0)	/* Force file to load */
#define	JIT_READELF_FLAG_DEBUG	(1 << 1)	/* Print debugging information */

/*
 * Result codes from "jit_readelf_open".
 */
#define	JIT_READELF_OK			0	/* File was opened successfully */
#define	JIT_READELF_CANNOT_OPEN	1	/* Could not open the file */
#define	JIT_READELF_NOT_ELF		2	/* Not an ELF-format binary */
#define	JIT_READELF_WRONG_ARCH	3	/* Wrong architecture for local system */
#define	JIT_READELF_BAD_FORMAT	4	/* ELF file, but badly formatted */
#define	JIT_READELF_MEMORY		5	/* Insufficient memory to load the file */

/*
 * External function declarations.
 */
int jit_readelf_open(jit_readelf_t *readelf, const char *filename, int flags);
void jit_readelf_close(jit_readelf_t readelf);
const char *jit_readelf_get_name(jit_readelf_t readelf);
void *jit_readelf_get_symbol(jit_readelf_t readelf, const char *name);
void *jit_readelf_get_section
	(jit_readelf_t readelf, const char *name, jit_nuint *size);
void *jit_readelf_get_section_by_type
	(jit_readelf_t readelf, jit_int type, jit_nuint *size);
void *jit_readelf_map_vaddr(jit_readelf_t readelf, jit_nuint vaddr);
unsigned int jit_readelf_num_needed(jit_readelf_t readelf);
const char *jit_readelf_get_needed(jit_readelf_t readelf, unsigned int index);
void jit_readelf_add_to_context(jit_readelf_t readelf, jit_context_t context);
int jit_readelf_resolve_all(jit_context_t context, int print_failures);

jit_writeelf_t jit_writeelf_create(const char *library_name);
void jit_writeelf_destroy(jit_writeelf_t writeelf);
int jit_writeelf_write(jit_writeelf_t writeelf, const char *filename);
int jit_writeelf_add_function
	(jit_writeelf_t writeelf, jit_function_t func, const char *name);
int jit_writeelf_add_needed
	(jit_writeelf_t writeelf, const char *library_name);
int jit_writeelf_write_section
	(jit_writeelf_t writeelf, const char *name, jit_int type,
	 const void *buf, unsigned int len, int discardable);

#ifdef	__cplusplus
};
#endif

#endif	/* _JIT_ELF_H */
