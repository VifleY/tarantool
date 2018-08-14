/*
 * Copyright 2010-2017, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "sqliteInt.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>
#include <sys/mman.h>


/*
 * Default permissions when creating a new file
 */
#ifndef SQLITE_DEFAULT_FILE_PERMISSIONS
#define SQLITE_DEFAULT_FILE_PERMISSIONS 0644
#endif

/*
 * Maximum supported path-length.
 */
#define MAX_PATHNAME 512

typedef struct unixInodeInfo unixInodeInfo;	/* An i-node */
typedef struct UnixUnusedFd UnixUnusedFd;	/* An unused file descriptor */

/*
 * Sometimes, after a file handle is closed by SQLite, the file descriptor
 * cannot be closed immediately. In these cases, instances of the following
 * structure are used to store the file descriptor while waiting for an
 * opportunity to either close or reuse it.
 */
struct UnixUnusedFd {
	int fd;			/* File descriptor to close */
	int flags;		/* Flags this file descriptor was opened with */
	UnixUnusedFd *pNext;	/* Next unused file descriptor on same file */
};

/*
 * The unixFile structure is subclass of sqlite3_file specific to the unix
 * VFS implementations.
 */
typedef struct unixFile unixFile;
struct unixFile {
	sqlite3_io_methods const *pMethod;	/* Always the first entry */
	sqlite3_vfs *pVfs;	/* The VFS that created this unixFile */
	unixInodeInfo *pInode;	/* Info about locks on this inode */
	int h;			/* The file descriptor */
	unsigned char eFileLock;	/* The type of lock held on this fd */
	unsigned short int ctrlFlags;	/* Behavioral bits.  UNIXFILE_* flags */
	int lastErrno;		/* The unix errno from last I/O error */
	UnixUnusedFd *pUnused;	/* Pre-allocated UnixUnusedFd */
	const char *zPath;	/* Name of the file */
	int szChunk;		/* Configured by FCNTL_CHUNK_SIZE */
	int nFetchOut;		/* Number of outstanding xFetch refs */
	sqlite3_int64 mmapSize;	/* Usable size of mapping at pMapRegion */
	sqlite3_int64 mmapSizeActual;	/* Actual size of mapping at pMapRegion */
	sqlite3_int64 mmapSizeMax;	/* Configured FCNTL_MMAP_SIZE value */
	void *pMapRegion;	/* Memory mapped region */
};

/* This variable holds the process id (pid) from when the xRandomness()
 * method was called.  If xOpen() is called from a different process id,
 * indicating that a fork() has occurred, the PRNG will be reset.
 */
static pid_t randomnessPid = 0;

/*
 * Allowed values for the unixFile.ctrlFlags bitmask:
 */
#define UNIXFILE_EXCL        0x01	/* Connections from one process only */
#define UNIXFILE_RDONLY      0x02	/* Connection is read only */
#define UNIXFILE_DIRSYNC    0x08	/* Directory sync needed */
#define UNIXFILE_DELETE      0x20	/* Delete on close */
#define UNIXFILE_URI         0x40	/* Filename might have query parameters */
#define UNIXFILE_NOLOCK      0x80	/* Do no file locking */

/*
 * Define various macros that are missing from some systems.
 */
#ifndef O_BINARY
#define O_BINARY 0
#endif
#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif
#ifdef SQLITE_DISABLE_LFS
#undef O_LARGEFILE
#define O_LARGEFILE 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

/*
 * Do not accept any file descriptor less than this value, in order to avoid
 * opening database file using file descriptors that are commonly used for
 * standard input, output, and error.
 */
#ifndef SQLITE_MINIMUM_FILE_DESCRIPTOR
#define SQLITE_MINIMUM_FILE_DESCRIPTOR 3
#endif

/*
 * Invoke open().  Do so multiple times, until it either succeeds or
 * fails for some reason other than EINTR.
 *
 * If the file creation mode "m" is 0 then set it to the default for
 * SQLite.  The default is SQLITE_DEFAULT_FILE_PERMISSIONS (normally
 * 0644) as modified by the system umask.  If m is not 0, then
 * make the file creation mode be exactly m ignoring the umask.
 *
 * The m parameter will be non-zero only when creating
 * -shm files.  We want those files to have *exactly* the same
 * permissions as their original database, unadulterated by the umask.
 * In that way, if a database file is -rw-rw-rw or -rw-rw-r-, and a
 * transaction crashes and leaves behind hot journals, then any
 * process that is able to write to the database will also be able to
 * recover the hot journals.
 */
