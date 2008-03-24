/*
 * jit-rules-x86-64.c - Rules that define the characteristics of the x86_64.
 *
 * Copyright (C) 2008  Southern Storm Software, Pty Ltd.
 *
 * This file is part of the libjit library.
 *
 * The libjit library is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation, either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * The libjit library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with the libjit library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include "jit-internal.h"
#include "jit-rules.h"
#include "jit-apply-rules.h"

#if defined(JIT_BACKEND_X86_64)

#include "jit-gen-x86-64.h"
#include "jit-reg-alloc.h"
#include "jit-setjmp.h"
#include <stdio.h>

/*
 * Pseudo register numbers for the x86_64 registers. These are not the
 * same as the CPU instruction register numbers.  The order of these
 * values must match the order in "JIT_REG_INFO".
 */
#define	X86_64_REG_RAX		0
#define	X86_64_REG_RCX		1
#define	X86_64_REG_RDX		2
#define	X86_64_REG_RBX		3
#define	X86_64_REG_RSI		4
#define	X86_64_REG_RDI		5
#define	X86_64_REG_R8		6
#define	X86_64_REG_R9		7
#define	X86_64_REG_R10		8
#define	X86_64_REG_R11		9
#define	X86_64_REG_R12		10
#define	X86_64_REG_R13		11
#define	X86_64_REG_R14		12
#define	X86_64_REG_R15		13
#define	X86_64_REG_RBP		14
#define	X86_64_REG_RSP		15
#define	X86_64_REG_XMM0		16
#define	X86_64_REG_XMM1		17
#define	X86_64_REG_XMM2		18
#define	X86_64_REG_XMM3		19
#define	X86_64_REG_XMM4		20
#define	X86_64_REG_XMM5		21
#define	X86_64_REG_XMM6		22
#define	X86_64_REG_XMM7		23
#define	X86_64_REG_XMM8		24
#define	X86_64_REG_XMM9		25
#define	X86_64_REG_XMM10	26
#define	X86_64_REG_XMM11	27
#define	X86_64_REG_XMM12	28
#define	X86_64_REG_XMM13	29
#define	X86_64_REG_XMM14	30
#define	X86_64_REG_XMM15	31
#define	X86_64_REG_ST0		32
#define	X86_64_REG_ST1		33
#define	X86_64_REG_ST2		34
#define	X86_64_REG_ST3		35
#define	X86_64_REG_ST4		36
#define	X86_64_REG_ST5		37
#define	X86_64_REG_ST6		38
#define	X86_64_REG_ST7		39

/*
 * Determine if a pseudo register number is general, xmm or fpu.
 */
#define	IS_GENERAL_REG(reg)	(((reg) & ~0x0f) == 0)
#define	IS_XMM_REG(reg)		(((reg) & ~0x0f) == 0x10)
#define	IS_FPU_REG(reg)		(((reg) & ~0x0f) == 0x20)

/*
 * Scratch register, that is used for calls via register and
 * for loading the exception pc to the setjmp buffer.
 * This register *MUST* not be used for parameter passing and
 * *MUST* not be a callee saved register.
 * For SysV abi R11 is perfect.
 */
#define X86_64_SCRATCH X86_64_R11

/*
 * Set this definition to 1 if the OS supports the SysV red zone.
 * This is a 128 byte area below the stack pointer that is guaranteed
 * to be not modified by interrupts or signal handlers.
 * This allows us to use a temporary area on the stack without
 * having to modify the stack pointer saving us two instructions.
 * TODO: Make this a configure switch.
 */
#define HAVE_RED_ZONE 1

/*
 * X86_64 argument types as specified in the X86_64 SysV ABI.
 */
#define X86_64_ARG_NO_CLASS		0x00
#define X86_64_ARG_INTEGER		0x01
#define X86_64_ARG_MEMORY		0x02
#define X86_64_ARG_SSE			0x11
#define X86_64_ARG_SSEUP		0x12
#define X86_64_ARG_X87			0x21
#define X86_64_ARG_X87UP		0x22

#define X86_64_ARG_IS_SSE(arg)	(((arg) & 0x10) != 0)
#define X86_64_ARG_IS_X87(arg)	(((arg) & 0x20) != 0)

/*
 * The granularity of the stack
 */
#define STACK_SLOT_SIZE	sizeof(void *)

/*
 * Get he number of complete stack slots used
 */
#define STACK_SLOTS_USED(size) ((size) >> 3)

/*
 * Round a size up to a multiple of the stack word size.
 */
#define	ROUND_STACK(size)	\
		(((size) + (STACK_SLOT_SIZE - 1)) & ~(STACK_SLOT_SIZE - 1))

/*
 * Setup or teardown the x86 code output process.
 */
#define	jit_cache_setup_output(needed)	\
	unsigned char *inst = gen->posn.ptr; \
	if(!jit_cache_check_for_n(&(gen->posn), (needed))) \
	{ \
		jit_cache_mark_full(&(gen->posn)); \
		return; \
	}
#define	jit_cache_end_output()	\
	gen->posn.ptr = inst

/*
 * Set this to 1 for debugging fixups
 */
#define DEBUG_FIXUPS 0

/*
 * The maximum block size copied inline
 */
#define _JIT_MAX_MEMCPY_INLINE	0x40

/*
 * va_list type as specified in x86_64 sysv abi version 0.99
 * Figure 3.34
 */
typedef struct
{
	unsigned int gp_offset;
	unsigned int fp_offset;
	void *overflow_arg_area;
	void *reg_save_area;
} _jit_va_list;

/* Registers used for INTEGER arguments */
static int _jit_word_arg_regs[] = {X86_64_REG_RDI, X86_64_REG_RSI,
								   X86_64_REG_RDX, X86_64_REG_RCX,
								   X86_64_REG_R8, X86_64_REG_R9};
#define _jit_num_word_regs	6

/* Registers used for float arguments */
static int _jit_float_arg_regs[] = {X86_64_REG_XMM0, X86_64_REG_XMM1,
									X86_64_REG_XMM2, X86_64_REG_XMM3,
									X86_64_REG_XMM4, X86_64_REG_XMM5,
									X86_64_REG_XMM6, X86_64_REG_XMM7};
#define _jit_num_float_regs	8

/* Registers used for returning INTEGER values */
static int _jit_word_return_regs[] = {X86_64_REG_RAX, X86_64_REG_RDX};
#define _jit_num_word_return_regs	2

/* Registers used for returning sse values */
static int _jit_sse_return_regs[] = {X86_64_REG_XMM0, X86_64_REG_XMM1};
#define _jit_num_sse_return_regs	2

/*
 * X86_64 register classes
 */
static _jit_regclass_t *x86_64_reg;		/* X86_64 general purpose registers */
static _jit_regclass_t *x86_64_creg;	/* X86_64 call clobbered general */
										/* purpose registers */
static _jit_regclass_t *x86_64_rreg;	/* general purpose registers not used*/
										/* for returning values */
static _jit_regclass_t *x86_64_freg;	/* X86_64 fpu registers */
static _jit_regclass_t *x86_64_xreg;	/* X86_64 xmm registers */

void
_jit_init_backend(void)
{
	x86_64_reg = _jit_regclass_create(
		"reg", JIT_REG_WORD | JIT_REG_LONG, 14,
		X86_64_REG_RAX, X86_64_REG_RCX,
		X86_64_REG_RDX, X86_64_REG_RBX,
		X86_64_REG_RSI, X86_64_REG_RDI,
		X86_64_REG_R8, X86_64_REG_R9,
		X86_64_REG_R10, X86_64_REG_R11,
		X86_64_REG_R12, X86_64_REG_R13,
		X86_64_REG_R14, X86_64_REG_R15);

	/* register class with all call clobbered registers */
	x86_64_creg = _jit_regclass_create(
		"creg", JIT_REG_WORD | JIT_REG_LONG, 9,
		X86_64_REG_RAX, X86_64_REG_RCX,
		X86_64_REG_RDX, X86_64_REG_RSI,
		X86_64_REG_RDI, X86_64_REG_R8,
		X86_64_REG_R9, X86_64_REG_R10,
		X86_64_REG_R11);

	/* register class with all registers not used for returning values */
	x86_64_rreg = _jit_regclass_create(
		"rreg", JIT_REG_WORD | JIT_REG_LONG, 12,
		X86_64_REG_RCX, X86_64_REG_RBX,
		X86_64_REG_RSI, X86_64_REG_RDI,
		X86_64_REG_R8, X86_64_REG_R9,
		X86_64_REG_R10, X86_64_REG_R11,
		X86_64_REG_R12, X86_64_REG_R13,
		X86_64_REG_R14, X86_64_REG_R15);

	x86_64_freg = _jit_regclass_create(
		"freg", JIT_REG_X86_64_FLOAT | JIT_REG_IN_STACK, 8,
		X86_64_REG_ST0, X86_64_REG_ST1,
		X86_64_REG_ST2, X86_64_REG_ST3,
		X86_64_REG_ST4, X86_64_REG_ST5,
		X86_64_REG_ST6, X86_64_REG_ST7);

	x86_64_xreg = _jit_regclass_create(
		"xreg", JIT_REG_FLOAT32 | JIT_REG_FLOAT64, 16,
		X86_64_REG_XMM0, X86_64_REG_XMM1,
		X86_64_REG_XMM2, X86_64_REG_XMM3,
		X86_64_REG_XMM4, X86_64_REG_XMM5,
		X86_64_REG_XMM6, X86_64_REG_XMM7,
		X86_64_REG_XMM8, X86_64_REG_XMM9,
		X86_64_REG_XMM10, X86_64_REG_XMM11,
		X86_64_REG_XMM12, X86_64_REG_XMM13,
		X86_64_REG_XMM14, X86_64_REG_XMM15);
}

int
_jit_opcode_is_supported(int opcode)
{
	switch(opcode)
	{
	#define JIT_INCLUDE_SUPPORTED
	#include "jit-rules-x86-64.inc"
	#undef JIT_INCLUDE_SUPPORTED
	}
	return 0;
}

int
_jit_setup_indirect_pointer(jit_function_t func, jit_value_t value)
{
	return jit_insn_outgoing_reg(func, value, X86_64_REG_R11);
}

/*
 * Do a xmm operation with a constant float32 value
 */
static int
_jit_xmm1_reg_imm_size_float32(jit_gencode_t gen, unsigned char **inst_ptr,
							   X86_64_XMM1_OP opc, int reg,
							   jit_float32 *float32_value)
{
	void *ptr;
	jit_nint offset;
	unsigned char *inst;

	inst = *inst_ptr;
	ptr = _jit_cache_alloc(&(gen->posn), sizeof(jit_float32));
	if(!ptr)
	{
		return 0;
	}
	jit_memcpy(ptr, float32_value, sizeof(jit_float32));

	offset = (jit_nint)ptr - ((jit_nint)inst + (reg > 7 ? 9 : 8));
	if((offset >= jit_min_int) && (offset <= jit_max_int))
	{
		/* We can use RIP relative addressing here */
		x86_64_xmm1_reg_membase(inst, opc, reg,
									 X86_64_RIP, offset, 0);
	}
	else if(((jit_nint)ptr >= jit_min_int) &&
			((jit_nint)ptr <= jit_max_int))
	{
		/* We can use absolute addressing */
		x86_64_xmm1_reg_mem(inst, opc, reg, (jit_nint)ptr, 0);
	}
	else
	{
		/* We have to use an extra general register */
		/* TODO */
		return 0;
	}
	*inst_ptr = inst;
	return 1;
}

/*
 * Do a xmm operation with a constant float64 value
 */
static int
_jit_xmm1_reg_imm_size_float64(jit_gencode_t gen, unsigned char **inst_ptr,
							   X86_64_XMM1_OP opc, int reg,
							   jit_float64 *float64_value)
{
	void *ptr;
	jit_nint offset;
	unsigned char *inst;

	inst = *inst_ptr;
	ptr = _jit_cache_alloc(&(gen->posn), sizeof(jit_float64));
	if(!ptr)
	{
		return 0;
	}
	jit_memcpy(ptr, float64_value, sizeof(jit_float64));

	offset = (jit_nint)ptr - ((jit_nint)inst + (reg > 7 ? 9 : 8));
	if((offset >= jit_min_int) && (offset <= jit_max_int))
	{
		/* We can use RIP relative addressing here */
		x86_64_xmm1_reg_membase(inst, opc, reg,
									 X86_64_RIP, offset, 1);
	}
	else if(((jit_nint)ptr >= jit_min_int) &&
			((jit_nint)ptr <= jit_max_int))
	{
		/* We can use absolute addressing */
		x86_64_xmm1_reg_mem(inst, opc, reg, (jit_nint)ptr, 1);
	}
	else
	{
		/* We have to use an extra general register */
		/* TODO */
		return 0;
	}
	*inst_ptr = inst;
	return 1;
}

