#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>

static const char *progname = "slkwvr";

/* print to file if file is present (sort of a hack) */
static int safe_fprintf(FILE *fp, const char *fmt, ...) {

	if (!fp) return 0;

	va_list args;
	va_start(args, fmt);
	int count = vfprintf(fp, fmt, args);
	va_end(args);

	return count;
}

/* position information */
struct position {
	FILE *fp;
	const char *path;
	int cc;
	long line, column;
};
#define POSITION_INIT(p_fp, p_path) {\
		.fp = (p_fp),\
		.path = (p_path),\
		.cc = 1,\
		.line = 1,\
		.column = 0,\
	}
static void position_advance(struct position *position) {

	if (!position->cc) return;

	position->cc = fgetc(position->fp);
	position->column++;

	if (position->cc == EOF)
		position->cc = 0;

	else if (position->cc == '\n') {

		position->line++;
		position->column = 0;
	}
}

/* Print error */
static void print_error(
		struct position *position,
		const char *format,
		...
) {
	fprintf(stderr, "%s:", progname);
	if (position)
		fprintf(
			stderr,
			"%s:%ld,%ld:",
			position->path,
			position->line,
			position->column
		);
	fputc(' ', stderr);

	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);

	fputc('\n', stderr);
}

/* token */
enum token_type {
	TOKEN_TYPE_NUMBER = 0,
	TOKEN_TYPE_KEYWORD,
	TOKEN_TYPE_BUILTIN_TYPE,
	TOKEN_TYPE_STRING,
	TOKEN_TYPE_TYPE,
	TOKEN_TYPE_FIELD,
	TOKEN_TYPE_GLOBAL,
	TOKEN_TYPE_LOCAL,
	TOKEN_TYPE_SYMBOL,

	TOKEN_TYPE_EOL,
	TOKEN_TYPE_EOF,

	TOKEN_TYPE_COUNT,
};
struct token {
	struct token *next;
	enum token_type type;
	struct position position;
	size_t value_size;
	char value[];
};

static const char *keywords[] = {
	"str", "proc", "var",
	"if", "else",
	"for", "continue", "break",
	"ret",
	"and", "or",
	NULL,
};
static const char *builtin_types[] = {
	"i8", "u8", "i16", "u16",
	"i32", "u32", "i64", "u64",
	"f32", "f64", "ptr",
};

static bool token_match(
		struct token *token,
		enum token_type type,
		const char *value
) {
	return token->type == type && !strcmp(token->value, value);
}

#define token_match_symbol(p_token, p_value) token_match((p_token), TOKEN_TYPE_SYMBOL, (p_value))
#define token_match_keyword(p_token, p_value) token_match((p_token), TOKEN_TYPE_KEYWORD, (p_value))

/* node */
#define NODE_MAX_TOKENS 4
#define NODE_MAX_VALUES 4

enum node_type {
	NODE_TYPE_BLOCK = 0,
	NODE_TYPE_TYPE,

	NODE_TYPE_STRUCT,
	NODE_TYPE_FIELD,
	NODE_TYPE_PROCEDURE,
	NODE_TYPE_PARAMETER,
	NODE_TYPE_VARIABLE,
	NODE_TYPE_IF,
	NODE_TYPE_FOR,
	NODE_TYPE_CONTINUE,
	NODE_TYPE_BREAK,
	NODE_TYPE_RETURN,
	NODE_TYPE_BINOP,
	NODE_TYPE_UNOP,
	NODE_TYPE_CALL,
	NODE_TYPE_NUMBER,
	NODE_TYPE_STRING,
	NODE_TYPE_GLOBAL,
	NODE_TYPE_LOCAL,
	NODE_TYPE_CAST,
	NODE_TYPE_MEMBER,
	NODE_TYPE_DEREFERENCE,
	NODE_TYPE_REFERENCE,

	NODE_TYPE_COUNT,
};

struct node {
	enum node_type type;
	struct position position;
	struct node *first, *last, *next;
	struct token *tokens[NODE_MAX_TOKENS];
	intmax_t values[NODE_MAX_VALUES];
};

static struct node *node_create(
		enum node_type type,
		struct position *position
) {
	struct node *node = (struct node *)malloc(sizeof(struct node));
	memset(node, 0, sizeof(struct node));

	node->type = type;
	node->position = *position;

	return node;
}

static void node_add_child(
		struct node *node,
		struct node *child
) {
	if (!node->first) node->first = child;
	if (node->last) node->last->next = child;
	node->last = child;
}

static void node_destroy(struct node *node) {

	struct node *cur = node->first;
	while (cur) {

		struct node *next = cur->next;
		node_destroy(cur);
		cur = next;
	}
	free(node);
}

/* get hash of value */
static uint32_t u32pow(uint32_t x, uint32_t y) {

	uint32_t r = 1;
	for (uint32_t i = 0; i < y; i++)
		r *= x;
	return r;
}
static uint32_t strhash(const char *s) {

	uint32_t r = 0;
	if (!*s) return 0;
	uint32_t l = (uint32_t)strlen(s);

	for (uint32_t i = 0; i < l-1; i++)
		r += (uint32_t)s[i] * u32pow(31, l-i-1);
	r += (uint32_t)s[l-1];

	return r;
}

/* hash map */
#define MAP_VALUES 4

struct map {
	uint32_t key_hash;
	uintptr_t values[MAP_VALUES];
	struct map *next;
};

static struct map *map_slot(struct map *map, const char *key) {

	uint32_t key_hash = strhash(key);

	struct map *previous = map;
	for (map = map->next; map; previous = map, map = map->next) {

		if (map->key_hash == key_hash)
			return map;
	}
	map = (struct map *)malloc(sizeof(struct map));
	memset(map, 0, sizeof(struct map));

	map->key_hash = key_hash;
	previous->next = map;

	return map;
}

static struct map *map_get(struct map *map, const char *key) {

	uint32_t key_hash = strhash(key);
	for (map = map->next; map; map = map->next) {

		if (map->key_hash == key_hash)
			return map;
	}
	return NULL;
}

static struct map *map_new_hash(struct map *map, uint32_t key_hash) {

	struct map *previous = map;
	for (map = map->next; map; previous = map, map = map->next);

	map = (struct map *)malloc(sizeof(struct map));
	memset(map, 0, sizeof(struct map));

	map->key_hash = key_hash;
	previous->next = map;

	return map;
}

static void map_destroy(struct map *map) {

	struct map *orig = map;

	map = map->next;
	while (map) {

		struct map *next = map->next;
		free(map);
		map = next;
	}
	orig->next = NULL;
}

/* common hash maps */
static struct map global_map;
static struct map struct_map;
static struct map string_map;
static size_t string_count = 0;

#define LOCAL_MAP_MAX 64
static struct map local_maps[LOCAL_MAP_MAX];
static size_t local_map_count = 0;

static struct map *push_scope(void) {

	if (local_map_count >= LOCAL_MAP_MAX) {

		print_error(
			NULL,
			"Reached scope limit (%zu)",
			local_map_count
		);
		return NULL;
	}
	return local_maps + local_map_count++;
}

static void pop_scope(void) {

	if (!local_map_count) return;

	size_t i = --local_map_count;

	map_destroy(local_maps+i);
	memset(local_maps+i, 0, sizeof(struct map));
}

static struct map *get_global(const char *key) {

	return map_get(&global_map, key);
}

static struct map *slot_global(const char *key) {

	return map_slot(&global_map, key);
}

static struct map *get_struct(const char *key) {

	return map_get(&struct_map, key);
}

static struct map *slot_struct(const char *key) {

	return map_slot(&struct_map, key);
}

static struct map *get_local(const char *key) {

	for (size_t i = local_map_count; i; i--) {

		struct map *map = map_get(local_maps+i-1, key);
		if (map) return map;
	}
	return NULL;
}

static struct map *slot_local(const char *key) {

	return map_slot(local_maps+local_map_count-1, key);
}

/* analysis result */
#define TYPE_NONE 0

#define TYPE_I8 1
#define TYPE_U8 2
#define TYPE_I16 3
#define TYPE_U16 4
#define TYPE_I32 5
#define TYPE_U32 6
#define TYPE_I64 7
#define TYPE_U64 8
#define TYPE_F32 9
#define TYPE_F64 10

#define TYPE_STRUCT 11
#define TYPE_BYTES 12

#define TYPE_PTR TYPE_U64

#define FLAG_CONSTANT 0x1

struct result {
	uint8_t type;
	uint8_t flags;
	uint16_t iota;
	uintptr_t data;
} __attribute__((packed));

#define RESULT(p_type, p_iota, ...) ((struct result){\
		.type = (p_type),\
		.iota = (p_iota),\
		__VA_ARGS__\
	})

#define RESULT_FAIL RESULT(0, 0)

#define RESULT_OKAY(p_result) ((p_result).type != 0)

static size_t result_get_type_size(struct result *result) {

	struct map *map;
	switch (result->type) {
		case TYPE_I8:
		case TYPE_U8:
			return 1;
		case TYPE_I16:
		case TYPE_U16:
			return 2;
		case TYPE_I32:
		case TYPE_U32:
		case TYPE_F32:
			return 4;
		case TYPE_I64:
		case TYPE_U64:
		case TYPE_F64:
			return 8;
		case TYPE_STRUCT:
			map = (struct map *)result->data;
			return map->values[1];
		case TYPE_BYTES:
			return (size_t)result->data;
		default:
			return 0;
	}
}

/* lexer */
struct lexer {
	struct position position;
	struct token *first, *last;
};
#define LEXER_INIT(p_fp, p_path) {\
		.position = POSITION_INIT((p_fp), (p_path)),\
		.first = NULL, .last = NULL,\
	}

static void lexer_advance(struct lexer *lexer) {

	position_advance(&lexer->position);
}

static void lexer_add_token(
		struct lexer *lexer,
		enum token_type type,
		struct position *position,
		const char *value,
		size_t value_size
) {
	size_t size = sizeof(struct token) + value_size + 1;
	struct token *token = (struct token *)malloc(size);
	memset(token, 0, size);

	token->type = type;
	token->position = *position;
	token->value_size = value_size;
	memcpy(token->value, value, value_size);

	if (!lexer->first) lexer->first = token;
	if (lexer->last) lexer->last->next = token;
	lexer->last = token;
}

/* generate token stream */
static const char *identifier_chars =
		"abcdefghijklmnopqrstuvwxyz"
		"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
		"0123456789_";

