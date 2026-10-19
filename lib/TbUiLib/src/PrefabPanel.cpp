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

#include "ui/PrefabPanel.h"

#include <QComboBox>
#include <QDesktopServices>
#include <QFileSystemWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QVBoxLayout>

#include "base/Logger.h"
#include "base/PreferenceManager.h"
#include "fs/DiskIO.h"
#include "mdl/GameInfo.h"
#include "mdl/GroupNode.h"
#include "mdl/Map.h"
#include "mdl/PrefabLibrary.h"
#include "mdl/PrefabRefresh.h"
#include "mdl/Selection.h"
#include "ui/BitmapButton.h"
#include "ui/MapDocument.h"
#include "ui/MapWindow.h"
#include "ui/PrefabBrowserView.h"
#include "ui/QPathUtils.h"
#include "ui/SavePrefabDialog.h"
#include "ui/SearchBox.h"
#include "ui/Splitter.h"
#include "ui/ViewConstants.h"

#include "kd/path_utils.h"
#include "kd/result.h"
#include "kd/string_format.h"
#include "kd/string_utils.h"

#include <fmt/format.h>

#include <chrono>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <utility>

namespace tb::ui
{
namespace
{

using namespace std::chrono_literals;

// how often a running scan is polled for finished files
constexpr auto ScanPollInterval = 50ms;

// how long the library's folders must be quiet before a change in them rescans
constexpr auto WatchDebounceInterval = 500ms;

// the category tree's item data for "All"
const auto AllCategories = QVariant{};

// the role of a category tree item's folder, a generic path relative to the roots
constexpr auto CategoryRole = Qt::UserRole;

// C1: <stem>.png beside <stem>.map
std::filesystem::path thumbnailPathFor(const std::filesystem::path& path)
{
  return kdl::path_replace_extension(path, ".png");
}

bool hasWriteRoot(const mdl::PrefabLibrary& library)
{
  return !library.writeRootFor(mdl::PrefabRoot::User).empty()
         || !library.writeRootFor(mdl::PrefabRoot::Mod).empty();
}

/**
 * The map window the panel's dock belongs to; a floating dock keeps its window as its
 * parent.
 */
MapWindow* findMapWindow(QWidget* widget)
{
  for (auto* parent = widget; parent; parent = parent->parentWidget())
  {
    if (auto* mapWindow = dynamic_cast<MapWindow*>(parent))
    {
      return mapWindow;
    }
  }
  return nullptr;
}

const mdl::PrefabEntry* findEntry(
  const mdl::PrefabLibrary& library, const std::filesystem::path& absolutePath)
{
  for (const auto& entry : library.entries())
  {
    if (entry->absolutePath == absolutePath)
    {
      return entry.get();
    }
  }
  return nullptr;
}

QString describePlacement(const mdl::PrefabPlacement& placement)
{
  const auto path = QString::fromStdString(placement.prefabPath);
  if (!placement.entry)
  {
    return QObject::tr("%1 (missing)").arg(path);
  }
  const auto& name = placement.entry->metadata.name;
  return name.empty() ? path : QString{"%1 (%2)"}.arg(QString::fromStdString(name), path);
}

bool hasPlacements(const mdl::PrefabLibrary& library)
{
  return !library.placements().empty();
}

} // namespace

PrefabPanel::PrefabPanel(const SidebarContext& context, QWidget* parent)
  : SidebarPanel{parent}
  , m_appController{context.appController}
  , m_document{context.document}
  , m_scanTimer{new QTimer{this}}
  , m_watcher{new QFileSystemWatcher{this}}
  , m_watchTimer{new QTimer{this}}
{
  createGui();
  connectObservers();
  connectLibraryObserver();
  updateSaveButton();
  updateRefreshButton();
  updateApplyButton();
  updateEmptyState();

  // the panel is hidden by default; the first scan waits until it is shown, unless the
  // map holds placed prefabs, which are checked for changed files (D17)
  m_promptAfterScan = hasPlacements(m_document.map().prefabLibrary());
  if (m_promptAfterScan)
  {
    scanOnce();
  }
}

PrefabPanel::~PrefabPanel() = default;

QString PrefabPanel::panelId() const
{
  return QString::fromLatin1(PanelId);
}

QString PrefabPanel::title() const
{
  return tr("Prefabs");
}

std::filesystem::path PrefabPanel::iconPath() const
{
  return "Prefabs.svg";
}

void PrefabPanel::panelShown()
{
  if (m_scanStarted)
  {
    processPendingResults();
  }
  else
  {
    startScan();
  }

  // what happened while the panel was hidden
  if (m_refreshCountStale)
  {
    updateRefreshButton();
  }
  if (m_watchMissed)
  {
    m_watchMissed = false;
    rescan();
  }
}

void PrefabPanel::saveSelectionAsPrefab()
{
  if (!m_document.map().selection().hasNodes())
  {
    return;
  }

  // the dialog offers the library's categories
  scanOnce();

  auto dialog = SavePrefabDialog{m_document, this};
  if (dialog.exec() == QDialog::Accepted && dialog.savedPath())
  {
    prefabWasSaved(*dialog.savedPath());
  }
}

void PrefabPanel::prefabWasSaved(const std::filesystem::path& path)
{
  // the other entries, and their previews, are kept
  m_document.map().prefabLibrary().updateEntry(path);
  processPendingResults();
}

void PrefabPanel::scanOnce()
{
  if (!m_scanStarted)
  {
    startScan();
  }
}

void PrefabPanel::rescan()
{
  if (m_scanStarted && !m_rescanPending)
  {
    m_rescanPending = true;
    QTimer::singleShot(0, this, [this]() {
      m_rescanPending = false;
      startScan();
    });
  }
}

std::size_t PrefabPanel::scanCount() const
{
  return m_scanCount;
}

std::size_t PrefabPanel::changedPlacementCount() const
{
  return m_changedPlacementCount;
}

void PrefabPanel::promptToRefreshPlacements(
  const std::vector<mdl::PrefabPlacement>& placements)
{
  if (placements.empty())
  {
    return;
  }

  // a placement is found again by its path and link ID before it is refreshed, since
  // refreshing one replaces nodes that another may point to
  auto keys = std::vector<std::pair<std::string, std::string>>{};
  auto lines = QStringList{};
  for (const auto& placement : placements)
  {
    lines.push_back(describePlacement(placement));
    if (placement.entry)
    {
      keys.emplace_back(placement.prefabPath, placement.firstMember->linkId());
    }
  }

  const auto count = placements.size();
  auto messageBox = QMessageBox{this};
  messageBox.setIcon(QMessageBox::Question);
  messageBox.setWindowTitle(tr("Prefabs"));
  messageBox.setText(QString::fromStdString(fmt::format(
    "{} {} changed on disk. Update?",
    count,
    kdl::str_plural(count, "prefab", "prefabs"))));
  messageBox.setInformativeText(lines.join(QChar{'\n'}));
  auto* updateButton = messageBox.addButton(tr("Update"), QMessageBox::AcceptRole);
  messageBox.addButton(tr("Not now"), QMessageBox::RejectRole);
  updateButton->setEnabled(!keys.empty());
  messageBox.exec();
  if (messageBox.clickedButton() != updateButton)
  {
    return;
  }

  auto& map = m_document.map();
  auto lostEntities = QStringList{};
  auto errors = QStringList{};
  for (const auto& [prefabPath, linkId] : keys)
  {
    const auto current = map.prefabLibrary().placements();
    const auto it = std::ranges::find_if(current, [&](const auto& placement) {
      return placement.prefabPath == prefabPath
             && placement.firstMember->linkId() == linkId;
    });
    if (it == current.end())
    {
      continue;
    }

    mdl::refreshPlacement(map, *it) | kdl::transform([&](const auto& report) {
      for (const auto& name : report.lostEntities)
      {
        lostEntities.push_back(QString{"%1: %2"}
                                 .arg(QString::fromStdString(prefabPath))
                                 .arg(QString::fromStdString(name)));
      }
    }) | kdl::transform_error([&](const auto& e) {
      map.logger().error() << e.msg;
      errors.push_back(QString::fromStdString(e.msg));
    });
  }

  updateRefreshButton();

  if (!errors.empty())
  {
    QMessageBox::warning(this, tr("Prefabs"), errors.join(QChar{'\n'}));
  }
  if (!lostEntities.empty())
  {
    auto lostBox = QMessageBox{this};
    lostBox.setIcon(QMessageBox::Information);
    lostBox.setWindowTitle(tr("Prefabs"));
    // the prefab file has no entity of the same class at the same place in the group
    const auto lostCount = std::size_t(lostEntities.size());
    lostBox.setText(QString::fromStdString(fmt::format(
      "{} {} lost the overrides of every copy:",
      lostCount,
      kdl::str_plural(lostCount, "entity", "entities"))));
    lostBox.setInformativeText(lostEntities.join(QChar{'\n'}));
    lostBox.exec();
  }
}

void PrefabPanel::applyToSelection()
{
  auto& map = m_document.map();
  if (const auto* entry = m_view->selectedEntry())
  {
    mdl::applyPrefabToSelection(map, *entry) | kdl::transform([](const auto&) {})
      | kdl::transform_error([&](const auto& e) { map.logger().error() << e.msg; });
  }
}

void PrefabPanel::updatePrefabFromSelection(const std::filesystem::path& path)
{
  const auto& map = m_document.map();
  const auto* entry = findEntry(map.prefabLibrary(), path);
  if (!entry || !map.selection().hasNodes())
  {
    return;
  }

  auto dialog = SavePrefabDialog{m_document, this};
  dialog.forEntry(*entry);
  if (dialog.exec() == QDialog::Accepted && dialog.savedPath())
  {
    prefabWasSaved(*dialog.savedPath());
  }
}

Result<void> PrefabPanel::deletePrefabFile(const std::filesystem::path& path)
{
  // the thumbnail goes with the file; a missing one is no error
  auto result =
    fs::Disk::deleteFile(path)
    | kdl::and_then([&](auto) { return fs::Disk::deleteFile(thumbnailPathFor(path)); })
    | kdl::transform([](auto) {});

  // the file may be gone even if its thumbnail is not
  rescan();
  return result;
}

Result<void> PrefabPanel::removePrefabThumbnail(const std::filesystem::path& path)
{
  auto result =
    fs::Disk::deleteFile(thumbnailPathFor(path)) | kdl::transform([](auto) {});

  // the entry learns that it has no thumbnail, and its cell draws the prefab
  rescan();
  return result;
}

void PrefabPanel::createGui()
{
  m_categoryTree = new QTreeWidget{};
  m_categoryTree->setObjectName("PrefabPanel_categoryTree");
  m_categoryTree->setToolTip(
    tr("Show the prefabs of one folder and the folders below it"));
  m_categoryTree->setHeaderHidden(true);
  m_categoryTree->setUniformRowHeights(true);
  m_categoryTree->setSelectionMode(QAbstractItemView::SingleSelection);
  connect(
    m_categoryTree, &QTreeWidget::currentItemChanged, this, &PrefabPanel::updateFilter);

  m_kindChoice = new QComboBox{};
  m_kindChoice->setObjectName("PrefabPanel_kindChoice");
  m_kindChoice->setToolTip(tr("Show entity snapshots, assemblies or both"));
  m_kindChoice->addItem(tr("All"), static_cast<int>(PrefabKindFilter::All));
  m_kindChoice->addItem(tr("Entities"), static_cast<int>(PrefabKindFilter::Entities));
  m_kindChoice->addItem(tr("Assemblies"), static_cast<int>(PrefabKindFilter::Assemblies));
  connect(
    m_kindChoice,
    QOverload<int>::of(&QComboBox::activated),
    this,
    &PrefabPanel::updateFilter);

  m_filterBox = createSearchBox();
  m_filterBox->setObjectName("PrefabPanel_filterBox");
  m_filterBox->setPlaceholderText(tr("Name, tag or class; tag:name for a whole tag"));
  m_filterBox->setToolTip(
    tr("Every word must be part of the name, a tag or the class; a word tag:name must be "
       "one of the tags, whole"));
  connect(m_filterBox, &QLineEdit::textEdited, this, &PrefabPanel::updateFilter);

  m_saveButton = createBitmapButton("Add.svg", tr("Save selection as prefab..."));
  m_saveButton->setObjectName("PrefabPanel_saveButton");
  connect(m_saveButton, &QToolButton::clicked, this, &PrefabPanel::saveSelectionAsPrefab);

  m_rescanButton = createBitmapButton("Refresh.svg", tr("Rescan the prefab folders"));
  m_rescanButton->setObjectName("PrefabPanel_rescanButton");
  connect(m_rescanButton, &QToolButton::clicked, this, [&]() {
    // the thumbnails of unchanged prefabs may have changed
    m_view->reloadThumbnails();
    rescan();
  });

  m_refreshButton = new QToolButton{};
  m_refreshButton->setObjectName("PrefabPanel_refreshButton");
  m_refreshButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
  connect(m_refreshButton, &QToolButton::clicked, this, [&]() {
    promptToRefreshPlacements(m_document.map().prefabLibrary().changedPlacements());
  });

  m_applyButton = new QToolButton{};
  m_applyButton->setObjectName("PrefabPanel_applyButton");
  m_applyButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
  m_applyButton->setText(tr("Apply to Selection"));
  connect(m_applyButton, &QToolButton::clicked, this, &PrefabPanel::applyToSelection);

  m_scrollBar = new QScrollBar{Qt::Vertical};
  m_view = new PrefabBrowserView{m_appController, m_scrollBar, m_document};
  connect(
    m_view, &PrefabBrowserView::editPrefabRequested, this, &PrefabPanel::editPrefab);
  connect(
    m_view,
    &PrefabBrowserView::selectedEntryChanged,
    this,
    &PrefabPanel::updateApplyButton);
  connect(
    m_view, &PrefabBrowserView::applyPrefabRequested, this, [&](const QString& path) {
      m_view->selectEntry(pathFromQString(path));
      applyToSelection();
    });
  connect(
    m_view, &PrefabBrowserView::updatePrefabRequested, this, [&](const QString& path) {
      updatePrefabFromSelection(pathFromQString(path));
    });
  connect(
    m_view, &PrefabBrowserView::refreshPrefabRequested, this, [&](const QString& path) {
      const auto absolutePath = pathFromQString(path);
      auto placements = m_document.map().prefabLibrary().changedPlacements();
      std::erase_if(placements, [&](const auto& placement) {
        return !placement.entry || placement.entry->absolutePath != absolutePath;
      });
      promptToRefreshPlacements(placements);
    });
  connect(
    m_view, &PrefabBrowserView::revealPrefabRequested, this, [&](const QString& path) {
      revealPrefabInFolder(pathFromQString(path));
    });
  connect(
    m_view, &PrefabBrowserView::openThumbnailRequested, this, [&](const QString& path) {
      openPrefabThumbnail(pathFromQString(path));
    });
  connect(
    m_view, &PrefabBrowserView::removeThumbnailRequested, this, [&](const QString& path) {
      confirmRemovePrefabThumbnail(pathFromQString(path));
    });
  connect(
    m_view, &PrefabBrowserView::deletePrefabRequested, this, [&](const QString& path) {
      confirmDeletePrefabFile(pathFromQString(path));
    });

  auto* controlLayout = new QHBoxLayout{};
  controlLayout->setContentsMargins(
    LayoutConstants::NarrowHMargin,
    LayoutConstants::NarrowVMargin,
    LayoutConstants::NarrowHMargin,
    LayoutConstants::NarrowVMargin);
  controlLayout->setSpacing(LayoutConstants::NarrowHMargin);
  controlLayout->addWidget(m_kindChoice, 0);
  controlLayout->addWidget(m_filterBox, 1);
  controlLayout->addWidget(m_saveButton, 0);
  controlLayout->addWidget(m_rescanButton, 0);

  auto* actionLayout = new QHBoxLayout{};
  actionLayout->setContentsMargins(
    LayoutConstants::NarrowHMargin,
    0,
    LayoutConstants::NarrowHMargin,
    LayoutConstants::NarrowVMargin);
  actionLayout->setSpacing(LayoutConstants::NarrowHMargin);
  actionLayout->addWidget(m_applyButton, 0);
  actionLayout->addStretch(1);
  actionLayout->addWidget(m_refreshButton, 0);

  auto* browserLayout = new QHBoxLayout{};
  browserLayout->setContentsMargins(0, 0, 0, 0);
  browserLayout->setSpacing(0);
  browserLayout->addWidget(m_view, 1);
  browserLayout->addWidget(m_scrollBar, 0);

  m_browserPage = new QWidget{};
  m_browserPage->setLayout(browserLayout);

  m_emptyLabel = new QLabel{};
  m_emptyLabel->setObjectName("PrefabPanel_emptyLabel");
  m_emptyLabel->setAlignment(Qt::AlignCenter);
  m_emptyLabel->setWordWrap(true);
  m_emptyLabel->setMargin(LayoutConstants::WideHMargin);

  m_browserStack = new QStackedWidget{};
  m_browserStack->addWidget(m_browserPage);
  m_browserStack->addWidget(m_emptyLabel);

  // the folders above the grid; dragging the handle up hides them
  auto* splitter = new Splitter{Qt::Vertical};
  splitter->setObjectName("PrefabPanel_splitter");
  splitter->addWidget(m_categoryTree);
  splitter->addWidget(m_browserStack);
  splitter->setCollapsible(1, false);
  splitter->setStretchFactor(0, 0);
  splitter->setStretchFactor(1, 1);
  splitter->setSizes({100, 300});

  auto* outerLayout = new QVBoxLayout{};
  outerLayout->setContentsMargins(0, 0, 0, 0);
  outerLayout->setSpacing(0);
  outerLayout->addLayout(controlLayout, 0);
  outerLayout->addLayout(actionLayout, 0);
  outerLayout->addWidget(splitter, 1);
  setLayout(outerLayout);

  m_scanTimer->setInterval(ScanPollInterval);
  connect(m_scanTimer, &QTimer::timeout, this, &PrefabPanel::processPendingResults);

  // never refreshes the map, only rescans the library (D17)
  m_watchTimer->setSingleShot(true);
  m_watchTimer->setInterval(WatchDebounceInterval);
  connect(m_watchTimer, &QTimer::timeout, this, &PrefabPanel::rescan);
  connect(
    m_watcher,
    &QFileSystemWatcher::directoryChanged,
    this,
    &PrefabPanel::folderDidChange);
  connect(
    m_watcher, &QFileSystemWatcher::fileChanged, this, &PrefabPanel::folderDidChange);
}

void PrefabPanel::connectObservers()
{
  // the document outlives the map it holds; on load the panel connects to the new map's
  // library and reads the roots of the new map
  m_notifierConnection += m_document.documentWasLoadedNotifier.connect([&]() {
    connectLibraryObserver();
    m_view->libraryDidChange();
    m_scanFinished = false;
    updateSaveButton();
    updateRefreshButton();
    updateApplyButton();
    updateEmptyState();
    rescan();

    // D17: the placed prefabs of the new map are checked once its library is scanned
    m_promptAfterScan = hasPlacements(m_document.map().prefabLibrary());
    if (m_promptAfterScan)
    {
      scanOnce();
    }
  });

  // the mod root follows the last enabled mod
  m_notifierConnection += m_document.modsDidChangeNotifier.connect([&]() {
    updateSaveButton();
    updateEmptyState();
    rescan();
  });

  // the library takes the new definitions in processPendingResults
  m_notifierConnection += m_document.entityDefinitionsDidChangeNotifier.connect(
    [&]() { processPendingResults(); });

  m_notifierConnection += m_document.selectionDidChangeNotifier.connect([&](const auto&) {
    updateSaveButton();
    updateApplyButton();
  });

  // a refresh, its undo or a removed placement changes the number on the Refresh button
  m_notifierConnection += m_document.transactionDoneNotifier.connect(
    [&](const std::string&, bool, bool) { mapDidChange(); });
  m_notifierConnection += m_document.transactionUndoneNotifier.connect(
    [&](const std::string&, bool, bool) { mapDidChange(); });

  // the document pushes the game path and the Prefab Path into the map first, because
  // it observes the preferences since before the panel exists
  m_notifierConnection +=
    PreferenceManager::instance().preferenceDidChangeNotifier.connect(
      [&](const std::filesystem::path& path) {
        const auto& gameInfo = m_document.map().gameInfo();
        if (
          path == gameInfo.gamePathPreference.path
          || path == gameInfo.prefabPathPreference.path)
        {
          updateSaveButton();
          updateEmptyState();
          rescan();
        }
      });
}

void PrefabPanel::connectLibraryObserver()
{
  m_libraryConnection.disconnect();
  m_libraryConnection +=
    m_document.map().prefabLibrary().libraryDidChangeNotifier.connect(
      this, &PrefabPanel::libraryDidChange);
}

void PrefabPanel::startScan()
{
  m_scanStarted = true;
  ++m_scanCount;

  // the game path or the mods may have changed, which clears the entity models that the
  // cells hold until the scan has finished
  m_view->entityModelsWillChange();

  m_document.map().prefabLibrary().rescan();
  // a scan of empty roots is finished at once
  processPendingResults();
}

void PrefabPanel::processPendingResults()
{
  // the timer runs while the roots are listed, while the files are read and while a
  // saved file is read again
  auto& library = m_document.map().prefabLibrary();
  library.processPendingResults();

  if (library.scanning())
  {
    if (!m_scanTimer->isActive())
    {
      m_scanTimer->start();
    }
  }
  else
  {
    m_scanTimer->stop();
  }
}

void PrefabPanel::libraryDidChange()
{
  if (!m_document.map().prefabLibrary().scanning())
  {
    m_scanFinished = true;
  }

  updateCategories();
  m_view->libraryDidChange();
  updateRefreshButton();
  updateApplyButton();
  updateEmptyState();
  updateWatchedPaths();
  promptAfterScanIfNeeded();
}

void PrefabPanel::updateRefreshButton()
{
  // until a scan has finished, every placed prefab would count as missing
  m_changedPlacementCount =
    m_scanFinished ? m_document.map().prefabLibrary().changedPlacements().size() : 0;
  m_refreshCountStale = false;

  m_refreshButton->setText(
    m_changedPlacementCount > 0 ? tr("Refresh (%1)").arg(m_changedPlacementCount)
                                : tr("Refresh"));
  m_refreshButton->setEnabled(m_changedPlacementCount > 0);
  m_refreshButton->setToolTip(
    m_changedPlacementCount > 0
      ? tr("Update the placed prefabs whose files changed on disk...")
      : tr("No placed prefab has changed on disk"));
}

void PrefabPanel::updateApplyButton()
{
  const auto* entry = m_view->selectedEntry();
  const auto canApply = entry && mdl::canApplyPrefabToSelection(m_document.map(), *entry);
  m_applyButton->setEnabled(canApply);
  m_applyButton->setToolTip(
    entry && entry->metadata.kind == mdl::PrefabKind::Entity
      ? tr("Make the selected %1 entities match the prefab %2")
          .arg(QString::fromStdString(entry->metadata.className))
          .arg(QString::fromStdString(entry->metadata.name))
      : tr("Click an entity prefab, then select entities of its class"));
}

void PrefabPanel::updateWatchedPaths()
{
  // the roots, the folders of the prefabs in them and the prefab files; a new folder is
  // found through the change of its parent
  const auto& library = m_document.map().prefabLibrary();
  auto paths = std::set<QString>{};
  for (const auto& root : library.roots())
  {
    auto ec = std::error_code{};
    if (std::filesystem::is_directory(root, ec))
    {
      paths.insert(pathAsQString(root));
    }
  }
  for (const auto& entry : library.entries())
  {
    const auto rootPath = library.writeRootFor(entry->root);
    for (auto folder = entry->relativePath.parent_path(); !folder.empty();
         folder = folder.parent_path())
    {
      paths.insert(pathAsQString(rootPath / folder));
    }
    paths.insert(pathAsQString(entry->absolutePath));
  }

  auto pathsToRemove = QStringList{};
  for (const auto& path : m_watcher->directories() + m_watcher->files())
  {
    if (paths.erase(path) == 0)
    {
      pathsToRemove.push_back(path);
    }
  }
  if (!pathsToRemove.empty())
  {
    m_watcher->removePaths(pathsToRemove);
  }
  if (!paths.empty())
  {
    m_watcher->addPaths(QStringList{paths.begin(), paths.end()});
  }
}

void PrefabPanel::folderDidChange()
{
  if (isVisible())
  {
    // restarted by every change, so that a copy of many files rescans once
    m_watchTimer->start();
  }
  else
  {
    m_watchMissed = true;
  }
}

void PrefabPanel::mapDidChange()
{
  if (!m_refreshCountStale)
  {
    m_refreshCountStale = true;
    if (isVisible())
    {
      // once per pass of the event loop; a hidden panel counts when it is shown
      QTimer::singleShot(0, this, [this]() {
        if (m_refreshCountStale)
        {
          updateRefreshButton();
        }
      });
    }
  }
}

void PrefabPanel::promptAfterScanIfNeeded()
{
  if (m_promptAfterScan && m_scanFinished)
  {
    m_promptAfterScan = false;
    // posted, since this runs inside the library's notification
    QTimer::singleShot(0, this, [this]() {
      promptToRefreshPlacements(m_document.map().prefabLibrary().changedPlacements());
    });
  }
}

void PrefabPanel::updateCategories()
{
  const auto& entries = m_document.map().prefabLibrary().entries();
  const auto* currentItem = m_categoryTree->currentItem();
  const auto current = currentItem ? currentItem->data(0, CategoryRole) : AllCategories;

  // a folder that the user collapsed stays collapsed
  auto collapsed = QSet<QString>{};
  for (auto it = QTreeWidgetItemIterator{m_categoryTree}; *it; ++it)
  {
    if ((*it)->childCount() > 0 && !(*it)->isExpanded())
    {
      collapsed.insert((*it)->data(0, CategoryRole).toString());
    }
  }

  const auto blocker = QSignalBlocker{m_categoryTree};
  m_categoryTree->clear();

  auto* allItem = new QTreeWidgetItem{m_categoryTree};
  allItem->setText(0, tr("All (%1)").arg(entries.size()));
  allItem->setData(0, CategoryRole, AllCategories);
  auto* newCurrentItem = allItem;

  // a folder comes before the folders below it, so its item exists when theirs are made
  auto items = std::map<std::filesystem::path, QTreeWidgetItem*>{};
  for (const auto& [path, count] : prefabCategoryCounts(entries))
  {
    const auto parentPath = path.parent_path();
    auto* item = parentPath.empty() ? new QTreeWidgetItem{m_categoryTree}
                                    : new QTreeWidgetItem{items.at(parentPath)};
    const auto pathStr = pathAsGenericQString(path);
    item->setText(0, QString{"%1 (%2)"}.arg(pathAsQString(path.filename())).arg(count));
    item->setData(0, CategoryRole, pathStr);
    item->setToolTip(0, pathStr);
    items.emplace(path, item);

    // keep the chosen folder while it has prefabs
    if (current.isValid() && current.toString() == pathStr)
    {
      newCurrentItem = item;
    }
  }

  // expanded once every item is in the tree
  for (const auto& [path, item] : items)
  {
    item->setExpanded(!collapsed.contains(item->data(0, CategoryRole).toString()));
  }

  m_categoryTree->setCurrentItem(newCurrentItem);
  updateFilter();
}

void PrefabPanel::updateFilter()
{
  auto filter = prefabFilterFromText(m_filterBox->text().trimmed().toStdString());
  filter.kind = static_cast<PrefabKindFilter>(m_kindChoice->currentData().toInt());
  if (const auto* item = m_categoryTree->currentItem())
  {
    if (const auto category = item->data(0, CategoryRole); category.isValid())
    {
      filter.category = pathFromQString(category.toString());
    }
  }
  m_view->setFilter(std::move(filter));
}

void PrefabPanel::updateEmptyState()
{
  // a scan that has not finished may still find prefabs
  const auto& library = m_document.map().prefabLibrary();
  const auto message = !hasWriteRoot(library)
                         ? tr("Set a game path or a Prefab Path in Preferences")
                       : m_scanFinished && library.entries().empty()
                         ? tr("No prefabs yet. Select something and press +")
                         : QString{};

  m_emptyLabel->setText(message);
  m_browserStack->setCurrentWidget(message.isEmpty() ? m_browserPage : m_emptyLabel);
}

void PrefabPanel::updateSaveButton()
{
  const auto& map = m_document.map();
  const auto hasRoot = hasWriteRoot(map.prefabLibrary());
  m_saveButton->setEnabled(hasRoot && map.selection().hasNodes());
  m_saveButton->setToolTip(
    hasRoot ? tr("Save selection as prefab...")
            : tr("Set a game path or a Prefab Path first"));
}

void PrefabPanel::editPrefab(const QString& path)
{
  // D20: until the prefab window exists, a prefab opens as an ordinary document; posted,
  // because in a single-document window the document is replaced
  QTimer::singleShot(0, this, [this, path]() {
    if (auto* mapWindow = findMapWindow(this))
    {
      mapWindow->editPrefab(pathFromQString(path));
    }
  });
}

void PrefabPanel::revealPrefabInFolder(const std::filesystem::path& path)
{
  // the folder, not the file, which the system would open with the editor
  const auto folder = path.parent_path();
  if (!QDesktopServices::openUrl(QUrl::fromLocalFile(pathAsQPath(folder))))
  {
    m_document.map().logger().error() << "Could not open " << folder;
  }
}

void PrefabPanel::openPrefabThumbnail(const std::filesystem::path& path)
{
  const auto thumbnailPath = thumbnailPathFor(path);
  if (!QDesktopServices::openUrl(QUrl::fromLocalFile(pathAsQPath(thumbnailPath))))
  {
    m_document.map().logger().error() << "Could not open " << thumbnailPath;
  }
}

void PrefabPanel::confirmDeletePrefabFile(const std::filesystem::path& path)
{
  auto ec = std::error_code{};
  const auto thumbnailPath = thumbnailPathFor(path);
  const auto hasThumbnail = std::filesystem::is_regular_file(thumbnailPath, ec);

  auto messageBox = QMessageBox{this};
  messageBox.setIcon(QMessageBox::Warning);
  messageBox.setWindowTitle(tr("Delete Prefab"));
  messageBox.setText(
    hasThumbnail ? tr("Delete %1 and its thumbnail %2?")
                     .arg(pathAsQString(path))
                     .arg(pathAsQString(thumbnailPath.filename()))
                 : tr("Delete %1?").arg(pathAsQString(path)));
  messageBox.setInformativeText(
    tr("The placed copies of the prefab stay in the map. This cannot be undone."));
  messageBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
  messageBox.setDefaultButton(QMessageBox::No);
  if (messageBox.exec() != QMessageBox::Yes)
  {
    return;
  }

  deletePrefabFile(path) | kdl::transform_error([&](const auto& e) {
    m_document.map().logger().error() << e.msg;
    QMessageBox::warning(this, tr("Delete Prefab"), QString::fromStdString(e.msg));
  });
}

void PrefabPanel::confirmRemovePrefabThumbnail(const std::filesystem::path& path)
{
  const auto thumbnailPath = thumbnailPathFor(path);
  const auto answer = QMessageBox::question(
    this,
    tr("Remove Thumbnail"),
    tr("Delete %1?").arg(pathAsQString(thumbnailPath)),
    QMessageBox::Yes | QMessageBox::No,
    QMessageBox::No);
  if (answer != QMessageBox::Yes)
  {
    return;
  }

  removePrefabThumbnail(path) | kdl::transform_error([&](const auto& e) {
    m_document.map().logger().error() << e.msg;
    QMessageBox::warning(this, tr("Remove Thumbnail"), QString::fromStdString(e.msg));
  });
}

} // namespace tb::ui
