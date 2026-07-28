/* A Bison parser, made by GNU Bison 3.8.2.  */

/* Bison interface for Yacc-like parsers in C

   Copyright (C) 1984, 1989-1990, 2000-2015, 2018-2021 Free Software Foundation,
   Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

#ifndef YY_SGUG_EXPR_YY_GRAMMAR_TAB_H_INCLUDED
# define YY_SGUG_EXPR_YY_GRAMMAR_TAB_H_INCLUDED
/* Debug traces.  */
#ifndef SGUG_EXPR_YYDEBUG
# if defined YYDEBUG
#if YYDEBUG
#   define SGUG_EXPR_YYDEBUG 1
#  else
#   define SGUG_EXPR_YYDEBUG 0
#  endif
# else /* ! defined YYDEBUG */
#  define SGUG_EXPR_YYDEBUG 0
# endif /* ! defined YYDEBUG */
#endif  /* ! defined SGUG_EXPR_YYDEBUG */
#if SGUG_EXPR_YYDEBUG
extern int sgug_expr_yydebug;
#endif
/* "%code requires" blocks.  */
#line 30 "grammar.y"

struct sgug_expr_parse;
struct sgug_expr_node;

#line 62 "grammar.tab.h"

/* Token kinds.  */
#ifndef SGUG_EXPR_YYTOKENTYPE
# define SGUG_EXPR_YYTOKENTYPE
  enum sgug_expr_yytokentype
  {
    SGUG_EXPR_YYEMPTY = -2,
    SGUG_EXPR_YYEOF = 0,           /* "end of file"  */
    SGUG_EXPR_YYerror = 256,       /* error  */
    SGUG_EXPR_YYUNDEF = 257,       /* "invalid token"  */
    NAME = 258,                    /* NAME  */
    STRING = 259,                  /* STRING  */
    NUMBER = 260,                  /* NUMBER  */
    TRUE = 261,                    /* TRUE  */
    FALSE = 262,                   /* FALSE  */
    NUL = 263,                     /* NUL  */
    AND = 264,                     /* AND  */
    OR = 265,                      /* OR  */
    EQ = 266,                      /* EQ  */
    NE = 267,                      /* NE  */
    LE = 268,                      /* LE  */
    GE = 269,                      /* GE  */
    STAR = 270                     /* STAR  */
  };
  typedef enum sgug_expr_yytokentype sgug_expr_yytoken_kind_t;
#endif

/* Value type.  */
#if ! defined SGUG_EXPR_YYSTYPE && ! defined SGUG_EXPR_YYSTYPE_IS_DECLARED
union SGUG_EXPR_YYSTYPE
{
#line 50 "grammar.y"

	double num;
	char *str;
	struct sgug_expr_node *node;
	struct sgug_expr_node *list;

#line 101 "grammar.tab.h"

};
typedef union SGUG_EXPR_YYSTYPE SGUG_EXPR_YYSTYPE;
# define SGUG_EXPR_YYSTYPE_IS_TRIVIAL 1
# define SGUG_EXPR_YYSTYPE_IS_DECLARED 1
#endif

/* Location type.  */
#if ! defined SGUG_EXPR_YYLTYPE && ! defined SGUG_EXPR_YYLTYPE_IS_DECLARED
typedef struct SGUG_EXPR_YYLTYPE SGUG_EXPR_YYLTYPE;
struct SGUG_EXPR_YYLTYPE
{
  int first_line;
  int first_column;
  int last_line;
  int last_column;
};
# define SGUG_EXPR_YYLTYPE_IS_DECLARED 1
# define SGUG_EXPR_YYLTYPE_IS_TRIVIAL 1
#endif




int sgug_expr_yyparse (struct sgug_expr_parse *p);

/* "%code provides" blocks.  */
#line 35 "grammar.y"

int sgug_expr_yylex(SGUG_EXPR_YYSTYPE *, SGUG_EXPR_YYLTYPE *,
    struct sgug_expr_parse *);
void sgug_expr_yyerror(SGUG_EXPR_YYLTYPE *, struct sgug_expr_parse *,
    const char *);

#line 136 "grammar.tab.h"

#endif /* !YY_SGUG_EXPR_YY_GRAMMAR_TAB_H_INCLUDED  */
