
#include <assert.h>
#include <stddef.h>

#include "rtrie.h"
#include "message.h"
#include "refal.h"

enum lexer_state {
   lex_leadingspace,    ///< Пробелы в начале строки.
   lex_whitespace,      ///< Пробелы после лексемы.
   lex_comment_line,    ///< Строка комментария, начинается со *
   lex_comment_c,       ///< Комментарии в стиле C /* */
   lex_string_quoted,   ///< Знаковая строка в одинарных кавычках.
   lex_string_dquoted,  ///< Знаковая строка в двойных кавычках.
   lex_number,          ///< Целое число.
   lex_identifier,      ///< Идентификатор (имя функции).
   lex_operator,        ///< Оператор.
};

enum semantic_state {
   ss_source,           ///< Верхний уровень синтаксиса.
   ss_identifier,       ///< Идентификатор.
   ss_expression,       ///< Общее выражение (простой функции).
};

size_t refal_parse_text(
      struct refal_trie    *const restrict ids,
      struct refal_vm      *const restrict vm,
      const char           *const begin,
      const char           *const end,
      struct refal_message *st)
{
   const char *restrict src = begin;
   rf_index command = 0;      // индекс ячейки с последней командой интерпретатора.
   rtrie_index node = 0;      // последний добавленный в префиксное дерево узел.
   rf_int number = 0;         // вычисляемое значение лексемы "число"

   unsigned line_num = 0;     // номер текущей строки

next_line:
   ++line_num;
   const char *line = src;    // начало текущей строки
   unsigned pos = 0;          // номер символа в строке

   enum semantic_state semantic = ss_source;
   enum lexer_state lexer  = lex_leadingspace;

next_char:
   if (src == end)
      goto complete;
   assert(src < end);

