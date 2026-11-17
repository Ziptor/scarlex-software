#include "runtime_log.h"
#include "game_entities.h"
#include "wmport_reader.h"
#include "ItemPresentation.h"
#include "game_data.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace game_entities {
namespace {

using Address = std::uint64_t;
using Entry = Dictionary::Entry;
// Confirmed build: +0xD8 is PaymentsProcessor, not an alternate GameModel.
constexpr Address game_model_offset = 0xD0;
constexpr std::uint64_t array_length_offset = 0x18;
constexpr std::uint64_t array_data_offset = 0x20;
constexpr std::uint64_t max_dictionary_capacity = 65536;
constexpr std::int32_t max_transform_count = 65536;
constexpr float estimated_standing_height = 1.8f;
using Quaternion = std::array<float, 4>;  // x, y, z, w
constexpr Quaternion identity_rotation{0.0f, 0.0f, 0.0f, 1.0f};

struct TransformAccess {
    Address hierarchy;
    std::int32_t index;
    std::uint32_t padding;
};

struct HierarchyArrays {
    Address local_trs;
    Address parents;
    Address unused;
    Address transforms;
    bool operator==(const HierarchyArrays&) const = default;
};

struct LocalTrs {
    Vec3 translation;
    float translation_padding;
    Quaternion rotation;
    Vec3 scale;
    float scale_padding;
};

static_assert(sizeof(void*) == 8, "GameEntities targets x64 Windows");
static_assert(sizeof(Vec3) == 0xC);
static_assert(sizeof(Dictionary) == 0x20 && offsetof(Dictionary, count) == 0x10);
static_assert(sizeof(Entry) == 0x18 && offsetof(Entry, value) == 0x10);
static_assert(sizeof(TransformAccess) == 0x10);
static_assert(sizeof(HierarchyArrays) == 0x20 && sizeof(LocalTrs) == 0x30);
static_assert(offsetof(LocalTrs, rotation) == 0x10 && offsetof(LocalTrs, scale) == 0x20);

struct HierarchyHeader {
    std::int32_t count;
    std::uint32_t padding;
    HierarchyArrays arrays;
    bool same_layout(const HierarchyHeader& other) const {
        return count == other.count && arrays == other.arrays;
    }
};
static_assert(offsetof(HierarchyHeader, arrays) == 8);

struct PoseCache {
    struct RoleDescription {
        CharacterRole role;
        std::string id;
    };
    struct HierarchySample {
        HierarchyHeader header{};
        HierarchyHeader before{}, after{};
        std::vector<LocalTrs> locals;
        std::vector<std::int32_t> parents;
        bool sampled{};
        bool valid{};
    };

    std::unordered_map<Address, Address> references;
    std::vector<Address> reference_slots;
    std::size_t reference_cursor{};
    std::unordered_map<Address, std::string_view> classes;
    std::unordered_map<Address, RoleDescription> roles;
    std::unordered_map<Address, TransformAccess> transforms;
    std::unordered_map<Address, HierarchySample> hierarchies;
    std::unordered_map<Address, std::array<Address, 3>> limb_transforms;
    bool stale{};

    void start_frame() {
        for (auto& [address, sample] : hierarchies) {
            sample.sampled = false;
        }
        stale = false;
    }
};

class Remote final {
public:
    Remote(WmPortReader& reader, std::uint32_t pid, bool diagnostics = true)
        : reader_(reader), pid_(pid), diagnostics_(diagnostics && scarlex::build::diagnostics) {
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        min_ = reinterpret_cast<std::uintptr_t>(info.lpMinimumApplicationAddress);
        max_ = reinterpret_cast<std::uintptr_t>(info.lpMaximumApplicationAddress);
        page_size_ = info.dwPageSize;
    }

    ~Remote() {
        if (!complete_) {
            const DWORD error = last_error_;
            detail("FAIL", "Update FAILED: last_stage=%s addr=0x%llX err=%lu",
                   last_stage_, static_cast<unsigned long long>(last_address_),
                   static_cast<unsigned long>(error));
            if (diagnostics_) {
                std::fflush(stdout);
            }
            SetLastError(error);
        }
    }

    // Failed candidate probes are not fatal until all candidates are exhausted.
    class Probe final {
    public:
        explicit Probe(const Remote& remote) : remote_(remote), previous_(remote.probing_) {
            remote_.probing_ = true;
        }
        ~Probe() { remote_.probing_ = previous_; }
        Probe(const Probe&) = delete;
        Probe& operator=(const Probe&) = delete;
    private:
        const Remote& remote_;
        bool previous_;
    };

    void phase(const char* stage) { phase_ = stage; }
    void use_pose_cache(PoseCache& cache) { pose_ = &cache; }
    [[nodiscard]] PoseCache* pose_cache() const { return pose_; }

    void complete(std::size_t entities, std::size_t vehicles, std::size_t heads) {
        complete_ = true;
        detail("OK", "Update OK: entities=%zu vehicles=%zu valid_heads=%zu", entities, vehicles, heads);
        if (diagnostics_) {
            std::fflush(stdout);
        }
        SetLastError(ERROR_SUCCESS);
    }

    [[nodiscard]] bool fail(const char* stage, Address address, DWORD error) const {
        last_stage_ = stage;
        last_address_ = address;
        last_error_ = error;
        log(stage, address, error, probing_ ? "REJECT" : "FAIL");
        SetLastError(error);
        return false;
    }

    template<class... Args>
    void detail(const char* status, const char* format, Args... args) const {
        if (!diagnostics_) {
            return;
        }
        const DWORD error = GetLastError();
        scarlex::diagnostic_printf("[GameEntities pid=%lu phase=%s] [%s] ",
                    static_cast<unsigned long>(pid_), phase_, status);
        scarlex::diagnostic_printf(format, args...);
        scarlex::diagnostic_printf("\n");
        SetLastError(error);
    }

    void log(const char* stage, Address address, DWORD error = ERROR_SUCCESS,
             const char* status = "OK") const {
        detail(status, "%s addr=0x%llX err=%lu", stage,
               static_cast<unsigned long long>(address), static_cast<unsigned long>(error));
    }

    void link(const char* stage, Address base, Address offset, Address target,
              const char* status = "OK") const {
        detail(status, "%s base=0x%llX offset=0x%llX -> 0x%llX", stage,
               static_cast<unsigned long long>(base), static_cast<unsigned long long>(offset),
               static_cast<unsigned long long>(target));
    }

    [[nodiscard]] bool pointer(Address address) const {
        return address != 0 && address >= min_ && address <= max_ && address % 8 == 0;
    }

    [[nodiscard]] bool bytes(Address base, Address offset, void* out,
                             std::size_t size, const char* stage) const {
        if (offset > max_ || base > max_ - offset) {
            return fail(stage, base, ERROR_INVALID_ADDRESS);
        }
        const Address address = base + offset;
        if (address < min_ || size == 0 || size - 1 > max_ - address) {
            return fail(stage, address, ERROR_INVALID_ADDRESS);
        }
        if (!reader_.read(pid_, address, out, size)) {
            const DWORD error = GetLastError();
            return fail(stage, address, error == ERROR_SUCCESS ? ERROR_READ_FAULT : error);
        }
        return true;
    }

    template<class T>
    [[nodiscard]] bool value(Address base, Address offset, T& out, const char* stage) const {
        return bytes(base, offset, &out, sizeof(out), stage);
    }

    [[nodiscard]] bool ptr(Address base, Address offset, Address& out, const char* stage,
                           bool nullable = false) const {
        out = 0;
        Address candidate{};
        if (!value(base, offset, candidate, stage)) {
            detail(probing_ ? "REJECT" : "FAIL", "%s base=0x%llX offset=0x%llX -> unread",
                   stage, static_cast<unsigned long long>(base), static_cast<unsigned long long>(offset));
            return false;
        }
        if (pointer(candidate) || (nullable && candidate == 0)) {
            out = candidate;
            return true;
        }
        link(stage, base, offset, candidate, probing_ ? "REJECT" : "FAIL");
        return fail(stage, base + offset, ERROR_INVALID_ADDRESS);
    }

    [[nodiscard]] bool name_is(Address name, std::string_view expected, bool& match,
                               const char* stage) const {
        match = false;
        std::array<char, 64> data{};
        const std::size_t size = expected.size() + 1;
        if (size > data.size() || page_size_ == 0 || name < min_ || name > max_ ||
            size - 1 > max_ - name) {
            return fail(stage, name, ERROR_INVALID_ADDRESS);
        }
        for (std::size_t done = 0; done < size;) {
            const auto in_page = static_cast<std::size_t>(page_size_ - (name + done) % page_size_);
            const std::size_t part = (std::min)(size - done, in_page);
            if (!bytes(name, done, data.data(), part, stage)) {
                return false;
            }
            for (std::size_t i = 0; i < part; ++i) {
                const char expected_char = done + i == expected.size() ? '\0' : expected[done + i];
                if (data[i] != expected_char) {
                    return true;
                }
            }
            done += part;
        }
        match = true;
        return true;
    }

    [[nodiscard]] bool class_has_name(Address klass, std::string_view expected, bool& match,
                                     const char* stage) const {
        Address name{};
        // Native strings can be byte-aligned.
        return value(klass, 0x10, name, stage) && name_is(name, expected, match, stage);
    }

    [[nodiscard]] bool is_class(Address object, std::string_view expected, bool& match,
                                const char* stage) const {
        Address klass{};
        return ptr(object, 0, klass, stage) && class_has_name(klass, expected, match, stage);
    }

    [[nodiscard]] bool require_class(Address object, std::string_view expected,
                                     const char* stage) const {
        bool match{};
        if (!is_class(object, expected, match, stage)) {
            return false;
        }
        if (match) {
            return true;
        }
        if (!diagnostics_) {
            return fail(stage, object, ERROR_INVALID_DATA);
        }
        Address klass{}, name{};
        std::array<char, 96> actual{};
        if (ptr(object, 0, klass, "Mismatch class") && value(klass, 0x10, name, "Mismatch class name")) {
            // Only used on rejection; byte reads avoid crossing a string's page.
            for (std::size_t i = 0; i + 1 < actual.size(); ++i) {
                if (!value(name, i, actual[i], "Mismatch class name text") || actual[i] == '\0') {
                    break;
                }
            }
        }
        detail(probing_ ? "REJECT" : "FAIL", "%s expected=%.*s actual=%s object=0x%llX", stage,
               static_cast<int>(expected.size()), expected.data(), actual.data(),
               static_cast<unsigned long long>(object));
        return fail(stage, object, ERROR_INVALID_DATA);
    }

    [[nodiscard]] bool pose_ptr(Address base, Address offset, Address& out, const char* stage,
                                bool nullable = false) const {
        if (!pose_) {
            return ptr(base, offset, out, stage, nullable);
        }
        const Address slot = base + offset;
        if (const auto hit = pose_->references.find(slot); hit != pose_->references.end()) {
            out = hit->second;
            return nullable || out != 0 || fail(stage, slot, ERROR_INVALID_ADDRESS);
        }
        if (!ptr(base, offset, out, stage, nullable)) {
            return false;
        }
        remember_pose_reference(slot, out);
        return true;
    }

    void remember_pose_reference(Address slot, Address value) const {
        if (pose_ && pose_->references.emplace(slot, value).second) {
            pose_->reference_slots.push_back(slot);
        }
    }

    [[nodiscard]] bool pose_class(Address object, std::string_view type, const char* stage) const {
        if (pose_) {
            const auto hit = pose_->classes.find(object);
            if (hit != pose_->classes.end() && hit->second == type) {
                return true;
            }
        }
        if (!require_class(object, type, stage)) {
            return false;
        }
        if (pose_) {
            pose_->classes.insert_or_assign(object, type);
        }
        return true;
    }

    void batch(std::span<WmPortReader::ReadRange> ranges) const {
        reader_.read_batch(pid_, ranges);
    }

    [[nodiscard]] bool trace(Address base, Address offset, Address& out, const char* type) const {
        if (!ptr(base, offset, out, type)) {
            return false;
        }
        link(type, base, offset, out, "PTR");
        if (!require_class(out, type, type)) {
            return false;
        }
        log(type, out);
        return true;
    }

private:
    WmPortReader& reader_;
    std::uint32_t pid_;
    bool diagnostics_;
    PoseCache* pose_{};
    Address min_{};
    Address max_{};
    std::uint32_t page_size_{};
    const char* phase_{"Init"};
    mutable const char* last_stage_{"Init"};
    mutable Address last_address_{};
    mutable DWORD last_error_{ERROR_GEN_FAILURE};
    mutable bool probing_{};
    bool complete_{};
};

bool find_entry_component(const Remote& remote, Address object, Address& managed);

bool find_entry_object(const Remote& remote, Address gom, Address& object, Address& managed) {
    object = 0;
    managed = 0;
    // Each pair is {last, first}, NOT {sentinel, first}. The last node is live.
    // These native layouts are version-specific, not guaranteed by IL2CPP.
    constexpr std::array<Address, 3> heads{0x18, 0x28, 0x08};
    for (const Address offset : heads) {
        const Remote::Probe probe(remote);
        Address node{}, last{};
        if (!remote.ptr(gom, offset, node, "GOM list head", true) ||
            !remote.ptr(gom, offset - 8, last, "GOM list last", true)) {
            continue;
        }
        remote.detail("PROBE", "GOM list first_offset=0x%llX first=0x%llX last=0x%llX",
                      static_cast<unsigned long long>(offset), static_cast<unsigned long long>(node),
                      static_cast<unsigned long long>(last));
        std::unordered_set<Address> seen;
        for (; node != 0;) {
            if (seen.size() == 16384 || !seen.insert(node).second) {
                (void)remote.fail("GOM list cycle/traversal limit", node, ERROR_INVALID_DATA);
                break;
            }
            Address candidate{}, candidate_component{};
            if (remote.ptr(node, 0x10, candidate, "GOM node GameObject")) {
                remote.detail("NODE", "first_offset=0x%llX node=0x%llX object=0x%llX; checking all components",
                              static_cast<unsigned long long>(offset), static_cast<unsigned long long>(node),
                              static_cast<unsigned long long>(candidate));
                // UnityEntry is a managed component class, NOT GameObject.name.
                // Do not read object +0x50 or gate the component scan on any name.
                if (find_entry_component(remote, candidate, candidate_component)) {
                    object = candidate;
                    managed = candidate_component;
                    remote.log("GameObject owning component class UnityEntry", object);
                    return true;
                }
            }
            if (node == last) {
                break;
            }
            Address next{};
            if (!remote.ptr(node, 0x08, next, "GOM next node", true)) {
                break;
            }
            node = next;
        }
        remote.detail("PROBE", "GOM list visited=%zu; component class UnityEntry not found", seen.size());
    }
    return remote.fail("Component class UnityEntry missing in all GOM lists", gom, ERROR_NOT_FOUND);
}

bool is_entry_candidate(const Remote& remote, Address object, std::uint32_t index,
                        Address candidate, const char* source, bool& match) {
    match = false;
    if (!remote.pointer(candidate)) {
        return true;
    }
    Address klass{};
    if (!remote.ptr(candidate, 0, klass, "Component candidate class")) {
        remote.detail("PROBE", "object=0x%llX index=%lu source=%s component=0x%llX class=<unreadable/invalid>",
                      static_cast<unsigned long long>(object), static_cast<unsigned long>(index), source,
                      static_cast<unsigned long long>(candidate));
        return false;
    }
    // Keep Remote unchanged. Read class text here only for diagnostics; acceptance
    // still uses Remote::class_has_name with an exact case-sensitive comparison.
    Address name_address{};
    std::array<char, 128> name{};
    bool complete{};
    if (remote.value(klass, 0x10, name_address, "Component class name pointer")) {
        for (std::size_t i = 0; i < name.size(); ++i) {
            unsigned char byte{};
            if (!remote.value(name_address, i, byte, "Component class name text") ||
                (byte != 0 && (byte < 0x20 || byte == 0x7F))) break;
            if (byte == 0) { complete = true; break; }
            if (i + 1 < name.size()) name[i] = static_cast<char>(byte);
        }
    }
    remote.detail("CLASS", "object=0x%llX index=%lu source=%s component=0x%llX klass=0x%llX class=\"%s\" expected=\"UnityEntry\"",
                  static_cast<unsigned long long>(object), static_cast<unsigned long>(index), source,
                  static_cast<unsigned long long>(candidate), static_cast<unsigned long long>(klass),
                  complete ? name.data() : "<unreadable/invalid/truncated>");
    if (!complete) return false;
    return remote.class_has_name(klass, "UnityEntry", match, "Component candidate class name");
}

bool find_entry_component(const Remote& remote, Address object, Address& managed) {
    managed = 0;
    Address components{};
    std::uint32_t count{};
    if (!remote.value(object, 0x30, count, "GameObject component count")) {
        return false;
    }
    if (count > 256) {
        return remote.fail("GameObject component count", object, ERROR_INVALID_DATA);
    }
    if (count == 0) {
        remote.detail("PROBE", "GameObject object=0x%llX has no components", static_cast<unsigned long long>(object));
        return false;
    }
    if (!remote.ptr(object, 0x20, components, "GameObject components")) return false;
    remote.detail("OK", "GameObject components=0x%llX count=%lu",
                  static_cast<unsigned long long>(components), static_cast<unsigned long>(count));
    for (std::uint32_t i = 0; i < count; ++i) {
        const Remote::Probe probe(remote);
        Address component{};
        if (!remote.ptr(components, 0x10ULL * i + 8, component, "Native component", true) || component == 0) {
            continue;
        }
        remote.detail("PROBE", "Component index=%lu native=0x%llX",
                      static_cast<unsigned long>(i), static_cast<unsigned long long>(component));
        Address tagged{}, candidate{};
        bool match{};
        // Some supplied layouts expose managed references directly in the slots.
        // If this is a native component, +0 is a vtable, NOT an Il2CppClass:
        // only a readable, exactly named IL2CPP class may pass this check.
        if (is_entry_candidate(remote, object, i, component, "array-slot", match) && match) {
            managed = component;
            remote.log("Component UnityEntry (managed array slot)", managed);
            return true;
        }
        // Not all components are MonoBehaviours. Probe failures must not prevent
        // testing the alternate reference form or the remaining components.
        if (remote.value(component, 0x18, tagged, "Native component tagged reference")) {
            const Address reference = tagged & ~Address{7};
            if (remote.pointer(reference) &&
                remote.value(reference, 0, candidate, "Indirect managed component") &&
                is_entry_candidate(remote, object, i, candidate, "native+0x18 indirect", match) && match) {
                managed = candidate;
                remote.log("Component UnityEntry (indirect)", managed);
                return true;
            }
        }
        if (remote.value(component, 0x28, candidate, "Direct managed component") &&
            is_entry_candidate(remote, object, i, candidate, "native+0x28 direct", match) && match) {
            managed = candidate;
            remote.log("Component UnityEntry (direct)", managed);
            return true;
        }
    }
    return remote.fail("Component UnityEntry missing", object, ERROR_NOT_FOUND);
}

bool find_view_updater(const Remote& remote, Address game, Address battle, Address entities,
                       Address& result, Address& battle_updater) {
    Address processor{}, manager{}, screen{};
    if (!remote.trace(game, 0x90, processor, "StartGameProcessor") ||
        !remote.trace(processor, 0x20, manager, "ScreenManager") ||
        !remote.ptr(manager, 0x50, screen, "ScreenManager._currentUpdater")) {
        return false;
    }
    bool start_screen{};
    if (!remote.is_class(screen, "StartGameScreenUpdater", start_screen, "Current screen class")) {
        return false;
    }
    if (start_screen && !remote.ptr(screen, 0xB8, screen, "StartGameScreenUpdater._battleUpdaters")) {
        return false;
    }
    Address owner_battle{}, owner_entities{};
    if (!remote.require_class(screen, "BattleScreenUpdater", "BattleScreenUpdater class") ||
        !remote.trace(screen, 0x18, battle_updater, "BattleUpdater") ||
        !remote.trace(battle_updater, 0x90, result, "BattleEntitiesUpdater") ||
        !remote.ptr(result, 0x70, owner_battle, "BattleEntitiesUpdater._battleModel") ||
        !remote.ptr(result, 0x18, owner_entities, "BattleEntitiesUpdater._battleEntitiesModel")) {
        return false;
    }
    return (owner_battle == battle && owner_entities == entities) ||
           remote.fail("BattleEntitiesUpdater owner changed", result, ERROR_RETRY);
}

struct ReferenceEntry {
    std::uint32_t hash_code;
    std::int32_t next;
    Address key, value;
};
static_assert(sizeof(ReferenceEntry) == sizeof(Entry));

template<class EntryType>
struct DictionarySnapshotFor {
    Address address{};
    Dictionary header{};
    std::vector<EntryType> live;
};
using DictionarySnapshot = DictionarySnapshotFor<Entry>;
using ReferenceDictionarySnapshot = DictionarySnapshotFor<ReferenceEntry>;

template<class EntryType>
bool collect_live(const std::vector<std::int32_t>& buckets, const std::vector<EntryType>& entries,
                  std::size_t expected, bool one_based, std::vector<bool>& seen) {
    seen.assign(entries.size(), false);
    std::size_t count = 0;
    const std::int32_t empty = one_based ? 0 : -1;
    for (const std::int32_t bucket : buckets) {
        if (bucket == empty) {
            continue;
        }
        if (bucket < 0) {
            return false;
        }
        std::int32_t index = bucket - (one_based ? 1 : 0);
        do {
            const auto slot = static_cast<std::size_t>(index);
            if (index < 0 || slot >= entries.size() || seen[slot] || count == expected) {
                return false;
            }
            if (!one_based && (entries[slot].hash_code & 0x80000000U) != 0) {
                return false;
            }
            seen[slot] = true;
            ++count;
            index = entries[slot].next;
        } while (index != -1);
    }
    return count == expected;
}

template<class EntryType>
bool recheck_dictionary(const Remote& remote, const DictionarySnapshotFor<EntryType>& snapshot, const char* stage) {
    Dictionary current{};
    if (!remote.value(snapshot.address, 0x10, current, stage)) {
        return false;
    }
    if (current == snapshot.header) {
        return true;
    }
    remote.detail("DATA", "%s old_count=%ld new_count=%ld old_version=%ld new_version=%ld",
                  stage, static_cast<long>(snapshot.header.count), static_cast<long>(current.count),
                  static_cast<long>(snapshot.header.version), static_cast<long>(current.version));
    return remote.fail(stage, snapshot.address, ERROR_RETRY);
}

template<class EntryType>
bool read_dictionary(const Remote& remote, Address parent, Address offset,
                     DictionarySnapshotFor<EntryType>& out, const char* stage,
                     const DictionarySnapshotFor<EntryType>* previous = nullptr) {
    out = {};
    if (!remote.ptr(parent, offset, out.address, stage)) {
        return false;
    }
    remote.link(stage, parent, offset, out.address);
    if (!remote.require_class(out.address, "Dictionary`2", stage) ||
        !remote.value(out.address, 0x10, out.header, stage)) {
        return false;
    }
    const auto& d = out.header;
    if (previous && out.address == previous->address && d == previous->header) {
        out.live = previous->live;
        return true;
    }
    remote.detail("DATA", "%s count=%ld free_count=%ld free_list=%ld version=%ld buckets=0x%llX entries=0x%llX",
                  stage, static_cast<long>(d.count), static_cast<long>(d.free_count),
                  static_cast<long>(d.free_list), static_cast<long>(d.version),
                  static_cast<unsigned long long>(d.buckets), static_cast<unsigned long long>(d.entries));
    if (d.count < 0 || d.free_count < 0 || d.free_count > d.count ||
        static_cast<std::uint64_t>(d.count) > max_dictionary_capacity) {
        return remote.fail(stage, out.address, ERROR_INVALID_DATA);
    }
    if (d.count == 0 && d.buckets == 0 && d.entries == 0) {
        remote.detail("OK", "%s live=0 (unallocated empty dictionary)", stage);
        return recheck_dictionary(remote, out, stage);
    }
    if (!remote.pointer(d.buckets) || !remote.pointer(d.entries)) {
        return remote.fail(stage, out.address, ERROR_INVALID_ADDRESS);
    }
    std::uint64_t bucket_count{}, capacity{};
    if (!remote.value(d.buckets, array_length_offset, bucket_count, stage) ||
        !remote.value(d.entries, array_length_offset, capacity, stage)) {
        return false;
    }
    remote.detail("DATA", "%s bucket_count=%llu entries_capacity=%llu stride=0x%zX",
                  stage, static_cast<unsigned long long>(bucket_count),
                  static_cast<unsigned long long>(capacity), sizeof(EntryType));
    if (bucket_count == 0 || bucket_count != capacity || capacity > max_dictionary_capacity ||
        static_cast<std::uint64_t>(d.count) > capacity) {
        return remote.fail(stage, out.address, ERROR_INVALID_DATA);
    }
    std::vector<std::int32_t> buckets(static_cast<std::size_t>(bucket_count));
    std::vector<EntryType> entries(static_cast<std::size_t>(d.count));
    if (!remote.bytes(d.buckets, array_data_offset, buckets.data(), buckets.size() * sizeof(buckets[0]), stage) ||
        (!entries.empty() && !remote.bytes(d.entries, array_data_offset, entries.data(),
                                           entries.size() * sizeof(EntryType), stage)) ||
        !recheck_dictionary(remote, out, stage)) {
        return false;
    }

    const auto live_count = static_cast<std::size_t>(d.count - d.free_count);
    std::vector<bool> legacy, modern;
    const bool legacy_ok = collect_live(buckets, entries, live_count, false, legacy);
    const bool modern_ok = collect_live(buckets, entries, live_count, true, modern);
    if ((!legacy_ok && !modern_ok) || (legacy_ok && modern_ok && legacy != modern)) {
        remote.detail("DATA", "%s inconsistent buckets: expected_live=%zu legacy_ok=%d modern_ok=%d",
                      stage, live_count, static_cast<int>(legacy_ok), static_cast<int>(modern_ok));
        return remote.fail(stage, out.address, ERROR_INVALID_DATA);
    }
    const auto& occupied = legacy_ok ? legacy : modern;
    out.live.reserve(live_count);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (occupied[i]) {
            if (!remote.pointer(entries[i].value)) {
                return remote.fail(stage, entries[i].value, ERROR_INVALID_ADDRESS);
            }
            out.live.push_back(entries[i]);
        }
    }
    remote.detail("OK", "%s live=%zu bucket_layout=%s", stage, out.live.size(),
                  legacy_ok ? "zero-based (-1 empty)" : "one-based (0 empty)");
    return true;
}

