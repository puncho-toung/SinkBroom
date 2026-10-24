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

#include "lit/FeedHost.h"

#include <fmt/format.h>

#include <utility>

namespace tb::lit::feed
{
namespace
{

using namespace std::chrono_literals;

// as LitSessionHost's ServicingInterval
constexpr auto PumpInterval = 10ms;
// how long a park is tried before the stop goes on without it (LitSessionHost parks
// within ParkTimeout and stops all the same when it does not)
constexpr auto ParkBound = 2000ms;

} // namespace

GuardedListener::GuardedListener(MaterialListener next)
  : m_next{std::move(next)}
{
}

MaterialListener GuardedListener::listener(std::shared_ptr<GuardedListener> self)
{
  return
    [self = std::move(self)](const MaterialServiceEvent& event) { self->handle(event); };
}

void GuardedListener::attach(LitSession* session)
{
  const auto lock = std::scoped_lock{m_mutex};
  m_session = session;
}

void GuardedListener::detach()
{
  // waits for a tableChanged() in progress: it holds the mutex
  const auto lock = std::scoped_lock{m_mutex};
  m_session = nullptr;
}

void GuardedListener::handle(const MaterialServiceEvent& event)
{
  if (event.kind == MaterialServiceEvent::Kind::Published)
  {
    const auto lock = std::scoped_lock{m_mutex};
    ++m_published;
    if (m_session)
    {
      // R2.2: sets a flag under the render thread's mutex and notifies; never blocks on
      // a frame, and never calls back here
      m_session->tableChanged();
      ++m_forwarded;
    }
    else
    {
      ++m_dropped;
    }
  }
  if (m_next)
  {
    m_next(event);
  }
}

GuardedListener::Counts GuardedListener::counts() const
{
  const auto lock = std::scoped_lock{m_mutex};
  return {m_published, m_forwarded, m_dropped};
}

TeardownReport teardown(
  std::unique_ptr<LitSession> session,
  std::unique_ptr<MaterialService> service,
  VulkanInstance instance,
  GuardedListener* listener,
  const VkSurfaceKHR surface,
  const std::function<void()>& pump)
{
  const auto pumpOnce = [&] {
    if (pump)
    {
      pump();
    }
  };

  // P5.12: no tableChanged() reaches the session from here on
  if (listener)
  {
    listener->detach();
  }

  // step 1 (LitSessionHost: TeardownStep::RenderThreadStopped): the parse thread first,
  // so that its scan or read stops while the render thread is stopped
  if (service)
  {
    service->requestStop();
  }
  if (session)
  {
    // what the park finds goes into the report (LitSessionHost::stopRenderThread)
    session->beginStop();
    if (surface != VK_NULL_HANDLE)
    {
      const auto deadline = std::chrono::steady_clock::now() + ParkBound;
      while (!session->park(PumpInterval) && std::chrono::steady_clock::now() < deadline)
      {
        pumpOnce();
      }
    }
    session->requestStop();
    while (!session->waitForRenderThread(PumpInterval))
    {
      pumpOnce();
    }
    session->stopRenderThread();
    // steps 2-5 (GpuObjectsReleased)
    session->releaseGpuObjects();
  }

  // step 6 (ContainerDeleted): the surface while the instance lives
  if (surface != VK_NULL_HANDLE && instance.valid())
  {
    if (
      const auto destroySurface =
        instance.get<PFN_vkDestroySurfaceKHR>("vkDestroySurfaceKHR"))
    {
      destroySurface(instance.instance(), surface, nullptr);
    }
  }

  // step 7 (VkDeviceDestroyed, InstanceDestroyed, DispatcherReset)
  auto report = TeardownReport{};
  if (session)
  {
    session->destroyVkDevice();
    report = session->teardownReport();
    session.reset();
  }
  instance.destroy();
  if (!resetDispatcher())
  {
    report.messages.emplace_back("The Vulkan dispatcher was still in use at the stop.");
  }

  // step 8 (MaterialsDestroyed): the registry destroyed on its thread, then the join
  if (service)
  {
    service->join();
    service.reset();
  }
  return report;
}

VulkanMessageSink vulkanMessagesAsWarnings(EventSink sink, const std::uint64_t generation)
{
  return [sink = std::move(sink), generation](const VulkanMessage& message) {
    if (sink)
    {
      sink(
        SessionEvent::Warning,
        fmt::format(
          "Vulkan validation {} [{}]: {}",
          message.error ? "error" : "warning",
          message.id,
          message.text),
        generation);
    }
  };
}

} // namespace tb::lit::feed
