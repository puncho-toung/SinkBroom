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

#include "ui/IssueBrowser.h"

#include <QCheckBox>
#include <QList>
#include <QStringList>
#include <QVBoxLayout>

#include "mdl/Map.h"
#include "mdl/Validator.h"
#include "mdl/WorldNode.h"
#include "ui/BorderLine.h"
#include "ui/FlagsPopupEditor.h"
#include "ui/IssueBrowserView.h"
#include "ui/MapDocument.h"
#include "ui/ViewConstants.h"

namespace tb::ui
{

IssueBrowser::IssueBrowser(MapDocument& document, QWidget* parent)
  : SidebarPanel{parent}
  , m_document{document}
  , m_view{new IssueBrowserView{m_document}}
{
  // no lower than the info panel that held this page
  setMinimumHeight(100);

  // the controls sat at the right of the info panel's tab bar; a dock's tab bar has no
  // room for them, so they head the panel, at the right as before
  auto* sizer = new QVBoxLayout{};
  sizer->setContentsMargins(0, 0, 0, 0);
  sizer->setSpacing(0);
  sizer->addWidget(createControlBar(), 0);
  sizer->addWidget(new BorderLine{}, 0);
  sizer->addWidget(m_view, 1);
  setLayout(sizer);

  connectObservers();
}

QString IssueBrowser::panelId() const
{
  return QString::fromLatin1(PanelId);
}

QString IssueBrowser::title() const
{
  return tr("Issues");
}

std::filesystem::path IssueBrowser::iconPath() const
{
  return "Issues.svg";
}

bool IssueBrowser::shownByDefault() const
{
  return true;
}

Qt::DockWidgetArea IssueBrowser::defaultArea() const
{
  return Qt::BottomDockWidgetArea;
}

QWidget* IssueBrowser::createControlBar()
{
  auto* barPage = new QWidget{};
  m_showHiddenIssuesCheckBox = new QCheckBox{"Show hidden issues"};
  connect(
    m_showHiddenIssuesCheckBox,
    &QCheckBox::checkStateChanged,
    this,
    &IssueBrowser::showHiddenIssuesChanged);

  m_filterEditor = new FlagsPopupEditor{1, "Filter", false};
  connect(
    m_filterEditor, &FlagsPopupEditor::flagChanged, this, &IssueBrowser::filterChanged);

  auto* barPageSizer = new QHBoxLayout{};
  barPageSizer->setContentsMargins(
    LayoutConstants::NarrowHMargin,
    LayoutConstants::NarrowVMargin,
    LayoutConstants::NarrowHMargin,
    LayoutConstants::NarrowVMargin);
  barPageSizer->addStretch(1);
  barPageSizer->addWidget(m_showHiddenIssuesCheckBox, 0, Qt::AlignVCenter);
  barPageSizer->addWidget(m_filterEditor, 0, Qt::AlignVCenter);
  barPage->setLayout(barPageSizer);

  return barPage;
}

void IssueBrowser::connectObservers()
{
  m_notifierConnection += m_document.documentWasLoadedNotifier.connect([&] {
    updateFilterFlags();
    m_view->reload();
  });
  m_notifierConnection +=
    m_document.documentWasSavedNotifier.connect([&] { m_view->update(); });
  m_notifierConnection +=
    m_document.documentDidChangeNotifier.connect([&] { m_view->reload(); });
  m_notifierConnection +=
    m_document.materialCollectionsDidChangeNotifier.connect([&] { m_view->reload(); });
  m_notifierConnection +=
    m_document.entityDefinitionsDidChangeNotifier.connect([&] { m_view->reload(); });
}

void IssueBrowser::updateFilterFlags()
{
  auto flags = QList<int>{};
  auto labels = QStringList{};

  const auto& map = m_document.map();
  const auto validators = map.worldNode().registeredValidators();

  for (const auto* validator : validators)
  {
    flags.push_back(validator->type());
    labels.push_back(QString::fromStdString(validator->description()));
  }

  m_filterEditor->setFlags(flags, labels);
  m_view->setHiddenIssueTypes(0);
  m_filterEditor->setFlagValue(~0);
}

void IssueBrowser::showHiddenIssuesChanged()
{
  m_view->setShowHiddenIssues(m_showHiddenIssuesCheckBox->isChecked());
}

void IssueBrowser::filterChanged(
  const size_t /* index */,
  const int /* value */,
  const int setFlag,
  const int /* mixedFlag */)
{
  m_view->setHiddenIssueTypes(~setFlag);
}

} // namespace tb::ui
