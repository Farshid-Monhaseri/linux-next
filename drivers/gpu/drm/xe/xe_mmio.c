// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021-2023 Intel Corporation
 */

#include "xe_mmio.h"

#include <linux/delay.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/minmax.h>
#include <linux/pci.h>

#include <drm/drm_managed.h>
#include <drm/drm_print.h>

#include "regs/xe_bars.h"
#include "regs/xe_regs.h"
#include "xe_device.h"
#include "xe_gt.h"
#include "xe_gt_printk.h"
#include "xe_gt_sriov_vf.h"
#include "xe_macros.h"
#include "xe_sriov.h"

static void tiles_fini(void *arg)
{
	struct xe_device *xe = arg;
	struct xe_tile *tile;
	int id;

	for_each_tile(tile, xe, id)
		tile->mmio.regs = NULL;
}

int xe_mmio_probe_tiles(struct xe_device *xe)
{
	size_t tile_mmio_size = SZ_16M, tile_mmio_ext_size = xe->info.tile_mmio_ext_size;
	u8 id, tile_count = xe->info.tile_count;
	struct xe_gt *gt = xe_root_mmio_gt(xe);
	struct xe_tile *tile;
	void __iomem *regs;
	u32 mtcfg;

	if (tile_count == 1)
		goto add_mmio_ext;

	if (!xe->info.skip_mtcfg) {
		mtcfg = xe_mmio_read64_2x32(gt, XEHP_MTCFG_ADDR);
		tile_count = REG_FIELD_GET(TILE_COUNT, mtcfg) + 1;
		if (tile_count < xe->info.tile_count) {
			drm_info(&xe->drm, "tile_count: %d, reduced_tile_count %d\n",
					xe->info.tile_count, tile_count);
			xe->info.tile_count = tile_count;

			/*
			 * FIXME: Needs some work for standalone media, but should be impossible
			 * with multi-tile for now.
			 */
			xe->info.gt_count = xe->info.tile_count;
		}
	}

	regs = xe->mmio.regs;
	for_each_tile(tile, xe, id) {
		tile->mmio.size = tile_mmio_size;
		tile->mmio.regs = regs;
		regs += tile_mmio_size;
	}

add_mmio_ext:
	/*
	 * By design, there's a contiguous multi-tile MMIO space (16MB hard coded per tile).
	 * When supported, there could be an additional contiguous multi-tile MMIO extension
	 * space ON TOP of it, and hence the necessity for distinguished MMIO spaces.
	 */
	if (xe->info.has_mmio_ext) {
		regs = xe->mmio.regs + tile_mmio_size * tile_count;

		for_each_tile(tile, xe, id) {
			tile->mmio_ext.size = tile_mmio_ext_size;
			tile->mmio_ext.regs = regs;

			regs += tile_mmio_ext_size;
		}
	}

	return devm_add_action_or_reset(xe->drm.dev, tiles_fini, xe);
}

static void mmio_fini(void *arg)
{
	struct xe_device *xe = arg;

	pci_iounmap(to_pci_dev(xe->drm.dev), xe->mmio.regs);
	xe->mmio.regs = NULL;
}

int xe_mmio_init(struct xe_device *xe)
{
	struct xe_tile *root_tile = xe_device_get_root_tile(xe);
	struct pci_dev *pdev = to_pci_dev(xe->drm.dev);
	const int mmio_bar = 0;

	/*
	 * Map the entire BAR.
	 * The first 16MB of the BAR, belong to the root tile, and include:
	 * registers (0-4MB), reserved space (4MB-8MB) and GGTT (8MB-16MB).
	 */
	xe->mmio.size = pci_resource_len(pdev, mmio_bar);
	xe->mmio.regs = pci_iomap(pdev, mmio_bar, GTTMMADR_BAR);
	if (xe->mmio.regs == NULL) {
		drm_err(&xe->drm, "failed to map registers\n");
		return -EIO;
	}

	/* Setup first tile; other tiles (if present) will be setup later. */
	root_tile->mmio.size = SZ_16M;
	root_tile->mmio.regs = xe->mmio.regs;

	return devm_add_action_or_reset(xe->drm.dev, mmio_fini, xe);
}