static int
robust_open(const char *z, int f, mode_t m)
{
	int fd;
	mode_t m2 = m ? m : SQLITE_DEFAULT_FILE_PERMISSIONS;
	while (1) {
		fd = open(z, f | O_CLOEXEC, m2);
		if (fd < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (fd >= SQLITE_MINIMUM_FILE_DESCRIPTOR)
			break;
		close(fd);
		sqlite3_log(SQLITE_WARNING,
			    "attempt to open \"%s\" as file descriptor %d", z,
			    fd);
		fd = -1;
		if (open("/dev/null", f, m) < 0)
			break;
	}
	if (fd >= 0) {
		if (m != 0) {
			struct stat statbuf;
			if (fstat(fd, &statbuf) == 0 &&
			    statbuf.st_size == 0 &&
			    (statbuf.st_mode & 0777) != m)
				fchmod(fd, m);
		}
	}
	return fd;
}

/*
 * Retry ftruncate() calls that fail due to EINTR
 *
 * All calls to ftruncate() within this file should be made through
 * this wrapper.
 */
static int
robust_ftruncate(int h, sqlite3_int64 sz)
{
	int rc;
	do {
		rc = ftruncate(h, sz);
	} while (rc < 0 && errno == EINTR);
	return rc;
}

/******************************************************************************
 ************************** Posix Advisory Locking ****************************
 *
 * POSIX advisory locks are broken by design.  ANSI STD 1003.1 (1996)
 * section 6.5.2.2 lines 483 through 490 specify that when a process
 * sets or clears a lock, that operation overrides any prior locks set
 * by the same process.  It does not explicitly say so, but this implies
 * that it overrides locks set by the same process using a different
 * file descriptor.  Consider this test case:
 *
 *       int fd1 = open("./file1", O_RDWR|O_CREAT, 0644);
 *       int fd2 = open("./file2", O_RDWR|O_CREAT, 0644);
 *
 * Suppose ./file1 and ./file2 are really the same file (because
 * one is a hard or symbolic link to the other) then if you set
 * an exclusive lock on fd1, then try to get an exclusive lock
 * on fd2, it works.  I would have expected the second lock to
 * fail since there was already a lock on the file due to fd1.
 * But not so.  Since both locks came from the same process, the
 * second overrides the first, even though they were on different
 * file descriptors opened on different file names.
 *
 * This means that we cannot use POSIX locks to synchronize file access
 * among competing threads of the same process.  POSIX locks will work fine
 * to synchronize access for threads in separate processes, but not
 * threads within the same process.
 *
 * To work around the problem, SQLite has to manage file locks internally
 * on its own.  Whenever a new database is opened, we have to find the
 * specific inode of the database file (the inode is determined by the
 * st_dev and st_ino fields of the stat structure that fstat() fills in)
 * and check for locks already existing on that inode.  When locks are
 * created or removed, we have to look at our own internal record of the
 * locks to see if another thread has previously set a lock on that same
 * inode.
 *
 * The sqlite3_file structure for POSIX is no longer just an integer file
 * descriptor.  It is now a structure that holds the integer file
 * descriptor and a pointer to a structure that describes the internal
 * locks on the corresponding inode.  There is one locking structure
 * per inode, so if the same inode is opened twice, both unixFile structures
 * point to the same locking structure.  The locking structure keeps
 * a reference count (so we will know when to delete it) and a "cnt"
 * field that tells us its internal lock status.  cnt==0 means the
 * file is unlocked.  cnt==-1 means the file has an exclusive lock.
 * cnt>0 means there are cnt shared locks on the file.
 *
 * Any attempt to lock or unlock a file first checks the locking
 * structure.  The fcntl() system call is only invoked to set a
 * POSIX lock if the internal lock structure transitions between
 * a locked and an unlocked state.
 *
 * But wait:  there are yet more problems with POSIX advisory locks.
 *
 * If you close a file descriptor that points to a file that has locks,
 * all locks on that file that are owned by the current process are
 * released.  To work around this problem, each unixInodeInfo object
 * maintains a count of the number of pending locks on tha inode.
 * When an attempt is made to close an unixFile, if there are
 * other unixFile open on the same inode that are holding locks, the call
 * to close() the file descriptor is deferred until all of the locks clear.
 * The unixInodeInfo structure keeps a list of file descriptors that need to
 * be closed and that list is walked (and cleared) when the last lock
 * clears.
 *
 * Yet another problem:  LinuxThreads do not play well with posix locks.
 *
 * Many older versions of linux use the LinuxThreads library which is
 * not posix compliant.  Under LinuxThreads, a lock created by thread
 * A cannot be modified or overridden by a different thread B.
 * Only thread A can modify the lock.  Locking behavior is correct
 * if the appliation uses the newer Native Posix Thread Library (NPTL)
 * on linux - with NPTL a lock created by thread A can override locks
 * in thread B.  But there is no way to know at compile-time which
 * threading library is being used.  So there is no way to know at
 * compile-time whether or not thread A can override locks on thread B.
 * One has to do a run-time check to discover the behavior of the
 * current process.
 *
 * SQLite used to support LinuxThreads.  But support for LinuxThreads
 * was dropped beginning with version 3.7.0.  SQLite will still work with
 * LinuxThreads provided that (1) there is no more than one connection
 * per database file in the same process and (2) database connections
 * do not move across threads.
 */

/*
 * An instance of the following structure serves as the key used
 * to locate a particular unixInodeInfo object.
 */
struct unixFileId {
	dev_t dev;		/* Device number */
	u64 ino;		/* Inode number */
};

/*
 * An instance of the following structure is allocated for each open
 * inode.  Or, on LinuxThreads, there is one of these structures for
 * each inode opened by each thread.
 *
 * A single inode can have multiple file descriptors, so each unixFile
 * structure contains a pointer to an instance of this object and this
 * object keeps a count of the number of unixFile pointing to it.
 */
struct unixInodeInfo {
	struct unixFileId fileId;	/* The lookup key */
	int nShared;		/* Number of SHARED locks held */
	unsigned char eFileLock;	/* One of SHARED_LOCK, RESERVED_LOCK etc. */
	unsigned char bProcessLock;	/* An exclusive process lock is held */
	int nRef;		/* Number of pointers to this structure */
	int nLock;		/* Number of outstanding file locks */
	UnixUnusedFd *pUnused;	/* Unused file descriptors to close */
	unixInodeInfo *pNext;	/* List of all unixInodeInfo objects */
	unixInodeInfo *pPrev;	/*    .... doubly linked */
};

/*
 * A lists of all unixInodeInfo objects.
 */
static unixInodeInfo *inodeList = 0;

/*
 *
 * This function - unixLogErrorAtLine(), is only ever called via the macro
 * unixLogError().
 *
 * It is invoked after an error occurs in an OS function and errno has been
 * set. It logs a message using sqlite3_log() containing the current value of
 * errno and, if possible, the human-readable equivalent from strerror() or
 * strerror_r().
 *
 * The first argument passed to the macro should be the error code that
 * will be returned to SQLite (e.g. SQLITE_IOERR_DELETE, SQLITE_CANTOPEN).
 * The two subsequent arguments should be the name of the OS function that
 * failed (e.g. "unlink", "open") and the associated file-system path,
 * if any.
 */
#define unixLogError(a,b,c)     unixLogErrorAtLine(a,b,c,__LINE__)
static int
unixLogErrorAtLine(int errcode,	/* SQLite error code */
		   const char *zFunc,	/* Name of OS function that failed */
		   const char *zPath,	/* File path associated with error */
		   int iLine	/* Source line number where error occurred */
    )
{
	char *zErr;		/* Message from strerror() or equivalent */
	int iErrno = errno;	/* Saved syscall error number */

	zErr = strerror(iErrno);

	if (zPath == 0)
		zPath = "";
	sqlite3_log(errcode,
		    "os_unix.c:%d: (%d) %s(%s) - %s",
		    iLine, iErrno, zFunc, zPath, zErr);

	return errcode;
}

/*
 * Close a file descriptor.
 *
 * We assume that close() almost always works, since it is only in a
 * very sick application or on a very sick platform that it might fail.
 * If it does fail, simply leak the file descriptor, but do log the
 * error.
 *
 * Note that it is not safe to retry close() after EINTR since the
 * file descriptor might have already been reused by another thread.
 * So we don't even try to recover from an EINTR.  Just log the error
 * and move on.
 */
static void
robust_close(unixFile * pFile, int h, int lineno)
{
	if (close(h) != 0) {
		unixLogErrorAtLine(SQLITE_IOERR_CLOSE, "close",
				   pFile ? pFile->zPath : 0, lineno);
	}
}

/*
 * Set the pFile->lastErrno.  Do this in a subroutine as that provides
 * a convenient place to set a breakpoint.
 */
static void
storeLastErrno(unixFile * pFile, int error)
{
	pFile->lastErrno = error;
}

/*
 * Close all file descriptors accumuated in the unixInodeInfo->pUnused list.
 */
static void
closePendingFds(unixFile * pFile)
{
	unixInodeInfo *pInode = pFile->pInode;
	UnixUnusedFd *p;
	UnixUnusedFd *pNext;
	for (p = pInode->pUnused; p; p = pNext) {
		pNext = p->pNext;
		robust_close(pFile, p->fd, __LINE__);
		sqlite3_free(p);
	}
	pInode->pUnused = 0;
}

/*
 * Release a unixInodeInfo structure previously allocated by findInodeInfo().
 */
static void
releaseInodeInfo(unixFile * pFile)
{
	unixInodeInfo *pInode = pFile->pInode;
	if (ALWAYS(pInode)) {
		pInode->nRef--;
		if (pInode->nRef == 0) {
			closePendingFds(pFile);
			if (pInode->pPrev) {
				assert(pInode->pPrev->pNext == pInode);
				pInode->pPrev->pNext = pInode->pNext;
			} else {
				assert(inodeList == pInode);
				inodeList = pInode->pNext;
			}
			if (pInode->pNext) {
				assert(pInode->pNext->pPrev == pInode);
				pInode->pNext->pPrev = pInode->pPrev;
			}
			sqlite3_free(pInode);
		}
	}
}

/*
 * Given a file descriptor, locate the unixInodeInfo object that
 * describes that file descriptor.  Create a new one if necessary.  The
 * return value might be uninitialized if an error occurs.
 *
 * Return an appropriate error code.
 */
static int
findInodeInfo(unixFile * pFile,	/* Unix file with file desc used in the key */
	      unixInodeInfo ** ppInode	/* Return the unixInodeInfo object here */
    )
{
	int rc;			/* System call return code */
	int fd;			/* The file descriptor for pFile */
	struct unixFileId fileId;	/* Lookup key for the unixInodeInfo */
	struct stat statbuf;	/* Low-level file information */
	unixInodeInfo *pInode = 0;	/* Candidate unixInodeInfo object */

	/* Get low-level information about the file that we can used to
	 * create a unique name for the file.
	 */
	fd = pFile->h;
	rc = fstat(fd, &statbuf);
	if (rc != 0) {
		storeLastErrno(pFile, errno);
		return SQLITE_IOERR;
	}

	memset(&fileId, 0, sizeof(fileId));
	fileId.dev = statbuf.st_dev;
	fileId.ino = (u64) statbuf.st_ino;
	pInode = inodeList;
	while (pInode && memcmp(&fileId, &pInode->fileId, sizeof(fileId))) {
		pInode = pInode->pNext;
	}
	if (pInode == 0) {
		pInode = sqlite3_malloc64(sizeof(*pInode));
		if (pInode == 0) {
			return SQLITE_NOMEM_BKPT;
		}
		memset(pInode, 0, sizeof(*pInode));
		memcpy(&pInode->fileId, &fileId, sizeof(fileId));
		pInode->nRef = 1;
		pInode->pNext = inodeList;
		pInode->pPrev = 0;
		if (inodeList)
			inodeList->pPrev = pInode;
		inodeList = pInode;
	} else {
		pInode->nRef++;
	}
	*ppInode = pInode;
	return SQLITE_OK;
}

/*
 * Return TRUE if pFile has been renamed or unlinked since it was first opened.
 */
static int
fileHasMoved(unixFile * pFile)
{
	struct stat buf;
	return pFile->pInode != NULL && (stat(pFile->zPath, &buf) != 0 ||
					 (u64) buf.st_ino !=
					 pFile->pInode->fileId.ino);
}

/*
 * Check a unixFile that is a database.  Verify the following:
 *
 * (1) There is exactly one hard link on the file
 * (2) The file is not a symbolic link
 * (3) The file has not been renamed or unlinked
 *
 * Issue sqlite3_log(SQLITE_WARNING,...) messages if anything is not right.
 */
static void
verifyDbFile(unixFile * pFile)
{
	struct stat buf;
	int rc;

	/* These verifications occurs for the main database only */
	if (pFile->ctrlFlags & UNIXFILE_NOLOCK)
		return;

	rc = fstat(pFile->h, &buf);
	if (rc != 0) {
		sqlite3_log(SQLITE_WARNING, "cannot fstat db file %s",
			    pFile->zPath);
		return;
	}
	if (buf.st_nlink == 0) {
		sqlite3_log(SQLITE_WARNING, "file unlinked while open: %s",
			    pFile->zPath);
		return;
	}
	if (buf.st_nlink > 1) {
		sqlite3_log(SQLITE_WARNING, "multiple links to file: %s",
			    pFile->zPath);
		return;
	}
	if (fileHasMoved(pFile)) {
		sqlite3_log(SQLITE_WARNING, "file renamed while open: %s",
			    pFile->zPath);
		return;
	}
}

/*
 * Attempt to set a system-lock on the file pFile.  The lock is
 * described by pLock.
 *
 * If the pFile was opened read/write from unix-excl, then the only lock
 * ever obtained is an exclusive lock, and it is obtained exactly once
 * the first time any lock is attempted.  All subsequent system locking
 * operations become no-ops.  Locking operations still happen internally,
 * in order to coordinate access between separate database connections
 * within this process, but all of that is handled in memory and the
 * operating system does not participate.
 *
 * This function is a pass-through to fcntl(F_SETLK) if pFile is using
 * any VFS other than "unix-excl" or if pFile is opened on "unix-excl"
 * and is read-only.
 *
 * Zero is returned if the call completes successfully, or -1 if a call
 * to fcntl() fails. In this case, errno is set appropriately (by fcntl()).
 */
static int
unixFileLock(unixFile * pFile, struct flock *pLock)
{
	int rc;
	unixInodeInfo *pInode = pFile->pInode;
	assert(pInode != 0);
	if ((pFile->ctrlFlags & (UNIXFILE_EXCL | UNIXFILE_RDONLY)) ==
	    UNIXFILE_EXCL) {
		if (pInode->bProcessLock == 0) {
			struct flock lock;
			assert(pInode->nLock == 0);
			lock.l_whence = SEEK_SET;
			lock.l_start = SHARED_FIRST;
			lock.l_len = SHARED_SIZE;
			lock.l_type = F_WRLCK;
			rc = fcntl(pFile->h, F_SETLK, &lock);
			if (rc < 0)
				return rc;
			pInode->bProcessLock = 1;
			pInode->nLock++;
		} else {
			rc = 0;
		}
	} else {
		rc = fcntl(pFile->h, F_SETLK, pLock);
	}
	return rc;
}

/*
 * Add the file descriptor used by file handle pFile to the corresponding
 * pUnused list.
 */
static void
setPendingFd(unixFile * pFile)
{
	unixInodeInfo *pInode = pFile->pInode;
	UnixUnusedFd *p = pFile->pUnused;
	p->pNext = pInode->pUnused;
	pInode->pUnused = p;
	pFile->h = -1;
	pFile->pUnused = 0;
}

/*
 * Lower the locking level on file descriptor pFile to eFileLock.  eFileLock
 * must be either NO_LOCK or SHARED_LOCK.
 *
 * If the locking level of the file descriptor is already at or below
 * the requested locking level, this routine is a no-op.
 *
 * If handleNFSUnlock is true, then on downgrading an EXCLUSIVE_LOCK to SHARED
 * the byte range is divided into 2 parts and the first part is unlocked then
 * set to a read lock, then the other part is simply unlocked.  This works
 * around a bug in BSD NFS lockd (also seen on MacOSX 10.3+) that fails to
 * remove the write lock on a region when a read lock is set.
 */
static int
posixUnlock(sqlite3_file * id, int eFileLock, int handleNFSUnlock)
{
	unixFile *pFile = (unixFile *) id;
	unixInodeInfo *pInode;
	struct flock lock;
	int rc = SQLITE_OK;

	assert(pFile);

	assert(eFileLock <= SHARED_LOCK);
	if (pFile->eFileLock <= eFileLock) {
		return SQLITE_OK;
	}
	pInode = pFile->pInode;
	assert(pInode->nShared != 0);
	if (pFile->eFileLock > SHARED_LOCK) {
		assert(pInode->eFileLock == pFile->eFileLock);

		/* downgrading to a shared lock on NFS involves clearing the write lock
		 * before establishing the readlock - to avoid a race condition we downgrade
		 * the lock in 2 blocks, so that part of the range will be covered by a
		 * write lock until the rest is covered by a read lock:
		 *  1:   [WWWWW]
		 *  2:   [....W]
		 *  3:   [RRRRW]
		 *  4:   [RRRR.]
		 */
		if (eFileLock == SHARED_LOCK) {
			(void)handleNFSUnlock;
			assert(handleNFSUnlock == 0);
			{
				lock.l_type = F_RDLCK;
				lock.l_whence = SEEK_SET;
				lock.l_start = SHARED_FIRST;
				lock.l_len = SHARED_SIZE;
				if (unixFileLock(pFile, &lock)) {
					/* In theory, the call to unixFileLock() cannot fail because another
					 * process is holding an incompatible lock. If it does, this
					 * indicates that the other process is not following the locking
					 * protocol. If this happens, return SQLITE_IOERR_RDLOCK. Returning
					 * SQLITE_BUSY would confuse the upper layer (in practice it causes
					 * an assert to fail).
					 */
					rc = SQLITE_IOERR_RDLOCK;
					storeLastErrno(pFile, errno);
					goto end_unlock;
				}
			}
		}
		lock.l_type = F_UNLCK;
		lock.l_whence = SEEK_SET;
		lock.l_start = PENDING_BYTE;
		lock.l_len = 2L;
		assert(PENDING_BYTE + 1 == RESERVED_BYTE);
		if (unixFileLock(pFile, &lock) == 0) {
			pInode->eFileLock = SHARED_LOCK;
		} else {
			rc = SQLITE_IOERR_UNLOCK;
			storeLastErrno(pFile, errno);
			goto end_unlock;
		}
	}
	if (eFileLock == NO_LOCK) {
		/* Decrement the shared lock counter.  Release the lock using an
		 * OS call only when all threads in this same process have released
		 * the lock.
		 */
		pInode->nShared--;
		if (pInode->nShared == 0) {
			lock.l_type = F_UNLCK;
			lock.l_whence = SEEK_SET;
			lock.l_start = lock.l_len = 0L;
			if (unixFileLock(pFile, &lock) == 0) {
				pInode->eFileLock = NO_LOCK;
			} else {
				rc = SQLITE_IOERR_UNLOCK;
				storeLastErrno(pFile, errno);
				pInode->eFileLock = NO_LOCK;
				pFile->eFileLock = NO_LOCK;
			}
		}

		/* Decrement the count of locks against this same file.  When the
		 * count reaches zero, close any other file descriptors whose close
		 * was deferred because of outstanding locks.
		 */
		pInode->nLock--;
		assert(pInode->nLock >= 0);
		if (pInode->nLock == 0) {
			closePendingFds(pFile);
		}
	}

 end_unlock:
	if (rc == SQLITE_OK)
		pFile->eFileLock = eFileLock;
	return rc;
}

/*
 * Lower the locking level on file descriptor pFile to eFileLock.  eFileLock
 * must be either NO_LOCK or SHARED_LOCK.
 *
 * If the locking level of the file descriptor is already at or below
 * the requested locking level, this routine is a no-op.
 */
static int
unixUnlock(sqlite3_file * id, int eFileLock)
{
	assert(eFileLock == SHARED_LOCK || ((unixFile *) id)->nFetchOut == 0);
	return posixUnlock(id, eFileLock, 0);
}

static int unixMapfile(unixFile * pFd, i64 nByte);
static void unixUnmapfile(unixFile * pFd);

/*
 * This function performs the parts of the "close file" operation
 * common to all locking schemes. It closes the directory and file
 * handles, if they are valid, and sets all fields of the unixFile
 * structure to 0.
 */
static int
closeUnixFile(sqlite3_file * id)
{
	unixFile *pFile = (unixFile *) id;
#if SQLITE_MAX_MMAP_SIZE>0
	unixUnmapfile(pFile);
#endif
	if (pFile->h >= 0) {
		robust_close(pFile, pFile->h, __LINE__);
		pFile->h = -1;
	}
	sqlite3_free(pFile->pUnused);
	memset(pFile, 0, sizeof(unixFile));
	return SQLITE_OK;
}

/*
 * Close a file.
 */
static int
unixClose(sqlite3_file * id)
{
	int rc;
	unixFile *pFile = (unixFile *) id;
	verifyDbFile(pFile);
	unixUnlock(id, NO_LOCK);

	/* unixFile.pInode is always valid here. Otherwise, a different close
	 * routine (e.g. nolockClose()) would be called instead.
	 */
	assert(pFile->pInode->nLock > 0 || pFile->pInode->bProcessLock == 0);
	if (ALWAYS(pFile->pInode) && pFile->pInode->nLock) {
		/* If there are outstanding locks, do not actually close the file just
		 * yet because that would clear those locks.  Instead, add the file
		 * descriptor to pInode->pUnused list.  It will be automatically closed
		 * when the last lock is cleared.
		 */
		setPendingFd(pFile);
	}
	releaseInodeInfo(pFile);
	rc = closeUnixFile(id);
	return rc;
}

/************** End of the posix advisory lock implementation *****************
 *****************************************************************************/

/*
 * Close the file.
 */
static int
nolockClose(sqlite3_file * id)
{
	return closeUnixFile(id);
}


/******************* End of the non-op lock implementation *******************
 *****************************************************************************/

/******************************************************************************
 *************** Non-locking sqlite3_file methods *****************************
 *
 * The next division contains implementations for all methods of the
 * sqlite3_file object other than the locking methods.  The locking
 * methods were defined in divisions above (one locking method per
 * division).  Those methods that are common to all locking modes
 * are gather together into this division.
 */

/*
 * Seek to the offset passed as the second argument, then read cnt
 * bytes into pBuf. Return the number of bytes actually read.
 *
 * NB:  If you define USE_PREAD or USE_PREAD64, then it might also
 * be necessary to define _XOPEN_SOURCE to be 500.  This varies from
 * one system to another.  Since SQLite does not define USE_PREAD
 * in any form by default, we will not attempt to define _XOPEN_SOURCE.
 * See tickets #2741 and #2681.
 *
 * To avoid stomping the errno value on a failed read the lastErrno value
 * is set before returning.
 */
static int
seekAndRead(unixFile * id, sqlite3_int64 offset, void *pBuf, int cnt)
{
	int got;
	int prior = 0;
	i64 newOffset;

	assert(cnt == (cnt & 0x1ffff));
	assert(id->h > 2);
	do {
		newOffset = lseek(id->h, offset, SEEK_SET);
		if (newOffset < 0) {
			storeLastErrno((unixFile *) id, errno);
			return -1;
		}
		got = read(id->h, pBuf, cnt);
		if (got == cnt)
			break;
		if (got < 0) {
			if (errno == EINTR) {
				got = 1;
				continue;
			}
			prior = 0;
			storeLastErrno((unixFile *) id, errno);
			break;
		} else if (got > 0) {
			cnt -= got;
			offset += got;
			prior += got;
			pBuf = (void *)(got + (char *)pBuf);
		}
	} while (got > 0);
	return got + prior;
}

/*
 * Read data from a file into a buffer.  Return SQLITE_OK if all
 * bytes were read successfully and SQLITE_IOERR if anything goes
 * wrong.
 */
static int
unixRead(sqlite3_file * id, void *pBuf, int amt, sqlite3_int64 offset)
{
	unixFile *pFile = (unixFile *) id;
	int got;
	assert(id);
	assert(offset >= 0);
	assert(amt > 0);

#if SQLITE_MAX_MMAP_SIZE>0
	/* Deal with as much of this read request as possible by transfering
	 * data from the memory mapping using memcpy().
	 */
	if (offset < pFile->mmapSize) {
		if (offset + amt <= pFile->mmapSize) {
			memcpy(pBuf, &((u8 *) (pFile->pMapRegion))[offset],
			       amt);
			return SQLITE_OK;
		} else {
			int nCopy = pFile->mmapSize - offset;
			memcpy(pBuf, &((u8 *) (pFile->pMapRegion))[offset],
			       nCopy);
			pBuf = &((u8 *) pBuf)[nCopy];
			amt -= nCopy;
			offset += nCopy;
		}
	}
#endif

	got = seekAndRead(pFile, offset, pBuf, amt);
	if (got == amt) {
		return SQLITE_OK;
	} else if (got < 0) {
		/* lastErrno set by seekAndRead */
		return SQLITE_IOERR_READ;
	} else {
		storeLastErrno(pFile, 0);	/* not a system error */
		/* Unread parts of the buffer must be zero-filled */
		memset(&((char *)pBuf)[got], 0, amt - got);
		return SQLITE_IOERR_SHORT_READ;
	}
}

/*
 * Attempt to seek the file-descriptor passed as the first argument to
 * absolute offset iOff, then attempt to write nBuf bytes of data from
 * pBuf to it. If an error occurs, return -1 and set *piErrno. Otherwise,
 * return the actual number of bytes written (which may be less than
 * nBuf).
 */
static int
seekAndWriteFd(int fd,		/* File descriptor to write to */
	       i64 iOff,	/* File offset to begin writing at */
	       const void *pBuf,	/* Copy data from this buffer to the file */
	       int nBuf,	/* Size of buffer pBuf in bytes */
	       int *piErrno	/* OUT: Error number if error occurs */
    )
{
	int rc = 0;		/* Value returned by system call */

	assert(nBuf == (nBuf & 0x1ffff));
	assert(fd > 2);
	assert(piErrno != 0);
	nBuf &= 0x1ffff;
	do {
		i64 iSeek = lseek(fd, iOff, SEEK_SET);
		if (iSeek < 0) {
			rc = -1;
			break;
		}
		rc = write(fd, pBuf, nBuf);
	} while (rc < 0 && errno == EINTR);

	if (rc < 0)
		*piErrno = errno;
	return rc;
}

/*
 * Seek to the offset in id->offset then read cnt bytes into pBuf.
 * Return the number of bytes actually read.  Update the offset.
 *
 * To avoid stomping the errno value on a failed write the lastErrno value
 * is set before returning.
 */
static int
seekAndWrite(unixFile * id, i64 offset, const void *pBuf, int cnt)
{
	return seekAndWriteFd(id->h, offset, pBuf, cnt, &id->lastErrno);
}

/*
 * Write data from a buffer into a file.  Return SQLITE_OK on success
 * or some other error code on failure.
 */
static int
unixWrite(sqlite3_file * id, const void *pBuf, int amt, sqlite3_int64 offset)
{
	unixFile *pFile = (unixFile *) id;
	int wrote = 0;
	assert(id);
	assert(amt > 0);

	while ((wrote = seekAndWrite(pFile, offset, pBuf, amt)) < amt
	       && wrote > 0) {
		amt -= wrote;
		offset += wrote;
		pBuf = &((char *)pBuf)[wrote];
	}

	if (amt > wrote) {
		if (wrote < 0 && pFile->lastErrno != ENOSPC) {
			/* lastErrno set by seekAndWrite */
			return SQLITE_IOERR_WRITE;
		} else {
			storeLastErrno(pFile, 0);	/* not a system error */
			return SQLITE_FULL;
		}
	}

	return SQLITE_OK;
}

/*
 * Open a file descriptor to the directory containing file zFilename.
 * If successful, *pFd is set to the opened file descriptor and
 * SQLITE_OK is returned. If an error occurs, either SQLITE_NOMEM
 * or SQLITE_CANTOPEN is returned and *pFd is set to an undefined
 * value.
 *
 * The directory file descriptor is used for only one thing - to
 * fsync() a directory to make sure file creation and deletion events
 * are flushed to disk.  Such fsyncs are not needed on newer
 * journaling filesystems, but are required on older filesystems.
 *
 * This routine can be overridden using the xSetSysCall interface.
 * The ability to override this routine was added in support of the
 * chromium sandbox.  Opening a directory is a security risk (we are
 * told) so making it overrideable allows the chromium sandbox to
 * replace this routine with a harmless no-op.  To make this routine
 * a no-op, replace it with a stub that returns SQLITE_OK but leaves
 * *pFd set to a negative number.
 *
 * If SQLITE_OK is returned, the caller is responsible for closing
 * the file descriptor *pFd using close().
 */
static int
openDirectory(const char *zFilename, int *pFd)
{
	int ii;
	int fd;
	char zDirname[MAX_PATHNAME + 1];

	sqlite3_snprintf(MAX_PATHNAME, zDirname, "%s", zFilename);
	for (ii = (int)strlen(zDirname); ii > 0 && zDirname[ii] != '/'; ii--) ;
	if (ii > 0) {
		zDirname[ii] = '\0';
	} else {
		if (zDirname[0] != '/')
			zDirname[0] = '.';
		zDirname[1] = 0;
	}
	fd = robust_open(zDirname, O_RDONLY | O_BINARY, 0);

	*pFd = fd;
	if (fd >= 0)
		return SQLITE_OK;
	return unixLogError(SQLITE_CANTOPEN_BKPT, "openDirectory", zDirname);
}

/*
 * This function is called to handle the SQLITE_FCNTL_SIZE_HINT
 * file-control operation.  Enlarge the database to nBytes in size
 * (rounded up to the next chunk-size).  If the database is already
 * nBytes or larger, this routine is a no-op.
 */
static int
fcntlSizeHint(unixFile * pFile, i64 nByte)
{
	if (pFile->szChunk > 0) {
		i64 nSize;	/* Required file size */
		struct stat buf;	/* Used to hold return values of fstat() */

		if (fstat(pFile->h, &buf))
			return SQLITE_IOERR_FSTAT;

		nSize =
		    ((nByte + pFile->szChunk -
		      1) / pFile->szChunk) * pFile->szChunk;
		if (nSize > (i64) buf.st_size) {
			int nBlk = buf.st_blksize;	/* File-system block size */
			int nWrite = 0;	/* Number of bytes written by seekAndWrite */
			i64 iWrite;	/* Next offset to write to */

			iWrite = (buf.st_size / nBlk) * nBlk + nBlk - 1;
			assert(iWrite >= buf.st_size);
			assert(((iWrite + 1) % nBlk) == 0);
			for ( /*no-op */ ; iWrite < nSize + nBlk - 1;
			     iWrite += nBlk) {
				if (iWrite >= nSize)
					iWrite = nSize - 1;
				nWrite = seekAndWrite(pFile, iWrite, "", 1);
				if (nWrite != 1)
					return SQLITE_IOERR_WRITE;
			}
		}
	}
	if (pFile->mmapSizeMax > 0 && nByte > pFile->mmapSize) {
		int rc;
		if (pFile->szChunk <= 0) {
			if (robust_ftruncate(pFile->h, nByte)) {
				storeLastErrno(pFile, errno);
				return unixLogError(SQLITE_IOERR_TRUNCATE,
						    "ftruncate", pFile->zPath);
			}
		}

		rc = unixMapfile(pFile, nByte);
		return rc;
	}

	return SQLITE_OK;
}

/* Forward declaration */
static int unixGetTempname(int nBuf, char *zBuf);

/*
 * Information and control of an open file handle.
 */
static int
unixFileControl(sqlite3_file * id, int op, void *pArg)
{
	unixFile *pFile = (unixFile *) id;
	switch (op) {
	case SQLITE_FCNTL_LOCKSTATE:{
			*(int *)pArg = pFile->eFileLock;
			return SQLITE_OK;
		}
	case SQLITE_FCNTL_LAST_ERRNO:{
			*(int *)pArg = pFile->lastErrno;
			return SQLITE_OK;
		}
	case SQLITE_FCNTL_CHUNK_SIZE:{
			pFile->szChunk = *(int *)pArg;
			return SQLITE_OK;
		}
	case SQLITE_FCNTL_SIZE_HINT:{
			int rc;
			rc = fcntlSizeHint(pFile, *(i64 *) pArg);
			return rc;
		}
	case SQLITE_FCNTL_VFSNAME:{
			*(char **)pArg =
			    sqlite3_mprintf("%s", pFile->pVfs->zName);
			return SQLITE_OK;
		}
	case SQLITE_FCNTL_TEMPFILENAME:{
			char *zTFile =
			    sqlite3_malloc64(pFile->pVfs->mxPathname);
			if (zTFile) {
				unixGetTempname(pFile->pVfs->mxPathname,
						zTFile);
				*(char **)pArg = zTFile;
			}
			return SQLITE_OK;
		}
	case SQLITE_FCNTL_HAS_MOVED:{
			*(int *)pArg = fileHasMoved(pFile);
			return SQLITE_OK;
		}
	case SQLITE_FCNTL_MMAP_SIZE:{
			i64 newLimit = *(i64 *) pArg;
			int rc = SQLITE_OK;
			if (newLimit > sqlite3GlobalConfig.mxMmap) {
				newLimit = sqlite3GlobalConfig.mxMmap;
			}
			*(i64 *) pArg = pFile->mmapSizeMax;
			if (newLimit >= 0 && newLimit != pFile->mmapSizeMax
			    && pFile->nFetchOut == 0) {
				pFile->mmapSizeMax = newLimit;
				if (pFile->mmapSize > 0) {
					unixUnmapfile(pFile);
					rc = unixMapfile(pFile, -1);
				}
			}
			return rc;
		}
	}
	return SQLITE_NOTFOUND;
}

/*
 * If it is currently memory mapped, unmap file pFd.
 */
static void
unixUnmapfile(unixFile * pFd)
{
	assert(pFd->nFetchOut == 0);
	if (pFd->pMapRegion) {
		munmap(pFd->pMapRegion, pFd->mmapSizeActual);
		pFd->pMapRegion = 0;
		pFd->mmapSize = 0;
		pFd->mmapSizeActual = 0;
	}
}

/*
 * Attempt to set the size of the memory mapping maintained by file
 * descriptor pFd to nNew bytes. Any existing mapping is discarded.
 *
 * If successful, this function sets the following variables:
 *
 *       unixFile.pMapRegion
 *       unixFile.mmapSize
 *       unixFile.mmapSizeActual
 *
 * If unsuccessful, an error message is logged via sqlite3_log() and
 * the three variables above are zeroed. In this case SQLite should
 * continue accessing the database using the xRead() and xWrite()
 * methods.
 */
static void
unixRemapfile(unixFile * pFd,	/* File descriptor object */
	      i64 nNew		/* Required mapping size */
    )
{
	const char *zErr = "mmap";
	int h = pFd->h;		/* File descriptor open on db file */
	u8 *pOrig = (u8 *) pFd->pMapRegion;	/* Pointer to current file mapping */
	i64 nOrig = pFd->mmapSizeActual;	/* Size of pOrig region in bytes */
	u8 *pNew = 0;		/* Location of new mapping */
	int flags = PROT_READ;	/* Flags to pass to mmap() */

	assert(pFd->nFetchOut == 0);
	assert(nNew > pFd->mmapSize);
	assert(nNew <= pFd->mmapSizeMax);
	assert(nNew > 0);
	assert(pFd->mmapSizeActual >= pFd->mmapSize);
	assert(MAP_FAILED != 0);

	if (pOrig) {
		i64 nReuse = pFd->mmapSize;
		u8 *pReq = &pOrig[nReuse];

		/* Unmap any pages of the existing mapping that cannot be reused. */
		if (nReuse != nOrig)
			munmap(pReq, nOrig - nReuse);
		#if !defined(__APPLE__) && !defined(__FreeBSD__)
		pNew = mremap(pOrig, nReuse, nNew, MREMAP_MAYMOVE);
		zErr = "mremap";
		#else
		pNew = mmap(pReq, nNew - nReuse, flags, MAP_SHARED, h, nReuse);
		if (pNew != MAP_FAILED) {
			if (pNew != pReq) {
				munmap(pNew, nNew - nReuse);
				pNew = NULL;
			} else {
				pNew = pOrig;
			}
		}
		#endif

		/* The attempt to extend the existing mapping failed. Free it. */
		if (pNew == MAP_FAILED || pNew == NULL)
			munmap(pOrig, nReuse);
	}

	/* If pNew is still NULL, try to create an entirely new mapping. */
	if (pNew == NULL)
		pNew = mmap(0, nNew, flags, MAP_SHARED, h, 0);

	if (pNew == MAP_FAILED) {
		pNew = 0;
		nNew = 0;
		unixLogError(SQLITE_OK, zErr, pFd->zPath);

		/* If the mmap() above failed, assume that all subsequent mmap() calls
		 * will probably fail too. Fall back to using xRead/xWrite exclusively
		 * in this case.
		 */
		pFd->mmapSizeMax = 0;
	}
	pFd->pMapRegion = (void *)pNew;
	pFd->mmapSize = pFd->mmapSizeActual = nNew;
}

/*
 * Memory map or remap the file opened by file-descriptor pFd (if the file
 * is already mapped, the existing mapping is replaced by the new). Or, if
 * there already exists a mapping for this file, and there are still
 * outstanding xFetch() references to it, this function is a no-op.
 *
 * If parameter nByte is non-negative, then it is the requested size of
 * the mapping to create. Otherwise, if nByte is less than zero, then the
 * requested size is the size of the file on disk. The actual size of the
 * created mapping is either the requested size or the value configured
 * using SQLITE_FCNTL_MMAP_LIMIT, whichever is smaller.
 *
 * SQLITE_OK is returned if no error occurs (even if the mapping is not
 * recreated as a result of outstanding references) or an SQLite error
 * code otherwise.
 */
static int
unixMapfile(unixFile * pFd, i64 nMap)
{
	assert(nMap >= 0 || pFd->nFetchOut == 0);
	assert(nMap > 0 || (pFd->mmapSize == 0 && pFd->pMapRegion == 0));
	if (pFd->nFetchOut > 0)
		return SQLITE_OK;

	if (nMap < 0) {
		struct stat statbuf;	/* Low-level file information */
		if (fstat(pFd->h, &statbuf))
			return SQLITE_IOERR_FSTAT;
		nMap = statbuf.st_size;
	}
	if (nMap > pFd->mmapSizeMax) {
		nMap = pFd->mmapSizeMax;
	}

	assert(nMap > 0 || (pFd->mmapSize == 0 && pFd->pMapRegion == 0));
	if (nMap != pFd->mmapSize) {
		unixRemapfile(pFd, nMap);
	}

	return SQLITE_OK;
}

/*
 * If possible, return a pointer to a mapping of file fd starting at offset
 * iOff. The mapping must be valid for at least nAmt bytes.
 *
 * If such a pointer can be obtained, store it in *pp and return SQLITE_OK.
 * Or, if one cannot but no error occurs, set *pp to 0 and return SQLITE_OK.
 * Finally, if an error does occur, return an SQLite error code. The final
 * value of *pp is undefined in this case.
 *
 * If this function does return a pointer, the caller must eventually
 * release the reference by calling unixUnfetch().
 */
static int
unixFetch(sqlite3_file * fd MAYBE_UNUSED,
	  i64 iOff MAYBE_UNUSED,
	  int nAmt MAYBE_UNUSED, void **pp)
{
#if SQLITE_MAX_MMAP_SIZE>0
	unixFile *pFd = (unixFile *) fd;	/* The underlying database file */
#endif
	*pp = 0;

#if SQLITE_MAX_MMAP_SIZE>0
	if (pFd->mmapSizeMax > 0) {
		if (pFd->pMapRegion == 0) {
			int rc = unixMapfile(pFd, -1);
			if (rc != SQLITE_OK)
				return rc;
		}
		if (pFd->mmapSize >= iOff + nAmt) {
			*pp = &((u8 *) pFd->pMapRegion)[iOff];
			pFd->nFetchOut++;
		}
	}
#endif
	return SQLITE_OK;
}

/*
 * If the third argument is non-NULL, then this function releases a
 * reference obtained by an earlier call to unixFetch(). The second
 * argument passed to this function must be the same as the corresponding
 * argument that was passed to the unixFetch() invocation.
 *
 * Or, if the third argument is NULL, then this function is being called
 * to inform the VFS layer that, according to POSIX, any existing mapping
 * may now be invalid and should be unmapped.
 */
static int
unixUnfetch(sqlite3_file * fd, i64 iOff, void *p)
{
	unixFile *pFd = (unixFile *) fd;	/* The underlying database file */
	UNUSED_PARAMETER(iOff);

	/* If p==0 (unmap the entire file) then there must be no outstanding
	 * xFetch references. Or, if p!=0 (meaning it is an xFetch reference),
	 * then there must be at least one outstanding.
	 */
	assert((p == 0) == (pFd->nFetchOut == 0));

	/* If p!=0, it must match the iOff value. */
	assert(p == 0 || p == &((u8 *) pFd->pMapRegion)[iOff]);

	if (p) {
		pFd->nFetchOut--;
	} else {
		unixUnmapfile(pFd);
	}

	assert(pFd->nFetchOut >= 0);
	return SQLITE_OK;
}

/*
 * Here ends the implementation of all sqlite3_file methods.
 *
 ********************* End sqlite3_file Methods *******************************
 *****************************************************************************/

/*
 * This division contains definitions of sqlite3_io_methods objects that
 * implement various file locking strategies.  It also contains definitions
 * of "finder" functions.  A finder-function is used to locate the appropriate
 * sqlite3_io_methods object for a particular database file.  The pAppData
 * field of the sqlite3_vfs VFS objects are initialized to be pointers to
 * the correct finder-function for that VFS.
 *
 * Most finder functions return a pointer to a fixed sqlite3_io_methods
 * object.  The only interesting finder-function is autolockIoFinder, which
 * looks at the filesystem type and tries to guess the best locking
 * strategy from that.
 *
 * For finder-function F, two objects are created:
 *
 *    (1) The real finder-function named "FImpt()".
 *
 *    (2) A constant pointer to this function named just "F".
 *
 *
 * A pointer to the F pointer is used as the pAppData value for VFS
 * objects.  We have to do this instead of letting pAppData point
 * directly at the finder-function since C90 rules prevent a void*
 * from be cast into a function pointer.
 *
 *
 * Each instance of this macro generates two objects:
 *
 *   *  A constant sqlite3_io_methods object call METHOD that has locking
 *      methods CLOSE, LOCK, UNLOCK, CKRESLOCK.
 *
 *   *  An I/O method finder function called FINDER that returns a pointer
 *      to the METHOD object in the previous bullet.
 */
#define IOMETHODS(FINDER,METHOD,VERSION,CLOSE)     \
static const sqlite3_io_methods METHOD = {                                   \
   VERSION,                    /* iVersion */                                \
   CLOSE,                      /* xClose */                                  \
   unixRead,                   /* xRead */                                   \
   unixWrite,                  /* xWrite */                                  \
   unixFileControl,            /* xFileControl */                            \
   unixFetch,                  /* xFetch */                                  \
   unixUnfetch,                /* xUnfetch */                                \
};                                                                           \
static const sqlite3_io_methods *FINDER##Impl(const char *z, unixFile *p){   \
  UNUSED_PARAMETER(z); UNUSED_PARAMETER(p);                                  \
  return &METHOD;                                                            \
}                                                                            \
static const sqlite3_io_methods *(*const FINDER)(const char*,unixFile *p)    \
    = FINDER##Impl;

