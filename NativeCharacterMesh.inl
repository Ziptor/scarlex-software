// Included by game_entities.cpp after the Unity Transform reader.
namespace native_mesh {

constexpr Address object_map_rva = 0x2230758;
constexpr Address mesh_vtable_rva = 0x1EF8308;
constexpr Address skinned_vtable_rva = 0x1DB9DF0; // Component vtable
constexpr Address skinned_renderer_vtable_rva = 0x1DB9D28;
constexpr Address filter_vtable_rva = 0x1DB9208;
constexpr Address transform_vtable_rva = 0x1DCCA70;
constexpr std::uint32_t max_vertices = 40000;
constexpr std::uint32_t max_indices = 240000;
constexpr std::size_t max_bones = 256;

struct ObjectMap {
    Address slots;
    std::uint32_t mask;
    std::uint32_t size;
};
struct ObjectSlot {
    std::uint32_t hash;
    std::uint32_t padding0;
    std::uint32_t id;
    std::uint32_t padding1;
    Address object;
};
static_assert(sizeof(ObjectSlot) == 24);

struct NativeVector {
    Address items;
    Address allocator;
    std::uint64_t size;
};
struct Channel {
    std::uint8_t stream, offset, format, dimension;
};
struct Stream {
    std::uint32_t mask, offset, stride;
};
struct Submesh {
    char draw_bounds[0x20];
    std::uint32_t first_byte, count, topology, base_vertex, first_vertex, vertex_count;
};
static_assert(sizeof(Submesh) == 0x38);

struct Binding {
    std::vector<TransformAccess> bones;
    std::shared_ptr<const MeshGeometry> geometry;
    std::chrono::steady_clock::time_point check_after{};
};

struct Cache {
    struct Asset {
        std::uint32_t id;
        std::shared_ptr<const MeshGeometry> geometry;
    };
    Address unity{};
    bool checked_build{};
    std::unordered_map<Address, Binding> bindings;
    std::unordered_map<Address, Asset> geometries;
    std::chrono::steady_clock::time_point next_discovery{};
};

bool profile(const Remote& remote, Cache& cache, WmPortReader& reader, std::uint32_t pid) {
    if (!cache.checked_build) {
        cache.checked_build = true;
        const auto unity = reader.module_base(pid, "UnityPlayer.dll");
        if (!unity) {
            return false;
        }
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS64 nt{};
        // File layout used to resolve the native getters with Unity's matching PDB.
        if (remote.value(*unity, 0, dos, "Mesh: UnityPlayer DOS") && dos.e_magic == IMAGE_DOS_SIGNATURE &&
            dos.e_lfanew > 0 && dos.e_lfanew < 0x1000 &&
            remote.value(*unity, static_cast<Address>(dos.e_lfanew), nt, "Mesh: UnityPlayer PE") &&
            nt.Signature == IMAGE_NT_SIGNATURE && nt.FileHeader.TimeDateStamp == 0x6A28669F &&
            nt.OptionalHeader.SizeOfImage == 0x2426000) {
            cache.unity = *unity;
        }
    }
    return cache.unity != 0;
}

bool lookup_objects(const Remote& remote, Address unity, std::span<const std::uint32_t> ids,
                    std::vector<Address>& objects) {
    Address map{};
    ObjectMap table{}, recheck{};
    if (!remote.ptr(unity, object_map_rva, map, "Unity Object registry") ||
        !remote.value(map, 0, table, "Unity Object registry header") || !remote.pointer(table.slots) ||
        (table.mask & 7u) != 0 || table.mask < 8 || table.mask > 0x1FFFFFFu ||
        !std::has_single_bit(table.mask / 8 + 1)) {
        return false;
    }
    struct Probe {
        std::uint32_t id, hash, slot, step;
        ObjectSlot entry;
        std::size_t index;
    };
    std::vector<Probe> pending;
    objects.assign(ids.size(), 0);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto hash = ids[i] * 0x9E3779B1u;
        if (ids[i]) {
            pending.push_back({ids[i], hash & ~3u, hash & table.mask, 8, {}, i});
        }
    }
    std::vector<WmPortReader::ReadRange> ranges;
    for (unsigned pass = 0; !pending.empty() && pass < 96; ++pass) {
        ranges.clear();
        for (auto& probe : pending) {
            ranges.push_back({table.slots + 3ull * probe.slot, &probe.entry, sizeof(probe.entry)});
        }
        remote.batch(ranges);
        for (std::size_t i = 0; i < pending.size(); ++i) {
            auto& probe = pending[i];
            if (ranges[i].success && probe.entry.hash == probe.hash && probe.entry.id == probe.id) {
                objects[probe.index] = probe.entry.object;
                probe.step = 0;
            } else if (!ranges[i].success || probe.entry.hash == 0xFFFFFFFFu) {
                probe.step = 0;
            } else {
                probe.slot = (probe.slot + probe.step) & table.mask;
                probe.step += 8;
            }
        }
        std::erase_if(pending, [](const auto& p) { return p.step == 0; });
    }
    return remote.value(map, 0, recheck, "Unity Object registry recheck") &&
           table.slots == recheck.slots && table.mask == recheck.mask;
}

