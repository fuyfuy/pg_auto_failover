/*
 * src/bin/pg_autoctl/pghba.c
 *	 Functions for manipulating pg_hba.conf
 *
 * Copyright (c) Microsoft Corporation. All rights reserved.
 * Licensed under the PostgreSQL License.
 *
 */
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "postgres_fe.h"
#include "pqexpbuffer.h"

#include "defaults.h"
#include "file_utils.h"
#include "ipaddr.h"
#include "parsing.h"
#include "pgctl.h"
#include "pghba.h"
#include "log.h"


#define HBA_LINE_COMMENT " # Auto-generated by pg_auto_failover"


static void append_database_field(PQExpBuffer destination,
								  HBADatabaseType databaseType,
								  const char *databaseName);
static void append_hostname_or_cidr(PQExpBuffer destination,
									const char *host);
static int escape_hba_string(char *destination, const char *hbaString);


/*
 * pghba_ensure_host_rule_exists ensures that a host rule exists in the
 * pg_hba file with the given database, username, host and authentication
 * scheme.
 */
bool
pghba_ensure_host_rule_exists(const char *hbaFilePath,
							  bool ssl,
							  HBADatabaseType databaseType,
							  const char *database,
							  const char *username,
							  const char *host,
							  const char *authenticationScheme)
{
	char *currentHbaContents = NULL;
	long currentHbaSize = 0L;
	char *includeLine = NULL;
	PQExpBuffer hbaLineBuffer = createPQExpBuffer();
	PQExpBuffer newHbaContents = NULL;

	if (hbaLineBuffer == NULL)
	{
		log_error("Failed to allocate memory");
		return false;
	}

	if (ssl)
	{
		appendPQExpBufferStr(hbaLineBuffer, "hostssl ");
	}
	else
	{
		appendPQExpBufferStr(hbaLineBuffer, "host ");
	}

	append_database_field(hbaLineBuffer, databaseType, database);
	appendPQExpBufferStr(hbaLineBuffer, " ");

	if (username)
	{
		char escapedUsername[BUFSIZE] = { 0 };
		(void) escape_hba_string(escapedUsername, username);

		appendPQExpBufferStr(hbaLineBuffer, escapedUsername);
		appendPQExpBufferStr(hbaLineBuffer, " ");
	}
	else
	{
		appendPQExpBufferStr(hbaLineBuffer, "all ");
	}

	append_hostname_or_cidr(hbaLineBuffer, host);
	appendPQExpBuffer(hbaLineBuffer, " %s", authenticationScheme);

	if (PQExpBufferBroken(hbaLineBuffer))
	{
		log_error("Failed to allocate memory");
		destroyPQExpBuffer(hbaLineBuffer);
		return false;
	}

	/*
	 * When the authentication method is "skip", the option --skip-pg-hba has
	 * been used. In that case, we still WARN about the HBA rule that we need,
	 * so that users can review their HBA settings and provisioning.
	 */
	if (SKIP_HBA(authenticationScheme))
	{
		log_warn("Skipping HBA edits (per --skip-pg-hba) for rule: %s",
				 hbaLineBuffer->data);
		return true;
	}

	log_debug("Ensuring the HBA file \"%s\" contains the line: %s",
			  hbaFilePath, hbaLineBuffer->data);

	if (!read_file(hbaFilePath, &currentHbaContents, &currentHbaSize))
	{
		/* read_file logs an error */
		return false;
	}

	includeLine = strstr(currentHbaContents, hbaLineBuffer->data);

	/*
	 * If the rule was found and it starts on a new line. We can
	 * skip adding it.
	 */
	if (includeLine != NULL && (includeLine == currentHbaContents ||
								includeLine[-1] == '\n'))
	{
		log_debug("Line already exists in %s, skipping", hbaFilePath);
		free(currentHbaContents);
		return true;
	}

	/* build the new postgresql.conf contents */
	newHbaContents = createPQExpBuffer();
	if (newHbaContents == NULL)
	{
		log_error("Failed to allocate memory");
		free(currentHbaContents);
		return false;
	}

	appendPQExpBufferStr(newHbaContents, currentHbaContents);
	appendPQExpBufferStr(newHbaContents, hbaLineBuffer->data);
	appendPQExpBufferStr(newHbaContents, HBA_LINE_COMMENT "\n");

	/* done with the old postgresql.conf contents */
	free(currentHbaContents);

	/* memory allocation could have failed while building string */
	if (PQExpBufferBroken(newHbaContents))
	{
		log_error("Failed to allocate memory");
		destroyPQExpBuffer(newHbaContents);
		return false;
	}

	/* write the new postgresql.conf */
	if (!write_file(newHbaContents->data, newHbaContents->len, hbaFilePath))
	{
		/* write_file logs an error */
		destroyPQExpBuffer(newHbaContents);
		return false;
	}

	destroyPQExpBuffer(hbaLineBuffer);
	destroyPQExpBuffer(newHbaContents);

	log_debug("Wrote new %s", hbaFilePath);

	return true;
}


/*
 * append_database_field writes the database field to destination according to
 * the databaseType. If the type is HBA_DATABASE_DBNAME then the databaseName
 * is written in quoted form.
 */
static void
append_database_field(PQExpBuffer destination,
					  HBADatabaseType databaseType,
					  const char *databaseName)
{
	switch (databaseType)
	{
		case HBA_DATABASE_ALL:
		{
			appendPQExpBufferStr(destination, "all");
			break;
		}

		case HBA_DATABASE_REPLICATION:
		{
			appendPQExpBufferStr(destination, "replication");
			break;
		}

		case HBA_DATABASE_DBNAME:
		default:
		{
			/* Postgres database names are NAMEDATALEN (64), BUFSIZE is 1024 */
			char escapedDatabaseName[BUFSIZE] = { 0 };
			(void) escape_hba_string(escapedDatabaseName, databaseName);

			appendPQExpBufferStr(destination, escapedDatabaseName);
			break;
		}
	}
}


