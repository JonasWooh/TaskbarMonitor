#pragma once

constexpr int kMaxCores = 256;

// A value below zero means "not available" and is rendered as "--".
struct Metrics {
    double cpuUtil = -1;   // %
    double cpuMHz = -1;    // effective frequency
    double cpuTemp = -1;   // °C, package
    double cpuPower = -1;  // W, package (RAPL)
    double gpuUtil = -1;
    double gpuMHz = -1;
    double gpuTemp = -1;
    double gpuPower = -1;
    double gpuMemUsedGB = -1;
    double gpuMemTotalGB = -1;
    double memUtil = -1;   // % of physical memory in use
    double memUsedGB = -1;
    double memTotalGB = -1;
    double commitUsedGB = -1;
    double commitLimitGB = -1;
    double diskRead = -1;  // bytes/s, all physical disks
    double diskWrite = -1;
    double netUp = -1;     // bytes/s
    double netDown = -1;
    int coreCount = 0;     // per-logical-processor load, only while requested
    float coreUtil[kMaxCores];
};

// Status of the ring-0 (PawnIO) sensors, used by the UI to explain missing values.
enum class HwSensorState { Ok, NotInstalled, NeedsAdmin, Unsupported };

class Collector {
public:
    bool Init();
    void Sample(Metrics& m);
    void Shutdown();
    // Per-core load costs an extra PDH counter array; only collect it while shown.
    void SetPerCore(bool on);
    HwSensorState HwState() const { return hwState_; }
    // Valid after Init(); empty when unknown.
    const wchar_t* CpuName() const { return cpuName_; }
    const wchar_t* GpuName() const { return gpuName_; }

private:
    void SampleCpu(Metrics& m);
    void SampleMemory(Metrics& m);
    void SampleNetwork(Metrics& m, double dt);
    void SampleGpu(Metrics& m);
    void SampleCpuHw(Metrics& m, double dt);

    HwSensorState hwState_ = HwSensorState::Unsupported;
    long long lastQpc_ = 0;
    long long qpcFreq_ = 1;
    wchar_t cpuName_[128] = {};
    wchar_t gpuName_[128] = {};
};
