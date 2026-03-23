#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <math.h>
#include <alsa/asoundlib.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <pthread.h>
#include <ctype.h>
#include <string.h>

/*
 * White Noise Generator Library
 * 
 * Implements a simple, state-preserving white noise generator using 
 * a Linear Congruential Generator (LCG) for randomness.
 */

typedef struct {
    float duration; // Duration in seconds
    float amp;      // Target Amplitude (Gate)
    float start;    // Decimation Start Factor
    float end;      // Decimation End Factor
    float pan_start; // Pan Start (-1.0 Left, 1.0 Right)
    float pan_end;   // Pan End
    float probability; // Probability (0.0 to 1.0)
    float stutter_freq; // Stutter frequency in Hz (0.0 = off)
} SeqStep;

typedef struct {
    float amplitude;  // Volume scalar (0.0 to 1.0 usually)
    uint32_t state;   // Internal random state
    float last_sample; // State for low-pass filter
    float alpha;       // Filter coefficient (0.0 to 1.0)
    pthread_mutex_t lock; // Thread synchronization
    int stop_flag;    // Flag to signal playback stop
    float fade_in_sec; // Fade-in duration in seconds
    float fade_out_sec; // Fade-out duration in seconds
    float crush_scale; // Factor for bit reduction (0.0 = disabled)
    float decimate_factor; // Sample rate reduction factor (>= 1.0)
    float decimate_accum; // Accumulator for decimation
    float held_sample; // Last value held for decimation
    unsigned int sample_rate; // Current playback sample rate
    int glitch_active; // Is a glitch sweep active?
    unsigned long glitch_samples_total; // Total duration of sweep in samples
    unsigned long glitch_samples_done; // Samples processed in sweep
    float glitch_start_factor; // Start decimation factor
    float glitch_end_factor; // End decimation factor
    // Sequencer state
    SeqStep* seq_steps;       // Array of sequence steps
    size_t seq_count;         // Number of steps
    size_t seq_index;         // Current step index
    unsigned long seq_progress; // Samples processed in current step
    int seq_active;           // Is sequencer active?
    int seq_loop;             // Should sequencer loop?
    unsigned int channels;    // Active channel count (for timing)
    unsigned int channel_index; // Current channel index (0=L, 1=R...)
    int seq_step_muted;         // Current step muted flag (probability check)
    float tempo;                // Global tempo scaler (1.0 = normal speed)
    uint32_t stutter_seed;      // Saved seed for stutter loop
    float stutter_last_sample;  // Saved filter state for stutter
    float stutter_decim_accum;  // Saved decimation accumulator
    unsigned long stutter_len;  // Length of stutter loop in samples (ticks)
    unsigned long stutter_cnt;  // Current stutter counter
    int super_stutter_active;
    unsigned long super_stutter_samples_total;
    unsigned long super_stutter_samples_done;
    unsigned long super_stutter_loop_len;
    unsigned long super_stutter_loop_cnt;
    uint32_t super_stutter_seed;
    float super_stutter_last_sample;
    float super_stutter_decim_accum;
    float auto_glitch_freq; // Auto-glitch frequency (events per second)
} NoiseContext;

typedef struct {
    pthread_barrier_t barrier;
} NoiseSync;

/**
 * Internal helper: Linear Congruential Generator.
 * Using standard glibc constants.
 */
static uint32_t _noise_lcg_rand(uint32_t* state) {
    *state = (*state * 1103515245 + 12345) & 0x7FFFFFFF;
    return *state;
}

/**
 * Initialize the noise context.
 * 
 * @param ctx Pointer to the NoiseContext structure.
 * @param amplitude Output volume (e.g., 0.5 for half volume).
 * @param seed Initial seed for the random number generator.
 */
void noise_init(NoiseContext* ctx, float amplitude, uint32_t seed) {
    if (!ctx) return;
    ctx->amplitude = amplitude;
    ctx->state = seed;
    ctx->last_sample = 0.0f;
    ctx->alpha = 1.0f; // Default: pass-through (no filtering)
    pthread_mutex_init(&ctx->lock, NULL);
    ctx->stop_flag = 0;
    ctx->fade_in_sec = 0.0f;
    ctx->fade_out_sec = 0.0f;
    ctx->crush_scale = 0.0f;
    ctx->decimate_factor = 1.0f;
    ctx->decimate_accum = 0.0f;
    ctx->held_sample = 0.0f;
    ctx->sample_rate = 44100; // Default
    ctx->glitch_active = 0;
    ctx->glitch_samples_total = 0;
    ctx->glitch_samples_done = 0;
    ctx->glitch_start_factor = 1.0f;
    ctx->glitch_end_factor = 1.0f;
    ctx->seq_steps = NULL;
    ctx->seq_count = 0;
    ctx->seq_index = 0;
    ctx->seq_progress = 0;
    ctx->seq_active = 0;
    ctx->seq_loop = 0;
    ctx->channels = 1; // Default to mono
    ctx->channel_index = 0;
    ctx->seq_step_muted = 0;
    ctx->tempo = 1.0f;
    ctx->stutter_seed = 0;
    ctx->stutter_last_sample = 0.0f;
    ctx->stutter_decim_accum = 0.0f;
    ctx->stutter_len = 0;
    ctx->stutter_cnt = 0;
    ctx->super_stutter_active = 0;
    ctx->auto_glitch_freq = 0.0f;
}

/**
 * Update the amplitude parameter safely.
 * 
 * @param ctx Pointer to the NoiseContext structure.
 * @param amplitude New amplitude value.
 */
