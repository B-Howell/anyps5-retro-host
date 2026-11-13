/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors */

/* A libretro core for the host's tests. It draws 640x480 frames, plays silence, offers one
 * option, takes .m3u playlists through the disc control interface, keeps a 128 KiB memory card
 * and rumbles port 1's large motor while Cross is held. It logs SwanStation's messages for a BIOS
 * it uses (fake-bios.bin in the system directory) or for none. Setting fake_hang to "yes" makes retro_run
 * never return, as a stuck core would. */

#include <libretro.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH 640
#define HEIGHT 480
#define CARD_BYTES (128 * 1024)
#define MAX_DISCS 8

static retro_environment_t environment;
static retro_video_refresh_t video;
static retro_audio_sample_batch_t audio_batch;
static retro_input_poll_t input_poll;
static retro_input_state_t input_state;
static retro_log_printf_t log_printf;
static struct retro_rumble_interface rumble;
static unsigned frame_count;
static unsigned rumbling;
static uint32_t pixels[WIDTH * HEIGHT];
static int16_t silence[2 * 735];
static unsigned char card[CARD_BYTES];
static char disc_paths[MAX_DISCS][2100];
static unsigned disc_count;
static unsigned disc_index;
static unsigned initial_disc;
static bool tray_open;

static bool set_eject_state(bool open) {
  tray_open = open;
  return true;
}
static bool get_eject_state(void) { return tray_open; }
static unsigned get_image_index(void) { return disc_index; }
static bool set_image_index(unsigned index) {
  if (!tray_open || index >= disc_count) return false;
  disc_index = index;
  return true;
}
static unsigned get_num_images(void) { return disc_count; }
static bool replace_image_index(unsigned index, const struct retro_game_info* info) {
  (void)index;
  (void)info;
  return false;
}
static bool add_image_index(void) { return false; }
static bool set_initial_image(unsigned index, const char* path) {
  (void)path;
  initial_disc = index;
  return true;
}
static bool get_image_path(unsigned index, char* path, size_t size) {
  if (index >= disc_count) return false;
  snprintf(path, size, "%s", disc_paths[index]);
  return true;
}
static bool get_image_label(unsigned index, char* label, size_t size) {
  if (index >= disc_count) return false;
  snprintf(label, size, "Disc %u", index + 1);
  return true;
}

static struct retro_disk_control_ext_callback disc_control = {
    set_eject_state, get_eject_state, get_image_index, set_image_index, get_num_images,
    replace_image_index, add_image_index, set_initial_image, get_image_path, get_image_label};

static struct retro_core_option_v2_definition definitions[] = {
    {"fake_color",
     "Color",
     NULL,
     "The color of the test frame.",
     NULL,
     NULL,
     {{"blue", "Blue"}, {"green", "Green"}, {NULL, NULL}},
     "blue"},
    {"fake_region",
     "Region (Restart)",
     NULL,
     "Applied when the game starts again.",
     NULL,
     NULL,
     {{"ntsc", "NTSC"}, {"pal", "PAL"}, {NULL, NULL}},
     "ntsc"},
    {"fake_hang",
     "Hang",
     NULL,
     "Makes retro_run never return.",
     NULL,
     NULL,
     {{"no", "No"}, {"yes", "Yes"}, {NULL, NULL}},
     "no"},
    {NULL, NULL, NULL, NULL, NULL, NULL, {{NULL, NULL}}, NULL},
};
static struct retro_core_options_v2 options = {NULL, definitions};

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }

RETRO_API void retro_set_environment(retro_environment_t callback) { environment = callback; }
RETRO_API void retro_set_video_refresh(retro_video_refresh_t callback) { video = callback; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t callback) { (void)callback; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t callback) { audio_batch = callback; }
RETRO_API void retro_set_input_poll(retro_input_poll_t callback) { input_poll = callback; }
RETRO_API void retro_set_input_state(retro_input_state_t callback) { input_state = callback; }

RETRO_API void retro_init(void) {
  struct retro_log_callback log;
  unsigned version = 0;
  log_printf = environment(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log) ? log.log : NULL;
  if (environment(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) && version >= 2) {
    environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options);
  }
  environment(RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE, &disc_control);
  if (!environment(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble)) rumble.set_rumble_state = NULL;
  frame_count = 0;
  rumbling = 0;
  disc_count = 0;
  disc_index = 0;
  initial_disc = 0;
  tray_open = false;
  memset(card, 0, sizeof card);
}

RETRO_API void retro_deinit(void) {}

RETRO_API void retro_get_system_info(struct retro_system_info* info) {
  memset(info, 0, sizeof *info);
  info->library_name = "fake";
  info->library_version = "1";
  info->valid_extensions = "cue|chd|pbp|m3u";
  info->need_fullpath = true;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info* info) {
  memset(info, 0, sizeof *info);
  info->geometry.base_width = WIDTH;
  info->geometry.base_height = HEIGHT;
  info->geometry.max_width = WIDTH;
  info->geometry.max_height = HEIGHT;
  info->geometry.aspect_ratio = 4.0f / 3.0f;
  info->timing.fps = 60.0;
  info->timing.sample_rate = 44100.0;
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) {
  (void)port;
  (void)device;
}