bool finite(const Vec3& p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

bool read_camera(const Remote& remote, Address camera, Camera& out) {
    struct Fields {
        Vec3 position;
        std::array<std::byte, 0x20> gap;
        Vec3 forward, right, up;
    } fields{};
    static_assert(offsetof(Fields, forward) == 0x2C);
    if (!remote.value(camera, 0x10, fields, "BattleCameraModel camera vectors")) {
        return false;
    }
    out.position = fields.position;
    out.forward = fields.forward;
    out.right = fields.right;
    out.up = fields.up;
    return (finite(out.position) && finite(out.forward) && finite(out.right) && finite(out.up)) ||
           remote.fail("BattleCameraModel non-finite vectors", camera, ERROR_INVALID_DATA);
}

void read_camera_lens(const Remote& remote, Address battle_updater, Camera& out) {
    struct Lens {
        float fov;
        float orthographic_size;
        float near_plane;
        float far_plane;
    };
    static_assert(sizeof(Lens) == 0x10);

    Address view{}, brain{};
    Lens lens{};
    // GameAssembly: CinemachineBrain.get_CurrentCameraState (RVA 0xDC4960)
    // reads the inline state at +0xB0. Its first member is LensSettings.
    if (!remote.pose_ptr(battle_updater, 0x10, view, "BattleScreenView") ||
        !remote.pose_class(view, "BattleScreenView", "BattleScreenView") ||
        !remote.pose_ptr(view, 0xA8, brain, "CinemachineBrain") ||
        !remote.pose_class(brain, "CinemachineBrain", "CinemachineBrain") ||
        !remote.value(brain, 0xB0, lens, "CinemachineBrain.CurrentCameraState.Lens")) {
        return;
    }
    if (std::isfinite(lens.fov) && lens.fov > 0.0f && lens.fov < 179.0f) {
        out.fov = lens.fov;
    }
    if (std::isfinite(lens.near_plane) && std::isfinite(lens.far_plane) &&
        lens.near_plane > 0.0f && lens.far_plane > lens.near_plane) {
        out.near_plane = lens.near_plane;
        out.far_plane = lens.far_plane;
    }
}

bool read_last_position(const Remote& remote, Address character, Vec3& position) {
    Address last_position{};
    if (!remote.pose_class(character, "BattleCharacterModel", "BattleCharacterModel class") ||
        !remote.pose_ptr(character, 0x68, last_position, "BattleCharacterModel.LastPositionModel") ||
        !remote.value(last_position, 0x10, position, "LastPositionModel XYZ")) {
        return false;
    }
    return finite(position) || remote.fail("LastPositionModel non-finite XYZ", last_position, ERROR_INVALID_DATA);
}

bool resolve_local_character(const Remote& remote, Address player, Address& character,
                             Address& selected_offset) {
    // PlayerEntityModel.ServerModel is BattleCharacterModel; ClientModel is PlayerClientModel.
    // Resolve identity without requiring LastPositionModel, which may stop updating in a vehicle.
    constexpr std::array<Address, 2> offsets{0x20, 0x18};
    DWORD last_error = ERROR_INVALID_DATA;
    for (const Address offset : offsets) {
        const Remote::Probe probe(remote);
        const char* field = offset == 0x18 ? "PlayerEntityModel.ClientModel" : "PlayerEntityModel.ServerModel";
        Address candidate{};
        if (remote.ptr(player, offset, candidate, field)) {
            remote.link(field, player, offset, candidate, "PTR");
            if (remote.require_class(candidate, "BattleCharacterModel", "Local character class")) {
                character = candidate;
                selected_offset = offset;
                remote.detail("OK", "Local character source=%s", field);
                return true;
            }
        }
        last_error = GetLastError();
        remote.detail("PROBE", "%s rejected: err=%lu", field, static_cast<unsigned long>(last_error));
    }
    return remote.fail("Local character: ClientModel and ServerModel rejected", player, last_error);
}

Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

bool normalize_rotation(Quaternion& rotation) {
    const auto& [x, y, z, w] = rotation;
    const float length2 = x * x + y * y + z * z + w * w;
    if (!(length2 > 1e-6f) || !std::isfinite(length2)) {
        return false;
    }
    const float inv_length = 1.0f / std::sqrt(length2);
    for (float& component : rotation) {
        component *= inv_length;
    }
    return true;
}

Vec3 rotate(const Quaternion& rotation, Vec3 point) {
    const auto& [x, y, z, w] = rotation;
    const Vec3 axis{x, y, z};
    const Vec3 first = cross(axis, point);
    const Vec3 second = cross(axis, first);
    return {point.x + 2.0f * (w * first.x + second.x),
            point.y + 2.0f * (w * first.y + second.y),
            point.z + 2.0f * (w * first.z + second.z)};
}

Quaternion multiply_rotation(const Quaternion& a, const Quaternion& b) {
    const auto& [x, y, z, w] = a;
    const auto& [u, v, s, t] = b;
    return {w * u + x * t + y * s - z * v,
            w * v - x * s + y * t + z * u,
            w * s + x * v - y * u + z * t,
            w * t - x * u - y * v - z * s};
}

bool apply_parent(const LocalTrs& trs, Vec3& point, Quaternion* world_rotation) {
    auto rotation = trs.rotation;
    if (!normalize_rotation(rotation) || !finite(trs.translation) || !finite(trs.scale)) {
        return false;
    }
    const Vec3 scaled{trs.scale.x * point.x, trs.scale.y * point.y, trs.scale.z * point.z};
    const Vec3 rotated = rotate(rotation, scaled);
    point = {trs.translation.x + rotated.x, trs.translation.y + rotated.y, trs.translation.z + rotated.z};
    if (world_rotation) {
        *world_rotation = multiply_rotation(rotation, *world_rotation);
    }
    return finite(point);
}

struct TransformParent {
    Address hierarchy{};
    std::int32_t index{-1};
    Vec3 position{};
};

void sample_cached_hierarchies(const Remote& remote, PoseCache& cache) {
    std::vector<WmPortReader::ReadRange> requests;
    std::vector<PoseCache::HierarchySample*> samples;
    for (auto& [address, sample] : cache.hierarchies) {
        if (!sample.valid || sample.locals.empty()) {
            continue;
        }
        samples.push_back(&sample);
        requests.push_back({address + 0x10, &sample.before, sizeof(sample.before)});
        requests.push_back({sample.header.arrays.local_trs, sample.locals.data(),
                            sample.locals.size() * sizeof(LocalTrs)});
        requests.push_back({sample.header.arrays.parents, sample.parents.data(),
                            sample.parents.size() * sizeof(std::int32_t)});
        requests.push_back({address + 0x10, &sample.after, sizeof(sample.after)});
    }
    remote.batch(requests);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        auto& sample = *samples[i];
        sample.sampled = true;
        const auto reads = std::span(requests).subspan(i * 4, 4);
        sample.valid = std::all_of(reads.begin(), reads.end(), [](const auto& read) { return read.success; }) &&
                       sample.header.same_layout(sample.before) && sample.header.same_layout(sample.after);
        cache.stale |= !sample.valid;
    }
}

bool check_pose_references(const Remote& remote, PoseCache& cache) {
    constexpr std::size_t per_update = 32;
    const auto count = (std::min)(cache.reference_slots.size(), per_update);
    if (count == 0) {
        return true;
    }
    std::array<WmPortReader::ReadRange, per_update> requests{};
    std::array<Address, per_update> observed{};
    for (std::size_t i = 0; i < count; ++i) {
        requests[i] = {cache.reference_slots[cache.reference_cursor], &observed[i], sizeof(Address)};
        cache.reference_cursor = (cache.reference_cursor + 1) % cache.reference_slots.size();
    }
    const auto reads = std::span(requests).first(count);
    remote.batch(reads);
    for (std::size_t i = 0; i < count; ++i) {
        if (!requests[i].success || observed[i] != cache.references.at(requests[i].address)) {
            return false;
        }
    }
    return true;
}

PoseCache::HierarchySample& sample_hierarchy(const Remote& remote, PoseCache& cache, Address hierarchy) {
    auto& sample = cache.hierarchies[hierarchy];
    if (sample.sampled) {
        return sample;
    }
    sample.sampled = true;
    sample.valid = false;
    HierarchyHeader header{};
    if (!remote.value(hierarchy, 0x10, header, "Transform hierarchy header") ||
        header.count < 1 || header.count > max_transform_count ||
        !remote.pointer(header.arrays.local_trs) || !remote.pointer(header.arrays.parents) ||
        !remote.pointer(header.arrays.transforms) ||
        (sample.header.count != 0 && !sample.header.same_layout(header))) {
        cache.stale = true;
        return sample;
    }
    sample.header = header;
    sample.locals.clear();
    sample.parents.clear();
    constexpr std::size_t max_bulk_bytes = 64 * 1024;
    const auto count = static_cast<std::size_t>(header.count);
    if (count <= max_bulk_bytes / sizeof(LocalTrs)) {
        sample.locals.resize(count);
        sample.parents.resize(count);
        HierarchyHeader check{};
        if (!remote.bytes(header.arrays.local_trs, 0, sample.locals.data(), count * sizeof(LocalTrs),
                          "Transform hierarchy TRS batch") ||
            !remote.bytes(header.arrays.parents, 0, sample.parents.data(), count * sizeof(std::int32_t),
                          "Transform hierarchy parents batch") ||
            !remote.value(hierarchy, 0x10, check, "Transform hierarchy header recheck") ||
            !header.same_layout(check)) {
            cache.stale = true;
            return sample;
        }
    }
    sample.valid = true;
    return sample;
}

bool read_transform_position(const Remote& remote, Address transform, Vec3& out,
                             Quaternion* world_rotation = nullptr, TransformParent* parent = nullptr) {
    PoseCache temporary;
    auto& cache = remote.pose_cache() ? *remote.pose_cache() : temporary;
    auto resolved = cache.transforms.find(transform);
    if (resolved == cache.transforms.end()) {
        Address native{}, reverse{};
        TransformAccess access{};
        if (!remote.pose_class(transform, "Transform", "Presentation Transform class") ||
            !remote.pose_ptr(transform, 0x10, native, "Transform.m_CachedPtr") ||
            !remote.value(native, 0x28, access, "Transform hierarchy access") ||
            !remote.pointer(access.hierarchy)) {
            return false;
        }
        const auto& sample = sample_hierarchy(remote, cache, access.hierarchy);
        if (!sample.valid || access.index < 0 || access.index >= sample.header.count ||
            !remote.pose_ptr(sample.header.arrays.transforms, static_cast<Address>(access.index) * sizeof(Address),
                             reverse, "Transform reverse reference") || reverse != native) {
            return false;
        }
        resolved = cache.transforms.emplace(transform, access).first;
    }
    const auto& access = resolved->second;
    const auto& sample = sample_hierarchy(remote, cache, access.hierarchy);
    if (!sample.valid) {
        return false;
    }

    Vec3 point{};
    TransformParent parent_sample{access.hierarchy};
    Quaternion orientation = identity_rotation;
    std::int32_t index = access.index;
    for (std::size_t depth = 0; index != -1; ++depth) {
        if (index < 0 || index >= sample.header.count || depth >= 512) {
            return remote.fail("Transform parent chain", access.hierarchy, ERROR_INVALID_DATA);
        }
        const auto slot = static_cast<std::size_t>(index);
        LocalTrs local{};
        if (sample.locals.empty()) {
            if (!remote.value(sample.header.arrays.local_trs, slot * sizeof(LocalTrs), local, "Transform local TRS") ||
                !remote.value(sample.header.arrays.parents, slot * sizeof(index), index, "Transform parent index")) {
                cache.stale = true;
                return false;
            }
        } else {
            local = sample.locals[slot];
            index = sample.parents[slot];
        }
        if (!apply_parent(local, point, world_rotation ? &orientation : nullptr)) {
            return remote.fail("Transform local TRS invalid", access.hierarchy, ERROR_INVALID_DATA);
        }
        if (parent) {
            if (depth == 0) {
                parent_sample.index = index;
            } else if (!apply_parent(local, parent_sample.position, nullptr)) {
                return remote.fail("Transform parent position invalid", access.hierarchy, ERROR_INVALID_DATA);
            }
        }
    }
    if (sample.locals.empty()) {
        HierarchyHeader check{};
        if (!remote.value(access.hierarchy, 0x10, check, "Transform hierarchy header recheck") ||
            !sample.header.same_layout(check)) {
            cache.stale = true;
            return false;
        }
    }
    if (world_rotation) {
        if (!normalize_rotation(orientation)) {
            return remote.fail("Transform world orientation", access.hierarchy, ERROR_INVALID_DATA);
        }
        *world_rotation = orientation;
    }
    if (parent) {
        *parent = parent_sample;
    }
    out = point;
    return true;
}

bool read_head(const Remote& remote, Address view, Vec3& out) {
    Address transform{};
    return remote.pose_ptr(view, 0xE8, transform, "BattleCharacterEntityView.HeadPosition", true) && transform &&
           read_transform_position(remote, transform, out);
}

bool read_feet(const Remote& remote, Address view, Vec3& out, Quaternion& world_rotation) {
    // get_MovementView loads [view+0x20]. CharacterMovementView.SetPosition
    // (GameAssembly RVA 0x996DD0) calls Unity's position setter on [movement+0x20].
    Address movement{}, transform{};
    return remote.pose_ptr(view, 0x20, movement, "CharacterMovementView", true) && movement &&
           remote.pose_class(movement, "CharacterMovementView", "CharacterMovementView") &&
           remote.pose_ptr(movement, 0x20, transform, "CharacterMovementView._transform", true) && transform &&
           read_transform_position(remote, transform, out, &world_rotation);
}

struct BoneRecord {
    Quaternion local_position_orientation;
    Quaternion model_orientation;
    float length;
};

// The instance fields starting at SkeletonConfig +0x10.
struct SkeletonConfig {
    std::int32_t count;
    std::int32_t stride;
    std::int32_t local_orientation_offset;
    std::int32_t model_orientation_offset;
    std::int32_t length_offset;
    std::uint32_t padding;
    Address children;
    std::int32_t max_children;
    std::array<std::int32_t, skeleton_bone_count> semantic_indices;
};