void noise_set_amplitude(NoiseContext* ctx, float amplitude) {
    if (!ctx) return;
    pthread_mutex_lock(&ctx->lock);
    ctx->amplitude = amplitude;
    pthread_mutex_unlock(&ctx->lock);
}

/**
 * Set the low-pass filter coefficient.
 * 
 * @param ctx Pointer to the NoiseContext structure.
 * @param alpha Filter coefficient (1.0 = no filter, 0.001 = heavy muffling).
 */
void noise_set_lpf_alpha(NoiseContext* ctx, float alpha) {
    if (!ctx) return;
    if (alpha < 0.0f) alpha = 0.0f; else if (alpha > 1.0f) alpha = 1.0f;
    pthread_mutex_lock(&ctx->lock);
    ctx->alpha = alpha;
    pthread_mutex_unlock(&ctx->lock);
}

/**
 * Set fade-in and fade-out durations.
 * 
 * @param ctx Pointer to the NoiseContext structure.
 * @param in_sec Fade-in duration in seconds.
 * @param out_sec Fade-out duration in seconds.
 */
void noise_set_fade(NoiseContext* ctx, float in_sec, float out_sec) {
    if (!ctx) return;
    pthread_mutex_lock(&ctx->lock);
    ctx->fade_in_sec = (in_sec < 0.0f) ? 0.0f : in_sec;
    ctx->fade_out_sec = (out_sec < 0.0f) ? 0.0f : out_sec;
    pthread_mutex_unlock(&ctx->lock);
}

/**
 * Set bit depth for bit reduction effect.
 * 
 * @param ctx Pointer to the NoiseContext structure.
 * @param bits Bit depth (e.g., 4.0 for 4-bit sound). Set to 0 or >= 32 to disable.
 */
void noise_set_bit_depth(NoiseContext* ctx, float bits) {
    if (!ctx) return;
    pthread_mutex_lock(&ctx->lock);
    if (bits > 0.0f && bits < 32.0f) {
        ctx->crush_scale = powf(2.0f, bits);
    } else {
        ctx->crush_scale = 0.0f;
    }
    pthread_mutex_unlock(&ctx->lock);
}

/**
 * Set sample rate reduction factor (decimation).
 * 
 * @param ctx Pointer to the NoiseContext structure.
 * @param factor Reduction factor (e.g., 1.0 = normal, 2.0 = half rate, 44.1 = 1kHz effective rate at 44.1kHz).
 */
void noise_set_decimation(NoiseContext* ctx, float factor) {
    if (!ctx) return;
    if (factor < 1.0f) factor = 1.0f;
    pthread_mutex_lock(&ctx->lock);
    ctx->decimate_factor = factor;
    pthread_mutex_unlock(&ctx->lock);
}

/**
 * Set sequencer tempo scaling factor.
 * 
 * @param ctx Pointer to the NoiseContext structure.
 * @param tempo Speed multiplier (1.0 = normal, 2.0 = double speed).
 */
void noise_set_tempo(NoiseContext* ctx, float tempo) {
    if (!ctx) return;
    if (tempo <= 0.0f) tempo = 0.001f;
    pthread_mutex_lock(&ctx->lock);
    ctx->tempo = tempo;
    pthread_mutex_unlock(&ctx->lock);
}

/**
 * Configure a dynamic glitch sweep (automates decimation factor).
 * 
 * @param ctx Pointer to the NoiseContext structure.
 * @param duration_sec Duration of the sweep in seconds.
 * @param start_factor Decimation factor at start.
 * @param end_factor Decimation factor at end.
 */
void noise_glitch(NoiseContext* ctx, float duration_sec, float start_factor, float end_factor) {
    if (!ctx) return;
    if (duration_sec <= 0.0f) duration_sec = 0.001f;
    
    pthread_mutex_lock(&ctx->lock);
    ctx->glitch_active = 1;
    ctx->glitch_samples_total = (unsigned long)(duration_sec * (float)ctx->sample_rate);
    ctx->glitch_samples_done = 0;
    ctx->glitch_start_factor = (start_factor < 1.0f) ? 1.0f : start_factor;
    ctx->glitch_end_factor = (end_factor < 1.0f) ? 1.0f : end_factor;
    // Set initial state immediately
    ctx->decimate_factor = ctx->glitch_start_factor;
    pthread_mutex_unlock(&ctx->lock);
}

/**
 * Trigger a "super stutter" effect that loops the current generator state.
 * 
 * @param ctx Pointer to the NoiseContext structure.
 * @param duration_sec Total duration of the effect.
 * @param freq Frequency of the loop (e.g. 20Hz = 50ms loop).
 */
void noise_super_stutter(NoiseContext* ctx, float duration_sec, float freq) {
    if (!ctx) return;
    pthread_mutex_lock(&ctx->lock);
    ctx->super_stutter_active = 1;
    ctx->super_stutter_samples_total = (unsigned long)(duration_sec * (float)ctx->sample_rate);
    ctx->super_stutter_samples_done = 0;
    
    unsigned int ch = (ctx->channels > 0) ? ctx->channels : 1;
    ctx->super_stutter_loop_len = (freq > 0.0f) ? (unsigned long)((float)ctx->sample_rate / freq * (float)ch) : 0;
    if (ctx->super_stutter_loop_len == 0) ctx->super_stutter_loop_len = 1;
    
    ctx->super_stutter_loop_cnt = 0;
    
    // Capture state immediately
    ctx->super_stutter_seed = ctx->state;
    ctx->super_stutter_last_sample = ctx->last_sample;
    ctx->super_stutter_decim_accum = ctx->decimate_accum;
    pthread_mutex_unlock(&ctx->lock);
}

