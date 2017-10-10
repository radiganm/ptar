/*
 * Read and Write Plain Text File Archives
 * Written in 2013.  See AUTHORS for a list of authors.
 *
 * To the extent possible under law, the author(s) have dedicated all copyright
 * and related and neighboring rights to this software to the public domain
 * worldwide. This software is distributed without any warranty.
 *
 * You should have received a copy of the CC0 Public Domain Dedication along
 * with this software. If not, see
 * <http://creativecommons.org/publicdomain/zero/1.0/>.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <ftw.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef	WRITE_BLOCKSIZE
#define	WRITE_BLOCKSIZE	32768
#endif	/* WRITE_BLOCKSIZE */

#ifndef   REQUESTED_FILES_GROWTH
#define   REQUESTED_FILES_GROWTH   8
#endif    /* REQUESTED_FILES_GROWTH */

static char linkpath[8192], verbose, extracttostdout;
static long openmax;

/* file type IDs */
enum { UNKNOWN, REGULARFILE, DIRECTORY, SYMLINK, CHARDEVICE, BLOCKDEVICE, FIFO, SOCKET };

/* stdout identifying info (from fstat(2)) */
static dev_t stdoutdev;
static ino_t stdoutino;

/* fseek(3) optimization (for 't' command) */
int skip_file_data_fread(size_t lineno);
int skip_file_data_fseek(size_t lineno);
int (*skip_file_data)(size_t) = skip_file_data_fseek;

/* file selection (for 'x' command) */
typedef struct requested_file {
	char *path_pattern;
	char found;
} requested_file_t;
int extract_if_requested_file(const char *file_path);
int (*should_extract_file)(const char *);
requested_file_t *requested_files;
size_t num_requested_files;
size_t requested_files_cap;

/* file entry metadata */
static char *fpath;
static int ftype = UNKNOWN;	/* see the enum above */
static size_t fsize;	/* for regular files only */
static char *flinktarget;	/* for symlinks only; 0 if not given */
static long fmajor = -1;	/* for devices only; -1 if not given */
static long fminor = -1;	/* for devices only; -1 if not given */
static char *fusername;	/* 0 if not given */
static char *fgroupname;	/* 0 if not given */
static uid_t fuid;
static gid_t fgid;
static mode_t fmode;
static time_t fmtime;

/* nonzero if specified, 0 otherwise */
static char fsizegiven, fuidgiven, fgidgiven, fmodegiven, fmtimegiven;

char *safe_strdup(const char *s) {
	char *ret;

	if ((ret = strdup(s)) == NULL) {
		(void) fprintf(stderr, "out of memory\n");
		exit(EXIT_FAILURE);
	}
	return ret;
}

long open_max(void) {
	if (openmax == 0) {
		errno = 0;
		if ((openmax = sysconf(_SC_OPEN_MAX)) < 0) {
			if (errno == 0) {
				openmax = 256;
			} else {
				perror("error: couldn't determine maximum number of open files");
				exit(EXIT_FAILURE);
			}
		}
	}
	return openmax;
}

int isvalidkeychar(char c) {
	return isalnum((unsigned char)c) || c == ' ' || c == '-' || c == '_';
}

int isvalidkey(const char *start) {
	if (!isalnum((unsigned char)*start)) {
		return 0;
	}
	for (start++; *start != ':' && *start != '\0'; start++) {
		if (!isvalidkeychar(*start)) {
			return 0;
		}
	}
	return 1;
}

int haskey(const char *line) {
	return strchr(line, ':') != NULL && isvalidkey(line);
}

void transformkey(char *start) {
	char *next;
	for (next = start; *start != '\0'; start++, next++) {
		while (isspace((unsigned char)*next) && *next != '\0') {
			next++;
		}
		*start = tolower(*next);
	}
}

char *trim(char *str) {
	size_t len;

	while (isspace((unsigned char)*str)) {
		str++;
	}
	if (*str != '\0') {
		len = strlen(str);
		for (len--; isspace((unsigned char)str[len]); len--) {
			str[len] = '\0';
		}
	}
	return str;
}

int parsemetadata(char *line, char **key, char **value) {
	char *colon;

	colon = strchr(line, ':');
	if (colon && isvalidkey(line)) {
		*colon = '\0';
		*value = trim(colon + 1);
		transformkey(line);
		*key = line;
		return 0;
	}
	colon = trim(line);
	if (*colon != '\0') {
		*key = NULL;
		*value = colon;
		return 0;
	}
	return 1;
}

