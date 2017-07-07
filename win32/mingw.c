#include <wchar.h>
#include "libbb.h"
#include <userenv.h>

#if defined(__MINGW64_VERSION_MAJOR)
#if ENABLE_GLOBBING
int _dowildcard = -1;
#else
int _dowildcard = 0;
#endif

#undef _fmode
int _fmode = _O_BINARY;
#endif

#if !defined(__MINGW64_VERSION_MAJOR)
#if ENABLE_GLOBBING
int _CRT_glob = 1;
#else
int _CRT_glob = 0;
#endif

unsigned int _CRT_fmode = _O_BINARY;
#endif

smallint bb_got_signal;

static inline int wisdirsep(wchar_t w)
{
	return w == L'\\' || w == L'/';
}

static wchar_t *pathconv_rest(wchar_t *result, int offset, const char *path)
{
	wchar_t *p, *q, *slash;

	if (!MultiByteToWideChar(CP_UTF8, 0, path, -1,
				result + offset, PATH_MAX_LONG - offset)) {
		errno = ENAMETOOLONG;
		return NULL;
	}

	slash = result + offset - 1;

	/* normalize ., .. and / */
	for (p = result + offset, q = p; *p; p++)
		if (wisdirsep(*p)) {
			/* beginning of a UNC path? */
			if (p == result && wisdirsep(p[1])) {
				*(q++) = L'\\';
				p++;
			}
			*(q++) = L'\\';
			/* condense runs of slashes */
			while (wisdirsep(p[1]))
				p++;
			slash = p;
		} else if (*p == L'.' && slash + 1 == p) {
			/* single . means: same directory */
			if (wisdirsep(p[1]))
				slash = ++p;
			else if (!p[1])
				q = slash;
			/* .. means: parent directory */
			else if (p[1] == L'.' && wisdirsep(p[2])) {
				/* search for parent directory */
				if (q != result && q != result + 1) {
					slash = q - 2;
					while (!wisdirsep(*slash) &&
							slash != result)
						slash--;
					if (slash - result > 2)
						q = slash + 1;
				}

				p += 2;
				slash = p;
			} else if (p[1] == L'.' && !p[2]) {
				/* search for parent directory */
				if (q != result && q != result + 1) {
					slash = q - 2;
					while (!wisdirsep(*slash) &&
							slash != result)
						slash--;
					if (slash - result > 2)
						q = slash;
				} else
					q--;
				break;
			} else
				*(q++) = *p;
		} else
			*(q++) = *p;
	*q = L'\0';


	return result;
}

/* This function is not thread safe. Does it need to be? */
wchar_t *mingw_pathconv(const char *path)
{
	static wchar_t pseudo_root[PATH_MAX];
#define MAX_CONCURRENT_PATHCONV 64
	static wchar_t tmp[MAX_CONCURRENT_PATHCONV][PATH_MAX_LONG];
	static int pseudo_root_len, next;
	wchar_t *result;

	if (!path)
		return NULL;

	if (next >= MAX_CONCURRENT_PATHCONV)
		next = 0;
	result = tmp[next++];

	if (!strcmp(path, "/dev/null"))
		return pathconv_rest(result, 0, "NUL");

	if (path[0] != '/') {
		/* regular absolute path with drive prefix */
		if (isalpha(path[0]) && path[1] == ':') {
			wcscpy(result, L"\\\\?\\");
			return pathconv_rest(result, 4, path);
		}

		/* relative path */
		if (path[0] != '\\') {
			DWORD len;

			/* NUL is special */
			if (!strcasecmp(path, "NUL"))
				return pathconv_rest(result, 0, path);

			wcscpy(result, L"\\\\?\\");
			len = GetCurrentDirectoryW(PATH_MAX_LONG - 4,
					result + 4);
			if (len + 6 >= PATH_MAX_LONG) {
				errno = ENAMETOOLONG;
				return NULL;
			}
			if (!wcsncmp(result + 4, L"\\\\?\\", 4)) {
				/* cwd already has \\?\ prefix */
				result += 4;
				len -= 4;
			}
			if (!len || !isalpha(result[4]) || result[5] != L':') {
				fprintf(stderr, "cwd lacks drive\n");
				errno = EINVAL;
				return NULL;
			}
			result[len + 4] = L'\\';
			return pathconv_rest(result, len + 5, path);
		}

		/* UNC path */
		if (path[1] == '\\')
			return pathconv_rest(result, 0, path);

		/* absolute path missing drive prefix */
		wcscpy(result, L"\\\\?\\");
		/* obtain current directory, just for the drive prefix */
		if (!GetCurrentDirectoryW(PATH_MAX_LONG - 4, result + 4) ||
				!isalpha(result[4]) || result[5] != L':') {
			fprintf(stderr, "Current directory lacks drive\n");
			errno = EINVAL;
			return NULL;
		}
		return pathconv_rest(result, 6, path);
	}

	/* UNC path with forward slashes */
	if (path[1] == '/')
		return pathconv_rest(result, 0, path);

	/* MINGW-style /<drive>/<path> */
	if (isalpha(path[1]) && path[2] == '/') {
		wcscpy(result, L"\\\\?\\");
		result[4] = path[1];
		result[5] = ':';
		return pathconv_rest(result, 6, path + 2);
	}

	/* /tmp/ is mapped to %TEMP% */
	if (!_strnicmp(path + 1, "tmp/", 4)) {
		wchar_t *temp = _wgetenv(L"TEMP");
		size_t len = wcslen(temp);

		if (!isalpha(temp[0]) || temp[1] != L':') {
			fprintf(stderr, "TEMP lacks drive: '%S'\n", temp);
			errno = EINVAL;
			return NULL;
		}
		if (len + 6 >= PATH_MAX_LONG) {
			errno = ENAMETOOLONG;
			return NULL;
		}
		wcscpy(result, L"\\\\?\\");
		wcscpy(result + 4, temp);
		if (result[len + 3] != L'\\')
			result[len++ + 4] = L'\\';
		return pathconv_rest(result, len + 4, path + 5);
	}

	if (!*pseudo_root) {
		const char *exec_path = bb_busybox_exec_path;
		size_t len = strlen(exec_path);

		/* skip \\?\ prefix, if any */
		if (!strncmp(exec_path, "\\\\?\\", 4)) {
			exec_path += 4;
			len -= 4;
		}

		if (len > 5 && !_strnicmp(exec_path + len - 4, ".exe", 4)) {
			len -= 3;
			while (--len && !is_dir_sep(exec_path[len]))
				; /* do nothing */
			if (len > 4 && is_dir_sep(exec_path[len - 4]) &&
			    !_strnicmp(exec_path + len - 3, "bin", 3)) {
				len -= 4;
				if (len > 8 && is_dir_sep(exec_path[len - 8]) &&
				    isdigit(exec_path[len - 1]) &&
				    isdigit(exec_path[len - 2]) &&
				    !_strnicmp(exec_path + len - 7, "mingw", 5))
					len -= 8;
			}
		}

		if (len > PATH_MAX)
			len = PATH_MAX;
		else if (!len) {
			exec_path = "C:\\";
			len = 2;
		}

		wcscpy(pseudo_root, L"\\\\?\\");
		pseudo_root_len = MultiByteToWideChar(CP_UTF8, 0,
				exec_path, len + 1,
				pseudo_root + 4, PATH_MAX_LONG - 4);
		if (!pseudo_root_len) {
			fprintf(stderr, "Could not convert '%.*s'\n",
					(int)len + 1, exec_path);
			errno = EINVAL;
			return NULL;
		}
		pseudo_root_len += 4;
		if (pseudo_root_len + 1 < PATH_MAX_LONG &&
				pseudo_root[pseudo_root_len - 1] != L'\\')
			pseudo_root[pseudo_root_len++] = L'\\';
	}

	memcpy(result, pseudo_root, pseudo_root_len * sizeof(wchar_t));
	return pathconv_rest(result, pseudo_root_len, path + 1);
}

