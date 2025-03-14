/*
  +----------------------------------------------------------------------+
  | Copyright (c) The PHP Group                                          |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | https://www.php.net/license/3_01.txt                                 |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Wez Furlong <wez@php.net>                                    |
  +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "ext/pdo/php_pdo.h"
#include "ext/pdo/php_pdo_driver.h"
/* this file actually lives in main/ */
#include "php_odbc_utils.h"
#include "php_pdo_odbc.h"
#include "php_pdo_odbc_int.h"
#include "zend_exceptions.h"

static void pdo_odbc_fetch_error_func(pdo_dbh_t *dbh, pdo_stmt_t *stmt, zval *info)
{
	pdo_odbc_db_handle *H = (pdo_odbc_db_handle *)dbh->driver_data;
	pdo_odbc_errinfo *einfo = &H->einfo;
	pdo_odbc_stmt *S = NULL;
	zend_string *message = NULL;

	if (stmt) {
		S = (pdo_odbc_stmt*)stmt->driver_data;
		einfo = &S->einfo;
	}

	/* If we don't have a driver error do not populate the info array */
	if (strlen(einfo->last_err_msg) == 0) {
		return;
	}

	message = strpprintf(0, "%s (%s[%ld] at %s:%d)",
				einfo->last_err_msg,
				einfo->what, (long) einfo->last_error,
				einfo->file, einfo->line);

	add_next_index_long(info, einfo->last_error);
	add_next_index_str(info, message);
	add_next_index_string(info, einfo->last_state);
}


void pdo_odbc_error(pdo_dbh_t *dbh, pdo_stmt_t *stmt, PDO_ODBC_HSTMT statement, char *what, const char *file, int line) /* {{{ */
{
	SQLRETURN rc;
	SQLSMALLINT	errmsgsize = 0;
	SQLHANDLE eh;
	SQLSMALLINT htype, recno = 1;
	pdo_odbc_db_handle *H = (pdo_odbc_db_handle*)dbh->driver_data;
	pdo_odbc_errinfo *einfo = &H->einfo;
	pdo_odbc_stmt *S = NULL;
	pdo_error_type *pdo_err = &dbh->error_code;

	if (stmt) {
		S = (pdo_odbc_stmt*)stmt->driver_data;

		einfo = &S->einfo;
		pdo_err = &stmt->error_code;
	}

	if (statement == SQL_NULL_HSTMT && S) {
		statement = S->stmt;
	}

	if (statement) {
		htype = SQL_HANDLE_STMT;
		eh = statement;
	} else if (H->dbc) {
		htype = SQL_HANDLE_DBC;
		eh = H->dbc;
	} else {
		htype = SQL_HANDLE_ENV;
		eh = H->env;
	}

	rc = SQLGetDiagRec(htype, eh, recno++, (SQLCHAR *) einfo->last_state, &einfo->last_error,
			(SQLCHAR *) einfo->last_err_msg, sizeof(einfo->last_err_msg)-1, &errmsgsize);

	if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
		errmsgsize = 0;
	}

	einfo->last_err_msg[errmsgsize] = '\0';
	einfo->file = file;
	einfo->line = line;
	einfo->what = what;

	strcpy(*pdo_err, einfo->last_state);
/* printf("@@ SQLSTATE[%s] %s\n", *pdo_err, einfo->last_err_msg); */
	if (!dbh->methods) {
		zend_throw_exception_ex(php_pdo_get_exception(), einfo->last_error, "SQLSTATE[%s] %s: %d %s",
				*pdo_err, what, einfo->last_error, einfo->last_err_msg);
	}

	/* just like a cursor, once you start pulling, you need to keep
	 * going until the end; SQL Server (at least) will mess with the
	 * actual cursor state if you don't finish retrieving all the
	 * diagnostic records (which can be generated by PRINT statements
	 * in the query, for instance). */
	while (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
		SQLCHAR discard_state[6];
		SQLCHAR discard_buf[1024];
		SQLINTEGER code;
		rc = SQLGetDiagRec(htype, eh, recno++, discard_state, &code,
				discard_buf, sizeof(discard_buf)-1, &errmsgsize);
	}

}
/* }}} */