void write_error(void) {
	(void) fprintf(stderr, "error: couldn't write to standard output: %s\n", strerror(errno));
	exit(EXIT_FAILURE);
}

void printline(const char *line) {
	if (fprintf(stdout, "%s\n", line) < 0) {
		write_error();
	}
}

void write_metadata(const char *key, const char *value) {
	if (fprintf(stdout, "%s:\t%s\n", key, value) < 0) {
		write_error();
	}
}

void write_numeric_metadata(const char *key, unsigned long long value) {
	if (fprintf(stdout, "%s:\t%llu\n", key, value) < 0) {
		write_error();
	}
}

void write_octal_metadata(const char *key, unsigned int value) {
	if (fprintf(stdout, "%s:\t%.7o\n", key, value) < 0) {
		write_error();
	}
}

void write_blank(void) {
	if (fputc('\n', stdout) == EOF) {
		write_error();
	}
}

void write_divider(void) {
	if (fputs("---\n", stdout) == EOF) {
		write_error();
	}
}

int add_file(const char *fname, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
	FILE *fp;
	struct passwd *passwordinfo;
	struct group *groupinfo;
	char buffer[WRITE_BLOCKSIZE];
	size_t numread;

	/* skip the file if it's the same as stdout (avoids infinite loops) or
	   the file is the current directory (avoids an unnecessary entry) */
	if ((sb->st_dev == stdoutdev && sb->st_ino == stdoutino) || strcmp(fname, ".") == 0) {
		return 0;
	}

	if (verbose) {
		if (fprintf(stderr, "%s\n", fname) < 0) {
			perror("stderr");
			return 1;
		}
	}
	if ((passwordinfo = getpwuid(sb->st_uid)) == NULL || (groupinfo = getgrgid(sb->st_gid)) == NULL) {
		perror(fname);
		return 1;
	}
	fp = NULL;
	write_blank();
	write_metadata("Path", fname);
	if (S_ISREG(sb->st_mode)) {
		write_metadata("Type", "Regular File");
		write_numeric_metadata("File Size", sb->st_size);
		if ((fp = fopen(fname, "r")) == NULL) {
			perror(fname);
			return 1;
		}
	} else if (S_ISDIR(sb->st_mode)) {
		write_metadata("Type", "Directory");
	} else if (S_ISLNK(sb->st_mode)) {
		write_metadata("Type", "Symbolic Link");
		if (readlink(fname, linkpath, sizeof (linkpath) - 1) == -1) {
			perror(fname);
			return 1;
		}
		write_metadata("Link Target", linkpath);
	} else if (S_ISCHR(sb->st_mode)) {
		write_metadata("Type", "Character Device");
		write_numeric_metadata("Major", major(sb->st_rdev));
		write_numeric_metadata("Minor", minor(sb->st_rdev));
	} else if (S_ISBLK(sb->st_mode)) {
		write_metadata("Type", "Block Device");
		write_numeric_metadata("Major", major(sb->st_rdev));
		write_numeric_metadata("Minor", minor(sb->st_rdev));
	} else if (S_ISFIFO(sb->st_mode)) {
		write_metadata("Type", "FIFO");
	} else if (S_ISSOCK(sb->st_mode)) {
		write_metadata("Type", "Socket");
	} else {
		(void) fprintf(stderr, "%s: illegal file type\n", fname);
		return 1;
	}
	write_metadata("User Name", passwordinfo->pw_name);
	write_numeric_metadata("User ID", sb->st_uid);
	write_metadata("Group Name", groupinfo->gr_name);
	write_numeric_metadata("Group ID", sb->st_gid);
	write_octal_metadata("Permissions", sb->st_mode & ~S_IFMT);
	write_numeric_metadata("Modification Time", sb->st_mtime);
	if (fp) {
		write_divider();
		while (!feof(fp)) {
			numread = fread(buffer, 1, sizeof (buffer), fp);
			if (ferror(fp)) {
				perror(fname);
				return 1;
			}
			if (fwrite(buffer, 1, numread, stdout) != numread) {
				perror(fname);
				fclose(fp);
				return 1;
			}
		}
		fclose(fp);
		write_divider();
	}
	return 0;
}