/*
 * Call a function
 */
static unsigned char *
x86_64_call_code(unsigned char *inst, jit_nint func)
{
	jit_nint offset;

	offset = func - ((jit_nint)inst + 5);
	if(offset >= jit_min_int && offset <= jit_max_int)
	{
		/* We can use the immediate call */
		x86_64_call_imm(inst, offset);
	}
	else
	{
		/* We have to do a call via register */
		x86_64_mov_reg_imm_size(inst, X86_64_SCRATCH, func, 8);
		x86_64_call_reg(inst, X86_64_SCRATCH);
	}
	return inst;
}

/*
 * Jump to a function
 */
static unsigned char *
x86_64_jump_to_code(unsigned char *inst, jit_nint func)
{
	jit_nint offset;

	offset = func - ((jit_nint)inst + 5);
	if(offset >= jit_min_int && offset <= jit_max_int)
	{
		/* We can use the immediate call */
		x86_64_jmp_imm(inst, offset);
	}
	else
	{
		/* We have to do a call via register */
		x86_64_mov_reg_imm_size(inst, X86_64_SCRATCH, func, 8);
		x86_64_jmp_reg(inst, X86_64_SCRATCH);
	}
	return inst;
}

/*
 * Throw a builtin exception.
 */
static unsigned char *
throw_builtin(unsigned char *inst, jit_function_t func, int type)
{
	/* We need to update "catch_pc" if we have a "try" block */
	if(func->builder->setjmp_value != 0)
	{
		_jit_gen_fix_value(func->builder->setjmp_value);

		x86_64_lea_membase_size(inst, X86_64_RDI, X86_64_RIP, 0, 8);
		x86_64_mov_membase_reg_size(inst, X86_64_RBP, 
					func->builder->setjmp_value->frame_offset
					+ jit_jmp_catch_pc_offset, X86_64_RDI, 8);
	}

	/* Push the exception type onto the stack */
	x86_64_mov_reg_imm_size(inst, X86_64_RDI, type, 4);

	/* Call the "jit_exception_builtin" function, which will never return */
	return x86_64_call_code(inst, (jit_nint)jit_exception_builtin);
}

/*
 * spill a register to it's place in the current stack frame.
 * The argument type must be in it's normalized form.
 */
static void
_spill_reg(unsigned char **inst_ptr, jit_type_t type,
		   jit_int reg, jit_int offset)
{
	unsigned char *inst = *inst_ptr;

	if(IS_GENERAL_REG(reg))
	{
		switch(type->kind)
		{
			case JIT_TYPE_SBYTE:
			case JIT_TYPE_UBYTE:
			{
				x86_64_mov_membase_reg_size(inst, X86_64_RBP, offset,
											_jit_reg_info[reg].cpu_reg, 1);
			}
			break;

			case JIT_TYPE_SHORT:
			case JIT_TYPE_USHORT:
			{
				x86_64_mov_membase_reg_size(inst, X86_64_RBP, offset,
											_jit_reg_info[reg].cpu_reg, 2);
			}
			break;

			case JIT_TYPE_INT:
			case JIT_TYPE_UINT:
			case JIT_TYPE_FLOAT32:
			{
				x86_64_mov_membase_reg_size(inst, X86_64_RBP, offset,
											_jit_reg_info[reg].cpu_reg, 4);
			}
			break;

			case JIT_TYPE_LONG:
			case JIT_TYPE_ULONG:
			case JIT_TYPE_FLOAT64:
			{
				x86_64_mov_membase_reg_size(inst, X86_64_RBP, offset,
											_jit_reg_info[reg].cpu_reg, 8);
			}
			break;

			case JIT_TYPE_STRUCT:
			case JIT_TYPE_UNION:
			{
				jit_nuint size = jit_type_get_size(type);

				if(size == 1)
				{
					x86_64_mov_membase_reg_size(inst, X86_64_RBP, offset,
												_jit_reg_info[reg].cpu_reg, 1);
				}
				else if(size == 2)
				{
					x86_64_mov_membase_reg_size(inst, X86_64_RBP, offset,
												_jit_reg_info[reg].cpu_reg, 2);
				}
				else if(size <= 4)
				{
					x86_64_mov_membase_reg_size(inst, X86_64_RBP, offset,
												_jit_reg_info[reg].cpu_reg, 4);
				}
				else
				{
					x86_64_mov_membase_reg_size(inst, X86_64_RBP, offset,
												_jit_reg_info[reg].cpu_reg, 8);
				}
			}
		}
	}
	else if(IS_XMM_REG(reg))
	{
		switch(type->kind)
		{
			case JIT_TYPE_FLOAT32:
			{
				x86_64_movss_membase_reg(inst, X86_64_RBP, offset,
										 _jit_reg_info[reg].cpu_reg);
			}
			break;

			case JIT_TYPE_FLOAT64:
			{
				x86_64_movsd_membase_reg(inst, X86_64_RBP, offset,
										 _jit_reg_info[reg].cpu_reg);
			}
			break;

			case JIT_TYPE_STRUCT:
			case JIT_TYPE_UNION:
			{
				jit_nuint size = jit_type_get_size(type);

				if(size <= 4)
				{
					x86_64_movss_membase_reg(inst, X86_64_RBP, offset,
											 _jit_reg_info[reg].cpu_reg);
				}
				else if(size <= 8)
				{
					x86_64_movsd_membase_reg(inst, X86_64_RBP, offset,
											 _jit_reg_info[reg].cpu_reg);
				}
				else
				{
					jit_nint alignment = jit_type_get_alignment(type);

					if((alignment & 0xf) == 0)
					{
						x86_64_movaps_membase_reg(inst, X86_64_RBP, offset,
												  _jit_reg_info[reg].cpu_reg);
					}
					else
					{
						x86_64_movups_membase_reg(inst, X86_64_RBP, offset,
												  _jit_reg_info[reg].cpu_reg);
					}
				}
			}
			break;
		}
	}
	else if(IS_FPU_REG(reg))
	{
		switch(type->kind)
		{
			case JIT_TYPE_FLOAT32:
			{
				x86_64_fstp_membase_size(inst, X86_64_RBP, offset, 4);
			}
			break;

			case JIT_TYPE_FLOAT64:
			{
				x86_64_fstp_membase_size(inst, X86_64_RBP, offset, 8);
			}
			break;

			case JIT_TYPE_NFLOAT:
			{
				if(sizeof(jit_nfloat) == sizeof(jit_float64))
				{
					x86_64_fstp_membase_size(inst, X86_64_RBP, offset, 8);
				}
				else
				{
					x86_64_fstp_membase_size(inst, X86_64_RBP, offset, 10);
				}
			}
			break;
		}
	}

	/* Write the current instruction pointer back */
	*inst_ptr = inst;
}

void
_jit_gen_fix_value(jit_value_t value)
{
	if(!(value->has_frame_offset) && !(value->is_constant))
	{
		jit_nuint alignment = jit_type_get_alignment(value->type);
		jit_nint size =jit_type_get_size(value->type);
		jit_nint frame_size = value->block->func->builder->frame_size;

		/* Round the size to a multiple of the stack item size */
		size = (jit_nint)(ROUND_STACK(size));

		/* Add the size to the existing local items */
		frame_size += size;

		/* Align the new frame_size for the value */
		frame_size = (frame_size + (alignment - 1)) & ~(alignment - 1);

		value->block->func->builder->frame_size = frame_size;
		value->frame_offset = -frame_size;
		value->has_frame_offset = 1;
	}
}

void
_jit_gen_spill_global(jit_gencode_t gen, int reg, jit_value_t value)
{
	jit_cache_setup_output(16);
	if(value)
	{
		jit_type_t type = jit_type_normalize(value->type);

		_jit_gen_fix_value(value);

		_spill_reg(&inst, type, value->global_reg, value->frame_offset);
	}
	else
	{
		x86_64_push_reg_size(inst, _jit_reg_info[reg].cpu_reg, 8);
	}
	jit_cache_end_output();
}

void
_jit_gen_load_global(jit_gencode_t gen, int reg, jit_value_t value)
{
	jit_cache_setup_output(16);
	if(value)
	{
		x86_64_mov_reg_membase_size(inst,
			_jit_reg_info[value->global_reg].cpu_reg,
			X86_64_RBP, value->frame_offset, 8);
	}
	else
	{
		x86_64_pop_reg_size(inst, _jit_reg_info[reg].cpu_reg, 8);
	}
	jit_cache_end_output();
}

void
_jit_gen_spill_reg(jit_gencode_t gen, int reg,
				   int other_reg, jit_value_t value)
{
	jit_type_t type;

	/* Make sure that we have sufficient space */
	jit_cache_setup_output(16);

	/* If the value is associated with a global register, then copy to that */
	if(value->has_global_register)
	{
		reg = _jit_reg_info[reg].cpu_reg;
		other_reg = _jit_reg_info[value->global_reg].cpu_reg;
		x86_64_mov_reg_reg_size(inst, other_reg, reg, sizeof(void *));
		jit_cache_end_output();
		return;
	}

	/* Fix the value in place within the local variable frame */
	_jit_gen_fix_value(value);

	/* Get the normalized type */
	type = jit_type_normalize(value->type);

	/* and spill the register */
	_spill_reg(&inst, type, reg, value->frame_offset);

	/* End the code output process */
	jit_cache_end_output();
}

void
_jit_gen_free_reg(jit_gencode_t gen, int reg,
				  int other_reg, int value_used)
{
	/* We only need to take explicit action if we are freeing a
	   floating-point register whose value hasn't been used yet */
	if(!value_used && IS_FPU_REG(reg))
	{
		if(jit_cache_check_for_n(&(gen->posn), 2))
		{
			x86_fstp(gen->posn.ptr, reg - X86_64_REG_ST0);
		}
		else
		{
			jit_cache_mark_full(&(gen->posn));
		}
	}
}

/*
 * Set a register value based on a condition code.
 */
static unsigned char *
setcc_reg(unsigned char *inst, int reg, int cond, int is_signed)
{
	/* Use a SETcc instruction if we have a basic register */
	x86_64_set_reg(inst, cond, reg, is_signed);
	x86_64_movzx8_reg_reg_size(inst, reg, reg, 4);
	return inst;
}

/*
 * Helper macros for fixup handling.
 *
 * We have only 4 bytes for the jump offsets.
 * Therefore we have do something tricky here.
 * We need some fixed value that is known to be fix throughout the
 * building of the function and that will be near the emitted code.
 * The posn limit looks like the perfect value to use.
 */
#define _JIT_GET_FIXVALUE(gen) ((gen)->posn.limit)

/*
 * Calculate the fixup value
 * This is the value stored as placeholder in the instruction.
 */
#define _JIT_CALC_FIXUP(fixup_list, inst) \
	((jit_int)((jit_nint)(inst) - (jit_nint)(fixup_list)))

/*
 * Calculate the pointer to the fixup value.
 */
#define _JIT_CALC_NEXT_FIXUP(fixup_list, fixup) \
	((fixup) ? ((jit_nint)(fixup_list) - (jit_nint)(fixup)) : (jit_nint)0)

/*
 * Get the long form of a branch opcode.
 */
static int
long_form_branch(int opcode)
{
	if(opcode == 0xEB)
	{
		return 0xE9;
	}
	else
	{
		return opcode + 0x0F10;
	}
}

/*
 * Output a branch instruction.
 */
static unsigned char *
output_branch(jit_function_t func, unsigned char *inst, int opcode,
			  jit_insn_t insn)
{
	jit_block_t block;

	if((insn->flags & JIT_INSN_VALUE1_IS_LABEL) != 0)
	{
		/* "address_of_label" instruction */
		block = jit_block_from_label(func, (jit_label_t)(insn->value1));
	}
	else
	{
		block = jit_block_from_label(func, (jit_label_t)(insn->dest));
	}
	if(!block)
	{
		return inst;
	}
	if(block->address)
	{
		jit_nint offset;

		/* We already know the address of the block */
		offset = ((unsigned char *)(block->address)) - (inst + 2);
		if(x86_is_imm8(offset))
		{
			/* We can output a short-form backwards branch */
			*inst++ = (unsigned char)opcode;
			*inst++ = (unsigned char)offset;
		}
		else
		{
			/* We need to output a long-form backwards branch */
			offset -= 3;
			opcode = long_form_branch(opcode);
			if(opcode < 256)
			{
				*inst++ = (unsigned char)opcode;
			}
			else
			{
				*inst++ = (unsigned char)(opcode >> 8);
				*inst++ = (unsigned char)opcode;
				--offset;
			}
			x86_imm_emit32(inst, offset);
		}
	}
	else
	{
		jit_int fixup;

		/* Output a placeholder and record on the block's fixup list */
		opcode = long_form_branch(opcode);
		if(opcode < 256)
		{
			*inst++ = (unsigned char)opcode;
		}
		else
		{
			*inst++ = (unsigned char)(opcode >> 8);
			*inst++ = (unsigned char)opcode;
		}
		if(block->fixup_list)
		{
			fixup = _JIT_CALC_FIXUP(block->fixup_list, inst);
		}
		else
		{
			fixup = 0;
		}
		block->fixup_list = (void *)inst;
		x86_imm_emit32(inst, fixup);

		if(DEBUG_FIXUPS)
		{
			fprintf(stderr,
					"Block: %lx, Current Fixup: %lx, Next fixup: %lx\n",
					(jit_nint)block, (jit_nint)(block->fixup_list),
					(jit_nint)fixup);
		}
	}
	return inst;
}

