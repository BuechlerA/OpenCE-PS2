/*
PS2_NULL_PLATFORM.C

The platform layer's services that the PS2 build does not have yet, or never
will, as nothing: sound (DirectSound fails to start, so the game runs
silent), the gamepads, the network (Winsock and XNet are down), voice chat,
internet play, the PC version's menus, and the high-res HUD and text. Each
returns what the Linux build's does when it has nothing to give (port/linux/src:
dsound_sdl.c, xinput_sdl.c, xnet.c, menu_files.c, hud_hires.c, text_hires.c).
The services the PS2 gets later (docs/ps2_port_plan.md, "Sound, input, saves
and streaming") replace these one at a time.
*/

#include "platform.h"

#include <stdlib.h>
#include <string.h>

struct p2p_listing;
struct halo_menus;
struct text_hires_glyph;

/* the compiler's barrier (source/scenario/scenario.c), a no-op for the EE's in-order calls */
void _ReadWriteBarrier(void) { }

/* ---------- DirectSound (dsound_sdl.c) */

HRESULT WINAPI DirectSoundCreate(LPGUID device_id, LPDIRECTSOUND *result, LPUNKNOWN outer)
{
	*result = NULL;
	return E_FAIL;
}

ULONG WINAPI IDirectSound_Release(LPDIRECTSOUND sound) { return 0; }
VOID WINAPI DirectSoundDoWork(void) { }
VOID WINAPI DirectSoundUseFullHRTF(void) { }
HRESULT WINAPI IDirectSound_GetCaps(LPDIRECTSOUND sound, LPDSCAPS caps) { return E_FAIL; }
HRESULT WINAPI IDirectSound_GetSpeakerConfig(LPDIRECTSOUND sound, LPDWORD speaker_config) { return E_FAIL; }
HRESULT WINAPI IDirectSound_DownloadEffectsImage(LPDIRECTSOUND sound, LPCVOID image, DWORD image_size,
	LPCDSEFFECTIMAGELOC image_location, LPDSEFFECTIMAGEDESC *image_description) { return E_FAIL; }
HRESULT WINAPI IDirectSound_CommitDeferredSettings(LPDIRECTSOUND sound) { return DS_OK; }
HRESULT WINAPI IDirectSound_SetMixBinHeadroom(LPDIRECTSOUND sound, DWORD mix_bin_mask, DWORD headroom) { return DS_OK; }
HRESULT WINAPI IDirectSound_SetI3DL2Listener(LPDIRECTSOUND sound, LPCDSI3DL2LISTENER listener_properties, DWORD apply) { return DS_OK; }
HRESULT WINAPI IDirectSound_SetDistanceFactor(LPDIRECTSOUND sound, FLOAT factor, DWORD apply) { return DS_OK; }
HRESULT WINAPI IDirectSound_SetRolloffFactor(LPDIRECTSOUND sound, FLOAT factor, DWORD apply) { return DS_OK; }
HRESULT WINAPI IDirectSound_SetPosition(LPDIRECTSOUND sound, FLOAT x, FLOAT y, FLOAT z, DWORD apply) { return DS_OK; }
HRESULT WINAPI IDirectSound_SetVelocity(LPDIRECTSOUND sound, FLOAT x, FLOAT y, FLOAT z, DWORD apply) { return DS_OK; }
HRESULT WINAPI IDirectSound_SetOrientation(LPDIRECTSOUND sound, FLOAT x_front, FLOAT y_front, FLOAT z_front,
	FLOAT x_top, FLOAT y_top, FLOAT z_top, DWORD apply) { return DS_OK; }
HRESULT WINAPI IDirectSound_CreateSoundStream(LPDIRECTSOUND sound, LPCDSSTREAMDESC description,
	LPDIRECTSOUNDSTREAM *result, LPUNKNOWN outer)
{
	*result = NULL;
	return E_FAIL;
}
HRESULT WINAPI IDirectSound_CreateSoundBuffer(LPDIRECTSOUND sound, LPCDSBUFFERDESC description,
	LPDIRECTSOUNDBUFFER *result, LPUNKNOWN outer)
{
	*result = NULL;
	return E_FAIL;
}
HRESULT WINAPI DirectSoundCreateBuffer(LPCDSBUFFERDESC description, LPDIRECTSOUNDBUFFER *result)
{
	*result = NULL;
	return E_FAIL;
}