int archive_file(const char *fname) {
	struct stat sb;

	if (lstat(fname, &sb) != 0) {
		perror(fname);
		return 1;
	} else if (S_ISDIR(sb.st_mode)) {
		return nftw(fname, add_file, open_max(), FTW_PHYS);
	}
	return add_file(fname, &sb, 0, NULL);
}

int handle_metadata(size_t lineno, char *key, char *value) {
	char *end;

	if (*value == '\0') {
		(void) fprintf(stderr, "stdin:%zu: empty metadata values are not allowed\n", lineno);
		return 1;
	}
	if (strcmp(key, "path") == 0) {
		if (fpath) {
			(void) fprintf(stderr, "stdin:%zu: file path already specified\n", lineno);
			return 1;
		}
		fpath = safe_strdup(value);
	} else if (strcmp(key, "type") == 0) {
		if (ftype != UNKNOWN) {
			(void) fprintf(stderr, "stdin:%zu: file type already specified\n", lineno);
			return 1;
		}
		transformkey(value);
		if (strcmp(value, "regularfile") == 0) {
			ftype = REGULARFILE;
		} else if (strcmp(value, "directory") == 0) {
			ftype = DIRECTORY;
		} else if (strcmp(value, "symboliclink") == 0) {
			ftype = SYMLINK;
		} else if (strcmp(value, "characterdevice") == 0) {
			ftype = CHARDEVICE;
		} else if (strcmp(value, "blockdevice") == 0) {
			ftype = BLOCKDEVICE;
		} else if (strcmp(value, "fifo") == 0) {
			ftype = FIFO;
		} else if (strcmp(value, "socket") == 0) {
			ftype = SOCKET;
		} else {
			(void) fprintf(stderr, "stdin:%zu: unrecognized file type: %s\n", lineno, value);
			return 1;
		}
	} else if (strcmp(key, "filesize") == 0) {
		if (fsizegiven) {
			(void) fprintf(stderr, "stdin:%zu: file size already specified\n", lineno);
			return 1;
		}
		errno = 0;
		fsize = strtoull(value, &end, 10);
		if (errno != 0 || end == value || *end != '\0') {
			(void) fprintf(stderr, "stdin:%zu: invalid file size: %s\n", lineno, value);
			return 1;
		}
		fsizegiven = 1;
	} else if (strcmp(key, "linktarget") == 0) {
		if (flinktarget != NULL) {
			(void) fprintf(stderr, "stdin:%zu: link target already specified\n", lineno);
			return 1;
		}
		flinktarget = safe_strdup(value);
	} else if (strcmp(key, "major") == 0) {
		if (fmajor != -1) {
			(void) fprintf(stderr, "stdin:%zu: major already specified\n", lineno);
			return 1;
		}
		errno = 0;
		fmajor = strtol(value, &end, 10);
		if (errno != 0 || end == value || *end != '\0') {
			(void) fprintf(stderr, "stdin:%zu: invalid major: %s\n", lineno, value);
			return 1;
		}
	} else if (strcmp(key, "minor") == 0) {
		if (fminor != -1) {
			(void) fprintf(stderr, "stdin:%zu: file path already specified\n", lineno);
			return 1;
		}
		errno = 0;
		fminor = strtol(value, &end, 10);
		if (errno != 0 || end == value || *end != '\0') {
			(void) fprintf(stderr, "stdin:%zu: invalid minor: %s\n", lineno, value);
			return 1;
		}
	} else if (strcmp(key, "username") == 0) {
		if (fusername != NULL) {
			(void) fprintf(stderr, "stdin:%zu: username already specified\n", lineno);
			return 1;
		}
		fusername = safe_strdup(value);
	} else if (strcmp(key, "userid") == 0) {
		if (fuidgiven) {
			(void) fprintf(stderr, "stdin:%zu: uid already specified\n", lineno);
			return 1;
		}
		errno = 0;
		fuid = strtoul(value, &end, 10);
		if (errno != 0 || end == value || *end != '\0') {
			(void) fprintf(stderr, "stdin:%zu: invalid uid: %s\n", lineno, value);
			return 1;
		}
		fuidgiven = 1;
	} else if (strcmp(key, "groupname") == 0) {
		if (fgroupname != NULL) {
			(void) fprintf(stderr, "stdin:%zu: groupname already specified\n", lineno);
			return 1;
		}
		fgroupname = safe_strdup(value);
	} else if (strcmp(key, "groupid") == 0) {
		if (fgidgiven) {
			(void) fprintf(stderr, "stdin:%zu: gid already specified\n", lineno);
			return 1;
		}
		errno = 0;
		fgid = strtoul(value, &end, 10);
		if (errno != 0 || end == value || *end != '\0') {
			(void) fprintf(stderr, "stdin:%zu: invalid gid: %s\n", lineno, value);
			return 1;
		}
		fgidgiven = 1;
	} else if (strcmp(key, "permissions") == 0) {
		if (fmodegiven) {
			(void) fprintf(stderr, "stdin:%zu: file permissions already specified\n", lineno);
			return 1;
		}
		errno = 0;
		fmode = strtoul(value, &end, 8);
		if (errno != 0 || end == value || *end != '\0' || (fmode & S_IFMT) != 0) {
			(void) fprintf(stderr, "stdin:%zu: invalid file permissions: %s\n", lineno, value);
			return 1;
		}
		fmodegiven = 1;
	} else if (strcmp(key, "modificationtime") == 0) {
		if (fmtimegiven) {
			(void) fprintf(stderr, "stdin:%zu: file modification time already specified\n", lineno);
			return 1;
		}
		errno = 0;
		fmtime = strtoull(value, &end, 10);
		if (errno != 0 || end == value || *end != '\0') {
			(void) fprintf(stderr, "stdin:%zu: invalid file modification time: %s\n", lineno, value);
			return 1;
		}
		fmtimegiven = 1;
	} else {
		(void) fprintf(stderr, "stdin:%zu: unrecognized metadata key name: %s\n", lineno, key);
		return 1;
	}
	return 0;
}

