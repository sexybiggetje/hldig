/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 1996, 1997, 1998, 1999
 *	Sleepycat Software.  All rights reserved.
 */

#include "db_config.h"

#ifndef lint
static const char sccsid[] = "@(#)env_open.c	11.8 (Sleepycat) 11/10/99";
#endif /* not lint */

#ifndef NO_SYSTEM_INCLUDES

#include <sys/types.h>

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER //_WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#endif //!NO_SYSTEM_INCLUDES

#include "db_int.h"
#include "db_page.h"
#include "db_shash.h"
#include "btree.h"
#include "hash.h"
#include "qam.h"
#include "lock.h"
#include "log.h"
#include "mp.h"
#include "txn.h"

static int CDB___dbenv_config
    __P((DB_ENV *, const char *, char * const *, u_int32_t));
static int CDB___dbenv_refresh __P((DB_ENV *));
static int CDB___db_home __P((DB_ENV *, const char *, u_int32_t));
static int CDB___db_parse __P((DB_ENV *, char *));
static int CDB___db_tmp_open __P((DB_ENV *, u_int32_t, char *, DB_FH *));

/*
 * CDB_db_version --
 *	Return version information.
 */
char *
CDB_db_version(majverp, minverp, patchp)
	int *majverp, *minverp, *patchp;
{
	if (majverp != NULL)
		*majverp = DB_VERSION_MAJOR;
	if (minverp != NULL)
		*minverp = DB_VERSION_MINOR;
	if (patchp != NULL)
		*patchp = DB_VERSION_PATCH;
	return ((char *)DB_VERSION_STRING);
}

/*
 * CDB___dbenv_open --
 *	Initialize an environment.
 *
 * PUBLIC: int CDB___dbenv_open __P((DB_ENV *,
 * PUBLIC:	   const char *, char * const *, u_int32_t, int));
 */