static_assert(sizeof(BoneRecord) == 9 * sizeof(float));
static_assert(offsetof(BoneRecord, model_orientation) == 4 * sizeof(float));
static_assert(offsetof(BoneRecord, length) == 8 * sizeof(float));
static_assert(sizeof(SkeletonConfig) == 0x40);
static_assert(offsetof(SkeletonConfig, semantic_indices) == 0x24);

using BoneRecords = std::array<BoneRecord, skeleton_bone_count>;
using BoneIndices = std::array<std::int8_t, skeleton_bone_count>;

struct SkeletonIssue {
    const char* field{};
    Address offset{};
    const char* reason{};
    DWORD error = ERROR_INVALID_DATA;
};

SkeletonIssue reconstruct_skeleton(const SkeletonConfig& config, BoneRecords& records,
                                   const std::int32_t* children, const Vec3& origin,
                                   const Quaternion& basis, Entity& entity) {
    BoneIndices semantic_for_slot;
    semantic_for_slot.fill(-1);
    for (std::size_t semantic = 0; semantic < skeleton_bone_count; ++semantic) {
        const auto slot = config.semantic_indices[semantic];
        if (slot < 0 || slot >= config.count || semantic_for_slot[slot] >= 0) {
            return {"SkeletonConfig.boneIndex", 0x34 + semantic * sizeof(std::int32_t), "invalid-index"};
        }
        semantic_for_slot[slot] = static_cast<std::int8_t>(semantic);
    }

    std::array<Vec3, skeleton_bone_count> points{};
    std::array<Quaternion, skeleton_bone_count> orientations{};
    BoneIndices parents;
    parents.fill(-1);
    std::array<std::int32_t, skeleton_bone_count> pending{config.semantic_indices.front()};
    std::size_t queued = 1;
    for (std::size_t cursor = 0; cursor < queued; ++cursor) {
        const auto slot = pending[cursor];
        auto& bone = records[slot];
        const auto record_offset = array_data_offset + static_cast<Address>(slot) * sizeof(BoneRecord);
        if (!std::isfinite(bone.length) || bone.length < 0.0f) {
            return {"_bonesData.length", record_offset + offsetof(BoneRecord, length), "invalid-length"};
        }
        if (!normalize_rotation(bone.local_position_orientation)) {
            return {"_bonesData.localPositionOrientation", record_offset, "invalid-quaternion"};
        }
        if (!normalize_rotation(bone.model_orientation)) {
            return {"_bonesData.modelOrientation", record_offset + offsetof(BoneRecord, model_orientation),
                    "invalid-quaternion"};
        }
        const auto parent = parents[slot];
        const auto& start = parent < 0 ? origin : points[parent];
        const auto& parent_rotation = parent < 0 ? basis : orientations[parent];
        // PlayerSkeletonDebugRenderer.RenderSkeleton (RVA 0x4ECE930) uses local
        // position orientation for the segment and model orientation for child axes.
        const auto rotation = multiply_rotation(parent_rotation, bone.local_position_orientation);
        const Vec3 step = rotate(rotation, {0.0f, bone.length, 0.0f});
        points[slot] = {start.x + step.x, start.y + step.y, start.z + step.z};
        if (!finite(points[slot])) {
            return {"_bonesData", record_offset, "nonfinite-world-position"};
        }
        orientations[slot] = multiply_rotation(basis, bone.model_orientation);

        const auto row_start = static_cast<std::size_t>(slot) * config.max_children;
        const auto* row = children + row_start;
        for (std::int32_t i = 0; i < config.max_children && row[i] >= 0; ++i) {
            const auto child = row[i];
            if (child >= config.count || child == pending.front() || parents[child] >= 0 ||
                queued == pending.size()) {
                return {"BonesHierarchyData", array_data_offset + (row_start + i) * sizeof(row[0]),
                        "invalid-child-or-cycle"};
            }
            parents[child] = static_cast<std::int8_t>(slot);
            pending[queued++] = child;
        }
    }
    if (queued != skeleton_bone_count) {
        return {"BonesHierarchyData", array_data_offset, "unreachable-bone"};
    }

    for (std::size_t semantic = 0; semantic < skeleton_bone_count; ++semantic) {
        const auto slot = config.semantic_indices[semantic];
        entity.bones[semantic] = points[slot];
        entity.bone_parents[semantic] = parents[slot] < 0 ? std::int8_t{-1} : semantic_for_slot[parents[slot]];
    }
    return {};
}

bool unchanged(const Remote& remote, Address base, Address offset, Address expected, const char* stage) {
    Address current{};
    if (!remote.ptr(base, offset, current, stage)) {
        return false;
    }
    if (current == expected) {
        return true;
    }
    remote.link(stage, base, offset, current, "CHANGED");
    remote.detail("DATA", "%s expected=0x%llX", stage, static_cast<unsigned long long>(expected));
    return remote.fail(stage, base + offset, ERROR_RETRY);
}

struct SkeletonSource {
    Address model{};
    Address skeleton{};
    std::array<Address, 2> links{};
    BoneRecords records{};
    bool have_records{};
};

SkeletonIssue read_compact_records(const Remote& remote, Address array, BoneRecords& records) {
    std::uint64_t length{};
    if (!remote.value(array, array_length_offset, length, "_bonesData length")) {
        return {"_bonesData.Length", array_length_offset, "read-failed", GetLastError()};
    }
    if (length != sizeof(records) / sizeof(float)) {
        return {"_bonesData.Length", array_length_offset, "expected-63-floats"};
    }
    if (!remote.value(array, array_data_offset, records, "_bonesData")) {
        return {"_bonesData.Values", array_data_offset, "read-failed", GetLastError()};
    }
    return {};
}

SkeletonIssue inspect_skeleton_source(const Remote& remote, Address entity, Address offset,
                                      const char* field, SkeletonSource& source) {
    if (!remote.ptr(entity, offset, source.model, field, true)) {
        return {field, offset, "pointer-read-failed", GetLastError()};
    }
    if (source.model == 0) {
        return {field, offset, "null-pointer", ERROR_INVALID_ADDRESS};
    }
    bool match{};
    if (!remote.is_class(source.model, "BattleCharacterModel", match, "Skeleton source class")) {
        return {field, offset, "class-check-failed", GetLastError()};
    }
    if (!match) {
        // BattleCharacterClientModel in the profiled build has no compact skeleton field.
        return {field, offset, "no-battle-character-model-layout"};
    }
    if (!remote.ptr(source.model, 0x138, source.skeleton, "BattleCharacterModel.SkeletonModel", true)) {
        return {"BattleCharacterModel.SkeletonModel", 0x138, "pointer-read-failed", GetLastError()};
    }
    if (source.skeleton == 0) {
        return {"BattleCharacterModel.SkeletonModel", 0x138, "null-pointer", ERROR_INVALID_ADDRESS};
    }
    if (!remote.require_class(source.skeleton, "SkeletonModel", "SkeletonModel class")) {
        return {"SkeletonModel.klass", 0, "class-check-failed", GetLastError()};
    }
    // SetSkeleton at RVA 0x4866D90 writes these two adjacent references.
    std::array<Address, 2> links{};
    if (!remote.value(source.skeleton, 0x10, links, "SkeletonModel references")) {
        return {"SkeletonModel.references", 0x10, "pointer-read-failed", GetLastError()};
    }
    source.links = links;
    if (links[0] == 0 && links[1] == 0) {
        return {"SkeletonModel._bonesData", 0x18, "description-and-data-null", ERROR_INVALID_ADDRESS};
    }
    if (!remote.pointer(links[1])) {
        return {"SkeletonModel._bonesData", 0x18, links[1] ? "invalid-pointer" : "null-pointer",
                ERROR_INVALID_ADDRESS};
    }
    const auto records_issue = read_compact_records(remote, links[1], source.records);
    if (records_issue.field) {
        return records_issue;
    }
    source.have_records = true;
    if (!remote.pointer(links[0])) {
        return {"SkeletonModel._skeletonDescription", 0x10, links[0] ? "invalid-pointer" : "null-pointer",
                ERROR_INVALID_ADDRESS};
    }
    return {};
}

SkeletonIssue read_compact_pose(const Remote& remote, const SkeletonSource& source, Address config_address,
                                const Quaternion& basis, Entity& entity) {
    SkeletonConfig config{};
    if (!remote.require_class(config_address, "SkeletonConfig", "SkeletonConfig class")) {
        return {"SkeletonConfig.klass", 0, "class-check-failed", GetLastError()};
    }
    if (!remote.value(config_address, 0x10, config, "SkeletonConfig fields")) {
        return {"SkeletonConfig", 0x10, "read-failed", GetLastError()};
    }
    // Seven records of {Quaternion, Quaternion, float} in the profiled build.
    if (config.count != static_cast<std::int32_t>(skeleton_bone_count)) {
        return {"SkeletonConfig.BonesCount", 0x10, "expected-7-bones"};
    }
    if (config.stride != 9) {
        return {"SkeletonConfig.BoneDataSize", 0x14, "expected-9-floats-per-bone"};
    }
    if (config.local_orientation_offset != 0) {
        return {"SkeletonConfig.BoneDataLocalPositionOrientationOffset", 0x18, "expected-0"};
    }
    if (config.model_orientation_offset != 4) {
        return {"SkeletonConfig.BoneDataModelOrientationOffset", 0x1C, "expected-4"};
    }
    if (config.length_offset != 8) {
        return {"SkeletonConfig.BoneDataLengthOffset", 0x20, "expected-8"};
    }
    if (config.max_children <= 0 || config.max_children >= config.count) {
        return {"SkeletonConfig.MaxChildren", 0x30, "out-of-range"};
    }
    if (!remote.pointer(config.children)) {
        return {"SkeletonConfig.BonesHierarchyData", 0x28, "invalid-pointer", ERROR_INVALID_ADDRESS};
    }

    auto records = source.records;
    std::array<std::int32_t, skeleton_bone_count * skeleton_bone_count> children{};
    const auto child_slots = skeleton_bone_count * static_cast<std::size_t>(config.max_children);
    std::uint64_t children_length{};
    std::array<Address, 2> current_links{};
    if (!remote.value(config.children, array_length_offset, children_length, "BonesHierarchyData length")) {
        return {"BonesHierarchyData.Length", array_length_offset, "read-failed", GetLastError()};
    }
    if (children_length < child_slots) {
        return {"BonesHierarchyData.Length", array_length_offset, "array-too-short"};
    }
    if (!remote.bytes(config.children, array_data_offset, children.data(), child_slots * sizeof(children[0]),
                      "BonesHierarchyData")) {
        return {"BonesHierarchyData.Values", array_data_offset, "read-failed", GetLastError()};
    }
    if (!remote.value(source.skeleton, 0x10, current_links, "SkeletonModel recheck")) {
        return {"SkeletonModel.references", 0x10, "recheck-failed", GetLastError()};
    }
    if (current_links != source.links) {
        return {"SkeletonModel.references", current_links[0] != source.links[0] ? 0x10ULL : 0x18ULL,
                "changed-during-read", ERROR_RETRY};
    }
    if (!unchanged(remote, source.model, 0x138, source.skeleton, "BattleCharacterModel.SkeletonModel recheck")) {
        return {"BattleCharacterModel.SkeletonModel", 0x138, "recheck-failed", GetLastError()};
    }
    return reconstruct_skeleton(config, records, children.data(), entity.position, basis, entity);
}

void log_skeleton(const Remote& remote, const Entity& entity, const char* source,
                  const char* config_source, std::size_t transform_joints) {
    const auto& root = entity.bones[static_cast<std::size_t>(SkeletonBone::root)];
    const auto& spine = entity.bones[static_cast<std::size_t>(SkeletonBone::spine2)];
    const auto& head = entity.bones[static_cast<std::size_t>(SkeletonBone::head1)];
    remote.detail("SKELETON",
                  "entity=0x%llX skeleton=ok source=%s config=%s transform_joints=%zu "
                  "Root=(%.3f,%.3f,%.3f) Spine2=(%.3f,%.3f,%.3f) Head1=(%.3f,%.3f,%.3f)",
                  static_cast<unsigned long long>(entity.address), source, config_source, transform_joints,
                  root.x, root.y, root.z, spine.x, spine.y, spine.z, head.x, head.y, head.z);
}

bool read_entity_skeleton(const Remote& remote, Entity& entity, const Quaternion& basis, bool report) {
    constexpr std::array<Address, 3> offsets{0x30, 0x28, 0x20};
    constexpr std::array<const char*, 3> names{"SimulatedModel", "ServerModel", "ClientModel"};
    std::array<SkeletonSource, names.size()> sources{};
    std::array<SkeletonIssue, names.size()> issues{};
    for (std::size_t i = 0; i < sources.size(); ++i) {
        issues[i] = inspect_skeleton_source(remote, entity.address, offsets[i], names[i], sources[i]);
    }

    const auto try_pose = [&](std::size_t pose_index, std::size_t config_index) {
        const auto& pose = sources[pose_index];
        const auto& config = sources[config_index];
        if (pose_index != config_index) {
            if (!unchanged(remote, entity.address, offsets[config_index], config.model, "Config source recheck") ||
                !unchanged(remote, config.model, 0x138, config.skeleton, "Config SkeletonModel recheck") ||
                !unchanged(remote, config.skeleton, 0x10, config.links[0], "Borrowed SkeletonConfig recheck")) {
                issues[pose_index] = {"SkeletonModel._skeletonDescription", 0x10, "config-source-changed",
                                      GetLastError()};
                return false;
            }
        }
        issues[pose_index] = read_compact_pose(remote, pose, config.links[0], basis, entity);
        if (issues[pose_index].field) {
            return false;
        }
        if (!unchanged(remote, entity.address, offsets[pose_index], pose.model, "Skeleton source recheck")) {
            issues[pose_index] = {names[pose_index], offsets[pose_index], "recheck-failed", GetLastError()};
            return false;
        }
        if (report) {
            log_skeleton(remote, entity, names[pose_index], names[config_index], 0);
        }
        return true;
    };

    // Prefer a model that carries its own complete pose.
    for (std::size_t i = 0; i < sources.size(); ++i) {
        if (!issues[i].field && try_pose(i, i)) {
            return true;
        }
    }
    for (std::size_t i = 0; i < sources.size(); ++i) {
        if (!sources[i].have_records || remote.pointer(sources[i].links[0])) {
            continue;
        }
        for (std::size_t j = 0; j < sources.size(); ++j) {
            if (i != j && remote.pointer(sources[j].links[0]) && try_pose(i, j)) {
                return true;
            }
        }
    }

    entity.bones = {};
    entity.bone_parents = {};
    if (report) {
        for (std::size_t i = 0; i < sources.size(); ++i) {
            const auto& source = sources[i];
            const auto& issue = issues[i];
            remote.detail("SKELETON",
                          "entity=0x%llX compact=unavailable source=%s field=%s offset=0x%llX "
                          "reason=%s win32=%lu description=0x%llX bones_data=0x%llX records=%s",
                          static_cast<unsigned long long>(entity.address), names[i], issue.field,
                          static_cast<unsigned long long>(issue.offset), issue.reason,
                          static_cast<unsigned long>(issue.error),
                          static_cast<unsigned long long>(source.links[0]),
                          static_cast<unsigned long long>(source.links[1]),
                          source.have_records ? "read" : "unavailable");
        }
    }
    return false;
}

bool read_view_joint(const Remote& remote, Address owner, Address offset, const char* field, Vec3& position) {
    Address transform{};
    Vec3 observed{};
    if (!remote.pose_ptr(owner, offset, transform, field, true) || transform == 0 ||
        !read_transform_position(remote, transform, observed)) {
        return false;
    }
    position = observed;
    return true;
}

bool read_view_skeleton(const Remote& remote, Address view, Entity& entity, bool report) {
    constexpr std::array<float, skeleton_bone_count> heights{0.40f, 0.0f, 0.52f, 0.66f, 0.80f, 0.92f, 1.0f};
    constexpr BoneIndices parents{-1, 0, 0, 2, 3, 4, 5};
    const auto& feet = entity.position;
    const auto& head = entity.head_position;
    if (!finite(feet) || !finite(head) || (feet.x == head.x && feet.y == head.y && feet.z == head.z)) {
        return false;
    }
    for (std::size_t i = 0; i < heights.size(); ++i) {
        entity.bones[i] = {std::lerp(feet.x, head.x, heights[i]), std::lerp(feet.y, head.y, heights[i]),
                           std::lerp(feet.z, head.z, heights[i])};
    }

    std::size_t joints{};
    Address animation{};
    if (remote.pose_ptr(view, 0x28, animation, "BattleCharacterEntityView.AnimationView", true) && animation &&
        remote.pose_class(animation, "CharacterAnimationView", "AnimationView class")) {
        const auto joint = [&](Address owner, Address offset, const char* field, SkeletonBone bone) {
            if (read_view_joint(remote, owner, offset, field, entity.bones[static_cast<std::size_t>(bone)])) {
                ++joints;
            }
        };
        Address torso{}, head_control{};
        if (remote.pose_ptr(animation, 0x30, torso, "CharacterAnimationView.TorsoDeformationView", true) && torso &&
            remote.pose_class(torso, "CharacterTorsoDeformationView", "TorsoDeformationView class")) {
            joint(torso, 0x20, "CharacterTorsoDeformationView._root", SkeletonBone::root);
            joint(torso, 0x28, "CharacterTorsoDeformationView._spine2", SkeletonBone::spine2);
            joint(torso, 0x30, "CharacterTorsoDeformationView._spine3", SkeletonBone::spine3);
        }
        if (remote.pose_ptr(animation, 0x28, head_control, "CharacterAnimationView.HeadManualControlView", true) &&
            head_control && remote.pose_class(head_control, "CharacterHeadManualControlView",
                                                 "HeadManualControlView class")) {
            joint(head_control, 0x20, "CharacterHeadManualControlView._neck", SkeletonBone::neck);
            joint(head_control, 0x28, "CharacterHeadManualControlView._head", SkeletonBone::head1);
        }
    }
    entity.bone_parents = parents;
    if (report) {
        log_skeleton(remote, entity, joints ? "ViewTransforms" : "HeadFeet", "none", joints);
    }
    return true;
}

bool read_limb_chain(const Remote& remote, Address weapon_view, Address offset, const char* field,
                     std::array<Vec3, 3>& points, TransformParent* parent) {
    constexpr std::uint64_t max_chain_length = 16;
    auto* pose = remote.pose_cache();
    const Address slot = weapon_view + offset;
    std::array<Address, 3> transforms{};
    if (pose) {
        const auto cached = pose->limb_transforms.find(slot);
        if (cached != pose->limb_transforms.end()) {
            transforms = cached->second;
        }
    }
    if (transforms[0] == 0) {
        Address limb{}, bones{};
        std::uint64_t length{};
        std::int32_t hint{};
        if (!remote.pose_ptr(weapon_view, offset, limb, field, true) || limb == 0 ||
            !remote.pose_class(limb, "LimbIKView", field) ||
            !remote.pose_ptr(limb, 0x40, bones, "LimbIKView._bones", true) || bones == 0 ||
            !remote.value(bones, array_length_offset, length, "LimbIKView._bones.Length") ||
            length < 3 || length > max_chain_length ||
            !remote.value(limb, 0x48, hint, "LimbIKView._hintIndex")) {
            return false;
        }
        const auto bend = hint > 0 && static_cast<std::uint64_t>(hint) < length - 1
            ? static_cast<std::uint64_t>(hint) : length / 2;
        std::array<Address, max_chain_length> entries{};
        if (!remote.bytes(bones, array_data_offset, entries.data(), length * sizeof(Address), "LimbIKView._bones items")) {
            return false;
        }
        transforms = {entries[0], entries[bend], entries[length - 1]};
        if (transforms[0] == transforms[1] || transforms[1] == transforms[2] ||
            transforms[0] == transforms[2] ||
            !unchanged(remote, limb, 0x40, bones, "LimbIKView._bones recheck") ||
            !unchanged(remote, weapon_view, offset, limb, field)) {
            return false;
        }
        if (pose && std::all_of(transforms.begin(), transforms.end(), [&](Address address) {
                return remote.pointer(address);
            })) {
            pose->limb_transforms.emplace(slot, transforms);
            remote.remember_pose_reference(bones + array_length_offset, length);
            const std::array<std::uint64_t, 3> indices{0, bend, length - 1};
            for (std::size_t i = 0; i < transforms.size(); ++i) {
                remote.remember_pose_reference(bones + array_data_offset + indices[i] * sizeof(Address),
                                               transforms[i]);
            }
        }
    }
    for (std::size_t i = 0; i < transforms.size(); ++i) {
        if (!remote.pointer(transforms[i]) ||
            !read_transform_position(remote, transforms[i], points[i], nullptr, i == 0 ? parent : nullptr)) {
            return false;
        }
    }
    return true;
}

