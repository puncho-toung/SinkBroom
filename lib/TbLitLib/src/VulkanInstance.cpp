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

#include "lit/VulkanInstance.h"

#include "LiveSession.h"

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <vulkan/vulkan.hpp>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <atomic>
#include <string_view>
#include <utility>

namespace tb::lit
{
namespace
{

// constant-initialised; no session exists before the first Start (C9)
constinit auto liveSession = std::atomic<bool>{false};

constexpr auto ValidationLayerName = std::string_view{"VK_LAYER_KHRONOS_validation"};

// the platform's loader (D107, P5.11)
const char* defaultLoaderName()
{
#if defined(_WIN32)
  return "vulkan-1.dll";
#elif defined(__APPLE__)
  return "libvulkan.1.dylib";
#else
  return "libvulkan.so.1";
#endif
}

// the platform's surface extensions besides VK_KHR_surface: every one is asked for, and
// at least one must be offered (Linux offers some of the three, by its window systems)
std::vector<std::string_view> platformSurfaceExtensions()
{
#if defined(_WIN32)
  return {"VK_KHR_win32_surface"};
#elif defined(__APPLE__)
  return {"VK_EXT_metal_surface"};
#else
  return {"VK_KHR_xcb_surface", "VK_KHR_xlib_surface", "VK_KHR_wayland_surface"};
#endif
}

// the loaded library: a module handle on Windows, a dlopen handle elsewhere
struct Library
{
  void* handle = nullptr;
  PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
};

Result<Library> loadLibrary(const std::string& name)
{
  auto library = Library{};
#ifdef _WIN32
  // the name is UTF-8; LoadLibraryW takes UTF-16
  const auto wideLength =
    MultiByteToWideChar(CP_UTF8, 0, name.data(), int(name.size()), nullptr, 0);
  auto wide = std::wstring(std::size_t(std::max(wideLength, 0)), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, name.data(), int(name.size()), wide.data(), wideLength);
  auto* module = LoadLibraryW(wide.c_str());
  if (!module)
  {
    return Error{fmt::format(
      "Cannot load the Vulkan loader {} (Windows error {}).", name, GetLastError())};
  }
  library.handle = static_cast<void*>(module);
  library.getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
    reinterpret_cast<void*>(GetProcAddress(module, "vkGetInstanceProcAddr")));
  if (!library.getInstanceProcAddr)
  {
    FreeLibrary(module);
    return Error{fmt::format("The Vulkan loader {} has no vkGetInstanceProcAddr.", name)};
  }
#else
  library.handle = dlopen(name.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!library.handle)
  {
    const auto* reason = dlerror();
    return Error{fmt::format(
      "Cannot load the Vulkan loader {} ({}).", name, reason ? reason : "dlopen failed")};
  }
  library.getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
    dlsym(library.handle, "vkGetInstanceProcAddr"));
  if (!library.getInstanceProcAddr)
  {
    dlclose(library.handle);
    return Error{fmt::format("The Vulkan loader {} has no vkGetInstanceProcAddr.", name)};
  }
#endif
  return library;
}

void freeLibrary(void* handle)
{
  if (!handle)
  {
    return;
  }
#ifdef _WIN32
  FreeLibrary(static_cast<HMODULE>(handle));
#else
  dlclose(handle);
#endif
}

bool listed(
  const std::vector<VkExtensionProperties>& extensions, const std::string_view name)
{
  return std::ranges::any_of(
    extensions, [&](const auto& e) { return std::string_view{e.extensionName} == name; });
}

} // namespace

bool claimLiveSession()
{
  auto expected = false;
  return liveSession.compare_exchange_strong(expected, true);
}

void releaseLiveSession()
{
  liveSession = false;
}

bool liveSessionExists()
{
  return liveSession;
}

bool initInstanceDispatcher(const InstanceHandles& handles)
{
  if (liveSessionExists())
  {
    return false;
  }
  if (handles.instance != VK_NULL_HANDLE && handles.getInstanceProcAddr != nullptr)
  {
    VULKAN_HPP_DEFAULT_DISPATCHER.init(handles.instance, handles.getInstanceProcAddr);
  }
  return true;
}

bool resetDispatcher()
{
  if (liveSessionExists())
  {
    return false;
  }
  VULKAN_HPP_DEFAULT_DISPATCHER = vk::detail::DispatchLoaderDynamic{};
  return true;
}

// what a VulkanInstance owns; on the heap, so that the messenger's user data stays put
// when the object moves
struct VulkanInstance::State
{
  Library library;
  VkInstance instance = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
  std::vector<std::string> extensions;
  bool validationLayer = false;
  bool messageLimitRaised = false;
  VulkanMessageSink sink;

  State() = default;
  State(const State&) = delete;
  State& operator=(const State&) = delete;
  ~State() { destroy(); }

  template <typename F>
  F get(const VkInstance owner, const char* name) const
  {
    return reinterpret_cast<F>(library.getInstanceProcAddr(owner, name));
  }

