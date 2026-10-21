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

#include "ui/LitPreviewPanel.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStackedLayout>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>

#include "LitSessionHost.h"
#include "base/Logger.h"
#include "base/PreferenceManager.h"
#include "gl/Camera.h"
#include "gl/PerspectiveCamera.h"
#include "lit/LitSession.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/Map.h"
#include "ui/LitPreviewWindow.h"
#include "ui/MapDocument.h"
#include "ui/QPathUtils.h"
#include "ui/ViewConstants.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <algorithm>
#include <filesystem>
#include <utility>

namespace tb::ui
{
namespace
{

using namespace std::chrono_literals;

// a Stop that arrives while the session host services the window system's messages is
// retried this often, from the event loop
constexpr auto DeferredStopInterval = 10ms;

// D53: the generation of the last session started in this process; GUI thread only
auto lastGeneration = std::uint64_t{0};

// D29: the panel that runs the process's one session; GUI thread only
struct SessionGuard
{
  const LitPreviewPanel* owner = nullptr;
  Notifier<> ownerDidChangeNotifier;
};

SessionGuard& sessionGuard()
{
  static SessionGuard guard;
  return guard;
}

/**
 * The options of a session, read once at Start: validation in Debug builds, and two
 * environment variables for tests and measurements, TB_LIT_FORCE_FIFO (any value other
 * than 0) and TB_LIT_FRAME_LOG (the frame log's path, C16).
 */
lit::SessionOptions sessionOptions()
{
  auto options = lit::SessionOptions{};
#ifndef NDEBUG
  options.validation = true;
#endif
  options.forceFifo = qEnvironmentVariableIsSet("TB_LIT_FORCE_FIFO")
                      && qEnvironmentVariable("TB_LIT_FORCE_FIFO") != QStringLiteral("0");
  if (const auto frameLog = qEnvironmentVariable("TB_LIT_FRAME_LOG"); !frameLog.isEmpty())
  {
    options.frameLogPath = pathFromQString(frameLog);
  }
  return options;
}

} // namespace

LitPreviewPanel::LitPreviewPanel(const SidebarContext& context, QWidget* parent)
  : SidebarPanel{parent}
  , m_document{context.document}
{
  createGui();
  connectObservers();

  m_notifierConnection += context.cameraDidChangeNotifier.connect(
    [&](const gl::Camera& camera) { cameraDidChange(camera); });

  updateStartPage();
}

std::vector<std::string> LitPreviewReport::problems() const
{
  auto result = std::vector<std::string>{};
  if (deviceReleaseCount != 0)
  {
    result.push_back(fmt::format(
      "The GPU device was still referenced {} times when it was released.",
      deviceReleaseCount));
  }
  if (nvrhiErrors != 0)
  {
    result.push_back(fmt::format("NVRHI reported {} errors.", nvrhiErrors));
  }
  if (frameLogError)
  {
    result.push_back(*frameLogError + ".");
  }
  for (const auto& message : messages)
  {
    result.push_back(message);
  }
  if (stopOverran)
  {
    result.push_back("The render thread took more than two seconds to stop.");
  }
  return result;
}

LitPreviewPanel::~LitPreviewPanel()
{
  // the guard's notification of this teardown reaches the other panels only, and nothing
  // here reads the document, which may be gone with the window
  m_notifierConnection.disconnect();
  if (const auto report = endSession())
  {
    sessionDidEndNotifier(*report);
  }
}

QString LitPreviewPanel::panelId() const
{
  return QString::fromLatin1(PanelId);
}

QString LitPreviewPanel::title() const
{
  return tr("Lit Preview");
}

void LitPreviewPanel::panelShown()
{
  // R5.26: another window's session may have stopped while this panel was out of sight
  updateStartPage();
}

void LitPreviewPanel::panelHidden()
{
  // a close by the user ends the session; a tab over the dock only hides the window,
  // which stops the frames
  stopSession({});
}

void LitPreviewPanel::setPrefabEditor(const bool prefabEditor)
{
  m_prefabEditor = prefabEditor;
  if (m_prefabEditor)
  {
    stopSession({});
  }
  else
  {
    updateStartPage();
  }
}

void LitPreviewPanel::start()
{
  if (m_host || !unavailableReason().isEmpty())
  {
    return;
  }

  // the host queues the session's messages to itself and passes them on while this panel
  // owns it; the generation is checked again here (R5.17)
  auto eventHandler = [this](
                        const std::uint64_t generation,
                        const lit::SessionEvent event,
                        const std::string& message) {
    sessionEventReceived(generation, event, message);
  };

  auto host = std::make_unique<LitSessionHost>(
    sessionOptions(), ++lastGeneration, std::move(eventHandler), diagnostics());
  host->start(*m_surfacePage) | kdl::transform([&](QWidget* container) {
    m_surfacePage->layout()->addWidget(container);
    m_host = std::move(host);
    m_stopReason.clear();
    m_stopProblems.clear();
    forwardCamera();
    claimSession(*this);
  }) | kdl::transform_error([&](const auto& e) {
    host.reset();
    m_stopReason = QString::fromStdString(e.msg);
  });

  // showing the surface page shows the window; its first expose runs phase B
  updateStartPage();
}

void LitPreviewPanel::stop()
{
  stopSession({});
}

QString LitPreviewPanel::unavailableReason() const
{
  if (!mdl::hasLightEditor(m_document.map().gameInfo().gameConfig))
  {
    return tr("The lit preview needs an idTech 4 game.");
  }
  if (m_prefabEditor)
  {
    return tr("The lit preview is not available in a prefab editor.");
  }
  if (const auto* owner = sessionOwner(); owner && owner != this)
  {
    return tr("Another window already runs the lit preview.");
  }
  return {};
}

const LitPreviewPanel* LitPreviewPanel::sessionOwner()
{
  return sessionGuard().owner;
}

bool LitPreviewPanel::claimSession(const LitPreviewPanel& panel)
{
  auto& guard = sessionGuard();
  if (guard.owner && guard.owner != &panel)
  {
    return false;
  }
  if (!guard.owner)
  {
    guard.owner = &panel;
    guard.ownerDidChangeNotifier();
  }
  return true;
}

void LitPreviewPanel::releaseSession(const LitPreviewPanel& panel)
{
  auto& guard = sessionGuard();
  if (guard.owner == &panel)
  {
    guard.owner = nullptr;
    guard.ownerDidChangeNotifier();
  }
}

Notifier<>& LitPreviewPanel::sessionOwnerDidChangeNotifier()
{
  return sessionGuard().ownerDidChangeNotifier;
}

bool LitPreviewPanel::sessionRunning() const
{
  return m_host != nullptr;
}

bool LitPreviewPanel::sessionDrawing() const
{
  return m_host && m_host->drawing();
}

std::uint64_t LitPreviewPanel::framesPresented() const
{
  return m_host ? m_host->framesPresented() : 0;
}

void LitPreviewPanel::debugInjectDeviceLost()
{
  if (m_host)
  {
    m_host->debugInjectDeviceLost();
  }
}

lit::LitSession* LitPreviewPanel::sessionForTesting() const
{
  return m_host ? m_host->session() : nullptr;
}

QWindow* LitPreviewPanel::surfaceWindow() const
{
  return m_host ? m_host->window() : nullptr;
}

QWidget* LitPreviewPanel::surfaceContainer() const
{
  return m_host ? m_host->container() : nullptr;
}

LitPreviewDiagnostics& LitPreviewPanel::diagnostics()
{
  // constant-initialised: it exists before the first Start without an allocation (C9)
  static LitPreviewDiagnostics diagnostics;
  return diagnostics;
}

void LitPreviewPanel::createGui()
{
  m_startButton = new QPushButton{tr("Start")};
  m_startButton->setObjectName("LitPreviewPanel_Start");
  m_stopButton = new QPushButton{tr("Stop")};
  m_stopButton->setObjectName("LitPreviewPanel_Stop");
  connect(m_startButton, &QPushButton::clicked, this, &LitPreviewPanel::start);
  connect(m_stopButton, &QPushButton::clicked, this, &LitPreviewPanel::stop);

  // Stop must be reachable while the surface page shows, so the buttons are outside the
  // pages
  auto* buttonLayout = new QHBoxLayout{};
  buttonLayout->setContentsMargins(
    LayoutConstants::WideHMargin,
    LayoutConstants::MediumVMargin,
    LayoutConstants::WideHMargin,
    LayoutConstants::MediumVMargin);
  buttonLayout->addWidget(m_startButton);
  buttonLayout->addWidget(m_stopButton);
  buttonLayout->addStretch(1);

  m_messageLabel = new QLabel{};
  m_messageLabel->setObjectName("LitPreviewPanel_Message");
  m_messageLabel->setAlignment(Qt::AlignCenter);
  m_messageLabel->setWordWrap(true);

  auto* startPageLayout = new QVBoxLayout{};
  startPageLayout->addWidget(m_messageLabel, 1);
  m_startPage = new QWidget{};
  m_startPage->setObjectName("LitPreviewPanel_StartPage");
  m_startPage->setLayout(startPageLayout);

  // the window container of a running session, added by start(); a stacked page that is
  // not current is hidden, and so is the contained window, which would otherwise stack
  // over every widget (R-B10a)
  auto* surfacePageLayout = new QVBoxLayout{};
  surfacePageLayout->setContentsMargins(0, 0, 0, 0);
  m_surfacePage = new QWidget{};
  m_surfacePage->setObjectName("LitPreviewPanel_SurfacePage");
  m_surfacePage->setLayout(surfacePageLayout);
  m_surfacePage->setMinimumSize(160, 120);

  m_stack = new QStackedLayout{};

  auto* layout = new QVBoxLayout{};
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  layout->addLayout(buttonLayout);
  layout->addLayout(m_stack, 1);
  setLayout(layout);

  // only now, so that the pages become the panel's children: a stack without a widget
  // shows its first page as a window of its own
  m_stack->addWidget(m_startPage);
  m_stack->addWidget(m_surfacePage);
}

void LitPreviewPanel::connectObservers()
{
  m_notifierConnection +=
    sessionGuard().ownerDidChangeNotifier.connect([&] { updateStartPage(); });

  // D41: the session's files and caches belong to the old document and game; a load may
  // also switch the game in the same window
  m_notifierConnection += m_document.documentWasLoadedNotifier.connect([&] {
    if (m_host)
    {
      stopSession(tr("A map was loaded. Press Start to preview it."));
    }
    else
    {
      updateStartPage();
    }
  });

  m_notifierConnection +=
    PreferenceManager::instance().preferenceDidChangeNotifier.connect(
      [&](const std::filesystem::path& path) {
        if (m_host && path == m_document.map().gameInfo().gamePathPreference.path)
        {
          stopSession(tr("The game path changed. Press Start to preview with it."));
        }
      });
}

void LitPreviewPanel::cameraDidChange(const gl::Camera& camera)
{
  // R4-2.18: the camera may be gone after this notification, so only its values are kept
  const auto& viewport = camera.viewport();
  const auto* perspectiveCamera = dynamic_cast<const gl::PerspectiveCamera*>(&camera);
  m_lastCamera = CameraCopy{
    camera.position(),
    camera.direction(),
    camera.up(),
    perspectiveCamera ? perspectiveCamera->zoomedFov() : 90.0f,
    viewport.width,
    viewport.height,
  };

  // R5.17: without a session nothing is passed on
  forwardCamera();
}

void LitPreviewPanel::forwardCamera()
{
  if (m_host && m_lastCamera)
  {
    const auto& camera = *m_lastCamera;
    m_host->setCamera(lit::CameraState{
      camera.position,
      camera.direction,
      camera.up,
      camera.fovDegrees,
      std::uint32_t(std::max(camera.viewportWidth, 0)),
      std::uint32_t(std::max(camera.viewportHeight, 0)),
    });
  }
}

void LitPreviewPanel::sessionEventReceived(
  const std::uint64_t generation,
  const lit::SessionEvent event,
  const std::string& message)
{
  // R5.17, D53: a message of a stopped session, or of an earlier one, is dropped
  if (!m_host || m_host->generation() != generation)
  {
    return;
  }

  auto& logger = m_document.logger();
  switch (event)
  {
  case lit::SessionEvent::Ready:
    logger.info() << message;
    break;
  case lit::SessionEvent::Parked:
  case lit::SessionEvent::SwapchainOutOfDate:
    // the host handles these itself
    break;
  case lit::SessionEvent::Warning:
    logger.warn() << "Lit preview: " << message;
    break;
  case lit::SessionEvent::DeviceLost:
  case lit::SessionEvent::Error:
    logger.error() << "Lit preview: " << message;
    stopSession(QString::fromStdString(message));
    break;
  }
}

void LitPreviewPanel::stopSession(QString reason)
{
  if (m_host && m_host->busy())
  {
    // the host waits inside one of its own frames and services the window system, which
    // brought this call here; it is torn down from the event loop instead
    QTimer::singleShot(
      DeferredStopInterval, this, [this, deferredReason = std::move(reason)]() mutable {
        stopSession(std::move(deferredReason));
      });
    return;
  }

  if (const auto report = endSession())
  {
    m_stopReason = std::move(reason);
    m_stopProblems.clear();
    auto& logger = m_document.logger();
    for (const auto& problem : report->problems())
    {
      logger.warn() << "Lit preview: " << problem;
      m_stopProblems +=
        (m_stopProblems.isEmpty() ? "" : " ") + QString::fromStdString(problem);
    }
    sessionDidEndNotifier(*report);
  }
  updateStartPage();
}

std::optional<LitPreviewReport> LitPreviewPanel::endSession()
{
  if (!m_host)
  {
    return std::nullopt;
  }

  auto report = std::optional<LitPreviewReport>{};
  if (m_host->busy())
  {
    // only from the destructor: the host ends itself once its wait is over
    m_host->abandon();
    static_cast<void>(m_host.release());
  }
  else
  {
    // R5.17: from here on, every message of this session is dropped; the teardown reports
    // what it finds here, synchronously
    auto host = std::move(m_host);
    report = host->teardown();
    host.reset();
  }
  releaseSession(*this);
  return report;
}

void LitPreviewPanel::updateStartPage()
{
  const auto reason = unavailableReason();
  m_startButton->setEnabled(!m_host && reason.isEmpty());
  m_stopButton->setEnabled(m_host != nullptr);
  const auto text = !reason.isEmpty() ? reason
                    : !m_stopReason.isEmpty()
                      ? m_stopReason
                      : tr("Press Start to see the map lit as the engine draws it.");
  m_messageLabel->setText(
    m_host || m_stopProblems.isEmpty()
      ? text
      : text + "\n" + tr("The last session did not stop cleanly: ") + m_stopProblems);
  m_stack->setCurrentWidget(m_host ? m_surfacePage : m_startPage);
}

} // namespace tb::ui
