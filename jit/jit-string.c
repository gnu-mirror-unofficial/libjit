/*
 * jit-string.c - String handling routines.
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

#include "jit-internal.h"
#include <config.h>
#ifdef HAVE_STRING_H
	#include <string.h>
#elif defined(HAVE_STRINGS_H)
	#include <strings.h>
#endif
#include <stdio.h>
#ifdef HAVE_STDARG_H
	#include <stdarg.h>
#elif HAVE_VARARGS_H
	#include <varargs.h>
#endif

/*@
 * @section String operations
 * @cindex String operations
 *
 * The following functions are provided to manipulate NULL-terminated
 * strings.  It is highly recommended that you use these functions in
 * preference to system functions, because the corresponding system
 * functions are extremely non-portable.
@*/

/*@
 * @deftypefun {unsigned int} jit_strlen ({const char *} str)
 * Returns the length of @code{str}.
 * @end deftypefun
@*/
unsigned int jit_strlen(const char *str)
{
#ifdef HAVE_STRLEN
	return (unsigned int)(strlen(str));
#else
	unsigned int len = 0;
	while(*str++ != '\0')
	{
		++len;
	}
	return len;
#endif
}

/*@
 * @deftypefun {char *} jit_strcpy ({char *} dest, {const char *} src)
 * Copy the string at @code{src} to @code{dest}.  Returns @code{dest}.
 * @end deftypefun
@*/
char *jit_strcpy(char *dest, const char *src)
{
#ifdef HAVE_STRCPY
	return strcpy(dest, src);
#else
	char ch;
	char *d = dest;
	while((ch = *src++) != '\0')
	{
		*d++ = ch;
	}
	*d = '\0';
	return dest;
#endif
}

/*@
 * @deftypefun {char *} jit_strcat ({char *} dest, {const char *} src)
 * Copy the string at @code{src} to the end of the string at @code{dest}.
 * Returns @code{dest}.
 * @end deftypefun
@*/
char *jit_strcat(char *dest, const char *src)
{
#ifdef HAVE_STRCAT
	return strcat(dest, src);
#else
	char ch;
	char *d = dest + jit_strlen(dest);
	while((ch = *src++) != '\0')
	{
		*d++ = ch;
	}
	*d = '\0';
	return dest;
#endif
}

/*@
 * @deftypefun {char *} jit_strncpy ({char *} dest, {const char *} src, {unsigned int} len)
 * Copy at most @code{len} characters from the string at @code{src} to
 * @code{dest}.  Returns @code{dest}.
 * @end deftypefun
@*/
char *jit_strncpy(char *dest, const char *src, unsigned int len)
{
#ifdef HAVE_STRNCPY
	return strncpy(dest, src, len);
#else
	char ch;
	char *d = dest;
	while(len > 0 && (ch = *src++) != '\0')
	{
		*d++ = ch;
		--len;
	}
	while(len > 0)
	{
		*d++ = '\0';
		--len;
	}
	return dest;
#endif
}

/*@
 * @deftypefun {char *} jit_strdup ({const char *} str)
 * Allocate a block of memory using @code{jit_malloc} and copy
 * @code{str} into it.  Returns NULL if @code{str} is NULL or there
 * is insufficient memory to perform the @code{jit_malloc} operation.
 * @end deftypefun
@*/
char *jit_strdup(const char *str)
{
	char *new_str;
	if(!str)
	{
		return 0;
	}
	new_str = jit_malloc(strlen(str) + 1);
	if(!new_str)
	{
		return 0;
	}
	strcpy(new_str, str);
	return new_str;
}

/*@
 * @deftypefun {char *} jit_strndup ({const char *} str, unsigned int len)
 * Allocate a block of memory using @code{jit_malloc} and copy at most
 * @code{len} characters of @code{str} into it.  The copied string is then
 * NULL-terminated.  Returns NULL if @code{str} is NULL or there
 * is insufficient memory to perform the @code{jit_malloc} operation.
 * @end deftypefun
@*/
char *jit_strndup(const char *str, unsigned int len)
{
	char *new_str;
	if(!str)
	{
		return 0;
	}
	new_str = jit_malloc(len + 1);
	if(!new_str)
	{
		return 0;
	}
	jit_memcpy(new_str, str, len);
	new_str[len] = '\0';
	return new_str;
}

