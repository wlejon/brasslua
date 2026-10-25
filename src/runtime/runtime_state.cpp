#include "runtime/runtime_internal.h"

#include <brass/gc/fiber.hpp>
#include <brass/gc/heap.hpp>
#include <brass/gc/object.hpp>
#include <brass/gc/tracer.hpp>

#include <csetjmp>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <unordered_map>
#include <vector>

namespace brasslua {

namespace {

uint32_t fnv1aHash(std::string_view sv) noexcept {
    uint32_t h = 2166136261u;
    for (unsigned char c : sv) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

struct BrassLuaLayouts {
    brass::gc::LayoutId leaf = brass::gc::kLeafLayout;
    brass::gc::LayoutId words = brass::gc::kLeafLayout;
    brass::gc::LayoutId table = brass::gc::kLeafLayout;
    brass::gc::LayoutId closure = brass::gc::kLeafLayout;
    brass::gc::LayoutId coroutine = brass::gc::kLeafLayout;
    brass::gc::LayoutId userdata = brass::gc::kLeafLayout;
};

bool isCollectableWeakObject(uint64_t bits) noexcept {
    Value v(bits);
    return v.isTable() || v.isFunction() || v.isCoroutine() || v.isFullUserdata();
}

void traceLuaTable(uintptr_t payload, size_t /*payload_bytes*/, brass::gc::Tracer& t) {
    auto* tbl = reinterpret_cast<LuaTable*>(payload);
    if (tbl->metatable) {
        t.visit_address(reinterpret_cast<uintptr_t*>(&tbl->metatable));
    }
    const bool weak_k = (tbl->flags & kMetaWeakKeys) != 0;
    const bool weak_v = (tbl->flags & kMetaWeakValues) != 0;

    for (size_t i = 0; i < LuaTable::kInlineSlots; ++i) {
        if (weak_v && isCollectableWeakObject(tbl->slots[i])) {
            t.visit_weak(&tbl->slots[i], kNilBits);
        } else {
            t.visit(&tbl->slots[i]);
        }
    }
    t.visit(&tbl->cached_index_mm);

    if (tbl->array_data) {
        if (tbl->array_data != tbl->inline_array) {
            t.visit_address(reinterpret_cast<uintptr_t*>(&tbl->array_data));
        }
        for (uint32_t i = 0; i < tbl->array_cap; ++i) {
            if (weak_v && isCollectableWeakObject(tbl->array_data[i])) {
                t.visit_weak(&tbl->array_data[i], kNilBits);
            } else {
                t.visit(&tbl->array_data[i]);
            }
        }
    }
    if (tbl->hash_data) {
        t.visit_address(reinterpret_cast<uintptr_t*>(&tbl->hash_data));
        for (uint32_t i = 0; i < tbl->hash_cap; ++i) {
            LuaHashEntry& entry = tbl->hash_data[i];
            if (entry.key == kNilBits || entry.key == kTombstoneBits) continue;
            if (entry.val == kNilBits) {
                entry.key = kTombstoneBits;
                continue;
            }
            const bool k_weak = weak_k && isCollectableWeakObject(entry.key);
            const bool v_weak = weak_v && isCollectableWeakObject(entry.val);
            if (k_weak && v_weak) {
                t.visit_weak(&entry.key, kTombstoneBits);
                t.visit_weak(&entry.val, kNilBits);
            } else if (k_weak) {
                t.visit_ephemeron(&entry.key, &entry.val, kTombstoneBits, kNilBits);
            } else if (v_weak) {
                t.visit(&entry.key);
                t.visit_weak(&entry.val, kNilBits);
            } else {
                t.visit(&entry.key);
                t.visit(&entry.val);
            }
        }
    }
}

void traceLuaClosure(uintptr_t payload, size_t /*payload_bytes*/, brass::gc::Tracer& t) {
    auto* cl = reinterpret_cast<LuaClosure*>(payload);
    // `upvalues` points at the closure's own trailing array (allocClosure),
    // not at an object of its own, so only the cells it holds are visited.
    if (!cl->upvalues) return;
    for (uint32_t i = 0; i < cl->upvalue_count; ++i) {
        if (cl->upvalues[i]) {
            t.visit_address(reinterpret_cast<uintptr_t*>(&cl->upvalues[i]));
            t.visit(&cl->upvalues[i]->value);
        }
    }
}

void traceLuaCoroutine(uintptr_t payload, size_t /*payload_bytes*/, brass::gc::Tracer& t) {
    auto* co = reinterpret_cast<LuaCoroutine*>(payload);
    if (co->metatable) {
        t.visit_address(reinterpret_cast<uintptr_t*>(&co->metatable));
    }
    if (co->entry_closure) {
        t.visit_address(reinterpret_cast<uintptr_t*>(&co->entry_closure));
    }
    if (co->caller) {
        t.visit_address(reinterpret_cast<uintptr_t*>(&co->caller));
    }
    if (co->ctx_state && co->trace_ctx) {
        co->trace_ctx(co->ctx_state, &t);
    }
}

void traceLuaUserdata(uintptr_t payload, size_t /*payload_bytes*/, brass::gc::Tracer& t) {
    auto* ud = reinterpret_cast<LuaUserdata*>(payload);
    if (ud->metatable) {
        t.visit_address(reinterpret_cast<uintptr_t*>(&ud->metatable));
    }
    t.visit(&ud->user_value);
}

const BrassLuaLayouts& getLayouts() {
    static const BrassLuaLayouts layouts = [] {
        BrassLuaLayouts l;
        {
            brass::gc::LayoutDescriptor d;
            d.kind = brass::gc::LayoutKind::Leaf;
            d.type_tag = 0xB51A;
            d.name = "brasslua.leaf";
            l.leaf = brass::gc::register_layout(d);
        }
        {
            brass::gc::LayoutDescriptor d;
            d.kind = brass::gc::LayoutKind::Words;
            d.type_tag = 0xB51A;
            d.name = "brasslua.words";
            l.words = brass::gc::register_layout(d);
        }
        {
            brass::gc::LayoutDescriptor d;
            d.kind = brass::gc::LayoutKind::Custom;
            d.trace = &traceLuaTable;
            d.type_tag = 0xB51A;
            d.name = "brasslua.table";
            l.table = brass::gc::register_layout(d);
        }
        {
            brass::gc::LayoutDescriptor d;
            d.kind = brass::gc::LayoutKind::Custom;
            d.trace = &traceLuaClosure;
            d.type_tag = 0xB51A;
            d.name = "brasslua.closure";
            l.closure = brass::gc::register_layout(d);
        }
        {
            brass::gc::LayoutDescriptor d;
            d.kind = brass::gc::LayoutKind::Custom;
            d.trace = &traceLuaCoroutine;
            d.type_tag = 0xB51A;
            d.name = "brasslua.coroutine";
            l.coroutine = brass::gc::register_layout(d);
        }
        {
            brass::gc::LayoutDescriptor d;
            d.kind = brass::gc::LayoutKind::Custom;
            d.trace = &traceLuaUserdata;
            d.type_tag = 0xB51A;
            d.name = "brasslua.userdata";
            l.userdata = brass::gc::register_layout(d);
        }
        return l;
    }();
    return layouts;
}

brass::gc::HeapConfig makeBrassLuaHeapConfig() {
    brass::gc::HeapConfig cfg;
    // Matches bronze::Heap reference tags so both runtimes can share one brass::gc::Heap seamlessly!
    cfg.reference_tags = {
        static_cast<uint16_t>(Tag::Object),
        static_cast<uint16_t>(Tag::String),
        static_cast<uint16_t>(Tag::Function),
        static_cast<uint16_t>(Tag::BoxedInt64),
    };
    cfg.read_environment = true;
    cfg.walk_stack_on_host_collection = false;
    return cfg;
}

} // namespace

struct TransparentStringHash {
    using is_transparent = void;
    size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
};

struct RuntimeState::Impl {
    std::unique_ptr<brass::gc::Heap> owned_heap;
    brass::gc::Heap* active_heap = nullptr;
    brass::gc::Heap::RootSourceId root_source_id = 0;
    uintptr_t stack_top_anchor = 0;

