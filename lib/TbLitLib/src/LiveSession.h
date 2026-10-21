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

namespace tb::lit
{

/**
 * The process's one live session (review L2.10). vulkan.hpp's default dispatcher is
 * process-wide, and a second session would route the first one's device calls through
 * its own device. LitSession claims the slot in create and releases it once its teardown
 * has run; the dispatcher is initialised and reset only while the slot is free.
 */
bool claimLiveSession();
void releaseLiveSession();
bool liveSessionExists();

} // namespace tb::lit
