/*
 * Copyright (C) 2012-2013 Avionic Design GmbH
 * Copyright (C) 2012 NVIDIA CORPORATION.  All rights reserved.
 *
 * Based on the KMS/FB CMA helpers
 *   Copyright (C) 2012 Analog Device Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/console.h>

#include "drm.h"
#include "gem.h"

static inline struct tegra_fb *to_tegra_fb(struct drm_framebuffer *fb)
{
	return container_of(fb, struct tegra_fb, base);
}

#ifdef CONFIG_DRM_FBDEV_EMULATION
static inline struct tegra_fbdev *to_tegra_fbdev(struct drm_fb_helper *helper)
{
	return container_of(helper, struct tegra_fbdev, base);
}
#endif

struct tegra_bo *tegra_fb_get_plane(struct drm_framebuffer *framebuffer,
				    unsigned int index)
{
	struct tegra_fb *fb = to_tegra_fb(framebuffer);

	if (index >= framebuffer->format->num_planes)
		return NULL;

	return fb->planes[index];
}

bool tegra_fb_is_bottom_up(struct drm_framebuffer *framebuffer)
{
	struct tegra_fb *fb = to_tegra_fb(framebuffer);

	if (fb->planes[0]->flags & TEGRA_BO_BOTTOM_UP)
		return true;

	return false;
}

int tegra_fb_get_tiling(struct drm_framebuffer *framebuffer,
			struct tegra_bo_tiling *tiling)
{
	struct tegra_fb *fb = to_tegra_fb(framebuffer);
	uint64_t modifier = fb->base.modifier;

	switch (fourcc_mod_tegra_mod(modifier)) {
	case NV_FORMAT_MOD_TEGRA_TILED:
		tiling->mode = TEGRA_BO_TILING_MODE_TILED;
		tiling->value = 0;
		break;

	case NV_FORMAT_MOD_TEGRA_16BX2_BLOCK(0):
		tiling->mode = TEGRA_BO_TILING_MODE_BLOCK;
		tiling->value = fourcc_mod_tegra_param(modifier);
		if (tiling->value > 5)
			return -EINVAL;
		break;

	default:
		/* TODO: handle YUV formats? */
		*tiling = fb->planes[0]->tiling;
		break;
	}

	return 0;
}

static void tegra_fb_destroy(struct drm_framebuffer *framebuffer)
{
	struct tegra_fb *fb = to_tegra_fb(framebuffer);
	unsigned int i;

	for (i = 0; i < fb->num_planes; i++) {
		struct tegra_bo *bo = fb->planes[i];

		if (bo) {
			if (bo->pages)
				vunmap(bo->vaddr);

			drm_gem_object_put_unlocked(&bo->gem);
		}
	}

	drm_framebuffer_cleanup(framebuffer);
	kfree(fb->planes);
	kfree(fb);
}

static int tegra_fb_create_handle(struct drm_framebuffer *framebuffer,
				  struct drm_file *file, unsigned int *handle)
{
	struct tegra_fb *fb = to_tegra_fb(framebuffer);

	return drm_gem_handle_create(file, &fb->planes[0]->gem, handle);
}

static const struct drm_framebuffer_funcs tegra_fb_funcs = {
	.destroy = tegra_fb_destroy,
	.create_handle = tegra_fb_create_handle,
};

static struct tegra_fb *tegra_fb_alloc(struct drm_device *drm,
				       const struct drm_mode_fb_cmd2 *mode_cmd,
				       struct tegra_bo **planes,
				       unsigned int num_planes)
{
	struct tegra_fb *fb;
	unsigned int i;
	int err;

	fb = kzalloc(sizeof(*fb), GFP_KERNEL);
	if (!fb)
		return ERR_PTR(-ENOMEM);

	fb->planes = kcalloc(num_planes, sizeof(*planes), GFP_KERNEL);
	if (!fb->planes) {
		kfree(fb);
		return ERR_PTR(-ENOMEM);
	}

	fb->num_planes = num_planes;

	drm_helper_mode_fill_fb_struct(drm, &fb->base, mode_cmd);

	for (i = 0; i < fb->num_planes; i++)
		fb->planes[i] = planes[i];

	err = drm_framebuffer_init(drm, &fb->base, &tegra_fb_funcs);
	if (err < 0) {
		dev_err(drm->dev, "failed to initialize framebuffer: %d\n",
			err);
		kfree(fb->planes);
		kfree(fb);
		return ERR_PTR(err);
	}

	return fb;
}