float half_to_float(std::uint16_t bits) {
    const auto exponent = (bits >> 10) & 31;
    const auto fraction = bits & 1023;
    if (exponent == 31) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    const float value = exponent == 0 ? std::ldexp(static_cast<float>(fraction), -24)
                                     : std::ldexp(static_cast<float>(1024 + fraction), exponent - 25);
    return bits & 0x8000 ? -value : value;
}

template<class T>
T element(const std::byte* bytes) {
    T value{};
    std::memcpy(&value, bytes, sizeof(T));
    return value;
}

struct VertexChannel {
    Channel descriptor{};
    Stream stream{};
    std::vector<std::byte> values;

    bool read(const Remote& remote, Address shared, Address base, std::uint32_t vertex_count,
              unsigned index, std::uint8_t width) {
        if (!remote.value(shared, 8 + index * 4, descriptor, "Mesh vertex channel") ||
            descriptor.stream >= 4 || dimension() == 0 || dimension() > 4 ||
            !remote.value(shared, 0x40 + descriptor.stream * 12, stream, "Mesh stream") ||
            stream.stride > 512 || stream.offset > 32 * 1024 * 1024 ||
            static_cast<unsigned>(descriptor.offset) + width * dimension() > stream.stride) {
            return false;
        }
        values.resize(static_cast<std::size_t>(vertex_count) * stream.stride);
        return remote.bytes(base, stream.offset, values.data(), values.size(), "Mesh CPU vertex stream");
    }
    unsigned dimension() const { return descriptor.dimension & 15; }
    const std::byte* at(std::size_t vertex) const {
        return values.data() + vertex * stream.stride + descriptor.offset;
    }
};

unsigned format_width(std::uint8_t format) {
    constexpr unsigned widths[] = {4, 2, 1, 1, 2, 2, 1, 1, 2, 2, 4, 4};
    return format < std::size(widths) ? widths[format] : 0;
}

float real_component(const std::byte* ptr, unsigned format) {
    switch (format) {
    case 0: return element<float>(ptr);
    case 1: return half_to_float(element<std::uint16_t>(ptr));
    case 2: return element<std::uint8_t>(ptr) / 255.0f;
    case 4: return element<std::uint16_t>(ptr) / 65535.0f;
    default: return std::numeric_limits<float>::quiet_NaN();
    }
}

std::uint32_t bone_component(const std::byte* ptr, unsigned format) {
    switch (format) {
    case 6: return element<std::uint8_t>(ptr);
    case 8: return element<std::uint16_t>(ptr);
    case 10: case 11: return element<std::uint32_t>(ptr);
    default: return UINT32_MAX;
    }
}

