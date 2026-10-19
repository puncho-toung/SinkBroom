/*
 Copyright (C) 2010 Kristian Duske

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

#include "ui/MapWindow.h"

#include <QApplication>
#include <QChildEvent>
#include <QClipboard>
#include <QComboBox>
#include <QDataStream>
#include <QDockWidget>
#include <QFileDialog>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QStatusBar>
#include <QString>
#include <QStringList>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
#include <QVariant>
#include <QtGlobal>

#include "base/PreferenceManager.h"
#include "gl/GlManager.h"
#include "gl/ResourceManager.h"
#include "mdl/Autosaver.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityNodeBase.h"
#include "mdl/EnvironmentConfig.h"
#include "mdl/ExportOptions.h"
#include "mdl/GameInfo.h"
#include "mdl/Grid.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/Map_Assets.h"
#include "mdl/Map_Brushes.h"
#include "mdl/Map_CopyPaste.h"
#include "mdl/Map_Entities.h"
#include "mdl/Map_Geometry.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_NodeVisibility.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Patches.h"
#include "mdl/Map_Selection.h"
#include "mdl/ModelUtils.h"
#include "mdl/Node.h"
#include "mdl/NodeContents.h"
#include "mdl/PasteType.h"
#include "mdl/PatchNode.h"
#include "mdl/PrefabFile.h"
#include "mdl/PrefabLibrary.h"
#include "mdl/Transaction.h"
#include "mdl/VisualEffect.h"
#include "mdl/WorldNode.h"
#include "prefs/Preferences.h"
#include "ui/Action.h"
#include "ui/ActionBuilder.h"
#include "ui/ActionExecutionContext.h"
#include "ui/ActionManager.h"
#include "ui/AppController.h"
#include "ui/ChoosePathTypeDialog.h"
#include "ui/ClipTool.h"
#include "ui/ColorButton.h"
#include "ui/CompilationDialog.h"
#include "ui/Console.h"
#include "ui/CrashReporter.h"
#include "ui/CreatePrefabTool.h"
#include "ui/EdgeTool.h"
#include "ui/EntityInspector.h"
#include "ui/FaceInspector.h"
#include "ui/FaceTool.h"
#include "ui/IssueBrowser.h"
#include "ui/LaunchGameEngineDialog.h"
#include "ui/MapDocument.h"
#include "ui/MapInspector.h"
#include "ui/MapView2D.h"
#include "ui/MapView3D.h"
#include "ui/MapViewBase.h"
#include "ui/MapViewToolBox.h"
#include "ui/MapWindowManager.h"
#include "ui/ObjExportDialog.h"
#include "ui/PrefabPanel.h"
#include "ui/QPathUtils.h"
#include "ui/QStringUtils.h"
#include "ui/QStyleUtils.h"
#include "ui/QVecUtils.h"
#include "ui/RecentDocuments.h"
#include "ui/RenderView.h"
#include "ui/ReplaceMaterialDialog.h"
#include "ui/SidebarManager.h"
#include "ui/SidebarPanel.h"
#include "ui/SidebarPanels.h"
#include "ui/SignalDelayer.h"
#include "ui/SwitchableMapViewContainer.h"
#include "ui/ToolSidebar.h"
#include "ui/VertexTool.h"
#include "ui/ViewUtils.h"
#include "ui/WadUtils.h"
#include "ui/WidgetState.h"
#include "update/Updater.h"

#include "kd/const_overload.h"
#include "kd/contracts.h"
#include "kd/k.h"
#include "kd/overload.h"
#include "kd/path_utils.h"
#include "kd/ranges/to.h"
#include "kd/string_format.h"
#include "kd/string_utils.h"

#include "vm/vec.h"
#include "vm/vec_io.h"

#include <fmt/format.h>
#include <fmt/std.h>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>


namespace tb::ui
{
namespace
{

void showModelessDialog(QDialog* dialog)
{
  // https://doc.qt.io/qt-5/qdialog.html#code-examples
  dialog->show();
  dialog->raise();
  dialog->activateWindow();
}

bool widgetOrChildHasFocus(const QWidget* widget)
{
  contract_pre(widget != nullptr);

  const auto* focusWidget = QApplication::focusWidget();
  return widget == focusWidget || widget->isAncestorOf(focusWidget);
}

QString lastCompilationProfileSettingsKey(const std::string& gameName)
{
  return QString::fromLatin1("Compilation/LastProfile/%1")
    .arg(QString::fromStdString(gameName));
}

/**
 * The version of the dock layout that saveWindowState writes next to the window state. A
 * saved state of an older version lacks docks that restoreWindowState then arranges once:
 * 1 = the Map, Entity and Face inspectors are docks; 2 = the Console and Issues panels
 * are docks.
 */
constexpr auto DockLayoutVersion = 2;

/**
 * The suffix of the dock layout version's settings key, beside the window's State.
 */
constexpr auto DockLayoutVersionKey = "DockLayoutVersion";

/**
 * The longer side of a prefab thumbnail at most: the prefab panel's cells show it at up
 * to 128 pixels, and this leaves room for a larger cell or a high-DPI screen.
 */
constexpr auto ThumbnailSize = 256;

/**
 * The inspector panels in the order of their tabs.
 */
std::vector<QString> inspectorPanelIds()
{
  return {
    QString::fromLatin1(MapInspector::PanelId),
    QString::fromLatin1(EntityInspector::PanelId),
    QString::fromLatin1(FaceInspector::PanelId),
  };
}

/**
 * The panels of the old info panel in the order of their tabs.
 */
std::vector<QString> infoPanelIds()
{
  return {
    QString::fromLatin1(Console::PanelId),
    QString::fromLatin1(IssueBrowser::PanelId),
  };
}

QDockWidget* findSidebarDock(const SidebarManager& sidebarManager, const QString& panelId)
{
  // a dock is the parent of the panel it shows
  auto* panel = sidebarManager.panel(panelId);
  return panel ? qobject_cast<QDockWidget*>(panel->parentWidget()) : nullptr;
}

QDockWidget* sidebarDock(const SidebarManager& sidebarManager, const QString& panelId)
{
  auto* dock = findSidebarDock(sidebarManager, panelId);
  contract_assert(dock != nullptr);
  return dock;
}

/**
 * The page that a tab book of a layout saved before its pages were docks showed, read
 * from the given key: the inspector's (Map, Entity, Face) or the info panel's (Console,
 * Issues).
 */
QString oldTabBookPage(const QString& key, const std::vector<QString>& panelIds)
{
  // TabBook::saveState wrote the index of the current page as one int; the first page
  // when the key is absent or unreadable
  const auto state = QSettings{}.value(key).toByteArray();
  auto stream = QDataStream{state};
  auto page = 0;
  stream >> page;

  return stream.status() == QDataStream::Ok && page >= 0 && page < int(panelIds.size())
           ? panelIds[size_t(page)]
           : panelIds.front();
}

/**
 * Whether the inspector or the info panel of a layout saved before their pages were docks
 * was visible, read from the given key.
 */
bool oldPanelVisible(const QString& key)
{
  // Inspector::saveState and InfoPanel::saveState wrote whether the panel was visible as
  // one bool; visible when the key is absent or unreadable
  const auto state = QSettings{}.value(key).toByteArray();
  auto stream = QDataStream{state};
  auto visible = true;
  stream >> visible;
  return stream.status() != QDataStream::Ok || visible;
}

/**
 * R1-pf3-1: a prefab editor saves and restores its own geometry and layout under this
 * name, not under the map windows' "MapWindow".
 */
const auto PrefabEditorObjectName = QString{"PrefabEditorWindow"};

using SettingsValues = std::vector<std::pair<QString, QVariant>>;

bool isPrefabEditorKey(const QString& key)
{
  return key.startsWith(PrefabEditorObjectName + "/");
}

/**
 * The saved state of the windows' widgets, but for the prefab editor's own. The panels
 * and splitters of every window save theirs under their own names when they are deleted,
 * so a prefab editor puts back what the map windows had saved once its own are gone.
 */
SettingsValues sharedWidgetState()
{
  auto settings = QSettings{};
  settings.beginGroup("Windows");
  auto values = SettingsValues{};
  for (const auto& key : settings.allKeys())
  {
    if (!isPrefabEditorKey(key))
    {
      values.emplace_back(key, settings.value(key));
    }
  }
  return values;
}

void restoreSharedWidgetState(const SettingsValues& values)
{
  auto settings = QSettings{};
  settings.beginGroup("Windows");
  for (const auto& key : settings.allKeys())
  {
    if (!isPrefabEditorKey(key))
    {
      settings.remove(key);
    }
  }
  for (const auto& [key, value] : values)
  {
    settings.setValue(key, value);
  }
}

/**
 * The one entity that the body of an entity snapshot holds, or nullptr if it holds
 * anything else.
 */
const mdl::EntityNode* singleBodyEntity(mdl::WorldNode& worldNode)
{
  auto bodyNodes = std::vector<const mdl::Node*>{};
  for (const auto* layerNode : worldNode.allLayers())
  {
    for (const auto* node : layerNode->children())
    {
      bodyNodes.push_back(node);
    }
  }
  return bodyNodes.size() == 1 ? dynamic_cast<const mdl::EntityNode*>(bodyNodes.front())
                               : nullptr;
}

} // namespace

using namespace std::chrono_literals;

MapWindow::MapWindow(
  AppController& appController,
  std::unique_ptr<MapDocument> document,
  const bool prefabEditor)
  : m_appController{appController}
  , m_document{std::move(document)}
  , m_prefabEditor{prefabEditor}
  , m_lastInputTime{std::chrono::system_clock::now()}
  , m_autosaveTimer{new QTimer{this}}
  , m_updateTitleSignalDelayer{new SignalDelayer{500ms, this}}
  , m_updateActionStateSignalDelayer{new SignalDelayer{this}}
  , m_updateStatusBarSignalDelayer{new SignalDelayer{500ms, this}}
{
  contract_pre(m_document != nullptr);

  setAttribute(Qt::WA_DeleteOnClose);
  // R1-pf3-1: a prefab editor's layout is its own
  setObjectName(m_prefabEditor ? PrefabEditorObjectName : QString{"MapWindow"});

  installEventFilter(this);

  createGui();
  createMenus();
  createToolBar();
  createToolSidebar();
  createStatusBar();

  updateShortcuts();
  updateActionState();
  updateUndoRedoActions();
  updateToolBarWidgets();
  loadLastCompilationProfileName();

  m_document->setTargetLogger(m_console);

  // H-PF3-3: the backups of a prefab would land in an autosave folder in the library,
  // which lists them as prefabs
  if (!m_prefabEditor)
  {
    m_autosaveTimer->start(1000);
  }

  connectObservers();
  bindEvents();

  restoreWidgetGeometry(this);
  restoreWindowState();

  setAcceptDrops(true);

  // act as if the document was loaded to update the UI
  m_document->documentWasLoadedNotifier();
}