RETRO_API void retro_reset(void) { frame_count = 0; }

static bool has_bios(void) {
  const char* system = NULL;
  char path[1100];
  FILE* file;
  if (!environment(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system) || !system) return false;
  snprintf(path, sizeof path, "%s/fake-bios.bin", system);
  file = fopen(path, "rb");
  if (!file) return false;
  fclose(file);
  return true;
}

static bool ends_with(const char* text, const char* suffix) {
  size_t a = strlen(text), b = strlen(suffix), i;
  if (a < b) return false;
  for (i = 0; i < b; ++i) {
    char c = text[a - b + i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (c != suffix[i]) return false;
  }
  return true;
}

static void load_playlist(const char* path) {
  char line[1024];
  char directory[1024];
  const char* slash = strrchr(path, '/');
  const char* backslash = strrchr(path, '\\');
  FILE* file = fopen(path, "rb");
  if (backslash && (!slash || backslash > slash)) slash = backslash;
  snprintf(directory, sizeof directory, "%.*s", slash ? (int)(slash - path + 1) : 0, path);
  if (!file) return;
  while (disc_count < MAX_DISCS && fgets(line, sizeof line, file)) {
    size_t length = strlen(line);
    while (length && (line[length - 1] == '\n' || line[length - 1] == '\r')) line[--length] = '\0';
    if (!length || line[0] == '#') continue;
    if (line[0] == '/' || line[0] == '\\' || (length > 2 && line[1] == ':')) {
      snprintf(disc_paths[disc_count++], sizeof disc_paths[0], "%s", line);
    } else {
      snprintf(disc_paths[disc_count++], sizeof disc_paths[0], "%s%s", directory, line);
    }
  }
  fclose(file);
}

RETRO_API bool retro_load_game(const struct retro_game_info* game) {
  enum retro_pixel_format format = RETRO_PIXEL_FORMAT_XRGB8888;
  if (!game || !game->path) return false;
  if (!environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format)) return false;
  if (log_printf) {
    if (has_bios()) log_printf(RETRO_LOG_INFO, "Using BIOS 'fake-bios.bin': fake BIOS\n");
    else log_printf(RETRO_LOG_INFO, "No BIOS image found for NTSC-U/C region\n");
  }
  if (ends_with(game->path, ".m3u")) {
    load_playlist(game->path);
    if (disc_count == 0) return false;
    disc_index = initial_disc < disc_count ? initial_disc : 0;
  } else {
    snprintf(disc_paths[0], sizeof disc_paths[0], "%s", game->path);
    disc_count = 1;
  }
  return true;
}

RETRO_API bool retro_load_game_special(unsigned type, const struct retro_game_info* info, size_t count) {
  (void)type;
  (void)info;
  (void)count;
  return false;
}

RETRO_API void retro_unload_game(void) {}

RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

RETRO_API void retro_run(void) {
  struct retro_variable color = {"fake_color", NULL};
  struct retro_variable hang = {"fake_hang", NULL};
  struct retro_variable region = {"fake_region", NULL};
  uint32_t fill;
  unsigned i;
  bool cross;
  input_poll();
  if (environment(RETRO_ENVIRONMENT_GET_VARIABLE, &hang) && hang.value && strcmp(hang.value, "yes") == 0) {
    for (;;) {
#ifdef _WIN32
      Sleep(1000);
#else
      sleep(1);
#endif
    }
  }
  environment(RETRO_ENVIRONMENT_GET_VARIABLE, &color);
  environment(RETRO_ENVIRONMENT_GET_VARIABLE, &region);
  fill = color.value && strcmp(color.value, "green") == 0 ? 0x0000C000u : 0x000000C0u;
  for (i = 0; i < WIDTH * HEIGHT; ++i) pixels[i] = fill + (frame_count & 0xFF) * 0x10000u;
  video(pixels, WIDTH, HEIGHT, WIDTH * sizeof(uint32_t));
  audio_batch(silence, 735);
  cross = input_state(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B) != 0;
  if (rumble.set_rumble_state && cross != (rumbling != 0)) {
    rumbling = cross ? 1 : 0;
    rumble.set_rumble_state(0, RETRO_RUMBLE_STRONG, cross ? 0xFFFF : 0);
  }
  card[0] = (unsigned char)(frame_count >= 30 ? 1 : 0);
  ++frame_count;
}

RETRO_API size_t retro_serialize_size(void) { return 0; }
RETRO_API bool retro_serialize(void* data, size_t size) {
  (void)data;
  (void)size;
  return false;
}
RETRO_API bool retro_unserialize(const void* data, size_t size) {
  (void)data;
  (void)size;
  return false;
}
RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char* code) {
  (void)index;
  (void)enabled;
  (void)code;
}
RETRO_API void* retro_get_memory_data(unsigned id) { return id == RETRO_MEMORY_SAVE_RAM ? card : NULL; }
RETRO_API size_t retro_get_memory_size(unsigned id) { return id == RETRO_MEMORY_SAVE_RAM ? sizeof card : 0; }