static int lexer_make_tokens(struct lexer *lexer) {

	lexer_advance(lexer);
	while (lexer->position.cc) {

		/* comment */
		if (lexer->position.cc == '#') {

			while (!strchr("\n", lexer->position.cc))
				lexer_advance(lexer);
		}

		/* whitespace */
		else if (strchr(" \r\t", lexer->position.cc))
			lexer_advance(lexer);

		/* newline */
		else if (strchr(";\n", lexer->position.cc)) {

			char cc = (char)lexer->position.cc;
			lexer_add_token(
				lexer,
				TOKEN_TYPE_EOL,
				&lexer->position,
				&cc, 1
			);
			lexer_advance(lexer);
		}

		/* ":", "::", "<", "<<", ">" and ">>" */
		else if (strchr(":<>", lexer->position.cc)) {

			struct position position = lexer->position;
			lexer_advance(lexer);

			char value[2] = {position.cc};
			size_t size = 1;

			if (lexer->position.cc == position.cc) {

				value[size++] = position.cc;
				lexer_advance(lexer);
			}
			lexer_add_token(
				lexer,
				TOKEN_TYPE_SYMBOL,
				&position,
				value, size
			);
		}

		/* "=", "==", "!" and "!=" */
		else if (strchr("!=", lexer->position.cc)) {

			struct position position = lexer->position;
			lexer_advance(lexer);

			char value[2] = {position.cc};
			size_t size = 1;

			if (lexer->position.cc == '=') {

				value[size++] = '=';
				lexer_advance(lexer);
			}
			lexer_add_token(
				lexer,
				TOKEN_TYPE_SYMBOL,
				&position,
				value, size
			);
		}

		/* "{" and "{}" */
		else if (lexer->position.cc == '{') {

			struct position position = lexer->position;
			lexer_advance(lexer);
			size_t size = 1;

			if (lexer->position.cc == '}') {

				lexer_advance(lexer);
				size++;
			}
			lexer_add_token(
				lexer,
				TOKEN_TYPE_SYMBOL,
				&position,
				"{}", size
			);
		}

		/* single character symbols */
		else if (strchr("()[],+-*/|&^~.}", lexer->position.cc)) {

			char cc = (char)lexer->position.cc;
			lexer_add_token(
				lexer,
				TOKEN_TYPE_SYMBOL,
				&lexer->position,
				&cc, 1
			);
			lexer_advance(lexer);
		}

		/* number */
		else if (strchr("0123456789", lexer->position.cc)) {

			struct position position = lexer->position;

			char value[64];
			size_t size = 0;

			while (lexer->position.cc &&
			       strchr("0123456789", lexer->position.cc)) {

				if (size < sizeof(value)-1)
					value[size++] = (char)lexer->position.cc;
				lexer_advance(lexer);
			}
			value[size] = 0;
			lexer_add_token(
				lexer,
				TOKEN_TYPE_NUMBER,
				&position,
				value, size
			);
		}

		/* keyword */
		else if (strchr(identifier_chars, lexer->position.cc)) {

			struct position position = lexer->position;
			enum token_type type = TOKEN_TYPE_KEYWORD;

			char value[64];
			size_t size = 0;

			while (lexer->position.cc &&
			       strchr(identifier_chars, lexer->position.cc)) {

				if (size < sizeof(value)-1)
					value[size++] = (char)lexer->position.cc;
				lexer_advance(lexer);
			}
			value[size] = 0;
			const char **p_keywords = keywords;
			for (; *p_keywords; p_keywords++) {

				if (!strcmp(*p_keywords, value))
					break;
			}
			if (!*p_keywords) {

				p_keywords = builtin_types;
				for (; *p_keywords; p_keywords++) {

					if (!strcmp(*p_keywords, value))
						break;
				}
				type = TOKEN_TYPE_BUILTIN_TYPE;
			}
			if (!*p_keywords) {

				print_error(
					&lexer->position,
					"Unrecognized keyword '%s'",
					value
				);
				return -1;
			}
			lexer_add_token(
				lexer,
				type,
				&position,
				value, size
			);
		}

		/* string literal */
		else if (lexer->position.cc == '"') {

			struct position position = lexer->position;
			lexer_advance(lexer);

			char value[1024];
			size_t size = 0;

			while (!strchr("\"", lexer->position.cc)) {

				char cc = lexer->position.cc;
				lexer_advance(lexer);

				if (cc == '\\' && lexer->position.cc) {

					cc = lexer->position.cc;
					lexer_advance(lexer);

					switch (cc) {
						case 'n': cc = '\n'; break;
						case 'r': cc = '\r'; break;
						case 't': cc = '\t'; break;
						case 'e': cc = 0x1b; break;
					}
				}
				if (size < sizeof(value)-1)
					value[size++] = cc;
			}
			value[size] = 0;
			if (lexer->position.cc != '"') {

				print_error(
					&lexer->position,
					"Unterminated string literal"
				);
				return -1;
			}
			lexer_advance(lexer);
			lexer_add_token(
				lexer,
				TOKEN_TYPE_STRING,
				&position,
				value, size
			);
		}

		/* identifier */
		else if (strchr("@'$%", lexer->position.cc)) {

			struct position position = lexer->position;
			lexer_advance(lexer);

			enum token_type type = TOKEN_TYPE_TYPE;
			switch (position.cc) {
				case '\'': type = TOKEN_TYPE_FIELD; break;
				case '$': type = TOKEN_TYPE_GLOBAL; break;
				case '%': type = TOKEN_TYPE_LOCAL; break;
			}

			char value[256];
			size_t size = 0;

			while (lexer->position.cc &&
			       strchr(identifier_chars, lexer->position.cc)) {

				if (size < sizeof(value)-1)
					value[size++] = (char)lexer->position.cc;
				lexer_advance(lexer);
			}
			value[size] = 0;

			if (!size && position.cc == '%') {

				type = TOKEN_TYPE_SYMBOL;
				value[size++] = '%';
			}

			lexer_add_token(
				lexer,
				type,
				&position,
				value, size
			);
		}

		/* unrecognized character */
		else {
			print_error(
				&lexer->position,
				"Unrecognized character '%c'",
				lexer->position.cc
			);
			return -1;
		}
	}

	/* end of file */
	lexer_add_token(
		lexer,
		TOKEN_TYPE_EOF,
		&lexer->position,
		"EOF", 3
	);
	return 0;
}

/* destroy lexer and token stream */
static void lexer_destroy(struct lexer *lexer) {

	struct token *token = lexer->first;
	while (token) {

		struct token *next = token->next;
		free(token);
		token = next;
	}
}

/* parser */
struct parser {
	struct token *token;
	struct node *node;
};
#define PARSER_INIT(p_token) {\
		.token = (p_token),\
	}

static void parser_advance(struct parser *parser) {

	if (!parser->token || parser->token->type == TOKEN_TYPE_EOF)
		return;
	parser->token = parser->token->next;
}

/* destroy parser */
static void parser_destroy(struct parser *parser) {

	if (parser->node) node_destroy(parser->node);
}

static struct node *parser_expr(struct parser *parser);
static struct node *parser_statement(struct parser *parser);
static struct node *parser_block(struct parser *parser);

/* parse type */
static struct node *parser_type(
		struct parser *parser,
		enum token_type type,
		bool allow_bytes
) {
	if (type == TOKEN_TYPE_COUNT) {

		if (parser->token->type == TOKEN_TYPE_BUILTIN_TYPE)
			type = TOKEN_TYPE_BUILTIN_TYPE;
		else
			type = TOKEN_TYPE_TYPE;
	}

	/* allocate array of bytes */
	if (allow_bytes && token_match_symbol(parser->token, "[")) {

		struct node *node = node_create(
			NODE_TYPE_TYPE,
			&parser->token->position
		);
		parser_advance(parser);

		struct node *expr_node = parser_expr(parser);
		if (!expr_node) return NULL;

		node_add_child(node, expr_node);

		if (!token_match_symbol(parser->token, "]")) {

			print_error(
				&parser->token->position,
				"Expected ']'"
			);
			return NULL;
		}
		parser_advance(parser);
		return node;
	}

	/* type specifier */
	else if (parser->token->type != type) {

		print_error(
			&parser->token->position,
			"Expected type"
		);
		return NULL;
	}
	struct node *node = node_create(
		NODE_TYPE_TYPE,
		&parser->token->position
	);
	node->tokens[0] = parser->token;
	parser_advance(parser);

	return node;
}

/* parse factor */
static struct node *parser_factor(struct parser *parser) {

	struct token *token = parser->token;

	/* number */
	if (token->type == TOKEN_TYPE_NUMBER) {

		struct node *node = node_create(
			NODE_TYPE_NUMBER,
			&token->position
		);
		node->tokens[0] = token;
		parser_advance(parser);

		if (parser->token->type != TOKEN_TYPE_BUILTIN_TYPE)
			return node;

		struct node *type_node = parser_type(parser, TOKEN_TYPE_BUILTIN_TYPE, false);
		if (!type_node) return NULL;

		node_add_child(node, type_node);
		return node;
	}

	/* string */
	else if (token->type == TOKEN_TYPE_STRING) {

		struct node *node = node_create(
			NODE_TYPE_STRING,
			&token->position
		);
		node->tokens[0] = token;
		parser_advance(parser);

		struct node *type_node = parser_type(parser, TOKEN_TYPE_BUILTIN_TYPE, false);
		if (!type_node) return NULL;

		node_add_child(node, type_node);
		return node;
	}

	/* global */
	else if (token->type == TOKEN_TYPE_GLOBAL) {

		struct node *node = node_create(
			NODE_TYPE_GLOBAL,
			&token->position
		);
		node->tokens[0] = token;
		parser_advance(parser);

		return node;
	}

	/* local */
	else if (token->type == TOKEN_TYPE_LOCAL) {

		struct node *node = node_create(
			NODE_TYPE_LOCAL,
			&token->position
		);
		node->tokens[0] = token;
		parser_advance(parser);

		return node;
	}

	/* cast */
	else if (token->type == TOKEN_TYPE_BUILTIN_TYPE) {

		struct node *node = node_create(
			NODE_TYPE_CAST,
			&token->position
		);
		struct node *type_node = parser_type(parser, TOKEN_TYPE_BUILTIN_TYPE, false);
		if (!type_node) return NULL;

		node_add_child(node, type_node);

		struct node *factor_node = parser_factor(parser);
		if (!factor_node) return NULL;

		node_add_child(node, factor_node);
		return node;
	}

	/* struct member */
	else if (token->type == TOKEN_TYPE_TYPE) {

		struct node *node = node_create(
			NODE_TYPE_MEMBER,
			&token->position
		);
		struct node *type_node = parser_type(parser, TOKEN_TYPE_TYPE, false);
		if (!type_node) return NULL;

		node_add_child(node, type_node);

		if (!token_match_symbol(parser->token, ".")) {

			print_error(
				&parser->token->position,
				"Expected '.'"
			);
			return NULL;
		}
		parser_advance(parser);

		if (parser->token->type != TOKEN_TYPE_FIELD) {

			print_error(
				&parser->token->position,
				"Expected field name"
			);
			return NULL;
		}
		node->tokens[0] = parser->token;
		parser_advance(parser);

		return node;
	}

	/* grouping */
	else if (token_match_symbol(token, "(")) {

		parser_advance(parser);

		struct node *node = parser_expr(parser);
		if (!node) return NULL;

		if (!token_match_symbol(parser->token, ")")) {

			print_error(
				&parser->token->position,
				"Expected ')'"
			);
			return NULL;
		}
		parser_advance(parser);
		return node;
	}

	/* dereference */
	else if (token_match_symbol(token, "*")) {

		struct node *node = node_create(
			NODE_TYPE_DEREFERENCE,
			&token->position
		);
		parser_advance(parser);

		struct node *type_node = parser_type(parser, TOKEN_TYPE_BUILTIN_TYPE, false);
		if (!type_node) return NULL;

		node_add_child(node, type_node);

		struct node *factor_node = parser_factor(parser);
		if (!factor_node) return NULL;

		node_add_child(node, factor_node);
		return node;
	}

	/* reference */
	else if (token_match_symbol(token, "&")) {

		struct node *node = node_create(
			NODE_TYPE_REFERENCE,
			&token->position
		);
		parser_advance(parser);

		struct node *factor_node = parser_factor(parser);
		if (!factor_node) return NULL;

		node_add_child(node, factor_node);
		return node;
	}

	/* unary operation */
	else if (token_match_symbol(token, "!") ||
		 token_match_symbol(token, "~") ||
		 token_match_symbol(token, "-")) {

		struct node *node = node_create(
			NODE_TYPE_UNOP,
			&token->position
		);
		node->tokens[0] = token;
		parser_advance(parser);

		struct node *factor_node = parser_factor(parser);
		if (!factor_node) return NULL;

		node_add_child(node, factor_node);
		return node;
	}

	/* block */
	else if (token_match_symbol(token, "{"))
		return parser_block(parser);

	/* error */
	print_error(
		&token->position,
		"Unexpected token '%s'",
		token->value
	);
	return NULL;
}

/* parse call */
static struct node *parser_call(struct parser *parser) {

	struct node *factor_node = parser_factor(parser);
	if (!token_match_symbol(parser->token, "("))
		return factor_node;

	struct node *node = node_create(
		NODE_TYPE_CALL,
		&parser->token->position
	);
	node_add_child(node, factor_node);
	parser_advance(parser);

	/* arguments */
	if (!token_match_symbol(parser->token, ")")) {

		struct node *expr_node = parser_expr(parser);
		if (!expr_node) return NULL;

		node_add_child(node, expr_node);
		node->values[0]++;

		while (token_match_symbol(parser->token, ",")) {

			parser_advance(parser);

			expr_node = parser_expr(parser);
			if (!expr_node) return NULL;

			node_add_child(node, expr_node);
			node->values[0]++;
		}
	}
	if (!token_match_symbol(parser->token, ")")) {

		print_error(
			&parser->token->position,
			"Expected ')'"
		);
		return NULL;
	}
	parser_advance(parser);