bool read_limb_pose(const Remote& remote, Address view, const Quaternion* body_rotation,
                    const Vec3& camera_right, Entity& entity, bool report) {
    const auto& feet = entity.position;
    const auto& head = entity.head_position;
    const Vec3 axis{head.x - feet.x, head.y - feet.y, head.z - feet.z};
    const float height = std::hypot(axis.x, axis.y, axis.z);
    if (!std::isfinite(height) || height <= 1e-4f) {
        return false;
    }
    const Vec3 up{axis.x / height, axis.y / height, axis.z / height};
    Vec3 right{};
    const auto set_side = [&](const Vec3& candidate) {
        const float vertical = up.x * candidate.x + up.y * candidate.y + up.z * candidate.z;
        const Vec3 side{candidate.x - vertical * up.x, candidate.y - vertical * up.y,
                        candidate.z - vertical * up.z};
        const float length = std::hypot(side.x, side.y, side.z);
        if (!std::isfinite(length) || length <= 1e-4f) {
            return false;
        }
        right = {side.x / length, side.y / length, side.z / length};
        return true;
    };
    const bool have_camera_side = set_side(camera_right);
    const auto camera_side = right;
    const bool have_body_side = body_rotation && set_side(rotate(*body_rotation, {1.0f, 0.0f, 0.0f}));
    if (have_camera_side && have_body_side) {
        const float lateral = right.x * camera_side.x + right.y * camera_side.y + right.z * camera_side.z;
        if (std::abs(lateral) < 0.2f) {
            // Keep estimated limbs visibly separated when the body is edge-on.
            const float sign = std::copysign(1.0f, lateral);
            right = {sign * camera_side.x, sign * camera_side.y, sign * camera_side.z};
        }
    } else if (!have_body_side && !have_camera_side &&
               !set_side({1.0f, 0.0f, 0.0f}) && !set_side({0.0f, 0.0f, 1.0f})) {
        return false;
    }

    // Unavailable chains use body proportions along the observed head-to-feet axis.
    constexpr std::array<std::array<float, 2>, limb_joint_count> proportions{{
        {0.49f, 0.0f},
        {0.79f, -0.115f}, {0.64f, -0.17f}, {0.49f, -0.19f},
        {0.79f,  0.115f}, {0.64f,  0.17f}, {0.49f,  0.19f},
        {0.48f, -0.075f}, {0.25f, -0.09f}, {0.02f, -0.11f},
        {0.48f,  0.075f}, {0.25f,  0.09f}, {0.02f,  0.11f}
    }};
    for (std::size_t i = 0; i < proportions.size(); ++i) {
        const auto [along, across] = proportions[i];
        entity.limb_joints[i] = {
            std::lerp(feet.x, head.x, along) + height * across * right.x,
            std::lerp(feet.y, head.y, along) + height * across * right.y,
            std::lerp(feet.z, head.z, along) + height * across * right.z
        };
    }

    constexpr std::array limb_starts{LimbJoint::left_shoulder, LimbJoint::right_shoulder,
                                     LimbJoint::left_hip, LimbJoint::right_hip};
    constexpr std::array<const char*, limb_starts.size()> limb_fields{
        "LeftHandIK", "RightHandIK", "LeftFootIK", "RightFootIK"
    };
    constexpr std::array<Address, limb_starts.size()> offsets{0x58, 0x60, 0x68, 0x70};
    constexpr auto left_hip = static_cast<std::size_t>(LimbJoint::left_hip);
    constexpr auto right_hip = static_cast<std::size_t>(LimbJoint::right_hip);
    constexpr auto pelvis = static_cast<std::size_t>(LimbJoint::pelvis);
    std::array<TransformParent, 2> hip_parents{};
    Address weapon_view{};
    if (remote.pose_ptr(view, 0x30, weapon_view, "BattleCharacterEntityView.WithWeaponAnimationView", true) &&
        weapon_view && remote.pose_class(weapon_view, "CharacterWithWeaponAnimationView", "Weapon animation view")) {
        auto joints = entity.limb_joints;
        std::bitset<limb_joint_count> measured;
        for (std::size_t i = 0; i < limb_starts.size(); ++i) {
            std::array<Vec3, 3> points{};
            auto* parent = i < 2 ? nullptr : &hip_parents[i - 2];
            if (!read_limb_chain(remote, weapon_view, offsets[i], limb_fields[i], points, parent)) {
                continue;
            }
            const auto first = static_cast<std::size_t>(limb_starts[i]);
            for (std::size_t j = 0; j < points.size(); ++j) {
                joints[first + j] = points[j];
                measured.set(first + j);
            }
        }
        if (measured.any()) {
            entity.limb_joints = joints;
            entity.measured_limb_joints = measured;
        }
    }
    const char* pelvis_source = "body-proportions";
    if (entity.measured_limb_joints.test(left_hip) && entity.measured_limb_joints.test(right_hip)) {
        const auto& left = hip_parents[0];
        const auto& right_parent = hip_parents[1];
        const bool shared_parent = left.index >= 0 && left.index == right_parent.index &&
                                   left.hierarchy == right_parent.hierarchy;
        const auto& a = shared_parent ? left.position : entity.limb_joints[left_hip];
        const auto& b = shared_parent ? right_parent.position : entity.limb_joints[right_hip];
        entity.limb_joints[pelvis] = {std::lerp(a.x, b.x, 0.5f), std::lerp(a.y, b.y, 0.5f),
                                      std::lerp(a.z, b.z, 0.5f)};
        entity.measured_limb_joints.set(pelvis, shared_parent);
        pelvis_source = shared_parent ? "shared-hip-parent" : "hip-midpoint";
    }
    if (report) {
        remote.detail("SKELETON",
                      "entity=0x%llX limbs LeftArm=%s RightArm=%s LeftLeg=%s RightLeg=%s pelvis=%s",
                      static_cast<unsigned long long>(entity.address),
                      entity.measured_limb_joints.test(static_cast<std::size_t>(LimbJoint::left_shoulder))
                          ? "Transform" : "estimated",
                      entity.measured_limb_joints.test(static_cast<std::size_t>(LimbJoint::right_shoulder))
                          ? "Transform" : "estimated",
                      entity.measured_limb_joints.test(left_hip) ? "Transform" : "estimated",
                      entity.measured_limb_joints.test(right_hip) ? "Transform" : "estimated", pelvis_source);
    }
    return true;
}

}  // namespace

GameEntities GameEntities::from_unity_entry(std::uint64_t address) noexcept {
    GameEntities result;
    result.unity_entry_ = address;
    return result;
}

namespace {

#include "NativeCharacterMesh.inl"

struct CachedEntity {
    Address address{};
    Address view{};
    std::int32_t id{};
    bool probe_compact = true;
    std::string nickname;
    bool nickname_is_public_id{};
    int team_id = -1;
    std::string team_name;
};

struct Structure {
    std::optional<Address> unity;
    Address gom{}, component{}, game{}, model{}, battle{}, battle_entities{};
    Address entity_models{}, player{}, local_character{}, local_character_offset{};
    Address camera_address{}, battle_updater{}, updater{}, vehicles_model{};
    DictionarySnapshot models, views, vehicles;
    std::vector<CachedEntity> entities;
    std::vector<Vehicle> vehicle_list;
};

bool same_dictionary(const DictionarySnapshot& a, const DictionarySnapshot& b) {
    return a.address == b.address && a.header == b.header;
}

constexpr const char* nickname_stage = "Character identity";
constexpr const char* team_stage = "Clan Battle team";

std::string read_identity_string(const Remote& remote, Address string, const char* stage) {
    if (!string || !remote.require_class(string, "String", stage)) return {};
    std::array<wchar_t, 128> characters{};
    std::int32_t length{};
    if (!remote.value(string, 0x10, length, stage) || length <= 0 ||
        length > static_cast<std::int32_t>(characters.size()) ||
        !remote.bytes(string, 0x14, characters.data(), static_cast<std::size_t>(length) * sizeof(wchar_t),
                      stage)) return {};

    // Keep labels on one line, including when the source contains embedded NULs.
    auto end = characters.begin() + length;
    std::replace_if(characters.begin(), end, [](wchar_t character) {
        return character < 0x20 || (character >= 0x7f && character <= 0x9f) ||
               character == 0x2028 || character == 0x2029;
    }, L' ');
    auto first = std::find_if(characters.begin(), end, [](wchar_t character) { return character != L' '; });
    while (end != first && end[-1] == L' ') --end;
    if (first == end) return {};

    std::array<char, characters.size() * 3> utf8{};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, &*first,
        static_cast<int>(end - first), utf8.data(), static_cast<int>(utf8.size()), nullptr, nullptr);
    return size > 0 ? std::string(utf8.data(), static_cast<std::size_t>(size)) : std::string{};
}

std::string read_identity_value(const Remote& remote, Address wrapper, const char* stage) {
    Address string{};
    if (!remote.pointer(wrapper) || !remote.ptr(wrapper, 0x18, string, stage, true)) return {};
    return read_identity_string(remote, string, stage);
}

struct SlotIdentity {
    Address description{};
    std::int64_t public_id{};
    std::string nickname, clan_tag;
};

bool read_slot_identity(const Remote& remote, Address slot, std::int32_t expected_id,
                        SlotIdentity& identity) {
    Address data{};
    if (!remote.require_class(slot, "BattleCharacterSlotModel", nickname_stage) ||
        !remote.ptr(slot, 0x10, data, nickname_stage) ||
        !remote.require_class(data, "BattleCharacterSlotData", nickname_stage)) return false;

    // BattleCharacterSlotModel.get_Nickname (RVA 0x4980A50) reads data +0x30.
    // Its IPrimitiveData<string>.Value resolves to BasePrimitiveData<object>.get_Value at +0x18.
    std::array<Address, 4> fields{};
    if (!remote.value(data, 0x20, fields, nickname_stage)) return false;
    const auto [entity_id_data, public_id_data, nickname_data, clan_tag_data] = fields;
    if (!remote.pointer(entity_id_data)) return false;
    std::int32_t entity_id{};
    if (!remote.value(entity_id_data, 0x18, entity_id, nickname_stage) || entity_id != expected_id) return false;

    if (remote.pointer(public_id_data)) {
        std::int64_t value{};
        if (remote.value(public_id_data, 0x18, value, nickname_stage)) identity.public_id = value;
    }
    identity.nickname = read_identity_value(remote, nickname_data, nickname_stage);
    identity.clan_tag = read_identity_value(remote, clan_tag_data, nickname_stage);
    static_cast<void>(remote.ptr(slot, 0x18, identity.description, nickname_stage, true));

    std::int32_t current_entity_id{};
    return remote.value(entity_id_data, 0x18, current_entity_id, nickname_stage) &&
           current_entity_id == expected_id;
}

struct ClanBattleSide {
    int index = -1;
    std::string name, tag, description;
    std::unordered_set<Address> character_slots;
};

std::array<ClanBattleSide, 2> read_clan_battle_sides(const Remote& remote, Address battle, Address slots) {
    std::array<ClanBattleSide, 2> sides{};
    Address team_provider{}, provider_slots{}, cache{}, model{};
    if (!battle || !slots ||
        !remote.ptr(battle, 0xA8, team_provider, team_stage, true) || !team_provider ||
        !remote.require_class(team_provider, "ClientClanBattleCharacterTeamModel", team_stage) ||
        !remote.ptr(team_provider, 0x10, provider_slots, team_stage) || provider_slots != slots ||
        !remote.ptr(team_provider, 0x18, cache, team_stage) ||
        !remote.require_class(cache, "ClanBattleTeamsCacheModel", team_stage) ||
        !remote.ptr(battle, 0x3D0, model, team_stage, true) || !model ||
        !remote.require_class(model, "ClanBattleModel", team_stage)) return sides;

    ReferenceDictionarySnapshot teams;
    if (!read_dictionary(remote, model, 0x10, teams, team_stage) || teams.live.size() != sides.size()) return sides;
    for (std::size_t i = 0; i < sides.size(); ++i) {
        // TryGetTeam checks Attackers first and Defenders second, by slot-description reference.
        const Address offset = static_cast<Address>(i) * sizeof(Address);
        Address side_model{}, members{}, data{};
        if (!remote.ptr(model, 0x28 + offset, side_model, team_stage) ||
            !remote.require_class(side_model, "ClanBattleTeamModel", team_stage) ||
            !remote.ptr(cache, 0x10 + offset, members, team_stage) ||
            !remote.require_class(members, "ClanBattleTeamMembersModel", team_stage) ||
            !remote.ptr(side_model, 0x10, data, team_stage) ||
            !remote.require_class(data, "ClanBattleTeamData", team_stage)) continue;
        const auto team = std::find_if(teams.live.begin(), teams.live.end(), [side_model](const auto& entry) {
            return entry.value == side_model;
        });
        if (team == teams.live.end() || !remote.pointer(team->key) ||
            !remote.require_class(team->key, "ClanBattleTeamDescription", team_stage)) continue;

        ClanBattleSide side;
        if (!remote.value(team->key, 0x18, side.index, team_stage) || side.index < 0) continue;
        Address clan_name{}, clan_tag{}, side_name{};
        if (remote.ptr(data, 0x28, clan_name, team_stage, true)) side.name = read_identity_value(remote, clan_name, team_stage);
        if (remote.ptr(data, 0x30, clan_tag, team_stage, true)) side.tag = read_identity_value(remote, clan_tag, team_stage);
        if (remote.ptr(team->key, 0x10, side_name, team_stage, true)) side.description = read_identity_string(remote, side_name, team_stage);

        ReferenceDictionarySnapshot members_by_slot;
        if (!read_dictionary(remote, members, 0x20, members_by_slot, team_stage) ||
            !unchanged(remote, members, 0x20, members_by_slot.address, team_stage) ||
            !recheck_dictionary(remote, members_by_slot, team_stage)) continue;
        side.character_slots.reserve(members_by_slot.live.size());
        for (const auto& member : members_by_slot.live) {
            if (remote.pointer(member.key)) side.character_slots.insert(member.key);
        }
        sides[i] = std::move(side);
    }
    if (!unchanged(remote, battle, 0xA8, team_provider, team_stage) ||
        !unchanged(remote, battle, 0x3D0, model, team_stage) ||
        !unchanged(remote, model, 0x10, teams.address, team_stage) ||
        !recheck_dictionary(remote, teams, team_stage)) return {};
    return sides;
}

void refresh_character_identities(const Remote& remote, Structure& structure) {
    const Remote::Probe probe(remote);
    std::int64_t local_public_id{};
    if (structure.player) {
        std::int64_t value{};
        if (remote.value(structure.player, 0x28, value, nickname_stage)) local_public_id = value;
    }
    Address slots{};
    DictionarySnapshot by_entity_id;
    const bool have_slots = remote.ptr(structure.battle_entities, 0x80, slots, nickname_stage) &&
        remote.require_class(slots, "ClientCharacterSlotsModel", nickname_stage) &&
        read_dictionary(remote, slots, 0x18, by_entity_id, nickname_stage);
    const auto sides = read_clan_battle_sides(remote, structure.battle, have_slots ? slots : 0);

    std::unordered_map<std::int32_t, Address> slot_by_id;
    if (have_slots) {
        slot_by_id.reserve(by_entity_id.live.size());
        for (const auto& entry : by_entity_id.live) slot_by_id.emplace(entry.entity_id, entry.value);
    }
    for (auto& entity : structure.entities) {
        entity.nickname.clear();
        entity.nickname_is_public_id = false;
        entity.team_id = -1;
        entity.team_name.clear();
        const bool is_local = entity.address == structure.player;
        SlotIdentity identity;
        const auto slot = slot_by_id.find(entity.id);
        if (slot == slot_by_id.end() || !read_slot_identity(remote, slot->second, entity.id, identity) ||
            (is_local && local_public_id > 0 && identity.public_id > 0 && identity.public_id != local_public_id)) {
            identity = {};
        }
        entity.nickname = std::move(identity.nickname);
        entity.team_name = std::move(identity.clan_tag);
        for (const auto& side : sides) {
            if (!side.character_slots.contains(identity.description)) continue;
            entity.team_id = side.index;
            if (!side.name.empty()) entity.team_name = side.name;
            else if (entity.team_name.empty()) entity.team_name = side.tag.empty() ? side.description : side.tag;
            break;
        }
        if (entity.nickname.empty()) {
            const auto public_id = is_local && local_public_id > 0 ? local_public_id : identity.public_id;
            if (public_id > 0) {
                entity.nickname = std::to_string(public_id);
                entity.nickname_is_public_id = true;
            }
        }
    }

    if (have_slots && (!unchanged(remote, structure.battle_entities, 0x80, slots, nickname_stage) ||
                       !unchanged(remote, slots, 0x18, by_entity_id.address, nickname_stage) ||
                       !recheck_dictionary(remote, by_entity_id, nickname_stage))) {
        for (auto& entity : structure.entities) {
            const bool use_local_id = entity.address == structure.player && local_public_id > 0;
            entity.nickname = use_local_id ? std::to_string(local_public_id) : std::string{};
            entity.nickname_is_public_id = use_local_id;
            entity.team_id = -1;
            entity.team_name.clear();
        }
    }
}

