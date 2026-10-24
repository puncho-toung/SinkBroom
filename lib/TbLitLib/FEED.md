# Driving the lit preview from a host outside SinkBroom

TbLitLib is the lit preview's renderer: the RBDOOM-3-BFG engine's material, image and
lighting code, driven through NVRHI on Vulkan 1.2. SinkBroom (the TrenchBroom editor) is
one host; this file is the contract for any other host — the Mixar material editor in
particular — that links TbLitLib without Qt and without a map document (plan
`plans/lit-preview/37-LPF-PACKAGES.md`, D107, D108). The two examples at the end are
compiled and run as the hidden test case `Feed host: FEED.md's examples` in
`test/src/tst_Feed.cpp`; that file's other `[.][feed]` cases are the specification of
everything below.

## 1. What a host gets, and what it does not

- A headless session (pixels read back) or a session presenting to the host's own
  `VkSurfaceKHR`.
- Geometry from snapshot records (`lit/FeedScene.h`: a quad brush, a sphere as one
  patchDef3, point lights, worldspawn) or from a map text (`lit::snapshotOfMapText`).
- Images handed over as bytes under a game image name, drawn within tens of milliseconds
  without a file (`MaterialService::setImageOverride`), and a material's decl text
  re-parsed in place (`MaterialService::reparseMaterial`).
- **K34:** a fed image is drawn uncompressed (RGBA8, or R11G11B10F for the HDR usages),
  converted by the engine's own fallback code (its YCoCg and normal layouts, its gamma
  mips), where the game draws the binarizer's DXT/BC6H cache. Save the file and let the
  game binarize it for the compressed look; after `clearImageOverride` the preview reads
  the file again.
- **K35 (D109):** without a map and its bake there is no ambient light and no image-based
  lighting: direct lights only, black ambient. A host-supplied environment is not part of
  LPF (Q28).
- **K36:** an override may carry `editorSize`, the size C21 scales a material's texture
  coordinates by (the engine takes it from the files alone). Without it a material whose
  images are all fed has the 8 × 8 default and tiles its quad 128 times.

## 2. The order of creation