/*@
 * @deftypefun int jit_strcmp ({const char *} str1, {const char *} str2)
 * Compare the two strings @code{str1} and @code{str2}, returning
 * a negative, zero, or positive value depending upon their relationship.
 * @end deftypefun
@*/
int jit_strcmp(const char *str1, const char *str2)
{
#ifdef HAVE_STRCMP
	return strcmp(str1, str2);
#else
	int ch1, ch2;
	for(;;)
	{
		ch1 = *str1++;
		ch2 = *str2++;
		if(ch1 != ch2 || !ch1 || !ch2)
		{
			break;
		}
	}
	return (ch1 - ch2);
#endif
}

/*@
 * @deftypefun int jit_strncmp ({const char *} str1, {const char *} str2, {unsigned int} len)
 * Compare the two strings @code{str1} and @code{str2}, returning
 * a negative, zero, or positive value depending upon their relationship.
 * At most @code{len} characters are compared.
 * @end deftypefun
@*/
int jit_strncmp(const char *str1, const char *str2, unsigned int len)
{
#ifdef HAVE_STRNCMP
	return strncmp(str1, str2, len);
#else
	int ch1, ch2;
	while(len > 0)
	{
		ch1 = *str1++;
		ch2 = *str2++;
		if(ch1 != ch2 || !ch1 || !ch2)
		{
			return (ch1 - ch2);
		}
		--len;
	}
	return 0;
#endif
}

/*@
 * @deftypefun int jit_stricmp ({const char *} str1, {const char *} str2)
 * Compare the two strings @code{str1} and @code{str2}, returning
 * a negative, zero, or positive value depending upon their relationship.
 * Instances of the English letters A to Z are converted into their
 * lower case counterparts before comparison.
 *
 * Note: this function is guaranteed to use English case comparison rules,
 * no matter what the current locale is set to.  Use @code{jit_stricoll} for
 * locale-sensitive string comparison.
 * @end deftypefun
@*/
int jit_stricmp(const char *str1, const char *str2)
{
	int ch1, ch2;
	for(;;)
	{
		ch1 = *str1++;
		ch2 = *str2++;
		if(ch1 >= 'A' && ch1 <= 'Z')
		{
			ch1 = ch1 - 'A' + 'a';
		}
		if(ch2 >= 'A' && ch2 <= 'Z')
		{
			ch2 = ch2 - 'A' + 'a';
		}
		if(ch1 != ch2 || !ch1 || !ch2)
		{
			break;
		}
	}
	return (ch1 - ch2);
}

/*@
 * @deftypefun int jit_strnicmp ({const char *} str1, {const char *} str2, {unsigned int} len)
 * Compare the two strings @code{str1} and @code{str2}, returning
 * a negative, zero, or positive value depending upon their relationship.
 * At most @code{len} characters are compared.  Instances of the English
 * letters A to Z are converted into their lower case counterparts
 * before comparison.
 *
 * Note: this function is guaranteed to use English case comparison rules,
 * no matter what the current locale is set to.  Use @code{jit_strnicoll} for
 * locale-sensitive string comparison.
 * @end deftypefun
@*/
int jit_strnicmp(const char *str1, const char *str2, unsigned int len)
{
	int ch1, ch2;
	while(len > 0)
	{
		ch1 = *str1++;
		ch2 = *str2++;
		if(ch1 >= 'A' && ch1 <= 'Z')
		{
			ch1 = ch1 - 'A' + 'a';
		}
		if(ch2 >= 'A' && ch2 <= 'Z')
		{
			ch2 = ch2 - 'A' + 'a';
		}
		if(ch1 != ch2 || !ch1 || !ch2)
		{
			return (ch1 - ch2);
		}
		--len;
	}
	return 0;
}

/*@
 * @deftypefun int jit_strcoll ({const char *} str1, {const char *} str2)
 * Compare the two strings @code{str1} and @code{str2}, returning
 * a negative, zero, or positive value depending upon their relationship.
 * This function uses locale-sensitive comparison rules, but case is
 * still considered significant.  If the system does not have locale
 * sensitive comparisons, this function will be identical to
 * @code{jit_strcmp}.
 * @end deftypefun
@*/
int jit_strcoll(const char *str1, const char *str2)
{
#if defined(HAVE_STRCOLL)
	return strcoll(str1, str2);
#elif defined(HAVE__STRCOLL)
	return _strcoll(str1, str2);
#elif defined(HAVE_STRCMP)
	return strcmp(str1, str2);
#else
	return jit_strcmp(str1, str2);
#endif
}

