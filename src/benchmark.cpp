#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;
using Ns = std::chrono::nanoseconds;
static_assert(Clock::is_steady);
std::int64_t ns(Clock::duration d) { return std::chrono::duration_cast<Ns>(d).count(); }
struct Event { unsigned slot; unsigned action; std::int64_t price; };
struct Order { std::int64_t price = 0; bool live = false; };
struct Sample { std::int64_t scheduled, start, end; bool growth; bool pause; };
constexpr unsigned slots = 4096;
constexpr unsigned batch = 4096;
const std::vector<std::string> modes = {
    "baseline", "allocate", "grow", "log-buffered", "log-flush", "pause"};

// A deliberately small order-state kernel, NOT a price-time matching engine.
// 0=add, 1=cancel, 2=execute entire remaining order.
class Engine {
    std::array<Order, slots> pool_{};
    std::array<std::unique_ptr<Order>, slots> heap_{};
    bool allocating_;
public:
    std::uint64_t checksum = 0;
    explicit Engine(bool allocating) : allocating_(allocating) {}
    std::uint64_t process(const Event& e) {
        if (e.slot >= slots || e.price <= 0 || e.action > 2)
            throw std::runtime_error("invalid event");
        Order* p = allocating_ ? heap_[e.slot].get() : &pool_[e.slot];
        if (e.action == 0) {
            if (p && p->live) throw std::runtime_error("duplicate add");
            if (allocating_) { heap_[e.slot] = std::make_unique<Order>(); p = heap_[e.slot].get(); }
            *p = {e.price, true};
        } else {
            if (!p || !p->live) throw std::runtime_error("missing order");
            checksum += static_cast<std::uint64_t>(p->price) * (e.action + 1) + e.slot;
            p->live = false;
            if (allocating_) heap_[e.slot].reset();
        }
        return checksum;
    }
    bool empty() const {
        for (unsigned i=0; i<slots; ++i)
            if (pool_[i].live || (heap_[i] && heap_[i]->live)) return false;
        return true;
    }
};

std::vector<Event> trace(std::size_t n) {
    std::vector<Event> out; out.reserve(n);
    for (std::size_t i=0; i<n; ++i) {
        const unsigned phase = static_cast<unsigned>(i % (slots * 2));
        const unsigned slot = phase % slots;
        out.push_back({slot, phase < slots ? 0U : (slot % 2 ? 1U : 2U),
                       100000 + static_cast<std::int64_t>(slot % 101)});
    }
    return out;
}

std::int64_t quantile(std::vector<std::int64_t> v, double q) {
    if (v.empty()) throw std::runtime_error("empty sample");
    std::sort(v.begin(), v.end());
    return v[static_cast<std::size_t>(std::ceil(q * static_cast<double>(v.size()))) - 1];
}

void self_test() {
    Engine a(false), b(true);
    for (const auto& e : trace(slots * 4)) {
        if (a.process(e) != b.process(e)) throw std::runtime_error("checksum mismatch");
    }
    if (!a.empty() || !b.empty()) throw std::runtime_error("orders leaked");
    bool rejected = false;
    try { a.process({0,1,100}); } catch (const std::runtime_error&) { rejected = true; }
    if (!rejected) throw std::runtime_error("invalid cancel accepted");
    if (quantile({1,2,3,4,5}, 0.5) != 3 || quantile({1,2,3,4,5}, 0.99) != 5)
        throw std::runtime_error("quantile failure");
    std::cout << "self-test passed\n";
}