/*
 * Here are all of the sqlite3_io_methods objects for each of the
 * locking strategies.  Functions that return pointers to these methods
 * are also created.
 */
IOMETHODS(posixIoFinder,	/* Finder function name */
	  posixIoMethods,	/* sqlite3_io_methods object name */
	  3,			/* shared memory and mmap are enabled */
	  unixClose		/* xClose method */
    )
    IOMETHODS(nolockIoFinder,	/* Finder function name */
	      nolockIoMethods,	/* sqlite3_io_methods object name */
	      3,		/* shared memory is disabled */
	      nolockClose	/* xClose method */
    )

/*
 * An abstract type for a pointer to an IO method finder function:
 */
typedef const sqlite3_io_methods *(*finder_type) (const char *, unixFile *);

/****************************************************************************
 *************************** sqlite3_vfs methods ****************************
 *
 * This division contains the implementation of methods on the
 * sqlite3_vfs object.
 */

/*
 * Initialize the contents of the unixFile structure pointed to by pId.
 */
static int
fillInUnixFile(sqlite3_vfs * pVfs,	/* Pointer to vfs object */
	       int h,		/* Open file descriptor of file being opened */
	       sqlite3_file * pId,	/* Write to the unixFile structure here */
	       const char *zFilename,	/* Name of the file being opened */
	       int ctrlFlags	/* Zero or more UNIXFILE_* values */
    )
{
	const sqlite3_io_methods *pLockingStyle;
	unixFile *pNew = (unixFile *) pId;
	int rc = SQLITE_OK;

	assert(pNew->pInode == NULL);

	/* Usually the path zFilename should not be a relative pathname. The
	 * exception is when opening the proxy "conch" file in builds that
	 * include the special Apple locking styles.
	 */
	assert(zFilename == 0 || zFilename[0] == '/');

	/* No locking occurs in temporary files */
	assert(zFilename != 0 || (ctrlFlags & UNIXFILE_NOLOCK) != 0);

	pNew->h = h;
	pNew->pVfs = pVfs;
	pNew->zPath = zFilename;
	pNew->ctrlFlags = (u8) ctrlFlags;
#if SQLITE_MAX_MMAP_SIZE>0
	pNew->mmapSizeMax = sqlite3GlobalConfig.szMmap;
#endif
	if (strcmp(pVfs->zName, "unix-excl") == 0) {
		pNew->ctrlFlags |= UNIXFILE_EXCL;
	}
	if (ctrlFlags & UNIXFILE_NOLOCK) {
		pLockingStyle = &nolockIoMethods;
	} else {
		pLockingStyle =
		    (**(finder_type *) pVfs->pAppData) (zFilename, pNew);
		assert(pLockingStyle == &posixIoMethods);
	}

	if (pLockingStyle == &posixIoMethods) {
		rc = findInodeInfo(pNew, &pNew->pInode);
		if (rc != SQLITE_OK) {
			/* If an error occurred in findInodeInfo(), close the file descriptor
			 * immediately. findInodeInfo() may fail
			 * in two scenarios:
			 *
			 *   (a) A call to fstat() failed.
			 *   (b) A malloc failed.
			 *
			 * Scenario (b) may only occur if the process is holding no other
			 * file descriptors open on the same file. If there were other file
			 * descriptors on this file, then no malloc would be required by
			 * findInodeInfo(). If this is the case, it is quite safe to close
			 * handle h - as it is guaranteed that no posix locks will be released
			 * by doing so.
			 *
			 * If scenario (a) caused the error then things are not so safe. The
			 * implicit assumption here is that if fstat() fails, things are in
			 * such bad shape that dropping a lock or two doesn't matter much.
			 */
			robust_close(pNew, h, __LINE__);
			h = -1;
		}
	}
	storeLastErrno(pNew, 0);
	if (rc != SQLITE_OK) {
		if (h >= 0)
			robust_close(pNew, h, __LINE__);
	} else {
		pNew->pMethod = pLockingStyle;
		verifyDbFile(pNew);
	}
	return rc;
}