int is_invalid_metadata(void) {
	if (!fpath) {
		return 1;
	}
	switch (ftype) {
	case UNKNOWN:
		return 1;
	case REGULARFILE:
		if (!fsizegiven) {
			return 1;
		}
		break;
	case DIRECTORY:
		break;
	case SYMLINK:
		if (!flinktarget) {
			return 1;
		}
		break;
	case CHARDEVICE:
	case BLOCKDEVICE:
		if (fmajor == -1 || fminor == -1) {
			return 1;
		}
		break;
	case FIFO:
		break;
	case SOCKET:
		break;
	default:
		abort();
		break;
	}
	return !fuidgiven || !fgidgiven || !fusername || !fgroupname || !fmtimegiven || !fmodegiven;
}

void clear_metadata(void) {
	free(fpath);
	fpath = NULL;
	ftype = UNKNOWN;
	free(flinktarget);
	flinktarget = NULL;
	fmajor = -1;
	fminor = -1;
	free(fusername);
	fusername = NULL;
	free(fgroupname);
	fgroupname = NULL;
	fsizegiven = fuidgiven = fgidgiven = fmodegiven = fmtimegiven = 0;
}

/* archive parser states */
enum { SEEKING_METADATA, METADATA, CONTENTS_END };

int scan_archive(int (*onentry)(size_t)) {
	char *line, *key, *value;
	size_t linecap, lineno;
	ssize_t numread;
	int state;

	/* archive metadata first */
	for (line = NULL, lineno = 1; (numread = getline(&line, &linecap, stdin)) != -1; lineno++) {
		if (parsemetadata(line, &key, &value) != 0) {
			break;
		}
		if (!key && value) {
			(void) fprintf(stderr, "stdin:%zu: illegal archive metadata key-value pair (missing key)\n", lineno);
			return 1;
		}
		if (strcmp(key, "metadataencoding") == 0) {
			transformkey(value);
			if (strcmp(value, "utf-8") != 0 && strcmp(value, "utf8") != 0 && strcmp(value, "ascii") != 0) {
				(void) fprintf(stderr, "stdin:%zu: unrecognized metadata encoding: %s\n", lineno, value);
				return 1;
			}
		} else if (strcmp(key, "extensions") == 0) {
			transformkey(value);
			if (*value != '\0') {
				(void) fprintf(stderr, "stdin:%zu: unrecognized extensions: %s\n", lineno, value);
				return 1;
			}
		} else if (strcmp(key, "archivecreationdate") != 0) {
			(void) fprintf(stderr, "stdin:%zu: unrecognized archive metadata key: %s\n", lineno, key);
			return 1;
		}
	}
	if (ferror(stdin)) {
		(void) fprintf(stderr, "stdin:%zu: %s\n", lineno, strerror(errno));
		return 1;
	} else if (feof(stdin)) {
		return 0;
	}

	/* now for the file entries */
	state = SEEKING_METADATA;
	for (; (numread = getline(&line, &linecap, stdin)) != -1; lineno++) {
		switch (state) {
		case SEEKING_METADATA:
			if (parsemetadata(line, &key, &value) == 0) {
				if (key == NULL) {
					(void) fprintf(stderr, "stdin:%zu: invalid metadata key-value pair (missing key)\n", lineno);
					return 1;
				} else {
					if (handle_metadata(lineno, key, value)) {
						return 1;
					}
					state = METADATA;
				}
			}
			break;
		case METADATA:
			if (parsemetadata(line, &key, &value) == 0) {
				if (key) {
					if (handle_metadata(lineno, key, value)) {
						return 1;
					}
				} else {
					if (strcmp(value, "---") == 0) {
						if (ftype != REGULARFILE) {
							(void) fprintf(stderr, "stdin:%zu: file contents marker found for non-regular file\n", lineno);
							return 1;
						} else if (!fsizegiven) {
							(void) fprintf(stderr, "stdin:%zu: file contents marker found but no file size specified\n", lineno);
							return 1;
						} else if (onentry(lineno)) {
							return 1;
						}
						clear_metadata();
						state = CONTENTS_END;
					} else {
						(void) fprintf(stderr, "stdin:%zu: invalid metadata key-value pair (missing key)\n", lineno);
						return 1;
					}
				}
			} else {
				if (ftype == REGULARFILE) {
					(void) fprintf(stderr, "stdin:%zu: end of regular file metadata reached but no file contents\n", lineno);
					return 1;
				} else if (onentry(lineno)) {
					return 1;
				}
				clear_metadata();
				state = SEEKING_METADATA;
			}
			break;
		case CONTENTS_END:
			if (parsemetadata(line, &key, &value) == 0) {
				if (key) {
					(void) fprintf(stderr, "stdin:%zu: unexpected metadata (expected end-of-file-contents marker \"---\")\n", lineno);
					return 1;
				} else if (strcmp(value, "---") != 0) {
					(void) fprintf(stderr, "stdin:%zu: unexpected additional file data found (expected end-of-file contents marker \"---\" after %zu bytes)\n", lineno, fsize);
					return 1;
				}
				state = SEEKING_METADATA;
			} else {
				(void) fprintf(stderr, "stdin:%zu: unexpected additional file data found (expected end-of-file contents marker \"---\" after %zu bytes)\n", lineno, fsize);
				return 1;
			}
			break;
		default:
			abort();
			break;
		}
	}
	if (ferror(stdin)) {
		(void) fprintf(stderr, "stdin:%zu: %s\n", lineno, strerror(errno));
		return 1;
	} else if (state == METADATA) {
		if (ftype == REGULARFILE) {
			(void) fprintf(stderr, "stdin:%zu: end-of-file reached before reading file contents\n", lineno);
			return 1;
		} else if (onentry(lineno)) {
			return 1;
		}
		clear_metadata();
	} else if (state == CONTENTS_END) {
		(void) fprintf(stderr, "stdin:%zu: end-of-file reached while reading file contents\n", lineno);
		return 1;
	}
	free(line);
	return 0;
}

