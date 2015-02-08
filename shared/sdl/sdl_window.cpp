#include <SDL.h>
#include <SDL_syswm.h>
#include "qcommon/qcommon.h"
#include "rd-common/tr_types.h"
#include "sys/sys_local.h"

enum rserr_t
{
	RSERR_OK,

	RSERR_INVALID_FULLSCREEN,
	RSERR_INVALID_MODE,

	RSERR_UNKNOWN
};

static SDL_Window *screen = NULL;
static SDL_GLContext opengl_context;
static float displayAspect;

cvar_t *r_sdlDriver;

// Window cvars
cvar_t	*r_fullscreen = 0;
cvar_t	*r_noborder;
cvar_t	*r_centerWindow;
cvar_t	*r_customwidth;
cvar_t	*r_customheight;
cvar_t	*r_swapInterval;
cvar_t	*r_stereo;
cvar_t	*r_mode;
cvar_t	*r_displayRefresh;

// Window surface cvars
cvar_t	*r_stencilbits;
cvar_t	*r_depthbits;
cvar_t	*r_colorbits;
cvar_t	*r_ignorehwgamma;
cvar_t  *r_ext_multisample;

/*
** R_GetModeInfo
*/
typedef struct vidmode_s
{
    const char *description;
    int         width, height;
} vidmode_t;

const vidmode_t r_vidModes[] = {
    { "Mode  0: 320x240",		320,	240 },
    { "Mode  1: 400x300",		400,	300 },
    { "Mode  2: 512x384",		512,	384 },
    { "Mode  3: 640x480",		640,	480 },
    { "Mode  4: 800x600",		800,	600 },
    { "Mode  5: 960x720",		960,	720 },
    { "Mode  6: 1024x768",		1024,	768 },
    { "Mode  7: 1152x864",		1152,	864 },
    { "Mode  8: 1280x1024",		1280,	1024 },
    { "Mode  9: 1600x1200",		1600,	1200 },
    { "Mode 10: 2048x1536",		2048,	1536 },
    { "Mode 11: 856x480 (wide)", 856,	 480 },
    { "Mode 12: 2400x600(surround)",2400,600 }
};
static const int	s_numVidModes = ARRAY_LEN( r_vidModes );

#define R_MODE_FALLBACK (4) // 640x480

qboolean R_GetModeInfo( int *width, int *height, int mode ) {
	const vidmode_t	*vm;

    if ( mode < -1 ) {
        return qfalse;
	}
	if ( mode >= s_numVidModes ) {
		return qfalse;
	}

	if ( mode == -1 ) {
		*width = r_customwidth->integer;
		*height = r_customheight->integer;
		return qtrue;
	}

	vm = &r_vidModes[mode];

    *width  = vm->width;
    *height = vm->height;

    return qtrue;
}

/*
** R_ModeList_f
*/
static void R_ModeList_f( void )
{
	int i;

	Com_Printf( "\n" );
	Com_Printf( "Mode -2: Use desktop resolution\n" );
	Com_Printf( "Mode -1: Use r_customWidth and r_customHeight variables\n" );
	for ( i = 0; i < s_numVidModes; i++ )
	{
		Com_Printf( "%s\n", r_vidModes[i].description );
	}
	Com_Printf( "\n" );
}

/*
===============
GLimp_Minimize

Minimize the game so that user is back at the desktop
===============
*/
void GLimp_Minimize(void)
{
	SDL_MinimizeWindow( screen );
}

void WIN_Present( window_t *window )
{
	if ( window->api == GRAPHICS_API_OPENGL )
	{
		SDL_GL_SwapWindow(screen);

		if ( r_swapInterval->modified )
		{
			r_swapInterval->modified = qfalse;
			SDL_GL_SetSwapInterval( r_swapInterval->integer );
		}
	}
}