1. **The instance.** `lit::VulkanInstance::create( VulkanInstanceOptions )`
   (`lit/VulkanInstance.h`) loads the Vulkan loader at runtime (`vulkan-1.dll`,
   `libvulkan.so.1`, `libvulkan.1.dylib`) and makes a Vulkan 1.2 instance; `surface =
   true` adds the platform's surface extensions (Windows: `VK_KHR_surface`,
   `VK_KHR_win32_surface`; Linux: `VK_KHR_surface` and each of `VK_KHR_xcb_surface`,
   `VK_KHR_xlib_surface`, `VK_KHR_wayland_surface` the loader offers; macOS:
   `VK_KHR_surface`, `VK_EXT_metal_surface`, and `VK_KHR_portability_enumeration` with its
   create flag), `validation = true` the Khronos layer when installed with a messenger
   whose messages go to `options.messages` (`feed::vulkanMessagesAsWarnings` turns them
   into the session's Warning events). Only Windows is built and tested here; Linux and
   macOS are written from the Vulkan headers and wait for their first run. Then
   `initInstanceDispatcher( instance.handles() )`.
2. **The listener.** `std::make_shared< feed::GuardedListener >( next )` (`lit/FeedHost.h`),
   `next` being the host's own handler of the service's events (its log lines), or empty.
3. **The material service.** `MaterialServiceOptions` with `makeSource` returning a
   `GameDeclSource( gamePath, &cancel )`, `parseAll = true`, `listener =
   feed::GuardedListener::listener( shared )`; `MaterialService::create`. The game folder
   holds the materials, the images and the entityDefs: a light's `light` entityDef (C22)
   and the default light shaders (`lights/defaultPointLight`, `lights/defaultProjectedLight`)
   are the game's.
4. **The session.** `LitSession::create( instance.handles(), surface, width, height,
   options, eventSink, generation, service.get() )`: `surface` is `VK_NULL_HANDLE` for a
   headless session (an offscreen target of width × height) or the host's `VkSurfaceKHR`
   made with the same instance; `SessionOptions::gamePath` is the game folder whose
   `base/renderprogs2/spirv` holds the engine's shader blobs (C1) — **without it every
   program is disabled** and nothing is lit. One session lives in a process at a time
   (the Vulkan dispatcher is process-wide). Then `listener->attach( session.get() )`.
5. **`session->setExposed( true )`.** A session draws nothing until it is exposed (it
   mirrors a window's exposure); a headless host sets it once.
6. **The first snapshot** (section 4), offered with `offerSnapshot`.
7. **The camera:** `setCamera( feed::cameraLookingAt( eye, target, fov ) )`, any time.
8. **Overrides and re-parses** (sections 5 and 6), any time.
9. **Pixels:** `readbackLdr( timeout )` (section 7), or the session presents to the
   surface by itself.
10. **The teardown** (section 8).

## 3. Threads

- Every public call on the session and the service may be made from any thread unless its
  header says otherwise; each enqueues (latest wins for the camera) and returns at once,
  except `readbackLdr`, which blocks its caller (never the render thread) until a settled
  frame has finished on the GPU or the timeout passes.
- `offerSnapshot`, the swapchain calls (`park`, `destroySwapchain`, `createSwapchain`,
  `unpark`) and the teardown belong to the thread that created the session (the editor's
  GUI thread); the snapshot records may be built on any thread.
- `tableChanged()` may be called from any thread; the guarded listener calls it from the
  service's parse thread on every published table.
- The event sink and the instance's message sink are called from the session's threads
  (render, compile, parse) and must neither block nor throw.
- Only the service's parse thread runs the copied engine code (C20); a host never calls
  into it.

## 4. Snapshots

A snapshot (`lit/Snapshot.h`, C23) is plain data: nodes, entities, faces, patches, keys
and a string pool. Every snapshot carries the session's `generation` (the one given to
`create`; a snapshot of another generation is dropped, D53) and a `sequence` rising from
1. The first snapshot of a session is complete; later ones carry the nodes that changed,
the ids removed and the entities whose keys changed. At most one waits in the session's
inbox: `offerSnapshot` returns false while the last one is still there; offer again on
`SessionEvent::ReadyForSnapshot` or when `snapshotSlotFree()`.

Worldspawn comes first: `feed::worldspawn( snapshot, worldId )`, and brushes and patches
whose entity is `worldId` are world geometry. `feed::quadBrush` makes an axial six-sided
brush whose face toward its normal carries one material and whose five other faces carry
another, with TrenchBroom's Valve axes and the editor's float round trip (R3.10);
`feed::patchDef3Sphere` a sphere of 9 × 17 control points and 8 × 8 subdivisions (2,145
vertices, within K19's bounds); `feed::pointLight` a light entity and node (one id for
both). Materials are named as a map names them, without `textures/`. Without a `mapPath`
no bake is read (K35). `lit::snapshotOfMapText( text, generation, sequence )` reads a map
in TrenchBroom's Doom 3 Valve 220 format; its brush polygons are exact for axial brushes
of float-exact coordinates and carry the rounding of a double-precision plane solve
otherwise (its header states the rule); a malformed text is an error with the line.

## 5. Image overrides

`service->setImageOverride( imageName, ImageOverride )` hands over an image in place of a
game image's files; `clearImageOverride( imageName )` ends it.

- **The name** is matched against the **whole** name of every image key as the engine's
  `ImageFromFile` folds names: every `.tga`, `.jpg`, `.png`, `.exr`, `.hdr` removed wherever
  it stands (case-sensitively), backslashes made slashes, then compared without case. So
  `textures/foo` feeds a stage `diffusemap textures/foo.tga`; a **program** key matches only
  its whole program text — an override of `textures/foo_local` does not feed `bumpmap
  addnormals( textures/foo_local, textures/foo_h )`. Names starting with `_` (the engine's
  built-ins), `""` and `"default"` are refused at once.
- **The usage** is the key's, as the copied time call gives it: the stage kind (diffuse,
  bump, specular), and for a specular stage the PBR RMAO usage only when its image name
  contains `_rmao` (Material.cpp:2278-2290). The engine's rename of an `_s` specular to its
  `_rmao` sibling needs the sibling file: **a fileless host names `<x>_rmao` in the
  material itself**, as `test/fixture/lit/materials/lp5_feed.mtr` does.