bool read_skin(const Remote& remote, Address shared, Address vertex_data, MeshGeometry& mesh) {
    Address bind_matrices{};
    std::uint32_t bind_count{};
    if (!remote.ptr(shared, 0xF0, bind_matrices, "Mesh bind poses") ||
        !remote.value(shared, 0x100, bind_count, "Mesh bind pose count") || bind_count == 0 || bind_count > max_bones) {
        return false;
    }
    std::vector<std::array<float, 16>> bind(bind_count);
    if (!remote.bytes(bind_matrices, 0, bind.data(), bind.size() * sizeof(bind[0]), "Mesh bind poses data")) {
        return false;
    }
    for (const auto& m : bind) {
        MeshAffine affine;
        if (!std::all_of(m.begin(), m.end(), [](float v) { return std::isfinite(v); }) ||
            std::abs(m[3]) > 1e-3f || std::abs(m[7]) > 1e-3f || std::abs(m[11]) > 1e-3f ||
            std::abs(m[15] - 1.0f) > 1e-3f) {
            return false;
        }
        for (std::size_t row = 0; row < 3; ++row) {
            for (std::size_t col = 0; col < 4; ++col) {
                affine.rows[row][col] = m[col * 4 + row];
            }
        }
        mesh.inverse_bind.push_back(affine);
    }
    const auto count = static_cast<std::uint32_t>(mesh.positions.size());
    NativeVector packed{};
    if (!remote.value(shared, 0x170, packed, "Mesh packed skin weights")) {
        return false;
    }
    mesh.weight_offsets.reserve(count + 1);
    if (packed.size != 0) {
        if (!remote.pointer(packed.items) || packed.size <= count || packed.size > count * 17ull + 1) {
            return false;
        }
        std::vector<std::uint32_t> words(static_cast<std::size_t>(packed.size));
        if (!remote.bytes(packed.items, 0, words.data(), words.size() * sizeof(words[0]), "Mesh weights data") ||
            words[0] != count + 1 || words[count] != words.size()) {
            return false;
        }
        for (std::size_t i = 0; i < count; ++i) {
            if (words[i] >= words[i + 1] || words[i + 1] > words.size() || words[i + 1] - words[i] > 16) {
                return false;
            }
            mesh.weight_offsets.push_back(static_cast<std::uint32_t>(mesh.weights.size()));
            for (std::size_t j = words[i]; j < words[i + 1]; ++j) {
                const auto bone = words[j] & 0xFFFFu;
                if (bone >= bind_count) {
                    return false;
                }
                mesh.weights.push_back({static_cast<float>(words[j] >> 16) / 65535.0f, bone});
            }
        }
    } else {
        Channel weights{}, indices{};
        VertexChannel w, b;
        if (!remote.value(shared, 8 + 12 * 4, weights, "Mesh BlendWeight") ||
            !remote.value(shared, 8 + 13 * 4, indices, "Mesh BlendIndices")) {
            return false;
        }
        const auto ww = format_width(weights.format), bw = format_width(indices.format);
        if (!bw || !b.read(remote, shared, vertex_data, count, 13, static_cast<std::uint8_t>(bw)) ||
            (b.dimension() != 1 &&
             (!ww || !w.read(remote, shared, vertex_data, count, 12, static_cast<std::uint8_t>(ww)) ||
              w.dimension() != b.dimension()))) {
            return false;
        }
        for (std::size_t i = 0; i < count; ++i) {
            mesh.weight_offsets.push_back(static_cast<std::uint32_t>(mesh.weights.size()));
            for (unsigned k = 0; k < b.dimension(); ++k) {
                const float weight = b.dimension() == 1 ? 1.0f : real_component(w.at(i) + k * ww, weights.format);
                const auto bone = bone_component(b.at(i) + k * bw, indices.format);
                if (!std::isfinite(weight) || weight < 0 || (weight > 0 && bone >= bind_count)) {
                    return false;
                }
                if (weight > 0) {
                    mesh.weights.push_back({weight, bone});
                }
            }
        }
    }
    mesh.weight_offsets.push_back(static_cast<std::uint32_t>(mesh.weights.size()));
    return true;
}