void __stdcall DirectSoundStopStream(LPDIRECTSOUNDSTREAM stream) { }
unsigned long __stdcall DirectSoundGetStreamVoiceStatus(LPDIRECTSOUNDSTREAM stream) { return 0; }
void dsound_sdl_stream_set_stereo_position(IDirectSoundStream *object, BOOL positioned, float pan,
	float distance, float minimum_distance, float distance_fade) { }

HRESULT WINAPI IDirectSoundStream_SetFrequency(LPDIRECTSOUNDSTREAM stream, DWORD frequency) { return DS_OK; }
HRESULT WINAPI IDirectSoundStream_SetVolume(LPDIRECTSOUNDSTREAM stream, LONG volume) { return DS_OK; }
HRESULT WINAPI IDirectSoundStream_SetMixBins(LPDIRECTSOUNDSTREAM stream, DWORD mix_bin_mask) { return DS_OK; }
HRESULT WINAPI IDirectSoundStream_SetMixBinVolumes(LPDIRECTSOUNDSTREAM stream, DWORD mix_bin_mask, const LONG *volumes) { return DS_OK; }
HRESULT WINAPI IDirectSoundStream_SetMode(LPDIRECTSOUNDSTREAM stream, DWORD mode, DWORD apply) { return DS_OK; }
HRESULT WINAPI IDirectSoundStream_SetPosition(LPDIRECTSOUNDSTREAM stream, FLOAT x, FLOAT y, FLOAT z, DWORD apply) { return DS_OK; }
HRESULT WINAPI IDirectSoundStream_SetMinDistance(LPDIRECTSOUNDSTREAM stream, FLOAT distance, DWORD apply) { return DS_OK; }
HRESULT WINAPI IDirectSoundStream_SetMaxDistance(LPDIRECTSOUNDSTREAM stream, FLOAT distance, DWORD apply) { return DS_OK; }
HRESULT WINAPI IDirectSoundStream_SetI3DL2Source(LPDIRECTSOUNDSTREAM stream, LPCDSI3DL2BUFFER source, DWORD apply) { return DS_OK; }
HRESULT WINAPI IDirectSoundStream_SetVelocity(LPDIRECTSOUNDSTREAM stream, FLOAT x, FLOAT y, FLOAT z, DWORD apply) { return DS_OK; }
HRESULT WINAPI IDirectSoundStream_SetConeAngles(LPDIRECTSOUNDSTREAM stream, DWORD inside, DWORD outside, DWORD apply) { return DS_OK; }
HRESULT WINAPI IDirectSoundStream_SetConeOrientation(LPDIRECTSOUNDSTREAM stream, FLOAT x, FLOAT y, FLOAT z, DWORD apply) { return DS_OK; }
HRESULT WINAPI IDirectSoundStream_SetConeOutsideVolume(LPDIRECTSOUNDSTREAM stream, LONG volume, DWORD apply) { return DS_OK; }

ULONG WINAPI IDirectSoundBuffer_Release(LPDIRECTSOUNDBUFFER buffer) { return 0; }
HRESULT WINAPI IDirectSoundBuffer_SetBufferData(LPDIRECTSOUNDBUFFER buffer, LPVOID data, DWORD size) { return DS_OK; }
HRESULT WINAPI IDirectSoundBuffer_Play(LPDIRECTSOUNDBUFFER buffer, DWORD reserved1, DWORD reserved2, DWORD flags) { return DS_OK; }
HRESULT WINAPI IDirectSoundBuffer_Stop(LPDIRECTSOUNDBUFFER buffer) { return DS_OK; }
HRESULT WINAPI IDirectSoundBuffer_SetCurrentPosition(LPDIRECTSOUNDBUFFER buffer, DWORD play_cursor) { return DS_OK; }
HRESULT WINAPI IDirectSoundBuffer_SetLoopRegion(LPDIRECTSOUNDBUFFER buffer, DWORD loop_start, DWORD loop_length) { return DS_OK; }
HRESULT WINAPI IDirectSoundBuffer_SetPitch(LPDIRECTSOUNDBUFFER buffer, LONG pitch) { return DS_OK; }
HRESULT WINAPI IDirectSoundBuffer_SetVolume(LPDIRECTSOUNDBUFFER buffer, LONG volume) { return DS_OK; }

/* ---------- keyboard and mouse (xinput_sdl.c); the gamepad is ps2_input.c's */

int halo_linux_mouse_look(short gamepad_index, float *yaw, float *pitch)
{
	*yaw = 0.0f;
	*pitch = 0.0f;
	return FALSE;
}