struct drm_framebuffer *tegra_fb_create(struct drm_device *drm,
					struct drm_file *file,
					const struct drm_mode_fb_cmd2 *cmd)
{
	unsigned int hsub, vsub, i;
	struct tegra_bo *planes[4];
	struct drm_gem_object *gem;
	struct tegra_fb *fb;
	int err;

	hsub = drm_format_horz_chroma_subsampling(cmd->pixel_format);
	vsub = drm_format_vert_chroma_subsampling(cmd->pixel_format);

	for (i = 0; i < drm_format_num_planes(cmd->pixel_format); i++) {
		unsigned int width = cmd->width / (i ? hsub : 1);
		unsigned int height = cmd->height / (i ? vsub : 1);
		unsigned int size, bpp;

		gem = drm_gem_object_lookup(file, cmd->handles[i]);
		if (!gem) {
			err = -ENXIO;
			goto unreference;
		}

		bpp = drm_format_plane_cpp(cmd->pixel_format, i);

		size = (height - 1) * cmd->pitches[i] +
		       width * bpp + cmd->offsets[i];

		if (gem->size < size) {
			err = -EINVAL;
			drm_gem_object_put(gem);
			goto unreference;
		}

		planes[i] = to_tegra_bo(gem);
	}

	fb = tegra_fb_alloc(drm, cmd, planes, i);
	if (IS_ERR(fb)) {
		err = PTR_ERR(fb);
		goto unreference;
	}

	return &fb->base;

unreference:
	while (i--)
		drm_gem_object_put_unlocked(&planes[i]->gem);

	return ERR_PTR(err);
}

#ifdef CONFIG_DRM_FBDEV_EMULATION
static struct fb_ops tegra_fb_ops = {
	.owner = THIS_MODULE,
	DRM_FB_HELPER_DEFAULT_OPS,
	.fb_fillrect = drm_fb_helper_sys_fillrect,
	.fb_copyarea = drm_fb_helper_sys_copyarea,
	.fb_imageblit = drm_fb_helper_sys_imageblit,
};

static int tegra_fbdev_probe(struct drm_fb_helper *helper,
			     struct drm_fb_helper_surface_size *sizes)
{
	struct tegra_fbdev *fbdev = to_tegra_fbdev(helper);
	struct tegra_drm *tegra = helper->dev->dev_private;
	struct drm_device *drm = helper->dev;
	struct drm_mode_fb_cmd2 cmd = { 0 };
	unsigned int bytes_per_pixel;
	struct drm_framebuffer *fb;
	unsigned long offset;
	struct fb_info *info;
	struct tegra_bo *bo;
	size_t size;
	int err;

	bytes_per_pixel = DIV_ROUND_UP(sizes->surface_bpp, 8);

	cmd.width = sizes->surface_width;
	cmd.height = sizes->surface_height;
	cmd.pitches[0] = round_up(sizes->surface_width * bytes_per_pixel,
				  tegra->pitch_align);
	cmd.pixel_format = drm_mode_legacy_fb_format(sizes->surface_bpp,
						     sizes->surface_depth);

	size = cmd.pitches[0] * cmd.height;

	bo = tegra_bo_create(drm, size, 0);
	if (IS_ERR(bo))
		return PTR_ERR(bo);

	info = drm_fb_helper_alloc_fbi(helper);
	if (IS_ERR(info)) {
		dev_err(drm->dev, "failed to allocate framebuffer info\n");
		drm_gem_object_put_unlocked(&bo->gem);
		return PTR_ERR(info);
	}

	fbdev->fb = tegra_fb_alloc(drm, &cmd, &bo, 1);
	if (IS_ERR(fbdev->fb)) {
		err = PTR_ERR(fbdev->fb);
		dev_err(drm->dev, "failed to allocate DRM framebuffer: %d\n",
			err);
		drm_gem_object_put_unlocked(&bo->gem);
		return PTR_ERR(fbdev->fb);
	}

	fb = &fbdev->fb->base;
	helper->fb = fb;
	helper->fbdev = info;

	info->par = helper;
	info->flags = FBINFO_FLAG_DEFAULT;
	info->fbops = &tegra_fb_ops;

	drm_fb_helper_fill_fix(info, fb->pitches[0], fb->format->depth);
	drm_fb_helper_fill_var(info, helper, fb->width, fb->height);

	offset = info->var.xoffset * bytes_per_pixel +
		 info->var.yoffset * fb->pitches[0];

	if (bo->pages) {
		bo->vaddr = vmap(bo->pages, bo->num_pages, VM_MAP,
				 pgprot_writecombine(PAGE_KERNEL));
		if (!bo->vaddr) {
			dev_err(drm->dev, "failed to vmap() framebuffer\n");
			err = -ENOMEM;
			goto destroy;
		}
	}

	drm->mode_config.fb_base = (resource_size_t)bo->paddr;
	info->screen_base = (void __iomem *)bo->vaddr + offset;
	info->screen_size = size;
	info->fix.smem_start = (unsigned long)(bo->paddr + offset);
	info->fix.smem_len = size;

