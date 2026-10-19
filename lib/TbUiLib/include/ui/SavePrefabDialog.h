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

#pragma once

#include <QDialog>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;

namespace tb
{
namespace mdl
{
class Node;
struct PrefabEntry;
struct SavePrefabOptions;
} // namespace mdl

namespace ui
{
class MapDocument;

/**
 * The name a new prefab of the given selection gets by default: the selected entity's
 * name, else the selected group's name, else the entity's classname, else "prefab".
 */
std::string defaultPrefabName(const MapDocument& document);

/**
 * Asks for the name, place and options of a new prefab made from the selection and
 * writes it. Saving a prefab does not touch the document.
 */
class SavePrefabDialog : public QDialog
{
  Q_OBJECT
private:
  MapDocument& m_document;
  // the selection, normalised once (normalizePrefabSelection)
  std::vector<mdl::Node*> m_nodes;
  // the selection is one entity, so that it can be saved as either kind
  bool m_singleEntity = false;
  std::optional<std::filesystem::path> m_savedPath;
  // the file of the prefab that the dialog updates (forEntry), as fileKey gives it
  std::optional<std::filesystem::path> m_updatedPath;

  QLineEdit* m_nameEdit = nullptr;
  // the library's root, where the file goes (DD-32: there is no choice)
  QLabel* m_locationLabel = nullptr;
  QComboBox* m_categoryChoice = nullptr;
  QLineEdit* m_tagsEdit = nullptr;
  // DD-33: at most mdl::PrefabDescriptionMaxLength characters
  QPlainTextEdit* m_descriptionEdit = nullptr;
  QLabel* m_descriptionCountLabel = nullptr;
  QLabel* m_kindLabel = nullptr;
  QCheckBox* m_forceAssemblyCheck = nullptr;
  QCheckBox* m_clearLinksCheck = nullptr;
  QListWidget* m_externalLinksList = nullptr;
  QCheckBox* m_includeDefaultsCheck = nullptr;
  QLabel* m_messageLabel = nullptr;
  QPushButton* m_saveButton = nullptr;

public:
  explicit SavePrefabDialog(MapDocument& document, QWidget* parent = nullptr);
  ~SavePrefabDialog() override;

  /**
   * Turns the dialog into "Update Prefab from Selection" (D13): the name, the category,
   * the tags and the description are those of the given prefab, the kind is derived from
   * the selection as always, and the prefab's file is overwritten without asking. If the
   * fields are changed so that they name another file, that file is asked about as usual.
   */
  void forEntry(const mdl::PrefabEntry& entry);

  /**
   * The file that was written, once the dialog was accepted.
   */
  const std::optional<std::filesystem::path>& savedPath() const;

  void accept() override;

private:
  void createGui();
  void updateControls();
  // cuts the description to its limit and shows its length (DD-33)
  void limitDescription();
  // the options name the file that forEntry was given
  bool updatesEntryFile(const std::filesystem::path& path) const;

  mdl::SavePrefabOptions options() const;
};

} // namespace ui
} // namespace tb
