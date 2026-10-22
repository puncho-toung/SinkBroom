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

#include "LitSessionHost.h"

#include <QByteArrayList>
#include <QMetaObject>
#include <QVersionNumber>
#include <QVulkanInstance>
#include <QWidget>
#include <QtLogging>

#include "ServiceSentMessages.h"
#include "lit/GameDeclSource.h"
#include "lit/MaterialService.h"
#include "ui/LitDocumentObserver.h"
#include "ui/LitPreviewPanel.h"
#include "ui/LitPreviewWindow.h"

#include "kd/result.h"
#include "kd/set_temp.h"

#include <fmt/format.h>

#include <atomic>
#include <chrono>
#include <utility>
#include <variant>

namespace tb::ui
{
namespace
{

using namespace std::chrono_literals;

constexpr auto ValidationLayerName = "VK_LAYER_KHRONOS_validation";

// a resize waits this long for the render thread to park before it goes back to the
// event loop, which the render thread's present may need; SessionEvent::Parked resumes it
constexpr auto HandshakeParkTimeout = 100ms;

// the waits that service the window system's sent messages look at the render thread this
// often
constexpr auto ServicingInterval = 10ms;

// how long a stop waits for the render thread before it says so; it keeps waiting,
// because a thread that still runs uses the session
constexpr auto StopWarningTime = 2000ms;

} // namespace

/**
 * Marks the host busy while it waits and services the window system's sent messages,
 * which can run Qt code that calls back into the host or its owner. Calls that arrive
 * meanwhile are deferred (see reconcile, screenChanged and the owner's Stop), and the
 * outermost scope runs them, from the event loop, once the wait is over.
 */
class LitSessionHost::BusyScope
{
public:
  explicit BusyScope(LitSessionHost& host)
    : m_host{host}
    , m_outermost{!host.m_busy}
  {
    m_host.m_busy = true;
  }

  ~BusyScope()
  {
    if (m_outermost)
    {
      m_host.m_busy = false;
      m_host.afterBusy();
    }
  }

  BusyScope(const BusyScope&) = delete;
  BusyScope& operator=(const BusyScope&) = delete;

private:
  LitSessionHost& m_host;
  bool m_outermost;
};

std::uint64_t LitSessionHost::UndeliveredMessages::add(
  const lit::SessionEvent event, std::string message)
{
  const auto lock = std::scoped_lock{m_mutex};
  const auto id = m_next++;
  m_queued.emplace(id, std::pair{event, std::move(message)});
  return id;
}

void LitSessionHost::UndeliveredMessages::remove(const std::uint64_t id)
{
  const auto lock = std::scoped_lock{m_mutex};
  m_queued.erase(id);
}

std::vector<std::pair<lit::SessionEvent, std::string>> LitSessionHost::
  UndeliveredMessages::take()
{
  const auto lock = std::scoped_lock{m_mutex};
  auto messages = std::vector<std::pair<lit::SessionEvent, std::string>>{};
  for (auto& entry : m_queued)
  {
    messages.push_back(std::move(entry.second));
  }
  m_queued.clear();
  return messages;
}

LitSessionHost::LitSessionHost(
  lit::SessionOptions options,
  const std::uint64_t generation,
  EventHandler eventHandler,
  CameraProvider camera,
  MapDocument& document,
  std::filesystem::path gamePath)
  : m_options{std::move(options)}
  , m_generation{generation}
  , m_eventHandler{std::move(eventHandler)}
  , m_camera{std::move(camera)}
  , m_document{document}
  , m_gamePath{std::move(gamePath)}
{
}

LitSessionHost::~LitSessionHost()
{
  teardown();
}

bool LitSessionHost::busy() const
{
  return m_busy;
}

void LitSessionHost::abandon()
{
  // The owner goes while this host waits inside one of its own frames. It ends itself
  // once that wait is over, and keeps its container until then, out of the owner's
  // children, so that the window and its surface outlive the swapchain.
  m_abandoned = true;
  m_eventHandler = nullptr;
  m_camera = nullptr;
  // the document may go with the owner; the observer is never on the stack of a wait
  m_observer.reset();
  if (m_container)
  {
    m_container->hide();
    m_container->setParent(nullptr);
  }
}

std::uint64_t LitSessionHost::generation() const
{
  return m_generation;
}

Result<QWidget*> LitSessionHost::start(QWidget& containerParent)
{
  // phase A (§3.1, D46); the first call into Qt's Vulkan support loads the loader (C9)
  m_instance = std::make_unique<QVulkanInstance>();
  if (m_instance->supportedApiVersion() < QVersionNumber{1, 2})
  {
    teardown();
    return Error{"The lit preview needs Vulkan 1.2, which this system does not offer."};
  }

  // Qt adds VK_KHR_surface and the platform's surface extension, and VK_EXT_debug_utils
  // for its debug output redirect, which validation needs; the session asks for nothing
  // else (the engine's other instance extensions are listed in TbLitLib's mirror.json)
  m_instance->setApiVersion(QVersionNumber{1, 2});
  if (m_options.validation)
  {
    // R-B16: only a layer that is installed
    if (m_instance->supportedLayers().contains(ValidationLayerName))
    {
      m_instance->setLayers({ValidationLayerName});
    }
  }
  else
  {
    m_instance->setFlags(QVulkanInstance::NoDebugOutputRedirect);
  }

  if (!m_instance->create())
  {
    const auto errorCode = int(m_instance->errorCode());
    teardown();
    return Error{
      fmt::format("Cannot create a Vulkan instance (VkResult {}).", errorCode)};
  }

  m_handles.instance = m_instance->vkInstance();
  m_handles.getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
    m_instance->getInstanceProcAddr("vkGetInstanceProcAddr"));
  for (const auto& name : m_instance->extensions())
  {
    m_handles.enabledExtensions.push_back(name.toStdString());
  }
  // R4-2.22: on every Start, for this instance
  if (!lit::initInstanceDispatcher(m_handles))
  {
    teardown();
    return Error{"Another lit preview session runs in this process."};
  }
  m_dispatcherInitialised = true;

