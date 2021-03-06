/*
 *  linux/drivers/video/bcm2708_fb.c
 *
 * Copyright (C) 2010 Broadcom
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of this archive
 * for more details.
 *
 *  Broadcom simple framebuffer driver
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/mm.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/list.h>
#include <linux/platform_device.h>
#include <linux/clk.h>

#include <mach/platform.h>
#include <mach/vcio.h>

#include <asm/sizes.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>

/* This is limited to 16 characters when displayed by X startup */
static const char *bcm2708_name = "BCM2708 FB";

#define DRIVER_NAME "bcm2708_fb"

/* this data structure describes each frame buffer device we find */

struct fbinfo_s {
   int xres, yres, xres_virtual, yres_virtual;
   int pitch, bpp;
   int xoffset, yoffset;
   int base;
   int screen_size;
};

struct bcm2708_fb {
	struct fb_info		fb;
	struct platform_device	*dev;
	void __iomem		*regs;
        volatile struct fbinfo_s         *info;
        dma_addr_t              dma;
	u32			cmap[16];
};

#define to_bcm2708(info)	container_of(info, struct bcm2708_fb, fb)

static int
bcm2708_fb_set_bitfields(struct fb_var_screeninfo *var)
{
	int ret = 0;

	memset(&var->transp, 0, sizeof(var->transp));

	var->red.msb_right = 0;
	var->green.msb_right = 0;
	var->blue.msb_right = 0;

	switch (var->bits_per_pixel) {
	case 1:
	case 2:
	case 4:
	case 8:
		var->red.length		= var->bits_per_pixel;
		var->red.offset		= 0;
		var->green.length	= var->bits_per_pixel;
		var->green.offset	= 0;
		var->blue.length	= var->bits_per_pixel;
		var->blue.offset	= 0;
		break;
	case 16:
		var->red.length = 5;
		var->blue.length = 5;
		/*
		 * Green length can be 5 or 6 depending whether
		 * we're operating in RGB555 or RGB565 mode.
		 */
		if (var->green.length != 5 && var->green.length != 6)
			var->green.length = 6;
		break;
	case 32:
		var->red.length		= 8;
		var->green.length	= 8;
		var->blue.length	= 8;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	/*
	 * >= 16bpp displays have separate colour component bitfields
	 * encoded in the pixel data.  Calculate their position from
	 * the bitfield length defined above.
	 */
	if (ret == 0 && var->bits_per_pixel >= 16) {
		var->blue.offset = 0;
		var->green.offset = var->blue.offset + var->blue.length;
		var->red.offset = var->green.offset + var->green.length;
	}

	return ret;
}

static int bcm2708_fb_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{

         // info input, var output
         int yres;
         /* memory size in pixels */
         unsigned pixels = info->screen_size * 8 / var->bits_per_pixel;

         // info input, var output
	 printk(KERN_ERR "bcm2708_fb_check_var info(%p) %dx%d (%dx%d), %d, %d\n", info, info->var.xres, info->var.yres, info->var.xres_virtual, info->var.yres_virtual, (int)info->screen_size, info->var.bits_per_pixel );
	 printk(KERN_ERR "bcm2708_fb_check_var var(%p) %dx%d (%dx%d), %d, %d\n", var, var->xres, var->yres, var->xres_virtual, var->yres_virtual, var->bits_per_pixel, pixels);

         if (!var->bits_per_pixel) var->bits_per_pixel = 16;

         if (0 && var->bits_per_pixel != 16 && var->bits_per_pixel != 32) {
                 printk(KERN_ERR "bcm2708_fb_check_var: ERROR: bits_per_pixel=%d\n", var->bits_per_pixel);
                 return -EINVAL;
         }

         bcm2708_fb_set_bitfields(var);

         if (var->xres_virtual < var->xres)
                 var->xres_virtual = var->xres;
         /* use highest possible virtual resolution */
         if (var->yres_virtual == -1) {
                 var->yres_virtual = 480; //pixels / var->xres_virtual;

                 printk(KERN_ERR
                          "bcm2708_fb_check_var: virtual resolution set to maximum of %dx%d\n",
                          var->xres_virtual, var->yres_virtual);
         }
         if (var->yres_virtual < var->yres)
                 var->yres_virtual = var->yres;

         #if 0
         if (var->xres_virtual * var->yres_virtual > pixels) {
                 printk(KERN_ERR "bcm2708_fb_check_var: mode %dx%dx%d rejected... "
                       "virtual resolution too high to fit into video memory!\n",
                         var->xres_virtual, var->yres_virtual,
                         var->bits_per_pixel);
                 return -EINVAL;
         }
         #endif
         if (var->xoffset < 0)
                 var->xoffset = 0;
         if (var->yoffset < 0)
                 var->yoffset = 0;

         /* truncate xoffset and yoffset to maximum if too high */
         if (var->xoffset > var->xres_virtual - var->xres)
                 var->xoffset = var->xres_virtual - var->xres - 1;
         if (var->yoffset > var->yres_virtual - var->yres)
                 var->yoffset = var->yres_virtual - var->yres - 1;

         var->red.msb_right =
             var->green.msb_right =
             var->blue.msb_right =
             var->transp.offset =
             var->transp.length =
             var->transp.msb_right = 0;

         yres = var->yres;
         if (var->vmode & FB_VMODE_DOUBLE)
                 yres *= 2;
         else if (var->vmode & FB_VMODE_INTERLACED)
                 yres = (yres + 1) / 2;

         if (yres > 1200) {
                 printk(KERN_ERR "bcm2708_fb_check_var: ERROR: VerticalTotal >= 1200; "
                         "special treatment required! (TODO)\n");
                 return -EINVAL;
         }

         //if (cirrusfb_check_pixclock(var, info))
         //        return -EINVAL;

         //if (!is_laguna(cinfo))
         //        var->accel_flags = FB_ACCELF_TEXT;

         return 0;
}

static int bcm2708_fb_set_par(struct fb_info *info)
{
        unsigned val = 0;
	struct bcm2708_fb *fb = to_bcm2708(info);
        volatile struct fbinfo_s *fbinfo = fb->info;
        fbinfo->xres = info->var.xres;
        fbinfo->yres = info->var.yres;
        fbinfo->xres_virtual = info->var.xres_virtual;
        fbinfo->yres_virtual = info->var.yres_virtual;
        fbinfo->bpp = info->var.bits_per_pixel;
        fbinfo->xoffset = info->var.xoffset;
        fbinfo->yoffset = info->var.yoffset;
        fbinfo->base = 0; // filled in by VC
        fbinfo->pitch = 0; // filled in by VC

        printk(KERN_ERR "bcm2708_fb_set_par info(%p) %dx%d (%dx%d), %d, %d\n", info, info->var.xres, info->var.yres, info->var.xres_virtual, info->var.yres_virtual, (int)info->screen_size, info->var.bits_per_pixel );

	// inform vc about new framebuffer
	bcm_mailbox_write(MBOX_CHAN_FB, fb->dma);

	// wait for response
        bcm_mailbox_read(MBOX_CHAN_FB, &val);

	fb->fb.fix.line_length = fbinfo->pitch;

	if (info->var.bits_per_pixel <= 8)
		fb->fb.fix.visual = FB_VISUAL_PSEUDOCOLOR;
	else
		fb->fb.fix.visual = FB_VISUAL_TRUECOLOR;

        fb->fb.fix.smem_start = fbinfo->base;
        fb->fb.fix.smem_len = fbinfo->pitch * fbinfo->yres_virtual;
        fb->fb.screen_size = fbinfo->screen_size;
        fb->fb.screen_base = (void *)ioremap_nocache(fb->fb.fix.smem_start, fb->fb.screen_size);

	printk(KERN_ERR "BCM2708FB: start = %p,%p,%p width=%d, height=%d, bpp=%d, pitch=%d\n",
	       (void *)fb->fb.screen_base, (void *)fb->fb.fix.smem_start, (void *)val, fbinfo->xres, fbinfo->yres, fbinfo->bpp, fbinfo->pitch);

	return val;
}

static inline u32 convert_bitfield(int val, struct fb_bitfield *bf)
{
	unsigned int mask = (1 << bf->length) - 1;

	return (val >> (16 - bf->length) & mask) << bf->offset;
}

static int bcm2708_fb_setcolreg(unsigned int regno, unsigned int red, unsigned int green,
		 unsigned int blue, unsigned int transp, struct fb_info *info)
{
	struct bcm2708_fb *fb = to_bcm2708(info);

	if (regno < 16)
		fb->cmap[regno] = convert_bitfield(transp, &fb->fb.var.transp) |
				  convert_bitfield(blue, &fb->fb.var.blue) |
				  convert_bitfield(green, &fb->fb.var.green) |
				  convert_bitfield(red, &fb->fb.var.red);

	return regno > 255;
}

static int bcm2708_fb_blank(int blank_mode, struct fb_info *info)
{
//printk(KERN_ERR "bcm2708_fb_blank\n");
	return -1;
}

static void bcm2708_fb_fillrect(struct fb_info *info, const struct fb_fillrect *rect)
{
// (is called) printk(KERN_ERR "bcm2708_fb_fillrect\n");
	cfb_fillrect(info, rect);
}

static void bcm2708_fb_copyarea(struct fb_info *info, const struct fb_copyarea *region)
{
//printk(KERN_ERR "bcm2708_fb_copyarea\n");
	cfb_copyarea(info, region);
}

static void bcm2708_fb_imageblit(struct fb_info *info, const struct fb_image *image)
{
// (is called) printk(KERN_ERR "bcm2708_fb_imageblit\n");
	cfb_imageblit(info, image);
}

static struct fb_ops bcm2708_fb_ops = {
	.owner		= THIS_MODULE,
	.fb_check_var	= bcm2708_fb_check_var,
	.fb_set_par	= bcm2708_fb_set_par,
	.fb_setcolreg	= bcm2708_fb_setcolreg,
	.fb_blank	= bcm2708_fb_blank,
	.fb_fillrect	= bcm2708_fb_fillrect,
	.fb_copyarea	= bcm2708_fb_copyarea,
	.fb_imageblit	= bcm2708_fb_imageblit,
};

static int FBWIDTH =800; /* module parameter */
static int FBHEIGHT =480; /* module parameter */


static int bcm2708_fb_register(struct bcm2708_fb *fb)
{
	int ret;
	dma_addr_t dma;
	void *mem;

	mem = dma_alloc_coherent(NULL, PAGE_ALIGN(sizeof(*fb->info)), &dma, GFP_KERNEL);

	if (NULL == mem) {
		printk(KERN_ERR ": unable to allocate fbinfo buffer\n");
		ret = -ENOMEM;
	} else {
		fb->info = (struct fbinfo_s *)mem;
                fb->dma = dma;
        }
	fb->fb.fbops		= &bcm2708_fb_ops;
	fb->fb.flags		= FBINFO_FLAG_DEFAULT | FBINFO_HWACCEL_COPYAREA | FBINFO_HWACCEL_FILLRECT |  FBINFO_HWACCEL_IMAGEBLIT;
	fb->fb.pseudo_palette	= fb->cmap;

	strncpy(fb->fb.fix.id, bcm2708_name, sizeof(fb->fb.fix.id));
	fb->fb.fix.type		= FB_TYPE_PACKED_PIXELS;
	fb->fb.fix.type_aux	= 0;
	fb->fb.fix.xpanstep	= 0;
	fb->fb.fix.ypanstep	= 0;
	fb->fb.fix.ywrapstep	= 0;
	fb->fb.fix.accel	= FB_ACCEL_NONE;

	fb->fb.var.xres		= FBWIDTH;
	fb->fb.var.yres		= FBHEIGHT;
	fb->fb.var.xres_virtual	= FBWIDTH;
	fb->fb.var.yres_virtual	= FBHEIGHT;
	fb->fb.var.bits_per_pixel = 16;
	fb->fb.var.vmode	= FB_VMODE_NONINTERLACED;
	fb->fb.var.activate	= FB_ACTIVATE_NOW;
	fb->fb.var.nonstd	= 0;
	fb->fb.var.height	= FBWIDTH;
	fb->fb.var.width	= FBHEIGHT;
	fb->fb.var.accel_flags	= 0;

	fb->fb.monspecs.hfmin	= 0;
	fb->fb.monspecs.hfmax   = 100000;
	fb->fb.monspecs.vfmin	= 0;
	fb->fb.monspecs.vfmax	= 400;
	fb->fb.monspecs.dclkmin = 1000000;
	fb->fb.monspecs.dclkmax	= 100000000;

	bcm2708_fb_set_bitfields(&fb->fb.var);

	/*
	 * Allocate colourmap.
	 */

	fb_set_var(&fb->fb, &fb->fb.var);

	printk(KERN_INFO "BCM2708FB: registering framebuffer (%d, %d)\n", FBWIDTH, FBHEIGHT);

	ret = register_framebuffer(&fb->fb);
	printk(KERN_ERR "BCM2708FB: register framebuffer (%d)\n", ret);
	if (ret == 0)
		goto out;

	printk(KERN_ERR "BCM2708FB: cannot register framebuffer (%d)\n", ret);

	iounmap(fb->regs);
 out:
	return ret;
}

static int bcm2708_fb_probe(struct platform_device *dev)
{
	struct bcm2708_fb *fb;
	int ret;

	fb = kmalloc(sizeof(struct bcm2708_fb), GFP_KERNEL);
	if (!fb) {
		dev_err(&dev->dev, "could not allocate new bcm2708_fb struct\n");
		ret = -ENOMEM;
		goto free_region;
	}
	memset(fb, 0, sizeof(struct bcm2708_fb));

	fb->dev = dev;

	ret = bcm2708_fb_register(fb);
	if (ret == 0) {
		platform_set_drvdata(dev, fb);
		goto out;
	}

	kfree(fb);
 free_region:
	dev_err(&dev->dev, "probe failed, err %d\n", ret);
 out:
	return ret;
}

static int bcm2708_fb_remove(struct platform_device *dev)
{
	struct bcm2708_fb *fb = platform_get_drvdata(dev);

	platform_set_drvdata(dev, NULL);

	unregister_framebuffer(&fb->fb);
	iounmap(fb->regs);

        dma_free_coherent(NULL, PAGE_ALIGN(sizeof(*fb->info)), (void *)fb->info, fb->dma);
	kfree(fb);

	return 0;
}

static struct platform_driver bcm2708_fb_driver = {
	.probe		= bcm2708_fb_probe,
	.remove		= bcm2708_fb_remove,
	.driver		= {
		.name	= DRIVER_NAME,
		.owner  = THIS_MODULE,
	},
};

static int __init bcm2708_fb_init(void)
{
	return platform_driver_register(&bcm2708_fb_driver);
}

module_init(bcm2708_fb_init);

static void __exit bcm2708_fb_exit(void)
{
	platform_driver_unregister(&bcm2708_fb_driver);
}

module_exit(bcm2708_fb_exit);

module_param(FBWIDTH, int, 0644);
module_param(FBHEIGHT, int, 0644);

MODULE_DESCRIPTION("BCM2708 framebuffer driver");
MODULE_LICENSE("GPL");

MODULE_PARM_DESC(FBWIDTH, "Width of ARM Framebuffer");
MODULE_PARM_DESC(FBHEIGHT, "Height of ARM Framebuffer");