/*
 * Jump to the current function's epilog.
 */
static unsigned char *
jump_to_epilog(jit_gencode_t gen, unsigned char *inst, jit_block_t block)
{
	jit_int fixup;

	/* If the epilog is the next thing that we will output,
	   then fall through to the epilog directly */
	block = block->next;
	while(block != 0 && block->first_insn > block->last_insn)
	{
		block = block->next;
	}
	if(!block)
	{
		return inst;
	}

	/* Output a placeholder for the jump and add it to the fixup list */
	*inst++ = (unsigned char)0xE9;
	if(gen->epilog_fixup)
	{
		fixup = _JIT_CALC_FIXUP(gen->epilog_fixup, inst);
	}
	else
	{
		fixup = 0;
	}
	gen->epilog_fixup = (void *)inst;
	x86_imm_emit32(inst, fixup);
	return inst;
}

/*
 * Support functiond for the FPU stack
 */

static int
fp_stack_index(jit_gencode_t gen, int reg)
{
	return gen->reg_stack_top - reg - 1;
}

void
_jit_gen_exch_top(jit_gencode_t gen, int reg)
{
	if(IS_FPU_REG(reg))
	{
		jit_cache_setup_output(2);
		x86_fxch(inst, fp_stack_index(gen, reg));
		jit_cache_end_output();
	}
}

void
 _jit_gen_move_top(jit_gencode_t gen, int reg)
{
	if(IS_FPU_REG(reg))
	{
		jit_cache_setup_output(2);
		x86_fstp(inst, fp_stack_index(gen, reg));
		jit_cache_end_output();
	}
}

void
_jit_gen_spill_top(jit_gencode_t gen, int reg, jit_value_t value, int pop)
{
	if(IS_FPU_REG(reg))
	{
		int offset;

		/* Make sure that we have sufficient space */
		jit_cache_setup_output(16);

		/* Fix the value in place within the local variable frame */
		_jit_gen_fix_value(value);

		/* Output an appropriate instruction to spill the value */
		offset = (int)(value->frame_offset);

		/* Spill the top of the floating-point register stack */
		switch(jit_type_normalize(value->type)->kind)
		{
			case JIT_TYPE_FLOAT32:
			{
				if(pop)
				{
					x86_64_fstp_membase_size(inst, X86_64_RBP, offset, 4);
				}
				else
				{
					x86_64_fst_membase_size(inst, X86_64_RBP, offset, 4);
				}
			}
			break;

			case JIT_TYPE_FLOAT64:
			{
				if(pop)
				{
					x86_64_fstp_membase_size(inst, X86_64_RBP, offset, 8);
				}
				else
				{
					x86_64_fst_membase_size(inst, X86_64_RBP, offset, 8);
				}
			}
			break;

			case JIT_TYPE_NFLOAT:
			{
				if(sizeof(jit_nfloat) == sizeof(jit_float64))
				{
					if(pop)
					{
						x86_64_fstp_membase_size(inst, X86_64_RBP, offset, 8);
					}
					else
					{
						x86_64_fst_membase_size(inst, X86_64_RBP, offset, 8);
					}
				}
				else
				{
					x86_64_fstp_membase_size(inst, X86_64_RBP, offset, 10);
					if(!pop)
					{
						x86_64_fld_membase_size(inst, X86_64_RBP, offset, 10);
					}
				}
			}
			break;
		}

		/* End the code output process */
		jit_cache_end_output();
	}
}

