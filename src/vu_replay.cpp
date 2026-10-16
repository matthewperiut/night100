#include "ps2_runtime.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <cstring>
#include <vector>
#include <algorithm>

static void read(const std::filesystem::path& path, void* data, size_t size) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.read(static_cast<char*>(data), size))
        throw std::runtime_error("Cannot read " + path.string());
}

int main(int argc, char** argv) {
    if (argc < 2 || argc > 4) { std::cerr << "vu_replay CAPTURE_DIRECTORY [ITERATIONS] [OUTPUT_DIRECTORY]\n"; return 1; }
    try {
        const std::filesystem::path directory = argv[1];
        PS2Memory memory;
        if (!memory.initialize()) return 1;
        GS gs;
        gs.init(memory.getGSVRAM(), PS2_GS_VRAM_SIZE);
        gs.setDebugHistoryPaused(false);
        VU1Interpreter vu;
        read(directory / "code.bin", memory.getVU1Code(), PS2_VU1_CODE_SIZE);
        read(directory / "data.bin", memory.getVU1Data(), PS2_VU1_DATA_SIZE);
        read(directory / "state.bin", &vu.state(), sizeof(VU1State));
        uint32_t arguments[4];
        read(directory / "arguments.bin", arguments, sizeof(arguments));
        std::cout << "start=0x" << std::hex << arguments[0] << std::dec
                  << " top=" << arguments[1] << " itop=" << arguments[2] << '\n';
        const unsigned iterations=argc>=3 ? std::stoul(argv[2]) : 1;
        if (!iterations || iterations>100000) throw std::runtime_error("Invalid iteration count");
        std::vector<uint8_t> initialState(sizeof(VU1State)),initialData(PS2_VU1_DATA_SIZE);
        std::memcpy(initialState.data(),&vu.state(),initialState.size());
        std::memcpy(initialData.data(),memory.getVU1Data(),initialData.size());
        std::vector<double> timings;
        for (unsigned iteration=0;iteration<iterations;++iteration) {
            std::memcpy(&vu.state(),initialState.data(),initialState.size());
            std::memcpy(memory.getVU1Data(),initialData.data(),initialData.size());
            gs.reset();
            const auto start=std::chrono::steady_clock::now();
            vu.execute(memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                       memory.getVU1Data(), PS2_VU1_DATA_SIZE, gs, nullptr,
                       arguments[0], arguments[1], arguments[2], arguments[3]);
            timings.push_back(std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count());
        }
        const auto hash=[](const void* ptr,size_t size) {
            uint64_t value=14695981039346656037ull;
            const auto* bytes=static_cast<const uint8_t*>(ptr);
            for(size_t i=0;i<size;++i) value=(value^bytes[i])*1099511628211ull;
            return value;
        };
        std::sort(timings.begin(),timings.end());
        std::cout<<"iterations="<<iterations<<" median_us="<<timings[timings.size()/2]
                 <<" cycles="<<vu.state().cycles<<" state_hash="<<std::hex<<hash(&vu.state(),sizeof(VU1State))
                 <<" data_hash="<<hash(memory.getVU1Data(),PS2_VU1_DATA_SIZE)<<std::dec<<'\n';
        if (argc==4) {
            const std::filesystem::path output=argv[3];std::filesystem::create_directories(output);
            std::ofstream(output/"state.bin",std::ios::binary).write(reinterpret_cast<const char*>(&vu.state()),sizeof(VU1State));
            std::ofstream(output/"data.bin",std::ios::binary).write(reinterpret_cast<const char*>(memory.getVU1Data()),PS2_VU1_DATA_SIZE);
        }
        unsigned draws = 0;
        for (const auto& event : gs.getDebugHistory()) {
            if (event.kind != GSDebugEventKind::Draw) continue;
            if (++draws <= 6)
                std::cout << "draw x=" << event.xMin << ',' << event.xMax
                          << " y=" << event.yMin << ',' << event.yMax << '\n';
        }
        std::cout << "draws=" << draws << " end=0x" << std::hex << vu.state().pc << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
