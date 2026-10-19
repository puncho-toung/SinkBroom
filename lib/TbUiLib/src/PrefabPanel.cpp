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
#include "mdl/ModelUtils.h"
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
#include <fmt/std.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tb::ui
{
namespace
{

using namespace std::chrono_literals;

// how often a running scan is polled for finished files
constexpr auto ScanPollInterval = 50ms;

// how long the library's folders must be quiet before a change in them rescans
constexpr auto WatchDebounceInterval = 500ms;

// how long the watcher's reports of the panel's own save are ignored
constexpr auto OwnWriteInterval = 2s;

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
  // the entries hold lexically normal paths, so only the argument is made normal
  // (R2-pf-ui-8)
  const auto normalPath = absolutePath.lexically_normal();
  const auto& entries = library.entries();
  const auto it = std::ranges::find_if(entries, [&](const auto& entry) {
    return entry->absolutePath.native() == normalPath.native();
  });
  return it != entries.end() ? it->get() : nullptr;
}

/**
 * The entry whose file the given path is, or an error that names the path; the file
 * actions act on entries only, never on a path that a caller made up (R1-pf-ui-2).
 */
Result<const mdl::PrefabEntry*> findEntryOrError(
  const mdl::PrefabLibrary& library, const std::filesystem::path& absolutePath)
{
  if (const auto* entry = findEntry(library, absolutePath))
  {
    return entry;
  }
  return Error{fmt::format("{} is not a prefab of the library", absolutePath)};
}

// the entry's thumbnail, or where it would be
std::filesystem::path thumbnailPathOf(const mdl::PrefabEntry& entry)
{
  return entry.thumbnailPath.value_or(thumbnailPathFor(entry.absolutePath));
}

// whether the given path lies below the given folder; both are lexically normal
bool isBelow(const std::filesystem::path& path, const std::filesystem::path& folder)
{
  const auto relativePath = path.lexically_relative(folder);
  return !relativePath.empty() && relativePath != "."
         && kdl::path_front(relativePath) != "..";
}

/**
 * Whether refreshing the given placement may replace the groups of another of the given
 * placements. A refresh replaces the children of the placement's members, and so every
 * group nested in them; if a member is itself inside a group, the linked copies of that
 * group may have their children replaced too, so such a placement always may.
 */
bool refreshMayReplaceOthers(
  const mdl::PrefabPlacement& placement,
  const std::map<mdl::PrefabPlacementKey, mdl::PrefabPlacement>& placements)
{
  if (std::ranges::any_of(placement.members, [](const auto* member) {
        return mdl::findContainingGroup(member) != nullptr;
      }))
  {
    return true;
  }

  const auto members = std::unordered_set<const mdl::Node*>{
    placement.members.begin(), placement.members.end()};
  return std::ranges::any_of(placements, [&](const auto& keyAndPlacement) {
    return std::ranges::any_of(keyAndPlacement.second.members, [&](const auto* member) {
      for (const auto* parent = member->parent(); parent; parent = parent->parent())
      {
        if (members.contains(parent))
        {
          return true;
        }
      }
      return false;
    });
  });
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
  m_watching = true;
  if (m_scanStarted)
  {
    processPendingResults();
  }
  else
  {
    // the first scan watches the folders once it has finished
    m_watchMissed = false;
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

void PrefabPanel::panelHidden()
{
  // R2-pf-ui-5: a closed panel holds no folder handles; panelShown rescans, and the end
  // of that scan watches the folders again
  m_watching = false;
  m_watchMissed = true;
  m_watchTimer->stop();
  m_changedPaths.clear();
  m_libraryFilesChanged = false;
  m_ownWrites.clear();
  if (const auto paths = m_watcher->directories() + m_watcher->files(); !paths.empty())
  {
    m_watcher->removePaths(paths);
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
  watchSavedFile(path);
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

std::size_t PrefabPanel::recountCount() const
{
  return m_recountCount;
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
  auto keys = std::vector<mdl::PrefabPlacementKey>{};
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
  // R2-pf-ui-7: one walk of the map for every key, again only after a refresh that may
  // have replaced the groups of another placement
  auto current = map.prefabLibrary().placementsByKey();
  for (const auto& key : keys)
  {
    const auto it = current.find(key);
    if (it == current.end())
    {
      continue;
    }

    const auto& prefabPath = key.first;
    const auto walkAgain = refreshMayReplaceOthers(it->second, current);
    mdl::refreshPlacement(map, it->second) | kdl::transform([&](const auto& report) {
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

    if (walkAgain)
    {
      current = map.prefabLibrary().placementsByKey();
    }
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
  return findEntryOrError(m_document.map().prefabLibrary(), path)
         | kdl::and_then([&](const auto* entry) {
             // copied, since the rescan replaces the entry
             const auto filePath = entry->absolutePath;
             const auto thumbnailPath = thumbnailPathOf(*entry);

             // the thumbnail goes with the file; a missing one is no error
             auto result =
               fs::Disk::deleteFile(filePath)
               | kdl::and_then([&](auto) { return fs::Disk::deleteFile(thumbnailPath); })
               | kdl::transform([](auto) {});

             // the file may be gone even if its thumbnail is not
             libraryFilesDidChange();
             return result;
           });
}

Result<void> PrefabPanel::removePrefabThumbnail(const std::filesystem::path& path)
{
  return findEntryOrError(m_document.map().prefabLibrary(), path)
         | kdl::and_then([&](const auto* entry) -> Result<void> {
             if (!entry->thumbnailPath)
             {
               return Error{fmt::format("{} has no thumbnail", entry->absolutePath)};
             }

             auto result =
               fs::Disk::deleteFile(*entry->thumbnailPath) | kdl::transform([](auto) {});

             // the entry learns that it has no thumbnail, and its cell draws the prefab
             libraryFilesDidChange();
             return result;
           });
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
  m_watcher->setObjectName("PrefabPanel_watcher");
  m_watchTimer->setSingleShot(true);
  m_watchTimer->setInterval(WatchDebounceInterval);
  connect(m_watchTimer, &QTimer::timeout, this, &PrefabPanel::watchTimerDidRunOut);
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
    ++m_loadGeneration;
    m_scanRunning = false;
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

  // a refresh, its undo or a removed placement changes the number on the Refresh button;
  // counted when the outermost transaction that changed the map is committed or undone,
  // not for a selection, and not for the nested transaction of every step of a drag
  // (R2-pf-ui-2)
  const auto transactionDone = [&](const std::string&, bool, const bool isModification) {
    if (isModification && !m_document.map().isTransactionActive())
    {
      mapDidChange();
    }
  };
  m_notifierConnection += m_document.transactionDoneNotifier.connect(transactionDone);
  m_notifierConnection += m_document.transactionUndoneNotifier.connect(transactionDone);

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
  // set before the library is told, since a scan of empty roots finishes at once
  m_scanRunning = true;
  ++m_scanCount;

  // R2-pf-ui-4: the view clears the entity models that its cells hold when the mods or
  // the game path change, which is not on every scan
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

  // H-F4a-1: a scan that changed nothing does not notify, so the end of a scan is seen
  // here, after the notification of a scan that did
  if (scanJustFinished())
  {
    scanDidFinish();
  }

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

bool PrefabPanel::scanJustFinished() const
{
  return m_scanRunning && !m_document.map().prefabLibrary().scanning();
}

void PrefabPanel::scanDidFinish()
{
  // only the end of a scan that the panel started, not new definitions or the update of
  // one saved file, makes missing files missing indeed (R1-pf-ui-3)
  m_scanRunning = false;
  m_scanFinished = true;

  updateRefreshButton();
  updateEmptyState();
  if (m_watching)
  {
    updateWatchedPaths();
  }
  else
  {
    // the folders are watched again, and read again, once the panel is shown
    m_watchMissed = true;
  }
  promptAfterScanIfNeeded();
}

void PrefabPanel::libraryDidChange()
{
  updateCategories();
  m_view->libraryDidChange();
  updateApplyButton();

  // at the end of a scan that the panel started, scanDidFinish follows at once
  if (!scanJustFinished())
  {
    updateRefreshButton();
    updateEmptyState();
  }
}

void PrefabPanel::updateRefreshButton()
{
  // until a scan has finished, every placed prefab would count as missing
  m_changedPlacementCount = 0;
  if (m_scanFinished)
  {
    m_changedPlacementCount = m_document.map().prefabLibrary().changedPlacements().size();
    ++m_recountCount;
  }
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
  // every folder under the roots as the last scan listed them, so that a new empty
  // folder, and a file copied into it later, is seen (R1-pf-ui-1), and the prefab files
  // in those folders, whose changes a folder does not report; an entry under a root that
  // the map no longer has is not watched (R1-pf-ui-5); the folders are not walked again
  // (R2-pf-ui-1)
  const auto& library = m_document.map().prefabLibrary();
  auto folders = std::set<QString>{};
  for (const auto& folder : library.directories())
  {
    folders.insert(pathAsQString(folder));
  }

  auto paths = folders;
  for (const auto& entry : library.entries())
  {
    const auto folder =
      pathAsQString(entry->absolutePath.parent_path().lexically_normal());
    if (folders.contains(folder))
    {
      paths.insert(pathAsQString(entry->absolutePath.lexically_normal()));
    }
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

void PrefabPanel::watchSavedFile(const std::filesystem::path& path)
{
  if (!m_watching)
  {
    return;
  }

  // a file outside the roots is not in the library
  const auto filePath = path.lexically_normal();
  const auto roots = m_document.map().prefabLibrary().roots();
  const auto rootIt = std::ranges::find_if(
    roots, [&](const auto& root) { return isBelow(filePath, root.lexically_normal()); });
  if (rootIt == roots.end())
  {
    return;
  }
  const auto rootPath = rootIt->lexically_normal();

  const auto now = std::chrono::steady_clock::now();
  if (now >= m_ownWritesUntil)
  {
    m_ownWrites.clear();
  }
  m_ownWritesUntil = now + OwnWriteInterval;

  // R2-pf-ui-1: the saved file, and the folders that the save made, are watched without
  // a rescan; the save's own change of them, and of the watched folder above them, is
  // not a reason for one (R2-pf-mdl-6)
  auto pathsToAdd = QStringList{};
  const auto fileString = pathAsQString(filePath);
  m_ownWrites.insert(fileString);
  if (!m_watcher->files().contains(fileString))
  {
    pathsToAdd.push_back(fileString);
  }

  const auto watchedFolders = m_watcher->directories();
  for (auto folder = filePath.parent_path();; folder = folder.parent_path())
  {
    const auto folderString = pathAsQString(folder);
    m_ownWrites.insert(folderString);
    if (watchedFolders.contains(folderString))
    {
      break;
    }
    pathsToAdd.push_back(folderString);
    if (!isBelow(folder, rootPath))
    {
      // the root
      break;
    }
  }

  if (!pathsToAdd.empty())
  {
    m_watcher->addPaths(pathsToAdd);
  }
}

bool PrefabPanel::isOwnWrite(const QString& path) const
{
  return m_ownWrites.contains(path)
         && std::chrono::steady_clock::now() < m_ownWritesUntil;
}

void PrefabPanel::folderDidChange(const QString& path)
{
  if (isVisible())
  {
    // restarted by every change, so that a copy of many files rescans once
    m_changedPaths.insert(path);
    m_watchTimer->start();
  }
  else if (!isOwnWrite(path))
  {
    m_watchMissed = true;
  }
}

void PrefabPanel::watchTimerDidRunOut()
{
  // R2-pf-mdl-6: the library has read what the panel's own save changed; the save is
  // known by now, even if the watcher reported it before the dialog had closed
  const auto changed =
    m_libraryFilesChanged || std::ranges::any_of(m_changedPaths, [&](const auto& path) {
      return !isOwnWrite(path);
    });
  m_changedPaths.clear();
  m_libraryFilesChanged = false;

  if (changed)
  {
    rescan();
  }
}

void PrefabPanel::libraryFilesDidChange()
{
  // R2-pf-ui-3: the watcher reports the panel's own delete too, so a visible panel waits
  // for the watcher, and the two make one scan; a hidden panel rescans at once, since the
  // library is used without it
  if (m_watching && isVisible())
  {
    m_libraryFilesChanged = true;
    m_watchTimer->start();
  }
  else
  {
    rescan();
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

void PrefabPanel::setOpenPromptForTesting(RefreshPrompt prompt)
{
  m_openPrompt = std::move(prompt);
}

void PrefabPanel::promptAfterScanIfNeeded()
{
  if (m_promptAfterScan && m_scanFinished)
  {
    m_promptAfterScan = false;
    // posted, since this runs inside the library's notification; a document loaded in
    // the meantime gets a prompt of its own (R1-pf-ui-4)
    QTimer::singleShot(0, this, [this, loadGeneration = m_loadGeneration]() {
      if (loadGeneration != m_loadGeneration || !m_scanFinished)
      {
        return;
      }

      // DD-24: a machine without the files is not asked on every load; the Refresh
      // button counts the missing placements
      const auto placements = m_document.map().prefabLibrary().changedPlacements();
      if (std::ranges::none_of(
            placements, [](const auto& placement) { return placement.entry != nullptr; }))
      {
        return;
      }

      if (m_openPrompt)
      {
        m_openPrompt(placements);
      }
      else
      {
        promptToRefreshPlacements(placements);
      }
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
  auto& logger = m_document.map().logger();
  findEntryOrError(m_document.map().prefabLibrary(), path)
    | kdl::transform([&](const auto* entry) {
        // the folder, not the file, which the system would open with the editor
        const auto folder = entry->absolutePath.parent_path();
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(pathAsQPath(folder))))
        {
          logger.error() << "Could not open " << folder;
        }
      })
    | kdl::transform_error([&](const auto& e) { logger.error() << e.msg; });
}

void PrefabPanel::openPrefabThumbnail(const std::filesystem::path& path)
{
  auto& logger = m_document.map().logger();
  findEntryOrError(m_document.map().prefabLibrary(), path)
    | kdl::transform([&](const auto* entry) {
        if (!entry->thumbnailPath)
        {
          logger.error() << entry->absolutePath << " has no thumbnail";
        }
        else if (!QDesktopServices::openUrl(
                   QUrl::fromLocalFile(pathAsQPath(*entry->thumbnailPath))))
        {
          logger.error() << "Could not open " << *entry->thumbnailPath;
        }
      })
    | kdl::transform_error([&](const auto& e) { logger.error() << e.msg; });
}

void PrefabPanel::confirmDeletePrefabFile(const std::filesystem::path& path)
{
  const auto* entry = findEntry(m_document.map().prefabLibrary(), path);
  if (!entry)
  {
    m_document.map().logger().error()
      << fmt::format("{} is not a prefab of the library", path);
    return;
  }

  auto ec = std::error_code{};
  const auto thumbnailPath = thumbnailPathOf(*entry);
  const auto hasThumbnail = std::filesystem::is_regular_file(thumbnailPath, ec);

  auto messageBox = QMessageBox{this};
  messageBox.setIcon(QMessageBox::Warning);
  messageBox.setWindowTitle(tr("Delete Prefab"));
  messageBox.setText(
    hasThumbnail ? tr("Delete %1 and its thumbnail %2?")
                     .arg(pathAsQString(entry->absolutePath))
                     .arg(pathAsQString(thumbnailPath.filename()))
                 : tr("Delete %1?").arg(pathAsQString(entry->absolutePath)));
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
  const auto* entry = findEntry(m_document.map().prefabLibrary(), path);
  if (!entry || !entry->thumbnailPath)
  {
    m_document.map().logger().error()
      << fmt::format("{} is not a prefab of the library with a thumbnail", path);
    return;
  }

  const auto thumbnailPath = *entry->thumbnailPath;
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
