#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <imgui.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

// Local A/B experiment. Selection is committed only at Present; it is deliberately
// not saved as a normal graphics preference. Profiling is entirely opt-in.
namespace lighting_experiment
{
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
inline std::atomic<bool> Enabled{false};
inline std::atomic<bool> Requested{false}, RecordRequested{false}, ResetRequested{false};
inline std::atomic<uint64_t> Epoch{1};
inline uint64_t CurrentEpoch() { return Epoch.load(); }
inline void Request(bool value) { Requested.store(value); }

struct Distribution
{
    static constexpr size_t Capacity = 600;
    std::array<double, Capacity> Values{};
    size_t Count = 0, Next = 0;
    uint64_t Total = 0;
    void Add(double value)
    {
        if (!std::isfinite(value) || value < 0) return;
        Values[Next] = value; Next = (Next + 1) % Capacity;
        Count = std::min(Count + 1, Capacity); ++Total;
    }
    double Mean() const
    {
        double sum = 0;
        for (size_t i = 0; i < Count; ++i) sum += Values[i];
        return Count ? sum / static_cast<double>(Count) : 0;
    }
    double P95() const
    {
        if (!Count) return 0;
        auto copy = Values;
        const size_t index = (Count * 95 + 99) / 100 - 1;
        std::nth_element(copy.begin(), copy.begin() + index, copy.begin() + Count);
        return copy[index];
    }
};
struct Results
{
    Distribution Gpu, Present, Lights, Transitions;
    UINT Width = 0, Height = 0;
    uint64_t Fallbacks = 0, Unavailable = 0;
};
inline std::mutex ResultsLock;
inline std::array<Results, 2> Modes;

// Buffer rows and flush once per second or when recording stops. No worker can
// outlive a dynamically unloaded add-on. Flush cost is logged separately; GPU
// query reads never flush or wait for GPU completion.
class CsvWriter
{
    FILE *File = nullptr;
    std::string Buffer;
    uint64_t Dropped = 0;
    bool Failed = false;
    Clock::time_point LastFlush{};
public:
    std::string Path, Error;
    bool Open()
    {
        Close(); Dropped = 0; Failed = false; Error.clear(); Path.clear();
        wchar_t executable[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
        if (!length || length >= std::size(executable)) { Error = "Cannot find the game directory"; return false; }
        SYSTEMTIME now{}; GetSystemTime(&now);
        wchar_t name[128]{};
        swprintf_s(name, L"lighting-%04u%02u%02u-%02u%02u%02u-%03u-%lu.csv", now.wYear,
            now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, GetCurrentProcessId());
        const auto folder = std::filesystem::path(executable).parent_path() / L"EQ2Graphics-captures" / L"lighting-comparisons";
        std::error_code error;
        std::filesystem::create_directories(folder, error);
        if (error) { Error = "Cannot create comparison log directory"; return false; }
        const auto path = folder / name;
        if (_wfopen_s(&File, path.c_str(), L"wx") != 0 || !File) { Error = "Cannot create comparison CSV"; return false; }
        Path = path.string(); LastFlush = Clock::now();
        Buffer = "utc,elapsed_ms,event,mode,epoch,frame,warmup,valid,gpu_lighting_ms,present_interval_ms,light_count,width,height,fallback,dropped_log_rows,settings_id,transition_lights\n";
        Buffer.reserve(256 * 1024);
        return true;
    }
    void Push(const std::string &row)
    {
        if (!File) return;
        if (Buffer.size() + row.size() > 1024 * 1024) { ++Dropped; return; }
        Buffer += row;
    }
    uint64_t Lost() const { return Dropped; }
    bool WriteFailed() const { return Failed; }
    double Flush(bool force = false)
    {
        const auto now = Clock::now();
        if (!File || Buffer.empty() || (!force && now - LastFlush < std::chrono::seconds(1))) return -1;
        if (fwrite(Buffer.data(), 1, Buffer.size(), File) != Buffer.size() || fflush(File) != 0) Failed = true;
        Buffer.clear(); LastFlush = Clock::now();
        return std::chrono::duration<double, std::milli>(LastFlush - now).count();
    }
    void Close()
    {
        Flush(true);
        if (File) { fclose(File); File = nullptr; }
        Buffer.clear();
    }
    ~CsvWriter() { Close(); }
};
inline CsvWriter Writer;
inline bool Recording = false;
inline uint64_t Frame = 0, WarmFrame = 0, Generation = 1;
inline uint64_t SettingsId = 0;
inline Clock::time_point SessionStart{}, WarmStart{}, PreviousPresent{};
inline bool SkipInterval = true;
inline bool Warming()
{
    return Frame - WarmFrame < 180 || Clock::now() - WarmStart < std::chrono::seconds(3);
}

inline void Log(const char *event, bool candidate, uint64_t epoch, uint64_t frame, bool warm,
    bool valid, double gpu, double present, UINT lights, UINT width, UINT height, bool fallback, uint64_t settings = SettingsId,
    UINT transitions = 0)
{
    if (!Recording) return;
    SYSTEMTIME utc{}; GetSystemTime(&utc);
    const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - SessionStart).count();
    char line[512]{};
    sprintf_s(line, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ,%.3f,%s,%s,%llu,%llu,%d,%d,%.6f,%.6f,%u,%u,%u,%d,%llu,%016llx,%u\n",
        utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds,
        elapsed, event, candidate ? "candidate" : "baseline", static_cast<unsigned long long>(epoch),
        static_cast<unsigned long long>(frame), warm ? 1 : 0, valid ? 1 : 0, gpu, present, lights, width, height,
        fallback ? 1 : 0, static_cast<unsigned long long>(Writer.Lost()), static_cast<unsigned long long>(settings), transitions);
    Writer.Push(line);
}

struct Query
{
    ComPtr<ID3D11Query> Disjoint, Begin, End;
    bool Pending = false, Candidate = false, Warm = false, Fallback = false;
    uint64_t ModeEpoch = 0, SampleFrame = 0, SampleGeneration = 0, Settings = 0;
    UINT Lights = 0, Width = 0, Height = 0, Transitions = 0;
};
inline std::array<Query, 8> Queries;
inline int ActiveQuery = -1;

inline void Poll(ID3D11DeviceContext *ctx)
{
    for (auto &query : Queries)
    {
        if (!query.Pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        UINT64 begin = 0, end = 0;
        const HRESULT a = ctx->GetData(query.Disjoint.Get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        const HRESULT b = ctx->GetData(query.Begin.Get(), &begin, sizeof(begin), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        const HRESULT c = ctx->GetData(query.End.Get(), &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (a == S_FALSE || b == S_FALSE || c == S_FALSE) continue;
        query.Pending = false;
        if (query.SampleGeneration != Generation) continue;
        const bool valid = SUCCEEDED(a) && SUCCEEDED(b) && SUCCEEDED(c) && !disjoint.Disjoint
            && disjoint.Frequency && end >= begin && !query.Fallback;
        const double ms = valid ? static_cast<double>(end - begin) * 1000.0 / static_cast<double>(disjoint.Frequency) : -1;
        if (valid && !query.Warm)
        {
            std::lock_guard lock(ResultsLock);
            auto &result = Modes[query.Candidate ? 1 : 0];
            result.Gpu.Add(ms); result.Lights.Add(query.Lights);
            result.Transitions.Add(query.Transitions);
            result.Width = query.Width; result.Height = query.Height;
        }
        Log("gpu", query.Candidate, query.ModeEpoch, query.SampleFrame, query.Warm, valid,
            ms, -1, query.Lights, query.Width, query.Height, query.Fallback, query.Settings, query.Transitions);
    }
}
inline void BeginPass(ID3D11DeviceContext *ctx, bool candidate, UINT lights, UINT width, UINT height, UINT transitions = 0)
{
    if (!Recording || ActiveQuery >= 0 || ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return;
    Poll(ctx);
    for (size_t i = 0; i < Queries.size(); ++i)
    {
        auto &query = Queries[i];
        if (query.Pending) continue;
        if (!query.Disjoint)
        {
            ComPtr<ID3D11Device> dev; ctx->GetDevice(&dev);
            const D3D11_QUERY_DESC disjoint{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, stamp{D3D11_QUERY_TIMESTAMP, 0};
            if (FAILED(dev->CreateQuery(&disjoint, &query.Disjoint)) || FAILED(dev->CreateQuery(&stamp, &query.Begin))
                || FAILED(dev->CreateQuery(&stamp, &query.End)))
            { query = {}; break; }
        }
        query.Candidate = candidate; query.Warm = Warming(); query.Fallback = false;
        query.ModeEpoch = CurrentEpoch(); query.SampleFrame = Frame; query.SampleGeneration = Generation;
        query.Settings = SettingsId;
        query.Lights = lights; query.Width = width; query.Height = height;
        query.Transitions = transitions;
        ctx->Begin(query.Disjoint.Get()); ctx->End(query.Begin.Get());
        ActiveQuery = static_cast<int>(i); return;
    }
    std::lock_guard lock(ResultsLock); ++Modes[candidate ? 1 : 0].Unavailable;
}
inline void EndPass(ID3D11DeviceContext *ctx)
{
    if (ActiveQuery < 0) return;
    auto &query = Queries[static_cast<size_t>(ActiveQuery)];
    ctx->End(query.End.Get()); ctx->End(query.Disjoint.Get());
    query.Pending = true; ActiveQuery = -1;
}
inline void RecordFallback()
{
    if (ActiveQuery >= 0) Queries[static_cast<size_t>(ActiveQuery)].Fallback = true;
    std::lock_guard lock(ResultsLock); ++Modes[1].Fallbacks;
}
inline void SaveSummary()
{
    if (Writer.Path.empty()) return;
    auto path = std::filesystem::path(Writer.Path); path.replace_extension("summary.csv");
    FILE *file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"w") != 0 || !file)
    { Writer.Error = "Cannot save comparison summary CSV"; return; }
    fputs("mode,settings_id,window_samples,gpu_total_samples,gpu_mean_ms,gpu_p95_ms,present_samples,present_mean_ms,present_p95_ms,mean_lights,width,height,fallbacks,unavailable_queries,dropped_log_rows,mean_transition_lights\n", file);
    std::lock_guard lock(ResultsLock);
    for (size_t i = 0; i < Modes.size(); ++i)
    {
        const auto &r = Modes[i];
        fprintf(file, "%s,%016llx,%zu,%llu,%.6f,%.6f,%zu,%.6f,%.6f,%.3f,%u,%u,%llu,%llu,%llu,%.3f\n",
            i ? "candidate" : "baseline", static_cast<unsigned long long>(SettingsId), r.Gpu.Count,
            static_cast<unsigned long long>(r.Gpu.Total), r.Gpu.Count ? r.Gpu.Mean() : -1,
            r.Gpu.Count ? r.Gpu.P95() : -1, r.Present.Count, r.Present.Count ? r.Present.Mean() : -1,
            r.Present.Count ? r.Present.P95() : -1, r.Lights.Count ? r.Lights.Mean() : -1, r.Width, r.Height,
            static_cast<unsigned long long>(r.Fallbacks), static_cast<unsigned long long>(r.Unavailable),
            static_cast<unsigned long long>(Writer.Lost()), r.Transitions.Count ? r.Transitions.Mean() : -1);
    }
    if (ferror(file)) Writer.Error = "Cannot write comparison summary CSV";
    if (fclose(file) != 0) Writer.Error = "Cannot finish comparison summary CSV";
}

// Called after upstream Present cleanup. Pending query slots are never reused
// until resolved, and delayed results retain the mode/generation of submission.
inline void Boundary(ID3D11DeviceContext *ctx, uint64_t settings = 0)
{
    if (!ctx || ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return;
    ++Frame;
    if (Recording) Poll(ctx);
    const auto now = Clock::now();
    if (settings != SettingsId)
    {
        SettingsId = settings; ++Generation;
        { std::lock_guard lock(ResultsLock); Modes = {}; }
        WarmStart = now; WarmFrame = Frame; SkipInterval = true;
        Log("settings_changed", Enabled.load(), CurrentEpoch(), Frame, true, false, -1, -1, 0, 0, 0, false);
    }
    if (Recording && PreviousPresent != Clock::time_point{} && !SkipInterval)
    {
        const double elapsed = std::chrono::duration<double, std::milli>(now - PreviousPresent).count();
        const bool valid = elapsed > 0 && elapsed < 1000;
        const bool warm = Warming(), candidate = Enabled.load();
        if (valid && !warm) { std::lock_guard lock(ResultsLock); Modes[candidate ? 1 : 0].Present.Add(elapsed); }
        Log("present", candidate, CurrentEpoch(), Frame, warm, valid, -1, elapsed, 0, 0, 0, false);
    }
    if (Recording)
    {
        const double flushMs = Writer.Flush();
        if (flushMs >= 0) Log("csv_flush_cpu_ms", Enabled.load(), CurrentEpoch(), Frame, Warming(), false, -1, flushMs, 0, 0, 0, false);
    }
    PreviousPresent = now; SkipInterval = false;
    if (ResetRequested.exchange(false))
    {
        { std::lock_guard lock(ResultsLock); Modes = {}; }
        ++Generation; WarmStart = now; WarmFrame = Frame; SkipInterval = true;
        Log("reset", Enabled.load(), CurrentEpoch(), Frame, true, false, -1, -1, 0, 0, 0, false);
    }
    if (Requested.load() != Enabled.load())
    {
        Enabled.store(Requested.load()); ++Epoch;
        WarmStart = now; WarmFrame = Frame; SkipInterval = true;
        Log("switch", Enabled.load(), CurrentEpoch(), Frame, true, false, -1, -1, 0, 0, 0, false);
    }
    if (RecordRequested.load() != Recording)
    {
        if (Recording)
        {
            Log("stop", Enabled.load(), CurrentEpoch(), Frame, Warming(), false, -1, -1, 0, 0, 0, false);
            SaveSummary(); Recording = false; Writer.Close(); ++Generation;
        }
        else
        {
            if (Writer.Open())
            {
                Recording = true; SessionStart = now; WarmStart = now; WarmFrame = Frame; ++Generation;
                { std::lock_guard lock(ResultsLock); Modes = {}; }
                Log("start", Enabled.load(), CurrentEpoch(), Frame, true, false, -1, -1, 0, 0, 0, false);
            }
            else RecordRequested = false;
        }
        SkipInterval = true;
    }
}
inline void Release()
{
    RecordRequested = false;
    if (Recording) Log("device_release", Enabled.load(), CurrentEpoch(), Frame, true, false, -1, -1, 0, 0, 0, false);
    if (Recording) SaveSummary();
    Recording = false; Writer.Close(); Queries = {}; ActiveQuery = -1; ++Generation;
    PreviousPresent = {}; SkipInterval = true; Enabled = false; Requested = false; ++Epoch;
}
inline void WriteState(FILE *file)
{
    fprintf(file, "lighting comparison: requested %d, active %d, epoch %llu, settings %016llx, recording %d, warmup %d\n",
        Requested.load() ? 1 : 0, Enabled.load() ? 1 : 0, static_cast<unsigned long long>(CurrentEpoch()),
        static_cast<unsigned long long>(SettingsId), Recording ? 1 : 0, Warming() ? 1 : 0);
    std::lock_guard lock(ResultsLock);
    for (size_t i = 0; i < Modes.size(); ++i)
        fprintf(file, "comparison %s: GPU mean %.6f ms p95 %.6f, samples %llu, Present mean %.6f ms p95 %.6f, fallback %llu\n",
            i ? "candidate" : "baseline", Modes[i].Gpu.Mean(), Modes[i].Gpu.P95(),
            static_cast<unsigned long long>(Modes[i].Gpu.Total), Modes[i].Present.Mean(), Modes[i].Present.P95(),
            static_cast<unsigned long long>(Modes[i].Fallbacks));
}
inline void DrawSettings()
{
    ImGui::SeparatorText("Lighting comparison");
    bool candidate = Requested.load();
    if (ImGui::Checkbox("Experimental improvements", &candidate)) Request(candidate);
    ImGui::TextWrapped("Unchecked: current 1.13.16 lighting. Checked: experimental source fixes, lighting and short shadow transitions. Switches at the next frame; caches rebuild. Selection resets when the game closes.");
    bool record = RecordRequested.load();
    if (ImGui::Checkbox("Record comparison performance (CSV)", &record)) RecordRequested = record;
    if (ImGui::Button("Clear comparison results")) ResetRequested = true;
    ImGui::TextWrapped("Compare the same scene and camera. Changing other hook settings clears both summaries. Minimum warm-up: 3 seconds and 180 frames after each switch. Wait for lights to settle; warm-up samples are logged but excluded below.");
    if (Recording) ImGui::TextUnformatted(Warming() ? "Recording: warming up" : "Recording: collecting samples");
    if (!Writer.Error.empty()) ImGui::TextWrapped("Log error: %s", Writer.Error.c_str());
    if (Writer.WriteFailed()) ImGui::TextUnformatted("CSV write failed; check disk space and access.");
    if (!Writer.Path.empty()) { ImGui::TextWrapped("Log: %s", Writer.Path.c_str()); if (ImGui::Button("Copy log path")) ImGui::SetClipboardText(Writer.Path.c_str()); }
    ImGui::TextWrapped("Stopping recording also saves a .summary.csv beside the detailed log.");
    if (Writer.Lost()) ImGui::Text("Dropped log rows: %llu", static_cast<unsigned long long>(Writer.Lost()));
    std::array<Results, 2> results;
    { std::lock_guard lock(ResultsLock); results = Modes; }
    if (ImGui::BeginTable("LightingAB", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("Metric"); ImGui::TableSetupColumn("Current 1.13.16"); ImGui::TableSetupColumn("Candidate"); ImGui::TableHeadersRow();
        const auto row = [&](const char *label, auto value) {
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(label);
            for (const auto &result : results) { ImGui::TableNextColumn(); value(result); }
        };
        row("GPU lighting mean / p95 (ms)", [](const Results &r) { if (r.Gpu.Count) ImGui::Text("%.3f / %.3f", r.Gpu.Mean(), r.Gpu.P95()); else ImGui::TextUnformatted("--"); });
        row("Game Present mean / p95 (ms)", [](const Results &r) { if (r.Present.Count) ImGui::Text("%.3f / %.3f", r.Present.Mean(), r.Present.P95()); else ImGui::TextUnformatted("--"); });
        row("GPU samples (total)", [](const Results &r) { ImGui::Text("%llu", static_cast<unsigned long long>(r.Gpu.Total)); });
        row("Mean submitted lights", [](const Results &r) { if (r.Lights.Count) ImGui::Text("%.1f", r.Lights.Mean()); else ImGui::TextUnformatted("--"); });
        row("Mean transitioning lights", [](const Results &r) { if (r.Transitions.Count) ImGui::Text("%.1f", r.Transitions.Mean()); else ImGui::TextUnformatted("--"); });
        row("Lighting resolution", [](const Results &r) { ImGui::Text("%u x %u", r.Width, r.Height); });
        row("Fallbacks / unavailable queries", [](const Results &r) { ImGui::Text("%llu / %llu", static_cast<unsigned long long>(r.Fallbacks), static_cast<unsigned long long>(r.Unavailable)); });
        ImGui::EndTable();
    }
    ImGui::TextWrapped("Statistics use the latest 600 valid samples per mode. GPU lighting includes candidate list construction, shading and upsampling; it excludes source measurement and shadow-map generation. Game Present intervals include other game work and frame caps; they are not display/output FPS. CSV uses -1 for an inapplicable metric. Buffered CSV writes occur once per second; their CPU cost is logged as csv_flush_cpu_ms.");
}
}