	/* return type */
	struct node *return_type_node = parser_type(parser, TOKEN_TYPE_BUILTIN_TYPE, false);
	if (!return_type_node) return NULL;

	node_add_child(node, return_type_node);
	return node;
}

/* parse expression */
static struct node *parser_expr(struct parser *parser) {

	struct node *call_node = parser_call(parser);
	if (!call_node) return NULL;

	static const char *expr_tokens[] = {
		"+", "-", "*", "/", "%",
		"|", "&", "^", "<<", ">>",
		"==", "!=", "<", ">", "=",
		NULL
	};
	const char **tokens = expr_tokens;
	bool match = false;
	for (; *tokens; tokens++) {

		if (!token_match_symbol(parser->token, *tokens))
			continue;
		match = true;
		break;
	}
	if (!match &&
	    token_match_keyword(parser->token, "and") ||
	    token_match_keyword(parser->token, "or"))
		match = true;

	/* binary operation */
	if (match) {

		struct token *token = parser->token;
		parser_advance(parser);

		struct node *expr_node = parser_expr(parser);
		if (!expr_node) return NULL;

		struct node *node = node_create(
			NODE_TYPE_BINOP,
			&parser->token->position
		);
		node->tokens[0] = token;
		node_add_child(node, call_node);
		node_add_child(node, expr_node);

		call_node = node;
	}
	return call_node;
}

/* parse var statement */
static struct node *parser_var_statement(
		struct parser *parser,
		struct token *name_token
) {
	struct node *node = node_create(
		NODE_TYPE_VARIABLE,
		&parser->token->position
	);
	node->tokens[0] = name_token;

	/* type */
	struct node *type_node = parser_type(parser, TOKEN_TYPE_COUNT, true);
	if (!type_node) return NULL;

	node_add_child(node, type_node);

	/* value */
	if (token_match_symbol(parser->token, "{}")) {

		parser_advance(parser);

		node->values[1] = 1;
		return node;
	}
	struct node *expr_node = parser_expr(parser);
	if (!expr_node) return NULL;

	node_add_child(node, expr_node);
	return node;
}

/* parse if statement */
static struct node *parser_if_statement(struct parser *parser) {

	struct node *node = node_create(
		NODE_TYPE_IF,
		&parser->token->position
	);
	parser_advance(parser);

	if (!token_match_symbol(parser->token, "(")) {

		print_error(
			&parser->token->position,
			"Expected '('"
		);
		return NULL;
	}
	parser_advance(parser);

	/* condition */
	struct node *condition_node = parser_expr(parser);
	if (!condition_node) return NULL;

	node_add_child(node, condition_node);

	if (!token_match_symbol(parser->token, ")")) {

		print_error(
			&parser->token->position,
			"Expected ')'"
		);
		return NULL;
	}
	parser_advance(parser);

	while (parser->token->type == TOKEN_TYPE_EOL)
		parser_advance(parser);

	/* body */
	struct node *body_node = parser_statement(parser);
	if (!body_node) return NULL;

	node_add_child(node, body_node);

	while (parser->token->type == TOKEN_TYPE_EOL &&
	       parser->token->next->type == TOKEN_TYPE_EOL)
		parser_advance(parser);

	/* else clause */
	if (token_match_keyword(parser->token, "else") ||
	    (parser->token->type == TOKEN_TYPE_EOL &&
	     token_match_keyword(parser->token->next, "else"))) {

		if (parser->token->type == TOKEN_TYPE_EOL)
			parser_advance(parser);
		parser_advance(parser);

		while (parser->token->type == TOKEN_TYPE_EOL)
			parser_advance(parser);

		struct node *else_node = parser_statement(parser);
		if (!else_node) return NULL;

		node_add_child(node, else_node);
	}
	return node;
}

/* parse for statement */
static struct node *parser_for_statement(struct parser *parser) {

	struct node *node = node_create(
		NODE_TYPE_FOR,
		&parser->token->position
	);
	parser_advance(parser);

	if (!token_match_symbol(parser->token, "(")) {

		print_error(
			&parser->token->position,
			"Expected '('"
		);
		return NULL;
	}
	parser_advance(parser);

	/* initializer */
	if (parser->token->type != TOKEN_TYPE_EOL) {

		struct node *initializer_node = parser_statement(parser);
		if (!initializer_node) return NULL;

		node_add_child(node, initializer_node);
		node->values[0] = 1;
	}
	if (parser->token->type != TOKEN_TYPE_EOL) {

		print_error(
			&parser->token->position,
			"Expected ';' or newline"
		);
		return NULL;
	}
	if (parser->token->type == TOKEN_TYPE_EOL)
		parser_advance(parser);

	/* condition */
	if (parser->token->type != TOKEN_TYPE_EOL) {

		struct node *condition_node = parser_expr(parser);
		if (!condition_node) return NULL;

		node_add_child(node, condition_node);
		node->values[1] = 1;
	}
	if (parser->token->type != TOKEN_TYPE_EOL) {

		print_error(
			&parser->token->position,
			"Expected ';' or newline"
		);
		return NULL;
	}
	if (parser->token->type == TOKEN_TYPE_EOL)
		parser_advance(parser);

	/* increment */
	if (!token_match_symbol(parser->token, ")")) {

		struct node *increment_node = parser_statement(parser);
		if (!increment_node) return NULL;

		node_add_child(node, increment_node);
		node->values[2] = 1;
	}
	if (!token_match_symbol(parser->token, ")")) {

		print_error(
			&parser->token->position,
			"Expected ')'"
		);
		return NULL;
	}
	parser_advance(parser);

	while (parser->token->type == TOKEN_TYPE_EOL)
		parser_advance(parser);

	/* body */
	struct node *body_node = parser_statement(parser);
	if (!body_node) return NULL;

	node_add_child(node, body_node);
	return node;
}

/* parse code statement */
static struct node *parser_statement(struct parser *parser) {

	struct token *token = parser->token;

	/* variable definition */
	if (token_match_keyword(token, "var")) {

		parser_advance(parser);
		if (parser->token->type != TOKEN_TYPE_LOCAL) {

			print_error(
				&parser->token->position,
				"Expected variable name"
			);
			return NULL;
		}
		struct token *name_token = parser->token;
		parser_advance(parser);

		if (!token_match_symbol(parser->token, "=")) {

			print_error(
				&parser->token->position,
				"Expected '='"
			);
			return NULL;
		}
		parser_advance(parser);

		return parser_var_statement(parser, name_token);
	}

	/* if statement */
	else if (token_match_keyword(token, "if"))
		return parser_if_statement(parser);

	/* for statement */
	else if (token_match_keyword(token, "for"))
		return parser_for_statement(parser);

	/* continue statement */
	else if (token_match_keyword(token, "continue")) {

		struct node *node = node_create(
			NODE_TYPE_CONTINUE,
			&parser->token->position
		);
		parser_advance(parser);
		return node;
	}

	/* break statement */
	else if (token_match_keyword(token, "break")) {

		struct node *node = node_create(
			NODE_TYPE_BREAK,
			&parser->token->position
		);
		parser_advance(parser);
		return node;
	}

	/* return statement */
	else if (token_match_keyword(token, "ret")) {

		struct node *node = node_create(
			NODE_TYPE_RETURN,
			&parser->token->position
		);
		parser_advance(parser);

		struct node *expr_node = parser_expr(parser);
		if (!expr_node) return NULL;

		node_add_child(node, expr_node);
		return node;
	}

	/* expression */
	return parser_expr(parser);
}

/* parse block of code statements */
static struct node *parser_block(struct parser *parser) {

	if (!token_match_symbol(parser->token, "{")) {

		print_error(
			&parser->token->position,
			"Expected '}'"
		);
		return NULL;
	}
	struct node *node = node_create(
		NODE_TYPE_BLOCK,
		&parser->token->position
	);
	parser_advance(parser);

	if (parser->token->type != TOKEN_TYPE_EOL) {

		print_error(
			&parser->token->position,
			"Expected ';' or newline"
		);
		return NULL;
	}
	while (parser->token->type == TOKEN_TYPE_EOL)
		parser_advance(parser);

	while (!token_match_symbol(parser->token, "}")) {

		struct node *statement_node = parser_statement(parser);
		if (!statement_node) return NULL;

		node_add_child(node, statement_node);

		if (parser->token->type != TOKEN_TYPE_EOL) {

			print_error(
				&parser->token->position,
				"Expected ';' or newline"
			);
			return NULL;
		}
		while (parser->token->type == TOKEN_TYPE_EOL)
			parser_advance(parser);
	}
	parser_advance(parser);
	return node;
}

/* parse struct statement */
static struct node *parser_struct_statement(
		struct parser *parser,
		struct token *name_token
) {
	if (!token_match_keyword(parser->token, "str")) {

		print_error(
			&parser->token->position,
			"Expected 'str'"
		);
		return NULL;
	}
	struct node *node = node_create(
		NODE_TYPE_STRUCT,
		&parser->token->position
	);
	parser_advance(parser);
	node->tokens[0] = name_token;

	if (!token_match_symbol(parser->token, "{")) {

		print_error(
			&parser->token->position,
			"Expected '{'"
		);
		return NULL;
	}
	parser_advance(parser);

	if (parser->token->type != TOKEN_TYPE_EOL) {

		print_error(
			&parser->token->position,
			"Expected ';' or newline"
		);
		return NULL;
	}
	while (parser->token->type == TOKEN_TYPE_EOL)
		parser_advance(parser);

	/* parse fields */
	while (!token_match_symbol(parser->token, "}")) {

		if (parser->token->type != TOKEN_TYPE_FIELD) {

			print_error(
				&parser->token->position,
				"Expected field name or '}'"
			);
			return NULL;
		}
		struct token *field_name_token = parser->token;
		parser_advance(parser);

		struct node *type_node = parser_type(parser, TOKEN_TYPE_COUNT, true);
		if (!type_node) return NULL;

		struct node *field_node = node_create(
			NODE_TYPE_FIELD,
			&field_name_token->position
		);
		field_node->tokens[0] = field_name_token;
		node_add_child(field_node, type_node);

		/* end of line */
		if (parser->token->type != TOKEN_TYPE_EOL) {

			print_error(
				&parser->token->position,
				"Expected ';' or newline"
			);
			return NULL;
		}
		while (parser->token->type == TOKEN_TYPE_EOL)
			parser_advance(parser);

		node_add_child(node, field_node);
	}
	parser_advance(parser);
	return node;
}

/* parse proc parameter */
static struct node *parser_proc_parameter(
		struct parser *parser
) {
	if (parser->token->type != TOKEN_TYPE_LOCAL) {

		print_error(
			&parser->token->position,
			"Expected parameter name"
		);
		return NULL;
	}
	struct token *name_token = parser->token;
	parser_advance(parser);

	struct node *type_node = parser_type(parser, TOKEN_TYPE_BUILTIN_TYPE, false);
	if (!type_node) return NULL;

	struct node *node = node_create(
		NODE_TYPE_PARAMETER,
		&name_token->position
	);
	node->tokens[0] = name_token;
	node_add_child(node, type_node);

	return node;
}

/* parse proc statement */
static struct node *parser_proc_statement(
		struct parser *parser,
		struct token *name_token
) {
	struct node *node = node_create(
		NODE_TYPE_PROCEDURE,
		&parser->token->position
	);
	node->tokens[0] = name_token;
	parser_advance(parser);

	if (!token_match_symbol(parser->token, "(")) {

		print_error(
			&parser->token->position,
			"Expected '('"
		);
		return NULL;
	}

	/* parameters */
	parser_advance(parser);
	if (!token_match_symbol(parser->token, ")")) {

		struct node *parameter_node = parser_proc_parameter(parser);
		if (!parameter_node) return NULL;

		node_add_child(node, parameter_node);
		node->values[0]++;

		while (token_match_symbol(parser->token, ",")) {

			parser_advance(parser);

			parameter_node = parser_proc_parameter(parser);
			if (!parameter_node) return NULL;

			node_add_child(node, parameter_node);
			node->values[0]++;
		}
	}
	if (!token_match_symbol(parser->token, ")")) {

		print_error(
			&parser->token->position,
			"Expected ')'"
		);
		return NULL;
	}
	parser_advance(parser);

	/* return type */
	struct node *return_type_node = parser_type(parser, TOKEN_TYPE_BUILTIN_TYPE, false);
	if (!return_type_node) return NULL;

	node_add_child(node, return_type_node);

	/* procedure body */
	if (!token_match_symbol(parser->token, "{"))
		return node;

	struct node *body_node = parser_block(parser);
	if (!body_node) return NULL;

	node_add_child(node, body_node);
	node->values[1] = 1;

	return node;
}