void
_jit_gen_load_value(jit_gencode_t gen, int reg, int other_reg, jit_value_t value)
{
	jit_type_t type;
	int src_reg, other_src_reg;
	void *ptr;
	int offset;

	/* Make sure that we have sufficient space */
	jit_cache_setup_output(16);

	type = jit_type_normalize(value->type);

	/* Load zero */
	if(value->is_constant)
	{
		switch(type->kind)
		{
			case JIT_TYPE_SBYTE:
			case JIT_TYPE_UBYTE:
			case JIT_TYPE_SHORT:
			case JIT_TYPE_USHORT:
			case JIT_TYPE_INT:
			case JIT_TYPE_UINT:
			{
				if((jit_nint)(value->address) == 0)
				{
					x86_64_clear_reg(inst, _jit_reg_info[reg].cpu_reg);
				}
				else
				{
					x86_64_mov_reg_imm_size(inst, _jit_reg_info[reg].cpu_reg,
							(jit_nint)(value->address), 4);
				}
			}
			break;

			case JIT_TYPE_LONG:
			case JIT_TYPE_ULONG:
			{
				if((jit_nint)(value->address) == 0)
				{
					x86_64_clear_reg(inst, _jit_reg_info[reg].cpu_reg);
				}
				else
				{
					x86_64_mov_reg_imm_size(inst, _jit_reg_info[reg].cpu_reg,
							(jit_nint)(value->address), 8);
				}
			}
			break;

			case JIT_TYPE_FLOAT32:
			{
				jit_float32 float32_value;

				float32_value = jit_value_get_float32_constant(value);

				if(IS_GENERAL_REG(reg))
				{
					union
					{
						jit_float32 float32_value;
						jit_int int_value;
					} un;

					un.float32_value = float32_value;
					x86_64_mov_reg_imm_size(inst, _jit_reg_info[reg].cpu_reg,
											un.int_value, 4);
				}
				else if(IS_XMM_REG(reg))
				{
					int xmm_reg = _jit_reg_info[reg].cpu_reg;

					_jit_xmm1_reg_imm_size_float32(gen, &inst, XMM1_MOV,
												   xmm_reg, &float32_value);
				}
				else
				{
					if(float32_value == (jit_float32) 0.0)
					{
						x86_fldz(inst);
					}
					else if(float32_value == (jit_float32) 1.0)
					{
						x86_fld1(inst);
					}
					else
					{
						jit_nint offset;

						ptr = _jit_cache_alloc(&(gen->posn), sizeof(jit_float32));
						jit_memcpy(ptr, &float32_value, sizeof(float32_value));

						offset = (jit_nint)ptr - ((jit_nint)inst + 7);
						if((offset >= jit_min_int) && (offset <= jit_max_int))
						{
							/* We can use RIP relative addressing here */
							x86_64_fld_membase_size(inst, X86_64_RIP, offset, 4);
						}
						else if(((jit_nint)ptr >= jit_min_int) &&
								((jit_nint)ptr <= jit_max_int))
						{
							/* We can use absolute addressing */
							x86_64_fld_mem_size(inst, (jit_nint)ptr, 4);
						}
						else
						{
							/* We have to use an extra general register */
							/* TODO */
						}
					}
				}
			}
			break;

			case JIT_TYPE_FLOAT64:
			{
				jit_float64 float64_value;
				float64_value = jit_value_get_float64_constant(value);
				if(IS_GENERAL_REG(reg))
				{
					union
					{
						jit_float64 float64_value;
						jit_long long_value;
					} un;

					un.float64_value = float64_value;
					x86_64_mov_reg_imm_size(inst, _jit_reg_info[reg].cpu_reg,
											un.long_value, 8);
				}
				else if(IS_XMM_REG(reg))
				{
					int xmm_reg = _jit_reg_info[reg].cpu_reg;

					_jit_xmm1_reg_imm_size_float64(gen, &inst, XMM1_MOV,
												   xmm_reg, &float64_value);
				}
				else
				{
					if(float64_value == (jit_float64) 0.0)
					{
						x86_fldz(inst);
					}
					else if(float64_value == (jit_float64) 1.0)
					{
						x86_fld1(inst);
					}
					else
					{
						jit_nint offset;

						ptr = _jit_cache_alloc(&(gen->posn), sizeof(jit_float64));
						jit_memcpy(ptr, &float64_value, sizeof(float64_value));

						offset = (jit_nint)ptr - ((jit_nint)inst + 7);
						if((offset >= jit_min_int) && (offset <= jit_max_int))
						{
							/* We can use RIP relative addressing here */
							x86_64_fld_membase_size(inst, X86_64_RIP, offset, 8);
						}
						else if(((jit_nint)ptr >= jit_min_int) &&
								((jit_nint)ptr <= jit_max_int))
						{
							/* We can use absolute addressing */
							x86_64_fld_mem_size(inst, (jit_nint)ptr, 8);
						}
						else
						{
							/* We have to use an extra general register */
							/* TODO */
						}
					}
				}
			}
			break;

			case JIT_TYPE_NFLOAT:
			{
				jit_nfloat nfloat_value;
				nfloat_value = jit_value_get_nfloat_constant(value);
				if(IS_GENERAL_REG(reg) && sizeof(jit_nfloat) == sizeof(jit_float64))
				{
					union
					{
						jit_nfloat nfloat_value;
						jit_long long_value;
					} un;

					un.nfloat_value = nfloat_value;
					x86_64_mov_reg_imm_size(inst, _jit_reg_info[reg].cpu_reg,
											un.long_value, 8);
				}
				else if(IS_XMM_REG(reg) && sizeof(jit_nfloat) == sizeof(jit_float64))
				{
					jit_nint offset;
					int xmm_reg = _jit_reg_info[reg].cpu_reg;

					ptr = _jit_cache_alloc(&(gen->posn), sizeof(jit_nfloat));
					jit_memcpy(ptr, &nfloat_value, sizeof(nfloat_value));
					offset = (jit_nint)ptr - 
								((jit_nint)inst + (xmm_reg > 7 ? 9 : 8));
					if((offset >= jit_min_int) && (offset <= jit_max_int))
					{
						/* We can use RIP relative addressing here */
						x86_64_movsd_reg_membase(inst, xmm_reg, X86_64_RIP, offset);
					}
					else if(((jit_nint)ptr >= jit_min_int) &&
							((jit_nint)ptr <= jit_max_int))
					{
						/* We can use absolute addressing */
						x86_64_movsd_reg_mem(inst, xmm_reg, (jit_nint)ptr);
					}
					else
					{
						/* We have to use an extra general register */
						/* TODO */
					}
				}
				else
				{
					if(nfloat_value == (jit_nfloat) 0.0)
					{
						x86_fldz(inst);
					}
					else if(nfloat_value == (jit_nfloat) 1.0)
					{
						x86_fld1(inst);
					}
					else
					{
						jit_nint offset;

						ptr = _jit_cache_alloc(&(gen->posn), sizeof(jit_nfloat));
						jit_memcpy(ptr, &nfloat_value, sizeof(nfloat_value));

						offset = (jit_nint)ptr - ((jit_nint)inst + 7);
						if((offset >= jit_min_int) && (offset <= jit_max_int))
						{
							/* We can use RIP relative addressing here */
							if(sizeof(jit_nfloat) == sizeof(jit_float64))
							{
								x86_64_fld_membase_size(inst, X86_64_RIP, offset, 8);
							}
							else
							{
								x86_64_fld_membase_size(inst, X86_64_RIP, offset, 10);
							}
						}
						else if(((jit_nint)ptr >= jit_min_int) &&
								((jit_nint)ptr <= jit_max_int))
						{
							/* We can use absolute addressing */
							if(sizeof(jit_nfloat) == sizeof(jit_float64))
							{
								x86_64_fld_mem_size(inst, (jit_nint)ptr, 8);
							}
							else
							{
								x86_64_fld_mem_size(inst, (jit_nint)ptr, 10);
							}
						}
						else
						{
							/* We have to use an extra general register */
							/* TODO */
						}
					}
				}
			}
			break;
		}
	}
	else if(value->in_register || value->in_global_register)
	{
		if(value->in_register)
		{
			src_reg = value->reg;
			other_src_reg = -1;
		}
		else
		{
			src_reg = value->global_reg;
			other_src_reg = -1;
		}

		switch(type->kind)
		{
#if 0
			case JIT_TYPE_SBYTE:
			{
				x86_widen_reg(inst, _jit_reg_info[reg].cpu_reg,
					      _jit_reg_info[src_reg].cpu_reg, 1, 0);
			}
			break;

			case JIT_TYPE_UBYTE:
			{
				x86_widen_reg(inst, _jit_reg_info[reg].cpu_reg,
					      _jit_reg_info[src_reg].cpu_reg, 0, 0);
			}
			break;

			case JIT_TYPE_SHORT:
			{
				x86_widen_reg(inst, _jit_reg_info[reg].cpu_reg,
					      _jit_reg_info[src_reg].cpu_reg, 1, 1);
			}
			break;

			case JIT_TYPE_USHORT:
			{
				x86_widen_reg(inst, _jit_reg_info[reg].cpu_reg,
					      _jit_reg_info[src_reg].cpu_reg, 0, 1);
			}
			break;
#else
			case JIT_TYPE_SBYTE:
			case JIT_TYPE_UBYTE:
			case JIT_TYPE_SHORT:
			case JIT_TYPE_USHORT:
#endif
			case JIT_TYPE_INT:
			case JIT_TYPE_UINT:
			{
				x86_64_mov_reg_reg_size(inst, _jit_reg_info[reg].cpu_reg,
										_jit_reg_info[src_reg].cpu_reg, 4);
			}
			break;

			case JIT_TYPE_LONG:
			case JIT_TYPE_ULONG:
			{
				x86_64_mov_reg_reg_size(inst, _jit_reg_info[reg].cpu_reg,
										_jit_reg_info[src_reg].cpu_reg, 8);
			}
			break;

			case JIT_TYPE_FLOAT32:
			{
				if(IS_FPU_REG(reg))
				{
					if(IS_FPU_REG(src_reg))
					{
						x86_fld_reg(inst, fp_stack_index(gen, src_reg));
					}
					else if(IS_XMM_REG(src_reg))
					{
						/* Fix the position of the value in the stack frame */
						_jit_gen_fix_value(value);
						offset = (int)(value->frame_offset);

						x86_64_movss_membase_reg(inst, X86_64_RBP, offset,
												 _jit_reg_info[src_reg].cpu_reg);
						x86_64_fld_membase_size(inst, X86_64_RBP, offset, 4);
					}
				}
				else if(IS_XMM_REG(reg))
				{
					if(IS_FPU_REG(src_reg))
					{
						/* Fix the position of the value in the stack frame */
						_jit_gen_fix_value(value);
						offset = (int)(value->frame_offset);

						x86_64_fst_membase_size(inst, X86_64_RBP, offset, 4);
						x86_64_movss_reg_membase(inst, _jit_reg_info[reg].cpu_reg,
												 X86_64_RBP, offset);
					}
					else if(IS_XMM_REG(src_reg))
					{
						x86_64_movss_reg_reg(inst, _jit_reg_info[reg].cpu_reg,
											 _jit_reg_info[src_reg].cpu_reg);
					}
				}
			}
			break;

			case JIT_TYPE_FLOAT64:
			{
				if(IS_FPU_REG(reg))
				{
					if(IS_FPU_REG(src_reg))
					{
						x86_fld_reg(inst, fp_stack_index(gen, src_reg));
					}
					else if(IS_XMM_REG(src_reg))
					{
						/* Fix the position of the value in the stack frame */
						_jit_gen_fix_value(value);
						offset = (int)(value->frame_offset);

						x86_64_movsd_membase_reg(inst, X86_64_RBP, offset,
												 _jit_reg_info[src_reg].cpu_reg);
						x86_64_fld_membase_size(inst, X86_64_RBP, offset, 8);
					}
				}
				else if(IS_XMM_REG(reg))
				{
					if(IS_FPU_REG(src_reg))
					{
						/* Fix the position of the value in the stack frame */
						_jit_gen_fix_value(value);
						offset = (int)(value->frame_offset);

						x86_64_fst_membase_size(inst, X86_64_RBP, offset, 8);
						x86_64_movsd_reg_membase(inst, _jit_reg_info[reg].cpu_reg,
												 X86_64_RBP, offset);
					}
					else if(IS_XMM_REG(src_reg))
					{
						x86_64_movsd_reg_reg(inst, _jit_reg_info[reg].cpu_reg,
											 _jit_reg_info[src_reg].cpu_reg);
					}
				}
			}
			break;

			case JIT_TYPE_NFLOAT:
			{
				if(IS_FPU_REG(reg))
				{
					if(IS_FPU_REG(src_reg))
					{
						x86_fld_reg(inst, fp_stack_index(gen, src_reg));
					}
					else
					{
						fputs("Unsupported native float reg - reg move\n", stderr);
					}
				}
			}
			break;
		}
	}
	else
	{
		/* Fix the position of the value in the stack frame */
		_jit_gen_fix_value(value);
		offset = (int)(value->frame_offset);

		/* Load the value into the specified register */
		switch(type->kind)
		{
			case JIT_TYPE_SBYTE:
			{
				x86_64_movsx8_reg_membase_size(inst, _jit_reg_info[reg].cpu_reg,
											   X86_64_RBP, offset, 4);
			}
			break;

			case JIT_TYPE_UBYTE:
			{
				x86_64_movzx8_reg_membase_size(inst, _jit_reg_info[reg].cpu_reg,
											   X86_64_RBP, offset, 4);
			}
			break;

			case JIT_TYPE_SHORT:
			{
				x86_64_movsx16_reg_membase_size(inst, _jit_reg_info[reg].cpu_reg,
												X86_64_RBP, offset, 4);
			}
			break;

			case JIT_TYPE_USHORT:
			{
				x86_64_movzx16_reg_membase_size(inst, _jit_reg_info[reg].cpu_reg,
												X86_64_RBP, offset, 4);
			}
			break;

			case JIT_TYPE_INT:
			case JIT_TYPE_UINT:
			{
				x86_64_mov_reg_membase_size(inst, _jit_reg_info[reg].cpu_reg,
											X86_64_RBP, offset, 4);
			}
			break;

			case JIT_TYPE_LONG:
			case JIT_TYPE_ULONG:
			{
				x86_64_mov_reg_membase_size(inst, _jit_reg_info[reg].cpu_reg,
											X86_64_RBP, offset, 8);
			}
			break;

			case JIT_TYPE_FLOAT32:
			{
				if(IS_GENERAL_REG(reg))
				{
					x86_64_mov_reg_membase_size(inst, _jit_reg_info[reg].cpu_reg,
												X86_64_RBP, offset, 4);
				}
				if(IS_XMM_REG(reg))
				{
					x86_64_movss_reg_membase(inst, _jit_reg_info[reg].cpu_reg,
											 X86_64_RBP, offset);
				}
				else
				{
					x86_64_fld_membase_size(inst, X86_64_RBP, offset, 4);
				}
			}
			break;

			case JIT_TYPE_FLOAT64:
			{
				if(IS_GENERAL_REG(reg))
				{
					x86_64_mov_reg_membase_size(inst, _jit_reg_info[reg].cpu_reg,
												X86_64_RBP, offset, 8);
				}
				else if(IS_XMM_REG(reg))
				{
					x86_64_movsd_reg_membase(inst, _jit_reg_info[reg].cpu_reg,
											 X86_64_RBP, offset);
				}
				else
				{
					x86_64_fld_membase_size(inst, X86_64_RBP, offset, 8);
				}
			}
			break;

			case JIT_TYPE_NFLOAT:
			{
				if(sizeof(jit_nfloat) == sizeof(jit_float64))
				{
					if(IS_GENERAL_REG(reg))
					{
						x86_64_mov_reg_membase_size(inst, _jit_reg_info[reg].cpu_reg,
													X86_64_RBP, offset, 8);
					}
					else if(IS_XMM_REG(reg))
					{
						x86_64_movsd_reg_membase(inst, _jit_reg_info[reg].cpu_reg,
												 X86_64_RBP, offset);
					}
					else
					{
						x86_64_fld_membase_size(inst, X86_64_RBP, offset, 8);
					}
				}
				else
				{
					x86_64_fld_membase_size(inst, X86_64_RBP, offset, 10);
				}
			}
			break;

			case JIT_TYPE_STRUCT:
			case JIT_TYPE_UNION:
			{
				jit_nuint size = jit_type_get_size(type);

				if(IS_GENERAL_REG(reg))
				{
					if(size == 1)
					{
						x86_64_mov_reg_membase_size(inst, _jit_reg_info[reg].cpu_reg,
													X86_64_RBP, offset, 1);
					}
					else if(size == 2)
					{
						x86_64_mov_reg_membase_size(inst, _jit_reg_info[reg].cpu_reg,
													X86_64_RBP, offset, 2);
					}
					else if(size <= 4)
					{
						x86_64_mov_reg_membase_size(inst, _jit_reg_info[reg].cpu_reg,
													X86_64_RBP, offset, 4);
					}
					else if(size <= 8)
					{
						x86_64_mov_reg_membase_size(inst, _jit_reg_info[reg].cpu_reg,
													X86_64_RBP, offset, 8);
					}
				}
				else if(IS_XMM_REG(reg))
				{
					if(size <= 4)
					{
						x86_64_movss_reg_membase(inst, _jit_reg_info[reg].cpu_reg,
												 X86_64_RBP, offset);
					}
					else if(size <= 8)
					{
						x86_64_movsd_reg_membase(inst, _jit_reg_info[reg].cpu_reg,
												 X86_64_RBP, offset);
					}
					else
					{
						int alignment = jit_type_get_alignment(type);

						if((alignment & 0xf) == 0)
						{
							x86_64_movaps_reg_membase(inst, _jit_reg_info[reg].cpu_reg,
													  X86_64_RBP, offset);
						}
						else
						{
							x86_64_movups_reg_membase(inst, _jit_reg_info[reg].cpu_reg,
													  X86_64_RBP, offset);
						}
					}
				}
			}
		}
	}

	/* End the code output process */
	jit_cache_end_output();
}

void
_jit_gen_get_elf_info(jit_elf_info_t *info)
{
	info->machine = 62;	/* EM_X86_64 */
	info->abi = 0;		/* ELFOSABI_SYSV */
	info->abi_version = 0;
}

void *
_jit_gen_prolog(jit_gencode_t gen, jit_function_t func, void *buf)
{
	unsigned char prolog[JIT_PROLOG_SIZE];
	unsigned char *inst = prolog;
	int reg;
	int frame_size = 0;
	int regs_to_save = 0;

	/* Push ebp onto the stack */
	x86_64_push_reg_size(inst, X86_64_RBP, 8);

	/* Initialize EBP for the current frame */
	x86_64_mov_reg_reg_size(inst, X86_64_RBP, X86_64_RSP, 8);

	/* Allocate space for the local variable frame */
	if(func->builder->frame_size > 0)
	{
		/* Make sure that the framesize is a multiple of 8 bytes */
		frame_size = (func->builder->frame_size + 0x7) & ~0x7;
	}

	/* Get the number of registers we need to preserve */
	for(reg = 0; reg < 14; ++reg)
	{
		if(jit_reg_is_used(gen->touched, reg) &&
		   (_jit_reg_info[reg].flags & JIT_REG_CALL_USED) == 0)
		{
			++regs_to_save;
		}
	}

	/* add the register save area to the initial frame size */
	frame_size += (regs_to_save << 3);

	/* Make sure that the framesize is a multiple of 16 bytes */
	/* so that the final RSP will be alligned on a 16byte boundary. */
	frame_size = (frame_size + 0xf) & ~0xf;

	if(frame_size > 0)
	{
		x86_64_sub_reg_imm_size(inst, X86_64_RSP, frame_size, 8);
	}

	if(regs_to_save > 0)
	{
		int current_offset = 0;

		/* Save registers that we need to preserve */
		for(reg = 0; reg <= 14; ++reg)
		{
			if(jit_reg_is_used(gen->touched, reg) &&
			   (_jit_reg_info[reg].flags & JIT_REG_CALL_USED) == 0)
			{
				x86_64_mov_membase_reg_size(inst, X86_64_RSP, current_offset,
											_jit_reg_info[reg].cpu_reg, 8);
				current_offset += 8;
			}
		}
	}

	/* Copy the prolog into place and return the adjusted entry position */
	reg = (int)(inst - prolog);
	jit_memcpy(((unsigned char *)buf) + JIT_PROLOG_SIZE - reg, prolog, reg);
	return (void *)(((unsigned char *)buf) + JIT_PROLOG_SIZE - reg);
}