/**
 * Set the frequency of automatic random glitches.
 * 
 * @param ctx Pointer to the NoiseContext structure.
 * @param freq Average glitches per second (e.g., 0.5 = one every 2s). 0.0 to disable.
 */
void noise_set_auto_glitch(NoiseContext* ctx, float freq) {
    if (!ctx) return;
    pthread_mutex_lock(&ctx->lock);
    ctx->auto_glitch_freq = (freq < 0.0f) ? 0.0f : freq;
    pthread_mutex_unlock(&ctx->lock);
}

/**
 * Internal unsafe tick function (must hold lock).
 * 
 * @param ctx Pointer to the NoiseContext structure.
 * @return A float sample in the range [-amplitude, amplitude].
 */
static float _noise_tick_unsafe(NoiseContext* ctx) {
    // Handle Sequencer

    // Auto Glitch Logic (Probabilistic trigger)
    // We check this before sequence logic to potentially override it
    if (ctx->auto_glitch_freq > 0.0f && !ctx->super_stutter_active && !ctx->glitch_active) {
        uint32_t r = _noise_lcg_rand(&ctx->state);
        float n = (float)r / (float)0x7FFFFFFF;
        
        // Calculate probability per sample based on events per second
        // P = freq / sample_rate
        float prob = ctx->auto_glitch_freq / (float)ctx->sample_rate;
        
        if (n < prob) {
            // Trigger Random Super Stutter
            // Duration: 0.05s to 0.3s
            // Freq: 20Hz to 200Hz
            
            uint32_t r2 = _noise_lcg_rand(&ctx->state);
            float dur = 0.05f + ((float)(r2 % 100) / 100.0f) * 0.25f;
            
            uint32_t r3 = _noise_lcg_rand(&ctx->state);
            float freq = 20.0f + ((float)(r3 % 100) / 100.0f) * 180.0f;
            
            // Setup Super Stutter
            ctx->super_stutter_active = 1;
            ctx->super_stutter_samples_total = (unsigned long)(dur * (float)ctx->sample_rate);
            ctx->super_stutter_samples_done = 0;
            unsigned int ch = (ctx->channels > 0) ? ctx->channels : 1;
            ctx->super_stutter_loop_len = (unsigned long)((float)ctx->sample_rate / freq * (float)ch);
            if (ctx->super_stutter_loop_len == 0) ctx->super_stutter_loop_len = 1;
            ctx->super_stutter_loop_cnt = 0;
            ctx->super_stutter_seed = ctx->state;
            ctx->super_stutter_last_sample = ctx->last_sample;
            ctx->super_stutter_decim_accum = ctx->decimate_accum;
        }
    }

    if (ctx->seq_active && ctx->seq_count > 0) {
        SeqStep* step = &ctx->seq_steps[ctx->seq_index];
        
        // Calculate duration in total samples (frames * channels)
        // If channels is 0 (safety), assume 1.
        unsigned int ch = (ctx->channels > 0) ? ctx->channels : 1;
        
        float duration = step->duration / ctx->tempo;
        unsigned long step_total_samples = (unsigned long)(duration * (float)ctx->sample_rate * (float)ch);
        if (step_total_samples == 0) step_total_samples = 1;

        ctx->seq_progress++;
        
        // Check probability at the very start of the step
        if (ctx->seq_progress == 1) {
            if (step->probability >= 1.0f) ctx->seq_step_muted = 0;
            else if (step->probability <= 0.0f) ctx->seq_step_muted = 1;
            else {
                uint32_t r = _noise_lcg_rand(&ctx->state);
                float n = (float)r / (float)0x7FFFFFFF;
                ctx->seq_step_muted = (n > step->probability) ? 1 : 0;
            }
        }

        // Handle Stutter (Repeat Buffer Segment)
        // Check at start of step
        if (ctx->seq_progress == 1) {
            if (step->stutter_freq > 0.0f) {
                unsigned int ch = (ctx->channels > 0) ? ctx->channels : 1;
                ctx->stutter_len = (unsigned long)((float)ctx->sample_rate / step->stutter_freq * (float)ch);
                if (ctx->stutter_len == 0) ctx->stutter_len = 1;
                // Save state
                ctx->stutter_seed = ctx->state;
                ctx->stutter_last_sample = ctx->last_sample;
                ctx->stutter_decim_accum = ctx->decimate_accum;
                ctx->stutter_cnt = 0;
            } else {
                ctx->stutter_len = 0;
            }
        }
        // Apply Stutter Loop
        if (ctx->stutter_len > 0) {
            if (ctx->stutter_cnt >= ctx->stutter_len) {
                ctx->state = ctx->stutter_seed;
                ctx->last_sample = ctx->stutter_last_sample;
                ctx->decimate_accum = ctx->stutter_decim_accum;
                ctx->stutter_cnt = 0;
            }
            ctx->stutter_cnt++;
        }

        // Calculate interpolation factor t (0.0 to 1.0)
        float t = (float)ctx->seq_progress / (float)step_total_samples;
        if (t > 1.0f) t = 1.0f;

        // Calculate Pan
        float pan = step->pan_start + t * (step->pan_end - step->pan_start);

        // Apply step parameters

        // Handle Super Stutter (Global override applied on top of sequence)
        if (ctx->super_stutter_active) {
            ctx->super_stutter_samples_done++;
            if (ctx->super_stutter_samples_done >= ctx->super_stutter_samples_total) {
                ctx->super_stutter_active = 0;
            } else {
                if (ctx->super_stutter_loop_cnt >= ctx->super_stutter_loop_len) {
                    // Restore state to loop start
                    ctx->state = ctx->super_stutter_seed;
                    ctx->last_sample = ctx->super_stutter_last_sample;
                    ctx->decimate_accum = ctx->super_stutter_decim_accum;
                    ctx->super_stutter_loop_cnt = 0;
                }
                ctx->super_stutter_loop_cnt++;
            }
        }
        
        // Lerp decimation factor
        ctx->decimate_factor = step->start + t * (step->end - step->start);
        // Set amplitude directly (hard gate)
        ctx->amplitude = (ctx->seq_step_muted) ? 0.0f : step->amp;

        // Step complete?
        if (ctx->seq_progress >= step_total_samples) {
            ctx->seq_progress = 0;
            ctx->seq_index++;
            // Sequence complete?
            if (ctx->seq_index >= ctx->seq_count) {
                if (ctx->seq_loop) {
                    ctx->seq_index = 0;
                } else {
                    ctx->seq_active = 0;
                    // Sequence finished
                }
            }
        }

        // Apply Pan Gain (Simple Balance)
        // Only applies if stereo (channels == 2)
        float pan_gain = 1.0f;
        if (ctx->channels == 2) {
            if (ctx->channel_index == 0) { // Left
                pan_gain = (pan > 0.0f) ? (1.0f - pan) : 1.0f;
            } else { // Right
                pan_gain = (pan < 0.0f) ? (1.0f + pan) : 1.0f;
            }
        }
        
        // Apply to amplitude temporarily for this return value calculation
        // We don't modify ctx->amplitude permanently because it's shared state for the step
        // But we return the gained value.
        
        ctx->decimate_accum += 1.0f;
        if (ctx->decimate_accum >= ctx->decimate_factor) {
            while (ctx->decimate_accum >= ctx->decimate_factor) {
                ctx->decimate_accum -= ctx->decimate_factor;
            }
            uint32_t r = _noise_lcg_rand(&ctx->state);
            float n = (float)r / (float)0x7FFFFFFF;
            float raw = (n * 2.0f) - 1.0f;
            ctx->last_sample += ctx->alpha * (raw - ctx->last_sample);
            float sample = ctx->last_sample;
            if (ctx->crush_scale > 0.0f) {
                sample = floorf(sample * ctx->crush_scale) / ctx->crush_scale;
            }
            ctx->held_sample = sample;
        }
        
        // Advance channel index
        ctx->channel_index++;
        if (ctx->channel_index >= ctx->channels) ctx->channel_index = 0;
        
        return ctx->held_sample * ctx->amplitude * pan_gain;
    }
    
    if (ctx->glitch_active) {
        // Handle Single Glitch Sweep Automation
        ctx->glitch_samples_done++;
        float t = (float)ctx->glitch_samples_done / (float)ctx->glitch_samples_total;
        if (t >= 1.0f) { t = 1.0f; ctx->glitch_active = 0; }
        
        // Lerp decimation factor
        ctx->decimate_factor = ctx->glitch_start_factor + t * (ctx->glitch_end_factor - ctx->glitch_start_factor);
    }

    // Handle Super Stutter (Global override for non-sequencer mode)
    if (ctx->super_stutter_active) {
        ctx->super_stutter_samples_done++;
        if (ctx->super_stutter_samples_done >= ctx->super_stutter_samples_total) {
            ctx->super_stutter_active = 0;
        } else {
            if (ctx->super_stutter_loop_cnt >= ctx->super_stutter_loop_len) {
                ctx->state = ctx->super_stutter_seed;
                ctx->last_sample = ctx->super_stutter_last_sample;
                ctx->decimate_accum = ctx->super_stutter_decim_accum;
                ctx->super_stutter_loop_cnt = 0;
            }
            ctx->super_stutter_loop_cnt++;
        }
    }

    ctx->decimate_accum += 1.0f;
    
    // Only update the internal state when the accumulator reaches the factor
    if (ctx->decimate_accum >= ctx->decimate_factor) {
        while (ctx->decimate_accum >= ctx->decimate_factor) {
            ctx->decimate_accum -= ctx->decimate_factor;
        }

        uint32_t r = _noise_lcg_rand(&ctx->state);
        float n = (float)r / (float)0x7FFFFFFF;
        float raw = (n * 2.0f) - 1.0f;

        // Apply simple one-pole low-pass filter
        ctx->last_sample += ctx->alpha * (raw - ctx->last_sample);

        float sample = ctx->last_sample;

        if (ctx->crush_scale > 0.0f) {
            sample = floorf(sample * ctx->crush_scale) / ctx->crush_scale;
        }
        ctx->held_sample = sample;
    }

    // Advance channel index for non-sequencer playback too (to keep state consistent if mixed)
    ctx->channel_index++;
    if (ctx->channel_index >= ctx->channels) ctx->channel_index = 0;

    return ctx->held_sample * ctx->amplitude;
}

