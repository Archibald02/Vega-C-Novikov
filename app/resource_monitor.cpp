#include "resource_monitor.hpp"
#include <QFile>
#include <algorithm>
#include <ctime>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#endif
namespace live {
namespace {
double cpu_seconds(){
#ifdef _WIN32
    FILETIME creation,exit,kernel,user;
    if(!GetProcessTimes(GetCurrentProcess(),&creation,&exit,&kernel,&user))return 0;
    auto value=[](FILETIME t){return (static_cast<unsigned long long>(t.dwHighDateTime)<<32)|t.dwLowDateTime;};
    return static_cast<double>(value(kernel)+value(user))/1e7;
#else
    return static_cast<double>(std::clock())/CLOCKS_PER_SEC;
#endif
}
double memory_mib(){
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS info{};info.cb=sizeof(info);
    return GetProcessMemoryInfo(GetCurrentProcess(),&info,sizeof(info))?static_cast<double>(info.WorkingSetSize)/(1024*1024):0;
#else
    QFile file("/proc/self/statm");if(!file.open(QIODevice::ReadOnly))return 0;
    const auto parts=file.readAll().simplified().split(' ');if(parts.size()<2)return 0;
    return parts[1].toDouble()*sysconf(_SC_PAGESIZE)/(1024*1024);
#endif
}
}
ResourceMonitor::ResourceMonitor():last_cpu_(cpu_seconds()){wall_.start();}
ResourceSnapshot ResourceMonitor::sample(){const double seconds=wall_.nsecsElapsed()/1e9;const double now=cpu_seconds();wall_.restart();const double cpu=seconds>0?std::max(0.0,(now-last_cpu_)*100/seconds):0;last_cpu_=now;return {cpu,memory_mib()};}
}
