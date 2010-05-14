/*
 * exceptions-ia64.c: exception support for IA64
 *
 * Authors:
 *   Zoltan Varga (vargaz@gmail.com)
 *
 * (C) 2001 Ximian, Inc.
 */

/*
 * We implement exception handling with the help of the libuwind library:
 * 
 * http://www.hpl.hp.com/research/linux/libunwind/
 *
 *  Under IA64 all functions are assumed to have unwind info, we do not need to save
 * the machine state in the LMF. But we have to generate unwind info for all 
 * dynamically generated code.
 */

#include <config.h>
#include <glib.h>
#include <signal.h>
#include <string.h>
#include <sys/ucontext.h>

#include <mono/arch/ia64/ia64-codegen.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/tabledefs.h>
#include <mono/metadata/threads.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/exception.h>
#include <mono/metadata/gc-internal.h>
#include <mono/metadata/mono-debug.h>

#include "mini.h"
#include "mini-ia64.h"

#define ALIGN_TO(val,align) (((val) + ((align) - 1)) & ~((align) - 1))

#define GP_SCRATCH_REG 31
#define GP_SCRATCH_REG2 30

G_GNUC_UNUSED static void
print_ctx (MonoContext *ctx)
{
	char name[256];
	unw_word_t off, ip, sp;
	unw_proc_info_t pi;
	int res;

	unw_get_proc_name (&ctx->cursor, name, 256, &off);
	unw_get_proc_info(&ctx->cursor, &pi);
	res = unw_get_reg (&ctx->cursor, UNW_IA64_IP, &ip);
	g_assert (res == 0);
	res = unw_get_reg (&ctx->cursor, UNW_IA64_SP, &sp);
	g_assert (res == 0);

	printf ("%s:%lx [%lx-%lx] SP: %lx\n", name, ip - pi.start_ip, pi.start_ip, pi.end_ip, sp);
}

static gpointer
ia64_create_ftnptr (gpointer ptr)
{
	gpointer *desc = mono_global_codeman_reserve (2 * sizeof (gpointer));
	desc [0] = ptr;
	desc [1] = NULL;

	return desc;
}

static void
restore_context (MonoContext *ctx)
{
	int res;
	unw_word_t ip;

	res = unw_get_reg (&ctx->cursor, UNW_IA64_IP, &ip);
	g_assert (res == 0);

	/* Set this to 0 to tell OP_START_HANDLER that it doesn't have to set the frame pointer */
	res = unw_set_reg (&ctx->cursor, UNW_IA64_GR + 15, 0);
	g_assert (res == 0);

	unw_resume (&ctx->cursor);
}

/*
 * mono_arch_get_restore_context:
 *
 * Returns a pointer to a method which restores a previously saved sigcontext.
 */
gpointer
mono_arch_get_restore_context (MonoTrampInfo **info, gboolean aot)
{
	g_assert (!aot);
	if (info)
		*info = NULL;

	return restore_context;
}