int err_win_to_posix(DWORD winerr)
{
	int error = ENOSYS;
	switch(winerr) {
	case ERROR_ACCESS_DENIED: error = EACCES; break;
	case ERROR_ACCOUNT_DISABLED: error = EACCES; break;
	case ERROR_ACCOUNT_RESTRICTION: error = EACCES; break;
	case ERROR_ALREADY_ASSIGNED: error = EBUSY; break;
	case ERROR_ALREADY_EXISTS: error = EEXIST; break;
	case ERROR_ARITHMETIC_OVERFLOW: error = ERANGE; break;
	case ERROR_BAD_COMMAND: error = EIO; break;
	case ERROR_BAD_DEVICE: error = ENODEV; break;
	case ERROR_BAD_DRIVER_LEVEL: error = ENXIO; break;
	case ERROR_BAD_EXE_FORMAT: error = ENOEXEC; break;
	case ERROR_BAD_FORMAT: error = ENOEXEC; break;
	case ERROR_BAD_LENGTH: error = EINVAL; break;
	case ERROR_BAD_PATHNAME: error = ENOENT; break;
	case ERROR_BAD_PIPE: error = EPIPE; break;
	case ERROR_BAD_UNIT: error = ENODEV; break;
	case ERROR_BAD_USERNAME: error = EINVAL; break;
	case ERROR_BROKEN_PIPE: error = EPIPE; break;
	case ERROR_BUFFER_OVERFLOW: error = ENAMETOOLONG; break;
	case ERROR_BUSY: error = EBUSY; break;
	case ERROR_BUSY_DRIVE: error = EBUSY; break;
	case ERROR_CALL_NOT_IMPLEMENTED: error = ENOSYS; break;
	case ERROR_CANNOT_MAKE: error = EACCES; break;
	case ERROR_CANTOPEN: error = EIO; break;
	case ERROR_CANTREAD: error = EIO; break;
	case ERROR_CANTWRITE: error = EIO; break;
	case ERROR_CRC: error = EIO; break;
	case ERROR_CURRENT_DIRECTORY: error = EACCES; break;
	case ERROR_DEVICE_IN_USE: error = EBUSY; break;
	case ERROR_DEV_NOT_EXIST: error = ENODEV; break;
	case ERROR_DIRECTORY: error = EINVAL; break;
	case ERROR_DIR_NOT_EMPTY: error = ENOTEMPTY; break;
	case ERROR_DISK_CHANGE: error = EIO; break;
	case ERROR_DISK_FULL: error = ENOSPC; break;
	case ERROR_DRIVE_LOCKED: error = EBUSY; break;
	case ERROR_ENVVAR_NOT_FOUND: error = EINVAL; break;
	case ERROR_EXE_MARKED_INVALID: error = ENOEXEC; break;
	case ERROR_FILENAME_EXCED_RANGE: error = ENAMETOOLONG; break;
	case ERROR_FILE_EXISTS: error = EEXIST; break;
	case ERROR_FILE_INVALID: error = ENODEV; break;
	case ERROR_FILE_NOT_FOUND: error = ENOENT; break;
	case ERROR_GEN_FAILURE: error = EIO; break;
	case ERROR_HANDLE_DISK_FULL: error = ENOSPC; break;
	case ERROR_INSUFFICIENT_BUFFER: error = ENOMEM; break;
	case ERROR_INVALID_ACCESS: error = EACCES; break;
	case ERROR_INVALID_ADDRESS: error = EFAULT; break;
	case ERROR_INVALID_BLOCK: error = EFAULT; break;
	case ERROR_INVALID_DATA: error = EINVAL; break;
	case ERROR_INVALID_DRIVE: error = ENODEV; break;
	case ERROR_INVALID_EXE_SIGNATURE: error = ENOEXEC; break;
	case ERROR_INVALID_FLAGS: error = EINVAL; break;
	case ERROR_INVALID_FUNCTION: error = ENOSYS; break;
	case ERROR_INVALID_HANDLE: error = EBADF; break;
	case ERROR_INVALID_LOGON_HOURS: error = EACCES; break;
	case ERROR_INVALID_NAME: error = EINVAL; break;
	case ERROR_INVALID_OWNER: error = EINVAL; break;
	case ERROR_INVALID_PARAMETER: error = EINVAL; break;
	case ERROR_INVALID_PASSWORD: error = EPERM; break;
	case ERROR_INVALID_PRIMARY_GROUP: error = EINVAL; break;
	case ERROR_INVALID_SIGNAL_NUMBER: error = EINVAL; break;
	case ERROR_INVALID_TARGET_HANDLE: error = EIO; break;
	case ERROR_INVALID_WORKSTATION: error = EACCES; break;
	case ERROR_IO_DEVICE: error = EIO; break;
	case ERROR_IO_INCOMPLETE: error = EINTR; break;
	case ERROR_LOCKED: error = EBUSY; break;
	case ERROR_LOCK_VIOLATION: error = EACCES; break;
	case ERROR_LOGON_FAILURE: error = EACCES; break;
	case ERROR_MAPPED_ALIGNMENT: error = EINVAL; break;
	case ERROR_META_EXPANSION_TOO_LONG: error = E2BIG; break;
	case ERROR_MORE_DATA: error = EPIPE; break;
	case ERROR_NEGATIVE_SEEK: error = ESPIPE; break;
	case ERROR_NOACCESS: error = EFAULT; break;
	case ERROR_NONE_MAPPED: error = EINVAL; break;
	case ERROR_NOT_ENOUGH_MEMORY: error = ENOMEM; break;
	case ERROR_NOT_READY: error = EAGAIN; break;
	case ERROR_NOT_SAME_DEVICE: error = EXDEV; break;
	case ERROR_NO_DATA: error = EPIPE; break;
	case ERROR_NO_MORE_SEARCH_HANDLES: error = EIO; break;
	case ERROR_NO_PROC_SLOTS: error = EAGAIN; break;
	case ERROR_NO_SUCH_PRIVILEGE: error = EACCES; break;
	case ERROR_OPEN_FAILED: error = EIO; break;
	case ERROR_OPEN_FILES: error = EBUSY; break;
	case ERROR_OPERATION_ABORTED: error = EINTR; break;
	case ERROR_OUTOFMEMORY: error = ENOMEM; break;
	case ERROR_PASSWORD_EXPIRED: error = EACCES; break;
	case ERROR_PATH_BUSY: error = EBUSY; break;
	case ERROR_PATH_NOT_FOUND: error = ENOENT; break;
	case ERROR_PIPE_BUSY: error = EBUSY; break;
	case ERROR_PIPE_CONNECTED: error = EPIPE; break;
	case ERROR_PIPE_LISTENING: error = EPIPE; break;
	case ERROR_PIPE_NOT_CONNECTED: error = EPIPE; break;
	case ERROR_PRIVILEGE_NOT_HELD: error = EACCES; break;
	case ERROR_READ_FAULT: error = EIO; break;
	case ERROR_SEEK: error = EIO; break;
	case ERROR_SEEK_ON_DEVICE: error = ESPIPE; break;
	case ERROR_SHARING_BUFFER_EXCEEDED: error = ENFILE; break;
	case ERROR_SHARING_VIOLATION: error = EACCES; break;
	case ERROR_STACK_OVERFLOW: error = ENOMEM; break;
	case ERROR_SWAPERROR: error = ENOENT; break;
	case ERROR_TOO_MANY_LINKS: error = EMLINK; break;
	case ERROR_TOO_MANY_MODULES: error = EMFILE; break;
	case ERROR_TOO_MANY_OPEN_FILES: error = EMFILE; break;
	case ERROR_UNRECOGNIZED_MEDIA: error = ENXIO; break;
	case ERROR_UNRECOGNIZED_VOLUME: error = ENODEV; break;
	case ERROR_WAIT_NO_CHILDREN: error = ECHILD; break;
	case ERROR_WRITE_FAULT: error = EIO; break;
	case ERROR_WRITE_PROTECT: error = EROFS; break;
	}
	return error;
}