int skip_file_data_fread(size_t lineno) {
	char buffer[WRITE_BLOCKSIZE];
	size_t numread, numleft;

	for (numleft = fsize; numleft > 0; ) {
		numread = fread(buffer, 1, numleft < sizeof (buffer) ? numleft : sizeof (buffer), stdin);
		numleft -= numread;
		if (ferror(stdin)) {
			(void) fprintf(stderr, "stdin:%zu: error while reading: %s\n", lineno, strerror(errno));
			return 1;
		} else if (feof(stdin) && numleft > 0) {
			(void) fprintf(stderr, "stdin:%zu: end-of-file reached while reading file contents (bad file size?)\n", lineno);
			return 1;
		}
	}
	return 0;
}

int skip_file_data_fseek(size_t lineno) {
	int ret;
	if (fsize <= (size_t)LONG_MAX) {
		if ((ret = fseek(stdin, (long)fsize, SEEK_CUR)) != 0) {
			if (errno == EBADF || errno == ESPIPE) {
				/* fall back on read(2) if fseek(3) fails on stdin */
				skip_file_data = skip_file_data_fread;
				return skip_file_data_fread(lineno);
			} else {
				(void) fprintf(stderr, "stdin:%zu: error while reading: %s\n", lineno, strerror(errno));
				return 1;
			}
		}
		return ret;
	} else {
		return skip_file_data_fread(lineno);
	}
}