void
_jit_gen_epilog(jit_gencode_t gen, jit_function_t func)
{
	unsigned char *inst;
	int reg;
	int current_offset;
	jit_int *fixup;
	jit_int *next;

	/* Bail out if there is insufficient space for the epilog */
	if(!jit_cache_check_for_n(&(gen->posn), 48))
	{
		jit_cache_mark_full(&(gen->posn));
		return;
	}

	inst = gen->posn.ptr;

	/* Perform fixups on any blocks that jump to the epilog */
	fixup = (jit_int *)(gen->epilog_fixup);
	while(fixup != 0)
	{
		if(DEBUG_FIXUPS)
		{
			fprintf(stderr, "Fixup Address: %lx, Value: %x\n",
					(jit_nint)fixup, fixup[0]);
		}
		next = (jit_int *)_JIT_CALC_NEXT_FIXUP(fixup, fixup[0]);
		fixup[0] = (jit_int)(((jit_nint)inst) - ((jit_nint)fixup) - 4);
		fixup = next;
	}
	gen->epilog_fixup = 0;

	/* Restore the used callee saved registers */
	if(gen->stack_changed)
	{
		int frame_size = func->builder->frame_size;
		int regs_saved = 0;

		/* Get the number of registers we preserves */
		for(reg = 0; reg < 14; ++reg)
		{
			if(jit_reg_is_used(gen->touched, reg) &&
			   (_jit_reg_info[reg].flags & JIT_REG_CALL_USED) == 0)
			{
				++regs_saved;
			}
		}

		/* add the register save area to the initial frame size */
		frame_size += (regs_saved << 3);

		/* Make sure that the framesize is a multiple of 16 bytes */
		/* so that the final RSP will be alligned on a 16byte boundary. */
		frame_size = (frame_size + 0xf) & ~0xf;

		current_offset = -frame_size;

		for(reg = 0; reg <= 14; ++reg)
		{
			if(jit_reg_is_used(gen->touched, reg) &&
			   (_jit_reg_info[reg].flags & JIT_REG_CALL_USED) == 0)
			{
				x86_64_mov_reg_membase_size(inst, _jit_reg_info[reg].cpu_reg,
											X86_64_RBP, current_offset, 8);
				current_offset += 8;
			}
		}
	}
	else
	{
		current_offset = 0;
		for(reg = 0; reg <= 14; ++reg)
		{
			if(jit_reg_is_used(gen->touched, reg) &&
			   (_jit_reg_info[reg].flags & JIT_REG_CALL_USED) == 0)
			{
				x86_64_mov_reg_membase_size(inst, _jit_reg_info[reg].cpu_reg,
											X86_64_RSP, current_offset, 8);
				current_offset += 8;
			}
		}
	}

	/* Restore stackpointer and frame register */
	x86_64_mov_reg_reg_size(inst, X86_64_RSP, X86_64_RBP, 8);
	x86_64_pop_reg_size(inst, X86_64_RBP, 8);

	/* and return */
	x86_64_ret(inst);

	gen->posn.ptr = inst;
}

/*
 * Copy a small block. This code will be inlined.
 * Set is_aligned to 0 if you don't know if the source and target locations
 * are aligned on a 16byte boundary and != 0 if you know that both blocks are
 * aligned.
 * We assume that offset + size is in the range -2GB ... +2GB.
 */
static unsigned char *
small_block_copy(jit_gencode_t gen, unsigned char *inst,
				 int dreg, jit_nint doffset,
				 int sreg, jit_nint soffset, jit_int size,
				 int scratch_reg, int scratch_xreg, int is_aligned)
{
	int offset = 0;

	while(size >= 16)
	{
		if(is_aligned)
		{
			x86_64_movaps_reg_membase(inst, scratch_xreg,
									  sreg, soffset + offset);
			x86_64_movaps_membase_reg(inst, dreg, doffset + offset,
									  scratch_xreg);
		}
		else
		{
			x86_64_movups_reg_membase(inst, scratch_xreg,
									  sreg, soffset + offset);
			x86_64_movups_membase_reg(inst, dreg, doffset + offset,
									  scratch_xreg);
		}
		size -= 16;
		offset += 16;
	}
	/* Now copy the rest of the struct */
	if(size >= 8)
	{
		x86_64_mov_reg_membase_size(inst, scratch_reg,
									sreg, soffset + offset, 8);
		x86_64_mov_membase_reg_size(inst, dreg, doffset + offset,
									scratch_reg, 8);
		size -= 8;
		offset += 8;
	}
	if(size >= 4)
	{
		x86_64_mov_reg_membase_size(inst, scratch_reg,
									sreg, soffset + offset, 4);
		x86_64_mov_membase_reg_size(inst, dreg, doffset + offset,
									scratch_reg, 4);
		size -= 4;
		offset += 4;
	}
	if(size >= 2)
	{
		x86_64_mov_reg_membase_size(inst, scratch_reg,
									sreg, soffset + offset, 2);
		x86_64_mov_membase_reg_size(inst, dreg, doffset + offset,
									scratch_reg, 2);
		size -= 2;
		offset += 2;
	}
	if(size >= 1)
	{
		x86_64_mov_reg_membase_size(inst, scratch_reg,
									sreg, soffset + offset, 1);
		x86_64_mov_membase_reg_size(inst, dreg, doffset + offset,
									scratch_reg, 1);
		size -= 1;
		offset += 1;
	}
	return inst;
}

/*
 * Copy a struct.
 * The size of the type must be <= 4 * 16bytes
 */
static unsigned char *
small_struct_copy(jit_gencode_t gen, unsigned char *inst,
				  int dreg, jit_nint doffset,
				  int sreg, jit_nint soffset, jit_type_t type,
				  int scratch_reg, int scratch_xreg)
{
	int size = jit_type_get_size(type);
	int alignment = jit_type_get_alignment(type);

	return small_block_copy(gen, inst, dreg, doffset,
							sreg, soffset, size, scratch_reg,
							scratch_xreg, ((alignment & 0xf) == 0));
}

/*
 * Copy a block of memory that has a specific size. All call clobbered
 * registers must be unused at this point.
 */
static unsigned char *
memory_copy(jit_gencode_t gen, unsigned char *inst,
			int dreg, jit_nint doffset,
			int sreg, jit_nint soffset, jit_nint size)
{
	if(dreg == X86_64_RDI)
	{
		if(sreg != X86_64_RSI)
		{
			x86_64_mov_reg_reg_size(inst, X86_64_RSI, sreg, 8);
		}
	}
	else if(dreg == X86_64_RSI)
	{
		if(sreg == X86_64_RDI)
		{
			/* The registers are swapped so we need a temporary register */
			x86_64_mov_reg_reg_size(inst, X86_64_RCX, X86_64_RSI, 8);
			x86_64_mov_reg_reg_size(inst, X86_64_RSI, X86_64_RDI, 8);
			x86_64_mov_reg_reg_size(inst, X86_64_RDI, X86_64_RCX, 8);
		}
		else
		{
			x86_64_mov_reg_reg_size(inst, X86_64_RDI, X86_64_RSI, 8);
			if(sreg != X86_64_RSI)
			{
				x86_64_mov_reg_reg_size(inst, X86_64_RSI, sreg, 8);
			}
		}
	}
	else
	{
		x86_64_mov_reg_reg_size(inst, X86_64_RSI, sreg, 8);
		x86_64_mov_reg_reg_size(inst, X86_64_RDI, dreg, 8);
	}
	/* Move the size to argument register 3 now */
	if((size > 0) && (size <= jit_max_uint))
	{
		x86_64_mov_reg_imm_size(inst, X86_64_RDX, size, 4);
	}
	else
	{
		x86_64_mov_reg_imm_size(inst, X86_64_RDX, size, 8);
	}
	if(soffset != 0)
	{
		x86_64_add_reg_imm_size(inst, X86_64_RSI, soffset, 8);
	}
	if(doffset != 0)
	{
		x86_64_add_reg_imm_size(inst, X86_64_RDI, doffset, 8);
	}
	inst = x86_64_call_code(inst, (jit_nint)jit_memcpy);
	return inst;
}

void
_jit_gen_start_block(jit_gencode_t gen, jit_block_t block)
{
	jit_int *fixup;
	jit_int *next;
	void **absolute_fixup;
	void **absolute_next;

	/* Set the address of this block */
	block->address = (void *)(gen->posn.ptr);

	/* If this block has pending fixups, then apply them now */
	fixup = (jit_int *)(block->fixup_list);
	if(DEBUG_FIXUPS && fixup)
	{
		fprintf(stderr, "Block: %lx\n", (jit_nint)block);
		fprintf(stderr, "Limit: %lx\n", (jit_nint)_JIT_GET_FIXVALUE(gen));
	}
	while(fixup != 0)
	{
		if(DEBUG_FIXUPS)
		{
			fprintf(stderr, "Fixup Address: %lx, Value: %x\n",
					(jit_nint)fixup, fixup[0]);
		}
		next = (jit_int *)_JIT_CALC_NEXT_FIXUP(fixup, fixup[0]);
		fixup[0] = (jit_int)
			(((jit_nint)(block->address)) - ((jit_nint)fixup) - 4);
		fixup = next;
	}
	block->fixup_list = 0;

	/* Absolute fixups contain complete pointers */
	absolute_fixup = (void**)(block->fixup_absolute_list);
	while(absolute_fixup != 0)
	{
		absolute_next = (void **)(absolute_fixup[0]);
		absolute_fixup[0] = (void *)((jit_nint)(block->address));
		absolute_fixup = absolute_next;
	}
	block->fixup_absolute_list = 0;
}

void
_jit_gen_end_block(jit_gencode_t gen, jit_block_t block)
{
	/* Nothing to do here for x86 */
}

int
_jit_gen_is_global_candidate(jit_type_t type)
{
	switch(jit_type_remove_tags(type)->kind)
	{
		case JIT_TYPE_INT:
		case JIT_TYPE_UINT:
		case JIT_TYPE_LONG:
		case JIT_TYPE_ULONG:
		case JIT_TYPE_NINT:
		case JIT_TYPE_NUINT:
		case JIT_TYPE_PTR:
		case JIT_TYPE_SIGNATURE:
		{
			return 1;
		}
	}
	return 0;
}

/*
 * Do the stuff usually handled in jit-rules.c for native implementations
 * here too because the common implementation is not enough for x86_64.
 */

/*
 * Flag that a parameter is passed on the stack.
 */
#define JIT_ARG_CLASS_STACK	0xFFFF

/*
 * Define the way the parameter is passed to a specific function
 */
typedef struct
{
	jit_value_t value;
	jit_ushort arg_class;
	jit_ushort stack_pad;		/* Number of stack words needed for padding */
	union
	{
		unsigned char reg[4];
		jit_int offset;
	} un;
} _jit_param_t;

/*
 * Structure that is used to help with parameter passing.
 */
typedef struct
{
	int				stack_size;			/* Number of bytes needed on the */
										/* stack for parameter passing */
	int				stack_pad;			/* Number of stack words we have */
										/* to push before pushing the */
										/* parameters for keeping the stack */
										/* aligned */
	unsigned int	word_index;			/* Number of word registers */
										/* allocated */
	unsigned int	max_word_regs;		/* Number of word registers */
										/* available for parameter passing */
	const int	   *word_regs;
	unsigned int	float_index;
	unsigned int	max_float_regs;
	const int	   *float_regs;
	_jit_param_t   *params;

} jit_param_passing_t;

/*
 * Allcate the slot for a parameter passed on the stack.
 */
static void
_jit_alloc_param_slot(jit_param_passing_t *passing, _jit_param_t *param,
					  jit_type_t type)
{
	jit_int size = jit_type_get_size(type);
	jit_int alignment = jit_type_get_alignment(type);

	/* Expand the size to a multiple of the stack slot size */
	size = ROUND_STACK(size);

	/* Expand the alignment to a multiple of the stack slot size */
	/* We expect the alignment to be a power of two after this step */
	alignment = ROUND_STACK(alignment);

	/* Make sure the current offset is aligned propperly for the type */
	if((passing->stack_size & (alignment -1)) != 0)
	{
		/* We need padding on the stack to fix the alignment constraint */
		jit_int padding = passing->stack_size & (alignment -1);

		/* Add the padding to the stack region */
		passing->stack_size += padding;

		/* record the number of pad words needed after pushing this arg */
		param->stack_pad = STACK_SLOTS_USED(padding);
	}
	/* Record the offset of the parameter in the arg region. */
	param->un.offset = passing->stack_size;

	/* And increase the argument region used. */
	passing->stack_size += size;
}

/*
 * Determine if a type corresponds to a structure or union.
 */
