#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include <stdbool.h>

void init_audio(void);
void play_mp3_file(const char *filename);
void set_volume(int volume);
int get_volume(void);
void increase_volume(void);
void decrease_volume(void);
void save_volume_to_nvs(int volume);
void load_volume_from_nvs(void);
void set_audio_enabled(bool enabled);
bool is_audio_enabled(void);
void save_audio_enabled_to_nvs(bool enabled);
void load_audio_enabled_from_nvs(void);

#endif