static void odbc_handle_closer(pdo_dbh_t *dbh)
{
	pdo_odbc_db_handle *H = (pdo_odbc_db_handle*)dbh->driver_data;

	if (H->dbc != SQL_NULL_HANDLE) {
		SQLEndTran(SQL_HANDLE_DBC, H->dbc, SQL_ROLLBACK);
		SQLDisconnect(H->dbc);
		SQLFreeHandle(SQL_HANDLE_DBC, H->dbc);
		H->dbc = NULL;
	}
	SQLFreeHandle(SQL_HANDLE_ENV, H->env);
	H->env = NULL;
	pefree(H, dbh->is_persistent);
	dbh->driver_data = NULL;
}

static bool odbc_handle_preparer(pdo_dbh_t *dbh, zend_string *sql, pdo_stmt_t *stmt, zval *driver_options)
{
	RETCODE rc;
	pdo_odbc_db_handle *H = (pdo_odbc_db_handle *)dbh->driver_data;
	pdo_odbc_stmt *S = ecalloc(1, sizeof(*S));
	enum pdo_cursor_type cursor_type = PDO_CURSOR_FWDONLY;
	int ret;
	zend_string *nsql = NULL;

	S->H = H;
	S->assume_utf8 = H->assume_utf8;

	/* before we prepare, we need to peek at the query; if it uses named parameters,
	 * we want PDO to rewrite them for us */
	stmt->supports_placeholders = PDO_PLACEHOLDER_POSITIONAL;
	ret = pdo_parse_params(stmt, sql, &nsql);

	if (ret == 1) {
		/* query was re-written */
		sql = nsql;
	} else if (ret == -1) {
		/* couldn't grok it */
		strcpy(dbh->error_code, stmt->error_code);
		efree(S);
		return false;
	}

	rc = SQLAllocHandle(SQL_HANDLE_STMT, H->dbc, &S->stmt);

	if (rc == SQL_INVALID_HANDLE || rc == SQL_ERROR) {
		efree(S);
		if (nsql) {
			zend_string_release(nsql);
		}
		pdo_odbc_drv_error("SQLAllocStmt");
		return false;
	}

	stmt->driver_data = S;

	cursor_type = pdo_attr_lval(driver_options, PDO_ATTR_CURSOR, PDO_CURSOR_FWDONLY);
	if (cursor_type != PDO_CURSOR_FWDONLY) {
		rc = SQLSetStmtAttr(S->stmt, SQL_ATTR_CURSOR_SCROLLABLE, (void*)SQL_SCROLLABLE, 0);
		if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
			pdo_odbc_stmt_error("SQLSetStmtAttr: SQL_ATTR_CURSOR_SCROLLABLE");
			SQLFreeHandle(SQL_HANDLE_STMT, S->stmt);
			if (nsql) {
				zend_string_release(nsql);
			}
			return false;
		}
	}

	rc = SQLPrepare(S->stmt, (SQLCHAR *) ZSTR_VAL(sql), SQL_NTS);
	if (nsql) {
		zend_string_release(nsql);
	}

	stmt->methods = &odbc_stmt_methods;

	if (rc != SQL_SUCCESS) {
		pdo_odbc_stmt_error("SQLPrepare");
		if (rc != SQL_SUCCESS_WITH_INFO) {
			/* clone error information into the db handle */
			strcpy(H->einfo.last_err_msg, S->einfo.last_err_msg);
			H->einfo.file = S->einfo.file;
			H->einfo.line = S->einfo.line;
			H->einfo.what = S->einfo.what;
			strcpy(dbh->error_code, stmt->error_code);
		}
	}

	if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
		return false;
	}
	return true;
}