MapWindow::~MapWindow()
{
  // R1-pf3-1: put back below what this prefab editor's widgets replace
  const auto sharedState =
    m_prefabEditor ? std::optional{sharedWidgetState()} : std::nullopt;

  disconnect(qApp, &QApplication::focusChanged, this, &MapWindow::focusChange);

  // Stop the autosave timer
  m_autosaveTimer->stop();

  // Search for a RenderView (QOpenGLWindow subclass) and make it current in order to
  // allow for calling OpenGL methods in destructors.
  auto* renderView = m_mapView->findChild<RenderView*>();
  if (renderView)
  {
    renderView->makeCurrent();
  }

  // The MapDocument's CachingLogger has a pointer to m_console, which
  // is about to be destroyed with its dock below. Clear the pointer
  // so we don't try to log to a dangling pointer (#1885).
  m_document->setTargetLogger(nullptr);

  m_mapView->toolBox().deactivateCurrentTool();

  // Delete the sidebar docks, and with them their panels, while the document and the tool
  // box that the panels observe still exist, and before the snapshot of the children
  // below, which deletes a child twice if one of its siblings destroys it first. They are
  // hidden first: deleting the second to last dock of a floating tab group makes Qt float
  // the last one on its own and show it again unless it is hidden, which would create a
  // GL context for a panel that is about to go. Hiding them only queues an action state
  // update on a child of this window, which goes with it.
  const auto docks = m_sidebarManager->docks();
  for (auto* dock : docks)
  {
    dock->hide();
  }
  for (auto* dock : docks)
  {
    delete dock;
  }

  m_notifierConnection.disconnect();
  removeRecentDocumentsMenu();

  // The order of deletion here is important because both the document and the children
  // need the context manager (and its embedded VBO) to clean up their resources.

  // Destroy the children first because they might still access document resources.
  // The children must be deleted in reverse order! Not the tab bars of the dock tab
  // groups, though: the main window's layout refers to them until it is deleted and shows
  // them whenever a child that it lays out is removed or one of their geometry animations
  // ends, which deleting a tab bar does to its own, so deleting them here would leave it
  // with dangling tab bars. QWidget's destructor deletes them after the layout.
  auto children = this->children();
  children.removeIf(
    [](const auto* child) { return qobject_cast<const QTabBar*>(child) != nullptr; });
  qDeleteAll(std::rbegin(children), std::rend(children));

  if (sharedState)
  {
    restoreSharedWidgetState(*sharedState);
  }

  // let's trigger a final autosave before releasing the document; a prefab editor makes
  // no backups (H-PF3-3)
  if (!m_prefabEditor)
  {
    m_document->triggerAutosave();
  }
  m_document.reset();

  // FIXME: m_contextManager is deleted via smart pointer; it may release openGL resources
  // in its destructor
}

void MapWindow::saveWindowState()
{
  saveWidgetGeometry(this);
  saveWidgetState(this);

  // the saved state knows every dock, so the next restore arranges none of them
  QSettings{}.setValue(widgetSettingsPath(this, DockLayoutVersionKey), DockLayoutVersion);
}

bool MapWindow::isPrefabEditor() const
{
  return m_prefabEditor;
}

void MapWindow::positionOnScreen(QWidget* reference)
{
  restoreWidgetGeometry(this);
  restoreWindowState();
  if (reference)
  {
    const auto offset = QApplication::style()->pixelMetric(QStyle::PM_TitleBarHeight);
    move(reference->pos() + QPoint(offset, offset));
  }
}

const MapDocument& MapWindow::document() const
{
  return *m_document;
}

MapDocument& MapWindow::document()
{
  return KDL_CONST_OVERLOAD(document());
}

const MapViewToolBox& MapWindow::toolBox() const
{
  return m_mapView->toolBox();
}

MapViewToolBox& MapWindow::toolBox()
{
  return KDL_CONST_OVERLOAD(toolBox());
}

Logger& MapWindow::logger() const
{
  return *m_console;
}

QAction* MapWindow::findAction(const std::filesystem::path& path)
{
  const auto& actionManager = m_appController.actionManager();
  auto& actionsMap = actionManager.actionsMap();
  if (const auto iAction = actionsMap.find(path); iAction != std::end(actionsMap))
  {
    const auto& action = iAction->second;
    if (const auto iQAction = m_actionMap.find(&action);
        iQAction != std::end(m_actionMap))
    {
      return iQAction->second;
    }
  }
  return nullptr;
}

void MapWindow::updateTitle()
{
  const auto& map = m_document->map();
  setWindowModified(map.modified());
  setWindowTitle(
    m_prefabEditor ? tr("%1[*] [Prefab] - TrenchBroom").arg(pathAsQString(map.filename()))
                   : tr("%1[*] - TrenchBroom").arg(pathAsQString(map.filename())));
  setWindowFilePath(pathAsQPath(map.path()));
}

void MapWindow::updateTitleDelayed()
{
  m_updateTitleSignalDelayer->queueSignal();
}

void MapWindow::createMenus()
{
  auto createMenuResult = populateMenuBar(
    m_appController.actionManager(), *menuBar(), m_actionMap, [&](const Action& action) {
      auto context = ActionExecutionContext{m_appController, this, currentMapViewBase()};
      action.execute(context);
    });

  m_recentDocumentsMenu = createMenuResult.recentDocumentsMenu;
  m_undoAction = createMenuResult.undoAction;
  m_redoAction = createMenuResult.redoAction;
  m_rerunAction = createMenuResult.rerunAction;

  // The toggle actions belong to the docks and are not in m_actionMap, so
  // updateActionState leaves them alone; each dock keeps its own action checked.
  contract_assert(createMenuResult.sidebarMenu != nullptr);
  // in the order of the tool sidebar's buttons, so that both list the panels alike
  auto toggleActions =
    m_sidebarManager->docks()
    | std::views::transform([](const auto* dock) { return dock->toggleViewAction(); })
    | kdl::ranges::to<std::vector>();
  std::ranges::stable_sort(toggleActions, [](const auto* lhs, const auto* rhs) {
    return toolSidebarEntryLess(*lhs, *rhs);
  });
  for (auto* toggleAction : toggleActions)
  {
    createMenuResult.sidebarMenu->addAction(toggleAction);
  }

  addRecentDocumentsMenu();
}

void MapWindow::updateShortcuts()
{
  for (auto [tbAction, qtAction] : m_actionMap)
  {
    updateActionKeySequence(*qtAction, *tbAction);
  }
}

void MapWindow::updateActionState()
{
  auto context = ActionExecutionContext{m_appController, this, currentMapViewBase()};
  for (auto [tAction, qAction] : m_actionMap)
  {
    if (qAction == m_undoAction || qAction == m_redoAction)
    {
      // These are handled specially for performance reasons.
      continue;
    }
    qAction->setEnabled(tAction->enabled(context));
    if (qAction->isCheckable())
    {
      qAction->setChecked(tAction->checked(context));
    }
  }
}

void MapWindow::updateActionStateDelayed()
{
  m_updateActionStateSignalDelayer->queueSignal();
}

void MapWindow::updateUndoRedoActions()
{
  const auto& map = m_document->map();
  if (m_undoAction)
  {
    if (const auto* undoCommandName = map.undoCommandName())
    {
      const auto text = "Undo " + *undoCommandName;
      m_undoAction->setText(QString::fromStdString(text));
      m_undoAction->setEnabled(true);
    }
    else
    {
      m_undoAction->setText("Undo");
      m_undoAction->setEnabled(false);
    }
  }
  if (m_redoAction)
  {
    if (const auto* redoCommandName = map.redoCommandName())
    {
      const auto text = "Redo " + *redoCommandName;
      m_redoAction->setText(QString::fromStdString(text));
      m_redoAction->setEnabled(true);
    }
    else
    {
      m_redoAction->setText("Redo");
      m_redoAction->setEnabled(false);
    }
  }
}

void MapWindow::addRecentDocumentsMenu()
{
  m_appController.recentDocuments().addMenu(*m_recentDocumentsMenu);
}

void MapWindow::removeRecentDocumentsMenu()
{
  m_appController.recentDocuments().removeMenu(*m_recentDocumentsMenu);
}

void MapWindow::updateRecentDocumentsMenu()
{
  const auto& map = m_document->map();
  const auto path = map.path();
  // a prefab is not a recent map: opened from the list, it would replace a map window's
  // document (H-PF3-4)
  if (path.is_absolute() && !m_prefabEditor)
  {
    m_appController.recentDocuments().updatePath(path);
  }
}

void MapWindow::createGui()
{
  setWindowIconTB(this);
  setWindowTitle("TrenchBroom");

  m_mapView = new SwitchableMapViewContainer{m_appController, document()};
  m_currentMapView = m_mapView->firstMapViewBase();

  // SwitchableMapViewContainer should have constructed a MapViewBase
  contract_assert(m_currentMapView);

  // the map views fill the window between the docks; the console and the issue browser
  // under them are docks too
  m_mapView->setMinimumSize(100, 100);
  setCentralWidget(m_mapView);

  // Any dock can be dropped on another to tab them. With grouped dragging, a group's
  // title bar moves the whole group and a single tab is dragged out of its tab bar. Qt
  // wants the options set before the first dock is added.
  setDockOptions(
    QMainWindow::AnimatedDocks | QMainWindow::AllowTabbedDocks
    | QMainWindow::GroupedDragging);

  // the tab bar of a dock tab group sits at its top, as the old inspector's did
  setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);

  // the side edges own the corners, so a group at a side spans the window's height as the
  // inspector did, and a group at the bottom sits under the map views only
  setCorner(Qt::TopLeftCorner, Qt::LeftDockWidgetArea);
  setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
  setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);
  setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);

  // The docks must exist before createMenus lists them in View > Sidebar and before the
  // window state that places them is restored.
  m_sidebarManager = new SidebarManager{*this};
  const auto sidebarContext =
    SidebarContext{m_appController, document(), m_mapView->toolBox()};
  for (auto& panel : createSidebarPanels(sidebarContext))
  {
    m_sidebarManager->registerPanel(std::move(panel));
  }

  // DD-28, DD-29: a prefab holds no placements of prefabs (RQ21), neither from its own
  // panel nor from a drag out of another window's panel
  if (m_prefabEditor)
  {
    auto* prefabPanel =
      qobject_cast<PrefabPanel*>(m_sidebarManager->panel(PrefabPanel::PanelId));
    contract_assert(prefabPanel != nullptr);
    prefabPanel->setPlacementEnabled(false);
    m_mapView->toolBox().createPrefabTool().setPlacementEnabled(false);
  }

  // the constructor makes the console the document's logger
  m_console = qobject_cast<Console*>(m_sidebarManager->panel(Console::PanelId));
  contract_assert(m_console != nullptr);

  // where the inspector and the info panel were, on their first pages; a restored window
  // state overrides this
  arrangeInspectorDocks(QString::fromLatin1(MapInspector::PanelId));
  arrangeInfoDocks(QString::fromLatin1(Console::PanelId));
}

void MapWindow::restoreWindowState()
{
  // The saved window state is applied once per window, although the app calls this twice
  // (the constructor, then positionOnScreen). Qt builds a new window for each floating
  // tab group on every restore and moves the docks into it one by one; when the old
  // group is down to its last dock, it lets that dock go as a floating dock, so a second
  // restore splits every floating group, and saving keeps the split.
  if (!m_windowStateRestored)
  {
    restoreWidgetState(this);
    m_windowStateRestored = true;
  }

  // A layout saved before some panels were docks does not know their docks, so Qt leaves
  // them where createGui put them, side by side. They take the place of the panel that
  // held them: tabbed, on its page, and hidden if it was hidden. The app restores twice
  // (the constructor, then positionOnScreen), so this runs again until the window is
  // closed and its layout saved with the version.
  const auto hideDockGroup = [&](
                               const std::vector<QString>& panelIds,
                               const QString& oldPage,
                               DockGroupRestoreSet& restoreSet) {
    auto otherPages = std::vector<QString>{};
    for (const auto& panelId : panelIds)
    {
      sidebarDock(*m_sidebarManager, panelId)->hide();
      if (panelId != oldPage)
      {
        otherPages.push_back(panelId);
      }
    }

    // a group whose docks are all hidden forgets its current tab when it is laid out, so
    // the group's toggle brings the old page to the front
    restoreSet = {.front = {oldPage}, .behind = std::move(otherPages)};
  };

  const auto version =
    QSettings{}.value(widgetSettingsPath(this, DockLayoutVersionKey), 0).toInt();
  if (version < 1)
  {
    // the inspector's pages
    const auto panelIds = inspectorPanelIds();
    const auto oldPage = oldTabBookPage("Windows/Inspector_TabBook/State", panelIds);
    arrangeInspectorDocks(oldPage);
    if (!oldPanelVisible("Windows/Inspector/State"))
    {
      hideDockGroup(panelIds, oldPage, m_inspectorPanelsToRestore);
    }
  }
  if (version < 2)
  {
    // the info panel's pages
    const auto panelIds = infoPanelIds();
    const auto oldPage = oldTabBookPage("Windows/InfoPanel_TabBook/State", panelIds);
    arrangeInfoDocks(oldPage);
    if (!oldPanelVisible("Windows/MapWindow_InfoPanel/State"))
    {
      hideDockGroup(panelIds, oldPage, m_infoPanelsToRestore);
    }
  }

  m_sidebarManager->redockOffScreenDocks();
}

