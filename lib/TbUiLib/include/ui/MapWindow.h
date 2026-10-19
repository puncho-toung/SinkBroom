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

#pragma once

#include <QDialog>
#include <QMainWindow>
#include <QPointer>
#include <QString>

#include "base/NotifierConnection.h"
#include "mdl/ExportOptions.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class QAction;
class QComboBox;
class QDropEvent;
class QMenuBar;
class QLabel;
class QTimer;
class QToolBar;

namespace tb
{
class Logger;

namespace gl
{
class ContextManager;
class Material;
class ResourceId;
} // namespace gl

namespace mdl
{
class GroupNode;
class LayerNode;
class Map;
class Node;

enum class PasteType;
enum class VisualEffect;

struct CompilationProfile;
struct SelectionChange;
} // namespace mdl

namespace ui
{
class Action;
class AppController;
class CompilationDialog;
class Console;
class FaceInspector;
class MapDocument;
class MapViewBase;
class MapViewToolBox;
class ObjExportDialog;
class SidebarManager;
class SignalDelayer;
class SwitchableMapViewContainer;
class Tool;
class ToolSidebar;

class MapWindow : public QMainWindow
{
  Q_OBJECT
private:
  AppController& m_appController;
  std::unique_ptr<MapDocument> m_document;

  std::chrono::time_point<std::chrono::system_clock> m_lastInputTime;
  QTimer* m_autosaveTimer = nullptr;

  QToolBar* m_toolBar = nullptr;
  ToolSidebar* m_toolSidebar = nullptr;

  SwitchableMapViewContainer* m_mapView = nullptr;
  /**
   * Last focused MapViewBase. It's a QPointer to handle changing from e.g. a 2-pane map
   * view to 1-pane.
   */
  QPointer<MapViewBase> m_currentMapView;
  Console* m_console = nullptr;
  SidebarManager* m_sidebarManager = nullptr;

  /**
   * The panels of a dock group that a toggle hid and brings back. A dock's toggle action
   * is checked while the dock can be seen, which holds for the dock in front of each tab
   * group and for a dock on its own, so several can be in front at once; they are raised
   * again after all are shown. None is in front while the window is not shown.
   */
  struct DockGroupRestoreSet
  {
    /** The panels whose toggle action was checked, raised last. */
    std::vector<QString> front;
    /** The other panels whose docks were not hidden. */
    std::vector<QString> behind;
  };

  DockGroupRestoreSet m_inspectorPanelsToRestore;
  DockGroupRestoreSet m_infoPanelsToRestore;

  /**
   * Whether restoreWindowState has applied the saved window state. It is applied once per
   * window, see restoreWindowState.
   */
  bool m_windowStateRestored = false;

  QComboBox* m_gridChoice = nullptr;
  QLabel* m_statusBarLabel = nullptr;

  QPointer<CompilationDialog> m_compilationDialog;
  QPointer<ObjExportDialog> m_objExportDialog;

  std::optional<std::string> m_lastCompilationProfileName;

  NotifierConnection m_notifierConnection;

private: // shortcuts
  using ActionMap = std::unordered_map<const Action*, QAction*>;
  ActionMap m_actionMap;

private: // special menu entries
  QMenu* m_recentDocumentsMenu = nullptr;
  QAction* m_undoAction = nullptr;
  QAction* m_redoAction = nullptr;
  QAction* m_rerunAction = nullptr;

private:
  SignalDelayer* m_updateTitleSignalDelayer = nullptr;
  SignalDelayer* m_updateActionStateSignalDelayer = nullptr;
  SignalDelayer* m_updateStatusBarSignalDelayer = nullptr;

public:
  MapWindow(AppController& appController, std::unique_ptr<MapDocument> document);
  ~MapWindow() override;

  void positionOnScreen(QWidget* reference);

  /**
   * Saves the window's geometry and layout, the docks included, as closing it does.
   */
  void saveWindowState();

  const MapDocument& document() const;
  MapDocument& document();

  const MapViewToolBox& toolBox() const;
  MapViewToolBox& toolBox();

public: // getters and such
  Logger& logger() const;
  QAction* findAction(const std::filesystem::path& path);

private: // title bar contents
  void updateTitle();
  void updateTitleDelayed();

private: // menu bar
  void createMenus();
  void updateShortcuts();
  void updateActionState();
  void updateActionStateDelayed();
  void updateUndoRedoActions();