static zend_long odbc_handle_doer(pdo_dbh_t *dbh, const zend_string *sql)
{
	pdo_odbc_db_handle *H = (pdo_odbc_db_handle *)dbh->driver_data;
	RETCODE rc;
	SQLLEN row_count = -1;
	PDO_ODBC_HSTMT	stmt;

	rc = SQLAllocHandle(SQL_HANDLE_STMT, H->dbc, &stmt);
	if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
		pdo_odbc_drv_error("SQLAllocHandle: STMT");
		return -1;
	}

	rc = SQLExecDirect(stmt, (SQLCHAR *) ZSTR_VAL(sql), ZSTR_LEN(sql));

	if (rc == SQL_NO_DATA) {
		/* If SQLExecDirect executes a searched update or delete statement that
		 * does not affect any rows at the data source, the call to
		 * SQLExecDirect returns SQL_NO_DATA. */
		row_count = 0;
		goto out;
	}

	if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
		pdo_odbc_doer_error("SQLExecDirect");
		goto out;
	}

	rc = SQLRowCount(stmt, &row_count);
	if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
		pdo_odbc_doer_error("SQLRowCount");
		goto out;
	}
	if (row_count == -1) {
		row_count = 0;
	}
out:
	SQLFreeHandle(SQL_HANDLE_STMT, stmt);
	return row_count;
}

/* TODO: Do ODBC quoter
static int odbc_handle_quoter(pdo_dbh_t *dbh, const char *unquoted, size_t unquotedlen, char **quoted, size_t *quotedlen, enum pdo_param_type param_type )
{
	// pdo_odbc_db_handle *H = (pdo_odbc_db_handle *)dbh->driver_data;
	// TODO: figure it out
	return 0;
}
*/

static bool odbc_handle_begin(pdo_dbh_t *dbh)
{
	if (dbh->auto_commit) {
		/* we need to disable auto-commit now, to be able to initiate a transaction */
		RETCODE rc;
		pdo_odbc_db_handle *H = (pdo_odbc_db_handle *)dbh->driver_data;

		rc = SQLSetConnectAttr(H->dbc, SQL_ATTR_AUTOCOMMIT, (SQLPOINTER)SQL_AUTOCOMMIT_OFF, SQL_IS_INTEGER);
		if (rc != SQL_SUCCESS) {
			pdo_odbc_drv_error("SQLSetConnectAttr AUTOCOMMIT = OFF");
			return false;
		}
	}
	return true;
}

static bool odbc_handle_commit(pdo_dbh_t *dbh)
{
	pdo_odbc_db_handle *H = (pdo_odbc_db_handle *)dbh->driver_data;
	RETCODE rc;

	rc = SQLEndTran(SQL_HANDLE_DBC, H->dbc, SQL_COMMIT);

	if (rc != SQL_SUCCESS) {
		pdo_odbc_drv_error("SQLEndTran: Commit");

		if (rc != SQL_SUCCESS_WITH_INFO) {
			return false;
		}
	}

	if (dbh->auto_commit) {
		/* turn auto-commit back on again */
		rc = SQLSetConnectAttr(H->dbc, SQL_ATTR_AUTOCOMMIT, (SQLPOINTER)SQL_AUTOCOMMIT_ON, SQL_IS_INTEGER);
		if (rc != SQL_SUCCESS) {
			pdo_odbc_drv_error("SQLSetConnectAttr AUTOCOMMIT = ON");
			return false;
		}
	}
	return true;
}

static bool odbc_handle_rollback(pdo_dbh_t *dbh)
{
	pdo_odbc_db_handle *H = (pdo_odbc_db_handle *)dbh->driver_data;
	RETCODE rc;

	rc = SQLEndTran(SQL_HANDLE_DBC, H->dbc, SQL_ROLLBACK);

	if (rc != SQL_SUCCESS) {
		pdo_odbc_drv_error("SQLEndTran: Rollback");

		if (rc != SQL_SUCCESS_WITH_INFO) {
			return false;
		}
	}
	if (dbh->auto_commit && H->dbc) {
		/* turn auto-commit back on again */
		rc = SQLSetConnectAttr(H->dbc, SQL_ATTR_AUTOCOMMIT, (SQLPOINTER)SQL_AUTOCOMMIT_ON, SQL_IS_INTEGER);
		if (rc != SQL_SUCCESS) {
			pdo_odbc_drv_error("SQLSetConnectAttr AUTOCOMMIT = ON");
			return false;
		}
	}

	return true;
}