void MapWindow::arrangeInspectorDocks(const QString& raisedPanelId)
{
  const auto panelIds = inspectorPanelIds();
  auto* mapDock = sidebarDock(*m_sidebarManager, panelIds[0]);
  auto* entityDock = sidebarDock(*m_sidebarManager, panelIds[1]);
  auto* faceDock = sidebarDock(*m_sidebarManager, panelIds[2]);

  // tabifyDockWidget takes the second dock out of wherever it is, a tab group or the
  // place that a closed dock keeps, and appends it to the first dock's group, so
  // arranging the docks again leaves them as they are
  tabifyDockWidget(mapDock, entityDock);
  tabifyDockWidget(entityDock, faceDock);

  if (auto* raisedDock = findSidebarDock(*m_sidebarManager, raisedPanelId))
  {
    raisedDock->raise();
  }

  // as wide as the old inspector started
  resizeDocks({mapDock}, {350}, Qt::Horizontal);
}

void MapWindow::arrangeInfoDocks(const QString& raisedPanelId)
{
  const auto panelIds = infoPanelIds();
  auto* consoleDock = sidebarDock(*m_sidebarManager, panelIds[0]);
  auto* issuesDock = sidebarDock(*m_sidebarManager, panelIds[1]);

  // like the inspector docks: tabifyDockWidget appends the second dock to the first
  // dock's group from wherever it is, so arranging the docks again leaves them as they
  // are
  tabifyDockWidget(consoleDock, issuesDock);

  if (auto* raisedDock = findSidebarDock(*m_sidebarManager, raisedPanelId))
  {
    raisedDock->raise();
  }

  // as tall as the old info panel started
  resizeDocks({consoleDock}, {100}, Qt::Vertical);
}

FaceInspector* MapWindow::faceInspector() const
{
  auto* faceInspector =
    qobject_cast<FaceInspector*>(m_sidebarManager->panel(FaceInspector::PanelId));
  contract_assert(faceInspector != nullptr);
  return faceInspector;
}

void MapWindow::createToolBar()
{
  m_toolBar = addToolBar("Toolbar");
  m_toolBar->setObjectName("MapWindowToolBar");
  m_toolBar->setFloatable(false);
  m_toolBar->setMovable(false);
  // macOS Qt bug: with the 32x32 default icon size, 24x24 highdpi icons get scaled up to
  // 32x32. We expect them to be drawn at 24x24 logical pixels centered in a 32x32 box, as
  // is the case with non-highdpi icons. As a workaround, just lower the toolbar size to
  // 24x24 (we could alternatively render the icons at 32x32).
  m_toolBar->setIconSize(QSize(24, 24));

  populateToolBar(
    m_appController.actionManager(), *m_toolBar, m_actionMap, [&](const auto& tbAction) {
      auto context = ActionExecutionContext{m_appController, this, currentMapViewBase()};
      tbAction.execute(context);
    });

  m_gridChoice = new QComboBox{};
  m_gridChoice->setObjectName("MapWindow_GridChoice");
  for (int i = mdl::Grid::MinSize; i <= mdl::Grid::MaxSize; ++i)
  {
    const auto gridSize = mdl::Grid::actualSize(i);
    const auto gridSizeStr = tr("Grid %1").arg(QString::number(gridSize, 'g'));
    m_gridChoice->addItem(gridSizeStr, QVariant(i));
  }

  m_toolBar->addWidget(m_gridChoice);
}

void MapWindow::createToolSidebar()
{
  m_toolSidebar = new ToolSidebar{this};
  addToolBar(Qt::LeftToolBarArea, m_toolSidebar);

  // one button per sidebar panel; the strip sorts its buttons
  for (auto* dock : m_sidebarManager->docks())
  {
    m_toolSidebar->addEntry(dock->toggleViewAction());
  }

  populateToolSidebar(
    m_appController.actionManager(),
    *m_toolSidebar,
    m_actionMap,
    [&](const auto& tbAction) {
      auto context = ActionExecutionContext{m_appController, this, currentMapViewBase()};
      tbAction.execute(context);
    });
}

void MapWindow::updateToolBarWidgets()
{
  const auto& map = m_document->map();
  const auto& grid = map.grid();
  const auto sizeIndex = grid.size() - mdl::Grid::MinSize;
  m_gridChoice->setCurrentIndex(sizeIndex);
}

void MapWindow::createStatusBar()
{
  m_statusBarLabel = new QLabel{};
  statusBar()->addWidget(m_statusBarLabel, 1);
  statusBar()->addWidget(m_appController.updater().createUpdateIndicator());
}

namespace
{
template <typename T>
const mdl::EntityNodeBase* commonEntityForNodeList(const std::vector<T*>& nodes)
{
  return !nodes.empty()
             && std::ranges::all_of(
               nodes,
               [&](const auto* node) {
                 return node->entity() == nodes.front()->entity();
               })
           ? nodes.front()->entity()
           : nullptr;
}

std::optional<std::string> commonClassnameForEntityList(
  const std::vector<mdl::EntityNode*>& nodes)
{
  return !nodes.empty()
             && std::ranges::all_of(
               nodes,
               [&](const auto* entityNode) {
                 return entityNode->entity().classname()
                        == nodes.front()->entity().classname();
               })
           ? std::optional{nodes.front()->entity().classname()}
           : std::nullopt;
}

std::string numberWithSuffix(
  size_t count, const std::string& singular, const std::string& plural)
{
  return std::to_string(count) + " " + kdl::str_plural(count, singular, plural);
}

QString describeSelection(const mdl::Map& map)
{
  const auto& editorContext = map.editorContext();

  const auto Arrow = QString(" ") + QString(QChar(0x203A)) + QString(" ");

  auto pipeSeparatedSections = QStringList{};

  pipeSeparatedSections << QString::fromStdString(map.gameInfo().gameConfig.name)
                        << QString::fromStdString(
                             mdl::formatName(map.worldNode().mapFormat()))
                        << QString::fromStdString(editorContext.currentLayer()->name());

  // open groups
  auto groups = std::vector<mdl::GroupNode*>{};
  for (auto* group = editorContext.currentGroup(); group != nullptr;
       group = group->containingGroup())
  {
    groups.push_back(group);
  }

  if (!groups.empty())
  {
    auto openGroups = QStringList{};

    // groups vector is sorted from innermost to outermost, so visit it in reverse order
    for (auto it = groups.rbegin(); it != groups.rend(); ++it)
    {
      const auto* group = *it;
      openGroups << QString::fromStdString(group->name());
    }

    const auto openGroupsString =
      QObject::tr("Open groups: %1").arg(openGroups.join(Arrow));
    pipeSeparatedSections << openGroupsString;
  }

  // build a vector of strings describing the things that are selected
  auto tokens = std::vector<std::string>{};

  const auto& selection = map.selection();

  // selected brushes
  if (selection.hasBrushes())
  {
    const auto* commonEntityNode = commonEntityForNodeList(selection.brushes);

    // if all selected brushes are from the same entity, print the entity name
    auto token = numberWithSuffix(selection.brushes.size(), "brush", "brushes");
    token += commonEntityNode ? " (" + commonEntityNode->entity().classname() + ")"
                              : " (multiple entities)";
    tokens.push_back(token);
  }

  // selected patches
  if (selection.hasPatches())
  {
    const auto* commonEntityNode = commonEntityForNodeList(selection.patches);

    // if all selected patches are from the same entity, print the entity name
    auto token = numberWithSuffix(selection.patches.size(), "patch", "patches");
    token += commonEntityNode ? " (" + commonEntityNode->entity().classname() + ")"
                              : " (multiple entities)";
    tokens.push_back(token);
  }

  // selected brush faces
  if (map.selection().hasBrushFaces())
  {
    const auto token =
      numberWithSuffix(map.selection().brushFaces.size(), "face", "faces");
    tokens.push_back(token);
  }

  // entities
  if (selection.hasEntities())
  {
    const auto commonClassname = commonClassnameForEntityList(selection.entities);

    auto token = numberWithSuffix(selection.entities.size(), "entity", "entities");
    token += " (" + commonClassname.value_or("multiple classnames") + ") ";
    tokens.push_back(token);
  }

  // groups
  if (selection.hasGroups())
  {
    tokens.push_back(numberWithSuffix(selection.groups.size(), "group", "groups"));
  }

  // get the layers of the selected nodes
  const auto selectedObjectLayers =
    mdl::collectContainingLayersUserSorted(selection.nodes);
  auto layersDescription = QString{};
  if (selectedObjectLayers.size() == 1)
  {
    auto* layer = selectedObjectLayers.front();
    layersDescription =
      QObject::tr(" in layer \"%1\"").arg(QString::fromStdString(layer->name()));
  }
  else if (selectedObjectLayers.size() > 1)
  {
    layersDescription = QObject::tr(" in %1 layers").arg(selectedObjectLayers.size());
  }

  // now, turn `tokens` into a comma-separated string
  if (!tokens.empty())
  {
    pipeSeparatedSections << QObject::tr("%1%2 selected")
                               .arg(QString::fromStdString(
                                 kdl::str_join(tokens, ", ", ", and ", " and ")))
                               .arg(layersDescription);
  }

  // count hidden objects
  size_t hiddenGroups = 0u;
  size_t hiddenEntities = 0u;
  size_t hiddenBrushes = 0u;
  size_t hiddenPatches = 0u;

  map.worldNode().accept(kdl::overload(
    [](auto&& thisLambda, const mdl::WorldNode& worldNode) {
      worldNode.visitChildren(thisLambda);
    },
    [](auto&& thisLambda, const mdl::LayerNode& layerNode) {
      layerNode.visitChildren(thisLambda);
    },
    [&](auto&& thisLambda, const mdl::GroupNode& groupNode) {
      if (!editorContext.visible(groupNode))
      {
        ++hiddenGroups;
      }
      groupNode.visitChildren(thisLambda);
    },
    [&](auto&& thisLambda, const mdl::EntityNode& entityNode) {
      if (!editorContext.visible(entityNode))
      {
        ++hiddenEntities;
      }
      entityNode.visitChildren(thisLambda);
    },
    [&](const mdl::BrushNode& brushNode) {
      if (!editorContext.visible(brushNode))
      {
        ++hiddenBrushes;
      }
    },
    [&](const mdl::PatchNode& patchNode) {
      if (!editorContext.visible(patchNode))
      {
        ++hiddenPatches;
      }
    }));

  // print hidden objects
  if (hiddenGroups > 0 || hiddenEntities > 0 || hiddenBrushes > 0)
  {
    auto hiddenDescriptors = std::vector<std::string>{};

    if (hiddenGroups > 0)
    {
      hiddenDescriptors.push_back(numberWithSuffix(hiddenGroups, "group", "groups"));
    }
    if (hiddenEntities > 0)
    {
      hiddenDescriptors.push_back(numberWithSuffix(hiddenEntities, "entity", "entities"));
    }
    if (hiddenBrushes > 0)
    {
      hiddenDescriptors.push_back(numberWithSuffix(hiddenBrushes, "brush", "brushes"));
    }
    if (hiddenPatches > 0)
    {
      hiddenDescriptors.push_back(numberWithSuffix(hiddenPatches, "patch", "patches"));
    }

    pipeSeparatedSections << QObject::tr("%1 hidden")
                               .arg(QString::fromStdString(kdl::str_join(
                                 hiddenDescriptors, ", ", ", and ", " and ")));
  }

  return QString::fromLatin1("   ")
         + pipeSeparatedSections.join(QLatin1String("   |   "));
}

} // namespace