/*@
 * @deftypefun int jit_stricoll ({const char *} str1, {const char *} str2)
 * Compare the two strings @code{str1} and @code{str2}, returning
 * a negative, zero, or positive value depending upon their relationship.
 * This function uses locale-sensitive comparison rules, while ignoring
 * case.  If the system does not have locale sensitive comparisons, this
 * function will be identical to @code{jit_stricmp}.
 * @end deftypefun
@*/
int jit_stricoll(const char *str1, const char *str2)
{
#if defined(HAVE_STRICOLL)
	return stricoll(str1, str2);
#elif defined(HAVE__STRICOLL)
	return _stricoll(str1, str2);
#elif defined(HAVE_STRCASECMP)
	return strcasecmp(str1, str2);
#else
	return jit_stricmp(str1, str2);
#endif
}

/*@
 * @deftypefun int jit_strncoll ({const char *} str1, {const char *} str2, {unsigned int} len)
 * Compare the two strings @code{str1} and @code{str2}, returning
 * a negative, zero, or positive value depending upon their relationship.
 * At most @code{len} characters are compared, but it is otherwise
 * the same as @code{jit_strcoll}.
 * @end deftypefun
@*/
int jit_strncoll(const char *str1, const char *str2, unsigned int len)
{
#if defined(HAVE_STRNCOLL)
	return strncoll(str1, str2, len);
#elif defined(HAVE__STRNCOLL)
	return _strncoll(str1, str2, len);
#elif defined(HAVE_STRNCMP)
	return strncmp(str1, str2, len);
#else
	return jit_strncmp(str1, str2, len);
#endif
}

/*@
 * @deftypefun int jit_strnicoll ({const char *} str1, {const char *} str2, {unsigned int} len)
 * Compare the two strings @code{str1} and @code{str2}, returning
 * a negative, zero, or positive value depending upon their relationship.
 * At most @code{len} characters are compared, but it is otherwise
 * the same as @code{jit_stricoll}.
 * @end deftypefun
@*/
int jit_strnicoll(const char *str1, const char *str2, unsigned int len)
{
#if defined(HAVE_STRNICOLL)
	return strincoll(str1, str2, len);
#elif defined(HAVE__STRNICOLL)
	return _strnicoll(str1, str2, len);
#elif defined(HAVE_STRNCASECMP)
	return strncasecmp(str1, str2, len);
#else
	return jit_strnicmp(str1, str2, len);
#endif
}

/*@
 * @deftypefun {char *} jit_strchr ({const char *} str, int ch)
 * Search @code{str} for the first occurrence of @code{ch}.  Returns
 * the address where @code{ch} was found, or NULL if not found.
 * @end deftypefun
@*/
char *jit_strchr(const char *str, int ch)
{
#ifdef HAVE_STRCHR
	return strchr(str, ch);
#else
	char *s = (char *)str;
	for(;;)
	{
		if(*s == (char)ch)
		{
			return s;
		}
		else if(*s == '\0')
		{
			break;
		}
		++s;
	}
	return 0;
#endif
}

/*@
 * @deftypefun {char *} jit_strrchr ({const char *} str, int ch)
 * Search @code{str} for the first occurrence of @code{ch}, starting
 * at the end of the string.  Returns the address where @code{ch}
 * was found, or NULL if not found.
 * @end deftypefun
@*/
char *jit_strrchr(const char *str, int ch)
{
#ifdef HAVE_STRRCHR
	return strrchr(str, ch);
#else
	unsigned int len = jit_strlen(str);
	char *s = (char *)(str + len);
	while(len > 0)
	{
		--s;
		if(*s == (char)ch)
		{
			return s;
		}
		--len;
	}
	return 0;
#endif
}

int jit_sprintf(char *str, const char *format, ...)
{
	va_list va;
	int result;
#ifdef HAVE_STDARG_H
	va_start(va, format);
#else
	va_start(va);
#endif
#ifdef VSPRINTF
	result = vsprintf(str, format, va);
#else
	*str = '\0';
	result = 0;
#endif
	va_end(va);
	return result;
}

int jit_snprintf(char *str, unsigned int len, const char *format, ...)
{
	va_list va;
	int result;
#ifdef HAVE_STDARG_H
	va_start(va, format);
#else
	va_start(va);
#endif
#if defined(HAVE_VSNPRINTF)
	result = vsnprintf(str, len, format, va);
#elif defined(HAVE__VSNPRINTF)
	result = _vsnprintf(str, len, format, va);
#else
	*str = '\0';
	result = 0;
#endif
	va_end(va);
	return result;
}