#undef open
int mingw_open (const char *filename, int oflags, ...)
{
	va_list args;
	unsigned mode;
	int fd;
	wchar_t *wpath;

	va_start(args, oflags);
	mode = va_arg(args, int);
	va_end(args);

	if (oflags & O_NONBLOCK) {
		oflags &= ~O_NONBLOCK;
	}
	wpath = mingw_pathconv(filename);
	if (!wpath)
		return -1;

	fd = _wopen(wpath, oflags, mode);
	if (fd < 0 && (oflags & O_ACCMODE) != O_RDONLY && errno == EACCES) {
		DWORD attrs = GetFileAttributes(filename);
		if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
			errno = EISDIR;
	}
	return fd;
}

#undef fopen
FILE *mingw_fopen (const char *filename, const char *otype)
{
	wchar_t *wpath = mingw_pathconv(filename);
	wchar_t wotype[16];
	if (!wpath)
		return NULL;
	if (!MultiByteToWideChar(CP_UTF8, 0, otype, -1, wotype, 16))
		return NULL;
	return !wpath ? NULL : _wfopen(wpath, wotype);
}

#undef dup2
int mingw_dup2 (int fd, int fdto)
{
	int ret = dup2(fd, fdto);
	return ret != -1 ? fdto : -1;
}

/*
 * The unit of FILETIME is 100-nanoseconds since January 1, 1601, UTC.
 * Returns the 100-nanoseconds ("hekto nanoseconds") since the epoch.
 */