void MapWindow::updateStatusBar()
{
  m_statusBarLabel->setText(QString{describeSelection(m_document->map())});
}

void MapWindow::updateStatusBarDelayed()
{
  m_updateStatusBarSignalDelayer->queueSignal();
}

void MapWindow::connectObservers()
{
  auto& prefs = PreferenceManager::instance();
  m_notifierConnection +=
    prefs.preferenceDidChangeNotifier.connect(this, &MapWindow::preferenceDidChange);

  auto& resourceManager = m_appController.glManager().resourceManager();
  m_notifierConnection += resourceManager.resourcesWereProcessedNotifier.connect(
    this, &MapWindow::resourcesWereProcessed);

  m_notifierConnection +=
    m_document->documentWasLoadedNotifier.connect(this, &MapWindow::documentWasLoaded);
  m_notifierConnection +=
    m_document->documentWasSavedNotifier.connect(this, &MapWindow::documentWasSaved);

  m_notifierConnection +=
    m_document->pointFileWasLoadedNotifier.connect(this, &MapWindow::pointFileDidChange);
  m_notifierConnection += m_document->pointFileWasUnloadedNotifier.connect(
    this, &MapWindow::pointFileDidChange);
  m_notifierConnection += m_document->portalFileWasLoadedNotifier.connect(
    this, &MapWindow::portalFileDidChange);
  m_notifierConnection += m_document->portalFileWasUnloadedNotifier.connect(
    this, &MapWindow::portalFileDidChange);

  m_notifierConnection += m_document->modificationStateDidChangeNotifier.connect(
    this, &MapWindow::mapModificationStateDidChange);
  m_notifierConnection +=
    m_document->selectionDidChangeNotifier.connect(this, &MapWindow::selectionDidChange);
  m_notifierConnection += m_document->currentLayerDidChangeNotifier.connect(
    this, &MapWindow::currentLayerDidChange);
  m_notifierConnection +=
    m_document->groupWasOpenedNotifier.connect(this, &MapWindow::groupWasOpened);
  m_notifierConnection +=
    m_document->groupWasClosedNotifier.connect(this, &MapWindow::groupWasClosed);
  m_notifierConnection += m_document->nodeVisibilityDidChangeNotifier.connect(
    this, &MapWindow::nodeVisibilityDidChange);
  m_notifierConnection += m_document->editorContextDidChangeNotifier.connect(
    this, &MapWindow::editorContextDidChange);
  m_notifierConnection += m_document->triggerVisualEffectNotifier.connect(
    this, &MapWindow::triggerVisualEffect);

  m_notifierConnection +=
    m_document->transactionDoneNotifier.connect(this, &MapWindow::transactionDone);
  m_notifierConnection +=
    m_document->transactionUndoneNotifier.connect(this, &MapWindow::transactionUndone);

  m_notifierConnection +=
    m_document->gridDidChangeNotifier.connect(this, &MapWindow::gridDidChange);

  m_notifierConnection +=
    m_mapView->toolBox().toolActivatedNotifier.connect(this, &MapWindow::toolActivated);
  m_notifierConnection += m_mapView->toolBox().toolDeactivatedNotifier.connect(
    this, &MapWindow::toolDeactivated);
  m_notifierConnection += m_mapView->toolBox().toolHandleSelectionChangedNotifier.connect(
    this, &MapWindow::toolHandleSelectionChanged);
}

void MapWindow::documentWasLoaded()
{
  updateTitle();
  updateActionState();
  updateUndoRedoActions();
  updateToolBarWidgets();
  updateRecentDocumentsMenu();
  loadLastCompilationProfileName();
}

void MapWindow::documentWasSaved()
{
  updateTitle();
  updateActionState();
  updateUndoRedoActions();
  updateRecentDocumentsMenu();

  if (m_prefabEditor)
  {
    // R1-pf3-10: grabbing the 3D view paints it, which processes the input recorded
    // since its last paint, so it is not done while the save notifies its observers
    m_pendingPrefabSavePath = m_document->map().path();
    QTimer::singleShot(0, this, [this]() { finishPrefabSave(); });
  }
}

void MapWindow::mapModificationStateDidChange()
{
  updateTitleDelayed();
}

void MapWindow::transactionDone(const std::string&, const bool, const bool)
{
  QTimer::singleShot(0, this, [this]() {
    // FIXME: Delaying this with QTimer::singleShot is a hack to work around the lack of
    // a notification that's called _after_ the CommandProcessor undo/redo stacks are
    // modified.
    //
    // The current transactionDoneNotifier is called after the transaction executes, but
    // before it's pushed onto the undo stack, but we need to read the undo stack in
    // updateUndoRedoActions(), so this QTimer::singleShot is needed for now.
    updateUndoRedoActions();
  });
}

void MapWindow::transactionUndone(const std::string&, const bool, const bool)
{
  QTimer::singleShot(0, this, [this]() {
    // FIXME: see MapWindow::transactionDone
    updateUndoRedoActions();
  });
}

void MapWindow::preferenceDidChange(const std::filesystem::path& path)
{
  if (path == Preferences::MapViewLayout.path)
  {
    m_mapView->switchToMapView(
      static_cast<MapViewLayout>(pref(Preferences::MapViewLayout)));
  }

  if (path == Preferences::ShowLightVolumes.path)
  {
    // the View > Light Volumes actions are not exclusive; only this corrects their
    // check marks
    updateActionStateDelayed();
  }

  updateShortcuts();
}

void MapWindow::resourcesWereProcessed(const std::vector<gl::ResourceId>&)
{
  updateActionState();
}

void MapWindow::gridDidChange()
{
  updateActionStateDelayed();
  updateToolBarWidgets();
}

void MapWindow::toolActivated(Tool&)
{
  updateActionStateDelayed();
}

void MapWindow::toolDeactivated(Tool&)
{
  updateActionStateDelayed();
}

void MapWindow::toolHandleSelectionChanged(Tool&)
{
  updateActionStateDelayed();
}

void MapWindow::selectionDidChange(const mdl::SelectionChange&)
{
  updateActionStateDelayed();
  updateStatusBarDelayed();
}

void MapWindow::currentLayerDidChange()
{
  updateStatusBarDelayed();
}

void MapWindow::groupWasOpened()
{
  updateStatusBarDelayed();
}

void MapWindow::groupWasClosed()
{
  updateStatusBarDelayed();
}

void MapWindow::nodeVisibilityDidChange(const std::vector<mdl::Node*>&)
{
  updateStatusBarDelayed();
}

void MapWindow::editorContextDidChange()
{
  // e.g. changing the view filters may cause the number of hidden brushes/entities to
  // change
  updateStatusBarDelayed();
}

void MapWindow::triggerVisualEffect(const mdl::VisualEffect visualEffect)
{
  switch (visualEffect)
  {
  case mdl::VisualEffect::FlashSelection:
    m_mapView->flashSelection();
    break;
  }
}

void MapWindow::pointFileDidChange()
{
  updateActionStateDelayed();
}

void MapWindow::portalFileDidChange()
{
  updateActionStateDelayed();
}

void MapWindow::bindEvents()
{
  connect(m_autosaveTimer, &QTimer::timeout, this, &MapWindow::triggerAutosave);
  connect(qApp, &QApplication::focusChanged, this, &MapWindow::focusChange);
  connect(
    m_gridChoice,
    QOverload<int>::of(&QComboBox::activated),
    this,
    [this](const int index) { setGridSize(index + mdl::Grid::MinSize); });
  connect(QApplication::clipboard(), &QClipboard::dataChanged, this, [this]() {
    // update the "Paste" menu items
    this->updateActionState();
  });
  connect(
    m_toolBar, &QToolBar::visibilityChanged, this, [this](const bool /* visible */) {
      // update the "Toggle Toolbar" menu item
      this->updateActionState();
    });
  connect(
    m_toolSidebar, &QToolBar::visibilityChanged, this, [this](const bool /* visible */) {
      // update the "Toggle Tool Sidebar" menu item
      this->updateActionState();
    });
  auto groupedPanelIds = inspectorPanelIds();
  std::ranges::copy(infoPanelIds(), std::back_inserter(groupedPanelIds));
  for (const auto& panelId : groupedPanelIds)
  {
    // update the "Toggle Inspector" and "Toggle Info Panel" menu items when a dock is
    // closed from its title bar, its tool sidebar button or View > Sidebar
    connect(
      sidebarDock(*m_sidebarManager, panelId),
      &QDockWidget::visibilityChanged,
      this,
      &MapWindow::updateActionStateDelayed);
  }

  connect(
    m_updateTitleSignalDelayer,
    &SignalDelayer::processSignal,
    this,
    &MapWindow::updateTitle);
  connect(
    m_updateActionStateSignalDelayer,
    &SignalDelayer::processSignal,
    this,
    &MapWindow::updateActionState);
  connect(
    m_updateStatusBarSignalDelayer,
    &SignalDelayer::processSignal,
    this,
    &MapWindow::updateStatusBar);
}

bool MapWindow::saveDocument()
{
  auto& map = m_document->map();

  if (map.persistent())
  {
    if (m_prefabEditor)
    {
      updatePrefabMetadata();
    }

    const auto startTime = std::chrono::high_resolution_clock::now();
    return map.save() | kdl::transform([&]() {
             const auto endTime = std::chrono::high_resolution_clock::now();

             logger().info() << "Saved " << map.path() << " in "
                             << std::chrono::duration_cast<std::chrono::milliseconds>(
                                  endTime - startTime)
                                  .count()
                             << "ms";
           })
           | kdl::transform_error([&](const auto& e) {
               QMessageBox::critical(
                 this,
                 "",
                 QString::fromStdString(
                   fmt::format("Error while saving {}: {}", map.path(), e.msg)),
                 QMessageBox::Ok);
             })
           | kdl::is_success();
  }
  return saveDocumentAs();
}

bool MapWindow::saveDocumentAs()
{
  auto& map = m_document->map();

  const auto defaultPath = map.persistent()
                             ? map.path()
                             : m_appController.environmentConfig().userDataFolderPath;

  const auto newFileName = QFileDialog::getSaveFileName(
    this, tr("Save map file"), pathAsQPath(defaultPath), "Map files (*.map)");
  if (newFileName.isEmpty())
  {
    return false;
  }

  const auto path = pathFromQString(newFileName);

  if (m_prefabEditor)
  {
    updatePrefabMetadata();
  }

  const auto startTime = std::chrono::high_resolution_clock::now();
  return map.saveAs(path) | kdl::transform([&]() {
           const auto endTime = std::chrono::high_resolution_clock::now();

           logger().info() << "Saved " << map.path() << " in "
                           << std::chrono::duration_cast<std::chrono::milliseconds>(
                                endTime - startTime)
                                .count()
                           << "ms";
         })
         | kdl::transform_error([&](const auto& e) {
             QMessageBox::critical(
               this,
               "",
               QString::fromStdString(
                 fmt::format("Error while saving {}: {}", path, e.msg)),
               QMessageBox::Ok);
           })
         | kdl::is_success();
}

