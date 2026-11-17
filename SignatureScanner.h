#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class WmPortReader;

// Read-only scanner for a mapped x64 PE image, not a DLL file on disk.
// Use on the initialization/diagnostic path, never on every overlay frame.
class SignatureScanner final {
public:
    using Address = std::uint64_t;
    enum class Region { Data, WritableData, Executable, Image, Text };

    struct Section {
        std::string name;
        Address address{};
        std::uint32_t size{};
        std::uint32_t characteristics{};
    };

    struct ScanResult {
        std::vector<Address> hits; // Absolute virtual addresses, NOT RVAs.
        std::uint64_t readable_bytes{};
        std::uint64_t unreadable_bytes{};
        bool truncated{}; // Hit limit reached: uniqueness is not established.
        std::uint32_t error{}; // Win32 error; zero does not imply every page was readable.
    };

    struct GomCandidate {
        Address slot{};         // Module global containing the manager pointer.
        Address rva{};          // slot - module_base; usable as GOM offset.
        Address manager{};      // Current heap/native address; do NOT cache across launches.
        Address entry_object{}; // Native GameObject owning a managed UnityEntry component.
        Address first_offset{};
        std::size_t visited_nodes{};
        std::string entry_name; // Matched managed class name, always exactly UnityEntry.
        Address entry_component{}; // Managed object: +0 klass, +0x28 Game.
    };

    struct GomOptions {
        Region region{Region::Data};
        std::size_t max_candidates{16};
        std::size_t max_nodes_per_list{16384};
        std::size_t max_manager_probes{65536};
        std::size_t max_total_nodes{250000};
        // Compatibility only: no GameObject name is used as an acceptance gate.
        // The target component class is hardcoded to exactly UnityEntry.
        std::vector<std::string> entry_names{};
        // Each first offset is preceded by its live last-node field at offset -8.
        std::vector<Address> list_first_offsets{0x18, 0x28, 0x08};
        Address game_object_name_offset{0x50};
        std::size_t max_name_bytes{128}; // Includes terminating NUL.
        std::size_t dump_name_limit{20}; // First distinct readable GameObject addresses.
        std::size_t max_components_per_object{256};
        std::size_t max_component_slots{1000000}; // Total examined slots per scan.
        bool log_component_classes{true}; // Every checked candidate, no name log cap.
    };

    struct ObjectName {
        Address manager{};
        Address node{};
        Address object{};
        Address name_address{};
        Address first_offset{};
        std::string name;
    };

    struct GomResult {
        std::vector<GomCandidate> candidates;
        std::uint64_t readable_bytes{};
        std::uint64_t unreadable_bytes{};
        std::size_t manager_probes{};
        std::size_t visited_nodes{};
        std::size_t readable_names{};
        std::size_t name_read_failures{};
        std::vector<ObjectName> sampled_names; // Available even when no target name matches.
        std::size_t rip_loads{}; // Only populated by the .text-based GOM search.
        std::size_t slot_read_failures{};
        std::size_t component_slots{};
        std::size_t readable_component_classes{};
        std::size_t component_class_failures{};
        bool limited{}; // Budget reached, including a per-list traversal budget.
        std::uint32_t error{};
    };

    struct RipLoad {
        Address instruction{};
        Address slot{};
        Address rva{}; // Slot RVA, not the instruction RVA.
        Address value{};
        bool loads_rcx{}; // false: 48 8B 05 (rax); true: 48 8B 0D (rcx).
        bool value_readable{};
        std::uint32_t read_error{};
    };

    struct RipLoadResult {
        std::vector<RipLoad> loads; // Retains multiple code references to the same slot.
        std::size_t byte_matches{}; // Both opcode forms, before target-section filtering.
        std::size_t slot_read_failures{};
        std::uint64_t readable_bytes{}; // .text coverage, not .data coverage.
        std::uint64_t unreadable_bytes{};
        bool limited{};
        std::uint32_t error{};
    };

    SignatureScanner(WmPortReader& reader, std::uint32_t pid, Address module_base);
    [[nodiscard]] bool initialize(); // Reads/validates DOS, PE32+ and section headers.