/**
 * Generate a single sample of white noise (thread-safe).
 */
float noise_tick(NoiseContext* ctx) {
    if (!ctx) return 0.0f;
    pthread_mutex_lock(&ctx->lock);
    float val = _noise_tick_unsafe(ctx);
    pthread_mutex_unlock(&ctx->lock);
    return val;
}

/**
 * Stop any ongoing playback.
 * 
 * @param ctx Pointer to the NoiseContext structure.
 */
void noise_stop(NoiseContext* ctx) {
    if (!ctx) return;
    pthread_mutex_lock(&ctx->lock);
    ctx->stop_flag = 1;
    pthread_mutex_unlock(&ctx->lock);
}

/**
 * Fill a buffer with continuous white noise.
 * 
 * @param ctx Pointer to the NoiseContext structure.
 * @param buffer Output buffer for float samples.
 * @param num_samples Number of samples to write.
 */
void noise_process_buffer(NoiseContext* ctx, float* buffer, size_t num_samples) {
    if (!ctx || !buffer) return;
    pthread_mutex_lock(&ctx->lock);
    for (size_t i = 0; i < num_samples; i++) {
        buffer[i] = _noise_tick_unsafe(ctx);
    }
    pthread_mutex_unlock(&ctx->lock);
}

/**
 * Plays the noise through the default ALSA audio device (speakers).
 * Note: Requires linking with -lasound.
 * 
 * @param ctx Pointer to the NoiseContext.
 * @param sample_rate Audio sample rate (e.g., 44100).
 * @param channels Number of audio channels (e.g., 1 for mono, 2 for stereo).
 * @param duration_sec Duration to play in seconds.
 * @return 0 on success, negative error code on failure.
 */