u8 xe_mmio_read8(struct xe_gt *gt, struct xe_reg reg)
{
	struct xe_tile *tile = gt_to_tile(gt);
	u32 addr = xe_mmio_adjusted_addr(gt, reg.addr);

	return readb((reg.ext ? tile->mmio_ext.regs : tile->mmio.regs) + addr);
}

u16 xe_mmio_read16(struct xe_gt *gt, struct xe_reg reg)
{
	struct xe_tile *tile = gt_to_tile(gt);
	u32 addr = xe_mmio_adjusted_addr(gt, reg.addr);

	return readw((reg.ext ? tile->mmio_ext.regs : tile->mmio.regs) + addr);
}

void xe_mmio_write32(struct xe_gt *gt, struct xe_reg reg, u32 val)
{
	struct xe_tile *tile = gt_to_tile(gt);
	u32 addr = xe_mmio_adjusted_addr(gt, reg.addr);

	writel(val, (reg.ext ? tile->mmio_ext.regs : tile->mmio.regs) + addr);
}

u32 xe_mmio_read32(struct xe_gt *gt, struct xe_reg reg)
{
	struct xe_tile *tile = gt_to_tile(gt);
	u32 addr = xe_mmio_adjusted_addr(gt, reg.addr);

	if (!reg.vf && IS_SRIOV_VF(gt_to_xe(gt)))
		return xe_gt_sriov_vf_read32(gt, reg);

	return readl((reg.ext ? tile->mmio_ext.regs : tile->mmio.regs) + addr);
}

u32 xe_mmio_rmw32(struct xe_gt *gt, struct xe_reg reg, u32 clr, u32 set)
{
	u32 old, reg_val;

	old = xe_mmio_read32(gt, reg);
	reg_val = (old & ~clr) | set;
	xe_mmio_write32(gt, reg, reg_val);

	return old;
}

int xe_mmio_write32_and_verify(struct xe_gt *gt,
			       struct xe_reg reg, u32 val, u32 mask, u32 eval)
{
	u32 reg_val;

	xe_mmio_write32(gt, reg, val);
	reg_val = xe_mmio_read32(gt, reg);

	return (reg_val & mask) != eval ? -EINVAL : 0;
}

bool xe_mmio_in_range(const struct xe_gt *gt,
		      const struct xe_mmio_range *range,
		      struct xe_reg reg)
{
	u32 addr = xe_mmio_adjusted_addr(gt, reg.addr);

	return range && addr >= range->start && addr <= range->end;
}

/**
 * xe_mmio_read64_2x32() - Read a 64-bit register as two 32-bit reads
 * @gt: MMIO target GT
 * @reg: register to read value from
 *
 * Although Intel GPUs have some 64-bit registers, the hardware officially
 * only supports GTTMMADR register reads of 32 bits or smaller.  Even if
 * a readq operation may return a reasonable value, that violation of the
 * spec shouldn't be relied upon and all 64-bit register reads should be
 * performed as two 32-bit reads of the upper and lower dwords.
 *
 * When reading registers that may be changing (such as
 * counters), a rollover of the lower dword between the two 32-bit reads
 * can be problematic.  This function attempts to ensure the upper dword has
 * stabilized before returning the 64-bit value.
 *
 * Note that because this function may re-read the register multiple times
 * while waiting for the value to stabilize it should not be used to read
 * any registers where read operations have side effects.
 *
 * Returns the value of the 64-bit register.
 */
u64 xe_mmio_read64_2x32(struct xe_gt *gt, struct xe_reg reg)
{
	struct xe_reg reg_udw = { .addr = reg.addr + 0x4 };
	u32 ldw, udw, oldudw, retries;

	reg.addr = xe_mmio_adjusted_addr(gt, reg.addr);
	reg_udw.addr = xe_mmio_adjusted_addr(gt, reg_udw.addr);

	/* we shouldn't adjust just one register address */
	xe_gt_assert(gt, reg_udw.addr == reg.addr + 0x4);

	oldudw = xe_mmio_read32(gt, reg_udw);
	for (retries = 5; retries; --retries) {
		ldw = xe_mmio_read32(gt, reg);
		udw = xe_mmio_read32(gt, reg_udw);

		if (udw == oldudw)
			break;

		oldudw = udw;
	}

	xe_gt_WARN(gt, retries == 0,
		   "64-bit read of %#x did not stabilize\n", reg.addr);

	return (u64)udw << 32 | ldw;
}