int
CDB___dbenv_open(dbenv, db_home, db_config, flags, mode)
	DB_ENV *dbenv;
	const char *db_home;
	char * const *db_config;
	u_int32_t flags;
	int mode;
{
	DB_ENV *rm_dbenv;
	int ret;

#undef	OKFLAGS
#define	OKFLAGS								\
	DB_CREATE | DB_INIT_CDB | DB_INIT_LOCK | DB_INIT_LOG |		\
	DB_INIT_MPOOL | DB_INIT_TXN | DB_LOCKDOWN | DB_NOMMAP |		\
	DB_PRIVATE | DB_RECOVER | DB_RECOVER_FATAL | DB_SYSTEM_MEM |	\
	DB_THREAD | DB_TXN_NOSYNC | DB_USE_ENVIRON | DB_USE_ENVIRON_ROOT
#undef	OKFLAGS_CDB
#define	OKFLAGS_CDB							\
	DB_CREATE | DB_INIT_CDB | DB_INIT_MPOOL | DB_LOCKDOWN |		\
	DB_NOMMAP | DB_PRIVATE | DB_SYSTEM_MEM | DB_THREAD |		\
	DB_USE_ENVIRON | DB_USE_ENVIRON_ROOT

	if ((ret = CDB___db_fchk(dbenv, "DBENV->open", flags, OKFLAGS)) != 0)
		return (ret);
	if (LF_ISSET(DB_INIT_CDB) &&
	    (ret = CDB___db_fchk(dbenv, "DBENV->open", flags, OKFLAGS_CDB)) != 0)
		return (ret);
	if ((ret = CDB___db_fcchk(dbenv,
	    "DBENV->open", flags, DB_PRIVATE, DB_SYSTEM_MEM)) != 0)
		return (ret);

	/*
	 * If we're doing recovery, destroy the environment so that we create
	 * all the regions from scratch.  I'd like to reuse already created
	 * regions, but that's hard.  We would have to create the environment
	 * region from scratch, at least, as we have no way of knowing if its
	 * linked lists are corrupted.
	 *
	 * I suppose we could set flags while modifying those links, but that
	 * is going to be difficult to get right.  The major concern I have
	 * is if the application stomps the environment with a rogue pointer.
	 * We have no way of detecting that, and we could be forced into a
	 * situation where we start up and then crash, repeatedly.
	 *
	 * Note that we do not check any flags like DB_PRIVATE before calling
	 * remove.  We don't care if the current environment was private or
	 * not, we just want to nail any files that are left-over for whatever
	 * reason, from whatever session.
	 */
	if (LF_ISSET(DB_RECOVER | DB_RECOVER_FATAL)) {
		if ((ret = CDB_db_env_create(&rm_dbenv, 0)) != 0)
			return (ret);
		if ((ret =
		    dbenv->remove(rm_dbenv, db_home, db_config, DB_FORCE)) != 0)
			return (ret);
	}

	/* Initialize the DB_ENV structure. */
	if ((ret = CDB___dbenv_config(dbenv, db_home, db_config, flags)) != 0)
		goto err;

	/* Convert the DBENV->open flags to internal flags. */
	if (LF_ISSET(DB_CREATE))
		F_SET(dbenv, DB_ENV_CREATE);
	if (LF_ISSET(DB_LOCKDOWN))
		F_SET(dbenv, DB_ENV_LOCKDOWN);
	if (LF_ISSET(DB_NOMMAP))
		F_SET(dbenv, DB_ENV_NOMMAP);
	if (LF_ISSET(DB_PRIVATE))
		F_SET(dbenv, DB_ENV_PRIVATE);
	if (LF_ISSET(DB_SYSTEM_MEM))
		F_SET(dbenv, DB_ENV_SYSTEM_MEM);
	if (LF_ISSET(DB_THREAD))
		F_SET(dbenv, DB_ENV_THREAD);
	if (LF_ISSET(DB_TXN_NOSYNC))
		F_SET(dbenv, DB_ENV_TXN_NOSYNC);

	/* Default permissions are read-write for both owner and group. */
	dbenv->db_mode = mode == 0 ? CDB___db_omode("rwrw--") : mode;

	/* Initialize for CDB product. */
	if (LF_ISSET(DB_INIT_CDB)) {
		LF_SET(DB_INIT_LOCK);
		F_SET(dbenv, DB_ENV_CDB);
	}

	/* Create/join the environment. */
	if ((ret = CDB___db_e_attach(dbenv)) != 0)
		goto err;

	/*
	 * Initialize the subsystems.  TXN uses log/lock, so do them first.
	 */
	if (LF_ISSET(DB_INIT_MPOOL) && (ret = CDB___memp_open(dbenv)) != 0)
		goto err;

	/*
	 * Transactions imply logging.  While almost all applications will want
	 * both locking and logging, it would not be unreasonable for a single
	 * threaded process to want transactions for atomicity guarantees, but
	 * not necessarily need concurrency.
	 */
	if (LF_ISSET(DB_INIT_LOG | DB_INIT_TXN)) {
		if ((ret = CDB___log_open(dbenv)) != 0)
			goto err;

		F_SET(dbenv, DB_ENV_LOGGING);
	}
	if (LF_ISSET(DB_INIT_LOCK)) {
		if ((ret = CDB___lock_open(dbenv)) != 0)
			goto err;

		if (!F_ISSET(dbenv, DB_ENV_CDB))
			F_SET(dbenv, DB_ENV_LOCKING);
	}
	if (LF_ISSET(DB_INIT_TXN)) {
		if ((ret = CDB___txn_open(dbenv)) != 0)
			goto err;
		F_SET(dbenv, DB_ENV_TXN);

		/*
		 * If the application is running with transactions, initialize
		 * the function tables.
		 */
		if ((ret = CDB___bam_init_recover(dbenv)) != 0)
			goto err;
		if ((ret = CDB___crdel_init_recover(dbenv)) != 0)
			goto err;
		if ((ret = CDB___db_init_recover(dbenv)) != 0)
			goto err;
		if ((ret = CDB___ham_init_recover(dbenv)) != 0)
			goto err;
		if ((ret = CDB___log_init_recover(dbenv)) != 0)
			goto err;
		if ((ret = CDB___qam_init_recover(dbenv)) != 0)
			goto err;
		if ((ret = CDB___txn_init_recover(dbenv)) != 0)
			goto err;

		/*
		 * If the application specified their own recovery
		 * initialization function, call it.
		 */
		if (dbenv->db_recovery_init != NULL &&
		    (ret = dbenv->db_recovery_init(dbenv)) != 0)
			goto err;

		/* Perform recovery for any previous run. */
		if (LF_ISSET(DB_RECOVER | DB_RECOVER_FATAL) &&
		    (ret = CDB___db_apprec(dbenv,
		    LF_ISSET(DB_RECOVER | DB_RECOVER_FATAL))) != 0)
			goto err;
	}
	return (0);

err:	(void)CDB___dbenv_refresh(dbenv);
	return (ret);
}

