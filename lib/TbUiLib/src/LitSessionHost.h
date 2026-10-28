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

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class QVulkanInstance;
class QWidget;

namespace tb::lit
{
class GameContainerIndex;
class MaterialService;
} // namespace tb::lit

namespace tb::ui
{
class LitDocumentObserver;
class LitPreviewWindow;
class MapDocument;

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
 * The material service (lit/MaterialService.h, the parse thread) starts with phase A,
 * over the game path the owner gives, its decl sources made through the owner's container
 * index when it gives one (LP6.3, C67 (a): the index outlives the host, which is made per
 * Start, so a Start or a reload whose containers' key stands skips their scan), and its
 * log lines come through the same queue as
 * the session's messages, as MaterialInfo and MaterialWarning. A new table comes the same
 * way, as MaterialsPublished: the parse thread's listener posts it without blocking, at
 * most one at a time until this host has handled it, and the owner reads the latest
 * table then. Phase B hands the service to the session, which forgets it in its own
 * teardown step 1, long before the service goes in the last step here.
 *
 * The document observer (LitDocumentObserver, §3.3, LP1b.1) is made with phase A, after
 * the material service, and watches the document for the session: it offers the
 * session its snapshots once phase B has run, while the window is exposed, and reloads
 * the material service when the material collections change. The host passes it
 * ReadyForSnapshot, the panel's panelShown and every window change.
 *
 * Teardown runs these steps in this order, each once, whatever ran of the start:
 * 1. ask the parse thread to stop after its current directory entry, container entry or
 *    decl file (plan step 1, R5.20, G4); then stop the render thread: park it, then stop
 *    and join it, while the window system's sent messages are serviced, because a
 *    present may wait for one (no bound on the join: a thread that still runs uses the
 *    session, so the wait goes on and says so); then the session stops its CPU pool and
 *    forgets the material service
 * 2. destroy the document observer, which drops its subscriptions: the render thread
 *    takes no snapshot any more, and the material service it reloads is still alive
 * 3. LitSession::releaseGpuObjects
 * 4. delete the window container, which destroys the window and Qt's VkSurfaceKHR
 * 5. LitSession::destroyVkDevice
 * 6. destroy the QVulkanInstance
 * 7. reset the dispatcher
 * 8. destroy the material registry on the parse thread, then join it (plan step 8)
 * The window is detached before the first step. A warning, error or loss that the
 * session posted and the event loop did not deliver before the stop is carried into the
 * report (review R.1), and the report lists the steps in the order they ran. An abandoned
 * host destroys its observer at once, because the document may go with the owner.
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
  // receives Ready, Warning, DeviceLost, Error and the material events on the GUI
  // thread, queued; ReadyForSnapshot goes to the document observer
  using EventHandler = std::function<void(
    std::uint64_t generation, lit::SessionEvent event, const std::string& message)>;
  // the owner's last copy of the 3D view's camera, if it has one
  using CameraProvider = std::function<std::optional<lit::CameraState>()>;

private:
  enum class TeardownStep
  {
    None,
    RenderThreadStopped,
    ObserverDestroyed,
    GpuObjectsReleased,
    ContainerDeleted,
    VkDeviceDestroyed,
    InstanceDestroyed,
    DispatcherReset,
    MaterialsDestroyed,
  };

  /**
   * The session's warnings, errors and losses that are queued to this host and not yet
   * delivered; any thread. The teardown carries what is left into the report, because
   * deleting the host drops its posted events (review R.1).
   */
  class UndeliveredMessages
  {
  private:
    std::mutex m_mutex;
    std::uint64_t m_next = 0;
    std::map<std::uint64_t, std::pair<lit::SessionEvent, std::string>> m_queued;

  public:
    std::uint64_t add(lit::SessionEvent event, std::string message);
    void remove(std::uint64_t id);
    std::vector<std::pair<lit::SessionEvent, std::string>> take();
  };

  class BusyScope;

  lit::SessionOptions m_options;
  std::uint64_t m_generation;
  EventHandler m_eventHandler;
  CameraProvider m_camera;
  // the owner's document, which outlives this host unless the host is abandoned
  MapDocument& m_document;
  // the game whose decls the material service reads; empty: no service
  std::filesystem::path m_gamePath;
  // LP6.3 (C67 (a), A52): the owner's container index, which the parse thread's
  // makeSource holds too; null: every build scans
  std::shared_ptr<lit::GameContainerIndex> m_containerIndex;
  std::shared_ptr<UndeliveredMessages> m_undelivered =
    std::make_shared<UndeliveredMessages>();
  // a MaterialsPublished is queued and not yet handled; set by the parse thread's
  // listener, cleared here before the owner reads the table
  std::shared_ptr<std::atomic<bool>> m_publishPending =
    std::make_shared<std::atomic<bool>>(false);

  std::unique_ptr<QVulkanInstance> m_instance;
  lit::InstanceHandles m_handles;
  bool m_dispatcherInitialised = false;
  // owned by the container, which teardown deletes
  QPointer<LitPreviewWindow> m_window;
  QPointer<QWidget> m_container;
  std::unique_ptr<lit::LitSession> m_session;
  // the parse thread; it outlives the session, whose render thread will read its tables
  std::unique_ptr<lit::MaterialService> m_materials;
  // from phase A to teardown step 2
  std::unique_ptr<LitDocumentObserver> m_observer;

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
    CameraProvider camera,
    MapDocument& document,
    std::filesystem::path gamePath = {},
    std::shared_ptr<lit::GameContainerIndex> containerIndex = nullptr);
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
  // the session after phase B, else null
  lit::LitSession* session() const;
  /**
   * LP4.3 (D90, K8): the session's status (lit::LitSession::status) with what the host
   * knows of the document: whether it holds unsaved changes (mdl::Map::modified) and its
   * file's last write in Unix seconds (lit::statGameFile; -1 for a document never saved
   * or a file that cannot be read), which the status line's stale notice compares with
   * the tree file's time. A default status without a session. GUI thread.
   */
  lit::SessionStatus status() const;
  // the parse thread from phase A until the teardown's last step, else null
  lit::MaterialService* materials() const;
  // C1 (LP2.1): the options phase B creates the session with: the owner's, with the
  // document's game path, whose blobs the compile thread reads; a game-path change ends
  // the session (D41). Tests
  lit::SessionOptions sessionOptionsForTesting() const;

  // passed on once phase B has run; phase B asks the camera provider
  void setCamera(const lit::CameraState& camera);
  // the panel was shown: the document observer offers what coalesced while it was hidden
  void panelShown();

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
  void startMaterials();
  void stopMaterials();

  void reconcile();
  void reconcileOnce();
  lit::SessionOptions sessionOptions() const;
  void createSession(QSize size);
  void replaceSwapchain(QSize size);
  bool parkServicingSentMessages();
  void afterBusy();
  QSize pixelSize() const;

  lit::EventSink eventSink();
  // what eventSink posts through: false when the message could not be queued (B0.8)
  std::function<bool(lit::SessionEvent, std::string, std::uint64_t)> eventQueue();
  void sessionEventReceived(
    std::uint64_t generation, lit::SessionEvent event, const std::string& message);
  void dispatchSessionEvent(lit::SessionEvent event, const std::string& message);
  void fail(std::string reason);

  deleteCopyAndMove(LitSessionHost);
};

} // namespace tb::ui
