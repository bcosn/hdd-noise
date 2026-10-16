#include "DiskMonitor.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

#ifdef _MSC_VER
#pragma comment(lib, "pdh.lib")
#endif

namespace hdd {
namespace {

constexpr auto kPollInterval = std::chrono::milliseconds(250);
constexpr auto kRetryInterval = std::chrono::seconds(2);
constexpr wchar_t kTotal[] = L"_Total";

struct Query {
    PDH_HQUERY handle = nullptr;
    ~Query() {
        if (handle) {
            PdhCloseQuery(handle);
        }
    }
};

struct CounterValues {
    std::vector<std::wstring> instances;
    std::map<std::wstring, double> valid;
};

bool usableStatus(DWORD status) {
    return status == PDH_CSTATUS_VALID_DATA || status == PDH_CSTATUS_NEW_DATA;
}

PDH_STATUS readCounter(PDH_HCOUNTER counter, CounterValues& values) {
    DWORD bytes = 0;
    DWORD count = 0;
    PDH_STATUS result = PdhGetFormattedCounterArrayW(
        counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, nullptr);
    if (result != PDH_MORE_DATA) {
        return result;
    }

    // Instances can appear between the sizing call and the actual read.
    std::vector<unsigned char> buffer;
    for (int attempt = 0; attempt < 4; ++attempt) {
        buffer.resize(bytes);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        DWORD availableBytes = bytes;
        result = PdhGetFormattedCounterArrayW(
            counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100,
            &availableBytes, &count, items);
        if (result == PDH_MORE_DATA) {
            bytes = availableBytes;
            continue;
        }
        if (result != ERROR_SUCCESS) {
            return result;
        }

        for (DWORD index = 0; index < count; ++index) {
            const auto& item = items[index];
            if (!item.szName || item.szName[0] == L'\0') {
                continue;
            }
            const std::wstring id(item.szName);
            values.instances.push_back(id);
            if (usableStatus(item.FmtValue.CStatus) &&
                std::isfinite(item.FmtValue.doubleValue)) {
                values.valid.emplace(id, item.FmtValue.doubleValue);
            }
        }
        return ERROR_SUCCESS;
    }
    return result;
}

std::wstring labelFor(const std::wstring& id) {
    return id == kTotal ? L"所有物理磁盘（合计）" : L"磁盘 " + id;
}

double nonnegativeValue(const CounterValues& values, const std::wstring& id) {
    const auto found = values.valid.find(id);
    return found == values.valid.end() ? 0.0 : std::max(0.0, found->second);
}

} // namespace

struct DiskMonitor::Impl {
    mutable std::mutex mutex;
    std::mutex lifecycleMutex;
    std::condition_variable wake;
    std::thread worker;
    bool stopping = true;
    std::wstring selected = kTotal;
    std::vector<DiskInfo> knownDisks{{kTotal, labelFor(kTotal)}};
    DiskSample current{0.0, 0.0, 0.0, false, L"磁盘监测尚未启动"};

    bool shouldStop() const {
        std::lock_guard<std::mutex> lock(mutex);
        return stopping;
    }

    template <typename Duration>
    bool waitFor(Duration duration) {
        std::unique_lock<std::mutex> lock(mutex);
        return wake.wait_for(lock, duration, [this] { return stopping; });
    }

    void unavailable(const std::wstring& status) {
        std::lock_guard<std::mutex> lock(mutex);
        current = {};
        current.status = status;
    }

    void publish(const CounterValues& idle, const CounterValues& transfers,
                 const CounterValues& bytes, bool warmedUp) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!idle.instances.empty()) {
            std::vector<DiskInfo> next{{kTotal, labelFor(kTotal)}};
            for (const auto& id : idle.instances) {
                if (id != kTotal) {
                    next.push_back({id, labelFor(id)});
                }
            }
            std::sort(next.begin() + 1, next.end(), [](const DiskInfo& a, const DiskInfo& b) {
                return a.id < b.id;
            });
            next.erase(std::unique(next.begin(), next.end(), [](const DiskInfo& a, const DiskInfo& b) {
                return a.id == b.id;
            }), next.end());
            knownDisks = std::move(next);
        }

