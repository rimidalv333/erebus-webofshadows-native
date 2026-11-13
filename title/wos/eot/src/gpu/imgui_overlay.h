// gpu/imgui_overlay.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <functional>
#include <memory>

#include <rex/types.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/presenter.h>

namespace plume {
struct RenderCommandList;
struct RenderFramebuffer;
}

namespace eot::gpu {

class OverlayVideoLockScope final {
public:
  OverlayVideoLockScope();
  ~OverlayVideoLockScope();

  OverlayVideoLockScope(const OverlayVideoLockScope &) = delete;
  OverlayVideoLockScope &operator=(const OverlayVideoLockScope &) = delete;

private:
  bool previous_;
};

class OverlayDrawContext final : public rex::ui::AppUIDrawContext {
public:
  OverlayDrawContext(u32 width, u32 height, plume::RenderCommandList *cmd,
                     plume::RenderFramebuffer *framebuffer)
      : rex::ui::AppUIDrawContext(width, height), cmd_(cmd), framebuffer_(framebuffer) {}

  plume::RenderCommandList *command_list() const { return cmd_; }
  plume::RenderFramebuffer *framebuffer() const { return framebuffer_; }

private:
  plume::RenderCommandList *cmd_;
  plume::RenderFramebuffer *framebuffer_;
};

std::unique_ptr<rex::ui::ImmediateDrawer> CreateOverlayDrawer();

using OverlayDrawHook = std::function<void(plume::RenderCommandList *, plume::RenderFramebuffer *,
                                           u32 width, u32 height)>;
void SetOverlayDrawHook(OverlayDrawHook hook);
void RunOverlayDrawHook(plume::RenderCommandList *cmd, plume::RenderFramebuffer *framebuffer,
                        u32 width, u32 height);

}
