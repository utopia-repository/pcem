#include <SDL2/SDL.h>

#ifdef __WINDOWS__
#define BITMAP WINDOWS_BITMAP
#undef UNICODE
#include <windows.h>
#include <windowsx.h>
#undef BITMAP
#endif

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <math.h>
#include "ibm.h"
#include "device.h"
#include "cassette.h"
#include "cdrom-ioctl.h"
#include "cdrom-image.h"
#include "config.h"
#include "video.h"
#include "cpu.h"
#include "ide.h"
#include "hdd.h"
#include "model.h"
#include "mouse.h"
#include "nvr.h"
#include "lpt.h"
#include "plat-joystick.h"
#include "plat-midi.h"
#include "scsi_zip.h"
#include "sound.h"
#include "thread.h"
#include "disc.h"
#include "disc_img.h"
#include "mem.h"
#include "paths.h"
#include "nethandler.h"

#include "sdl2-video.h"
#include "sdl2-display.h"

#include "plugin.h"
#include "pic.h"

#if __APPLE__
#define pause __pause
#include <util.h>
#include <fcntl.h>
#include <unistd.h>
#undef pause
#include <sys/types.h>
#include <sys/stat.h>
#endif

typedef enum
{
	EMULATION_STOPPED,
	EMULATION_PAUSED,
	EMULATION_RUNNING
} emulation_state_t;

#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

extern void creatediscimage_open(void *hwnd);

#define IDM_CDROM_REAL 1500

#define MIN_SND_BUF 50

uint64_t timer_freq;

int gfx_present[GFX_MAX];

SDL_mutex *ghMutex;
SDL_mutex *mainMutex;
SDL_cond *mainCond;

SDL_Thread *mainthreadh = NULL;

SDL_TimerID onesectimer;

int running = 0;

int drawits = 0;

int romspresent[ROM_MAX];
int quited = 0;

SDL_Rect oldclip;

void *ghwnd = 0;

void *menu;

emulation_state_t emulation_state = EMULATION_STOPPED;
int pause = 0;

int window_doreset = 0;
int window_dosetresize = 0;
int renderer_doreset = 0;
int window_dofullscreen = 0;
int window_dowindowed = 0;
int window_doremember = 0;
int window_doinputgrab = 0;
int window_doinputrelease = 0;
int window_dotogglefullscreen = 0;

int video_scale = 1;

int video_width = 640;
int video_height = 480;

char menuitem[60];

extern int config_selection_open(void *hwnd, int inited);
extern int shader_manager_open(void *hwnd);

extern void sdl_set_window_title(const char *title);

extern float gl3_shader_refresh_rate;
extern float gl3_input_scale;
extern int gl3_input_stretch;
extern char gl3_shader_file[20][512];

char screenshot_format[10];
int screenshot_flash = 1;
int take_screenshot = 0;

void updatewindowsize(int x, int y)
{
	if (video_width == x && video_height == y)
	{
		return;
	}
	video_width = x;
	video_height = y;

	display_resize(x, y);
}

unsigned int get_ticks()
{
	return SDL_GetTicks();
}

void delay_ms(unsigned int ms)
{
	SDL_Delay(ms);
}

void startblit()
{
	SDL_LockMutex(ghMutex);
}

void endblit()
{
	SDL_UnlockMutex(ghMutex);
}

void enter_fullscreen()
{
	window_dofullscreen = window_doinputgrab = 1;
}

void leave_fullscreen()
{
	window_dowindowed = window_doinputrelease = 1;
}

void toggle_fullscreen()
{
	window_dotogglefullscreen = 1;
}

uint64_t main_time;

