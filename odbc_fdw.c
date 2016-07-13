/*----------------------------------------------------------
 *
 *        foreign-data wrapper for ODBC
 *
 * Copyright (c) 2011, PostgreSQL Global Development Group
 *
 * This software is released under the PostgreSQL Licence.
 *
 * Author: Zheng Yang <zhengyang4k@gmail.com>
 * Updated to 9.2+ by Gunnar "Nick" Bluth <nick@pro-open.de>
 *   based on tds_fdw code from Geoff Montee
 *
 * IDENTIFICATION
 *      odbc_fdw/odbc_fdw.c
 *
 *----------------------------------------------------------
 */

/* Debug mode flag */
/*
#define DEBUG
*/
#include "postgres.h"
#include <string.h>

#include "funcapi.h"
#include "access/reloptions.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_user_mapping.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "utils/memutils.h"
#include "utils/builtins.h"
#include "utils/relcache.h"
#include "storage/lock.h"
#include "miscadmin.h"
#include "mb/pg_wchar.h"
#include "optimizer/cost.h"
#include "storage/fd.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/rel.h"

#include "optimizer/pathnode.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/planmain.h"

#include <stdio.h>
#include <sql.h>
#include <sqlext.h>

PG_MODULE_MAGIC;

#define PROCID_TEXTEQ 67
#define PROCID_TEXTCONST 25

/* Provisional limit to name lengths in characters */
#define MAXIMUM_CATALOG_NAME_LEN 255
#define MAXIMUM_SCHEMA_NAME_LEN 255
#define MAXIMUM_TABLE_NAME_LEN 255
#define MAXIMUM_COLUMN_NAME_LEN 255

typedef struct odbcFdwOptions
{
	/* ODBC common attributes */
	char  *dsn;       /* Data Source Name */
	char  *driver;    /* ODBC driver name */
	char  *host;      /* server address (SERVER) */
	char  *port;      /* server port  */
	char  *database;  /* Database name */
	char  *username;  /* Username (UID) */
	char  *password;  /* Password (PWD) */

	/* table specification */
	char  *schema;     /* Foreign schema name */
	char  *table;      /* Foreign table */
	char  *prefix;     /* Prefix for imported foreign table names */
	char  *sql_query;  /* SQL query (overrides table) */
	char  *sql_count;  /* SQL query for counting results */
	char  *encoding;   /* Character encoding name */

	List  *mapping_list; /* Column name mapping */
} odbcFdwOptions;

typedef struct odbcFdwExecutionState
{
	AttInMetadata   *attinmeta;
	odbcFdwOptions  options;
	SQLHSTMT        stmt;
	int             num_of_result_cols;
	int             num_of_table_cols;
	StringInfoData  *table_columns;
	bool            first_iteration;
	List            *col_position_mask;
	List            *col_size_array;
	char            *sql_count;
	int             encoding;
} odbcFdwExecutionState;

struct odbcFdwOption
{
	const char   *optname;
	Oid     optcontext; /* Oid of catalog in which option may appear */
};

/*
 * Array of valid options
 *
 */
static struct odbcFdwOption valid_options[] =
{
	/* Foreign server options */
	{ "dsn",        ForeignServerRelationId },
	{ "driver",     ForeignServerRelationId },
	{ "host",       ForeignServerRelationId },
	{ "port",       ForeignServerRelationId },
	{ "database",   ForeignServerRelationId },
	{ "encoding",   ForeignServerRelationId },
	{ "username",   ForeignServerRelationId },
	{ "password",   ForeignServerRelationId },

	/* Foreign table options */
	{ "schema",     ForeignTableRelationId },
	{ "table",      ForeignTableRelationId },
	{ "prefix",     ForeignTableRelationId },
	{ "sql_query",  ForeignTableRelationId },
	{ "sql_count",  ForeignTableRelationId },

	/* User mapping options */
	{ "username",   UserMappingRelationId },
	{ "password",   UserMappingRelationId },

	/* Sentinel */
	{ NULL,       InvalidOid}
};

/*
 * SQL functions
 */
extern Datum odbc_fdw_handler(PG_FUNCTION_ARGS);
extern Datum odbc_fdw_validator(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(odbc_fdw_handler);
PG_FUNCTION_INFO_V1(odbc_fdw_validator);

/*
 * FDW callback routines
 */
static void odbcExplainForeignScan(ForeignScanState *node, ExplainState *es);
static void odbcBeginForeignScan(ForeignScanState *node, int eflags);
static TupleTableSlot *odbcIterateForeignScan(ForeignScanState *node);
static void odbcReScanForeignScan(ForeignScanState *node);
static void odbcEndForeignScan(ForeignScanState *node);
static void odbcGetForeignRelSize(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid);
static void odbcEstimateCosts(PlannerInfo *root, RelOptInfo *baserel, Cost *startup_cost, Cost *total_cost, Oid foreigntableid);
static void odbcGetForeignPaths(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid);
static bool odbcAnalyzeForeignTable(Relation relation, AcquireSampleRowsFunc *func, BlockNumber *totalpages);
static ForeignScan* odbcGetForeignPlan(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid, ForeignPath *best_path, List *tlist, List *scan_clauses, Plan *outer_plan);
List* odbcImportForeignSchema(ImportForeignSchemaStmt *stmt, Oid serverOid);

/*
 * helper functions
 */
static bool odbcIsValidOption(const char *option, Oid context);
static void check_return(SQLRETURN ret, char *msg, SQLHANDLE handle, SQLSMALLINT type);
static char* empty_string_if_null(char *string);
static void extract_odbcFdwOptions(List *options_list, odbcFdwOptions *extracted_options);
static void init_odbcFdwOptions(odbcFdwOptions* options);
static void copy_odbcFdwOptions(odbcFdwOptions* to, odbcFdwOptions* from);
static void odbc_connection(odbcFdwOptions* options, SQLHENV *env, SQLHDBC *dbc);
static void sql_data_type(SQLSMALLINT odbc_data_type, SQLULEN column_size, SQLSMALLINT decimal_digits, SQLSMALLINT nullable, StringInfo sql_type);
static void odbcGetOptions(Oid server_oid, List *add_options, odbcFdwOptions *extracted_options);
static void odbcGetTableOptions(Oid foreigntableid, odbcFdwOptions *extracted_options);
static void check_return(SQLRETURN ret, char *msg, SQLHANDLE handle, SQLSMALLINT type);
static void odbcConnStr(StringInfoData *conn_str, odbcFdwOptions* options);

/*
 * Check if string pointer is NULL or points to empty string
 */
inline bool is_blank_string(const char *s)
{
	return s == NULL || s[0] == '\0';
}

Datum
odbc_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *fdwroutine = makeNode(FdwRoutine);
	/* FIXME */
	fdwroutine->GetForeignRelSize = odbcGetForeignRelSize;
	fdwroutine->GetForeignPaths = odbcGetForeignPaths;
	fdwroutine->AnalyzeForeignTable = odbcAnalyzeForeignTable;
	fdwroutine->GetForeignPlan = odbcGetForeignPlan;
	fdwroutine->ExplainForeignScan = odbcExplainForeignScan;
	fdwroutine->BeginForeignScan = odbcBeginForeignScan;
	fdwroutine->IterateForeignScan = odbcIterateForeignScan;
	fdwroutine->ReScanForeignScan = odbcReScanForeignScan;
	fdwroutine->EndForeignScan = odbcEndForeignScan;
	fdwroutine->ImportForeignSchema = odbcImportForeignSchema;
	PG_RETURN_POINTER(fdwroutine);
}

