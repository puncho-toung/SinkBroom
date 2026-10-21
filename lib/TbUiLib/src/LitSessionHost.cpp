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
#include "ui/LitPreviewPanel.h"
#include "ui/LitPreviewWindow.h"

#include "kd/result.h"
#include "kd/set_temp.h"

#include <fmt/format.h>

#include <chrono>
#include <utility>

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

LitSessionHost::LitSessionHost(
  lit::SessionOptions options,
  const std::uint64_t generation,
  EventHandler eventHandler,
  LitPreviewDiagnostics& diagnostics)
  : m_options{std::move(options)}
  , m_generation{generation}
  , m_eventHandler{std::move(eventHandler)}
  , m_diagnostics{diagnostics}
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

  // called on whichever thread made the Vulkan call; the messages still go to Qt's log
  auto& diagnostics = m_diagnostics;
  m_instance->installDebugOutputFilter(
    [&diagnostics](
      const QVulkanInstance::DebugMessageSeverityFlags severity,
      QVulkanInstance::DebugMessageTypeFlags,
      const void*) {
      if (severity.testFlag(QVulkanInstance::ErrorSeverity))
      {
        ++diagnostics.validationErrors;
      }
      else if (severity.testFlag(QVulkanInstance::WarningSeverity))
      {
        ++diagnostics.validationWarnings;
      }
      return false;
    });

  if (!m_instance->create())
  {
    const auto errorCode = int(m_instance->errorCode());
    teardown();
    return Error{
      fmt::format("Cannot create a Vulkan instance (VkResult {}).", errorCode)};
  }

  m_diagnostics.validationLayer = m_instance->layers().contains(ValidationLayerName);
  m_handles.instance = m_instance->vkInstance();
  m_handles.getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
    m_instance->getInstanceProcAddr("vkGetInstanceProcAddr"));
  for (const auto& name : m_instance->extensions())
  {
    m_handles.enabledExtensions.push_back(name.toStdString());
  }
  // R4-2.22: on every Start, for this instance
  lit::initInstanceDispatcher(m_handles);
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
  return m_container.data();
}

bool LitSessionHost::drawing() const
{
  return m_session != nullptr && m_teardownStep == TeardownStep::None;
}

LitPreviewWindow* LitSessionHost::window() const
{
  return m_window.data();
}

QWidget* LitSessionHost::container() const
{
  return m_container.data();
}

std::uint64_t LitSessionHost::framesPresented() const
{
  return m_session ? m_session->framesPresented() : 0;
}

lit::LitSession* LitSessionHost::session() const
{
  return drawing() ? m_session.get() : nullptr;
}

void LitSessionHost::debugInjectDeviceLost()
{
  if (m_session)
  {
    m_session->debugInjectDeviceLost();
  }
}

void LitSessionHost::setCamera(const lit::CameraState& camera)
{
  m_camera = camera;
  if (drawing())
  {
    m_session->setCamera(camera);
  }
}

void LitSessionHost::windowChanged()
{
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
  runTeardownThrough(TeardownStep::DispatcherReset);
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
      if (m_session)
      {
        stopRenderThread();
      }
      break;
    case TeardownStep::GpuObjectsReleased:
      if (m_session)
      {
        m_session->releaseGpuObjects();
      }
      break;
    case TeardownStep::ContainerDeleted:
      // the window and Qt's VkSurfaceKHR go with it, while the instance is still alive
      delete m_container.data();
      break;
    case TeardownStep::VkDeviceDestroyed:
      if (m_session)
      {
        m_session->destroyVkDevice();
      }
      recordTeardown();
      m_session.reset();
      break;
    case TeardownStep::InstanceDestroyed:
      m_instance.reset();
      break;
    case TeardownStep::DispatcherReset:
      if (m_dispatcherInitialised)
      {
        lit::resetDispatcher();
      }
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
  }

  auto& d = m_diagnostics;
  ++d.teardowns;
  if (m_session)
  {
    ++d.sessionsWithDevice;
  }
  d.lastGeneration = m_generation;
  d.lastReachedPhaseB = m_session != nullptr;
  d.lastDeviceReleaseCount = m_session ? m_session->lastDeviceReleaseCount() : 0;
  d.lastNvrhiErrorCount = m_session ? m_session->nvrhiErrorCount() : 0;
  d.lastFramesPresented = m_session ? m_session->framesPresented() : 0;
  d.lastDeviceLost = m_session && m_session->deviceLost();
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
  if (
    exposed
    && (m_swapchainStale || !m_session->hasSwapchain() || size != m_swapchainSize))
  {
    replaceSwapchain(size);
    return;
  }

  // Nothing to replace, for example because the size came back before the render thread
  // parked: a park asked for earlier is released, or the preview would stay frozen.
  if (m_parkPending)
  {
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

  lit::LitSession::create(
    m_handles,
    surface,
    std::uint32_t(size.width()),
    std::uint32_t(size.height()),
    m_options,
    eventSink(),
    m_generation)
    | kdl::transform([&](std::unique_ptr<lit::LitSession> session) {
        m_session = std::move(session);
        m_swapchainSize = size;
        m_swapchainStale = false;
        m_session->setExposed(m_window->isExposed());
        if (m_camera)
        {
          m_session->setCamera(*m_camera);
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
  // Called on the session's threads, and on this thread inside create, createSwapchain
  // and the teardown steps; always queued to this host, which outlives the session's
  // threads, so that no message runs inside the call that sent it. The host's owner sees
  // a message only while it still owns the host (R5.17).
  return [this](
           const lit::SessionEvent event,
           std::string message,
           const std::uint64_t generation) {
    try
    {
      QMetaObject::invokeMethod(
        this,
        [this, event, queuedMessage = std::move(message), generation] {
          sessionEventReceived(generation, event, queuedMessage);
        },
        Qt::QueuedConnection);
    }
    catch (...)
    {
      // a message that cannot be queued is lost, not the session
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

  switch (event)
  {
  case lit::SessionEvent::Parked:
    reconcile();
    break;
  case lit::SessionEvent::SwapchainOutOfDate:
    if (drawing())
    {
      m_swapchainStale = true;
      reconcile();
    }
    break;
  case lit::SessionEvent::Ready:
  case lit::SessionEvent::Warning:
  case lit::SessionEvent::DeviceLost:
  case lit::SessionEvent::Error:
    if (m_eventHandler)
    {
      m_eventHandler(generation, event, message);
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