/*
 * CDB___dbenv_remove --
 *	Discard an environment.
 *
 * PUBLIC: int CDB___dbenv_remove __P((DB_ENV *,
 * PUBLIC:	   const char *, char * const *, u_int32_t));
 */
int
CDB___dbenv_remove(dbenv, db_home, db_config, flags)
	DB_ENV *dbenv;
	const char *db_home;
	char * const *db_config;
	u_int32_t flags;
{
	int ret, t_ret;

#undef	OKFLAGS
#define	OKFLAGS								\
	DB_FORCE | DB_USE_ENVIRON | DB_USE_ENVIRON_ROOT

	/* Validate arguments. */
	if ((ret = CDB___db_fchk(dbenv, "DBENV->remove", flags, OKFLAGS)) != 0)
		return (ret);

	/* Initialize the DB_ENV structure. */
	if ((ret = CDB___dbenv_config(dbenv, db_home, db_config, flags)) != 0)
		goto err;

	/* Remove the environment. */
	ret = CDB___db_e_remove(dbenv, LF_ISSET(DB_FORCE) ? 1 : 0);

	/* Discard any resources we've acquired. */
err:	if ((t_ret = CDB___dbenv_refresh(dbenv)) != 0 && ret == 0)
		ret = t_ret;

	memset(dbenv, CLEAR_BYTE, sizeof(DB_ENV));
	CDB___os_free(dbenv, sizeof(DB_ENV));

	return (ret);
}

/*
 * CDB___dbenv_config --
 *	Initialize the DB_ENV structure.
 */
static int
CDB___dbenv_config(dbenv, db_home, db_config, flags)
	DB_ENV *dbenv;
	const char *db_home;
	char * const *db_config;
	u_int32_t flags;
{
	FILE *fp;
	int ret;
	char * const *p;
	char *lp, buf[MAXPATHLEN * 2];

	/* Set the database home. */
	if ((ret = CDB___db_home(dbenv, db_home, flags)) != 0)
		return (ret);

	/* Parse the config array. */
	for (p = db_config; p != NULL && *p != NULL; ++p)
		if ((ret = CDB___db_parse(dbenv, *p)) != 0)
			return (ret);

	/*
	 * Parse the config file.
	 *
	 * XXX
	 * Don't use sprintf(3)/snprintf(3) -- the former is dangerous, and
	 * the latter isn't standard, and we're manipulating strings handed
	 * us by the application.
	 */
	if (dbenv->db_home != NULL) {
#define	CONFIG_NAME	"/DB_CONFIG"
		if (strlen(dbenv->db_home) +
		    strlen(CONFIG_NAME) + 1 > sizeof(buf)) {
			ret = ENAMETOOLONG;
			return (ret);
		}
		(void)strcpy(buf, dbenv->db_home);
		(void)strcat(buf, CONFIG_NAME);
		if ((fp = fopen(buf, "r")) != NULL) {
			while (fgets(buf, sizeof(buf), fp) != NULL) {
				if ((lp = strchr(buf, '\n')) == NULL) {
					CDB___db_err(dbenv,
					    "%s: line too long", CONFIG_NAME);
					(void)fclose(fp);
					ret = EINVAL;
					return (ret);
				}
				*lp = '\0';
				if (buf[0] == '\0' ||
				    buf[0] == '#' || isspace((int)buf[0]))
					continue;

				if ((ret = CDB___db_parse(dbenv, buf)) != 0) {
					(void)fclose(fp);
					return (ret);
				}
			}
			(void)fclose(fp);
		}
	}

	/* Set up the tmp directory path. */
	if (dbenv->db_tmp_dir == NULL && (ret = CDB___os_tmpdir(dbenv, flags)) != 0)
		return (ret);

	/*
	 * The locking file descriptor is rarely on.  Set the fd to -1, not
	 * because it's ever tested, but to make sure we catch mistakes.
	 */
	if ((ret =
	    CDB___os_calloc(1, sizeof(*dbenv->lockfhp), &dbenv->lockfhp)) != 0)
		return (ret);
	dbenv->lockfhp->fd = -1;

	/*
	 * Flag that the DB_ENV structure has been initialized.  Note, this
	 * must be set before calling into the subsystems as it's used during
	 * file naming.
	 */
	F_SET(dbenv, DB_ENV_OPEN_CALLED);

	return (0);
}

