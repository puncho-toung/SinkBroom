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

#include "lit/FrameLog.h"

#include <fmt/format.h>

#include <system_error>
#include <utility>

namespace tb::lit
{
namespace
{

// rows are written out once this much is buffered (about 600 frames)
constexpr auto WriteThreshold = std::size_t{64 * 1024};


std::string optionalField(
  const std::optional<double>& value, const std::string_view format)
{
  return value ? fmt::format(fmt::runtime(format), *value) : std::string{};
}

std::string_view eventName(const FrameLogEvent event)
{
  switch (event)
  {
  case FrameLogEvent::Start:
    return "start";
  case FrameLogEvent::Frame:
    return "frame";
  case FrameLogEvent::Stop:
    return "stop";
  }
  return "";
}

} // namespace

std::string_view frameLogHeader()
{
  return "session,event,frame,cpu_record_ms,acquire_ms,present_ms,gpu_clear_ms,present_"
         "mode,swapchain_generation,frames_skipped,width,height,vram_used_mb,vram_budget_"
         "mb\n";
}

std::string formatFrameLogRow(const FrameLogRow& row)
{
  const auto vram = fmt::format(
    "{},{}",
    optionalField(row.vramUsedMb, "{:.1f}"),
    optionalField(row.vramBudgetMb, "{:.1f}"));
  if (row.event != FrameLogEvent::Frame)
  {
    return fmt::format("{},{},,,,,,,,,,,{}\n", row.session, eventName(row.event), vram);
  }
  return fmt::format(
    "{},{},{},{:.3f},{:.3f},{:.3f},{},{},{},{},{},{},{}\n",
    row.session,
    eventName(row.event),
    row.frameIndex,
    row.cpuRecordMs,
    row.acquireMs,
    row.presentMs,
    optionalField(row.gpuClearMs, "{:.3f}"),
    row.presentMode,
    row.swapchainGeneration,
    row.framesSkipped,
    row.width,
    row.height,
    vram);
}

std::optional<std::string> FrameLogWriter::open(const std::filesystem::path& path)
{
  close();
  if (path.empty())
  {
    return std::nullopt;
  }

  // appended to, so that a log holds every session written into it (review L3.16)
  auto existing = std::error_code{};
  const auto empty = !std::filesystem::exists(path, existing)
                     || std::filesystem::file_size(path, existing) == 0;

  m_path = path;
  m_file.open(path, std::ios::out | std::ios::app | std::ios::binary);
  if (!m_file)
  {
    return fmt::format("Cannot open the frame log {}", path.string());
  }

  m_buffer = empty ? std::string{frameLogHeader()} : std::string{};
  return std::nullopt;
}

bool FrameLogWriter::isOpen() const
{
  return m_file.is_open();
}

void FrameLogWriter::append(const FrameLogRow& row)
{
  if (!m_file.is_open())
  {
    return;
  }

  m_buffer += formatFrameLogRow(row);
  if (m_buffer.size() >= WriteThreshold)
  {
    writeBuffer();
  }
}

std::optional<std::string> FrameLogWriter::close()
{
  if (m_file.is_open())
  {
    writeBuffer();
    m_file.flush();
    m_file.close();
    if (!m_file && !m_error)
    {
      m_error = fmt::format("Cannot write the frame log {}", m_path.string());
    }
  }
  m_buffer.clear();
  return std::exchange(m_error, std::nullopt);
}

void FrameLogWriter::writeBuffer()
{
  m_file.write(m_buffer.data(), std::streamsize(m_buffer.size()));
  m_buffer.clear();
  if (!m_file && !m_error)
  {
    m_error = fmt::format("Cannot write the frame log {}", m_path.string());
  }
}

} // namespace tb::lit