static void
init_odbcFdwOptions(odbcFdwOptions* options)
{
	memset(options, 0, sizeof(odbcFdwOptions));
}

static void
copy_odbcFdwOptions(odbcFdwOptions* to, odbcFdwOptions* from)
{
	if (to && from)
	{
		*to = *from;
	}
}

/*
 * Avoid NULL string: return original string, or empty string if NULL
 */
static char*
empty_string_if_null(char *string)
{
	static char* empty_string = "";
	return string == NULL ? empty_string : string;
}

static void
extract_odbcFdwOptions(List *options_list, odbcFdwOptions *extracted_options)
{
	ListCell        *lc;
	List            *mapping_list;

	#ifdef DEBUG
		elog(DEBUG1, "extract_init_odbcFdwOptions");
	#endif

	init_odbcFdwOptions(extracted_options);

	/* Loop through the options, and get the foreign table options */
	foreach(lc, options_list)
	{
		DefElem *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "dsn") == 0)
		{
			extracted_options->dsn = defGetString(def);
			continue;
		}

		if (strcmp(def->defname, "driver") == 0)
		{
			extracted_options->driver = defGetString(def);
			continue;
		}

		if (strcmp(def->defname, "host") == 0)
		{
			extracted_options->host = defGetString(def);
			continue;
		}

		if (strcmp(def->defname, "port") == 0)
		{
			extracted_options->port = defGetString(def);
			continue;
		}

		if (strcmp(def->defname, "database") == 0)
		{
			extracted_options->database = defGetString(def);
			continue;
		}

		if (strcmp(def->defname, "schema") == 0)
		{
			extracted_options->schema = defGetString(def);
			continue;
		}

		if (strcmp(def->defname, "table") == 0)
		{
			extracted_options->table = defGetString(def);
			continue;
		}

		if (strcmp(def->defname, "prefix") == 0)
		{
			extracted_options->prefix = defGetString(def);
			continue;
		}

		if (strcmp(def->defname, "sql_query") == 0)
		{
			extracted_options->sql_query = defGetString(def);
			continue;
		}

		if (strcmp(def->defname, "sql_count") == 0)
		{
			extracted_options->sql_count = defGetString(def);
			continue;
		}

		if (strcmp(def->defname, "username") == 0)
		{
			extracted_options->username = defGetString(def);
			continue;
		}

		if (strcmp(def->defname, "password") == 0)
		{
			extracted_options->password = defGetString(def);
			continue;
		}

		if (strcmp(def->defname, "encoding") == 0)
		{
			extracted_options->encoding = defGetString(def);
			continue;
		}

		/* Column mapping goes here */
		/* TODO: is this useful? if so, how can columns names coincident
		   with option names be escaped? */
		extracted_options->mapping_list = lappend(extracted_options->mapping_list, def);
	}
}

/*
 * Get the schema name from the options
 */
char* get_schema_name(odbcFdwOptions *options)
{
	char* schema_name = options->schema;
	if (is_blank_string(schema_name))
	{
		/* TODO: this is just a MySQL convenience; should remove it? */
		schema_name = options->database;
	}
	return schema_name;
}

/*
 * Establish ODBC connection
 */
static void
odbc_connection(odbcFdwOptions* options, SQLHENV *env, SQLHDBC *dbc)
{
	StringInfoData  conn_str;
	SQLCHAR OutConnStr[1024];
	SQLSMALLINT OutConnStrLen;
	SQLRETURN ret;

	odbcConnStr(&conn_str, options);

	/* Allocate an environment handle */
	SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, env);
	/* We want ODBC 3 support */
	SQLSetEnvAttr(*env, SQL_ATTR_ODBC_VERSION, (void *) SQL_OV_ODBC3, 0);

	/* Allocate a connection handle */
	SQLAllocHandle(SQL_HANDLE_DBC, *env, dbc);
	/* Connect to the DSN */
	ret = SQLDriverConnect(*dbc, NULL, (SQLCHAR *) conn_str.data, SQL_NTS,
	                       OutConnStr, 1024, &OutConnStrLen, SQL_DRIVER_COMPLETE);
	check_return(ret, "Connecting to driver", dbc, SQL_HANDLE_DBC);
}

/*
 * Validate function
 */
Datum
odbc_fdw_validator(PG_FUNCTION_ARGS)
{
	List  *options_list = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid   catalog = PG_GETARG_OID(1);
	char  *dsn          = NULL;
	char  *driver       = NULL;
	char  *svr_host     = NULL;
	char  *svr_port     = NULL;
	char  *svr_database = NULL;
	char  *svr_schema   = NULL;
	char  *svr_table    = NULL;
	char  *svr_prefix   = NULL;
	char  *sql_query    = NULL;
	char  *sql_count    = NULL;
	char  *username     = NULL;
	char  *password     = NULL;
	ListCell *cell;

	#ifdef DEBUG
		elog(DEBUG1, "odbc_fdw_validator");
	#endif

	/*
	 * Check that the necessary options: address, port, database
	 */
	foreach(cell, options_list)
	{
		DefElem    *def = (DefElem *) lfirst(cell);

		/* Complain invalid options */
		if (!odbcIsValidOption(def->defname, catalog))
		{
			struct odbcFdwOption *opt;
			StringInfoData buf;

			/*
			 * Unknown option specified, complain about it. Provide a hint
			 * with list of valid options for the object.
			 */
			initStringInfo(&buf);
			for (opt = valid_options; opt->optname; opt++)
			{
				if (catalog == opt->optcontext)
					appendStringInfo(&buf, "%s%s", (buf.len > 0) ? ", " : "",
					                 opt->optname);
			}

			ereport(ERROR,
			        (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
			         errmsg("invalid option \"%s\"", def->defname),
			         errhint("Valid options in this context are: %s", buf.len ? buf.data : "<none>")
			        ));
		}

		/* Complain about redundent options */
		if (strcmp(def->defname, "dsn") == 0)
		{
			if (!is_blank_string(dsn))
				ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
				                errmsg("conflicting or redundant options: dsn (%s)", defGetString(def))
				               ));

			dsn = defGetString(def);
		}
		else if (strcmp(def->defname, "driver") == 0)
		{
			if (!is_blank_string(driver))
				ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
				                errmsg("conflicting or redundant options: driver (%s)", defGetString(def))
				               ));

			driver = defGetString(def);
		}
		else if (strcmp(def->defname, "host") == 0)
		{
			if (!is_blank_string(svr_host))
				ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
				                errmsg("conflicting or redundant options: host (%s)", defGetString(def))
				               ));

			svr_host = defGetString(def);
		}
		else if (strcmp(def->defname, "port") == 0)
		{
			if (!is_blank_string(svr_port))
				ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
				                errmsg("conflicting or redundant options: port (%s)", defGetString(def))
				               ));

			svr_port = defGetString(def);
		}
		else if (strcmp(def->defname, "database") == 0)
		{
			if (!is_blank_string(svr_database))
				ereport(ERROR,
				        (errcode(ERRCODE_SYNTAX_ERROR),
				         errmsg("conflicting or redundant options: database (%s)", defGetString(def))
				        ));

			svr_database = defGetString(def);
		}
		else if (strcmp(def->defname, "schema") == 0)
		{
			if (!is_blank_string(svr_schema))
				ereport(ERROR,
				        (errcode(ERRCODE_SYNTAX_ERROR),
				         errmsg("conflicting or redundant options: schema (%s)", defGetString(def))
				        ));

			svr_schema = defGetString(def);
		}
		else if (strcmp(def->defname, "table") == 0)
		{
			if (!is_blank_string(svr_table))
				ereport(ERROR,
				        (errcode(ERRCODE_SYNTAX_ERROR),
				         errmsg("conflicting or redundant options: table (%s)", defGetString(def))
				        ));

			svr_table = defGetString(def);
		}
		else if (strcmp(def->defname, "prefix") == 0)
		{
			if (!is_blank_string(svr_prefix))
				ereport(ERROR,
				        (errcode(ERRCODE_SYNTAX_ERROR),
				         errmsg("conflicting or redundant options: prefix (%s)", defGetString(def))
				        ));

			svr_prefix = defGetString(def);
		}
		else if (strcmp(def->defname, "sql_query") == 0)
		{
			if (sql_query)
				ereport(ERROR,
				       (errcode(ERRCODE_SYNTAX_ERROR),
			            errmsg("conflicting or redundant options: sql_query (%s)", defGetString(def))
			           ));

			sql_query = defGetString(def);
		}
		else if (strcmp(def->defname, "sql_count") == 0)
		{
			if (!is_blank_string(sql_count))
				ereport(ERROR,
				        (errcode(ERRCODE_SYNTAX_ERROR),
				         errmsg("conflicting or redundant options: sql_count (%s)", defGetString(def))
				        ));

			sql_count = defGetString(def);
		}
		else if (strcmp(def->defname, "username") == 0)
		{
			if (!is_blank_string(username))
				ereport(ERROR,
				        (errcode(ERRCODE_SYNTAX_ERROR),
				         errmsg("conflicting or redundant options: username (%s)", defGetString(def))
				        ));

			username = defGetString(def);
		}
		else if (strcmp(def->defname, "password") == 0)
		{
			if (!is_blank_string(password))
				ereport(ERROR,
				        (errcode(ERRCODE_SYNTAX_ERROR),
				         errmsg("conflicting or redundant options: password (%s)", defGetString(def))
				        ));

			password = defGetString(def);
		}
	}

	/* Complain about missing essential options: dsn */
	if (is_blank_string(dsn) && is_blank_string(driver) && catalog == ForeignServerRelationId)
		ereport(ERROR,
		        (errcode(ERRCODE_SYNTAX_ERROR),
		         errmsg("missing essential information: dsn (Database Source Name) or driver")
		        ));

	PG_RETURN_VOID();
}