std::shared_ptr<const MeshGeometry> read_geometry(const Remote& remote, Address mesh, bool skinned) {
    Address shared{}, vertices{}, index_data{};
    std::uint32_t count{}, index_format{};
    std::uint64_t index_bytes{};
    NativeVector submeshes{};
    if (!remote.ptr(mesh, 0x28, shared, "Mesh shared data") ||
        !remote.value(shared, 0x78, count, "Mesh vertex count") || count < 3 || count > max_vertices ||
        !remote.ptr(shared, 0xA0, vertices, "Mesh vertex data") ||
        !remote.ptr(shared, 0xA8, index_data, "Mesh index data") ||
        !remote.value(shared, 0xB8, index_bytes, "Mesh index bytes") ||
        !remote.value(shared, 0xC8, index_format, "Mesh index format") || index_format > 1 ||
        !remote.value(shared, 0xD0, submeshes, "Mesh submeshes") || !remote.pointer(submeshes.items) ||
        submeshes.size == 0 || submeshes.size > 64) {
        return {};
    }
    auto out = std::make_shared<MeshGeometry>();
    Channel position{};
    VertexChannel positions;
    if (!remote.value(shared, 8, position, "Mesh position channel") || position.format > 1 ||
        !positions.read(remote, shared, vertices, count, 0, static_cast<std::uint8_t>(format_width(position.format))) ||
        positions.dimension() != 3) {
        return {};
    }
    out->positions.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        std::array<float, 3> p{};
        for (std::size_t j = 0; j < p.size(); ++j) {
            p[j] = real_component(positions.at(i) + j * format_width(position.format), position.format);
        }
        if (!std::all_of(p.begin(), p.end(), [](float x) { return std::isfinite(x); })) {
            return {};
        }
        out->positions.push_back(p);
    }
    std::vector<Submesh> parts(static_cast<std::size_t>(submeshes.size));
    if (!remote.bytes(submeshes.items, 0, parts.data(), parts.size() * sizeof(parts[0]), "Mesh submesh data")) {
        return {};
    }
    const std::uint32_t stride = index_format == 1 ? 4u : 2u;
    std::vector<std::byte> packed;
    for (const auto& part : parts) {
        if (part.topology != 0 || part.count == 0) { // Unity MeshTopology.Triangles
            continue;
        }
        const auto bytes = static_cast<std::uint64_t>(part.count) * stride;
        if (part.count % 3 || part.count > max_indices - out->indices.size() ||
            part.first_byte > index_bytes || bytes > index_bytes - part.first_byte) {
            return {};
        }
        packed.resize(static_cast<std::size_t>(bytes));
        if (!remote.bytes(index_data, part.first_byte, packed.data(), packed.size(), "Mesh indices")) {
            return {};
        }
        for (std::size_t i = 0; i < part.count; ++i) {
            const auto* data = packed.data() + i * stride;
            const std::uint32_t index = stride == 2 ? element<std::uint16_t>(data) : element<std::uint32_t>(data);
            if (part.base_vertex >= count || index >= count - part.base_vertex) {
                return {};
            }
            out->indices.push_back(index + part.base_vertex);
        }
    }
    if (out->indices.empty() || (skinned && !read_skin(remote, shared, vertices, *out)) ||
        !unchanged(remote, mesh, 0x28, shared, "Mesh shared data recheck")) {
        return {};
    }
    return out;
}

bool native_transforms(const Remote& remote, PoseCache& pose, Address unity, std::span<const Address> objects,
                       std::vector<TransformAccess>& palette) {
    struct TransformHeader {
        Address vtable;
        std::byte prefix[0x20];
        TransformAccess access;
    };
    static_assert(offsetof(TransformHeader, access) == 0x28);
    std::vector<TransformHeader> headers(objects.size());
    std::vector<WmPortReader::ReadRange> requests;
    requests.reserve(objects.size());
    for (std::size_t i = 0; i < objects.size(); ++i) {
        if (!remote.pointer(objects[i])) {
            return false;
        }
        requests.push_back({objects[i], &headers[i], sizeof(headers[i])});
    }
    remote.batch(requests);
    std::vector<Address> reverse(objects.size());
    for (std::size_t i = 0; i < objects.size(); ++i) {
        const auto& [vtable, prefix, access] = headers[i];
        if (!requests[i].success || vtable != unity + transform_vtable_rva || !remote.pointer(access.hierarchy)) {
            return false;
        }
        const auto& sample = sample_hierarchy(remote, pose, access.hierarchy);
        if (!sample.valid || sample.locals.empty() || access.index < 0 || access.index >= sample.header.count) {
            return false;
        }
        const Address slot = sample.header.arrays.transforms + static_cast<Address>(access.index) * sizeof(Address);
        requests[i] = {slot, &reverse[i], sizeof(Address)};
    }
    remote.batch(requests);
    palette.reserve(objects.size());
    for (std::size_t i = 0; i < objects.size(); ++i) {
        if (!requests[i].success || reverse[i] != objects[i]) {
            return false;
        }
        remote.remember_pose_reference(requests[i].address, objects[i]);
        palette.push_back(headers[i].access);
    }
    return true;
}

