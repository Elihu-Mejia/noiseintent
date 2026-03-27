#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <math.h>
#include <AudioToolbox/AudioToolbox.h>
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
    float robot_freq; // Robotic modulation frequency (Hz)
    float robot_depth; // Robotic modulation depth (0.0-1.0)
    unsigned long robot_samples_done; // Global sample counter for modulation
    // Delay effect
    float* delay_buffer;
    size_t delay_buffer_len;
    size_t delay_write_idx;
    float delay_time_sec;
    float delay_feedback;
    float delay_mix;
} NoiseContext;

/* MacOS Barrier Implementation */
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    unsigned int count;
    unsigned int initial_count;
    unsigned int generation;
} barrier_t;

static int barrier_init(barrier_t *b, unsigned int count) {
    if (pthread_mutex_init(&b->mutex, NULL) != 0) return -1;
    if (pthread_cond_init(&b->cond, NULL) != 0) {
        pthread_mutex_destroy(&b->mutex);
        return -1;
    }
    b->count = count;
    b->initial_count = count;
    b->generation = 0;
    return 0;
}

static int barrier_destroy(barrier_t *b) {
    pthread_mutex_destroy(&b->mutex);
    pthread_cond_destroy(&b->cond);
    return 0;
}

static int barrier_wait(barrier_t *b) {
    pthread_mutex_lock(&b->mutex);
    unsigned int gen = b->generation;
    b->count--;
    if (b->count == 0) {
        b->generation++;
        b->count = b->initial_count;
        pthread_cond_broadcast(&b->cond);
        pthread_mutex_unlock(&b->mutex);
        return 1;
    }
    while (gen == b->generation) {
        pthread_cond_wait(&b->cond, &b->mutex);
    }
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

typedef struct {
    barrier_t barrier;
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
    ctx->robot_freq = 0.0f;
    ctx->robot_depth = 0.0f;
    ctx->robot_samples_done = 0;
    ctx->delay_time_sec = 0.0f;
    ctx->delay_feedback = 0.0f;
    ctx->delay_mix = 0.0f;
    ctx->delay_buffer_len = 44100 * 2 * 2; // 2 seconds at 44.1kHz stereo
    ctx->delay_buffer = (float*)calloc(ctx->delay_buffer_len, sizeof(float));
    ctx->delay_write_idx = 0;
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
 * Set global robotic modulation (envelope pulse).
 * 
 * @param ctx Pointer to the NoiseContext structure.
 * @param freq Modulation frequency in Hz (e.g., 40.0Hz). 0.0 to disable.
 * @param depth Modulation depth (0.0 to 1.0). 1.0 is full silence on off-cycle.
 */
void noise_set_robot(NoiseContext* ctx, float freq, float depth) {
    if (!ctx) return;
    pthread_mutex_lock(&ctx->lock);
    ctx->robot_freq = (freq < 0.0f) ? 0.0f : freq;
    ctx->robot_depth = (depth < 0.0f) ? 0.0f : (depth > 1.0f ? 1.0f : depth);
    pthread_mutex_unlock(&ctx->lock);
}

/**
 * Set global delay effect parameters.
 * 
 * @param ctx Pointer to context.
 * @param time_sec Delay time in seconds (max 2.0).
 * @param feedback Feedback amount (0.0 to 0.95 recommended).
 * @param mix Dry/Wet mix (0.0 to 1.0).
 */
void noise_set_delay(NoiseContext* ctx, float time_sec, float feedback, float mix) {
    if (!ctx) return;
    pthread_mutex_lock(&ctx->lock);
    ctx->delay_time_sec = (time_sec < 0.0f) ? 0.0f : (time_sec > 2.0f ? 2.0f : time_sec);
    ctx->delay_feedback = (feedback < 0.0f) ? 0.0f : (feedback > 0.99f ? 0.99f : feedback);
    ctx->delay_mix = (mix < 0.0f) ? 0.0f : (mix > 1.0f ? 1.0f : mix);
    pthread_mutex_unlock(&ctx->lock);
}

/**
 * Internal unsafe tick function (must hold lock).
 * 
 * @param ctx Pointer to the NoiseContext structure.
 * @return A float sample in the range [-amplitude, amplitude].
 */
static float _noise_tick_unsafe(NoiseContext* ctx) {
    // Handle Global Robot Modulation
    float robot_mod = 1.0f;
    float current_alpha = ctx->alpha;
    if (ctx->robot_freq > 0.0f) {
        // Increment counter once per frame (on the first channel)
        if (ctx->channel_index == 0) {
            ctx->robot_samples_done++;
        }
        
        float period = (float)ctx->sample_rate / ctx->robot_freq;
        if (period > 0.0f) {
            // Square wave envelope
            if (fmodf((float)ctx->robot_samples_done, period) < period * 0.5f) {
                robot_mod = 1.0f - ctx->robot_depth;
                current_alpha = ctx->alpha * (1.0f - ctx->robot_depth);
            }
        }
    }

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

    float out_sample = 0.0f;
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
            ctx->last_sample += current_alpha * (raw - ctx->last_sample);
            ctx->held_sample = ctx->last_sample;
        }
        
        // Advance channel index
        ctx->channel_index++;
        if (ctx->channel_index >= ctx->channels) ctx->channel_index = 0;
        
        out_sample = ctx->held_sample * ctx->amplitude * pan_gain * robot_mod;
    } else {
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
        ctx->last_sample += current_alpha * (raw - ctx->last_sample);

        ctx->held_sample = ctx->last_sample;
    }

    // Advance channel index for non-sequencer playback too (to keep state consistent if mixed)
    ctx->channel_index++;
    if (ctx->channel_index >= ctx->channels) ctx->channel_index = 0;
        out_sample = ctx->held_sample * ctx->amplitude * robot_mod;
    }

    // Apply Delay Effect
    if (ctx->delay_buffer && ctx->delay_time_sec > 0.0f) {
        unsigned int delay_samples = (unsigned int)(ctx->delay_time_sec * (float)ctx->sample_rate);
        if (delay_samples > 0) {
            unsigned int ch_count = (ctx->channels > 0) ? ctx->channels : 1;
            size_t offset = (size_t)delay_samples * ch_count;
            
            // Circular read
            size_t read_idx = (ctx->delay_write_idx + ctx->delay_buffer_len - offset) % ctx->delay_buffer_len;
            float delayed = ctx->delay_buffer[read_idx];
            
            // Circular write with feedback
            ctx->delay_buffer[ctx->delay_write_idx] = out_sample + (delayed * ctx->delay_feedback);
            ctx->delay_write_idx = (ctx->delay_write_idx + 1) % ctx->delay_buffer_len;
            
            // Mix
            out_sample = (out_sample * (1.0f - ctx->delay_mix)) + (delayed * ctx->delay_mix);
        }
    }

    return out_sample;
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