/*
 * Map ODBC data types to PostgreSQL
 */
static void
sql_data_type(
	SQLSMALLINT odbc_data_type,
	SQLULEN     column_size,
	SQLSMALLINT decimal_digits,
	SQLSMALLINT nullable,
	StringInfo sql_type
)
{
	initStringInfo(sql_type);
	switch(odbc_data_type)
	{
		case SQL_CHAR:
		case SQL_WCHAR :
			appendStringInfo(sql_type, "char(%u)", (unsigned)column_size);
			break;
		case SQL_VARCHAR :
		case SQL_WVARCHAR :
			appendStringInfo(sql_type, "varchar(%u)", (unsigned)column_size);
			break;
		case SQL_LONGVARCHAR :
		case SQL_WLONGVARCHAR :
		    appendStringInfo(sql_type, "text");
			break;
		case SQL_DECIMAL :
			appendStringInfo(sql_type, "decimal(%u,%d)", (unsigned)column_size, decimal_digits);
		    break;
		case SQL_NUMERIC :
			appendStringInfo(sql_type, "numeric(%u,%d)", (unsigned)column_size, decimal_digits);
			break;
		case SQL_INTEGER :
		    appendStringInfo(sql_type, "integer");
			break;
		case SQL_REAL :
			appendStringInfo(sql_type, "real");
			break;
		case SQL_FLOAT :
			appendStringInfo(sql_type, "real");
			break;
		case SQL_DOUBLE :
		   appendStringInfo(sql_type, "float8");
			break;
		case SQL_BIT :
		    appendStringInfo(sql_type, "bit(1)");
			break;
		case SQL_SMALLINT :
		case SQL_TINYINT :
			appendStringInfo(sql_type, "smallint");
			break;
		case SQL_BIGINT :
			appendStringInfo(sql_type, "bigint");
			break;
		case SQL_BINARY :
		    appendStringInfo(sql_type, "bit(%u)", (unsigned)column_size);
			break;
		case SQL_VARBINARY :
		    appendStringInfo(sql_type, "varbit(%u)", (unsigned)column_size);
			break;
		case SQL_LONGVARBINARY :
			appendStringInfo(sql_type, "bytea");
			break;
		case SQL_TYPE_DATE :
		case SQL_DATE :
			appendStringInfo(sql_type, "date");
			break;
		case SQL_TYPE_TIME :
		case SQL_TIME :
		    appendStringInfo(sql_type, "time");
			break;
		case SQL_TYPE_TIMESTAMP :
		case SQL_TIMESTAMP :
			appendStringInfo(sql_type, "timestamp");
			break;
		case SQL_GUID :
			appendStringInfo(sql_type, "uuid");
			break;
		default :
			ereport(ERROR,
			        (errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
			         errmsg("Data type not supported, code %d", odbc_data_type)
			        ));
	};
}

/*
 * Fetch the options for a server and options list
 */
static void
odbcGetOptions(Oid server_oid, List *add_options, odbcFdwOptions *extracted_options)
{
	ForeignServer   *server;
	UserMapping     *mapping;
	List            *options;
	ListCell        *lc;

	#ifdef DEBUG
		elog(DEBUG1, "odbcGetOptions");
	#endif

    server  = GetForeignServer(server_oid);
	mapping = GetUserMapping(GetUserId(), server_oid);

	options = NIL;
	options = list_concat(options, add_options);
	options = list_concat(options, server->options);
	options = list_concat(options, mapping->options);

	extract_odbcFdwOptions(options, extracted_options);
}

/*
 * Fetch the options for a odbc_fdw foreign table.
 */
static void
odbcGetTableOptions(Oid foreigntableid, odbcFdwOptions *extracted_options)
{
	ForeignTable    *table;
	ForeignServer   *server;

	#ifdef DEBUG
		elog(DEBUG1, "odbcGetTableOptions");
	#endif

	table = GetForeignTable(foreigntableid);
    odbcGetOptions(table->serverid, table->options, extracted_options);
}

static void
check_return(SQLRETURN ret, char *msg, SQLHANDLE handle, SQLSMALLINT type)
{
	int err_code = ERRCODE_SYSTEM_ERROR;
	#ifdef DEBUG
		SQLINTEGER   i = 0;
		SQLINTEGER   native;
		SQLCHAR  state[ 7 ];
		SQLCHAR  text[256];
		SQLSMALLINT  len;
		SQLRETURN    diag_ret;
		if (SQL_SUCCEEDED(ret))
			elog(DEBUG1, "Successful result: %s", msg);
	#endif

	if (!SQL_SUCCEEDED(ret))
	{
		#ifdef DEBUG
			elog(DEBUG1, "Error result (%d): %s", ret, msg);
			if (handle)
			{
				do
				{
					diag_ret = SQLGetDiagRec(type, handle, ++i, state, &native, text,
					                    sizeof(text), &len );
					if (SQL_SUCCEEDED(diag_ret))
						elog(DEBUG1, " %s:%ld:%ld:%s\n", state, (long int) i, (long int) native, text);
				}
				while( diag_ret == SQL_SUCCESS );
			}
		#endif
		ereport(ERROR, (errcode(err_code), errmsg("%s", msg)));
	}
}