static gpointer
get_real_call_filter (void)
{
	static gpointer filter;
	static gboolean inited = FALSE;
	guint8 *start;
	Ia64CodegenState code;
	int in0, local0, out0, nout;
	unw_dyn_info_t *di;
	unw_dyn_region_info_t *r_pro, *r_body, *r_epilog;

	if (inited)
		return filter;

	start = mono_global_codeman_reserve (1024);

	/* int call_filter (guint64 fp, guint64 ip) */

	/*
	 * We have to create a register+stack frame similar to the frame which 
	 * contains the filter. 
	 * - setting fp
	 * - setting up a register stack frame
	 * These cannot be set up in this function, because the fp register is a 
	 * stacked register which is different in each method. Also, the register 
	 * stack frame is different in each method. So we pass the FP value in a a 
	 * non-stacked register and the code generated by the OP_START_HANDLER 
	 * opcode will copy it to the appropriate register after setting up the 
	 * register stack frame.
	 * The stacked registers are not need to be set since variables used in
	 * handler regions are never allocated to registers.
	 */

	in0 = 32;
	local0 = in0 + 2;
	out0 = local0 + 4;
	nout = 0;

	ia64_codegen_init (code, start);

	ia64_codegen_set_one_ins_per_bundle (code, TRUE);

	ia64_unw_save_reg (code, UNW_IA64_AR_PFS, UNW_IA64_GR + local0 + 0);
	ia64_alloc (code, local0 + 0, local0 - in0, out0 - local0, nout, 0);
	ia64_unw_save_reg (code, UNW_IA64_RP, UNW_IA64_GR + local0 + 1);
	ia64_mov_from_br (code, local0 + 1, IA64_B0);

	ia64_begin_bundle (code);

	r_pro = mono_ia64_create_unwind_region (&code);

	/* Frame pointer */
	ia64_mov (code, IA64_R15, in0 + 0);
	/* Target ip */
	ia64_mov_to_br (code, IA64_B6, in0 + 1);

	/* Call the filter */
	ia64_br_call_reg (code, IA64_B0, IA64_B6);

	/* R8 contains the result of the filter */

	/* FIXME: Add unwind info for this */

	ia64_begin_bundle (code);

	r_body = mono_ia64_create_unwind_region (&code);
	r_pro->next = r_body;

	ia64_mov_to_ar_i (code, IA64_PFS, local0 + 0);
	ia64_mov_ret_to_br (code, IA64_B0, local0 + 1);
	ia64_br_ret_reg (code, IA64_B0);

	ia64_begin_bundle (code);

	r_epilog = mono_ia64_create_unwind_region (&code);
	r_body->next = r_epilog;

	ia64_codegen_set_one_ins_per_bundle (code, FALSE);

	ia64_codegen_close (code);

	g_assert ((code.buf - start) <= 256);

	mono_arch_flush_icache (start, code.buf - start);

	di = g_malloc0 (sizeof (unw_dyn_info_t));
	di->start_ip = (unw_word_t) start;
	di->end_ip = (unw_word_t) code.buf;
	di->gp = 0;
	di->format = UNW_INFO_FORMAT_DYNAMIC;
	di->u.pi.name_ptr = (unw_word_t)"throw_trampoline";
	di->u.pi.regions = r_body;

	_U_dyn_register (di);

    filter = ia64_create_ftnptr (start);

	inited = TRUE;

	return filter;
}

static int
call_filter (MonoContext *ctx, gpointer ip)
{
	int (*filter) (MonoContext *, gpointer);
	gpointer fp = MONO_CONTEXT_GET_BP (ctx);

	filter = get_real_call_filter ();

	return filter (fp, ip);
}

/*
 * mono_arch_get_call_filter:
 *
 * Returns a pointer to a method which calls an exception filter. We
 * also use this function to call finally handlers (we pass NULL as 
 * @exc object in this case).
 */
gpointer
mono_arch_get_call_filter (MonoTrampInfo **info, gboolean aot)
{
	g_assert (!aot);
	if (info)
		*info = NULL;

	/* Initialize the real filter non-lazily */
	get_real_call_filter ();

	return call_filter;
}

static void
throw_exception (MonoObject *exc, guint64 rethrow)
{
	unw_context_t unw_ctx;
	MonoContext ctx;
	MonoJitInfo *ji;
	unw_word_t ip, sp;
	int res;

	if (mono_object_isinst (exc, mono_defaults.exception_class)) {
		MonoException *mono_ex = (MonoException*)exc;
		if (!rethrow)
			mono_ex->stack_trace = NULL;
	}

	res = unw_getcontext (&unw_ctx);
	g_assert (res == 0);
	res = unw_init_local (&ctx.cursor, &unw_ctx);
	g_assert (res == 0);

	/* 
	 * Unwind until the first managed frame. This is needed since 
	 * mono_handle_exception expects the variables in the original context to
	 * correspond to the method returned by mono_find_jit_info.
	 */
	while (TRUE) {
		res = unw_get_reg (&ctx.cursor, UNW_IA64_IP, &ip);
		g_assert (res == 0);

		res = unw_get_reg (&ctx.cursor, UNW_IA64_SP, &sp);
		g_assert (res == 0);

		ji = mini_jit_info_table_find (mono_domain_get (), (gpointer)ip, NULL);

		//printf ("UN: %s %lx %lx\n", ji ? ji->method->name : "", ip, sp);

		if (ji)
			break;

		res = unw_step (&ctx.cursor);

		if (res == 0) {
			/*
			 * This means an unhandled exception during the compilation of a
			 * topmost method like Main
			 */
			break;
		}
		g_assert (res >= 0);
	}
	ctx.precise_ip = FALSE;

	mono_handle_exception (&ctx, exc, (gpointer)(ip), FALSE);
	restore_context (&ctx);

	g_assert_not_reached ();
}