typedef struct {
    NoiseContext* ctx;
    unsigned long frames_played;
    unsigned long total_frames;
    unsigned long max_frames;
    float fade_in_sec;
    float fade_out_sec;
    unsigned int sample_rate;
    unsigned int channels;
    int done;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} AudioCallbackData;

static void OutputCallback(void *inUserData, AudioQueueRef inAQ, AudioQueueBufferRef inBuffer) {
    AudioCallbackData *data = (AudioCallbackData *)inUserData;
    
    pthread_mutex_lock(&data->mutex);
    if (data->done) {
        pthread_mutex_unlock(&data->mutex);
        return;
    }
    
    // Check stop flag
    pthread_mutex_lock(&data->ctx->lock);
    int stop = data->ctx->stop_flag;
    pthread_mutex_unlock(&data->ctx->lock);
    
    if (stop || data->frames_played >= data->total_frames) {
        data->done = 1;
        pthread_cond_signal(&data->cond);
        pthread_mutex_unlock(&data->mutex);
        
        // Enqueue silence
        memset(inBuffer->mAudioData, 0, inBuffer->mAudioDataBytesCapacity);
        inBuffer->mAudioDataByteSize = inBuffer->mAudioDataBytesCapacity;
        AudioQueueEnqueueBuffer(inAQ, inBuffer, 0, NULL);
        return; 
    }

    unsigned int frames_cap = inBuffer->mAudioDataBytesCapacity / (sizeof(int16_t) * data->channels);
    unsigned long remaining = data->total_frames - data->frames_played;
    unsigned int frames_to_write = (remaining < frames_cap) ? (unsigned int)remaining : frames_cap;
    
    size_t samples_to_process = frames_to_write * data->channels;
    
    // Temporary float buffer
    float float_buffer[samples_to_process];
    
    noise_process_buffer(data->ctx, float_buffer, samples_to_process);
    
    int16_t* out_ptr = (int16_t*)inBuffer->mAudioData;
    
    // Fade logic
    unsigned long current_frame_base = data->frames_played;
    
    for (unsigned int i = 0; i < frames_to_write; i++) {
        unsigned long current_frame = current_frame_base + i;
        float envelope = 1.0f;
        
        if (data->fade_in_sec > 0.0f) {
            float in_frames = data->fade_in_sec * data->sample_rate;
            if (current_frame < in_frames) envelope *= (float)current_frame / in_frames;
        }
        if (data->fade_out_sec > 0.0f) {
            float out_frames = data->fade_out_sec * data->sample_rate;
            if (current_frame >= data->max_frames - out_frames) envelope *= (float)(data->max_frames - current_frame) / out_frames;
        }
        
        for (unsigned int ch = 0; ch < data->channels; ch++) {
            float s = float_buffer[i * data->channels + ch];
            if (s > 1.0f) s = 1.0f; else if (s < -1.0f) s = -1.0f;
            out_ptr[i * data->channels + ch] = (int16_t)(s * 32767.0f);
        }
    }
    
    inBuffer->mAudioDataByteSize = frames_to_write * data->channels * sizeof(int16_t);
    data->frames_played += frames_to_write;
    
    AudioQueueEnqueueBuffer(inAQ, inBuffer, 0, NULL);
    
    if (data->frames_played >= data->total_frames) {
        data->done = 1;
        pthread_cond_signal(&data->cond);
    }
    
    pthread_mutex_unlock(&data->mutex);
}

