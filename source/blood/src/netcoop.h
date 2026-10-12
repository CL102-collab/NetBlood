// SPDX-License-Identifier: GPL-2.0-or-later
// Authoritative per-player life accounting, separate from legacy game state.
#pragma once

namespace netcoop
{
class LifeState
{
    int m_remaining = -1;
    bool m_spawned = true;

public:
    // The initial spawn consumes one of these lives only when it dies. Zero
    // preserves unlimited respawns; the wire and command-line limit is 255.
    void reset(unsigned limit)
    {
        m_remaining = limit ? static_cast<int>(limit > 255 ? 255 : limit) : -1;
        m_spawned = true;
    }

    // Repeated death callbacks, including corpses damaged again, cannot spend
    // another life until an actual respawn rearms this guard.
    bool die()
    {
        if (!m_spawned || exhausted()) return false;
        m_spawned = false;
        if (m_remaining > 0) --m_remaining;
        return true;
    }

    bool respawn()
    {
        if (exhausted()) return false;
        m_spawned = true;
        return true;
    }

    int remaining() const { return m_remaining; }
    bool exhausted() const { return m_remaining == 0; }
};
}
