#include "../lzc-mesh-client/windows/src/outbox.h"
#include <cassert>
#include <future>
#include <iostream>
#include <vector>
using namespace std::chrono_literals;
int main() {
    HubOutbox queue;
    std::promise<void> writing, release;
    auto releaseWriter=release.get_future();
    std::vector<std::string> written;
    auto writer=std::async(std::launch::async,[&]{
        HubOutbox::Frame f;
        while(queue.take(f)) {
            if(f.text=="first"){writing.set_value();releaseWriter.wait();}
            written.push_back(f.text);
            if(f.text=="last")break;
        }
    });
    assert(queue.push("first")); writing.get_future().wait();
    auto producer=std::async(std::launch::async,[&]{return queue.push("button-down") && queue.push("button-up") && queue.push("last");});
    const bool nonblocking=producer.wait_for(100ms)==std::future_status::ready;
    release.set_value();
    assert(producer.get()); writer.get(); assert(nonblocking);
    assert((written==std::vector<std::string>{"first","button-down","button-up","last"}));
    HubOutbox retired;assert(retired.push("old-session"));retired.stop();
    HubOutbox::Frame frame;assert(!retired.take(frame));assert(!retired.push("late-callback"));
    HubOutbox fresh;assert(fresh.push("new-session"));assert(fresh.take(frame) && frame.text=="new-session");
    HubOutbox bounded;
    for(int n=0;n<1024;n++)assert(bounded.push("control"));
    assert(!bounded.push("overflow"));bounded.stop();assert(!bounded.take(frame));
    HubOutbox bytes;assert(!bytes.push(std::string(2*1024*1024+1,'x')));
    HubOutbox idle;auto waiting=std::async(std::launch::async,[&]{HubOutbox::Frame f;return idle.take(f);});
    idle.stop();assert(waiting.wait_for(1s)==std::future_status::ready && !waiting.get());
    std::cout<<"PASS: slow writer isolation, FIFO, bounds, stop and fresh connection\n";
}