    std::unordered_map<std::string, uint32_t, TransparentStringHash, std::equal_to<>> str_to_id;
    std::vector<LuaString*> id_to_lstr;

    uint32_t slot_to_intern[LuaTable::kInlineSlots] = {0};
    int32_t next_slot = 0;

    std::vector<Value> globals_by_id;
    LuaTable* env_table = nullptr;
    LuaTable* string_metatable = nullptr;
    LuaCoroutine* running_coroutine = nullptr;
    bool in_explicit_gc = false;

    void traceRoots(brass::gc::Tracer& t, RuntimeState& rt) {
        if (env_table) {
            t.visit_address(reinterpret_cast<uintptr_t*>(&env_table));
        }
        if (string_metatable) {
            t.visit_address(reinterpret_cast<uintptr_t*>(&string_metatable));
        }
        if (running_coroutine) {
            t.visit_address(reinterpret_cast<uintptr_t*>(&running_coroutine));
        }
        for (LuaString*& s : id_to_lstr) {
            if (s) {
                t.visit_address(reinterpret_cast<uintptr_t*>(&s));
            }
        }
        for (Value& v : globals_by_id) {
            t.visit(reinterpret_cast<uint64_t*>(&v));
        }
        if (rt.multiret_mode_ == 1) {
            t.visit(&rt.single_ret_);
        } else if (rt.multiret_mode_ == 2) {
            for (uint64_t& mv : rt.multiret_) {
                t.visit(&mv);
            }
        }

        // Conservative scan of active C/JIT stack frames so in-flight pinned
        // brasslua temporaries on the CPU stack or registers are kept alive across
        // any GC cycle without touching movable young objects on a shared heap.
        // Every stack of the thread is scanned: the running one from here,
        // and each coroutine's (a brass fiber) or the main stack while
        // switched out, from where it switched away. The main stack's base is
        // the anchor taken when the runtime was made.
        std::jmp_buf reg_buf;
        setjmp(reg_buf);
        volatile uintptr_t frame_var = 0;
        const uintptr_t sp = reinterpret_cast<uintptr_t>(&frame_var);
        struct ScanCtx {
            Impl* self;
            brass::gc::Tracer* tracer;
        } scan{this, &t};
        brass::gc::Fiber::for_each_stack_span(sp, [](void* p, const brass::gc::Fiber::StackSpan& span) {
            auto* c = static_cast<ScanCtx*>(p);
            const uintptr_t hi = span.high != 0 ? span.high : c->self->stack_top_anchor;
            if (hi > span.low && (hi - span.low) < (size_t{16} << 20)) {
                c->self->scanWords(*c->tracer, span.low, hi);
            }
            if (span.regs != nullptr) {
                c->self->scanWords(*c->tracer, reinterpret_cast<uintptr_t>(span.regs),
                                   reinterpret_cast<uintptr_t>(span.regs_end));
            }
        }, &scan);
    }

