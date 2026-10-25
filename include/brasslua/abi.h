#ifndef BRASSLUA_ABI_H
#define BRASSLUA_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- Table operations ----
uint64_t brasslua_rt_table_new(uint32_t array_hint, uint32_t hash_hint);
uint64_t brasslua_rt_table_get(uint64_t table, uint64_t key);
uint64_t brasslua_rt_table_set(uint64_t table, uint64_t key, uint64_t val);
uint64_t brasslua_rt_table_get_int(uint64_t table, int64_t idx);
uint64_t brasslua_rt_table_set_int(uint64_t table, int64_t idx, uint64_t val);
// `key` is an interned string's Value bits (interned strings never move or die).
uint64_t brasslua_rt_table_get_field(uint64_t table, uint64_t key);
uint64_t brasslua_rt_table_set_field(uint64_t table, uint64_t key, uint64_t val);
uint64_t brasslua_rt_table_append_multiret(uint64_t table, int64_t start_idx);
int64_t  brasslua_rt_table_len(uint64_t table);
uint64_t brasslua_rt_rawget(uint64_t table, uint64_t key);
uint64_t brasslua_rt_rawset(uint64_t table, uint64_t key, uint64_t val);
uint64_t brasslua_rt_next(uint64_t table, uint64_t key, uint64_t* out_val);

// ---- Globals & Environment ----
uint64_t brasslua_rt_get_global(uint32_t intern_id);
void     brasslua_rt_set_global(uint32_t intern_id, uint64_t val);

// ---- Strings, Boxing & Interning ----
uint64_t brasslua_rt_box_int64(int64_t v);
uint64_t brasslua_rt_concat(uint64_t a, uint64_t b);
int64_t  brasslua_rt_obj_len(uint64_t v);

// ---- Closures, Upvalues & To-Be-Closed Locals ----
uint64_t brasslua_rt_closure_new(void* fn_ptr, void* unboxed_ptr, uint32_t upval_count, uint32_t arity);
void*    brasslua_rt_upvalue_new(uint64_t initial_val);
uint64_t brasslua_rt_upvalue_get(void* closure_or_upval, uint32_t idx);
void     brasslua_rt_upvalue_set(void* closure_or_upval, uint32_t idx, uint64_t val);
void     brasslua_rt_close_local(uint64_t val);

// ---- Dynamic Calls, Method Calls & Multi-Return Buffer ----
uint64_t brasslua_rt_call(uint64_t callee, const uint64_t* args, int64_t nargs);
uint64_t brasslua_rt_call_0(uint64_t callee);
uint64_t brasslua_rt_call_1(uint64_t callee, uint64_t a0);
uint64_t brasslua_rt_call_2(uint64_t callee, uint64_t a0, uint64_t a1);
uint64_t brasslua_rt_call_3(uint64_t callee, uint64_t a0, uint64_t a1, uint64_t a2);
uint64_t brasslua_rt_call_4(uint64_t callee, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
// Explicit method call (`obj:method(...)`) where compiler statically knows receiver `obj`:
// args[0] (and prefix[0]) is obj itself, the method's self.
uint64_t brasslua_rt_call_method(uint64_t obj, uint64_t method_key, const uint64_t* args, int64_t nargs);
uint64_t brasslua_rt_call_multiret(uint64_t callee, const uint64_t* prefix_args, int64_t prefix_count);
uint64_t brasslua_rt_call_method_multiret(uint64_t obj, uint64_t method_key,
                                          const uint64_t* prefix_args, int64_t prefix_count);
void     brasslua_rt_set_multiret(const uint64_t* vals, int64_t count);
uint64_t brasslua_rt_prepend_multiret(const uint64_t* prefix, int64_t prefix_count);
uint64_t brasslua_rt_set_vararg_multiret(const uint64_t* args_ptr, int64_t nargs, int64_t vararg_offset);
int64_t  brasslua_rt_get_multiret_count(void);
uint64_t brasslua_rt_get_multiret(int64_t idx);

// ---- Boxed Dynamic Arithmetic / Metamethod Fallbacks ----
uint64_t brasslua_rt_add(uint64_t a, uint64_t b);
uint64_t brasslua_rt_sub(uint64_t a, uint64_t b);
uint64_t brasslua_rt_mul(uint64_t a, uint64_t b);
uint64_t brasslua_rt_div(uint64_t a, uint64_t b);
uint64_t brasslua_rt_idiv(uint64_t a, uint64_t b);
uint64_t brasslua_rt_mod(uint64_t a, uint64_t b);
uint64_t brasslua_rt_pow(uint64_t a, uint64_t b);
uint64_t brasslua_rt_unm(uint64_t a);
uint64_t brasslua_rt_band(uint64_t a, uint64_t b);
uint64_t brasslua_rt_bor(uint64_t a, uint64_t b);
uint64_t brasslua_rt_bxor(uint64_t a, uint64_t b);
uint64_t brasslua_rt_bnot(uint64_t a);
uint64_t brasslua_rt_shl(uint64_t a, uint64_t b);
uint64_t brasslua_rt_shr(uint64_t a, uint64_t b);
int32_t  brasslua_rt_eq(uint64_t a, uint64_t b);
int32_t  brasslua_rt_lt(uint64_t a, uint64_t b);
int32_t  brasslua_rt_le(uint64_t a, uint64_t b);
double   brasslua_rt_to_number(uint64_t v);
int64_t  brasslua_rt_to_int64(uint64_t v);

// ---- Unboxed Math Intrinsics ----
double   brasslua_rt_fmod(double a, double b);
double   brasslua_rt_pow_f64(double a, double b);
int64_t  brasslua_rt_idiv_i64(int64_t a, int64_t b);
int64_t  brasslua_rt_mod_i64(int64_t a, int64_t b);

#ifdef __cplusplus
}
#endif

#endif // BRASSLUA_ABI_H