int halo_linux_mouse_aiming(short gamepad_index) { return FALSE; }
unsigned long halo_keyboard_actions(short controller_index) { return 0; }
int halo_push_to_talk_held(void) { return FALSE; }
void halo_input_name(int input, char *name, size_t size) { if (size) name[0] = '\0'; }
void platform_text_typing(int typing) { }
void platform_text_field(int typing, int password) { }
void platform_binding_capture_begin(void) { }
int platform_binding_capture_poll(int *input) { return FALSE; }

/* ---------- the network (xnet.c): Winsock and XNet are down */

#define NETWORK_DOWN 10050 /* WSAENETDOWN */
#define NETWORK_FAILED (-1) /* SOCKET_ERROR, INVALID_SOCKET */

int WSAAPI WSAStartup(WORD version_requested, LPWSADATA data) { return 10091; /* WSASYSNOTREADY */ }
int WSAAPI WSACleanup(void) { return 0; }
int WSAAPI WSAGetLastError(void) { return NETWORK_DOWN; }
int PASCAL __WSAFDIsSet(SOCKET socket, halo_ws_fd_set *set) { return 0; }

SOCKET WSAAPI halo_ws_socket(int family, int type, int protocol) { return NETWORK_FAILED; }
int WSAAPI halo_ws_closesocket(SOCKET socket) { return NETWORK_FAILED; }
int WSAAPI halo_ws_bind(SOCKET socket, const struct sockaddr *address, int address_length) { return NETWORK_FAILED; }
int WSAAPI halo_ws_connect(SOCKET socket, const struct sockaddr *address, int address_length) { return NETWORK_FAILED; }
int WSAAPI halo_ws_listen(SOCKET socket, int backlog) { return NETWORK_FAILED; }
SOCKET WSAAPI halo_ws_accept(SOCKET socket, struct sockaddr *address, int *address_length) { return NETWORK_FAILED; }
int WSAAPI halo_ws_send(SOCKET socket, const char *buffer, int length, int flags) { return NETWORK_FAILED; }
int WSAAPI halo_ws_sendto(SOCKET socket, const char *buffer, int length, int flags,
	const struct sockaddr *address, int address_length) { return NETWORK_FAILED; }
int WSAAPI halo_ws_recv(SOCKET socket, char *buffer, int length, int flags) { return NETWORK_FAILED; }
int WSAAPI halo_ws_recvfrom(SOCKET socket, char *buffer, int length, int flags,
	struct sockaddr *address, int *address_length) { return NETWORK_FAILED; }
int WSAAPI halo_ws_ioctlsocket(SOCKET socket, long command, u_long *argument) { return NETWORK_FAILED; }
int WSAAPI halo_ws_setsockopt(SOCKET socket, int level, int name, const char *value, int length) { return NETWORK_FAILED; }
int WSAAPI halo_ws_getsockopt(SOCKET socket, int level, int name, char *value, int *length) { return NETWORK_FAILED; }
int WSAAPI halo_ws_getsockname(SOCKET socket, struct sockaddr *address, int *address_length) { return NETWORK_FAILED; }
int WSAAPI halo_ws_getpeername(SOCKET socket, struct sockaddr *address, int *address_length) { return NETWORK_FAILED; }
int WSAAPI halo_ws_select(int descriptor_count, halo_ws_fd_set *read_set, halo_ws_fd_set *write_set,
	halo_ws_fd_set *error_set, const struct halo_ws_timeval *timeout) { return NETWORK_FAILED; }

INT WSAAPI XNetStartup(const XNetStartupParams *parameters) { return NETWORK_DOWN; }
INT WSAAPI XNetCleanup(void) { return 0; }
INT WSAAPI XNetRandom(BYTE *buffer, UINT size)
{
	UINT index;

	for (index = 0; index < size; index++)
		buffer[index] = (BYTE)rand();
	return 0;
}
INT WSAAPI XNetCreateKey(XNKID *key_identifier, XNKEY *key) { return NETWORK_DOWN; }
INT WSAAPI XNetRegisterKey(const XNKID *key_identifier, const XNKEY *key) { return NETWORK_DOWN; }
INT WSAAPI XNetUnregisterKey(const XNKID *key_identifier) { return NETWORK_DOWN; }
INT WSAAPI XNetXnAddrToInAddr(const XNADDR *address, const XNKID *key_identifier, IN_ADDR *result) { return NETWORK_DOWN; }
DWORD WSAAPI XNetGetTitleXnAddr(XNADDR *address)
{
	memset(address, 0, sizeof(*address));
	address->bSizeOfStruct = sizeof(*address);
	return XNET_GET_XNADDR_NONE;
}
DWORD WSAAPI XNetGetEthernetLinkStatus(void) { return 0; }