  // the reverse of create: the messenger, the instance, the loader
  void destroy()
  {
    if (messenger != VK_NULL_HANDLE)
    {
      if (
        const auto destroyMessenger = get<PFN_vkDestroyDebugUtilsMessengerEXT>(
          instance, "vkDestroyDebugUtilsMessengerEXT"))
      {
        destroyMessenger(instance, messenger, nullptr);
      }
      messenger = VK_NULL_HANDLE;
    }
    if (instance != VK_NULL_HANDLE)
    {
      if (
        const auto destroyInstance =
          get<PFN_vkDestroyInstance>(instance, "vkDestroyInstance"))
      {
        destroyInstance(instance, nullptr);
      }
      instance = VK_NULL_HANDLE;
    }
    freeLibrary(std::exchange(library.handle, nullptr));
    library.getInstanceProcAddr = nullptr;
  }

  static VKAPI_ATTR VkBool32 VKAPI_CALL callback(
    const VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* user)
  {
    const auto* self = static_cast<const State*>(user);
    if (!self || !self->sink)
    {
      return VK_FALSE;
    }
    // never through the driver's frames
    try
    {
      auto message = VulkanMessage{};
      message.error = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
      message.id = data && data->pMessageIdName ? data->pMessageIdName : "";
      message.idNumber = data ? data->messageIdNumber : 0;
      message.text = data && data->pMessage ? data->pMessage : "";
      self->sink(message);
    }
    catch (...)
    {
    }
    return VK_FALSE;
  }
};

VulkanInstance::VulkanInstance(std::unique_ptr<State> state)
  : m_state{std::move(state)}
{
}

VulkanInstance::VulkanInstance(VulkanInstance&& other) noexcept = default;
VulkanInstance& VulkanInstance::operator=(VulkanInstance&& other) noexcept = default;
VulkanInstance::~VulkanInstance() = default;

