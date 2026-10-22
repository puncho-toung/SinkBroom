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
// MaterialDecls exists at a time (C17). The pure functions canonicalName,
// compareEnginePaths and findKey touch no registry state, and the engine-message
// functions of D63 may be called from any thread.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
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

/**
 * A5: what the C8 search finds for a game file without reading its body (the engine's
 * ReadFile with no buffer, GetTimestamp): where the file is, and its time as the engine
 * keeps times (A4).
 */
struct GameFileProbe
{
  enum class Kind
  {
    Missing,
    Loose,
    Pk4,
    Resources,
  };

  Kind kind = Kind::Missing;
  // the container's name as the engine registers it ("_rbdoom_core.pk4",
  // "maps/admin.resources"); empty for a loose or a missing file
  std::string container;
  // FILE_NOT_FOUND_TIMESTAMP (-1) for a missing file; a loose file's modification time in
  // whole Unix seconds (Sys_FileTimeStamp); 0 inside a container (SC95)
  std::int64_t time = -1;
  // ReadFile's length: a loose file's or a .resources entry's size; 0 for a .pk4 entry,
  // whose size only inflating it tells (no copied caller reads it), and for no file
  std::uint64_t size = 0;
  // a loose file's modification time in the file system's finest unit, which tells two
  // saves within one second apart; 0 otherwise. For polling only, never an engine time
  std::int64_t changeStamp = 0;

  bool exists() const;
  bool operator==(const GameFileProbe& other) const;
  bool operator!=(const GameFileProbe& other) const;
};

// The game files the registry reads decl text from, and the preview reads its other game
// files from (images, bakes; C8). It answers the questions the engine's file system
// answers: the files of a folder with an extension (idFileSystem::ListFiles, names
// relative to the folder), a file's text and time (idFileSystem::ReadFile), and, without
// reading the file, its time (ReadFile with no buffer). lit/GameFileSource.h implements
// it with the game-file search. The registry calls it only on the thread that owns the
// registry, and so does every other caller (the parse thread, C20). A source may throw:
// the registry's file system catches and reports it (C17).
class DeclSource
{
public:
  virtual ~DeclSource();

  // the names of the files in folder ending in extension (".mtr"), relative to folder, in
  // the order GetFileList adds them; the registry adds each once case-insensitively and
  // sorts them as the engine does
  virtual std::vector<std::string> listFiles(
    const std::string& folder, const std::string& extension) = 0;

  // the file's bytes and its time (A4: Unix seconds, 0 in a container), or nullopt when
  // it cannot be read; path is relative to the game folder ("materials/base_wall.mtr").
  // A read may open and keep containers until closeFiles.
  virtual std::optional<std::string> readFile(
    const std::string& path, std::int64_t* timestamp) = 0;

  // A5: the file a read of path finds, and its time, without reading its body; the
  // default reads it (readFile)
  virtual GameFileProbe probe(const std::string& path);

  // C8: the bytes of any game file (an image cache, a bake), searched as readFile
  // searches, with where it was found in found; unlike readFile it is not a decl read,
  // so it never asks for another build. The default is readFile's
  virtual std::optional<std::string> readGameFile(
    const std::string& path, GameFileProbe* found);

  // LP1c.2 (C21): the first maxBytes bytes of a game file (all of a shorter one),
  // searched as readGameFile searches, with where it was found in found; a header without
  // the body. The default reads the whole file and cuts it
  virtual std::optional<std::string> readGameFileHead(
    const std::string& path, std::size_t maxBytes, GameFileProbe* found);

  // the game-relative path ("maps/x.map") of a file path below the game's base folder,
  // or nullopt; a source without a game folder has none
  virtual std::optional<std::string> gameRelativePath(const std::string& filePath) const;

  // idFileSystem::UsingResourceFiles: a .resources container was registered, so the
  // image loader's BFHACK names apply (SC100)
  virtual bool usingResourceFiles() const;

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