/* parse toplevel file statement */
static struct node *parser_file_statement(struct parser *parser) {

	struct token *token = parser->token;
	parser_advance(parser);

	/* struct definition */
	if (token->type == TOKEN_TYPE_TYPE) {

		if (!token_match_symbol(parser->token, "::")) {

			print_error(
				&parser->token->position,
				"Expected '::'"
			);
			return NULL;
		}
		parser_advance(parser);

		return parser_struct_statement(parser, token);
	}

	/* procedure declaration or variable definition */
	else if (token->type == TOKEN_TYPE_GLOBAL) {

		if (!token_match_symbol(parser->token, "::")) {

			print_error(
				&parser->token->position,
				"Expected '::'"
			);
			return NULL;
		}
		parser_advance(parser);

		if (token_match_keyword(parser->token, "proc"))
			return parser_proc_statement(parser, token);

		else if (parser->token->type == TOKEN_TYPE_BUILTIN_TYPE ||
			 parser->token->type == TOKEN_TYPE_TYPE ||
			 token_match_symbol(parser->token, "["))
			return parser_var_statement(parser, token);

		print_error(
			&parser->token->position,
			"Expected 'proc' or variable"
		);
		return NULL;
	}

	/* error */
	print_error(
		&parser->token->position,
		"Unexpected token '%s'",
		parser->token->value
	);
	return NULL;
}

/* parse tokens */
static struct node *parser_parse(struct parser *parser) {

	parser->node = node_create(
		NODE_TYPE_BLOCK,
		&parser->token->position
	);
	while (parser->token->type != TOKEN_TYPE_EOF) {

		struct node *node = parser_file_statement(parser);
		if (!node) return NULL;

		if (parser->token->type != TOKEN_TYPE_EOL) {

			print_error(
				&parser->token->position,
				"Expected ';' or newline"
			);
			return NULL;
		}
		while (parser->token->type == TOKEN_TYPE_EOL)
			parser_advance(parser);
		node_add_child(parser->node, node);
	}
	return parser->node;
}

/* print node */
static size_t print_node_level = 0;

static void print_node(struct node *node) {

	struct token **tokens = node->tokens;
	intmax_t *values = node->values;

	switch (node->type) {

		/* block */
		case NODE_TYPE_BLOCK:
			fputs("{\n", stdout);

			print_node_level++;

			node = node->first;
			for (; node; node = node->next) {

				for (size_t i = 0; i < print_node_level; i++)
					fputc('\t', stdout);
				print_node(node);
				fputc('\n', stdout);
			}
			print_node_level--;
			for (size_t i = 0; i < print_node_level; i++)
				fputc('\t', stdout);
			fputc('}', stdout);
			break;

		/* type */
		case NODE_TYPE_TYPE:
			if (node->first) {

				fputs("[", stdout);
				print_node(node->first);
				fputs("]", stdout);
			}
			else {
				if (tokens[0]->type == TOKEN_TYPE_TYPE)
					fputc('@', stdout);
				fputs(tokens[0]->value, stdout);
			}
			break;

		/* struct */
		case NODE_TYPE_STRUCT:
			printf("@%s :: str {\n", tokens[0]->value);

			print_node_level++;

			node = node->first;
			for (; node; node = node->next) {

				for (size_t i = 0; i < print_node_level; i++)
					fputc('\t', stdout);
				print_node(node);
				fputc('\n', stdout);
			}
			print_node_level--;
			for (size_t i = 0; i < print_node_level; i++)
				fputc('\t', stdout);
			fputc('}', stdout);
			break;

		/* field */
		case NODE_TYPE_FIELD:
			printf("'%s ", tokens[0]->value);
			print_node(node->first);
			break;

		/* procedure */
		case NODE_TYPE_PROCEDURE:
			printf("$%s :: proc(", tokens[0]->value);

			node = node->first;
			for (size_t i = 0; i < values[0]; i++, node = node->next) {

				if (i) fputs(", ", stdout);
				print_node(node);
			}
			fputs(") ", stdout);
			print_node(node);
			node = node->next;

			if (node) {
				fputc(' ', stdout);
				print_node(node);
			}
			break;

		/* parameter */
		case NODE_TYPE_PARAMETER:
			printf("%%%s ", tokens[0]->value);
			print_node(node->first);
			break;

		/* variable */
		case NODE_TYPE_VARIABLE:
			if (print_node_level > 1)
				printf("var %%%s = ", tokens[0]->value);
			else
				printf("$%s :: ", tokens[0]->value);
			node = node->first;

			print_node(node);
			node = node->next;

			if (values[1])
				fputs("{}", stdout);
			else
				print_node(node);
			break;

		/* if statement */
		case NODE_TYPE_IF:
			fputs("if (", stdout);
			node = node->first;
			print_node(node);
			node = node->next;
			fputs(") ", stdout);
			print_node(node);
			node = node->next;
			if (node) {
				fputc('\n', stdout);
				for (size_t i = 0; i < print_node_level; i++)
					fputc('\t', stdout);
				fputs("else ", stdout);
				print_node(node);
			}
			break;

		/* for statement */
		case NODE_TYPE_FOR:
			fputs("for (", stdout);
			node = node->first;
			if (values[0]) {
				print_node(node);
				node = node->next;
			}
			fputs(";", stdout);
			if (values[1]) {
				fputc(' ', stdout);
				print_node(node);
				node = node->next;
			}
			fputs(";", stdout);
			if (values[2]) {
				fputc(' ', stdout);
				print_node(node);
				node = node->next;
			}
			fputs(") ", stdout);
			print_node(node);
			break;

		/* continue */
		case NODE_TYPE_CONTINUE:
			fputs("continue", stdout);
			break;

		/* break */
		case NODE_TYPE_BREAK:
			fputs("break", stdout);
			break;

		/* return */
		case NODE_TYPE_RETURN:
			fputs("ret ", stdout);
			print_node(node->first);
			break;

		/* binary operation */
		case NODE_TYPE_BINOP:
			fputc('(', stdout);
			node = node->first;
			print_node(node);
			node = node->next;
			printf(" %s ", tokens[0]->value);
			print_node(node);
			fputc(')', stdout);
			break;

		/* unary operation */
		case NODE_TYPE_UNOP:
			fputc('(', stdout);
			printf("%s ", tokens[0]->value);
			print_node(node->first);
			fputc(')', stdout);
			break;

		/* function call */
		case NODE_TYPE_CALL:
			node = node->first;
			print_node(node);
			node = node->next;
			fputc('(', stdout);
			for (size_t i = 0; i < values[0]; i++, node = node->next) {

				print_node(node);
				if (node->next) fputs(", ", stdout);
			}
			fputc(')', stdout);
			print_node(node);
			break;

		/* number */
		case NODE_TYPE_NUMBER:
			printf("%s", tokens[0]->value);
			if (node->first) print_node(node->first);
			break;

		/* string */
		case NODE_TYPE_STRING:
			printf("\"%s\"", tokens[0]->value);
			print_node(node->first);
			break;

		/* global */
		case NODE_TYPE_GLOBAL:
			printf("$%s", tokens[0]->value);
			break;

		/* local */
		case NODE_TYPE_LOCAL:
			printf("%%%s", tokens[0]->value);
			break;

		/* cast */
		case NODE_TYPE_CAST:
			fputc('(', stdout);
			node = node->first;
			print_node(node);
			node = node->next;
			fputc(' ', stdout);
			print_node(node);
			fputc(')', stdout);
			break;

		/* member access */
		case NODE_TYPE_MEMBER:
			print_node(node->first);
			printf(".'%s", tokens[0]->value);
			break;

		/* dereference */
		case NODE_TYPE_DEREFERENCE:
			fputc('*', stdout);
			node = node->first;
			print_node(node);
			node = node->next;
			fputc(' ', stdout);
			print_node(node);
			break;

		/* reference */
		case NODE_TYPE_REFERENCE:
			fputc('&', stdout);
			print_node(node->first);
			break;

		/* unknown */
		default:
			printf("(Unknown node)");
			break;
	}
	if (!print_node_level)
		fputc('\n', stdout);
}

/* convert from type to qbe type */
static char get_qbe_type(uint8_t type) {

	switch (type) {
		case TYPE_F32:
			return 's';
		case TYPE_F64:
			return 'd';
		default:
			return 'l';
	}
}

static char get_qbe_type_raw(uint8_t type) {

	switch (type) {
		case TYPE_I8:
		case TYPE_U8:
			return 'b';
		case TYPE_I16:
		case TYPE_U16:
			return 'h';
		case TYPE_I32:
		case TYPE_U32:
			return 'w';
		case TYPE_I64:
		case TYPE_U64:
			return 'l';
		case TYPE_F32:
			return 's';
		case TYPE_F64:
			return 'd';
		default:
			return 'l';
	}
}

static const char *get_qbe_type_load(uint8_t type) {

	switch (type) {
		case TYPE_I8:
			return "loadsb";
		case TYPE_U8:
			return "loadub";
		case TYPE_I16:
			return "loadsh";
		case TYPE_U16:
			return "loaduh";
		case TYPE_I32:
			return "loadsw";
		case TYPE_U32:
			return "loaduw";
		case TYPE_F32:
			return "loads";
		case TYPE_F64:
			return "loadd";
		default:
			return "loadl";
	}
}

static const char *get_qbe_type_store(uint8_t type) {

	switch (type) {
		case TYPE_I8:
		case TYPE_U8:
			return "storeb";
		case TYPE_I16:
		case TYPE_U16:
			return "storeh";
		case TYPE_I32:
		case TYPE_U32:
			return "storew";
		case TYPE_F32:
			return "stores";
		case TYPE_F64:
			return "stored";
		default:
			return "storel";
	}
}

/* compiler */
struct compiler {
	FILE *fp;
	uint16_t iota;
	size_t level;
	uint8_t return_type;
	uint16_t loop_start_iota;
	uint16_t loop_end_iota;
	bool no_block_push_scope;
	struct map locals;
};
#define COMPILER_INIT(p_fp) {\
		.fp = (p_fp),\
		.iota = 1,\
		.level = 0,\
		.no_block_push_scope = true,\
	}

static struct result compiler_visit(
		struct compiler *compiler,
		struct node *node
);

/* visit block */
static struct result compiler_visit_block(
		struct compiler *compiler,
		struct node *node
) {
	struct result result = {
		.type = TYPE_U64,
		.flags = 0,
	};
	if (!compiler->no_block_push_scope) {

		if (!push_scope())
			return RESULT_FAIL;
	}
	bool no_block_push_scope = compiler->no_block_push_scope;
	compiler->no_block_push_scope = false;
	compiler->level++;

	node = node->first;
	for (; node; node = node->next) {

		struct result node_result =
			compiler_visit(compiler, node);
		if (!RESULT_OKAY(node_result))
			return RESULT_FAIL;

		if (node_result.iota) {

			result.type = node_result.type;
			result.iota = node_result.iota;
		}
	}
	if (!result.iota && compiler->level > 1) {

		result.iota = compiler->iota++;
		safe_fprintf(
			compiler->fp,
			"%%.t%d =l copy 0\n",
			(int)result.iota
		);
	}

	compiler->level--;
	if (!no_block_push_scope) pop_scope();

	return result;
}