static gpointer
get_throw_trampoline (gboolean rethrow)
{
	guint8* start;
	Ia64CodegenState code;
	gpointer ptr = throw_exception;
	int i, in0, local0, out0;
	unw_dyn_info_t *di;
	unw_dyn_region_info_t *r_pro;

	start = mono_global_codeman_reserve (256);

	in0 = 32;
	local0 = in0 + 1;
	out0 = local0 + 2;

	ia64_codegen_init (code, start);
	ia64_alloc (code, local0 + 0, local0 - in0, out0 - local0, 3, 0);
	ia64_mov_from_br (code, local0 + 1, IA64_B0);	

	/* FIXME: This depends on the current instruction emitter */

	r_pro = g_malloc0 (_U_dyn_region_info_size (2));
	r_pro->op_count = 2;
	r_pro->insn_count = 6;
	i = 0;
	_U_dyn_op_save_reg (&r_pro->op[i++], _U_QP_TRUE, /* when=*/ 2,
						/* reg=*/ UNW_IA64_AR_PFS, /* dst=*/ UNW_IA64_GR + local0 + 0);
	_U_dyn_op_save_reg (&r_pro->op[i++], _U_QP_TRUE, /* when=*/ 5,
						/* reg=*/ UNW_IA64_RP, /* dst=*/ UNW_IA64_GR + local0 + 1);
	g_assert ((unsigned) i <= r_pro->op_count);	

	/* Set args */
	ia64_mov (code, out0 + 0, in0 + 0);
	ia64_adds_imm (code, out0 + 1, rethrow, IA64_R0);

	/* Call throw_exception */
	ia64_movl (code, GP_SCRATCH_REG, ptr);
	ia64_ld8_inc_imm (code, GP_SCRATCH_REG2, GP_SCRATCH_REG, 8);
	ia64_mov_to_br (code, IA64_B6, GP_SCRATCH_REG2);
	ia64_ld8 (code, IA64_GP, GP_SCRATCH_REG);
	ia64_br_call_reg (code, IA64_B0, IA64_B6);

	/* Not reached */
	ia64_break_i (code, 1000);
	ia64_codegen_close (code);

	g_assert ((code.buf - start) <= 256);

	mono_arch_flush_icache (start, code.buf - start);

	di = g_malloc0 (sizeof (unw_dyn_info_t));
	di->start_ip = (unw_word_t) start;
	di->end_ip = (unw_word_t) code.buf;
	di->gp = 0;
	di->format = UNW_INFO_FORMAT_DYNAMIC;
	di->u.pi.name_ptr = (unw_word_t)"throw_trampoline";
	di->u.pi.regions = r_pro;

	_U_dyn_register (di);

	return ia64_create_ftnptr (start);
}

/**
 * mono_arch_get_throw_exception:
 *
 * Returns a function pointer which can be used to raise 
 * exceptions. The returned function has the following 
 * signature: void (*func) (MonoException *exc); 
 *
 */
gpointer
mono_arch_get_throw_exception (MonoTrampInfo **info, gboolean aot)
{
	g_assert (!aot);
	if (info)
		*info = NULL;

	return get_throw_trampoline (FALSE);
}

gpointer
mono_arch_get_rethrow_exception (MonoTrampInfo **info, gboolean aot)
{
	g_assert (!aot);
	if (info)
		*info = NULL;

	return get_throw_trampoline (TRUE);
}