/*
 * Get name qualifier char
 */
static void
getNameQualifierChar(SQLHDBC dbc, StringInfoData *nq_char)
{
	SQLCHAR name_qualifier_char[2];

	#ifdef DEBUG
		elog(DEBUG1, "getNameQualifierChar");
	#endif

	SQLGetInfo(dbc,
	           SQL_CATALOG_NAME_SEPARATOR,
	           (SQLPOINTER)&name_qualifier_char,
	           2,
	           NULL);
	name_qualifier_char[1] = 0; // some drivers fail to copy the trailing zero

	initStringInfo(nq_char);
	appendStringInfo(nq_char, "%s", (char *) name_qualifier_char);
}

/*
 * Get quote cahr
 */
static void
getQuoteChar(SQLHDBC dbc, StringInfoData *q_char)
{
	SQLCHAR quote_char[2];

	#ifdef DEBUG
		elog(DEBUG1, "getQuoteChar");
	#endif

	SQLGetInfo(dbc,
	           SQL_IDENTIFIER_QUOTE_CHAR,
	           (SQLPOINTER)&quote_char,
	            2,
	            NULL);
	quote_char[1] = 0; // some drivers fail to copy the trailing zero

	initStringInfo(q_char);
	appendStringInfo(q_char, "%s", (char *) quote_char);
}

static void odbcConnStr(StringInfoData *conn_str, odbcFdwOptions* options)
{
	bool sep = FALSE;
	static char *sep_str = ";";
	initStringInfo(conn_str);
	if (!is_blank_string(options->dsn))
	{
		appendStringInfo(conn_str, "DSN=%s", options->dsn);
		sep = TRUE;
	}
	if (!is_blank_string(options->driver))
	{
		if (sep)
			appendStringInfoString(conn_str, sep_str);
		appendStringInfo(conn_str, "DRIVER=%s", options->driver);
		sep = TRUE;
	}
	if (!is_blank_string(options->host))
	{
		if (sep)
			appendStringInfoString(conn_str, sep_str);
		appendStringInfo(conn_str, "SERVER=%s", options->host);
		sep = TRUE;
 	}
	if (!is_blank_string(options->port))
	{
		if (sep)
			appendStringInfoString(conn_str, sep_str);
		appendStringInfo(conn_str, "PORT=%s", options->port);
		sep = TRUE;
	}
	if (!is_blank_string(options->database))
	{
		if (sep)
			appendStringInfoString(conn_str, sep_str);
		appendStringInfo(conn_str, "DATABASE=%s", options->database);
		sep = TRUE;
	}
	if (!is_blank_string(options->username))
	{
		if (sep)
			appendStringInfoString(conn_str, sep_str);
		appendStringInfo(conn_str, "UID=%s", options->username);
		sep = TRUE;
	}
	if (!is_blank_string(options->password))
	{
		if (sep)
			appendStringInfoString(conn_str, sep_str);
		appendStringInfo(conn_str, "PWD=%s", options->password);
		sep = TRUE;
	}
}

/*
 * get table size of a table
 */
static void
odbcGetTableSize(odbcFdwOptions* options, unsigned int *size)
{
	SQLHENV env;
	SQLHDBC dbc;
	SQLHSTMT stmt;
	SQLRETURN ret;

	StringInfoData  sql_str;

	SQLUBIGINT table_size;
	SQLLEN indicator;

	StringInfoData name_qualifier_char;
	StringInfoData quote_char;

	const char* schema_name;

	schema_name = get_schema_name(options);

	odbc_connection(options, &env, &dbc);

	/* Allocate a statement handle */
	SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);

	if (is_blank_string(options->sql_count))
	{
		/* Get quote char */
		getQuoteChar(dbc, &quote_char);

		/* Get name qualifier char */
		getNameQualifierChar(dbc, &name_qualifier_char);

		initStringInfo(&sql_str);
		if (is_blank_string(options->sql_query))
		{
			if (is_blank_string(schema_name))
			{
				appendStringInfo(&sql_str, "SELECT COUNT(*) FROM %s%s%s",
				                 quote_char.data, options->table, quote_char.data);
			}
			else
			{
				appendStringInfo(&sql_str, "SELECT COUNT(*) FROM %s%s%s%s%s%s%s",
				                 quote_char.data, schema_name, quote_char.data,
				                 name_qualifier_char.data,
				                 quote_char.data, options->table, quote_char.data);
			}
		}
		else
		{
			if (options->sql_query[strlen(options->sql_query)-1] == ';')
			{
				/* Remove trailing semicolon if present */
				options->sql_query[strlen(options->sql_query)-1] = 0;
			}
			appendStringInfo(&sql_str, "SELECT COUNT(*) FROM (%s) AS _odbc_fwd_count_wrapped", options->sql_query);
		}
	}
	else
	{
		initStringInfo(&sql_str);
		appendStringInfo(&sql_str, "%s", options->sql_count);
	}

    #ifdef DEBUG
		elog(DEBUG1, "Count query: %s", sql_str.data);
	#endif

	ret = SQLExecDirect(stmt, (SQLCHAR *) sql_str.data, SQL_NTS);
    check_return(ret, "Executing ODBC query", stmt, SQL_HANDLE_STMT);
	if (SQL_SUCCEEDED(ret))
	{
		SQLFetch(stmt);
		/* retrieve column data as a big int */
		ret = SQLGetData(stmt, 1, SQL_C_UBIGINT, &table_size, 0, &indicator);
		if (SQL_SUCCEEDED(ret))
		{
			*size = (unsigned int) table_size;
			#ifdef DEBUG
				elog(DEBUG1, "Count query result: %lu", table_size);
			#endif
		}
	}
	else
	{
		elog(DEBUG1, "Oops!");
	}

	/* Free handles, and disconnect */
	if (stmt)
	{
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		stmt = NULL;
	}
	if (dbc)
	{
		SQLFreeHandle(SQL_HANDLE_DBC, dbc);
		dbc = NULL;
	}
	if (env)
	{
		SQLFreeHandle(SQL_HANDLE_ENV, env);
		env = NULL;
	}
	if (dbc)
		SQLDisconnect(dbc);
}

/*
 * get quals in the select if there is one
 */
static void
odbcGetQual(Node *node, TupleDesc tupdesc, List *col_mapping_list, char **key, char **value, bool *pushdown)
{
	ListCell *col_mapping;
	*key = NULL;
	*value = NULL;
	*pushdown = false;

	#ifdef DEBUG
		elog(DEBUG1, "odbcGetQual");
	#endif

    if (!node)
		return;

	if (IsA(node, OpExpr))
	{
		OpExpr  *op = (OpExpr *) node;
		Node    *left, *right;
		Index   varattno;

		if (list_length(op->args) != 2)
			return;

		left = list_nth(op->args, 0);
		if (!IsA(left, Var))

			return;

		varattno = ((Var *) left)->varattno;

		right = list_nth(op->args, 1);

		if (IsA(right, Const))
		{
			StringInfoData  buf;
			initStringInfo(&buf);
			/* And get the column and value... */
			*key = NameStr(tupdesc->attrs[varattno - 1]->attname);

            if (((Const *) right)->consttype == PROCID_TEXTCONST)
				*value = TextDatumGetCString(((Const *) right)->constvalue);
			else
			{
				return;
			}

			/* convert qual keys to mapped couchdb attribute name */
			foreach(col_mapping, col_mapping_list)
			{
				DefElem *def = (DefElem *) lfirst(col_mapping);
				if (strcmp(def->defname, *key) == 0)
				{
					*key = defGetString(def);
					break;
				}
			}

			/*
			 * We can push down this qual if:
			 * - The operatory is TEXTEQ
			 * - The qual is on the _id column (in addition, _rev column can be also valid)
			 */

			if (op->opfuncid == PROCID_TEXTEQ)
				*pushdown = true;

            return;
		}
	}
	return;
}