int extract_file_contents(size_t lineno, FILE *fp) {
	char buffer[WRITE_BLOCKSIZE];
	size_t numleft, numread;

	for (numleft = fsize; numleft; ) {
		numread = fread(buffer, 1, numleft < sizeof (buffer) ? numleft : sizeof (buffer), stdin);
		numleft -= numread;
		if (ferror(stdin)) {
			(void) fprintf(stderr, "stdin:%zu: error while reading: %s\n", lineno, strerror(errno));
			return 1;
		} else if (feof(stdin) && numleft > 0) {
			(void) fprintf(stderr, "stdin:%zu: end-of-file reached while reading file contents (bad file size?)\n", lineno);
			return 1;
		} else if (fwrite(buffer, 1, numread, fp) != numread) {
			perror(fpath);
			return 1;
		}
	}
	if (fp != stdout) {
		(void) fclose(fp);
	}
	return 0;
}

int listfiles(size_t lineno) {
	if (is_invalid_metadata()) {
		(void) fprintf(stderr, "stdin:%zu: incomplete file metadata\n", lineno);
		return 1;
	}
	printline(fpath);
	if (ftype == REGULARFILE) {
		return skip_file_data(lineno);
	}
	return 0;
}

int extract_if_requested_file(const char *file_path) {
	size_t n;
	int result;

	for (n = 0; n < num_requested_files; n++) {
		if ((result = fnmatch(requested_files[n].path_pattern, file_path, 0)) == 0) {
			requested_files[n].found = 1;
			return 1;
		} else if (result == FNM_NOSYS) {
			(void) fprintf(stderr, "error: fnmatch(3) is not implemented on your system; cannot extract individual files\n");
			exit(EXIT_FAILURE);
		}
	}
	return 0;
}

int extract(size_t lineno) {
	FILE *fp;
	struct stat sb;
	struct timespec times[2];
	int result;

	if (is_invalid_metadata()) {
		(void) fprintf(stderr, "stdin:%zu: incomplete file metadata\n", lineno);
		return 1;
	}
	if (should_extract_file == NULL || should_extract_file(fpath)) {
		if (verbose) {
			if (fprintf(stderr, "%s\n", fpath) < 0) {
				perror("stderr");
				return 1;
			}
		}
		if (extracttostdout) {
			return ftype == REGULARFILE ? extract_file_contents(lineno, stdout) : 0;
		}
		fp = NULL;
		if (ftype != DIRECTORY && unlink(fpath) != 0 && errno != ENOENT) {
			perror(fpath);
			return 1;
		}
		switch (ftype) {
		case REGULARFILE:
			result = 0;
			if ((fp = fopen(fpath, "w")) == NULL) {
				result = 1;
			}
			break;
		case DIRECTORY:
			if ((result = mkdir(fpath, fmode)) != 0 && errno == EEXIST) {
				if (lstat(fpath, &sb) == 0 && S_ISDIR(sb.st_mode)) {
					result = chmod(fpath, fmode);
				}
			}
			break;
		case SYMLINK:
			result = symlink(flinktarget, fpath);
			break;
		case CHARDEVICE:
			result = mknod(fpath, S_IFCHR | fmode, makedev(fmajor, fminor));
			break;
		case BLOCKDEVICE:
			result = mknod(fpath, S_IFBLK | fmode, makedev(fmajor, fminor));
			break;
		case FIFO:
			result = mkfifo(fpath, fmode);
			break;
		case SOCKET:
			result = mknod(fpath, S_IFSOCK | fmode, makedev(fmajor, fminor));
			break;
		default:
			abort();
			break;
		}
		if (result) {
			perror(fpath);
			return 1;
		}
		if (fp) {
			if (extract_file_contents(lineno, fp) != 0) {
				return 1;
			}
			if (chmod(fpath, fmode) != 0) {
				perror(fpath);
				return 1;
			}
		}
		times[0].tv_sec = 0;
		times[0].tv_nsec = UTIME_OMIT;
		times[1].tv_sec = fmtime;
		times[1].tv_nsec = 0;
		if (utimensat(AT_FDCWD, fpath, times, 0) != 0) {
			perror(fpath);
			return 1;
		}
		if (lchown(fpath, fuid, fgid) != 0) {
			perror(fpath);
			return 1;
		}
	} else if (ftype == REGULARFILE) {
		return skip_file_data(lineno);
	}
	return 0;
}

