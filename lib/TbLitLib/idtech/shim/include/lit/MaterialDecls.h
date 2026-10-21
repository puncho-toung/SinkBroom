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

// The facade over the engine's material parser in TbLitIdLib (D12, C7). It uses standard
// types only: no id type and no id macro reaches a file that includes it. It compiles as
// C++17 and C++20.
//
// Threading: every member of MaterialDecls is single-threaded, and it must stay on one
// thread for its whole life, because the copied engine code it drives is bound to process
// globals. lit/MaterialService.h confines every MaterialDecls to the parse thread (D32);
// other threads see only the immutable MaterialEntry values it returns. At most one
// MaterialDecls exists at a time (C17). The two pure name functions, canonicalName and
// compareEnginePaths, touch no registry state.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tb::lit
{
namespace shim
{
class Registry;
}

// a loose decl file and its time, for reload polling
struct DeclFileTime
{
  // relative to the game folder ("materials/base_wall.mtr")
  std::string path;
  std::int64_t time = 0;
  std::uint64_t size = 0;

  bool operator==(const DeclFileTime& other) const;
  bool operator!=(const DeclFileTime& other) const;
};

// The game files the registry reads decl text from. It answers the two questions the
// engine's file system answers for the decl manager: the files of a folder with an
// extension (idFileSystem::ListFiles, names relative to the folder) and a file's text and
// time (idFileSystem::ReadFile). lit/GameDeclSource.h implements it with the game-file
// search. The registry calls it only on the thread that owns the registry. A source may
// throw: the registry's file system catches and reports it (C17).
class DeclSource
{
public:
  virtual ~DeclSource();

  // the names of the files in folder ending in extension (".mtr"), relative to folder, in
  // the order GetFileList adds them; the registry adds each once case-insensitively and
  // sorts them as the engine does
  virtual std::vector<std::string> listFiles(
    const std::string& folder, const std::string& extension) = 0;

  // the file's bytes and its time, or nullopt when it cannot be read; path is relative to
  // the game folder ("materials/base_wall.mtr"). A read may open and keep containers
  // until closeFiles.
  virtual std::optional<std::string> readFile(
    const std::string& path, std::int64_t* timestamp) = 0;

  // the loose files of folder ending in extension, with their times, sorted by path; only
  // loose decl folders are polled for a reload (R4-3.26), so a source without loose files
  // returns none
  virtual std::vector<DeclFileTime> looseFileTimes(
    const std::string& folder, const std::string& extension) const;

  // what the source could not read (an unreadable or corrupt container, ...), oldest
  // first; the registry's owner logs them
  virtual std::vector<std::string> takeWarnings();

  // closes every game file the source holds open; the registry's owner calls it after
  // each read batch, so no container stays open (R4-3.26)
  virtual void closeFiles();

  // a file kept changing while it was read, so what was read may be torn; the registry's
  // owner builds again (A3.5)
  virtual bool changedWhileRead() const;
};

// idStr::IcmpPath, the engine's path order (case-insensitive, folders first): negative,
// zero or positive. A pure function, as in the engine.
int compareEnginePaths(const std::string& lhs, const std::string& rhs);

// one decl file held in memory
struct DeclFile
{
  std::string path;
  // nullopt makes the file listed but unreadable
  std::optional<std::string> text;
  std::int64_t timestamp = 0;
};

// decl text given in memory (LP1a.1 and the tests)
class MemoryDeclSource : public DeclSource
{
public:
  explicit MemoryDeclSource(std::vector<DeclFile> files);

  std::vector<std::string> listFiles(
    const std::string& folder, const std::string& extension) override;
  std::optional<std::string> readFile(
    const std::string& path, std::int64_t* timestamp) override;

private:
  std::vector<DeclFile> m_files;
};

enum class DeclMessageKind
{
  // idCommon::Printf
  Info,
  // idCommon::Warning and idLexer::Warning (developer prints and warnings are dropped, as
  // at com_developer 0)
  Warning,
  // idCommon::Error: the engine would have stopped the frame here
  Error,
  // idCommon::FatalError: the engine would have quit here
  FatalError,
  // a failed assert in the copied code
  Assert,
};

struct DeclMessage
{
  DeclMessageKind kind;
  std::string text;
};

// An image as a material stage names it: the key of the engine's image record
// (ImageManager.cpp's ImageFromFile). The integers are the engine's enum values, which
// the cache name uses as is (#__UUCC).
struct MaterialImageKey
{
  std::string name;
  // textureUsage_t, e.g. 1 = TD_DIFFUSE, 3 = TD_BUMP, 12 = TD_SPECULAR_PBR_RMAO
  int usage = 0;
  // textureFilter_t: 0 = TF_LINEAR, 1 = TF_NEAREST, 2 = TF_NEAREST_MIPMAP, 3 = TF_DEFAULT
  int filter = 0;
  // textureRepeat_t: 0 = TR_REPEAT, 1 = TR_CLAMP, 2 = TR_CLAMP_TO_ZERO, 3 =
  // ..._ZERO_ALPHA
  int repeat = 0;
  // cubeFiles_t: 0 = CF_2D, 1 = CF_NATIVE, 2 = CF_CAMERA, ...
  int cube = 0;
  int cubeMapSize = 0;
  // the engine's intrinsic image of this name (created by CreateIntrinsicImages)
  bool intrinsic = false;
  // globalImages->defaultImage
  bool isDefault = false;

  bool operator==(const MaterialImageKey& other) const;
  bool operator!=(const MaterialImageKey& other) const;
};

// one shaderStage_t of an idMaterial
struct MaterialStage
{
  // stageLighting_t: 0 = SL_AMBIENT, 1 = SL_BUMP, 2 = SL_DIFFUSE, 3 = SL_SPECULAR,
  // 4 = SL_COVERAGE
  int lighting = 0;
  // the GLS_* state bits (blend, alpha, depth func, masks, polygon offset, ...)
  std::uint64_t drawStateBits = 0;
  // register indices: skip the stage when registers[conditionRegister] == 0
  int conditionRegister = 0;
  std::array<int, 4> colorRegisters{};
  bool hasAlphaTest = false;
  int alphaTestRegister = 0;
  bool ignoreAlphaTest = false;
  // stageVertexColor_t: 0 = SVC_IGNORE, 1 = SVC_MODULATE, 2 = SVC_INVERSE_MODULATE
  int vertexColor = 0;
  float privatePolygonOffset = 0.0f;

  // the stage image, or nullopt for a stage without one (cinematic, dynamic, program)
  std::optional<MaterialImageKey> image;
  // texgen_t: 0 = TG_EXPLICIT, 1 = TG_DIFFUSE_CUBE, 2 = TG_REFLECT_CUBE, ...
  int texgen = 0;
  bool hasTextureMatrix = false;
  // register indices of the 2x3 texture matrix
  std::array<std::array<int, 3>, 2> textureMatrixRegisters{};
  // dynamicidImage_t, and the size of a dynamic image
  int dynamicImage = 0;
  int dynamicWidth = 0;
  int dynamicHeight = 0;
  // the file a videomap or soundmap stage names, or ""
  std::string cinematic;
  bool soundMap = false;

  // the stage has a newShaderStage_t: a program, vertexParm or fragmentMap stage
  bool newStage = false;
  // the program names FindShader was given, or "" when the stage names none
  std::string vertexProgram;
  std::string fragmentProgram;
  // register indices of the vertex parms, and the fragment-map images
  std::vector<std::array<int, 4>> vertexParms;
  std::vector<std::optional<MaterialImageKey>> fragmentImages;

  bool hasStencil = false;
};

// one parsed idMaterial: plain data, no id pointer (D32)
struct MaterialEntry
{
  // the registry that made the entry (MaterialDecls::id); evaluate refuses an entry of
  // another registry, also one of an earlier build of the same game
  std::uint64_t registry = 0;
  // the decl's canonical name
  std::string name;
  // the decl file the text came from, or "<implicit file>" for an implicit material
  std::string sourceFile;
  int sourceLine = 0;
  // the decl's index among the registry's materials
  int index = -1;
  // the name had no decl; the engine generated the text (idMaterial::SetDefaultText)
  bool implicit = false;
  // DS_DEFAULTED: the parse failed and MakeDefault replaced the material with the default
  // definition
  bool defaulted = false;

  // materialFlags_t bits
  int materialFlags = 0;
  int contentFlags = 0;
  int surfaceFlags = 0;
  // materialSort_t values or an explicit sort number
  float sort = 0.0f;
  // materialCoverage_t: 0 = MC_BAD, 1 = MC_OPAQUE, 2 = MC_PERFORATED, 3 = MC_TRANSLUCENT
  int coverage = 0;
  // cullType_t: 0 = CT_FRONT_SIDED, 1 = CT_BACK_SIDED, 2 = CT_TWO_SIDED
  int cullType = 0;
  float polygonOffset = 0.0f;
  // deform_t, its registers and the particle decl a particle deform names
  int deform = 0;
  std::array<int, 4> deformRegisters{};
  std::string deformDecl;
  int spectrum = 0;
  bool fogLight = false;
  bool blendLight = false;
  bool ambientLight = false;
  bool receivesFog = false;
  bool portalSky = false;
  bool hasSubview = false;
  bool receivesLighting = false;
  bool surfaceCastsShadow = false;
  int entityGui = 0;
  std::string gui;
  // qer_editorimage as the decl spells it, "" for none; the engine makes the image only
  // when an editor asks for it (GetEditorImage)
  std::string editorImageName;
  std::optional<MaterialImageKey> lightFalloffImage;
  std::optional<MaterialImageKey> fastPathBumpImage;
  std::optional<MaterialImageKey> fastPathDiffuseImage;
  std::optional<MaterialImageKey> fastPathSpecularImage;

  int numRegisters = 0;
  // the registers are constant (CheckForConstantRegisters kept them)
  bool constantRegisters = false;
  bool hasAmbient = false;
  std::vector<MaterialStage> stages;
};

// the expression registers idMaterial::EvaluateRegisters filled in
struct EvaluatedRegisters
{
  std::vector<float> registers;
  // empty, unless the evaluation was refused (a material of an earlier registry, or one
  // whose parse failed); then registers is empty and this says why
  std::string error;
};

// a value, or the error the engine would have stopped at
template <typename T>
struct DeclResult
{
  std::optional<T> value;
  std::string error;

  explicit operator bool() const { return value.has_value(); }
};

// a decl folder the registry registered (RegisterDeclFolder)
struct DeclFolderInfo
{
  std::string folder;
  std::string extension;
};

// the decls of one registered type ("material", "table", ...)
struct DeclTypeCount
{
  std::string type;
  int count = 0;
};

// The decl registry around the copied parser. Constructing it reads every decl folder the
// engine reads, in the engine's order, through the source; parsing is lazy, as in the
// engine. Unknown material names get the engine's implicit material.
class MaterialDecls
{
public:
  // the number of entity shaderParms (MAX_ENTITY_SHADER_PARMS)
  static constexpr int ShaderParmCount = 12;

  // source must outlive this object; once cancel is set, the decl files not read yet are
  // read as empty, so a construction stops after the file it is reading (R5.20). Never
  // throws: a failure is the construction error.
  explicit MaterialDecls(DeclSource& source, const std::atomic<bool>* cancel = nullptr);
  ~MaterialDecls();

  // the decl folders the registry registers, in the engine's order (D33): materials,
  // skins, sound, then the game's def, fx, particles, af and newpdas
  static std::vector<DeclFolderInfo> engineDeclFolders();

  MaterialDecls(const MaterialDecls&) = delete;
  MaterialDecls& operator=(const MaterialDecls&) = delete;

  // non-empty when the registry could not be built (another one exists, or it threw)
  const std::string& constructionError() const;

  // unique in the process; every entry this registry hands out carries it
  std::uint64_t id() const;
  // the cancel flag cut the construction short: some decl files were left unread
  bool cancelled() const;

  // the material of this name: its decl, or the implicit material the engine makes for a
  // name without one; an error when its parse reached an engine Error or FatalError,
  // where the engine would not have returned (then on every later lookup too)
  DeclResult<std::shared_ptr<const MaterialEntry>> lookup(std::string_view name);

  // idMaterial::EvaluateRegisters with these entity shaderParms, global shaderParms of 0,
  // the time in seconds and no sound emitter
  DeclResult<EvaluatedRegisters> evaluate(
    const MaterialEntry& material,
    const std::array<float, ShaderParmCount>& shaderParms,
    float time);

  // the engine's canonical decl name (DeclManager.cpp's MakeNameCanonical): lowercase,
  // backslashes to slashes, cut at the last dot
  static std::string canonicalName(std::string_view name);

  // the material decl at this index among the registry's materials, parsed, as lookup
  // returns it (bulk parsing)
  DeclResult<std::shared_ptr<const MaterialEntry>> lookupIndex(int materialIndex);
  // the canonical name of the material at this index, without parsing it; "" for no index
  std::string materialName(int materialIndex) const;
  // parses the table decl at this index; its name, or the engine error its parse reached
  DeclResult<std::string> parseTable(int tableIndex);

  // the number of decls of each kind the registry holds
  int materialCount() const;
  int tableCount() const;
  int entityDefCount() const;
  // every registered decl type with its number of decls, in the engine's type order
  std::vector<DeclTypeCount> declCounts() const;
  // the decls of a type (an engine type name, "material") in the DS_DEFAULTED state: a
  // parse failed and MakeDefault replaced them
  int defaultedCount(std::string_view typeName) const;
  // the names of every decl of a type, in index order, without parsing any (the engine's
  // "list<Type>s all")
  std::vector<std::string> declNames(std::string_view typeName) const;
  // every image record the parses made so far, in the order they were made (the engine's
  // listImages), also those of a parse that failed later
  std::vector<MaterialImageKey> imageKeys() const;
  // the registered decl folders, in the engine's order (D33)
  std::vector<DeclFolderInfo> declFolders() const;
  // the decl files read, and their bytes (the engine's listDecls totals)
  int declFileCount() const;
  std::size_t declFileBytes() const;

  // the merged keys of an entityDef, inherited ones included, in the dict's order
  DeclResult<std::vector<std::pair<std::string, std::string>>> entityDefKeys(
    std::string_view name);

  // The console's "touch <type> <name>" (TouchDecl_f): FindType without making a default,
  // which parses the decl and marks it referenced. False when there is no such decl; an
  // error when the parse reached an engine error.
  DeclResult<bool> touch(std::string_view typeName, std::string_view name);

  // The engine's console output for "print<Type> <name>" (printMaterial, printTable,
  // ...): the copied idDeclManagerLocal::PrintType with the type's own Print, as the
  // engine's console receives it, newlines included. Like the command it neither parses
  // nor references the decl; touch first does both.
  DeclResult<std::string> printDecl(std::string_view typeName, std::string_view name);

  // the bytes of decl text the registry keeps (all decl types, R5.10)
  std::size_t declTextBytes() const;

  // the messages the copied code reported, oldest first
  const std::vector<DeclMessage>& messages() const;
  std::vector<DeclMessage> takeMessages();

private:
  std::uint64_t m_id = 0;
  bool m_claimed = false;
  std::string m_constructionError;
  std::vector<DeclMessage> m_constructionMessages;
  std::unique_ptr<shim::Registry> m_registry;
  // the entries handed out so far, by material index; a material is parsed once
  std::vector<std::shared_ptr<const MaterialEntry>> m_materials;
  // the engine error a material's parse reached, by material index; the half-parsed
  // material is never handed out
  std::vector<std::string> m_failures;
};

} // namespace tb::lit
