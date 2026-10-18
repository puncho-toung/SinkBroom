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

#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QRect>
#include <QSize>
#include <QStackedLayout>
#include <QStackedWidget>
#include <QSvgRenderer>

#include "ui/LightEditorPanel.h"
#include "ui/MapDocument.h"
#include "ui/QPathUtils.h"
#include "ui/SidebarManager.h"
#include "ui/SystemPaths.h"

#include <memory>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{

StubPanel::StubPanel(
  QString panelId,
  std::filesystem::path iconPath,
  const bool shownByDefault,
  const Qt::DockWidgetArea defaultArea)
  : m_panelId{std::move(panelId)}
  , m_iconPath{std::move(iconPath)}
  , m_shownByDefault{shownByDefault}
  , m_defaultArea{defaultArea}
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

std::filesystem::path StubPanel::iconPath() const
{
  return m_iconPath;
}

bool StubPanel::shownByDefault() const
{
  return m_shownByDefault;
}

Qt::DockWidgetArea StubPanel::defaultArea() const
{
  return m_defaultArea;
}

void StubPanel::panelShown()
{
  ++shownCount;
}

void StubPanel::panelHidden()
{
  ++hiddenCount;
}

RegisteredStub registerStubPanel(
  SidebarManager& manager,
  const QString& panelId,
  std::filesystem::path iconPath,
  const bool shownByDefault,
  const Qt::DockWidgetArea defaultArea)
{
  auto panel = std::make_unique<StubPanel>(
    panelId, std::move(iconPath), shownByDefault, defaultArea);
  auto* panelPtr = panel.get();
  auto* dock = manager.registerPanel(std::move(panel));
  return {dock, panelPtr};
}

void checkGreyscaleIcon(const std::filesystem::path& imagePath)
{
  const auto name = imagePath.string();
  CAPTURE(name);

  auto renderer = QSvgRenderer{pathAsQPath(
    SystemPaths::findResourceFile(std::filesystem::path{"images"} / imagePath))};
  REQUIRE(renderer.isValid());
  CHECK(renderer.defaultSize() == QSize{24, 24});
  CHECK(renderer.viewBox() == QRect{0, 0, 24, 24});

  // the dark theme inverts an icon only if it is greyscale
  auto image = QImage{renderer.defaultSize(), QImage::Format_ARGB32_Premultiplied};
  image.fill(Qt::transparent);
  auto painter = QPainter{&image};
  renderer.render(&painter);
  painter.end();
  CHECK(image.isGrayscale());
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
  const auto* label = stack->currentWidget()->findChild<QLabel*>(
    stack->currentIndex() == 0 ? "LightEditorPanel_Message" : "LightEditorPanel_Header");
  REQUIRE(label != nullptr);
  return label->text();
}

} // namespace tb::ui