  // a new window for every session, never reused (§3.1)
  auto* window = new LitPreviewWindow{*this};
  window->setVulkanInstance(m_instance.get());
  m_window = window;

  // never inside a scroll area, which would make the whole parent chain native
  // (R4-1.15); NoFocus, so that the 3D view keeps the keyboard (D44)
  m_container = QWidget::createWindowContainer(window, &containerParent);
  m_container->setObjectName("LitPreviewPanel_Container");
  m_container->setFocusPolicy(Qt::NoFocus);

  // the parse thread needs no device, so it starts with phase A (§3.1, §3.5)
  startMaterials();

  // §3.3: the session's view of the document; its first snapshot goes to the session as
  // soon as phase B has run and the render thread is ready
  m_observer = std::make_unique<LitDocumentObserver>(
    m_document,
    m_generation,
    LitSnapshotTarget{
      [this] { return m_window && m_window->isExposed(); },
      [this] {
        const auto* liveSession = session();
        return liveSession && liveSession->snapshotSlotFree();
      },
      [this](std::shared_ptr<const lit::Snapshot> snapshot) {
        auto* liveSession = session();
        return liveSession && liveSession->offerSnapshot(std::move(snapshot));
      },
      [this] {
        if (m_materials)
        {
          m_materials->reload();
        }
      },
    });
  return m_container.data();
}

void LitSessionHost::startMaterials()
{
  if (m_gamePath.empty())
  {
    return;
  }

  auto options = lit::MaterialServiceOptions{};
  // C1: the game path's base folder only; a reload scans its containers again, and the
  // stop flag cuts the scan short (G4)
  options.makeSource = [gamePath = m_gamePath](const std::atomic<bool>& cancel) {
    return std::unique_ptr<lit::DeclSource>{
      std::make_unique<lit::GameDeclSource>(gamePath, &cancel)};
  };
  // R4-1.17, R5.17: the parse thread's log lines and its new tables come through the
  // session's queue, with its generation. The listener must not block: a table is posted
  // only when none is pending, and the GUI thread reads the latest one when it handles it
  options.listener =
    [queue = eventQueue(), generation = m_generation, publishPending = m_publishPending](
      const lit::MaterialServiceEvent& event) {
      switch (event.kind)
      {
      case lit::MaterialServiceEvent::Kind::Message:
        static_cast<void>(queue(
          event.level == lit::DeclMessageKind::Info ? lit::SessionEvent::MaterialInfo
                                                    : lit::SessionEvent::MaterialWarning,
          event.text,
          generation));
        break;
      case lit::MaterialServiceEvent::Kind::Published:
        // B0.8: a table that could not be posted leaves the flag free, so that the next
        // one is posted
        if (
          !publishPending->exchange(true)
          && !queue(lit::SessionEvent::MaterialsPublished, {}, generation))
        {
          publishPending->store(false);
        }
        break;
      }
    };
  options.parseAll = true;
  lit::MaterialService::create(std::move(options))
    | kdl::transform([&](std::unique_ptr<lit::MaterialService> service) {
        m_materials = std::move(service);
      })
    | kdl::transform_error([&](const auto& e) {
        // no materials: the preview draws without them
        eventSink()(lit::SessionEvent::MaterialWarning, e.msg, m_generation);
      });
}

