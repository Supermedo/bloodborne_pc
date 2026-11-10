// SPDX-License-Identifier: GPL-2.0-or-later
#include "pad_mailbox.h"

#include <atomic>
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>

int main() {
    AtlasInput::PadMailbox mailbox;
    std::vector<AtlasInput::PadSample> batch;

    // A map opened during a slow frame must see the initial neutral sample so
    // its arming guard unlocks, then the freshest stick position.
    mailbox.Push({0,128,128,128,128});
    for (int i=160; i<240; ++i) mailbox.Push({0,128,128,uint8_t(i),128});
    mailbox.Take(batch);
    assert(batch.size()==2 && batch[0].Neutral() && batch[1].rx==239);

    // Short close/zoom taps must survive even if both edges arrive while a
    // renderer holds its own state lock. Neutral releases remain distinct.
    for (uint32_t buttons : {0x2u,0u,0x100u,0u,0x200u,0u})
        mailbox.Push({buttons,128,128,128,128});
    mailbox.Take(batch);
    assert(batch.size()==6);
    assert(batch[0].buttons==0x2 && batch[1].buttons==0);
    assert(batch[2].buttons==0x100 && batch[3].buttons==0);
    assert(batch[4].buttons==0x200 && batch[5].buttons==0);

    // Focus loss discards the old device's actions.
    mailbox.Push({0x2,128,128,128,128});
    mailbox.Clear();
    mailbox.Take(batch);
    assert(batch.empty());

    // Producer and render consumer race: no button transition is reordered or
    // lost. Button masks vary on every sample, so each is significant.
    std::atomic<bool> done{false};
    constexpr uint32_t count=100000;
    std::thread producer([&] {
        for (uint32_t i=0;i<count;++i)
            mailbox.Push({i%65536,128,128,uint8_t(i%256),128});
        done.store(true,std::memory_order_release);
    });
    uint32_t received=0;
    while (true) {
        mailbox.Take(batch);
        for (const auto& sample : batch) {
            assert(sample.buttons==received%65536);
            assert(sample.rx==received%256);
            ++received;
        }
        if (done.load(std::memory_order_acquire) && received==count) break;
        std::this_thread::yield();
    }
    producer.join();
    assert(received==count);
    std::cout << "PASS: neutral arming, latest axes, short taps, focus reset, 100000 concurrent transitions\n";
}