bool refresh_structure(Remote& remote, WmPortReader& reader, std::uint32_t pid,
                       std::optional<Address> unity_entry, Address gom_rva,
                       const Structure& previous, Structure& out) {
    std::optional<Address> unity;
    Address gom{}, component{}, game{}, model{}, battle{}, battle_entities{};
    if (unity_entry) {
        remote.phase("UnityEntry");
        component = *unity_entry;
        bool match{};
        if (!remote.is_class(component, "UnityEntry", match, "UnityEntry class")) {
            return false;
        }
        if (!match) {
            return remote.fail("UnityEntry class changed", component, ERROR_INVALID_DATA);
        }
        remote.log("UnityEntry from ClassScanner", component);
    } else {
        remote.phase("Module");
        unity = reader.module_base(pid, "UnityPlayer.dll");
        if (!unity) {
            const DWORD error = GetLastError();
            return remote.fail("UnityPlayer.dll", 0, error == ERROR_SUCCESS ? ERROR_MOD_NOT_FOUND : error);
        }
        if (!remote.pointer(*unity)) {
            return remote.fail("UnityPlayer.dll base", *unity, ERROR_INVALID_ADDRESS);
        }
        remote.log("UnityPlayer.dll", *unity);

        remote.phase("GOM");
        if (!remote.ptr(*unity, gom_rva, gom, "GOM")) {
            return false;
        }
        remote.link("GOM", *unity, gom_rva, gom);
        Address object{};
        if (!find_entry_object(remote, gom, object, component)) {
            return false;
        }
    }
    remote.phase("Game");
    if (!remote.trace(component, 0x28, game, "Game")) {
        return false;
    }
    remote.phase("GameModel");
    if (!remote.trace(game, game_model_offset, model, "GameModel")) {
        return false;
    }
    remote.phase("BattleModel");
    if (!remote.trace(model, 0x10, battle, "BattleModel")) {
        return false;
    }
    remote.phase("BattleEntitiesModel");
    if (!remote.trace(battle, 0x90, battle_entities, "BattleEntitiesModel")) {
        return false;
    }

    remote.phase("EntityModels dictionary");
    Address entity_models{};
    DictionarySnapshot models;
    if (!remote.trace(battle_entities, 0xE0, entity_models, "ReadOnlyDictionaryNonAlloc`2") ||
        !read_dictionary(remote, entity_models, 0x10, models, "EntityModels._dictionary", &previous.models)) {
        return false;
    }
    remote.phase("Local player");
    Address player{}, local_character{}, camera_address{};
    if (!remote.trace(battle_entities, 0xD0, player, "PlayerEntityModel")) {
        return false;
    }
    // PublicId is int64; it is NOT the int32 EntityModel.Id dictionary key.
    {
        const Remote::Probe probe(remote);
        std::int64_t public_id{};
        if (remote.value(player, 0x28, public_id, "PlayerEntityModel.PublicId")) {
            remote.detail("DATA", "PlayerEntityModel.PublicId=%lld", static_cast<long long>(public_id));
        }
    }
    Address local_character_offset{};
    if (!resolve_local_character(remote, player, local_character, local_character_offset)) {
        return false;
    }
    remote.phase("Camera");
    if (!remote.trace(battle, 0x358, camera_address, "BattleCameraModel")) {
        return false;
    }

    remote.phase("Vehicles (optional)");
    Address vehicles_model{};
    DictionarySnapshot vehicles;
    bool have_vehicles{};
    {
        const Remote::Probe probe(remote);
        have_vehicles = remote.trace(battle_entities, 0x88, vehicles_model, "VehiclesClientModel") &&
            read_dictionary(remote, vehicles_model, 0x10, vehicles, "VehiclesClientModel._vehicles", &previous.vehicles);
    }
    if (!have_vehicles) {
        remote.log("Vehicles unavailable; publishing characters without vehicles", vehicles_model,
                   GetLastError(), "WARN");
        vehicles = {};
    }

    remote.phase("Character views (optional)");
    Address updater{}, battle_updater{};
    DictionarySnapshot views;
    bool have_views{};
    {
        const Remote::Probe probe(remote);
        if (find_view_updater(remote, game, battle, battle_entities, updater, battle_updater)) {
            have_views = read_dictionary(remote, updater, 0x110, views, "_characterEntityViews", &previous.views);
        } else {
            battle_updater = 0;
        }
    }
    if (!have_views) {
        remote.log("Character views unavailable; head positions will be invalid", updater,
                   GetLastError(), "WARN");
        views = {};
    }
    if (same_dictionary(previous.models, models) && same_dictionary(previous.views, views) &&
        player == previous.player) {
        out.entities = previous.entities;
    } else {
        std::unordered_map<std::int32_t, Address> views_by_id;
        views_by_id.reserve(views.live.size());
        for (const auto& entry : views.live) {
            if (!views_by_id.emplace(entry.entity_id, entry.value).second) {
                remote.log("_characterEntityViews duplicate ID; ignoring views", views.address, ERROR_INVALID_DATA, "WARN");
                views_by_id.clear();
                have_views = false;
                break;
            }
        }
        out.entities.reserve(models.live.size());
        for (const auto& entry : models.live) {
            remote.detail("ITEM", "EntityModels id=%ld value=0x%llX", static_cast<long>(entry.entity_id),
                          static_cast<unsigned long long>(entry.value));
            const bool is_local = entry.value == player;
            if (!is_local) {
                bool match{};
                if (!remote.is_class(entry.value, "BattleCharacterEntityModel", match, "EntityModels item class")) {
                    return false;
                }
                if (!match) {
                    continue;
                }
            }
            std::int32_t id{};
            if (!remote.value(entry.value, 0x10, id, "EntityModel.Id")) {
                return false;
            }
            if (id != entry.entity_id) {
                return remote.fail("EntityModel.Id differs from dictionary key", entry.value, ERROR_INVALID_DATA);
            }
            const auto view = views_by_id.find(id);
            CachedEntity entity;
            entity.address = entry.value;
            entity.view = view == views_by_id.end() ? 0 : view->second;
            entity.id = id;
            out.entities.push_back(std::move(entity));
        }
    }
    out.vehicle_list.reserve(vehicles.live.size());
    for (const auto& entry : vehicles.live) {
        out.vehicle_list.push_back({entry.entity_id, entry.value});
    }
    out.unity = unity;
    out.gom = gom;
    out.component = component;
    out.game = game;
    out.model = model;
    out.battle = battle;
    out.battle_entities = battle_entities;
    out.entity_models = entity_models;
    out.player = player;
    out.local_character = local_character;
    out.local_character_offset = local_character_offset;
    out.camera_address = camera_address;
    out.battle_updater = battle_updater;
    out.updater = have_views ? updater : 0;
    out.vehicles_model = have_vehicles ? vehicles_model : 0;
    out.models = std::move(models);
    out.views = std::move(views);
    out.vehicles = std::move(vehicles);
    refresh_character_identities(remote, out);
    return true;
}

bool check_structure(const Remote& remote, const Structure& s, Address gom_rva) {
    struct Link {
        Address slot;
        Address expected;
        const char* stage;
    };
    const std::array links{
        Link{s.component + 0x28, s.game, "Game recheck"},
        Link{s.game + game_model_offset, s.model, "GameModel recheck"},
        Link{s.model + 0x10, s.battle, "BattleModel recheck"},
        Link{s.battle + 0x90, s.battle_entities, "BattleEntitiesModel recheck"},
        Link{s.battle_entities + 0xE0, s.entity_models, "EntityModels wrapper recheck"},
        Link{s.entity_models + 0x10, s.models.address, "EntityModels dictionary recheck"},
        Link{s.battle_entities + 0xD0, s.player, "PlayerEntityModel recheck"},
        Link{s.player + s.local_character_offset, s.local_character, "Local character recheck"},
        Link{s.battle + 0x358, s.camera_address, "BattleCameraModel recheck"},
        Link{s.unity.value_or(0) + gom_rva, s.gom, "GOM recheck"},
    };
    std::array<Address, links.size()> observed{};
    std::array<WmPortReader::ReadRange, links.size() + 1> requests{};
    Dictionary header{};
    requests[0] = {s.models.address + 0x10, &header, sizeof(header)};
    const auto count = s.unity ? links.size() : links.size() - 1;
    for (std::size_t i = 0; i < count; ++i) {
        requests[i + 1] = {links[i].slot, &observed[i], sizeof(Address)};
    }
    remote.batch(std::span(requests).first(count + 1));
    for (std::size_t i = 0; i < count; ++i) {
        if (!requests[i + 1].success) {
            return remote.fail(links[i].stage, links[i].slot, ERROR_PARTIAL_COPY);
        }
        if (links[i].expected != observed[i]) {
            return remote.fail(links[i].stage, links[i].slot, ERROR_RETRY);
        }
    }
    if (!requests[0].success) {
        return remote.fail("EntityModels header recheck", requests[0].address, ERROR_PARTIAL_COPY);
    }
    if (header != s.models.header) {
        return remote.fail("EntityModels header recheck", requests[0].address, ERROR_RETRY);
    }
    return true;
}

bool check_views(const Remote& remote, const Structure& s) {
    return s.updater == 0 ||
           (unchanged(remote, s.updater, 0x110, s.views.address, "_characterEntityViews pointer recheck") &&
            recheck_dictionary(remote, s.views, "_characterEntityViews header recheck"));
}

bool check_vehicles(const Remote& remote, const Structure& s) {
    return s.vehicles_model == 0 ||
           (unchanged(remote, s.battle_entities, 0x88, s.vehicles_model, "VehiclesClientModel recheck") &&
            unchanged(remote, s.vehicles_model, 0x10, s.vehicles.address, "_vehicles pointer recheck") &&
            recheck_dictionary(remote, s.vehicles, "_vehicles header recheck"));
}

struct LocalPositionSample {
    Vec3 position{};
    Vec3 player_position{};
    bool player_position_valid{};
    std::int32_t vehicle_id = -1;
    const char* source = "player";
};

bool read_local_vehicle_id(const Remote& remote, Address character, std::int32_t& id) {
    Address state{}, data{};
    // InVehicleModel.get_IsInsideVehicle (RVA 0x48D86C0) tests EntityId >= 0.
    // Clear() (0x48D84F0) sets it to -1. This includes passengers, not just the driver.
    if (!remote.pose_class(character, "BattleCharacterModel", "Local vehicle character class") ||
        !remote.ptr(character, 0x1C8, state, "BattleCharacterModel.InVehicleModel") ||
        !remote.pose_class(state, "InVehicleModel", "InVehicleModel class") ||
        !remote.ptr(state, 0x10, data, "InVehicleModel._entityId")) return false;
    bool supported{};
    for (const char* name : {"BattleEntityIdData", "IntData", "PrimitiveData`1"}) {
        bool match{};
        if (!remote.is_class(data, name, match, "Vehicle EntityId data class")) return false;
        if (match) { supported = true; break; }
    }
    if (!supported) return remote.fail("Unsupported vehicle EntityId data class", data, ERROR_INVALID_DATA);
    // BasePrimitiveData<int>.get_Value at RVA 0x7AA140 is mov eax,[rcx+0x18].
    return remote.value(data, 0x18, id, "InVehicleModel.EntityId");
}

bool read_client_vehicle_position(const Remote& remote, Address client, Vec3& position) {
    bool car{}, helicopter{};
    Address movement{};
    if (!remote.is_class(client, "CarClientModel", car, "Local car client class") ||
        (!car && !remote.is_class(client, "HelicopterClientModel", helicopter, "Local helicopter client class")))
        return false;
    if (!car && !helicopter) return remote.fail("Unsupported vehicle client class", client, ERROR_INVALID_DATA);
    // VehicleClientModel.Position is already the interpolated WORLD position.
    if (!remote.ptr(client, 0x10, movement, "VehicleClientModel.Position") ||
        !remote.pose_class(movement, "SetMovementModel", "Vehicle position class") ||
        !remote.value(movement, 0x10, position, "Vehicle SetMovementModel XYZ") ||
        !unchanged(remote, client, 0x10, movement, "Vehicle position owner recheck")) return false;
    return finite(position) || remote.fail("Non-finite vehicle position", movement, ERROR_INVALID_DATA);
}

bool read_occupied_vehicle_position(const Remote& remote, const Structure& s, std::int32_t id,
                                    Vec3& position, const char*& source) {
    const Remote::Probe probe(remote);
    const auto find = [id](const DictionarySnapshot& dictionary) -> Address {
        const auto it = std::find_if(dictionary.live.begin(), dictionary.live.end(), [id](const auto& e) {
            return e.entity_id == id;
        });
        return it == dictionary.live.end() ? 0 : it->value;
    };
    if (s.vehicles_model && check_vehicles(remote, s)) {
        if (const Address client = find(s.vehicles); client && read_client_vehicle_position(remote, client, position) &&
            check_vehicles(remote, s)) {
            source = "vehicle-client-world";
            return true;
        }
    }
    // A vehicle may appear after the one-second structural refresh. Resolve the live registry immediately.
    Address clients{};
    DictionarySnapshot dictionary;
    if (remote.ptr(s.battle_entities, 0x88, clients, "Live VehiclesClientModel") &&
        remote.pose_class(clients, "VehiclesClientModel", "Live VehiclesClientModel class") &&
        read_dictionary(remote, clients, 0x10, dictionary, "Live vehicle dictionary", &s.vehicles)) {
        if (const Address client = find(dictionary); client && read_client_vehicle_position(remote, client, position) &&
            unchanged(remote, s.battle_entities, 0x88, clients, "Live vehicles owner recheck") &&
            unchanged(remote, clients, 0x10, dictionary.address, "Live vehicles dictionary recheck") &&
            recheck_dictionary(remote, dictionary, "Live vehicles header recheck")) {
            source = "vehicle-client-world";
            return true;
        }
    }
    // Fallback: authoritative world position, not a seat-local Transform or a lazy cached pivot.
    Address world{};
    if (!remote.ptr(s.battle_entities, 0x60, world, "Vehicle WorldEntitiesModel") ||
        !remote.pose_class(world, "WorldEntitiesModel", "Vehicle world class")) return false;
    struct Layout { Address registry; Address transform; const char* type; };
    constexpr std::array layouts{Layout{0x60, 0xC0, "CarModel"}, Layout{0x68, 0x70, "HelicopterModel"}};
    for (const auto& layout : layouts) {
        if (!read_dictionary(remote, world, layout.registry, dictionary, "World vehicle registry")) continue;
        const Address vehicle = find(dictionary);
        Address transform{}, data{};
        bool vector_data{}, primitive_data{};
        if (!vehicle || !remote.pose_class(vehicle, layout.type, "World vehicle class") ||
            !remote.ptr(vehicle, layout.transform, transform, "VehicleModel.Transform") ||
            !remote.pose_class(transform, "VehicleTransformModel", "Vehicle world transform class") ||
            !remote.ptr(transform, 0x10, data, "VehicleTransformModel._position") ||
            !remote.is_class(data, "Vector3Data", vector_data, "Vehicle position data class") ||
            (!vector_data && !remote.is_class(data, "PrimitiveData`1", primitive_data, "Vehicle vector data class")) ||
            (!vector_data && !primitive_data) ||
            !remote.value(data, 0x18, position, "Vehicle world position data") || !finite(position)) continue;
        if (unchanged(remote, s.battle_entities, 0x60, world, "Vehicle world owner recheck") &&
            unchanged(remote, world, layout.registry, dictionary.address, "World vehicle dictionary recheck") &&
            unchanged(remote, vehicle, layout.transform, transform, "Vehicle transform recheck") &&
            unchanged(remote, transform, 0x10, data, "Vehicle position data recheck") &&
            recheck_dictionary(remote, dictionary, "World vehicle header recheck")) {
            source = "vehicle-server-world";
            return true;
        }
    }
    return false;
}

bool sample_local_position(const Remote& remote, const Structure& s, LocalPositionSample& out) {
    out = {};
    if (!read_local_vehicle_id(remote, s.local_character, out.vehicle_id)) return false;
    if (out.vehicle_id >= 0) {
        // LastPosition is diagnostic only while seated; it may be stale or absent.
        const Remote::Probe probe(remote);
        out.player_position_valid = read_last_position(remote, s.local_character, out.player_position);
        if (!read_occupied_vehicle_position(remote, s, out.vehicle_id, out.position, out.source))
            return remote.fail("Occupied vehicle position unavailable; refusing stale player origin",
                               s.local_character, ERROR_RETRY);
    } else {
        if (!read_last_position(remote, s.local_character, out.position)) return false;
        out.player_position = out.position;
        out.player_position_valid = true;
    }
    return true;
}

bool recheck_local_position(const Remote& remote, const Structure& s, const LocalPositionSample& sample) {
    std::int32_t current{};
    return read_local_vehicle_id(remote, s.local_character, current) &&
        (current == sample.vehicle_id || remote.fail("Local vehicle changed during snapshot", s.local_character, ERROR_RETRY));
}

struct LocalPositionTrace {
    bool initialized{};
    std::int32_t vehicle_id = -1;
    std::chrono::steady_clock::time_point next_log{};
};

void trace_local_position(LocalPositionTrace& trace, const LocalPositionSample& sample,
                          std::chrono::steady_clock::time_point now, bool enabled) {
    const bool changed = !trace.initialized || trace.vehicle_id != sample.vehicle_id;
    if (enabled && sample.vehicle_id >= 0 && (changed || now >= trace.next_log)) {
        const auto& p = sample.position;
        const auto& old = sample.player_position;
        const bool same = sample.player_position_valid && p.x == old.x && p.y == old.y && p.z == old.z;
        scarlex::diagnostic_printf("[APP] Local player in vehicle: vehicle_pos=(%.3f,%.3f,%.3f), player_pos=(%.3f,%.3f,%.3f) "
                    "vehicle_id=%ld source=%s player_pos_valid=%d positions_equal=%d vehicle_zero=%d\n",
                    p.x,p.y,p.z,old.x,old.y,old.z,static_cast<long>(sample.vehicle_id),sample.source,
                    sample.player_position_valid,same,p.x==0.0f && p.y==0.0f && p.z==0.0f);
        trace.next_log = now + std::chrono::seconds(1);
    } else if (enabled && trace.initialized && trace.vehicle_id >= 0 && sample.vehicle_id < 0) {
        scarlex::diagnostic_printf("[APP] Local player left vehicle: player_pos=(%.3f,%.3f,%.3f) source=player\n",
                    sample.position.x,sample.position.y,sample.position.z);
    }
    trace.initialized = true;
    trace.vehicle_id = sample.vehicle_id;
}

void sample_entity(const Remote& remote, CachedEntity& entry, Address player, const Vec3& local_position,
                   const Camera& camera, bool report_skeletons, Entity& entity, std::size_t& valid_heads) {
    const bool is_local = entry.address == player;
    entity.address = entry.address;
    entity.nickname = entry.nickname;
    entity.nickname_is_public_id = entry.nickname_is_public_id;
    entity.team_id = entry.team_id;
    entity.team_name = entry.team_name;
    auto skipped_skeleton = is_local
        ? SkeletonIssue{"BattleEntitiesModel.Player", 0xD0, "local-player", ERROR_SUCCESS}
        : SkeletonIssue{"BattleEntitiesUpdater._characterEntityViews", 0x110, "view-not-found", ERROR_NOT_FOUND};
    bool have_position = is_local;
    if (is_local) {
        entity.position = local_position;
    }
    Address skeleton_view{};
    Quaternion world_rotation{};
    bool have_skeleton_basis = false;
    if (entry.view != 0) {
        const Remote::Probe probe(remote);
        const bool view_ok = remote.pose_class(entry.view, "BattleCharacterEntityView",
                                                  "BattleCharacterEntityView class");
        if (!is_local) {
            if (!view_ok) {
                skipped_skeleton = {"BattleCharacterEntityView.klass", 0, "class-check-failed",
                                    GetLastError()};
            } else {
                skeleton_view = entry.view;
                have_position = read_feet(remote, entry.view, entity.position, world_rotation);
                have_skeleton_basis = have_position;
                if (!have_skeleton_basis) {
                    skipped_skeleton = {"MovementView/CharacterMovementView._transform", 0x20,
                                        "world-basis-unavailable", GetLastError()};
                }
            }
        }
        if (view_ok && read_head(remote, entry.view, entity.head_position)) {
            entity.valid = true;
            ++valid_heads;
        } else {
            remote.log("Head unavailable; entity.valid=false", entry.address, GetLastError(), "WARN");
        }
    }
    if (!have_position && entity.valid) {
        entity.position = entity.head_position;
        entity.position.y -= estimated_standing_height;
        have_position = true;
        remote.detail("DATA", "entity=%ld feet source=estimated-from-head height=%.1f m",
                      static_cast<long>(entry.id), estimated_standing_height);
    }
    if (!have_position) {
        // A model without a presentation view can still have a last known position.
        const Remote::Probe probe(remote);
        Address character{};
        have_position = remote.pose_ptr(entry.address, 0x30, character, "BattleCharacterEntityModel.SimulatedModel", true) && character &&
                        read_last_position(remote, character, entity.position);
    }
    if (skeleton_view != 0) {
        const Remote::Probe probe(remote);
        if (have_skeleton_basis && entry.probe_compact) {
            entity.skeleton_valid = read_entity_skeleton(remote, entity, world_rotation, report_skeletons);
            entry.probe_compact = entity.skeleton_valid;
            skipped_skeleton = {"SkeletonModel", 0, "no-usable-compact-pose"};
        }
        if (!entity.skeleton_valid && have_position && entity.valid) {
            entity.skeleton_valid = read_view_skeleton(remote, skeleton_view, entity, report_skeletons);
            if (!entity.skeleton_valid) {
                skipped_skeleton = {"BattleCharacterEntityView", 0, "invalid-head-feet-segment"};
            }
        }
        if (entity.skeleton_valid && have_position && entity.valid) {
            entity.limbs_valid = read_limb_pose(remote, skeleton_view,
                                                have_skeleton_basis ? &world_rotation : nullptr,
                                                camera.right, entity, report_skeletons);
        }
    }
    if (report_skeletons && !entity.skeleton_valid && skipped_skeleton.field) {
        remote.detail("SKELETON", "entity=0x%llX skeleton=unavailable field=%s offset=0x%llX "
                      "reason=%s win32=%lu",
                      static_cast<unsigned long long>(entity.address), skipped_skeleton.field,
                      static_cast<unsigned long long>(skipped_skeleton.offset), skipped_skeleton.reason,
                      static_cast<unsigned long>(skipped_skeleton.error));
    }
    if (!have_position) {
        entity.distance = std::numeric_limits<float>::infinity();
        return;
    }
    const double dx = static_cast<double>(entity.position.x) - local_position.x;
    const double dy = static_cast<double>(entity.position.y) - local_position.y;
    const double dz = static_cast<double>(entity.position.z) - local_position.z;
    const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (distance > (std::numeric_limits<float>::max)()) {
        entity.distance = std::numeric_limits<float>::infinity();
        return;
    }
    entity.distance = static_cast<float>(distance);
}

CharacterRole classify_role(std::wstring_view id) {
    if (id == L"cop") {
        return CharacterRole::cop;
    }
    if (id == L"robber") {
        return CharacterRole::robber;
    }
    if (id == L"settler" || id == L"citizen" || id == L"hubber") {
        return CharacterRole::neutral;
    }
    return CharacterRole::unknown;
}

