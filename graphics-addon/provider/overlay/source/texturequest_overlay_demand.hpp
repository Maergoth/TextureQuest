/*
 * TextureQuest optional overlay demand. Event registration and dispatch stay
 * under ReShade's existing lifecycle rules; toggles never touch those vectors.
 */
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <vector>

namespace tq_overlay_demand
{
class registry
{
public:
	void register_callback(void *owner, void *callback)
	{
		const std::lock_guard<std::mutex> lock(_mutex);
		const auto found = find(owner, callback);
		if (found == _entries.end())
			_entries.push_back({ owner, callback, true });
		else
			found->enabled = true; // Every new registration is demanding by default.
		publish();
	}

	void unregister_callback(void *owner, void *callback)
	{
		const std::lock_guard<std::mutex> lock(_mutex);
		_entries.erase(std::remove_if(_entries.begin(), _entries.end(),
			[owner, callback](const entry &value) { return value.owner == owner && value.callback == callback; }), _entries.end());
		publish();
	}

	bool set_demand(void *owner, void *callback, bool enabled)
	{
		if (owner == nullptr || callback == nullptr)
			return false;
		const std::lock_guard<std::mutex> lock(_mutex);
		const auto found = find(owner, callback);
		if (found == _entries.end())
			return false;
		found->enabled = enabled;
		publish();
		return true;
	}

	bool has_demand() const noexcept
	{
		return _active.load(std::memory_order_acquire) != 0;
	}

private:
	struct entry { void *owner; void *callback; bool enabled; };
	std::mutex _mutex;
	std::vector<entry> _entries;
	std::atomic<size_t> _active { 0 };

	std::vector<entry>::iterator find(void *owner, void *callback)
	{
		return std::find_if(_entries.begin(), _entries.end(),
			[owner, callback](const entry &value) { return value.owner == owner && value.callback == callback; });
	}

	void publish()
	{
		_active.store(static_cast<size_t>(std::count_if(_entries.begin(), _entries.end(),
			[](const entry &value) { return value.enabled; })), std::memory_order_release);
	}
};

inline registry callbacks;
inline bool has_overlay_demand() noexcept { return callbacks.has_demand(); }
}
