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

#include "mdl/Doom3MaterialParser.h"

#include "base/FileLocation.h"
#include "base/ParserStatus.h"
#include "gl/Material.h"
#include "mdl/IdTech4MaterialName.h"
#include "mdl/Quake3Shader.h"

#include "kd/string_compare.h"
#include "kd/string_format.h"
#include "kd/string_utils.h"

#include <algorithm>
#include <array>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// The engine source cited below is RBDOOM-3-BFG's neo/ directory: idLexer in
// idlib/Lexer.cpp, idDeclFile::LoadAndParse in framework/DeclManager.cpp, idMaterial in
// renderer/Material.cpp and R_ParseImageProgram_r in renderer/Image_program.cpp.

namespace tb::mdl
{
namespace
{

// ---------------------------------------------------------------------------------------
// Layer 1: a tokenizer with idLexer's rules for decl files (DECL_LEXER_FLAGS). Unlike
// idLexer, it never fails: malformed input is reported and tokenized as well as possible.
// ---------------------------------------------------------------------------------------

enum class TokenKind
{
  Name,
  Number,
  String,
  Punct,
  Eof,
};

struct Token
{
  TokenKind kind = TokenKind::Eof;
  // a quoted string's text does not include the quotes
  std::string_view text;
  size_t line = 0;
  size_t column = 0;
  // differs from line only for strings concatenated across lines with a backslash
  size_t endLine = 0;
};

FileLocation locationOf(const Token& token)
{
  return FileLocation{token.line, token.column};
}

// idToken's operator== compares the text whatever the token type, so a quoted "}" ends
// a stage just like a bare one does.
bool isText(const Token& token, const std::string_view text)
{
  return token.text == text;
}

bool isKeyword(const Token& token, const std::string_view keyword)
{
  // comparing the sizes first skips most comparisons, which matters because every token
  // is compared with many keywords
  return token.text.size() == keyword.size()
         && kdl::ci::str_is_equal(token.text, keyword);
}

template <size_t N>
bool isAnyKeyword(const Token& token, const std::array<std::string_view, N>& keywords)
{
  return std::ranges::any_of(
    keywords, [&](const auto& keyword) { return isKeyword(token, keyword); });
}

bool isPunct(const Token& token, const std::string_view text)
{
  return token.kind == TokenKind::Punct && token.text == text;
}

bool isNonAscii(const char c)
{
  return static_cast<unsigned char>(c) >= 0x80;
}

bool hasNonAscii(const std::string_view text)
{
  return std::ranges::any_of(text, isNonAscii);
}

// id Tech 4 names are ASCII. Only quoted strings can carry other bytes, and those bytes
// would make std::filesystem::path throw (see idTech4Path), so they become '_'.
std::string asciiText(const std::string_view text)
{
  auto result = std::string{text};
  std::ranges::replace_if(result, isNonAscii, '_');
  return result;
}

std::string describe(const Token& token)
{
  return token.kind == TokenKind::Eof ? std::string{"end of material"}
                                      : "'" + asciiText(token.text) + "'";
}

// idLexer's default punctuation table, longest first
// clang-format off
constexpr auto Punctuation = std::array<std::string_view, 52>{
  ">>=", "<<=", "...", "##", "&&", "||", ">=", "<=", "==", "!=", "*=", "/=", "%=", "+=",
  "-=", "++", "--", "&=", "|=", "^=", ">>", "<<", "->", "::", ".*", "*", "/", "%", "+",
  "-", "=", "&", "|", "^", "~", "!", ">", "<", ".", ",", ";", ":", "?", "(", ")", "{",
  "}", "[", "]", "\\", "#", "$",
};
// clang-format on

bool isDigit(const char c)
{
  return c >= '0' && c <= '9';
}

bool isLetter(const char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool isHexDigit(const char c)
{
  return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// idLexer compares signed chars with ' ', so bytes from 0x80 up are whitespace as well.
bool isWhitespace(const char c)
{
  const auto u = static_cast<unsigned char>(c);
  return u <= ' ' || u >= 0x80;
}

// LEXFL_ALLOWPATHNAMES adds '/', '\\', ':' and '.' to names (idLexer::ReadName).
bool isNameChar(const char c)
{
  return isLetter(c) || isDigit(c) || c == '_' || c == '/' || c == '\\' || c == ':'
         || c == '.';
}

// Two idLexer quirks are deliberately not copied:
// - idLexer::ReadWhiteSpace steps over one more character after the "*/" that closes a
//   block comment, so in the engine "/* c */{" loses its brace.
// - idLexer fails on a character it doesn't know, such as '@'. ReadToken then returns
//   false, which ends idDeclFile::LoadAndParse's loop, so the rest of the file is
//   ignored, or defaults the material being parsed. Here, such a character is reported
//   and kept as a one-character token.
class Lexer
{
private:
  std::string_view m_str;
  // null when re-reading text that has already been reported
  ParserStatus* m_status;
  // storage for the text of strings concatenated with a backslash
  std::deque<std::string>& m_concatenatedStrings;
  size_t m_pos = 0;
  size_t m_line = 1;
  size_t m_lineStart = 0;

public:
  Lexer(
    const std::string_view str,
    ParserStatus* status,
    std::deque<std::string>& concatenatedStrings)
    : m_str{str}
    , m_status{status}
    , m_concatenatedStrings{concatenatedStrings}
  {
  }

  // Returns every token in the input, followed by one Eof token.
  std::vector<Token> tokenize()
  {
    auto tokens = std::vector<Token>{};
    // decl files average more than four bytes per token
    tokens.reserve(m_str.size() / 4 + 1);
    while (true)
    {
      skipWhitespace(true);
      if (m_pos >= m_str.size())
      {
        tokens.push_back(Token{TokenKind::Eof, {}, m_line, column(), m_line});
        return tokens;
      }
      tokens.push_back(readToken());
    }
  }

private:
  char at(const size_t pos) const { return pos < m_str.size() ? m_str[pos] : '\0'; }

  size_t column() const { return m_pos - m_lineStart + 1; }

  void newLine(const size_t pos)
  {
    ++m_line;
    m_lineStart = pos + 1;
  }

  void warn(const size_t line, const size_t col, const std::string& message)
  {
    if (m_status)
    {
      m_status->warn(FileLocation{line, col}, message);
    }
  }

  // idLexer::ReadWhiteSpace: whitespace, // comments and /* */ comments
  void skipWhitespace(const bool report)
  {
    while (m_pos < m_str.size())
    {
      const auto c = m_str[m_pos];
      if (isWhitespace(c))
      {
        if (c == '\n')
        {
          newLine(m_pos);
        }
        ++m_pos;
      }
      else if (c == '/' && at(m_pos + 1) == '/')
      {
        m_pos += 2;
        while (m_pos < m_str.size() && m_str[m_pos] != '\n')
        {
          ++m_pos;
        }
      }
      else if (c == '/' && at(m_pos + 1) == '*')
      {
        const auto startLine = m_line;
        const auto startColumn = column();

        // like idLexer, the comment ends at the first '/' preceded by '*', looking from
        // the character after the opening "/*", so "/*/" is a complete comment
        auto pos = m_pos + 2;
        while (pos < m_str.size() && !(m_str[pos] == '/' && m_str[pos - 1] == '*'))
        {
          if (m_str[pos] == '\n')
          {
            newLine(pos);
          }
          ++pos;
        }

        if (pos >= m_str.size())
        {
          if (report)
          {
            warn(
              startLine, startColumn, "Unterminated comment runs to the end of the file");
          }
          m_pos = m_str.size();
          return;
        }
        m_pos = pos + 1;
      }
      else
      {
        return;
      }
    }
  }

  Token readToken()
  {
    const auto c = m_str[m_pos];
    if (isDigit(c) || (c == '.' && isDigit(at(m_pos + 1))))
    {
      return readNumber();
    }
    if (c == '"' || c == '\'')
    {
      return readString(c);
    }
    // names may start with a path separator or a dot (LEXFL_ALLOWPATHNAMES)
    if (isLetter(c) || c == '_' || c == '/' || c == '\\' || c == '.')
    {
      return readName();
    }
    return readPunctuation();
  }

  Token makeToken(const TokenKind kind, const size_t start, const size_t end) const
  {
    return Token{
      kind, m_str.substr(start, end - start), m_line, start - m_lineStart + 1, m_line};
  }

  Token readName()
  {
    const auto start = m_pos++;
    while (isNameChar(at(m_pos)))
    {
      ++m_pos;
    }
    return makeToken(TokenKind::Name, start, m_pos);
  }

  // idLexer::ReadNumber, without the floating point exception forms (1.#INF)
  Token readNumber()
  {
    const auto start = m_pos;
    const auto c = m_str[m_pos];
    const auto c2 = at(m_pos + 1);
    auto isFloat = false;

    if (c == '0' && c2 != '.')
    {
      if (c2 == 'x' || c2 == 'X')
      {
        m_pos += 2;
        while (isHexDigit(at(m_pos)))
        {
          ++m_pos;
        }
      }
      else if (c2 == 'b' || c2 == 'B')
      {
        m_pos += 2;
        while (at(m_pos) == '0' || at(m_pos) == '1')
        {
          ++m_pos;
        }
      }
      else
      {
        ++m_pos;
        while (at(m_pos) >= '0' && at(m_pos) <= '7')
        {
          ++m_pos;
        }
      }
    }
    else
    {
      auto dots = 0;
      while (isDigit(at(m_pos)) || at(m_pos) == '.')
      {
        if (at(m_pos) == '.')
        {
          ++dots;
        }
        ++m_pos;
      }

      if (at(m_pos) == 'e' && dots == 0)
      {
        dots = 1;
      }

      if (dots == 1)
      {
        isFloat = true;
        if (at(m_pos) == 'e')
        {
          ++m_pos;
          if (at(m_pos) == '-' || at(m_pos) == '+')
          {
            ++m_pos;
          }
          while (isDigit(at(m_pos)))
          {
            ++m_pos;
          }
        }
      }
      else if (dots > 1)
      {
        warn(m_line, start - m_lineStart + 1, "More than one dot in a number");
      }
    }

    auto token = makeToken(TokenKind::Number, start, m_pos);

    // type suffixes are consumed but are not part of the token's text
    if (isFloat)
    {
      const auto s = at(m_pos);
      if (s == 'f' || s == 'F' || s == 'l' || s == 'L')
      {
        ++m_pos;
      }
    }
    else
    {
      for (auto i = 0; i < 2; ++i)
      {
        const auto s = at(m_pos);
        if (s != 'l' && s != 'L' && s != 'u' && s != 'U')
        {
          break;
        }
        ++m_pos;
      }
    }

    return token;
  }

  // Reads the body of a quoted string after its opening quote. Returns the end of the
  // text and whether the closing quote was found.
  std::pair<size_t, bool> readStringBody(
    const char quote, const size_t startLine, const size_t startColumn)
  {
    while (true)
    {
      if (m_pos >= m_str.size())
      {
        warn(startLine, startColumn, "Missing closing quote");
        return {m_pos, false};
      }
      const auto c = m_str[m_pos];
      if (c == quote)
      {
        return {m_pos++, true};
      }
      if (c == '\n')
      {
        // idLexer fails here; end the string at the line break instead
        warn(startLine, startColumn, "Line break inside a quoted string");
        return {m_pos, false};
      }
      ++m_pos;
    }
  }

  // idLexer::ReadString with LEXFL_NOSTRINGCONCAT, LEXFL_NOSTRINGESCAPECHARS,
  // LEXFL_ALLOWMULTICHARLITERALS and LEXFL_ALLOWBACKSLASHSTRINGCONCAT
  Token readString(const char quote)
  {
    const auto startLine = m_line;
    const auto startColumn = column();
    const auto textStart = ++m_pos;
    const auto [textEnd, closed] = readStringBody(quote, startLine, startColumn);

    auto token = Token{
      TokenKind::String,
      m_str.substr(textStart, textEnd - textStart),
      startLine,
      startColumn,
      m_line};

    if (quote != '"' || !closed)
    {
      return token;
    }

    // "a" \ "b" is one string "ab"
    auto concatenated = std::optional<std::string>{};
    while (true)
    {
      const auto savedPos = m_pos;
      const auto savedLine = m_line;
      const auto savedLineStart = m_lineStart;
      const auto restore = [&]() {
        m_pos = savedPos;
        m_line = savedLine;
        m_lineStart = savedLineStart;
      };

      skipWhitespace(false);
      if (at(m_pos) != '\\')
      {
        restore();
        break;
      }

      const auto backslashLine = m_line;
      const auto backslashColumn = column();
      ++m_pos;
      skipWhitespace(false);
      if (at(m_pos) != '"')
      {
        warn(backslashLine, backslashColumn, "Expected a string after '\\'");
        restore();
        break;
      }

      const auto nextStart = ++m_pos;
      const auto [nextEnd, nextClosed] = readStringBody(quote, m_line, column());
      if (!concatenated)
      {
        concatenated = std::string{token.text};
      }
      *concatenated += m_str.substr(nextStart, nextEnd - nextStart);
      if (!nextClosed)
      {
        break;
      }
    }

    if (concatenated)
    {
      m_concatenatedStrings.push_back(std::move(*concatenated));
      token.text = m_concatenatedStrings.back();
      token.endLine = m_line;
    }
    return token;
  }

  Token readPunctuation()
  {
    const auto start = m_pos;

    const auto rest = m_str.substr(m_pos);
    for (const auto& punctuation : Punctuation)
    {
      if (rest.starts_with(punctuation))
      {
        m_pos += punctuation.size();
        return makeToken(TokenKind::Punct, start, m_pos);
      }
    }

    // idLexer fails on characters it doesn't know; keep them as one-character tokens
    warn(
      m_line,
      column(),
      "Unknown character '" + std::string{m_str.substr(m_pos, 1)} + "'");
    ++m_pos;
    return makeToken(TokenKind::Punct, start, m_pos);
  }
};

// ---------------------------------------------------------------------------------------
// Layer 3: the per-material block parser, with idMaterial::ParseMaterial's and
// idMaterial::ParseStage's keyword arities. It never evaluates expressions; it only
// consumes exactly the tokens the engine consumes.
// ---------------------------------------------------------------------------------------

using Lighting = Quake3ShaderStage::Lighting;
using Coverage = Quake3Shader::Coverage;

// infoParms in Material.cpp, matched case-insensitively by CheckSurfaceParm
// clang-format off
constexpr auto InfoParms = std::array<std::string_view, 39>{
  "solid", "water", "playerclip", "monsterclip", "moveableclip", "ikclip", "blood",
  "trigger", "aassolid", "aasobstacle", "flashlight_trigger", "nonsolid", "nullNormal",
  "areaportal", "qer_nocarve", "discrete", "noFragment", "occlusion", "slick",
  "collision", "noimpact", "nodamage", "ladder", "nosteps", "metal", "stone", "flesh",
  "wood", "cardboard", "liquid", "glass", "plastic", "ricochet", "surftype10",
  "surftype11", "surftype12", "surftype13", "surftype14", "surftype15",
};
// clang-format on

// argument-less material keywords that only set a flag
// clang-format off
constexpr auto MaterialFlags = std::array<std::string_view, 25>{
  "noShadows", "suppressInSubview", "portalSky", "noSelfShadow", "noPortalFog",
  "forceShadows", "noOverlays", "forceOverlays", "zeroclamp", "clamp", "alphazeroclamp",
  "fogLight", "blendLight", "ambientLight", "noFog", "unsmoothedTangents", "origin",
  "mikktspace", "unlit", "DECAL_MACRO", "lod1", "lod2", "lod3", "lod4", "persistentLOD",
};
// clang-format on

// argument-less stage keywords
// clang-format off
constexpr auto StageFlags = std::array<std::string_view, 23>{
  "screen", "screen2", "glassWarp", "ignoreAlphaTest", "nearest", "linear", "clamp",
  "noclamp", "zeroclamp", "alphazeroclamp", "forceHighQuality", "highquality",
  "uncompressedCubeMap", "nopicmip", "vertexColor", "inverseVertexColor", "maskRed",
  "maskGreen", "maskBlue", "maskAlpha", "maskColor", "maskDepth", "colored",
};
// clang-format on

constexpr auto RenderMapKeywords = std::array<std::string_view, 4>{
  "remoteRenderMap", "mirrorRenderMap", "xrayRenderMap", "guiRenderMap"};

constexpr auto CubeMapKeywords = std::array<std::string_view, 6>{
  "cubeMap", "cubeMapSingle", "panoramaMap", "hdriMap", "cameraCubeMap", "quakeCubeMap"};

// keywords that take expression , expression
constexpr auto TextureMatrixKeywords =
  std::array<std::string_view, 5>{"scroll", "translate", "scale", "centerScale", "shear"};

constexpr auto ColorKeywords =
  std::array<std::string_view, 6>{"red", "green", "blue", "alpha", "rgb", "rgba"};

constexpr auto ProgramKeywords =
  std::array<std::string_view, 3>{"program", "fragmentProgram", "vertexProgram"};

// options of fragmentMap before its image program (idMaterial::ParseFragmentMap)
// clang-format off
constexpr auto FragmentMapOptions = std::array<std::string_view, 17>{
  "normalMap", "cubeMap", "cameraCubeMap", "quakeCubeMap", "cubeMapSingle", "panoramaMap",
  "hdriMap", "nearest", "linear", "clamp", "noclamp", "zeroclamp", "alphazeroclamp",
  "forceHighQuality", "highquality", "uncompressed", "nopicmip",
};
// clang-format on

constexpr auto StencilKeywords =
  std::array<std::string_view, 5>{"Ref", "Comp", "Pass", "Fail", "ZFail"};

// the expression registers known to idMaterial::ParseTerm
// clang-format off
constexpr auto ExpressionRegisters = std::array<std::string_view, 22>{
  "time", "parm0", "parm1", "parm2", "parm3", "parm4", "parm5", "parm6", "parm7", "parm8",
  "parm9", "parm10", "parm11", "global0", "global1", "global2", "global3", "global4",
  "global5", "global6", "global7", "fragmentPrograms",
};
// clang-format on

constexpr auto BinaryOperators = std::array<std::string_view, 13>{
  "*", "/", "%", "+", "-", ">", ">=", "<", "<=", "==", "!=", "&&", "||"};

constexpr int MaxVertexParms = 4;    // MAX_VERTEX_PARMS in Material.h
constexpr int MaxFragmentImages = 8; // MAX_FRAGMENT_IMAGES in Material.h

int engineLightingOrder(const Lighting lighting)
{
  // stageLighting_t: SL_AMBIENT, SL_BUMP, SL_DIFFUSE, SL_SPECULAR
  switch (lighting)
  {
  case Lighting::None:
    return 0;
  case Lighting::Bump:
    return 1;
  case Lighting::Diffuse:
    return 2;
  case Lighting::Specular:
    return 3;
  }
  return 0;
}

// idToken::NumberValue: the value of a number token, including the hexadecimal, binary
// and octal integers that idLexer::ReadNumber reads; 0 for anything else
float numberValue(const std::string_view text)
{
  const auto integer = [](const std::string_view digits, const double base) {
    auto value = 0.0;
    for (const auto c : digits)
    {
      const auto digit = isDigit(c) ? c - '0' : (c | 0x20) - 'a' + 10;
      value = value * base + digit;
    }
    return static_cast<float>(value);
  };

  if (text.size() > 1 && text[0] == '0')
  {
    if (text[1] == 'x' || text[1] == 'X')
    {
      return integer(text.substr(2), 16.0);
    }
    if (text[1] == 'b' || text[1] == 'B')
    {
      return integer(text.substr(2), 2.0);
    }
    if (text[1] != '.')
    {
      return integer(text.substr(1), 8.0);
    }
  }
  return kdl::str_to_float(text).value_or(0.0f);
}

// idStr::IsNumeric
bool isNumericText(std::string_view text)
{
  if (text.starts_with('-'))
  {
    text.remove_prefix(1);
  }
  auto dot = false;
  for (const auto c : text)
  {
    if (!isDigit(c))
    {
      if (c == '.' && !dot)
      {
        dot = true;
        continue;
      }
      return false;
    }
  }
  return true;
}

bool blendsWithDestination(const Quake3ShaderStage::BlendFunc& blendFunc)
{
  using BlendFunc = Quake3ShaderStage::BlendFunc;

  // a stage without blend draws with GL_ONE, GL_ZERO
  if (!blendFunc.enable())
  {
    return false;
  }
  return blendFunc.destFactor != BlendFunc::Zero
         || blendFunc.srcFactor == BlendFunc::DestColor
         || blendFunc.srcFactor == BlendFunc::OneMinusDestColor
         || blendFunc.srcFactor == BlendFunc::DestAlpha
         || blendFunc.srcFactor == BlendFunc::OneMinusDestAlpha;
}

std::string toLower(const std::string_view str)
{
  return kdl::str_to_lower(str);
}

// Whether the text of a quoted string is still one token when it is read without quotes.
bool staysOneToken(const std::string_view text)
{
  auto concatenatedStrings = std::deque<std::string>{};
  const auto tokens = Lexer{text, nullptr, concatenatedStrings}.tokenize();
  // the last token is the end of the text
  return tokens.size() == 2;
}

} // namespace

// idMaterial::Parse calls MakeDefault (DeclManager.cpp) for a material it failed to
// parse. That parses the default definition "{ { blend blend map _default } }"
// (idMaterial::DefaultDefinition) into the material after CommonInit has reset
// everything except the editor image name, so GetEditorImage still prefers it.
void makeEngineDefaulted(Quake3Shader& shader)
{
  using BlendFunc = Quake3ShaderStage::BlendFunc;

  auto shaderPath = std::move(shader.shaderPath);
  auto editorImage = std::move(shader.editorImage);

  // generated images such as _default are no preview images
  const auto& editorImageName = editorImage.native();
  auto previewImages = std::vector<std::filesystem::path>{};
  if (!editorImageName.empty() && editorImageName.front() != '_')
  {
    previewImages.push_back(editorImage);
  }

  shader = Quake3Shader{
    .shaderPath = std::move(shaderPath),
    .editorImage = std::move(editorImage),
    .lightImage = {},
    .culling = Quake3Shader::Culling::Front,
    // translucent materials get noShadows
    .surfaceParms = {"coverage_translucent", "engine_defaulted", "noshadows"},
    .stages = {Quake3ShaderStage{
      .map = "_default",
      .blendFunc = {BlendFunc::SrcAlpha, BlendFunc::OneMinusSrcAlpha},
      .alphaFunc = std::nullopt,
      .lighting = Lighting::None,
    }},
    .qerAlphaFunc = std::nullopt,
    .format = Quake3Shader::Format::Doom3,
    .coverage = Coverage::Translucent,
    .previewImages = std::move(previewImages),
    .engineDefaulted = true,
    .referencedTables = {},
  };
}

namespace
{

class MaterialBlockParser
{
private:
  struct StageState
  {
    Quake3ShaderStage stage;
    // the first image of the last map (or cube map) keyword's image program
    std::optional<std::string> imageName;
    // a video, a sound, a render map or a shader program, which leave the stage without
    // an image unless it also has a map
    bool hasDynamicImage = false;
    bool texGenReflect = false;
    bool referencesCurrentRender = false;
  };

  struct EngineStage
  {
    Lighting lighting = Lighting::None;
    // nullopt if the engine leaves the stage's image null
    std::optional<std::string> image;
  };

  // Every recursion (parenthesised expressions, table lookups, image programs) counts
  // against this limit, so that no input can overflow the stack.
  class NestingGuard
  {
  private:
    int& m_nesting;

  public:
    explicit NestingGuard(int& nesting)
      : m_nesting{++nesting}
    {
    }

    ~NestingGuard() { --m_nesting; }

    NestingGuard(const NestingGuard&) = delete;
    NestingGuard& operator=(const NestingGuard&) = delete;
  };

  static constexpr int MaxNesting = 64;

  const std::vector<Token>& m_tokens;
  size_t m_pos;
  size_t m_end;
  Token m_endToken;
  bool m_unterminated;
  ParserStatus& m_status;
  // ASCII only, see asciiText
  std::string m_name;
  bool m_nonAsciiReported;

  Quake3Shader m_shader;
  bool m_defaulted = false;
  int m_nesting = 0;
  std::string m_editorImageName;
  std::optional<Coverage> m_explicitCoverage;
  bool m_sortPortalSky = false;
  bool m_referencesCurrentRender = false;
  // parallel to m_shader.stages
  std::vector<std::optional<std::string>> m_stageImages;
  std::vector<bool> m_stageTexGenReflect;

public:
  // Parses the tokens in [begin, end); tokens[begin - 1] is the material's opening brace,
  // tokens[end - 1] its closing brace unless the material is unterminated.
  MaterialBlockParser(
    const std::vector<Token>& tokens,
    const size_t begin,
    const size_t end,
    const bool unterminated,
    std::string name,
    const bool nonAsciiReported,
    std::filesystem::path shaderPath,
    ParserStatus& status)
    : m_tokens{tokens}
    , m_pos{begin}
    , m_end{end}
    , m_endToken{endToken(tokens, end, unterminated)}
    , m_unterminated{unterminated}
    , m_status{status}
    , m_name{std::move(name)}
    , m_nonAsciiReported{nonAsciiReported}
  {
    m_shader.shaderPath = std::move(shaderPath);
    m_shader.format = Quake3Shader::Format::Doom3;
  }

  // idMaterial::ParseMaterial
  Quake3Shader parse()
  {
    while (!m_defaulted)
    {
      if (atEnd())
      {
        setDefaulted(m_endToken, "Unexpected end of material");
        break;
      }

      const auto& token = next();
      if (isText(token, "}"))
      {
        break;
      }
      parseMaterialKeyword(token);
    }

    finish();
    return std::move(m_shader);
  }

private:
  static Token endToken(
    const std::vector<Token>& tokens, const size_t end, const bool unterminated)
  {
    // report an unexpected end at the closing brace, or at the end of the file
    auto token = unterminated ? tokens[end] : tokens[end - 1];
    token.kind = TokenKind::Eof;
    token.text = {};
    return token;
  }

  bool atEnd() const { return m_pos >= m_end; }

  const Token& peek() const { return atEnd() ? m_endToken : m_tokens[m_pos]; }

  const Token& next() { return atEnd() ? m_endToken : m_tokens[m_pos++]; }

  // idLexer::ReadTokenOnLine: the next token if no line break precedes it
  const Token* nextOnLine()
  {
    if (!atEnd() && m_tokens[m_pos].line == m_tokens[m_pos - 1].endLine)
    {
      return &m_tokens[m_pos++];
    }
    return nullptr;
  }

  // idLexer::SkipRestOfLine
  void skipRestOfLine()
  {
    while (nextOnLine())
    {
    }
  }

  void warn(const Token& token, const std::string& message)
  {
    // the decl splitter has already reported an unterminated material
    if (token.kind != TokenKind::Eof || !m_unterminated)
    {
      m_status.warn(locationOf(token), message);
    }
  }

  // The engine sets MF_DEFAULTED, stops parsing the material, and later replaces it with
  // its default material.
  void setDefaulted(const Token& token, const std::string& message)
  {
    if (!m_defaulted)
    {
      m_defaulted = true;
      warn(
        token,
        message + " in material '" + m_name
          + "'; the engine replaces this material with its default material");
    }
  }

  // The text of a token that goes into the record, see asciiText. Reports the first
  // non-ASCII field of a material.
  std::string recordText(const Token& token, const std::string_view field)
  {
    if (hasNonAscii(token.text) && !m_nonAsciiReported)
    {
      m_nonAsciiReported = true;
      warn(
        token,
        "Non-ASCII characters in the " + std::string{field} + " of material '" + m_name
          + "' were replaced with '_'");
    }
    return asciiText(token.text);
  }

  // Returns false and defaults the material if the input nests deeper than MaxNesting.
  bool checkNesting(const std::string_view what)
  {
    if (m_nesting > MaxNesting)
    {
      setDefaulted(
        peek(),
        std::string{what} + " nested too deeply (more than " + std::to_string(MaxNesting)
          + " levels)");
      return false;
    }
    return true;
  }

  // idMaterial::MatchToken
  void matchToken(const std::string_view expected)
  {
    const auto& token = next();
    if (!isText(token, expected))
    {
      setDefaulted(
        token, "Expected '" + std::string{expected} + "' but found " + describe(token));
    }
  }

  // idLexer::ExpectTokenString as called without a check of its result
  void expectToken(const std::string_view expected)
  {
    const auto& token = next();
    if (!isText(token, expected))
    {
      warn(
        token, "Expected '" + std::string{expected} + "' but found " + describe(token));
    }
  }

  // idLexer::ParseFloat and idLexer::ParseInt
  void parseNumber()
  {
    const auto* token = &next();
    if (isPunct(*token, "-"))
    {
      token = &next();
    }
    if (token->kind != TokenKind::Number)
    {
      warn(*token, "Expected a number but found " + describe(*token));
    }
  }

  // idLexer::Parse1DMatrix
  void parseNumberList(const size_t count)
  {
    const auto& open = next();
    if (!isText(open, "("))
    {
      warn(open, "Expected '(' but found " + describe(open));
      return;
    }
    for (size_t i = 0; i < count; ++i)
    {
      parseNumber();
    }
    expectToken(")");
  }

  // idMaterial::ParseExpression; returns the value if the expression is one number
  std::optional<float> parseExpression()
  {
    const auto guard = NestingGuard{m_nesting};
    if (!checkNesting("Expression"))
    {
      return std::nullopt;
    }

    auto constant = parseTerm();
    while (!m_defaulted && !atEnd() && isAnyKeyword(peek(), BinaryOperators))
    {
      next();
      constant = std::nullopt;
      parseTerm();
    }
    return m_defaulted ? std::nullopt : constant;
  }

  // idMaterial::ParseTerm
  std::optional<float> parseTerm()
  {
    const auto& token = next();
    if (isText(token, "("))
    {
      parseExpression();
      if (!m_defaulted)
      {
        matchToken(")");
      }
      return std::nullopt;
    }

    if (isAnyKeyword(token, ExpressionRegisters) || isKeyword(token, "sound"))
    {
      return std::nullopt;
    }

    if (isText(token, "-"))
    {
      const auto& number = next();
      if (number.kind == TokenKind::Number || isText(number, "."))
      {
        return -numberValue(number.text);
      }
      setDefaulted(number, "Bad negative number " + describe(number));
      return std::nullopt;
    }

    if (token.kind == TokenKind::Number || isText(token, "."))
    {
      return numberValue(token.text);
    }

    // anything else must be a table lookup: name [ expression ]; whether the table
    // exists is not known here, because tables can be declared in any file
    if (token.kind != TokenKind::Eof && isText(peek(), "["))
    {
      addReferencedTable(token.text);
      next();
      parseExpression();
      if (!m_defaulted)
      {
        matchToken("]");
      }
      return std::nullopt;
    }

    setDefaulted(token, "Bad term " + describe(token) + " in an expression");
    return std::nullopt;
  }

  // R_ParsePastImageProgram; returns the first image name in the program, from left to
  // right, which is what the editor previews
  std::optional<std::string> parseImageProgram(const std::string_view field)
  {
    // at the end, the engine reads an empty image program, which names no image
    if (atEnd())
    {
      return std::nullopt;
    }

    auto leaf = std::optional<std::string>{};
    parseImageProgram(field, leaf);
    return leaf.value_or(std::string{});
  }

  // R_ParseImageProgram_r
  void parseImageProgram(const std::string_view field, std::optional<std::string>& leaf)
  {
    const auto guard = NestingGuard{m_nesting};
    if (!checkNesting("Image program"))
    {
      return;
    }

    // parses a nested image program; false if that defaulted the material
    const auto argument = [&]() {
      parseImageProgram(field, leaf);
      return !m_defaulted;
    };

    const auto& token = next();
    if (isKeyword(token, "heightmap"))
    {
      expectToken("(");
      if (argument())
      {
        expectToken(",");
        next();
        expectToken(")");
      }
    }
    else if (isKeyword(token, "addnormals") || isKeyword(token, "add"))
    {
      expectToken("(");
      if (argument())
      {
        expectToken(",");
        if (argument())
        {
          expectToken(")");
        }
      }
    }
    else if (
      isKeyword(token, "smoothnormals") || isKeyword(token, "invertAlpha")
      || isKeyword(token, "invertGreen") || isKeyword(token, "invertColor")
      || isKeyword(token, "makeIntensity") || isKeyword(token, "makeAlpha"))
    {
      expectToken("(");
      if (argument())
      {
        expectToken(")");
      }
    }
    else if (isKeyword(token, "scale"))
    {
      expectToken("(");
      if (argument())
      {
        for (auto i = 0; i < 4; ++i)
        {
          expectToken(",");
          next();
        }
        expectToken(")");
      }
    }
    else if (isKeyword(token, "combineRgba"))
    {
      expectToken("(");
      if (argument())
      {
        expectToken(",");
        if (argument())
        {
          expectToken(",");
          if (argument())
          {
            expectToken(")");
          }
        }
      }
    }
    else if (!leaf)
    {
      // a punctuation token is consumed as an image name too, but can't name a file
      if (token.kind == TokenKind::Punct || token.kind == TokenKind::Eof)
      {
        leaf = std::string{};
      }
      // the image program maps these two intrinsic names to files
      else if (isText(token, "_black"))
      {
        leaf = "textures/black";
      }
      else if (isText(token, "_white"))
      {
        leaf = "guis/assets/white";
      }
      else
      {
        leaf = recordText(token, field);
      }
    }
  }

  // idMaterial::ParseDeform
  void parseDeform()
  {
    if (atEnd())
    {
      return;
    }

    const auto& token = next();
    if (isKeyword(token, "sprite") || isKeyword(token, "tube"))
    {
      m_shader.culling = Quake3Shader::Culling::None;
    }
    else if (isKeyword(token, "flare"))
    {
      m_shader.culling = Quake3Shader::Culling::None;
      parseExpression();
    }
    else if (isKeyword(token, "expand") || isKeyword(token, "move"))
    {
      parseExpression();
    }
    else if (isKeyword(token, "turbulent"))
    {
      if (atEnd())
      {
        setDefaulted(m_endToken, "Missing table name after deform turbulent");
        return;
      }
      next();
      for (auto i = 0; i < 3 && !m_defaulted; ++i)
      {
        parseExpression();
      }
    }
    else if (isKeyword(token, "particle") || isKeyword(token, "particle2"))
    {
      if (atEnd())
      {
        setDefaulted(m_endToken, "Missing particle name after deform");
        return;
      }
      next();
    }
    else if (!isKeyword(token, "eyeBall"))
    {
      setDefaulted(token, "Bad deform type " + describe(token));
    }
  }

  // Whether the token at the given index looks like the start of a decl: a path-like name
  // at the start of a line, followed by an opening brace.
  bool looksLikeDeclStart(const size_t index) const
  {
    const auto& token = m_tokens[index];
    return token.kind == TokenKind::Name && token.line > m_tokens[index - 1].endLine
           && token.text.find('/') != std::string_view::npos
           && index + 1 < m_tokens.size() && isPunct(m_tokens[index + 1], "{");
  }

  // A material that lacks a closing brace swallows the decls after it, in the engine
  // too, and is defaulted at the first swallowed name, which the engine reads as an
  // unknown keyword. If the unknown keyword just read looks like the start of a decl,
  // point at it and at the further decls at the same brace depth.
  void warnAboutSwallowedDecls()
  {
    const auto keyword = m_pos - 1;
    if (!looksLikeDeclStart(keyword))
    {
      return;
    }

    auto depth = 0;
    for (auto k = keyword; k < m_end; ++k)
    {
      const auto& token = m_tokens[k];
      if (isPunct(token, "{"))
      {
        ++depth;
      }
      else if (isPunct(token, "}"))
      {
        if (--depth < 0)
        {
          return;
        }
      }
      else if (depth == 0 && looksLikeDeclStart(k))
      {
        warn(
          token,
          "Possible missing '}' before line " + std::to_string(token.line)
            + ": material '" + m_name + "' now contains '" + std::string{token.text}
            + "'");
      }
    }
  }

  void addReferencedTable(const std::string_view name)
  {
    auto canonicalName = canonicalIdTech4DeclName(name);
    if (
      std::ranges::find(m_shader.referencedTables, canonicalName)
      == m_shader.referencedTables.end())
    {
      m_shader.referencedTables.push_back(std::move(canonicalName));
    }
  }

  void addFlag(const std::string_view flag)
  {
    m_shader.surfaceParms.insert(toLower(flag));
  }

  void parseMaterialKeyword(const Token& token)
  {
    if (isKeyword(token, "qer_editorimage"))
    {
      const auto* image = nextOnLine();
      m_editorImageName = image ? recordText(*image, "editor image") : std::string{};
      skipRestOfLine();
    }
    else if (isKeyword(token, "description"))
    {
      nextOnLine();
    }
    else if (const auto infoParm = std::ranges::find_if(
               InfoParms, [&](const auto& parm) { return isKeyword(token, parm); });
             infoParm != InfoParms.end())
    {
      addFlag(*infoParm);
    }
    else if (isKeyword(token, "polygonOffset"))
    {
      addFlag("polygonOffset");
      nextOnLine();
    }
    else if (const auto flag = std::ranges::find_if(
               MaterialFlags, [&](const auto& f) { return isKeyword(token, f); });
             flag != MaterialFlags.end())
    {
      addFlag(*flag);
    }
    else if (isKeyword(token, "translucent"))
    {
      addFlag("translucent");
      m_explicitCoverage = Coverage::Translucent;
    }
    else if (isKeyword(token, "forceOpaque"))
    {
      addFlag("forceOpaque");
      m_explicitCoverage = Coverage::Opaque;
    }
    else if (isKeyword(token, "twoSided"))
    {
      addFlag("twoSided");
      m_shader.culling = Quake3Shader::Culling::None;
    }
    else if (isKeyword(token, "backSided"))
    {
      addFlag("backSided");
      m_shader.culling = Quake3Shader::Culling::Back;
    }
    else if (isKeyword(token, "mirror") || isKeyword(token, "directPortal"))
    {
      addFlag(token.text);
      m_explicitCoverage = Coverage::Opaque;
      // both set the sort to SS_SUBVIEW
      m_sortPortalSky = false;
    }
    else if (isKeyword(token, "lightFalloffImage"))
    {
      parseImageProgram("light falloff image");
    }
    else if (isKeyword(token, "guisurf"))
    {
      addFlag("guisurf");
      nextOnLine();
    }
    else if (isKeyword(token, "sort"))
    {
      if (const auto* sort = nextOnLine())
      {
        m_sortPortalSky = isKeyword(*sort, "portalSky");
      }
      else
      {
        setDefaulted(token, "Missing sort parameter");
      }
    }
    else if (isKeyword(token, "stereoeye"))
    {
      if (!nextOnLine())
      {
        setDefaulted(token, "Missing eye parameter");
      }
    }
    else if (isKeyword(token, "spectrum"))
    {
      nextOnLine();
    }
    else if (isKeyword(token, "deform"))
    {
      parseDeform();
    }
    else if (isKeyword(token, "decalInfo"))
    {
      parseNumber();
      parseNumber();
      parseNumberList(4);
      parseNumberList(4);
    }
    else if (isKeyword(token, "renderbump"))
    {
      skipRestOfLine();
    }
    else if (isKeyword(token, "diffusemap") || isKeyword(token, "basecolormap"))
    {
      parseShorthandStage(token, Lighting::Diffuse);
    }
    else if (
      isKeyword(token, "specularmap") || isKeyword(token, "rmaomap")
      || isKeyword(token, "reflectionmap") || isKeyword(token, "pbrmap"))
    {
      parseShorthandStage(token, Lighting::Specular);
    }
    else if (isKeyword(token, "bumpmap") || isKeyword(token, "normalmap"))
    {
      parseShorthandStage(token, Lighting::Bump);
    }
    else if (isText(token, "{"))
    {
      parseStage();
    }
    else
    {
      warnAboutSwallowedDecls();
      setDefaulted(token, "Unknown keyword " + describe(token));
    }
  }

  // "diffusemap X" is parsed by the engine as the stage "{ blend diffusemap map X }"
  void parseShorthandStage(const Token& keyword, const Lighting lighting)
  {
    const auto begin = m_pos;
    auto stage = StageState{};
    stage.imageName = parseImageProgram(keyword.text);
    stage.stage.lighting = lighting;

    // The engine joins the program's tokens into text and parses that text again as a
    // stage (idMaterial::ParseMaterial), so quoted strings lose their quotes. A string
    // that is no longer one token then breaks that stage, and the material defaults.
    for (auto k = begin; k < m_pos; ++k)
    {
      const auto& token = m_tokens[k];
      if (token.kind == TokenKind::String && !staysOneToken(token.text))
      {
        setDefaulted(
          token,
          "Quoted image " + describe(token) + " splits when the engine reads the "
            + kdl::str_to_lower(keyword.text) + " shorthand again without quotes");
        return;
      }
    }

    commitStage(std::move(stage));
  }

  // idMaterial::ParseBlend
  void parseBlend(StageState& state)
  {
    using BlendFunc = Quake3ShaderStage::BlendFunc;

    if (atEnd())
    {
      return;
    }

    auto& stage = state.stage;
    const auto& token = next();
    if (isKeyword(token, "blend"))
    {
      stage.blendFunc = {BlendFunc::SrcAlpha, BlendFunc::OneMinusSrcAlpha};
    }
    else if (isKeyword(token, "add"))
    {
      stage.blendFunc = {BlendFunc::One, BlendFunc::One};
    }
    else if (isKeyword(token, "filter") || isKeyword(token, "modulate"))
    {
      stage.blendFunc = {BlendFunc::DestColor, BlendFunc::Zero};
    }
    else if (isKeyword(token, "none"))
    {
      stage.blendFunc = {BlendFunc::Zero, BlendFunc::One};
    }
    else if (isKeyword(token, "bumpmap") || isKeyword(token, "normalmap"))
    {
      stage.lighting = Lighting::Bump;
    }
    else if (isKeyword(token, "diffusemap") || isKeyword(token, "basecolormap"))
    {
      stage.lighting = Lighting::Diffuse;
    }
    else if (isKeyword(token, "specularmap") || isKeyword(token, "rmaomap"))
    {
      stage.lighting = Lighting::Specular;
    }
    else
    {
      // NameToSrcBlendMode and NameToDstBlendMode accept the same names as
      // BlendFunc::validateSrcFactor and BlendFunc::validateDestFactor
      auto blendFunc = BlendFunc{kdl::str_to_upper(token.text), ""};
      if (!blendFunc.validateSrcFactor())
      {
        setDefaulted(token, "Unknown blend source factor " + describe(token));
        return;
      }

      matchToken(",");
      if (m_defaulted || atEnd())
      {
        return;
      }

      const auto& dest = next();
      blendFunc.destFactor = kdl::str_to_upper(dest.text);
      if (!blendFunc.validateDestFactor())
      {
        setDefaulted(dest, "Unknown blend destination factor " + describe(dest));
        return;
      }
      stage.blendFunc = std::move(blendFunc);
    }
  }

  // the index of vertexParm, vertexParm2 and fragmentMap
  bool parseParmIndex(const Token& keyword, const int max)
  {
    // a missing index reads as an empty token, which the engine takes for 0
    const auto* index = nextOnLine();
    if (!index)
    {
      return true;
    }

    // The engine converts the number to an integer, which wraps around modulo 2^32 for
    // huge values and may then land in range. The range is checked on the value as
    // written instead, so a huge index defaults the material; converting an out-of-range
    // float to int would be undefined behaviour.
    const auto value = index->kind == TokenKind::Number ? numberValue(index->text) : 0.0f;
    if (!isNumericText(index->text) || !(value >= 0.0f && value < float(max)))
    {
      setDefaulted(
        *index, "Bad " + std::string{keyword.text} + " number " + describe(*index));
      return false;
    }
    return true;
  }

  // idMaterial::ParseVertexParm
  void parseVertexParm(const Token& keyword)
  {
    if (!parseParmIndex(keyword, MaxVertexParms))
    {
      return;
    }

    parseExpression();
    for (auto i = 0; i < 3 && !m_defaulted; ++i)
    {
      // a token other than ',' on the same line is consumed and ends the parm
      const auto* separator = nextOnLine();
      if (!separator || !isText(*separator, ","))
      {
        return;
      }
      parseExpression();
    }
  }

  // idMaterial::ParseVertexParm2
  void parseVertexParm2(const Token& keyword)
  {
    if (!parseParmIndex(keyword, MaxVertexParms))
    {
      return;
    }

    parseExpression();
    for (auto i = 0; i < 3 && !m_defaulted; ++i)
    {
      matchToken(",");
      if (!m_defaulted)
      {
        parseExpression();
      }
    }
  }

  // idMaterial::ParseFragmentMap
  void parseFragmentMap(const Token& keyword, StageState& state)
  {
    if (!parseParmIndex(keyword, MaxFragmentImages))
    {
      return;
    }

    while (!atEnd() && m_tokens[m_pos].line == m_tokens[m_pos - 1].endLine
           && isAnyKeyword(m_tokens[m_pos], FragmentMapOptions))
    {
      next();
    }

    // without a token on the line, the engine parses an empty image name
    if (atEnd() || m_tokens[m_pos].line != m_tokens[m_pos - 1].endLine)
    {
      return;
    }

    const auto image = parseImageProgram("fragment map");
    if (image && kdl::ci::str_is_equal(*image, "_currentRender"))
    {
      state.referencesCurrentRender = true;
    }
  }

  // idMaterial::ParseStencil
  void parseStencil()
  {
    const auto& open = next();
    if (!isText(open, "{"))
    {
      warn(open, "Missing '{' after stencil");
      return;
    }

    while (!m_defaulted)
    {
      if (atEnd())
      {
        setDefaulted(m_endToken, "Unexpected end of material in a stencil block");
        return;
      }

      const auto& token = next();
      if (isText(token, "}"))
      {
        return;
      }

      if (isAnyKeyword(token, StencilKeywords))
      {
        nextOnLine();
      }
      else
      {
        warn(token, "Unknown stencil keyword " + describe(token));
      }
    }
  }

  // idMaterial::ParseStage, after the opening brace
  void parseStage()
  {
    auto state = StageState{};
    while (true)
    {
      if (m_defaulted)
      {
        // the engine doesn't count a stage it failed to parse
        return;
      }
      if (atEnd())
      {
        setDefaulted(m_endToken, "Unexpected end of material in a stage");
        return;
      }

      const auto& token = next();
      if (isText(token, "}"))
      {
        break;
      }
      parseStageKeyword(token, state);
    }

    commitStage(std::move(state));
  }

  void parseStageKeyword(const Token& token, StageState& state)
  {
    auto& stage = state.stage;

    if (isKeyword(token, "name"))
    {
      skipRestOfLine();
    }
    else if (isKeyword(token, "blend"))
    {
      parseBlend(state);
    }
    else if (isKeyword(token, "map") || isAnyKeyword(token, CubeMapKeywords))
    {
      state.imageName = parseImageProgram("stage image");
    }
    else if (isAnyKeyword(token, RenderMapKeywords))
    {
      state.hasDynamicImage = true;
      parseNumber();
      parseNumber();
    }
    else if (isKeyword(token, "videomap"))
    {
      // [loop] file
      if (!atEnd())
      {
        if (isKeyword(next(), "loop"))
        {
          if (atEnd())
          {
            return;
          }
          next();
        }
        state.hasDynamicImage = true;
      }
    }
    else if (isKeyword(token, "soundmap"))
    {
      if (!atEnd())
      {
        next();
        state.hasDynamicImage = true;
      }
    }
    else if (isKeyword(token, "cubeMapSize"))
    {
      parseNumber();
    }
    else if (isAnyKeyword(token, StageFlags))
    {
    }
    else if (isKeyword(token, "privatePolygonOffset"))
    {
      if (!atEnd() && m_tokens[m_pos].line == m_tokens[m_pos - 1].endLine)
      {
        parseNumber();
      }
    }
    else if (isKeyword(token, "texGen"))
    {
      const auto& texGen = next();
      if (isKeyword(texGen, "reflect"))
      {
        state.texGenReflect = true;
      }
      else if (isKeyword(texGen, "wobbleSky"))
      {
        for (auto i = 0; i < 3 && !m_defaulted; ++i)
        {
          parseExpression();
        }
      }
      else if (
        !isKeyword(texGen, "normal") && !isKeyword(texGen, "reflect2")
        && !isKeyword(texGen, "skybox"))
      {
        setDefaulted(texGen, "Bad texGen " + describe(texGen));
      }
    }
    else if (isAnyKeyword(token, TextureMatrixKeywords))
    {
      parseExpression();
      if (!m_defaulted)
      {
        matchToken(",");
      }
      if (!m_defaulted)
      {
        parseExpression();
      }
    }
    else if (isKeyword(token, "rotate"))
    {
      // the engine looks up the tables sinTable and cosTable
      addReferencedTable("sinTable");
      addReferencedTable("cosTable");
      parseExpression();
    }
    else if (isKeyword(token, "alphaTest"))
    {
      const auto threshold = parseExpression();
      stage.alphaFunc = gl::MaterialAlphaFunc{
        gl::MaterialAlphaFunc::Compare::GreaterEqual, threshold.value_or(0.5f)};
      m_explicitCoverage = Coverage::Perforated;
    }
    else if (isKeyword(token, "color"))
    {
      parseExpression();
      for (auto i = 0; i < 3 && !m_defaulted; ++i)
      {
        matchToken(",");
        if (!m_defaulted)
        {
          parseExpression();
        }
      }
    }
    else if (isAnyKeyword(token, ColorKeywords) || isKeyword(token, "if"))
    {
      parseExpression();
    }
    else if (isAnyKeyword(token, ProgramKeywords))
    {
      if (nextOnLine())
      {
        state.hasDynamicImage = true;
      }
    }
    else if (isKeyword(token, "vertexParm2"))
    {
      parseVertexParm2(token);
    }
    else if (isKeyword(token, "vertexParm"))
    {
      parseVertexParm(token);
    }
    else if (isKeyword(token, "fragmentMap"))
    {
      parseFragmentMap(token, state);
    }
    else if (isKeyword(token, "stencil"))
    {
      parseStencil();
    }
    else
    {
      warnAboutSwallowedDecls();
      setDefaulted(token, "Unknown stage keyword " + describe(token));
    }
  }

  void commitStage(StageState state)
  {
    const auto imageName = state.imageName.value_or(std::string{});
    if (
      state.referencesCurrentRender || kdl::ci::str_is_equal(imageName, "_currentRender"))
    {
      m_referencesCurrentRender = true;
    }

    // the stage's image in idMaterial::ParseStage: the map, else null for a dynamic
    // image, else the default image
    m_stageImages.push_back(
      state.imageName         ? state.imageName
      : state.hasDynamicImage ? std::nullopt
                              : std::optional<std::string>{"_default"});

    state.stage.map = idTech4Path(imageName);
    m_shader.stages.push_back(std::move(state.stage));
    m_stageTexGenReflect.push_back(state.texGenReflect);
  }

  // The stages as idMaterial has them after ParseMaterial: with the implicit stages that
  // AddImplicitStages adds, ordered by SortInteractionStages.
  std::vector<EngineStage> engineStages() const
  {
    auto result = std::vector<EngineStage>{};
    for (size_t i = 0; i < m_shader.stages.size(); ++i)
    {
      result.push_back({m_shader.stages[i].lighting, m_stageImages[i]});
    }

    const auto has = [&](const Lighting lighting) {
      return std::ranges::any_of(
        m_shader.stages, [&](const auto& stage) { return stage.lighting == lighting; });
    };
    const auto hasBump = has(Lighting::Bump);
    const auto hasDiffuse = has(Lighting::Diffuse);
    const auto hasSpecular = has(Lighting::Specular);
    const auto hasReflection =
      std::ranges::find(m_stageTexGenReflect, true) != m_stageTexGenReflect.end();

    if (hasBump || hasDiffuse || hasSpecular)
    {
      if (!hasBump)
      {
        result.push_back({Lighting::Bump, "_flat"});
      }
      if (!hasDiffuse && !hasSpecular && !hasReflection)
      {
        // "map _white", which the image program turns into guis/assets/white
        result.push_back({Lighting::Diffuse, "guis/assets/white"});
      }
    }

    // SortInteractionStages: a stable bubble sort of each group of stages that starts
    // with a bump stage; if the first stage is not a bump stage, everything is one group
    const auto count = result.size();
    for (size_t i = 0, j = 0; i < count; i = j)
    {
      for (j = i + 1; j < count; ++j)
      {
        if (result[j].lighting == Lighting::Bump)
        {
          if (result[i].lighting != Lighting::Bump)
          {
            continue;
          }
          break;
        }
      }

      for (size_t l = 1; l < j - i; ++l)
      {
        for (size_t k = i; k < j - l; ++k)
        {
          if (
            engineLightingOrder(result[k].lighting)
            > engineLightingOrder(result[k + 1].lighting))
          {
            std::swap(result[k], result[k + 1]);
          }
        }
      }
    }

    return result;
  }

  // Drops empty names and the engine's generated images (names starting with '_'),
  // which the editor can't load, and duplicates.
  void setPreviewImages(const std::vector<std::string>& candidates)
  {
    auto names = std::vector<std::string>{};
    for (const auto& candidate : candidates)
    {
      if (
        !candidate.empty() && !candidate.starts_with('_')
        && std::ranges::none_of(names, [&](const auto& name) {
             return kdl::ci::str_is_equal(name, candidate);
           }))
      {
        names.push_back(candidate);
        m_shader.previewImages.push_back(idTech4Path(candidate));
      }
    }
  }

  void finish()
  {
    m_shader.editorImage = idTech4Path(m_editorImageName);

    if (m_defaulted)
    {
      makeEngineDefaulted(m_shader);
      return;
    }

    const auto stages = engineStages();

    // idMaterial::Parse: explicit coverage, else derived from the stages
    if (m_explicitCoverage)
    {
      m_shader.coverage = *m_explicitCoverage;
    }
    else if (stages.empty())
    {
      m_shader.coverage = Coverage::Translucent;
    }
    else if (std::ranges::any_of(stages, [](const auto& stage) {
               return stage.lighting != Lighting::None;
             }))
    {
      m_shader.coverage = Coverage::Opaque;
    }
    else
    {
      // without interaction stages, nothing was added or reordered
      m_shader.coverage = blendsWithDestination(m_shader.stages.front().blendFunc)
                            ? Coverage::Translucent
                            : Coverage::Opaque;
    }

    if (m_referencesCurrentRender && !m_sortPortalSky)
    {
      m_shader.coverage = Coverage::Translucent;
    }

    // idMaterial::GetEditorImage: the editor image, falling back to the first diffuse
    // stage if the editor image can't be loaded; without an editor image, the first
    // diffuse stage. In both cases, if the first diffuse stage has no image (a video,
    // say), the first stage's image is used. Stages without an image add nothing.
    const auto firstDiffuse = std::ranges::find_if(
      stages, [](const auto& stage) { return stage.lighting == Lighting::Diffuse; });
    const auto diffuseImage = [&]() {
      return firstDiffuse != stages.end() && firstDiffuse->image ? firstDiffuse->image
                                                                 : stages.front().image;
    };

    auto candidates = std::vector<std::string>{};
    if (!m_editorImageName.empty())
    {
      candidates.push_back(m_editorImageName);
      if (firstDiffuse != stages.end())
      {
        if (const auto image = diffuseImage())
        {
          candidates.push_back(*image);
        }
      }
    }
    else if (!stages.empty())
    {
      if (const auto image = diffuseImage())
      {
        candidates.push_back(*image);
      }
    }
    setPreviewImages(candidates);

    switch (m_shader.coverage)
    {
    case Coverage::Opaque:
      addFlag("coverage_opaque");
      break;
    case Coverage::Perforated:
      addFlag("coverage_perforated");
      break;
    case Coverage::Translucent:
      addFlag("coverage_translucent");
      break;
    }

    if (m_shader.stages.empty())
    {
      addFlag("nostages");
    }
  }
};

// ---------------------------------------------------------------------------------------
// Layer 2: the decl splitter, a copy of idDeclFile::LoadAndParse
// ---------------------------------------------------------------------------------------

// the decl types registered by the engine and the game, matched case-insensitively
// clang-format off
constexpr auto DeclTypes = std::array<std::string_view, 15>{
  "table", "material", "skin", "entityDef", "sound", "mapDef", "fx", "particle",
  "articulatedFigure", "pda", "email", "video", "audio", "model", "export",
};
// clang-format on

class DeclSplitter
{
private:
  struct BracedSection
  {
    size_t end;
    bool terminated;
  };

  const std::vector<Token>& m_tokens;
  ParserStatus& m_status;
  std::vector<std::string> m_declaredTables;

public:
  DeclSplitter(const std::vector<Token>& tokens, ParserStatus& status)
    : m_tokens{tokens}
    , m_status{status}
  {
  }

  std::vector<std::string>& declaredTables() { return m_declaredTables; }

  std::vector<Quake3Shader> split()
  {
    auto result = std::vector<Quake3Shader>{};
    auto firstDefinitions = std::unordered_map<std::string, size_t>{};

    auto i = size_t(0);
    while (m_tokens[i].kind != TokenKind::Eof)
    {
      // the decl type, or the name of a material
      const auto& first = m_tokens[i];
      const auto type = std::ranges::find_if(
        DeclTypes, [&](const auto& t) { return isKeyword(first, t); });
      if (type != DeclTypes.end())
      {
        ++i;
      }
      else if (isText(first, "{"))
      {
        m_status.warn(locationOf(first), "Missing decl name; skipping this block");
        i = skipBraced(i + 1, 1).end;
        continue;
      }

      const auto& nameToken = m_tokens[i];
      if (nameToken.kind == TokenKind::Eof)
      {
        m_status.warn(locationOf(first), "Type without definition at end of file");
        break;
      }
      ++i;

      if (isText(nameToken, "{"))
      {
        m_status.warn(locationOf(nameToken), "Missing decl name; skipping this block");
        i = skipBraced(i, 1).end;
        continue;
      }

      if (type != DeclTypes.end() && *type == "export")
      {
        // the engine skips export decls with SkipBracedSection( true )
        i = skipBraced(i, 0).end;
        continue;
      }

      const auto& open = m_tokens[i];
      if (open.kind == TokenKind::Eof)
      {
        m_status.warn(locationOf(nameToken), "Type without definition at end of file");
        break;
      }
      ++i;

      if (!isText(open, "{"))
      {
        // like the engine, continue with the token after the unexpected one
        m_status.warn(
          locationOf(open), "Expecting '{' but found '" + asciiText(open.text) + "'");
        continue;
      }

      const auto begin = i;
      // SkipBracedSection counts punctuation braces only; a quoted "{" opens nothing
      const auto section =
        open.kind == TokenKind::Punct ? skipBraced(i, 1) : BracedSection{i, true};
      i = section.end;

      // only quoted names can contain non-ASCII bytes; see asciiText
      const auto name = asciiText(nameToken.text);
      if (type != DeclTypes.end() && *type == "table")
      {
        auto tableName = canonicalIdTech4DeclName(nameToken.text);
        if (std::ranges::find(m_declaredTables, tableName) == m_declaredTables.end())
        {
          m_declaredTables.push_back(std::move(tableName));
        }
      }
      if (type != DeclTypes.end() && *type != "material")
      {
        m_status.debug(
          locationOf(nameToken), "Skipped " + std::string{*type} + " '" + name + "'");
        continue;
      }

      const auto canonicalName = canonicalIdTech4DeclName(nameToken.text);
      const auto [firstDefinition, inserted] =
        firstDefinitions.emplace(canonicalName, nameToken.line);
      if (!inserted)
      {
        m_status.warn(
          locationOf(nameToken),
          "Material '" + name + "' was already defined at line "
            + std::to_string(firstDefinition->second) + "; ignoring this definition");
        continue;
      }

      const auto nonAsciiName = hasNonAscii(nameToken.text);
      if (nonAsciiName)
      {
        m_status.warn(
          locationOf(nameToken),
          "Non-ASCII characters in the name of material '" + name
            + "' were replaced with '_'");
      }
      if (!section.terminated)
      {
        m_status.warn(
          locationOf(nameToken),
          "Material '" + name + "' has no matching '}' and runs to the end of the file");
      }

      result.push_back(MaterialBlockParser{
        m_tokens,
        begin,
        section.end,
        !section.terminated,
        name,
        nonAsciiName,
        idTech4Path(canonicalName),
        m_status}
                         .parse());
    }

    return result;
  }

private:
  // idLexer::SkipBracedSection, starting at the given depth: returns the index after the
  // matching closing brace, or the index of the Eof token if the file ends first
  BracedSection skipBraced(size_t i, int depth) const
  {
    do
    {
      const auto& token = m_tokens[i];
      if (token.kind == TokenKind::Eof)
      {
        return {i, false};
      }
      ++i;

      if (isPunct(token, "{"))
      {
        ++depth;
      }
      else if (isPunct(token, "}"))
      {
        --depth;
      }
    } while (depth != 0);

    return {i, true};
  }
};

} // namespace

Doom3MaterialParser::Doom3MaterialParser(const std::string_view str)
  : m_str{str}
{
}

std::vector<Quake3Shader> Doom3MaterialParser::parse(ParserStatus& status)
{
  auto concatenatedStrings = std::deque<std::string>{};
  const auto tokens = Lexer{m_str, &status, concatenatedStrings}.tokenize();

  auto splitter = DeclSplitter{tokens, status};
  auto shaders = splitter.split();
  m_declaredTables = std::move(splitter.declaredTables());
  return shaders;
}

const std::vector<std::string>& Doom3MaterialParser::declaredTables() const
{
  return m_declaredTables;
}

} // namespace tb::mdl
