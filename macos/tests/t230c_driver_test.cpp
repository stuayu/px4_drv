#include "t230c_silicon.hpp"
#include "t230c_tuning.hpp"
#include <cstdlib>
#include <cstdio>
#include <cerrno>
#include <chrono>
#include <thread>
#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #condition); return __LINE__; } } while (0)
#include <cstdint>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <vector>

int main()
{
    using namespace px4::t230c;
    CHECK(ResolveTuneLockResult(0, true) == 0);
    CHECK(ResolveTuneLockResult(0, false) == -ETIMEDOUT);
    CHECK(ResolveTuneLockResult(-EIO, false) == -EIO);
    CHECK(DemodTuneCommands(5274, 6).at(7) == Bytes({0x14,0,0x0a,0x10,0x36,0}));
    CHECK(DemodTuneCommands(5274, 6).at(8) == Bytes({0x14,0,0x02,0x11,0x9a,0x14}));
    const auto tuner = TunerTuneCommands(473000, 6);
    CHECK(tuner.at(0) == Bytes({0x14,0,0x03,0x07,0x36,0}));
    CHECK(tuner.at(2) == Bytes({0x14,0,0x06,0x07,0x88,0x13}));
    CHECK(tuner.at(3) == Bytes({0x41,0,0,0,0x40,0x68,0x31,0x1c}));
    CHECK(DemodTuneCommands(5274, 7).empty());

    Bytes invalid(18, 0);
    CHECK(ValidateFirmware(invalid) != 0);
    const auto bad_path=std::filesystem::temp_directory_path()/"t230c-invalid.fw";
    { std::ofstream f(bad_path,std::ios::binary); f.write(reinterpret_cast<const char*>(invalid.data()),invalid.size()); }
    Bytes loaded; std::string error;
    CHECK(LoadFirmwareFile(bad_path.wstring(),loaded,error)!=0 && !error.empty());
    std::filesystem::remove(bad_path);

    Bytes firmware(17,0); firmware[0]=6;
    for(std::size_t i=1;i<=6;i++) firmware[i]=static_cast<std::uint8_t>(i);
    std::vector<std::pair<std::uint8_t,Bytes>> writes;
    std::vector<std::size_t> read_sizes;
    bool check_deadlines = false;
    bool deadlines_valid = true;
    Deadline tune_deadline = Deadline::max();
    Silicon silicon([&](std::uint8_t address,const Bytes &write,Bytes &read, Deadline deadline){
        if (check_deadlines) {
            const auto command_limit = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(address == 0x60 ? 80 : 140);
            deadlines_valid = deadlines_valid && deadline <= tune_deadline && deadline <= command_limit;
        }
        if(!write.empty()) writes.emplace_back(address,write);
        if(!read.empty()) { read_sizes.push_back(read.size()); read[0]=0x81; if (address==0x60 && read.size()==13) {read[2]=41; read[12]=0x60;} if (read.size()==9) { read[2]=6; read[3]=100; } }
        return 0;
    },[](unsigned){});
    std::uint8_t selected_rom = 0;
    std::string firmware_error;
    CHECK(silicon.Initialize(firmware, [&](std::uint8_t rom, Bytes &out, std::string &) {
        selected_rom = rom; out = firmware; return 0;
    }, firmware_error)==0);
    CHECK(selected_rom == 0x60);
    CHECK(std::find(writes.begin(),writes.end(),std::make_pair(std::uint8_t(0x64),Bytes({1,2,3,4,5,6})))!=writes.end());
    writes.clear();
    read_sizes.clear();
    tune_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    check_deadlines = true;
    CHECK(silicon.Tune(473000,5274,6,tune_deadline)==0);
    check_deadlines = false;
    CHECK(deadlines_valid);
    CHECK(read_sizes == std::vector<std::size_t>({4,4,4,1,1,5,3,12,3,4,4,4,4,4,4,4,4,4,4,1,4,4}));
    // si2157.c:507-549 と si2168.c:294-400 のバイト列を順番に比較する。
    const std::vector<std::pair<std::uint8_t,Bytes>> kernel_tune = {
        {0x60,{0x14,0,0x03,0x07,0x36,0}}, {0x60,{0x14,0,0x02,0x07,0,0}},
        {0x60,{0x14,0,0x06,0x07,0x88,0x13}}, {0x60,{0x41,0,0,0,0x40,0x68,0x31,0x1c}},
        {0x64,{0x88,2,2,2,2}}, {0x64,{0x89,0x21,6,0x11,0x89,0xf0}},
        {0x64,{0x51,3}}, {0x64,{0x12,8,4}}, {0x64,{0x14,0,0x0c,0x10,0x12,0}},
        {0x64,{0x14,0,0x06,0x10,0x24,0}}, {0x64,{0x14,0,0x07,0x10,0,0x24}},
        {0x64,{0x14,0,0x0a,0x10,0x36,0}}, {0x64,{0x14,0,0x02,0x11,0x9a,0x14}},
        {0x64,{0x14,0,0x0f,0x10,0x10,0}}, {0x64,{0x14,0,0x09,0x10,0xe3,8}},
        {0x64,{0x14,0,0x08,0x10,0xd7,5}}, {0x64,{0x14,0,0x01,0x12,0,0}},
        {0x64,{0x14,0,0x01,0x03,0x0c,0}}, {0x64,{0x85}},
        {0x64,{0x14,0,0x0d,0x10,0xe8,3}}, {0x64,{0x14,0,0x01,0x10,0x26,0}}
    };
    CHECK(writes == kernel_tune);
    CHECK(std::find(writes.begin(),writes.end(),std::make_pair(std::uint8_t(0x64),Bytes({0x14,0,0x02,0x11,0x9a,0x14})))!=writes.end());
    CHECK(std::find(writes.begin(),writes.end(),std::make_pair(std::uint8_t(0x60),Bytes({0x41,0,0,0,0x40,0x68,0x31,0x1c})))!=writes.end());
    CHECK(writes.size() >= 2);
    CHECK(writes[writes.size()-2] == std::make_pair(std::uint8_t(0x64), Bytes({0x14,0,0x0d,0x10,0xe8,0x03})));
    CHECK(writes.back() == std::make_pair(std::uint8_t(0x64), Bytes({0x14,0,0x01,0x10,0x26,0})));
    std::vector<Bytes> status_writes;
    int status_reads = 0;
    Silicon status_device([&](std::uint8_t address, const Bytes &write, Bytes &read, Deadline) {
        CHECK(address == 0x64);
        CHECK(write.empty() || read.empty());
        if (!write.empty()) status_writes.push_back(write);
        if (!read.empty()) {
            ++status_reads;
            read[0] = status_reads == 1 ? 0x40 : 0x80;
            read[2] = 6; read[3] = 100;
        }
        return 0;
    }, [](unsigned) {});
    bool status_locked = false; std::int32_t status_cnr = 0;
    CHECK(status_device.ReadLock(status_locked, status_cnr, std::chrono::steady_clock::now() + std::chrono::seconds(1)) == 0);
    CHECK(status_reads == 2 && status_writes == std::vector<Bytes>({Bytes({0x90,1})}));
    CHECK(status_locked && status_cnr == 25000);
    Silicon bad_status([](std::uint8_t, const Bytes &, Bytes &read, Deadline) {
        if (!read.empty()) read[0] = 0xc0;
        return 0;
    }, [](unsigned) {});
    CHECK(bad_status.ReadLock(status_locked, status_cnr, std::chrono::steady_clock::now() + std::chrono::seconds(1)) != 0);

    const auto temp = std::filesystem::temp_directory_path() /
        ("t230c-rom-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    CHECK(std::filesystem::create_directory(temp));
    TunerFirmwarePaths paths{(temp/"rom60.fw").wstring(), (temp/"rom61.fw").wstring(), (temp/"fallback.fw").wstring()};
    auto save = [](const std::wstring &p, const Bytes &data) {
        std::ofstream f(std::filesystem::path(p), std::ios::binary);
        f.write(reinterpret_cast<const char *>(data.data()), data.size());
    };
    Bytes primary = firmware; primary[1] = 0x60;
    Bytes fallback = firmware; fallback[1] = 0xa1;
    save(paths.rom60, primary); save(paths.fallback, fallback);
    Bytes chosen; std::string select_error;
    CHECK(LoadTunerFirmware(0x60, paths, chosen, select_error) == 0 && chosen == primary);
    Bytes primary61 = firmware; primary61[1] = 0x61; save(paths.rom61, primary61);
    CHECK(LoadTunerFirmware(0x61, paths, chosen, select_error) == 0 && chosen == primary61);
    std::filesystem::remove(paths.rom61);
    CHECK(LoadTunerFirmware(0x61, paths, chosen, select_error) == 0 && chosen == fallback && select_error.empty());
    CHECK(LoadTunerFirmware(0x62, paths, chosen, select_error) == 0 && chosen == fallback);
    save(paths.rom61, invalid);
    CHECK(LoadTunerFirmware(0x61, paths, chosen, select_error) != 0);
    std::filesystem::remove(paths.rom61); std::filesystem::remove(paths.fallback);
    CHECK(LoadTunerFirmware(0x61, paths, chosen, select_error) != 0 && select_error.find("0x61") != std::string::npos);
    std::filesystem::remove(paths.rom60); std::filesystem::remove(temp);

    bool locked = false;
    std::int32_t cnr = 0;
    CHECK(silicon.ReadLock(locked, cnr, std::chrono::steady_clock::now() + std::chrono::seconds(1)) == 0);
    CHECK(locked && cnr == 25000);
    writes.clear();
    CHECK(silicon.Tune(473000, 5274, 6, std::chrono::steady_clock::now()) != 0);
    CHECK(writes.empty());

    Silicon stalled([](std::uint8_t, const Bytes &, Bytes &read, Deadline) {
        if (!read.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            read[0] = 0;
        }
        return 0;
    }, [](unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); });
    const auto begin = std::chrono::steady_clock::now();
    CHECK(stalled.Tune(473000, 5274, 6, begin + std::chrono::milliseconds(25)) != 0);
    CHECK(std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(250));

    // si2157.c:228-265: xtal 照会の失敗と CTS 後の power-up error は継続。
    Bytes last_tuner;
    int power_reads = 0;
    std::vector<Bytes> init_commands;
    Silicon cold([&](std::uint8_t address, const Bytes &write, Bytes &read, Deadline) {
        if (address == 0x60 && !write.empty()) { last_tuner = write; init_commands.push_back(write); }
        if (!read.empty()) {
            if (address == 0x60 && last_tuner == Bytes({0x15,0,2,4})) return -EIO;
            read[0] = 0x80;
            if (address == 0x60 && last_tuner == Bytes({0xc0,0,0x0d,0x0e,0,1,1,1,1,3}))
                read[0] = ++power_reads == 1 ? 0x40 : 0xc0;
            if (address == 0x60 && last_tuner == Bytes({2}) && read.size() == 13) {
                read[2] = 41; read[12] = 0x61;
            }
        }
        return 0;
    }, [](unsigned) {});
    std::uint8_t cold_rom = 0;
    CHECK(cold.Initialize(firmware, [&](std::uint8_t rom, Bytes &out, std::string &) {
        cold_rom = rom; out = firmware; return 0;
    }, firmware_error) == 0);
    CHECK(cold_rom == 0x61 && power_reads == 2);
    auto wake = std::find(init_commands.begin(), init_commands.end(), Bytes({0xc0,8,1,2,0,0,1}));
    auto part = std::find(init_commands.begin(), init_commands.end(), Bytes({2}));
    CHECK(wake != init_commands.end() && part != init_commands.end() && wake < part);
    firmware_error.clear();
    CHECK(cold.Initialize(firmware, [&](std::uint8_t rom, Bytes &out, std::string &detail) {
        return LoadTunerFirmware(rom, paths, out, detail);
    }, firmware_error) != 0);
    CHECK(firmware_error.find("0x61") != std::string::npos);

    // tune_wait は CTS の後に 0x81 を待つ。読出し失敗はカーネルと同じく選局を中断しない。
    for (bool wait_error : {false, true}) {
        int tune_phase = 0; int sleeps = 0; bool demod_after_wait = false;
        Silicon waiter([&](std::uint8_t address, const Bytes &write, Bytes &read, Deadline) {
            if (address == 0x60 && !write.empty() && write[0] == 0x41) tune_phase = 1;
            if (address == 0x64 && write == Bytes({0x88,2,2,2,2})) demod_after_wait = tune_phase >= 3;
            if (!read.empty()) {
                read[0] = 0x80;
                if (address == 0x60 && tune_phase == 1) { tune_phase = 2; }
                else if (address == 0x60 && tune_phase == 2) {
                    tune_phase = 3;
                    if (wait_error) return -EIO;
                } else if (address == 0x60 && tune_phase == 3) { read[0] = 0x81; tune_phase = 4; }
            }
            return 0;
        }, [&](unsigned ms) { if (ms == 5) ++sleeps; });
        CHECK(waiter.Tune(473000, 5274, 6, std::chrono::steady_clock::now() + std::chrono::seconds(1)) == 0);
        CHECK(demod_after_wait && (wait_error || (tune_phase == 4 && sleeps == 1)));
    }

    // TUNINT を待つ 40 ms は demod コマンド開始前に使い、要求全体の期限で打ち切る。
    for (bool short_deadline : {false, true}) {
        int phase = 0; bool demod_sent = false;
        Silicon timeout_waiter([&](std::uint8_t address, const Bytes &write, Bytes &read, Deadline) {
            if (address == 0x60 && !write.empty() && write[0] == 0x41) phase = 1;
            if (address == 0x64 && !write.empty()) demod_sent = true;
            if (!read.empty()) {
                read[0] = 0x80;
                if (address == 0x60 && phase == 1) phase = 2;
            }
            return 0;
        }, [](unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); });
        const auto start = std::chrono::steady_clock::now();
        const int ret = timeout_waiter.Tune(473000, 5274, 6,
            start + std::chrono::milliseconds(short_deadline ? 15 : 1000));
        const auto elapsed = std::chrono::steady_clock::now() - start;
        if (short_deadline) CHECK(ret != 0 && !demod_sent);
        else CHECK(ret == 0 && demod_sent && elapsed >= std::chrono::milliseconds(35));
        CHECK(elapsed < std::chrono::seconds(1));
    }

    // power-up の転送失敗は error bit 由来の -EAGAIN と区別して返す。
    Bytes failing_last;
    Silicon failing_power([&](std::uint8_t address, const Bytes &write, Bytes &read, Deadline) {
        if (address == 0x60 && !write.empty()) failing_last = write;
        if (!read.empty()) {
            if (address == 0x60 && failing_last == Bytes({0xc0,0,0x0d,0x0e,0,1,1,1,1,3})) return -EIO;
            read[0] = 0x80;
        }
        return 0;
    }, [](unsigned) {});
    CHECK(failing_power.Initialize(firmware, [&](std::uint8_t, Bytes &out, std::string &) {
        out = firmware; return 0;
    }, firmware_error) == -EIO);

    // 壊れた候補と端数を挟んでも、次の連続パケットへ復帰する。
    TsSynchronizer sync(188 * 8);
    Bytes output;
    auto emit = [&](const std::uint8_t *p) { output.insert(output.end(), p, p + 188); };
    Bytes one(188, 0x55); one[0] = 0x47;
    Bytes noise(188 * 6, 0x33); noise[0] = 0x47;
    for (int i = 0; i < 100; ++i) {
        sync.Push(noise.data(), noise.size(), emit);
        CHECK(sync.PendingSize() <= 188 * 8);
        CHECK(output.empty());
    }
    Bytes packets;
    for (int i = 0; i < 5; ++i) {
        Bytes packet(188, static_cast<std::uint8_t>(i)); packet[0] = 0x47;
        packets.insert(packets.end(), packet.begin(), packet.end());
    }
    sync.Push(packets.data(), 333, emit);
    sync.Push(packets.data() + 333, packets.size() - 333, emit);
    CHECK(output == packets);
    sync.Reset(); output.clear();
    sync.Push(one.data(), one.size(), emit);
    CHECK(output.empty() && sync.PendingSize() == 188);
    sync.Reset();
    CHECK(sync.PendingSize() == 0);
    return 0;
}