  // LP1c fix (S2.2): the registry's owner polls the loose files: a source that keeps a
  // listing of them looks again (GameFileSource); the default does nothing
  virtual void looseFilesMayHaveChanged();

  // a file kept changing while it was read, so what was read may be torn; the registry's
  // owner builds again (A3.5)
  virtual bool changedWhileRead() const;

  // the containers (.pk4 and .resources files) whose tables the source holds; fewer than
  // the game has when a cancel cut its scan short (G4); none for a source without them
  virtual std::size_t containerCount() const;
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

/**
 * D63: what the copied code reports through common and idLib::common (Printf, Warning,
 * Error, FatalError) and its failed asserts goes to one permanent, thread-safe sink, kept
 * per calling thread, oldest first, until that thread's owner takes it. A registry takes
 * its own thread's messages into MaterialDecls::messages; the owner of any other thread
 * that runs copied code (the session's CPU pool) takes that thread's with this, without
 * the trailing newlines. Any thread.
 */
std::vector<DeclMessage> drainEngineMessagesForThread(std::thread::id thread);

/**
 * B0.1: the sink keeps at most MaxQueuedEngineMessagesOfAllThreads messages of all
 * threads; beyond that a message is dropped and counted for the thread that reported it
 * (B0.5, R2.5, R2.6), and that thread's next drain ends with a Warning that says how many
 * were dropped and, when Errors or FatalErrors were among them, an Error that says how
 * many, so a CPU job that lost an error fails; no other thread's drain reports them, and
 * a discard forgets them. Formatting a message takes MAX_PRINT_MSG (16 KB) of the
 * reporting thread's stack. drainEngineMessagesForThread's caller is its thread's owner.
 */
inline constexpr std::size_t MaxQueuedEngineMessagesOfAllThreads = 4096;
// drops the messages the sink keeps for a thread, unread; any thread
void discardEngineMessagesForThread(std::thread::id thread) noexcept;

// the messages the sink keeps for every thread; any thread
std::size_t pendingEngineMessagesForTesting();

// reports text on the calling thread as the copied code would: Info through
// idLib::common->Printf, Warning through idLib::common->Warning, Error and FatalError
// through common, Assert as a failed assert; the never-returning ones return (C17)
void reportEngineMessageForTesting(DeclMessageKind kind, const std::string& text);

/**
 * LP1c.2 (D51): the size limits of the loader pre-validation. The engine has none (a TGA
 * may declare 65535 × 65535, stb_image allows 2^24 a side); the preview refuses an image
 * file beyond these before a loader allocates for it (M-IMG-DIFF-04).
 */
inline constexpr int MaxImageFileDimension = 16384;
inline constexpr std::int64_t MaxImageFilePixels = std::int64_t(8192) * 8192;
// LP1c fix (S2.4): an OpenEXR or Radiance HDR file, whose decoders hold float planes (16
// bytes and more a pixel), may hold a quarter of that
inline constexpr std::int64_t MaxFloatImageFilePixels = MaxImageFilePixels / 4;

// whether the copied imageLoaders table has a loader for this path's extension (tga,
// png, jpg, exr, hdr, any case). Pure; any thread.
bool isImageLoaderFile(std::string_view path);

// LP1c fix (S3.3): whether the loader for this path's extension is LoadSTB_RGBA8 (png,
// jpg, any case), which decodes by content and whose failure lets R_LoadImage try the
// next extension. Pure; any thread.
bool isStbLoaderFile(std::string_view path);

/**
 * LP1c.2 (D51, R5.3, M-IMG-GUARD-01): why the copied loader that the path's extension
 * picks must not read these bytes, or nullopt when it may. Outside the copied code it
 * checks every read the loader will make: a TGA's header, type, colour map, pixel size,
 * declared size against the file and its run-length packets walked to the last pixel; a
 * PNG's header as stb_image checks it, its chunks to IEND and the declared size against
 * what its IDAT bytes can inflate to; a JPG's frame header as stb_image checks it, its
 * marker segments and scans to EOI and the declared size against its scan bytes; an
 * OpenEXR header through tinyexr's parsers, its channels and its offset table; a Radiance
 * HDR header and its scanlines as stb_image reads them; and every size against the
 * limits above. LP1c fix (S3.3): a .png or .jpg is checked by its content, as
 * LoadSTB_RGBA8's stb_image decodes it: PNG rules for a PNG signature, JPEG rules for a
 * start-of-image marker, else stb_image's own header reader for its other formats (BMP,
 * GIF, PSD, PIC, PNM, HDR, TGA), which checks every read itself, with the size limits. An
 * empty file fails; a path no loader takes passes. Pure; for a .png or .jpg of another
 * format stb_image's failure reason (a plain static) is written: parse thread.
 */
std::optional<std::string> imageFileProblem(
  std::string_view path, std::string_view bytes);

// LP1c.2 (C21): an image file's size as its header declares it, when the loader
// pre-validation passes it, else why not
struct ImageFileHeader
{
  int width = 0;
  int height = 0;
  std::optional<std::string> problem;
  // LP1c fix (S2.2): imageFileHeadHeader only: the header goes on beyond the bytes given
  bool needsMore = false;
};

/**
 * LP1c.2 (C21): imageFileProblem's checks, and the width and height the file's header
 * declares, which are the size its loader gives (R_LoadImage's picture: a TGA's, PNG's or
 * JPG's header, a Radiance HDR's resolution line, an OpenEXR data window), without
 * decoding a pixel. Pure (see imageFileProblem for stb_image's other formats).
 */
ImageFileHeader imageFileHeader(std::string_view path, std::string_view bytes);

/**
 * LP1c fix (S2.2): the size the header in a file's first bytes declares (head; the
 * file's length is fileSize), with imageFileProblem's header rules and the size checks a
 * header alone tells (an uncompressed TGA's pixels against fileSize), without the body:
 * a TGA's 18 bytes, a PNG's IHDR, a JPEG's segments up to its frame header, an OpenEXR
 * header, a Radiance HDR's text header and resolution line, or stb_image's other headers.
 * needsMore (and no problem) when the header goes on beyond head. Run-length data, PNG
 * chunks, JPEG scans and EXR blocks are not walked: a file whose body the loader
 * pre-validation refuses may have a header size here (M-IMG-DIFF-07). Pure, as
 * imageFileProblem.
 */
ImageFileHeader imageFileHeadHeader(
  std::string_view path, std::string_view head, std::uint64_t fileSize);

/**
 * LP1c fix (S2.2): imageFileHeadHeader of a game file read through source's C8 search
 * with small bounded reads (its first 4 KB, then 64 KB, then 1 MB; never the whole
 * file), where it was found in found. A file that cannot be read, or whose header goes on
 * beyond 1 MB, has a problem. Parse thread (the source's).
 */
ImageFileHeader readImageFileHeader(
  DeclSource& source, const std::string& path, GameFileProbe* found);

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

// a game file one of the copied loaders' reads found, and where (C8)
struct FoundGameFile
{
  std::string path;
  GameFileProbe probe;
};

/**
 * LP1c.1 (C8, R5.5, SC92, SC96, SC100): where the engine looks for an image's cache, and
 * the source time it compares the cache's with: idImage::ActuallyLoadImage up to the
 * generated name, with com_productionMode 0.
 */
struct ImageCacheName
{
  // the image's name after the PBR rename (an RMAO usage turns <base>_s into <base>_rmao)
  std::string imageName;
  // textureUsage_t after the timestamp call: a TD_SPECULAR <base>_s whose <base>_rmao
  // sibling exists becomes TD_SPECULAR_PBR_RMAO (12)
  int usage = 0;
  // cubeFiles_t, as the key has it
  int cube = 0;
  // the textureType_t the cache must have: DTT_2D 1, DTT_CUBIC 2, DTT_2D_ARRAY 3
  int textureType = 0;
  // the source time (A4): the newest leaf's Unix seconds, 0 in a container and for a
  // missing source, -1 for CF_2D_ARRAY, which makes no timestamp call
  std::int64_t sourceTime = -1;
  // GetGeneratedName: "<name>#__UUCC", the name's extension after it
  std::string generatedName;
  // the cache file (BinaryImage.cpp GetGeneratedFileName): "generated/images/<generated
  // name>.bimage" with '(' and ',' made '/' and ')' and ' ' removed
  std::string file;
  // the BFHACK rename's cache file, tried when the first is not found and resource files
  // are in use; "" when no rule names this image
  std::string bfhackFile;
  // the engine generates the image (an intrinsic, the default image): it has no cache
  bool generated = false;
  // LP1c.2: the files the time call's probes found, in order (a single image's one leaf,
  // a program's leaves, a cube's sides), for the sizes and the re-probe (C21)
  std::vector<FoundGameFile> sources;
  // LP1c fix (S2.5, S3.4): the names the time call's probes looked for and did not find
  // (Missing probes: the .tga default, the png/tga/jpg/exr/hdr loop, the _rmao sibling),
  // once each, so that the re-probe sees a file that appears later
  std::vector<FoundGameFile> missing;
  // LP1c fix (S2.2): the time call's last probe found its file. A missing leaf ends the
  // copied time call of a 2D image or program, so for one of those this says that every
  // leaf exists (the pixel path then has a picture to make)
  bool resolved = false;
};

/**
 * LP1c fix (S2.1, S2.2): what a six-file cube's sides are, read without decoding: each
 * side's file as the copied time call resolves it (R_LoadImageProgram of the name and
 * the side's suffix, as R_LoadCubeImages names them), and its header's size
 * (readImageFileHeader).
 */
struct CubeSideSizes
{
  // the sides' size (each side square, all alike); 0 with a problem
  int size = 0;
  // why R_LoadCubeImages would fail: a side missing, a side's header refused, sides of
  // different sizes; "" when the six sides would load
  std::string problem;
  // the sides' files and the names their time calls missed, for the re-probe
  std::vector<FoundGameFile> files;
};

// one mip level (one side's, for a cube) in CachedImage::data
struct CachedImageLevel
{
  int level = 0;
  // the cube side (0-5), 0 for a 2D image
  int destZ = 0;
  int width = 0;
  int height = 0;
  std::size_t offset = 0;
  std::size_t size = 0;
};

/**
 * A .bimage as LoadFromGeneratedFile reads it (SC98, LP1c.1), or an image the source
 * fallback made in the same shape (LP1c.2, D24): plain data for the render thread, which
 * LP2 uploads alike. Immutable once published.
 */
struct CachedImage
{
  // the cache file the search found, and where (C8); for a source image its generated
  // name and the first file its loaders read
  std::string file;
  GameFileProbe origin;
  // bimageFile_t: the source time the cache was made from (Unix seconds, A4), the magic
  // (BIM 10 or 11; 0 for a source image), textureType, the format as stored (for a source
  // image the format the engine would store, DeriveOpts'), the format of data (RGBA8 for
  // a stored RGB565, which is expanded as the engine expands it; for a source image RGBA8
  // where the engine would encode DXT1 or DXT5, R11G11B10F where it would encode BC6H),
  // colorFormat (the channel layout of data), the size and the mip count
  std::int64_t sourceTime = 0;
  std::uint32_t magic = 0;
  int textureType = 0;
  int storedFormat = 0;
  int format = 0;
  int colorFormat = 0;
  int width = 0;
  int height = 0;
  int numLevels = 0;
  // made by the source fallback, not read from a cache
  bool fromSource = false;
  // numLevels records, six times for a cube (each level's six sides, as written); each
  // level's bytes in the engine's format: BC blocks as stored (the engine pads its own
  // copy to NVRHI's row pitch; LP2 pads at upload), RGBA8 as stored, RGB565 as RGBA8
  std::vector<CachedImageLevel> levels;
  std::vector<std::uint8_t> data;
};

/**
 * LP1c.2 (S14, D51): an image's source as idImage::ActuallyLoadImage's pixel path reads
 * it with the copied loaders: R_LoadImageProgram for a 2D image, R_LoadCubeImages for a
 * cube. Plain data.
 */
struct SourcePixels
{
  // a 2D image's size; a cube's side
  int width = 0;
  int height = 0;
  // 1, or 6 for a cube, in R_LoadCubeImages' side order
  int faces = 1;
  // textureUsage_t after the pixel path (heightmap and addnormals make a program TD_BUMP)
  int usage = 0;
  // the time the pixel path found: a program's newest leaf's (A4)
  std::int64_t sourceTime = 0;
  // faces × height rows of width × 4 bytes, top row first, as the loaders give them:
  // RGBA8, or R11G11B10F packed in 4 bytes for an image the EXR and HDR loaders read
  std::vector<std::uint8_t> pixels;
  // every game file the loaders read, in order
  std::vector<FoundGameFile> files;
  // LP1c fix (S2.5): the names the loaders looked for and did not find, once each
  std::vector<FoundGameFile> missing;
};

/**
 * LP1c.2: what a source load ended with. pixels, or error with the engine's warning
 * ("Couldn't load image: <name> : <generated name>", "Couldn't load cube image: <name>")
 * when no loader made a picture; rejected names every file the loader pre-validation
 * refused (D51), "<path>: <reason>". A refused TGA, EXR or HDR file makes the image the
 * default however the rest loaded, because the engine would have read past a buffer or
 * stopped on it; a refused .png or .jpg is what stb_image's own failure is in the engine
 * (LP1c fix S3.3): the copied R_LoadImage goes on to the next extension, and the image is
 * what that finds.
 */
struct SourceLoad
{
  std::optional<SourcePixels> pixels;
  std::string error;
  std::vector<std::string> rejected;
  // the copied code's messages while it loaded, when the load was quiet
  std::vector<DeclMessage> messages;
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

/**
 * C21: the upload size of a material's editor image (GetEditorImage), which divides the
 * face texture coordinates (D59). Default is the 8×8 the engine gives a missing image;
 * until LP1c.2 loads images every material has it. Ready is a size an image load
 * measured; the material service keeps the last Ready size of each material across
 * reloads, so that a reload never turns it back into Default (D52).
 */
struct EditorImageSize
{
  enum class State
  {
    Default,
    Ready,
  };

