// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/fsr_support_cache.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>
#include <vector>

int main() {
    using namespace Vulkan;
    const auto folder=std::filesystem::temp_directory_path()/std::filesystem::path("bb-fsr-support-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(folder);
    const auto file=folder/"runtime.dll",other=folder/"other.dll";
    {std::ofstream f(file);f<<"first";}
    {std::ofstream f(other);f<<"first";}
    FsrSupportCache cache;
    int calls{};
    const auto ready=[&]{++calls;return FsrSupportResult{true,0};};
    assert(cache.Get(0,file,false,ready).supported && calls==1);
    assert(cache.Get(0,file,false,ready).supported && calls==1);
    assert(cache.Get(1,file,false,ready).supported && calls==2);
    cache.Get(0,file,true,ready);assert(calls==3); // Explicit user retry always probes.
    cache.Get(0,other,false,ready);assert(calls==4);
    cache.Get(0,file,false,ready);assert(calls==5);
    {std::ofstream f(file);f<<"second-longer";}
    cache.Get(0,file,false,ready);assert(calls==6);
    std::filesystem::last_write_time(file,std::filesystem::last_write_time(file)+std::chrono::seconds(1));
    cache.Get(0,file,false,ready);assert(calls==7);
    std::filesystem::remove(file);
    const auto failed=[&]{++calls;return FsrSupportResult{false,4};};
    assert(!cache.Get(0,file,false,failed).supported && calls==8);
    assert(!cache.Get(0,file,false,failed).supported && calls==9); // Failures never stick.
    {std::ofstream f(file);f<<"restored";}
    assert(cache.Get(0,file,false,ready).supported && calls==10);
    FsrSupportCache another_device;
    another_device.Get(0,file,false,ready);assert(calls==11);
    FsrSupportCache concurrent;
    std::atomic_uint concurrent_calls{};
    std::vector<std::jthread> threads;
    for (int i=0;i<8;++i) threads.emplace_back([&]{
        assert(concurrent.Get(0,file,false,[&]{++concurrent_calls;return FsrSupportResult{true,0};}).supported);
    });
    threads.clear();assert(concurrent_calls==1);
    std::filesystem::remove(file);std::filesystem::remove(other);std::filesystem::remove(folder);
    std::puts("PASS: per-device/method success reuse, path/size/time/repair invalidation, explicit retry and concurrent callers");
}