void LitSessionHost::stopMaterials()
{
  if (m_materials)
  {
    // plan step 8: the registry is destroyed on its thread, then the thread is joined
    m_materials->requestStop();
    m_materials->join();
    m_materials.reset();
  }
}

bool LitSessionHost::drawing() const
{
  return m_session != nullptr && m_teardownStep == TeardownStep::None;
}

lit::LitSession* LitSessionHost::session() const
{
  return drawing() ? m_session.get() : nullptr;
}

lit::MaterialService* LitSessionHost::materials() const
{
  return m_materials.get();
}

void LitSessionHost::setCamera(const lit::CameraState& camera)
{
  if (drawing())
  {
    m_session->setCamera(camera);
  }
}

void LitSessionHost::panelShown()
{
  if (m_observer)
  {
    m_observer->requestFlush();
  }
}

void LitSessionHost::windowChanged()
{
  reconcile();
  // an exposed window takes what coalesced while it was hidden (S31)
  if (m_observer)
  {
    m_observer->requestFlush();
  }
}

void LitSessionHost::devicePixelRatioChanged()
{
  if (drawing())
  {
    m_session->surfaceChanged();
  }
  reconcile();
}

void LitSessionHost::surfaceAboutToBeDestroyed()
{
  if (!drawing())
  {
    // phase A: nothing uses the surface yet
    return;
  }

  // Qt destroys the VkSurfaceKHR when this returns, so the swapchain goes now; the
  // render thread stays parked until the next expose brings the new surface
  if (parkServicingSentMessages())
  {
    m_session->destroySwapchain();
    m_swapchainStale = true;
    m_parkPending = true;
    return;
  }

  // it never parked: stop it, servicing messages while it finishes its frame, so that the
  // swapchain is gone before the surface; the rest of the teardown follows with the
  // owner's Stop
  runTeardownThrough(TeardownStep::GpuObjectsReleased);
  fail(
    "The lit preview did not stop drawing before its window's surface was destroyed. "
    "Press Start to run it again.");
}

void LitSessionHost::screenChanged()
{
  if (!drawing() || m_failed)
  {
    return;
  }
  if (m_busy)
  {
    m_screenChangeDeferred = true;
    return;
  }

  // R5.33: on a hybrid-GPU laptop the new screen may belong to the other GPU
  if (!m_instance->supportsPresent(
        m_session->physicalDevice(), m_session->queueFamily(), m_window.data()))
  {
    fail(
      "The preview's GPU cannot present to this screen, which can happen on a "
      "hybrid-GPU laptop. Press Start to run it on this screen's GPU.");
    return;
  }

  m_session->surfaceChanged();
  m_swapchainStale = true;
  reconcile();
}

void LitSessionHost::updateRequested()
{
  if (drawing() && m_window->isExposed())
  {
    m_session->requestFrame();
  }
}

const LitPreviewReport& LitSessionHost::teardown()
{
  runTeardownThrough(TeardownStep::MaterialsDestroyed);
  return m_report;
}