/*
 * Check if the provided option is one of the valid options.
 * context is the Oid of the catalog holding the object the option is for.
 */
static bool
odbcIsValidOption(const char *option, Oid context)
{
	struct odbcFdwOption *opt;

	#ifdef DEBUG
		elog(DEBUG1, "odbcIsValidOption");
	#endif

	/* Check if the options presents in the valid option list */
	for (opt = valid_options; opt->optname; opt++)
	{
		if (context == opt->optcontext && strcmp(opt->optname, option) == 0)
			return true;
	}

	/* Foreign table may have anything as a mapping option */
	if (context == ForeignTableRelationId)
		return true;
	else
		return false;
}

static void odbcGetForeignRelSize(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid)
{
	unsigned int table_size   = 0;
	odbcFdwOptions options;

	#ifdef DEBUG
		elog(DEBUG1, "odbcGetForeignRelSize");
	#endif

	/* Fetch the foreign table options */
	odbcGetTableOptions(foreigntableid, &options);

	odbcGetTableSize(&options, &table_size);

	baserel->rows = table_size;
	baserel->tuples = baserel->rows;
}

static void odbcEstimateCosts(PlannerInfo *root, RelOptInfo *baserel, Cost *startup_cost, Cost *total_cost, Oid foreigntableid)
{
	unsigned int table_size   = 0;
	odbcFdwOptions options;

	#ifdef DEBUG
		elog(DEBUG1, "----> finishing odbcEstimateCosts");
	#endif

	/* Fetch the foreign table options */
	odbcGetTableOptions(foreigntableid, &options);

	odbcGetTableSize(&options, &table_size);

	*startup_cost = 25;

	*total_cost = baserel->rows + *startup_cost;

	#ifdef DEBUG
		ereport(DEBUG1,
		        (errmsg("----> finishing odbcEstimateCosts")
		       ));
	#endif

}

static void odbcGetForeignPaths(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid)
{
	Cost startup_cost;
	Cost total_cost;

	#ifdef DEBUG
		ereport(DEBUG,
		        (errmsg("----> starting odbcGetForeignPaths")
		       ));
	#endif

	odbcEstimateCosts(root, baserel, &startup_cost, &total_cost, foreigntableid);

	add_path(baserel,
	         (Path *) create_foreignscan_path(root, baserel, baserel->rows, startup_cost, total_cost,
	         NIL, NULL, NULL, NIL /* no fdw_private list */));

	#ifdef DEBUG
		ereport(DEBUG1,
		        (errmsg("----> finishing odbcGetForeignPaths")
		       ));
	#endif
}

static bool odbcAnalyzeForeignTable(Relation relation, AcquireSampleRowsFunc *func, BlockNumber *totalpages)
{
	#ifdef DEBUG
		ereport(DEBUG,
		        (errmsg("----> starting odbcAnalyzeForeignTable")
		       ));
	#endif

	#ifdef DEBUG
		ereport(DEBUG1,
		        (errmsg("----> finishing odbcAnalyzeForeignTable")
		       ));
	#endif

	return false;
}

static ForeignScan* odbcGetForeignPlan(PlannerInfo *root, RelOptInfo *baserel,
	Oid foreigntableid, ForeignPath *best_path, List *tlist, List *scan_clauses, Plan *outer_plan)
{
	Index scan_relid = baserel->relid;
	#ifdef DEBUG
		ereport(DEBUG,
		        (errmsg("----> starting odbcGetForeignPlan")
		       ));
	#endif

	scan_clauses = extract_actual_clauses(scan_clauses, false);

	#ifdef DEBUG
		ereport(DEBUG1,
		        (errmsg("----> finishing odbcGetForeignPlan")
		       ));
	#endif

	return make_foreignscan(tlist, scan_clauses,
	                        scan_relid, NIL, NIL,
	                        NIL /* fdw_scan_tlist */, NIL, /* fdw_recheck_quals */
	                        NULL /* outer_plan */ );
}

/*
 * odbcBeginForeignScan
 *
 */
static void
odbcBeginForeignScan(ForeignScanState *node, int eflags)
{
	SQLHENV env;
	SQLHDBC dbc;
	odbcFdwExecutionState   *festate;
	SQLSMALLINT result_columns;
	SQLHSTMT stmt;
	SQLRETURN ret;

#ifdef DEBUG
	char dsn[256];
	char desc[256];
	SQLSMALLINT dsn_ret;
	SQLSMALLINT desc_ret;
	SQLUSMALLINT direction;
#endif

	odbcFdwOptions options;

	Relation rel;
	int num_of_columns;
	StringInfoData *columns;
	int i;
	ListCell *col_mapping;
	StringInfoData sql;
	StringInfoData col_str;
	StringInfoData name_qualifier_char;
	StringInfoData quote_char;

	char *qual_key         = NULL;
	char *qual_value       = NULL;
	bool pushdown          = FALSE;

	const char* schema_name;
	int encoding = -1;

	#ifdef DEBUG
		elog(DEBUG1, "odbcBeginForeignScan");
	#endif

	/* Fetch the foreign table options */
	odbcGetTableOptions(RelationGetRelid(node->ss.ss_currentRelation), &options);

	schema_name = get_schema_name(&options);

    odbc_connection(&options, &env, &dbc);

	/* Get quote char */
	getQuoteChar(dbc, &quote_char);

	/* Get name qualifier char */
	getNameQualifierChar(dbc, &name_qualifier_char);

	if (!is_blank_string(options.encoding))
	{
		encoding = pg_char_to_encoding(options.encoding);
		if (encoding < 0)
		{
			ereport(ERROR,
			        (errcode(ERRCODE_FDW_INVALID_ATTRIBUTE_VALUE),
			         errmsg("invalid encoding name \"%s\"", options.encoding)
			        ));
		}
	}

	/* Fetch the table column info */
	rel = heap_open(RelationGetRelid(node->ss.ss_currentRelation), AccessShareLock);
	num_of_columns = rel->rd_att->natts;
	columns = (StringInfoData *) palloc(sizeof(StringInfoData) * num_of_columns);
	initStringInfo(&col_str);
	for (i = 0; i < num_of_columns; i++)
	{
		StringInfoData col;
		StringInfoData mapping;
		bool    mapped;

		/* retrieve the column name */
		initStringInfo(&col);
		appendStringInfo(&col, "%s", NameStr(rel->rd_att->attrs[i]->attname));
		mapped = FALSE;

		/* check if the column name is mapping to a different name in remote table */
		foreach(col_mapping, options.mapping_list)
		{
			DefElem *def = (DefElem *) lfirst(col_mapping);
			if (strcmp(def->defname, col.data) == 0)
			{
				initStringInfo(&mapping);
				appendStringInfo(&mapping, "%s", defGetString(def));
				mapped = TRUE;
				break;
			}
		}

		/* decide which name is going to be used */
		if (mapped)
			columns[i] = mapping;
		else
			columns[i] = col;
		appendStringInfo(&col_str, i == 0 ? "%s%s%s" : ",%s%s%s", (char *) quote_char.data, columns[i].data, (char *) quote_char.data);
	}
	heap_close(rel, NoLock);

	/* See if we've got a qual we can push down */
	if (node->ss.ps.plan->qual)
	{
		ListCell    *lc;

		foreach (lc, node->ss.ps.qual)
		{
			/* Only the first qual can be pushed down to remote DBMS */
			ExprState  *state = lfirst(lc);
			odbcGetQual((Node *) state->expr, node->ss.ss_currentRelation->rd_att, options.mapping_list, &qual_key, &qual_value, &pushdown);
			if (pushdown)
				break;
		}
	}

	/* Construct the SQL statement used for remote querying */
	initStringInfo(&sql);
	if (!is_blank_string(options.sql_query))
	{
		/* Use custom query if it's available */
		appendStringInfo(&sql, "%s", options.sql_query);
	}
	else
	{
		/* Get options.table */
		if (is_blank_string(schema_name))
		{
			appendStringInfo(&sql, "SELECT %s FROM %s%s%s", col_str.data,
							 (char *) quote_char.data, options.table, (char *) quote_char.data);
		}
		else
		{
			appendStringInfo(&sql, "SELECT %s FROM %s%s%s%s%s%s%s", col_str.data,
							 (char *) quote_char.data, schema_name, (char *) quote_char.data,
							 (char *) name_qualifier_char.data,
							 (char *) quote_char.data, options.table, (char *) quote_char.data);
		}
		if (pushdown)
		{
			appendStringInfo(&sql, " WHERE %s%s%s = '%s'",
			                 (char *) quote_char.data, qual_key, (char *) quote_char.data, qual_value);
		}
	}

	/* Allocate a statement handle */
	SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);

    #ifdef DEBUG
		elog(DEBUG1, "Executing query: %s", sql.data);
	#endif

	/* Retrieve a list of rows */
	ret = SQLExecDirect(stmt, (SQLCHAR *) sql.data, SQL_NTS);
    check_return(ret, "Executing ODBC query", stmt, SQL_HANDLE_STMT);
	SQLNumResultCols(stmt, &result_columns);

	festate = (odbcFdwExecutionState *) palloc(sizeof(odbcFdwExecutionState));
	festate->attinmeta = TupleDescGetAttInMetadata(node->ss.ss_currentRelation->rd_att);
	copy_odbcFdwOptions(&(festate->options), &options);
	festate->stmt = stmt;
	festate->table_columns = columns;
	festate->num_of_table_cols = num_of_columns;
	/* prepare for the first iteration, there will be some precalculation needed in the first iteration*/
	festate->first_iteration = TRUE;
	festate->encoding = encoding;
	node->fdw_state = (void *) festate;
}