void MapWindow::revertDocument()
{
  auto& map = m_document->map();
  if (map.persistent() && confirmRevertDocument())
  {
    m_document->reload() | kdl::transform_error([&](auto e) {
      logger().error() << "Failed to revert document: " << e.msg;
    });
  }
}

bool MapWindow::exportDocumentAsObj()
{
  if (!m_objExportDialog)
  {
    m_objExportDialog = new ObjExportDialog{this};
  }

  m_objExportDialog->updateExportPath();
  showModelessDialog(m_objExportDialog);
  return true;
}

bool MapWindow::exportDocumentAsMap()
{
  const auto& map = m_document->map();
  const auto& originalPath = map.path();

  const auto newFileName = QFileDialog::getSaveFileName(
    this, tr("Export Map file"), pathAsQPath(originalPath), "Map files (*.map)");
  if (newFileName.isEmpty())
  {
    return false;
  }

  const auto options = mdl::MapExportOptions{
    pathFromQString(newFileName),
    !K(stripTbProperties),
    std::nullopt,
    std::nullopt,
  };
  return exportDocument(options);
}

bool MapWindow::exportDocument(const mdl::ExportOptions& options)
{
  const auto& map = m_document->map();
  const auto exportPath = std::visit([](const auto& o) { return o.exportPath; }, options);

  if (exportPath == map.path())
  {
    QMessageBox::critical(
      this,
      "",
      tr("You can't overwrite the current document.\nPlease choose a different file name "
         "to export "
         "to."));
    return false;
  }

  return map.exportAs(options) | kdl::transform([&]() {
           logger().info() << "Exported " << exportPath;
           return true;
         })
         | kdl::transform_error([&](auto e) {
             logger().error() << "Could not export '" << exportPath << "': " + e.msg;
             QMessageBox::critical(this, "", QString::fromStdString(e.msg));
             return false;
           })
         | kdl::value();
}

/**
 * Returns whether the window should close.
 */
bool MapWindow::confirmOrDiscardChanges()
{
  const auto& map = m_document->map();
  if (!map.modified())
  {
    return true;
  }

  const auto result = QMessageBox::question(
    this,
    "TrenchBroom",
    tr("%1 has been modified. Do you want to save the changes?")
      .arg(pathAsQString(map.filename())),
    QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);

  if (result == QMessageBox::Yes)
  {
    return saveDocument();
  }
  return result == QMessageBox::No;
}

/**
 * Returns whether the document should be reverted.
 */
bool MapWindow::confirmRevertDocument()
{
  const auto& map = m_document->map();
  if (!map.modified())
  {
    return true;
  }

  auto messageBox = QMessageBox{this};
  messageBox.setWindowTitle("TrenchBroom");
  messageBox.setIcon(QMessageBox::Question);
  messageBox.setText(tr("Revert %1 to %2?")
                       .arg(pathAsQString(map.filename()))
                       .arg(pathAsQString(map.path())));
  messageBox.setInformativeText(
    tr("This will discard all unsaved changes and reload the document from disk."));

  auto* revertButton = messageBox.addButton(tr("Revert"), QMessageBox::DestructiveRole);
  auto* cancelButton = messageBox.addButton(QMessageBox::Cancel);
  messageBox.setDefaultButton(cancelButton);

  messageBox.exec();

  return messageBox.clickedButton() == revertButton;
}

void MapWindow::loadPointFile()
{
  const auto& map = m_document->map();
  const auto path = map.path();
  const auto defaultDir = !path.empty() ? pathAsQPath(path.parent_path()) : QString{};

  const auto fileName = QFileDialog::getOpenFileName(
    this,
    tr("Load Point File"),
    defaultDir,
    "Point files (*.pts *.lin);;Any files (*.*)");

  if (!fileName.isEmpty())
  {
    m_document->loadPointFile(pathFromQString(fileName));
  }
}

void MapWindow::reloadPointFile()
{
  if (canReloadPointFile())
  {
    m_document->reloadPointFile();
  }
}

void MapWindow::unloadPointFile()
{
  if (canUnloadPointFile())
  {
    m_document->unloadPointFile();
  }
}

bool MapWindow::canUnloadPointFile() const
{
  return m_document->isPointFileLoaded();
}

bool MapWindow::canReloadPointFile() const
{
  return m_document->canReloadPointFile();
}

void MapWindow::loadPortalFile()
{
  const auto& map = m_document->map();
  const auto path = map.path();
  const auto defaultDir = !path.empty() ? pathAsQPath(path.parent_path()) : QString{};

  const auto fileName = QFileDialog::getOpenFileName(
    this, tr("Load Portal File"), defaultDir, "Portal files (*.prt);;Any files (*.*)");

  if (!fileName.isEmpty())
  {
    m_document->loadPortalFile(pathFromQString(fileName));
  }
}

void MapWindow::reloadPortalFile()
{
  if (canReloadPortalFile())
  {
    m_document->reloadPortalFile();
  }
}

void MapWindow::unloadPortalFile()
{
  if (canUnloadPortalFile())
  {
    m_document->unloadPortalFile();
  }
}

bool MapWindow::canUnloadPortalFile() const
{
  return m_document->isPortalFileLoaded();
}

bool MapWindow::canReloadPortalFile() const
{
  return m_document->canReloadPortalFile();
}

void MapWindow::reloadMaterialCollections()
{
  mdl::reloadMaterialCollections(m_document->map());
}

void MapWindow::reloadEntityDefinitions()
{
  mdl::reloadEntityDefinitions(m_document->map());
}

bool MapWindow::canReloadMaterialCollections() const
{
  return !m_appController.glManager().resourceManager().needsProcessing();
}

bool MapWindow::canReloadEntityDefinitions() const
{
  return !m_appController.glManager().resourceManager().needsProcessing();
}

void MapWindow::closeDocument()
{
  close();
}

void MapWindow::undo()
{
  if (canUndo() && !m_mapView->cancelMouseDrag() && !faceInspector()->cancelMouseDrag())
  {
    m_document->map().undoCommand();
  }
}

void MapWindow::redo()
{
  if (canRedo())
  {
    m_document->map().redoCommand();
  }
}

bool MapWindow::canUndo() const
{
  return m_document->map().canUndoCommand();
}

bool MapWindow::canRedo() const
{
  return m_document->map().canRedoCommand();
}

void MapWindow::repeatLastCommands()
{
  m_document->map().repeatCommands();
}

void MapWindow::clearRepeatableCommands()
{
  if (hasRepeatableCommands())
  {
    m_document->map().clearRepeatableCommands();
  }
}

bool MapWindow::hasRepeatableCommands() const
{
  return m_document->map().canRepeatCommands();
}

void MapWindow::cutSelection()
{
  if (canCutSelection())
  {
    auto& map = m_document->map();

    copyToClipboard();
    auto transaction = mdl::Transaction{map, "Cut"};
    removeSelectedNodes(map);
    transaction.commit();
  }
}

void MapWindow::copySelection()
{
  if (canCopySelection())
  {
    copyToClipboard();
  }
}

void MapWindow::copyToClipboard()
{
  auto& map = m_document->map();
  const auto& selection = map.selection();
  const auto str = selection.hasNodes()        ? serializeSelectedNodes(map)
                   : selection.hasBrushFaces() ? serializeSelectedBrushFaces(map)
                                               : std::string{};

  auto* clipboard = QApplication::clipboard();
  clipboard->setText(mapStringToUnicode(map.encoding(), str));
}

bool MapWindow::canCutSelection() const
{
  const auto& map = m_document->map();
  const auto& selection = map.selection();
  return widgetOrChildHasFocus(m_mapView) && selection.hasNodes()
         && !m_mapView->toolBox().anyModalToolActive();
}

bool MapWindow::canCopySelection() const
{
  const auto& map = m_document->map();
  const auto& selection = map.selection();
  return widgetOrChildHasFocus(m_mapView)
         && (selection.hasNodes() || selection.hasBrushFaces());
}

void MapWindow::pasteAtCursorPosition()
{
  if (canPaste())
  {
    auto& map = m_document->map();
    const auto referenceBounds = map.referenceBounds();

    auto transaction = mdl::Transaction{map, "Paste"};
    switch (paste())
    {
    case mdl::PasteType::Node:
      if (const auto bounds = map.selectionBounds())
      {
        // The pasted objects must be hidden to prevent the picking done in
        // pasteObjectsDelta from hitting them
        // (https://github.com/TrenchBroom/TrenchBroom/issues/2755)
        const auto nodes = map.selection().nodes;

        hideNodes(map, nodes);
        const auto delta = m_mapView->pasteObjectsDelta(*bounds, referenceBounds);
        showNodes(map, nodes);
        selectNodes(map, nodes); // Hiding deselected the nodes, so reselect them
        if (!translateSelection(map, delta))
        {
          transaction.cancel();
          break;
        }
      }
      transaction.commit();
      break;
    case mdl::PasteType::BrushFace:
      transaction.commit();
      break;
    case mdl::PasteType::Failed:
      transaction.cancel();
      break;
    }
  }
}

void MapWindow::pasteAtOriginalPosition()
{
  if (canPaste())
  {
    paste();
  }
}

mdl::PasteType MapWindow::paste()
{
  auto* clipboard = QApplication::clipboard();
  const auto qtext = clipboard->text();

  if (qtext.isEmpty())
  {
    logger().error() << "Clipboard is empty";
    return mdl::PasteType::Failed;
  }

  auto& map = m_document->map();
  return mdl::paste(map, mapStringFromUnicode(map.encoding(), qtext));
}

/**
 * This is relatively expensive so only call it when the clipboard changes or e.g. the
 * user tries to paste.
 */
bool MapWindow::canPaste() const
{
  if (!widgetOrChildHasFocus(m_mapView) || !m_mapView->isCurrent())
  {
    return false;
  }

  const auto* clipboard = QApplication::clipboard();
  const auto* mimeData = clipboard->mimeData();
  return mimeData && mimeData->hasText();
}

void MapWindow::duplicateSelection()
{
  if (canDuplicateSelection())
  {
    duplicateSelectedNodes(m_document->map());
  }
}

bool MapWindow::canDuplicateSelection() const
{
  const auto& map = m_document->map();
  return map.selection().hasNodes();
}

void MapWindow::deleteSelection()
{
  if (canDeleteSelection())
  {
    auto& toolBox = m_mapView->toolBox();

    if (toolBox.clipToolActive())
    {
      toolBox.clipTool().removeLastPoint();
    }
    else if (toolBox.vertexToolActive())
    {
      toolBox.vertexTool().removeSelection();
    }
    else if (toolBox.edgeToolActive())
    {
      toolBox.edgeTool().removeSelection();
    }
    else if (toolBox.faceToolActive())
    {
      toolBox.faceTool().removeSelection();
    }
    else if (!toolBox.anyModalToolActive())
    {
      auto& map = m_document->map();
      removeSelectedNodes(map);
    }
  }
}

bool MapWindow::canDeleteSelection() const
{
  auto& toolBox = m_mapView->toolBox();

  if (toolBox.clipToolActive())
  {
    return toolBox.clipTool().canRemoveLastPoint();
  }
  if (toolBox.vertexToolActive())
  {
    return toolBox.vertexTool().canRemoveSelection();
  }
  if (toolBox.edgeToolActive())
  {
    return toolBox.edgeTool().canRemoveSelection();
  }
  if (toolBox.faceToolActive())
  {
    return toolBox.faceTool().canRemoveSelection();
  }
  return canCutSelection();
}

void MapWindow::saveSelectionAsPrefab()
{
  if (canSaveSelectionAsPrefab())
  {
    // the prefab panel reads the saved file into the library
    if (
      auto* prefabPanel =
        dynamic_cast<PrefabPanel*>(m_sidebarManager->panel(PrefabPanel::PanelId)))
    {
      prefabPanel->saveSelectionAsPrefab();
    }
  }
}

