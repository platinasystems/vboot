/* Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * High-level firmware wrapper API - entry points for kernel selection
 */

#include "2api.h"
#include "2common.h"
#include "2misc.h"
#include "2nvstorage.h"
#include "2rsa.h"
#include "2secdata.h"
#include "2sysincludes.h"
#include "load_kernel_fw.h"
#include "utility.h"
#include "vb2_common.h"
#include "vboot_api.h"
#include "vboot_kernel.h"
#include "vboot_struct.h"
#include "vboot_test.h"

/* Global variables */
static LoadKernelParams lkp;

#ifdef CHROMEOS_ENVIRONMENT
/* Global variable accessor for unit tests */
struct LoadKernelParams *VbApiKernelGetParams(void)
{
	return &lkp;
}
#endif

static vb2_error_t handle_battery_cutoff(struct vb2_context *ctx)
{
	vb2_error_t rv;

	/*
	 * Check if we need to cut-off battery. This should be done after EC
	 * FW and Aux FW are updated, and before the kernel is started.  This
	 * is to make sure all firmware is up-to-date before shipping (which
	 * is the typical use-case for cutoff).
	 */
	if (vb2_nv_get(ctx, VB2_NV_BATTERY_CUTOFF_REQUEST)) {
		VB2_DEBUG("Request to cut-off battery\n");
		vb2_nv_set(ctx, VB2_NV_BATTERY_CUTOFF_REQUEST, 0);

		/* May lose power immediately, so commit our update now. */
		rv = vb2ex_commit_data(ctx);
		if (rv)
			return rv;

		vb2ex_ec_battery_cutoff();
		return VBERROR_SHUTDOWN_REQUESTED;
	}

	return VB2_SUCCESS;
}

test_mockable
vb2_error_t VbTryLoadKernel(struct vb2_context *ctx, uint32_t get_info_flags)
{
	vb2_error_t rv = VB2_ERROR_LK_NO_DISK_FOUND;
	VbDiskInfo* disk_info = NULL;
	uint32_t disk_count = 0;
	uint32_t i;

	lkp.disk_handle = NULL;

	/* Find disks */
	if (VB2_SUCCESS != VbExDiskGetInfo(&disk_info, &disk_count,
					   get_info_flags))
		disk_count = 0;

	/* Loop over disks */
	for (i = 0; i < disk_count; i++) {
		VB2_DEBUG("trying disk %d\n", (int)i);
		/*
		 * Sanity-check what we can. FWIW, VbTryLoadKernel() is always
		 * called with only a single bit set in get_info_flags.
		 *
		 * Ensure that we got a partition with only the flags we asked
		 * for.
		 */
		if (disk_info[i].bytes_per_lba < 512 ||
			(disk_info[i].bytes_per_lba &
				(disk_info[i].bytes_per_lba  - 1)) != 0 ||
					16 > disk_info[i].lba_count ||
					get_info_flags != (disk_info[i].flags &
					~VB_DISK_FLAG_EXTERNAL_GPT)) {
			VB2_DEBUG("  skipping: bytes_per_lba=%" PRIu64
				  " lba_count=%" PRIu64 " flags=%#x\n",
				  disk_info[i].bytes_per_lba,
				  disk_info[i].lba_count,
				  disk_info[i].flags);
			continue;
		}
		lkp.disk_handle = disk_info[i].handle;
		lkp.bytes_per_lba = disk_info[i].bytes_per_lba;
		lkp.gpt_lba_count = disk_info[i].lba_count;
		lkp.streaming_lba_count = disk_info[i].streaming_lba_count
						?: lkp.gpt_lba_count;
		lkp.boot_flags |= disk_info[i].flags & VB_DISK_FLAG_EXTERNAL_GPT
				? BOOT_FLAG_EXTERNAL_GPT : 0;

		vb2_error_t new_rv = LoadKernel(ctx, &lkp);
		VB2_DEBUG("LoadKernel() = %#x\n", new_rv);

		/* Stop now if we found a kernel. */
		if (VB2_SUCCESS == new_rv) {
			VbExDiskFreeInfo(disk_info, lkp.disk_handle);
			return VB2_SUCCESS;
		}

		/* Don't update error if we already have a more specific one. */
		if (VB2_ERROR_LK_INVALID_KERNEL_FOUND != rv)
			rv = new_rv;
	}

	/* If we drop out of the loop, we didn't find any usable kernel. */
	if (get_info_flags & VB_DISK_FLAG_FIXED) {
		switch (rv) {
		case VB2_ERROR_LK_INVALID_KERNEL_FOUND:
			vb2api_fail(ctx, VB2_RECOVERY_RW_INVALID_OS, rv);
			break;
		case VB2_ERROR_LK_NO_KERNEL_FOUND:
			vb2api_fail(ctx, VB2_RECOVERY_RW_NO_KERNEL, rv);
			break;
		case VB2_ERROR_LK_NO_DISK_FOUND:
			vb2api_fail(ctx, VB2_RECOVERY_RW_NO_DISK, rv);
			break;
		default:
			vb2api_fail(ctx, VB2_RECOVERY_LK_UNSPECIFIED, rv);
			break;
		}
	}

	/* If we didn't find any good kernels, don't return a disk handle. */
	VbExDiskFreeInfo(disk_info, NULL);

	return rv;
}