void LitSessionHost::runTeardownThrough(const TeardownStep last)
{
  // the window's handlers reach nothing from here on
  if (m_window)
  {
    m_window->detach();
  }

  while (m_teardownStep < last)
  {
    m_teardownStep = TeardownStep(int(m_teardownStep) + 1);
    switch (m_teardownStep)
    {
    case TeardownStep::None:
      break;
    case TeardownStep::RenderThreadStopped:
      // plan step 1, R5.20, G4: first, so that the parse thread's scan or read stops
      // while the render thread is stopped; it reports nothing more
      if (m_materials)
      {
        m_materials->requestStop();
      }
      if (m_session)
      {
        stopRenderThread();
      }
      m_report.teardownSteps.emplace_back("render thread stopped");
      break;
    case TeardownStep::ObserverDestroyed:
      // after the render thread, which takes no snapshot any more, and before the
      // material service, which the observer reloads
      m_observer.reset();
      m_report.teardownSteps.emplace_back("document observer destroyed");
      break;
    case TeardownStep::GpuObjectsReleased:
      if (m_session)
      {
        m_session->releaseGpuObjects();
      }
      m_report.teardownSteps.emplace_back("GPU objects released");
      break;
    case TeardownStep::ContainerDeleted:
      // the window and Qt's VkSurfaceKHR go with it, while the instance is still alive
      delete m_container.data();
      m_report.teardownSteps.emplace_back("container deleted");
      break;
    case TeardownStep::VkDeviceDestroyed:
      if (m_session)
      {
        m_session->destroyVkDevice();
      }
      recordTeardown();
      m_session.reset();
      m_report.teardownSteps.emplace_back("device destroyed");
      break;
    case TeardownStep::InstanceDestroyed:
      m_instance.reset();
      m_report.teardownSteps.emplace_back("instance destroyed");
      break;
    case TeardownStep::DispatcherReset:
      // after the session, which held the dispatcher, is gone
      if (m_dispatcherInitialised && !lit::resetDispatcher())
      {
        m_report.messages.push_back(
          "The Vulkan dispatcher was still in use at the stop.");
      }
      m_report.teardownSteps.emplace_back("dispatcher reset");
      break;
    case TeardownStep::MaterialsDestroyed:
      stopMaterials();
      // review R.1: what the event loop did not deliver before the stop would go with
      // this host's posted events, so it goes into the report
      for (auto& [event, message] : m_undelivered->take())
      {
        m_report.messages.push_back(
          fmt::format("{} (not delivered before the stop)", message));
        m_report.deviceLost =
          m_report.deviceLost || event == lit::SessionEvent::DeviceLost;
      }
      m_report.teardownSteps.emplace_back("materials destroyed");
      break;
    }
  }
}

void LitSessionHost::recordTeardown()
{
  m_report.generation = m_generation;
  m_report.hadDevice = m_session != nullptr;
  m_report.stopOverran = m_stopOverran;
  m_report.parkTimeouts = m_parkTimeouts;
  if (m_session)
  {
    auto sessionReport = m_session->teardownReport();
    m_report.deviceReleaseCount = sessionReport.deviceReleaseCount;
    m_report.nvrhiErrors = sessionReport.nvrhiErrors;
    m_report.deviceLost = sessionReport.deviceLost;
    m_report.frameLogError = std::move(sessionReport.frameLogError);
    m_report.messages = std::move(sessionReport.messages);
    m_report.framesPresented = sessionReport.framesPresented;
    m_report.swapchainGenerations = sessionReport.swapchainGenerations;
    if (sessionReport.vramAtStart)
    {
      m_report.vramAtStartBytes = sessionReport.vramAtStart->usedBytes;
    }
    if (sessionReport.vramAtStop)
    {
      m_report.vramAtStopBytes = sessionReport.vramAtStop->usedBytes;
    }
  }
}

void LitSessionHost::reconcile()
{
  if (!m_window || m_failed || m_teardownStep != TeardownStep::None)
  {
    return;
  }

  // never inside a wait that services messages, for example while the surface is going
  if (m_busy)
  {
    m_reconcileDeferred = true;
    return;
  }

  // a swapchain call may dispatch window messages that come back here
  if (m_reconciling)
  {
    m_reconcileAgain = true;
    return;
  }

  const auto reconciling = kdl::set_temp{m_reconciling};
  do
  {
    m_reconcileAgain = false;
    reconcileOnce();
  } while (m_reconcileAgain && m_window && !m_failed);
}