bool MapWindow::canSaveSelectionAsPrefab() const
{
  // as the prefab panel's button: a selection and somewhere to save it
  const auto& map = m_document->map();
  const auto& library = map.prefabLibrary();
  return map.selection().hasNodes()
         && (!library.writeRootFor(mdl::PrefabRoot::User).empty()
             || !library.writeRootFor(mdl::PrefabRoot::Mod).empty());
}

void MapWindow::editPrefab(const std::filesystem::path& path)
{
  // PF3: a window of its own on every platform, so this document stays
  m_appController.mapWindowManager().openPrefabEditor(*this, path)
    | kdl::transform_error([&](const auto& e) {
        logger().error() << "Could not open prefab " << path << ": " << e.msg;
        QMessageBox::critical(
          this,
          "TrenchBroom",
          QString::fromStdString(
            fmt::format("Could not open prefab {}: {}", path, e.msg)),
          QMessageBox::Ok);
      });
}

void MapWindow::prefabWasSaved(const std::filesystem::path& path)
{
  if (
    auto* prefabPanel =
      dynamic_cast<PrefabPanel*>(m_sidebarManager->panel(PrefabPanel::PanelId)))
  {
    prefabPanel->prefabWasSaved(path);
  }
}

void MapWindow::setThumbnailGrabberForTesting(ThumbnailGrabber thumbnailGrabber)
{
  m_thumbnailGrabber = std::move(thumbnailGrabber);
}

void MapWindow::selectAll()
{
  if (canSelect())
  {
    selectAllNodes(m_document->map());
  }
}

void MapWindow::selectSiblings()
{
  if (canSelectSiblings())
  {
    selectSiblingNodes(m_document->map());
  }
}

void MapWindow::selectTouching()
{
  if (canSelectByBrush())
  {
    selectTouchingNodes(m_document->map(), true);
  }
}

void MapWindow::selectInside()
{
  if (canSelectByBrush())
  {
    selectContainedNodes(m_document->map(), true);
  }
}

void MapWindow::selectTall()
{
  if (canSelectTall())
  {
    m_mapView->selectTall();
  }
}

void MapWindow::selectByLineNumber()
{
  if (canSelect())
  {
    const auto string = QInputDialog::getText(
      this,
      "Select by Line Numbers",
      "Enter a comma- or space separated list of line numbers.");
    if (!string.isEmpty())
    {
      auto positions = std::vector<size_t>{};
      for (const auto& token : string.split(QRegularExpression{"[, ]"}))
      {
        auto ok = false;
        const auto position = token.toLong(&ok);
        if (ok && position > 0)
        {
          positions.push_back(static_cast<size_t>(position));
        }
      }

      selectNodesWithFilePosition(m_document->map(), positions);
    }
  }
}

void MapWindow::selectInverse()
{
  if (canSelectInverse())
  {
    invertNodeSelection(m_document->map());
  }
}

void MapWindow::selectNone()
{
  if (canDeselect())
  {
    auto& map = m_document->map();
    deselectAll(map);
  }
}

bool MapWindow::canSelect() const
{
  return canChangeSelection();
}

bool MapWindow::canSelectSiblings() const
{
  auto& map = m_document->map();
  return canChangeSelection() && map.selection().hasNodes();
}

bool MapWindow::canSelectByBrush() const
{
  auto& map = m_document->map();
  return canChangeSelection() && map.selection().hasOnlyBrushes();
}

bool MapWindow::canSelectTall() const
{
  auto& map = m_document->map();
  return canChangeSelection() && map.selection().hasOnlyBrushes()
         && m_mapView->canSelectTall();
}

bool MapWindow::canDeselect() const
{
  auto& map = m_document->map();
  return canChangeSelection() && map.selection().hasNodes();
}

bool MapWindow::canChangeSelection() const
{
  auto& map = m_document->map();
  return map.editorContext().canChangeSelection();
}

bool MapWindow::canSelectInverse() const
{
  auto& map = m_document->map();
  return map.editorContext().canChangeSelection();
}

void MapWindow::groupSelectedObjects()
{
  if (canGroupSelectedObjects())
  {
    const auto name = queryGroupName(this, "Unnamed");
    if (!name.empty())
    {
      groupSelectedNodes(m_document->map(), name);
    }
  }
}

bool MapWindow::canGroupSelectedObjects() const
{
  auto& map = m_document->map();
  return map.selection().hasNodes() && !m_mapView->toolBox().anyModalToolActive();
}

void MapWindow::ungroupSelectedObjects()
{
  if (canUngroupSelectedObjects())
  {
    ungroupSelectedNodes(m_document->map());
  }
}

bool MapWindow::canUngroupSelectedObjects() const
{
  auto& map = m_document->map();
  return map.selection().hasGroups() && !m_mapView->toolBox().anyModalToolActive();
}

void MapWindow::renameSelectedGroups()
{
  if (canRenameSelectedGroups())
  {
    auto& map = m_document->map();
    contract_assert(map.selection().hasOnlyGroups());

    const auto suggestion = map.selection().groups.front()->name();
    const auto name = queryGroupName(this, suggestion);
    if (!name.empty())
    {
      mdl::renameSelectedGroups(map, name);
    }
  }
}

bool MapWindow::canRenameSelectedGroups() const
{
  auto& map = m_document->map();
  return map.selection().hasOnlyGroups();
}

void MapWindow::replaceMaterial()
{
  auto dialog = ReplaceMaterialDialog{m_appController, document(), this};
  dialog.exec();
}

void MapWindow::moveSelectedObjects()
{
  auto ok = false;
  const auto str = QInputDialog::getText(
    this,
    "Move Objects",
    "Enter coordinates: X Y Z",
    QLineEdit::Normal,
    toString(vm::vec3d{}),
    &ok);
  if (ok)
  {
    if (const auto offset = parse<double, 3>(str))
    {
      auto& map = m_document->map();
      translateSelection(map, *offset);
    }
    else
    {
      QMessageBox::warning(this, "Error", tr("Invalid coordinates: '%1'").arg(str));
    }
  }
}

bool MapWindow::canMoveSelectedObjects() const
{
  auto& map = m_document->map();
  return map.selection().hasNodes() && !m_mapView->toolBox().anyModalToolActive();
}

void MapWindow::csgConvexMerge()
{
  if (canDoCsgConvexMerge())
  {
    auto& toolBox = m_mapView->toolBox();

    if (toolBox.vertexToolActive() && toolBox.vertexTool().canDoCsgConvexMerge())
    {
      toolBox.vertexTool().csgConvexMerge();
    }
    else if (toolBox.edgeToolActive() && toolBox.edgeTool().canDoCsgConvexMerge())
    {
      toolBox.edgeTool().csgConvexMerge();
    }
    else if (toolBox.faceToolActive() && toolBox.faceTool().canDoCsgConvexMerge())
    {
      toolBox.faceTool().csgConvexMerge();
    }
    else
    {
      mdl::csgConvexMerge(m_document->map());
    }
  }
}

bool MapWindow::canDoCsgConvexMerge() const
{
  const auto& toolBox = m_mapView->toolBox();
  const auto& map = m_document->map();
  const auto& selection = map.selection();

  return (selection.hasBrushFaces() && selection.brushFaces.size() > 1)
         || (selection.hasOnlyBrushes() && selection.brushes.size() > 1)
         || (toolBox.vertexToolActive() && toolBox.vertexTool().canDoCsgConvexMerge())
         || (toolBox.edgeToolActive() && toolBox.edgeTool().canDoCsgConvexMerge())
         || (toolBox.faceToolActive() && toolBox.faceTool().canDoCsgConvexMerge());
}

void MapWindow::csgSubtract()
{
  if (canDoCsgSubtract())
  {
    mdl::csgSubtract(m_document->map());
  }
}

bool MapWindow::canDoCsgSubtract() const
{
  const auto& map = m_document->map();
  const auto& selection = map.selection();
  return selection.hasOnlyBrushes() && !selection.brushes.empty();
}

void MapWindow::csgHollow()
{
  if (canDoCsgHollow())
  {
    mdl::csgHollow(m_document->map());
  }
}

bool MapWindow::canDoCsgHollow() const
{
  const auto& map = m_document->map();
  const auto& selection = map.selection();
  return selection.hasOnlyBrushes() && !selection.brushes.empty();
}

void MapWindow::csgIntersect()
{
  if (canDoCsgIntersect())
  {
    mdl::csgIntersect(m_document->map());
  }
}

bool MapWindow::canDoCsgIntersect() const
{
  const auto& map = m_document->map();
  const auto& selection = map.selection();
  return selection.hasOnlyBrushes() && selection.brushes.size() > 1;
}

void MapWindow::snapVerticesToInteger()
{
  if (canSnapVertices())
  {
    auto& map = m_document->map();
    snapVertices(map, 1u);
  }
}

void MapWindow::snapVerticesToGrid()
{
  if (canSnapVertices())
  {
    auto& map = m_document->map();
    snapVertices(map, map.grid().actualSize());
  }
}

bool MapWindow::canSnapVertices() const
{
  const auto& map = m_document->map();
  const auto& selection = map.selection();
  return !selection.allBrushes().empty();
}

void MapWindow::convertSelectionToPatches()
{
  if (canConvertSelectionToPatches())
  {
    auto& map = m_document->map();
    mdl::convertSelectionToPatches(map, 3, 3);

    if (!toolBox().controlPointToolActive())
    {
      toolBox().toggleControlPointTool();
    }
  }
}

bool MapWindow::canConvertSelectionToPatches() const
{
  const auto& map = m_document->map();
  const auto& selection = map.selection();
  return mdl::hasPatchSupport(map.worldNode().mapFormat())
         && selection.hasAnyBrushFaces();
}

void MapWindow::toggleAlignmentLock()
{
  togglePref(Preferences::AlignmentLock);
}

void MapWindow::toggleUvLock()
{
  togglePref(Preferences::UvLock);
}

void MapWindow::toggleShowGrid()
{
  auto& map = m_document->map();
  map.grid().toggleVisible();
}

void MapWindow::toggleSnapToGrid()
{
  auto& map = m_document->map();
  map.grid().toggleSnap();
}

void MapWindow::incGridSize()
{
  if (canIncGridSize())
  {
    auto& map = m_document->map();
    map.grid().incSize();
  }
}

bool MapWindow::canIncGridSize() const
{
  const auto& map = m_document->map();
  return map.grid().size() < mdl::Grid::MaxSize;
}

void MapWindow::decGridSize()
{
  if (canDecGridSize())
  {
    auto& map = m_document->map();
    map.grid().decSize();
  }
}

bool MapWindow::canDecGridSize() const
{
  const auto& map = m_document->map();
  return map.grid().size() > mdl::Grid::MinSize;
}

void MapWindow::setGridSize(const int size)
{
  auto& map = m_document->map();
  map.grid().setSize(size);
}

void MapWindow::moveCameraToNextPoint()
{
  if (canMoveCameraToNextPoint())
  {
    m_mapView->moveCameraToNextTracePoint();
  }
}

bool MapWindow::canMoveCameraToNextPoint() const
{
  return m_mapView->canMoveCameraToNextTracePoint();
}

void MapWindow::moveCameraToPreviousPoint()
{
  if (canMoveCameraToPreviousPoint())
  {
    m_mapView->moveCameraToPreviousTracePoint();
  }
}

bool MapWindow::canMoveCameraToPreviousPoint() const
{
  return m_mapView->canMoveCameraToPreviousTracePoint();
}

void MapWindow::reset2dCameras()
{
  if (auto* mapView2d = dynamic_cast<ui::MapView2D*>(currentMapViewBase()))
  {
    m_mapView->reset2dCameras(mapView2d->camera(), true);
  }
}

void MapWindow::focusCameraOnSelection()
{
  if (canFocusCamera())
  {
    m_mapView->focusCameraOnSelection(true);
  }
}

