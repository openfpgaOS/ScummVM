/*
 * config.h -- ScummVM build configuration for openfpgaOS
 *
 * This replaces the auto-generated config.h from ScummVM's configure script.
 * Included when HAVE_CONFIG_H is defined.
 */

#ifndef OPENFPGA_CONFIG_H
#define OPENFPGA_CONFIG_H

/* Platform */
#define SCUMM_LITTLE_ENDIAN
#define SCUMM_NEED_ALIGNMENT

/* Enabled engines */
#define ENABLE_SCUMM 1
/* Do NOT define ENABLE_SCUMM_7_8 — we only want v0-v6 */
/* Do NOT define ENABLE_HE — no Humongous Entertainment games */
#define ENABLE_SCI 1
#define ENABLE_SCI32 1

/* Feature flags */
#define SCUMMVM_USE_PRAGMA_PACK

/* Disabled features */
/* #undef USE_ZLIB */
/* #undef USE_PNG */
/* #undef USE_JPEG */
/* #undef USE_FLAC */
/* #undef USE_VORBIS */
/* #undef USE_MAD */
/* #undef USE_FAAD */
/* #undef USE_FREETYPE2 */
/* #undef USE_CURL */
/* #undef USE_SDL_NET */
/* #undef USE_CLOUD */
/* #undef USE_LIBCURL */
/* #undef USE_UPDATES */
/* #undef USE_LUA */
/* #undef USE_IMGUI */
/* #undef USE_DISCORD */
/* #undef USE_TINYGL */
/* #undef USE_OPENGL */
/* #undef USE_THEORADEC */
/* #undef USE_A52 */
/* #undef USE_MPEG2 */
/* #undef USE_RETROWAVE */
/* #undef USE_MT32EMU */
/* #undef USE_FLUIDSYNTH */
/* #undef USE_SONIVOX */
/* #undef USE_MIKMOD */
/* #undef USE_OPENMPT */
/* #undef USE_GIF */
/* #undef USE_FRIBIDI */
/* #undef USE_SPEECH_DISPATCHER */
/* #undef USE_SNDIO */
/* #undef USE_ALSA */
/* #undef USE_SEQ_MIDI */
/* #undef USE_TIMIDITY */
/* #undef POSIX */
/* #undef USE_RGB_COLOR */
/* #undef USE_SCALERS */
/* #undef USE_HQ_SCALERS */
/* #undef USE_ASPECT */
/* #undef USE_BINK */
/* #undef USE_VKEYBD */
/* #undef ENABLE_EVENTRECORDER */
/* #undef ENABLE_RECORDER */
/* #undef USE_TTS */
/* #undef DETECTION_FULL */
#define USE_TEXT_CONSOLE_FOR_DEBUGGER 1

/* Package info */
#define SCUMMVM_VERSION "2.9.0-openfpga"
#define PLUGIN_DIRECTORY ""

/* Single engine — static plugin */
#define STATIC_PLUGIN 1
#define DYNAMIC_MODULES 0
#define DETECTION_STATIC 1

#endif /* OPENFPGA_CONFIG_H */