   char chr = *src++;
   ++pos;
   switch (chr) {

   // Пробельные символы.
   case '\t':  //TODO pos += 7;
   case ' ':
      switch (lexer) {
      case lex_leadingspace:
      case lex_whitespace:
      case lex_comment_c:
      case lex_comment_line:
      case lex_string_quoted: // TODO табуляция внутри строк?
      case lex_string_dquoted:
         goto next_char;
      case lex_number:
         assert(0);
         goto next_char;
      case lex_identifier:
         lexer = lex_whitespace;
         semantic = ss_identifier;
         goto next_char;
      }

   // Конец строки.
   case '\r':
      if (src != end && *src == '\n') {
         ++src;
      }
   case '\n':
      switch (lexer) {
      case lex_leadingspace:
      case lex_whitespace:
      case lex_comment_c:
         goto next_line;
      case lex_comment_line:
         lexer = lex_whitespace;
         goto next_line;
      case lex_string_quoted:
      case lex_string_dquoted:
         syntax_error(st, "отсутствует закрывающая кавычка", line_num, pos, line, end);
         goto error;
      case lex_number:
         assert(0);
         goto next_char;
      case lex_identifier:
         lexer = lex_whitespace;
         semantic = ss_identifier;
         goto next_char;
      }

   // Начинает строку комментариев, либо может завершать комментарий в стиле Си.
   case '*':
      switch (lexer) {
      case lex_number:
         assert(0);

      case lex_leadingspace:
         lexer = lex_comment_line;
         goto next_char;
      case lex_comment_c:
         if (src != end && *src == '/') {
            ++src;
            lexer = lex_whitespace;
         }
         goto next_char;
      case lex_comment_line:
      case lex_string_quoted:
      case lex_string_dquoted:
         goto next_char;
      case lex_whitespace:
         assert(0);
//         goto operator;
         break;
      }

   // Начинает комментарий в стиле Си.
   case '/':
      assert(0);
      break;

   // Разделяет части предложения (выражение-образец слева от общего выражения
   // справа) в блоке функции, либо идентификатор простой функции от общего
   // выражения (расширение Рефал-5: entry = <Prout "Привет!">;).
   case '=':
      switch (lexer) {
      case lex_comment_c:
      case lex_comment_line:
      case lex_string_quoted:
      case lex_string_dquoted:
         goto next_char;
      case lex_number:
         assert(0);
         goto next_char;
      case lex_leadingspace:
      case lex_whitespace:
      case lex_identifier:
         switch (semantic) {
         case ss_source:
            goto error_identifier_missing;
         case ss_identifier:
            command = rf_alloc_command(vm, rf_equal);
            lexer = lex_whitespace;
            semantic = ss_expression;
            goto next_char;
         case ss_expression:
            syntax_error(st, "недопустимый оператор в выражении (пропущена ; ?)", line_num, pos, line, end);
            goto error;
         }
      }

   case '<':
      switch (lexer) {
      case lex_comment_c:
      case lex_comment_line:
      case lex_string_quoted:
      case lex_string_dquoted:
         goto next_char;
      case lex_number:
         assert(0);
         goto next_char;
      case lex_identifier:    // TODO в выражении после идентификатора?
      case lex_leadingspace:
      case lex_whitespace:
         switch (semantic) {
         case ss_source:
            goto error_identifier_missing;
         case ss_identifier:
            syntax_error(st, "некорректное определение функции (пропущено = или { ?)", line_num, pos, line, end);
            goto error;
         case ss_expression:
            command = rf_alloc_command(vm, rf_execute);
            lexer = lex_whitespace;
            goto next_char;
         }
      }

   // Начинает и заканчивает строку знаковых символов.
   case '"':
   case '\'':
      switch (lexer) {
      case lex_number:
         lexer = chr == '"' ? lex_string_dquoted : lex_string_quoted;
//         string = src;
//         store_number(number);
         assert(0);
         goto next_char;
      case lex_leadingspace:
      case lex_whitespace:
         lexer = chr == '"' ? lex_string_dquoted : lex_string_quoted;
//         string = src;
         assert(0);
         goto next_char;
      case lex_string_dquoted:
      case lex_string_quoted:
         // Кавычка внутри строки кодируется сдвоенной, иначе завершает строку.
         if (src != end && *src == (lexer == lex_string_quoted ? '\'':'"')) {
            ++src;
            assert(0);
            goto next_char;
         } else {
            lexer = lex_whitespace;
            goto next_char;
         }
      case lex_comment_c:
      case lex_comment_line:
         goto next_char;
      }

   // Начало целого числа, либо продолжение идентификатора.
   case '0'...'9':
      switch (lexer) {
      case lex_leadingspace:
      case lex_whitespace:
         lexer = lex_number;
         number = chr - '0';
         goto next_char;
      case lex_number:
         number = number * 10 + (chr - '0');
         if (number < (chr - '0')) {
            warning(st, "целочисленное переполнение", line_num, pos, line, end);
         }
         goto next_char;
      case lex_string_quoted:
      case lex_string_dquoted:
      case lex_comment_c:
      case lex_comment_line:
         goto next_char;
      case lex_identifier:
         goto lexem_identifier;
      }
      break;

   // Оставшиеся символы считаются допустимыми для идентификаторов.
   default:
      switch (lexer) {
      case lex_number:
         assert(0);
      case lex_leadingspace:
      case lex_whitespace:
         lexer = lex_identifier;
         switch (semantic) {
         case ss_source:
            node = rtrie_insert_first(ids, chr);
            goto next_char;
         case ss_identifier:
            goto error_identifier_odd;
         case ss_expression:
            node = rtrie_find_first(ids, chr);
            if (node < 0) {
               goto error_identifier_undefined;
            }
            goto next_char;
         }
      case lex_identifier:
lexem_identifier:
         switch (semantic) {
         case ss_source:
            node = rtrie_insert_next(ids, node, chr);
            goto next_char;
         case ss_identifier:
            goto error_identifier_odd;
         case ss_expression:
            node = rtrie_find_next(ids, node, chr);
            if (node < 0) {
               goto error_identifier_undefined;
            }
            goto next_char;
         }
      case lex_string_quoted:
      case lex_string_dquoted:
      case lex_comment_c:
      case lex_comment_line:
         goto next_char;
      }
      break;
   }
   assert(0);

complete:
error:
   return begin - src;

error_identifier_missing:
   syntax_error(st, "пропущено имя функции", line_num, pos, line, end);
   goto error;

error_identifier_odd:
   syntax_error(st, "лишний идентификатор (пропущено = или { в определении функции?)", line_num, pos, line, end);
   goto error;

error_identifier_undefined:
   syntax_error(st, "идентификатор не определён", line_num, pos, line, end);
   goto error;
}