int mainthread(void *param)
{
	SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);

	int frames = 0;
	uint32_t old_time, new_time;

	drawits = 0;
	old_time = SDL_GetTicks();
	running = 1;
	while (running)
	{
		new_time = SDL_GetTicks();
		drawits += new_time - old_time;
		old_time = new_time;

		if (drawits > 0 && !pause)
		{
			uint64_t start_time = timer_read();
			uint64_t end_time;
			drawits -= 10;
			if (drawits > 50)
				drawits = 0;
			runpc();
			frames++;
			if (frames >= 200 && nvr_dosave)
			{
				frames = 0;
				nvr_dosave = 0;
				savenvr();
			}
			end_time = timer_read();
			main_time += end_time - start_time;
		}
		else
			SDL_Delay(1);
	}

	SDL_LockMutex(mainMutex);
	SDL_CondSignal(mainCond);
	SDL_UnlockMutex(mainMutex);

	return TRUE;
}

void stop_emulation_now(void)
{
	/*Deduct a sufficiently large number of cycles that no instructions will
	  run before the main thread is terminated*/
	cycles -= 99999999;
}

void set_window_title(const char *s)
{
	sdl_set_window_title(s);
}

float flash_func(float x)
{
	return 1 - pow(x, 4);
}

float flash_failed_func(float x)
{
	return fabs(sin(x * 3.1415926 * 2));
}

void screenshot_taken(unsigned char *rgb, int width, int height)
{
// TODO: Use non wxWidgets API
#if 0
	char name[512];
	char date[128];
	strcpy(name, "Screenshot from ");
	wx_date_format(date, "%Y-%m-%d %H-%M-%S");
	strcat(name, date);
	if (wx_image_save(screenshots_path, name, screenshot_format, rgb, width, height, 0))
	{
		pclog("Screenshot saved\n");
		if (screenshot_flash)
			color_flash(flash_func, 500, 0xff, 0xff, 0xff, 0xff);
	}
	else
	{
		pclog("Screenshot was not saved\n");
		if (screenshot_flash)
			color_flash(flash_failed_func, 500, 0xff, 0, 0, 0xff);
	}
#endif
}

uint64_t timer_read()
{
	return SDL_GetPerformanceCounter();
}

Uint32 timer_onesec(Uint32 interval, void *param)
{
	onesec();
	return interval;
}

void sdl_loadconfig()
{
	vid_resize = config_get_int(CFG_MACHINE, NULL, "vid_resize", 0);
	video_fullscreen_scale = config_get_int(CFG_MACHINE, NULL, "video_fullscreen_scale", 0);
	video_fullscreen_first = config_get_int(CFG_MACHINE, NULL, "video_fullscreen_first", 1);

	strcpy(screenshot_format, config_get_string(CFG_MACHINE, "SDL2", "screenshot_format", 0));
	screenshot_flash = config_get_int(CFG_MACHINE, "SDL2", "screenshot_flash", 1);

	custom_resolution_width = config_get_int(CFG_MACHINE, "SDL2", "custom_width", custom_resolution_width);
	custom_resolution_height = config_get_int(CFG_MACHINE, "SDL2", "custom_height", custom_resolution_height);

	video_fullscreen = config_get_int(CFG_MACHINE, "SDL2", "fullscreen", video_fullscreen);
	video_fullscreen_mode = config_get_int(CFG_MACHINE, "SDL2", "fullscreen_mode", video_fullscreen_mode);
	video_scale = config_get_int(CFG_MACHINE, "SDL2", "scale", video_scale);
	video_scale_mode = config_get_int(CFG_MACHINE, "SDL2", "scale_mode", video_scale_mode);
	video_vsync = config_get_int(CFG_MACHINE, "SDL2", "vsync", video_vsync);
	video_focus_dim = config_get_int(CFG_MACHINE, "SDL2", "focus_dim", video_focus_dim);
	video_alternative_update_lock = config_get_int(CFG_MACHINE, "SDL2", "alternative_update_lock", video_alternative_update_lock);
	requested_render_driver = sdl_get_render_driver_by_name(config_get_string(CFG_MACHINE, "SDL2", "render_driver", ""), RENDERER_SOFTWARE);

#if 0
	gl3_input_scale = config_get_float(CFG_MACHINE, "GL3", "input_scale", gl3_input_scale);
	gl3_input_stretch = config_get_int(CFG_MACHINE, "GL3", "input_stretch", gl3_input_stretch);
	gl3_shader_refresh_rate = config_get_float(CFG_MACHINE, "GL3", "shader_refresh_rate", gl3_shader_refresh_rate);

	memset(&gl3_shader_file, 0, sizeof(gl3_shader_file));
	int num_shaders = config_get_int(CFG_MACHINE, "GL3 Shaders", "shaders", 0);
	char s[20];
	int i;
	for (i = 0; i < num_shaders; ++i)
	{
		sprintf(s, "shader%d", i);
		strncpy(gl3_shader_file[i], config_get_string(CFG_MACHINE, "GL3 Shaders", s, ""), 511);
		gl3_shader_file[i][511] = 0;
	}
#endif
}