/*
 * Return the name of a directory in which to put temporary files.
 * If no suitable temporary file directory can be found, return NULL.
 */
static const char *
unixTempFileDir(void)
{
	static const char *azDirs[] = {
		0,
		0,
		"/var/tmp",
		"/usr/tmp",
		"/tmp",
		"."
	};
	unsigned int i = 0;
	struct stat buf;
	const char *zDir = sqlite3_temp_directory;

	if (!azDirs[0])
		azDirs[0] = getenv("SQLITE_TMPDIR");
	if (!azDirs[1])
		azDirs[1] = getenv("TMPDIR");
	while (1) {
		if (zDir != 0 && stat(zDir, &buf) == 0 &&
		    S_ISDIR(buf.st_mode) && access(zDir, 03) == 0)
			return zDir;
		if (i >= sizeof(azDirs) / sizeof(azDirs[0]))
			break;
		zDir = azDirs[i++];
	}
	return 0;
}

/*
 * Create a temporary file name in zBuf.  zBuf must be allocated
 * by the calling process and must be big enough to hold at least
 * pVfs->mxPathname bytes.
 */
static int
unixGetTempname(int nBuf, char *zBuf)
{
	const char *zDir;
	int iLimit = 0;

	/* It's odd to simulate an io-error here, but really this is just
	 * using the io-error infrastructure to test that SQLite handles this
	 * function failing.
	 */
	zBuf[0] = 0;

	zDir = unixTempFileDir();
	if (zDir == 0)
		return SQLITE_IOERR_GETTEMPPATH;
	do {
		u64 r;
		sqlite3_randomness(sizeof(r), &r);
		assert(nBuf > 2);
		zBuf[nBuf - 2] = 0;
		sqlite3_snprintf(nBuf, zBuf,
				 "%s/" SQLITE_TEMP_FILE_PREFIX "%llx%c", zDir,
				 r, 0);
		if (zBuf[nBuf - 2] != 0 || (iLimit++) > 10)
			return SQLITE_ERROR;
	} while (access(zBuf, 0) == 0);
	return SQLITE_OK;
}