static bool odbc_handle_set_attr(pdo_dbh_t *dbh, zend_long attr, zval *val)
{
	pdo_odbc_db_handle *H = (pdo_odbc_db_handle *)dbh->driver_data;
	bool bval;

	switch (attr) {
		case PDO_ODBC_ATTR_ASSUME_UTF8:
			if (!pdo_get_bool_param(&bval, val)) {
				return false;
			}
			H->assume_utf8 = bval;
			return true;
		case PDO_ATTR_AUTOCOMMIT:
			if (!pdo_get_bool_param(&bval, val)) {
				return false;
			}
			if (dbh->in_txn) {
				pdo_raise_impl_error(dbh, NULL, "HY000", "Cannot change autocommit mode while a transaction is already open");
				return false;
			}
			if (dbh->auto_commit ^ bval) {
				dbh->auto_commit = bval;
				RETCODE rc = SQLSetConnectAttr(
					H->dbc,
					SQL_ATTR_AUTOCOMMIT,
					dbh->auto_commit ? (SQLPOINTER) SQL_AUTOCOMMIT_ON : (SQLPOINTER) SQL_AUTOCOMMIT_OFF,
					SQL_IS_INTEGER
				);
				if (rc != SQL_SUCCESS) {
					pdo_odbc_drv_error(
						dbh->auto_commit ? "SQLSetConnectAttr AUTOCOMMIT = ON" : "SQLSetConnectAttr AUTOCOMMIT = OFF"
					);
					return false;
				}
			}
			return true;
		default:
			strcpy(H->einfo.last_err_msg, "Unknown Attribute");
			H->einfo.what = "setAttribute";
			strcpy(H->einfo.last_state, "IM001");
			return false;
	}
}

static int pdo_odbc_get_info_string(pdo_dbh_t *dbh, SQLUSMALLINT type, zval *val)
{
	RETCODE rc;
	SQLSMALLINT out_len;
	char buf[256];
	pdo_odbc_db_handle *H = (pdo_odbc_db_handle *)dbh->driver_data;
	rc = SQLGetInfo(H->dbc, type, (SQLPOINTER)buf, sizeof(buf), &out_len);
	/* returning -1 is treated as an error, not as unsupported */
	if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
		return -1;
	}
	ZVAL_STRINGL(val, buf, out_len);
	return 1;
}

static int odbc_handle_get_attr(pdo_dbh_t *dbh, zend_long attr, zval *val)
{
	pdo_odbc_db_handle *H = (pdo_odbc_db_handle *)dbh->driver_data;
	switch (attr) {
		case PDO_ATTR_CLIENT_VERSION:
			ZVAL_STRING(val, "ODBC-" PDO_ODBC_TYPE);
			return 1;

		case PDO_ATTR_SERVER_VERSION:
			return pdo_odbc_get_info_string(dbh, SQL_DBMS_VER, val);
		case PDO_ATTR_SERVER_INFO:
			return pdo_odbc_get_info_string(dbh, SQL_DBMS_NAME, val);
		case PDO_ATTR_PREFETCH:
		case PDO_ATTR_TIMEOUT:
		case PDO_ATTR_CONNECTION_STATUS:
			break;
		case PDO_ODBC_ATTR_ASSUME_UTF8:
			ZVAL_BOOL(val, H->assume_utf8);
			return 1;
		case PDO_ATTR_AUTOCOMMIT:
			ZVAL_BOOL(val, dbh->auto_commit);
			return 1;
	}
	return 0;
}

static zend_result odbc_handle_check_liveness(pdo_dbh_t *dbh)
{
	RETCODE ret;
	UCHAR d_name[32];
	SQLSMALLINT len;
	SQLUINTEGER dead = SQL_CD_FALSE;
	pdo_odbc_db_handle *H = (pdo_odbc_db_handle *)dbh->driver_data;

	ret = SQLGetConnectAttr(H->dbc, SQL_ATTR_CONNECTION_DEAD, &dead, 0, NULL);
	if (ret == SQL_SUCCESS && dead == SQL_CD_TRUE) {
		/* Bail early here, since we know it's gone */
		return FAILURE;
	}
	/*
	 * If the driver doesn't support SQL_ATTR_CONNECTION_DEAD, or if
	 * it returns false (which could be a false positive), fall back
	 * to using SQL_DATA_SOURCE_READ_ONLY, which isn't semantically
	 * correct, but works with many drivers.
	 */
	ret = SQLGetInfo(H->dbc, SQL_DATA_SOURCE_READ_ONLY, d_name,
		sizeof(d_name), &len);

	if (ret != SQL_SUCCESS || len == 0) {
		return FAILURE;
	}
	return SUCCESS;
}

