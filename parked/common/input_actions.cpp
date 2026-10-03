#include "input_actions.hpp"

#include <algorithm>
#include <cstring>

namespace dayz::input_actions
{
    namespace
    {
        bool SameName(const char* entryName, std::string_view name) noexcept
        {
            return std::strlen(entryName) == name.size() &&
                std::memcmp(entryName, name.data(), name.size()) == 0;
        }
    }

    Table::Entry* Table::Find(std::string_view name) noexcept
    {
        for (Entry& entry : entries_)
            if (entry.active && SameName(entry.name, name))
                return &entry;
        return nullptr;
    }

    const Table::Entry* Table::Find(std::string_view name) const noexcept
    {
        for (const Entry& entry : entries_)
            if (entry.active && SameName(entry.name, name))
                return &entry;
        return nullptr;
    }

    bool Table::Set(std::string_view name, float value, bool held) noexcept
    {
        if (name.empty() || name.size() >= kNameLength)
            return false;
        const std::lock_guard<std::mutex> lock(mutex_);
        Entry* entry = Find(name);
        if (!entry)
        {
            for (Entry& candidate : entries_)
                if (!candidate.active)
                {
                    entry = &candidate;
                    break;
                }
            if (!entry)
                return false;
            *entry = Entry{};
            std::memcpy(entry->name, name.data(), name.size());
            entry->name[name.size()] = '\0';
            entry->active = true;
        }
        entry->removing = false;
        entry->value = value;
        entry->held = held;
        return true;
    }

    void Table::Clear(std::string_view name) noexcept
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (Entry* entry = Find(name))
        {
            entry->value = 0.0f;
            entry->held = false;
            entry->removing = true;
        }
    }

    void Table::ClearAll() noexcept
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (Entry& entry : entries_)
            if (entry.active)
            {
                entry.value = 0.0f;
                entry.held = false;
                entry.removing = true;
            }
    }

    void Table::Latch() noexcept
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (Entry& entry : entries_)
        {
            if (!entry.active)
                continue;
            Latched latched;
            latched.value = entry.value;
            latched.held = entry.held;
            latched.press = entry.held && !entry.previousHeld;
            latched.release = !entry.held && entry.previousHeld;
            latched.holdBegin = latched.press;
            entry.latched = latched;
            entry.previousHeld = entry.held;
            // A cleared action is dropped once its release edge has been delivered
            // for one frame, so the engine saw the key go up.
            if (entry.removing && !entry.held && !latched.release)
                entry.active = false;
        }
    }

    bool Table::Get(std::string_view name, Latched& out) const noexcept
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const Entry* entry = Find(name);
        if (!entry)
            return false;
        out = entry->latched;
        return true;
    }

    bool Table::GetByHandle(std::uintptr_t handle, Latched& out) const noexcept
    {
        if (!handle)
            return false;
        const std::lock_guard<std::mutex> lock(mutex_);
        for (const Entry& entry : entries_)
            if (entry.active && entry.resolved && entry.handle == handle)
            {
                out = entry.latched;
                return true;
            }
        return false;
    }

    bool Table::GetById(int id, Latched& out) const noexcept
    {
        if (id < 0)
            return false;
        const std::lock_guard<std::mutex> lock(mutex_);
        for (const Entry& entry : entries_)
            if (entry.active && entry.resolved && entry.id == id)
            {
                out = entry.latched;
                return true;
            }
        return false;
    }

    bool Table::SetHandle(std::string_view name, std::uintptr_t handle, int id) noexcept
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        Entry* entry = Find(name);
        if (!entry)
            return false;
        entry->handle = handle;
        entry->id = id;
        entry->resolved = handle != 0;
        return entry->resolved;
    }

    std::size_t Table::CopyUnresolvedNames(char (*names)[kNameLength]) const noexcept
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::size_t count = 0;
        for (const Entry& entry : entries_)
            if (entry.active && !entry.resolved)
            {
                std::memcpy(names[count], entry.name, kNameLength);
                ++count;
            }
        return count;
    }

    std::size_t Table::Count() const noexcept
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<std::size_t>(std::count_if(std::begin(entries_), std::end(entries_),
            [](const Entry& entry) { return entry.active; }));
    }

    Table& Global() noexcept
    {
        static Table table;
        return table;
    }
}