/*
 * CDB___dbenv_close --
 *	DB_ENV destructor.
 *
 * PUBLIC: int CDB___dbenv_close __P((DB_ENV *, u_int32_t));
 */
int
CDB___dbenv_close(dbenv, flags)
	DB_ENV *dbenv;
	u_int32_t flags;
{
	int ret;

	COMPQUIET(flags, 0);

	PANIC_CHECK(dbenv);

	ret = CDB___dbenv_refresh(dbenv);

	/* Discard the structure if we allocated it. */
	if (!F_ISSET(dbenv, DB_ENV_USER_ALLOC)) {
		memset(dbenv, CLEAR_BYTE, sizeof(DB_ENV));
		CDB___os_free(dbenv, sizeof(DB_ENV));
	}

	return (ret);
}

/*
 * CDB___dbenv_refresh --
 *	Refresh the DB_ENV structure, releasing any allocated resources.
 */
static int
CDB___dbenv_refresh(dbenv)
	DB_ENV *dbenv;
{
	int ret, t_ret;
	char **p;

	ret = 0;

	/*
	 * Close subsystems, in the reverse order they were opened (txn
	 * must be first, it may want to discard locks and flush the log).
	 */
	if (dbenv->tx_handle != NULL) {
		if ((t_ret = CDB___txn_close(dbenv)) != 0 && ret == 0)
			ret = t_ret;
		dbenv->tx_handle = NULL;
	}

	if (dbenv->lk_handle != NULL) {
		if ((t_ret = CDB___lock_close(dbenv)) != 0 && ret == 0)
			ret = t_ret;
		dbenv->lk_handle = NULL;
	}
	CDB___lock_dbenv_close(dbenv);

	if (dbenv->lg_handle != NULL) {
		if ((t_ret = CDB___log_close(dbenv)) != 0 && ret == 0)
			ret = t_ret;
		dbenv->lg_handle = NULL;
	}

	if (dbenv->mp_handle != NULL) {
		if ((t_ret = CDB___memp_close(dbenv)) != 0 && ret == 0)
			ret = t_ret;
		dbenv->mp_handle = NULL;
	}

	/* Detach from the region. */
	if (dbenv->reginfo != NULL) {
		if ((t_ret = CDB___db_e_detach(dbenv, 0)) != 0 && ret == 0)
			ret = t_ret;
		/*
		 * !!!
		 * Don't free dbenv->reginfo or set the reference to NULL,
		 * that was done by CDB___db_e_detach().
		 */
	}

	/* Clean up the structure. */
	dbenv->db_panic = 0;

	if (dbenv->db_home != NULL) {
		CDB___os_freestr(dbenv->db_home);
		dbenv->db_home = NULL;
	}
	if (dbenv->db_log_dir != NULL) {
		CDB___os_freestr(dbenv->db_log_dir);
		dbenv->db_log_dir = NULL;
	}
	if (dbenv->db_tmp_dir != NULL) {
		CDB___os_freestr(dbenv->db_tmp_dir);
		dbenv->db_tmp_dir = NULL;
	}
	if (dbenv->db_data_dir != NULL) {
		for (p = dbenv->db_data_dir; *p != NULL; ++p)
			CDB___os_freestr(*p);
		CDB___os_free(dbenv->db_data_dir,
		    dbenv->data_cnt * sizeof(char **));
		dbenv->db_data_dir = NULL;
	}
	dbenv->data_cnt = dbenv->data_next = 0;

	dbenv->db_mode = 0;

	if (dbenv->lockfhp != NULL) {
		CDB___os_free(dbenv->lockfhp, sizeof(*dbenv->lockfhp));
		dbenv->lockfhp = NULL;
	}

	if (dbenv->dtab != NULL) {
		CDB___os_free(dbenv->dtab,
		    dbenv->dtab_size * sizeof(dbenv->dtab[0]));
		dbenv->dtab = NULL;
		dbenv->dtab_size = 0;
	}

	dbenv->mp_mmapsize = 0;
	dbenv->links.tqe_next = NULL;
	dbenv->links.tqe_prev = NULL;
	dbenv->xa_rmid = 0;
	dbenv->xa_txn = 0;

	F_CLR(dbenv, ~(DB_ENV_STANDALONE | DB_ENV_USER_ALLOC));

	return (ret);
}

