# Animations in X12, then a Venus GPU path

## Context

The owner wants animations in the compositor X12 (checklist item G4 in
`docs/plan/visual-polish.md`: "X12 has none"). The owner assumed
animations need GPU acceleration and asked for a study of Venus, a Vulkan
subset in the guest, and the Vulkan implementations on macOS.

Findings:

- **CPU budget.** `docs/design/graphics-performance.md` records a window
  drag at a median of 5.4 ms and the `anim` benchmark at 2.9 ms per frame at
  2560x1600@2. The frame interval is 16 ms. Fades and slides of single
  windows fit on the CPU. Bilinear scaling of a full window costs more.
- **X12 today.** `user/compositor/scene.c` composes damage rectangles in
  software. `draw_surface` (scene.c:276) has no opacity and no transform.
  `struct csurface` (comp.h:142) has no visual parameters. Minimize is
  instant (`toplevel_set_minimized`, wm/window.c:173). A closed window
  releases its buffer at once. `scene_render_window` (scene.c:607) renders
  a toplevel with shadow and corners into an ARGB image. `pixel_over` and
  related functions in `lib/libgui/src/pixel.c` use GCC vector extensions
  (NEON on aarch64).
- **Guest GPU path today.** `kernel/drivers/virtio/virtio_gpu.c` is 2D
  only. The driver negotiates no GPU feature bit, reads no capset, has no
  contexts, no blob resources and no fences. `virtio.c` ignores the shared
  memory capability (type 8). `/dev/fb0` is the only device node.
  `vma_map_device` (mm/mmap.c:406) maps physical ranges into a process with
  write-back, Normal-NC or Device attributes.