  int width = 8;
  int height = 8;
  State state = State::Default;
  // LP1c.2: why the size is Default (the engine's load defaults the image, or its size is
  // the window's); "" for Ready. Not compared by ==
  std::string reason;

  bool operator==(const EditorImageSize& other) const;
  bool operator!=(const EditorImageSize& other) const;
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
  // C21: Default (8×8) from the registry; the material service applies its kept Ready
  // sizes when it publishes the entry
  EditorImageSize editorImageSize;
  // C25: the geometry flags the copied parser sets: the "mikktspace" and
  // "unsmoothedTangents" keywords, and ShouldCreateBackSides (a two-sided material with a
  // lit or texgen stage, which Parse turns front-sided)
  bool mikktspace = false;
  bool unsmoothedTangents = false;
  bool createBackSides = false;
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

// an entity's or an entityDef's keys and values, in order
using EntityKeys = std::vector<std::pair<std::string, std::string>>;

/**
 * C22: one parsed entityDef, plain data, no id pointer. keys is its merged dictionary in
 * the dictionary's order (idDeclEntityDef::Parse): its own keys with "classname" set to
 * its name and the "inherit" keys removed, then the keys of each entityDef it inherits
 * that it does not set already (idDict::SetDefaults).
 *
 * The engine looks keys up in such a dictionary case-insensitively, and an entity's map
 * keys go into its dictionary with Set, so a key given twice keeps the last value (plan
 * §13 A10); TrenchBroom's own key lookups are case-sensitive and keep the first. Look
 * keys up here, and in an entity's map keys, with findKey.
 */
struct EntityDefEntry
{
  // the registry that made the entry (MaterialDecls::id)
  std::uint64_t registry = 0;
  // the decl's canonical name
  std::string name;
  EntityKeys keys;
  // idDeclEntityDef::IsMapOnlyClass: "dmapOnly" is set in the classname's own def (not
  // the map keys, not an _mp def): dmap and the game drop the entity's primitives
  bool dmapOnly = false;
};

/**
 * The engine's lookup rule of A10: the value of the last key equal to name when ASCII
 * letters are compared without case (idDict::FindKey's idStr::Icmp; Set replaces a value
 * in place), or null; an empty name finds nothing, as FindKey's does. A pure function;
 * any thread.
 */
const std::string* findKey(const EntityKeys& keys, std::string_view name);

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
  // read as empty, so a construction stops after the file it is reading (R5.20). A
  // registry that cannot be built is the construction error; only an allocation failure
  // (or afterBuildForTesting, called once the registry is built) leaves by an exception,
  // and then the registry is destroyed and the one-registry claim released (W.5).
  explicit MaterialDecls(
    DeclSource& source,
    const std::atomic<bool>* cancel = nullptr,
    const std::function<void()>& afterBuildForTesting = {});
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

