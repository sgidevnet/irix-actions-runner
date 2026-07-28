/*
 * The GitHub Actions expression language.
 *
 * Written from GitHub.DistributedTask.Expressions2 in actions/runner, which is
 * the only specification that exists: no grammar is published by GitHub, and
 * neither actionlint nor act nor the TypeScript language service ships one.
 * Every reference implementation hand-writes the parse.
 *
 * Regenerate with `make grammar`. The generated .c and .h are committed so an
 * ordinary build needs no bison.
 *
 * Two things about this language surprise people:
 *
 *   - There is no arithmetic. The operator set is closed, and `-` is legal
 *     inside an identifier, which is why `1-2` cannot be subtraction.
 *   - `&&` and `||` yield an operand, not a boolean. `a && b` is b when a is
 *     truthy and a otherwise.
 *
 * Precedence below matches Token.cs exactly. The C# numbers are 19 for the
 * postfix group, 16 for `!`, 11 relational, 10 equality, 6 `&&`, 5 `||`.
 * `!` is the only right-associative operator.
 */

%define api.pure full
%define api.prefix {sgug_expr_yy}
%define parse.error verbose
%locations
%param {struct sgug_expr_parse *p}

%code requires {
struct sgug_expr_parse;
struct sgug_expr_node;
}

%code provides {
int sgug_expr_yylex(SGUG_EXPR_YYSTYPE *, SGUG_EXPR_YYLTYPE *,
    struct sgug_expr_parse *);
void sgug_expr_yyerror(SGUG_EXPR_YYLTYPE *, struct sgug_expr_parse *,
    const char *);
}

%{
#include "expr/parse.h"

/* bison would otherwise use alloca for its stack. IRIX has it, but a parser
 * that cannot exhaust the stack is the reason we are using bison at all. */
#define YYSTACK_USE_ALLOCA 0
%}

%union {
	double num;
	char *str;
	struct sgug_expr_node *node;
	struct sgug_expr_node *list;
}

%token <str> NAME STRING
%token <num> NUMBER
%token TRUE FALSE NUL
%token AND OR EQ NE LE GE STAR

%type <node> expr primary
%type <list> args arglist

%left OR
%left AND
%left EQ NE
%left '<' '>' LE GE
%right '!'
%left '.' '[' '('

%start start

%%

start
	: expr				{ p->root = $1; }
	;

expr
	: expr OR expr			{ $$ = sgug_expr_binary(p, SGUG_EXPR_OP_OR, $1, $3); }
	| expr AND expr			{ $$ = sgug_expr_binary(p, SGUG_EXPR_OP_AND, $1, $3); }
	| expr EQ expr			{ $$ = sgug_expr_binary(p, SGUG_EXPR_OP_EQ, $1, $3); }
	| expr NE expr			{ $$ = sgug_expr_binary(p, SGUG_EXPR_OP_NE, $1, $3); }
	| expr '<' expr			{ $$ = sgug_expr_binary(p, SGUG_EXPR_OP_LT, $1, $3); }
	| expr '>' expr			{ $$ = sgug_expr_binary(p, SGUG_EXPR_OP_GT, $1, $3); }
	| expr LE expr			{ $$ = sgug_expr_binary(p, SGUG_EXPR_OP_LE, $1, $3); }
	| expr GE expr			{ $$ = sgug_expr_binary(p, SGUG_EXPR_OP_GE, $1, $3); }
	| '!' expr			{ $$ = sgug_expr_unary(p, SGUG_EXPR_OP_NOT, $2); }
	| primary			{ $$ = $1; }
	;

primary
	: '(' expr ')'			{ $$ = $2; }
	| primary '.' NAME		{ $$ = sgug_expr_index_lit(p, $1, $3); }
	| primary '.' STAR		{ $$ = sgug_expr_wildcard(p, $1); }
	| primary '[' expr ']'		{ $$ = sgug_expr_binary(p,
					      SGUG_EXPR_OP_INDEX, $1, $3); }
	| primary '[' STAR ']'		{ $$ = sgug_expr_wildcard(p, $1); }
	| NAME '(' args ')'		{ $$ = sgug_expr_call(p, $1, $3, @1.first_column); }
	| NAME				{ $$ = sgug_expr_named(p, $1, @1.first_column); }
	| NUMBER			{ $$ = sgug_expr_number(p, $1); }
	| STRING			{ $$ = sgug_expr_string(p, $1); }
	| TRUE				{ $$ = sgug_expr_bool(p, 1); }
	| FALSE				{ $$ = sgug_expr_bool(p, 0); }
	| NUL				{ $$ = sgug_expr_null(p); }
	;

args
	: /* empty */			{ $$ = NULL; }
	| arglist			{ $$ = $1; }
	;

arglist
	: expr				{ $$ = sgug_expr_arg(p, NULL, $1); }
	| arglist ',' expr		{ $$ = sgug_expr_arg(p, $1, $3); }
	;

%%

void
sgug_expr_yyerror(SGUG_EXPR_YYLTYPE *loc, struct sgug_expr_parse *p,
    const char *msg)
{
	sgug_expr_parse_error(p, loc->first_column, msg);
}
