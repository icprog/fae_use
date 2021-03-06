/*
 * Copyright (c) 2013-2014, ARM Limited and Contributors. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * Neither the name of ARM nor the names of its contributors may be used
 * to endorse or promote products derived from this software without specific
 * prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


/*******************************************************************************
 * This is the Secure Payload Dispatcher (SPD). The dispatcher is meant to be a
 * plug-in component to the Secure Monitor, registered as a runtime service. The
 * SPD is expected to be a functional extension of the Secure Payload (SP) that
 * executes in Secure EL1. The Secure Monitor will delegate all SMCs targeting
 * the Trusted OS/Applications range to the dispatcher. The SPD will either
 * handle the request locally or delegate it to the Secure Payload. It is also
 * responsible for initialising and maintaining communication with the SP.
 ******************************************************************************/
#include <arch_helpers.h>
#include <assert.h>
#include <bl_common.h>
#include <bl31.h>
#include <context_mgmt.h>
#include <debug.h>
#include <errno.h>
#include <platform.h>
#include <runtime_svc.h>
#include <stddef.h>
#include <tkcored.h>
#include <uuid.h>
#include "tkcored_private.h"
#include "teesmc.h"
#include "teesmc_opteed.h"

/*******************************************************************************
 * Address of the entrypoint vector table in OPTEE. It is
 * initialised once on the primary core after a cold boot.
 ******************************************************************************/
optee_vectors_t *optee_vectors;

/*******************************************************************************
 * Array to keep track of per-cpu OPTEE state
 ******************************************************************************/
optee_context_t opteed_sp_context[OPTEED_CORE_COUNT];
uint32_t opteed_rw;

//static uint64_t opteeBaseCoreMpidr;


//static int32_t opteed_init(void);
static int32_t opteed_init(unsigned int tee_entry, unsigned int boot_arg_addr);

/*******************************************************************************
 * This function is the handler registered for S-EL1 interrupts by the
 * OPTEED. It validates the interrupt and upon success arranges entry into
 * the OPTEE at 'optee_fiq_entry()' for handling the interrupt.
 ******************************************************************************/

static uint64_t opteed_sel1_interrupt_handler(uint32_t id,
					    uint32_t flags,
					    void *handle,
					    void *cookie)
{
	uint32_t linear_id;
	uint64_t mpidr;
	optee_context_t *optee_ctx;

	/* Check the security state when the exception was generated */
	assert(get_interrupt_src_ss(flags) == NON_SECURE);

#if IMF_READ_INTERRUPT_ID
	/* Check the security status of the interrupt */
	assert(plat_ic_get_interrupt_type(id) == INTR_TYPE_S_EL1);
#endif

	/* Sanity check the pointer to this cpu's context */
	mpidr = read_mpidr();
	assert(handle == cm_get_context(mpidr, NON_SECURE));

	/* Save the non-secure context before entering the OPTEE */
	cm_el1_sysregs_context_save(NON_SECURE);

	/* Get a reference to this cpu's OPTEE context */
	linear_id = platform_get_core_pos(mpidr);
	optee_ctx = &opteed_sp_context[linear_id];
	assert(&optee_ctx->cpu_ctx == cm_get_context(mpidr, SECURE));

#if 0
	SMC_SET_EL3(&optee_ctx->cpu_ctx,
		    CTX_SPSR_EL3,
		    SPSR_64(MODE_EL1, MODE_SP_ELX, DISABLE_ALL_EXCEPTIONS));
#endif
	SMC_SET_EL3(&optee_ctx->cpu_ctx,
		    CTX_ELR_EL3,
		    (uint64_t)&optee_vectors->fiq_entry);
	cm_el1_sysregs_context_restore(SECURE);
	cm_set_next_eret_context(SECURE);

	/*
	 * Tell the OPTEE that it has to handle an FIQ synchronously. Also the
	 * instruction in normal world where the interrupt was generated is
	 * passed for debugging purposes. It is safe to retrieve this address
	 * from ELR_EL3 as the secure context will not take effect until
	 * el3_exit().
	 */
	SMC_RET2(&optee_ctx->cpu_ctx, OPTEE_HANDLE_FIQ_AND_RETURN,
		 read_elr_el3());
}