void LitSessionHost::reconcileOnce()
{
  const auto exposed = m_window->isExposed();
  const auto size = pixelSize();

  if (!m_session)
  {
    // phase B waits for the first expose with a size (R5.2)
    if (exposed && !size.isEmpty())
    {
      createSession(size);
    }
    return;
  }

  // R5.25: frames are drawn only while the window can be seen; a hidden window keeps its
  // swapchain until it is shown again
  m_session->setExposed(exposed);
  // review N.5: a 0x0 extent made no swapchain, and none is wanted until the size
  // changes; replacing it again would park again and post another Parked
  const auto missing =
    !m_session->hasSwapchain() && !(size.isEmpty() && size == m_swapchainSize);
  if (exposed && (m_swapchainStale || missing || size != m_swapchainSize))
  {
    replaceSwapchain(size);
    return;
  }

  // Nothing to replace, for example because the size came back while a park asked for
  // earlier timed out: that park is released, or the preview would stay frozen.
  if (m_parkPending)
  {
    if (m_session->isParked())
    {
      // Review V.1: a thread that parked released its generation's image wrappers, so it
      // is resumed only on a new generation, made when the window can be seen.
      m_swapchainStale = true;
      if (exposed)
      {
        replaceSwapchain(size);
      }
      return;
    }
    m_session->unpark();
    m_parkPending = false;
  }
}

void LitSessionHost::createSession(const QSize size)
{
  const auto surface = QVulkanInstance::surfaceForWindow(m_window.data());
  if (surface == VK_NULL_HANDLE)
  {
    fail("Cannot create the Vulkan surface of the preview's window.");
    return;
  }

  // the service outlives the session: it goes in the last teardown step, the session in
  // step 4, and the session forgets it in its step 1
  lit::LitSession::create(
    m_handles,
    surface,
    std::uint32_t(size.width()),
    std::uint32_t(size.height()),
    m_options,
    eventSink(),
    m_generation,
    m_materials.get())
    | kdl::transform([&](std::unique_ptr<lit::LitSession> session) {
        m_session = std::move(session);
        m_swapchainSize = size;
        m_swapchainStale = false;
        m_session->setExposed(m_window->isExposed());
        if (const auto camera = m_camera ? m_camera() : std::nullopt)
        {
          m_session->setCamera(*camera);
        }
      })
    | kdl::transform_error([&](const auto& e) { fail(e.msg); });
}

void LitSessionHost::replaceSwapchain(const QSize size)
{
  // D49: the swapchain is replaced only while the render thread is parked; if it does not
  // park in time it is still presenting, so this goes back to the event loop, and the
  // Parked event or the next window event resumes it
  m_parkPending = true;
  if (!m_session->park(HandshakeParkTimeout))
  {
    ++m_parkTimeouts;
    return;
  }

  m_session->destroySwapchain();
  const auto surface = QVulkanInstance::surfaceForWindow(m_window.data());
  if (surface == VK_NULL_HANDLE)
  {
    fail("The preview's window has no Vulkan surface.");
    return;
  }

  // a 0×0 extent, as of a minimised window, makes no swapchain; the next expose tries
  // again
  m_session->createSwapchain(
    surface, std::uint32_t(size.width()), std::uint32_t(size.height()))
    | kdl::transform([&]() {
        m_swapchainSize = size;
        m_swapchainStale = false;
        m_session->unpark();
        m_parkPending = false;
      })
    | kdl::transform_error([&](const auto& e) { fail(e.msg); });
}

bool LitSessionHost::parkServicingSentMessages()
{
  const auto busy = BusyScope{*this};
  const auto deadline = std::chrono::steady_clock::now() + lit::ParkTimeout;
  while (!m_session->park(ServicingInterval))
  {
    if (std::chrono::steady_clock::now() >= deadline)
    {
      return false;
    }
    // R5.18: a present may wait for a message that the window system sends to this thread
    serviceSentMessages();
  }
  return true;
}

void LitSessionHost::stopRenderThread()
{
  const auto busy = BusyScope{*this};

  // what the park finds, a lost device for example, goes into the report (review V.4)
  m_session->beginStop();
  // parked first, so that the render thread is not inside a present that waits for this
  // thread's messages; one that does not park in time is stopped all the same
  parkServicingSentMessages();

  m_session->requestStop();
  const auto start = std::chrono::steady_clock::now();
  while (!m_session->waitForRenderThread(ServicingInterval))
  {
    serviceSentMessages();
    if (!m_stopOverran && std::chrono::steady_clock::now() - start >= StopWarningTime)
    {
      // never detached and never abandoned: it runs on this session's objects
      m_stopOverran = true;
      qWarning(
        "Lit preview: the render thread has not stopped after %lld ms; still waiting",
        static_cast<long long>(StopWarningTime.count()));
    }
  }
  m_session->stopRenderThread();
}

