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

namespace tb::ui
{

/**
 * Delivers the messages that other threads have sent to this thread's windows and are
 * waiting for, and nothing else: posted messages stay queued, so no Qt event runs. On
 * Windows a present can wait for such a message, so a GUI thread that waits for the lit
 * preview's render thread calls this between short waits. Elsewhere it does nothing.
 */
void serviceSentMessages();

} // namespace tb::ui