  // C22: the entityDef of a name (FindType without a default, which parses it once), with
  // its merged keys and dmapOnly; an error when there is none or its parse reached an
  // engine error
  DeclResult<std::shared_ptr<const EntityDefEntry>> entityDef(std::string_view name);

  // The console's "touch <type> <name>" (TouchDecl_f): FindType without making a default,
  // which parses the decl and marks it referenced. False when there is no such decl; an
  // error when the parse reached an engine error.
  DeclResult<bool> touch(std::string_view typeName, std::string_view name);

  // The engine's console output for "print<Type> <name>" (printMaterial, printTable,
  // ...): the copied idDeclManagerLocal::PrintType with the type's own Print, as the
  // engine's console receives it, newlines included. Like the command it neither parses
  // nor references the decl; touch first does both.
  DeclResult<std::string> printDecl(std::string_view typeName, std::string_view name);

  /**
   * LP1c.1 (C8, R5.5): the cache an image of this key is looked for in, and the time its
   * source has, as ActuallyLoadImage works them out before it opens the cache: the copied
   * R_LoadImageProgram timestamp call (R_LoadCubeImages' for a cube), whose leaves' time
   * comes from the source's probe without reading a file (A5), then the PBR rename,
   * GetGeneratedName, GetGeneratedFileName and the BFHACK name. The key need not be one
   * of the registry's records (a bake's probe images). An error when the call reached an
   * engine error. Parse thread (C20).
   */
  DeclResult<ImageCacheName> imageCacheName(const MaterialImageKey& key);

