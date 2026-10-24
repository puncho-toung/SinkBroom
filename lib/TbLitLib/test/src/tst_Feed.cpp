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

// LPF.2 (D107, D108, D110): the Qt-free host API on this machine's GPU. Hidden
// ("[.][feed]", its own tag, never in "[.][litgpu]"): it needs a Vulkan driver and, for
// the lit sections, the game folder's shader blobs and light entityDef (TB_LIT_GAME_PATH,
// read only). Windows only, as every GPU case.

#include "lit/VulkanInstance.h"

#include "kd/result.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#ifdef _WIN32

namespace tb::lit
{

TEST_CASE("Feed host: lit::VulkanInstance (LPF.2, D107)", "[.][feed]")
{
  SECTION("the message sink gets what the messenger reports")
  {
    // a message submitted through VK_EXT_debug_utils reaches the product's sink, as the
    // validation layer's do (the D71 case's intentional OutputNotConsumed warnings)
    auto mutex = std::mutex{};
    auto received = std::vector<VulkanMessage>{};
    for (const auto validation : {false, true})
    {
      CAPTURE(validation);
      {
        const auto lock = std::scoped_lock{mutex};
        received.clear();
      }
      auto options = VulkanInstanceOptions{};
      options.validation = validation;
      options.messages = [&](const VulkanMessage& message) {
        const auto lock = std::scoped_lock{mutex};
        received.push_back(message);
      };
      auto instance = VulkanInstance::create(std::move(options))
                      | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
      REQUIRE(instance.valid());
      CHECK(instance.handles().instance == instance.instance());
      CHECK(instance.handles().getInstanceProcAddr != nullptr);
      if (!validation)
      {
        // no messenger, no layer: nothing to report through
        CHECK_FALSE(instance.messenger());
        CHECK_FALSE(instance.validationLayer());
        continue;
      }
      REQUIRE(instance.messenger());
      CHECK(instance.validationLayer());
      const auto submit =
        instance.get<PFN_vkSubmitDebugUtilsMessageEXT>("vkSubmitDebugUtilsMessageEXT");
      REQUIRE(submit != nullptr);
      auto data = VkDebugUtilsMessengerCallbackDataEXT{};
      data.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT;
      data.pMessageIdName = "LPF2-feed-host";
      data.messageIdNumber = 4242;
      data.pMessage = "an intentional message";
      submit(
        instance.instance(),
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
        &data);
      submit(
        instance.instance(),
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,
        &data);
      const auto lock = std::scoped_lock{mutex};
      std::erase_if(
        received, [](const auto& message) { return message.id != "LPF2-feed-host"; });
      REQUIRE(received.size() == 2u);
      CHECK(received[0].error);
      CHECK(received[0].idNumber == 4242);
      CHECK(received[0].text == "an intentional message");
      CHECK_FALSE(received[1].error);
    }
  }

  SECTION("destroy runs once, and a moved-from instance is none")
  {
    auto instance = VulkanInstance::create({})
                    | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
    auto moved = std::move(instance);
    CHECK_FALSE(instance.valid()); // NOLINT(bugprone-use-after-move)
    CHECK(instance.handles().instance == VK_NULL_HANDLE);
    CHECK(moved.valid());
    CHECK(moved.extensions().empty());
    moved.destroy();
    moved.destroy();
    CHECK_FALSE(moved.valid());
    CHECK(moved.procAddr("vkCreateDevice") == nullptr);
  }
}

} // namespace tb::lit

#endif