- **Host QEMU.** The Homebrew QEMU 11.0.3 on the Mac of the owner has no virglrenderer:
  no `virtio-gpu-gl` device, displays only cocoa, curses, dbus and none.
  Venus for macOS hosts exists only in unmerged series: Akihiko Odaki and
  the UTM developers ("virtio-gpu-virgl: introduce Venus support for
  macOS", RFC v2, December 2025, plus virglrenderer changes) and Lucas
  Amaral ("virtio-gpu: enable Venus/Vulkan without OpenGL display", v5,
  2026, with `VIRTIO_GPU_F_BLOB_ALIGNMENT` for the 16 KiB `hv_vm_map`
  granule). In the no-GL mode, Vulkan scanout does not exist. The guest
  copies rendered frames into a 2D resource.
- **Host Vulkan.** MoltenVK 1.4.2 is installed (Homebrew). KosmicKrisp
  (LunarG, Mesa, on Metal 4, macOS 26 and later) is a conformant Vulkan 1.4
  driver since September 2026. UTM issue #8011 reports device-lost errors
  with Venus on KosmicKrisp for optimal-tiled images. MoltenVK works for
  optimal-tiled images. glslc, glslangValidator, spirv-as and spirv-val are installed
  under /usr/local/bin, and meson and ninja under /opt/homebrew/bin.
- **Venus guest requirements.** Context init, blob resources, host-visible
  memory (the shared memory region of the device) and capset 4 (Venus).
  Guest commands use the encoding of `VK_EXT_command_serialization` and
  `VK_MESA_venus_protocol`. The guest assigns the object ids.
  virglrenderer also offers `virgl_test_server --venus`, a Unix socket
  transport (vtest). vtest allows host checks without QEMU.

The owner chose: CPU animations first, the GPU path afterwards as an
optional X12 backend, and a build script in `tools/` for the host QEMU.

## Plan document

A new plan `docs/plan/animation-gpu.md` records the milestones below, with
a row in `docs/plan/README.md`. Each milestone ends with its tests,
`make test-changed` (run only on the owner's request during the current session) and
one commit without attribution lines. Sizes are estimates of new lines.

## Part 1: CPU animations

### AN1 Animation clock (about 400 lines, completed 2026-10-30)

- New `user/compositor/anim.c` with `struct anim` (start in µs, duration,
  easing, progress 0..65536) and fixed point easing curves (ease-out cubic
  and a cubic Bézier evaluator). Fixed point matches the pixel code.
- `schedule_frame` (main.c:173) treats a running animation as pending work.
  `frame()` advances all animations with the frame time and damages the
  union of the old and new extents.
- New setting `anim_percent` in `debug.c` (0 disables all animations, 100
  is normal, up to 400 for inspection), shown by x12settings and compstat.
- Boot tests set `anim_percent 0` through the existing settings interface
  in `kernel/tests/gui_helpers.h`, so pixel checks see final states.

### AN2 Visual parameters in the blend path (about 700 lines, completed 2026-10-30)

- `struct csurface` gains `struct visual { uint8_t alpha; int32_t scale
  (16.16); int dx, dy; }` with an identity default.
- libgui `pixel.c` and `gui/pixel.h`: `pixel_over_alpha(to, from, n, a)`
  and `pixel_copy_alpha` in the vector form of `pixel_over`, plus
  `pixel_sample_bilinear` for fractional scales. Host tests in
  `lib/libgui/tests` compare the vector forms with scalar references.
- `draw_surface` uses the visual parameters. A surface with alpha below
  255 or a scale other than 1 occludes nothing: `subtract_opaque`,
  `compose_rect` coverage and `scene_damage_surface` respect the flag.
- `wm/decor.c` shadow (`pixel_shade`) and corner coverage take the alpha
  and the scale of the window. As implemented: a transformed surface is drawn from
  an image of its extent. The image contains the decorations and the
  shadow. `decor.c` therefore remains unchanged
  (`docs/design/animations.md`).
- `compbench` gains a fade scenario and a scale scenario.
  `graphics-performance.md` records the results at 2560x1600@2. As
  implemented: compbench starts the animations with the request
  `animate` of the debug interface of version 4. The recorded results
  come from x86_64 under TCG. The aarch64 results with hvf remain open.

### AN3 Window animations (about 500 lines)

- Map: fade in and scale 0.96 to 1 over 150 ms.
- Close: X12 renders the last frame with `scene_render_window` into a
  snapshot before the buffer goes away. The snapshot fades out and scales
  to 0.96 over 120 ms, then X12 frees the snapshot.
- Minimize and restore: scale and move toward the task button of the
  panel over 200 ms. The panel reports the button rectangle per toplevel
  through the shell protocol (new request `set_task_rect`, version bump in
  `docs/design/protocol.md`). Without a rectangle, the target is the bottom
  centre of the screen.
- Input goes to the final state at once. A click during an animation acts
  on the final geometry.

### AN4 Popup fades (about 200 lines)

- Popups (`ROLE_POPUP`: menus, combo box lists, tooltips) fade in over
  90 ms and fade out from a snapshot over 90 ms.
- Panel menus and notifications are layer surfaces. Layer surfaces with
  the popup or notification layer fade the same way.

Tests: a boot case `x12_anim` sets `anim_percent 400`, opens a window,
samples a pixel at mid animation (alpha between the end values), then
checks the final state. Host tests cover the pixel functions.

## Part 2: Venus GPU path

### GV1 Host QEMU with Venus (about 300 lines of script, plus patches)

- `tools/build-qemu-venus.sh` clones QEMU and virglrenderer at pinned
  commits into `build/host/src`, applies the series stored under
  `tools/qemu-venus/patches/` (the macOS Venus RFC and the no-GL Venus
  series, with their patchew message ids recorded in a README), and builds
  with meson and ninja against MoltenVK. Output:
  `build/host/qemu-venus/bin/qemu-system-{aarch64,x86_64}` and
  `virgl_test_server`.
- If the series no longer applies to current upstream, the script uses the
  UTM forks of QEMU and virglrenderer at pinned commits instead. The
  README records the choice.
- `tools/run.sh --gpu venus` selects the GV1 QEMU and adds
  `-device virtio-gpu-gl-pci,blob=on,venus=on,hostmem=256M` (aarch64) or
  `virtio-vga-gl` with the same options (x86_64). KosmicKrisp is selectable
  through `VK_ICD_FILENAMES` for comparison.
- `docs/design/build.md` describes the script.

### GV2 Kernel virtio-gpu 3D and `/dev/gpu0` (about 1,500 lines)

- `virtio.c`: parse capability type 8 (`virtio_pci_cap64`) and add a
  shared `virtio_shm_region(vdev, id, &pa, &len)`.
- `virtio_gpu.c`: negotiate `VIRGL`, `RESOURCE_BLOB`, `CONTEXT_INIT` and
  `BLOB_ALIGNMENT` when offered. Add `GET_CAPSET_INFO`, `GET_CAPSET`,
  `CTX_CREATE` (with the capset id in `context_init`), `CTX_DESTROY`,
  `CTX_ATTACH_RESOURCE`, `SUBMIT_3D`, `RESOURCE_CREATE_BLOB`,
  `RESOURCE_MAP_BLOB` and `RESOURCE_UNMAP_BLOB`. Fenced requests complete
  asynchronously per context and ring index. The 2D path and `/dev/fb0`
  remain unchanged.
- New `kernel/drivers/gpu.c`: `/dev/gpu0` with one context per open file.
  Ioctls: `GPU_GETPARAM`, `GPU_GET_CAPSET`, `GPU_CONTEXT_INIT`,
  `GPU_BLOB_CREATE`, `GPU_SUBMIT`, `GPU_WAIT`, `GPU_RESOURCE_UNREF`. `poll`
  reports completed fences. `mmap` of a blob maps the region of the shared
  memory BAR with `vma_map_device` and write-back attributes. Each blob
  offset respects the blob alignment (16 KiB under HVF).
- ABI in `kernel/include/minios/abi.h`, lock comments, and an entry in
  `docs/design/locking.md` before the new lock.
- Boot case `gpu_venus` with a new case file `qemu` (value `venus`). The
  runner uses the GV1 QEMU for such a case and reports SKIP when the
  binary is absent. The case reads capset 4, creates a context and a blob,
  maps the blob, writes and reads a pattern, and waits on a fenced empty
  submit.
- `docs/design/gpu.md` describes the driver.

### GV3 Venus encoder and the Vulkan subset `libvk` (about 4,000 lines)

- `third_party/vulkan-headers` (Khronos `vk.xml` and `vulkan_core.h`) and
  `third_party/venus-protocol` (XML only, MIT).
- `tools/venus-gen.py` reads both registries and generates encoders and
  reply decoders for the commands listed in `lib/libvk/commands.txt`
  (about 55 commands: instance, physical device queries, device, queue,
  memory, buffer, image, image view, sampler, shader module, descriptor
  set layout, pool and update, pipeline layout, graphics pipeline, command
  pool and buffer, `vkCmdBeginRendering`, bind, push constants, draw,
  viewport, scissor, barrier, copy buffer to image and back, queue submit,
  fence, destroy). The generated files are checked in, so the normal
  build needs no Python step.
- `lib/libvk`: the standard Vulkan entry points for the subset, guest
  object ids, a reply stream in a shared memory blob
  (`vkSetReplyCommandStreamMESA`), host-visible memory as blobs mapped
  through `/dev/gpu0`, and two transports: `/dev/gpu0` in the guest and
  vtest on the host.
- Host check `make check-libvk`: libvk on macOS against the GV1
  `virgl_test_server --venus`. The check clears an image, copies the image
  into a buffer and compares the pixels.
- Boot case `gpu_vulkan`: the same test in the guest.

### GV4 Compositor shaders (about 600 lines)

- GLSL sources under `user/compositor/shaders/`: textured quad with
  opacity, transform and rounded corner coverage, and the window shadow
  as a distance function with the falloff of `decor.c`.
- `make shaders` compiles the sources with glslc into SPIR-V C arrays.
  The arrays are checked in, so the normal build needs no glslc.
- A host check renders a test scene through libvk and vtest and compares
  the result with the CPU compositor output within a tolerance.

### GV5 X12 GPU backend (about 2,500 lines)

- `user/compositor/backend_vk.c` beside `backend_fb.c`. X12 selects the
  backend when `/dev/gpu0` offers Venus. The setting `backend cpu` forces
  the software path.
- One texture per surface. X12 uploads the damaged parts of the shared
  memory buffers through a staging blob.
- One command buffer per frame draws the scene: surfaces with the AN2
  visual parameters, shadows, decorations, lock, overlay and cursor.
- Output: X12 copies the damaged rectangles from a host-visible buffer
  into the `/dev/fb0` mapping and calls `FBIO_FLUSH_RECTS`. An experiment
  attaches the host-visible blob as backing of the 2D scanout resource to
  avoid the copy, and the result is recorded.
- compstat reports the backend and the GPU time per frame.
  `graphics-performance.md` compares both backends.

## Critical files

- `user/compositor/main.c`, `scene.c`, `comp.h`, `debug.c`, `wm/window.c`,
  `wm/decor.c`, `surface.c`, `backend_fb.c`, new `anim.c`, `backend_vk.c`
- `lib/libgui/src/pixel.c`, `lib/libgui/include/gui/pixel.h`
- `kernel/drivers/virtio/virtio.c`, `virtio_gpu.c`, new `kernel/drivers/gpu.c`
- `kernel/include/minios/abi.h`, `kernel/mm/mmap.c` (`vma_map_device`)
- `tools/run.sh`, `tests/run_qemu_test.sh`, `tests/map`
- New `tools/build-qemu-venus.sh`, `tools/venus-gen.py`, `lib/libvk/`
- Docs: `docs/design/compositor.md`, `gpu.md` (new), `display.md`,
  `graphics-performance.md`, `locking.md`, `protocol.md`, `build.md`

## Verification

- AN1 to AN4: `make check-libgui` for the pixel functions, boot cases
  `x12_anim` plus the existing GUI cases (`gui_widgets`, `gui_tools`,
  `gui_dnd` and the panel cases) with animations disabled, compbench
  numbers at 2560x1600@2 on aarch64 with hvf, and a screen recording
  through the QEMU monitor for the owner's review.
- GV1: the built QEMU starts minios with `--gpu venus`, and minios lists
  the device with the shared memory capability.
- GV2: boot case `gpu_venus`.
- GV3: `make check-libvk` on the host, boot case `gpu_vulkan`.
- GV4: the host comparison of the shader output with the CPU output.
- GV5: the GUI boot cases with the Vulkan backend and with the CPU
  backend, and compbench numbers for both.

## Order

AN1 to AN4 first, with a commit per milestone. The GV milestones follow in order.
GV1 comes first because the series for macOS might not build. Its result
decides between the upstream series and the UTM forks before any guest
work starts.