/*
 * append_hostname_or_cidr checks whether the host is an IP and if so converts
 * it to a CIDR and writes it to destination. Otherwise, convert_ip_to_cidr
 * writes the host directly to the destination.
 */
static void
append_hostname_or_cidr(PQExpBuffer destination, const char *host)
{
	switch (ip_address_type(host))
	{
		case IPTYPE_V4:
		{
			appendPQExpBuffer(destination, "%s/32", host);
			break;
		}

		case IPTYPE_V6:
		{
			appendPQExpBuffer(destination, "%s/128", host);
			break;
		}

		case IPTYPE_NONE:
		default:
		{
			appendPQExpBufferStr(destination, host);
			break;
		}
	}
}


/*
 * escape_hba_string escapes a string that is used in a pg_hba.conf file
 * and writes it to the destination. escape_hba_string returns the number
 * of characters written.
 *
 * While this is not documented, the code in hba.c (next_token) implements
 * two double-quotes as a literal double quote.
 */
static int
escape_hba_string(char *destination, const char *hbaString)
{
	int charIndex = 0;
	int length = strlen(hbaString);
	int escapedStringLength = 0;

	destination[escapedStringLength++] = '"';

	for (charIndex = 0; charIndex < length; charIndex++)
	{
		char currentChar = hbaString[charIndex];
		if (currentChar == '"')
		{
			destination[escapedStringLength++] = '"';
		}

		destination[escapedStringLength++] = currentChar;
	}

	destination[escapedStringLength++] = '"';
	destination[escapedStringLength] = '\0';

	return escapedStringLength;
}


/*
 * pghba_enable_lan_cidr adds our local CIDR network notation (e.g.
 * 192.168.0.0/23) to the HBA file of the PostgreSQL server, so that any node
 * in the local network may connect already.
 *
 * Failure is a warning only.
 *
 * In normal cases, pgdata is NULL and pghba_enable_lan_cidr queries the local
 * PostgreSQL server for the location of its HBA file.
 *
 * When initializing a PostgreSQL cluster in a test environment using
 * PG_REGRESS_SOCK_DIR="" and --listen options, then we have to add an HBA rule
 * before starting PostgreSQL, otherwise we don't have a path to connect to it.
 * In that case we pass in PGDATA and pghba_enable_lan_cidr uses the file
 * PGDATA/pg_hba.conf as the hbaFilePath: we just did `pg_ctl initdb` after
 * all, it should be safe.
 */
bool
pghba_enable_lan_cidr(PGSQL *pgsql,
					  bool ssl,
					  HBADatabaseType databaseType,
					  const char *database,
					  const char *hostname,
					  const char *username,
					  const char *authenticationScheme,
					  const char *pgdata)
{
	char hbaFilePath[MAXPGPATH];
	char ipAddr[BUFSIZE];
	char cidr[BUFSIZE];

	/* Compute the CIDR notation for our hostname */
	if (!findHostnameLocalAddress(hostname, ipAddr, BUFSIZE))
	{
		int logLevel = SKIP_HBA(authenticationScheme) ? LOG_WARN : LOG_FATAL;

		log_level(logLevel,
				  "Failed to find IP address for hostname \"%s\", "
				  "see above for details",
				  hostname);

		/* when --skip-pg-hba is used, we don't mind the failure here */
		return SKIP_HBA(authenticationScheme) ? true : false;
	}

	if (!fetchLocalCIDR(ipAddr, cidr, BUFSIZE))
	{
		log_warn("Failed to determine network configuration for "
				 "IP address \"%s\", skipping HBA settings", ipAddr);

		/* when --skip-pg-hba is used, we don't mind the failure here */
		return SKIP_HBA(authenticationScheme) ? true : false;
	}

	log_debug("HBA: adding CIDR from hostname \"%s\"", hostname);
	log_debug("HBA: local ip address: %s", ipAddr);
	log_debug("HBA: CIDR address to open: %s", cidr);

	log_info("Granting connection privileges on %s", cidr);

	/* The caller gives pgdata when PostgreSQL is not yet running */
	if (pgdata == NULL)
	{
		if (!pgsql_get_hba_file_path(pgsql, hbaFilePath, MAXPGPATH))
		{
			/* unexpected */
			log_error("Failed to obtain the HBA file path from the local "
					  "PostgreSQL server.");
			return false;
		}
	}
	else
	{
		sformat(hbaFilePath, MAXPGPATH, "%s/pg_hba.conf", pgdata);
	}

	/*
	 * We still go on when skipping HBA, so that we display a useful message to
	 * the user with the specific rule we are skipping here.
	 */
	if (!pghba_ensure_host_rule_exists(hbaFilePath, ssl, databaseType, database,
									   username, cidr, authenticationScheme))
	{
		log_error("Failed to add the local network to PostgreSQL HBA file: "
				  "couldn't modify the pg_hba file");
		return false;
	}

	/*
	 * pgdata is given when PostgreSQL is not yet running, don't reload then...
	 */
	if (pgdata == NULL &&
		!SKIP_HBA(authenticationScheme) &&
		!pgsql_reload_conf(pgsql))
	{
		log_error("Failed to reload PostgreSQL configuration for new HBA rule");
		return false;
	}
	return true;
}
