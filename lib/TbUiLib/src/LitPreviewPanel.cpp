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

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QStackedLayout>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>

#include "LitSessionHost.h"
#include "base/Logger.h"
#include "base/Notifier.h"
#include "base/PreferenceManager.h"
#include "gl/Camera.h"
#include "gl/PerspectiveCamera.h"
#include "lit/LitSession.h"
#include "lit/MaterialService.h"
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

// D9 (LP2.3): how often the status line under the surface reads the session's state
constexpr auto StatusInterval = 250ms;

// the status line's widget and its timer, children of the panel found by name
constexpr auto StatusLabelName = "LitPreviewPanel_Status";
constexpr auto StatusTimerName = "LitPreviewPanel_StatusTimer";
// LP3.4 (D158): the debug-view choice beside the status line, its items in
// lit::DebugView's order
constexpr auto DebugViewName = "LitPreviewPanel_DebugView";

/**
 * D29: the panel that runs the process's one session, GUI thread only. Every change
 * notifies, so that every panel updates its Start page (R5.26).
 */
class SessionGuard
{
private:
  const LitPreviewPanel* m_owner = nullptr;

public:
  Notifier<> ownerDidChangeNotifier;

  const LitPreviewPanel* owner() const { return m_owner; }

  bool claim(const LitPreviewPanel& panel)
  {
    if (m_owner && m_owner != &panel)
    {
      return false;
    }
    if (!m_owner)
    {
      m_owner = &panel;
      ownerDidChangeNotifier();
    }
    return true;
  }

  void release(const LitPreviewPanel& panel)
  {
    if (m_owner == &panel)
    {
      m_owner = nullptr;
      ownerDidChangeNotifier();
    }
  }
};

SessionGuard& sessionGuard()
{
  static auto guard = SessionGuard{};
  return guard;
}

bool environmentFlag(const char* name)
{
  const auto value = qEnvironmentVariable(name).trimmed().toLower();
  return value == "1" || value == "true" || value == "yes";
}

/**
 * The options of a session, read once at Start: validation in Debug builds, and two
 * environment variables for tests and measurements: TB_LIT_FORCE_FIFO (1, true or yes)
 * and TB_LIT_FRAME_LOG (the frame log's absolute path, C16).
 */
