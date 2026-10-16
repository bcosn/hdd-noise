#pragma once

#include <memory>
#include <string>
#include <vector>

namespace hdd {

struct DiskInfo {
    std::wstring id;
    std::wstring label;
};

struct DiskSample {
    double busy = 0.0;
    double transfersPerSecond = 0.0;
    double bytesPerSecond = 0.0;
    bool available = false;
    std::wstring status;
};

// Reads the Windows performance counters without opening a disk or generating I/O.
// All public methods are thread-safe. Disk identifiers are PDH instance names.
class DiskMonitor {
public:
    DiskMonitor();
    ~DiskMonitor();

    DiskMonitor(const DiskMonitor&) = delete;
    DiskMonitor& operator=(const DiskMonitor&) = delete;

    void start();
    void stop();
    std::vector<DiskInfo> disks() const;
    void select(const std::wstring& id);
    DiskSample sample() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hdd