        current = {};
        if (!warmedUp) {
            current.status = L"正在采集磁盘活动，请稍候…";
            return;
        }
        const auto found = idle.valid.find(selected);
        if (found == idle.valid.end()) {
            current.status = L"所选磁盘当前离线或未提供性能数据";
            return;
        }

        current.busy = std::clamp(1.0 - found->second / 100.0, 0.0, 1.0);
        current.transfersPerSecond = nonnegativeValue(transfers, selected);
        current.bytesPerSecond = nonnegativeValue(bytes, selected);
        current.available = true;
        current.status = L"正在监测：" + labelFor(selected);
    }

    void monitorSession() {
        Query query;
        PDH_HCOUNTER idleCounter = nullptr;
        PDH_HCOUNTER transfersCounter = nullptr;
        PDH_HCOUNTER bytesCounter = nullptr;

        if (PdhOpenQueryW(nullptr, 0, &query.handle) != ERROR_SUCCESS ||
            PdhAddEnglishCounterW(query.handle, L"\\PhysicalDisk(*)\\% Idle Time", 0,
                                  &idleCounter) != ERROR_SUCCESS ||
            PdhAddEnglishCounterW(query.handle, L"\\PhysicalDisk(*)\\Disk Transfers/sec", 0,
                                  &transfersCounter) != ERROR_SUCCESS ||
            PdhAddEnglishCounterW(query.handle, L"\\PhysicalDisk(*)\\Disk Bytes/sec", 0,
                                  &bytesCounter) != ERROR_SUCCESS) {
            unavailable(L"系统未提供物理磁盘性能计数器，正在重试");
            return;
        }

        unsigned int collections = 0;
        unsigned int invalidSamples = 0;
        while (!shouldStop()) {
            if (PdhCollectQueryData(query.handle) != ERROR_SUCCESS) {
                unavailable(L"暂时无法读取磁盘活动，正在重试");
                return;
            }
            ++collections;

            CounterValues idle;
            CounterValues transfers;
            CounterValues bytes;
            const PDH_STATUS idleStatus = readCounter(idleCounter, idle);
            readCounter(transfersCounter, transfers);
            readCounter(bytesCounter, bytes);
            publish(idle, transfers, bytes, collections >= 2);

            // Rate counters need two observations; prolonged invalid data may mean
            // a provider was restarted, so rebuild the query after a short delay.
            if (idleStatus != ERROR_SUCCESS || idle.valid.empty()) {
                if (++invalidSamples >= 12) {
                    unavailable(L"物理磁盘性能数据暂不可用，正在重试");
                    return;
                }
            } else {
                invalidSamples = 0;
            }
            if (waitFor(kPollInterval)) {
                return;
            }
        }
    }

    void run() {
        while (!shouldStop()) {
            try {
                monitorSession();
            } catch (...) {
                unavailable(L"磁盘监测暂不可用，正在重试");
            }
            if (waitFor(kRetryInterval)) {
                break;
            }
        }
    }
};

DiskMonitor::DiskMonitor() : impl_(std::make_unique<Impl>()) {}

DiskMonitor::~DiskMonitor() {
    stop();
}

void DiskMonitor::start() {
    std::lock_guard<std::mutex> lifecycleLock(impl_->lifecycleMutex);
    if (impl_->worker.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stopping = false;
        impl_->current = {};
        impl_->current.status = L"正在初始化磁盘监测…";
    }
    try {
        impl_->worker = std::thread([this] { impl_->run(); });
    } catch (...) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stopping = true;
        impl_->current.status = L"无法启动磁盘监测线程";
        throw;
    }
}

void DiskMonitor::stop() {
    std::lock_guard<std::mutex> lifecycleLock(impl_->lifecycleMutex);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stopping = true;
    }
    impl_->wake.notify_all();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    impl_->unavailable(L"磁盘监测已停止");
}

std::vector<DiskInfo> DiskMonitor::disks() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->knownDisks;
}

void DiskMonitor::select(const std::wstring& id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const std::wstring next = id.empty() ? kTotal : id;
    if (impl_->selected != next) {
        impl_->selected = next;
        impl_->current = {};
        impl_->current.status = impl_->stopping ? L"磁盘监测已停止" : L"正在切换监测磁盘…";
    }
}

DiskSample DiskMonitor::sample() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->current;
}

} // namespace hdd