/* visit type */
static struct result compiler_visit_type(
		struct compiler *compiler,
		struct node *node
) {
	struct result result = {
		.type = TYPE_I8,
		.iota = 0,
		.data = 0,
	};
	struct token *token = node->tokens[0];

	/* bytes */
	if (node->first) {

		result.type = TYPE_BYTES;

		struct result expr_result =
			compiler_visit(compiler, node->first);
		if (!RESULT_OKAY(expr_result))
			return RESULT_FAIL;

		if (!(expr_result.flags & FLAG_CONSTANT)) {

			print_error(
				&node->first->position,
				"Byte count must be constant at compile time"
			);
			return RESULT_FAIL;
		}
		result.data = expr_result.data;
	}

	/* builtin type identifier */
	else if (token->type == TOKEN_TYPE_BUILTIN_TYPE) {

		if (!strcmp(token->value, "i8"))
			result.type = TYPE_I8;
		else if (!strcmp(token->value, "u8"))
			result.type = TYPE_U8;
		else if (!strcmp(token->value, "i16"))
			result.type = TYPE_I16;
		else if (!strcmp(token->value, "u16"))
			result.type = TYPE_U16;
		else if (!strcmp(token->value, "i32"))
			result.type = TYPE_I32;
		else if (!strcmp(token->value, "u32"))
			result.type = TYPE_U32;
		else if (!strcmp(token->value, "i64"))
			result.type = TYPE_I64;
		else if (!strcmp(token->value, "u64"))
			result.type = TYPE_U64;
		else if (!strcmp(token->value, "f32"))
			result.type = TYPE_F32;
		else if (!strcmp(token->value, "f64"))
			result.type = TYPE_F64;
		else if (!strcmp(token->value, "ptr"))
			result.type = TYPE_PTR;
	}

	/* struct type */
	else {
		result.type = TYPE_STRUCT;
		result.data = (uintptr_t)get_struct(token->value);

		if (!result.data) {

			print_error(
				&node->position,
				"Undefined struct type '%s'",
				token->value
			);
			return RESULT_FAIL;
		}
	}
	return result;
}

/* visit struct */
static struct result compiler_visit_struct(
		struct compiler *compiler,
		struct node *node
) {
	struct token *name_token = node->tokens[0];
	struct map *map = slot_struct(name_token->value);

	if (map->values[0]) {

		print_error(
			&node->position,
			"Can't redefine struct type '%s'",
			name_token->value
		);
		return RESULT_FAIL;
	}
	map->values[0] = 1;
	map->values[1] = 0; /* total size */

	struct map *fields_map = (struct map *)malloc(sizeof(struct map));
	memset(fields_map, 0, sizeof(struct map));

	map->values[2] = (uintptr_t)fields_map;

	/* process fields */
	node = node->first;
	for (; node; node = node->next) {

		name_token = node->tokens[0];
		struct node *type_node = node->first;

		struct result type_result =
			compiler_visit(compiler, type_node);
		if (!RESULT_OKAY(type_result))
			return RESULT_FAIL;

		/* check conflicting names */
		struct map *field = map_slot(fields_map, name_token->value);
		if (field->values[0]) {

			print_error(
				&node->position,
				"Can't redefine struct field '%s'",
				name_token->value
			);
			return RESULT_FAIL;
		}

		/* determine type size */
		uintptr_t size = 0;
		struct map *struct_map;
		switch (type_result.type) {
			case TYPE_I8:
			case TYPE_U8:
				size = 1;
				break;
			case TYPE_I16:
			case TYPE_U16:
				size = 2;
				break;
			case TYPE_I32:
			case TYPE_U32:
			case TYPE_F32:
				size = 4;
				break;
			case TYPE_I64:
			case TYPE_U64:
			case TYPE_F64:
				size = 8;
				break;
			case TYPE_STRUCT:
				struct_map = (struct map *)type_result.data;
				size = struct_map->values[1];
				break;
			case TYPE_BYTES:
				size = type_result.data;
				break;
		}
		field->values[0] = map->values[1];
		field->values[1] = size;
		map->values[1] += size;
	}
	return RESULT(TYPE_I8, 0);
}

/* visit procedure */
#define PARAM_MAX 64

static struct result compiler_visit_procedure(
		struct compiler *compiler,
		struct node *node
) {
	compiler->iota = 1;

	if (!node->values[1])
		return RESULT(TYPE_I8, 0);

	struct token *name_token = node->tokens[0];
	struct map *map = slot_global(name_token->value);

	if (map->values[0]) {

		print_error(
			&node->position,
			"Can't redefine global '%s'",
			name_token->value
		);
		return RESULT_FAIL;
	}
	map->values[0] = 1;

	struct node *return_type_node = node->first;
	for (size_t i = 0; i < node->values[0]; i++)
		return_type_node = return_type_node->next;

	/* check return value */
	struct result return_type_result =
		compiler_visit(compiler, return_type_node);
	if (!RESULT_OKAY(return_type_result))
		return RESULT_FAIL;

	compiler->return_type = return_type_result.type;

	safe_fprintf(
		compiler->fp,
		"export function %c $%s(",
		get_qbe_type(return_type_result.type),
		name_token->value
	);

	/* check arguments */
	if (!push_scope())
		return RESULT_FAIL;

	size_t i = 0;
	struct node *param_node = node->first;

	struct result param_results[PARAM_MAX];
	for (; i < node->values[0]; i++, param_node = param_node->next) {

		if (i >= PARAM_MAX) {

			print_error(
				&node->position,
				"Too many parameters"
			);
			return RESULT_FAIL;
		}
		name_token = param_node->tokens[0];
		struct node *type_node = param_node->first;

		param_results[i] =
			compiler_visit(compiler, type_node);
		if (!RESULT_OKAY(param_results[i]))
			return RESULT_FAIL;

		/* setup local variable */
		struct map *local = slot_local(name_token->value);
		if (local->values[0]) {

			print_error(
				&param_node->position,
				"Can't redefine parameter '%s'",
				name_token->value
			);
			return RESULT_FAIL;
		}
		local->values[0] = 1;
		local->values[1] = (uintptr_t)param_results[i].type;
		local->values[2] = local_map_count-1; /* scope */

		/* argument */
		if (i) fputs(", ", compiler->fp);
		safe_fprintf(
			compiler->fp,
			"%c %%%s.0.data",
			get_qbe_type(param_results[i].type),
			name_token->value
		);
	}
	fputs(") {\n@start\n", compiler->fp);

	/* allocate stack space for parameters */
	param_node = node->first;
	i = 0;
	for (; i < node->values[0]; i++, param_node = param_node->next) {

		size_t size = result_get_type_size(param_results+i);
		name_token = param_node->tokens[0];

		safe_fprintf(
			compiler->fp,
			"%%%s.0 =l alloc8 %zu\n"
			"%s %%%s.0.data, %%%s.0\n",
			name_token->value, size,
			get_qbe_type_store(param_results[i].type),
			name_token->value,
			name_token->value
		);
	}

	/* dry run for local variable info */
	FILE *fp = compiler->fp;
	compiler->fp = NULL;

	compiler->no_block_push_scope = true;

	struct node *body_node = return_type_node->next;
	if (!RESULT_OKAY(compiler_visit(compiler, body_node)))
		return RESULT_FAIL;

	compiler->fp = fp;

	/* allocate stack space for locals */
	struct map *local = compiler->locals.next;
	for (; local; local = local->next) {

		const char *name = (const char *)local->values[0];

		if (local->values[1])
			safe_fprintf(
				compiler->fp,
				"%%%s.%jd.data =l alloc8 %jd\n"
				"%%%s.%jd =l alloc8 8\n"
				"storel %%%s.%jd.data, %%%s.%jd\n",
				name, local->values[2], local->values[3],
				name, local->values[2],
				name, local->values[2], name, local->values[2]
			);
		else
			safe_fprintf(
				compiler->fp,
				"%%%s.%jd =l alloc8 %jd\n",
				name, local->values[2], local->values[3]
			);
	}
	map_destroy(&compiler->locals);
	pop_scope();

	if (!push_scope())
		return RESULT_FAIL;

	/* redeclare parameters */
	i = 0;
	param_node = node->first;
	for (; i < node->values[0]; i++, param_node = param_node->next) {

		name_token = param_node->tokens[0];
		struct map *local = slot_local(name_token->value);

		local->values[0] = 1;
		local->values[1] = (uintptr_t)param_results[i].type;
		local->values[2] = local_map_count-1;
	}

	/* actually compile body */
	compiler->no_block_push_scope = true;
	if (!RESULT_OKAY(compiler_visit(compiler, body_node)))
		return RESULT_FAIL;

	map_destroy(&compiler->locals);
	fputs("@return\nret 0\n}\n", compiler->fp);

	pop_scope();
	return RESULT(TYPE_I8, 0);
}

/* visit variable definition */
static struct result compiler_visit_variable(
		struct compiler *compiler,
		struct node *node
) {
	struct token *name_token = node->tokens[0];
	struct node *type_node = node->first;
	struct node *value_node = type_node->next;

	struct result type_result =
		compiler_visit(compiler, type_node);
	if (!RESULT_OKAY(type_result))
		return RESULT_FAIL;

	struct result value_result;
	if (value_node) {

		value_result = compiler_visit(compiler, value_node);
		if (!RESULT_OKAY(value_result))
			return RESULT_FAIL;
	}

	/* global variable */
	if (name_token->type == TOKEN_TYPE_GLOBAL) {

		struct map *global = slot_global(name_token->value);
		if (global->values[0]) {

			print_error(
				&node->position,
				"Can't redeclare global '%s'",
				name_token->value
			);
			return RESULT_FAIL;
		}
		global->values[0] = 1;
		safe_fprintf(
			compiler->fp,
			"export data $%s = { ",
			name_token->value
		);
		if (value_node) {

			if (type_result.type != value_result.type) {

				print_error(
					&node->position,
					"Mismatched variable types"
				);
				return RESULT_FAIL;
			}

			/* string */
			if (value_node->type == NODE_TYPE_STRING)
				safe_fprintf(
					compiler->fp,
					"l $.str%d",
					(int)value_result.data
				);

			/* constant value */
			else {
				if (!(value_result.flags & FLAG_CONSTANT)) {

					print_error(
						&node->position,
						"Default value for global must be constant"
					);
					return RESULT_FAIL;
				}
				safe_fprintf(
					compiler->fp,
					"%c %zu",
					get_qbe_type_raw(value_result.type),
					(size_t)value_result.data
				);
			}
		}
		else {
			size_t size = result_get_type_size(&type_result);
			safe_fprintf(
				compiler->fp,
				"z %zu", size
			);
		}
		fputs(" }\n", compiler->fp);
	}

	/* local variable */
	else {
		struct map *local = slot_local(name_token->value);
		if (local->values[0]) {

			print_error(
				&node->position,
				"Can't redeclare local '%s'",
				name_token->value
			);
			return RESULT_FAIL;
		}
		local->values[0] = 1;
		local->values[1] = (uintptr_t)type_result.type;
		local->values[2] = local_map_count-1; /* scope */

		if (local->values[1] > TYPE_F64)
			local->values[1] = TYPE_PTR;

		size_t size = result_get_type_size(&type_result);

		/* value */
		if (value_node) {

			if (type_result.type != value_result.type) {

				print_error(
					&node->position,
					"Mismatched variable types"
				);
				return RESULT_FAIL;
			}
			struct map *local_info = map_new_hash(&compiler->locals, 0);

			local_info->values[0] = (uintptr_t)name_token->value;
			local_info->values[2] = local_map_count-1;
			local_info->values[3] = (uintptr_t)size;

			safe_fprintf(
				compiler->fp,
				"%s %%.t%d, %%%s.%d\n",
				get_qbe_type_store(type_result.type),
				(int)value_result.iota,
				name_token->value,
				(int)local->values[2]
			);
		}

		/* default */
		else {
			if (type_result.type <= TYPE_F64) {

				struct map *local_info = map_new_hash(&compiler->locals, 0);

				local_info->values[0] = (uintptr_t)name_token->value;
				local_info->values[2] = local_map_count-1;
				local_info->values[3] = (uintptr_t)size;

				uint16_t iota = compiler->iota++;
				safe_fprintf(
					compiler->fp,
					"%%.t%d =%c copy 0\n"
					"%s %%.t%d, %%%s.%d\n",
					(int)iota,
					get_qbe_type(type_result.type),
					get_qbe_type_store(type_result.type),
					(int)iota,
					name_token->value,
					(int)local->values[2]
				);
			}
			else {
				struct map *local_info = map_new_hash(&compiler->locals, 0);

				local_info->values[0] = (uintptr_t)name_token->value;
				local_info->values[1] = 1;
				local_info->values[2] = local_map_count-1;
				local_info->values[3] = (uintptr_t)size;

				safe_fprintf(
					compiler->fp,
					"call $memset(l %%%s.%d.data, w 0, l %zu)\n"
					"storel %%%s.%d.data, %%%s.%d\n",
					name_token->value,
					(int)local->values[2],
					size,
					name_token->value,
					(int)local->values[2],
					name_token->value,
					(int)local->values[2]
				);
			}
		}
	}
	return RESULT(TYPE_I8, 0);
}