/**
 * Plays the noise through the default CoreAudio device.
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

    AudioStreamBasicDescription format;
    memset(&format, 0, sizeof(format));
    format.mSampleRate = (Float64)sample_rate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    format.mFramesPerPacket = 1;
    format.mChannelsPerFrame = channels;
    format.mBitsPerChannel = 16;
    format.mBytesPerFrame = channels * 2;
    format.mBytesPerPacket = channels * 2;

    AudioCallbackData data;
    data.ctx = ctx;
    data.frames_played = 0;
    data.total_frames = (unsigned long)sample_rate * duration_sec;
    data.max_frames = data.total_frames;
    data.sample_rate = sample_rate;
    data.channels = channels;
    data.done = 0;
    pthread_mutex_init(&data.mutex, NULL);
    pthread_cond_init(&data.cond, NULL);

    pthread_mutex_lock(&ctx->lock);
    data.fade_in_sec = ctx->fade_in_sec;
    data.fade_out_sec = ctx->fade_out_sec;
    pthread_mutex_unlock(&ctx->lock);

    AudioQueueRef queue;
    if (AudioQueueNewOutput(&format, OutputCallback, &data, NULL, NULL, 0, &queue) != noErr) {
        return -1;
    }

    // Allocate and prime buffers
    int buffer_byte_size = 1024 * channels * 2; 
    for (int i = 0; i < 3; ++i) {
        AudioQueueBufferRef buffer;
        AudioQueueAllocateBuffer(queue, buffer_byte_size, &buffer);
        OutputCallback(&data, queue, buffer);
    }

    AudioQueueStart(queue, NULL);

    // Wait for completion (buffers enqueued)
    pthread_mutex_lock(&data.mutex);
    while (!data.done) {
        pthread_cond_wait(&data.cond, &data.mutex);
    }
    pthread_mutex_unlock(&data.mutex);

    Boolean immediate = (ctx->stop_flag) ? true : false;
    AudioQueueStop(queue, immediate);
    
    UInt32 isRunning = 1;
    UInt32 size = sizeof(isRunning);
    while (isRunning) {
        AudioQueueGetProperty(queue, kAudioQueueProperty_IsRunning, &isRunning, &size);
        if (isRunning) usleep(10000); 
    }

    AudioQueueDispose(queue, true);
    pthread_mutex_destroy(&data.mutex);
    pthread_cond_destroy(&data.cond);

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

static int l_noise_set_robot(lua_State *L) {
    NoiseContext *ctx = (NoiseContext *)luaL_checkudata(L, 1, METATABLE_NAME);
    float freq = (float)luaL_checknumber(L, 2);
    float depth = (float)luaL_optnumber(L, 3, 1.0);
    noise_set_robot(ctx, freq, depth);
    return 0;
}

static int l_noise_set_delay(lua_State *L) {
    NoiseContext *ctx = (NoiseContext *)luaL_checkudata(L, 1, METATABLE_NAME);
    float time = (float)luaL_checknumber(L, 2);
    float feedback = (float)luaL_optnumber(L, 3, 0.3);
    float mix = (float)luaL_optnumber(L, 4, 0.4);
    noise_set_delay(ctx, time, feedback, mix);
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
        lua_pushstring(L, "Audio Error");
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

    barrier_init(&sync->barrier, count);
    return 1;
}

static int l_noise_sync_gc(lua_State *L) {
    NoiseSync *sync = (NoiseSync *)luaL_checkudata(L, 1, SYNC_METATABLE_NAME);
    barrier_destroy(&sync->barrier);
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
        barrier_wait(&args->sync->barrier);
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
    if (ctx->delay_buffer) free(ctx->delay_buffer);
    if (ctx->seq_steps) free(ctx->seq_steps);
    return 0;
}

static const struct luaL_Reg noise_methods[] = {
    {"set_amplitude", l_noise_set_amplitude},
    {"set_lpf_alpha", l_noise_set_lpf_alpha},
    {"set_fade", l_noise_set_fade},
    {"set_decimation", l_noise_set_decimation},
    {"set_tempo", l_noise_set_tempo},
    {"glitch", l_noise_glitch},
    {"set_delay", l_noise_set_delay},
    {"set_robot", l_noise_set_robot},
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