int noise_play_speaker(NoiseContext* ctx, unsigned int sample_rate, unsigned int channels, unsigned int duration_sec) {
    if (!ctx) return -1;
    if (channels == 0) return -1;

    // Update sample rate in context for glitch calculations
    pthread_mutex_lock(&ctx->lock);
    ctx->sample_rate = sample_rate;
    ctx->channels = channels;
    ctx->channel_index = 0;
    pthread_mutex_unlock(&ctx->lock);

    snd_pcm_t *pcm_handle;
    int err;
    
    // Open the default device for playback
    if ((err = snd_pcm_open(&pcm_handle, "default", SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        return err;
    }

    // Set parameters: S16_LE, interleaved channels, specified rate
    // Latency set to 500000us (0.5s) to prevent underruns
    if ((err = snd_pcm_set_params(pcm_handle,
                                  SND_PCM_FORMAT_S16_LE,
                                  SND_PCM_ACCESS_RW_INTERLEAVED,
                                  channels,
                                  sample_rate,
                                  1,
                                  500000)) < 0) {
        snd_pcm_close(pcm_handle);
        return err;
    }

    const size_t chunk_size = 1024; // Buffer capacity in samples (not frames)
    int16_t buffer[chunk_size];
    float float_buffer[chunk_size];
    
    pthread_mutex_lock(&ctx->lock);
    float fade_in_sec = ctx->fade_in_sec;
    float fade_out_sec = ctx->fade_out_sec;
    pthread_mutex_unlock(&ctx->lock);

    unsigned long frames_played = 0;
    unsigned long total_frames = (unsigned long)sample_rate * duration_sec;
    unsigned long max_frames = total_frames;
    
    while (total_frames > 0) {
        pthread_mutex_lock(&ctx->lock);
        if (ctx->stop_flag) {
            pthread_mutex_unlock(&ctx->lock);
            break;
        }
        pthread_mutex_unlock(&ctx->lock);

        snd_pcm_uframes_t frames_capacity = chunk_size / channels;
        snd_pcm_uframes_t frames = (total_frames < frames_capacity) ? total_frames : frames_capacity;
        size_t samples_to_process = frames * channels;
        
        noise_process_buffer(ctx, float_buffer, samples_to_process);

        // Apply fade envelope
        for (unsigned int i = 0; i < frames; i++) {
            unsigned long current_frame = frames_played + i;
            float envelope = 1.0f;

            if (fade_in_sec > 0.0f) {
                float in_frames = fade_in_sec * sample_rate;
                if (current_frame < in_frames) envelope *= (float)current_frame / in_frames;
            }
            if (fade_out_sec > 0.0f) {
                float out_frames = fade_out_sec * sample_rate;
                if (current_frame >= max_frames - out_frames) envelope *= (float)(max_frames - current_frame) / out_frames;
            }

            for (unsigned int ch = 0; ch < channels; ch++) {
                float_buffer[i * channels + ch] *= envelope;
            }
        }

        for (unsigned int i = 0; i < samples_to_process; i++) {
            float s = float_buffer[i];
            if (s > 1.0f) s = 1.0f; else if (s < -1.0f) s = -1.0f;
            buffer[i] = (int16_t)(s * 32767.0f);
        }

        if ((err = snd_pcm_writei(pcm_handle, buffer, frames)) < 0) {
            if (snd_pcm_recover(pcm_handle, err, 0) < 0) break;
        }
        total_frames -= frames;
        frames_played += frames;
    }

    snd_pcm_drain(pcm_handle);
    snd_pcm_close(pcm_handle);
    return 0;
}

/* --- Lua Bindings --- */

#define METATABLE_NAME "NoiseContext"
#define SYNC_METATABLE_NAME "NoiseSync"

/* Forward compatibility: lua_newuserdata is deprecated in 5.4, likely removed in 5.5 */
#if LUA_VERSION_NUM >= 504
#define lua_newuserdata_compat(L, s) lua_newuserdatauv(L, s, 0)
#else
#define lua_newuserdata_compat(L, s) lua_newuserdata(L, s)
#endif

static int l_noise_new(lua_State *L) {
    float amplitude = (float)luaL_checknumber(L, 1);
    uint32_t seed = (uint32_t)luaL_checkinteger(L, 2);

    NoiseContext *ctx = (NoiseContext *)lua_newuserdata_compat(L, sizeof(NoiseContext));
    
    luaL_getmetatable(L, METATABLE_NAME);
    lua_setmetatable(L, -2);

    noise_init(ctx, amplitude, seed);
    return 1;
}

static int l_noise_set_amplitude(lua_State *L) {
    NoiseContext *ctx = (NoiseContext *)luaL_checkudata(L, 1, METATABLE_NAME);
    float amplitude = (float)luaL_checknumber(L, 2);
    noise_set_amplitude(ctx, amplitude);
    return 0;
}

static int l_noise_set_lpf_alpha(lua_State *L) {
    NoiseContext *ctx = (NoiseContext *)luaL_checkudata(L, 1, METATABLE_NAME);
    float alpha = (float)luaL_checknumber(L, 2);
    noise_set_lpf_alpha(ctx, alpha);
    return 0;
}

static int l_noise_set_fade(lua_State *L) {
    NoiseContext *ctx = (NoiseContext *)luaL_checkudata(L, 1, METATABLE_NAME);
    float in = (float)luaL_checknumber(L, 2);
    float out = (float)luaL_checknumber(L, 3);
    noise_set_fade(ctx, in, out);
    return 0;
}

static int l_noise_set_bit_depth(lua_State *L) {
    NoiseContext *ctx = (NoiseContext *)luaL_checkudata(L, 1, METATABLE_NAME);
    float bits = (float)luaL_checknumber(L, 2);
    noise_set_bit_depth(ctx, bits);
    return 0;
}

static int l_noise_set_decimation(lua_State *L) {
    NoiseContext *ctx = (NoiseContext *)luaL_checkudata(L, 1, METATABLE_NAME);
    float factor = (float)luaL_checknumber(L, 2);
    noise_set_decimation(ctx, factor);
    return 0;
}

static int l_noise_set_tempo(lua_State *L) {
    NoiseContext *ctx = (NoiseContext *)luaL_checkudata(L, 1, METATABLE_NAME);
    float tempo = (float)luaL_checknumber(L, 2);
    noise_set_tempo(ctx, tempo);
    return 0;
}

static int l_noise_glitch(lua_State *L) {
    NoiseContext *ctx = (NoiseContext *)luaL_checkudata(L, 1, METATABLE_NAME);
    float duration = (float)luaL_checknumber(L, 2);
    float start = (float)luaL_checknumber(L, 3);
    float end = (float)luaL_checknumber(L, 4);
    noise_glitch(ctx, duration, start, end);
    return 0;
}

static int l_noise_super_stutter(lua_State *L) {
    NoiseContext *ctx = (NoiseContext *)luaL_checkudata(L, 1, METATABLE_NAME);
    float duration = (float)luaL_checknumber(L, 2);
    float freq = (float)luaL_checknumber(L, 3);
    noise_super_stutter(ctx, duration, freq);
    return 0;
}

static int l_noise_set_auto_glitch(lua_State *L) {
    NoiseContext *ctx = (NoiseContext *)luaL_checkudata(L, 1, METATABLE_NAME);
    float freq = (float)luaL_checknumber(L, 2);
    noise_set_auto_glitch(ctx, freq);
    return 0;
}

static int l_noise_sequence(lua_State *L) {
    NoiseContext *ctx = (NoiseContext *)luaL_checkudata(L, 1, METATABLE_NAME);
    luaL_checktype(L, 2, LUA_TTABLE);

    // Check 'loop' field in the main table
    lua_getfield(L, 2, "loop");
    int loop = lua_toboolean(L, -1);
    lua_pop(L, 1);

    // Count integer keys to determine number of steps
    size_t count = lua_rawlen(L, 2);
    if (count == 0) return 0;

    // Allocate temporary buffer
    SeqStep* steps = malloc(sizeof(SeqStep) * count);
    if (!steps) return luaL_error(L, "Memory allocation failed");

    // Parse table
    for (size_t i = 0; i < count; i++) {
        lua_rawgeti(L, 2, (int)(i + 1)); // Lua is 1-based
        if (!lua_istable(L, -1)) {
            free(steps);
            return luaL_error(L, "Sequence index %d is not a table", (int)(i + 1));
        }

        lua_getfield(L, -1, "amp");
        steps[i].amp = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, -1, "start");
        steps[i].start = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, -1, "ends");
        steps[i].end = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, -1, "dur");
        steps[i].duration = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);

        // Parse Pan (Optional)
        float p_start = 0.0f;
        float p_end = 0.0f;
        lua_getfield(L, -1, "pan");
        if (!lua_isnil(L, -1)) {
            float p = (float)lua_tonumber(L, -1);
            p_start = p; p_end = p;
        }
        lua_pop(L, 1);
        lua_getfield(L, -1, "pan_start");
        if (!lua_isnil(L, -1)) p_start = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "pan_end");
        if (!lua_isnil(L, -1)) p_end = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
        steps[i].pan_start = p_start;
        steps[i].pan_end = p_end;

        // Parse Probability (Optional, default 1.0)
        lua_getfield(L, -1, "prob");
        if (lua_isnil(L, -1)) steps[i].probability = 1.0f;
        else steps[i].probability = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);

        // Parse Stutter (Optional, default 0.0 off)
        lua_getfield(L, -1, "stutter");
        if (lua_isnil(L, -1)) steps[i].stutter_freq = 0.0f;
        else steps[i].stutter_freq = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);

        lua_pop(L, 1); // Pop the step table
    }

    pthread_mutex_lock(&ctx->lock);
    if (ctx->seq_steps) free(ctx->seq_steps);
    ctx->seq_steps = steps;
    ctx->seq_count = count;
    ctx->seq_index = 0;
    ctx->seq_progress = 0;
    ctx->seq_active = 1;
    ctx->seq_loop = loop;
    // Initialize first step state immediately
    if (count > 0) {
        ctx->decimate_factor = steps[0].start;
        ctx->amplitude = steps[0].amp;
    }
    pthread_mutex_unlock(&ctx->lock);

    return 0;
}