void read_roles(const Remote& remote, PoseCache& pose, Address local_player, std::vector<Entity>& entities) {
    const Remote::Probe probe(remote);
    std::vector<Address> descriptions(entities.size());
    std::vector<WmPortReader::ReadRange> ranges;
    ranges.reserve(entities.size());
    for (std::size_t i = 0; i < entities.size(); ++i) {
        const auto entity = entities[i].address;
        Address character{}, model{}, data{};
        const Address server_offset = entity == local_player ? 0x20 : 0x28;
        // BattleCharacterModel.get_Role (RVA 0x9158C0) loads +0x208.
        // The role model wraps ReferenceData<PlayerRoleDescription>.
        if (remote.pose_ptr(entity, server_offset, character, "Role ServerModel", true) && character &&
            remote.pose_class(character, "BattleCharacterModel", "Role character class") &&
            remote.pose_ptr(character, 0x208, model, "BattleCharacterModel.Role", true) && model &&
            remote.pose_class(model, "PrimitiveModel`1", "Role model class") &&
            remote.pose_ptr(model, 0x10, data, "Role model data", true) && data &&
            remote.pose_class(data, "ReferenceData`1", "Role data class")) {
            // The wrapper is stable. Sample its mutable value every update.
            ranges.push_back({data + 0x18, &descriptions[i], sizeof(Address)});
        }
    }
    remote.batch(ranges);
    for (const auto& range : ranges) {
        if (!range.success) {
            *static_cast<Address*>(range.buffer) = 0;
        }
    }
    for (std::size_t i = 0; i < entities.size(); ++i) {
        const auto description = descriptions[i];
        if (!remote.pointer(description)) {
            continue;
        }
        if (const auto saved = pose.roles.find(description); saved != pose.roles.end()) {
            entities[i].role = saved->second.role;
            entities[i].role_id = saved->second.id;
            continue;
        }
        Address id{};
        std::int32_t length{};
        std::array<wchar_t, 32> name{};
        if (remote.pose_class(description, "PlayerRoleDescription", "Role description class") &&
            remote.pose_ptr(description, 0x10, id, "PlayerRoleDescription.Id") &&
            remote.value(id, 0x10, length, "Role ID length") && length > 0 &&
            static_cast<std::size_t>(length) <= name.size() &&
            remote.bytes(id, 0x14, name.data(), static_cast<std::size_t>(length) * sizeof(wchar_t), "Role ID")) {
            std::array<char, name.size() * 3> utf8{};
            const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name.data(), length,
                                                   utf8.data(), static_cast<int>(utf8.size()), nullptr, nullptr);
            if (bytes > 0) {
                auto& entity = entities[i];
                entity.role = classify_role({name.data(), static_cast<std::size_t>(length)});
                entity.role_id.assign(utf8.data(), static_cast<std::size_t>(bytes));
                pose.roles.emplace(description, PoseCache::RoleDescription{entity.role, entity.role_id});
            }
        }
    }
}

using MotionClock = std::chrono::steady_clock;

constexpr const char* loot_stage = "World loot";
constexpr std::size_t max_inventory_slots = 512;

struct LootCatalog {
    struct Description {
        std::string id;
        std::uint8_t type{};
        int rarity = -1;
    };
    Address module{};
    Address items{};
    std::int32_t size{};
    std::array<std::uint32_t, 5> packing{};  // PayloadShift, ItemShift, DurabilityMask, PayloadMask, ItemMask.
    std::unordered_map<std::uint32_t, Address> by_index;
    std::unordered_map<Address, Description> descriptions;
    std::unordered_map<Address, Address> descriptions_by_delegate;
    MotionClock::time_point retry_after{};
};

enum class LootSource {
    bags, containers, cargo, vehicle_loot, static_items, random_items, nodes, trunks,
    building_cargo, storage, plot_containers, furniture, building, cash_nodes, plot, shelves, collection,
    plot_trunks,
};

struct LootRegistry {
    LootSource source;
    Address offset;
    const char* field;
    LootKind type;
    const char* model;
};

constexpr std::array loot_registries{
    LootRegistry{LootSource::bags, 0x38, "_loots", LootKind::bag, "LootModel"},
    LootRegistry{LootSource::containers, 0x20, "_containers", LootKind::container, "ContainerModel"},
    LootRegistry{LootSource::cargo, 0xC8, "_cargos", LootKind::cargo, "CargoModel"},
    LootRegistry{LootSource::vehicle_loot, 0x40, "_vehicleLoots", LootKind::vehicle, "VehicleLootModel"},
    LootRegistry{LootSource::static_items, 0x98, "_staticLootObjects", LootKind::ground_item, "StaticLootObjectModel"},
    LootRegistry{LootSource::random_items, 0xA0, "_randomLootObjects", LootKind::ground_item, "RandomLootObjectModel"},
    LootRegistry{LootSource::nodes, 0xB0, "_lootNodes", LootKind::container, "LootNodeModel"},
    LootRegistry{LootSource::trunks, 0xC0, "_vehicleTrunks", LootKind::vehicle, "VehicleTrunkModel"},
    LootRegistry{LootSource::building_cargo, 0x100, "_buildingCargos", LootKind::cargo, nullptr},
    LootRegistry{LootSource::storage, 0x128, "_storages", LootKind::container, "StorageBoxModel"},
    LootRegistry{LootSource::plot_containers, 0x140, "_plotContainers", LootKind::container, "PlotContainerModel"},
    LootRegistry{LootSource::furniture, 0x220, "_furnitureLoots", LootKind::container, "FurnitureLootModel"},
    LootRegistry{LootSource::building, 0x238, "_buildingLoots", LootKind::container, "BuildingLootModel"},
    LootRegistry{LootSource::cash_nodes, 0x268, "_cashLootNodes", LootKind::ground_item, "CashLootNodeModel"},
    LootRegistry{LootSource::plot, 0x2F0, "_plotLoots", LootKind::container, "PlotLootModel"},
    LootRegistry{LootSource::shelves, 0x148, "_shelvings", LootKind::container, "ShelvingModel"},
    LootRegistry{LootSource::collection, 0xD8, "_collections", LootKind::cargo, "CollectionTableModel"},
    LootRegistry{LootSource::plot_trunks, 0x1E8, "_plotVehicleTrunks", LootKind::vehicle, "PlotVehicleTrunkModel"},
};

struct LootCollection {
    DictionarySnapshot dictionary;
    std::vector<LootItem> items;
    std::vector<LootNode> nodes;
    MotionClock::time_point refresh_after{};
};

struct WorldLootCache {
    Address world{};
    std::array<LootCollection, loot_registries.size()> collections;
    MotionClock::time_point refresh_after{};
    LootCatalog catalog;
    std::size_t cursor{};
};

bool useful_loot_position(Vec3 position) noexcept {
    return finite(position) && (position.x != 0.0f || position.y != 0.0f || position.z != 0.0f);
}

bool read_loot_position(const Remote& remote, Address object, Vec3& position, unsigned depth = 0) {
    Address klass{};
    if (depth >= 4 || !remote.ptr(object, 0, klass, "Loot position class")) return false;
    const auto is_class = [&](std::string_view name) {
        bool match{};
        return remote.class_has_name(klass, name, match, "Loot position class name") && match;
    };
    if (is_class("PositionModel") || is_class("InteractPositionModel") || is_class("SetMovementModel") ||
        is_class("VehicleTrunkPositionModel") || is_class("NoReplicationSimpleDotPhysicsSourceModel")) {
        return remote.value(object, 0x10, position, "Loot position XYZ") && finite(position);
    }
    if (is_class("Transform")) return read_transform_position(remote, object, position);
    if (is_class("OffsetPositionModel")) {
        Address base{};
        Vec3 offset{};
        if (!remote.ptr(object, 0x10, base, "Loot relative position base") ||
            !remote.value(object, 0x18, offset, "Loot relative position offset") ||
            !read_loot_position(remote, base, position, depth + 1)) return false;
        position = {position.x + offset.x, position.y + offset.y, position.z + offset.z};
        return finite(position);
    }
    if (is_class("TransformModel") || is_class("SetPositionModel")) {
        std::array<Address, 3> axes{};
        if (!remote.value(object, 0x10, axes, "Loot position coordinates")) return false;
        const std::array<float*, 3> xyz{&position.x, &position.y, &position.z};
        std::array<WmPortReader::ReadRange, 3> ranges{};
        for (std::size_t i = 0; i < axes.size(); ++i) {
            Address axis_class{};
            bool is_float{}, is_primitive{};
            if (!remote.pointer(axes[i]) || !remote.ptr(axes[i], 0, axis_class, "Loot coordinate class") ||
                !remote.class_has_name(axis_class, "FloatData", is_float, "Loot coordinate class") ||
                (!is_float && (!remote.class_has_name(axis_class, "PrimitiveData`1", is_primitive,
                                                     "Loot coordinate class") || !is_primitive))) return false;
            ranges[i] = {axes[i] + 0x18, xyz[i], sizeof(float)};
        }
        remote.batch(ranges);
        return std::all_of(ranges.begin(), ranges.end(), [](const auto& range) { return range.success; }) &&
               finite(position);
    }
    return false;
}

Address loot_ref(const Remote& remote, Address object, Address offset) {
    Address value{};
    return object && remote.ptr(object, offset, value, loot_stage, true) ? value : 0;
}

bool loot_position_at(const Remote& remote, Address object, Address offset, Vec3& out) {
    const Address model = loot_ref(remote, object, offset);
    return model && read_loot_position(remote, model, out) && useful_loot_position(out);
}

bool inline_loot_position(const Remote& remote, Address object, Address offset, Vec3& out) {
    return object && remote.value(object, offset, out, loot_stage) && useful_loot_position(out);
}

std::string loot_id(const Remote& remote, Address description) {
    const Address id = loot_ref(remote, description, 0x10);
    std::int32_t size{};
    std::array<wchar_t, 96> name{};
    if (!id || !remote.value(id, 0x10, size, loot_stage) || size <= 0 ||
        static_cast<std::size_t>(size) > name.size() ||
        !remote.bytes(id, 0x14, name.data(), static_cast<std::size_t>(size) * sizeof(wchar_t), loot_stage)) return {};
    std::array<char, name.size() * 3> utf8{};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name.data(), size,
                                          utf8.data(), static_cast<int>(utf8.size()), nullptr, nullptr);
    return length > 0 ? std::string(utf8.data(), static_cast<std::size_t>(length)) : std::string{};
}

bool sample_loot_node(const Remote& remote, const Entry& entry, LootNode& node) {
    node = {};
    node.runtime_id = entry.entity_id;
    node.address = entry.value;
    if (!remote.require_class(entry.value, "LootNodeModel", "Loot node model")) return false;
    const Address description = loot_ref(remote, entry.value, 0x10);
    if (!description || !remote.require_class(description, "LocationLootNodeDescription", "Loot node definition"))
        return false;
    node.definition_address = description;
    node.definition_id = loot_id(remote, description);
    if (!remote.value(description, 0x18, node.definition_index, "Loot node definition index"))
        node.definition_index = -1;
    if (!remote.value(description, 0x20, node.definition_entity_id, "Loot node definition entity ID")) return false;
    if (node.runtime_id != node.definition_entity_id) {
        return remote.fail("Loot node definition owner changed", description, ERROR_RETRY);
    }
    node.position_valid = remote.value(description, 0x38, node.position, "Loot node world position") &&
                          finite(node.position);
    if (!node.position_valid) {
        node.position_from_interact_point = true;
        node.position_valid = remote.value(description, 0x54, node.position, "Loot node interaction position") &&
                              finite(node.position);
    }
    if (!node.position_valid) node.position = {};
    return true;
}

void log_loot_nodes(const std::vector<LootNode>& current, const std::vector<LootNode>& previous) {
    for (const auto& node : current) {
        const auto metadata = game_data::lookup(node.definition_id);
        game_data::report_unmatched(node.definition_id, metadata, "loot-node");
        const auto old = std::find_if(previous.begin(), previous.end(), [&](const auto& other) {
            return other.runtime_id == node.runtime_id && other.address == node.address &&
                   other.definition_address == node.definition_address;
        });
        if (old != previous.end() && node.definition_id == old->definition_id &&
            node.position_valid == old->position_valid &&
            node.position_from_interact_point == old->position_from_interact_point &&
            node.position.x == old->position.x && node.position.y == old->position.y &&
            node.position.z == old->position.z) continue;
        scarlex::diagnostic_printf("[LOOT-NODE] runtime_id=%ld definition_id=%s raw_id=%s normalized_id=%s matched=%d override=%d index=%ld "
                    "position=(%.6f,%.6f,%.6f) valid=%d source=%s "
                    "model=0x%llX definition=0x%llX donor_id=unmapped\n",
                    static_cast<long>(node.runtime_id), node.definition_id.c_str(), node.definition_id.c_str(),
                    metadata.normalized_id.c_str(), metadata.found(), metadata.override_applied,
                    static_cast<long>(node.definition_index), node.position.x, node.position.y, node.position.z,
                    node.position_valid, node.position_from_interact_point ? "InteractPoint+0x54" : "Position+0x38",
                    static_cast<unsigned long long>(node.address),
                    static_cast<unsigned long long>(node.definition_address));
    }
}

bool taken_loot(const Remote& remote, Address object, Address offset) {
    const Address flag = loot_ref(remote, object, offset);
    std::uint8_t value{};
    return flag && (remote.require_class(flag, "BoolData", loot_stage) ||
                    remote.require_class(flag, "PrimitiveData`1", loot_stage)) &&
           remote.value(flag, 0x18, value, loot_stage) && value == 1;
}

int description_rarity(const Remote& remote, Address description, Address offset) {
    int value = -1;
    return description && remote.value(description, offset, value, "Item rarity") &&
           value >= 0 && value <= 6 ? value : -1;
}

const LootCatalog::Description* describe_inventory_item(const Remote& remote, LootCatalog& catalog,
                                                        Address description) {
    if (const auto saved = catalog.descriptions.find(description); saved != catalog.descriptions.end()) {
        if (saved->second.rarity < 0) saved->second.rarity = description_rarity(remote, description, 0x30);
        if (saved->second.id.empty()) saved->second.id = loot_id(remote, description);
        return &saved->second;
    }
    if (!description || !remote.require_class(description, "InventoryItemDescription", loot_stage)) return nullptr;
    LootCatalog::Description metadata;
    metadata.id = loot_id(remote, description);
    if (!remote.value(description, 0x20, metadata.type, loot_stage)) return nullptr;
    metadata.rarity = description_rarity(remote, description, 0x30);
    return &catalog.descriptions.emplace(description, std::move(metadata)).first->second;
}

bool weapon_type(std::uint8_t type) noexcept {
    return type == 3 || type == 6 || type == 13 || type == 14;
}

void prepare_catalog(const Remote& remote, LootCatalog& catalog, WmPortReader& reader, std::uint32_t pid,
                     MotionClock::time_point now) {
    if (catalog.items || now < catalog.retry_after) return;
    catalog.retry_after = now + std::chrono::seconds(2);
    if (!catalog.module) catalog.module = reader.module_base(pid, "GameAssembly.dll").value_or(0);
    if (!catalog.module) return;
    // Verified in InventorySlot.get_Item and get_Payload (0x48DF400, 0x48DF4D0).
    const Address packing_class = loot_ref(remote, catalog.module, 0x8202E50);
    const Address packing = loot_ref(remote, packing_class, 0xB8);
    const Address parameter_class = loot_ref(remote, catalog.module, 0x8146750);
    const Address singleton = loot_ref(remote, loot_ref(remote, parameter_class, 0xB8), 0);
    const Address descriptions = loot_ref(remote, singleton, 0x658);
    const Address list = loot_ref(remote, descriptions, 0x10);
    std::int32_t size{};
    std::uint64_t capacity{};
    const Address items = loot_ref(remote, list, 0x10);
    bool slot_class{};
    if (packing && remote.class_has_name(packing_class, "InventorySlot", slot_class, loot_stage) && slot_class &&
        remote.value(packing, 0x10, catalog.packing, loot_stage) &&
        catalog.packing[0] < catalog.packing[1] && catalog.packing[1] < 32 &&
        catalog.packing[3] && catalog.packing[4] && items &&
        remote.value(list, 0x18, size, loot_stage) && size > 0 && size < 8192 &&
        remote.value(items, array_length_offset, capacity, loot_stage) && static_cast<std::uint64_t>(size) <= capacity) {
        catalog.items = items;
        catalog.size = size;
    }
}

std::optional<LootStack> inventory_item(const Remote& remote, LootCatalog& catalog, std::uint32_t raw) {
    if (!raw || !catalog.items) return std::nullopt;
    const auto index = (raw >> catalog.packing[1]) & catalog.packing[4];
    if (!index || index > static_cast<std::uint32_t>(catalog.size)) return std::nullopt;
    Address description{};
    if (const auto it = catalog.by_index.find(index); it != catalog.by_index.end()) {
        description = it->second;
    } else {
        description = loot_ref(remote, catalog.items, array_data_offset + (index - 1) * sizeof(Address));
        std::int32_t actual_index{};
        if (!description || !remote.require_class(description, "InventoryItemDescription", loot_stage) ||
            !remote.value(description, 0x18, actual_index, loot_stage) ||
            actual_index != static_cast<std::int32_t>(index - 1)) return std::nullopt;
        catalog.by_index.emplace(index, description);
    }
    std::uint8_t stackable{};
    if (!remote.value(description, 0x34, stackable, loot_stage) || stackable > 1) return std::nullopt;
    const auto count = stackable ? (raw >> catalog.packing[0]) & catalog.packing[3] : 1u;
    const auto* metadata = describe_inventory_item(remote, catalog, description);
    if (!count || !metadata) return std::nullopt;
    LootStack item;
    item.name = metadata->id;
    item.name_clean = item_presentation::clean_name(item.name, std::to_string(index - 1));
    item.count = count;
    item.type = weapon_type(metadata->type) || item_presentation::weapon_name(item.name)
        ? LootKind::weapon : LootKind::ground_item;
    item.rarity = metadata->rarity;
    return item;
}

void add_loot_stack(std::vector<LootStack>& items, LootStack stack) {
    for (auto& item : items) {
        if (item.type == stack.type && item.name == stack.name && item.rarity == stack.rarity &&
            item.name_clean == stack.name_clean) {
            item.count += stack.count;
            return;
        }
    }
    items.push_back(std::move(stack));
}

bool read_loot_inventory(const Remote& remote, LootCatalog& catalog, Address inventory,
                         std::vector<LootStack>& contents) {
    if (!inventory || !catalog.items || !remote.require_class(inventory, "InventoryModel", loot_stage)) return false;
    const Address slots = loot_ref(remote, inventory, 0x18);
    DictionarySnapshot entries;
    // Reference keys still leave the reference value at +0x10 and stride +0x18.
    if (!slots || !remote.require_class(slots, "InventorySlotsModel", loot_stage) ||
        !read_dictionary(remote, slots, 0x10, entries, loot_stage) || entries.live.size() > max_inventory_slots)
        return false;
    std::vector<LootStack> sampled;
    for (const auto& entry : entries.live) {
        if (!remote.require_class(entry.value, "InventorySlotModel", loot_stage)) return false;
        const Address stack = loot_ref(remote, entry.value, 0x20);
        const Address data = loot_ref(remote, stack, 0x10);
        std::uint32_t raw{};
        if (!stack || !data || !remote.require_class(stack, "InventorySlotStackModel", loot_stage) ||
            !remote.require_class(data, "InventorySlotData", loot_stage) ||
            !remote.value(data, 0x18, raw, loot_stage)) return false;
        if (((raw >> catalog.packing[1]) & catalog.packing[4]) == 0) continue;
        auto item = inventory_item(remote, catalog, raw);
        if (!item) return false;
        add_loot_stack(sampled, std::move(*item));
    }
    if (!unchanged(remote, slots, 0x10, entries.address, loot_stage) ||
        !recheck_dictionary(remote, entries, loot_stage)) return false;
    for (auto& item : sampled) add_loot_stack(contents, std::move(item));
    return true;
}

bool read_cargo_inventory(const Remote& remote, Address inventory, std::vector<LootStack>& contents) {
    if (!inventory || !remote.require_class(inventory, "CargoInventoryModel", loot_stage)) return false;
    DictionarySnapshot slots;
    if (!read_dictionary(remote, inventory, 0x10, slots, loot_stage) ||
        slots.live.size() > max_inventory_slots) return false;
    std::vector<LootStack> sampled;
    for (const auto& entry : slots.live) {
        if (!remote.require_class(entry.value, "CargoInventorySlotModel", loot_stage)) return false;
        const Address cargo = loot_ref(remote, entry.value, 0x10);
        const Address description_data = loot_ref(remote, cargo, 0x20);
        const Address description = loot_ref(remote, description_data, 0x18);
        if (description && remote.require_class(description, "CargoDescription", loot_stage)) {
            LootStack item;
            item.name = loot_id(remote, description);
            item.name_clean = item_presentation::clean_name(item.name);
            item.count = 1;
            item.type = LootKind::cargo;
            item.rarity = description_rarity(remote, description, 0x20);
            add_loot_stack(sampled, std::move(item));
        }
    }
    if (!unchanged(remote, inventory, 0x10, slots.address, loot_stage) ||
        !recheck_dictionary(remote, slots, loot_stage)) return false;
    for (auto& item : sampled) add_loot_stack(contents, std::move(item));
    return true;
}