bool renderer_for_lod(const Remote& remote, Address view, Address& renderer, Address& game_object) {
    Address customization{}, lods{};
    const auto read_view = [&] {
        if (remote.pose_ptr(view, 0xB8, customization, "Character.CustomizationView", true) && customization) {
            return true;
        }
        Address battle{};
        return remote.pose_ptr(view, 0xC0, battle, "BattleCustomizationView", true) && battle &&
               remote.pose_class(battle, "BattleCharacterCustomizationView", "Battle customization class") &&
               remote.pose_ptr(battle, 0x20, customization, "BattleCustomizationView._view", true) && customization;
    };
    if (read_view() && remote.pose_class(customization, "CharacterCustomizationView", "Customization class") &&
        remote.pose_ptr(customization, 0x20, lods, "CharacterCustomizationView._lods", true) && lods) {
        std::uint64_t count{};
        if (remote.value(lods, 0x18, count, "Character mesh LOD count") && count <= 16 && count > 0) {
            std::array<Address, 16> levels{};
            if (remote.bytes(lods, 0x20, levels.data(), count * sizeof(Address), "Character LOD list")) {
                // Prefer coarser body assets to keep CPU skinning bounded.
                for (std::size_t i = static_cast<std::size_t>(count); i-- > 0;) {
                    Address managed{};
                    if (remote.pointer(levels[i]) && remote.pose_class(levels[i], "LodCharacterView", "LOD class") &&
                        remote.pose_ptr(levels[i], 0x38, managed, "LodCharacterView.Body", true) && managed &&
                        remote.pose_class(managed, "SkinnedMeshRenderer", "Body renderer class") &&
                        remote.ptr(managed, 0x10, renderer, "SkinnedMeshRenderer.m_CachedPtr")) {
                        (void)remote.ptr(renderer, 0x20, game_object, "Body renderer GameObject", true);
                        return true;
                    }
                }
            }
        }
    }
    Address native{};
    return remote.ptr(view, 0x10, native, "Character view native") &&
           remote.ptr(native, 0x20, game_object, "Character view GameObject", true);
}

bool find_filter(const Remote& remote, Address unity, Address owner, Address& filter, Address& transform) {
    Address components{};
    std::uint32_t count{};
    if (!owner || !remote.ptr(owner, 0x20, components, "MeshFilter components") ||
        !remote.value(owner, 0x30, count, "MeshFilter component count") || count == 0 || count > 64) {
        return false;
    }
    std::array<std::array<Address, 2>, 64> list{};
    if (!remote.bytes(components, 0, list.data(), count * sizeof(list[0]), "Static mesh components")) {
        return false;
    }
    transform = list[0][1];
    for (std::size_t i = 1; i < count; ++i) {
        Address vtable{};
        if (remote.pointer(list[i][1]) && remote.ptr(list[i][1], 0, vtable, "MeshFilter vtable") &&
            vtable == unity + filter_vtable_rva) {
            filter = list[i][1];
            return true;
        }
    }
    return false;
}

bool bind(const Remote& remote, Cache& cache, PoseCache& pose, Address renderer, bool skinned, Address rigid_transform,
          Binding& binding) {
    Address vtable{};
    std::uint32_t mesh_id{};
    const Address id_offset = skinned ? 0x1E0 : 0x28;
    if (!remote.ptr(renderer, 0, vtable, "Mesh renderer vtable") ||
        (skinned ? (vtable != cache.unity + skinned_vtable_rva && vtable != cache.unity + skinned_renderer_vtable_rva)
                 : vtable != cache.unity + filter_vtable_rva) ||
        !remote.value(renderer, id_offset, mesh_id, "Renderer shared mesh ID") || !mesh_id) {
        return false;
    }
    std::vector<std::uint32_t> ids{mesh_id};
    NativeVector bones{};
    if (skinned) {
        if (!remote.value(renderer, 0x1F8, bones, "Renderer bone vector") ||
            bones.size == 0 || bones.size > max_bones || !remote.pointer(bones.items)) {
            return false;
        }
        ids.resize(static_cast<std::size_t>(bones.size) + 1);
        if (!remote.bytes(bones.items, 0, ids.data() + 1, (ids.size() - 1) * sizeof(ids[0]), "Renderer bone IDs")) {
            return false;
        }
    }
    std::vector<Address> objects;
    if (!lookup_objects(remote, cache.unity, ids, objects) ||
        !remote.pointer(objects[0]) ||
        !remote.ptr(objects[0], 0, vtable, "Unity Mesh vtable") || vtable != cache.unity + mesh_vtable_rva) {
        return false;
    }
    // The same asset may be shared by most players. Cache only validated successful geometry.
    const auto key = objects[0] | (skinned ? Address{1} : Address{0});
    auto found = cache.geometries.find(key);
    if (found == cache.geometries.end() || found->second.id != mesh_id) {
        const auto geometry = read_geometry(remote, objects[0], skinned);
        if (!geometry) {
            return false;
        }
        if (cache.geometries.size() >= 32) {
            cache.geometries.clear();
        }
        found = cache.geometries.insert_or_assign(key, Cache::Asset{mesh_id, geometry}).first;
    }
    const auto& geometry = found->second.geometry;
    if (skinned && geometry->inverse_bind.size() != bones.size) {
        return false;
    }
    std::vector<TransformAccess> palette;
    const auto bone_objects = skinned ? std::span<const Address>(objects).subspan(1)
                                     : std::span<const Address>(&rigid_transform, 1);
    if (!native_transforms(remote, pose, cache.unity, bone_objects, palette)) {
        return false;
    }
    binding.bones = std::move(palette);
    binding.geometry = geometry;
    return true;
}