    // Visits every word of [lo, hi) that may be a reference to an old object
    // (young objects move, so a word that cannot be updated never names one).
    void scanWords(brass::gc::Tracer& t, uintptr_t lo, uintptr_t hi) {
        const uint64_t raw_tag = active_heap->raw_address_tag();
        for (uintptr_t p = (lo + 7) & ~uintptr_t{7}; p + sizeof(uint64_t) <= hi; p += sizeof(uint64_t)) {
            uint64_t word = *reinterpret_cast<const uint64_t*>(p);
            if (active_heap->is_reference_tag(word)) {
                uintptr_t addr = static_cast<uintptr_t>(word & brass::gc::kAddressMask);
                if (!active_heap->is_young(addr)) {
                    t.visit_conservative(&word);
                }
            } else {
                uintptr_t addr = static_cast<uintptr_t>(word);
                if (active_heap->contains(addr) && !active_heap->is_young(addr)) {
                    uintptr_t obj = active_heap->find_object(addr);
                    if (obj != 0) {
                        uint8_t kind = reinterpret_cast<const uint8_t*>(obj)[-1];
                        if (in_explicit_gc && kind == kHostKindTable) {
                            continue;
                        }
                        uint64_t tagged = (static_cast<uint64_t>(obj) & brass::gc::kAddressMask) | raw_tag;
                        t.visit(&tagged);
                    }
                }
            }
        }
    }