  void addRecentDocumentsMenu();
  void removeRecentDocumentsMenu();
  void updateRecentDocumentsMenu();

private: // tool bar
  void createToolBar();
  void createToolSidebar();
  void updateToolBarWidgets();

private: // status bar
  void createStatusBar();
  void updateStatusBar();
  void updateStatusBarDelayed();

private: // gui creation
  void createGui();

  /**
   * Restores the saved layout, on the first call only; the arrangements below and the
   * docking of floating docks that are off screen run on every call. A layout saved
   * before the inspectors were docks does not know them, so they are arranged like the
   * old inspector once more; one saved before the Console and Issues panels were docks
   * does not know those, so they are arranged like the old info panel once more.
   */
  void restoreWindowState();
  /**
   * Tabs the three inspector docks together, in the order Map, Entity, Face, and brings
   * the given one to the front. Arranging docks that are arranged so changes nothing.
   */
  void arrangeInspectorDocks(const QString& raisedPanelId);
  /**
   * Tabs the Console and Issues docks together, in that order, and brings the given one
   * to the front. Arranging docks that are arranged so changes nothing.
   */
  void arrangeInfoDocks(const QString& raisedPanelId);
  FaceInspector* faceInspector() const;

  /**
   * Hides the given panels' docks if any of them is open, and remembers in the given set
   * which were open and which of them could be seen; otherwise brings back the
   * remembered ones, or all, and brings the ones that could be seen to the front again.
   */
  void toggleDockGroup(
    const std::vector<QString>& panelIds, DockGroupRestoreSet& restoreSet);
  /**
   * Whether any of the given panels' docks is open.
   */
  bool dockGroupVisible(const std::vector<QString>& panelIds) const;

private: // notification handlers
  void connectObservers();

  void documentWasLoaded();
  void documentWasSaved();
  void mapModificationStateDidChange();

  void transactionDone(const std::string& name, bool observable, bool isModification);
  void transactionUndone(const std::string& name, bool observable, bool isModification);

  void preferenceDidChange(const std::filesystem::path& path);
  void resourcesWereProcessed(const std::vector<gl::ResourceId>& resourceIds);

  void gridDidChange();
  void toolActivated(Tool& tool);
  void toolDeactivated(Tool& tool);
  void toolHandleSelectionChanged(Tool& tool);
  void selectionDidChange(const mdl::SelectionChange& selectionChange);
  void currentLayerDidChange();
  void groupWasOpened();
  void groupWasClosed();
  void nodeVisibilityDidChange(const std::vector<mdl::Node*>& nodes);
  void editorContextDidChange();
  void triggerVisualEffect(mdl::VisualEffect visualEffect);

  void pointFileDidChange();
  void portalFileDidChange();

private: // menu event handlers
  void bindEvents();

public:
  bool saveDocument();
  bool saveDocumentAs();
  void revertDocument();
  bool exportDocumentAsObj();
  bool exportDocumentAsMap();
  bool exportDocument(const mdl::ExportOptions& options);

private:
  bool confirmOrDiscardChanges();
  bool confirmRevertDocument();

public:
  void loadPointFile();
  void reloadPointFile();
  void unloadPointFile();
  bool canReloadPointFile() const;
  bool canUnloadPortalFile() const;

  void loadPortalFile();
  void reloadPortalFile();
  void unloadPortalFile();
  bool canReloadPortalFile() const;
  bool canUnloadPointFile() const;

  void reloadMaterialCollections();
  void reloadEntityDefinitions();
  bool canReloadMaterialCollections() const;
  bool canReloadEntityDefinitions() const;

  void closeDocument();

  void undo();
  void redo();
  bool canUndo() const;
  bool canRedo() const;

  void repeatLastCommands();
  void clearRepeatableCommands();
  bool hasRepeatableCommands() const;

  void cutSelection();
  void copySelection();
  void copyToClipboard();
  bool canCutSelection() const;
  bool canCopySelection() const;

  void pasteAtCursorPosition();
  void pasteAtOriginalPosition();
  mdl::PasteType paste();
  bool canPaste() const;

  void duplicateSelection();
  bool canDuplicateSelection() const;

  void deleteSelection();
  bool canDeleteSelection() const;

  void saveSelectionAsPrefab();
  bool canSaveSelectionAsPrefab() const;
  void editPrefab(const std::filesystem::path& path);

