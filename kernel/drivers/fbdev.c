#define KLOG_SUBSYS "fbdev"
#include <drivers/fbdev.h>
#include <console.h>
#include <boot.h>
#include <fs/vfs.h>
#include <fs/devfs.h>
#include <mm/vma.h>
#include <mm/memlayout.h>
#include <mm/slab.h>
#include <sched/thread.h>
#include <sched/proc.h>
#include <sync/mutex.h>
#include <lib/string.h>
#include <klog.h>
#include <errno.h>
#include <drivers/devinfo.h>
#include <ipc/poll.h>

struct limine_framebuffer fb_screen;
bool fb_screen_present;
uint32_t fb_screen_scale = 1;

/* The file that acquired the display, if any. Protected by fbdev_lock. */
static DEFINE_SPINLOCK(fbdev_lock);
static struct file *owner;
/* The last size request of the host display and the serial of the request
 * that the owner read. fbdev_lock protects both. */
static struct fb_display display_request;
static uint32_t owner_seen;
/* Poll waiters of /dev/fb0 for size requests. */
static struct poll_source fb_poll;

/* The GPU driver, registered once at boot and read only afterwards. */
static struct {
    const struct fb_gpu_ops *ops;
    void *priv;
    size_t size;
} gpu;

/* Serializes mode changes against readers of the geometry in ioctls. */
static struct mutex fb_mode_lock;

/* The cursor of the display owner (G9). fb_cursor_lock protects it and
 * serializes the cursor calls of the GPU driver. */
static struct mutex fb_cursor_lock;
static struct fb_cursor_state cursor;

void fb_screen_init(void)
{
    mutex_init(&fb_mode_lock, "fb_mode");
    mutex_init(&fb_cursor_lock, "fb_cursor");
    poll_source_init(&fb_poll, "fb0");
    if (!bootinfo.have_framebuffer)
        return;
    fb_screen = bootinfo.framebuffer;
    fb_screen_present = true;
    fb_screen_scale = bootinfo.fb_scale ? bootinfo.fb_scale : 1;
}

bool fb_has_gpu(void)
{
    return gpu.ops != NULL;
}

size_t fb_map_size(void)
{
    return gpu.ops ? gpu.size : (size_t)fb_screen.pitch * fb_screen.height;
}

int fb_flush_rects(const struct fb_rect *r, int n)
{
    if (!gpu.ops || n <= 0)
        return 0;
    return gpu.ops->flush(gpu.priv, r, n);
}

int fb_flush(struct fb_rect r)
{
    return fb_flush_rects(&r, 1);
}

void fb_panic_flush(void)
{
    if (!gpu.ops || !fb_screen_present)
        return;
    struct fb_rect r = { 0, 0, (int32_t)fb_screen.width, (int32_t)fb_screen.height };
    gpu.ops->flush_poll(gpu.priv, r);
}

static bool mode_ok(uint32_t w, uint32_t h, uint32_t scale)
{
    return w >= 320 && h >= 200 && w <= 8192 && h <= 8192 && scale >= 1 && scale <= 4 &&
           (uint64_t)w * h * 4 <= gpu.size;
}

void fb_gpu_register(const struct fb_gpu_ops *ops, void *priv,
                     const struct limine_framebuffer *screen, size_t size)
{
    mutex_lock(&fb_mode_lock);
    gpu.ops = ops;
    gpu.priv = priv;
    gpu.size = size;
    console_set_screen(screen, fb_screen_scale);
    ops->commit_mode(priv);
    struct fb_rect all = { 0, 0, (int32_t)screen->width, (int32_t)screen->height };
    ops->flush(priv, &all, 1);
    mutex_unlock(&fb_mode_lock);
}

int fb_set_mode(uint32_t width, uint32_t height, uint32_t scale)
{
    if (!fb_screen_present)
        return -ENODEV;
    mutex_lock(&fb_mode_lock);
    int r = 0;
    if (!gpu.ops) {
        if (width != fb_screen.width || height != fb_screen.height || scale < 1 || scale > 4)
            r = -EOPNOTSUPP;
        else
            console_set_screen(&fb_screen, scale);
        mutex_unlock(&fb_mode_lock);
        return r;
    }
    if (!mode_ok(width, height, scale)) {
        mutex_unlock(&fb_mode_lock);
        return -EINVAL;
    }
    if (width != fb_screen.width || height != fb_screen.height) {
        r = gpu.ops->prepare_mode(gpu.priv, width, height);
        if (r < 0) {
            mutex_unlock(&fb_mode_lock);
            return r;
        }
        struct limine_framebuffer next = fb_screen;
        next.width = width;
        next.height = height;
        next.pitch = (uint64_t)width * 4;
        console_set_screen(&next, scale);
        r = gpu.ops->commit_mode(gpu.priv);
    } else {
        console_set_screen(&fb_screen, scale);
    }
    struct fb_rect all = { 0, 0, (int32_t)width, (int32_t)height };
    gpu.ops->flush(gpu.priv, &all, 1);
    klog_info("mode %ux%u scale %u", width, height, scale);
    mutex_unlock(&fb_mode_lock);
    return r;
}