/* visit if */
static struct result compiler_visit_if(
		struct compiler *compiler,
		struct node *node
) {
	struct node *condition_node = node->first;
	struct node *body_node = condition_node->next;
	struct node *else_node = body_node->next;

	struct result condition_result =
		compiler_visit(compiler, condition_node);
	if (!RESULT_OKAY(condition_result))
		return RESULT_FAIL;

	if (condition_result.type >= TYPE_F32) {

		print_error(
			&node->position,
			"Condition must resolve to integer"
		);
		return RESULT_FAIL;
	}

	uint16_t b1_iota = compiler->iota++; /* main */
	uint16_t b2_iota = compiler->iota++; /* else */
	uint16_t b3_iota = compiler->iota++; /* end */

	safe_fprintf(
		compiler->fp,
		"@branch.%d\n"
		"jnz %%.t%d, @branch.%d.2, @branch.%d\n"
		"@branch.%d.2\n",
		(int)b1_iota,
		(int)condition_result.iota, (int)b1_iota, (int)b2_iota,
		(int)b1_iota
	);

	/* body and else clause */
	struct result body_result =
		compiler_visit(compiler, body_node);
	if (!RESULT_OKAY(body_result))
		return RESULT_FAIL;

	safe_fprintf(
		compiler->fp,
		"@branch.%d.3\n"
		"jmp @branch.%d\n"
		"@branch.%d\n",
		(int)b1_iota,
		(int)b3_iota,
		(int)b2_iota
	);
	if (else_node) {

		struct result else_result =
			compiler_visit(compiler, else_node);
		if (!RESULT_OKAY(else_result))
			return RESULT_FAIL;
	}
	safe_fprintf(
		compiler->fp,
		"@branch.%d\n",
		(int)b3_iota
	);
	return RESULT(TYPE_I8, 0);
}

/* visit for */
static struct result compiler_visit_for(
		struct compiler *compiler,
		struct node *node
) {
	uintptr_t *values = node->values;

	struct node *initializer_node = NULL;
	struct node *condition_node = NULL;
	struct node *increment_node = NULL;

	node = node->first;

	if (values[0]) {
		initializer_node = node;
		node = node->next;
	}
	if (values[1]) {
		condition_node = node;
		node = node->next;
	}
	if (values[2]) {
		increment_node = node;
		node = node->next;
	}
	struct node *body_node = node;

	uint16_t init_iota = compiler->iota++;
	uint16_t start_iota = compiler->iota++;
	uint16_t post_inc_iota = compiler->iota++;
	uint16_t body_iota = compiler->iota++;
	uint16_t jump_iota = compiler->iota++;
	uint16_t end_iota = compiler->iota++;

	if (!push_scope())
		return RESULT_FAIL;

	/* initializer */
	safe_fprintf(
		compiler->fp,
		"@branch.%d\n",
		(int)init_iota
	);
	if (initializer_node) {

		struct result initializer_result =
			compiler_visit(compiler, initializer_node);
		if (!RESULT_OKAY(initializer_result))
			return RESULT_FAIL;
	}
	safe_fprintf(
		compiler->fp,
		"@branch.%d.2\n"
		"jmp @branch.%d\n"
		"@branch.%d\n",
		(int)init_iota,
		(int)post_inc_iota,
		(int)start_iota
	);

	/* increment */
	if (increment_node) {

		struct result increment_result =
			compiler_visit(compiler, increment_node);
		if (!RESULT_OKAY(increment_result))
			return RESULT_FAIL;
	}
	safe_fprintf(
		compiler->fp,
		"@branch.%d\n",
		(int)post_inc_iota
	);

	/* condition */
	if (condition_node) {

		struct result condition_result =
			compiler_visit(compiler, condition_node);
		if (!RESULT_OKAY(condition_result))
			return RESULT_FAIL;

		if (condition_result.type >= TYPE_F32) {

			print_error(
				&condition_node->position,
				"Condition must resolve to integer"
			);
			return RESULT_FAIL;
		}
		safe_fprintf(
			compiler->fp,
			"jnz %%.t%d, @branch.%d, @branch.%d\n",
			(int)condition_result.iota,
			(int)body_iota,
			(int)end_iota
		);
	}
	safe_fprintf(
		compiler->fp,
		"@branch.%d\n",
		(int)body_iota
	);

	/* body */
	uint16_t old_start_iota = compiler->loop_start_iota;
	uint16_t old_end_iota = compiler->loop_end_iota;
	compiler->loop_start_iota = start_iota;
	compiler->loop_end_iota = end_iota;

	compiler->no_block_push_scope = true;

	struct result body_result =
		compiler_visit(compiler, body_node);
	if (!RESULT_OKAY(body_result))
		return RESULT_FAIL;

	pop_scope();

	compiler->loop_start_iota = old_start_iota;
	compiler->loop_end_iota = old_end_iota;

	safe_fprintf(
		compiler->fp,
		"@branch.%d\n"
		"jmp @branch.%d\n"
		"@branch.%d\n",
		(int)jump_iota,
		(int)start_iota,
		(int)end_iota
	);
	return RESULT(TYPE_I8, 0);
}

/* visit continue */
static struct result compiler_visit_continue(
		struct compiler *compiler,
		struct node *node
) {
	if (!compiler->loop_start_iota) {

		print_error(
			&node->position,
			"Attempt to break outside of loop"
		);
		return RESULT_FAIL;
	}
	uint16_t iota = compiler->iota++;
	safe_fprintf(
		compiler->fp,
		"@branch.%d\n"
		"jmp @branch.%d\n"
		"@branch.%d.2\n",
		(int)iota,
		(int)compiler->loop_start_iota,
		(int)iota
	);
	return RESULT(TYPE_I8, 0);
}

/* visit break */
static struct result compiler_visit_break(
		struct compiler *compiler,
		struct node *node
) {
	if (!compiler->loop_start_iota) {

		print_error(
			&node->position,
			"Attempt to continue outside of loop"
		);
		return RESULT_FAIL;
	}
	uint16_t iota = compiler->iota++;
	safe_fprintf(
		compiler->fp,
		"@branch.%d\n"
		"jmp @branch.%d\n"
		"@branch.%d.2\n",
		(int)iota,
		(int)compiler->loop_end_iota,
		(int)iota
	);
	return RESULT(TYPE_I8, 0);
}

/* visit return */
static struct result compiler_visit_return(
		struct compiler *compiler,
		struct node *node
) {
	struct result result =
		compiler_visit(compiler, node->first);
	if (!RESULT_OKAY(result))
		return RESULT_FAIL;

	if (result.type != compiler->return_type) {

		print_error(
			&node->position,
			"Mismatched return types"
		);
		return RESULT_FAIL;
	}
	safe_fprintf(
		compiler->fp,
		"@return%d\n"
		"ret %%.t%d\n"
		"@return%d\n",
		(int)compiler->iota++,
		(int)result.iota,
		(int)compiler->iota++
	);
	return RESULT(TYPE_I8, 0);
}