/*
===============
GLimp_CompareModes
===============
*/
static int GLimp_CompareModes( const void *a, const void *b )
{
	const float ASPECT_EPSILON = 0.001f;
	SDL_Rect *modeA = (SDL_Rect *)a;
	SDL_Rect *modeB = (SDL_Rect *)b;
	float aspectA = (float)modeA->w / (float)modeA->h;
	float aspectB = (float)modeB->w / (float)modeB->h;
	int areaA = modeA->w * modeA->h;
	int areaB = modeB->w * modeB->h;
	float aspectDiffA = fabs( aspectA - displayAspect );
	float aspectDiffB = fabs( aspectB - displayAspect );
	float aspectDiffsDiff = aspectDiffA - aspectDiffB;

	if( aspectDiffsDiff > ASPECT_EPSILON )
		return 1;
	else if( aspectDiffsDiff < -ASPECT_EPSILON )
		return -1;
	else
		return areaA - areaB;
}

/*
===============
GLimp_DetectAvailableModes
===============
*/
static bool GLimp_DetectAvailableModes(void)
{
	int i, j;
	char buf[ MAX_STRING_CHARS ] = { 0 };
	SDL_Rect modes[ 128 ];
	int numModes = 0;

	int display = SDL_GetWindowDisplayIndex( screen );
	SDL_DisplayMode windowMode;

	if( SDL_GetWindowDisplayMode( screen, &windowMode ) < 0 )
	{
		Com_Printf( "Couldn't get window display mode, no resolutions detected (%s).\n", SDL_GetError() );
		return false;
	}

	int numDisplayModes = SDL_GetNumDisplayModes( display );
	for( i = 0; i < numDisplayModes; i++ )
	{
		SDL_DisplayMode mode;

		if( SDL_GetDisplayMode( display, i, &mode ) < 0 )
			continue;

		if( !mode.w || !mode.h )
		{
			Com_Printf( "Display supports any resolution\n" );
			return true;
		}

		if( windowMode.format != mode.format )
			continue;

		// SDL can give the same resolution with different refresh rates.
		// Only list resolution once.
		for( j = 0; j < numModes; j++ )
		{
			if( mode.w == modes[ j ].w && mode.h == modes[ j ].h )
				break;
		}
		
		if( j != numModes )
			continue;

		modes[ numModes ].w = mode.w;
		modes[ numModes ].h = mode.h;
		numModes++;
	}

	if( numModes > 1 )
		qsort( modes, numModes, sizeof( SDL_Rect ), GLimp_CompareModes );

	for( i = 0; i < numModes; i++ )
	{
		const char *newModeString = va( "%ux%u ", modes[ i ].w, modes[ i ].h );

		if( strlen( newModeString ) < (int)sizeof( buf ) - strlen( buf ) )
			Q_strcat( buf, sizeof( buf ), newModeString );
		else
			Com_Printf( "Skipping mode %ux%u, buffer too small\n", modes[ i ].w, modes[ i ].h );
	}

	if( *buf )
	{
		buf[ strlen( buf ) - 1 ] = 0;
		Com_Printf( "Available modes: '%s'\n", buf );
		Cvar_Set( "r_availableModes", buf );
	}

	return true;
}