int add_requested_path(const char *file_path) {
	if (num_requested_files == requested_files_cap && (requested_files 
      = reinterpret_cast<requested_file_t *>(realloc(requested_files, (requested_files_cap += REQUESTED_FILES_GROWTH) * sizeof (*requested_files)))) == NULL) {
		(void) fprintf(stderr, "out of memory\n");
		exit(EXIT_FAILURE);
	}
	requested_files[num_requested_files].path_pattern = safe_strdup(file_path);
	requested_files[num_requested_files++].found = 0;
	return 0;
}

int process_stdin_lines(int (*process_line)(const char *)) {
	int error;
	char *line;
	size_t linecap;
	ssize_t numread;

	error = 0;
	line = NULL;
	while (!error && (numread = getline(&line, &linecap, stdin)) != -1) {
		if (numread > 0) {
			if (line[numread - 1] == '\n') {
				line[numread - 1] = '\0';
			}
			if (line[0] != '\0') {
				error = process_line(line);
			}
		}
	}
	if (ferror(stdin)) {
		perror("stdin");
		error = 1;
	}
	free(line);
	return error;
}

void help(void) {
	(void) fprintf(stdout,
"Usage: ptar [-h] [OPTION ...] -(c|x|t) [PATH ...]\n\n"

"     Manipulate plain text archives that are similar to traditional tar(1)\n"
"     files but are more human-readable.\n\n"

"Commands:\n\n"

"    -c         Create a new archive and print its contents on standard\n"
"               output.  The files whose PATHs are listed on the command\n"
"               line will be added to the archive.\n\n"

"    -x         Extract the contents of the archive from standard input\n"
"               and write the contents to the file system relative to\n"
"               the current working directory.  The files whose PATHs are\n"
"               listed on the command line will be extracted from the\n"
"               archive.  If no PATHs are specified, then all contents\n"
"               will be extracted.  PATHs are treated as shell wildcard\n"
"               patterns: '*.jpg' 'pictures/2013-*.jpg' and other such\n"
"               patterns will match archived paths the same way these\n"
"               patterns match paths in interactive shells.\n\n"

"    -t         List the PATHs stored in the archive from standard input.\n"
"               This also checks whether the archive conforms to the plain\n"
"               text archive standard (including recognized extensions) and\n"
"               can be extracted via 'x' without parse errors.  This does\n"
"               not check your file system's state and thus cannot\n"
"               guarantee that extraction would succeed.  This also does\n"
"               not verify file or metadata contents unless the archive\n"
"               contains a recognized extension that permits such\n"
"               verification.  Unrecognized extensions are treated\n"
"               as errors.\n\n"

"Options:\n\n"

"     NOTE: Options must precede command letters.\n\n"

"     -h, --help                   Show this help message and exit.\n"
"     -n, --no-archive-metadata    Don't write global archive metadata when\n"
"                                  creating an archive with the 'c'\n"
"                                  command.  (NOTE: Archives created with\n"
"                                  this option will lack top-level archive\n"
"                                  metadata and thus will not be standards-\n"
"                                  compliant.  However, you can append\n"
"                                  such archives onto standards-compliant\n"
"                                  archives without affecting their\n"
"                                  compliance.  This creates a way to add\n"
"                                  files to already-existing archives\n"
"                                  through shell redirection.)\n"
"     -o, --extract-to-stdout      Override default 'x' command behavior by\n"
"                                  writing extracted regular files' contents\n"
"                                  to standard output.  The file system is\n"
"                                  not altered.  (This only makes sense for\n"
"                                  the 'x' command.)\n"
"     --paths-from-stdin           Read PATHs to be archived from standard\n"
"                                  input, one PATH per line, after\n"
"                                  archiving PATHs specified on the command\n"
"                                  line.  (This only makes sense for the\n"
"                                  'c' command.)\n"
"     -u, --unbuffered             Disable standard output buffering.\n"
"     -v, --verbose                Verbose output: List PATHs added or\n"
"                                  extracted on standard error.\n\n");
}