  /**
   * LP1c.2 (S14, D51): the source of an image as ActuallyLoadImage's pixel path reads it,
   * after the cache name (Image_load.cpp:716-865 up to Load2DFromMemory): the copied
   * R_LoadCubeImages for CF_NATIVE, CF_CAMERA, CF_QUAKE1, CF_SINGLE and CF_PANORAMA (a
   * side size of 0 fails), the copied R_LoadImageProgram of name.imageName with
   * name.usage for anything else, every file each loader reads validated first by
   * imageFileProblem. cubeMapSize is the key's. A quiet load keeps the copied code's
   * messages in its result instead of the registry's (the editor-image sizes, C21). Parse
   * thread (C20); one image's files at most, so a stop waits for one image (R5.20).
   *
   * LP1c fix (S2.1): before the copied R_LoadCubeImages reads a six-file cube's pixels,
   * its sides are checked by their headers (cubeSideSizes): a side missing or of another
   * size fails the load without the copy, which would leak the mismatched side
   * (M-IMG-DEFECT-02). When the copy returns false it has freed the sides it loaded
   * itself, and nothing more is freed; a loaded cube of side 0 is freed whole.
   */
  SourceLoad loadImagePixels(const ImageCacheName& name, int cubeMapSize, bool quiet);

  /**
   * LP1c fix (S2.1, S2.2): the sides of a cube that R_LoadCubeImages loads from six
   * files (CF_NATIVE, CF_CAMERA, CF_QUAKE1, and CF_SINGLE with no cube size): each side's
   * file as the copied time call resolves its name (R_LoadImageProgram, no pixels), its
   * header through the source (readImageFileHeader, no body), and whether the six are one
   * square size, as R_LoadCubeImages' loop requires. A problem for any other cube kind.
   * Parse thread (C20).
   */
  CubeSideSizes cubeSideSizes(const ImageCacheName& name, int cubeMapSize);