/*
===============
GLimp_SetMode
===============
*/
static rserr_t GLimp_SetMode(glconfig_t *glConfig, graphicsApi_t graphicsApi, const char *windowTitle, int mode, qboolean fullscreen, qboolean noborder)
{
	int perChannelColorBits;
	int colorBits, depthBits, stencilBits;
	int samples;
	int i = 0;
	Uint32 flags = SDL_WINDOW_SHOWN;
	SDL_DisplayMode desktopMode;
	int display = 0;
	int x = SDL_WINDOWPOS_UNDEFINED, y = SDL_WINDOWPOS_UNDEFINED;

	if ( graphicsApi == GRAPHICS_API_OPENGL )
	{
		flags |= SDL_WINDOW_OPENGL;
	}

	Com_Printf( "Initializing display\n");

	// If a window exists, note its display index
	if( screen != NULL )
		display = SDL_GetWindowDisplayIndex( screen );

	if( SDL_GetDesktopDisplayMode( display, &desktopMode ) == 0 )
	{
		displayAspect = (float)desktopMode.w / (float)desktopMode.h;

		Com_Printf( "Display aspect: %.3f\n", displayAspect );
	}
	else
	{
		Com_Memset( &desktopMode, 0, sizeof( SDL_DisplayMode ) );

		Com_Printf( "Cannot determine display aspect, assuming 1.333\n" );
	}

	Com_Printf( "...setting mode %d:", mode );

	if (mode == -2)
	{
		// use desktop video resolution
		if( desktopMode.h > 0 )
		{
			glConfig->vidWidth = desktopMode.w;
			glConfig->vidHeight = desktopMode.h;
		}
		else
		{
			glConfig->vidWidth = 640;
			glConfig->vidHeight = 480;
			Com_Printf( "Cannot determine display resolution, assuming 640x480\n" );
		}

		//glConfig.windowAspect = (float)glConfig.vidWidth / (float)glConfig.vidHeight;
	}
	else if ( !R_GetModeInfo( &glConfig->vidWidth, &glConfig->vidHeight, /*&glConfig.windowAspect,*/ mode ) )
	{
		Com_Printf( " invalid mode\n" );
		return RSERR_INVALID_MODE;
	}
	Com_Printf( " %d %d\n", glConfig->vidWidth, glConfig->vidHeight);

	// Center window
	if( r_centerWindow->integer && !fullscreen )
	{
		x = ( desktopMode.w / 2 ) - ( glConfig->vidWidth / 2 );
		y = ( desktopMode.h / 2 ) - ( glConfig->vidHeight / 2 );
	}

	// Destroy existing state if it exists
	if( opengl_context != NULL )
	{
		SDL_GL_DeleteContext( opengl_context );
		opengl_context = NULL;
	}

	if( screen != NULL )
	{
		SDL_GetWindowPosition( screen, &x, &y );
		Com_DPrintf( "Existing window at %dx%d before being destroyed\n", x, y );
		SDL_DestroyWindow( screen );
		screen = NULL;
	}

	if( fullscreen )
	{
		flags |= SDL_WINDOW_FULLSCREEN;
		glConfig->isFullscreen = qtrue;
	}
	else
	{
		if( noborder )
			flags |= SDL_WINDOW_BORDERLESS;

		glConfig->isFullscreen = qfalse;
	}

	colorBits = r_colorbits->integer;
	if ((!colorBits) || (colorBits >= 32))
		colorBits = 24;

	if (!r_depthbits->integer)
		depthBits = 24;
	else
		depthBits = r_depthbits->integer;

	stencilBits = r_stencilbits->integer;
	samples = r_ext_multisample->integer;

	if ( graphicsApi == GRAPHICS_API_OPENGL )
	{
		for (i = 0; i < 16; i++)
		{
			int testColorBits, testDepthBits, testStencilBits;

			// 0 - default
			// 1 - minus colorBits
			// 2 - minus depthBits
			// 3 - minus stencil
			if ((i % 4) == 0 && i)
			{
				// one pass, reduce
				switch (i / 4)
				{
					case 2 :
						if (colorBits == 24)
							colorBits = 16;
						break;
					case 1 :
						if (depthBits == 24)
							depthBits = 16;
						else if (depthBits == 16)
							depthBits = 8;
					case 3 :
						if (stencilBits == 24)
							stencilBits = 16;
						else if (stencilBits == 16)
							stencilBits = 8;
				}
			}

			testColorBits = colorBits;
			testDepthBits = depthBits;
			testStencilBits = stencilBits;

			if ((i % 4) == 3)
			{ // reduce colorBits
				if (testColorBits == 24)
					testColorBits = 16;
			}

			if ((i % 4) == 2)
			{ // reduce depthBits
				if (testDepthBits == 24)
					testDepthBits = 16;
				else if (testDepthBits == 16)
					testDepthBits = 8;
			}

			if ((i % 4) == 1)
			{ // reduce stencilBits
				if (testStencilBits == 24)
					testStencilBits = 16;
				else if (testStencilBits == 16)
					testStencilBits = 8;
				else
					testStencilBits = 0;
			}

			if (testColorBits == 24)
				perChannelColorBits = 8;
			else
				perChannelColorBits = 4;

			SDL_GL_SetAttribute( SDL_GL_RED_SIZE, perChannelColorBits );
			SDL_GL_SetAttribute( SDL_GL_GREEN_SIZE, perChannelColorBits );
			SDL_GL_SetAttribute( SDL_GL_BLUE_SIZE, perChannelColorBits );
			SDL_GL_SetAttribute( SDL_GL_DEPTH_SIZE, testDepthBits );
			SDL_GL_SetAttribute( SDL_GL_STENCIL_SIZE, testStencilBits );

			SDL_GL_SetAttribute( SDL_GL_MULTISAMPLEBUFFERS, samples ? 1 : 0 );
			SDL_GL_SetAttribute( SDL_GL_MULTISAMPLESAMPLES, samples );

			if(r_stereo->integer)
			{
				glConfig->stereoEnabled = qtrue;
				SDL_GL_SetAttribute(SDL_GL_STEREO, 1);
			}
			else
			{
				glConfig->stereoEnabled = qfalse;
				SDL_GL_SetAttribute(SDL_GL_STEREO, 0);
			}

			SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 );
			SDL_GL_SetAttribute( SDL_GL_ACCELERATED_VISUAL, 1 );

			if( ( screen = SDL_CreateWindow( windowTitle, x, y,
					glConfig->vidWidth, glConfig->vidHeight, flags ) ) == NULL )
			{
				Com_DPrintf( "SDL_CreateWindow failed: %s\n", SDL_GetError( ) );
				continue;
			}

			if( fullscreen )
			{
				SDL_DisplayMode mode;

				switch( testColorBits )
				{
					case 16: mode.format = SDL_PIXELFORMAT_RGB565; break;
					case 24: mode.format = SDL_PIXELFORMAT_RGB24;  break;
					default: Com_DPrintf( "testColorBits is %d, can't fullscreen\n", testColorBits ); continue;
				}

				mode.w = glConfig->vidWidth;
				mode.h = glConfig->vidHeight;
				mode.refresh_rate = glConfig->displayFrequency = r_displayRefresh->integer;
				mode.driverdata = NULL;

				if( SDL_SetWindowDisplayMode( screen, &mode ) < 0 )
				{
					Com_DPrintf( "SDL_SetWindowDisplayMode failed: %s\n", SDL_GetError( ) );
					continue;
				}
			}

			if( ( opengl_context = SDL_GL_CreateContext( screen ) ) == NULL )
			{
				Com_Printf( "SDL_GL_CreateContext failed: %s\n", SDL_GetError( ) );
				continue;
			}

			SDL_GL_SetSwapInterval( r_swapInterval->integer );

			glConfig->colorBits = testColorBits;
			glConfig->depthBits = testDepthBits;
			glConfig->stencilBits = testStencilBits;

			Com_Printf( "Using %d color bits, %d depth, %d stencil display.\n",
					glConfig->colorBits, glConfig->depthBits, glConfig->stencilBits );
			break;
		}
	}
	else
	{
		// Just create a regular window
		if( ( screen = SDL_CreateWindow( windowTitle, x, y,
				glConfig->vidWidth, glConfig->vidHeight, flags ) ) == NULL )
		{
			Com_DPrintf( "SDL_CreateWindow failed: %s\n", SDL_GetError( ) );
		}
		else
		{
			if( fullscreen )
			{
				if( SDL_SetWindowDisplayMode( screen, NULL ) < 0 )
				{
					Com_DPrintf( "SDL_SetWindowDisplayMode failed: %s\n", SDL_GetError( ) );
				}
			}
		}
	}

	if (!GLimp_DetectAvailableModes())
	{
		return RSERR_UNKNOWN;
	}

	return RSERR_OK;
}

