/*
 * jit-thread.c - Internal thread management routines for libjit.
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

#if defined(JIT_THREADS_PTHREAD)

/*
 * The thread-specific key to use to fetch the control object.
 */
static pthread_key_t control_key;

/*
 * Initialize the pthread support routines.  Only called once.
 */
static void init_pthread(void)
{
	/* Allocate a thread-specific variable for the JIT's thread
	   control object, and arrange for it to be freed when the
	   thread exits or is otherwise terminated */
	pthread_key_create(&control_key, jit_free);
}

#elif defined(JIT_THREADS_WIN32)

/*
 * The thread-specific key to use to fetch the control object.
 */
static DWORD control_key;

/*
 * Initialize the Win32 thread support routines.  Only called once.
 */
static void init_win32_thread(void)
{
	control_key = TlsAlloc();
}

#else /* No thread package */

/*
 * The control object for the only thread in the system.
 */
static void *control_object = 0;

#endif /* No thread package */

void _jit_thread_init(void)
{
#if defined(JIT_THREADS_PTHREAD)
	static pthread_once_t once_control = PTHREAD_ONCE_INIT;
	pthread_once(&once_control, init_pthread);
#elif defined(JIT_THREADS_WIN32)
	static LONG volatile once_control = 0;
	if(!InterlockedExchange((PLONG)&once_control, 1))
	{
		init_win32_thread();
	}
#endif
}

static void *get_raw_control(void)
{
	_jit_thread_init();
#if defined(JIT_THREADS_PTHREAD)
	return pthread_getspecific(control_key);
#elif defined(JIT_THREADS_WIN32)
	return (void *)(TlsGetValue(control_key));
#else
	return control_object;
#endif
}

static void set_raw_control(void *obj)
{
	_jit_thread_init();
#if defined(JIT_THREADS_PTHREAD)
	pthread_setspecific(control_key, obj);
#elif defined(JIT_THREADS_WIN32)
	TlsSetValue(control_key, obj);
#else
	control_object = obj;
#endif
}

jit_thread_control_t _jit_thread_get_control(void)
{
	jit_thread_control_t control;
	control = (jit_thread_control_t)get_raw_control();
	if(!control)
	{
		control = jit_cnew(struct jit_thread_control);
		if(control)
		{
			set_raw_control(control);
		}
	}
	return control;
}

jit_thread_id_t _jit_thread_current_id(void)
{
#if defined(JIT_THREADS_PTHREAD)
	return pthread_self();
#elif defined(JIT_THREADS_WIN32)
	return GetCurrentThread();
#else
	/* There is only one thread, so lets give it an identifier of 1 */
	return 1;
#endif
}
