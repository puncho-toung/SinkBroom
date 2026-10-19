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

#include "ui/SavePrefabDialog.h"

#include <QBoxLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>

#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/GroupNode.h"
#include "mdl/Map.h"
#include "mdl/PatchNode.h"
#include "mdl/PrefabFile.h"
#include "mdl/PrefabLibrary.h"
#include "mdl/Selection.h"
#include "mdl/WorldNode.h"
#include "ui/DialogButtonLayout.h"
#include "ui/MapDocument.h"
#include "ui/PrefabBrowserView.h"
#include "ui/QPathUtils.h"
#include "ui/QStyleUtils.h"
#include "ui/ViewConstants.h"

#include "kd/ranges/to.h"
#include "kd/result.h"
#include "kd/string_format.h"
#include "kd/string_utils.h"

#include <ranges>
#include <system_error>

namespace tb::ui
{
namespace
{

const auto DefaultNameKey = std::string{"name"};

/**
 * The entity the selection stands for: the selected entity, or the brush entity whose
 * brushes or patches are selected.
 */
const mdl::EntityNode* selectedEntityNode(const mdl::Selection& selection)
{
  if (!selection.entities.empty())
  {
    return selection.entities.front();
  }
  for (const auto* brushNode : selection.brushes)
  {
    if (
      const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(brushNode->parent()))
    {
      return entityNode;
    }
  }
  for (const auto* patchNode : selection.patches)
  {
    if (
      const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(patchNode->parent()))
    {
      return entityNode;
    }
  }
  return nullptr;
}

std::vector<std::string> splitTags(const QString& text)
{
  return kdl::str_split(text.toStdString(), ";")
         | std::views::transform([](const auto& tag) { return kdl::str_trim(tag); })
         | std::views::filter([](const auto& tag) { return !tag.empty(); })
         | kdl::ranges::to<std::vector>();
}

QString describeExternalLink(const mdl::ExternalLink& link)
{
  const auto& entity = link.node->entity();
  const auto* name = entity.property(DefaultNameKey);
  const auto& who = name && !name->empty() ? *name : entity.classname();
  return QString{"%1: %2 \"%3\""}.arg(
    QString::fromStdString(who),
    QString::fromStdString(link.key),
    QString::fromStdString(link.value));
}

} // namespace

std::string defaultPrefabName(const MapDocument& document)
{
  const auto& map = document.map();
  const auto& selection = map.selection();
  const auto& naming = map.worldNode().entityPropertyConfig().naming;
  const auto& nameKey = naming ? naming->nameKey : DefaultNameKey;

  const auto* entityNode = selectedEntityNode(selection);
  if (entityNode)
  {
    if (const auto* name = entityNode->entity().property(nameKey); name && !name->empty())
    {
      return *name;
    }
  }
  if (!selection.groups.empty())
  {
    return selection.groups.front()->group().name();
  }
  if (entityNode)
  {
    return entityNode->entity().classname();
  }
  return "prefab";
}

SavePrefabDialog::SavePrefabDialog(MapDocument& document, QWidget* parent)
  : QDialog{parent}
  , m_document{document}
  , m_nodes{mdl::normalizePrefabSelection(document.map().selection().nodes)}
  , m_singleEntity{mdl::derivePrefabKind(m_nodes, false) == mdl::PrefabKind::Entity}
{
  createGui();
  updateControls();
}

SavePrefabDialog::~SavePrefabDialog() = default;

const std::optional<std::filesystem::path>& SavePrefabDialog::savedPath() const
{
  return m_savedPath;
}

void SavePrefabDialog::accept()
{
  const auto options = this->options();

  auto ec = std::error_code{};
  if (const auto path = mdl::prefabFilePath(m_document.map().prefabLibrary(), options)
                        | kdl::value_or(std::filesystem::path{});
      !path.empty() && std::filesystem::is_regular_file(path, ec))
  {
    const auto answer = QMessageBox::question(
      this,
      tr("Save Prefab"),
      tr("%1 exists. Replace it?").arg(pathAsQString(path)),
      QMessageBox::Yes | QMessageBox::No,
      QMessageBox::No);
    if (answer != QMessageBox::Yes)
    {
      return;
    }
  }

  mdl::savePrefab(m_document.map(), m_nodes, options) | kdl::transform([&](auto path) {
    m_savedPath = std::move(path);
    QDialog::accept();
  }) | kdl::transform_error([&](const auto& e) {
    QMessageBox::critical(
      this, tr("Save Prefab"), QString::fromStdString(e.msg), QMessageBox::Ok);
  });
}

void SavePrefabDialog::createGui()
{
  setWindowIconTB(this);
  setWindowTitle(tr("Save Selection as Prefab"));

  const auto& library = m_document.map().prefabLibrary();

  m_nameEdit = new QLineEdit{QString::fromStdString(defaultPrefabName(m_document))};

  m_rootChoice = new QComboBox{};
  if (!library.writeRootFor(mdl::PrefabRoot::User).empty())
  {
    m_rootChoice->addItem(tr("User"), static_cast<int>(mdl::PrefabRoot::User));
  }
  if (!library.writeRootFor(mdl::PrefabRoot::Mod).empty())
  {
    m_rootChoice->addItem(tr("Mod"), static_cast<int>(mdl::PrefabRoot::Mod));
  }
  for (int i = 0; i < m_rootChoice->count(); ++i)
  {
    const auto root = static_cast<mdl::PrefabRoot>(m_rootChoice->itemData(i).toInt());
    m_rootChoice->setItemData(
      i, pathAsQString(library.writeRootFor(root)), Qt::ToolTipRole);
  }

  m_categoryChoice = new QComboBox{};
  m_categoryChoice->setObjectName("SavePrefabDialog_categoryChoice");
  m_categoryChoice->setEditable(true);
  m_categoryChoice->setInsertPolicy(QComboBox::NoInsert);
  for (const auto& category : prefabCategories(library.entries()))
  {
    m_categoryChoice->addItem(pathAsGenericQString(category));
  }
  m_categoryChoice->setCurrentText("");
  m_categoryChoice->setToolTip(
    tr("A folder inside the library; empty for the top level"));

  m_tagsEdit = new QLineEdit{};
  m_tagsEdit->setPlaceholderText(tr("tag; tag; ..."));

  m_kindLabel = new QLabel{};

  m_forceAssemblyCheck = new QCheckBox{tr("Save as assembly anyway")};
  m_forceAssemblyCheck->setToolTip(
    tr("Save the entity as a one-entity group whose placements are linked"));
  // the override is offered only where it changes the kind
  m_forceAssemblyCheck->setVisible(m_singleEntity);

  const auto externalLinks = mdl::findExternalLinks(m_nodes);
  m_clearLinksCheck = new QCheckBox{tr("Clear links that leave the selection")};
  m_clearLinksCheck->setChecked(true);

  m_externalLinksList = new QListWidget{};
  m_externalLinksList->setSelectionMode(QAbstractItemView::NoSelection);
  m_externalLinksList->setFocusPolicy(Qt::NoFocus);
  m_externalLinksList->setMaximumHeight(100);
  for (const auto& link : externalLinks)
  {
    m_externalLinksList->addItem(describeExternalLink(link));
  }
  if (externalLinks.empty())
  {
    m_externalLinksList->addItem(tr("(none)"));
  }

  m_includeDefaultsCheck = new QCheckBox{tr("Include definition defaults")};
  m_includeDefaultsCheck->setToolTip(
    tr("Write every default of the entity definition that the entity lacks as its own "
       "key"));

  m_messageLabel = new QLabel{};
  m_messageLabel->setObjectName("SavePrefabDialog_messageLabel");
  setInfoStyle(m_messageLabel);
  m_messageLabel->setWordWrap(true);

  auto* formLayout = new QFormLayout{};
  formLayout->setContentsMargins(
    LayoutConstants::DialogOuterMargin,
    LayoutConstants::DialogOuterMargin,
    LayoutConstants::DialogOuterMargin,
    LayoutConstants::DialogOuterMargin);
  formLayout->setHorizontalSpacing(LayoutConstants::WideHMargin);
  formLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
  formLayout->addRow(tr("Name"), m_nameEdit);
  formLayout->addRow(tr("Library"), m_rootChoice);
  formLayout->addRow(tr("Category"), m_categoryChoice);
  formLayout->addRow(tr("Tags"), m_tagsEdit);
  formLayout->addRow(tr("Kind"), m_kindLabel);
  formLayout->addRow("", m_forceAssemblyCheck);
  formLayout->addRow("", m_clearLinksCheck);
  formLayout->addRow("", m_externalLinksList);
  formLayout->addRow("", m_includeDefaultsCheck);
  formLayout->addRow("", m_messageLabel);

  auto* buttonBox = new QDialogButtonBox{};
  m_saveButton = buttonBox->addButton(tr("Save"), QDialogButtonBox::AcceptRole);
  m_saveButton->setObjectName("SavePrefabDialog_saveButton");
  buttonBox->addButton(QDialogButtonBox::Cancel);
  connect(buttonBox, &QDialogButtonBox::accepted, this, &SavePrefabDialog::accept);
  connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

  auto* outerLayout = new QVBoxLayout{};
  outerLayout->setContentsMargins(QMargins{});
  outerLayout->setSpacing(0);
  outerLayout->addLayout(formLayout, 1);
  outerLayout->addLayout(wrapDialogButtonBox(buttonBox), 0);
  insertTitleBarSeparator(outerLayout);
  setLayout(outerLayout);

  connect(m_nameEdit, &QLineEdit::textChanged, this, [&]() { updateControls(); });
  connect(m_rootChoice, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [&]() {
    updateControls();
  });
  connect(
    m_categoryChoice, &QComboBox::currentTextChanged, this, [&]() { updateControls(); });
  connect(m_forceAssemblyCheck, &QCheckBox::toggled, this, [&]() { updateControls(); });
  connect(m_clearLinksCheck, &QCheckBox::toggled, this, [&]() { updateControls(); });

  setMinimumWidth(400);
}

void SavePrefabDialog::updateControls()
{
  const auto kind = m_singleEntity && !m_forceAssemblyCheck->isChecked()
                      ? mdl::PrefabKind::Entity
                      : mdl::PrefabKind::Assembly;
  m_kindLabel->setText(
    kind == mdl::PrefabKind::Entity ? tr("Entity snapshot (placed as a plain entity)")
                                    : tr("Assembly (placed as a linked group)"));

  m_externalLinksList->setEnabled(m_clearLinksCheck->isChecked());

  // the name, the category and the root give the file, or the reason there is none
  const auto hasRoot = m_rootChoice->count() > 0;
  auto message = hasRoot ? QString{} : tr("Set a game path or a Prefab Path first");
  const auto hasPath = mdl::prefabFilePath(m_document.map().prefabLibrary(), options())
                       | kdl::transform([](const auto&) { return true; })
                       | kdl::transform_error([&](const auto& e) {
                           if (message.isEmpty())
                           {
                             message = QString::fromStdString(e.msg);
                           }
                           return false;
                         })
                       | kdl::value();
  m_rootChoice->setEnabled(hasRoot);
  m_messageLabel->setText(message);
  m_messageLabel->setVisible(!message.isEmpty());
  m_saveButton->setEnabled(hasPath && !m_nodes.empty());
}

mdl::SavePrefabOptions SavePrefabDialog::options() const
{
  auto options = mdl::SavePrefabOptions{};
  options.name = m_nameEdit->text().trimmed().toStdString();
  options.category = pathFromQString(m_categoryChoice->currentText().trimmed());
  options.tags = splitTags(m_tagsEdit->text());
  options.root = m_rootChoice->count() > 0
                   ? static_cast<mdl::PrefabRoot>(m_rootChoice->currentData().toInt())
                   : mdl::PrefabRoot::User;
  options.forceAssembly = m_singleEntity && m_forceAssemblyCheck->isChecked();
  options.clearExternalLinks = m_clearLinksCheck->isChecked();
  options.includeDefaults = m_includeDefaultsCheck->isChecked();
  return options;
}

} // namespace tb::ui
