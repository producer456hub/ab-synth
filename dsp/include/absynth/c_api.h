// c_api.h - flat C ABI exported to WebAssembly (see c_api.cpp).
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

int    ab_init(float sampleRate, int maxBlock);
float* ab_out_ptr(int ch);
int    ab_out_capacity(void);
void   ab_process(int n);
void   ab_process_single(int inst, int n);
void   ab_set_mix(float x);
void   ab_set_trim_db(int inst, float db);
void   ab_note_on(int note, float vel);
void   ab_note_off(int note);
void   ab_all_notes_off(void);
void   ab_panic(void);
void   ab_set_param(int id, float v);
float  ab_get_param(int id);
void   ab_set_profile(int inst, int profile);
void   ab_set_constraint(int inst, int c, int on);
int    ab_active_voices(int inst);
int    ab_latency(int inst);
int    ab_max_voices(int inst);
int    ab_param_count(void);
const char* ab_param_name(int id);
float  ab_param_min(int id);
float  ab_param_max(int id);
float  ab_param_default(int id);
int    ab_param_index(const char* name);
int    ab_constraint_count(void);

#ifdef __cplusplus
}
#endif