static int
is_struct_or_union(jit_type_t type)
{
	type = jit_type_normalize(type);
	if(type)
	{
		if(type->kind == JIT_TYPE_STRUCT || type->kind == JIT_TYPE_UNION)
		{
			return 1;
		}
	}
	return 0;
}

/*
 * Classify the argument type.
 * The type has to be in it's normalized form.
 */
static int
_jit_classify_arg(jit_type_t arg_type, int is_return)
{
	switch(arg_type->kind)
	{
		case JIT_TYPE_SBYTE:
		case JIT_TYPE_UBYTE:
		case JIT_TYPE_SHORT:
		case JIT_TYPE_USHORT:
		case JIT_TYPE_INT:
		case JIT_TYPE_UINT:
		case JIT_TYPE_NINT:
		case JIT_TYPE_NUINT:
		case JIT_TYPE_LONG:
		case JIT_TYPE_ULONG:
		case JIT_TYPE_SIGNATURE:
		case JIT_TYPE_PTR:
		{
			return X86_64_ARG_INTEGER;
		}
		break;

		case JIT_TYPE_FLOAT32:
		case JIT_TYPE_FLOAT64:
		{
			return X86_64_ARG_SSE;
		}
		break;

		case JIT_TYPE_NFLOAT:
		{
			/* we assume the nfloat type to be long double (80bit) */
			if(is_return)
			{
				return X86_64_ARG_X87;
			}
			else
			{
				return X86_64_ARG_MEMORY;
			}
		}
		break;

		case JIT_TYPE_STRUCT:
		case JIT_TYPE_UNION:
		{
			int size = jit_type_get_size(arg_type);

			if(size > 16)
			{
				return X86_64_ARG_MEMORY;
			}
			else if(size <= 8)
			{
				return X86_64_ARG_INTEGER;
			}
			/* For structs and unions with sizes between 8 ant 16 bytes */
			/* we have to look at the elements. */
			/* TODO */
		}
	}
	return X86_64_ARG_NO_CLASS;
}

/*
 * On X86_64 the alignment of native types matches their size.
 * This leads to the result that all types except nfloats and aggregates
 * (structs and unions) must start and end in an eightbyte (or the part
 * we are looking at).
 */
static int
_jit_classify_structpart(jit_type_t struct_type, unsigned int start,
						 unsigned int start_offset, unsigned int end_offset)
{
	int arg_class = X86_64_ARG_NO_CLASS;
	unsigned int num_fields = jit_type_num_fields(struct_type);
	unsigned int current_field;
	
	for(current_field = 0; current_field < num_fields; ++current_field)
	{
		jit_nuint field_offset = jit_type_get_offset(struct_type,
													 current_field);

		if(field_offset <= end_offset)
		{
			/* The field starts at a place that's inerresting for us */
			jit_type_t field_type = jit_type_get_field(struct_type,
													   current_field);
			jit_nuint field_size = jit_type_get_size(field_type); 

			if(field_offset + field_size > start_offset)
			{
				/* The field is at least partially in the part we are */
				/* looking at */
				int arg_class2 = X86_64_ARG_NO_CLASS;

				if(is_struct_or_union(field_type))
				{
					/* We have to check this struct recursively */
					unsigned int current_start;
					unsigned int nested_struct_start;
					unsigned int nested_struct_end;

					current_start = start + start_offset;
					if(field_offset < current_start)
					{
						nested_struct_start = current_start - field_offset;
					}
					else
					{
						nested_struct_start = 0;
					}
					if(field_offset + field_size - 1 > end_offset)
					{
						/* The struct ends beyond the part we are looking at */
						nested_struct_end = field_offset + field_size -
												(nested_struct_start + 1);
					}
					else
					{
						nested_struct_end = field_size - 1;
					}
					arg_class2 = _jit_classify_structpart(field_type,
														  start + field_offset,
														  nested_struct_start,
														  nested_struct_end);
				}
				else
				{
					if((start + start_offset) & (field_size - 1))
					{
						/* The field is misaligned */
						return X86_64_ARG_MEMORY;
					}
					arg_class2 = _jit_classify_arg(field_type, 0);
				}
				if(arg_class == X86_64_ARG_NO_CLASS)
				{
					arg_class = arg_class2;
				}
				else if(arg_class != arg_class2)
				{
					if(arg_class == X86_64_ARG_MEMORY ||
					   arg_class2 == X86_64_ARG_MEMORY)
					{
						arg_class = X86_64_ARG_MEMORY;
					}
					else if(arg_class == X86_64_ARG_INTEGER ||
					   arg_class2 == X86_64_ARG_INTEGER)
					{
						arg_class = X86_64_ARG_INTEGER;
					}
					else if(arg_class == X86_64_ARG_X87 ||
					   arg_class2 == X86_64_ARG_X87)
					{
						arg_class = X86_64_ARG_MEMORY;
					}
					else
					{
						arg_class = X86_64_ARG_SSE;
					}
				}
			}
		}
	}
	return arg_class;
}

static int
_jit_classify_struct(jit_param_passing_t *passing,
					_jit_param_t *param, jit_type_t param_type)
{
	jit_nuint size = (jit_nuint)jit_type_get_size(param_type);

	if(size <= 8)
	{
		int arg_class;
	
		arg_class = _jit_classify_structpart(param_type, 0, 0, size - 1);
		if(arg_class == X86_64_ARG_NO_CLASS)
		{
			arg_class = X86_64_ARG_SSE;
		}
		if(arg_class == X86_64_ARG_INTEGER)
		{
			if(passing->word_index < passing->max_word_regs)
			{
				/* Set the arg class to the number of registers used */
				param->arg_class = 1;

				/* Set the first register to the register used */
				param->un.reg[0] = passing->word_regs[passing->word_index];
				++(passing->word_index);
			}
			else
			{
				/* Set the arg class to stack */
				param->arg_class = JIT_ARG_CLASS_STACK;

				/* Allocate the slot in the arg passing frame */
				_jit_alloc_param_slot(passing, param, param_type);
			}			
		}
		else if(arg_class == X86_64_ARG_SSE)
		{
			if(passing->float_index < passing->max_float_regs)
			{
				/* Set the arg class to the number of registers used */
				param->arg_class = 1;

				/* Set the first register to the register used */
				param->un.reg[0] =	passing->float_regs[passing->float_index];
				++(passing->float_index);
			}
			else
			{
				/* Set the arg class to stack */
				param->arg_class = JIT_ARG_CLASS_STACK;

				/* Allocate the slot in the arg passing frame */
				_jit_alloc_param_slot(passing, param, param_type);
			}
		}
		else
		{
			/* Set the arg class to stack */
			param->arg_class = JIT_ARG_CLASS_STACK;

			/* Allocate the slot in the arg passing frame */
			_jit_alloc_param_slot(passing, param, param_type);
		}
	}
	else if(size <= 16)
	{
		int arg_class1;
		int arg_class2;

		arg_class1 = _jit_classify_structpart(param_type, 0, 0, 7);
		arg_class2 = _jit_classify_structpart(param_type, 0, 8, size - 1);
		if(arg_class1 == X86_64_ARG_NO_CLASS)
		{
			arg_class1 = X86_64_ARG_SSE;
		}
		if(arg_class2 == X86_64_ARG_NO_CLASS)
		{
			arg_class2 = X86_64_ARG_SSE;
		}
		if(arg_class1 == X86_64_ARG_SSE && arg_class2 == X86_64_ARG_SSE)
		{
			/* We use only one sse register in this case */
			if(passing->float_index < passing->max_float_regs)
			{
				/* Set the arg class to the number of registers used */
				param->arg_class = 1;

				/* Set the first register to the register used */
				param->un.reg[0] =	passing->float_regs[passing->float_index];
				++(passing->float_index);
			}
			else
			{
				/* Set the arg class to stack */
				param->arg_class = JIT_ARG_CLASS_STACK;

				/* Allocate the slot in the arg passing frame */
				_jit_alloc_param_slot(passing, param, param_type);
			}
		}
		else if(arg_class1 == X86_64_ARG_MEMORY ||
				arg_class2 == X86_64_ARG_MEMORY)
		{
			/* Set the arg class to stack */
			param->arg_class = JIT_ARG_CLASS_STACK;

			/* Allocate the slot in the arg passing frame */
			_jit_alloc_param_slot(passing, param, param_type);
		}
		else if(arg_class1 == X86_64_ARG_INTEGER &&
				arg_class2 == X86_64_ARG_INTEGER)
		{
			/* We need two general purpose registers in this case */
			if((passing->word_index + 1) < passing->max_word_regs)
			{
				/* Set the arg class to the number of registers used */
				param->arg_class = 2;

				/* Assign the registers */
				param->un.reg[0] = passing->word_regs[passing->word_index];
				++(passing->word_index);
				param->un.reg[1] = passing->word_regs[passing->word_index];
				++(passing->word_index);
			}
			else
			{
				/* Set the arg class to stack */
				param->arg_class = JIT_ARG_CLASS_STACK;

				/* Allocate the slot in the arg passing frame */
				_jit_alloc_param_slot(passing, param, param_type);
			}			
		}
		else
		{
			/* We need one xmm and one general purpose register */
			if((passing->word_index < passing->max_word_regs) &&
			   (passing->float_index < passing->max_float_regs))
			{
				/* Set the arg class to the number of registers used */
				param->arg_class = 2;

				if(arg_class1 == X86_64_ARG_INTEGER)
				{
					param->un.reg[0] = passing->word_regs[passing->word_index];
					++(passing->word_index);
					param->un.reg[1] =	passing->float_regs[passing->float_index];
					++(passing->float_index);
				}
				else
				{
					param->un.reg[0] =	passing->float_regs[passing->float_index];
					++(passing->float_index);
					param->un.reg[1] = passing->word_regs[passing->word_index];
					++(passing->word_index);
				}
			}
			else
			{
				/* Set the arg class to stack */
				param->arg_class = JIT_ARG_CLASS_STACK;

				/* Allocate the slot in the arg passing frame */
				_jit_alloc_param_slot(passing, param, param_type);
			}
		}
	}
	else
	{
		/* Set the arg class to stack */
		param->arg_class = JIT_ARG_CLASS_STACK;

		/* Allocate the slot in the arg passing frame */
		_jit_alloc_param_slot(passing, param, param_type);
	}
	return 1;
}

int
_jit_classify_param(jit_param_passing_t *passing,
					_jit_param_t *param, jit_type_t param_type)
{
	if(is_struct_or_union(param_type))
	{
		return _jit_classify_struct(passing, param, param_type);
	}
	else
	{
		int arg_class;

		arg_class = _jit_classify_arg(param_type, 0);

		switch(arg_class)
		{
			case X86_64_ARG_INTEGER:
			{
				if(passing->word_index < passing->max_word_regs)
				{
					/* Set the arg class to the number of registers used */
					param->arg_class = 1;

					/* Set the first register to the register used */
					param->un.reg[0] = passing->word_regs[passing->word_index];
					++(passing->word_index);
				}
				else
				{
					/* Set the arg class to stack */
					param->arg_class = JIT_ARG_CLASS_STACK;

					/* Allocate the slot in the arg passing frame */
					_jit_alloc_param_slot(passing, param, param_type);
				}
			}
			break;

			case X86_64_ARG_SSE:
			{
				if(passing->float_index < passing->max_float_regs)
				{
					/* Set the arg class to the number of registers used */
					param->arg_class = 1;

					/* Set the first register to the register used */
					param->un.reg[0] =	passing->float_regs[passing->float_index];
					++(passing->float_index);
				}
				else
				{
					/* Set the arg class to stack */
					param->arg_class = JIT_ARG_CLASS_STACK;

					/* Allocate the slot in the arg passing frame */
					_jit_alloc_param_slot(passing, param, param_type);
				}
			}
			break;

			case X86_64_ARG_MEMORY:
			{
				/* Set the arg class to stack */
				param->arg_class = JIT_ARG_CLASS_STACK;

				/* Allocate the slot in the arg passing frame */
				_jit_alloc_param_slot(passing, param, param_type);
			}
			break;
		}
	}
	return 1;
}

static int
_jit_classify_struct_return(jit_param_passing_t *passing,
					_jit_param_t *param, jit_type_t return_type)
{
	/* Initialize the param passing structure */
	jit_memset(passing, 0, sizeof(jit_param_passing_t));
	jit_memset(param, 0, sizeof(_jit_param_t));

	passing->word_regs = _jit_word_return_regs;
	passing->max_word_regs = _jit_num_word_return_regs;
	passing->float_regs = _jit_sse_return_regs;
	passing->max_float_regs = _jit_num_sse_return_regs;

	if(!(_jit_classify_struct(passing, param, return_type)))
	{
		return 0;
	}

	return 1;
}