static int l_noise_sequence_string(lua_State *L) {
    NoiseContext *ctx = (NoiseContext *)luaL_checkudata(L, 1, METATABLE_NAME);
    const char *str = luaL_checkstring(L, 2);
    float base_dur = (float)luaL_optnumber(L, 3, 0.1);
    int loop = 1;
    if (lua_gettop(L) >= 4) {
        loop = lua_toboolean(L, 4);
    }
    
    size_t len = strlen(str);
    if (len == 0) return 0;
    
    SeqStep* steps = malloc(sizeof(SeqStep) * len);
    if (!steps) return luaL_error(L, "Memory allocation failed");
    
    float speed = 1.0f;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)str[i];
        
        // Defaults
        steps[i].duration = base_dur / speed;
        steps[i].amp = 0.0f;
        steps[i].start = 1.0f;
        steps[i].end = 1.0f;
        steps[i].pan_start = 0.0f;
        steps[i].pan_end = 0.0f;
        steps[i].probability = 1.0f;
        steps[i].stutter_freq = 0.0f;
        
        if (isspace(c) || c == '_') {
            steps[i].amp = 0.0f;
        } else if (isdigit(c)) {
            float val = (float)(c - '0');
            steps[i].amp = 0.8f;
            steps[i].start = 1.0f + (val * 5.0f);
            steps[i].end = steps[i].start;
            steps[i].duration = (base_dur / 2.0f) / speed;
        } else if (isupper(c)) {
            float val = (float)(c - 'A');
            steps[i].amp = 0.9f;
            steps[i].start = (val * 3.0f) + 10.0f;
            steps[i].end = steps[i].start;
        } else if (islower(c)) {
            float val = (float)(c - 'a');
            steps[i].amp = 0.6f;
            steps[i].start = val + 2.0f;
            steps[i].end = steps[i].start + 20.0f;
        } else {
            switch(c) {
                case '.': steps[i].amp = 0.0f; steps[i].duration = (base_dur / 4.0f) / speed; break;
                case '-': steps[i].amp = 0.0f; steps[i].duration = base_dur / speed; break;
                case '!': steps[i].amp = 0.9f; steps[i].start = 10.0f; steps[i].end = 10.0f; steps[i].stutter_freq = 60.0f; break;
                case '*': steps[i].amp = 0.8f; steps[i].start = 1.0f; steps[i].end = 100.0f; break;
                case '?': steps[i].amp = 0.8f; steps[i].start = 20.0f; steps[i].end = 20.0f; steps[i].probability = 0.5f; break;
                case '<': steps[i].amp = 0.8f; steps[i].pan_start = -1.0f; steps[i].pan_end = -1.0f; steps[i].start = 5.0f; steps[i].end = 5.0f; break;
                case '>': steps[i].amp = 0.8f; steps[i].pan_start = 1.0f; steps[i].pan_end = 1.0f; steps[i].start = 5.0f; steps[i].end = 5.0f; break;
                case '^': steps[i].amp = 0.8f; steps[i].pan_start = 0.0f; steps[i].pan_end = 0.0f; steps[i].start = 5.0f; steps[i].end = 5.0f; break;
                case '+': speed *= 2.0f; steps[i].duration = 0.0f; steps[i].amp = 0.0f; break;
                case '/': speed *= 0.5f; steps[i].duration = 0.0f; steps[i].amp = 0.0f; break;
                default: steps[i].amp = 0.0f; break;
            }
        }
    }
    
    pthread_mutex_lock(&ctx->lock);
    if (ctx->seq_steps) free(ctx->seq_steps);
    ctx->seq_steps = steps;
    ctx->seq_count = len;
    ctx->seq_index = 0;
    ctx->seq_progress = 0;
    ctx->seq_active = 1;
    ctx->seq_loop = loop;
    if (len > 0) {
        ctx->decimate_factor = steps[0].start;
        ctx->amplitude = steps[0].amp;
    }
    pthread_mutex_unlock(&ctx->lock);
    
    return 0;
}