static inline long long filetime_to_hnsec(const FILETIME *ft)
{
	long long winTime = ((long long)ft->dwHighDateTime << 32) + ft->dwLowDateTime;
	/* Windows to Unix Epoch conversion */
	return winTime - 116444736000000000LL;
}

static inline time_t filetime_to_time_t(const FILETIME *ft)
{
	return (time_t)(filetime_to_hnsec(ft) / 10000000);
}

static inline int file_attr_to_st_mode (DWORD attr)
{
	int fMode = S_IREAD;
	if (attr & FILE_ATTRIBUTE_DIRECTORY)
		fMode |= S_IFDIR|S_IWRITE|S_IEXEC;
	else
		fMode |= S_IFREG;
	if (!(attr & FILE_ATTRIBUTE_READONLY))
		fMode |= S_IWRITE;
	return fMode;
}

static inline int get_file_attr(const wchar_t *wpath, WIN32_FIND_DATAW *fdata)
{
	if (GetFileAttributesExW(wpath, GetFileExInfoStandard, fdata))
		return 0;

	switch (GetLastError()) {
	case ERROR_ACCESS_DENIED:
	case ERROR_SHARING_VIOLATION:
	case ERROR_LOCK_VIOLATION:
	case ERROR_SHARING_BUFFER_EXCEEDED:
		return EACCES;
	case ERROR_BUFFER_OVERFLOW:
		return ENAMETOOLONG;
	case ERROR_NOT_ENOUGH_MEMORY:
		return ENOMEM;
	default:
		return ENOENT;
	}
}

/* We keep the do_lstat code in a separate function to avoid recursion.
 * When a path ends with a slash, the stat will fail with ENOENT. In
 * this case, we strip the trailing slashes and stat again.
 *
 * If follow is true then act like stat() and report on the link
 * target. Otherwise report on the link itself.
 */