/*
 * Search for an unused file descriptor that was opened on the database
 * file (not a journal or master-journal file) identified by pathname
 * zPath with SQLITE_OPEN_XXX flags matching those passed as the second
 * argument to this function.
 *
 * Such a file descriptor may exist if a database connection was closed
 * but the associated file descriptor could not be closed because some
 * other file descriptor open on the same file is holding a file-lock.
 * Refer to comments in the unixClose() function and the lengthy comment
 * describing "Posix Advisory Locking" at the start of this file for
 * further details. Also, ticket #4018.
 *
 * If a suitable file descriptor is found, then it is returned. If no
 * such file descriptor is located, -1 is returned.
 */
static UnixUnusedFd *
findReusableFd(const char *zPath, int flags)
{
	UnixUnusedFd *pUnused = 0;

	struct stat sStat;	/* Results of stat() call */

	/* A stat() call may fail for various reasons. If this happens, it is
	 * almost certain that an open() call on the same path will also fail.
	 * For this reason, if an error occurs in the stat() call here, it is
	 * ignored and -1 is returned. The caller will try to open a new file
	 * descriptor on the same path, fail, and return an error to SQLite.
	 *
	 * Even if a subsequent open() call does succeed, the consequences of
	 * not searching for a reusable file descriptor are not dire.
	 */
	if (0 == stat(zPath, &sStat)) {
		unixInodeInfo *pInode;

		pInode = inodeList;
		while (pInode && (pInode->fileId.dev != sStat.st_dev
				  || pInode->fileId.ino !=
				  (u64) sStat.st_ino)) {
			pInode = pInode->pNext;
		}
		if (pInode) {
			UnixUnusedFd **pp;
			for (pp = &pInode->pUnused;
			     *pp && (*pp)->flags != flags;
			     pp = &((*pp)->pNext)) ;
			pUnused = *pp;
			if (pUnused) {
				*pp = pUnused->pNext;
			}
		}
	}
	return pUnused;
}

