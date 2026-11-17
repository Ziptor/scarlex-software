# SignatureScanner: finding a GOM candidate

`SignatureScanner.h` / `SignatureScanner.cpp` perform **read-only** scans through
`WmPortReader`. No device access or driver loading is added. They operate on a
mapped PE32+ x64 image, not on a DLL file's raw offsets.

## What the addresses mean

- `scan_string("UnityEntry")` returns the absolute VA of matching ASCII text.
- `scan_pointer(string_va)` returns VAs of aligned 8-byte fields containing that
  VA. A reference is **not** automatically a GameObject or a GOM field.
- A native GameObject's name normally lives in the heap, not inside
  `UnityPlayer.dll`. DLL text can be unrelated metadata. Searching the DLL does
  not search the whole process or heap; an empty string result is normal.
- If a field is independently confirmed as `GameObject::name` at +0x50, its
  owner is `field_va - 0x50`. This subtraction is valid only after confirming the
  native structure and this build's offset. It does **not** yield the GOM RVA.
- A backward reference search is a graph search: a name reference identifies a
  candidate GameObject, then a list node references that GameObject, then a
  manager references list endpoints, then a module global references the manager.
  Interior nodes usually have no direct reference from a manager, so list links
  also need following. There is no fixed `string_rva +/- constant` formula.
- `GomCandidate::slot` is the module-global VA. The desired offset is
  **`slot - module_base`**, already returned as `GomCandidate::rva`.

**Correction for the confirmed game dump:** `UnityEntry` is the name of the
managed component's IL2CPP class, not the name of its native GameObject.
GOM validation now visits every reachable node and each component until the
exact class `UnityEntry` is found. GameObject names never gate acceptance.
`GomCandidate::entry_component` is the managed component address; its +0x28
field leads to `Game`. `Game +0xD0` leads to `GameModel`; +0xD8 is no longer
probed because it contains `PaymentsProcessor` in this build.

## App::init integration

Add `#include "SignatureScanner.h"` to `src/app.cpp`. After creating `reader_`
and resolving `target_pid_`, replace the current GameEntities construction with
the following diagnostic initialization block:

```cpp
const auto unity = reader_->module_base(target_pid_, "UnityPlayer.dll");
if (!unity) return false;

SignatureScanner scanner(*reader_, target_pid_, *unity);
if (!scanner.initialize()) return false;

SignatureScanner::GomOptions options;
options.region = SignatureScanner::Region::Data;
auto result = scanner.find_gom_candidates(options);

// .data can be renamed or split: scan other writable, non-executable sections.
if ((result.error == 0 || result.error == ERROR_NOT_FOUND) &&
    result.candidates.empty() && !result.limited) {
    options.region = SignatureScanner::Region::WritableData;
    result = scanner.find_gom_candidates(options);
}
if (result.error != 0 || result.limited || result.unreadable_bytes != 0) {
    std::cerr << "Incomplete GOM scan; inspect scanner diagnostics.\n";
    return false; // Do not mistake an incomplete scan for a unique match.
}

std::optional<std::uint64_t> confirmed_rva;
for (const auto& candidate : result.candidates) {
    // Runtime constructor uses candidate.rva for both root read and recheck.
    game_entities::GameEntities probe(candidate.rva);
    if (!probe.update(*reader_, target_pid_)) continue;

    if (confirmed_rva) {
        std::cerr << "Multiple working GOM slots: inspect aliases manually.\n";
        return false;
    }
    confirmed_rva = candidate.rva;
}
if (!confirmed_rva) {
    std::cerr << "GOM not confirmed. Load a scene containing UnityEntry and a battle.\n";
    return false;
}
entities_ = std::make_unique<game_entities::GameEntities>(*confirmed_rva);
std::printf("[App] Confirmed UnityPlayer.dll GOM RVA=0x%llX\n",
            static_cast<unsigned long long>(*confirmed_rva));
```

Also include `<cstdio>`, `<cstdint>`, and `<optional>` if the surrounding file
does not already include them. This is an explicit diagnostic block: it is **not
automatically installed** in App::init, and the scanner is never run per frame.

`GameEntities` now accepts a GOM RVA in its constructor; its default retains the
old RVA for existing callers. Just adding the scanner files without changing
initialization will therefore still use the old offset.

