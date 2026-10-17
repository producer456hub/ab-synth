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
int    ab_steals(int inst);
void   ab_reset_steals(int inst);
// multitrack session ("Gen1 OS")
int    ab_session_init(float sampleRate, int maxBlock, int nTracks);
void   ab_session_process(int n);
void   ab_session_set_param(int t, int id, float v);
void   ab_session_note_on(int t, int note, float vel);
void   ab_session_note_off(int t, int note);
void   ab_session_all_off(int t);
void   ab_session_panic(void);
void   ab_session_set_profile(int t, int profile);
void   ab_session_set_constraint(int t, int c, int on);
void   ab_session_mix(int t, float g, float pan, int mute, int solo);
void   ab_session_master(float g);
int    ab_session_voices(int t);
int    ab_session_max_voices(int t);
int    ab_session_steals(int t);
void   ab_session_reset_steals(int t);
float  ab_session_peak(int t);

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