static int do_lstat(int follow, const wchar_t *wpath, struct mingw_stat *buf)
{
	int err;
	WIN32_FIND_DATAW fdata;
	mode_t usermode;

	if (!(err = get_file_attr(wpath, &fdata))) {
		int len = wcslen(wpath);

		buf->st_ino = 0;
		buf->st_uid = DEFAULT_UID;
		buf->st_gid = DEFAULT_GID;
		buf->st_nlink = 1;
		buf->st_mode = file_attr_to_st_mode(fdata.dwFileAttributes);
		if (len > 4 && (!_wcsicmp(wpath+len-4, L".exe") ||
				!_wcsicmp(wpath+len-4, L".com")))
			buf->st_mode |= S_IEXEC;
		buf->st_size = fdata.nFileSizeLow |
			(((off64_t)fdata.nFileSizeHigh)<<32);
		buf->st_dev = buf->st_rdev = 0; /* not used by Git */
		buf->st_atime = filetime_to_time_t(&(fdata.ftLastAccessTime));
		buf->st_mtime = filetime_to_time_t(&(fdata.ftLastWriteTime));
		buf->st_ctime = filetime_to_time_t(&(fdata.ftCreationTime));
		if (fdata.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
			WIN32_FIND_DATAW findbuf;
			HANDLE handle = FindFirstFileW(wpath, &findbuf);
			if (handle != INVALID_HANDLE_VALUE) {
				if ((findbuf.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
						(findbuf.dwReserved0 == IO_REPARSE_TAG_SYMLINK)) {
					if (follow) {
						//char buffer[MAXIMUM_REPARSE_DATA_BUFFER_SIZE];
						/* TODO: replace by a wide char version of readlink() */
						//buf->st_size = _wreadlink(wpath, buffer, MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
						errno = ENOSYS;
						return -1;
					} else {
						buf->st_mode = S_IFLNK;
					}
					buf->st_mode |= S_IREAD;
					if (!(findbuf.dwFileAttributes & FILE_ATTRIBUTE_READONLY))
						buf->st_mode |= S_IWRITE;
				}
				FindClose(handle);
			}
		}
		usermode = buf->st_mode & S_IRWXU;
		buf->st_mode |= (usermode >> 3) | ((usermode >> 6) & ~S_IWOTH);

		/*
		 * Assume a block is 4096 bytes and calculate number of 512 byte
		 * sectors.
		 */
		buf->st_blksize = 4096;
		buf->st_blocks = ((buf->st_size+4095)>>12)<<3;
		return 0;
	}
	errno = err;
	return -1;
}

/* We provide our own lstat/fstat functions, since the provided
 * lstat/fstat functions are so slow. These stat functions are
 * tailored for Git's usage (read: fast), and are not meant to be
 * complete. Note that Git stat()s are redirected to mingw_lstat()
 * too, since Windows doesn't really handle symlinks that well.
 */
static int do_stat_internal(int follow, const wchar_t *wpath, struct mingw_stat *buf)
{
	int namelen;
	wchar_t alt_name[PATH_MAX_LONG];

	if (!do_lstat(follow, wpath, buf))
		return 0;

	/* if file_name ended in a '/', Windows returned ENOENT;
	 * try again without trailing slashes
	 */
	if (errno != ENOENT)
		return -1;

	namelen = wcslen(wpath);
	if (namelen && wpath[namelen-1] != '/')
		return -1;
	while (namelen && wpath[namelen-1] == '/')
		--namelen;
	if (!namelen || namelen >= PATH_MAX)
		return -1;

	memcpy(alt_name, wpath, namelen * sizeof(wchar_t));
	alt_name[namelen] = 0;
	return do_lstat(follow, alt_name, buf);
}

int mingw_lstat(const char *file_name, struct mingw_stat *buf)
{
	wchar_t *wpath = mingw_pathconv(file_name);
	if (!wpath)
		return -1;
	return do_stat_internal(0, wpath, buf);
}
int mingw_stat(const char *file_name, struct mingw_stat *buf)
{
	wchar_t *wpath = mingw_pathconv(file_name);
	if (!wpath)
		return -1;
	return do_stat_internal(1, wpath, buf);
}

int mingw_fstat(int fd, struct mingw_stat *buf)
{
	HANDLE fh = (HANDLE)_get_osfhandle(fd);
	BY_HANDLE_FILE_INFORMATION fdata;

	if (fh == INVALID_HANDLE_VALUE) {
		errno = EBADF;
		return -1;
	}
	/* direct non-file handles to MS's fstat() */
	if (GetFileType(fh) != FILE_TYPE_DISK) {
		struct _stati64 buf64;

		if ( _fstati64(fd, &buf64) != 0 )  {
			return -1;
		}
		buf->st_dev = 0;
		buf->st_ino = 0;
		buf->st_mode = S_IREAD|S_IWRITE;
		buf->st_nlink = 1;
		buf->st_uid = DEFAULT_UID;
		buf->st_gid = DEFAULT_GID;
		buf->st_rdev = 0;
		buf->st_size = buf64.st_size;
		buf->st_atime = buf64.st_atime;
		buf->st_mtime = buf64.st_mtime;
		buf->st_ctime = buf64.st_ctime;
		buf->st_blksize = 4096;
		buf->st_blocks = ((buf64.st_size+4095)>>12)<<3;
	}

	if (GetFileInformationByHandle(fh, &fdata)) {
		buf->st_ino = 0;
		buf->st_uid = DEFAULT_UID;
		buf->st_gid = DEFAULT_GID;
		/* could use fdata.nNumberOfLinks but it's inconsistent with stat */
		buf->st_nlink = 1;
		buf->st_mode = file_attr_to_st_mode(fdata.dwFileAttributes);
		buf->st_size = fdata.nFileSizeLow |
			(((off64_t)fdata.nFileSizeHigh)<<32);
		buf->st_dev = buf->st_rdev = 0; /* not used by Git */
		buf->st_atime = filetime_to_time_t(&(fdata.ftLastAccessTime));
		buf->st_mtime = filetime_to_time_t(&(fdata.ftLastWriteTime));
		buf->st_ctime = filetime_to_time_t(&(fdata.ftCreationTime));
		buf->st_blksize = 4096;
		buf->st_blocks = ((buf->st_size+4095)>>12)<<3;
		return 0;
	}
	errno = EBADF;
	return -1;
}

static inline void timeval_to_filetime(const struct timeval tv, FILETIME *ft)
{
	long long winTime = ((tv.tv_sec * 1000000LL) + tv.tv_usec) * 10LL + 116444736000000000LL;
	ft->dwLowDateTime = winTime;
	ft->dwHighDateTime = winTime >> 32;
}

int utimes(const char *file_name, const struct timeval tims[2])
{
	FILETIME mft, aft;
	HANDLE fh;
	DWORD flags, attrs;
	int rc;
	wchar_t *wpath = mingw_pathconv(file_name);

	if (!wpath)
		return -1;

	flags = FILE_ATTRIBUTE_NORMAL;

	/* must have write permission */
	attrs = GetFileAttributesW(wpath);
	if ( attrs != INVALID_FILE_ATTRIBUTES ) {
	    if ( attrs & FILE_ATTRIBUTE_READONLY ) {
			/* ignore errors here; open() will report them */
			SetFileAttributesW(wpath, attrs & ~FILE_ATTRIBUTE_READONLY);
		}

	    if ( attrs & FILE_ATTRIBUTE_DIRECTORY ) {
			flags = FILE_FLAG_BACKUP_SEMANTICS;
		}
	}

	fh = CreateFileW(wpath, GENERIC_READ|GENERIC_WRITE,
				FILE_SHARE_READ|FILE_SHARE_WRITE,
				NULL, OPEN_EXISTING, flags, NULL);
	if ( fh == INVALID_HANDLE_VALUE ) {
		errno = err_win_to_posix(GetLastError());
		rc = -1;
		goto revert_attrs;
	}

	if (tims) {
		timeval_to_filetime(tims[0], &aft);
		timeval_to_filetime(tims[1], &mft);
	}
	else {
		GetSystemTimeAsFileTime(&mft);
		aft = mft;
	}
	if (!SetFileTime(fh, NULL, &aft, &mft)) {
		errno = EINVAL;
		rc = -1;
	} else
		rc = 0;
	CloseHandle(fh);

revert_attrs:
	if (attrs != INVALID_FILE_ATTRIBUTES &&
	    (attrs & FILE_ATTRIBUTE_READONLY)) {
		/* ignore errors again */
		SetFileAttributesW(wpath, attrs);
	}
	return rc;
}

unsigned int sleep (unsigned int seconds)
{
	Sleep(seconds*1000);
	return 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
	if (req->tv_nsec < 0 || 1000000000 <= req->tv_nsec) {
		errno = EINVAL;
		return -1;
	}

	Sleep(req->tv_sec*1000 + req->tv_nsec/1000000);

	/* Sleep is not interruptible.  So there is no remaining delay.  */
	if (rem != NULL) {
		rem->tv_sec = 0;
		rem->tv_nsec = 0;
	}

	return 0;
}

/*
 * Windows' mktemp returns NULL on error whereas POSIX always returns the
 * template and signals an error by making it an empty string.
 */
#undef mktemp
char *mingw_mktemp(char *template)
{
	if ( mktemp(template) == NULL ) {
		template[0] = '\0';
	}

	return template;
}

int mkstemp(char *template)
{
	char *filename = mktemp(template);
	if (filename == NULL)
		return -1;
	return open(filename, O_RDWR | O_CREAT, 0600);
}

int gettimeofday(struct timeval *tv, void *tz UNUSED_PARAM)
{
	FILETIME ft;
	long long hnsec;

	GetSystemTimeAsFileTime(&ft);
	hnsec = filetime_to_hnsec(&ft);
	tv->tv_sec = hnsec / 10000000;
	tv->tv_usec = (hnsec % 10000000) / 10;
	return 0;
}

int pipe(int filedes[2])
{
	if (_pipe(filedes, PIPE_BUF, 0) < 0)
		return -1;
	return 0;
}

struct tm *gmtime_r(const time_t *timep, struct tm *result)
{
	/* gmtime() in MSVCRT.DLL is thread-safe, but not reentrant */
	memcpy(result, gmtime(timep), sizeof(struct tm));
	return result;
}

struct tm *localtime_r(const time_t *timep, struct tm *result)
{
	/* localtime() in MSVCRT.DLL is thread-safe, but not reentrant */
	memcpy(result, localtime(timep), sizeof(struct tm));
	return result;
}

#undef getcwd
char *mingw_getcwd(char *pointer, int len)
{
	int i;
	char *ret = getcwd(pointer, len);
	if (!ret)
		return ret;
	if (!strncmp(ret, "\\\\?\\", 4)) {
		for (i = 0; ret[i + 4]; i++)
			ret[i] = ret[i + 4] == '\\' ? '/' : ret[i + 4];
		ret[i] = '\0';
	} else
		for (i = 0; ret[i]; i++)
			if (ret[i] == '\\')
				ret[i] = '/';
	return ret;
}

#undef rename
int mingw_rename(const char *pold, const char *pnew)
{
	DWORD attrs;
	wchar_t *wold = mingw_pathconv(pold);
	wchar_t *wnew = mingw_pathconv(pnew);

	if (!wold || !wnew)
		return -1;

	/*
	 * Try native rename() first to get errno right.
	 * It is based on MoveFile(), which cannot overwrite existing files.
	 */
	if (!_wrename(wold, wnew))
		return 0;
	if (errno != EEXIST)
		return -1;
	if (MoveFileExW(wold, wnew, MOVEFILE_REPLACE_EXISTING))
		return 0;
	/* TODO: translate more errors */
	if (GetLastError() == ERROR_ACCESS_DENIED &&
	    (attrs = GetFileAttributesW(wnew)) != INVALID_FILE_ATTRIBUTES) {
		if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
			errno = EISDIR;
			return -1;
		}
		if ((attrs & FILE_ATTRIBUTE_READONLY) &&
		    SetFileAttributesW(wnew, attrs & ~FILE_ATTRIBUTE_READONLY)) {
			if (MoveFileExW(wold, wnew, MOVEFILE_REPLACE_EXISTING))
				return 0;
			/* revert file attributes on failure */
			SetFileAttributesW(wnew, attrs);
		}
	}
	errno = EACCES;
	return -1;
}