lit::SessionOptions sessionOptions(Logger& logger)
{
  auto options = lit::SessionOptions{};
#ifndef NDEBUG
  options.validation = true;
#endif
  options.forceFifo = environmentFlag("TB_LIT_FORCE_FIFO");
  if (const auto frameLog = qEnvironmentVariable("TB_LIT_FRAME_LOG"); !frameLog.isEmpty())
  {
    // a relative path would depend on the working directory (review L3.16)
    if (const auto path = pathFromQString(frameLog); path.is_absolute())
    {
      options.frameLogPath = path;
    }
    else
    {
      logger.warn() << "Lit preview: TB_LIT_FRAME_LOG must be an absolute path; "
                       "no frame log is written";
    }
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
  // review R.1: also a loss whose message never reached the panel
  if (deviceLost)
  {
    result.push_back("The GPU device was lost.");
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
  // also when the guard was claimed without a session, so that it never names a panel
  // that is gone (review L2.13)
  sessionGuard().release(*this);
}

QString LitPreviewPanel::panelId() const
{
  return QString::fromLatin1(PanelId);
}

QString LitPreviewPanel::title() const
{
  return tr("Lit Preview");
}

std::filesystem::path LitPreviewPanel::iconPath() const
{
  return "LitPreview.svg";
}

void LitPreviewPanel::panelShown()
{
  // R5.26: another window's session may have stopped while this panel was out of sight
  updateStartPage();
  // S31: what the document observer coalesced while the panel was hidden
  if (m_host)
  {
    m_host->panelShown();
  }
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

  // phase B starts from the last camera the 3D view reported
  auto camera = [this]() -> std::optional<lit::CameraState> {
    return m_lastCamera ? std::optional{*m_lastCamera} : std::nullopt;
  };

  // C1: the material service reads the game the document's map belongs to; the host's
  // document observer watches the document for the session
  auto host = std::make_unique<LitSessionHost>(
    sessionOptions(m_document.logger()),
    ++lastGeneration,
    std::move(eventHandler),
    std::move(camera),
    m_document,
    m_document.map().gamePath());
  host->start(*m_surfacePage) | kdl::transform([&](QWidget* container) {
    // above the status line, taking the page's height
    static_cast<QVBoxLayout*>(m_surfacePage->layout())->insertWidget(0, container, 1);
    m_host = std::move(host);
    m_materialsVersion = 0;
    m_stopReason.clear();
    m_stopProblems.clear();
    sessionGuard().claim(*this);
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
  if (const auto* owner = sessionGuard().owner(); owner && owner != this)
  {
    return tr("Another window already runs the lit preview.");
  }
  return {};
}

lit::LitSession* LitPreviewPanel::sessionForTesting() const
{
  return m_host ? m_host->session() : nullptr;
}

lit::MaterialService* LitPreviewPanel::materialsForTesting() const
{
  return m_host ? m_host->materials() : nullptr;
}

std::uint64_t LitPreviewPanel::materialsVersionForTesting() const
{
  return m_materialsVersion;
}

bool LitPreviewPanel::claimSessionForTesting(const LitPreviewPanel& panel)
{
  return sessionGuard().claim(panel);
}

void LitPreviewPanel::releaseSessionForTesting(const LitPreviewPanel& panel)
{
  sessionGuard().release(panel);
}

void LitPreviewPanel::createGui()
{
  // a click leaves the keyboard where it was, with the 3D view; disabling a button that
  // had the focus would move it to the next widget in the tab chain (review L3.12)
  m_startButton = new QPushButton{tr("Start")};
  m_startButton->setObjectName("LitPreviewPanel_Start");
  m_startButton->setFocusPolicy(Qt::TabFocus);
  m_stopButton = new QPushButton{tr("Stop")};
  m_stopButton->setObjectName("LitPreviewPanel_Stop");
  m_stopButton->setFocusPolicy(Qt::TabFocus);
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

  // D9 (LP2.3): one line under the surface: the session's state (settled or drawing),
  // the lit passes' counts, the VRAM and the disabled programs with their reasons;
  // start() puts the window container above it. One line of a fixed height whose width
  // is ignored: a new text never resizes the surface (which would draw frames); the
  // tooltip holds the whole line
  auto* statusLabel = new QLabel{};
  statusLabel->setObjectName(StatusLabelName);
  statusLabel->setWordWrap(false);
  statusLabel->setContentsMargins(
    LayoutConstants::WideHMargin,
    LayoutConstants::NarrowVMargin,
    LayoutConstants::WideHMargin,
    LayoutConstants::NarrowVMargin);
  statusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  statusLabel->setFixedHeight(
    statusLabel->fontMetrics().height() + 2 * LayoutConstants::NarrowVMargin);
  statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  // LP3.4 (D158, R5.15): the shadow debug views, a combo box beside the line, of the
  // line's height (the line's own height unchanged): no view by default, the atlas, or
  // the atlas with its tiles tinted by age; the session takes the choice at once and the
  // status timer hands it to a session started later
  auto* debugView = new QComboBox{};
  debugView->setObjectName(DebugViewName);
  debugView->addItem(tr("No debug view"));
  debugView->addItem(tr("Shadow atlas"));
  debugView->addItem(tr("Shadow tiles by age"));
  debugView->setCurrentIndex(0);
  debugView->setToolTip(tr("Draw a shadow debug view over the lit preview"));
  debugView->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  debugView->setFixedHeight(statusLabel->maximumHeight());
  const auto chosenView = [debugView] {
    return lit::DebugView(std::clamp(debugView->currentIndex(), 0, 2));
  };
  connect(debugView, &QComboBox::currentIndexChanged, this, [this, chosenView] {
    if (auto* session = m_host ? m_host->session() : nullptr)
    {
      session->setDebugView(chosenView());
    }
  });
  auto* statusLayout = new QHBoxLayout{};
  statusLayout->setContentsMargins(0, 0, 0, 0);
  statusLayout->setSpacing(0);
  statusLayout->addWidget(statusLabel, 1);
  statusLayout->addWidget(debugView);
  surfacePageLayout->addLayout(statusLayout);
  auto* statusTimer = new QTimer{this};
  statusTimer->setObjectName(StatusTimerName);
  statusTimer->setInterval(StatusInterval);
  connect(statusTimer, &QTimer::timeout, this, [this, statusLabel, chosenView] {
    auto* session = m_host ? m_host->session() : nullptr;
    // LP3.4 (D158): a session started after the choice draws it too
    if (session && session->debugView() != chosenView())
    {
      session->setDebugView(chosenView());
    }
    // LP4.3 (D90, K8): the host's status adds the document's unsaved changes and its
    // file's time, which the bake's stale notice reads
    const auto text = session  ? QString::fromStdString(lit::sessionStatusLine(
                                  m_host->status(), session->programReport()))
                      : m_host ? tr("starting")
                               : QString{};
    if (text != statusLabel->text())
    {
      statusLabel->setText(text);
      statusLabel->setToolTip(text);
    }
  });

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
  if (!m_lastCamera)
  {
    m_lastCamera = std::make_unique<lit::CameraState>();
  }
  *m_lastCamera = lit::CameraState{
    camera.position(),
    camera.direction(),
    camera.up(),
    perspectiveCamera ? perspectiveCamera->zoomedFov() : 90.0f,
    std::uint32_t(std::max(viewport.width, 0)),
    std::uint32_t(std::max(viewport.height, 0)),
  };

  // R5.17: without a session nothing is passed on
  if (m_host)
  {
    m_host->setCamera(*m_lastCamera);
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
  case lit::SessionEvent::ReadyForSnapshot:
    // the host handles these itself; ReadyForSnapshot goes to its document observer
    break;
  case lit::SessionEvent::MaterialsPublished:
    // the latest table, which the readers of LP1b and LP1c take from here
    if (auto* materials = m_host->materials())
    {
      m_materialsVersion = materials->table()->version;
    }
    break;
  case lit::SessionEvent::Warning:
    logger.warn() << "Lit preview: " << message;
    break;
  case lit::SessionEvent::MaterialInfo:
    logger.info() << "Lit preview materials: " << message;
    break;
  case lit::SessionEvent::MaterialWarning:
    // R4-1.17: the copied parser's warnings, as the engine's console shows them
    logger.warn() << "Lit preview materials: " << message;
    break;
  case lit::SessionEvent::DeviceLost:
  case lit::SessionEvent::Error:
    logger.error() << "Lit preview: " << message;
    // this runs inside the host's own event handler, which the teardown would delete
    deferStopSession(QString::fromStdString(message), generation);
    break;
  }
}

void LitPreviewPanel::deferStopSession(QString reason, const std::uint64_t generation)
{
  QTimer::singleShot(
    0, this, [this, generation, deferredReason = std::move(reason)]() mutable {
      stopSession(std::move(deferredReason), generation);
    });
}

void LitPreviewPanel::stopSession(
  QString reason, const std::optional<std::uint64_t> generation)
{
  if (!m_host || (generation && m_host->generation() != *generation))
  {
    // the session this stop was meant for is gone
    if (!generation)
    {
      updateStartPage();
    }
    return;
  }

  if (m_host->busy())
  {
    // the host waits inside one of its own frames and services the window system, which
    // brought this call here; it is torn down from the event loop instead
    QTimer::singleShot(
      DeferredStopInterval,
      this,
      [this,
       hostGeneration = m_host->generation(),
       deferredReason = std::move(reason)]() mutable {
        stopSession(std::move(deferredReason), hostGeneration);
      });
    return;
  }

  // a panel deleted from a message serviced inside the teardown is not touched again
  const auto self = QPointer<LitPreviewPanel>{this};
  const auto report = endSession();
  if (!self)
  {
    return;
  }
  if (report)
  {
    m_stopReason = std::move(reason);
    m_stopProblems.clear();
    auto& logger = m_document.logger();
    if (report->hadDevice)
    {
      const auto megabytes = [](const std::optional<std::uint64_t>& bytes) {
        return bytes ? fmt::format("{:.1f} MB", double(*bytes) / (1024.0 * 1024.0))
                     : std::string{"unknown"};
      };
      logger.info() << fmt::format(
        "Lit preview stopped after {} frames; VRAM in use: {} at start, {} at stop",
        report->framesPresented,
        megabytes(report->vramAtStartBytes),
        megabytes(report->vramAtStopBytes));
    }
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
  // R5.24: the session's document subscriptions end with its observer, in the teardown
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
    const auto self = QPointer<LitPreviewPanel>{this};
    report = host->teardown();
    host.reset();
    if (!self)
    {
      return report;
    }
  }
  sessionGuard().release(*this);
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

  // D9 (LP2.3): the status line reads a running session only
  if (auto* statusTimer = findChild<QTimer*>(StatusTimerName))
  {
    if (m_host && !statusTimer->isActive())
    {
      statusTimer->start();
    }
    else if (!m_host)
    {
      statusTimer->stop();
      if (auto* statusLabel = findChild<QLabel*>(StatusLabelName))
      {
        statusLabel->clear();
        statusLabel->setToolTip({});
      }
    }
  }
}

} // namespace tb::ui