Full `update()` confirmation requires the current game scene to have a local
player and camera. A structurally valid GOM can exist before those are ready.
For a production startup flow, retain all structural candidates and repeat only
their validation after the scene loads, rather than rescanning .data per frame.
An initialization failure is not proof that every candidate RVA is wrong.

The existing CMakeLists.txt uses a source glob. Re-run CMake configure so the new
root `SignatureScanner.cpp` is added, then build as usual.

## String and reference diagnostics

```cpp
for (const auto text : {"UnityEntry", "[Entry]"}) {
    const auto strings = scanner.scan_string(text);
    for (const auto address : strings.hits) {
        std::printf("text=%s va=0x%llX rva=0x%llX\n", text,
                    static_cast<unsigned long long>(address),
                    static_cast<unsigned long long>(address - scanner.module_base()));
        const auto refs = scanner.scan_pointer(address);
        for (const auto field : refs.hits) {
            std::printf("  reference field va=0x%llX rva=0x%llX\n",
                        static_cast<unsigned long long>(field),
                        static_cast<unsigned long long>(field - scanner.module_base()));
        }
    }
}
```

## AOB and RIP-relative resolution

Obtain a signature from the matching Unity build's disassembly. Verify the
function and instruction that load/store the actual GOM global. Do not reuse a
signature from another Unity version without checking the surrounding code.

```cpp
// pass verified_pattern from your build's disassembly; no GOM signature is
// hardcoded here because the current UnityPlayer.dll was not supplied.
const auto code = scanner.scan_pattern(verified_pattern,
                                      SignatureScanner::Region::Executable);
if (code.error == 0 && !code.truncated && code.unreadable_bytes == 0 &&
    code.hits.size() == 1) {
    // Only if the matched instruction is verified as a 7-byte
    // mov rcx, [rip+disp32] (48 8B 0D ...) or corresponding store/lea:
    const auto slot = scanner.resolve_rip_relative(code.hits[0], 3, 7);
    if (slot && *slot >= scanner.module_base() &&
        *slot - scanner.module_base() <= scanner.image_size() - sizeof(std::uint64_t)) {
        const std::uint64_t candidate_rva = *slot - scanner.module_base();
        game_entities::GameEntities probe(candidate_rva);
        // Confirm the complete chain before accepting/caching candidate_rva.
        const bool confirmed = probe.update(*reader_, target_pid_);
        (void)confirmed;
    }
}
```

The broad byte sequence `48 8B 0D ?? ?? ?? ??` is merely a common RIP-relative
instruction form, **not a GOM signature**. Many unrelated globals use it. If
the instruction begins partway into the match, add that known byte offset to
`code.hits[0]`. `resolve_rip_relative` performs signed displacement arithmetic;
it is not a disassembler and does not establish the meaning of the target.

For an instruction beginning at `I` with length `L`:

```text
global_slot_va = I + L + sign_extend(disp32)
gom_rva       = global_slot_va - base(UnityPlayer.dll)
gom_pointer   = read<uint64_t>(global_slot_va)
```

## Component-class discovery and .text RIP-load discovery

The target is strictly the managed component class `UnityEntry`, case-sensitive.
`UnityEntry_1`, `unityentry`, and GameObject names such as `[Entry]` or `Game`
are not accepted. Both structural searches enumerate all reachable GOM nodes
and all component slots, stopping at the first exact match or explicit budget.

```cpp
SignatureScanner::GomOptions options;
options.dump_name_limit = 20;
options.log_component_classes = true;
const auto data = scanner.find_gom_candidates(options);
```

With zero matches, the scanner prints `NAME DUMP` followed by the first 20
distinct readable GameObject addresses seen in plausible native lists. Names
may repeat if different objects have the same name. These are **diagnostic
samples, not confirmed GameObjects from the real GOM**: coincidental pointer
graphs can still supply readable strings. These names are optional context,
not match criteria. `readable_classes`, `class_failures`, and `component_slots`
are the important counters for the corrected search. Samples
are also returned as `GomResult::sampled_names`. Collection does not stop
traversal after 20 objects, so a matching component later in a list can still win.