struct LootLookup {
    const Remote& remote;
    Address world;
    std::unordered_map<Address, std::unordered_map<std::int32_t, Address>> by_field;

    Address find(Address offset, std::int32_t id) {
        auto [it, fresh] = by_field.try_emplace(offset);
        if (fresh) {
            DictionarySnapshot list;
            if (read_dictionary(remote, world, offset, list, loot_stage) &&
                unchanged(remote, world, offset, list.address, loot_stage) &&
                recheck_dictionary(remote, list, loot_stage)) {
                for (const auto& entry : list.live) it->second.emplace(entry.entity_id, entry.value);
            }
        }
        const auto found = it->second.find(id);
        return found == it->second.end() ? 0 : found->second;
    }
};

bool sample_trunk_position(const Remote& remote, LootLookup& lookup, std::int32_t id, bool parked, Vec3& out) {
    if (id == (std::numeric_limits<std::int32_t>::min)()) return false;
    // VehicleTrunkSettingsDescription.GetVehicleEntityId at 0x403CA60 returns trunkId - 1.
    if (parked) {
        const Address model = lookup.find(0x1D0, id - 1);
        if (!model || (!remote.require_class(model, "PlotCarModel", loot_stage) &&
                       !remote.require_class(model, "PlotHelicopterModel", loot_stage))) return false;
        const Address location = loot_ref(remote, loot_ref(remote, model, 0x20), 0x10);
        return location && remote.require_class(location, "LocationVehicleSlotDescription`1", loot_stage) &&
               inline_loot_position(remote, location, 0x20, out);
    }
    struct VehiclePosition { Address registry, offset; const char* type; };
    constexpr std::array positions{
        VehiclePosition{0x58, 0xF8, "VehicleModel"}, VehiclePosition{0x60, 0x1A0, "CarModel"},
        VehiclePosition{0x68, 0x150, "HelicopterModel"},
    };
    for (const auto& p : positions) {
        const Address model = lookup.find(p.registry, id - 1);
        if (model && remote.require_class(model, p.type, loot_stage) &&
            loot_position_at(remote, model, p.offset, out)) return true;
    }
    return false;
}

void attach_contents(const Remote& remote, LootCatalog& catalog, LootLookup& lookup, LootSource source,
                     Address description, LootItem& item) {
    const auto id_at = [&](Address object, Address offset) {
        std::int32_t id{};
        return object && remote.value(object, offset, id, loot_stage) && id != 0 ? id : item.entity_id;
    };
    Address inventory{}, cargo{}, pages{}, cargo_pages{};
    switch (source) {
    case LootSource::bags: {
        if (const auto model = lookup.find(0x160, item.entity_id);
            model && remote.require_class(model, "LootEntityInventoryModel", loot_stage)) {
            inventory = loot_ref(remote, model, 0x30);
        }
        break;
    }
    case LootSource::containers: {
        if (const auto model = lookup.find(0x168, id_at(description, 0x24));
            model && remote.require_class(model, "ContainerEntityInventoryModel", loot_stage)) {
            inventory = loot_ref(remote, model, 0x18);
        }
        break;
    }
    case LootSource::vehicle_loot: {
        if (const auto model = lookup.find(0x170, item.entity_id);
            model && remote.require_class(model, "VehicleLootEntityInventoryModel", loot_stage)) {
            inventory = loot_ref(remote, model, 0x18);
            cargo = loot_ref(remote, model, 0x28);
        }
        break;
    }
    case LootSource::plot_trunks:
    case LootSource::trunks: {
        inventory = loot_ref(remote, item.address, 0x20);
        cargo = loot_ref(remote, item.address, 0x38);
        if ((!inventory || !cargo) && source == LootSource::trunks) {
            const Address model = lookup.find(0x1F0, item.entity_id);
            if (model && remote.require_class(model, "VehicleTrunkInventoriesModel", loot_stage)) {
                if (!inventory) inventory = loot_ref(remote, model, 0x18);
                if (!cargo) cargo = loot_ref(remote, model, 0x20);
            }
        }
        break;
    }
    case LootSource::shelves:
        cargo = loot_ref(remote, item.address, 0x20);
        break;
    case LootSource::storage: {
        const auto ids = loot_ref(remote, description, 0x10);
        if (const auto model = lookup.find(0x130, id_at(ids, 0x14));
            model && remote.require_class(model, "StorageBoxInventoryModel", loot_stage)) {
            inventory = loot_ref(remote, model, 0x18);
        }
        break;
    }
    case LootSource::plot_containers: {
        const auto ids = loot_ref(remote, description, 0x18);
        if (const auto model = lookup.find(0x138, id_at(ids, 0x14));
            model && remote.require_class(model, "PlotContainerInventoryModel", loot_stage)) {
            inventory = loot_ref(remote, model, 0x18);
        }
        break;
    }
    case LootSource::furniture: {
        if (const auto model = lookup.find(0x228, item.entity_id);
            model && remote.require_class(model, "FurnitureLootEntityInventoryModel", loot_stage)) {
            pages = loot_ref(remote, model, 0x28);
            cargo_pages = loot_ref(remote, model, 0x30);
        }
        break;
    }
    case LootSource::building: {
        const auto ids = loot_ref(remote, description, 0x10);
        if (const auto model = lookup.find(0x240, id_at(ids, 0x14));
            model && remote.require_class(model, "BuilderLootEntityInventoryModel", loot_stage)) {
            pages = loot_ref(remote, model, 0x20);
            cargo_pages = loot_ref(remote, model, 0x28);
        }
        break;
    }
    default: return;
    }
    std::vector<LootStack> contents;
    bool read = read_loot_inventory(remote, catalog, inventory, contents);
    read |= read_cargo_inventory(remote, cargo, contents);
    const auto read_pages = [&](Address owner, Address offset, const char* type, bool cargo_page) {
        DictionarySnapshot entries;
        if (!owner || !remote.require_class(owner, type, loot_stage) ||
            !read_dictionary(remote, owner, offset, entries, loot_stage) || entries.live.size() > 32) return false;
        std::vector<LootStack> sampled;
        bool available = entries.live.empty();
        for (const auto& entry : entries.live) {
            available |= cargo_page ? read_cargo_inventory(remote, entry.value, sampled)
                                    : read_loot_inventory(remote, catalog, entry.value, sampled);
        }
        if (!available || !unchanged(remote, owner, offset, entries.address, loot_stage) ||
            !recheck_dictionary(remote, entries, loot_stage)) return false;
        for (auto& stack : sampled) add_loot_stack(contents, std::move(stack));
        return true;
    };
    read |= read_pages(pages, 0x20, "InventoryPagesModel", false);
    read |= read_pages(cargo_pages, 0x28, "CargoInventoryPagesModel", true);
    if (read) item.contents = std::move(contents);
}

Address loot_delegate_value(const Remote& remote, LootCatalog& catalog, Address delegate,
                            std::string_view expected) {
    if (!delegate || !catalog.module) return 0;
    if (const auto it = catalog.descriptions_by_delegate.find(delegate);
        it != catalog.descriptions_by_delegate.end()) return it->second;
    const Address info = loot_ref(remote, delegate, 0x28);
    Address code{};
    if ((!info || !remote.value(info, 0, code, loot_stage)) &&
        !remote.value(delegate, 0x10, code, loot_stage)) return 0;
    // Match generated getters and their thunks without invoking game code.
    for (int hop = 0; hop < 3; ++hop) {
        if (code < catalog.module + 0x1000 || code >= catalog.module + 0x673C000) return 0;
        std::array<std::uint8_t, 104> bytes{};
        if (!remote.value(code, 0, bytes, loot_stage)) return 0;
        if (bytes[0] == 0xE9) {
            std::int32_t displacement{};
            std::memcpy(&displacement, bytes.data() + 1, sizeof(displacement));
            code = static_cast<Address>(static_cast<std::int64_t>(code + 5) + displacement);
            continue;
        }
        Address result{};
        if (bytes[0] == 0x48 && bytes[1] == 0x8B && bytes[2] == 0x41 && bytes[4] == 0xC3) {
            result = loot_ref(remote, loot_ref(remote, delegate, 0x20), bytes[3]);
        }
        // Shared generated closure: return this.enum[this.index].
        constexpr std::array<std::uint8_t, 18> indexed_getter{
            0x48, 0x83, 0xEC, 0x28, 0x4C, 0x8B, 0x49, 0x10, 0x4D, 0x85, 0xC9,
            0x74, 0x1E, 0x48, 0x8B, 0x42, 0x20, 0x8B,
        };
        if (!result && std::equal(indexed_getter.begin(), indexed_getter.end(), bytes.begin()) &&
            bytes[18] == 0x51 && bytes[19] == 0x18) {
            const Address closure = loot_ref(remote, delegate, 0x20);
            const Address list = loot_ref(remote, loot_ref(remote, closure, 0x10), 0x10);
            const Address items = loot_ref(remote, list, 0x10);
            std::int32_t index{}, size{};
            std::uint64_t capacity{};
            if (closure && items && remote.value(closure, 0x18, index, loot_stage) &&
                remote.value(list, 0x18, size, loot_stage) &&
                remote.value(items, array_length_offset, capacity, loot_stage) &&
                index >= 0 && size <= 8192 && index < size && static_cast<std::uint64_t>(size) <= capacity) {
                result = loot_ref(remote, items, array_data_offset + static_cast<Address>(index) * sizeof(Address));
            }
        }
        for (std::size_t i = 0; !result && i + 21 <= bytes.size(); ++i) {
            const auto* p = bytes.data() + i;
            constexpr std::array<std::uint8_t, 7> statics{0x48, 0x8B, 0x80, 0xB8, 0, 0, 0};
            if (p[0] != 0x48 || p[1] != 0x8B || p[2] != 0x05 ||
                !std::equal(statics.begin(), statics.end(), p + 7) || p[14] != 0x48 || p[15] != 0x8B) continue;
            std::int32_t displacement{}, offset{};
            std::memcpy(&displacement, p + 3, sizeof(displacement));
            if (p[16] == 0x40) offset = p[17];
            else if (p[16] == 0x80) std::memcpy(&offset, p + 17, sizeof(offset));
            else continue;
            const auto slot = static_cast<Address>(static_cast<std::int64_t>(code + i + 7) + displacement);
            if (offset < 0 || offset > 0x4000 || slot < catalog.module + 0x7F39000 ||
                slot >= catalog.module + 0x88DE000) continue;
            result = loot_ref(remote, loot_ref(remote, loot_ref(remote, slot, 0), 0xB8),
                              static_cast<Address>(offset));
        }
        if (!result || !remote.require_class(result, expected, loot_stage)) return 0;
        catalog.descriptions_by_delegate.emplace(delegate, result);
        return result;
    }
    return 0;
}

void classify_ground_loot(const Remote& remote, LootCatalog& catalog, Address description, LootItem& item) {
    if (!description || !remote.require_class(description, "LootObjectItemDescription", loot_stage)) return;
    const Address inventory_item = loot_delegate_value(remote, catalog,
        loot_ref(remote, description, 0x20), "InventoryItemDescription");
    const auto* metadata = describe_inventory_item(remote, catalog, inventory_item);
    if (metadata) {
        item.name = metadata->id;
        item.rarity = metadata->rarity;
        if (weapon_type(metadata->type)) item.type = LootKind::weapon;
    }
    if (item.name.empty()) item.name = loot_id(remote, description);
}

bool sample_world_loot(const Remote& remote, LootCatalog& catalog, LootLookup& lookup,
                       const LootRegistry& layout, const Entry& entry, LootItem& item) {
    const Address model = entry.value;
    if (layout.model && !remote.require_class(model, layout.model, layout.field)) return false;
    item.entity_id = entry.entity_id;
    item.address = model;
    item.type = layout.type;
    auto& p = item.position;
    Address description{}, identified{};
    bool located{};
    const auto checked = [&](Address parent, Address offset, std::string_view type) {
        const Address result = loot_ref(remote, parent, offset);
        return result && remote.require_class(result, type, loot_stage) ? result : Address{};
    };
    switch (layout.source) {
    case LootSource::bags:
    case LootSource::vehicle_loot:
    case LootSource::cargo: {
        const bool cargo = layout.source == LootSource::cargo;
        const bool bag = layout.source == LootSource::bags;
        located = loot_position_at(remote, model, cargo ? 0x30 : 0x20, p) ||
                  loot_position_at(remote, model, bag ? 0x48 : 0x40, p) ||
                  loot_position_at(remote, model, cargo ? 0x38 : 0x28, p);
        if (!located) return false;
        if (cargo) identified = checked(model, 0x28, "CargoDescription");
        if (bag) identified = checked(loot_ref(remote, loot_ref(remote, model, 0x30), 0x10), 0x18,
                                      "CharacterBagDescription");
        break;
    }
    case LootSource::containers:
        description = identified = checked(model, 0x18, "LocationContainerDescription");
        located = loot_position_at(remote, model, 0x28, p) || loot_position_at(remote, model, 0x20, p) ||
                  inline_loot_position(remote, description, 0x30, p) ||
                  inline_loot_position(remote, description, 0x4C, p);
        break;
    case LootSource::static_items:
    case LootSource::random_items: {
        if (taken_loot(remote, model, 0x10)) return false;
        const bool random = layout.source == LootSource::random_items;
        description = identified = checked(model, random ? 0x20 : 0x18,
            random ? "RandomLootObjectDescription" : "StaticLootObjectDescription");
        located = inline_loot_position(remote, description, random ? 0x24 : 0x30, p) ||
                  inline_loot_position(remote, description, random ? 0x40 : 0x4C, p);
        if (!located) return false;
        const Address item_desc = random ? loot_ref(remote, loot_ref(remote, model, 0x18), 0x18)
            : loot_delegate_value(remote, catalog, loot_ref(remote, description, 0x28), "LootObjectItemDescription");
        if (random && !item_desc) return false;
        classify_ground_loot(remote, catalog, item_desc, item);
        break;
    }
    case LootSource::nodes: {
        description = identified = checked(model, 0x10, "LocationLootNodeDescription");
        located = inline_loot_position(remote, description, 0x38, p) ||
                  inline_loot_position(remote, description, 0x54, p);
        const auto loot = loot_delegate_value(remote, catalog, loot_ref(remote, description, 0x30),
                                               "LootNodeLootDescription");
        const auto definition = loot_delegate_value(remote, catalog, loot_ref(remote, loot, 0x30),
                                                     "InventoryItemDescription");
        if (const auto* metadata = describe_inventory_item(remote, catalog, definition)) {
            item.name = metadata->id;
            item.rarity = metadata->rarity;
        }
        break;
    }
    case LootSource::trunks:
    case LootSource::plot_trunks:
        located = sample_trunk_position(remote, lookup, item.entity_id, layout.source == LootSource::plot_trunks, p);
        item.name = "Vehicle trunk";
        break;
    case LootSource::building_cargo: {
        bool random{};
        if (!remote.is_class(model, "BuildingRandomCargoModel", random, loot_stage)) return false;
        if (!random && (!remote.require_class(model, "BuildingStaticCargoModel", loot_stage) ||
                        taken_loot(remote, model, 0x10))) return false;
        description = identified = checked(model, 0x18,
            random ? "LocationRandomCargoDescription" : "LocationStaticCargoDescription");
        located = inline_loot_position(remote, description, 0x24, p);
        if (random) {
            identified = checked(loot_ref(remote, model, 0x10), 0x18, "CargoDescription");
            if (!identified) return false;
        } else {
            const auto cargo = loot_delegate_value(remote, catalog, loot_ref(remote, description, 0x30),
                                                   "CargoDescription");
            if (cargo) identified = cargo;
        }
        break;
    }
    case LootSource::storage:
        description = checked(loot_ref(remote, model, 0x20), 0x10, "LocationStorageDescription");
        identified = checked(description, 0x18, "StorageDescription");
        located = inline_loot_position(remote, description, 0x34, p) ||
                  inline_loot_position(remote, description, 0x28, p);
        break;
    case LootSource::plot_containers:
        description = checked(model, 0x10, "LocationPlotContainerDescription");
        identified = checked(description, 0x20, "PlotContainerDescription");
        located = loot_position_at(remote, model, 0x38, p) ||
                  inline_loot_position(remote, description, 0x50, p) ||
                  inline_loot_position(remote, description, 0x38, p);
        break;
    case LootSource::furniture:
        located = loot_position_at(remote, model, 0x18, p) || loot_position_at(remote, model, 0x20, p);
        item.name = "Furniture loot";
        break;
    case LootSource::building:
        description = checked(model, 0x10, "LocationBuildingLootDescription");
        located = loot_position_at(remote, model, 0x18, p) || inline_loot_position(remote, description, 0x18, p);
        item.name = "Building loot";
        break;
    case LootSource::cash_nodes: {
        description = identified = checked(model, 0x18, "LocationCashLootNodeDescription");
        located = inline_loot_position(remote, description, 0x24, p) ||
                  inline_loot_position(remote, description, 0x30, p);
        const Address cash = loot_ref(remote, model, 0x10);
        std::int32_t amount{};
        if (cash && remote.require_class(cash, "IntData", loot_stage) &&
            remote.value(cash, 0x18, amount, loot_stage)) {
            if (amount <= 0) return false;
            LootStack stack;
            stack.name = stack.name_clean = "Cash";
            stack.count = static_cast<std::uint32_t>(amount);
            item.contents = std::vector<LootStack>{std::move(stack)};
        }
        break;
    }
    case LootSource::plot:
        located = loot_position_at(remote, model, 0x18, p);
        item.name = "Plot loot";
        break;
    case LootSource::shelves:
        description = checked(loot_ref(remote, model, 0x18), 0x10, "LocationShelvingDescription");
        identified = checked(description, 0x18, "ShelvingDescription");
        located = inline_loot_position(remote, description, 0x20, p) ||
                  inline_loot_position(remote, description, 0x70, p);
        break;
    case LootSource::collection: {
        description = checked(loot_ref(remote, model, 0x10), 0x10, "LocationCollectionTableDescription");
        const Address reward = checked(model, 0x30, "CollectionTableRewardModel");
        identified = checked(loot_ref(remote, reward, 0x10), 0x18, "CargoDescription");
        located = identified && (inline_loot_position(remote, description, 0x20, p) ||
                                 inline_loot_position(remote, description, 0x3C, p));
        break;
    }
    }
    if (!located) return false;
    if (item.name.empty()) item.name = loot_id(remote, identified);
    if (item.type == LootKind::container && (item.name.empty() ||
        std::all_of(item.name.begin(), item.name.end(), [](unsigned char c) { return c >= '0' && c <= '9'; }))) {
        const auto container = loot_delegate_value(remote, catalog, loot_ref(remote, identified, 0x28),
                                                    "ContainerDescription");
        const auto label = loot_id(remote, container);
        if (!label.empty()) item.name = label;
    }
    if (item.type == LootKind::cargo && identified &&
        remote.require_class(identified, "CargoDescription", loot_stage)) {
        item.rarity = description_rarity(remote, identified, 0x20);
    }
    if (item_presentation::weapon_name(item.name) &&
        (item.type == LootKind::ground_item || item.type == LootKind::weapon)) {
        item.type = LootKind::weapon;
    }
    item.name_clean = item_presentation::clean_name(item.name, std::to_string(item.entity_id));
    attach_contents(remote, catalog, lookup, layout.source, description, item);
    return true;
}

