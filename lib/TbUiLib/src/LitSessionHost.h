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

#include <QObject>
#include <QPointer>
#include <QSize>

#include "base/Macros.h"
#include "base/Result.h"
#include "lit/LitSession.h"
#include "lit/VulkanInstance.h"
#include "ui/LitPreviewPanel.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

class QVulkanInstance;
class QWidget;

namespace tb::ui
{
class LitPreviewWindow;

/**
 * The Qt half of one lit preview session: the QVulkanInstance, the window and its
 * container, and the LitSession, with the generation that every message of the session
 * carries. GUI thread only. Private to TbUiLib: it needs TbLitLib's headers.
 *
 * Start runs in two phases. start() is phase A: the instance with the dispatcher, then a
 * new window and its container, which the caller shows. Phase B runs on the window's
 * first expose with a non-zero size, because only then does the window have a surface:
 * LitSession::create on it, then the exposure and the camera.
 *
 * The surface handshake: a resize, a DPR or screen change and an out-of-date swapchain
 * park the render thread, replace the swapchain and unpark it. A render thread that does
 * not park in time is still presenting, so the rest waits for SessionEvent::Parked, and a
 * park that is no longer needed when it arrives is released. SurfaceAboutToBeDestroyed
 * cannot wait: Qt destroys the surface when it returns, so the swapchain goes first.
 *
 * Teardown runs these steps in this order, each once, whatever ran of the start:
 * 1. stop the render thread: park it, then stop and join it, while the window system's
 *    sent messages are serviced, because a present may wait for one (no bound on the
 *    join: a thread that still runs uses the session, so the wait goes on and says so)
 * 2. LitSession::releaseGpuObjects
 * 3. delete the window container, which destroys the window and Qt's VkSurfaceKHR
 * 4. LitSession::destroyVkDevice
 * 5. destroy the QVulkanInstance
 * 6. reset the dispatcher
 * The window is detached before the first step.
 *
 * While the host services sent messages it is busy: Qt code that runs then and calls back
 * into the host is deferred to the event loop, and an owner that goes away meanwhile
 * abandons the host, which ends itself once the wait is over. Nothing here stops the
 * session itself: a failure is queued as SessionEvent::Error, so that the owner's Stop
 * never deletes the window inside one of its own event handlers.
 */
class LitSessionHost : public QObject
{
public:
  // receives Ready, Warning, DeviceLost and Error on the GUI thread, queued
  using EventHandler = std::function<void(
    std::uint64_t generation, lit::SessionEvent event, const std::string& message)>;

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

  class BusyScope;

  lit::SessionOptions m_options;
  std::uint64_t m_generation;
  EventHandler m_eventHandler;
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
  // a park was asked for and not released yet
  bool m_parkPending = false;
  std::uint64_t m_parkTimeouts = 0;
  bool m_failed = false;
  bool m_reconciling = false;
  bool m_reconcileAgain = false;

  // inside a wait that services sent messages
  bool m_busy = false;
  bool m_reconcileDeferred = false;
  bool m_screenChangeDeferred = false;
  bool m_abandoned = false;
  // the render thread did not stop within StopWarningTime
  bool m_stopOverran = false;

  TeardownStep m_teardownStep = TeardownStep::None;
  LitPreviewReport m_report;

public:
  LitSessionHost(
    lit::SessionOptions options,
    std::uint64_t generation,
    EventHandler eventHandler,
    LitPreviewDiagnostics& diagnostics);
  ~LitSessionHost() override;

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
  // the session after phase B, else null
  lit::LitSession* session() const;

  // Kept for phase B, and passed on once it has run.
  void setCamera(const lit::CameraState& camera);

  /**
   * Whether the host is inside a wait that services the window system's sent messages.
   * The owner must not tear it down then; it defers its Stop, or abandons the host if it
   * goes away itself.
   */
  bool busy() const;
  void abandon();

  // Window events.

  // an expose or a resize: phase B, the exposure, the swapchain's size
  void windowChanged();
  void devicePixelRatioChanged();
  void surfaceAboutToBeDestroyed();
  void screenChanged();
  void updateRequested();

  // Runs every teardown step that has not run, and says what it found.
  const LitPreviewReport& teardown();

private:
  void runTeardownThrough(TeardownStep last);
  void stopRenderThread();
  void recordTeardown();

  void reconcile();
  void reconcileOnce();
  void createSession(QSize size);
  void replaceSwapchain(QSize size);
  bool parkServicingSentMessages();
  void afterBusy();
  QSize pixelSize() const;

  lit::EventSink eventSink();
  void sessionEventReceived(
    std::uint64_t generation, lit::SessionEvent event, const std::string& message);
  void fail(std::string reason);

  deleteCopyAndMove(LitSessionHost);
};

} // namespace tb::ui