/*
 * Find the mode, uid and gid of file zFile.
 */
static int
getFileMode(const char *zFile,	/* File name */
	    mode_t * pMode,	/* OUT: Permissions of zFile */
	    uid_t * pUid,	/* OUT: uid of zFile. */
	    gid_t * pGid	/* OUT: gid of zFile. */
    )
{
	struct stat sStat;	/* Output of stat() on database file */
	int rc = SQLITE_OK;
	if (0 == stat(zFile, &sStat)) {
		*pMode = sStat.st_mode & 0777;
		*pUid = sStat.st_uid;
		*pGid = sStat.st_gid;
	} else {
		rc = SQLITE_IOERR_FSTAT;
	}
	return rc;
}

/*
 * This function is called by unixOpen() to determine the unix permissions
 * to create new files with. If no error occurs, then SQLITE_OK is returned
 * and a value suitable for passing as the third argument to open(2) is
 * written to *pMode. If an IO error occurs, an SQLite error code is
 * returned and the value of *pMode is not modified.
 *
 * In most cases, this routine sets *pMode to 0, which will become
 * an indication to robust_open() to create the file using
 * SQLITE_DEFAULT_FILE_PERMISSIONS adjusted by the umask.
 * But if the file being opened is a regular journal file, then
 * this function queries the file-system for the permissions on the
 * corresponding database file and sets *pMode to this value. Whenever
 * possible, journal files are created using the same permissions
 * as the associated database file.
 *
 * If the SQLITE_ENABLE_8_3_NAMES option is enabled, then the
 * original filename is unavailable.  But 8_3_NAMES is only used for
 * FAT filesystems and permissions do not matter there, so just use
 * the default permissions.
 */