/*
===============
GLimp_StartDriverAndSetMode
===============
*/
static qboolean GLimp_StartDriverAndSetMode(glconfig_t *glConfig, graphicsApi_t graphicsApi, int mode, qboolean fullscreen, qboolean noborder)
{
	rserr_t err;

	if (!SDL_WasInit(SDL_INIT_VIDEO))
	{
		const char *driverName;

		if (SDL_Init(SDL_INIT_VIDEO) == -1)
		{
			Com_Printf( "SDL_Init( SDL_INIT_VIDEO ) FAILED (%s)\n", SDL_GetError());
			return qfalse;
		}

		driverName = SDL_GetCurrentVideoDriver();

		if (!driverName)
		{
			Com_Error( ERR_FATAL, "No video driver initialized" );
			return qfalse;
		}

		Com_Printf( "SDL using driver \"%s\"\n", driverName );
		Cvar_Set( "r_sdlDriver", driverName );
	}

	if (SDL_GetNumVideoDisplays() <= 0)
	{
		Com_Error( ERR_FATAL, "SDL_GetNumVideoDisplays() FAILED (%s)", SDL_GetError() );
	}

	if (fullscreen && Cvar_VariableIntegerValue( "in_nograb" ) )
	{
		Com_Printf( "Fullscreen not allowed with in_nograb 1\n");
		Cvar_Set( "r_fullscreen", "0" );
		r_fullscreen->modified = qfalse;
		fullscreen = qfalse;
	}

	err = GLimp_SetMode(glConfig, graphicsApi, CLIENT_WINDOW_TITLE, mode, fullscreen, noborder);

	switch ( err )
	{
		case RSERR_INVALID_FULLSCREEN:
			Com_Printf( "...WARNING: fullscreen unavailable in this mode\n" );
			return qfalse;
		case RSERR_INVALID_MODE:
			Com_Printf( "...WARNING: could not set the given mode (%d)\n", mode );
			return qfalse;
		case RSERR_UNKNOWN:
			Com_Printf( "...ERROR: no display modes could be found.\n" );
			return qfalse;
		default:
			break;
	}

	return qtrue;
}