#define	DB_ADDSTR(add) {						\
	if ((add) != NULL) {						\
		/* If leading slash, start over. */			\
		if (CDB___os_abspath(add)) {				\
			p = str;					\
			slash = 0;					\
		}							\
		/* Append to the current string. */			\
		len = strlen(add);					\
		if (slash)						\
			*p++ = PATH_SEPARATOR[0];			\
		memcpy(p, add, len);					\
		p += len;						\
		slash = strchr(PATH_SEPARATOR, p[-1]) == NULL;		\
	}								\
}

/*
 * CDB___db_appname --
 *	Given an optional DB environment, directory and file name and type
 *	of call, build a path based on the DBENV->open rules, and return
 *	it in allocated space.
 *
 * PUBLIC: int CDB___db_appname __P((DB_ENV *, APPNAME,
 * PUBLIC:    const char *, const char *, u_int32_t, DB_FH *, char **));
 */
int
CDB___db_appname(dbenv, appname, dir, file, tmp_oflags, fhp, namep)
	DB_ENV *dbenv;
	APPNAME appname;
	const char *dir, *file;
	u_int32_t tmp_oflags;
	DB_FH *fhp;
	char **namep;
{
	DB_ENV etmp;
	size_t len, str_len;
	int data_entry, ret, slash, tmp_create, tmp_free;
	const char *a, *b, *c;
	char *p, *str;

	a = b = c = NULL;
	data_entry = -1;
	tmp_create = tmp_free = 0;

	/*
	 * We don't return a name when creating temporary files, just a
	 * file handle.  Default to an error now.
	 */
	if (fhp != NULL)
		F_CLR(fhp, DB_FH_VALID);
	if (namep != NULL)
		*namep = NULL;

	/*
	 * Absolute path names are never modified.  If the file is an absolute
	 * path, we're done.  If the directory is, simply append the file and
	 * return.
	 */
	if (file != NULL && CDB___os_abspath(file))
		return (CDB___os_strdup(file, namep));
	if (dir != NULL && CDB___os_abspath(dir)) {
		a = dir;
		goto done;
	}

	/*
	 * DB_ENV  DIR	   APPNAME	   RESULT
	 * -------------------------------------------
	 * null	   null	   none		   <tmp>/file
	 * null	   set	   none		   DIR/file
	 * set	   null	   none		   DB_HOME/file
	 * set	   set	   none		   DB_HOME/DIR/file
	 *
	 * DB_ENV  FILE	   APPNAME	   RESULT
	 * -------------------------------------------
	 * null	   null	   DB_APP_DATA	   <tmp>/<create>
	 * null	   set	   DB_APP_DATA	   ./file
	 * set	   null	   DB_APP_DATA	   <tmp>/<create>
	 * set	   set	   DB_APP_DATA	   DB_HOME/DB_DATA_DIR/file
	 *
	 * DB_ENV  DIR	   APPNAME	   RESULT
	 * -------------------------------------------
	 * null	   null	   DB_APP_LOG	   <tmp>/file
	 * null	   set	   DB_APP_LOG	   DIR/file
	 * set	   null	   DB_APP_LOG	   DB_HOME/DB_LOG_DIR/file
	 * set	   set	   DB_APP_LOG	   DB_HOME/DB_LOG_DIR/DIR/file
	 *
	 * DB_ENV	   APPNAME	   RESULT
	 * -------------------------------------------
	 * null		   DB_APP_TMP*	   <tmp>/<create>
	 * set		   DB_APP_TMP*	   DB_HOME/DB_TMP_DIR/<create>
	 */
retry:	switch (appname) {
	case DB_APP_NONE:
		if (dbenv == NULL || !F_ISSET(dbenv, DB_ENV_OPEN_CALLED)) {
			if (dir == NULL)
				goto tmp;
			a = dir;
		} else {
			a = dbenv->db_home;
			b = dir;
		}
		break;
	case DB_APP_DATA:
		if (dir != NULL) {
			CDB___db_err(dbenv,
			    "DB_APP_DATA: illegal directory specification");
			return (EINVAL);
		}

		if (file == NULL) {
			tmp_create = 1;
			goto tmp;
		}
		if (dbenv == NULL || !F_ISSET(dbenv, DB_ENV_OPEN_CALLED))
			a = PATH_DOT;
		else {
			a = dbenv->db_home;
			if (dbenv->db_data_dir != NULL &&
			    (b = dbenv->db_data_dir[++data_entry]) == NULL) {
				data_entry = -1;
				b = dbenv->db_data_dir[0];
			}
		}
		break;
	case DB_APP_LOG:
		if (dbenv == NULL || !F_ISSET(dbenv, DB_ENV_OPEN_CALLED)) {
			if (dir == NULL)
				goto tmp;
			a = dir;
		} else {
			a = dbenv->db_home;
			b = dbenv->db_log_dir;
			c = dir;
		}
		break;
	case DB_APP_TMP:
		if (dir != NULL || file != NULL) {
			CDB___db_err(dbenv,
		    "DB_APP_TMP: illegal directory or file specification");
			return (EINVAL);
		}

		tmp_create = 1;
		if (dbenv == NULL || !F_ISSET(dbenv, DB_ENV_OPEN_CALLED))
			goto tmp;
		else {
			a = dbenv->db_home;
			b = dbenv->db_tmp_dir;
		}
		break;
	}

	/* Reference a file from the appropriate temporary directory. */
	if (0) {
tmp:		if (dbenv == NULL || !F_ISSET(dbenv, DB_ENV_OPEN_CALLED)) {
			memset(&etmp, 0, sizeof(etmp));
			if ((ret = CDB___os_tmpdir(&etmp, DB_USE_ENVIRON)) != 0)
				return (ret);
			tmp_free = 1;
			a = etmp.db_tmp_dir;
		} else
			a = dbenv->db_tmp_dir;
	}

done:	len =
	    (a == NULL ? 0 : strlen(a) + 1) +
	    (b == NULL ? 0 : strlen(b) + 1) +
	    (c == NULL ? 0 : strlen(c) + 1) +
	    (file == NULL ? 0 : strlen(file) + 1);

	/*
	 * Allocate space to hold the current path information, as well as any
	 * temporary space that we're going to need to create a temporary file
	 * name.
	 */
#define	DB_TRAIL	"BDBXXXXXX"
	str_len = len + sizeof(DB_TRAIL) + 10;
	if ((ret = CDB___os_malloc(str_len, NULL, &str)) != 0) {
		if (tmp_free)
			CDB___os_freestr(etmp.db_tmp_dir);
		return (ret);
	}

	slash = 0;
	p = str;
	DB_ADDSTR(a);
	DB_ADDSTR(b);
	DB_ADDSTR(file);
	*p = '\0';

	/* Discard any space allocated to find the temp directory. */
	if (tmp_free) {
		CDB___os_freestr(etmp.db_tmp_dir);
		tmp_free = 0;
	}

	/*
	 * If we're opening a data file, see if it exists.  If it does,
	 * return it, otherwise, try and find another one to open.
	 */
	if (data_entry != -1 && CDB___os_exists(str, NULL) != 0) {
		CDB___os_free(str, str_len);
		a = b = c = NULL;
		goto retry;
	}

	/* Create the file if so requested. */
	if (tmp_create &&
	    (ret = CDB___db_tmp_open(dbenv, tmp_oflags, str, fhp)) != 0) {
		CDB___os_free(str, str_len);
		return (ret);
	}

	if (namep == NULL)
		CDB___os_free(str, str_len);
	else
		*namep = str;
	return (0);
}