static int
findCreateFileMode(const char *zPath,	/* Path of file (possibly) being created */
		   int flags,	/* Flags passed as 4th argument to xOpen() */
		   mode_t * pMode,	/* OUT: Permissions to open file with */
		   uid_t * pUid,	/* OUT: uid to set on the file */
		   gid_t * pGid	/* OUT: gid to set on the file */
    )
{
	int rc = SQLITE_OK;	/* Return Code */
	*pMode = 0;
	*pUid = 0;
	*pGid = 0;
	if (flags & SQLITE_OPEN_DELETEONCLOSE) {
		*pMode = 0600;
	} else if (flags & SQLITE_OPEN_URI) {
		/* If this is a main database file and the file was opened using a URI
		 * filename, check for the "modeof" parameter. If present, interpret
		 * its value as a filename and try to copy the mode, uid and gid from
		 * that file.
		 */
		const char *z = sqlite3_uri_parameter(zPath, "modeof");
		if (z) {
			rc = getFileMode(z, pMode, pUid, pGid);
		}
	}
	return rc;
}

/*
 * Open the file zPath.
 *
 * Previously, the SQLite OS layer used three functions in place of this
 * one:
 *
 *     sqlite3OsOpenReadWrite();
 *     sqlite3OsOpenReadOnly();
 *     sqlite3OsOpenExclusive();
 *
 * These calls correspond to the following combinations of flags:
 *
 *     ReadWrite() ->     (READWRITE | CREATE)
 *     ReadOnly()  ->     (READONLY)
 *     OpenExclusive() -> (READWRITE | CREATE | EXCLUSIVE)
 *
 * The old OpenExclusive() accepted a boolean argument - "delFlag". If
 * true, the file was configured to be automatically deleted when the
 * file handle closed. To achieve the same effect using this new
 * interface, add the DELETEONCLOSE flag to those specified above for
 * OpenExclusive().
 */
