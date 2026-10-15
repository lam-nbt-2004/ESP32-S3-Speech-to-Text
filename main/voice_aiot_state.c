/*
 * Module: shared firmware state definitions
 *
 * Global handles and cross-task state are defined in exactly one translation
 * unit and declared extern in voice_aiot_internal.h. Zero-initialization is
 * provided by C; app_main() initializes resources before tasks use them.
 */
#include "voice_aiot_internal.h"

EventGroupHandle_t s_wifi_event_group;
i2s_chan_handle_t rx_chan;
i2s_chan_handle_t tx_chan;
QueueHandle_t playback_ref_queue;
ref_stats_t ref_stats = { .final_frames = -1 };
SemaphoreHandle_t speaker_playback_mutex;
int afe_feed_chunk_samples;
const esp_afe_sr_iface_t *afe_handle;
esp_afe_sr_data_t *afe_data;
srmodel_list_t *sr_models;
int16_t *record_buffers[AUDIO_BUFFER_COUNT];
int16_t *record_buffer;
int active_record_buffer_index = -1;
QueueHandle_t audio_queue;
QueueHandle_t free_buffer_queue;
volatile bool is_recording;
size_t recorded_samples;
int silence_frames;
TickType_t command_start_tick;
bool command_speech_started;
bool post_wake_silence_seen;
bool skip_until_silence;
