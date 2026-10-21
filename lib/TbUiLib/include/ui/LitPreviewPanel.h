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

#include <QString>

#include "base/Notifier.h"
#include "base/NotifierConnection.h"
#include "ui/SidebarPanel.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QLabel;
class QPushButton;
class QStackedLayout;
class QWidget;

namespace tb::gl
{
class Camera;
}

namespace tb::lit
{
struct CameraState;
class LitSession;
class MaterialService;
enum class SessionEvent;
} // namespace tb::lit

namespace tb::ui
{
class LitSessionHost;
class MapDocument;

/**
 * What a lit preview session left when it stopped. The panel logs a report with problems
 * and shows them on its Start page, and passes every report to sessionDidEndNotifier.
 */
struct LitPreviewReport
{
  std::uint64_t generation = 0;
  // phase B ran: the session had a device
  bool hadDevice = false;
  // what the NVRHI device's last Release() returned; 0 expected
  std::uint32_t deviceReleaseCount = 0;
  std::size_t nvrhiErrors = 0;
  bool deviceLost = false;
  std::optional<std::string> frameLogError;
  // the warnings and errors raised while the session stopped
  std::vector<std::string> messages;
  // the render thread took longer than two seconds to stop; the stop waited on
  bool stopOverran = false;
  std::uint64_t framesPresented = 0;
  std::uint64_t swapchainGenerations = 0;
  // swapchain handshakes that had to wait for the render thread to park
  std::uint64_t parkTimeouts = 0;
  // the device-local VRAM in use when the session had its device and once its objects
  // were released, when the device reports it
  std::optional<std::uint64_t> vramAtStartBytes;
  std::optional<std::uint64_t> vramAtStopBytes;

  // what a user should know, one sentence each; empty after a clean stop
  std::vector<std::string> problems() const;
};

/**
 * The Lit Preview sidebar panel (§1, §3.1): the map lit as D3-Sink draws it, in a Vulkan
 * surface that follows the 3D view's camera. Nothing of the preview exists until Start,
 * and Stop, closing the panel or closing its window tears all of it down (C9, C18).
 *
 * A row with Start and Stop sits above two pages: the Start page, one line of text that
 * says why Start is unavailable or why the last session stopped, and the surface page
 * that holds a running session's window container. Start is unavailable for a game
 * without an idTech 4 light editor, in a prefab editor, and while another window runs
 * the process's one session (D29).
 *
 * Opening the panel never starts a session. Closing it stops one; a panel tabbed behind
 * another dock only stops drawing, because its window is no longer exposed. Loading a
 * document and changing the game path stop the session, whose files belong to the old
 * game (D41).
 */
class LitPreviewPanel : public SidebarPanel
{
  Q_OBJECT
public:
  static constexpr auto PanelId = "LitPreview";

private:
  MapDocument& m_document;
  bool m_prefabEditor = false;

  QPushButton* m_startButton = nullptr;
  QPushButton* m_stopButton = nullptr;
  QStackedLayout* m_stack = nullptr;
  QWidget* m_startPage = nullptr;
  QLabel* m_messageLabel = nullptr;
  QWidget* m_surfacePage = nullptr;

  // why the last session stopped, unless Stop or a close stopped it
  QString m_stopReason;
  // what the last session's teardown found wrong
  QString m_stopProblems;
  // a copy of the last camera the 3D view reported, never the camera itself, so that
  // phase B starts with it; the session host asks for it there
  std::unique_ptr<lit::CameraState> m_lastCamera;
  std::unique_ptr<LitSessionHost> m_host;

  NotifierConnection m_notifierConnection;

public:
  // after every session's teardown, also one that ended with the panel
  Notifier<const LitPreviewReport&> sessionDidEndNotifier;

  explicit LitPreviewPanel(const SidebarContext& context, QWidget* parent = nullptr);
  ~LitPreviewPanel() override;

  QString panelId() const override;
  QString title() const override;
  std::filesystem::path iconPath() const override;

  void panelShown() override;
  void panelHidden() override;

  /**
   * The panel of a prefab editor offers no Start (D29); MapWindow calls this once after
   * registering the panels, as it configures the prefab panel.
   */
  void setPrefabEditor(bool prefabEditor);

  /**
   * Phase A of Start (§3.1, D46): the instance, the window and its container, then the
   * surface page; phase B follows on the window's first expose. Does nothing while Start
   * is unavailable or a session runs.
   */
  void start();
  // Runs the session's teardown and shows the Start page.
  void stop();

  // Why Start is unavailable, or empty.
  QString unavailableReason() const;

  // the running session once phase B has run, else null
  lit::LitSession* sessionForTesting() const;
  // the running session's parse thread from phase A on, else null
  lit::MaterialService* materialsForTesting() const;
  /**
   * The process's one session is claimed by Start and released by the teardown or the
   * panel's destructor, and every change updates the Start pages of all panels (D29,
   * R5.26). Tests claim it for a panel without a GPU.
   */
  static bool claimSessionForTesting(const LitPreviewPanel& panel);
  static void releaseSessionForTesting(const LitPreviewPanel& panel);

private:
  void createGui();
  void connectObservers();

  void cameraDidChange(const gl::Camera& camera);
  void sessionEventReceived(
    std::uint64_t generation, lit::SessionEvent event, const std::string& message);

  /**
   * The teardown, then the Start page with the reason. With a generation, only that
   * session is stopped: a stop that was deferred never ends a later one (review V.7).
   */
  void stopSession(
    QString reason, std::optional<std::uint64_t> generation = std::nullopt);
  // stopSession from the event loop, never inside the host's own call (review V.5)
  void deferStopSession(QString reason, std::uint64_t generation);
  std::optional<LitPreviewReport> endSession();
  void updateStartPage();
};

} // namespace tb::ui