    [[nodiscard]] Address module_base() const noexcept { return base_; }
    [[nodiscard]] std::uint32_t image_size() const noexcept { return image_size_; }
    [[nodiscard]] std::uint32_t timestamp() const noexcept { return timestamp_; }
    [[nodiscard]] const std::vector<Section>& sections() const noexcept { return sections_; }

    // Space-separated hex bytes, with ? or ?? wildcards. No invented signature
    // is built in; an actual signature must come from the matching Unity build.
    [[nodiscard]] ScanResult scan_pattern(std::string_view pattern,
                                         Region region = Region::Executable,
                                         std::size_t max_hits = 128) const;
    [[nodiscard]] ScanResult scan_string(std::string_view text, Region region = Region::Image,
                                        std::size_t max_hits = 128,
                                        bool include_nul = true) const;
    [[nodiscard]] ScanResult scan_pointer(Address target, Region region = Region::Data,
                                         std::size_t max_hits = 128) const;

    // target = instruction + instruction_size + sign_extend(disp32).
    // Instruction encoding and displacement location must be verified externally.
    [[nodiscard]] std::optional<Address> resolve_rip_relative(
        Address instruction, std::size_t displacement_offset,
        std::size_t instruction_size) const;

    // Structural candidates: global -> GOM lists -> any GameObject -> its
    // components -> managed component with exact class name UnityEntry.
    // Native offsets match the current parser and remain Unity-version-specific.
    [[nodiscard]] GomResult find_gom_candidates(const GomOptions& options) const;
    [[nodiscard]] GomResult find_gom_candidates() const { return find_gom_candidates(GomOptions{}); }

    // Byte candidates, NOT decoded instruction boundaries or a GOM signature.
    // Scans exactly executable .text, filters slots fully contained in .data
    // (or writable non-executable sections when WritableData is explicitly set).
    [[nodiscard]] RipLoadResult scan_rip_loads(Region target_region = Region::Data,
                                              std::size_t max_loads = 8192,
                                              std::size_t log_limit = 20) const;
    [[nodiscard]] GomResult find_gom_candidates_from_text(const GomOptions& options,
                                                         std::size_t max_loads = 8192) const;
    [[nodiscard]] GomResult find_gom_candidates_from_text() const {
        return find_gom_candidates_from_text(GomOptions{});
    }

private:
    struct Stats {
        std::uint64_t readable{};
        std::uint64_t unreadable{};
        std::uint32_t error{};
    };

    using Visitor = std::function<bool(Address, std::span<const std::uint8_t>)>;
    [[nodiscard]] bool visit(Region region, const Visitor& visitor, Stats& stats) const;
    [[nodiscard]] ScanResult scan_bytes(const std::vector<int>& pattern, Region region,
                                       std::size_t max_hits, std::size_t alignment) const;
    [[nodiscard]] bool read(Address address, void* out, std::size_t size) const;
    [[nodiscard]] bool pointer(Address address) const noexcept;
    [[nodiscard]] bool contains(Region region, Address address, std::size_t size) const;
    [[nodiscard]] std::optional<std::string> read_name(Address address, std::size_t max_bytes) const;
    [[nodiscard]] bool valid_gom_options(const GomOptions& options) const;
    [[nodiscard]] bool entry_component_class(Address object, std::uint32_t index,
                                              Address component, const char* source,
                                              const GomOptions& options, GomResult& result) const;
    [[nodiscard]] std::optional<Address> find_entry_component(
        Address object, const GomOptions& options, GomResult& result) const;
    void finish_gom_result(const GomResult& result, const char* source) const;
    [[nodiscard]] bool record_candidate(Address slot, const GomCandidate& candidate,
                                        const GomOptions& options, GomResult& result) const;
    [[nodiscard]] std::optional<GomCandidate> probe_gom(
        Address manager, const GomOptions& options, GomResult& result) const;
    void log(const char* stage, Address address, std::uint32_t error = 0) const;

    WmPortReader& reader_;
    std::uint32_t pid_{};
    Address base_{};
    Address min_{};
    Address max_{};
    std::uint32_t page_size_{};
    std::uint32_t image_size_{};
    std::uint32_t timestamp_{};
    std::vector<Section> sections_;
};