static char *gethomedir(void)
{
	static char buf[PATH_MAX];
	DWORD len = sizeof(buf);
	HANDLE h;
	char *s;

	buf[0] = '\0';
	if ( !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &h) )
		return buf;

	if ( !GetUserProfileDirectory(h, buf, &len) ) {
		CloseHandle(h);
		return buf;
	}

	CloseHandle(h);

	for ( s=buf; *s; ++s ) {
		if ( *s == '\\' ) {
			*s = '/';
		}
	}

	return buf;
}

static char *get_user_name(void)
{
	static char user_name[100] = "";
	char *s;
	DWORD len = sizeof(user_name);

	if ( user_name[0] != '\0' ) {
		return user_name;
	}

	if ( !GetUserName(user_name, &len) ) {
		return NULL;
	}

	for ( s=user_name; *s; ++s ) {
		if ( *s == ' ' ) {
			*s = '_';
		}
	}

	return user_name;
}

struct passwd *getpwnam(const char *name)
{
	const char *myname;

	if ( (myname=get_user_name()) != NULL &&
			strcmp(myname, name) == 0 ) {
		return getpwuid(DEFAULT_UID);
	}

	return NULL;
}

struct passwd *getpwuid(uid_t uid UNUSED_PARAM)
{
	static struct passwd p;

	if ( (p.pw_name=get_user_name()) == NULL ) {
		return NULL;
	}
	p.pw_passwd = (char *)"secret";
	p.pw_gecos = (char *)"unknown";
	p.pw_dir = gethomedir();
	p.pw_shell = NULL;
	p.pw_uid = DEFAULT_UID;
	p.pw_gid = DEFAULT_GID;

	return &p;
}

struct group *getgrgid(gid_t gid UNUSED_PARAM)
{
	static char *members[2] = { NULL, NULL };
	static struct group g;

	if ( (g.gr_name=get_user_name()) == NULL ) {
		return NULL;
	}
	g.gr_passwd = (char *)"secret";
	g.gr_gid = DEFAULT_GID;
	members[0] = g.gr_name;
	g.gr_mem = members;

	return &g;
}

int getgrouplist(const char *user UNUSED_PARAM, gid_t group UNUSED_PARAM,
					gid_t *groups, int *ngroups)
{
	if ( *ngroups == 0 ) {
		*ngroups = 1;
		return -1;
	}

	*ngroups = 1;
	groups[0] = DEFAULT_GID;
	return 1;
}

int getgroups(int n, gid_t *groups)
{
	if ( n == 0 ) {
		return 1;
	}

	groups[0] = DEFAULT_GID;
	return 1;
}

int getlogin_r(char *buf, size_t len)
{
	char *name;

	if ( (name=get_user_name()) == NULL ) {
		return -1;
	}

	if ( strlen(name) >= len ) {
		errno = ERANGE;
		return -1;
	}

	strcpy(buf, name);
	return 0;
}

long sysconf(int name)
{
	if ( name == _SC_CLK_TCK ) {
		return TICKS_PER_SECOND;
	}
	errno = EINVAL;
	return -1;
}

clock_t times(struct tms *buf)
{
	buf->tms_utime = 0;
	buf->tms_stime = 0;
	buf->tms_cutime = 0;
	buf->tms_cstime = 0;

	return 0;
}