/*
 * Load a struct to the register(s) in which it will be returned.
 */
static unsigned char *
return_struct(unsigned char *inst, jit_function_t func, int ptr_reg)
{
	jit_type_t return_type;
	jit_type_t signature = jit_function_get_signature(func);

	return_type = jit_type_get_return(signature);
	if(is_struct_or_union(return_type))
	{
		jit_nuint size;
		jit_param_passing_t passing;
		_jit_param_t return_param;

		if(!_jit_classify_struct_return(&passing, &return_param,
										return_type))
		{
			/* It's an error so simply return insn */
			return inst;	
		}
		
		size = jit_type_get_size(return_type);
		if(size <= 8)
		{
			/* one register is used for returning the value */
			if(IS_GENERAL_REG(return_param.un.reg[0]))
			{
				int reg = _jit_reg_info[return_param.un.reg[0]].cpu_reg;

				if(size <= 4)
				{
					x86_64_mov_reg_regp_size(inst, reg, ptr_reg, 4);
				}
				else
				{
					x86_64_mov_reg_regp_size(inst, reg, ptr_reg, 8);
				}
			}
			else
			{
				int reg = _jit_reg_info[return_param.un.reg[0]].cpu_reg;

				if(size <= 4)
				{
					x86_64_movss_reg_regp(inst, reg, ptr_reg);
				}
				else
				{
					x86_64_movsd_reg_regp(inst, reg, ptr_reg);
				}
			}
		}
		else
		{
			/* In this case we might need up to two registers */
			if(return_param.arg_class == 1)
			{
				/* This must be one xmm register */
				int reg = _jit_reg_info[return_param.un.reg[0]].cpu_reg;
				int alignment = jit_type_get_alignment(return_type);

				if((alignment & 0xf) == 0)
				{
					/* The type is aligned on a 16 byte boundary */
					x86_64_movaps_reg_regp(inst, reg, ptr_reg);
				}
				else
				{
					x86_64_movups_reg_regp(inst, reg, ptr_reg);
				}
			}
			else
			{
				int reg = _jit_reg_info[return_param.un.reg[0]].cpu_reg;

				if(IS_GENERAL_REG(return_param.un.reg[0]))
				{
					x86_64_mov_reg_regp_size(inst, reg,
											 ptr_reg, 8);
				}
				else
				{
					x86_64_movsd_reg_regp(inst, reg, ptr_reg);
				}
				size -= 8;
				reg = _jit_reg_info[return_param.un.reg[1]].cpu_reg;
				if(IS_GENERAL_REG(return_param.un.reg[1]))
				{
					if(size <= 4)
					{
						x86_64_mov_reg_membase_size(inst, reg, ptr_reg,
													8, 4);
					}
					else
					{
						x86_64_mov_reg_membase_size(inst, reg, ptr_reg,
													8, 8);
					}
				}
				else
				{
					if(size <= 4)
					{
						x86_64_movss_reg_membase(inst, reg,
												 ptr_reg, 8);
					}
					else
					{
						x86_64_movsd_reg_membase(inst, reg,
												 ptr_reg, 8);
					}
				}
			}
		}
	}
	return inst;
}

/*
 * Flush a struct return value from the registers to the value
 * on the stack.
 */
static unsigned char *
flush_return_struct(unsigned char *inst, jit_value_t value)
{
	jit_type_t return_type;

	return_type = jit_value_get_type(value);
	if(is_struct_or_union(return_type))
	{
		jit_nuint size;
		jit_nint offset;
		jit_param_passing_t passing;
		_jit_param_t return_param;

		if(!_jit_classify_struct_return(&passing, &return_param, return_type))
		{
			/* It's an error so simply return insn */
			return inst;	
		}

		return_param.value = value;

		_jit_gen_fix_value(value);
		size = jit_type_get_size(return_type);
		offset = value->frame_offset;
		if(size <= 8)
		{
			/* one register is used for returning the value */
			if(IS_GENERAL_REG(return_param.un.reg[0]))
			{
				int reg = _jit_reg_info[return_param.un.reg[0]].cpu_reg;

				if(size <= 4)
				{
					x86_64_mov_membase_reg_size(inst, X86_64_RBP, offset, reg, 4);
				}
				else
				{
					x86_64_mov_membase_reg_size(inst, X86_64_RBP, offset, reg, 8);
				}
			}
			else
			{
				int reg = _jit_reg_info[return_param.un.reg[0]].cpu_reg;

				if(size <= 4)
				{
					x86_64_movss_membase_reg(inst, X86_64_RBP, offset, reg);
				}
				else
				{
					x86_64_movsd_membase_reg(inst, X86_64_RBP, offset, reg);
				}
			}
		}
		else
		{
			/* In this case we might need up to two registers */
			if(return_param.arg_class == 1)
			{
				/* This must be one xmm register */
				int reg = _jit_reg_info[return_param.un.reg[0]].cpu_reg;
				int alignment = jit_type_get_alignment(return_type);

				if((alignment & 0xf) == 0)
				{
					/* The type is aligned on a 16 byte boundary */
					x86_64_movaps_membase_reg(inst, X86_64_RBP, offset, reg);
				}
				else
				{
					x86_64_movups_membase_reg(inst, X86_64_RBP, offset, reg);
				}
			}
			else
			{
				int reg = _jit_reg_info[return_param.un.reg[0]].cpu_reg;

				if(IS_GENERAL_REG(return_param.un.reg[0]))
				{
					x86_64_mov_membase_reg_size(inst, X86_64_RBP, offset,
												reg, 8);
				}
				else
				{
					x86_64_movsd_membase_reg(inst, X86_64_RBP, offset, reg);
				}
				size -= 8;
				reg = _jit_reg_info[return_param.un.reg[1]].cpu_reg;
				if(IS_GENERAL_REG(return_param.un.reg[1]))
				{
					if(size <= 4)
					{
						x86_64_mov_membase_reg_size(inst, X86_64_RBP,
													offset + 8, reg, 4);
					}
					else
					{
						x86_64_mov_membase_reg_size(inst, X86_64_RBP,
													offset + 8, reg, 8);
					}
				}
				else
				{
					if(size <= 4)
					{
						x86_64_movss_membase_reg(inst, X86_64_RBP,
												 offset + 8, reg);
					}
					else
					{
						x86_64_movsd_membase_reg(inst, X86_64_RBP,
												 offset + 8, reg);
					}
				}
			}
		}
	}
	return inst;
}

#define	TODO()		\
	do { \
		fprintf(stderr, "TODO at %s, %d\n", __FILE__, (int)__LINE__); \
	} while (0)

void
_jit_gen_insn(jit_gencode_t gen, jit_function_t func,
			  jit_block_t block, jit_insn_t insn)
{
	switch(insn->opcode)
	{
	#define JIT_INCLUDE_RULES
	#include "jit-rules-x86-64.inc"
	#undef JIT_INCLUDE_RULES

	default:
		{
			fprintf(stderr, "TODO(%x) at %s, %d\n",
				(int)(insn->opcode), __FILE__, (int)__LINE__);
		}
		break;
	}
}

/*
 * Fixup the passing area after all parameters have been allocated either
 * in registers or on the stack.
 * This is typically used for adding pad words for keeping the stack aligned.
 */
void
_jit_fix_call_stack(jit_param_passing_t *passing)
{
	if((passing->stack_size & 0x0f) != 0)
	{
		passing->stack_size = (passing->stack_size + 0x0f) & ~((jit_nint)0x0f);
		passing->stack_pad = 1;
	}
}

/*
 * Setup the call stack before pushing any parameters.
 * This is used usually for pushing pad words for alignment.
 * The function is needed only if the backend doesn't work with the
 * parameter area.
 */
int
_jit_setup_call_stack(jit_function_t func, jit_param_passing_t *passing)
{
	if(passing->stack_pad)
	{
		int current;
		jit_value_t pad_value;

		pad_value = jit_value_create_nint_constant(func, jit_type_nint, 0);
		if(!pad_value)
		{
			return 0;
		}
		for(current = 0; current < passing->stack_pad; ++current)
		{
			if(!jit_insn_push(func, pad_value))
			{
				return 0;
			}
		}
	}
	return 1;
}

/*
 * Push a parameter onto the stack.
 */
static int
push_param(jit_function_t func, _jit_param_t *param, jit_type_t type)
{
	if(is_struct_or_union(type) && !is_struct_or_union(param->value->type))
	{
		jit_value_t value;

		if(!(value = jit_insn_address_of(func, param->value)))
		{
			return 0;
		}
	#ifdef JIT_USE_PARAM_AREA
		/* Copy the value into the outgoing parameter area, by pointer */
		if(!jit_insn_set_param_ptr(func, value, type, param->un.offset))
		{
			return 0;
		}
	#else
		/* Push the parameter value onto the stack, by pointer */
		if(!jit_insn_push_ptr(func, value, type))
		{
			return 0;
		}
		if(param->stack_pad)
		{
			int current;
			jit_value_t pad_value;

			pad_value = jit_value_create_nint_constant(func, jit_type_nint, 0);
			if(!pad_value)
			{
				return 0;
			}
			for(current = 0; current < param->stack_pad; ++current)
			{
				if(!jit_insn_push(func, pad_value))
				{
					return 0;
				}
			}
		}
	#endif
	}
	else
	{
	#ifdef JIT_USE_PARAM_AREA
		/* Copy the value into the outgoing parameter area */
		if(!jit_insn_set_param(func, param->value, param->un.offset))
		{
			return 0;
		}
	#else
		/* Push the parameter value onto the stack */
		if(!jit_insn_push(func, param->value))
		{
			return 0;
		}
		if(param->stack_pad)
		{
			int current;
			jit_value_t pad_value;

			pad_value = jit_value_create_nint_constant(func, jit_type_nint, 0);
			if(!pad_value)
			{
				return 0;
			}
			for(current = 0; current < param->stack_pad; ++current)
			{
				if(!jit_insn_push(func, pad_value))
				{
					return 0;
				}
			}
		}
	#endif
	}
	return 1;
}

int
_jit_setup_incoming_param(jit_function_t func, _jit_param_t *param,
						  jit_type_t param_type)
{
	if(param->arg_class == JIT_ARG_CLASS_STACK)
	{
		/* The parameter is passed on the stack */
		if(!jit_insn_incoming_frame_posn
				(func, param->value, param->un.offset))
		{
			return 0;
		}
	}
	else
	{
		param_type = jit_type_remove_tags(param_type);

		switch(param_type->kind)
		{
			case JIT_TYPE_STRUCT:
			case JIT_TYPE_UNION:
			{
				if(param->arg_class == 1)
				{
					if(!jit_insn_incoming_reg(func, param->value, param->un.reg[0]))
					{
						return 0;
					}
				}
				else
				{
					/* These cases have to be handled specially */
				}
			}
			break;

			default:
			{
				if(!jit_insn_incoming_reg(func, param->value, param->un.reg[0]))
				{
					return 0;
				}
			}
			break;
		}
	}
	return 1;
}

