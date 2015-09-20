#ifndef PARSER_H
#define PARSER_H

#include <stdbool.h>

#include "int.h"
#include "limits.h"

#define CMD_BEGIN    'b'
#define CMD_COMMIT   'c'
#define CMD_ABORT    'a'
#define CMD_SNAPSHOT 'h'
#define CMD_STATUS   's'

typedef struct cmd_t {
	char cmd;
	int argc;
	xid_t argv[MAX_NODES * 2];
} cmd_t;

// Do not rely on the inner structure, it may change tomorrow.
typedef struct parser_data_t *parser_t;

// Allocates and initializes a parser.
parser_t parser_create();

// Destroys the parser. The 'p' handle becomes invalid, so do not refer to it
// after destroying the parser.
void parser_destroy(parser_t p);

// Initializes the parser.
void parser_init(parser_t p);

// Checks if parser has failed.
bool parser_failed(parser_t p);

// Gets the error message for the parser.
char *parser_errormsg(parser_t p);

// Feeds a character to the parser, and returns a parsed command if it is
// complete. Returns NULL if command is not complete. The caller should check
// for errors, please use parser_failed() method for that. Also the caller
// should free the cmd after use.
cmd_t *parser_feed(parser_t p, char c);

#endif