int main(int argc, char **argv) {
	int error, n;
	char noarchivemetadata, pathsfromstdin;
	struct stat sb;
	size_t index;
	time_t now = time(NULL);

	pathsfromstdin = noarchivemetadata = 0;
	for (n = 1; n < argc; n++) {
		if (strcmp(argv[n], "-h") == 0 || strcmp(argv[n], "--help") == 0) {
			help();
			exit(EXIT_SUCCESS);
		}
	}
	if (argc == 1) {
		help();
		exit(EXIT_FAILURE);
	}
	for (n = 1; n < argc; n++) {
		if (strcmp(argv[n], "--paths-from-stdin") == 0) {
			pathsfromstdin = 1;
		} else if (strcmp(argv[n], "-u") == 0 || strcmp(argv[n], "--unbuffered") == 0) {
			if (setvbuf(stdout, NULL, _IONBF, 0) != 0) {
				(void) fprintf(stderr, "error: unable to disable standard output buffering: %s\n", strerror(errno));
				exit(EXIT_FAILURE);
			}
		} else if (strcmp(argv[n], "-v") == 0 || strcmp(argv[n], "--verbose") == 0) {
			verbose = 1;
		} else if (strcmp(argv[n], "-n") == 0 || strcmp(argv[n], "--no-archive-metadata") == 0) {
			noarchivemetadata = 1;
		} else if (strcmp(argv[n], "-o") == 0 || strcmp(argv[n], "--extract-to-stdout") == 0) {
			extracttostdout = 1;
		} else if (n == argc) {
			(void) fprintf(stderr, "error: no command given (specify -h for help)\n");
			exit(EXIT_FAILURE);
		} else if (strlen(argv[n]) > 2) {
			(void) fprintf(stderr, "error: command must be exactly one of '-c', '-x', or '-t'\n");
			exit(EXIT_FAILURE);
		} else {
			break;
		}
	}
	error = 0;
	unsigned char flag = ('-'==argv[n][0]) ?  argv[n][1] : argv[n][0];
	switch (flag) {
	case 'c':
		if (fstat(1, &sb) != 0) {
			(void) fprintf(stderr, "error: couldn't stat standard output: %s\n", strerror(errno));
			exit(EXIT_FAILURE);
		}
		stdoutdev = sb.st_dev;
		stdoutino = sb.st_ino;
		now = time(NULL);
		struct tm *nowtm;
		if ((nowtm = localtime(&now)) == NULL) {
			(void) fprintf(stderr, "error: couldn't convert Epoch timestamp to broken-down time representation\n");
			exit(EXIT_FAILURE);
		}
		if (strftime(linkpath, sizeof (linkpath), "%Y-%m-%dT%H:%M:%SZ", nowtm) == 0) {
			(void) fprintf(stderr, "error: current date and time are too large to fit in ptar's internal buffer\n");
			exit(EXIT_FAILURE);
		}
		if (!noarchivemetadata) {
			write_metadata("Metadata Encoding", "utf-8");
			write_metadata("Archive Creation Date", linkpath);
		}
		for (n++; !error && n < argc; n++) {
			error = archive_file(argv[n]);
		}
		if (!error && pathsfromstdin) {
			error = process_stdin_lines(archive_file);
		}
		break;
	case 'x':
		if (++n < argc) {
			do {
				error = add_requested_path(argv[n]);
			} while (!error && ++n < argc);
			should_extract_file = extract_if_requested_file;
		}
		if (!error) {
			error = scan_archive(extract);
		}
		break;
	case 't':
		error = scan_archive(listfiles);
		break;
	default:
		(void) fprintf(stderr, "error: unrecognized command: %s (must be one of 'c', 'x', or 't')\n", argv[n]);
		error = 1;
	}
	clear_metadata();
	for (index = 0; index < num_requested_files; index++) {
		if (!requested_files[index].found) {
			(void) fprintf(stderr, "error: no archived files matching this pattern: %s\n", requested_files[index].path_pattern);
			error = 1;
		}
		free(requested_files[index].path_pattern);
	}
	free(requested_files);
	return error ? EXIT_FAILURE : EXIT_SUCCESS;
}

