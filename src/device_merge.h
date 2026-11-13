#pragma once

#include <string>
#include <vector>

#include "protocols.h"

// Stable key shared by transports of the same physical device. Falls back to
// the transport-specific id when no cross-transport identity is known.
std::wstring DeviceIdentity(const Device& d);

// Collapse entries that describe the same physical device (for example one
// mouse seen through its 2.4G receiver and through its USB charging cable).
// The entry with the most live data wins; missing fields are filled from its
// siblings. Routing fields (id/path/ifaces) always stay with the kept entry.
void DedupeDevicesByIdentity(std::vector<Device>& devices);

// Merge the previous snapshot with a fresh scan. Only devices that are online in
// the fresh scan are returned; offline/sleeping devices and entries replaced by
// another transport of the same physical device are dropped.
std::vector<Device> MergeDeviceSnapshots(const std::vector<Device>& oldDevices,
                                         std::vector<Device> fresh);