/**
 * Reset any NVRAM requests.
 *
 * @param ctx		Vboot context
 * @return 1 if a reboot is required, 0 otherwise.
 */
static int vb2_reset_nv_requests(struct vb2_context *ctx)
{
	int need_reboot = 0;

	if (vb2_nv_get(ctx, VB2_NV_DISPLAY_REQUEST)) {
		VB2_DEBUG("Unset display request (undo display init)\n");
		vb2_nv_set(ctx, VB2_NV_DISPLAY_REQUEST, 0);
		need_reboot = 1;
	}

	if (vb2_nv_get(ctx, VB2_NV_DIAG_REQUEST)) {
		VB2_DEBUG("Unset diagnostic request (undo display init)\n");
		vb2_nv_set(ctx, VB2_NV_DIAG_REQUEST, 0);
		need_reboot = 1;
	}

	return need_reboot;
}

vb2_error_t VbBootNormal(struct vb2_context *ctx)
{
	struct vb2_shared_data *sd = vb2_get_sd(ctx);
	uint32_t max_rollforward = vb2_nv_get(ctx,
					      VB2_NV_KERNEL_MAX_ROLLFORWARD);

	/* Boot from fixed disk only */
	VB2_DEBUG("Entering\n");

	if (vb2_reset_nv_requests(ctx)) {
		VB2_DEBUG("Normal mode: reboot to reset NVRAM requests\n");
		return VBERROR_REBOOT_REQUIRED;
	}

	vb2_error_t rv = VbTryLoadKernel(ctx, VB_DISK_FLAG_FIXED);

	VB2_DEBUG("Checking if TPM kernel version needs advancing\n");

	/*
	 * Special case for when we're trying a slot with new firmware.
	 * Firmware updates also usually change the kernel key, which means
	 * that the new firmware can only boot a new kernel, and the old
	 * firmware in the previous slot can only boot the previous kernel.
	 *
	 * Don't roll-forward the kernel version, because we don't yet know if
	 * the new kernel will successfully boot.
	 */
	if (vb2_nv_get(ctx, VB2_NV_FW_RESULT) == VB2_FW_RESULT_TRYING) {
		VB2_DEBUG("Trying new FW; skip kernel version roll-forward.\n");
		return rv;
	}

	/*
	 * Limit kernel version rollforward if needed.  Can't limit kernel
	 * version to less than the version currently in the TPM.  That is,
	 * we're limiting rollforward, not allowing rollback.
	 */
	if (max_rollforward < sd->kernel_version_secdata)
		max_rollforward = sd->kernel_version_secdata;

	if (sd->kernel_version > max_rollforward) {
		VB2_DEBUG("Limiting TPM kernel version roll-forward "
			  "to %#x < %#x\n",
			  max_rollforward, sd->kernel_version);

		sd->kernel_version = max_rollforward;
	}

	if (sd->kernel_version > sd->kernel_version_secdata) {
		vb2_secdata_kernel_set(ctx, VB2_SECDATA_KERNEL_VERSIONS,
				       sd->kernel_version);
	}

	return rv;
}

static vb2_error_t vb2_kernel_init_kparams(struct vb2_context *ctx,
					   VbSelectAndLoadKernelParams *kparams)
{
	/* Fill in params for calls to LoadKernel() */
	memset(&lkp, 0, sizeof(lkp));
	lkp.kernel_buffer = kparams->kernel_buffer;
	lkp.kernel_buffer_size = kparams->kernel_buffer_size;

	/* Clear output params in case we fail */
	kparams->disk_handle = NULL;
	kparams->partition_number = 0;
	kparams->bootloader_address = 0;
	kparams->bootloader_size = 0;
	kparams->flags = 0;
	memset(kparams->partition_guid, 0, sizeof(kparams->partition_guid));

	return VB2_SUCCESS;
}