  /**
   * LP1c.2 (D24, S14): the levels idBinaryImage::Load2DFromMemory (a 2D image) or
   * LoadCubeFromMemory (six faces) would hand its encoder, with
   * image_highQualityCompression 0 and the encoder skipped (no BC on the CPU): the 2D
   * colour conversion per colorFormat (YCoCg through the copied
   * idColorSpace::ConvertRGBToCoCg_Y, the normal map's x into alpha, green-alpha), the
   * mip chain through the copied R_MipMap, R_MipMapWithGamma or R_MipMapR11G11B10F, and
   * each level's bytes as the engine stores them for an uncompressed format (RGB565 high
   * byte first, LUM8, INT8, ALPHA, L8A8, RG16F, R11G11B10F, RGBA8), as RGBA8 where it
   * would encode DXT1 or DXT5 and as R11G11B10F where it would encode BC6H. format,
   * colorFormat, numLevels and gammaMips are DeriveOpts' for the image; a cube's colour
   * format is CFM_DEFAULT, as the engine stores it. The result is tagged fromSource; its
   * levels are unpadded (the engine pads a DXT level to whole 4 × 4 blocks before
   * encoding). Parse thread (C20).
   *
   * LP1c fix (S2.2, S2.4): the source's pixels are taken (moved from, not copied) as the
   * working buffer, the levels are written into one buffer reserved for them all, and a
   * stop is looked at between levels: once the registry is stopping, no level more is
   * made and the result has none.
   */
  CachedImage sourceImageLevels(
    SourcePixels&& source, int format, int colorFormat, int numLevels, bool gammaMips);