/*
 * CDB___db_home --
 *	Find the database home.
 */
static int
CDB___db_home(dbenv, db_home, flags)
	DB_ENV *dbenv;
	const char *db_home;
	u_int32_t flags;
{
	const char *p;

	/*
	 * Use db_home by default, this allows utilities to reasonably
	 * override the environment either explicitly or by using a -h
	 * option.  Otherwise, use the environment if it's permitted
	 * and initialized.
	 */
	if ((p = db_home) == NULL &&
	    (LF_ISSET(DB_USE_ENVIRON) ||
	    (LF_ISSET(DB_USE_ENVIRON_ROOT) && CDB___os_isroot() == 0)) &&
	    (p = getenv("DB_HOME")) != NULL && p[0] == '\0') {
		CDB___db_err(dbenv, "illegal DB_HOME environment variable");
		return (EINVAL);
	}

	return (p == NULL ? 0 : CDB___os_strdup(p, &dbenv->db_home));
}

/*
 * CDB___db_parse --
 *	Parse a single NAME VALUE pair.
 */
static int
CDB___db_parse(dbenv, s)
	DB_ENV *dbenv;
	char *s;
{
	int ret;
	char *local_s, *name, *value, **p, *tp;

	/*
	 * We need to strdup the argument in case the caller passed us
	 * static data.
	 */
	if ((ret = CDB___os_strdup(s, &local_s)) != 0)
		return (ret);

	/*
	 * Name/value pairs are parsed as two white-space separated strings.
	 * Leading and trailing white-space is trimmed from the value, but
	 * it may contain embedded white-space.  Note: we use the isspace(3)
	 * macro because it's more portable, but that means that you can use
	 * characters like form-feed to separate the strings.
	 */
	name = local_s;
	for (tp = name; *tp != '\0' && !isspace((int)*tp); ++tp)
		;
	if (*tp == '\0' || tp == name)
		goto illegal;
	*tp = '\0';
	for (++tp; isspace((int)*tp); ++tp)
		;
	if (*tp == '\0')
		goto illegal;
	value = tp;
	for (++tp; *tp != '\0'; ++tp)
		;
	for (--tp; isspace((int)*tp); --tp)
		;
	++tp;
	if (tp == value) {
illegal:	ret = EINVAL;
		CDB___db_err(dbenv, "illegal name-value pair: %s", s);
		goto err;
	}
	*tp = '\0';

#define	DATA_INIT_CNT	20			/* Start with 20 data slots. */
	if (!strcmp(name, "DB_DATA_DIR")) {
		if (dbenv->db_data_dir == NULL) {
			if ((ret = CDB___os_calloc(DATA_INIT_CNT,
			    sizeof(char **), &dbenv->db_data_dir)) != 0)
				goto err;
			dbenv->data_cnt = DATA_INIT_CNT;
		} else if (dbenv->data_next == dbenv->data_cnt - 1) {
			dbenv->data_cnt *= 2;
			if ((ret =
			    CDB___os_realloc(dbenv->data_cnt * sizeof(char **),
			    NULL, &dbenv->db_data_dir)) != 0)
				goto err;
		}
		p = &dbenv->db_data_dir[dbenv->data_next++];
	} else if (!strcmp(name, "DB_LOG_DIR")) {
		if (dbenv->db_log_dir != NULL)
			CDB___os_freestr(dbenv->db_log_dir);
		p = &dbenv->db_log_dir;
	} else if (!strcmp(name, "DB_TMP_DIR")) {
		if (dbenv->db_tmp_dir != NULL)
			CDB___os_freestr(dbenv->db_tmp_dir);
		p = &dbenv->db_tmp_dir;
	} else
		goto err;

	ret = CDB___os_strdup(value, p);

err:	/*
	 * !!!
	 * We've inserted nuls into the string as part of parsing it,
	 * so we can't use CDB___os_freestr(), the length won't be correct.
	 */
	CDB___os_free(local_s, 0); return (ret);
}