  void selectAll();
  void selectSiblings();
  void selectTouching();
  void selectInside();
  void selectTall();
  void selectByLineNumber();
  void selectInverse();
  void selectNone();

  bool canSelect() const;
  bool canSelectSiblings() const;
  bool canSelectByBrush() const;
  bool canSelectTall() const;
  bool canDeselect() const;
  bool canChangeSelection() const;
  bool canSelectInverse() const;

  void groupSelectedObjects();
  bool canGroupSelectedObjects() const;

  void ungroupSelectedObjects();
  bool canUngroupSelectedObjects() const;

  void renameSelectedGroups();
  bool canRenameSelectedGroups() const;

  void moveSelectedObjects();
  bool canMoveSelectedObjects() const;

  void csgConvexMerge();
  bool canDoCsgConvexMerge() const;

  void csgSubtract();
  bool canDoCsgSubtract() const;

  void csgHollow();
  bool canDoCsgHollow() const;

  void csgIntersect();
  bool canDoCsgIntersect() const;

  void snapVerticesToInteger();
  void snapVerticesToGrid();
  bool canSnapVertices() const;

  void convertSelectionToPatches();
  bool canConvertSelectionToPatches() const;

  void replaceMaterial();

  void toggleAlignmentLock();
  void toggleUvLock();

  void toggleShowGrid();
  void toggleSnapToGrid();

  void incGridSize();
  bool canIncGridSize() const;

  void decGridSize();
  bool canDecGridSize() const;

  void setGridSize(int size);

  void moveCameraToNextPoint();
  bool canMoveCameraToNextPoint() const;

  void moveCameraToPreviousPoint();
  bool canMoveCameraToPreviousPoint() const;

  void reset2dCameras();

  void focusCameraOnSelection();
  bool canFocusCamera() const;

  void moveCameraToPosition();

  void isolateSelection();
  bool canIsolateSelection() const;

  void hideSelection();
  bool canHideSelection() const;

  void showAll();

  /**
   * Shows the given sidebar panel's dock and brings it to the front of its tab group.
   */
  void showSidebarPanel(const QString& panelId);

  void toggleToolbar();
  bool toolbarVisible() const;

  void toggleToolSidebar();
  bool toolSidebarVisible() const;

  /**
   * Hides the Console and Issues docks if either of them is open; otherwise brings back
   * the ones that it hid, or both, with the one that was in front in front again.
   */
  void toggleInfoPanels();
  bool infoPanelsVisible() const;

  /**
   * Hides the three inspector docks if any of them is open; otherwise brings back the
   * ones that it hid, or all three, with the one that was in front in front again.
   */
  void toggleInspectorPanels();
  bool inspectorPanelsVisible() const;

  void toggleMaximizeCurrentView();
  bool currentViewMaximized() const;

  void showCompileDialog();
  bool closeCompileDialog();
  void rerunLastCompilation();
  bool hasLastCompilationProfile() const;

  void showLaunchEngineDialog();

  bool canRevealMaterial() const;
  void revealMaterial();

  void revealMaterial(const gl::Material* material);

  void debugPrintVertices();
  void debugCreateBrush();
  void debugCreateCube();
  void debugCrash();
  void debugThrowExceptionDuringCommand();
  void debugSetWindowSize();
  void debugShowPalette();

  void focusChange(QWidget* oldFocus, QWidget* newFocus);

  MapViewBase* currentMapViewBase();

private:
  const mdl::CompilationProfile* lastCompilationProfile() const;
  void setLastCompilationProfileName(std::string name);
  void loadLastCompilationProfileName();
  void updateRerunAction();

  bool canCompile() const;
  bool canLaunch() const;

public: // drag and drop
  void dragEnterEvent(QDragEnterEvent* event) override;
  void dropEvent(QDropEvent* event) override;

public: // the context menu of the toolbars and the docks
  QMenu* createPopupMenu() override;

protected: // other event handlers
  void changeEvent(QEvent* event) override;
  void closeEvent(QCloseEvent* event) override;

public: // event filter (suppress autosave for user input events)
  bool eventFilter(QObject* target, QEvent* event) override;

private:
  void triggerAutosave();
};

class DebugPaletteWindow : public QDialog
{
  Q_OBJECT
public:
  explicit DebugPaletteWindow(QWidget* parent = nullptr);
  ~DebugPaletteWindow() override;
};

} // namespace ui
} // namespace tb
