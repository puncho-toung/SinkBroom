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

#include "ui/WidgetState.h"

#include <QWidget>

namespace tb::ui
{

namespace
{
const auto WidgetSettingsScopeProperty = "tbWidgetSettingsScope";
}

QString widgetSettingsPath(const QWidget* widget, const QString& suffix)
{
  contract_pre(widget != nullptr);
  contract_pre(!widget->objectName().isEmpty());

  // the parents, not window(): a floating dock is a window of its own
  for (const auto* parent = widget->parentWidget(); parent;
       parent = parent->parentWidget())
  {
    if (const auto scope = parent->property(WidgetSettingsScopeProperty).toString();
        !scope.isEmpty())
    {
      return "Windows/" + scope + "/" + widget->objectName() + "/" + suffix;
    }
  }
  return "Windows/" + widget->objectName() + "/" + suffix;
}

void setWidgetSettingsScope(QWidget* window, const QString& scope)
{
  contract_pre(window != nullptr);

  window->setProperty(WidgetSettingsScopeProperty, scope);
}

void saveWidgetGeometry(QWidget* widget)
{
  contract_pre(widget != nullptr);

  const auto path = widgetSettingsPath(widget, "Geometry");
  auto settings = QSettings{};
  settings.setValue(path, widget->saveGeometry());
}

void restoreWidgetGeometry(QWidget* widget)
{
  contract_pre(widget != nullptr);

  const auto path = widgetSettingsPath(widget, "Geometry");
  auto settings = QSettings{};
  widget->restoreGeometry(settings.value(path).toByteArray());
}

} // namespace tb::ui
