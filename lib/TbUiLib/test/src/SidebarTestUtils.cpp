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

#include "SidebarTestUtils.h"

#include <QLabel>
#include <QStackedLayout>
#include <QStackedWidget>

#include "ui/LightEditorPanel.h"
#include "ui/MapDocument.h"
#include "ui/SidebarManager.h"

#include <memory>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{

StubPanel::StubPanel(QString panelId)
  : m_panelId{std::move(panelId)}
{
}

QString StubPanel::panelId() const
{
  return m_panelId;
}

QString StubPanel::title() const
{
  return m_panelId + " title";
}

void StubPanel::panelShown()
{
  ++shownCount;
}

void StubPanel::panelHidden()
{
  ++hiddenCount;
}

RegisteredStub registerStubPanel(SidebarManager& manager, const QString& panelId)
{
  auto panel = std::make_unique<StubPanel>(panelId);
  auto* panelPtr = panel.get();
  auto* dock = manager.registerPanel(std::move(panel));
  return {dock, panelPtr};
}

SidebarContextFixture::SidebarContextFixture(mdl::MapFixtureConfig config)
  : m_document{m_documentFixture.create(std::move(config))}
  , m_toolBox{m_document, new QStackedLayout{&m_toolPages}}
{
}

SidebarContextFixture::~SidebarContextFixture() = default;

MapDocument& SidebarContextFixture::document()
{
  return m_document;
}

SidebarContext SidebarContextFixture::context()
{
  return {m_appControllerFixture.appController(), m_document, m_toolBox};
}

QString currentText(const LightEditorPanel& panel)
{
  const auto* stack = panel.findChild<QStackedWidget*>("LightEditorPanel_Stack");
  REQUIRE(stack != nullptr);
  const auto* label = stack->currentWidget()->findChild<QLabel*>();
  REQUIRE(label != nullptr);
  return label->text();
}

} // namespace tb::ui