void read_world_loot(const Remote& remote, Address battle_entities, WorldLootCache& cache,
                     Vec3 origin, MotionClock::time_point now, std::vector<LootItem>& output,
                     WmPortReader& reader, std::uint32_t pid, std::vector<LootNode>* nodes = nullptr) {
    using namespace std::chrono_literals;
    const Remote::Probe probe(remote);
    if (now >= cache.refresh_after) {
        const Address world = loot_ref(remote, battle_entities, 0x60);
        if (!world || !remote.require_class(world, "WorldEntitiesModel", "World loot owner class")) {
            cache = {};
            cache.refresh_after = now + 250ms;
            return;
        }
        if (cache.world != world) {
            cache = {};
            cache.world = world;
        }
        cache.refresh_after = now + 250ms;
    }
    if (!cache.world) return;
    prepare_catalog(remote, cache.catalog, reader, pid, now);

    for (std::size_t attempts = 0; attempts < loot_registries.size(); ++attempts) {
        const std::size_t i = cache.cursor++ % loot_registries.size();
        auto& collection = cache.collections[i];
        if (now < collection.refresh_after) continue;
        const auto& layout = loot_registries[i];
        collection.refresh_after = now + (i < 4 ? 250ms : 750ms);
        DictionarySnapshot dictionary;
        std::vector<LootItem> items;
        std::vector<LootNode> node_list;
        if (read_dictionary(remote, cache.world, layout.offset, dictionary, layout.field, &collection.dictionary)) {
            LootLookup lookup{remote, cache.world, {}};
            items.reserve(dictionary.live.size());
            for (const auto& entry : dictionary.live) {
                if (layout.source == LootSource::nodes) {
                    LootNode node;
                    if (sample_loot_node(remote, entry, node)) node_list.push_back(std::move(node));
                }
                LootItem item;
                if (sample_world_loot(remote, cache.catalog, lookup, layout, entry, item)) {
                    game_data::report_unmatched(item.name, game_data::lookup(item.name), "loot-reader");
                    if (item.contents) for (const auto& stack : *item.contents)
                        game_data::report_unmatched(stack.name, game_data::lookup(stack.name), "loot-contents-reader");
                    remote.detail("LOOT", "id=%ld name=%s raw=%s rarity=%d type=%d",
                        static_cast<long>(item.entity_id), item.name_clean.c_str(), item.name.c_str(),
                        item.rarity, static_cast<int>(item.type));
                    items.push_back(std::move(item));
                }
            }
            if (unchanged(remote, cache.world, layout.offset, dictionary.address, layout.field) &&
                recheck_dictionary(remote, dictionary, layout.field)) {
                remote.detail("LOOT", "%s live=%zu positioned=%zu", layout.field, dictionary.live.size(), items.size());
                if (layout.source == LootSource::nodes) {
                    log_loot_nodes(node_list, collection.nodes);
                    collection.nodes = std::move(node_list);
                }
                collection.dictionary = std::move(dictionary);
                collection.items = std::move(items);
            } else {
                collection = {};
                collection.refresh_after = now + 100ms;
            }
        } else {
            collection.dictionary = {};
            collection.items.clear();
            collection.nodes.clear();
        }
        break;
    }
    for (const auto& collection : cache.collections) {
        if (nodes) nodes->insert(nodes->end(), collection.nodes.begin(), collection.nodes.end());
        for (const auto& item : collection.items) {
            const float distance = std::hypot(item.position.x - origin.x, item.position.y - origin.y,
                                              item.position.z - origin.z);
            if (std::isfinite(distance)) {
                output.push_back(item);
                output.back().distance = distance;
            }
        }
    }
}


Address reference_dictionary_value(const Remote& remote, Address owner, Address key) {
    DictionarySnapshot dictionary;
    if (!owner || !key || !read_dictionary(remote, owner, 0x10, dictionary, "Weapon ammo slots") ||
        dictionary.header.count > 64) return 0;
    const auto& header = dictionary.header;
    if (header.count == 0) return 0;
    struct ReferenceEntry {
        std::uint32_t hash;
        std::int32_t next;
        Address key, value;
    };
    static_assert(sizeof(ReferenceEntry) == sizeof(Entry));
    std::vector<ReferenceEntry> entries(static_cast<std::size_t>(header.count));
    std::uint64_t bucket_count{};
    if (!remote.value(header.buckets, array_length_offset, bucket_count, "Weapon ammo slots") ||
        bucket_count > max_dictionary_capacity) return 0;
    std::vector<std::int32_t> buckets(static_cast<std::size_t>(bucket_count));
    if (!remote.bytes(header.entries, array_data_offset, entries.data(),
                      entries.size() * sizeof(ReferenceEntry), "Weapon ammo slots") ||
        !remote.bytes(header.buckets, array_data_offset, buckets.data(),
                      buckets.size() * sizeof(std::int32_t), "Weapon ammo slots")) return 0;
    std::vector<Entry> topology;
    topology.reserve(entries.size());
    for (const auto& entry : entries) topology.push_back({entry.hash, entry.next, 0, entry.value});
    std::vector<bool> legacy, modern;
    const auto size = static_cast<std::size_t>(header.count - header.free_count);
    const bool old = collect_live(buckets, topology, size, false, legacy);
    const bool current = collect_live(buckets, topology, size, true, modern);
    if ((!old && !current) || (old && current && legacy != modern)) return 0;
    if (!unchanged(remote, owner, 0x10, dictionary.address, "Weapon ammo slots") ||
        !recheck_dictionary(remote, dictionary, "Weapon ammo slots")) return 0;
    const auto& occupied = old ? legacy : modern;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (occupied[i] && entries[i].key == key) return entries[i].value;
    }
    return 0;
}

int read_weapon_magazine(const Remote& remote, Address character, Address expected_weapon,
                         Address definition) {
    Address private_model{}, magazine{}, weapon{}, selected{}, ammo{};
    if (!remote.ptr(character, 0x10, private_model, "Weapon private model", true) || !private_model ||
        !remote.pose_class(private_model, "BattleCharacterPrivateModel", "Weapon private model") ||
        !remote.pose_ptr(private_model, 0x198, magazine, "Weapon magazine", true) || !magazine ||
        !remote.pose_class(magazine, "WeaponMagazineModel", "Weapon magazine") ||
        !remote.pose_ptr(magazine, 0x10, weapon, "Magazine weapon") || weapon != expected_weapon ||
        !remote.pose_ptr(magazine, 0x20, selected, "Magazine selected slot") ||
        !remote.pose_ptr(magazine, 0x18, ammo, "Magazine ammo collection")) return -1;
    const auto selected_data = loot_ref(remote, selected, 0x10);
    const auto quick_slot = loot_ref(remote, selected_data, 0x18);
    if (!quick_slot || !remote.pose_class(quick_slot, "QuickSlotDescription", "Magazine slot")) return -1;
    const auto equipment_slot = loot_ref(remote, quick_slot, 0x28);
    const auto ammo_slot = loot_ref(remote, equipment_slot, 0x30);
    const auto model = reference_dictionary_value(remote, ammo, ammo_slot);
    if (!model || (!remote.pose_class(model, "AmmoModel", "Magazine amount") &&
                   !remote.pose_class(model, "BattleCharacterAmmoModel", "Magazine amount"))) return -1;
    const auto data = loot_ref(remote, model, 0x10);
    int count = -1;
    if (!data || !remote.value(data, 0x18, count, "Current magazine ammo") || count < 0 || count > 10000) return -1;
    Address check_slot{};
    if (!remote.value(selected_data, 0x18, check_slot, "Magazine slot recheck") || check_slot != quick_slot) return -1;
    // The WeaponMagazine getter uses the selected equipment slot, so verify it still holds this item.
    const auto equip_model = loot_ref(remote, expected_weapon, 0x10);
    const auto item_data = loot_ref(remote, equip_model, 0x10);
    return item_data && loot_ref(remote, item_data, 0x18) == definition ? count : -1;
}

void read_character_weapons(const Remote& remote, LootCatalog& catalog, Address local,
                            std::vector<Entity>& entities, bool report) {
    const Remote::Probe probe(remote);
    struct Sample { Address character{}, weapon{}, data{}, description{}; bool read{}; };
    std::vector<Sample> samples(entities.size());
    std::vector<WmPortReader::ReadRange> reads;
    std::vector<std::size_t> read_indices;
    reads.reserve(entities.size());
    read_indices.reserve(entities.size());
    for (std::size_t i = 0; i < entities.size(); ++i) {
        auto& entity = entities[i];
        entity.weapon_name.clear();
        entity.weapon_id.clear();
        entity.weapon_ammo = entity.weapon_reserve_ammo = entity.weapon_rarity = -1;
        entity.weapon_readable = entity.has_weapon = false;
        auto& sample = samples[i];
        const auto link = [&](Address at, Address offset, Address& result, const char* name) {
            return remote.pose_ptr(at, offset, result, name, true) && result;
        };
        Address data{}, common{};
        if (!link(entity.address, entity.address == local ? 0x20 : 0x28, sample.character, "Weapon ServerModel") ||
            !remote.pose_class(sample.character, "BattleCharacterModel", "Weapon character")) continue;
        if (link(sample.character, 0x40, data, "Character.Data") &&
            link(data, 0x20, common, "Character.CommonData") &&
            link(common, 0xB8, sample.data, "CommonData.ItemInHands") &&
            remote.pose_class(sample.data, "ReferenceData`1", "Current item data")) {
            (void)remote.pose_ptr(sample.character, 0x150, sample.weapon, "Character.Weapon", true);
        } else {
            Address item{};
            if (!link(sample.character, 0x150, sample.weapon, "Character.Weapon") ||
                !link(sample.weapon, 0x10, item, "Weapon._itemInHands") ||
                !remote.pose_class(item, "EquipItemModel", "Equipped item") ||
                !link(item, 0x10, sample.data, "Equipped item data")) continue;
        }
        reads.push_back({sample.data + 0x18, &sample.description, sizeof(Address)});
        read_indices.push_back(i);
    }
    remote.batch(reads);
    for (std::size_t i = 0; i < reads.size(); ++i) {
        samples[read_indices[i]].read = reads[i].success;
    }
    for (std::size_t i = 0; i < entities.size(); ++i) {
        auto& entity = entities[i];
        const auto& sample = samples[i];
        if (!sample.read) {
            if (report) remote.detail("WEAPON", "entity=0x%llX weapon=unavailable field=CommonData.ItemInHands offset=0xB8",
                                      static_cast<unsigned long long>(entity.address));
            continue;
        }
        if (sample.description == 0) {
            entity.weapon_readable = true;
        } else if (const auto* metadata = describe_inventory_item(remote, catalog, sample.description)) {
            entity.weapon_readable = true;
            entity.has_weapon = weapon_type(metadata->type) || item_presentation::weapon_name(metadata->id);
            if (entity.has_weapon) {
                entity.weapon_id = metadata->id;
                entity.weapon_name = item_presentation::clean_name(metadata->id);
                entity.weapon_rarity = metadata->rarity;
                entity.weapon_ammo = read_weapon_magazine(remote, sample.character, sample.weapon, sample.description);
                Address current{};
                if (!remote.value(sample.data, 0x18, current, "Held item recheck") || current != sample.description) {
                    entity.weapon_name.clear();
                    entity.weapon_id.clear();
                    entity.weapon_ammo = entity.weapon_rarity = -1;
                    entity.weapon_readable = entity.has_weapon = false;
                }
            }
        }
        if (report) {
            remote.detail("WEAPON", "entity=0x%llX weapon=%s raw=%s ammo=%d rarity=%d",
                static_cast<unsigned long long>(entity.address),
                !entity.weapon_readable ? "unavailable" : entity.has_weapon ? entity.weapon_name.c_str() : "Unarmed",
                entity.weapon_id.c_str(), entity.weapon_ammo, entity.weapon_rarity);
        }
    }
}

struct HeadMotion {
    Address view{};
    std::int32_t id{};
    Vec3 previous{};
    Vec3 baseline{};
    Vec3 velocity{};
    MotionClock::time_point observed_at{};
    MotionClock::time_point baseline_at{};
};

void update_head_motion(std::unordered_map<Address, HeadMotion>& history,
                        const std::vector<CachedEntity>& members, std::vector<Entity>& entities,
                        MotionClock::time_point now) {
    using namespace std::chrono_literals;
    for (std::size_t i = 0; i < entities.size(); ++i) {
        auto& entity = entities[i];
        if (!entity.valid || !finite(entity.head_position)) {
            continue;
        }
        const auto& member = members[i];
        const auto& head = entity.head_position;
        auto& last = history[entity.address];
        if (last.view != member.view || last.id != member.id ||
            last.observed_at == MotionClock::time_point{} || now - last.observed_at > 250ms) {
            last = {member.view, member.id, head, head, {}, now, now};
        }
        entity.prev_head_position = last.previous;
        const auto age = now - last.baseline_at;
        // Rendered poses can repeat between simulation ticks. Use a short baseline
        // to avoid dividing a full animation step by the reader's polling interval.
        if (age >= 40ms) {
            const float seconds = std::chrono::duration<float>(age).count();
            const Vec3 sample{(head.x - last.baseline.x) / seconds,
                              (head.y - last.baseline.y) / seconds,
                              (head.z - last.baseline.z) / seconds};
            last.velocity = std::hypot(sample.x, sample.y, sample.z) <= 90.0f ? sample : Vec3{};
            last.baseline = head;
            last.baseline_at = now;
        }
        entity.velocity = last.velocity;
        last.previous = head;
        last.observed_at = now;
    }
    std::erase_if(history, [now](const auto& entry) { return entry.second.observed_at != now; });
}

}  // namespace

struct GameEntities::Cache {
    std::uint32_t pid{};
    Structure structure;
    PoseCache pose;
    native_mesh::Cache meshes;
    WorldLootCache loot;
    std::unordered_map<Address, HeadMotion> motion;
    LocalPositionTrace local_trace;
    std::chrono::steady_clock::time_point refresh_after{};
    std::chrono::steady_clock::time_point probe_compact_after{};
    std::size_t compact_cursor{};
    bool dirty = true;
};

void GameEntities::swap_frame(GameEntities& other) noexcept {
    entities_.swap(other.entities_);
    vehicles_.swap(other.vehicles_);
    loot_items_.swap(other.loot_items_);
    loot_nodes_.swap(other.loot_nodes_);
    std::swap(local_player_address_, other.local_player_address_);
    std::swap(local_player_position_, other.local_player_position_);
    std::swap(camera_, other.camera_);
}

bool GameEntities::update(WmPortReader& reader, std::uint32_t pid) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (update_once(reader, pid)) {
            return true;
        }
        const DWORD error = GetLastError();
        if (cache_) {
            cache_->dirty = true;
        }
        if (error != ERROR_RETRY) {
            return false;
        }
    }
    return false;
}

bool GameEntities::update_once(WmPortReader& reader, std::uint32_t pid) {
    entities_.clear();
    vehicles_.clear();
    loot_items_.clear();
    loot_nodes_.clear();
    local_player_address_ = 0;
    local_player_position_.reset();
    camera_ = {};
    Remote remote(reader, pid, diagnostics_);
    if (pid == 0) {
        return remote.fail("PID", 0, ERROR_INVALID_PARAMETER);
    }
    try {
        if (!cache_ || cache_.use_count() != 1 || cache_->pid != pid) {
            cache_ = std::make_shared<Cache>();
            cache_->pid = pid;
        }
        auto& cache = *cache_;
        auto& structure = cache.structure;
        const auto now = std::chrono::steady_clock::now();
        if (!cache.dirty && now < cache.refresh_after) {
            const Remote::Probe probe(remote);
            remote.phase("Cached structure");
            cache.dirty = !check_structure(remote, structure, gom_rva_) ||
                          !check_views(remote, structure) || !check_vehicles(remote, structure);
        }
        if (cache.dirty || now >= cache.refresh_after) {
            Structure fresh;
            if (!refresh_structure(remote, reader, pid, unity_entry_, gom_rva_, structure, fresh)) {
                return false;
            }
            if (cache.dirty || fresh.battle_entities != structure.battle_entities ||
                fresh.player != structure.player || fresh.local_character != structure.local_character ||
                fresh.battle_updater != structure.battle_updater || fresh.updater != structure.updater ||
                !same_dictionary(fresh.models, structure.models) || !same_dictionary(fresh.views, structure.views)) {
                cache.pose = {};
                cache.meshes.bindings.clear();
                if (fresh.battle != structure.battle) {
                    cache.meshes.geometries.clear();
                    cache.motion.clear();
                }
                if (fresh.player != structure.player || fresh.local_character != structure.local_character) {
                    cache.local_trace = {};
                }
                if (fresh.battle_entities != structure.battle_entities) {
                    cache.loot = {};
                    cache.local_trace = {};
                }
                cache.compact_cursor = 0;
                cache.probe_compact_after = now + std::chrono::milliseconds(40);
            }
            structure = std::move(fresh);
            cache.refresh_after = now + std::chrono::seconds(1);
            cache.dirty = false;
        }

        cache.pose.start_frame();
        if (!check_pose_references(remote, cache.pose)) {
            cache.pose = {};
            cache.meshes.bindings.clear();
        }
        sample_cached_hierarchies(remote, cache.pose);
        remote.use_pose_cache(cache.pose);
        remote.phase("Camera and local position");
        LocalPositionSample local;
        Camera camera{};
        if (!sample_local_position(remote, structure, local) ||
            !read_camera(remote, structure.camera_address, camera)) {
            return false;
        }
        const Vec3 local_position = local.position;
        if (structure.battle_updater != 0) {
            const Remote::Probe probe(remote);
            read_camera_lens(remote, structure.battle_updater, camera);
        }

        remote.phase("Entity positions");
        static thread_local std::chrono::steady_clock::time_point next_skeleton_log{};
        const bool report_skeletons = diagnostics_ && now >= next_skeleton_log;
        if (report_skeletons) {
            next_skeleton_log = now + std::chrono::seconds(1);
        }
        std::vector<Entity> next;
        next.swap(entities_);
        next.resize(structure.entities.size());
        if (!structure.entities.empty() && now >= cache.probe_compact_after) {
            cache.compact_cursor %= structure.entities.size();
            structure.entities[cache.compact_cursor++].probe_compact = true;
            cache.probe_compact_after = now + std::chrono::milliseconds(40);
        }
        std::size_t valid_heads{};
        bool read_meshes{};
        if (mesh_overlay_) {
            const Remote::Probe probe(remote);
            read_meshes = native_mesh::profile(remote, cache.meshes, reader, pid);
        }
        for (std::size_t i = 0; i < structure.entities.size(); ++i) {
            auto& item = structure.entities[i];
            sample_entity(remote, item, structure.player, local_position, camera,
                          report_skeletons, next[i], valid_heads);
            if (read_meshes && item.view && next[i].valid && item.address != structure.player) {
                const Remote::Probe probe(remote);
                native_mesh::sample(remote, cache.meshes, cache.pose, item.view, now, next[i]);
            }
        }

        remote.phase("Character roles");
        read_roles(remote, cache.pose, structure.player, next);
        remote.phase("Character weapons");
        read_character_weapons(remote, cache.loot.catalog, structure.player, next, report_skeletons);
        for (const auto& entity : next) if (entity.has_weapon || !entity.weapon_id.empty())
            game_data::report_unmatched(entity.weapon_id, game_data::lookup(entity.weapon_id), "weapon-reader");

        remote.phase("Snapshot recheck");
        if (!check_structure(remote, structure, gom_rva_) || !recheck_local_position(remote, structure, local)) {
            return false;
        }
        cache.dirty = cache.pose.stale;
        {
            const Remote::Probe probe(remote);
            if (check_vehicles(remote, structure)) {
                vehicles_ = structure.vehicle_list;
            } else {
                cache.dirty = true;
            }
            if (!check_views(remote, structure)) {
                cache.dirty = true;
                for (auto& entity : next) {
                    entity.head_position = {};
                    entity.valid = false;
                    entity.bones = {};
                    entity.bone_parents = {};
                    entity.skeleton_valid = false;
                    entity.limb_joints = {};
                    entity.measured_limb_joints.reset();
                    entity.limbs_valid = false;
                    entity.mesh = {};
                }
                valid_heads = 0;
            }
        }
        update_head_motion(cache.motion, structure.entities, next, MotionClock::now());
        remote.phase("World loot");
        read_world_loot(remote, structure.battle_entities, cache.loot, local_position, now, loot_items_,
                        reader, pid, &loot_nodes_);
        if (!recheck_local_position(remote, structure, local)) return false;
        trace_local_position(cache.local_trace, local, now, diagnostics_);
        entities_ = std::move(next);
        local_player_address_ = structure.player;
        local_player_position_ = local_position;
        camera_ = camera;
        remote.complete(entities_.size(), vehicles_.size(), valid_heads);
        return true;
    } catch (const std::bad_alloc&) {
        return remote.fail("Snapshot allocation", 0, ERROR_OUTOFMEMORY);
    }
}

}  // namespace game_entities

