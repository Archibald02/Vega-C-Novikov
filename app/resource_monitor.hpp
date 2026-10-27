#pragma once
#include <QElapsedTimer>
namespace live {
/** CPU is relative to one logical core; RSS includes Qt and all process memory. */
struct ResourceSnapshot {double cpu_percent=0,memory_mib=0;};
class ResourceMonitor {
public:
    ResourceMonitor();
    ResourceSnapshot sample();
private:
    QElapsedTimer wall_;
    double last_cpu_=0;
};
}
