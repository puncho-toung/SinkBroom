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

#include "vm/vec.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

class QLabel;
class QPushButton;
class QStackedLayout;
class QWidget;
class QWindow;

namespace tb::gl
{
class Camera;
}

namespace tb::lit
{
class LitSession;
enum class SessionEvent;
} // namespace tb::lit

namespace tb::ui
{
class LitSessionHost;
class MapDocument;

/**
 * What the lit preview's sessions leave behind, for the teardown tests of LP0: the
 * validation layer's messages of every session so far, counted on any thread, and what
 * the last session reported when its teardown ended (GUI thread). Nothing in it is
 * allocated, so it exists before the first Start without breaking C9.
 */
struct LitPreviewDiagnostics
{
  std::atomic<int> validationErrors = 0;
  std::atomic<int> validationWarnings = 0;
  // the last instance had the Khronos validation layer
  bool validationLayer = false;

  std::uint64_t teardowns = 0;
  // the sessions that reached phase B
  std::uint64_t sessionsWithDevice = 0;
  std::uint64_t lastGeneration = 0;
  // phase B ran: the session had a device
  bool lastReachedPhaseB = false;
  // what the NVRHI device's last Release() returned; 0 expected
  std::uint32_t lastDeviceReleaseCount = 0;
  std::size_t lastNvrhiErrorCount = 0;
  std::uint64_t lastFramesPresented = 0;
  bool lastDeviceLost = false;
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
  // what the panel copies from the 3D view's camera (§3.2); never the camera itself
  struct CameraCopy
  {
    vm::vec3f position;
    vm::vec3f direction;
    vm::vec3f up;
    float fovDegrees = 90.0f;
    int viewportWidth = 0;
    int viewportHeight = 0;
  };

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
  // the last camera the 3D view reported, so that phase B starts with it
  std::optional<CameraCopy> m_lastCamera;
  std::unique_ptr<LitSessionHost> m_host;

  NotifierConnection m_notifierConnection;

public:
  explicit LitPreviewPanel(const SidebarContext& context, QWidget* parent = nullptr);
  ~LitPreviewPanel() override;

  QString panelId() const override;
  QString title() const override;

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

  /**
   * The one-session guard (D29), GUI thread only: the panel that runs the process's
   * session, or null. Start claims it and teardown releases it; every change notifies, so
   * that the other windows' panels update their Start pages (R5.26).
   */
  static const LitPreviewPanel* sessionOwner();
  static bool claimSession(const LitPreviewPanel& panel);
  static void releaseSession(const LitPreviewPanel& panel);
  static Notifier<>& sessionOwnerDidChangeNotifier();

  // Test hooks

  // a session exists: phase A ran
  bool sessionRunning() const;
  // the session has its device: phase B ran
  bool sessionDrawing() const;
  // the running session's presented frames, 0 without one
  std::uint64_t framesPresented() const;
  // the running session's render thread throws a device loss at its next frame
  void debugInjectDeviceLost();
  // the running session once phase B has run, else null
  lit::LitSession* sessionForTesting() const;
  QWindow* surfaceWindow() const;
  QWidget* surfaceContainer() const;
  static LitPreviewDiagnostics& diagnostics();

private:
  void createGui();
  void connectObservers();

  void cameraDidChange(const gl::Camera& camera);
  void forwardCamera();
  void sessionEventReceived(
    std::uint64_t generation, lit::SessionEvent event, const std::string& message);

  // the teardown, then the Start page with the reason
  void stopSession(QString reason);
  void teardownSession();
  void updateStartPage();
};

} // namespace tb::ui