/* visit binary operation */
static struct result compiler_visit_binop(
		struct compiler *compiler,
		struct node *node
) {
	struct node *left_node = node->first;
	struct node *right_node = left_node->next;
	struct token *op_token = node->tokens[0];

	if (!strcmp(op_token->value, "=")) {

		/* assign to local variable */
		if (left_node->type == NODE_TYPE_LOCAL) {

			struct token *name_token = left_node->tokens[0];
			struct map *local = get_local(name_token->value);
			if (!local) {

				print_error(
					&node->position,
					"Undefined local '%s'",
					name_token->value
				);
				return RESULT_FAIL;
			}
			struct result right_result =
				compiler_visit(compiler, right_node);
			if (!RESULT_OKAY(right_result))
				return RESULT_FAIL;

			/* check types */
			if (local->values[1] != right_result.type) {

				print_error(
					&node->position,
					"Mismatched assignment types"
				);
				return RESULT_FAIL;
			}

			/* operation */
			struct result result = {
				.type = local->values[1],
				.flags = 0,
				.iota = right_result.iota,
			};
			safe_fprintf(
				compiler->fp,
				"%s %%.t%d, %%%s.%d\n",
				get_qbe_type_store(local->values[1]),
				result.iota,
				name_token->value,
				(int)local->values[2]
			);
			return result;
		}

		/* assign to global variable */
		else if (left_node->type == NODE_TYPE_GLOBAL) {

			struct token *name_token = left_node->tokens[0];

			struct result right_result =
				compiler_visit(compiler, right_node);
			if (!RESULT_OKAY(right_result))
				return RESULT_FAIL;

			struct result result = {
				.type = right_result.type,
				.flags = 0,
				.iota = right_result.iota,
			};
			safe_fprintf(
				compiler->fp,
				"%s %%.t%d, $%s\n",
				get_qbe_type_store(result.type),
				result.iota,
				name_token->value
			);
			return result;
		}

		/* assign to pointer */
		else if (left_node->type == NODE_TYPE_DEREFERENCE) {

			struct node *type_node = left_node->first;
			struct node *pointer_node = type_node->next;

			struct result type_result =
				compiler_visit(compiler, type_node);
			if (!RESULT_OKAY(type_result))
				return RESULT_FAIL;

			struct result pointer_result =
				compiler_visit(compiler, pointer_node);
			if (!RESULT_OKAY(pointer_result))
				return RESULT_FAIL;

			struct result right_result =
				compiler_visit(compiler, right_node);
			if (!RESULT_OKAY(right_result))
				return RESULT_FAIL;

			/* check types */
			if (pointer_result.type != TYPE_PTR) {

				print_error(
					&node->position,
					"Pointer to dereference must be pointer type"
				);
				return RESULT_FAIL;
			}
			else if (type_result.type != right_result.type) {

				print_error(
					&node->position,
					"Mismatched assignment types"
				);
				return RESULT_FAIL;
			}

			/* operation */
			struct result result = {
				.type = type_result.type,
				.flags = 0,
				.iota = right_result.iota,
			};
			safe_fprintf(
				compiler->fp,
				"%s %%.t%d, %%.t%d\n",
				get_qbe_type_store(type_result.type),
				result.iota,
				(int)pointer_result.iota
			);
			return result;
		}

		/* unassignable */
		else {

			print_error(
				&node->position,
				"Value is not assignable"
			);
			return RESULT_FAIL;
		}
	}

	/* and */
	else if (!strcmp(op_token->value, "and")) {

		struct result left_result =
			compiler_visit(compiler, left_node);
		if (!RESULT_OKAY(left_result))
			return RESULT_FAIL;

		uint16_t b1_iota = compiler->iota++; /* left */
		uint16_t b2_iota = compiler->iota++; /* right */
		uint16_t b3_iota = compiler->iota++; /* fail */

		struct result result = {
			.type = TYPE_U8,
			.flags = 0,
			.iota = compiler->iota,
		};
		safe_fprintf(
			compiler->fp,
			"@branch.%d\n"
			"jnz %%.t%d, @branch.%d.2, @branch.%d\n"
			"@branch.%d.2\n",
			(int)b1_iota,
			(int)left_result.iota, (int)b1_iota, (int)b3_iota,
			(int)b1_iota
		);

		/* right value */
		struct result right_result =
			compiler_visit(compiler, right_node);
		if (!RESULT_OKAY(right_result))
			return RESULT_FAIL;

		safe_fprintf(
			compiler->fp,
			"@branch.%d\n"
			"jnz %%.t%d, @branch.%d.2, @branch.%d\n"
			"@branch.%d.2\n"
			"jmp @branch.%d\n"
			"@branch.%d\n"
			"jmp @branch.%d\n"
			"@branch.%d\n"
			"%%.t%d =l phi @branch.%d.2 1, @branch.%d 0\n",
			(int)b2_iota,
			(int)right_result.iota, (int)b2_iota, (int)b3_iota,
			(int)b2_iota,
			(int)result.iota,
			(int)b3_iota,
			(int)result.iota,
			(int)result.iota,
			(int)result.iota, (int)b2_iota, (int)b3_iota
		);
		return result;
	}

	/* or */
	else if (!strcmp(op_token->value, "or")) {

		struct result left_result =
			compiler_visit(compiler, left_node);
		if (!RESULT_OKAY(left_result))
			return RESULT_FAIL;

		uint16_t b1_iota = compiler->iota++; /* left */
		uint16_t b2_iota = compiler->iota++; /* right */
		uint16_t b3_iota = compiler->iota++; /* fail */

		struct result result = {
			.type = TYPE_U8,
			.flags = 0,
			.iota = compiler->iota,
		};
		safe_fprintf(
			compiler->fp,
			"@branch.%d\n"
			"jnz %%.t%d, @branch.%d.2, @branch.%d.2\n"
			"@branch.%d.2\n",
			(int)b1_iota,
			(int)left_result.iota, (int)b2_iota, (int)b1_iota,
			(int)b1_iota
		);

		/* right value */
		struct result right_result =
			compiler_visit(compiler, right_node);
		if (!RESULT_OKAY(right_result))
			return RESULT_FAIL;

		safe_fprintf(
			compiler->fp,
			"@branch.%d\n"
			"jnz %%.t%d, @branch.%d.2, @branch.%d\n"
			"@branch.%d.2\n"
			"jmp @branch.%d\n"
			"@branch.%d\n"
			"jmp @branch.%d\n"
			"@branch.%d\n"
			"%%.t%d =l phi @branch.%d.2 1, @branch.%d 0\n",
			(int)b2_iota,
			(int)right_result.iota, (int)b2_iota, (int)b3_iota,
			(int)b2_iota,
			(int)result.iota,
			(int)b3_iota,
			(int)result.iota,
			(int)result.iota,
			(int)result.iota, (int)b2_iota, (int)b3_iota
		);
		return result;
	}

	/* check values */
	struct result left_result =
		compiler_visit(compiler, left_node);
	if (!RESULT_OKAY(left_result))
		return RESULT_FAIL;

	struct result right_result =
		compiler_visit(compiler, right_node);
	if (!RESULT_OKAY(right_result))
		return RESULT_FAIL;

	if (left_result.type != right_result.type) {

		print_error(
			&node->position,
			"Mismatched types (operation '%s')",
			op_token->value
		);
		return RESULT_FAIL;
	}

	/* operations */
	bool is_unsigned = true;
	switch (left_result.type) {
		case TYPE_I8:
		case TYPE_I16:
		case TYPE_I32:
		case TYPE_I64:
		case TYPE_F32:
		case TYPE_F64:
			is_unsigned = false;
	}
	const char *q_operation = NULL;

	if ((strchr("%&|^", op_token->value[0]) ||
	     !strcmp(op_token->value, "<<") ||
	     !strcmp(op_token->value, ">>")) &&
	    left_result.type >= TYPE_F32) {

		print_error(
			&node->position,
			"Invalid operation"
		);
		return RESULT_FAIL;
	}

	/* determine operation */
	struct result result = {
		.type = left_result.type,
		.flags = 0,
	};
	if (strchr("+-*/%&|^", op_token->value[0])) {
		switch (op_token->value[0]) {
			case '+': q_operation = "add"; break;
			case '-': q_operation = "sub"; break;
			case '*': q_operation = "mul"; break;
			case '/':
				if (is_unsigned) q_operation = "udiv";
				else q_operation = "div";
				break;
			case '%':
				if (is_unsigned) q_operation = "urem";
				else q_operation = "rem";
				break;
			case '&': q_operation = "and"; break;
			case '|': q_operation = "or"; break;
			case '^': q_operation = "xor"; break;
		}
	}
	else if (!strcmp(op_token->value, "<<")) {
		if (is_unsigned) q_operation = "shl";
		else q_operation = "sal";
	}
	else if (!strcmp(op_token->value, ">>")) {
		if (is_unsigned) q_operation = "shr";
		else q_operation = "sar";
	}
	else if (!strcmp(op_token->value, "==")) {
		if (left_result.type == TYPE_F32) q_operation = "ceqs";
		else if (left_result.type == TYPE_F64) q_operation = "ceqd";
		else q_operation = "ceql";
		result.type = TYPE_U8;
	}
	else if (!strcmp(op_token->value, "!=")) {
		if (left_result.type == TYPE_F32) q_operation = "cnes";
		else if (left_result.type == TYPE_F64) q_operation = "cned";
		else q_operation = "cnel";
		result.type = TYPE_U8;
	}
	else if (!strcmp(op_token->value, "<")) {
		if (left_result.type == TYPE_F32) q_operation = "clts";
		else if (left_result.type == TYPE_F64) q_operation = "cltd";
		else if (is_unsigned) q_operation = "cultl";
		else q_operation = "csltl";
		result.type = TYPE_U8;
	}
	else if (!strcmp(op_token->value, ">")) {
		if (left_result.type == TYPE_F32) q_operation = "cgts";
		else if (left_result.type == TYPE_F64) q_operation = "cgtd";
		else if (is_unsigned) q_operation = "cugtl";
		else q_operation = "csgtl";
		result.type = TYPE_U8;
	}

	/* operation */
	if (!local_map_count)
		return result;

	result.iota = compiler->iota++;

	safe_fprintf(
		compiler->fp,
		"%%.t%d =%c %s %%.t%d, %%.t%d\n",
		result.iota,
		get_qbe_type(result.type),
		q_operation,
		left_result.iota,
		right_result.iota
	);
	return result;
}

/* visit unary operation */
static struct result compiler_visit_unop(
		struct compiler *compiler,
		struct node *node
) {
	struct token *op_token = node->tokens[0];
	struct node *value_node = node->first;

	struct result value_result =
		compiler_visit(compiler, value_node);
	if (!RESULT_OKAY(value_result))
		return RESULT_FAIL;

	struct result result = {
		.type = value_result.type,
		.flags = 0,
	};

	/* logical not */
	if (!strcmp(op_token->value, "!")) {

		if (value_result.type >= TYPE_F32) {

			print_error(
				&node->position,
				"Invalid operation"
			);
			return RESULT_FAIL;
		}
		uint16_t b1_iota = compiler->iota++;
		uint16_t b2_iota = compiler->iota++;
		uint16_t b3_iota = compiler->iota++;

		result.type = TYPE_U8;
		result.iota = compiler->iota++;

		safe_fprintf(
			compiler->fp,
			"@branch.%d\n"
			"jnz %%.t%d, @branch.%d, @branch.%d\n"
			"@branch.%d\n"
			"jmp @result.%d\n"
			"@branch.%d\n"
			"jmp @result.%d\n"
			"@result.%d\n"
			"%%.t%d =l phi @branch.%d 0, @branch.%d 1\n",
			(int)b1_iota,
			(int)value_result.iota, (int)b2_iota, (int)b3_iota,
			(int)b2_iota,
			(int)result.iota,
			(int)b3_iota,
			(int)result.iota,
			(int)result.iota,
			(int)result.iota, (int)b2_iota, (int)b3_iota
		);
	}

	/* bitwise not */
	else if (!strcmp(op_token->value, "~")) {

		if (value_result.type >= TYPE_F32) {

			print_error(
				&node->position,
				"Invalid operation"
			);
			return RESULT_FAIL;
		}
		result.iota = compiler->iota++;

		safe_fprintf(
			compiler->fp,
			"%%.t%d =l xor %%.t%d, 18446744073709551615\n",
			(int)result.iota, (int)value_result.iota
		);
	}

	/* negation */
	else if (!strcmp(op_token->value, "-")) {

		result.iota = compiler->iota++;

		safe_fprintf(
			compiler->fp,
			"%%.t%d =%c neg %%.t%d\n",
			(int)result.iota,
			get_qbe_type(result.type),
			(int)value_result.iota
		);
	}
	return result;
}

/* visit call */
#define ARG_RESULT_MAX 64

static struct result compiler_visit_call(
		struct compiler *compiler,
		struct node *node
) {
	size_t arg_count = (size_t)node->values[0];

	node = node->first;
	struct result call_result =
		compiler_visit(compiler, node);
	if (!RESULT_OKAY(call_result))
		return RESULT_FAIL;

	struct node *return_type_node = node->next;
	for (
		size_t i = 0; i < arg_count;
		i++, return_type_node = return_type_node->next
	);

	/* arguments */
	struct result arg_results[ARG_RESULT_MAX];
	struct node *arg_node = node->next;

	for (size_t i = 0; i < arg_count; i++, arg_node = arg_node->next) {

		if (i >= ARG_RESULT_MAX) {

			print_error(
				&arg_node->position,
				"Reached argument limit (%d)",
				(int)ARG_RESULT_MAX
			);
			return RESULT_FAIL;
		}
		arg_results[i] = compiler_visit(compiler, arg_node);
		if (!RESULT_OKAY(arg_results[i]))
			return RESULT_FAIL;
	}

	/* return value and call */
	struct result return_type_result =
		compiler_visit(compiler, return_type_node);
	if (!RESULT_OKAY(return_type_result))
		return RESULT_FAIL;

	struct result result = {
		.type = return_type_result.type,
		.flags = 0,
		.iota = compiler->iota++,
	};
	safe_fprintf(
		compiler->fp,
		"%%.t%d =%c call %%.t%d(",
		(int)result.iota,
		get_qbe_type(result.type),
		(int)call_result.iota
	);

	/* arguments */
	arg_node = node->next;
	for (size_t i = 0; i < arg_count; i++, arg_node = arg_node->next) {

		if (i) fputs(", ", compiler->fp);
		safe_fprintf(
			compiler->fp,
			"%c %%.t%d",
			get_qbe_type(arg_results[i].type),
			(int)arg_results[i].iota
		);
	}
	fputs(")\n", compiler->fp);

	return result;
}

/* visit number */
static struct result compiler_visit_number(
		struct compiler *compiler,
		struct node *node
) {
	intmax_t value;
	sscanf(node->tokens[0]->value, "%jd", &value);

	struct result type_result = {
		.type = TYPE_U64,
	};
	if (node->first) {

		type_result = compiler_visit(compiler, node->first);
		if (!RESULT_OKAY(type_result))
			return RESULT_FAIL;
	}

	struct result result = {
		.type = type_result.type,
		.flags = FLAG_CONSTANT,
		.data = (uintptr_t)value
	};
	if (!local_map_count)
		return result;

	result.iota = compiler->iota++;

	safe_fprintf(
		compiler->fp,
		"%%.t%d =%c copy %ju\n",
		(int)result.iota,
		get_qbe_type(type_result.type),
		(uintmax_t)value
	);
	return result;
}

/* visit string */
static struct result compiler_visit_string(
		struct compiler *compiler,
		struct node *node
) {
	struct token *value_token = node->tokens[0];
	struct node *type_node = node->first;

	struct result type_result =
		compiler_visit(compiler, type_node);
	if (!RESULT_OKAY(type_result))
		return RESULT_FAIL;

	if (type_result.type != TYPE_U8) {

		print_error(
			&node->position,
			"Unsupported string type"
		);
		return RESULT_FAIL;
	}

	/* register string */
	size_t i = string_count++;
	struct map *map = map_new_hash(&string_map, (uint32_t)i);

	map->values[0] = (uintptr_t)value_token->value;

	/* result */
	struct result result = {
		.type = TYPE_PTR,
		.flags = 0,
		.data = (uintptr_t)i,
	};
	if (!local_map_count)
		return result;

	result.iota = compiler->iota++;

	safe_fprintf(
		compiler->fp,
		"%%.t%d =l copy $.str%zu\n",
		(int)result.iota, i
	);
	return result;
}

