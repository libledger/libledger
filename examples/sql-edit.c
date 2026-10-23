/* sql-edit: run one SQL statement on a database file, bypassing libledger.
   Usage: sql-edit <file> <statement> */
#include "sqlite3.h"
#include <stdio.h>

int main(int argc, char **argv) {
    sqlite3 *db = NULL;
    char *err = NULL;
    if (argc != 3 || sqlite3_open(argv[1], &db) != SQLITE_OK) {
        fprintf(stderr, "usage: sql-edit <file> <statement>\n");
        return 2;
    }
    if (sqlite3_exec(db, argv[2], NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "sql-edit: %s\n", err);
        sqlite3_close(db);
        return 1;
    }
    printf("%d row(s) changed\n", sqlite3_changes(db));
    sqlite3_close(db);
    return 0;
}