void fb_display_changed(uint32_t width, uint32_t height)
{
    spin_lock(&fbdev_lock);
    display_request.width = width;
    display_request.height = height;
    display_request.serial++;
    spin_unlock(&fbdev_lock);
    klog_info("the host display requests %ux%u", width, height);
    poll_source_notify(&fb_poll);
}

void fb_display_get(struct fb_display *out)
{
    spin_lock(&fbdev_lock);
    *out = display_request;
    spin_unlock(&fbdev_lock);
}

static bool fb_has_cursor(void)
{
    return gpu.ops && gpu.ops->cursor_set && gpu.ops->cursor_move;
}

void fb_cursor_get(struct fb_cursor_state *out)
{
    mutex_lock(&fb_cursor_lock);
    *out = cursor;
    mutex_unlock(&fb_cursor_lock);
}

static bool fb_is_owner(struct file *f)
{
    spin_lock(&fbdev_lock);
    bool is_owner = owner == f;
    spin_unlock(&fbdev_lock);
    return is_owner;
}

/* Show the image of c or hide the cursor (width 0). */
static int cursor_set(const struct fb_cursor *c)
{
    if (c->width > FB_CURSOR_MAX || c->height > FB_CURSOR_MAX)
        return -EINVAL;
    if (c->width && (c->height == 0 || c->hot_x >= c->width || c->hot_y >= c->height))
        return -EINVAL;
    mutex_lock(&fb_cursor_lock);
    int r;
    if (!c->width) {
        r = gpu.ops->cursor_set(gpu.priv, NULL, 0, 0, 0, 0);
        if (r == 0)
            cursor.visible = false;
    } else {
        /* The device receives the whole square with transparent pixels
         * outside the image. */
        for (uint32_t y = 0; y < FB_CURSOR_MAX; y++)
            for (uint32_t x = 0; x < FB_CURSOR_MAX; x++)
                cursor.image[y * FB_CURSOR_MAX + x] =
                    x < c->width && y < c->height ? c->pixels[y * FB_CURSOR_MAX + x] : 0;
        r = gpu.ops->cursor_set(gpu.priv, cursor.image, c->hot_x, c->hot_y, c->x, c->y);
        cursor.visible = r == 0;
        cursor.width = c->width;
        cursor.height = c->height;
        cursor.hot_x = c->hot_x;
        cursor.hot_y = c->hot_y;
        cursor.x = c->x;
        cursor.y = c->y;
    }
    cursor.sets++;
    mutex_unlock(&fb_cursor_lock);
    return r;
}

static int cursor_move(struct fb_cursor_pos pos)
{
    mutex_lock(&fb_cursor_lock);
    int r = 0;
    if (cursor.visible) {
        r = gpu.ops->cursor_move(gpu.priv, pos.x, pos.y);
        cursor.x = pos.x;
        cursor.y = pos.y;
        cursor.moves++;
    }
    mutex_unlock(&fb_cursor_lock);
    return r;
}

/* The display returns to the console: the cursor of the owner disappears. */
static void cursor_hide(void)
{
    if (!fb_has_cursor())
        return;
    mutex_lock(&fb_cursor_lock);
    if (cursor.visible && gpu.ops->cursor_set(gpu.priv, NULL, 0, 0, 0, 0) == 0)
        cursor.visible = false;
    mutex_unlock(&fb_cursor_lock);
}