/*
 * odbcIterateForeignScan
 *
 */
static TupleTableSlot *
odbcIterateForeignScan(ForeignScanState *node)
{
	EState *executor_state = node->ss.ps.state;
	MemoryContext prev_context;
	/* ODBC API return status */
	SQLRETURN ret;
	odbcFdwExecutionState *festate = (odbcFdwExecutionState *) node->fdw_state;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	SQLSMALLINT columns;
	char    **values;
	HeapTuple   tuple;
	StringInfoData  col_data;
	SQLHSTMT stmt = festate->stmt;
	bool first_iteration = festate->first_iteration;
	int num_of_table_cols = festate->num_of_table_cols;
	int num_of_result_cols;
	StringInfoData  *table_columns = festate->table_columns;
	List *col_position_mask = NIL;
	List *col_size_array = NIL;
	int encoding = 0;

	#ifdef DEBUG
		elog(DEBUG1, "odbcIterateForeignScan");
	#endif

	ret = SQLFetch(stmt);

	SQLNumResultCols(stmt, &columns);

	/*
	 * If this is the first iteration,
	 * we need to calculate the mask for column mapping as well as the column size
	 */
	if (first_iteration == TRUE)
	{
		SQLCHAR *ColumnName;
		SQLSMALLINT NameLengthPtr;
		SQLSMALLINT DataTypePtr;
		SQLULEN     ColumnSizePtr;
		SQLSMALLINT DecimalDigitsPtr;
		SQLSMALLINT NullablePtr;
		int i;
		int k;
		bool found;

        StringInfoData sql_type;

        SQLPOINTER      CharacterAttributePtr;
        SQLSMALLINT     BufferLength;
        SQLSMALLINT    ActualLengthPtr;
        SQLULEN        NumericAttribute;
		SQLCHAR *buffer;
		BufferLength = 1024;
		buffer = (SQLCHAR*)malloc( BufferLength*sizeof(char) );

		/* Allocate memory for the masks in a memory context that
		   persists between IterateForeignScan calls */
		prev_context = MemoryContextSwitchTo(executor_state->es_query_cxt);
		col_position_mask = NIL;
		col_size_array = NIL;
		num_of_result_cols = columns;
		/* Obtain the column information of the first row. */
		for (i = 1; i <= columns; i++)
		{
			found = FALSE;
			ColumnName = (SQLCHAR *) palloc(sizeof(SQLCHAR) * MAXIMUM_COLUMN_NAME_LEN);
			SQLDescribeCol(stmt,
			               i,                       /* ColumnName */
			               ColumnName,
			               sizeof(SQLCHAR) * MAXIMUM_COLUMN_NAME_LEN, /* BufferLength */
			               &NameLengthPtr,
			               &DataTypePtr,
			               &ColumnSizePtr,
			               &DecimalDigitsPtr,
			               &NullablePtr);

			sql_data_type(DataTypePtr, ColumnSizePtr, DecimalDigitsPtr, NullablePtr, &sql_type);

			/* Get the position of the column in the FDW table */
			for (k=0; k<num_of_table_cols; k++)
			{
				if (strcmp(table_columns[k].data, (char *) ColumnName) == 0)
				{
					found = TRUE;
					col_position_mask = lappend_int(col_position_mask, k);
					col_size_array = lappend_int(col_size_array, (int) ColumnSizePtr);
					break;
				}
			}
			/* if current column is not used by the foreign table */
			if (!found)
			{
				col_position_mask = lappend_int(col_position_mask, -1);
				col_size_array = lappend_int(col_size_array, -1);
			}
			pfree(ColumnName);
		}
		festate->num_of_result_cols = num_of_result_cols;
		festate->col_position_mask = col_position_mask;
		festate->col_size_array = col_size_array;
		festate->first_iteration = FALSE;

		MemoryContextSwitchTo(prev_context);
	}
	else
	{
		num_of_result_cols = festate->num_of_result_cols;
		col_position_mask = festate->col_position_mask;
		col_size_array = festate->col_size_array;
	}

	ExecClearTuple(slot);
	if (SQL_SUCCEEDED(ret))
	{
		SQLSMALLINT i;
		values = (char **) palloc(sizeof(char *) * columns);

		/* Loop through the columns */
		for (i = 1; i <= columns; i++)
		{
			SQLLEN indicator;
			char * buf;

			int mask_index = i - 1;
			int col_size = list_nth_int(col_size_array, mask_index);
			int mapped_pos = list_nth_int(col_position_mask, mask_index);

			/* Ignore this column if position is marked as invalid */
			if (mapped_pos == -1)
				continue;

			buf = (char *) palloc(sizeof(char) * (col_size+1));

			/* retrieve column data as a zero-terminated string */
			/* TODO:
			   binary fields (SQL_C_BIT, SQL_C_BINARY) do not have
			   a traling zero; they should be copied as now but without
			   adding 1 to col_size, or using SQL_C_BIT or SQL_C_BINARY
			   and then encoded into a binary PG literal (e.g. X'...'
			   or B'...')
			   For floating point types we should use SQL_C_FLOAT/SQL_C_DOUBLE
			   to avoid precision loss.
			   For date/time/timestamp these structures can be used:
			   SQL_C_TYPE_DATE/SQL_C_TYPE_TIME/SQL_C_TYPE_TIMESTAMP.
			   And finally, SQL_C_NUMERIC and SQL_C_GUID could also be used.
			*/
			ret = SQLGetData(stmt, i, SQL_C_CHAR,
							 buf, sizeof(char) * (col_size+1), &indicator);

			if (SQL_SUCCEEDED(ret))
			{
				/* Handle null columns */
				if (indicator == SQL_NULL_DATA)
				{
				  // BuildTupleFromCStrings expects NULLs to be NULL pointers
				  values[mapped_pos] = NULL;
				}
				else
				{
					if (festate->encoding != -1)
					{
						/* Convert character encoding */
						buf = pg_any_to_server(buf, strlen(buf), festate->encoding);
					}
				 	initStringInfo(&col_data);
				 	appendStringInfoString (&col_data, buf);

					values[mapped_pos] = col_data.data;
				}
			}
			pfree(buf);
		}

		tuple = BuildTupleFromCStrings(festate->attinmeta, values);
		ExecStoreTuple(tuple, slot, InvalidBuffer, false);
		pfree(values);
	}

	return slot;
}