// Core-specific context initialization for non-primary cores
void opteed_init_core(uint64_t mpidr) {
#if 0
	uint32_t linear_id = platform_get_core_pos(mpidr);
	optee_context_t *optee_ctx = &opteed_sp_context[linear_id];
	uint32_t boot_core_nro;
  
	tbase_init_secure_context(tbase_ctx);
  
	boot_core_nro = platform_get_core_pos(opteeBootCoreMpidr);
	save_sysregs_core(boot_core_nro, linear_id);
#endif
}

/*******************************************************************************
 * Secure Payload Dispatcher setup. The SPD finds out the SP entrypoint and type
 * (aarch32/aarch64) if not already known and initialises the context for entry
 * into the SP for its initialisation.
 ******************************************************************************/
int32_t opteed_setup(void)
{
	entry_point_info_t *image_info;
	int32_t rc;
	uint64_t mpidr = read_mpidr();
	uint32_t linear_id;

	linear_id = platform_get_core_pos(mpidr);

	/*
	 * Get information about the Secure Payload (BL32) image. Its
	 * absence is a critical failure.  TODO: Add support to
	 * conditionally include the SPD service
	 */
	image_info = bl31_plat_get_next_image_ep_info(SECURE);
	assert(image_info);

	/*
	 * If there's no valid entry point for SP, we return a non-zero value
	 * signalling failure initializing the service. We bail out without
	 * registering any handlers
	 */
	if (!image_info->pc)
		return 1;

	/*
	 * We could inspect the SP image and determine it's execution
	 * state i.e whether AArch32 or AArch64. Assuming it's AArch32
	 * for the time being.
	 */
	opteed_rw = OPTEE_AARCH32;
	rc = opteed_init_secure_context(image_info->pc, opteed_rw,
				        mpidr, &opteed_sp_context[linear_id]);
	assert(rc == 0);

	/*
	 * All OPTEED initialization done. Now register our init function with
	 * BL31 for deferred invocation
	 */
	printf("bl31_register_bl32_init tkcored\n");
    bl31_register_bl32_init(&opteed_init);

	return rc;
}

/*******************************************************************************
 * This function passes control to the OPTEE image (BL32) for the first time
 * on the primary cpu after a cold boot. It assumes that a valid secure
 * context has already been created by opteed_setup() which can be directly
 * used.  It also assumes that a valid non-secure context has been
 * initialised by PSCI so it does not need to save and restore any
 * non-secure state. This function performs a synchronous entry into
 * OPTEE. OPTEE passes control back to this routine through a SMC.
 ******************************************************************************/
static int32_t opteed_init(unsigned int tee_entry, unsigned int boot_arg_addr)
{
	uint64_t mpidr = read_mpidr();
	uint32_t linear_id = platform_get_core_pos(mpidr), flags;
	uint64_t rc;

	optee_context_t *optee_ctx = &opteed_sp_context[linear_id];

	// Save el1 registers in case non-secure world has already been set up.
  	cm_el1_sysregs_context_save(NON_SECURE);

	/*
	 * Arrange for an entry into the test secure payload. We expect an array
	 * of vectors in return
	 */
	printf("entry tkcore 0x%x\n", tee_entry);
	printf("tee_boot_arg_addr : 0x%x\n", boot_arg_addr);
	int i = 0;
	for (i = 0; i < 16; i++) {
		printf("0x%x \n", ((char *)(uintptr_t)boot_arg_addr)[i]);	
	}
	rc = opteed_synchronous_sp_entry(optee_ctx);
	printf("Init TEE return\n");
	assert(rc != 0);
	if (rc) {
		set_optee_pstate(optee_ctx->state, OPTEE_PSTATE_ON);

		/*
		 * OPTEE has been successfully initialized. Register power
		 * managemnt hooks with PSCI
		 */
		psci_register_spd_pm_hook(&opteed_pm);
	}

	/*
	 * Register an interrupt handler for S-EL1 interrupts when generated
	 * during code executing in the non-secure state.
	 */
	flags = 0;
	set_interrupt_rm_flag(flags, NON_SECURE);
	rc = register_interrupt_type_handler(INTR_TYPE_S_EL1,
					     opteed_sel1_interrupt_handler,
					     flags);
	if (rc)
		panic();

	cm_el1_sysregs_context_restore(NON_SECURE);

	return rc;
}