static const struct pdo_dbh_methods odbc_methods = {
	odbc_handle_closer,
	odbc_handle_preparer,
	odbc_handle_doer,
	NULL, /* quoter */
	odbc_handle_begin,
	odbc_handle_commit,
	odbc_handle_rollback,
	odbc_handle_set_attr,
	NULL,	/* last id */
	pdo_odbc_fetch_error_func,
	odbc_handle_get_attr,	/* get attr */
	odbc_handle_check_liveness, /* check_liveness */
	NULL, /* get_driver_methods */
	NULL, /* request_shutdown */
	NULL, /* in transaction, use PDO's internal tracking mechanism */
	NULL, /* get_gc */
	NULL /* scanner */
};

static int pdo_odbc_handle_factory(pdo_dbh_t *dbh, zval *driver_options) /* {{{ */
{
	pdo_odbc_db_handle *H;
	RETCODE rc;
	int use_direct = 0;
	zend_ulong cursor_lib;

	H = pecalloc(1, sizeof(*H), dbh->is_persistent);

	dbh->driver_data = H;

	rc = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &H->env);
	if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
		pdo_odbc_drv_error("SQLAllocHandle: ENV");
		goto fail;
	}

	rc = SQLSetEnvAttr(H->env, SQL_ATTR_ODBC_VERSION, (void*)SQL_OV_ODBC3, 0);

	if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
		pdo_odbc_drv_error("SQLSetEnvAttr: ODBC3");
		goto fail;
	}

#ifdef SQL_ATTR_CONNECTION_POOLING
	if (pdo_odbc_pool_on != SQL_CP_OFF) {
		rc = SQLSetEnvAttr(H->env, SQL_ATTR_CP_MATCH, (void*)pdo_odbc_pool_mode, 0);
		if (rc != SQL_SUCCESS) {
			pdo_odbc_drv_error("SQLSetEnvAttr: SQL_ATTR_CP_MATCH");
			goto fail;
		}
	}