void LitSessionHost::afterBusy()
{
  if (m_abandoned)
  {
    deleteLater();
    return;
  }

  // from the event loop, outside the wait that deferred them
  if (m_screenChangeDeferred)
  {
    m_screenChangeDeferred = false;
    QMetaObject::invokeMethod(this, [this] { screenChanged(); }, Qt::QueuedConnection);
  }
  if (m_reconcileDeferred)
  {
    m_reconcileDeferred = false;
    QMetaObject::invokeMethod(this, [this] { reconcile(); }, Qt::QueuedConnection);
  }
}

lit::EventSink LitSessionHost::eventSink()
{
  return [queue = eventQueue()](
           const lit::SessionEvent event,
           std::string message,
           const std::uint64_t generation) {
    static_cast<void>(queue(event, std::move(message), generation));
  };
}

std::function<bool(lit::SessionEvent, std::string, std::uint64_t)> LitSessionHost::
  eventQueue()
{
  // Called on the session's threads, and on this thread inside create, createSwapchain
  // and the teardown steps; always queued to this host, which outlives the session's
  // threads, so that no message runs inside the call that sent it. The host's owner sees
  // a message only while it still owns the host (R5.17).
  return [this, undelivered = m_undelivered](
           const lit::SessionEvent event,
           std::string message,
           const std::uint64_t generation) {
    try
    {
      // review R.1: a warning, error or loss that a Stop overtakes reaches the report
      const auto tracked = event == lit::SessionEvent::Warning
                           || event == lit::SessionEvent::Error
                           || event == lit::SessionEvent::DeviceLost;
      const auto id =
        tracked ? std::optional{undelivered->add(event, message)} : std::nullopt;
      QMetaObject::invokeMethod(
        this,
        [this, event, queuedMessage = std::move(message), generation, id, undelivered] {
          if (id)
          {
            undelivered->remove(*id);
          }
          sessionEventReceived(generation, event, queuedMessage);
        },
        Qt::QueuedConnection);
      return true;
    }
    catch (...)
    {
      // a message that cannot be queued is lost, not the session
      return false;
    }
  };
}

void LitSessionHost::sessionEventReceived(
  const std::uint64_t generation,
  const lit::SessionEvent event,
  const std::string& message)
{
  if (generation != m_generation || m_abandoned)
  {
    return;
  }

  // in Qt's event loop, which an exception must not reach (review L3.18)
  try
  {
    dispatchSessionEvent(event, message);
  }
  catch (const std::exception& e)
  {
    qWarning("Lit preview: a session message could not be handled: %s", e.what());
  }
}

void LitSessionHost::dispatchSessionEvent(
  const lit::SessionEvent event, const std::string& message)
{
  switch (event)
  {
  case lit::SessionEvent::Parked:
    reconcile();
    break;
  case lit::SessionEvent::ReadyForSnapshot:
    // S31, B0.3: the inbox is empty
    if (m_observer)
    {
      m_observer->readyForSnapshot();
    }
    break;
  case lit::SessionEvent::SwapchainOutOfDate:
    // a request for a generation that is gone already was answered (review V.2)
    if (drawing() && m_session->swapchainReplacementRequested())
    {
      m_swapchainStale = true;
      reconcile();
    }
    break;
  case lit::SessionEvent::MaterialsPublished:
    // the next table posts again; the owner reads the latest one now
    m_publishPending->store(false);
    // R2.2: the render thread adopts it, also while it waits for nothing else
    if (drawing())
    {
      m_session->tableChanged();
    }
    if (m_eventHandler)
    {
      m_eventHandler(m_generation, event, message);
    }
    break;
  case lit::SessionEvent::Ready:
  case lit::SessionEvent::Warning:
  case lit::SessionEvent::MaterialInfo:
  case lit::SessionEvent::MaterialWarning:
  case lit::SessionEvent::DeviceLost:
  case lit::SessionEvent::Error:
    if (m_eventHandler)
    {
      m_eventHandler(m_generation, event, message);
    }
    break;
  }
}

QSize LitSessionHost::pixelSize() const
{
  return m_window->size() * m_window->devicePixelRatio();
}

void LitSessionHost::fail(std::string reason)
{
  if (!m_failed)
  {
    m_failed = true;
    // the owner stops the session from the event loop, never from inside this call
    eventSink()(lit::SessionEvent::Error, std::move(reason), m_generation);
  }
}

} // namespace tb::ui