/*
 * odbcExplainForeignScan
 *
 */
static void
odbcExplainForeignScan(ForeignScanState *node, ExplainState *es)
{
	odbcFdwExecutionState *festate;
	unsigned int table_size = 0;

	#ifdef DEBUG
		elog(DEBUG1, "odbcExplainForeignScan");
	#endif

	festate = (odbcFdwExecutionState *) node->fdw_state;

	odbcGetTableSize(&(festate->options), &table_size);

	/* Suppress file size if we're not showing cost details */
	if (es->costs)
	{
		ExplainPropertyLong("Foreign Table Size", table_size, es);
	}
}

/*
 * odbcEndForeignScan
 *      Finish scanning foreign table and dispose objects used for this scan
 */
static void
odbcEndForeignScan(ForeignScanState *node)
{
	odbcFdwExecutionState *festate;

	#ifdef DEBUG
		elog(DEBUG1, "odbcEndForeignScan");
	#endif

	/* if festate is NULL, we are in EXPLAIN; nothing to do */
	festate = (odbcFdwExecutionState *) node->fdw_state;
	if (festate)
	{
		if (festate->stmt)
		{
			SQLFreeHandle(SQL_HANDLE_STMT, festate->stmt);
			festate->stmt = NULL;
		}
	}
}

/*
 * odbcReScanForeignScan
 *      Rescan table, possibly with new parameters
 */
static void
odbcReScanForeignScan(ForeignScanState *node)
{
	#ifdef DEBUG
		elog(DEBUG1, "odbcReScanForeignScan");
	#endif
}


static void
appendOption(StringInfo str, bool first, const char* option_name, const char* option_value)
{
	if (!first)
	{
		appendStringInfo(str, ",\n");
	}
	appendStringInfo(str, "%s '%s'", option_name, option_value);
}

