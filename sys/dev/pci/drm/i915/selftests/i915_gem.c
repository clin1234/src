/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2018 Intel Corporation
 */

#include <linux/random.h>

#include "gem/selftests/igt_gem_utils.h"
#include "gem/selftests/mock_context.h"
#include "gt/intel_gt.h"
#include "gt/intel_gt_pm.h"

#include "i915_selftest.h"

#include "igt_flush_test.h"
#include "mock_drm.h"

static int switch_to_context(struct i915_gem_context *ctx)
{
	struct i915_gem_engines_iter it;
	struct intel_context *ce;
	int err = 0;

	for_each_gem_engine(ce, i915_gem_context_lock_engines(ctx), it) {
		struct i915_request *rq;

		rq = intel_context_create_request(ce);
		if (IS_ERR(rq)) {
			err = PTR_ERR(rq);
			break;
		}

		i915_request_add(rq);
	}
	i915_gem_context_unlock_engines(ctx);

	return err;
}

static void trash_stolen(struct drm_i915_private *i915)
{
	struct i915_ggtt *ggtt = &i915->ggtt;
	const u64 slot = ggtt->error_capture.start;
	const resource_size_t size = resource_size(&i915->dsm);
	unsigned long page;
	u32 prng = 0x12345678;

	/* XXX: fsck. needs some more thought... */
	if (!i915_ggtt_has_aperture(ggtt))
		return;

	for (page = 0; page < size; page += PAGE_SIZE) {
		const dma_addr_t dma = i915->dsm.start + page;
		u32 __iomem *s;
		int x;

		ggtt->vm.insert_page(&ggtt->vm, dma, slot, I915_CACHE_NONE, 0);

		s = io_mapping_map_atomic_wc(&ggtt->iomap, slot);
		for (x = 0; x < PAGE_SIZE / sizeof(u32); x++) {
			prng = next_pseudo_random32(prng);
			iowrite32(prng, &s[x]);
		}
		io_mapping_unmap_atomic(s);
	}

	ggtt->vm.clear_range(&ggtt->vm, slot, PAGE_SIZE);
}

static void simulate_hibernate(struct drm_i915_private *i915)
{
	intel_wakeref_t wakeref;

	wakeref = intel_runtime_pm_get(&i915->runtime_pm);

	/*
	 * As a final sting in the tail, invalidate stolen. Under a real S4,
	 * stolen is lost and needs to be refilled on resume. However, under
	 * CI we merely do S4-device testing (as full S4 is too unreliable
	 * for automated testing across a cluster), so to simulate the effect
	 * of stolen being trashed across S4, we trash it ourselves.
	 */
	trash_stolen(i915);

	intel_runtime_pm_put(&i915->runtime_pm, wakeref);
}

static int pm_prepare(struct drm_i915_private *i915)
{
	i915_gem_suspend(i915);

	return 0;
}

static void pm_suspend(struct drm_i915_private *i915)
{
	intel_wakeref_t wakeref;

	with_intel_runtime_pm(&i915->runtime_pm, wakeref) {
		i915_ggtt_suspend(&i915->ggtt);
		i915_gem_suspend_late(i915);
	}
}

static void pm_hibernate(struct drm_i915_private *i915)
{
	intel_wakeref_t wakeref;

	with_intel_runtime_pm(&i915->runtime_pm, wakeref) {
		i915_ggtt_suspend(&i915->ggtt);

		i915_gem_freeze(i915);
		i915_gem_freeze_late(i915);
	}
}

static void pm_resume(struct drm_i915_private *i915)
{
	intel_wakeref_t wakeref;

	/*
	 * Both suspend and hibernate follow the same wakeup path and assume
	 * that runtime-pm just works.
	 */
	with_intel_runtime_pm(&i915->runtime_pm, wakeref) {
		i915_ggtt_resume(&i915->ggtt);
		i915_gem_restore_fences(&i915->ggtt);

		i915_gem_resume(i915);
	}
}

static int igt_gem_suspend(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct i915_gem_context *ctx;
	struct file *file;
	int err;

	file = mock_file(i915);
	if (IS_ERR(file))
		return PTR_ERR(file);

	err = -ENOMEM;
	ctx = live_context(i915, file);
	if (!IS_ERR(ctx))
		err = switch_to_context(ctx);
	if (err)
		goto out;

	err = pm_prepare(i915);
	if (err)
		goto out;

	pm_suspend(i915);

	/* Here be dragons! Note that with S3RST any S3 may become S4! */
	simulate_hibernate(i915);

	pm_resume(i915);

	err = switch_to_context(ctx);
out:
	fput(file);
	return err;
}

static int igt_gem_hibernate(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct i915_gem_context *ctx;
	struct file *file;
	int err;

	file = mock_file(i915);
	if (IS_ERR(file))
		return PTR_ERR(file);

	err = -ENOMEM;
	ctx = live_context(i915, file);
	if (!IS_ERR(ctx))
		err = switch_to_context(ctx);
	if (err)
		goto out;

	err = pm_prepare(i915);
	if (err)
		goto out;

	pm_hibernate(i915);

	/* Here be dragons! */
	simulate_hibernate(i915);

	pm_resume(i915);

	err = switch_to_context(ctx);
out:
	fput(file);
	return err;
}

int i915_gem_live_selftests(struct drm_i915_private *i915)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(igt_gem_suspend),
		SUBTEST(igt_gem_hibernate),
	};

	if (intel_gt_is_wedged(&i915->gt))
		return 0;

	return i915_live_subtests(tests, i915);
}