  // the bytes of decl text the registry keeps (all decl types, R5.10)
  std::size_t declTextBytes() const;

  /**
   * LP1c.2 (C21): the key of the image GetEditorImage loads for a material's
   * qer_editorimage (Material.cpp:261, ImageFromFile( editorImageName, TF_DEFAULT,
   * TR_REPEAT, TD_DIFFUSE )), named and looked up as ImageFromFile names and looks it up
   * (the default image for an empty or "default" name, the "fonts" and "lights" usages,
   * the extensions stripped, an existing record of the name), without making a record;
   * nullopt for a material without qer_editorimage. Parse thread (C20).
   */
  std::optional<MaterialImageKey> editorImageKey(const MaterialEntry& material);

  // the messages the copied code reported, oldest first
  const std::vector<DeclMessage>& messages() const;
  std::vector<DeclMessage> takeMessages();
  // LP1c.2: the messages from index first on, taken out (a quiet computation keeps or
  // drops them); errors among them stay counted
  std::vector<DeclMessage> takeMessagesFrom(std::size_t first);

private:
  // The process's one registry slot (C17, A2.6), claimed when it is made and released
  // when it goes. As a member it is released also when the constructor throws after the
  // claim (W.5), and only after the registry, which is declared after it, is destroyed.
  class Claim
  {
  public:
    Claim();
    ~Claim();

    bool held() const;
    void release();

    Claim(const Claim&) = delete;
    Claim& operator=(const Claim&) = delete;

  private:
    bool m_held = false;
  };

  std::uint64_t m_id = 0;
  Claim m_claim;
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