/* visit global */
static struct result compiler_visit_global(
		struct compiler *compiler,
		struct node *node
) {
	struct token *name_token = node->tokens[0];
	struct result result = {
		.type = TYPE_PTR,
		.flags = 0,
	};
	if (!local_map_count)
		return result;

	result.iota = compiler->iota++;

	safe_fprintf(
		compiler->fp,
		"%%.t%d =l copy $%s\n",
		result.iota,
		name_token->value
	);
	return result;
}

/* visit local */
static struct result compiler_visit_local(
		struct compiler *compiler,
		struct node *node
) {
	struct token *name_token = node->tokens[0];
	struct map *local = get_local(name_token->value);
	if (!local) {

		print_error(
			&node->position,
			"Undefined local '%s'",
			name_token->value
		);
		return RESULT_FAIL;
	}
	struct result result = {
		.type = local->values[1],
		.flags = 0,
	};
	result.iota = compiler->iota++;

	safe_fprintf(
		compiler->fp,
		"%%.t%d =%c %s %%%s.%d\n",
		result.iota,
		get_qbe_type(local->values[1]),
		get_qbe_type_load(local->values[1]),
		name_token->value,
		local->values[2]
	);
	return result;
}

/* visit cast */
static struct result compiler_visit_cast(
		struct compiler *compiler,
		struct node *node
) {
	struct node *type_node = node->first;
	struct node *value_node = type_node->next;

	struct result type_result =
		compiler_visit(compiler, type_node);
	if (!RESULT_OKAY(type_result))
		return RESULT_FAIL;

	struct result value_result =
		compiler_visit(compiler, value_node);
	if (!RESULT_OKAY(value_result))
		return RESULT_FAIL;

	if (type_result.type == value_result.type)
		return value_result;

	struct result result = {
		.type = type_result.type,
		.flags = 0,
	};

	/* casting from float */
	const char *q_operation = NULL;
	if (value_result.type == TYPE_F32) {

		switch (type_result.type) {
			case TYPE_F64: q_operation = "exts"; break;
			case TYPE_I8:
			case TYPE_I16:
			case TYPE_I32:
			case TYPE_I64:
				q_operation = "stosi"; break;
			default: q_operation = "stoui"; break;
		}
	}
	else if (value_result.type == TYPE_F64) {

		switch (type_result.type) {
			case TYPE_F32: q_operation = "truncd"; break;
			case TYPE_I8:
			case TYPE_I16:
			case TYPE_I32:
			case TYPE_I64:
				q_operation = "dtosi"; break;
			default: q_operation = "dtoui"; break;
		}
	}

	/* casting from integer to float */
	else if (type_result.type == TYPE_F32 ||
		 type_result.type == TYPE_F64) {

		const char *mask = NULL;
		bool is_unsigned = true;

		switch (value_result.type) {
			case TYPE_I8:
				is_unsigned = false;
			case TYPE_U8:
				mask = "255";
				break;
			case TYPE_I16:
				is_unsigned = false;
			case TYPE_U16:
				mask = "65535";
				break;
			case TYPE_I32:
				is_unsigned = false;
			case TYPE_U32:
				mask = "4294967295";
				break;
			case TYPE_I64:
				is_unsigned = false;
			case TYPE_U64:
				mask = "18446744073709551615";
				break;
		}

		if (is_unsigned)
			q_operation = "sltof";
		else q_operation = "ultof";

		uint16_t iota = compiler->iota++;
		result.iota = compiler->iota++;

		safe_fprintf(
			compiler->fp,
			"%%.t%d =l and %%.t%d, %s\n"
			"%%.t%d =%c %s %%.t%d\n",
			(int)iota,
			(int)value_result.iota,
			mask,
			(int)result.iota,
			get_qbe_type(result.type),
			q_operation,
			(int)iota
		);
		return result;
	}

	/* normal cast */
	if (!q_operation) {

		result.iota = value_result.iota;
		return result;
	}
	result.iota = compiler->iota++;

	safe_fprintf(
		compiler->fp,
		"%%.t%d =%c %s %%.t%d\n",
		(int)result.iota,
		get_qbe_type(result.type),
		q_operation,
		(int)value_result.iota
	);
	return result;
}

/* visit member */
static struct result compiler_visit_member(
		struct compiler *compiler,
		struct node *node
) {
	struct token *field_token = node->tokens[0];

	struct result type_result =
		compiler_visit(compiler, node->first);
	if (!RESULT_OKAY(type_result))
		return RESULT_FAIL;

	struct map *map = (struct map *)type_result.data;
	struct map *field_map = map_get(
			(struct map *)map->values[2],
			field_token->value
	);
	if (!field_map) {

		print_error(
			&node->position,
			"Undefined struct field '%s'",
			field_token->value
		);
		return RESULT_FAIL;
	}

	/* write value */
	struct result result = {
		.type = TYPE_PTR,
		.flags = FLAG_CONSTANT,
		.data = field_map->values[1],
	};
	if (!local_map_count)
		return result;

	result.iota = compiler->iota++;

	safe_fprintf(
		compiler->fp,
		"%%.t%d =l copy %zu\n",
		(int)result.iota,
		(size_t)result.data
	);
	return result;
}

/* visit dereference */
static struct result compiler_visit_dereference(
		struct compiler *compiler,
		struct node *node
) {
	struct node *type_node = node->first;
	struct node *pointer_node = type_node->next;

	struct result type_result =
		compiler_visit(compiler, type_node);
	if (!RESULT_OKAY(type_result))
		return RESULT_FAIL;

	struct result pointer_result =
		compiler_visit(compiler, pointer_node);
	if (!RESULT_OKAY(pointer_result))
		return RESULT_FAIL;

	/* check types */
	if (pointer_result.type != TYPE_PTR) {

		print_error(
			&node->position,
			"Dereference pointer must be pointer type"
		);
		return RESULT_FAIL;
	}

	/* operation */
	struct result result = {
		.type = type_result.type,
		.flags = 0,
		.iota = compiler->iota++,
	};
	safe_fprintf(
		compiler->fp,
		"%%.t%d =%c %s %%.t%d\n",
		(int)result.iota,
		get_qbe_type(type_result.type),
		get_qbe_type_load(type_result.type),
		(int)pointer_result.iota
	);
	return result;
}

/* visit reference */
static struct result compiler_visit_reference(
		struct compiler *compiler,
		struct node *node
) {
	node = node->first;
	if (node->type != NODE_TYPE_LOCAL) {

		print_error(
			&node->position,
			"Unreferenceable value"
		);
		return RESULT_FAIL;
	}
	struct token *name_token = node->tokens[0];
	struct map *local = get_local(name_token->value);
	if (!local) {

		print_error(
			&node->position,
			"Undefined local '%s'",
			name_token->value
		);
		return RESULT_FAIL;
	}

	/* operation */
	struct result result = {
		.type = TYPE_PTR,
		.flags = 0,
		.iota = compiler->iota++,
	};
	safe_fprintf(
		compiler->fp,
		"%%.t%d =l copy %%%s.%d\n",
		(int)result.iota,
		name_token->value,
		(int)local->values[2]
	);
	return result;
}

/* visit node */
static struct result compiler_visit(
		struct compiler *compiler,
		struct node *node
) {
	switch (node->type) {
		case NODE_TYPE_BLOCK:
			return compiler_visit_block(compiler, node);
		case NODE_TYPE_TYPE:
			return compiler_visit_type(compiler, node);
		case NODE_TYPE_STRUCT:
			return compiler_visit_struct(compiler, node);
		case NODE_TYPE_PROCEDURE:
			return compiler_visit_procedure(compiler, node);
		case NODE_TYPE_VARIABLE:
			return compiler_visit_variable(compiler, node);
		case NODE_TYPE_IF:
			return compiler_visit_if(compiler, node);
		case NODE_TYPE_FOR:
			return compiler_visit_for(compiler, node);
		case NODE_TYPE_CONTINUE:
			return compiler_visit_continue(compiler, node);
		case NODE_TYPE_BREAK:
			return compiler_visit_break(compiler, node);
		case NODE_TYPE_RETURN:
			return compiler_visit_return(compiler, node);
		case NODE_TYPE_BINOP:
			return compiler_visit_binop(compiler, node);
		case NODE_TYPE_UNOP:
			return compiler_visit_unop(compiler, node);
		case NODE_TYPE_CALL:
			return compiler_visit_call(compiler, node);
		case NODE_TYPE_NUMBER:
			return compiler_visit_number(compiler, node);
		case NODE_TYPE_STRING:
			return compiler_visit_string(compiler, node);
		case NODE_TYPE_GLOBAL:
			return compiler_visit_global(compiler, node);
		case NODE_TYPE_LOCAL:
			return compiler_visit_local(compiler, node);
		case NODE_TYPE_CAST:
			return compiler_visit_cast(compiler, node);
		case NODE_TYPE_MEMBER:
			return compiler_visit_member(compiler, node);
		case NODE_TYPE_DEREFERENCE:
			return compiler_visit_dereference(compiler, node);
		case NODE_TYPE_REFERENCE:
			return compiler_visit_reference(compiler, node);
	}
	print_error(
		&node->position,
		"Unimplemented node type %d",
		node->type
	);
	return RESULT_FAIL;
}

/* parse arguments */
static const char *arg_input = NULL;
static const char *arg_output = NULL;

static int parse_args(int argc, const char **argv) {

	progname = argv[0];
	if (argc != 3) {

		fprintf(
			stderr,
			"%s: Expected input and output\n"
			"Usage: %s <input> <output>\n",
			progname, progname
		);
		return -1;
	}
	arg_input = argv[1];
	arg_output = argv[2];

	return 0;
}

/* run application */
static struct lexer lexer;
static struct parser parser;
static struct compiler compiler;

static FILE *read_fp = NULL;
static FILE *write_fp = NULL;

static int run(int argc, const char **argv) {

	if (parse_args(argc, argv) < 0)
		return 1;

	/* open input file */
	read_fp = fopen(arg_input, "rb");
	if (!read_fp) {

		print_error(
			NULL, "Can't open file '%s': %s\n",
			arg_input, strerror(errno)
		);
		return -1;
	}

	/* generate tokens */
	lexer = (struct lexer)LEXER_INIT(read_fp, arg_input);
	if (lexer_make_tokens(&lexer) < 0)
		return 1;
	fclose(read_fp);
	read_fp = NULL;

	/* generate ast */
	parser = (struct parser)PARSER_INIT(lexer.first);
	if (!parser_parse(&parser))
		return 1;

	/* open output file */
	write_fp = fopen(arg_output, "wb");
	if (!write_fp) {

		print_error(
			NULL, "Can't open file '%s': %s\n",
			arg_output, strerror(errno)
		);
		return -1;
	}

	/* compile ast */
	compiler = (struct compiler)COMPILER_INIT(write_fp);
	if (!RESULT_OKAY(compiler_visit(&compiler, parser.node)))
		return 1;

	/* write string table */
	struct map *string = string_map.next;
	for (; string; string = string->next) {

		safe_fprintf(
			compiler.fp,
			"data $.str%zu = { b \"",
			(size_t)string->key_hash
		);
		const char *data = (const char *)string->values[0];
		for (size_t i = 0; data[i]; i++) {

			if (data[i] == '\\' || data[i] == '"')
				fputc('\\', compiler.fp);

			if (data[i] == '\n')
				fputs("\\n", compiler.fp);
			else
				fputc(data[i], compiler.fp);
		}
		safe_fprintf(
			compiler.fp,
			"\", b 0 }\n"
		);
	}
	return 0;
}

/* clean up resources */
static void cleanup(void) {

	for (size_t i = 0; i < local_map_count; i++)
		map_destroy(local_maps + i);
	map_destroy(&struct_map);
	map_destroy(&global_map);

	parser_destroy(&parser);
	lexer_destroy(&lexer);
	if (write_fp) fclose(write_fp);
	if (read_fp) fclose(read_fp);
}

int main(int argc, const char **argv) {

	int code = run(argc, argv);
	cleanup();
	return code;
}