static long fb_ioctl(struct file *f, unsigned long req, uintptr_t arg)
{
    struct proc *p = thread_current()->proc;
    switch (req) {
    case FBIOGET_INFO: {
        if (!vma_range_ok(p->vm, arg, sizeof(struct fb_info), true))
            return -EFAULT;
        mutex_lock(&fb_mode_lock);
        struct fb_info info = {
            .width = (uint32_t)fb_screen.width,
            .height = (uint32_t)fb_screen.height,
            .pitch = (uint32_t)fb_screen.pitch,
            .bpp = fb_screen.bpp,
            .red_size = fb_screen.red_mask_size,
            .red_shift = fb_screen.red_mask_shift,
            .green_size = fb_screen.green_mask_size,
            .green_shift = fb_screen.green_mask_shift,
            .blue_size = fb_screen.blue_mask_size,
            .blue_shift = fb_screen.blue_mask_shift,
            .scale = (uint8_t)fb_screen_scale,
            .caps = (gpu.ops ? FB_CAP_FLUSH | FB_CAP_SET_MODE | FB_CAP_FLUSH_RECTS : 0) |
                    (fb_has_cursor() ? FB_CAP_CURSOR : 0),
            .size = (uint32_t)fb_map_size(),
        };
        mutex_unlock(&fb_mode_lock);
        memcpy((void *)arg, &info, sizeof info);
        return 0;
    }
    case FBIO_FLUSH: {
        if (!vma_range_ok(p->vm, arg, sizeof(struct fb_rect), false))
            return -EFAULT;
        struct fb_rect r;
        memcpy(&r, (const void *)arg, sizeof r);
        return fb_flush(r);
    }
    case FBIO_FLUSH_RECTS: {
        if (!vma_range_ok(p->vm, arg, sizeof(struct fb_flush_rects), false))
            return -EFAULT;
        struct fb_flush_rects fr;
        memcpy(&fr, (const void *)arg, sizeof fr);
        if (fr.count > FB_FLUSH_MAX || fr.flags)
            return -EINVAL;
        /* Another file than the owner could send a half composed frame of
         * the owner to the display. */
        spin_lock(&fbdev_lock);
        bool allowed = !owner || owner == f;
        spin_unlock(&fbdev_lock);
        if (!allowed)
            return -EPERM;
        return fb_flush_rects(fr.rects, (int)fr.count);
    }
    case FBIO_SET_MODE: {
        if (!vma_range_ok(p->vm, arg, sizeof(struct fb_mode), false))
            return -EFAULT;
        struct fb_mode m;
        memcpy(&m, (const void *)arg, sizeof m);
        spin_lock(&fbdev_lock);
        bool is_owner = owner == f;
        spin_unlock(&fbdev_lock);
        if (!is_owner)
            return -EPERM;
        return fb_set_mode(m.width, m.height, m.scale);
    }
    case FBIO_CURSOR_SET: {
        if (!fb_has_cursor())
            return -ENOTTY;
        if (!vma_range_ok(p->vm, arg, sizeof(struct fb_cursor), false))
            return -EFAULT;
        if (!fb_is_owner(f))
            return -EPERM;
        struct fb_cursor *c = kmalloc(sizeof *c);
        if (!c)
            return -ENOMEM;
        memcpy(c, (const void *)arg, sizeof *c);
        int r = cursor_set(c);
        kfree(c);
        return r;
    }
    case FBIO_CURSOR_MOVE: {
        if (!fb_has_cursor())
            return -ENOTTY;
        if (!vma_range_ok(p->vm, arg, sizeof(struct fb_cursor_pos), false))
            return -EFAULT;
        if (!fb_is_owner(f))
            return -EPERM;
        struct fb_cursor_pos pos;
        memcpy(&pos, (const void *)arg, sizeof pos);
        return cursor_move(pos);
    }
    case FBIOGET_DISPLAY: {
        if (!vma_range_ok(p->vm, arg, sizeof(struct fb_display), true))
            return -EFAULT;
        spin_lock(&fbdev_lock);
        struct fb_display d = display_request;
        if (owner == f)
            owner_seen = d.serial;
        spin_unlock(&fbdev_lock);
        memcpy((void *)arg, &d, sizeof d);
        return 0;
    }
    case FBIO_ACQUIRE:
        spin_lock(&fbdev_lock);
        if (owner && owner != f) {
            spin_unlock(&fbdev_lock);
            return -EBUSY;
        }
        /* A new owner learns a request that arrived before it. */
        if (owner != f)
            owner_seen = 0;
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
        cursor_hide();
        console_set_fb_enabled(true);
        klog_info("display released by pid %d", p->pid);
        return 0;
    }
    return -ENOTTY;
}

static long fb_mmap(struct file *f, struct vmspace *vm, uintptr_t hint, size_t len, unsigned flags,
                    uint64_t off)
{
    size_t size = fb_map_size();
    if (off || len > ALIGN_UP(size, PAGE_SIZE))
        return -EINVAL;
    uintptr_t pa = virt_to_phys(fb_screen.address);
    /* Video memory is mapped write combining; the GPU driver's buffer is
     * ordinary RAM read by the host through DMA. */
    return vma_map_device(vm, hint, pa, len, flags | (gpu.ops ? 0 : VM_WC));
}