	return 0;

destroy:
	drm_framebuffer_remove(fb);
	return err;
}

static const struct drm_fb_helper_funcs tegra_fb_helper_funcs = {
	.fb_probe = tegra_fbdev_probe,
};

static struct tegra_fbdev *tegra_fbdev_create(struct drm_device *drm)
{
	struct tegra_fbdev *fbdev;

	fbdev = kzalloc(sizeof(*fbdev), GFP_KERNEL);
	if (!fbdev) {
		dev_err(drm->dev, "failed to allocate DRM fbdev\n");
		return ERR_PTR(-ENOMEM);
	}

	drm_fb_helper_prepare(drm, &fbdev->base, &tegra_fb_helper_funcs);

	return fbdev;
}

static void tegra_fbdev_free(struct tegra_fbdev *fbdev)
{
	kfree(fbdev);
}

static int tegra_fbdev_init(struct tegra_fbdev *fbdev,
			    unsigned int preferred_bpp,
			    unsigned int num_crtc,
			    unsigned int max_connectors)
{
	struct drm_device *drm = fbdev->base.dev;
	int err;

	err = drm_fb_helper_init(drm, &fbdev->base, max_connectors);
	if (err < 0) {
		dev_err(drm->dev, "failed to initialize DRM FB helper: %d\n",
			err);
		return err;
	}

	err = drm_fb_helper_single_add_all_connectors(&fbdev->base);
	if (err < 0) {
		dev_err(drm->dev, "failed to add connectors: %d\n", err);
		goto fini;
	}

	err = drm_fb_helper_initial_config(&fbdev->base, preferred_bpp);
	if (err < 0) {
		dev_err(drm->dev, "failed to set initial configuration: %d\n",
			err);
		goto fini;
	}

	return 0;

fini:
	drm_fb_helper_fini(&fbdev->base);
	return err;
}

static void tegra_fbdev_exit(struct tegra_fbdev *fbdev)
{
	drm_fb_helper_unregister_fbi(&fbdev->base);

	if (fbdev->fb)
		drm_framebuffer_remove(&fbdev->fb->base);

	drm_fb_helper_fini(&fbdev->base);
	tegra_fbdev_free(fbdev);
}

void tegra_fbdev_restore_mode(struct tegra_fbdev *fbdev)
{
	if (fbdev)
		drm_fb_helper_restore_fbdev_mode_unlocked(&fbdev->base);
}

void tegra_fb_output_poll_changed(struct drm_device *drm)
{
	struct tegra_drm *tegra = drm->dev_private;

	if (tegra->fbdev)
		drm_fb_helper_hotplug_event(&tegra->fbdev->base);
}
#endif

int tegra_drm_fb_prepare(struct drm_device *drm)
{
#ifdef CONFIG_DRM_FBDEV_EMULATION
	struct tegra_drm *tegra = drm->dev_private;

	tegra->fbdev = tegra_fbdev_create(drm);
	if (IS_ERR(tegra->fbdev))
		return PTR_ERR(tegra->fbdev);
#endif

	return 0;
}

void tegra_drm_fb_free(struct drm_device *drm)
{
#ifdef CONFIG_DRM_FBDEV_EMULATION
	struct tegra_drm *tegra = drm->dev_private;

	tegra_fbdev_free(tegra->fbdev);
#endif
}

int tegra_drm_fb_init(struct drm_device *drm)
{
#ifdef CONFIG_DRM_FBDEV_EMULATION
	struct tegra_drm *tegra = drm->dev_private;
	int err;

	err = tegra_fbdev_init(tegra->fbdev, 32, drm->mode_config.num_crtc,
			       drm->mode_config.num_connector);
	if (err < 0)
		return err;
#endif

	return 0;
}

void tegra_drm_fb_exit(struct drm_device *drm)
{
#ifdef CONFIG_DRM_FBDEV_EMULATION
	struct tegra_drm *tegra = drm->dev_private;

	tegra_fbdev_exit(tegra->fbdev);
#endif
}

void tegra_drm_fb_suspend(struct drm_device *drm)
{
#ifdef CONFIG_DRM_FBDEV_EMULATION
	struct tegra_drm *tegra = drm->dev_private;

	console_lock();
	drm_fb_helper_set_suspend(&tegra->fbdev->base, 1);
	console_unlock();
#endif
}

void tegra_drm_fb_resume(struct drm_device *drm)
{
#ifdef CONFIG_DRM_FBDEV_EMULATION
	struct tegra_drm *tegra = drm->dev_private;

	console_lock();
	drm_fb_helper_set_suspend(&tegra->fbdev->base, 0);
	console_unlock();
#endif
}
