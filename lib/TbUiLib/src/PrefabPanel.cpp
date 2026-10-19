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
#include <QHBoxLayout>
#include <QLineEdit>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "base/PreferenceManager.h"
#include "mdl/GameInfo.h"
#include "mdl/Map.h"
#include "mdl/PrefabLibrary.h"
#include "mdl/Selection.h"
#include "ui/BitmapButton.h"
#include "ui/MapDocument.h"
#include "ui/MapWindow.h"
#include "ui/PrefabBrowserView.h"
#include "ui/QPathUtils.h"
#include "ui/SavePrefabDialog.h"
#include "ui/SearchBox.h"
#include "ui/ViewConstants.h"

#include "kd/string_utils.h"

#include <chrono>

namespace tb::ui
{
namespace
{

using namespace std::chrono_literals;

// how often a running scan is polled for finished files
constexpr auto ScanPollInterval = 50ms;

// the category choice's item data for "All"
const auto AllCategories = QVariant{};

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

} // namespace

PrefabPanel::PrefabPanel(const SidebarContext& context, QWidget* parent)
  : SidebarPanel{parent}
  , m_appController{context.appController}
  , m_document{context.document}
  , m_scanTimer{new QTimer{this}}
{
  createGui();
  connectObservers();
  connectLibraryObserver();
  updateSaveButton();
  // the panel is hidden by default; the first scan waits until it is shown
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

void PrefabPanel::createGui()
{
  m_categoryChoice = new QComboBox{};
  m_categoryChoice->setObjectName("PrefabPanel_categoryChoice");
  m_categoryChoice->setToolTip(tr("Show the prefabs of one folder"));
  m_categoryChoice->addItem(tr("All"), AllCategories);
  connect(
    m_categoryChoice,
    QOverload<int>::of(&QComboBox::activated),
    this,
    &PrefabPanel::updateFilter);

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
  m_filterBox->setToolTip(tr("Filter by name, tag or class"));
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

  m_scrollBar = new QScrollBar{Qt::Vertical};
  m_view = new PrefabBrowserView{m_appController, m_scrollBar, m_document};
  connect(
    m_view, &PrefabBrowserView::editPrefabRequested, this, &PrefabPanel::editPrefab);

  auto* controlLayout = new QHBoxLayout{};
  controlLayout->setContentsMargins(
    LayoutConstants::NarrowHMargin,
    LayoutConstants::NarrowVMargin,
    LayoutConstants::NarrowHMargin,
    LayoutConstants::NarrowVMargin);
  controlLayout->setSpacing(LayoutConstants::NarrowHMargin);
  controlLayout->addWidget(m_categoryChoice, 0);
  controlLayout->addWidget(m_kindChoice, 0);
  controlLayout->addWidget(m_filterBox, 1);
  controlLayout->addWidget(m_saveButton, 0);
  controlLayout->addWidget(m_rescanButton, 0);

  auto* browserLayout = new QHBoxLayout{};
  browserLayout->setContentsMargins(0, 0, 0, 0);
  browserLayout->setSpacing(0);
  browserLayout->addWidget(m_view, 1);
  browserLayout->addWidget(m_scrollBar, 0);

  auto* outerLayout = new QVBoxLayout{};
  outerLayout->setContentsMargins(0, 0, 0, 0);
  outerLayout->setSpacing(0);
  outerLayout->addLayout(controlLayout, 0);
  outerLayout->addLayout(browserLayout, 1);
  setLayout(outerLayout);

  m_scanTimer->setInterval(ScanPollInterval);
  connect(m_scanTimer, &QTimer::timeout, this, &PrefabPanel::processPendingResults);
}

void PrefabPanel::connectObservers()
{
  // the document outlives the map it holds; on load the panel connects to the new map's
  // library and reads the roots of the new map
  m_notifierConnection += m_document.documentWasLoadedNotifier.connect([&]() {
    connectLibraryObserver();
    m_view->libraryDidChange();
    updateSaveButton();
    rescan();
  });

  // the mod root follows the last enabled mod
  m_notifierConnection += m_document.modsDidChangeNotifier.connect([&]() {
    updateSaveButton();
    rescan();
  });

  // the library takes the new definitions in processPendingResults
  m_notifierConnection += m_document.entityDefinitionsDidChangeNotifier.connect(
    [&]() { processPendingResults(); });

  m_notifierConnection += m_document.selectionDidChangeNotifier.connect(
    [&](const auto&) { updateSaveButton(); });

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
  updateCategories();
  m_view->libraryDidChange();
}

void PrefabPanel::updateCategories()
{
  const auto current = m_categoryChoice->currentData();
  const auto categories = prefabCategories(m_document.map().prefabLibrary().entries());

  const auto blocker = QSignalBlocker{m_categoryChoice};
  m_categoryChoice->clear();
  m_categoryChoice->addItem(tr("All"), AllCategories);
  for (const auto& category : categories)
  {
    const auto categoryStr = pathAsGenericQString(category);
    m_categoryChoice->addItem(
      categoryStr.isEmpty() ? tr("(top level)") : categoryStr, categoryStr);
  }

  // keep the chosen folder while it has prefabs
  const auto index = m_categoryChoice->findData(current);
  m_categoryChoice->setCurrentIndex(index >= 0 ? index : 0);
  updateFilter();
}

void PrefabPanel::updateFilter()
{
  auto filter = PrefabFilter{};
  filter.kind = static_cast<PrefabKindFilter>(m_kindChoice->currentData().toInt());
  if (const auto category = m_categoryChoice->currentData(); category.isValid())
  {
    filter.category = pathFromQString(category.toString());
  }
  filter.words = kdl::str_split(m_filterBox->text().trimmed().toStdString(), " ");
  m_view->setFilter(std::move(filter));
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

} // namespace tb::ui