int link(const char *oldpath, const char *newpath)
{
	typedef BOOL (WINAPI *T)(wchar_t*, wchar_t*, LPSECURITY_ATTRIBUTES);
	static T create_hard_link = NULL;
	wchar_t *woldpath = mingw_pathconv(oldpath);
	wchar_t *wnewpath = mingw_pathconv(newpath);

	if (!woldpath || !wnewpath)
		return -1;

	if (!create_hard_link) {
		create_hard_link = (T) GetProcAddress(
			GetModuleHandle("kernel32.dll"), "CreateHardLinkW");
		if (!create_hard_link)
			create_hard_link = (T)-1;
	}
	if (create_hard_link == (T)-1) {
		errno = ENOSYS;
		return -1;
	}
	if (!create_hard_link(wnewpath, woldpath, NULL)) {
		errno = err_win_to_posix(GetLastError());
		return -1;
	}
	return 0;
}

char *realpath(const char *path, char *resolved_path)
{
	wchar_t *wpath = mingw_pathconv(path);
	if (!wpath)
		return NULL;
	/* FIXME: need normalization */
	return strcpy(resolved_path, path);
}

const char *get_busybox_exec_path(void)
{
	static char path[PATH_MAX] = "";

	if (!*path)
		GetModuleFileName(NULL, path, PATH_MAX);
	return path;
}

#undef mkdir
int mingw_mkdir(const char *path, int mode UNUSED_PARAM)
{
	int ret;
	struct stat st;
	int lerrno = 0;
	const wchar_t *wpath;

	wpath = mingw_pathconv(path);
	if (!wpath)
		return -1;
	if ((ret = _wmkdir(wpath)) < 0) {
		lerrno = errno;
		if (lerrno == EACCES && !do_stat_internal(1, wpath, &st)) {
			ret = 0;
			lerrno = 0;
		}
	}

	errno = lerrno;
	return ret;
}

#undef chmod
int mingw_chmod(const char *path, int mode)
{
	WIN32_FIND_DATAW fdata;
	wchar_t *wpath = mingw_pathconv(path);

	if (!wpath)
		return -1;

	if ( get_file_attr(wpath, &fdata) == 0 &&
			fdata.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) {
		mode |= 0222;
	}

	return _wchmod(wpath, mode);
}

int fcntl(int fd, int cmd, ...)
{
	va_list arg;
	int result = -1;
	char *fds;
	int target, i, newfd;

	va_start(arg, cmd);

	switch (cmd) {
	case F_GETFD:
	case F_SETFD:
	case F_GETFL:
		/*
		 * Our fake F_GETFL won't matter if the return value is used as
		 *    fcntl(fd, F_SETFL, ret|something);
		 * because F_SETFL isn't supported either.
		 */
		result = 0;
		break;
	case F_DUPFD:
		target = va_arg(arg, int);
		fds = xzalloc(target);
		while ((newfd = dup(fd)) < target && newfd >= 0) {
			fds[newfd] = 1;
		}
		for (i = 0; i < target; ++i) {
			if (fds[i]) {
				close(i);
			}
		}
		free(fds);
		result = newfd;
		break;
	default:
		errno = ENOSYS;
		break;
	}

	va_end(arg);
	return result;
}

#undef unlink
int mingw_unlink(const char *pathname)
{
	wchar_t *wpath = mingw_pathconv(pathname);
	if (!wpath)
		return -1;
	/* read-only files cannot be removed */
	_wchmod(wpath, 0666);
	return _wunlink(wpath);
}

#undef strftime
size_t mingw_strftime(char *buf, size_t max, const char *format, const struct tm *tm)
{
	size_t ret;
	char day[3];
	char *t;
	char *fmt, *newfmt;
	struct tm tm2;
	int m;

	/*
	 * Emulate the '%e' and '%s' formats that Windows' strftime lacks.
	 * Happily, the string that replaces '%e' is two characters long.
	 * '%s' is a bit more complicated.
	 */
	fmt = xstrdup(format);
	for ( t=fmt; *t; ++t ) {
		if ( *t == '%' ) {
			if ( t[1] == 'e' ) {
				if ( tm->tm_mday >= 0 && tm->tm_mday <= 99 ) {
					sprintf(day, "%2d", tm->tm_mday);
				}
				else {
					strcpy(day, "  ");
				}
				memcpy(t++, day, 2);
			}
			else if ( t[1] == 's' ) {
				*t = '\0';
				m = t - fmt;
				tm2 = *tm;
				newfmt = xasprintf("%s%d%s", fmt, (int)mktime(&tm2), t+2);
				free(fmt);
				t = newfmt + m + 1;
				fmt = newfmt;
			}
			else if ( t[1] == 'z' ) {
				char buffer[16] = "";

				*t = '\0';
				m = t - fmt;
				_tzset();
				if ( tm->tm_isdst >= 0 ) {
					int offset = (int)_timezone - (tm->tm_isdst > 0 ? 3600 : 0);
					int hr, min;

					if ( offset > 0 ) {
						buffer[0] = '-';
					}
					else {
						buffer[0] = '+';
						offset = -offset;
					}

					hr = offset / 3600;
					min = (offset % 3600) / 60;
					sprintf(buffer+1, "%02d%02d", hr, min);
				}
				newfmt = xasprintf("%s%s%s", fmt, buffer, t+2);
				free(fmt);
				t = newfmt + m + 1;
				fmt = newfmt;
			}
			else if ( t[1] != '\0' ) {
				++t;
			}
		}
	}

	ret = strftime(buf, max, fmt, tm);
	free(fmt);

	return ret;
}

int stime(time_t *t UNUSED_PARAM)
{
	errno = EPERM;
	return -1;
}