static int l_noise_tick(lua_State *L) {
    NoiseContext *ctx = (NoiseContext *)luaL_checkudata(L, 1, METATABLE_NAME);
    lua_pushnumber(L, (lua_Number)noise_tick(ctx));
    return 1;
}

static int l_noise_get_buffer(lua_State *L) {
    NoiseContext *ctx = (NoiseContext *)luaL_checkudata(L, 1, METATABLE_NAME);
    size_t samples = (size_t)luaL_checkinteger(L, 2);

    float *temp = malloc(samples * sizeof(float));
    if (!temp) return luaL_error(L, "Memory allocation failed");

    noise_process_buffer(ctx, temp, samples);

    lua_createtable(L, (int)samples, 0);
    for (size_t i = 0; i < samples; i++) {
        lua_pushnumber(L, (lua_Number)temp[i]);
        lua_rawseti(L, -2, (int)(i + 1));
    }

    free(temp);
    return 1;
}

static int l_noise_play(lua_State *L) {
    NoiseContext *ctx = (NoiseContext *)luaL_checkudata(L, 1, METATABLE_NAME);
    unsigned int rate = (unsigned int)luaL_checkinteger(L, 2);
    unsigned int channels = (unsigned int)luaL_checkinteger(L, 3);
    unsigned int duration = (unsigned int)luaL_checkinteger(L, 4);

    pthread_mutex_lock(&ctx->lock);
    ctx->stop_flag = 0;
    pthread_mutex_unlock(&ctx->lock);

    int err = noise_play_speaker(ctx, rate, channels, duration);
    if (err < 0) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, snd_strerror(err));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_noise_create_sync(lua_State *L) {
    unsigned int count = (unsigned int)luaL_checkinteger(L, 1);
    
    NoiseSync *sync = (NoiseSync *)lua_newuserdata_compat(L, sizeof(NoiseSync));
    luaL_getmetatable(L, SYNC_METATABLE_NAME);
    lua_setmetatable(L, -2);

    pthread_barrier_init(&sync->barrier, NULL, count);
    return 1;
}

