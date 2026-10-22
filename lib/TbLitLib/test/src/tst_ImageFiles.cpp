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

// LP1c.2 (D51, R5.3, S14): the loader pre-validation, and the copied loaders behind it.
// Every bad file is refused outside the copied code, so its loader is handed no buffer
// (no read past an end: Debug CRT here, ASan on the user's Linux checklist), and the
// image is the default with one warning naming the file.

#include "ImageTestFiles.h"
#include "LitTestUtils.h"
#include "fs/TestEnvironment.h"
#include "lit/GameFileSource.h"
#include "lit/ImageCache.h"
#include "lit/MaterialDecls.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::lit
{
namespace
{

using namespace engine_image;
using namespace test;

// the same file with its last bytes cut
std::string cut(const std::string& bytes, const std::size_t count)
{
  return bytes.substr(0, bytes.size() - count);
}

std::optional<std::string> problemOf(const std::string& path, const std::string& bytes)
{
  return imageFileProblem(path, bytes);
}

std::vector<std::uint8_t> greyOf(const Rgba& image)
{
  auto out = std::vector<std::uint8_t>{};
  for (auto i = 0; i < image.width * image.height; ++i)
  {
    const auto r = image.pixels[std::size_t(4 * i)];
    out.insert(out.end(), {r, r, r, 255});
  }
  return out;
}

std::vector<std::uint8_t> opaque(const Rgba& image)
{
  auto out = image.pixels;
  for (std::size_t i = 3; i < out.size(); i += 4)
  {
    out[i] = 255;
  }
  return out;
}

// Mesa's unsigned 11- and 10-bit floats, decoded independently of the copied header
float unpackUf(const std::uint32_t bits, const int mantissaBits)
{
  const auto exponent = int((bits >> mantissaBits) & 0x1F);
  const auto mantissa = float(bits & ((1u << mantissaBits) - 1));
  const auto scale = float(1u << mantissaBits);
  return exponent == 0 ? std::ldexp(mantissa / scale, -14)
                       : std::ldexp(1.0f + mantissa / scale, exponent - 15);
}

std::vector<float> unpackR11G11B10F(const std::vector<std::uint8_t>& pixels)
{
  auto out = std::vector<float>{};
  for (std::size_t i = 0; i + 3 < pixels.size(); i += 4)
  {
    const auto value = std::uint32_t(pixels[i]) | (std::uint32_t(pixels[i + 1]) << 8)
                       | (std::uint32_t(pixels[i + 2]) << 16)
                       | (std::uint32_t(pixels[i + 3]) << 24);
    out.push_back(unpackUf(value & 0x7FF, 6));
    out.push_back(unpackUf((value >> 11) & 0x7FF, 6));
    out.push_back(unpackUf((value >> 22) & 0x3FF, 5));
  }
  return out;
}

// powers of two: exact in RGBE and in R11G11B10F
const std::vector<float> HdrRgb = {
  1.0f, 0.5f, 2.0f, 4.0f, 0.25f, 1.0f, 0.125f, 8.0f, 0.5f, 2.0f, 2.0f, 1.0f};

std::vector<float> hdrRows(const int width, const int height)
{
  auto rgb = std::vector<float>{};
  for (auto i = 0; i < width * height; ++i)
  {
    for (auto c = 0; c < 3; ++c)
    {
      rgb.push_back(HdrRgb[std::size_t(3 * i + c) % HdrRgb.size()]);
    }
  }
  return rgb;
}

// one image file per case under textures/lp1c/ of a game folder
class LoaderGame
{
private:
  fs::TestEnvironment m_env;

public:
  void file(const std::string& name, const std::string& bytes) const
  {
    writeTextFile(m_env.dir() / "base" / "textures" / "lp1c" / name, bytes);
  }

  const std::filesystem::path& path() const { return m_env.dir(); }
};

struct Loaded
{
  SourceLoad load;
  // the warnings the registry received during the load
  std::vector<std::string> warnings;
};

Loaded loadFrom(MaterialDecls& decls, const std::string& image, const int usage)
{
  auto key = MaterialImageKey{};
  key.name = "textures/lp1c/" + image;
  key.usage = usage;
  key.filter = FilterDefault;
  const auto name = decls.imageCacheName(key);
  REQUIRE(name);
  static_cast<void>(decls.takeMessages());
  auto loaded = Loaded{decls.loadImagePixels(*name.value, 0, false), {}};
  for (const auto& message : decls.takeMessages())
  {
    if (message.kind == DeclMessageKind::Warning)
    {
      loaded.warnings.push_back(message.text);
    }
  }
  return loaded;
}

} // namespace

TEST_CASE("Image files, the loader pre-validation (D51)")
{
  const auto image = gradient(6, 5);

  SECTION("TGA: type, colour map, pixel size, length and run-length packets")
  {
    CHECK_FALSE(problemOf("a.tga", tgaFile(image, 32)));
    CHECK_FALSE(problemOf("a.TGA", tgaFile(image, 24)));
    CHECK_FALSE(problemOf("a.tga", greyTgaFile(image)));
    CHECK_FALSE(problemOf("a.tga", rleTgaFile(image, 32)));
    CHECK_FALSE(problemOf("a.tga", rleTgaFile(flat(9, 7, 1, 2, 3), 24)));

    const auto truncated = problemOf("a.tga", cut(tgaFile(image, 32), 7));
    REQUIRE(truncated);
    CHECK_THAT(*truncated, Catch::Matchers::ContainsSubstring("cut short"));

    // 16 bits, in an RGB and in a grey image: LoadTGA's Error sites
    auto sixteen = tgaFile(image, 32);
    sixteen[16] = 16;
    const auto sixteenProblem = problemOf("a.tga", sixteen);
    REQUIRE(sixteenProblem);
    CHECK_THAT(*sixteenProblem, Catch::Matchers::ContainsSubstring("16"));
    auto greySixteen = greyTgaFile(image);
    greySixteen[16] = 16;
    CHECK(problemOf("a.tga", greySixteen));

    // run-length packets that run past the end, and garbage after a good header
    CHECK(problemOf("a.tga", cut(rleTgaFile(image, 32), 3)));
    auto garbage = rleTgaFile(flat(64, 64, 9, 9, 9), 32).substr(0, 18);
    garbage += std::string{"\x05\x01\x02\x03", 4};
    CHECK(problemOf("a.tga", garbage));

    // an oversized header: 65535 × 65535 declared over a few bytes
    auto oversized = tgaFile(image, 32);
    oversized[12] = oversized[13] = oversized[14] = oversized[15] = '\xFF';
    CHECK(problemOf("a.tga", oversized));
    // a run-length header beyond the limits, whatever its packets
    auto huge = rleTgaFile(flat(2, 2, 1, 1, 1), 32);
    huge[12] = huge[14] = char(0x00);
    huge[13] = huge[15] = char(0x41);
    CHECK_THAT(
      *problemOf("a.tga", huge), Catch::Matchers::ContainsSubstring("more than the"));

    auto mapped = tgaFile(image, 32);
    mapped[1] = 1;
    CHECK(problemOf("a.tga", mapped));
    auto typeOne = tgaFile(image, 32);
    typeOne[2] = 1;
    CHECK(problemOf("a.tga", typeOne));
    CHECK(problemOf("a.tga", tgaFile(image, 32).substr(0, 17)));
  }

  SECTION("PNG: stb_image's header rules, the chunks and the declared size")
  {
    const auto png = pngFile(image);
    CHECK_FALSE(problemOf("a.png", png));
    CHECK_FALSE(problemOf("a.png", pngFile(image, false)));
    // a lying IHDR: 4096 × 4096 over the IDAT of 6 × 5 pixels
    auto lying = png;
    lying[16 + 2] = 0x10;
    lying[16 + 3] = 0x00;
    lying[20 + 2] = 0x10;
    lying[20 + 3] = 0x00;
    const auto lyingProblem = problemOf("a.png", lying);
    REQUIRE(lyingProblem);
    CHECK_THAT(*lyingProblem, Catch::Matchers::ContainsSubstring("cannot hold"));
    CHECK(problemOf("a.png", cut(png, 12)));
    CHECK(problemOf("a.png", "\x89PNG but not one"));
  }

  SECTION("PNG and JPG by their content, as stb_image decodes them (S3.3)")
  {
    // a .png holding a JPEG, a .jpg holding a PNG: checked by the JPEG and PNG rules
    CHECK_FALSE(problemOf("a.png", greyJpgFile(8, 8)));
    CHECK_FALSE(problemOf("a.jpg", pngFile(image)));
    CHECK(problemOf("a.png", greyJpgFile(4096, 4096, 1)));
    // stb_image's other formats in a .png: a TGA, read by stb_image's own TGA decoder
    CHECK_FALSE(problemOf("a.png", tgaFile(image, 32)));
    CHECK(isStbLoaderFile("textures/a.PNG"));
    CHECK_FALSE(isStbLoaderFile("textures/a.tga"));
  }

  SECTION("headers from a file's first bytes (S2.2)")
  {
    const auto check = [](
                         const std::string& path,
                         const std::string& bytes,
                         const std::size_t head,
                         const int width,
                         const int height) {
      CAPTURE(path, head);
      const auto header = imageFileHeadHeader(
        path, bytes.substr(0, std::min(head, bytes.size())), bytes.size());
      INFO(header.problem.value_or(""));
      CHECK_FALSE(header.problem);
      CHECK_FALSE(header.needsMore);
      CHECK(header.width == width);
      CHECK(header.height == height);
    };
    check("a.tga", tgaFile(image, 32), 18, 6, 5);
    check("a.tga", rleTgaFile(image, 32), 18, 6, 5);
    check("a.png", pngFile(image), 33, 6, 5);
    check("a.jpg", greyJpgFile(40, 24), 4096, 40, 24);
    check("a.png", greyJpgFile(40, 24), 4096, 40, 24);
    check("a.hdr", hdrFile(8, 2, hdrRows(8, 2)), 4096, 8, 2);
    check("a.exr", exrFile(3, 2, hdrRows(3, 2)), 4096, 3, 2);

    // a header that goes on beyond the bytes given asks for more
    const auto jpg = greyJpgFile(40, 24);
    CHECK(imageFileHeadHeader("a.jpg", jpg.substr(0, 10), jpg.size()).needsMore);
    CHECK(imageFileHeadHeader("a.tga", tgaFile(image).substr(0, 10), 200).needsMore);
    // the header's own rules and the length a header tells
    auto sixteen = tgaFile(image, 32);
    sixteen[16] = 16;
    CHECK(imageFileHeadHeader("a.tga", sixteen.substr(0, 18), sixteen.size()).problem);
    const auto tga = tgaFile(image, 32);
    CHECK(imageFileHeadHeader("a.tga", tga.substr(0, 18), tga.size() - 7).problem);
    CHECK(imageFileHeadHeader("a.tga", "", 0).problem);
  }

  SECTION("JPG: the frame header, the segments and scans to EOI, the declared size")
  {
    CHECK_FALSE(problemOf("a.jpg", greyJpgFile(8, 8)));
    CHECK_FALSE(problemOf("a.jpg", greyJpgFile(10, 1)));
    CHECK_FALSE(problemOf("a.jpg", greyJpgFile(40, 24)));
    // a 4096 × 4096 header over the scan bits of one block (the "10-pixel JPG")
    const auto lying = problemOf("a.jpg", greyJpgFile(4096, 4096, 1));
    REQUIRE(lying);
    CHECK_THAT(*lying, Catch::Matchers::ContainsSubstring("cannot hold"));
    CHECK(problemOf("a.jpg", greyJpgFile(8, 8).substr(0, 10)));
    CHECK(problemOf("a.jpg", greyJpgFile(40, 24, -1, true)));
    // an arithmetic-coded frame (SOF9)
    auto arithmetic = greyJpgFile(8, 8);
    arithmetic[2 + 69 + 1] = '\xC9';
    CHECK(problemOf("a.jpg", arithmetic));
  }

  SECTION("HDR: the Radiance header and the scanlines as stb_image reads them")
  {
    CHECK_FALSE(problemOf("a.hdr", hdrFile(2, 2, hdrRows(2, 2))));
    const auto rle = hdrFile(8, 2, hdrRows(8, 2));
    CHECK_FALSE(problemOf("a.hdr", rle));
    CHECK(problemOf("a.hdr", cut(rle, 5)));
    CHECK(problemOf("a.hdr", cut(hdrFile(2, 2, hdrRows(2, 2)), 1)));
    CHECK(problemOf("a.hdr", "#?NOTHING\n\n-Y 1 +X 1\n"));
    CHECK(problemOf("a.hdr", "#?RADIANCE\n\n-Y 1 +X 1\n\x01\x02\x03\x04"));
    auto badLength = rle;
    badLength[rle.find("+X 8\n") + 5 + 3] = 9;
    CHECK(problemOf("a.hdr", badLength));
    // S2.4: a float format holds a quarter of the pixels (4097 x 4096 is beyond it)
    CHECK_THAT(
      *problemOf("a.hdr", "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 4097 +X 4096\n"),
      Catch::Matchers::ContainsSubstring("more than the preview loads"));
    CHECK_THAT(
      *problemOf("a.hdr", "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 4096 +X 4096\n"),
      Catch::Matchers::ContainsSubstring("cut short"));
  }

  SECTION("EXR: tinyexr's header parse, the channels and the offset table")
  {
    const auto exr = exrFile(3, 2, hdrRows(3, 2));
    const auto good = problemOf("a.exr", exr);
    INFO(good.value_or(""));
    CHECK_FALSE(good);
    CHECK(problemOf("a.exr", cut(exr, 4)));
    CHECK(problemOf("a.exr", exr.substr(0, 30)));
    CHECK(problemOf("a.exr", "not an exr"));
  }

  SECTION("an empty file fails; a path no loader takes passes")
  {
    for (const auto* path : {"a.tga", "a.png", "a.jpg", "a.exr", "a.hdr"})
    {
      CAPTURE(path);
      const auto problem = problemOf(path, "");
      REQUIRE(problem);
      CHECK(*problem == "it is empty");
    }
    CHECK_FALSE(problemOf("materials/a.mtr", ""));
    CHECK_FALSE(problemOf("a.dds", "anything"));
    CHECK(isImageLoaderFile("textures/a.JPG"));
    CHECK_FALSE(isImageLoaderFile("textures.tga/a"));
  }
}

TEST_CASE("Image files, the copied loaders behind the pre-validation (D51, S14)")
{
  const auto game = LoaderGame{};
  const auto image = gradient(6, 5);
  game.file("rgba.tga", tgaFile(image, 32));
  game.file("rgb.tga", tgaFile(image, 24));
  game.file("grey.tga", greyTgaFile(image));
  game.file("rle.tga", rleTgaFile(image, 32));
  game.file("pic.png", pngFile(image));
  game.file("pic.jpg", greyJpgFile(10, 1));
  game.file("flat.hdr", hdrFile(2, 2, hdrRows(2, 2)));
  game.file("runs.hdr", hdrFile(8, 2, hdrRows(8, 2)));
  game.file("pic.exr", exrFile(3, 2, hdrRows(3, 2)));

  game.file("truncated.tga", cut(tgaFile(image, 32), 7));
  auto sixteen = tgaFile(image, 32);
  sixteen[16] = 16;
  game.file("sixteen.tga", sixteen);
  auto garbage = rleTgaFile(flat(64, 64, 9, 9, 9), 32).substr(0, 18);
  garbage += std::string{"\x05\x01\x02\x03", 4};
  game.file("garbage.tga", garbage);
  auto oversized = tgaFile(image, 32);
  oversized[12] = oversized[13] = oversized[14] = oversized[15] = '\xFF';
  game.file("oversized.tga", oversized);
  auto lying = pngFile(image);
  lying[18] = lying[22] = 0x10;
  lying[19] = lying[23] = 0x00;
  game.file("lying.png", lying);
  game.file("empty.tga", "");
  game.file("ten.jpg", greyJpgFile(4096, 4096, 1));
  game.file("cut.jpg", greyJpgFile(8, 8).substr(0, 10));
  // S3.3: a .png holding a JPEG, and a refused .png with a .tga beside it
  game.file("jpeg_in.png", greyJpgFile(10, 1));
  game.file("fall.png", lying);
  game.file("fall.tga", tgaFile(image, 32));

  auto source = GameFileSource{game.path()};
  auto decls = MaterialDecls{source};
  REQUIRE(decls.constructionError().empty());

  SECTION("good files decode as the engine decodes them")
  {
    for (const auto& [file, expected] :
         std::vector<std::pair<std::string, std::vector<std::uint8_t>>>{
           {"rgba", image.pixels},
           {"rgb", opaque(image)},
           {"grey", greyOf(image)},
           {"rle", image.pixels},
           {"pic.png", image.pixels},
         })
    {
      CAPTURE(file);
      const auto loaded = loadFrom(decls, file, UsageDefault);
      INFO(loaded.load.error);
      REQUIRE(loaded.load.pixels);
      CHECK(loaded.load.pixels->width == 6);
      CHECK(loaded.load.pixels->height == 5);
      CHECK(loaded.load.pixels->pixels == expected);
      CHECK(loaded.load.rejected.empty());
      CHECK(loaded.warnings.empty());
      REQUIRE_FALSE(loaded.load.pixels->files.empty());
      CHECK(loaded.load.pixels->files.back().probe.kind == GameFileProbe::Kind::Loose);
    }

    // the JPG: ten grey pixels (one flat block)
    const auto jpg = loadFrom(decls, "pic.jpg", UsageDefault);
    REQUIRE(jpg.load.pixels);
    CHECK(jpg.load.pixels->width == 10);
    CHECK(jpg.load.pixels->height == 1);
    CHECK(jpg.load.pixels->pixels == greyOf(flat(10, 1, 128, 0, 0)));

    // the HDR and EXR loaders give R11G11B10F, here exact (powers of two)
    for (const auto* file : {"flat.hdr", "runs.hdr", "pic.exr"})
    {
      CAPTURE(file);
      const auto loaded = loadFrom(decls, file, UsageHdri);
      INFO(loaded.load.error);
      REQUIRE(loaded.load.pixels);
      const auto& pixels = *loaded.load.pixels;
      CHECK(unpackR11G11B10F(pixels.pixels) == hdrRows(pixels.width, pixels.height));
    }
  }

  SECTION("bad TGA files are the default, with one warning naming the file")
  {
    for (const auto* file : {"truncated", "sixteen", "garbage", "oversized", "empty"})
    {
      CAPTURE(file);
      const auto loaded = loadFrom(decls, file, UsageDiffuse);
      CHECK_FALSE(loaded.load.pixels);
      REQUIRE(loaded.load.rejected.size() == 1u);
      REQUIRE(loaded.warnings.size() == 1u);
      CHECK_THAT(
        loaded.warnings.front(),
        Catch::Matchers::ContainsSubstring(std::string{"textures/lp1c/"} + file));
      CHECK_THAT(
        loaded.warnings.front(), Catch::Matchers::ContainsSubstring("the default image"));
    }
  }

  SECTION("a bad PNG or JPG is stb_image's failure: the next extension is tried (S3.3)")
  {
    for (const auto* file : {"lying.png", "ten.jpg", "cut.jpg"})
    {
      CAPTURE(file);
      const auto loaded = loadFrom(decls, file, UsageDiffuse);
      CHECK_FALSE(loaded.load.pixels);
      REQUIRE(loaded.load.rejected.size() == 1u);
      // the engine's own warning for the failed load, then the refusal
      REQUIRE(loaded.warnings.size() == 2u);
      CHECK_THAT(
        loaded.warnings.front(),
        Catch::Matchers::ContainsSubstring("Couldn't load image"));
      CHECK_THAT(
        loaded.warnings.back(),
        Catch::Matchers::ContainsSubstring(std::string{"textures/lp1c/"} + file));
      CHECK_THAT(
        loaded.warnings.back(), Catch::Matchers::ContainsSubstring("next extension"));
    }

    // a refused .png with a .tga beside it loads the .tga, as the engine's loop does
    const auto fall = loadFrom(decls, "fall.png", UsageDiffuse);
    INFO(fall.load.error);
    REQUIRE(fall.load.pixels);
    CHECK(fall.load.pixels->pixels == image.pixels);
    CHECK(fall.load.rejected.size() == 1u);
    CHECK(fall.load.pixels->files.back().path == "textures/lp1c/fall.tga");
  }

  SECTION("a .png holding a JPEG loads (S3.3)")
  {
    const auto loaded = loadFrom(decls, "jpeg_in.png", UsageDefault);
    INFO(loaded.load.error);
    REQUIRE(loaded.load.pixels);
    CHECK(loaded.load.pixels->width == 10);
    CHECK(loaded.load.pixels->height == 1);
    CHECK(loaded.load.pixels->pixels == greyOf(flat(10, 1, 128, 0, 0)));
    CHECK(loaded.load.rejected.empty());
  }

  SECTION("a missing image gives the engine's warning")
  {
    const auto loaded = loadFrom(decls, "none", UsageDiffuse);
    CHECK_FALSE(loaded.load.pixels);
    CHECK(loaded.load.rejected.empty());
    REQUIRE(loaded.warnings.size() == 1u);
    CHECK(
      loaded.warnings.front()
      == "Couldn't load image: textures/lp1c/none : textures/lp1c/none#__0100");
  }

  SECTION("a quiet load keeps its messages")
  {
    auto key = MaterialImageKey{};
    key.name = "textures/lp1c/truncated";
    key.usage = UsageDiffuse;
    const auto name = decls.imageCacheName(key);
    REQUIRE(name);
    static_cast<void>(decls.takeMessages());
    const auto load = decls.loadImagePixels(*name.value, 0, true);
    CHECK_FALSE(load.pixels);
    CHECK(load.messages.size() == 1u);
    CHECK(decls.takeMessages().empty());
  }

  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
}

} // namespace tb::lit
