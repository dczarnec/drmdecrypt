#define __INTEL_COMPILER

#include <tchar.h>
#include <io.h>
#include <stdlib.h>
#include <intrin.h>

#include "XGetopt.h"

#ifndef PATH_MAX
#define PATH_MAX _MAX_PATH
#endif

static char *dirname(char *path)
{
	static char dirn[_MAX_DIR+_MAX_DRIVE+1];
	char dir[_MAX_DIR];
	char drv[_MAX_DRIVE];
	_splitpath_s(path,
		drv, _countof(drv),
		dir, _countof(dir),
		NULL, 0,
		NULL, 0
	);
	_makepath_s(dirn, drv, dir, NULL, NULL);
	return dirn;
}

static char *basename(char *path)
{
	static char name[_MAX_DIR];
	_splitpath_s(path,
		NULL, 0,
		NULL, 0,
		name, _countof(name),
		NULL, 0
		);
	return name;
}