static int l_noise_sync_gc(lua_State *L) {
    NoiseSync *sync = (NoiseSync *)luaL_checkudata(L, 1, SYNC_METATABLE_NAME);
    pthread_barrier_destroy(&sync->barrier);
    return 0;
}

typedef struct {
    NoiseContext* ctx;
    unsigned int sample_rate;
    unsigned int channels;
    unsigned int duration;
    NoiseSync* sync;
} AsyncPlayArgs;

static void* _noise_play_thread(void* arg) {
    AsyncPlayArgs* args = (AsyncPlayArgs*)arg;
    if (args->sync) {
        pthread_barrier_wait(&args->sync->barrier);
    }
    noise_play_speaker(args->ctx, args->sample_rate, args->channels, args->duration);
    free(args);
    return NULL;
}

static int l_noise_play_async(lua_State *L) {
    NoiseContext *ctx = (NoiseContext *)luaL_checkudata(L, 1, METATABLE_NAME);
    unsigned int rate = (unsigned int)luaL_checkinteger(L, 2);
    unsigned int channels = (unsigned int)luaL_checkinteger(L, 3);
    unsigned int duration = (unsigned int)luaL_checkinteger(L, 4);

    NoiseSync* sync = NULL;
    if (lua_gettop(L) >= 5) {
        sync = (NoiseSync *)luaL_checkudata(L, 5, SYNC_METATABLE_NAME);
    }

    AsyncPlayArgs* args = malloc(sizeof(AsyncPlayArgs));
    if (!args) return luaL_error(L, "Memory allocation failed");
    
    args->ctx = ctx;
    args->sample_rate = rate;
    args->channels = channels;
    args->duration = duration;
    args->sync = sync;

    pthread_mutex_lock(&ctx->lock);
    ctx->stop_flag = 0;
    pthread_mutex_unlock(&ctx->lock);

    pthread_t thread_id;
    if (pthread_create(&thread_id, NULL, _noise_play_thread, args) != 0) {
        free(args);
        return luaL_error(L, "Failed to create thread");
    }
    pthread_detach(thread_id); // Detach so resources are freed on exit
    return 0;
}

static int l_noise_stop(lua_State *L) {
    NoiseContext *ctx = (NoiseContext *)luaL_checkudata(L, 1, METATABLE_NAME);
    noise_stop(ctx);
    return 0;
}

static int l_noise_gc(lua_State *L) {
    NoiseContext *ctx = (NoiseContext *)luaL_checkudata(L, 1, METATABLE_NAME);
    pthread_mutex_destroy(&ctx->lock);
    if (ctx->seq_steps) free(ctx->seq_steps);
    return 0;
}

static const struct luaL_Reg noise_methods[] = {
    {"set_amplitude", l_noise_set_amplitude},
    {"set_lpf_alpha", l_noise_set_lpf_alpha},
    {"set_fade", l_noise_set_fade},
    {"set_bit_depth", l_noise_set_bit_depth},
    {"set_decimation", l_noise_set_decimation},
    {"set_tempo", l_noise_set_tempo},
    {"glitch", l_noise_glitch},
    {"set_auto_glitch", l_noise_set_auto_glitch},
    {"super_stutter", l_noise_super_stutter},
    {"sequence", l_noise_sequence},
    {"sequence_string", l_noise_sequence_string},
    {"tick", l_noise_tick},
    {"get_buffer", l_noise_get_buffer},
    {"play", l_noise_play},
    {"play_async", l_noise_play_async},
    {"stop", l_noise_stop},
    {"__gc", l_noise_gc},
    {NULL, NULL}
};

static const struct luaL_Reg noise_lib[] = {
    {"new", l_noise_new},
    {"create_sync", l_noise_create_sync},
    {NULL, NULL}
};

int luaopen_noiseintent(lua_State *L) {
    luaL_newmetatable(L, METATABLE_NAME);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, noise_methods, 0);

    luaL_newmetatable(L, SYNC_METATABLE_NAME);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, l_noise_sync_gc);
    lua_setfield(L, -2, "__gc");
    
    luaL_newlib(L, noise_lib);
    return 1;
}