int main(int argc, char** argv) {
    try {
        std::string mode="baseline", output="results/run";
        std::size_t n=1000000;
        std::int64_t interval=1000;
        for (int i=1; i<argc; ++i) {
            const std::string arg=argv[i];
            if (arg=="--self-test") { self_test(); return 0; }
            if (i+1>=argc) throw std::runtime_error("missing option value");
            const std::string v=argv[++i];
            if (arg=="--mode") mode=v;
            else if (arg=="--events") n=std::stoull(v);
            else if (arg=="--interval-ns") interval=std::stoll(v);
            else if (arg=="--output") output=v;
            else throw std::runtime_error("unknown option: " + arg);
        }
        if (std::find(modes.begin(),modes.end(),mode)==modes.end()) throw std::runtime_error("unknown mode");
        if (!n || n>100000000 || interval<0 || interval>1000000000)
            throw std::runtime_error("events must be 1..100000000; interval 0..1000000000 ns");
        const auto parent=std::filesystem::path(output).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
        auto events=trace(n);
        auto warm=trace(slots*16);
        { Engine e(mode=="allocate"); for (auto v:warm) e.process(v); }
        Engine engine(mode=="allocate");
        std::vector<Sample> samples(n); // Allocate and initialize outside measurement.
        std::vector<std::uint64_t> audit;
        if (mode!="grow") audit.reserve(batch);
        std::array<char,65536> log_buffer{};
        auto close_file=[](std::FILE* f) { if(f) std::fclose(f); };
        std::unique_ptr<std::FILE, decltype(close_file)> log(nullptr, close_file);
        if (mode=="log-buffered" || mode=="log-flush") {
            log.reset(std::fopen((output+".bin").c_str(),"wb"));
            if (!log) throw std::runtime_error("cannot open log");
            if (std::setvbuf(log.get(),log_buffer.data(),_IOFBF,log_buffer.size())!=0)
                throw std::runtime_error("setvbuf failed");
        }
        const auto origin=Clock::now();
        std::size_t growths=0, pauses=0;
        for (std::size_t i=0;i<n;++i) {
            const auto planned=origin+Ns(static_cast<std::int64_t>(i)*interval);
            if (interval) while (Clock::now()<planned) {}
            const auto start=Clock::now();
            if (i%batch==0) {
                if (mode=="grow") std::vector<std::uint64_t>().swap(audit);
                else audit.clear();
            }
            const auto old_capacity=audit.capacity();
            const auto result=engine.process(events[i]);
            audit.push_back(result);
            const bool grew=audit.capacity()!=old_capacity;
            const bool paused=mode=="pause" && (i+1)%10000==0;
            if (paused) { const auto until=Clock::now()+Ns(50000); while(Clock::now()<until) {} }
            if (log && (std::fwrite(&result,sizeof(result),1,log.get())!=1 ||
                        (mode=="log-flush" && std::fflush(log.get())!=0)))
                throw std::runtime_error("log write failed");
            const auto end=Clock::now();
            samples[i]={ns(planned-origin),ns(start-origin),ns(end-origin),grew,paused};
            growths+=grew; pauses+=paused;
        }
        const auto finish=Clock::now();
        // Buffered mode's final flush is outside event timing, but measured separately.
        const auto flush_start=Clock::now();
        if(log && std::fflush(log.get())!=0) throw std::runtime_error("final flush failed");
        const auto flush_ns=ns(Clock::now()-flush_start);
        std::vector<std::int64_t> service,wait,response;
        service.reserve(n); wait.reserve(n); response.reserve(n);
        std::ofstream raw(output+".samples.csv");
        if(!raw) throw std::runtime_error("cannot open samples");
        raw<<"event,action,scheduled_ns,start_ns,end_ns,service_ns,wait_ns,response_ns,growth,pause\n";
        for(std::size_t i=0;i<n;++i) {
            const auto& s=samples[i];
            auto sv=s.end-s.start;
            auto w=interval?s.start-s.scheduled:0;
            service.push_back(sv); wait.push_back(w); response.push_back(sv+w);
            raw<<i<<','<<events[i].action<<','<<s.scheduled<<','<<s.start<<','<<s.end<<','<<sv<<','<<w<<','<<sv+w<<','<<s.growth<<','<<s.pause<<'\n';
        }
        raw.close(); if(!raw) throw std::runtime_error("samples write failed");
        std::ofstream summary(output+".summary.csv");
        summary<<"mode,events,interval_ns,metric,p50_ns,p99_ns,p999_ns,max_ns,over_100us,throughput_eps,checksum,growth_events,pause_events,final_flush_ns\n";
        auto report=[&](const char* metric,const std::vector<std::int64_t>& v) {
            summary<<mode<<','<<n<<','<<interval<<','<<metric<<','<<quantile(v,.5)<<','<<quantile(v,.99)<<','<<quantile(v,.999)<<','<<*std::max_element(v.begin(),v.end())<<','<<std::count_if(v.begin(),v.end(),[](auto x){return x>100000;})<<','<<static_cast<double>(n)*1e9/static_cast<double>(ns(finish-origin))<<','<<engine.checksum<<','<<growths<<','<<pauses<<','<<flush_ns<<'\n';
        };
        report("service",service); report("wait",wait); report("response",response);
        summary.close(); if(!summary) throw std::runtime_error("summary write failed");
        std::cout<<mode<<": "<<n<<" events, checksum "<<engine.checksum<<" -> "<<output<<".summary.csv\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