void posix_random_bytes(void *buffer, unsigned long size)
{
	unsigned char *bytes = buffer;
	unsigned long index;

	for (index = 0; index < size; index++)
		bytes[index] = (unsigned char)rand();
}

/* ---------- internet play and voice chat (p2p*.c, voice_audio.c) */

void p2p_discord_identity(char *id, int id_size, char *name, int name_size)
{
	if (id_size > 0)
		id[0] = '\0';
	if (name_size > 0)
		name[0] = '\0';
}
void p2p_discord_sanitize(char *destination, int size, const char *source, int name) { if (size > 0) destination[0] = '\0'; }
void p2p_hardware_id(char *hex, int size) { if (size > 0) hex[0] = '\0'; }
void p2p_hardware_id_sanitize(char *destination, int size, const char *source) { if (size > 0) destination[0] = '\0'; }
int p2p_invite_link(char *link, int size) { if (size > 0) link[0] = '\0'; return 0; }
int p2p_join_invite(const char *text) { return 0; }
int p2p_listing_unlock(struct p2p_listing *listing, const char *password) { return 0; }
void p2p_lobby_browse(int on) { }
int p2p_lobby_games(struct p2p_listing *games, int maximum_count) { return 0; }
void p2p_lobby_mark_failed(const unsigned char *identifier) { }
void p2p_lobby_refresh(void) { }
int p2p_peer_address(const unsigned char *identifier, unsigned long *address) { return 0; }
unsigned long p2p_peer_endpoint_address(unsigned long virtual_address) { return 0; }
void p2p_set_game_listing(const char *name, const char *map, const char *gametype, int engine_type, int open,
	int in_progress, int has_teams) { }
void p2p_set_game_player_counts(int count, int maximum) { }
void p2p_set_hosting_allowed(int allowed) { }
void p2p_set_hosting_password(const char *password) { }
void p2p_set_hosting_public(int public) { }

int voice_audio_encode(const float *frame, int bitrate, unsigned char *packet, int maximum) { return 0; }
void voice_audio_forget(int speaker) { }
void voice_audio_forget_all(void) { }
float voice_audio_level(const float *frame) { return 0.0f; }
int voice_audio_microphone(int open) { return 0; }
void voice_audio_play(int speaker, unsigned short sequence, const unsigned char *packet, int length, float gain,
	float pan) { }
int voice_audio_read_frame(float *frame) { return 0; }
void voice_audio_set_volume(float volume) { }

/* ---------- the PC version's menus, and the high-res HUD and text */

struct halo_menus const *halo_menus_load(void) { return NULL; }
void halo_menus_log(char const *file, long line, char const *message, char const *detail) { }
long halo_menus_utf16(char const *utf8, unsigned short *out, long capacity) { return 0; }
void halo_menus_art_register(void const *texture, char const *png) { }
void halo_menus_art_forget(void) { }

long hud_hires_asset_count(void) { return 0; }
char const *hud_hires_asset_tag(long asset) { return ""; }
long hud_hires_asset_bitmap(long asset) { return 0; }
long hud_hires_asset_fits(long asset, long width, long height) { return 0; }

long text_hires_font(char const *tag_name, float cap_height, float oversample) { return -1; }
int text_hires_covers(long font, unsigned long code) { return 0; }
int text_hires_glyph(long font, unsigned long code, struct text_hires_glyph *glyph) { return 0; }
void text_hires_register_atlas(const unsigned long *texture, unsigned long width, unsigned long height) { }

/* ---------- the window, the display and the desktop */

int halo_interpolation_enabled(void) { return FALSE; }
void platform_request_quit(void) { }
void platform_show_message(const char *title, const char *message) { }
int platform_clipboard_get(char *text, int size) { if (size > 0) text[0] = '\0'; return 0; }
void platform_clipboard_set(const char *text) { }
void platform_display_apply(void) { }
int platform_display_resolutions(long *widths, long *heights, int maximum) { return 0; }
int platform_window_sizes(long *widths, long *heights, int maximum) { return 0; }
int platform_audio_devices(int recording, char (*names)[128], int maximum) { return 0; }
void platform_scoreboard_scroll(int open, long *notches, long *pages)
{
	*notches = 0;
	*pages = 0;
}