- **Raw pixels** (`RawImagePixels`): width × height texels a face, **rows top first**, four
  bytes a texel in R, G, B, A order, **alpha 255 where the source has none** (the picture
  the engine's `LoadTGA` returns after its flip); one face, or six for a cube key in
  `R_LoadCubeImages`' order (`_px`, `_nx`, `_py`, `_ny`, `_pz`, `_nz`). `RGBA8` for every
  usage but the HDR ones; `RGBA32F` only for `TD_HDRI` and `TD_HDR_LIGHTPROBE`. A cross
  pair is refused. **The TGA equivalence (D110):** the same pixels written as a loose
  32-bit uncompressed TGA (type 2, no flip bit), rows bottom first and each texel B, G,
  R, A, load into exactly these rows (`LoadTGA` reads the file's rows from the last
  row up, Image_files.cpp:259-302, and flips only for bit 5 of the attributes,
  :414-420); the `[.][feed]` test shows the two give byte-identical pictures.
- **GPU-ready levels** (a `CachedImage`, as a `.bimage` reads into): the usage's format or
  its uncompressed twin, the usage's colour format, six faces exactly for a cube.
- **The cap (D103, Q27):** a level whose bytes over all its faces exceed 32 MB (the render
  side's upload per frame) is refused: a raw RGBA8 2D image above 2896 texels a side, a
  cube above 1182. Hand larger images as GPU-ready levels or downsampled.
- **`editorSize`** (D111, K36): the editor-image size of the name; when it changes a
  material's size, every material whose image chain holds the name is measured again.
- **The outcome** is read from `imageOverrideStatus( name )`: Pending, Applied, Refused
  with the reason, None after a clear. `setImageOverride` fails only when the service's
  thread has stopped.
- **A storm** (a painting host): one queued item per name, the newest hand-over winning
  (older bytes dropped, `stats().droppedFeeds`); every item publishes one table, and the
  session draws at most one frame per 33 ms (the frame pacer). While hand-overs keep
  coming, no frame is **settled** (section 7). The old texture stays bound until the new
  one has every level: a frame shows the old image or the new one, never a placeholder.
- The frame log (`SessionOptions::frameLogPath`) has three columns for it: 46
  `feed_applied`, 47 `feed_ms` (hand-over to the first frame with the image resident, ms,
  three decimals; at most 32 records per publication), 48 `override_bytes`.

## 6. Re-parses

`service->reparseMaterial( name, text )` parses one material's decl again in place,
`text` being the decl body with or without its `material <name>` head, at most 1 MB;
`clearMaterialReparse( name )` restores the file's text. A guard refusal or an engine
error publishes the material as Failed with the reason (it is not drawn until a later
re-parse succeeds). The text is applied again after every registry rebuild. **Re-parse
existing names only:** a name without a decl stays implicit, and a light whose `texture`
names it gets the default light shader.

## 7. Pixels

`session->readbackLdr( timeout )` returns the LDR target (`RGBA8`, after the tonemap,
before the blit) of the first **settled** frame after the call: `LdrImage{ width, height,
rgba }` at the session's target extent, **rows top first**, four bytes a pixel (alpha as
the tonemap pass writes it; the engine's screenshots force 255). Settled means nothing is
pending: pipelines, materials, images and their levels, chunks, lights, **and every
override and re-parse handed over before the frame's convergence is in its table**. So a
readback after `setImageOverride` shows the override (or its refusal) without waiting for
anything else — and **a host that never stops feeding never gets a settled frame**: the
readback returns the timeout's error. The render thread keeps one staging texture per
extent. A presenting host gets frames on its surface; on `SessionEvent::SwapchainOutOfDate`
its window thread parks the render thread, destroys and creates the swapchain, and
unparks it (`LitSession.h`, "Surface handshake").

## 8. The teardown

`feed::teardown( std::move( session ), std::move( service ), std::move( instance ),
listener.get(), surface, pump )` runs the editor's steps in its order (C18; plan §3.1,
`LitSessionHost::runTeardownThrough`): the listener detached (no `tableChanged()` after
it returns); the parse thread asked to stop; the render thread parked (with a surface,
calling `pump` — the host's window message pump — every 10 ms), stopped and joined, the
CPU pool and the compile thread with it; every GPU object, the swapchain and the NVRHI
device released; the host's surface destroyed while the instance lives; the VkDevice, the
session, the instance, then `resetDispatcher()`; last, the material registry destroyed on
its thread and the thread joined. The host's window may go after it. It returns the
session's `TeardownReport` (a release count other than 0, NVRHI errors or a lost device
are worth logging).

## 9. What a host must not do

- Write anything under the game folder (C8): an override is memory only.
- Touch the copied engine code (C20): only the service's parse thread runs it.
- Feed the engine's or the preview's intrinsic names (`_white`, `_flat`, …).
- Expect the `_s` → `_rmao` rename without the sibling file, or a program key to match by
  one of its leaf names.
- Destroy the session, the service or the instance in another order than section 8's, or
  let a listener call into a session it has not detached.
- Run two sessions in one process.

C9's note: no SinkBroom path constructs a `VulkanInstance`; the editor's instance is Qt's
`QVulkanInstance`. The class is for hosts outside SinkBroom and for the tests.

## 10. Examples

Both are `tst_Feed.cpp`'s, verbatim; `gamePath` holds the shader blobs, `declPath` the
materials (for a host the same game folder; the test's temp root holds the fixture's
`lp5_feed.mtr`, the default light shaders and a copy of the game's `def/misc.def`).
`must` throws a refused step's reason; `| kdl::value()` throws too.

### Example 1: a quad, a sphere and a light from the builders

```cpp
Result<LdrImage> feedExampleRecords(
  const std::filesystem::path& gamePath, const std::filesystem::path& declPath)
{
  using namespace std::chrono_literals;

  // 1. the instance and the dispatcher
  auto instance = VulkanInstance::create({}) | kdl::value();
  initInstanceDispatcher(instance.handles());

  // 2. the listener, then the material service over the game folder
  auto listener = std::make_shared<feed::GuardedListener>();
  auto serviceOptions = MaterialServiceOptions{};
  serviceOptions.makeSource = [declPath](const std::atomic<bool>& cancel) {
    return std::unique_ptr<DeclSource>{
      std::make_unique<GameDeclSource>(declPath, &cancel)};
  };
  serviceOptions.parseAll = true;
  serviceOptions.listener = feed::GuardedListener::listener(listener);
  auto service = MaterialService::create(std::move(serviceOptions)) | kdl::value();

  // 3. the session: headless (a null surface), the blobs from the game path
  auto sessionOptions = SessionOptions{};
  sessionOptions.gamePath = gamePath;
  const auto generation = std::uint64_t{1};
  auto session = LitSession::create(
                   instance.handles(),
                   VK_NULL_HANDLE,
                   320,
                   240,
                   sessionOptions,
                   [](SessionEvent, std::string, std::uint64_t) {},
                   generation,
                   service.get())
                 | kdl::value();
  listener->attach(session.get());
  session->setExposed(true);

  // 4. the first snapshot: worldspawn first, then its geometry and the light
  auto snapshot = Snapshot{};
  snapshot.generation = generation;
  snapshot.sequence = 1;
  feed::worldspawn(snapshot, 1);
  must(feed::quadBrush(
    snapshot,
    1,
    2,
    {0, 0, 0},
    {0, -1, 0},
    512,
    "lp5/feed_pbr",
    "lp5/feed_side",
    1.0,
    {512, 512}));
  must(feed::patchDef3Sphere(snapshot, 1, 3, {300, -300, -200}, 128, "lp5/feed_sphere"));
  feed::pointLight(snapshot, 4, {0, -500, 0}, 1200, {1, 1, 1});
  const auto shared = std::make_shared<const Snapshot>(std::move(snapshot));
  while (!session->offerSnapshot(shared))
  {
    std::this_thread::sleep_for(1ms);
  }

  // 5. the camera
  session->setCamera(feed::cameraLookingAt({-600, -900, 400}, {0, 0, 0}));

  // 6. an image under its game image name: 1024², rows top first, RGBA8
  auto pixels = RawImagePixels{};
  pixels.width = 1024;
  pixels.height = 1024;
  pixels.bytes.resize(std::size_t{1024} * 1024 * 4);
  for (std::size_t i = 0; i < pixels.bytes.size(); i += 4)
  {
    pixels.bytes[i] = 200;
    pixels.bytes[i + 1] = 120;
    pixels.bytes[i + 2] = 40;
    pixels.bytes[i + 3] = 255;
  }
  auto image = ImageOverride{};
  image.image = std::move(pixels);
  image.editorSize = EditorImageSize{1024, 1024, EditorImageSize::State::Ready, {}};
  must(service->setImageOverride("textures/lp5/feed_pbr", std::move(image)));

  // 7. the picture of the first settled frame that holds the override
  auto picture = session->readbackLdr(60s);

  // 8. the teardown, in the editor's order
  feed::teardown(
    std::move(session), std::move(service), std::move(instance), listener.get());
  return picture;
}
```

### Example 2: the same kind of scene from a map text, and a re-parse

```cpp
Result<LdrImage> feedExampleMapText(
  const std::filesystem::path& gamePath, const std::filesystem::path& declPath)
{
  using namespace std::chrono_literals;

  const auto mapText = std::string{R"({
"classname" "worldspawn"
{
( -512 0 -512 ) ( -512 0 -511 ) ( -511 0 -512 ) lp5/feed_pbr [ 1 0 0 512 ] [ 0 0 -1 512 ] 0 1 1
( 512 16 512 ) ( 513 16 512 ) ( 512 16 513 ) lp5/feed_side [ 1 0 0 0 ] [ 0 0 -1 0 ] 0 1 1
( -512 0 -512 ) ( -512 1 -512 ) ( -512 0 -511 ) lp5/feed_side [ 0 1 0 0 ] [ 0 0 -1 0 ] 0 1 1
( 512 16 512 ) ( 512 16 513 ) ( 512 17 512 ) lp5/feed_side [ 0 1 0 0 ] [ 0 0 -1 0 ] 0 1 1
( -512 0 -512 ) ( -511 0 -512 ) ( -512 1 -512 ) lp5/feed_side [ 1 0 0 0 ] [ 0 -1 0 0 ] 0 1 1
( 512 16 512 ) ( 512 17 512 ) ( 513 16 512 ) lp5/feed_side [ 1 0 0 0 ] [ 0 -1 0 0 ] 0 1 1
}
{
patchDef3
{
lp5/feed_sphere
( 3 3 4 4 0 0 0 )
(
( ( 200 -300 -300 0 0 ) ( 300 -400 -300 0.5 0 ) ( 400 -300 -300 1 0 ) )
( ( 200 -300 -200 0 0.5 ) ( 300 -400 -200 0.5 0.5 ) ( 400 -300 -200 1 0.5 ) )
( ( 200 -300 -100 0 1 ) ( 300 -400 -100 0.5 1 ) ( 400 -300 -100 1 1 ) )
)
}
}
}
{
"classname" "light"
"origin" "0 -500 0"
"light_radius" "1200 1200 1200"
"_color" "1 1 1"
}
)"};
  const auto generation = std::uint64_t{1};
  auto snapshot = snapshotOfMapText(mapText, generation, 1);
  if (snapshot.is_error())
  {
    return std::get<Error>(snapshot.error());
  }

  auto instance = VulkanInstance::create({}) | kdl::value();
  initInstanceDispatcher(instance.handles());
  auto listener = std::make_shared<feed::GuardedListener>();
  auto serviceOptions = MaterialServiceOptions{};
  serviceOptions.makeSource = [declPath](const std::atomic<bool>& cancel) {
    return std::unique_ptr<DeclSource>{
      std::make_unique<GameDeclSource>(declPath, &cancel)};
  };
  serviceOptions.parseAll = true;
  serviceOptions.listener = feed::GuardedListener::listener(listener);
  auto service = MaterialService::create(std::move(serviceOptions)) | kdl::value();
  auto sessionOptions = SessionOptions{};
  sessionOptions.gamePath = gamePath;
  auto session = LitSession::create(
                   instance.handles(),
                   VK_NULL_HANDLE,
                   320,
                   240,
                   sessionOptions,
                   [](SessionEvent, std::string, std::uint64_t) {},
                   generation,
                   service.get())
                 | kdl::value();
  listener->attach(session.get());
  session->setExposed(true);

  const auto shared = std::make_shared<const Snapshot>(std::move(snapshot).value());
  while (!session->offerSnapshot(shared))
  {
    std::this_thread::sleep_for(1ms);
  }
  session->setCamera(feed::cameraLookingAt({-600, -900, 400}, {0, 0, 0}));

  // the patch's material re-parsed in place: its text, as an .mtr file would hold it
  must(service->reparseMaterial(
    "textures/lp5/feed_sphere",
    "{\n\ttwoSided\n\tbasecolormap\ttextures/lp5/feed_sphere\n}\n"));

  auto picture = session->readbackLdr(60s);
  feed::teardown(
    std::move(session), std::move(service), std::move(instance), listener.get());
  return picture;
}
```