int
_jit_setup_outgoing_param(jit_function_t func, _jit_param_t *param,
						  jit_type_t param_type)
{
	if(param->arg_class == JIT_ARG_CLASS_STACK)
	{
		/* The parameter is passed on the stack */
		if(!push_param(func, param, param_type))
		{
			return 0;
		}
	}
	else
	{
		param_type = jit_type_remove_tags(param_type);

		switch(param_type->kind)
		{
			case JIT_TYPE_STRUCT:
			case JIT_TYPE_UNION:
			{
				/* These cases have to be handled specially */
				if(param->arg_class == 1)
				{
					/* Only one xmm register is used for passing this argument */
					if(!jit_insn_outgoing_reg(func, param->value, param->un.reg[0]))
					{
						return 0;
					}
				}
				else
				{
					/* We need two registers for passing the value */
					jit_nuint size = (jit_nuint)jit_type_get_size(param_type);

					jit_value_t struct_ptr;

					if(!(struct_ptr = jit_insn_address_of(func, param->value)))
					{
						return 0;
					}
					if(IS_GENERAL_REG(param->un.reg[0]))
					{
						jit_value_t param_value;

						param_value = jit_insn_load_relative(func, struct_ptr,
															 0, jit_type_ulong);
						if(!param_value)
						{
							return 0;
						}
						if(!jit_insn_outgoing_reg(func, param_value, param->un.reg[0]))
						{
							return 0;
						}
					}
					else
					{
						jit_value_t param_value;

						param_value = jit_insn_load_relative(func, struct_ptr,
															 0, jit_type_float64);
						if(!param_value)
						{
							return 0;
						}
						if(!jit_insn_outgoing_reg(func, param_value, param->un.reg[0]))
						{
							return 0;
						}
					}
					size -= 8;
					if(IS_GENERAL_REG(param->un.reg[1]))
					{
						if(size == 1)
						{
							jit_value_t param_value;

							param_value = jit_insn_load_relative(func, struct_ptr,
																 8, jit_type_ubyte);
							if(!param_value)
							{
								return 0;
							}
							if(!jit_insn_outgoing_reg(func, param_value, param->un.reg[1]))
							{
								return 0;
							}
						}
						else if(size == 2)
						{
							jit_value_t param_value;

							param_value = jit_insn_load_relative(func, struct_ptr,
																 8, jit_type_ushort);
							if(!param_value)
							{
								return 0;
							}
							if(!jit_insn_outgoing_reg(func, param_value, param->un.reg[0]))
							{
								return 0;
							}
						}
						else if(size <= 4)
						{
							jit_value_t param_value;

							param_value = jit_insn_load_relative(func, struct_ptr,
																 8, jit_type_uint);
							if(!param_value)
							{
								return 0;
							}
							if(!jit_insn_outgoing_reg(func, param_value, param->un.reg[0]))
							{
								return 0;
							}
						}
						else
						{
							jit_value_t param_value;

							param_value = jit_insn_load_relative(func, struct_ptr,
																 8, jit_type_ulong);
							if(!param_value)
							{
								return 0;
							}
							if(!jit_insn_outgoing_reg(func, param_value, param->un.reg[0]))
							{
								return 0;
							}
						}
					}
					else
					{
						if(size <= 4)
						{
							jit_value_t param_value;

							param_value = jit_insn_load_relative(func, struct_ptr,
																 8, jit_type_float32);
							if(!param_value)
							{
								return 0;
							}
							if(!jit_insn_outgoing_reg(func, param_value, param->un.reg[0]))
							{
								return 0;
							}
						}
						else
						{
							jit_value_t param_value;

							param_value = jit_insn_load_relative(func, struct_ptr,
																 8, jit_type_float64);
							if(!param_value)
							{
								return 0;
							}
							if(!jit_insn_outgoing_reg(func, param_value, param->un.reg[0]))
							{
								return 0;
							}
						}
					}
				}
			}
			break;

			default:
			{
				if(!jit_insn_outgoing_reg(func, param->value, param->un.reg[0]))
				{
					return 0;
				}
			}
			break;
		}
	}
	return 1;
}

int
_jit_setup_return_value(jit_function_t func, jit_value_t return_value,
						jit_type_t return_type)

{
	/* Structure values must be flushed into the frame, and
	   everything else ends up in a register */
	if(is_struct_or_union(return_type))
	{
		jit_param_passing_t passing;
		_jit_param_t return_param;

		if(!_jit_classify_struct_return(&passing, &return_param, return_type))
		{
			/* It's an error so simply return insn */
			return 0;	
		}

		if(return_param.arg_class == 1)
		{
			if(!jit_insn_return_reg(func, return_value,
									return_param.un.reg[0]))
			{
				return 0;
			}
		}
		else
		{
			if(!jit_insn_flush_struct(func, return_value))
			{
				return 0;
			}
		}
	}
	else if(return_type == jit_type_float32 ||
			return_type == jit_type_float64)
	{
		if(!jit_insn_return_reg(func, return_value, X86_64_REG_XMM0))
		{
			return 0;
		}
	}
	else if(return_type == jit_type_nfloat)
	{
		if(!jit_insn_return_reg(func, return_value, X86_64_REG_ST0))
		{
			return 0;
		}
	}
	else if(return_type->kind != JIT_TYPE_VOID)
	{
		if(!jit_insn_return_reg(func, return_value, X86_64_REG_RAX))
		{
			return 0;
		}
	}
	return 1;
}

void
_jit_init_args(int abi, jit_param_passing_t *passing)
{
	passing->max_word_regs = _jit_num_word_regs;
	passing->word_regs = _jit_word_arg_regs;
	passing->max_float_regs = _jit_num_float_regs;
	passing->float_regs = _jit_float_arg_regs;
}

int
_jit_create_entry_insns(jit_function_t func)
{
	jit_type_t signature = func->signature;
	int abi = jit_type_get_abi(signature);
	unsigned int num_args = jit_type_num_params(signature);
	jit_param_passing_t passing;
	_jit_param_t param[num_args];
	_jit_param_t nested_param;
	_jit_param_t struct_return_param;
	int current_param;

	/* Reset the local variable frame size for this function */
	func->builder->frame_size = JIT_INITIAL_FRAME_SIZE;

	/* Initialize the param passing structure */
	jit_memset(&passing, 0, sizeof(jit_param_passing_t));
	jit_memset(param, 0, sizeof(_jit_param_t) * num_args);

	passing.params = param;
	passing.stack_size = JIT_INITIAL_STACK_OFFSET;

	/* Let the specific backend initialize it's part of the params */
	_jit_init_args(abi, &passing);

	/* If the function is nested, then we need an extra parameter
	   to pass the pointer to the parent's local variable frame */
	if(func->nested_parent)
	{
		jit_memset(&nested_param, 0, sizeof(_jit_param_t));
		if(!(_jit_classify_param(&passing, &nested_param,
								 jit_type_void_ptr)))
		{
			return 0;
		}
	}

	/* Allocate the structure return pointer */
	if(jit_value_get_struct_pointer(func))
	{
		jit_memset(&struct_return_param, 0, sizeof(_jit_param_t));
		if(!(_jit_classify_param(&passing, &struct_return_param,
								 jit_type_void_ptr)))
		{
			return 0;
		}
	}

	/* Let the backend classify the parameters */
	for(current_param = 0; current_param < num_args; current_param++)
	{
		jit_type_t param_type;

		param_type = jit_type_get_param(signature, current_param);
		param_type = jit_type_normalize(param_type);
		
		if(!(_jit_classify_param(&passing, &(passing.params[current_param]),
								 param_type)))
		{
			return 0;
		}
	}

	/* Now we can setup the incoming parameters */
	for(current_param = 0; current_param < num_args; current_param++)
	{
		jit_type_t param_type;

		param_type = jit_type_get_param(signature, current_param);
		if(!(param[current_param].value))
		{
			if(!(param[current_param].value = jit_value_get_param(func, current_param)))
			{
				return 0;
			}
		}
		if(!_jit_setup_incoming_param(func, &(param[current_param]), param_type))
		{
			return 0;
		}
	}

	return 1;
}

int _jit_create_call_setup_insns
	(jit_function_t func, jit_type_t signature,
	 jit_value_t *args, unsigned int num_args,
	 int is_nested, int nesting_level, jit_value_t *struct_return, int flags)
{
	int abi = jit_type_get_abi(signature);
	jit_type_t return_type;
	jit_value_t value;
	jit_value_t return_ptr;
	int current_param;
	jit_param_passing_t passing;
	_jit_param_t param[num_args];
	_jit_param_t nested_param;
	_jit_param_t struct_return_param;

	/* Initialize the param passing structure */
	jit_memset(&passing, 0, sizeof(jit_param_passing_t));
	jit_memset(param, 0, sizeof(_jit_param_t) * num_args);

	passing.params = param;
	passing.stack_size = 0;

	/* Let the specific backend initialize it's part of the params */
	_jit_init_args(abi, &passing);

	/* Determine how many parameters are going to end up in word registers,
	   and compute the largest stack size needed to pass stack parameters */
	if(is_nested)
	{
		jit_memset(&nested_param, 0, sizeof(_jit_param_t));
		if(!(_jit_classify_param(&passing, &nested_param,
								 jit_type_void_ptr)))
		{
			return 0;
		}
	}

	/* Determine if we need an extra hidden parameter for returning a 
	   structure */
	return_type = jit_type_get_return(signature);
	if(jit_type_return_via_pointer(return_type))
	{
		value = jit_value_create(func, return_type);
		if(!value)
		{
			return 0;
		}
		*struct_return = value;
		return_ptr = jit_insn_address_of(func, value);
		if(!return_ptr)
		{
			return 0;
		}
		jit_memset(&struct_return_param, 0, sizeof(_jit_param_t));
		if(!(_jit_classify_param(&passing, &struct_return_param,
								 jit_type_void_ptr)))
		{
			return 0;
		}
		struct_return_param.value = return_ptr;
	}
	else
	{
		*struct_return = 0;
		return_ptr = 0;
	}

	/* Let the backend classify the parameters */
	for(current_param = 0; current_param < num_args; current_param++)
	{
		jit_type_t param_type;

		param_type = jit_type_get_param(signature, current_param);
		param_type = jit_type_normalize(param_type);
		
		if(!(_jit_classify_param(&passing, &(passing.params[current_param]),
								 param_type)))
		{
			return 0;
		}
		/* Set the argument value */
		passing.params[current_param].value = args[current_param];
	}

#ifdef JIT_USE_PARAM_AREA
	if(passing.stack_size > func->builder->param_area_size)
	{
		func->builder->param_area_size = passing.stack_size;
	}
#else
	/* Let the backend do final adjustments to the passing area */
	_jit_fix_call_stack(&passing);

	/* Flush deferred stack pops from previous calls if too many
	   parameters have collected up on the stack since last time */
	if(!jit_insn_flush_defer_pop(func, 32 - passing.stack_size))
	{
		return 0;
	}

	if(!_jit_setup_call_stack(func, &passing))
	{
		return 0;
	}
#endif

	/* Now setup the arguments on the stack or in the registers in reverse order */
	current_param = num_args;
	while(current_param > 0)
	{
		jit_type_t param_type;

		--current_param;
		param_type = jit_type_get_param(signature, current_param);
		if(!_jit_setup_outgoing_param(func, &(param[current_param]), param_type))
		{
			return 0;
		}
	}

	/* Add the structure return pointer if required */
	if(return_ptr)
	{
		if(!_jit_setup_outgoing_param(func, &struct_return_param, return_type))
		{
			return 0;
		}
	}

	return 1;
}

int
_jit_create_call_return_insns(jit_function_t func, jit_type_t signature,
							  jit_value_t *args, unsigned int num_args,
							  jit_value_t return_value, int is_nested)
{
	int abi = jit_type_get_abi(signature);
	jit_type_t return_type;
	int ptr_return;
	int current_param;
#ifndef JIT_USE_PARAM_AREA
	jit_param_passing_t passing;
	_jit_param_t param[num_args];
	_jit_param_t nested_param;
	_jit_param_t struct_return_param;
#endif /* !JIT_USE_PARAM_AREA */

	return_type = jit_type_normalize(jit_type_get_return(signature));
	ptr_return = jit_type_return_via_pointer(return_type);
#ifndef JIT_USE_PARAM_AREA
	/* Initialize the param passing structure */
	jit_memset(&passing, 0, sizeof(jit_param_passing_t));
	jit_memset(param, 0, sizeof(_jit_param_t) * num_args);

	passing.params = param;
	passing.stack_size = 0;

	/* Let the specific backend initialize it's part of the params */
	_jit_init_args(abi, &passing);

	/* Determine how many parameters are going to end up in word registers,
	   and compute the largest stack size needed to pass stack parameters */
	if(is_nested)
	{
		jit_memset(&nested_param, 0, sizeof(_jit_param_t));
		if(!(_jit_classify_param(&passing, &nested_param,
								 jit_type_void_ptr)))
		{
			return 0;
		}
	}

	/* Determine if we need an extra hidden parameter for returning a 
	   structure */
	if(ptr_return)
	{
		jit_memset(&struct_return_param, 0, sizeof(_jit_param_t));
		if(!(_jit_classify_param(&passing, &struct_return_param,
								 jit_type_void_ptr)))
		{
			return 0;
		}
	}

	/* Let the backend classify the parameters */
	for(current_param = 0; current_param < num_args; current_param++)
	{
		jit_type_t param_type;

		param_type = jit_type_get_param(signature, current_param);
		param_type = jit_type_normalize(param_type);
		
		if(!(_jit_classify_param(&passing, &(passing.params[current_param]),
								 param_type)))
		{
			return 0;
		}
	}

	/* Let the backend do final adjustments to the passing area */
	_jit_fix_call_stack(&passing);

	/* Pop the bytes from the system stack */
	if(passing.stack_size > 0)
	{
		if(!jit_insn_defer_pop_stack(func, passing.stack_size))
		{
			return 0;
		}
	}
#endif /* !JIT_USE_PARAM_AREA */

	/* Bail out now if we don't need to worry about return values */
	if(!return_value || ptr_return)
	{
		return 1;
	}

	if(!_jit_setup_return_value(func, return_value, return_type))
	{
		return 0;
	}

	/* Everything is back where it needs to be */
	return 1;
}

#endif /* JIT_BACKEND_X86_64 */