static void fb_release(struct file *f)
{
    spin_lock(&fbdev_lock);
    bool was_owner = owner == f;
    if (was_owner)
        owner = NULL;
    spin_unlock(&fbdev_lock);
    if (was_owner) {
        cursor_hide();
        console_set_fb_enabled(true);
        klog_info("display released at close");
    }
}

/* POLLIN for the display owner while a size request is unread. Other
 * files of the device never become ready. */
static int fb_poll_ready(struct file *f)
{
    spin_lock(&fbdev_lock);
    int ready = owner == f && display_request.serial != owner_seen ? POLLIN : 0;
    spin_unlock(&fbdev_lock);
    return ready;
}

static struct poll_source *fb_poll_source(struct file *f)
{
    return &fb_poll;
}

static const struct file_ops fb_fops = {
    .ioctl = fb_ioctl,
    .mmap = fb_mmap,
    .release = fb_release,
    .poll = fb_poll_ready,
    .poll_source = fb_poll_source,
};

void fbdev_init(void)
{
    if (!fb_screen_present) {
        klog_warn("no framebuffer, /dev/fb0 not created");
        return;
    }
    devfs_register("fb0", S_IFCHR | 0600, &fb_fops, NULL, 0);
    const struct limine_framebuffer *fb = &fb_screen;
    klog_info("/dev/fb0: %lux%lu, %u bpp, r%u@%u g%u@%u b%u@%u, %zu bytes, scale %u", fb->width,
              fb->height, fb->bpp, fb->red_mask_size, fb->red_mask_shift, fb->green_mask_size,
              fb->green_mask_shift, fb->blue_mask_size, fb->blue_mask_shift,
              (size_t)fb->pitch * fb->height, fb_screen_scale);
}

/* ---- /dev/devices (docs/design/sysinfo.md) ---- */

/* The geometry is read under fb_mode_lock, which mode changes take. The
 * GPU driver describes itself under its own lock. */
void fbdev_describe(struct devinfo *d)
{
    devinfo_node(d, "display", "Display");
    if (!fb_screen_present) {
        devinfo_prop(d, "framebuffer", "none");
        return;
    }
    mutex_lock(&fb_mode_lock);
    struct limine_framebuffer s = fb_screen;
    uint32_t scale = fb_screen_scale;
    size_t map = fb_map_size();
    mutex_unlock(&fb_mode_lock);
    spin_lock(&fbdev_lock);
    bool acquired = owner != NULL;
    spin_unlock(&fbdev_lock);
    devinfo_prop(d, "resolution", "%lu x %lu", (unsigned long)s.width, (unsigned long)s.height);
    devinfo_prop(d, "logical_resolution", "%lu x %lu, scale %u", (unsigned long)(s.width / scale),
                 (unsigned long)(s.height / scale), scale);
    devinfo_prop(d, "bits_per_pixel", "%u", s.bpp);
    devinfo_prop(d, "pitch", "%lu bytes", (unsigned long)s.pitch);
    devinfo_prop(d, "pixel_format", "red %u bits at %u, green %u bits at %u, blue %u bits at %u", s.red_mask_size,
                 s.red_mask_shift, s.green_mask_size, s.green_mask_shift, s.blue_mask_size, s.blue_mask_shift);
    devinfo_size(d, "framebuffer_size", map);
    devinfo_prop(d, "driver", "%s", gpu.ops ? "virtio-gpu" : "boot framebuffer");
    devinfo_prop(d, "device_node", "/dev/fb0");
    devinfo_prop(d, "acquired", "%s", acquired ? "yes, by the display server" : "no");
    struct fb_display request;
    fb_display_get(&request);
    if (request.serial)
        devinfo_prop(d, "host_request", "%u x %u, %u requests", request.width, request.height, request.serial);
    if (bootinfo.have_framebuffer)
        devinfo_prop(d, "boot_framebuffer", "%lu x %lu, %u bits per pixel, at 0x%lx",
                     (unsigned long)bootinfo.framebuffer.width, (unsigned long)bootinfo.framebuffer.height,
                     bootinfo.framebuffer.bpp,
                     (unsigned long)((uintptr_t)bootinfo.framebuffer.address - bootinfo.hhdm_offset));
    if (gpu.ops && gpu.ops->describe)
        gpu.ops->describe(gpu.priv, d);
}