List *
odbcImportForeignSchema(ImportForeignSchemaStmt *stmt, Oid serverOid)
{
	/* TODO: review memory management in this function; any leaks? */
	odbcFdwOptions options;

	List* create_statements = NIL;
	List* tables = NIL;
	List* table_columns = NIL;
	ListCell *tables_cell;
	ListCell *table_columns_cell;
	RangeVar *table_rangevar;

	SQLHENV env;
	SQLHDBC dbc;
	SQLHSTMT query_stmt;
	SQLHSTMT columns_stmt;
	SQLHSTMT tables_stmt;
	SQLRETURN ret;
	SQLSMALLINT result_columns;
	StringInfoData col_str;
	SQLCHAR *ColumnName;
	SQLCHAR *TableName;
	SQLSMALLINT NameLength;
	SQLSMALLINT DataType;
	SQLULEN     ColumnSize;
	SQLSMALLINT DecimalDigits;
	SQLSMALLINT Nullable;
	int i;
	StringInfoData sql_type;
	SQLLEN indicator;
	const char* schema_name;

	#ifdef DEBUG
		elog(DEBUG1, "odbcImportForeignSchema");
	#endif

	odbcGetOptions(serverOid, stmt->options, &options);

	schema_name = get_schema_name(&options);

	if (!is_blank_string(options.sql_query))
	{
		/* Generate foreign table for a query */
		if (is_blank_string(options.table))
		{
			/* TODO: error */
			elog(ERROR, "Must provide 'table' option to name the foreign table");
		}

		odbc_connection(&options, &env, &dbc);

		/* Allocate a statement handle */
		SQLAllocHandle(SQL_HANDLE_STMT, dbc, &query_stmt);

		/* Retrieve a list of rows */
		ret = SQLExecDirect(query_stmt, (SQLCHAR *) options.sql_query, SQL_NTS);
		check_return(ret, "Executing ODBC query", query_stmt, SQL_HANDLE_STMT);

		SQLNumResultCols(query_stmt, &result_columns);

		initStringInfo(&col_str);
		ColumnName = (SQLCHAR *) palloc(sizeof(SQLCHAR) * MAXIMUM_COLUMN_NAME_LEN);

		for (i = 1; i <= result_columns; i++)
		{
			SQLDescribeCol(query_stmt,
			               i,                       /* ColumnName */
			               ColumnName,
			               sizeof(SQLCHAR) * MAXIMUM_COLUMN_NAME_LEN, /* BufferLength */
			               &NameLength,
			               &DataType,
			               &ColumnSize,
			               &DecimalDigits,
			               &Nullable);

			sql_data_type(DataType, ColumnSize, DecimalDigits, Nullable, &sql_type);
			if (i > 1)
			{
				appendStringInfo(&col_str, ", ");
			}
			appendStringInfo(&col_str, "\"%s\" %s", ColumnName, (char *) sql_type.data);
		}
		SQLCloseCursor(query_stmt);
		SQLFreeHandle(SQL_HANDLE_STMT, query_stmt);

		tables        = lappend(tables, (void*)options.table);
		table_columns = lappend(table_columns, (void*)col_str.data);
	}
	else
	{
		/* Reflect one or more foreign tables */
		if (!is_blank_string(options.table))
		{
			tables = lappend(tables, (void*)options.table);
		}
		else if (stmt->list_type == FDW_IMPORT_SCHEMA_ALL || stmt->list_type == FDW_IMPORT_SCHEMA_EXCEPT)
		{
			/* Will obtain the foreign tables with SQLTables() */

			odbc_connection(&options, &env, &dbc);

			/* Allocate a statement handle */
			SQLAllocHandle(SQL_HANDLE_STMT, dbc, &tables_stmt);

			ret = SQLTables(
				 tables_stmt,
				 NULL, 0, /* Catalog: (SQLCHAR*)SQL_ALL_CATALOGS, SQL_NTS would include also tables from internal catalogs */
				 NULL, 0, /* Schema: we avoid filtering by schema here to avoid problems with some drivers */
				 NULL, 0, /* Table */
				 (SQLCHAR*)"TABLE", SQL_NTS /* Type of table (we're not interested in views, temporary tables, etc.) */
			);
			check_return(ret, "Obtaining ODBC tables", tables_stmt, SQL_HANDLE_STMT);

			initStringInfo(&col_str);
			SQLCHAR *table_catalog = (SQLCHAR *) palloc(sizeof(SQLCHAR) * MAXIMUM_CATALOG_NAME_LEN);
			SQLCHAR *table_schema = (SQLCHAR *) palloc(sizeof(SQLCHAR) * MAXIMUM_SCHEMA_NAME_LEN);
			while (SQL_SUCCESS == ret)
			{
				ret = SQLFetch(tables_stmt);
				if (SQL_SUCCESS == ret)
				{
					int excluded = FALSE;
					TableName = (SQLCHAR *) palloc(sizeof(SQLCHAR) * MAXIMUM_TABLE_NAME_LEN);
					ret = SQLGetData(tables_stmt, 3, SQL_C_CHAR, TableName, MAXIMUM_TABLE_NAME_LEN, &indicator);
					check_return(ret, "Reading table name", tables_stmt, SQL_HANDLE_STMT);

					/* Since we're not filtering the SQLTables call by schema
					   we must exclude here tables that belong to other schemas.
					   For some ODBC drivers tables may not be organized into
					   schemas and the schema of the table will be blank.
					   So we only reject tables for which the schema is not
					   blank and different from the desired schema:
					 */
					ret = SQLGetData(tables_stmt, 2, SQL_C_CHAR, table_schema, MAXIMUM_SCHEMA_NAME_LEN, &indicator);
					if (!is_blank_string(table_schema) && strcmp(table_schema, schema_name) )
					{
						excluded = TRUE;
					}

					/* Since we haven't specified SQL_ALL_CATALOGS in the
					   call to SQLTables we shouldn't get tables from special
					   catalogs and only from the regular catalog of the database
					   (named as the database or blank, depending on the driver)
					   but to be sure we'll reject tables from catalogs with
					   other names:
					 */
					ret = SQLGetData(tables_stmt, 1, SQL_C_CHAR, table_catalog, MAXIMUM_CATALOG_NAME_LEN, &indicator);
					if (!is_blank_string(table_catalog) && strcmp(table_catalog, schema_name))
					{
						if (is_blank_string(options.database) || strcmp(table_catalog, options.database))
						{
							excluded = TRUE;
						}
					}

					/* And now we'll handle tables excluded by an EXCEPT clause */
					if (!excluded && stmt->list_type == FDW_IMPORT_SCHEMA_EXCEPT)
					{
						foreach(tables_cell,  stmt->table_list)
						{
							table_rangevar = (RangeVar*)lfirst(tables_cell);
							if (strcmp(TableName, table_rangevar->relname) == 0)
							{
								excluded = TRUE;
							}
						}
					}

					if (!excluded)
					{
						tables = lappend(tables, (void*)TableName);
					}
				}
			}
			SQLCloseCursor(tables_stmt);
			SQLFreeHandle(SQL_HANDLE_STMT, tables_stmt);
		}
		else if (stmt->list_type == FDW_IMPORT_SCHEMA_LIMIT_TO)
		{
			foreach(tables_cell, stmt->table_list)
			{
				table_rangevar = (RangeVar*)lfirst(tables_cell);
				tables = lappend(tables, (void*)table_rangevar->relname);
			}
		}
		else
		{
			elog(ERROR,"Unknown list type in IMPORT FOREIGN SCHEMA");
		}
        foreach(tables_cell, tables)
		{
			char *table_name = (char*)lfirst(tables_cell);

			odbc_connection(&options, &env, &dbc);

			/* Allocate a statement handle */
			SQLAllocHandle(SQL_HANDLE_STMT, dbc, &columns_stmt);

			ret = SQLColumns(
				 columns_stmt,
				 NULL, 0,
				 (SQLCHAR*)schema_name, SQL_NTS,
				 (SQLCHAR*)table_name,  SQL_NTS,
				 NULL, 0
			);
			check_return(ret, "Obtaining ODBC columns", columns_stmt, SQL_HANDLE_STMT);

            i = 0;
			initStringInfo(&col_str);
			ColumnName = (SQLCHAR *) palloc(sizeof(SQLCHAR) * MAXIMUM_COLUMN_NAME_LEN);
			while (SQL_SUCCESS == ret)
			{
				ret = SQLFetch(columns_stmt);
				if (SQL_SUCCESS == ret)
				{
					if (++i > 1)
					{
						appendStringInfo(&col_str, ", ");
					}
					ret = SQLGetData(columns_stmt, 4, SQL_C_CHAR, ColumnName, MAXIMUM_COLUMN_NAME_LEN, &indicator);
					// check_return(ret, "Reading column name", columns_stmt, SQL_HANDLE_STMT);
					ret = SQLGetData(columns_stmt, 5, SQL_C_SSHORT, &DataType, MAXIMUM_COLUMN_NAME_LEN, &indicator);
					// check_return(ret, "Reading column type", columns_stmt, SQL_HANDLE_STMT);
					ret = SQLGetData(columns_stmt, 7, SQL_C_SLONG, &ColumnSize, 0, &indicator);
					// check_return(ret, "Reading column size", columns_stmt, SQL_HANDLE_STMT);
					ret = SQLGetData(columns_stmt, 9, SQL_C_SSHORT, &DecimalDigits, 0, &indicator);
					// check_return(ret, "Reading column decimals", columns_stmt, SQL_HANDLE_STMT);
					ret = SQLGetData(columns_stmt, 11, SQL_C_SSHORT, &Nullable, 0, &indicator);
					// check_return(ret, "Reading column nullable", columns_stmt, SQL_HANDLE_STMT);
					sql_data_type(DataType, ColumnSize, DecimalDigits, Nullable, &sql_type);
					appendStringInfo(&col_str, "\"%s\" %s", ColumnName, (char *) sql_type.data);
				}
			}
			SQLCloseCursor(columns_stmt);
			SQLFreeHandle(SQL_HANDLE_STMT, columns_stmt);
			table_columns = lappend(table_columns, (void*)col_str.data);
		}
	}

	/* Generate create statements */
	table_columns_cell = list_head(table_columns);
	foreach(tables_cell, tables)
	{
		// temporarily define vars here...
		char *table_name = (char*)lfirst(tables_cell);
		char *columns    = (char*)lfirst(table_columns_cell);

		table_columns_cell = lnext(table_columns_cell);
        StringInfoData create_statement;
		ListCell *option;
		int option_count = 0;
		char *prefix = empty_string_if_null(options.prefix);
		initStringInfo(&create_statement);
		appendStringInfo(&create_statement, "CREATE FOREIGN TABLE \"%s%s\" (", prefix, (char *) table_name);
		appendStringInfo(&create_statement, "%s", columns);
		appendStringInfo(&create_statement, ") SERVER %s\n", stmt->server_name);
		appendStringInfo(&create_statement, "OPTIONS (\n");
		foreach(option, stmt->options)
		{
			DefElem *def = (DefElem *) lfirst(option);
			appendOption(&create_statement, ++option_count == 1, def->defname, defGetString(def));
		}
		if (is_blank_string(options.table))
		{
			appendOption(&create_statement, ++option_count == 1, "table", table_name);
		}
		appendStringInfo(&create_statement, ");");
		elog(DEBUG1, "CREATE: %s", create_statement.data);
		create_statements = lappend(create_statements, (void*)create_statement.data);
	}

   return create_statements;
}