Result<VulkanInstance> VulkanInstance::create(VulkanInstanceOptions options)
{
  const auto loaderName =
    options.loaderName.empty() ? std::string{defaultLoaderName()} : options.loaderName;
  auto state = std::make_unique<State>();
  state->sink = std::move(options.messages);

  return loadLibrary(loaderName) | kdl::and_then([&](Library library) -> Result<void> {
           state->library = library;

           // vkEnumerateInstanceVersion is 1.1's; a 1.0 loader has none
           auto version = std::uint32_t{VK_API_VERSION_1_0};
           if (
             const auto enumerateVersion = state->get<PFN_vkEnumerateInstanceVersion>(
               VK_NULL_HANDLE, "vkEnumerateInstanceVersion"))
           {
             enumerateVersion(&version);
           }
           if (version < VK_API_VERSION_1_2)
           {
             return Error{fmt::format(
               "The Vulkan loader {} offers Vulkan {}.{}; the lit preview needs 1.2.",
               loaderName,
               VK_API_VERSION_MAJOR(version),
               VK_API_VERSION_MINOR(version))};
           }

           const auto enumerateExtensions =
             state->get<PFN_vkEnumerateInstanceExtensionProperties>(
               VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
           const auto enumerateLayers =
             state->get<PFN_vkEnumerateInstanceLayerProperties>(
               VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties");
           const auto createInstance =
             state->get<PFN_vkCreateInstance>(VK_NULL_HANDLE, "vkCreateInstance");
           if (!enumerateExtensions || !enumerateLayers || !createInstance)
           {
             return Error{fmt::format(
               "The Vulkan loader {} lacks the global functions of Vulkan 1.0.",
               loaderName)};
           }

           auto count = std::uint32_t{0};
           enumerateExtensions(nullptr, &count, nullptr);
           auto supported = std::vector<VkExtensionProperties>(count);
           enumerateExtensions(nullptr, &count, supported.data());
           supported.resize(count);

           auto enabled = std::vector<std::string>{};
           const auto enable = [&](const std::string_view name) {
             if (listed(supported, name))
             {
               enabled.emplace_back(name);
               return true;
             }
             return false;
           };

           if (options.surface)
           {
             if (!enable(VK_KHR_SURFACE_EXTENSION_NAME))
             {
               return Error{fmt::format(
                 "The Vulkan loader {} offers no {}.",
                 loaderName,
                 VK_KHR_SURFACE_EXTENSION_NAME)};
             }
             auto platform = false;
             for (const auto name : platformSurfaceExtensions())
             {
               platform = enable(name) || platform;
             }
             if (!platform)
             {
               return Error{fmt::format(
                 "The Vulkan loader {} offers none of this platform's surface extensions "
                 "({}).",
                 loaderName,
                 fmt::join(platformSurfaceExtensions(), ", "))};
             }
           }

           auto flags = VkInstanceCreateFlags{0};
#if defined(__APPLE__)
           // MoltenVK is a portability implementation: without this the 1.3 loaders
           // enumerate no device (DeviceManager_VK::createInstance does the same)
           if (enable(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
           {
             flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
           }
#endif

           auto layers = std::vector<const char*>{};
           auto useMessenger = false;
           if (options.validation)
           {
             useMessenger = enable(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
             enumerateLayers(&count, nullptr);
             auto available = std::vector<VkLayerProperties>(count);
             enumerateLayers(&count, available.data());
             available.resize(count);
             // R-B16: only a layer that is installed
             if (std::ranges::any_of(available, [](const auto& l) {
                   return std::string_view{l.layerName} == ValidationLayerName;
                 }))
             {
               layers.push_back(ValidationLayerName.data());
               state->validationLayer = true;
             }
           }

           // the layer's own setting of how many messages of one id it reports
           auto limit = options.duplicateMessageLimit.value_or(0);
           auto setting = VkLayerSettingEXT{};
           setting.pLayerName = ValidationLayerName.data();
           setting.pSettingName = "duplicate_message_limit";
           setting.type = VK_LAYER_SETTING_TYPE_UINT32_EXT;
           setting.valueCount = 1;
           setting.pValues = &limit;
           auto layerSettings = VkLayerSettingsCreateInfoEXT{};
           layerSettings.sType = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT;
           layerSettings.settingCount = 1;
           layerSettings.pSettings = &setting;
           auto layerSettingsEnabled = false;
           if (state->validationLayer && options.duplicateMessageLimit)
           {
             enumerateExtensions(ValidationLayerName.data(), &count, nullptr);
             auto layerExtensions = std::vector<VkExtensionProperties>(count);
             enumerateExtensions(
               ValidationLayerName.data(), &count, layerExtensions.data());
             layerExtensions.resize(count);
             if (listed(layerExtensions, VK_EXT_LAYER_SETTINGS_EXTENSION_NAME))
             {
               enabled.emplace_back(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
               layerSettingsEnabled = true;
             }
           }

           auto messengerInfo = VkDebugUtilsMessengerCreateInfoEXT{};
           messengerInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
           messengerInfo.messageSeverity =
             VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
             | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
           messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                                       | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                                       | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
           messengerInfo.pfnUserCallback = &State::callback;
           messengerInfo.pUserData = state.get();

           auto application = VkApplicationInfo{};
           application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
           application.pApplicationName = options.applicationName.c_str();
           application.apiVersion = VK_API_VERSION_1_2;

           auto names = std::vector<const char*>{};
           for (const auto& name : enabled)
           {
             names.push_back(name.c_str());
           }
           auto info = VkInstanceCreateInfo{};
           info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
           // the messenger in the chain also reports vkCreateInstance and
           // vkDestroyInstance themselves
           info.pNext = useMessenger ? &messengerInfo : nullptr;
           if (layerSettingsEnabled)
           {
             layerSettings.pNext = info.pNext;
             info.pNext = &layerSettings;
           }
           info.flags = flags;
           info.pApplicationInfo = &application;
           info.enabledLayerCount = std::uint32_t(layers.size());
           info.ppEnabledLayerNames = layers.data();
           info.enabledExtensionCount = std::uint32_t(names.size());
           info.ppEnabledExtensionNames = names.data();
           if (const auto result = createInstance(&info, nullptr, &state->instance);
               result != VK_SUCCESS)
           {
             state->instance = VK_NULL_HANDLE;
             return Error{fmt::format(
               "Cannot create a Vulkan 1.2 instance (VkResult {}).", int(result))};
           }
           state->extensions = std::move(enabled);
           state->messageLimitRaised = layerSettingsEnabled;

           if (useMessenger)
           {
             if (
               const auto createMessenger =
                 state->get<PFN_vkCreateDebugUtilsMessengerEXT>(
                   state->instance, "vkCreateDebugUtilsMessengerEXT"))
             {
               createMessenger(
                 state->instance, &messengerInfo, nullptr, &state->messenger);
             }
           }
           return kdl::void_success;
         })
         | kdl::transform([&]() { return VulkanInstance{std::move(state)}; });
}

bool VulkanInstance::valid() const
{
  return m_state && m_state->instance != VK_NULL_HANDLE;
}

InstanceHandles VulkanInstance::handles() const
{
  if (!valid())
  {
    return {};
  }
  return {m_state->instance, m_state->library.getInstanceProcAddr, m_state->extensions};
}

VkInstance VulkanInstance::instance() const
{
  return valid() ? m_state->instance : VK_NULL_HANDLE;
}

std::vector<std::string> VulkanInstance::extensions() const
{
  return valid() ? m_state->extensions : std::vector<std::string>{};
}

bool VulkanInstance::validationLayer() const
{
  return valid() && m_state->validationLayer;
}

bool VulkanInstance::messenger() const
{
  return valid() && m_state->messenger != VK_NULL_HANDLE;
}

bool VulkanInstance::messageLimitRaised() const
{
  return valid() && m_state->messageLimitRaised;
}

PFN_vkVoidFunction VulkanInstance::procAddr(const char* name) const
{
  return valid() ? m_state->library.getInstanceProcAddr(m_state->instance, name)
                 : nullptr;
}

void VulkanInstance::destroy()
{
  if (m_state)
  {
    m_state->destroy();
    m_state.reset();
  }
}

} // namespace tb::lit
