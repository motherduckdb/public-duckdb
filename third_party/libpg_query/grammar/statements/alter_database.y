/*****************************************************************************
 *
 * Alter Database Statement
 *
 *****************************************************************************/
AlterDatabaseStmt:
			ALTER DATABASE ColId SET IDENT TO ColId
				{
					char *lower = pstrdup($5);
					for (char *p = lower; *p; p++) { *p = (*p >= 'A' && *p <= 'Z') ? *p + ('a' - 'A') : *p; }
					if (strcmp(lower, "alias") != 0) {
						ereport(ERROR,
								(errcode(PG_ERRCODE_SYNTAX_ERROR),
								 errmsg("expected SET ALIAS TO, got SET %s TO", $5),
								 parser_errposition(@5)));
					}
					PGAlterDatabaseStmt *n = makeNode(PGAlterDatabaseStmt);
					n->dbname = $3;
					n->new_name = $7;
					n->alter_type = PG_ALTER_DATABASE_RENAME;
					n->missing_ok = false;
					$$ = (PGNode *)n;
				}
			| ALTER DATABASE IF_P EXISTS ColId SET IDENT TO ColId
				{
					char *lower = pstrdup($7);
					for (char *p = lower; *p; p++) { *p = (*p >= 'A' && *p <= 'Z') ? *p + ('a' - 'A') : *p; }
					if (strcmp(lower, "alias") != 0) {
						ereport(ERROR,
								(errcode(PG_ERRCODE_SYNTAX_ERROR),
								 errmsg("expected SET ALIAS TO, got SET %s TO", $7),
								 parser_errposition(@7)));
					}
					PGAlterDatabaseStmt *n = makeNode(PGAlterDatabaseStmt);
					n->dbname = $5;
					n->new_name = $9;
					n->alter_type = PG_ALTER_DATABASE_RENAME;
					n->missing_ok = true;
					$$ = (PGNode *)n;
				}
		;