/*******************************************************************************
 * This function is responsible for handling all SMCs in the Trusted OS/App
 * range from the non-secure state as defined in the SMC Calling Convention
 * Document. It is also responsible for communicating with the Secure
 * payload to delegate work and return results back to the non-secure
 * state. Lastly it will also return any information that OPTEE needs to do
 * the work assigned to it.
 ******************************************************************************/
uint64_t opteed_smc_handler(uint32_t smc_fid,
			 uint64_t x1,
			 uint64_t x2,
			 uint64_t x3,
			 uint64_t x4,
			 void *cookie,
			 void *handle,
			 uint64_t flags)
{
	cpu_context_t *ns_cpu_context;
	unsigned long mpidr = read_mpidr();
	uint32_t linear_id = platform_get_core_pos(mpidr);
	optee_context_t *optee_ctx = &opteed_sp_context[linear_id];

	printf("%s ns 0x%x\n", __func__, is_caller_non_secure(flags));

	/*
	 * Determine which security state this SMC originated from
	 */

	if (is_caller_non_secure(flags)) {
		/*
		 * This is a fresh request from the non-secure client.
		 * The parameters are in x1 and x2. Figure out which
		 * registers need to be preserved, save the non-secure
		 * state and send the request to the secure payload.
		 */
		assert(handle == cm_get_context(mpidr, NON_SECURE));

		cm_el1_sysregs_context_save(NON_SECURE);

		/*
		 * We are done stashing the non-secure context. Ask the
		 * OPTEE to do the work now.
		 */

		/*
		 * Verify if there is a valid context to use, copy the
		 * operation type and parameters to the secure context
		 * and jump to the fast smc entry point in the secure
		 * payload. Entry into S-EL1 will take place upon exit
		 * from this function.
		 */
		assert(&optee_ctx->cpu_ctx ==
			cm_get_context(mpidr, SECURE));

		/* Set appropriate entry for SMC.
		 * We expect OPTEE to manage the PSTATE.I and PSTATE.F
		 * flags as appropriate.
		 */
		if (GET_SMC_TYPE(smc_fid) == SMC_TYPE_FAST) {
			cm_set_elr_el3(SECURE, (uint64_t)
					&optee_vectors->fast_smc_entry);
		} else {
			cm_set_elr_el3(SECURE, (uint64_t)
					&optee_vectors->std_smc_entry);
		}

		cm_el1_sysregs_context_restore(SECURE);
		cm_set_next_eret_context(SECURE);

		/* Propagate hypervisor client ID */
		write_ctx_reg(get_gpregs_ctx(&optee_ctx->cpu_ctx),
			      CTX_GPREG_X7,
			      read_ctx_reg(get_gpregs_ctx(handle),
					   CTX_GPREG_X7));

		SMC_RET4(&optee_ctx->cpu_ctx, smc_fid, x1, x2, x3);
	}

	/*
	 * Returning from OPTEE
	 */

	switch (smc_fid) {
	/*
	 * OPTEE has finished initialising itself after a cold boot
	 */
	case TEESMC32_OPTEED_RETURN_ENTRY_DONE:
	case TEESMC64_OPTEED_RETURN_ENTRY_DONE:
		/*
		 * Stash the OPTEE entry points information. This is done
		 * only once on the primary cpu
		 */
		assert(optee_vectors == NULL);
		optee_vectors = (optee_vectors_t *) x1;

		/*
		 * OPTEE reports completion. The OPTEED must have initiated
		 * the original request through a synchronous entry into
		 * the SP. Jump back to the original C runtime context.
		 */
		 printf("RETURN_ENTRY_DONE smc_fid %x x1 0x%x x2 0x%x x3 0x%x x4 0x%x\n",
		 	smc_fid, x1, x2, x3, x4);

		opteed_synchronous_sp_exit(optee_ctx, x1);


	/*
	 * OPTEE has finished turning itself on in response to an earlier
	 * psci cpu_on request
	 */
	case TEESMC32_OPTEED_RETURN_ON_DONE:
	case TEESMC64_OPTEED_RETURN_ON_DONE:

	/*
	 * OPTEE has finished turning itself off in response to an earlier
	 * psci cpu_off request.
	 */
	case TEESMC32_OPTEED_RETURN_OFF_DONE:
	case TEESMC64_OPTEED_RETURN_OFF_DONE:
	/*
	 * OPTEE has finished resuming itself after an earlier psci
	 * cpu_suspend request.
	 */
	case TEESMC32_OPTEED_RETURN_RESUME_DONE:
	case TEESMC64_OPTEED_RETURN_RESUME_DONE:
	/*
	 * OPTEE has finished suspending itself after an earlier psci
	 * cpu_suspend request.
	 */
	case TEESMC32_OPTEED_RETURN_SUSPEND_DONE:
	case TEESMC64_OPTEED_RETURN_SUSPEND_DONE:

		printf("RETURN_SUSPEND_DONE: smc_fid %x x1 0x%x x2 0x%x x3 0x%x x4 0x%x\n",
			smc_fid, x1, x2, x3, x4);

		/*
		 * OPTEE reports completion. The OPTEED must have initiated the
		 * original request through a synchronous entry into OPTEE.
		 * Jump back to the original C runtime context, and pass x1 as
		 * return value to the caller
		 */
		opteed_synchronous_sp_exit(optee_ctx, x1);

	/*
	 * OPTEE is returning from a call or being preemted from a call, in
	 * either case execution should resume in the normal world.
	 */
	case TEESMC32_OPTEED_RETURN_CALL_DONE:
	case TEESMC64_OPTEED_RETURN_CALL_DONE:
		/*
		 * This is the result from the secure client of an
		 * earlier request. The results are in x0-x3. Copy it
		 * into the non-secure context, save the secure state
		 * and return to the non-secure state.
		 */
		assert(handle == cm_get_context(mpidr, SECURE));

		printf("RETURN_CALL_DONE: smc_fid %x x1 0x%x x2 0x%x x3 0x%x x4 0x%x\n",
			smc_fid, x1, x2, x3, x4);

		cm_el1_sysregs_context_save(SECURE);

		/* Get a reference to the non-secure context */
		ns_cpu_context = cm_get_context(mpidr, NON_SECURE);
		assert(ns_cpu_context);

		/* Restore non-secure state */
		cm_el1_sysregs_context_restore(NON_SECURE);
		cm_set_next_eret_context(NON_SECURE);

		x1 &= ((1UL << 32) - 1);
		x2 &= ((1UL << 32) - 1);
		x3 &= ((1UL << 32) - 1);
		x4 &= ((1UL << 32) - 1);

		printf("RETURN_CALL_DONE_after: smc_fid %x x1 0x%x x2 0x%x x3 0x%x x4 0x%x\n",
			smc_fid, x1, x2, x3, x4);

		SMC_RET4(ns_cpu_context, x1, x2, x3, x4);

	/*
	 * OPTEE has finished handling a S-EL1 FIQ interrupt. Execution
	 * should resume in the normal world.
	 */
	case TEESMC32_OPTEED_RETURN_FIQ_DONE:
	case TEESMC64_OPTEED_RETURN_FIQ_DONE:
		/* Get a reference to the non-secure context */
		ns_cpu_context = cm_get_context(mpidr, NON_SECURE);
		assert(ns_cpu_context);

		printf("RETURN_FIQ_DONE: smc_fid %x x1 0x%x x2 0x%x x3 0x%x x4 0x%x\n",
			smc_fid, x1, x2, x2, x3, x4);

		/*
		 * Restore non-secure state. There is no need to save the
		 * secure system register context since OPTEE was supposed
		 * to preserve it during S-EL1 interrupt handling.
		 */
		cm_el1_sysregs_context_restore(NON_SECURE);
		cm_set_next_eret_context(NON_SECURE);

		SMC_RET0((uint64_t) ns_cpu_context);

	default:
		assert(0);
	}
}

/* Define an OPTEED runtime service descriptor for fast SMC calls */
DECLARE_RT_SVC(
	opteed_fast,

	OEN_TOS_START,
	OEN_TOS_END,
	SMC_TYPE_FAST,
	opteed_setup,
	opteed_smc_handler
);

/* Define an OPTEED runtime service descriptor for standard SMC calls */
DECLARE_RT_SVC(
	opteed_std,

	OEN_TOS_START,
	OEN_TOS_END,
	SMC_TYPE_STD,
	NULL,
	opteed_smc_handler
);