#undef access
int mingw_access(const char *name, int mode)
{
	int ret;
	struct stat s;
	int fd, n, sig;
	unsigned int offset;
	unsigned char buf[1024];
	wchar_t *wpath = mingw_pathconv(name);

	if (!wpath)
		return -1;

	/* Windows can only handle test for existence, read or write */
	if (mode == F_OK || (mode & ~X_OK)) {
		ret = _waccess(wpath, mode & ~X_OK);
		if (ret < 0 || !(mode & X_OK)) {
			return ret;
		}
	}

	if (!do_stat_internal(1, wpath, &s) && S_ISREG(s.st_mode)) {

		/* stat marks .exe and .com files as executable */
		if ((s.st_mode&S_IEXEC)) {
			return 0;
		}

		fd = _wopen(wpath, O_RDONLY);
		if (fd < 0)
			return -1;
		n = read(fd, buf, sizeof(buf)-1);
		close(fd);
		if (n < 4)	/* at least '#!/x' and not error */
			return -1;

		/* shell script */
		if (buf[0] == '#' && buf[1] == '!') {
			return 0;
		}

		/*
		 * Poke about in file to see if it's a PE binary.  I've just copied
		 * the magic from the file command.
		 */
		if (buf[0] == 'M' && buf[1] == 'Z') {
			offset = (buf[0x19] << 8) + buf[0x18];
			if (offset > 0x3f) {
				offset = (buf[0x3f] << 24) + (buf[0x3e] << 16) +
							(buf[0x3d] << 8) + buf[0x3c];
				if (offset < sizeof(buf)-100) {
					if (memcmp(buf+offset, "PE\0\0", 4) == 0) {
						sig = (buf[offset+25] << 8) + buf[offset+24];
						if (sig == 0x10b || sig == 0x20b) {
							sig = (buf[offset+23] << 8) + buf[offset+22];
							if ((sig & 0x2000) != 0) {
								/* DLL */
								return -1;
							}
							sig = buf[offset+92];
							return !(sig == 1 || sig == 2 ||
										sig == 3 || sig == 7);
						}
					}
				}
			}
		}
	}

	return -1;
}

#undef rmdir
int mingw_rmdir(const char *path)
{
	wchar_t *wpath = mingw_pathconv(path);

	if (!wpath)
		return -1;

	/* read-only directories cannot be removed */
	_wchmod(wpath, 0666);
	return _wrmdir(wpath);
}

int has_exe_suffix(const char *name)
{
	int len = strlen(name);

	return len > 4 && (!strcasecmp(name+len-4, ".exe") ||
				!strcasecmp(name+len-4, ".com"));
}

/* check if path can be made into an executable by adding a suffix;
 * return an allocated string containing the path if it can;
 * return NULL if not.
 *
 * if path already has a suffix don't even bother trying
 */
char *file_is_win32_executable(const char *p)
{
	char *path;

	if (has_exe_suffix(p)) {
		return NULL;
	}

	path = xasprintf("%s.exe", p);
	if (file_is_executable(path)) {
		return path;
	}

	memcpy(path+strlen(p), ".com", 5);
	if (file_is_executable(path)) {
		return path;
	}

	free(path);

	return NULL;
}

#undef opendir
DIR *mingw_opendir(const char *path)
{
	char name[4];
	wchar_t *wpath;
	size_t wpath_len;
	DIR *ret;

	if (isalpha(path[0]) && path[1] == ':' && path[2] == '\0') {
		strcpy(name, path);
		name[2] = '/';
		name[3] = '\0';
		path = name;
	}

	wpath = mingw_pathconv(path);
	if (!wpath)
		return NULL;

	wpath_len = wcslen(wpath);
	if (wpath_len && wpath[wpath_len - 1] == L'\\')
		wpath_len--;
	if (wpath_len + 3 >= PATH_MAX_LONG) {
		errno = ENAMETOOLONG;
		return NULL;
	}
	wcscpy(wpath + wpath_len, L"\\*");

	ret = malloc(sizeof(*ret));
	if (!ret) {
		errno = ENOMEM;
		return NULL;
	}

	ret->handle = FindFirstFileW(wpath, &ret->find_data);
	if (ret->handle == INVALID_HANDLE_VALUE) {
		if (GetLastError() != ERROR_FILE_NOT_FOUND) {
			free(ret);
			errno = ENOENT;
			return NULL;
		}
		ret->find_data.cFileName[0] = L'\0';
	}

	return ret;
}

#undef readdir
struct dirent *mingw_readdir(DIR *dir)
{
	if (!dir->find_data.cFileName[0])
		return NULL;

	WideCharToMultiByte(CP_UTF8, 0, dir->find_data.cFileName, -1,
			dir->dirent.d_name, PATH_MAX_LONG, NULL, NULL);
	if (!FindNextFileW(dir->handle, &dir->find_data))
		dir->find_data.cFileName[0] = L'\0';

	return &dir->dirent;
}

#undef closedir
int mingw_closedir(DIR *dir)
{
	int ret = 0;

	if (!FindClose(dir->handle)) {
		errno = EBADF;
		ret = -1;
	}
	free(dir);

	return ret;
}

off_t mingw_lseek(int fd, off_t offset, int whence)
{
	HANDLE h = (HANDLE)_get_osfhandle(fd);
	if (h == INVALID_HANDLE_VALUE) {
		errno = EBADF;
		return -1;
	}
	if (GetFileType(h) != FILE_TYPE_DISK) {
		errno = ESPIPE;
		return -1;
	}
	return _lseeki64(fd, offset, whence);
}

#if ENABLE_FEATURE_PS_TIME || ENABLE_FEATURE_PS_LONG
#undef GetTickCount64
#include "lazyload.h"

ULONGLONG CompatGetTickCount64(void)
{
	DECLARE_PROC_ADDR(kernel32.dll, ULONGLONG, GetTickCount64, void);

	if (!INIT_PROC_ADDR(GetTickCount64)) {
		return (ULONGLONG)GetTickCount();
	}

	return GetTickCount64();
}
#endif