window_t WIN_Init( graphicsApi_t api, glconfig_t *glConfig )
{
	Cmd_AddCommand("modelist", R_ModeList_f);
	Cmd_AddCommand("minimize", GLimp_Minimize);

	r_sdlDriver			= Cvar_Get( "r_sdlDriver",			"",			CVAR_ROM );

	// Window cvars
	r_fullscreen		= Cvar_Get( "r_fullscreen",			"0",		CVAR_ARCHIVE|CVAR_LATCH );
	r_noborder			= Cvar_Get( "r_noborder",			"0",		CVAR_ARCHIVE|CVAR_LATCH );
	r_centerWindow		= Cvar_Get( "r_centerWindow",		"0",		CVAR_ARCHIVE|CVAR_LATCH );
	r_customwidth		= Cvar_Get( "r_customwidth",		"1600",		CVAR_ARCHIVE|CVAR_LATCH );
	r_customheight		= Cvar_Get( "r_customheight",		"1024",		CVAR_ARCHIVE|CVAR_LATCH );
	r_swapInterval		= Cvar_Get( "r_swapInterval",		"0",		CVAR_ARCHIVE );
	r_stereo			= Cvar_Get( "r_stereo",				"0",		CVAR_ARCHIVE|CVAR_LATCH );
	r_mode				= Cvar_Get( "r_mode",				"4",		CVAR_ARCHIVE|CVAR_LATCH );
	r_displayRefresh	= Cvar_Get( "r_displayRefresh",		"0",		CVAR_LATCH );
	Cvar_CheckRange( r_displayRefresh, 0, 240, qtrue );

	// Window render surface cvars
	r_stencilbits		= Cvar_Get( "r_stencilbits",		"8",		CVAR_ARCHIVE|CVAR_LATCH );
	r_depthbits			= Cvar_Get( "r_depthbits",			"0",		CVAR_ARCHIVE|CVAR_LATCH );
	r_colorbits			= Cvar_Get( "r_colorbits",			"0",		CVAR_ARCHIVE|CVAR_LATCH );
	r_ignorehwgamma		= Cvar_Get( "r_ignorehwgamma",		"0",		CVAR_ARCHIVE|CVAR_LATCH );
	r_ext_multisample	= Cvar_Get( "r_ext_multisample",	"0",		CVAR_ARCHIVE|CVAR_LATCH );

	// Create the window and set up the context
	if(!GLimp_StartDriverAndSetMode( glConfig, api, r_mode->integer,
										(qboolean)r_fullscreen->integer, (qboolean)r_noborder->integer ))
	{
		if( r_mode->integer != R_MODE_FALLBACK )
		{
			Com_Printf( "Setting r_mode %d failed, falling back on r_mode %d\n", r_mode->integer, R_MODE_FALLBACK );

			if (!GLimp_StartDriverAndSetMode( glConfig, api, R_MODE_FALLBACK, qfalse, qfalse ))
			{
				// Nothing worked, give up
				Com_Error( ERR_FATAL, "GLimp_Init() - could not load OpenGL subsystem" );
			}
		}
	}

	glConfig->deviceSupportsGamma =
		(qboolean)(!r_ignorehwgamma->integer && SDL_SetWindowBrightness( screen, 1.0f ) >= 0);

	Cvar_Get( "r_availableModes", "", CVAR_ROM );

	// This depends on SDL_INIT_VIDEO, hence having it here
	IN_Init( screen );

	// window_t is only really useful for Windows if the renderer wants to create a D3D context.
	window_t window = {};

	window.api = api;

#if defined(_WIN32)
	SDL_SysWMinfo info;
	SDL_VERSION(&info.version);

	if ( SDL_GetWindowWMInfo(screen, &info) )
	{	
		switch(info.subsystem) {
			case SDL_SYSWM_WINDOWS:
				window.handle = info.info.win.window;
				break;

			default:
				break;
		}
	}
#endif

	return window;
}