bool MapWindow::canFocusCamera() const
{
  auto& map = m_document->map();
  return map.selection().hasNodes();
}

void MapWindow::moveCameraToPosition()
{
  auto ok = false;
  const auto str = QInputDialog::getText(
    this,
    "Move Camera",
    "Enter a position (x y z) for the camera.",
    QLineEdit::Normal,
    toString(vm::vec3d{}),
    &ok);
  if (ok)
  {
    if (const auto position = parse<float, 3>(str))
    {
      m_mapView->moveCameraToPosition(*position, true);
    }
  }
}

void MapWindow::isolateSelection()
{
  if (canIsolateSelection())
  {
    isolateSelectedNodes(m_document->map());
  }
}

bool MapWindow::canIsolateSelection() const
{
  const auto& map = m_document->map();
  return map.selection().hasNodes();
}

void MapWindow::hideSelection()
{
  if (canHideSelection())
  {
    hideSelectedNodes(m_document->map());
  }
}

bool MapWindow::canHideSelection() const
{
  const auto& map = m_document->map();
  return map.selection().hasNodes();
}

void MapWindow::showAll()
{
  showAllNodes(m_document->map());
}

void MapWindow::showSidebarPanel(const QString& panelId)
{
  m_sidebarManager->showPanel(panelId);
}

void MapWindow::toggleToolbar()
{
  m_toolBar->setVisible(!m_toolBar->isVisible());
}

bool MapWindow::toolbarVisible() const
{
  return m_toolBar->isVisible();
}

void MapWindow::toggleToolSidebar()
{
  // by the hidden flag like the inspector, so that it also toggles while the window is
  // not shown yet
  m_toolSidebar->setHidden(!m_toolSidebar->isHidden());
}

bool MapWindow::toolSidebarVisible() const
{
  return m_toolSidebar->isVisible();
}

void MapWindow::toggleInfoPanels()
{
  toggleDockGroup(infoPanelIds(), m_infoPanelsToRestore);
}

bool MapWindow::infoPanelsVisible() const
{
  return dockGroupVisible(infoPanelIds());
}

void MapWindow::toggleInspectorPanels()
{
  toggleDockGroup(inspectorPanelIds(), m_inspectorPanelsToRestore);
}

bool MapWindow::inspectorPanelsVisible() const
{
  return dockGroupVisible(inspectorPanelIds());
}

void MapWindow::toggleDockGroup(
  const std::vector<QString>& panelIds, DockGroupRestoreSet& restoreSet)
{
  if (dockGroupVisible(panelIds))
  {
    // remember the open docks to bring back, and which of them could be seen
    restoreSet = {};
    for (const auto& panelId : panelIds)
    {
      const auto* dock = sidebarDock(*m_sidebarManager, panelId);
      if (!dock->isHidden())
      {
        auto& panelIdsOfKind =
          dock->toggleViewAction()->isChecked() ? restoreSet.front : restoreSet.behind;
        panelIdsOfKind.push_back(panelId);
      }
    }

    for (const auto& panelId : panelIds)
    {
      sidebarDock(*m_sidebarManager, panelId)->hide();
    }
  }
  else
  {
    auto panelIdsToShow = restoreSet.front;
    panelIdsToShow.insert(
      panelIdsToShow.end(), restoreSet.behind.begin(), restoreSet.behind.end());
    if (panelIdsToShow.empty())
    {
      panelIdsToShow = panelIds;
    }

    for (const auto& panelId : panelIdsToShow)
    {
      sidebarDock(*m_sidebarManager, panelId)->show();
    }

    // a dock shown into its tab group does not come to the front, and raising each in
    // turn would leave the last one in front, so only the docks that were in front are
    // raised, after all are shown; they are in different groups. Without them, each
    // group keeps its current tab.
    for (const auto& panelId : restoreSet.front)
    {
      sidebarDock(*m_sidebarManager, panelId)->raise();
    }

    restoreSet = {};
  }
}

bool MapWindow::dockGroupVisible(const std::vector<QString>& panelIds) const
{
  // by the hidden flag, so that it also holds while the window is not shown yet
  return std::ranges::any_of(panelIds, [&](const auto& panelId) {
    return !sidebarDock(*m_sidebarManager, panelId)->isHidden();
  });
}

void MapWindow::toggleMaximizeCurrentView()
{
  m_mapView->toggleMaximizeCurrentView();
}

bool MapWindow::currentViewMaximized() const
{
  return m_mapView->currentViewMaximized();
}

void MapWindow::showCompileDialog()
{
  if (!m_compilationDialog)
  {
    const auto& camera = m_mapView->perspectiveCamera();

    m_compilationDialog =
      new CompilationDialog{m_appController, *m_document, camera, this};
    connect(
      m_compilationDialog,
      &CompilationDialog::compilationProfileStarted,
      this,
      [this](const std::string& profileName) {
        setLastCompilationProfileName(profileName);
      });
  }

  if (const auto* profile = lastCompilationProfile())
  {
    m_compilationDialog->selectProfile(*profile);
  }
  else
  {
    m_compilationDialog->selectFirstProfile();
  }

  showModelessDialog(m_compilationDialog);
}

void MapWindow::rerunLastCompilation()
{
  if (m_lastCompilationProfileName)
  {
    showCompileDialog();

    if (const auto* profile = lastCompilationProfile())
    {
      m_compilationDialog->selectProfile(*profile);
      m_compilationDialog->runSelectedProfile();
    }
  }
}

bool MapWindow::hasLastCompilationProfile() const
{
  return lastCompilationProfile() != nullptr;
}

const mdl::CompilationProfile* MapWindow::lastCompilationProfile() const
{
  if (m_lastCompilationProfileName)
  {
    const auto& compilationProfiles =
      m_document->map().gameInfo().compilationConfig.profiles;

    if (const auto iLastCompilationProfile = std::ranges::find_if(
          compilationProfiles,
          [&](const auto& profile) {
            return profile.name == *m_lastCompilationProfileName;
          });
        iLastCompilationProfile != compilationProfiles.end())
    {
      return &*iLastCompilationProfile;
    }
  }

  return nullptr;
}

void MapWindow::setLastCompilationProfileName(std::string name)
{
  m_lastCompilationProfileName = std::move(name);

  const auto key =
    lastCompilationProfileSettingsKey(m_document->map().gameInfo().gameConfig.name);

  auto settings = QSettings{};
  settings.setValue(key, QString::fromStdString(*m_lastCompilationProfileName));

  updateRerunAction();
}

void MapWindow::loadLastCompilationProfileName()
{
  const auto key =
    lastCompilationProfileSettingsKey(m_document->map().gameInfo().gameConfig.name);

  const auto settings = QSettings{};
  const auto value = settings.value(key);

  m_lastCompilationProfileName =
    value.isValid() ? std::optional{value.toString().toStdString()} : std::nullopt;

  updateRerunAction();
}

void MapWindow::updateRerunAction()
{
  if (m_rerunAction)
  {
    m_rerunAction->setText(
      m_lastCompilationProfileName
        ? tr("Re-run \"%1\"...")
            .arg(QString::fromStdString(*m_lastCompilationProfileName))
        : tr("Re-run compilation..."));
  }

  updateActionState();
}

bool MapWindow::closeCompileDialog()
{
  if (!m_compilationDialog)
  {
    return true;
  }

  if (m_compilationDialog->close())
  {
    m_compilationDialog = nullptr;
    return true;
  }

  return false;
}

void MapWindow::showLaunchEngineDialog()
{
  auto dialog = LaunchGameEngineDialog{m_appController, document(), this};
  dialog.exec();
}

namespace
{

const gl::Material* materialToReveal(const mdl::Map& map)
{
  const auto& selection = map.selection();

  const auto& allBrushFaces = selection.allBrushFaces();
  const auto* firstMaterial = allBrushFaces.front().face().material();
  const auto allFacesHaveIdenticalMaterial = std::ranges::all_of(
    allBrushFaces,
    [&](const auto& face) { return face.face().material() == firstMaterial; });

  return allFacesHaveIdenticalMaterial ? firstMaterial : nullptr;
}

} // namespace

bool MapWindow::canRevealMaterial() const
{
  return materialToReveal(m_document->map()) != nullptr;
}

void MapWindow::revealMaterial()
{
  if (const auto* material = materialToReveal(m_document->map()))
  {
    revealMaterial(material);
  }
}

void MapWindow::revealMaterial(const gl::Material* material)
{
  showSidebarPanel(FaceInspector::PanelId);
  faceInspector()->revealMaterial(material);
}

void MapWindow::debugPrintVertices()
{
  const auto& selection = m_document->map().selection();
  if (selection.hasBrushFaces())
  {
    for (const auto& handle : selection.brushFaces)
    {
      auto str = std::stringstream{};
      str.precision(17);
      for (const auto* vertex : handle.face().vertices())
      {
        str << "(" << vertex->position() << ") ";
      }
      logger().info() << str.str();
    }
  }
  else if (selection.hasBrushes())
  {
    for (const auto* brushNode : selection.brushes)
    {
      const auto& brush = brushNode->brush();

      auto str = std::stringstream{};
      str.precision(17);
      for (const auto* vertex : brush.vertices())
      {
        str << vertex->position() << " ";
      }
      logger().info() << str.str();
    }
  }
}

void MapWindow::debugCreateBrush()
{
  auto ok = false;
  const auto str = QInputDialog::getText(
    this,
    "Create Brush",
    "Enter a list of at least 4 points (x y z) (x y z) ...",
    QLineEdit::Normal,
    "",
    &ok);
  if (ok)
  {
    auto positions = std::vector<vm::vec3d>{};
    vm::parse_all<double, 3>(str.toStdString(), std::back_inserter(positions));

    createBrush(m_document->map(), positions);
  }
}

void MapWindow::debugCreateCube()
{
  auto ok = false;
  const auto str = QInputDialog::getText(
    this, "Create Cube", "Enter bounding box size", QLineEdit::Normal, "", &ok);
  if (ok)
  {
    const auto size = str.toDouble();
    const auto bounds = vm::bbox3d{size / 2.0};
    const auto positions = bounds.vertices() | kdl::ranges::to<std::vector>();

    createBrush(m_document->map(), positions);
  }
}

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wold-style-cast"
#pragma clang diagnostic ignored "-Wcast-qual"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
static void debugSegfault()
{
  volatile void* test = nullptr;
  printf("%p\n", *((void**)test));
}
#ifdef __clang__
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

[[noreturn]] static void debugException()
{
  const auto e = std::runtime_error{"Let's crash!"};
  throw e;
}

void MapWindow::debugCrash()
{
  auto items = QStringList{};
  items << "Null pointer dereference"
        << "Unhandled exception"
        << "Contract failed";

  bool ok;
  const auto item =
    QInputDialog::getItem(this, "Crash", "Choose a crash type", items, 0, false, &ok);
  if (ok)
  {
    const auto idx = items.indexOf(item);
    if (idx == 0)
    {
      debugSegfault();
    }
    else if (idx == 1)
    {
      debugException();
    }
    else if (idx == 2)
    {
      contract_assert(false);
    }
  }
}

void MapWindow::debugThrowExceptionDuringCommand()
{
  auto& map = m_document->map();
  map.throwExceptionDuringCommand();
}

void MapWindow::debugSetWindowSize()
{
  auto ok = false;
  const auto str = QInputDialog::getText(
    this, "Window Size", "Enter Size (W H)", QLineEdit::Normal, "1920 1080", &ok);
  if (ok)
  {
    if (const auto size = parse<int, 2>(str))
    {
      resize(size->x(), size->y());
    }
  }
}

void MapWindow::debugShowPalette()
{
  auto* window = new DebugPaletteWindow{this};
  showModelessDialog(window);
}