/**
 * mono_arch_get_throw_corlib_exception:
 *
 * Returns a function pointer which can be used to raise 
 * corlib exceptions. The returned function has the following 
 * signature: void (*func) (guint32 ex_token_index, guint32 offset); 
 * Here, offset is the offset which needs to be substracted from the caller IP 
 * to get the IP of the throw. Passing the offset has the advantage that it 
 * needs no relocations in the caller.
 */
gpointer
mono_arch_get_throw_corlib_exception (MonoTrampInfo **info, gboolean aot)
{
	static guint8* res;
	static gboolean inited = FALSE;
	guint8 *start;
	gpointer ptr;
	int i, in0, local0, out0, nout;
	Ia64CodegenState code;
	unw_dyn_info_t *di;
	unw_dyn_region_info_t *r_pro;

	g_assert (!aot);
	if (info)
		*info = NULL;

	if (inited)
		return res;

	start = mono_global_codeman_reserve (1024);

	in0 = 32;
	local0 = in0 + 2;
	out0 = local0 + 4;
	nout = 3;

	ia64_codegen_init (code, start);
	ia64_alloc (code, local0 + 0, local0 - in0, out0 - local0, nout, 0);
	ia64_mov_from_br (code, local0 + 1, IA64_RP);

	r_pro = g_malloc0 (_U_dyn_region_info_size (2));
	r_pro->op_count = 2;
	r_pro->insn_count = 6;
	i = 0;
	_U_dyn_op_save_reg (&r_pro->op[i++], _U_QP_TRUE, /* when=*/ 2,
						/* reg=*/ UNW_IA64_AR_PFS, /* dst=*/ UNW_IA64_GR + local0 + 0);
	_U_dyn_op_save_reg (&r_pro->op[i++], _U_QP_TRUE, /* when=*/ 5,
						/* reg=*/ UNW_IA64_RP, /* dst=*/ UNW_IA64_GR + local0 + 1);
	g_assert ((unsigned) i <= r_pro->op_count);	

	/* Call exception_from_token */
	ia64_movl (code, out0 + 0, mono_defaults.exception_class->image);
	ia64_mov (code, out0 + 1, in0 + 0);
	ia64_movl (code, GP_SCRATCH_REG, MONO_TOKEN_TYPE_DEF);
	ia64_add (code, out0 + 1, in0 + 0, GP_SCRATCH_REG);
	ptr = mono_exception_from_token;
	ia64_movl (code, GP_SCRATCH_REG, ptr);
	ia64_ld8_inc_imm (code, GP_SCRATCH_REG2, GP_SCRATCH_REG, 8);
	ia64_mov_to_br (code, IA64_B6, GP_SCRATCH_REG2);
	ia64_ld8 (code, IA64_GP, GP_SCRATCH_REG);
	ia64_br_call_reg (code, IA64_B0, IA64_B6);
	ia64_mov (code, local0 + 3, IA64_R8);

	/* Compute throw ip */
	ia64_mov (code, local0 + 2, local0 + 1);
	ia64_sub (code, local0 + 2, local0 + 2, in0 + 1);

	/* Trick the unwind library into using throw_ip as the IP in the caller frame */
	ia64_mov (code, local0 + 1, local0 + 2);

	/* Set args */
	ia64_mov (code, out0 + 0, local0 + 3);
	ia64_mov (code, out0 + 1, IA64_R0);

	/* Call throw_exception */
	ptr = throw_exception;
	ia64_movl (code, GP_SCRATCH_REG, ptr);
	ia64_ld8_inc_imm (code, GP_SCRATCH_REG2, GP_SCRATCH_REG, 8);
	ia64_mov_to_br (code, IA64_B6, GP_SCRATCH_REG2);
	ia64_ld8 (code, IA64_GP, GP_SCRATCH_REG);
	ia64_br_call_reg (code, IA64_B0, IA64_B6);

	ia64_break_i (code, 1002);
	ia64_codegen_close (code);

	g_assert ((code.buf - start) <= 1024);

	di = g_malloc0 (sizeof (unw_dyn_info_t));
	di->start_ip = (unw_word_t) start;
	di->end_ip = (unw_word_t) code.buf;
	di->gp = 0;
	di->format = UNW_INFO_FORMAT_DYNAMIC;
	di->u.pi.name_ptr = (unw_word_t)"throw_corlib_exception_trampoline";
	di->u.pi.regions = r_pro;

	_U_dyn_register (di);

	mono_arch_flush_icache (start, code.buf - start);

	res = ia64_create_ftnptr (start);
	inited = TRUE;

	return res;
}