bool matrix(const Remote& remote, PoseCache& pose, const TransformAccess& access, MeshAffine& out) {
    auto& sample = sample_hierarchy(remote, pose, access.hierarchy);
    if (!sample.valid || sample.locals.empty()) {
        return false;
    }
    out.rows = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
    std::int32_t index = access.index;
    for (unsigned depth = 0; index != -1; ++depth) {
        if (index < 0 || index >= sample.header.count || depth == 256) {
            return false;
        }
        const auto slot = static_cast<std::size_t>(index);
        const auto& local = sample.locals[slot];
        auto rotation = local.rotation;
        if (!normalize_rotation(rotation) || !finite(local.scale) || !finite(local.translation)) {
            return false;
        }
        const auto x = rotate(rotation, {local.scale.x, 0, 0});
        const auto y = rotate(rotation, {0, local.scale.y, 0});
        const auto z = rotate(rotation, {0, 0, local.scale.z});
        const auto t = local.translation;
        MeshAffine parent{{{{x.x, y.x, z.x, t.x}, {x.y, y.y, z.y, t.y}, {x.z, y.z, z.z, t.z}}}};
        out = multiply(parent, out);
        index = sample.parents[slot];
    }
    return true;
}

void sample(const Remote& remote, Cache& cache, PoseCache& pose, Address view,
            std::chrono::steady_clock::time_point now, Entity& entity) {
    auto& binding = cache.bindings[view];
    if ((!binding.geometry || now >= binding.check_after) && now >= cache.next_discovery &&
        now >= binding.check_after) {
        // Spread first discovery across updates. Existing characters keep their sampled pose.
        cache.next_discovery = now + std::chrono::milliseconds(10);
        Address renderer{}, owner{}, transform{}, filter{};
        Binding fresh;
        if (renderer_for_lod(remote, view, renderer, owner)) {
            if (!renderer || !bind(remote, cache, pose, renderer, true, 0, fresh)) {
                if (find_filter(remote, cache.unity, owner, filter, transform)) {
                    bind(remote, cache, pose, filter, false, transform, fresh);
                }
            }
        }
        fresh.check_after = now + (fresh.geometry ? std::chrono::seconds(3) : std::chrono::seconds(1));
        binding = std::move(fresh);
        remote.detail("MESH", "entity=0x%llX vertices=%zu bones=%zu",
                      static_cast<unsigned long long>(entity.address),
                      binding.geometry ? binding.geometry->positions.size() : 0, binding.bones.size());
    }
    if (binding.geometry) {
        entity.mesh.bone_to_world.resize(binding.bones.size());
        for (std::size_t i = 0; i < binding.bones.size(); ++i) {
            if (!matrix(remote, pose, binding.bones[i], entity.mesh.bone_to_world[i])) {
                binding.geometry.reset();
                entity.mesh.bone_to_world.clear();
                return;
            }
        }
        entity.mesh.geometry = binding.geometry;
    }
}

}  // namespace native_mesh
