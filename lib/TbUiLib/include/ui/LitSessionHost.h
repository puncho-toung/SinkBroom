/*
 Copyright (C) 2026 TrenchBroom contributors

 This file is part of TrenchBroom.

 TrenchBroom is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 TrenchBroom is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with TrenchBroom. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <QPointer>
#include <QSize>

#include "base/Macros.h"
#include "base/Result.h"
#include "lit/LitSession.h"
#include "lit/VulkanInstance.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

class QVulkanInstance;
class QWidget;

namespace tb::ui
{
class LitPreviewWindow;
struct LitPreviewDiagnostics;

/**
 * The Qt half of one lit preview session (§3.1, C18): the QVulkanInstance, the window and
 * its container, and the LitSession, with the generation that every message of the
 * session carries (D53). GUI thread only.
 *
 * Start runs in two phases (D46). start() is phase A: the instance with the dispatcher,
 * then a new window and its container, which the caller shows. Phase B runs on the
 * window's first expose with a non-zero size: LitSession::create on the window's surface,
 * then the exposure and the camera.
 *
 * The surface handshake (§3.1, D49): a resize, a DPR or screen change and an
 * out-of-date swapchain park the render thread, replace the swapchain and unpark it; a
 * render thread that does not park in time is still presenting, so the rest waits for
 * SessionEvent::Parked. SurfaceAboutToBeDestroyed cannot wait: Qt destroys the surface
 * when it returns, so the swapchain goes first, and a render thread that never parks is
 * stopped there.
 *
 * Teardown runs these steps in this order, each once, whatever ran of the start:
 * 1. LitSession::stopRenderThread
 * 2. LitSession::releaseGpuObjects
 * 3. delete the window container, which destroys the window and Qt's VkSurfaceKHR
 * 4. LitSession::destroyVkDevice
 * 5. destroy the QVulkanInstance
 * 6. reset the dispatcher
 * The window is detached before the first step.
 *
 * Nothing here stops the session itself: a failure is posted to the event sink as
 * SessionEvent::Error, so that the owner's Stop never deletes the window inside one of
 * its own event handlers.
 */
class LitSessionHost
{
private:
  enum class TeardownStep
  {
    None,
    RenderThreadStopped,
    GpuObjectsReleased,
    ContainerDeleted,
    VkDeviceDestroyed,
    InstanceDestroyed,
    DispatcherReset,
  };

  lit::SessionOptions m_options;
  std::uint64_t m_generation;
  lit::EventSink m_eventSink;
  LitPreviewDiagnostics& m_diagnostics;

  std::unique_ptr<QVulkanInstance> m_instance;
  lit::InstanceHandles m_handles;
  bool m_dispatcherInitialised = false;
  // owned by the container, which teardown deletes
  QPointer<LitPreviewWindow> m_window;
  QPointer<QWidget> m_container;
  std::unique_ptr<lit::LitSession> m_session;

  std::optional<lit::CameraState> m_camera;
  // the size in pixels that the current swapchain was made for
  QSize m_swapchainSize;
  // the swapchain must be replaced: out of date, or a new screen or surface
  bool m_swapchainStale = false;
  bool m_failed = false;
  bool m_reconciling = false;
  bool m_reconcileAgain = false;

  TeardownStep m_teardownStep = TeardownStep::None;

public:
  LitSessionHost(
    lit::SessionOptions options,
    std::uint64_t generation,
    lit::EventSink eventSink,
    LitPreviewDiagnostics& diagnostics);
  ~LitSessionHost();

  std::uint64_t generation() const;

  /**
   * Phase A: creates the instance, initialises the dispatcher, and makes the window and
   * its container as a child of the given widget. On an error the reason is returned and
   * the teardown runs over what exists.
   */
  Result<QWidget*> start(QWidget& containerParent);

  // phase B ran
  bool drawing() const;
  LitPreviewWindow* window() const;
  QWidget* container() const;
  std::uint64_t framesPresented() const;
  void debugInjectDeviceLost();

  // Kept for phase B, and passed on once it has run (§3.2).
  void setCamera(const lit::CameraState& camera);

  // Window events.

  // an expose, a resize or a DPR change: phase B, the exposure, the swapchain's size
  void windowChanged();
  void surfaceAboutToBeDestroyed();
  void screenChanged();
  void updateRequested();

  // Session events, queued to the GUI thread.

  void swapchainOutOfDate();
  void parked();

  // Runs every teardown step that has not run.
  void teardown();

private:
  void runTeardownThrough(TeardownStep last);
  void recordTeardown();

  void reconcile();
  void reconcileOnce();
  void createSession(QSize size);
  void replaceSwapchain(QSize size);
  bool parkServicingSentMessages();
  QSize pixelSize() const;
  void fail(std::string reason);

  deleteCopyAndMove(LitSessionHost);
};

} // namespace tb::ui