static int
unixOpen(sqlite3_vfs * pVfs,	/* The VFS for which this is the xOpen method */
	 const char *zPath,	/* Pathname of file to be opened */
	 sqlite3_file * pFile,	/* The file descriptor to be filled in */
	 int flags,		/* Input flags to control the opening */
	 int *pOutFlags		/* Output flags returned to SQLite core */
    )
{
	unixFile *p = (unixFile *) pFile;
	int fd = -1;		/* File descriptor returned by open() */
	int openFlags = 0;	/* Flags to pass to open() */
	int eType = flags & 0xFFFFFF00;	/* Type of file to open */
	int noLock;		/* True to omit locking primitives */
	int rc;			/* Function Return Code */
	int ctrlFlags = 0;	/* UNIXFILE_* flags */

	int isExclusive = (flags & SQLITE_OPEN_EXCLUSIVE);
	int isDelete = (flags & SQLITE_OPEN_DELETEONCLOSE);
	int isCreate = (flags & SQLITE_OPEN_CREATE);
	int isReadonly = (flags & SQLITE_OPEN_READONLY);
	int isReadWrite = (flags & SQLITE_OPEN_READWRITE);

	/* If creating a master or main-file journal, this function will open
	 * a file-descriptor on the directory too. The first time unixSync()
	 * is called the directory file descriptor will be fsync()ed and close()d.
	 */
	int syncDir = isCreate;

	/* If argument zPath is a NULL pointer, this function is required to open
	 * a temporary file. Use this buffer to store the file name in.
	 */
	char zTmpname[MAX_PATHNAME + 2];
	const char *zName = zPath;

	/* Check the following statements are true:
	 *
	 *   (a) Exactly one of the READWRITE and READONLY flags must be set, and
	 *   (b) if CREATE is set, then READWRITE must also be set, and
	 *   (c) if EXCLUSIVE is set, then CREATE must also be set.
	 *   (d) if DELETEONCLOSE is set, then CREATE must also be set.
	 */
	assert((isReadonly == 0 || isReadWrite == 0)
	       && (isReadWrite || isReadonly));
	assert(isCreate == 0 || isReadWrite);
	assert(isExclusive == 0 || isCreate);
	assert(isDelete == 0 || isCreate);

	/* Detect a pid change and reset the PRNG.  There is a race condition
	 * here such that two or more threads all trying to open databases at
	 * the same instant might all reset the PRNG.  But multiple resets
	 * are harmless.
	 */
	if (randomnessPid != getpid()) {
		randomnessPid = getpid();
		sqlite3_randomness(0, 0);
	}

	memset(p, 0, sizeof(unixFile));

	if (eType == SQLITE_OPEN_MAIN_DB) {
		UnixUnusedFd *pUnused;
		pUnused = findReusableFd(zName, flags);
		if (pUnused) {
			fd = pUnused->fd;
		} else {
			pUnused = sqlite3_malloc64(sizeof(*pUnused));
			if (!pUnused) {
				return SQLITE_NOMEM_BKPT;
			}
		}
		p->pUnused = pUnused;

		/* Database filenames are double-zero terminated if they are not
		 * URIs with parameters.  Hence, they can always be passed into
		 * sqlite3_uri_parameter().
		 */
		assert((flags & SQLITE_OPEN_URI)
		       || zName[strlen(zName) + 1] == 0);

	} else if (!zName) {
		/* If zName is NULL, the upper layer is requesting a temp file. */
		assert(isDelete);
		rc = unixGetTempname(pVfs->mxPathname, zTmpname);
		if (rc != SQLITE_OK) {
			return rc;
		}
		zName = zTmpname;

		/* Generated temporary filenames are always double-zero terminated
		 * for use by sqlite3_uri_parameter().
		 */
		assert(zName[strlen(zName) + 1] == 0);
	}

	/* Determine the value of the flags parameter passed to POSIX function
	 * open(). These must be calculated even if open() is not called, as
	 * they may be stored as part of the file handle and used by the
	 * 'conch file' locking functions later on.
	 */
	if (isReadonly)
		openFlags |= O_RDONLY;
	if (isReadWrite)
		openFlags |= O_RDWR;
	if (isCreate)
		openFlags |= O_CREAT;
	if (isExclusive)
		openFlags |= (O_EXCL | O_NOFOLLOW);
	openFlags |= O_BINARY;
	#ifndef __APPLE__
	openFlags |= O_LARGEFILE;
	#endif

	if (fd < 0) {
		mode_t openMode;	/* Permissions to create file with */
		uid_t uid;	/* Userid for the file */
		gid_t gid;	/* Groupid for the file */
		rc = findCreateFileMode(zName, flags, &openMode, &uid, &gid);
		if (rc != SQLITE_OK) {
			assert(!p->pUnused);
			return rc;
		}
		fd = robust_open(zName, openFlags, openMode);
		assert(!isExclusive || (openFlags & O_CREAT) != 0);
		if (fd < 0 && errno != EISDIR && isReadWrite) {
			/* Failed to open the file for read/write access. Try read-only. */
			flags &= ~(SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
			openFlags &= ~(O_RDWR | O_CREAT);
			flags |= SQLITE_OPEN_READONLY;
			openFlags |= O_RDONLY;
			isReadonly = 1;
			fd = robust_open(zName, openFlags, openMode);
		}
		if (fd < 0) {
			rc = unixLogError(SQLITE_CANTOPEN_BKPT, "open", zName);
			goto open_finished;
		}
		
	}
	assert(fd >= 0);
	if (pOutFlags) {
		*pOutFlags = flags;
	}

	if (p->pUnused) {
		p->pUnused->fd = fd;
		p->pUnused->flags = flags;
	}

	if (isDelete)
		unlink(zName);

	/* Set up appropriate ctrlFlags */
	if (isDelete)
		ctrlFlags |= UNIXFILE_DELETE;
	if (isReadonly)
		ctrlFlags |= UNIXFILE_RDONLY;
	noLock = eType != SQLITE_OPEN_MAIN_DB;
	if (noLock)
		ctrlFlags |= UNIXFILE_NOLOCK;
	if (syncDir)
		ctrlFlags |= UNIXFILE_DIRSYNC;
	if (flags & SQLITE_OPEN_URI)
		ctrlFlags |= UNIXFILE_URI;

	rc = fillInUnixFile(pVfs, fd, pFile, zPath, ctrlFlags);

 open_finished:
	if (rc != SQLITE_OK) {
		sqlite3_free(p->pUnused);
	}
	return rc;
}

/*
 * Delete the file at zPath. If the dirSync argument is true, fsync()
 * the directory after deleting the file.
 */
static int
unixDelete(sqlite3_vfs * NotUsed,	/* VFS containing this as the xDelete method */
	   const char *zPath,	/* Name of file to be deleted */
	   int dirSync		/* If true, fsync() directory after deleting file */
    )
{
	int rc = SQLITE_OK;
	UNUSED_PARAMETER(NotUsed);
	if (unlink(zPath) == (-1)) {
		if (errno == ENOENT) {
			rc = SQLITE_IOERR_DELETE_NOENT;
		} else {
			rc = unixLogError(SQLITE_IOERR_DELETE, "unlink", zPath);
		}
		return rc;
	}
#ifndef SQLITE_DISABLE_DIRSYNC
	if ((dirSync & 1) != 0) {
		int fd;
		rc = openDirectory(zPath, &fd);
		if (rc == SQLITE_OK) {
			struct stat buf;
			if (fstat(fd, &buf)) {
				rc = unixLogError(SQLITE_IOERR_DIR_FSYNC,
						  "fsync", zPath);
			}
			robust_close(0, fd, __LINE__);
		} else {
			assert(rc == SQLITE_CANTOPEN);
			rc = SQLITE_OK;
		}
	}
#endif
	return rc;
}

/*
 * Write nBuf bytes of random data to the supplied buffer zBuf.
 */
static int
unixRandomness(sqlite3_vfs * NotUsed, int nBuf, char *zBuf)
{
	UNUSED_PARAMETER(NotUsed);
	assert((size_t) nBuf >= (sizeof(time_t) + sizeof(int)));

	/* We have to initialize zBuf to prevent valgrind from reporting
	 * errors.  The reports issued by valgrind are incorrect - we would
	 * prefer that the randomness be increased by making use of the
	 * uninitialized space in zBuf - but valgrind errors tend to worry
	 * some users.  Rather than argue, it seems easier just to initialize
	 * the whole array and silence valgrind, even if that means less randomness
	 * in the random seed.
	 *
	 * When testing, initializing zBuf[] to zero is all we do.  That means
	 * that we always use the same random number sequence.  This makes the
	 * tests repeatable.
	 */
	memset(zBuf, 0, nBuf);
	randomnessPid = getpid();
	return nBuf;
}

/*
 * Sleep for a little while.  Return the amount of time slept.
 * The argument is the number of microseconds we want to sleep.
 * The return value is the number of microseconds of sleep actually
 * requested from the underlying operating system, a number which
 * might be greater than or equal to the argument, but not less
 * than the argument.
 */
static int
unixSleep(sqlite3_vfs * NotUsed, int microseconds)
{
	int seconds = (microseconds + 999999) / 1000000;
	sleep(seconds);
	UNUSED_PARAMETER(NotUsed);
	return seconds * 1000000;
}

/* Fake system time in seconds since 1970. */
int sqlite3_current_time = 0;

/*
 * Find the current time (in Universal Coordinated Time).  Write into *piNow
 * the current time and date as a Julian Day number times 86_400_000.  In
 * other words, write into *piNow the number of milliseconds since the Julian
 * epoch of noon in Greenwich on November 24, 4714 B.C according to the
 * proleptic Gregorian calendar.
 *
 * On success, return SQLITE_OK.  Return SQLITE_ERROR if the time and date
 * cannot be found.
 */
static int
unixCurrentTimeInt64(sqlite3_vfs * NotUsed, sqlite3_int64 * piNow)
{
	static const sqlite3_int64 unixEpoch =
	    24405875 * (sqlite3_int64) 8640000;
	int rc = SQLITE_OK;
	struct timeval sNow;
	(void)gettimeofday(&sNow, 0);	/* Cannot fail given valid arguments */
	*piNow =
	    unixEpoch + 1000 * (sqlite3_int64) sNow.tv_sec +
	    sNow.tv_usec / 1000;

#ifdef SQLITE_TEST
	if (sqlite3_current_time) {
		*piNow =
		    1000 * (sqlite3_int64) sqlite3_current_time + unixEpoch;
	}
#endif
	UNUSED_PARAMETER(NotUsed);
	return rc;
}

/*
 * The xGetLastError() method is designed to return a better
 * low-level error message when operating-system problems come up
 * during SQLite operation.  Only the integer return code is currently
 * used.
 */
static int
unixGetLastError(sqlite3_vfs * NotUsed, int NotUsed2, char *NotUsed3)
{
	UNUSED_PARAMETER(NotUsed);
	UNUSED_PARAMETER(NotUsed2);
	UNUSED_PARAMETER(NotUsed3);
	return errno;
}

/*
 *********************** End of sqlite3_vfs methods ***************************
 *****************************************************************************/

/*
 * The proxy locking style is intended for use with AFP filesystems.
 * And since AFP is only supported on MacOSX, the proxy locking is also
 * restricted to MacOSX.
 *
 *
 ****************** End of the proxy lock implementation **********************
 *****************************************************************************/

#define UNIXVFS(VFSNAME, FINDER) {                        \
    3,                    /* iVersion */                    \
    sizeof(unixFile),     /* szOsFile */                    \
    MAX_PATHNAME,         /* mxPathname */                  \
    NULL,                 /* pNext */                       \
    VFSNAME,              /* zName */                       \
    (void*)&FINDER,       /* pAppData */                    \
    unixOpen,             /* xOpen */                       \
    unixDelete,           /* xDelete */                     \
    unixRandomness,       /* xRandomness */                 \
    unixSleep,            /* xSleep */                      \
    NULL,                 /* xCurrentTime */                \
    unixGetLastError,     /* xGetLastError */               \
    unixCurrentTimeInt64, /* xCurrentTimeInt64 */           \
  }

/*
 * Initialize the operating system interface.
 *
 * This routine registers all VFS implementations for unix-like operating
 * systems.  This routine, and the sqlite3_os_end() routine that follows,
 * should be the only routines in this file that are visible from other
 * files.
 *
 * This routine is called once during SQLite initialization and by a
 * single thread.  The memory allocation subsystem have not
 * necessarily been initialized when this routine \is called, and so they
 * should not be used.
 */
int
sqlite3_os_init(void)
{
	/*
	 * All default VFSes for unix are contained in the following array.
	 *
	 * Note that the sqlite3_vfs.pNext field of the VFS object is modified
	 * by the SQLite core when the VFS is registered.  So the following
	 * array cannot be const.
	 */
	static sqlite3_vfs aVfs[] = {
		UNIXVFS("unix-none", nolockIoFinder),
		UNIXVFS("unix-excl", posixIoFinder)
	};

	/* Register all VFSes defined in the aVfs[] array. */
	for (unsigned int i = 0; i < (sizeof(aVfs) / sizeof(sqlite3_vfs)); i++)
		sqlite3_vfs_register(&aVfs[i], i == 0);
	return SQLITE_OK;
}

/*
 * Shutdown the operating system interface.
 *
 * Some operating systems might need to do some cleanup in this routine,
 * to release dynamically allocated objects.  But not on unix.
 * This routine is a no-op for unix.
 */
int
sqlite3_os_end(void)
{
	return SQLITE_OK;
}