/*
===============
GLimp_Shutdown
===============
*/
void WIN_Shutdown( void )
{
	Cmd_RemoveCommand("modelist");
	Cmd_RemoveCommand("minimize");

	IN_Shutdown();

	SDL_QuitSubSystem( SDL_INIT_VIDEO );
	screen = NULL;
}

void GLimp_EnableLogging( qboolean enable )
{
}

void GLimp_LogComment( char *comment )
{
}

void WIN_SetGamma( glconfig_t *glConfig, byte red[256], byte green[256], byte blue[256] )
{
	Uint16 table[3][256];
	int i, j;

	if( !glConfig->deviceSupportsGamma || r_ignorehwgamma->integer > 0 )
		return;

	for (i = 0; i < 256; i++)
	{
		table[0][i] = ( ( ( Uint16 ) red[i] ) << 8 ) | red[i];
		table[1][i] = ( ( ( Uint16 ) green[i] ) << 8 ) | green[i];
		table[2][i] = ( ( ( Uint16 ) blue[i] ) << 8 ) | blue[i];
	}

#if defined(_WIN32)
	// Win2K and newer put this odd restriction on gamma ramps...
	{
		OSVERSIONINFO	vinfo;

		vinfo.dwOSVersionInfoSize = sizeof( vinfo );
		GetVersionEx( &vinfo );
		if( vinfo.dwMajorVersion >= 5 && vinfo.dwPlatformId == VER_PLATFORM_WIN32_NT )
		{
			Com_DPrintf( "performing gamma clamp.\n" );
			for( j = 0 ; j < 3 ; j++ )
			{
				for( i = 0 ; i < 128 ; i++ )
				{
					table[j][i] = Q_min(table[j][i], (128 + i) << 8);
				}

				table[j][127] = Q_min(table[j][127], 254 << 8);
			}
		}
	}
#endif

	// enforce constantly increasing
	for (j = 0; j < 3; j++)
	{
		for (i = 1; i < 256; i++)
		{
			if (table[j][i] < table[j][i-1])
				table[j][i] = table[j][i-1];
		}
	}

	SDL_SetWindowGammaRamp(screen, table[0], table[1], table[2]);
}

void *WIN_GL_GetProcAddress( const char *proc )
{
	return SDL_GL_GetProcAddress( proc );
}