void sdl_saveconfig()
{
	config_set_int(CFG_MACHINE, NULL, "vid_resize", vid_resize);
	config_set_int(CFG_MACHINE, NULL, "video_fullscreen_scale", video_fullscreen_scale);
	config_set_int(CFG_MACHINE, NULL, "video_fullscreen_first", video_fullscreen_first);

	config_set_string(CFG_MACHINE, "SDL2", "screenshot_format", screenshot_format);
	config_set_int(CFG_MACHINE, "SDL2", "screenshot_flash", screenshot_flash);

	config_set_int(CFG_MACHINE, "SDL2", "custom_width", custom_resolution_width);
	config_set_int(CFG_MACHINE, "SDL2", "custom_height", custom_resolution_height);

	config_set_int(CFG_MACHINE, "SDL2", "fullscreen", video_fullscreen);
	config_set_int(CFG_MACHINE, "SDL2", "fullscreen_mode", video_fullscreen_mode);
	config_set_int(CFG_MACHINE, "SDL2", "scale", video_scale);
	config_set_int(CFG_MACHINE, "SDL2", "scale_mode", video_scale_mode);
	config_set_int(CFG_MACHINE, "SDL2", "vsync", video_vsync);
	config_set_int(CFG_MACHINE, "SDL2", "focus_dim", video_focus_dim);
	config_set_int(CFG_MACHINE, "SDL2", "alternative_update_lock", video_alternative_update_lock);
	config_set_string(CFG_MACHINE, "SDL2", "render_driver", (char *)requested_render_driver.sdl_id);

#if 0
	config_set_float(CFG_MACHINE, "GL3", "input_scale", gl3_input_scale);
	config_set_int(CFG_MACHINE, "GL3", "input_stretch", gl3_input_stretch);
	config_set_float(CFG_MACHINE, "GL3", "shader_refresh_rate", gl3_shader_refresh_rate);

	char s[20];
	int i;
	for (i = 0; i < 20; ++i)
	{
		sprintf(s, "shader%d", i);
		if (strlen(gl3_shader_file[i]))
			config_set_string(CFG_MACHINE, "GL3 Shaders", s, gl3_shader_file[i]);
		else
			break;
	}
	config_set_int(CFG_MACHINE, "GL3 Shaders", "shaders", i);
#endif
}

void sdl_onconfigloaded()
{
	/* create directories */
	if (!dir_exists(configs_path))
		dir_create(configs_path);
	if (!dir_exists(nvr_path))
		dir_create(nvr_path);
	if (!dir_exists(logs_path))
		dir_create(logs_path);
	if (!dir_exists(screenshots_path))
		dir_create(screenshots_path);
}

