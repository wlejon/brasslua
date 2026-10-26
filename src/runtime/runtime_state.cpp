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

    t.visit(&tbl->cached_index_mm);

    // An inline part lives in this (pinned) object, and kEmptyHashPart off the
    // heap: only an out-of-line part is an object of its own to keep.
    if (tbl->array_data) {
        if (!tbl->isInline(tbl->array_data)) {
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
    if (tbl->hash_cap != 0) {
        if (!tbl->isInline(tbl->hash_data)) {
            t.visit_address(reinterpret_cast<uintptr_t*>(&tbl->hash_data));
        }
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

// The intern table: open addressing over (hash, id) pairs, so a probe reads
// one array and touches a string's bytes only when the hashes agree.
class InternTable {
public:
    // The id of the interned string of these bytes, or kNotInterned.
    uint32_t find(std::string_view bytes, uint32_t hash, const std::vector<LuaString*>& strings) const noexcept {
        if (slots_.empty()) return kNotInterned;
        const size_t mask = slots_.size() - 1;
        for (size_t i = hash & mask;; i = (i + 1) & mask) {
            const Slot& s = slots_[i];
            if (s.id == kNotInterned) return kNotInterned;
            if (s.hash == hash) {
                const LuaString* str = strings[s.id];
                if (str->length == bytes.size() && std::memcmp(str->data, bytes.data(), bytes.size()) == 0) {
                    return s.id;
                }
            }
        }
    }

    // Adds an id not in the table.
    void insert(uint32_t hash, uint32_t id) {
        if ((count_ + 1) * 2 > slots_.size()) grow();
        place(hash, id);
        ++count_;
    }

    void clear() noexcept {
        slots_.clear();
        count_ = 0;
    }

    // Holds just the strings still interned (null entries are gone).
    void rebuild(const std::vector<LuaString*>& strings) {
        std::fill(slots_.begin(), slots_.end(), Slot{});
        count_ = 0;
        for (uint32_t id = 0; id < strings.size(); ++id) {
            if (strings[id]) {
                place(strings[id]->hash, id);
                ++count_;
            }
        }
    }

private:
    struct Slot {
        uint32_t hash = 0;
        uint32_t id = kNotInterned;
    };

    void place(uint32_t hash, uint32_t id) noexcept {
        const size_t mask = slots_.size() - 1;
        size_t i = hash & mask;
        while (slots_[i].id != kNotInterned) i = (i + 1) & mask;
        slots_[i] = Slot{hash, id};
    }

    void grow() {
        std::vector<Slot> old = std::move(slots_);
        slots_.assign(old.empty() ? 1024 : old.size() * 2, Slot{});
        for (const Slot& s : old) {
            if (s.id != kNotInterned) place(s.hash, s.id);
        }
    }

    std::vector<Slot> slots_;
    size_t count_ = 0;
};

struct RuntimeState::Impl {
    std::unique_ptr<brass::gc::Heap> owned_heap;
    brass::gc::Heap* active_heap = nullptr;
    brass::gc::Heap::RootSourceId root_source_id = 0;
    uintptr_t stack_top_anchor = 0;

    // Interned strings never move. Those the compiler or the runtime names
    // (constants, globals, metamethod names) are roots for as long as the
    // runtime lives; one only made at run time is held weakly, so it is
    // collected like Lua's, when nothing else refers to it, and its id goes
    // back to be reused.
    enum InternState : uint8_t { kFree, kWeak, kStrong };
    InternTable str_to_id;
    std::vector<LuaString*> id_to_lstr;
    std::vector<uint8_t> intern_state;
    // A weak string's value: the slot the collector clears (a weak slot must
    // hold a tagged reference, not id_to_lstr's raw pointer).
    std::vector<uint64_t> intern_weak_slot;
    std::vector<uint32_t> free_ids;
    brass::gc::Heap::RootSourceId post_collection_hook_id = 0;

    // After a full collection settles weak slots: drops the dead strings.
    void sweepInterned() {
        bool any = false;
        for (uint32_t id = 0; id < id_to_lstr.size(); ++id) {
            if (intern_state[id] == kWeak && intern_weak_slot[id] == kNilBits) {
                id_to_lstr[id] = nullptr;
                intern_state[id] = kFree;
                free_ids.push_back(id);
                any = true;
            }
        }
        if (any) str_to_id.rebuild(id_to_lstr);
    }

    std::vector<RuntimeState::RootedValues*> rooted_values;
    LuaTable* env_table = nullptr;
    std::unordered_map<std::string, uint64_t> builtins; // registerBuiltin
    LuaTable* string_metatable = nullptr;
    LuaCoroutine* running_coroutine = nullptr;
    LuaCoroutine* main_thread = nullptr; // made when first asked for
    bool in_explicit_gc = false;

    void traceRoots(brass::gc::Tracer& t, RuntimeState& rt) {
        if (main_thread) {
            t.visit_address(reinterpret_cast<uintptr_t*>(&main_thread));
        }
        if (env_table) {
            t.visit_address(reinterpret_cast<uintptr_t*>(&env_table));
        }
        if (string_metatable) {
            t.visit_address(reinterpret_cast<uintptr_t*>(&string_metatable));
        }
        if (running_coroutine) {
            t.visit_address(reinterpret_cast<uintptr_t*>(&running_coroutine));
        }
        for (size_t id = 0; id < id_to_lstr.size(); ++id) {
            LuaString*& s = id_to_lstr[id];
            if (!s) continue;
            if (intern_state[id] == kStrong) {
                t.visit_address(reinterpret_cast<uintptr_t*>(&s));
            } else {
                intern_weak_slot[id] = Value::fromString(s).rawBits();
                t.visit_weak(&intern_weak_slot[id], kNilBits);
            }
        }
        for (RuntimeState::RootedValues* r : rooted_values) {
            for (uint64_t& v : r->values) t.visit(&v);
        }
        for (auto& [name, bits] : builtins) t.visit(&bits);
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
        unbindHeap();
        active_heap = h;
        if (active_heap) {
            brass::gc::Heap::set_current(active_heap);
            root_source_id = active_heap->add_root_source(
                [this, &rt](brass::gc::Tracer& t) { traceRoots(t, rt); });
            post_collection_hook_id = active_heap->add_post_collection_hook(
                [this](brass::gc::Heap&, brass::gc::CollectionKind kind) {
                    if (kind == brass::gc::CollectionKind::Full) sweepInterned();
                });
        }
    }

    void unbindHeap() noexcept {
        if (active_heap && root_source_id != 0) {
            active_heap->remove_root_source(root_source_id);
            root_source_id = 0;
        }
        if (active_heap && post_collection_hook_id != 0) {
            active_heap->remove_post_collection_hook(post_collection_hook_id);
            post_collection_hook_id = 0;
        }
    }

    ~Impl() { unbindHeap(); }
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
        impl_->unbindHeap();
        impl_->active_heap = nullptr;
        impl_->owned_heap = std::make_unique<brass::gc::Heap>(makeBrassLuaHeapConfig());
        impl_->bindHeap(impl_->owned_heap.get(), *this);
    }

    impl_->str_to_id.clear();
    impl_->id_to_lstr.clear();
    impl_->intern_state.clear();
    impl_->intern_weak_slot.clear();
    impl_->free_ids.clear();
    impl_->env_table = nullptr;
    impl_->builtins.clear();
    impl_->string_metatable = nullptr;
    impl_->running_coroutine = nullptr;
    impl_->main_thread = nullptr;
    multiret_.clear();
    single_ret_ = kNilBits;
    multiret_mode_ = 0;

    // Reserve intern_id 0 for the empty string ""
    internString("");

    impl_->env_table = allocTable(0, 64);
    registerStdlib();
}

void RuntimeState::registerStdlib() {
    registerCoreStdlib(*this);
    registerMathAndBitStdlib(*this);
    registerStringStdlib(*this);
    registerTableStdlib(*this);
    registerIoStdlib(*this);
}

uint32_t RuntimeState::internString(std::string_view str) {
    const uint32_t id = internAs(str, false);
    impl_->intern_state[id] = Impl::kStrong;
    return id;
}

uint32_t RuntimeState::internAs(std::string_view str, bool weak) {
    const uint32_t hash = fnv1aHash(str);
    const uint32_t id = impl_->str_to_id.find(str, hash, impl_->id_to_lstr);
    if (id != kNotInterned) return id;
    return adoptInterned(allocString(str), hash, weak);
}

LuaString* RuntimeState::allocString(std::string_view str) {
    size_t alloc_bytes = sizeof(LuaString) + str.size() + 1;
    uintptr_t mem = impl_->active_heap->allocate(
        alloc_bytes, getLayouts().leaf, brass::gc::kAllocPinned | brass::gc::kAllocUninitialized,
        kHostKindString);
    auto* lstr = reinterpret_cast<LuaString*>(mem);
    lstr->hash = 0;
    lstr->length = static_cast<uint32_t>(str.size());
    lstr->intern_id = kNotInterned;
    lstr->reserved = 0;
    if (!str.empty()) {
        std::memcpy(lstr->data, str.data(), str.size());
    }
    lstr->data[str.size()] = '\0';
    return lstr;
}

uint32_t RuntimeState::adoptInterned(LuaString* lstr, uint32_t hash, bool weak) {
    uint32_t id = 0;
    if (!impl_->free_ids.empty()) {
        id = impl_->free_ids.back();
        impl_->free_ids.pop_back();
        impl_->id_to_lstr[id] = lstr;
    } else {
        id = static_cast<uint32_t>(impl_->id_to_lstr.size());
        impl_->id_to_lstr.push_back(lstr);
        impl_->intern_state.push_back(Impl::kFree);
        impl_->intern_weak_slot.push_back(kNilBits);
    }
    impl_->intern_state[id] = weak ? Impl::kWeak : Impl::kStrong;
    lstr->hash = hash;
    lstr->intern_id = id;
    impl_->str_to_id.insert(hash, id);
    return id;
}

uint64_t RuntimeState::internKey(uint64_t str_bits, bool insert) {
    auto* lstr = static_cast<LuaString*>(Value(str_bits).asPointer());
    if (lstr->intern_id != kNotInterned) return str_bits;
    std::string_view bytes(lstr->data, lstr->length);
    const uint32_t hash = fnv1aHash(bytes);
    const uint32_t id = impl_->str_to_id.find(bytes, hash, impl_->id_to_lstr);
    if (id != kNotInterned) {
        return Value::fromString(impl_->id_to_lstr[id]).rawBits();
    }
    if (!insert) return kNilBits;
    adoptInterned(lstr, hash, true);
    return str_bits;
}

RuntimeState::RootedValues::RootedValues(RuntimeState& rt) : rt_(rt) {
    rt_.impl_->rooted_values.push_back(this);
}

RuntimeState::RootedValues::~RootedValues() {
    auto& list = rt_.impl_->rooted_values;
    // Guards nest, so this one is last unless a guard outlives its scope.
    if (!list.empty() && list.back() == this) {
        list.pop_back();
    } else {
        list.erase(std::find(list.begin(), list.end(), this));
    }
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
    if (str.size() > kShortStringMax) {
        return Value::fromString(allocString(str)).rawBits();
    }
    uint32_t id = internAs(str, true);
    return Value::fromString(impl_->id_to_lstr[id]).rawBits();
}

LuaTable* RuntimeState::envTable() const noexcept {
    return impl_->env_table;
}

void RuntimeState::registerBuiltin(std::string_view name, Value closure) {
    impl_->builtins[std::string(name)] = closure.rawBits();
}

uint64_t RuntimeState::builtinBits(std::string_view name) const {
    auto it = impl_->builtins.find(std::string(name));
    return it == impl_->builtins.end() ? kNilBits : it->second;
}

void RuntimeState::setGlobal(std::string_view name, Value val) {
    const uint32_t id = internString(name);
    brasslua_rt_rawset(Value::fromTable(impl_->env_table).rawBits(),
                       Value::fromString(impl_->id_to_lstr[id]).rawBits(), val.rawBits());
}

Value RuntimeState::getGlobal(std::string_view name) {
    const uint32_t id = internString(name);
    uint64_t v = brasslua_rt_rawget(Value::fromTable(impl_->env_table).rawBits(),
                                    Value::fromString(impl_->id_to_lstr[id]).rawBits());
    if (Value(v).isNil() && host_bridge_.getGlobal) v = host_bridge_.getGlobal(name);
    return Value(v);
}

void RuntimeState::ensureTableArrayCapacity(LuaTable* tbl, uint32_t min_cap) {
    if (min_cap <= tbl->array_cap) return;
    uint32_t new_cap = tbl->array_cap < 4 ? 4 : tbl->array_cap;
    while (new_cap < min_cap) {
        new_cap *= 2;
    }
    uintptr_t mem = impl_->active_heap->allocate(
        static_cast<size_t>(new_cap) * sizeof(uint64_t),
        getLayouts().leaf,
        brass::gc::kAllocPinned | brass::gc::kAllocUninitialized,  // every slot written below
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
    uint32_t required = min_entries < 2 ? 4 : min_entries * 2;
    if (required <= tbl->hash_cap) return;
    uint32_t new_cap = tbl->hash_cap == 0 ? 4 : tbl->hash_cap;
    while (new_cap < required) {
        new_cap *= 2;
    }
    uintptr_t mem = impl_->active_heap->allocate(
        static_cast<size_t>(new_cap) * sizeof(LuaHashEntry),
        getLayouts().leaf,
        brass::gc::kAllocPinned | brass::gc::kAllocUninitialized,  // every entry written below
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
    tbl->hash_mask = new_cap - 1;
    tbl->hash_len = new_len;
}

const LuaHashEntry kEmptyHashPart[1] = {};

uint32_t tableHashCapFor(uint32_t entries) noexcept {
    if (entries == 0) return 0;
    uint32_t cap = 2;
    while (cap < entries * 2) cap *= 2;
    return cap;
}

LuaTable* RuntimeState::allocTable(uint32_t array_hint, uint32_t hash_hint) {
    const uint32_t inline_arr = array_hint <= LuaTable::kMaxInlineArray ? array_hint : 0;
    uint32_t inline_hash = tableHashCapFor(hash_hint);
    if (inline_hash > LuaTable::kMaxInlineHash) inline_hash = 0;
    const size_t inline_bytes = size_t{inline_arr} * sizeof(uint64_t) + size_t{inline_hash} * sizeof(LuaHashEntry);
    uintptr_t mem = impl_->active_heap->allocate(
        sizeof(LuaTable) + inline_bytes, getLayouts().table, brass::gc::kAllocPinned, kHostKindTable);
    auto* tbl = new (reinterpret_cast<void*>(mem)) LuaTable();
    tbl->inline_bytes = static_cast<uint32_t>(inline_bytes);
    uint64_t* store = tbl->inlineStorage();
    for (size_t i = 0; i < inline_bytes / sizeof(uint64_t); ++i) {
        store[i] = kNilBits;
    }
    tbl->hash_data = const_cast<LuaHashEntry*>(kEmptyHashPart);
    if (inline_arr > 0) {
        tbl->array_data = store;
        tbl->array_cap = inline_arr;
    } else if (array_hint > 0) {
        ensureTableArrayCapacity(tbl, array_hint);
    }
    if (inline_hash > 0) {
        tbl->hash_data = reinterpret_cast<LuaHashEntry*>(store + inline_arr);
        tbl->hash_cap = inline_hash;
        tbl->hash_mask = inline_hash - 1;
    } else if (hash_hint > 0) {
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

LuaCoroutine* getMainThread(RuntimeState& rt) {
    if (!rt.impl()->main_thread) {
        LuaCoroutine* co = rt.allocCoroutine(nullptr);
        co->status = CoroutineStatus::Running;
        rt.impl()->main_thread = co;
    }
    return rt.impl()->main_thread;
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