/* mono_arch_find_jit_info:
 *
 * This function is used to gather information from @ctx. It return the 
 * MonoJitInfo of the corresponding function, unwinds one stack frame and
 * stores the resulting context into @new_ctx. It also stores a string 
 * describing the stack location into @trace (if not NULL), and modifies
 * the @lmf if necessary. @native_offset return the IP offset from the 
 * start of the function or -1 if that info is not available.
 */
MonoJitInfo *
mono_arch_find_jit_info (MonoDomain *domain, MonoJitTlsData *jit_tls, MonoJitInfo *res, MonoJitInfo *prev_ji, MonoContext *ctx, 
			 MonoContext *new_ctx, MonoLMF **lmf, gboolean *managed)
{
	MonoJitInfo *ji;
	int err;
	unw_word_t ip;

	*new_ctx = *ctx;
	new_ctx->precise_ip = FALSE;

	while (TRUE) {
		err = unw_get_reg (&new_ctx->cursor, UNW_IA64_IP, &ip);
		g_assert (err == 0);

		/* Avoid costly table lookup during stack overflow */
		if (prev_ji && ((guint8*)ip > (guint8*)prev_ji->code_start && ((guint8*)ip < ((guint8*)prev_ji->code_start) + prev_ji->code_size)))
			ji = prev_ji;
		else
			ji = mini_jit_info_table_find (domain, (gpointer)ip, NULL);

		if (managed)
			*managed = FALSE;

		/*
		{
			char name[256];
			unw_word_t off;

			unw_get_proc_name (&new_ctx->cursor, name, 256, &off);
			printf ("F: %s\n", name);
		}
		*/

		if (ji != NULL) {
			if (managed)
				if (!ji->method->wrapper_type)
					*managed = TRUE;

			break;
		}

		/* This is an unmanaged frame, so just unwind through it */
		/* FIXME: This returns -3 for the __clone2 frame in libc */
		err = unw_step (&new_ctx->cursor);
		if (err < 0)
			break;

		if (err == 0)
			break;
	}

	if (ji) {
		//print_ctx (new_ctx);

		err = unw_step (&new_ctx->cursor);
		g_assert (err >= 0);

		//print_ctx (new_ctx);

		return ji;
	}
	else
		return (gpointer)(gssize)-1;
}

/**
 * mono_arch_handle_exception:
 *
 * @ctx: saved processor state
 * @obj: the exception object
 */
gboolean
mono_arch_handle_exception (void *sigctx, gpointer obj, gboolean test_only)
{
	/* libunwind takes care of this */
	unw_context_t unw_ctx;
	MonoContext ctx;
	MonoJitInfo *ji;
	unw_word_t ip;
	int res;

	res = unw_getcontext (&unw_ctx);
	g_assert (res == 0);
	res = unw_init_local (&ctx.cursor, &unw_ctx);
	g_assert (res == 0);

	/* 
	 * Unwind until the first managed frame. This skips the signal handler frames
	 * too.
	 */
	while (TRUE) {
		res = unw_get_reg (&ctx.cursor, UNW_IA64_IP, &ip);
		g_assert (res == 0);

		ji = mini_jit_info_table_find (mono_domain_get (), (gpointer)ip, NULL);

		if (ji)
			break;

		res = unw_step (&ctx.cursor);
		g_assert (res >= 0);
	}
	ctx.precise_ip = TRUE;

	mono_handle_exception (&ctx, obj, (gpointer)ip, test_only);

	restore_context (&ctx);

	g_assert_not_reached ();
}

gpointer
mono_arch_ip_from_context (void *sigctx)
{
	ucontext_t *ctx = (ucontext_t*)sigctx;

	return (gpointer)ctx->uc_mcontext.sc_ip;
}