Each checked managed candidate logs GameObject address, component index,
reference source, component address, klass address, actual class name, and
expected name. Native array entries have a vtable at +0, not an IL2CPP class:
they are unwrapped through the existing `native +0x18` tagged indirect reference
or `native +0x28` direct managed reference. Direct managed array-slot references
are also supported. Only the managed object's +0 is treated as `Il2CppClass*`.
The confirmed native component array uses 0x10-byte slots with its reference
at slot +8. Component logging defaults to every candidate; set
`log_component_classes=false` after diagnosis to avoid expensive console output.
Budgets are 256 components per object and 1,000,000 examined slots per scan;
`limited=true` must not be interpreted as a complete search.

The legacy `entry_names` option remains source-compatible but is ignored:
it cannot broaden the strict class match. Do not set it in new code.

For the requested code search:

```cpp
// Raw byte candidates: exactly executable .text, target slot fully in .data.
const auto loads = scanner.scan_rip_loads();
for (const auto& load : loads.loads) {
    // load.instruction: code VA; load.slot: global VA; load.rva: global RVA.
    // Check load.value_readable before using load.value.
}

// Same .text search followed by native GOM validation for unique global slots.
// Multiple instructions referencing one slot are deduplicated for GOM probing.
const auto code_candidates = scanner.find_gom_candidates_from_text(options);
```

`scan_rip_loads()` recognizes byte forms `48 8B 05 disp32` (rax) and
`48 8B 0D disp32` (rcx). It resolves the signed displacement directly from
the fully read seven-byte match, including matches crossing block boundaries.
It does **not** decode surrounding instructions: matches inside instruction
operands or embedded data remain possible. Inspect disassembly and validate the
native graph before accepting any slot as GOM.

`RipLoadResult::loads` retains duplicate code references and references to null,
unaligned, or unreadable slots for diagnosis. Each slot must occupy eight bytes
fully inside the target section; the structural follow-up rejects invalid manager
pointers. RIP scan `unreadable_bytes` covers `.text` only; use
`slot_read_failures` to see failures reading referenced `.data` fields. `limited`
and `error` must also be checked before making completeness/uniqueness claims.

Default limits: 8192 accepted code references, first 20 printed references.
Configure with `scan_rip_loads(Region::Data, max_loads, log_limit)` or
`find_gom_candidates_from_text(options, max_loads)`. Set `options.region` to
`WritableData` only if target globals may reside in `.bss` or other writable
non-executable sections. The source remains strictly `.text`.

The .text path narrows the globals to those used by the two opcode forms; it
does not magically repair incorrect GOM/node/component offsets. For **explicitly
verified** alternative native layouts, both GOM searches accept:

```cpp
options.list_first_offsets = {0x18, 0x28, 0x08}; // Each last field at first -8.
options.game_object_name_offset = 0x50; // Optional dump only, NOT acceptance.
```

Do not blindly add arbitrary offsets and treat readable names as proof. Native
node `next +8` and `GameObject +0x10` still match the current parser. Both
`GameEntities` and the scanner now search component class `UnityEntry`, never
GameObject names. Inspect returned `entry_component`, first offset, and addresses
before synchronizing the parser to a verified alternative native layout.

## Bounds, limitations, and tests

- Scan results include unreadable coverage and hit-limit status. Missing pages
  are skipped, not filled with zeros; partial buffers are discarded.
- GOM probing has explicit candidate, manager, total-node, and per-list budgets.
  `limited=true` means absence/uniqueness cannot be established. Inspect the log
  before raising limits: kernel reads for native-list traversal can be slow.
- Native GOM lists, node fields, GameObject names, and component layouts are
  still version-specific. A new global RVA cannot repair changed native layouts.
- Scans are not atomic. Do not claim uniqueness from an empty or changing scene.
- Cache an RVA only with the corresponding module/build identity. ASLR changes
  the base VA, not an RVA within the same image. TimeDateStamp and SizeOfImage
  are useful diagnostics but are not a cryptographically unique build identity.

Standalone tests use synthetic memory and require no process/device/driver.
From an x64 Native Tools Command Prompt, with the output directory present:

```bat
cl /nologo /std:c++20 /EHsc /W4 /WX tests\signature_scanner_tests.cpp /Fo"%TEMP%\opencode\signature_scanner_tests.obj" /Fe"%TEMP%\opencode\signature_scanner_tests.exe"
"%TEMP%\opencode\signature_scanner_tests.exe"
```

Do not also link SignatureScanner.cpp or wmport_reader.cpp into this test: the
test includes the scanner and supplies the reader implementation. Passing
synthetic tests does not establish the current game's GOM RVA.
