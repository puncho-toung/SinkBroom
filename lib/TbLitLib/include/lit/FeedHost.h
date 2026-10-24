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

// LPF.2 (D107, P5.12, C18): what a host outside SinkBroom needs besides the instance
// (lit/VulkanInstance.h) and the records (lit/FeedScene.h) to run a session the way the
// editor's LitSessionHost (TbUiLib) does: the material service's listener that tells the
// session about new tables without a GUI event loop, the teardown in the editor's order,
// and the instance's messages as session warnings. The contract is lib/TbLitLib/FEED.md.

#include "lit/LitSession.h"
#include "lit/MaterialService.h"
#include "lit/VulkanInstance.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

namespace tb::lit::feed
{

/**
 * LPF.2 (D101, P5.12): the material service's listener for a host without an event
 * loop. Every Published event calls the attached session's tableChanged() on the parse
 * thread (the editor posts it through its GUI queue instead, LitSessionHost.cpp's
 * MaterialsPublished path); every event, Published or Message, then goes to next, the
 * host's own handler (logging), outside the lock. The session pointer lives under a
 * mutex that the call holds, so detach() returns only once no call is in progress and
 * none follows: a host detaches before it stops the render thread (teardown below), and
 * the service may go on publishing (a Feed item in flight, a re-parse) while the session
 * is torn down. Make it with std::make_shared before the service, hand
 * listener( shared ) to MaterialServiceOptions::listener (which keeps it alive), and
 * attach the session once LitSession::create returned it. Any thread.
 */
class GuardedListener
{
private:
  mutable std::mutex m_mutex;
  LitSession* m_session = nullptr;
  MaterialListener m_next;
  std::uint64_t m_published = 0;
  std::uint64_t m_forwarded = 0;
  std::uint64_t m_dropped = 0;

public:
  explicit GuardedListener(MaterialListener next = {});

  // the listener for MaterialServiceOptions::listener, holding self
  static MaterialListener listener(std::shared_ptr<GuardedListener> self);

  void attach(LitSession* session);
  // after it returns, no tableChanged() is in progress or follows
  void detach();
  void handle(const MaterialServiceEvent& event);

  struct Counts
  {
    // Published events received; those handed to a session; those after detach
    std::uint64_t published = 0;
    std::uint64_t forwarded = 0;
    std::uint64_t dropped = 0;
  };
  Counts counts() const;
};

/**
 * LPF.2 (D107, C18, P5.12): a session's teardown in the order of the editor's
 * LitSessionHost::runTeardownThrough (LitSessionHost.cpp, plan §3.1's steps; the
 * document observer, which a host without a document has not, left out): the guarded
 * listener detached (no tableChanged() reaches the session from here on); step 1, the
 * parse thread asked to stop (its current decl file or material ends first, R5.20), then
 * the render thread: with a surface it is parked first, pumping (the host's window
 * messages, may be empty) every 10 ms, so that it is not inside a present that waits for
 * this thread's messages, then stopped and joined (pumping while it is waited for), the
 * CPU pool and the compile thread with it; step 2-5, every GPU object released, the
 * swapchain and the NVRHI device; step 6, the host's VkSurfaceKHR destroyed while the
 * instance lives (the editor deletes its window container there); step 7, the VkDevice,
 * the session object, then the instance (VulkanInstance::destroy) and the dispatcher
 * (resetDispatcher, after the session that held it is gone); step 8, the material
 * registry destroyed on its thread and the thread joined. Any of session, service,
 * listener may be null (a create that failed). The session's report, complete (a
 * dispatcher that could not be reset adds a message). The thread that created the
 * session; it blocks until every thread has ended.
 */
TeardownReport teardown(
  std::unique_ptr<LitSession> session,
  std::unique_ptr<MaterialService> service,
  VulkanInstance instance,
  GuardedListener* listener,
  VkSurfaceKHR surface = VK_NULL_HANDLE,
  const std::function<void()>& pump = {});

/**
 * LPF.2 (D107): the instance's messages as the session's warnings: every VulkanMessage
 * goes to sink as SessionEvent::Warning ("Vulkan validation error|warning [id]: text")
 * with generation, the channel the session reports NVRHI's warnings on
 * (GpuDevice's message callback). For VulkanInstanceOptions::messages.
 */
VulkanMessageSink vulkanMessagesAsWarnings(EventSink sink, std::uint64_t generation);

} // namespace tb::lit::feed
