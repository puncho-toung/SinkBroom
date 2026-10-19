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
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextDocument>

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

#include "kd/path_utils.h"
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
 * The form in which two spellings of one file's path are equal: lexically normal, and in
 * lower case on Windows, where file names ignore case.
 */
std::filesystem::path fileKey(const std::filesystem::path& path)
{
#ifdef _WIN32
  return kdl::path_to_lower(path.lexically_normal());
#else
  return path.lexically_normal();
#endif
}

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
  limitDescription();
  updateControls();
}

SavePrefabDialog::~SavePrefabDialog() = default;

void SavePrefabDialog::forEntry(const mdl::PrefabEntry& entry)
{
  m_updatedPath = fileKey(entry.absolutePath);

  setWindowTitle(tr("Update Prefab from Selection"));
  m_saveButton->setText(tr("Update"));

  m_nameEdit->setText(
    entry.metadata.name.empty() ? pathAsQString(entry.relativePath.stem())
                                : QString::fromStdString(entry.metadata.name));
  m_categoryChoice->setCurrentText(
    pathAsGenericQString(entry.relativePath.parent_path()));
  m_tagsEdit->setText(QString::fromStdString(kdl::str_join(entry.metadata.tags, "; ")));
  m_descriptionEdit->setPlainText(QString::fromStdString(entry.metadata.description));

  updateControls();
}

const std::optional<std::filesystem::path>& SavePrefabDialog::savedPath() const
{
  return m_savedPath;
}

bool SavePrefabDialog::updatesEntryFile(const std::filesystem::path& path) const
{
  // per keystroke, so the paths are compared without opening the files (R2-pf-ui-10)
  return m_updatedPath && fileKey(path) == *m_updatedPath;
}

void SavePrefabDialog::accept()
{
  const auto options = this->options();

  auto ec = std::error_code{};
  if (const auto path = mdl::prefabFilePath(m_document.map().prefabLibrary(), options)
                        | kdl::value_or(std::filesystem::path{});
      !path.empty() && std::filesystem::is_regular_file(path, ec)
      && !updatesEntryFile(path))
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
  m_nameEdit->setObjectName("SavePrefabDialog_nameEdit");

  m_locationLabel = new QLabel{};
  m_locationLabel->setObjectName("SavePrefabDialog_locationLabel");
  m_locationLabel->setWordWrap(true);
  m_locationLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  m_locationLabel->setToolTip(
    tr("<game path>/<mod>/maps/prefabs, or the Prefab Path of Preferences > Games when "
       "that is set"));
  if (const auto root = library.writeRoot())
  {
    m_locationLabel->setText(pathAsQString(library.writeRootFor(*root)));
  }
  else
  {
    m_locationLabel->setText(tr("(none)"));
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
  m_tagsEdit->setObjectName("SavePrefabDialog_tagsEdit");
  m_tagsEdit->setPlaceholderText(tr("tag; tag; ..."));

  m_descriptionEdit = new QPlainTextEdit{};
  m_descriptionEdit->setObjectName("SavePrefabDialog_descriptionEdit");
  m_descriptionEdit->setPlaceholderText(
    tr("What it is and how to use it; shown in the "
       "prefab's tooltip"));
  m_descriptionEdit->setTabChangesFocus(true);
  m_descriptionEdit->setFixedHeight(
    m_descriptionEdit->fontMetrics().lineSpacing() * 4
    + 2 * static_cast<int>(m_descriptionEdit->document()->documentMargin())
    + 2 * m_descriptionEdit->frameWidth());

  m_descriptionCountLabel = new QLabel{};
  m_descriptionCountLabel->setObjectName("SavePrefabDialog_descriptionCountLabel");
  setInfoStyle(m_descriptionCountLabel);

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
  formLayout->addRow(tr("Folder"), m_locationLabel);
  formLayout->addRow(tr("Category"), m_categoryChoice);
  formLayout->addRow(tr("Tags"), m_tagsEdit);
  formLayout->addRow(tr("Description"), m_descriptionEdit);
  formLayout->addRow("", m_descriptionCountLabel);
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
  connect(
    m_descriptionEdit, &QPlainTextEdit::textChanged, this, [&]() { limitDescription(); });
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
  const auto hasRoot = m_document.map().prefabLibrary().writeRoot().has_value();
  auto message = hasRoot ? QString{} : tr("Set a game path or a Prefab Path first");
  const auto hasPath = mdl::prefabFilePath(m_document.map().prefabLibrary(), options())
                       | kdl::transform([&](const auto& path) {
                           if (m_updatedPath && !updatesEntryFile(path))
                           {
                             message = tr(
                               "These fields name another file than the "
                               "prefab's; that file is written instead");
                           }
                           return true;
                         })
                       | kdl::transform_error([&](const auto& e) {
                           if (message.isEmpty())
                           {
                             message = QString::fromStdString(e.msg);
                           }
                           return false;
                         })
                       | kdl::value();
  m_messageLabel->setText(message);
  m_messageLabel->setVisible(!message.isEmpty());
  m_saveButton->setEnabled(hasPath && !m_nodes.empty());
}

void SavePrefabDialog::limitDescription()
{
  const auto text = m_descriptionEdit->toPlainText();
  if (text.size() > mdl::PrefabDescriptionMaxLength)
  {
    // typing or pasting past the limit keeps the first characters; this edit calls
    // limitDescription again, which then finds the text short enough
    m_descriptionEdit->setPlainText(text.left(mdl::PrefabDescriptionMaxLength));
    m_descriptionEdit->moveCursor(QTextCursor::End);
    return;
  }
  m_descriptionCountLabel->setText(
    tr("%1 / %2").arg(text.size()).arg(mdl::PrefabDescriptionMaxLength));
}

mdl::SavePrefabOptions SavePrefabDialog::options() const
{
  auto options = mdl::SavePrefabOptions{};
  options.name = m_nameEdit->text().trimmed().toStdString();
  options.category = pathFromQString(m_categoryChoice->currentText().trimmed());
  options.tags = splitTags(m_tagsEdit->text());
  options.description = m_descriptionEdit->toPlainText().toStdString();
  options.root =
    m_document.map().prefabLibrary().writeRoot().value_or(mdl::PrefabRoot::User);
  options.forceAssembly = m_singleEntity && m_forceAssemblyCheck->isChecked();
  options.clearExternalLinks = m_clearLinksCheck->isChecked();
  options.includeDefaults = m_includeDefaultsCheck->isChecked();
  return options;
}

} // namespace tb::ui