#endif

	rc = SQLAllocHandle(SQL_HANDLE_DBC, H->env, &H->dbc);
	if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
		pdo_odbc_drv_error("SQLAllocHandle: DBC");
		goto fail;
	}

	rc = SQLSetConnectAttr(H->dbc, SQL_ATTR_AUTOCOMMIT,
		(SQLPOINTER)(intptr_t)(dbh->auto_commit ? SQL_AUTOCOMMIT_ON : SQL_AUTOCOMMIT_OFF), SQL_IS_INTEGER);
	if (rc != SQL_SUCCESS) {
		pdo_odbc_drv_error("SQLSetConnectAttr AUTOCOMMIT");
		goto fail;
	}

	/* set up the cursor library, if needed, or if configured explicitly */
	cursor_lib = pdo_attr_lval(driver_options, PDO_ODBC_ATTR_USE_CURSOR_LIBRARY, SQL_CUR_USE_IF_NEEDED);
	rc = SQLSetConnectAttr(H->dbc, SQL_ODBC_CURSORS, (void*)cursor_lib, SQL_IS_INTEGER);
	if (rc != SQL_SUCCESS && cursor_lib != SQL_CUR_USE_IF_NEEDED) {
		pdo_odbc_drv_error("SQLSetConnectAttr SQL_ODBC_CURSORS");
		goto fail;
	}

	/* a connection string may have = but not ; - i.e. "DSN=PHP" */
	if (strchr(dbh->data_source, '=')) {
		SQLCHAR dsnbuf[1024];
		SQLSMALLINT dsnbuflen;

		use_direct = 1;

		bool use_uid_arg = dbh->username != NULL && !php_memnistr(dbh->data_source, "uid=", strlen("uid="), dbh->data_source + dbh->data_source_len);
		bool use_pwd_arg = dbh->password != NULL && !php_memnistr(dbh->data_source, "pwd=", strlen("pwd="), dbh->data_source + dbh->data_source_len);

		if (use_uid_arg || use_pwd_arg) {
			char *db = (char*) estrndup(dbh->data_source, dbh->data_source_len);
			char *db_end = db + dbh->data_source_len;
			db_end--;
			if ((unsigned char)*(db_end) == ';') {
				*db_end = '\0';
			}

			char *uid = NULL, *pwd = NULL, *dsn = NULL;
			bool should_quote_uid, should_quote_pwd;
			size_t new_dsn_size;

			if (use_uid_arg) {
				should_quote_uid = !php_odbc_connstr_is_quoted(dbh->username) && php_odbc_connstr_should_quote(dbh->username);
				if (should_quote_uid) {
					size_t estimated_length = php_odbc_connstr_estimate_quote_length(dbh->username);
					uid = emalloc(estimated_length);
					php_odbc_connstr_quote(uid, dbh->username, estimated_length);
				} else {
					uid = dbh->username;
				}

				if (!use_pwd_arg) {
					new_dsn_size = strlen(db) + strlen(uid) + strlen(";UID=;") + 1;
					dsn = pemalloc(new_dsn_size, dbh->is_persistent);
					snprintf(dsn, new_dsn_size, "%s;UID=%s;", db, uid);
				}
			}

			if (use_pwd_arg) {
				should_quote_pwd = !php_odbc_connstr_is_quoted(dbh->password) && php_odbc_connstr_should_quote(dbh->password);
				if (should_quote_pwd) {
					size_t estimated_length = php_odbc_connstr_estimate_quote_length(dbh->password);
					pwd = emalloc(estimated_length);
					php_odbc_connstr_quote(pwd, dbh->password, estimated_length);
				} else {
					pwd = dbh->password;
				}

				if (!use_uid_arg) {
					new_dsn_size = strlen(db) + strlen(pwd) + strlen(";PWD=;") + 1;
					dsn = pemalloc(new_dsn_size, dbh->is_persistent);
					snprintf(dsn, new_dsn_size, "%s;PWD=%s;", db, pwd);
				}
			}

			if (use_uid_arg && use_pwd_arg) {
				new_dsn_size = strlen(db)
					+ strlen(uid) + strlen(pwd)
					+ strlen(";UID=;PWD=;") + 1;
				dsn = pemalloc(new_dsn_size, dbh->is_persistent);
				snprintf(dsn, new_dsn_size, "%s;UID=%s;PWD=%s;", db, uid, pwd);
			}

			pefree((char*)dbh->data_source, dbh->is_persistent);
			dbh->data_source = dsn;
			dbh->data_source_len = strlen(dsn);
			if (uid && should_quote_uid) {
				efree(uid);
			}
			if (pwd && should_quote_pwd) {
				efree(pwd);
			}
			efree(db);
		}

		rc = SQLDriverConnect(H->dbc, NULL, (SQLCHAR *) dbh->data_source, dbh->data_source_len,
				dsnbuf, sizeof(dsnbuf)-1, &dsnbuflen, SQL_DRIVER_NOPROMPT);
	}
	if (!use_direct) {
		rc = SQLConnect(H->dbc, (SQLCHAR *) dbh->data_source, SQL_NTS, (SQLCHAR *) dbh->username, SQL_NTS, (SQLCHAR *) dbh->password, SQL_NTS);
	}

	if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
		pdo_odbc_drv_error(use_direct ? "SQLDriverConnect" : "SQLConnect");
		goto fail;
	}

	/* TODO: if we want to play nicely, we should check to see if the driver really supports ODBC v3 or not */

	dbh->methods = &odbc_methods;
	dbh->alloc_own_columns = 1;

	return 1;

fail:
	dbh->methods = &odbc_methods;
	return 0;
}
/* }}} */

const pdo_driver_t pdo_odbc_driver = {
	PDO_DRIVER_HEADER(odbc),
	pdo_odbc_handle_factory
};