int pc_main(int argc, char **argv)
{
	// Expose some functions to libpcem-plugin-api without moving them over to
	// the plugin api proper
	_savenvr = savenvr;
	_dumppic = dumppic;
	_dumpregs = dumpregs;
	_sound_speed_changed = sound_speed_changed;

	paths_init();

	init_plugin_engine();
	model_init_builtin();
	video_init_builtin();
	lpt_init_builtin();
	sound_init_builtin();
	hdd_controller_init_builtin();
#ifdef USE_NETWORKING
	network_card_init_builtin();
#endif

	add_config_callback(sdl_loadconfig, sdl_saveconfig, sdl_onconfigloaded);

	initpc(argc, argv);
	resetpchard();

	sound_init();

#ifndef __APPLE__
	display_init();
#endif
	sdl_video_init();
	joystick_init();

	start_emulation(NULL);

	return TRUE;
}

int resume_emulation()
{
	if (emulation_state == EMULATION_PAUSED)
	{
		emulation_state = EMULATION_RUNNING;
		pause = 0;
		return TRUE;
	}
	return FALSE;
}

int start_emulation(void *params)
{
	if (resume_emulation())
		return TRUE;
	int c;
	pclog("Starting emulation...\n");
	loadconfig(NULL);

	emulation_state = EMULATION_RUNNING;
	pause = 0;

	ghMutex = SDL_CreateMutex();
	mainMutex = SDL_CreateMutex();
	mainCond = SDL_CreateCond();

	if (!loadbios())
	{
		if (romset != -1)
			// wx_messagebox(ghwnd,
			//			  "Configured romset not available.\nDefaulting to available romset.",
			//			  "PCem error", WX_MB_OK);
			pclog("Error: Configured romset not available.\nDefaulting to available romset.");
		for (c = 0; c < ROM_MAX; c++)
		{
			if (romspresent[c])
			{
				romset = c;
				model = model_getmodel(romset);
				break;
			}
		}
	}

	if (!video_card_available(video_old_to_new(gfxcard)))
	{
		if (romset != -1)
			// wx_messagebox(ghwnd,
			//			  "Configured video BIOS not available.\nDefaulting to available romset.",
			//			  "PCem error", WX_MB_OK);
			pclog("Error: Configured video BIOS not available.\nDefaulting to available romset.");
		for (c = GFX_MAX - 1; c >= 0; c--)
		{
			if (gfx_present[c])
			{
				gfxcard = c;
				break;
			}
		}
	}

	loadbios();
	resetpchard();
	midi_init();

	display_start(params);

	mainthreadh = SDL_CreateThread(mainthread, "Main Thread", NULL);

	onesectimer = SDL_AddTimer(1000, timer_onesec, NULL);

	updatewindowsize(640, 480);

	timer_freq = SDL_GetPerformanceFrequency();

	// if (show_machine_on_start)
	//	wx_show_status(ghwnd);
	while (emulation_state == EMULATION_RUNNING)
		;

	return TRUE;
}

int pause_emulation()
{
	pclog("Emulation paused.\n");
	emulation_state = EMULATION_PAUSED;
	pause = 1;
	return TRUE;
}

int stop_emulation()
{
	emulation_state = EMULATION_STOPPED;
	pclog("Stopping emulation...\n");
	SDL_LockMutex(mainMutex);
	running = 0;
	SDL_CondWaitTimeout(mainCond, mainMutex, 10 * 1000);
	SDL_UnlockMutex(mainMutex);

	SDL_DestroyCond(mainCond);
	SDL_DestroyMutex(mainMutex);

	startblit();
	display_stop();

#if SDL_VERSION_ATLEAST(2, 0, 2)
	SDL_DetachThread(mainthreadh);
#endif
	mainthreadh = NULL;
	SDL_RemoveTimer(onesectimer);
	savenvr();
	saveconfig(NULL);

	endblit();
	SDL_DestroyMutex(ghMutex);

	device_close_all();
	midi_close();

	pclog("Emulation stopped.\n");

	return TRUE;
}

void reset_emulation()
{
	pause_emulation();
	SDL_Delay(100);
	resetpchard();
	resume_emulation();
}

int wx_stop()
{
	pclog("Shutting down...\n");
	closepc();
	display_close();
	sdl_video_close();

	printf("Shut down successfully!\n");
	return TRUE;
}