/*
 * CDB___db_tmp_open --
 *	Create a temporary file.
 */
static int
CDB___db_tmp_open(dbenv, tmp_oflags, path, fhp)
	DB_ENV *dbenv;
	u_int32_t tmp_oflags;
	char *path;
	DB_FH *fhp;
{
	u_long pid;
	int mode, isdir, ret;
	const char *p;
	char *trv;

	/*
	 * Check the target directory; if you have six X's and it doesn't
	 * exist, this runs for a *very* long time.
	 */
	if ((ret = CDB___os_exists(path, &isdir)) != 0) {
		CDB___db_err(dbenv, "%s: %s", path, CDB_db_strerror(ret));
		return (ret);
	}
	if (!isdir) {
		CDB___db_err(dbenv, "%s: %s", path, CDB_db_strerror(EINVAL));
		return (EINVAL);
	}

	/* Build the path. */
	for (trv = path; *trv != '\0'; ++trv)
		;
	*trv = PATH_SEPARATOR[0];
	for (p = DB_TRAIL; (*++trv = *p) != '\0'; ++p)
		;

	/*
	 * Replace the X's with the process ID.  Pid should be a pid_t,
	 * but we use unsigned long for portability.
	 */
	for (pid = getpid(); *--trv == 'X'; pid /= 10)
		switch (pid % 10) {
		case 0: *trv = '0'; break;
		case 1: *trv = '1'; break;
		case 2: *trv = '2'; break;
		case 3: *trv = '3'; break;
		case 4: *trv = '4'; break;
		case 5: *trv = '5'; break;
		case 6: *trv = '6'; break;
		case 7: *trv = '7'; break;
		case 8: *trv = '8'; break;
		case 9: *trv = '9'; break;
		}
	++trv;

	/* Set up open flags and mode. */
	mode = CDB___db_omode("rw----");

	/* Loop, trying to open a file. */
	for (;;) {
		if ((ret = CDB___os_open(path,
		    tmp_oflags | DB_OSO_CREATE | DB_OSO_EXCL, mode, fhp)) == 0)
			return (0);

		/*
		 * XXX:
		 * If we don't get an EEXIST error, then there's something
		 * seriously wrong.  Unfortunately, if the implementation
		 * doesn't return EEXIST for O_CREAT and O_EXCL regardless
		 * of other possible errors, we've lost.
		 */
		if (ret != EEXIST) {
			CDB___db_err(dbenv,
			    "tmp_open: %s: %s", path, CDB_db_strerror(ret));
			return (ret);
		}

		/*
		 * Tricky little algorithm for backward compatibility.
		 * Assumes sequential ordering of lower-case characters.
		 */
		for (;;) {
			if (*trv == '\0')
				return (EINVAL);
			if (*trv == 'z')
				*trv++ = 'a';
			else {
				if (isdigit((int)*trv))
					*trv = 'a';
				else
					++*trv;
				break;
			}
		}
	}
	/* NOTREACHED */
}