void MapWindow::focusChange(QWidget* /* oldFocus */, QWidget* newFocus)
{
  if (auto* newMapView = dynamic_cast<MapViewBase*>(newFocus))
  {
    m_currentMapView = newMapView;
  }

  updateActionState();
  updateUndoRedoActions();
}

MapViewBase* MapWindow::currentMapViewBase()
{
  if (!m_currentMapView)
  {
    // This happens when the current map view is deleted (e.g. 4-pane to 1-pane layout)
    m_currentMapView = m_mapView->firstMapViewBase();

    // SwitchableMapViewContainer should have constructed a MapViewBase
    contract_assert(m_currentMapView != nullptr);
  }
  return m_currentMapView;
}

bool MapWindow::canCompile() const
{
  const auto& map = m_document->map();
  return map.persistent() && !m_prefabEditor;
}

bool MapWindow::canLaunch() const
{
  const auto& map = m_document->map();
  return map.persistent() && !m_prefabEditor;
}

void MapWindow::updatePrefabMetadata()
{
  // DD-30: _tb_prefab_saved keeps the time of the snapshot, so a save of an unmodified
  // prefab writes the file as it was and adds no undo step
  auto& map = m_document->map();
  if (!map.modified())
  {
    return;
  }

  auto& worldNode = map.worldNode();
  const auto& worldEntity = worldNode.entity();
  const auto* kind = worldEntity.property(mdl::PrefabPropertyKeys::Kind);
  if (!kind || *kind != mdl::PrefabPropertyValues::KindEntity)
  {
    // only an entity snapshot has a key that an edit can make stale
    return;
  }

  const auto* entityNode = singleBodyEntity(worldNode);
  if (!entityNode)
  {
    logger().warn() << "Prefab " << map.filename()
                    << " is an entity snapshot, but it no longer holds exactly one "
                       "entity";
    return;
  }

  // R1-pf3-11: the class that savePrefab wrote, which a plain save would keep as it was
  const auto& className = entityNode->entity().classname();
  if (const auto* oldClassName = worldEntity.property(mdl::PrefabPropertyKeys::Class);
      oldClassName && *oldClassName == className)
  {
    return;
  }

  auto newWorldEntity = worldEntity;
  newWorldEntity.addOrUpdateProperty(mdl::PrefabPropertyKeys::Class, className);

  auto transaction = mdl::Transaction{map, "Update Prefab Metadata"};
  if (!mdl::updateNodeContents(
        map,
        "Update Prefab Metadata",
        {{&worldNode, mdl::NodeContents{std::move(newWorldEntity)}}}))
  {
    transaction.cancel();
    return;
  }
  transaction.commit();
}

void MapWindow::finishPrefabSave()
{
  if (const auto mapPath = std::exchange(m_pendingPrefabSavePath, std::nullopt))
  {
    // the thumbnail first, so that the library reads it with the file
    writePrefabThumbnail(*mapPath);
    m_appController.mapWindowManager().prefabWasSaved(*this, *mapPath);
  }
}

void MapWindow::writePrefabThumbnail(const std::filesystem::path& mapPath)
{
  const auto thumbnailPath = kdl::path_replace_extension(mapPath, ".png");

  auto image = m_thumbnailGrabber ? m_thumbnailGrabber() : grabThumbnail();
  if (image.isNull())
  {
    const auto message = tr("No thumbnail saved: no 3D view is shown");
    logger().info() << message.toStdString();
    statusBar()->showMessage(message, 5000);
    return;
  }

  if (image.width() > ThumbnailSize || image.height() > ThumbnailSize)
  {
    image = image.scaled(
      ThumbnailSize, ThumbnailSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  }

  // written beside the file and renamed, so that the library never reads half of it;
  // not with WritableFileSystem::createFileAtomic, which writes text (H-PF3-5)
  auto file = QSaveFile{pathAsQString(thumbnailPath)};
  auto error = std::optional<QString>{};
  if (!file.open(QIODevice::WriteOnly))
  {
    error = file.errorString();
  }
  else if (!image.save(&file, "PNG"))
  {
    // R1-pf3-7: the file's error, if writing failed, or else the encoder's, which says
    // nothing
    error =
      file.error() != QFileDevice::NoError ? file.errorString() : tr("PNG encode failed");
    file.cancelWriting();
  }
  else if (!file.commit())
  {
    error = file.errorString();
  }

  if (error)
  {
    const auto message = tr("Could not write thumbnail %1: %2")
                           .arg(pathAsQString(thumbnailPath.filename()), *error);
    logger().error() << message.toStdString();
    statusBar()->showMessage(message, 5000);
    return;
  }
  logger().info() << "Saved thumbnail " << thumbnailPath;
}

QImage MapWindow::grabThumbnail()
{
  // one pane cycles through its views, so the 3D view may be there but not shown
  const auto views = m_mapView->findChildren<MapView3D*>();
  const auto it =
    std::ranges::find_if(views, [](const auto* view) { return view->isVisible(); });
  return it != views.end() ? (*it)->grabFramebuffer() : QImage{};
}

QMenu* MapWindow::createPopupMenu()
{
  auto* menu = QMainWindow::createPopupMenu();
  if (menu)
  {
    // Qt lists the docks in the order they were created, then a separator and the
    // toolbars; list the docks in the order of the tool sidebar and View > Sidebar
    const auto actions = menu->actions();
    const auto iSeparator = std::ranges::find_if(
      actions, [](const auto* action) { return action->isSeparator(); });
    if (iSeparator != actions.end())
    {
      auto dockActions = std::vector<QAction*>(actions.begin(), iSeparator);
      std::ranges::stable_sort(dockActions, [](const auto* lhs, const auto* rhs) {
        return toolSidebarEntryLess(*lhs, *rhs);
      });
      for (auto* action : dockActions)
      {
        menu->removeAction(action);
      }
      menu->insertActions(
        *iSeparator, QList<QAction*>(dockActions.begin(), dockActions.end()));
    }
  }
  return menu;
}

void MapWindow::dragEnterEvent(QDragEnterEvent* event)
{
  const auto& map = m_document->map();
  if (
    map.gameInfo().gameConfig.materialConfig.property && event->mimeData()->hasUrls()
    && std::ranges::all_of(event->mimeData()->urls(), [](const auto& url) {
         if (!url.isLocalFile())
         {
           return false;
         }

         const auto fileInfo = QFileInfo{url.toLocalFile()};
         return fileInfo.isFile() && fileInfo.fileName().toLower().endsWith(".wad");
       }))
  {
    event->accept();
  }
}

void MapWindow::dropEvent(QDropEvent* event)
{
  const auto urls = event->mimeData()->urls();
  if (urls.empty())
  {
    return;
  }

  auto pathQStrs = QStringList{};
  pathQStrs.reserve(urls.size());
  for (const auto& url : urls)
  {
    pathQStrs.push_back(url.toLocalFile());
  }

  if (addWadPaths(pathQStrs, m_document->map(), this))
  {
    event->acceptProposedAction();
  }
}

void MapWindow::changeEvent(QEvent*)
{
  if (m_mapView)
  {
    m_mapView->windowActivationStateChanged(isActiveWindow());
  }
}

void MapWindow::closeEvent(QCloseEvent* event)
{
  if (!closeCompileDialog() || !confirmOrDiscardChanges())
  {
    event->ignore();
    return;
  }

  // R1-pf3-10: the thumbnail of a save that the close asked for, while the window is
  // still shown
  finishPrefabSave();

  disconnect(qApp, &QApplication::focusChanged, this, &MapWindow::focusChange);

  saveWindowState();

  m_appController.mapWindowManager().removeMapWindow(this);
  event->accept();
  // Don't call superclass implementation
}

template <typename F>
static void applyRecursively(QObject* object, const F& f)
{
  f(object);
  for (auto* child : object->children())
  {
    applyRecursively(child, f);
  }
}

bool MapWindow::eventFilter(QObject* target, QEvent* event)
{
  if (
    event->type() == QEvent::MouseButtonPress
    || event->type() == QEvent::MouseButtonRelease
    || event->type() == QEvent::MouseButtonDblClick || event->type() == QEvent::MouseMove
    || event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease)
  {
    m_lastInputTime = std::chrono::system_clock::now();
  }
  else if (event->type() == QEvent::ChildAdded)
  {
    auto* childEvent = static_cast<QChildEvent*>(event);
    applyRecursively(
      childEvent->child(), [&](auto* object) { object->installEventFilter(this); });
  }
  else if (event->type() == QEvent::ChildRemoved)
  {
    auto* childEvent = static_cast<QChildEvent*>(event);
    applyRecursively(
      childEvent->child(), [&](auto* object) { object->removeEventFilter(this); });
  }
  return QMainWindow::eventFilter(target, event);
}

void MapWindow::triggerAutosave()
{
  using namespace std::chrono_literals;
  if (
    QGuiApplication::mouseButtons() == Qt::NoButton
    && std::chrono::system_clock::now() - m_lastInputTime > 2s)
  {
    m_document->triggerAutosave();
  }
}

// DebugPaletteWindow

DebugPaletteWindow::DebugPaletteWindow(QWidget* parent)
  : QDialog(parent)
{
  setWindowTitle(tr("Palette"));

  const auto roles = std::vector<std::pair<QPalette::ColorRole, QString>>{
    {QPalette::Window, "Window"},
    {QPalette::WindowText, "WindowText"},
    {QPalette::Base, "Base"},
    {QPalette::AlternateBase, "AlternateBase"},
    {QPalette::ToolTipBase, "ToolTipBase"},
    {QPalette::ToolTipText, "ToolTipText"},
    {QPalette::PlaceholderText, "PlaceholderText"},
    {QPalette::Text, "Text"},
    {QPalette::Button, "Button"},
    {QPalette::ButtonText, "ButtonText"},
    {QPalette::BrightText, "BrightText"},
    {QPalette::Light, "Light"},
    {QPalette::Midlight, "Midlight"},
    {QPalette::Dark, "Dark"},
    {QPalette::Mid, "Mid"},
    {QPalette::Shadow, "Shadow"},
    {QPalette::Highlight, "Highlight"},
    {QPalette::HighlightedText, "HighlightedText"}};

  const auto groups = std::vector<std::pair<QPalette::ColorGroup, QString>>{
    {QPalette::Disabled, "Disabled"},
    {QPalette::Active, "Active"},
    {QPalette::Inactive, "Inactive"}};

  auto verticalHeaderLabels = QStringList{};
  for (const auto& [role, roleLabel] : roles)
  {
    verticalHeaderLabels.append(roleLabel);
  }

  auto horizontalHeaderLabels = QStringList{};
  for (const auto& [group, groupLabel] : groups)
  {
    horizontalHeaderLabels.append(groupLabel);
  }

  auto* table = new QTableWidget{int(roles.size()), int(groups.size())};
  table->setHorizontalHeaderLabels(horizontalHeaderLabels);
  table->setVerticalHeaderLabels(verticalHeaderLabels);

  for (int x = 0; x < table->columnCount(); ++x)
  {
    for (int y = 0; y < table->rowCount(); ++y)
    {
      const auto role = roles.at(size_t(y)).first;
      const auto group = groups.at(size_t(x)).first;

      auto* button = new ColorButton{};
      table->setCellWidget(y, x, button);

      button->setColor(qApp->palette().color(group, role));

      connect(button, &ColorButton::colorChangedByUser, this, [=](const QColor& color) {
        auto palette = qApp->palette();
        palette.setColor(group, role, color);
        qApp->setPalette(palette);
      });
    }
  }

  auto* layout = new QVBoxLayout{};
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  layout->addWidget(table);
  setLayout(layout);
}

DebugPaletteWindow::~DebugPaletteWindow() = default;

} // namespace tb::ui