/**
 * xe_mmio_wait32() - Wait for a register to match the desired masked value
 * @gt: MMIO target GT
 * @reg: register to read value from
 * @mask: mask to be applied to the value read from the register
 * @val: desired value after applying the mask
 * @timeout_us: time out after this period of time. Wait logic tries to be
 * smart, applying an exponential backoff until @timeout_us is reached.
 * @out_val: if not NULL, points where to store the last unmasked value
 * @atomic: needs to be true if calling from an atomic context
 *
 * This function polls for the desired masked value and returns zero on success
 * or -ETIMEDOUT if timed out.
 *
 * Note that @timeout_us represents the minimum amount of time to wait before
 * giving up. The actual time taken by this function can be a little more than
 * @timeout_us for different reasons, specially in non-atomic contexts. Thus,
 * it is possible that this function succeeds even after @timeout_us has passed.
 */
int xe_mmio_wait32(struct xe_gt *gt, struct xe_reg reg, u32 mask, u32 val, u32 timeout_us,
		   u32 *out_val, bool atomic)
{
	ktime_t cur = ktime_get_raw();
	const ktime_t end = ktime_add_us(cur, timeout_us);
	int ret = -ETIMEDOUT;
	s64 wait = 10;
	u32 read;

	for (;;) {
		read = xe_mmio_read32(gt, reg);
		if ((read & mask) == val) {
			ret = 0;
			break;
		}

		cur = ktime_get_raw();
		if (!ktime_before(cur, end))
			break;

		if (ktime_after(ktime_add_us(cur, wait), end))
			wait = ktime_us_delta(end, cur);

		if (atomic)
			udelay(wait);
		else
			usleep_range(wait, wait << 1);
		wait <<= 1;
	}

	if (ret != 0) {
		read = xe_mmio_read32(gt, reg);
		if ((read & mask) == val)
			ret = 0;
	}

	if (out_val)
		*out_val = read;

	return ret;
}

/**
 * xe_mmio_wait32_not() - Wait for a register to return anything other than the given masked value
 * @gt: MMIO target GT
 * @reg: register to read value from
 * @mask: mask to be applied to the value read from the register
 * @val: value to match after applying the mask
 * @timeout_us: time out after this period of time. Wait logic tries to be
 * smart, applying an exponential backoff until @timeout_us is reached.
 * @out_val: if not NULL, points where to store the last unmasked value
 * @atomic: needs to be true if calling from an atomic context
 *
 * This function polls for a masked value to change from a given value and
 * returns zero on success or -ETIMEDOUT if timed out.
 *
 * Note that @timeout_us represents the minimum amount of time to wait before
 * giving up. The actual time taken by this function can be a little more than
 * @timeout_us for different reasons, specially in non-atomic contexts. Thus,
 * it is possible that this function succeeds even after @timeout_us has passed.
 */
int xe_mmio_wait32_not(struct xe_gt *gt, struct xe_reg reg, u32 mask, u32 val, u32 timeout_us,
		       u32 *out_val, bool atomic)
{
	ktime_t cur = ktime_get_raw();
	const ktime_t end = ktime_add_us(cur, timeout_us);
	int ret = -ETIMEDOUT;
	s64 wait = 10;
	u32 read;

	for (;;) {
		read = xe_mmio_read32(gt, reg);
		if ((read & mask) != val) {
			ret = 0;
			break;
		}

		cur = ktime_get_raw();
		if (!ktime_before(cur, end))
			break;

		if (ktime_after(ktime_add_us(cur, wait), end))
			wait = ktime_us_delta(end, cur);

		if (atomic)
			udelay(wait);
		else
			usleep_range(wait, wait << 1);
		wait <<= 1;
	}

	if (ret != 0) {
		read = xe_mmio_read32(gt, reg);
		if ((read & mask) != val)
			ret = 0;
	}

	if (out_val)
		*out_val = read;

	return ret;
}
