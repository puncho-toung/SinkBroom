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

#include "base/Result.h"

#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tb::lit
{

/**
 * The Vulkan instance a session runs on. The instance belongs to its creator (Qt's
 * QVulkanInstance, a VulkanInstance below, or a test), which loaded the Vulkan library at
 * runtime (C9) and destroys the instance after the session's last teardown step.
 */
struct InstanceHandles
{
  VkInstance instance = VK_NULL_HANDLE;
  PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
  // the instance extensions it was created with (QVulkanInstance::extensions()); NVRHI
  // uses VK_EXT_debug_utils for object names and markers only when it is listed here
  std::vector<std::string> enabledExtensions;
};

/**
 * Loads vulkan.hpp's default dispatcher for this instance; done on every Start
 * (R4-2.22). False, and nothing loaded, while a session is live: the dispatcher is
 * process-wide, so only one session exists at a time.
 */
bool initInstanceDispatcher(const InstanceHandles& handles);

/**
 * Clears the default dispatcher back to null pointers, after the instance is destroyed.
 * False, and nothing cleared, while a session is live.
 */
bool resetDispatcher();

/**
 * LPF.2 (D107, P5.11): one message of the validation layer or of the loader, as the
 * instance's VK_EXT_debug_utils messenger hands it over (warnings and errors of the
 * general, validation and performance types).
 */
struct VulkanMessage
{
  // VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT; otherwise a warning
  bool error = false;
  // pMessageIdName ("" when the layer gives none) and messageIdNumber
  std::string id;
  std::int32_t idNumber = 0;
  // pMessage
  std::string text;
};

/**
 * Called on the thread that made the Vulkan call the message is about (the render
 * thread, the compile thread, the GUI thread), so it must be safe to call from any
 * thread and must not block; an exception it throws is swallowed. lit/FeedHost.h's
 * vulkanMessagesAsWarnings hands every message to a session's EventSink as a Warning.
 */
using VulkanMessageSink = std::function<void(const VulkanMessage&)>;

struct VulkanInstanceOptions
{
  // VK_LAYER_KHRONOS_validation when the loader lists it (R-B16), with VK_EXT_debug_utils
  // and its messenger when the loader offers that extension
  bool validation = false;
  // the platform's surface extensions, for a host that presents to a VkSurfaceKHR it
  // makes itself: VK_KHR_surface and VK_KHR_win32_surface (Windows); VK_KHR_surface and
  // each of VK_KHR_xcb_surface, VK_KHR_xlib_surface, VK_KHR_wayland_surface the loader
  // offers, at least one (Linux and the other Unix systems); VK_KHR_surface and
  // VK_EXT_metal_surface (macOS). A headless host leaves it false.
  bool surface = false;
  // the most messages of one id the validation layer reports, through the layer's own
  // VK_EXT_layer_settings (duplicate_message_limit) when it offers it; unset: the
  // layer's default (10)
  std::optional<std::uint32_t> duplicateMessageLimit;
  // where the messenger's messages go; empty: nowhere (the layer still validates)
  VulkanMessageSink messages;
  // the loader's file name; empty: the platform's, vulkan-1.dll (Windows),
  // libvulkan.so.1 (Linux and the other Unix systems), libvulkan.1.dylib (macOS)
  std::string loaderName;
  std::string applicationName = "SinkBroom lit preview host";
};

/**
 * LPF.2 (D107, P5.11, C9, P5.29; mirror.json M-INST-02): a Vulkan instance for a host
 * outside SinkBroom (Qt-free) and for the tests. No SinkBroom path constructs one: the
 * editor's instance is Qt's QVulkanInstance (LitSessionHost, M-INST-01), and the panel's
 * C9 test sees that no Vulkan library is loaded before Start; a VulkanInstance loads the
 * Vulkan loader only inside create.
 *
 * create loads the loader at runtime (LoadLibraryW on Windows, dlopen elsewhere; the
 * executable never links it, C9), asks it for Vulkan 1.2 (vkEnumerateInstanceVersion)
 * and creates a 1.2 instance with the options' extensions and layer; on macOS also
 * VK_KHR_portability_enumeration with VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
 * when the loader offers it, so that MoltenVK is enumerated (the engine's own
 * DeviceManager_VK::createInstance does the same). An error, with the reason, when the
 * loader cannot be loaded, offers no vkGetInstanceProcAddr, offers less than Vulkan 1.2,
 * lacks a surface extension asked for, or refuses the instance (the VkResult). Only
 * Windows is built and run here; the Linux and macOS paths are written from the Vulkan
 * headers and go to the user's checklist.
 *
 * The host passes handles() to initInstanceDispatcher before LitSession::create and calls
 * resetDispatcher after the instance is destroyed (lit/FeedHost.h's teardown does both
 * ends in the order of LitSessionHost). The destructor, or destroy, runs in reverse: the
 * messenger, the instance, then the loader. Movable, not copyable; every call but create
 * belongs to the thread that owns the object, except the message sink (any thread).
 */
class VulkanInstance
{
private:
  struct State;
  std::unique_ptr<State> m_state;

  explicit VulkanInstance(std::unique_ptr<State> state);

public:
  static Result<VulkanInstance> create(VulkanInstanceOptions options);

  VulkanInstance(VulkanInstance&& other) noexcept;
  VulkanInstance& operator=(VulkanInstance&& other) noexcept;
  VulkanInstance(const VulkanInstance&) = delete;
  VulkanInstance& operator=(const VulkanInstance&) = delete;
  ~VulkanInstance();

  // false once destroyed or moved from
  bool valid() const;
  // the instance, the loader's vkGetInstanceProcAddr and the extensions it was made with
  InstanceHandles handles() const;
  VkInstance instance() const;
  std::vector<std::string> extensions() const;
  // the validation layer is on the instance (options.validation and the loader lists it)
  bool validationLayer() const;
  // the messenger exists (options.validation and VK_EXT_debug_utils offered)
  bool messenger() const;
  // the layer reports options.duplicateMessageLimit messages of one id
  bool messageLimitRaised() const;
  // vkGetInstanceProcAddr( instance, name ); null once destroyed
  PFN_vkVoidFunction procAddr(const char* name) const;
  template <typename F>
  F get(const char* name) const
  {
    return reinterpret_cast<F>(procAddr(name));
  }

  // the messenger, the instance and the loader, in that order; every later call answers
  // as for a moved-from object
  void destroy();
};

} // namespace tb::lit
