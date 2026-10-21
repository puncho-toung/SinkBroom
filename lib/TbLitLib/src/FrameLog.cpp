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

#include <utility>

namespace tb::lit
{
namespace
{

// rows are written out once this much is buffered (about 800 frames)
constexpr auto WriteThreshold = std::size_t{64 * 1024};

} // namespace

std::string_view frameLogHeader()
{
  return "frame,cpu_record_ms,gpu_clear_ms,present_mode,swapchain_generation,frames_"
         "skipped,width,height\n";
}

std::string formatFrameLogRow(const FrameLogRow& row)
{
  const auto gpu =
    row.gpuClearMs ? fmt::format("{:.3f}", *row.gpuClearMs) : std::string{};
  return fmt::format(
    "{},{:.3f},{},{},{},{},{},{}\n",
    row.frameIndex,
    row.cpuRecordMs,
    gpu,
    row.presentMode,
    row.swapchainGeneration,
    row.framesSkipped,
    row.width,
    row.height);
}

std::optional<std::string> FrameLogWriter::open(const std::filesystem::path& path)
{
  close();
  if (path.empty())
  {
    return std::nullopt;
  }

  m_path = path;
  m_file.open(path, std::ios::out | std::ios::trunc | std::ios::binary);
  if (!m_file)
  {
    return fmt::format("Cannot open the frame log {}", path.string());
  }

  m_buffer = std::string{frameLogHeader()};
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