static void vb2_kernel_fill_kparams(struct vb2_context *ctx,
				    VbSelectAndLoadKernelParams *kparams)
{
	/* Save disk parameters */
	kparams->disk_handle = lkp.disk_handle;
	kparams->partition_number = lkp.partition_number;
	kparams->bootloader_address = lkp.bootloader_address;
	kparams->bootloader_size = lkp.bootloader_size;
	kparams->flags = lkp.flags;
	kparams->kernel_buffer = lkp.kernel_buffer;
	kparams->kernel_buffer_size = lkp.kernel_buffer_size;
	memcpy(kparams->partition_guid, lkp.partition_guid,
	       sizeof(kparams->partition_guid));
}

vb2_error_t VbSelectAndLoadKernel(struct vb2_context *ctx,
				  VbSelectAndLoadKernelParams *kparams)
{
	struct vb2_shared_data *sd = vb2_get_sd(ctx);
	vb2_error_t rv;

	/* Init nvstorage space. TODO(kitching): Remove once we add assertions
	   to vb2_nv_get and vb2_nv_set. */
	vb2_nv_init(ctx);

	rv = vb2_kernel_init_kparams(ctx, kparams);
	if (rv)
		return rv;

	rv = vb2api_kernel_phase1(ctx);
	if (rv)
		return rv;

	VB2_DEBUG("GBB flags are %#x\n", vb2_get_gbb(ctx)->flags);

	/*
	 * Do EC and Aux FW software sync unless we're in recovery mode. This
	 * has UI but it's just a single non-interactive WAIT screen.
	 */
	if (!(ctx->flags & VB2_CONTEXT_RECOVERY_MODE)) {
		rv = vb2api_ec_sync(ctx);
		if (rv)
			return rv;

		rv = vb2api_auxfw_sync(ctx);
		if (rv)
			return rv;

		rv = handle_battery_cutoff(ctx);
		if (rv)
			return rv;
	}

	/* Select boot path */
	if (ctx->flags & VB2_CONTEXT_RECOVERY_MODE) {
		vb2_clear_recovery(ctx);

		/* If we're in recovery mode just to do memory retraining, all
		   we need to do is reboot. */
		if (sd->recovery_reason == VB2_RECOVERY_TRAIN_AND_REBOOT) {
			VB2_DEBUG("Reboot after retraining in recovery\n");
			return VBERROR_REBOOT_REQUIRED;
		}

		/*
		 * Need to commit nvdata changes immediately, since we will be
		 * entering either manual recovery UI or BROKEN screen shortly.
		 */
		vb2ex_commit_data(ctx);

		/* Recovery boot.  This has UI. */
		if (LEGACY_MENU_UI)
			rv = VbBootRecoveryLegacyMenu(ctx);
		else
			rv = VbBootRecoveryLegacyClamshell(ctx);
	} else if (DIAGNOSTIC_UI && vb2_nv_get(ctx, VB2_NV_DIAG_REQUEST)) {
		vb2_nv_set(ctx, VB2_NV_DIAG_REQUEST, 0);

		/*
		 * Diagnostic boot. This has a UI but only power button
		 * is used for input so no detachable-specific UI is
		 * needed.  This mode is also 1-shot so it's placed
		 * before developer mode.
		 */
		rv = VbBootDiagnosticLegacyClamshell(ctx);
		/*
		 * The diagnostic menu should either boot a rom, or
		 * return either of reboot or shutdown.  The following
		 * check is a safety precaution.
		 */
		if (!rv)
			rv = VBERROR_REBOOT_REQUIRED;
	} else if (ctx->flags & VB2_CONTEXT_DEVELOPER_MODE) {
		/* Developer boot.  This has UI. */
		if (LEGACY_MENU_UI)
			rv = VbBootDeveloperLegacyMenu(ctx);
		else
			rv = VbBootDeveloperLegacyClamshell(ctx);
	} else {
		/* Normal boot */
		rv = VbBootNormal(ctx);
	}

	if (rv == VB2_SUCCESS)
		vb2_kernel_fill_kparams(ctx, kparams);

	return rv;
}