    void bindHeap(brass::gc::Heap* h, RuntimeState& rt) {
        if (active_heap && root_source_id != 0) {
            active_heap->remove_root_source(root_source_id);
            root_source_id = 0;
        }
        active_heap = h;
        if (active_heap) {
            brass::gc::Heap::set_current(active_heap);
            root_source_id = active_heap->add_root_source(
                [this, &rt](brass::gc::Tracer& t) { traceRoots(t, rt); });
        }
    }

    ~Impl() {
        if (active_heap && root_source_id != 0) {
            active_heap->remove_root_source(root_source_id);
            root_source_id = 0;
        }
    }
};

namespace {
RuntimeState* g_active_runtime_state = nullptr;
} // namespace

RuntimeState& RuntimeState::current() {
    if (g_active_runtime_state) {
        return *g_active_runtime_state;
    }
    static RuntimeState instance;
    return instance;
}

RuntimeState::RuntimeState() : impl_(new Impl()) {
    g_active_runtime_state = this;
    volatile uintptr_t anchor = 0;
    impl_->stack_top_anchor = reinterpret_cast<uintptr_t>(&anchor);
    reset();
}

RuntimeState::~RuntimeState() {
    if (g_active_runtime_state == this) {
        g_active_runtime_state = nullptr;
    }
    delete impl_;
}

brass::gc::Heap& RuntimeState::heap() noexcept {
    return *impl_->active_heap;
}

void RuntimeState::attachExternalHeap(brass::gc::Heap* external_heap) {
    if (external_heap) {
        impl_->bindHeap(external_heap, *this);
        impl_->owned_heap.reset();
    } else {
        impl_->owned_heap = std::make_unique<brass::gc::Heap>(makeBrassLuaHeapConfig());
        impl_->bindHeap(impl_->owned_heap.get(), *this);
    }
    reset();
}

#if defined(_MSC_VER)
#define BRASSLUA_NOINLINE __declspec(noinline)
#else
#define BRASSLUA_NOINLINE __attribute__((noinline))
#endif

static BRASSLUA_NOINLINE void scrubStackForGc() noexcept {
    volatile uint64_t scrub[128];
    for (size_t i = 0; i < 128; ++i) {
        scrub[i] = 0;
    }
}

void RuntimeState::collectGarbage() {
    if (impl_->active_heap && !impl_->active_heap->in_collection()) {
        scrubStackForGc();
        impl_->in_explicit_gc = true;
        impl_->active_heap->collect(brass::gc::CollectionKind::Full);
        impl_->in_explicit_gc = false;
    }
}

void RuntimeState::reset() {
    volatile uintptr_t anchor = 0;
    if (reinterpret_cast<uintptr_t>(&anchor) > impl_->stack_top_anchor) {
        impl_->stack_top_anchor = reinterpret_cast<uintptr_t>(&anchor);
    }

    if (!impl_->active_heap || impl_->owned_heap != nullptr) {
        if (impl_->active_heap && impl_->root_source_id != 0) {
            impl_->active_heap->remove_root_source(impl_->root_source_id);
            impl_->root_source_id = 0;
        }
        impl_->owned_heap = std::make_unique<brass::gc::Heap>(makeBrassLuaHeapConfig());
        impl_->bindHeap(impl_->owned_heap.get(), *this);
    }

    impl_->str_to_id.clear();
    impl_->id_to_lstr.clear();
    for (size_t i = 0; i < LuaTable::kInlineSlots; ++i) {
        impl_->slot_to_intern[i] = 0;
    }
    impl_->next_slot = 0;
    impl_->globals_by_id.clear();
    impl_->env_table = nullptr;
    impl_->string_metatable = nullptr;
    impl_->running_coroutine = nullptr;
    multiret_.clear();
    single_ret_ = kNilBits;
    multiret_mode_ = 0;

    // Reserve intern_id 0 for the empty string ""
    internString("");

    // Pre-seed deterministic inline record field slots (0..15)
    static constexpr const char* kCommonFields[] = {
        "x", "y", "z", "vx", "vy", "vz", "mass", "next", "prev", "left", "right", "val", "key"
    };
    for (const char* f : kCommonFields) {
        uint32_t id = internString(f);
        getOrAssignFieldSlot(id);
    }

    impl_->env_table = allocTable(0, 64);
    registerStdlib();
}

void RuntimeState::registerStdlib() {
    registerCoreStdlib(*this);
    registerMathAndBitStdlib(*this);
    registerStringAndTableStdlib(*this);
}

uint32_t RuntimeState::internString(std::string_view str) {
    auto it = impl_->str_to_id.find(str);
    if (it != impl_->str_to_id.end()) {
        return it->second;
    }
    uint32_t id = static_cast<uint32_t>(impl_->id_to_lstr.size());
    size_t alloc_bytes = sizeof(LuaString) + str.size() + 1;
    uintptr_t mem = impl_->active_heap->allocate(
        alloc_bytes, getLayouts().leaf, brass::gc::kAllocPinned, kHostKindString);
    auto* lstr = reinterpret_cast<LuaString*>(mem);
    lstr->hash = fnv1aHash(str);
    lstr->length = static_cast<uint32_t>(str.size());
    lstr->intern_id = id;
    // Never assign inline slots 0..15 to metamethod names ("__*"), so any mutation to a
    // metamethod goes through hash_data and invalidates cached_mt_flags.
    lstr->slot_id = (str.size() >= 2 && str[0] == '_' && str[1] == '_') ? -2 : -1;
    if (!str.empty()) {
        std::memcpy(lstr->data, str.data(), str.size());
    }
    lstr->data[str.size()] = '\0';

    impl_->id_to_lstr.push_back(lstr);
    impl_->str_to_id.emplace(std::string(str), id);
    if (impl_->globals_by_id.size() <= id) {
        impl_->globals_by_id.resize(id + 1, Value::nil());
    }
    return id;
}

std::string_view RuntimeState::getStringById(uint32_t id) const {
    if (id >= impl_->id_to_lstr.size()) return {};
    const LuaString* s = impl_->id_to_lstr[id];
    return std::string_view(s->data, s->length);
}

LuaString* RuntimeState::getLuaStringById(uint32_t id) {
    if (id >= impl_->id_to_lstr.size()) return nullptr;
    return impl_->id_to_lstr[id];
}

uint64_t RuntimeState::makeStringValue(std::string_view str) {
    uint32_t id = internString(str);
    return Value::fromString(impl_->id_to_lstr[id]).rawBits();
}

int32_t RuntimeState::getOrAssignFieldSlot(uint32_t intern_id) {
    if (intern_id >= impl_->id_to_lstr.size()) return -1;
    LuaString* s = impl_->id_to_lstr[intern_id];
    if (s->slot_id >= 0) return s->slot_id;
    if (s->slot_id == -2) return -1;
    if (s->length >= 2 && s->data[0] == '_' && s->data[1] == '_') {
        s->slot_id = -2;
        return -1;
    }
    if (impl_->next_slot < static_cast<int32_t>(LuaTable::kInlineSlots)) {
        int32_t slot = impl_->next_slot++;
        s->slot_id = slot;
        impl_->slot_to_intern[slot] = intern_id;
        return slot;
    }
    s->slot_id = -2;
    return -1;
}

int32_t RuntimeState::findFieldSlot(uint32_t intern_id) const {
    if (intern_id >= impl_->id_to_lstr.size()) return -1;
    int32_t slot = impl_->id_to_lstr[intern_id]->slot_id;
    return slot >= 0 ? slot : -1;
}

LuaTable* RuntimeState::envTable() const noexcept {
    return impl_->env_table;
}

void RuntimeState::setGlobal(std::string_view name, Value val) {
    uint32_t id = internString(name);
    setGlobalById(id, val);
}

void RuntimeState::setGlobalById(uint32_t intern_id, Value val) {
    if (intern_id >= impl_->globals_by_id.size()) {
        impl_->globals_by_id.resize(intern_id + 1, Value::nil());
    }
    impl_->globals_by_id[intern_id] = val;
    if (impl_->env_table) {
        int32_t slot = findFieldSlot(intern_id);
        brasslua_rt_table_set_field(
            Value::fromTable(impl_->env_table).rawBits(),
            intern_id,
            slot >= 0 ? static_cast<uint32_t>(slot) : 0xFFFFFFFFu,
            val.rawBits());
    }
}

Value RuntimeState::getGlobal(std::string_view name) {
    uint32_t id = internString(name);
    return getGlobalById(id);
}

Value RuntimeState::getGlobalById(uint32_t intern_id) {
    if (intern_id < impl_->globals_by_id.size()) {
        Value v = impl_->globals_by_id[intern_id];
        if (!v.isNil()) {
            return v;
        }
    }
    if (impl_->env_table) {
        uint64_t tv = brasslua_rt_rawget(
            Value::fromTable(impl_->env_table).rawBits(),
            Value::fromString(getLuaStringById(intern_id)).rawBits());
        if (!Value(tv).isNil()) {
            return Value(tv);
        }
    }
    if (host_bridge_.getGlobal) {
        std::string_view name = getStringById(intern_id);
        uint64_t hv = host_bridge_.getGlobal(name);
        if (!Value(hv).isNil()) {
            return Value(hv);
        }
    }
    return Value::nil();
}

void RuntimeState::ensureTableArrayCapacity(LuaTable* tbl, uint32_t min_cap) {
    if (min_cap <= tbl->array_cap) return;
    uint32_t new_cap = tbl->array_cap < 16 ? 16 : tbl->array_cap;
    while (new_cap < min_cap) {
        new_cap *= 2;
    }
    uintptr_t mem = impl_->active_heap->allocate(
        static_cast<size_t>(new_cap) * sizeof(uint64_t),
        getLayouts().leaf,
        brass::gc::kAllocPinned,
        kHostKindBuffer);
    auto* new_arr = reinterpret_cast<uint64_t*>(mem);
    for (uint32_t i = 0; i < tbl->array_cap; ++i) {
        new_arr[i] = tbl->array_data[i];
    }
    for (uint32_t i = tbl->array_cap; i < new_cap; ++i) {
        new_arr[i] = kNilBits;
    }
    tbl->array_data = new_arr;
    tbl->array_cap = new_cap;
}

void RuntimeState::ensureTableHashCapacity(LuaTable* tbl, uint32_t min_entries) {
    uint32_t required = min_entries < 4 ? 8 : min_entries * 2;
    if (required <= tbl->hash_cap) return;
    uint32_t new_cap = tbl->hash_cap == 0 ? 8 : tbl->hash_cap;
    while (new_cap < required) {
        new_cap *= 2;
    }
    uintptr_t mem = impl_->active_heap->allocate(
        static_cast<size_t>(new_cap) * sizeof(LuaHashEntry),
        getLayouts().leaf,
        brass::gc::kAllocPinned,
        kHostKindBuffer);
    auto* new_hash = reinterpret_cast<LuaHashEntry*>(mem);
    for (uint32_t i = 0; i < new_cap; ++i) {
        new_hash[i].key = kNilBits;
        new_hash[i].val = kNilBits;
    }
    uint32_t mask = new_cap - 1;
    uint32_t new_len = 0;
    if (tbl->hash_data) {
        for (uint32_t i = 0; i < tbl->hash_cap; ++i) {
            const LuaHashEntry& old_e = tbl->hash_data[i];
            if (old_e.key != kNilBits && old_e.key != kTombstoneBits && old_e.val != kNilBits) {
                uint32_t h = hashLuaKey(old_e.key) & mask;
                while (new_hash[h].key != kNilBits) {
                    h = (h + 1) & mask;
                }
                new_hash[h] = old_e;
                ++new_len;
            }
        }
    }
    tbl->hash_data = new_hash;
    tbl->hash_cap = new_cap;
    tbl->hash_len = new_len;
}

LuaTable* RuntimeState::allocTable(uint32_t array_hint, uint32_t hash_hint) {
    uintptr_t mem = impl_->active_heap->allocate(
        sizeof(LuaTable), getLayouts().table, brass::gc::kAllocPinned, kHostKindTable);
    auto* tbl = new (reinterpret_cast<void*>(mem)) LuaTable();
    tbl->initInlineSlots();
    tbl->array_data = tbl->inline_array;
    tbl->array_cap = LuaTable::kInlineArrayCap;
    if (array_hint > LuaTable::kInlineArrayCap) {
        ensureTableArrayCapacity(tbl, array_hint);
    }
    if (hash_hint > 0) {
        ensureTableHashCapacity(tbl, hash_hint);
    }
    return tbl;
}

LuaClosure* RuntimeState::allocClosure(LuaNativeFn fn, void* unboxed_fn, uint32_t upval_count, uint32_t arity,
                                       bool init_upvalues) {
    size_t total_bytes = sizeof(LuaClosure) + static_cast<size_t>(upval_count) * sizeof(LuaUpvalue*);
    uintptr_t mem = impl_->active_heap->allocate(
        total_bytes, getLayouts().closure, brass::gc::kAllocPinned, kHostKindClosure);
    auto* cl = new (reinterpret_cast<void*>(mem)) LuaClosure();
    cl->fn = fn;
    cl->unboxed_fn = unboxed_fn;
    cl->upvalue_count = upval_count;
    cl->arity = arity;
    if (upval_count > 0) {
        cl->upvalues = reinterpret_cast<LuaUpvalue**>(mem + sizeof(LuaClosure));
        for (uint32_t i = 0; i < upval_count; ++i) {
            cl->upvalues[i] = init_upvalues ? allocUpvalue(kNilBits) : nullptr;
        }
    }
    return cl;
}

LuaUpvalue* RuntimeState::allocUpvalue(uint64_t initial_val) {
    uintptr_t mem = impl_->active_heap->allocate(
        sizeof(LuaUpvalue), getLayouts().words, brass::gc::kAllocPinned, kHostKindUpvalue);
    auto* uv = new (reinterpret_cast<void*>(mem)) LuaUpvalue();
    uv->value = initial_val;
    return uv;
}

LuaBoxedInt64* RuntimeState::allocBoxedInt64(int64_t val) {
    uintptr_t mem = impl_->active_heap->allocate(
        sizeof(LuaBoxedInt64), getLayouts().leaf, brass::gc::kAllocPinned, kHostKindBoxedInt64);
    auto* box = new (reinterpret_cast<void*>(mem)) LuaBoxedInt64();
    box->value = val;
    return box;
}

LuaCoroutine* RuntimeState::allocCoroutine(LuaClosure* entry) {
    uintptr_t mem = impl_->active_heap->allocate(
        sizeof(LuaCoroutine), getLayouts().coroutine, brass::gc::kAllocPinned, kHostKindCoroutine);
    auto* co = new (reinterpret_cast<void*>(mem)) LuaCoroutine();
    co->entry_closure = entry;
    co->status = CoroutineStatus::Suspended;
    return co;
}

LuaUserdata* RuntimeState::allocUserdata(size_t size, uint32_t type_tag) {
    size_t total_bytes = sizeof(LuaUserdata) + size;
    uintptr_t mem = impl_->active_heap->allocate(
        total_bytes, getLayouts().userdata, brass::gc::kAllocPinned, kHostKindUserdata);
    auto* ud = new (reinterpret_cast<void*>(mem)) LuaUserdata();
    ud->data = size > 0 ? reinterpret_cast<void*>(mem + sizeof(LuaUserdata)) : nullptr;
    ud->size = static_cast<uint32_t>(size);
    ud->type_tag = type_tag;
    impl_->active_heap->add_finalizer(
        mem,
        [](void* ctx) {
            auto* u = static_cast<LuaUserdata*>(ctx);
            if (u && u->finalizer) {
                auto fn = u->finalizer;
                u->finalizer = nullptr;
                fn(u->data, u->type_tag);
            }
        },
        ud);
    return ud;
}

void RuntimeState::setMultiRet(const uint64_t* vals, size_t count) {
    if (count == 1 && vals) {
        single_ret_ = vals[0];
        multiret_mode_ = 1;
        return;
    }
    if (vals && count > 0) {
        multiret_.assign(vals, vals + count);
    } else {
        multiret_.clear();
    }
    multiret_mode_ = 2;
}

const std::vector<uint64_t>& RuntimeState::multiRetVec() {
    if (multiret_mode_ == 1) {
        multiret_.assign(&single_ret_, &single_ret_ + 1);
    } else if (multiret_mode_ == 0) {
        multiret_.clear();
    }
    return multiret_;
}

void RuntimeState::writeOutput(std::string_view text) {
    if (output_cb_) {
        output_cb_(text);
    } else {
        std::cout.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
}

void RuntimeState::setScriptArgs(const std::string& script_path, const std::vector<std::string>& args) {
    LuaTable* arg_tbl = allocTable(static_cast<uint32_t>(args.size() + 2), 4);
    uint64_t tbl_bits = Value::fromTable(arg_tbl).rawBits();
    brasslua_rt_table_set_int(tbl_bits, 0, makeStringValue(script_path));
    for (size_t i = 0; i < args.size(); ++i) {
        brasslua_rt_table_set_int(tbl_bits, static_cast<int64_t>(i + 1), makeStringValue(args[i]));
    }
    setGlobal("arg", Value(tbl_bits));
}

// Internal accessors
bool isClosurePtr(RuntimeState& rt, const void* ptr) noexcept {
    auto addr = reinterpret_cast<uintptr_t>(ptr);
    if (addr == 0 || !rt.heap().contains(addr)) return false;
    return reinterpret_cast<const uint8_t*>(addr)[-1] == kHostKindClosure;
}

bool isUpvaluePtr(RuntimeState& rt, const void* ptr) noexcept {
    auto addr = reinterpret_cast<uintptr_t>(ptr);
    if (addr == 0 || !rt.heap().contains(addr)) return false;
    return reinterpret_cast<const uint8_t*>(addr)[-1] == kHostKindUpvalue;
}

uint32_t getSlotInternId(const RuntimeState& rt, uint32_t slot) noexcept {
    if (slot >= LuaTable::kInlineSlots) return 0;
    return rt.impl()->slot_to_intern[slot];
}

void setStringMetatable(RuntimeState& rt, LuaTable* mt) noexcept {
    rt.impl()->string_metatable = mt;
}

LuaTable* getStringMetatable(RuntimeState& rt) noexcept {
    return rt.impl()->string_metatable;
}

LuaCoroutine* getRunningCoroutine(RuntimeState& rt) noexcept {
    return rt.impl()->running_coroutine;
}

void setRunningCoroutine(RuntimeState& rt, LuaCoroutine* co) noexcept {
    rt.impl()->running_coroutine = co;
}

void armTableFinalizerIfNeeded(RuntimeState& rt, LuaTable* tbl) {
    if (!tbl || !tbl->metatable) return;
    if ((tbl->flags & kMetaGc) != 0 && (tbl->flags & kMetaGcArmed) == 0) {
        tbl->flags |= kMetaGcArmed;
        LuaTable* mt = tbl->metatable;
        uint32_t gc_id = rt.internString("__gc");
        uint64_t gc_fn_bits = brasslua_rt_rawget(
            Value::fromTable(mt).rawBits(),
            Value::fromString(rt.getLuaStringById(gc_id)).rawBits());
        if (Value(gc_fn_bits).isFunction()) {
            rt.heap().add_finalizer(
                reinterpret_cast<uintptr_t>(tbl),
                [](void* ctx) {
                    uint64_t fn_bits = reinterpret_cast<uintptr_t>(ctx)
                                           ? (static_cast<uint64_t>(Tag::Function) << kTagShift) |
                                                 (reinterpret_cast<uintptr_t>(ctx) & kPayloadMask)
                                           : kNilBits;
                    if (Value(fn_bits).isFunction()) {
                        try {
                            brasslua_rt_call_1(fn_bits, kNilBits);
                        } catch (...) {
                        }
                    }
                },
                Value(gc_fn_bits).asPointer());
        }
    }
}

} // namespace brasslua
