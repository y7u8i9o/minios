#define KLOG_SUBSYS "fbdev"
#include <drivers/fbdev.h>
#include <console.h>
#include <arch/boot.h>
#include <fs/vfs.h>
#include <fs/devfs.h>
#include <mm/vma.h>
#include <mm/memlayout.h>
#include <sched/thread.h>
#include <sched/proc.h>
#include <lib/string.h>
#include <klog.h>
#include <errno.h>

/* The file that acquired the display, if any. Protected by fbdev_lock. */
static DEFINE_SPINLOCK(fbdev_lock);
static struct file *owner;

static long fb_ioctl(struct file *f, unsigned long req, uintptr_t arg)
{
    struct proc *p = thread_current()->proc;
    switch (req) {
    case FBIOGET_INFO: {
        if (!vma_range_ok(p->vm, arg, sizeof(struct fb_info), true))
            return -EFAULT;
        struct fb_info info = {
            .width = (uint32_t)bootinfo.framebuffer.width,
            .height = (uint32_t)bootinfo.framebuffer.height,
            .pitch = (uint32_t)bootinfo.framebuffer.pitch,
            .bpp = bootinfo.framebuffer.bpp,
            .red_size = bootinfo.framebuffer.red_mask_size,
            .red_shift = bootinfo.framebuffer.red_mask_shift,
            .green_size = bootinfo.framebuffer.green_mask_size,
            .green_shift = bootinfo.framebuffer.green_mask_shift,
            .blue_size = bootinfo.framebuffer.blue_mask_size,
            .blue_shift = bootinfo.framebuffer.blue_mask_shift,
        };
        memcpy((void *)arg, &info, sizeof info);
        return 0;
    }
    case FBIO_ACQUIRE:
        spin_lock(&fbdev_lock);
        if (owner && owner != f) {
            spin_unlock(&fbdev_lock);
            return -EBUSY;
        }
        owner = f;
        spin_unlock(&fbdev_lock);
        console_set_fb_enabled(false);
        klog_info("display acquired by pid %d", p->pid);
        return 0;
    case FBIO_RELEASE:
        spin_lock(&fbdev_lock);
        if (owner != f) {
            spin_unlock(&fbdev_lock);
            return -EPERM;
        }
        owner = NULL;
        spin_unlock(&fbdev_lock);
        console_set_fb_enabled(true);
        klog_info("display released by pid %d", p->pid);
        return 0;
    }
    return -ENOTTY;
}

static long fb_mmap(struct file *f, struct vmspace *vm, uintptr_t hint, size_t len, unsigned flags,
                    uint64_t off)
{
    size_t size = bootinfo.framebuffer.pitch * bootinfo.framebuffer.height;
    if (off || len > ALIGN_UP(size, PAGE_SIZE))
        return -EINVAL;
    uintptr_t pa = virt_to_phys(bootinfo.framebuffer.address);
    return vma_map_device(vm, hint, pa, len, flags | VM_WC);
}

static void fb_release(struct file *f)
{
    spin_lock(&fbdev_lock);
    bool was_owner = owner == f;
    if (was_owner)
        owner = NULL;
    spin_unlock(&fbdev_lock);
    if (was_owner) {
        console_set_fb_enabled(true);
        klog_info("display released at close");
    }
}

static const struct file_ops fb_fops = {
    .ioctl = fb_ioctl,
    .mmap = fb_mmap,
    .release = fb_release,
};

void fbdev_init(void)
{
    if (!bootinfo.have_framebuffer) {
        klog_warn("no framebuffer, /dev/fb0 not created");
        return;
    }
    size_t size = bootinfo.framebuffer.pitch * bootinfo.framebuffer.height;
    devfs_register("fb0", S_IFCHR | 0600, &fb_fops, NULL, size);
    const struct limine_framebuffer *fb = &bootinfo.framebuffer;
    klog_info("/dev/fb0: %lux%lu, %u bpp, r%u@%u g%u@%u b%u@%u, %zu bytes", fb->width, fb->height,
              fb->bpp, fb->red_mask_size, fb->red_mask_shift, fb->green_mask_size,
              fb->green_mask_shift, fb->blue_mask_size, fb->blue_mask_shift, size);
}
