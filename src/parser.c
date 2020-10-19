
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
   rtrie_index node = 0;      // последний добавленный в префиксное дерево узел.
   rf_int number = 0;         // вычисляемое значение лексемы "число"

   rf_index cmd_execute = 0;      // ячейка с командой rf_execute.
   int execution_bracket_count = 0;

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

   wchar_t chr = *(const unsigned char*)src++;
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
         goto next_char;
      case lex_string_quoted: // TODO табуляция внутри строк?
      case lex_string_dquoted:
         goto lexem_string;
      case lex_number:
         assert(0);
         goto next_char;
      case lex_identifier:
lexem_identifier_complete:
         lexer = lex_whitespace;
         switch (semantic) {
         case ss_identifier:  // TODO 2 идентификатора подряд?
            assert(0);        // Д.б обработано при появлении 2го в error_identifier_odd
         case ss_source:
            semantic = ss_identifier;
            goto next_char;
         case ss_expression:
            if (!(node < 0) && ids->n[node].val.tag != rft_undefined) {
               // TODO заменить символы идентификатора связанным значением
               // из префиксного дерева (пока символы не копируются, см. далее).
               assert(cmd_execute);
               vm->cell[cmd_execute].data = rtrie_val_to_raw(ids->n[node].val);
               cmd_execute = 0;
            } else {
               // TODO РЕФАЛ позволяет объявлять функции после использования в тексте.
               goto error_identifier_undefined;
            }
            goto next_char;
         }
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
         goto lexem_identifier_complete;
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
      case lex_comment_line:
         goto next_char;
      case lex_string_quoted:
      case lex_string_dquoted:
         goto lexem_string;
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
         goto next_char;
      case lex_string_quoted:
      case lex_string_dquoted:
         goto lexem_string;
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
            if (ids->n[node].val.tag != rft_undefined) {
               // TODO надо бы отобразить прежнее определение
               syntax_error(st, "повторное определение функции", line_num, pos, line, end);
               goto error;
            }
            ids->n[node].val.value = rf_alloc_command(vm, rf_equal);
            ids->n[node].val.tag   = rft_byte_code;
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
         goto next_char;
      case lex_string_quoted:
      case lex_string_dquoted:
         goto lexem_string;
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
            goto error_incorrect_function_definition;
         case ss_expression:
            cmd_execute = rf_alloc_command(vm, rf_execute);
            ++execution_bracket_count;
            lexer = lex_whitespace;
            goto next_char;
         }
      }

   case '>':
      switch (lexer) {
      case lex_comment_c:
      case lex_comment_line:
         goto next_char;
      case lex_string_quoted:
      case lex_string_dquoted:
         goto lexem_string;
      case lex_number:
         assert(0);
         goto next_char;
      case lex_identifier:
      case lex_leadingspace:
      case lex_whitespace:
         switch (semantic) {
         case ss_source:
            goto error_identifier_missing;
         case ss_identifier:
            goto error_incorrect_function_definition;
         case ss_expression:
            if (!execution_bracket_count) {
               syntax_error(st, "непарная вычислительная скобка", line_num, pos, line, end);
               goto error;
            }
            assert(execution_bracket_count > 0);
            rf_alloc_command(vm, rf_execute_close);
            --execution_bracket_count;
            lexer = lex_whitespace;
            goto next_char;
         }
      }

   case ';':
      switch (lexer) {
      case lex_comment_c:
      case lex_comment_line:
         goto next_char;
      case lex_string_dquoted:
      case lex_string_quoted:
            goto lexem_string;
      case lex_number:
         assert(0);
         goto next_char;
      case lex_identifier:
      case lex_leadingspace:
      case lex_whitespace:
         switch (semantic) {
         case ss_source:
            goto error_identifier_missing;
         // Идентификатор пустой функции (ENUM в Refal-05).
         case ss_identifier:
            ids->n[node].val.tag   = rft_byte_code,
            ids->n[node].val.value = -1,
            lexer = lex_whitespace;
            semantic = ss_source;
            goto next_char;
         case ss_expression:
            if (execution_bracket_count) {
               syntax_error(st, "не закрыта вычислительная скобка", line_num, pos, line, end);
               goto error;
            }
            semantic = ss_source;
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
         switch (semantic) {
         case ss_source:
            goto error_identifier_missing;
         case ss_identifier:
            syntax_error(st, "строка неуместна (пропущено = или { в определении функции?)", line_num, pos, line, end);
            goto error;
         case ss_expression:
            lexer = chr == '"' ? lex_string_dquoted : lex_string_quoted;
            goto next_char;
         }
      case lex_string_dquoted:
      case lex_string_quoted:
         // Кавычка внутри строки кодируется сдвоенной, иначе завершает строку.
         if (src != end && *src == (lexer == lex_string_quoted ? '\'':'"')) {
            ++src;
            goto lexem_string;
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
         goto lexem_string;
      case lex_comment_c:
      case lex_comment_line:
         goto next_char;
      case lex_identifier:
         goto lexem_identifier;
      }
      break;

   // Оставшиеся символы считаются допустимыми для идентификаторов.
   default:
      // Декодируем UTF-8
      switch (chr) {
      // ASCII
      case 0x00 ... 0x7f:
         break;
      // 2 байта на символ.
      // TODO 0xc0 0xc1 кодируют символы из диапазона ASCII
      case 0xc0 ... 0xdf:
         chr = 0x1f & chr;
utf8_0:  if (src == end)
            goto error_utf8_incomplete;
         // TODO нет проверки на диапазон байта продолжения.
         chr = (chr << 6) | (0x3f & *(const unsigned char*)src++);
         break;
      // 3 байта на символ.
      case 0xe0 ... 0xef:
         chr = (0xf & chr);
utf8_1:  if (src == end)
            goto error_utf8_incomplete;
         chr = (chr << 6) | (0x3f & *(const unsigned char*)src++);
         goto utf8_0;
      // 4 байта на символ.
      case 0xf0 ... 0xf4:
         chr = (0x3 & chr);
         if (src == end)
            goto error_utf8_incomplete;
         chr = (chr << 6) | (0x3f & *(const unsigned char*)src++);
         goto utf8_1;
      // байты продолжения — не должны идти первыми
      case 0x80 ... 0xbf:
      case 0xf5 ... 0xff:
      default:
         syntax_error(st, "недействительный символ UTF-8", line_num, pos, line, end);
         goto error;
      }

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
            // Если идентификатор уже определён, можно будет заменить его значением.
            node = rtrie_find_first(ids, chr);
#if 0
            rf_alloc_atom(vm, chr);
#endif
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
#if 0
            rf_alloc_atom(vm, chr);
#endif
            goto next_char;
         }
      case lex_string_quoted:
      case lex_string_dquoted:
lexem_string:
         rf_alloc_char(vm, chr);
         goto next_char;
      case lex_comment_c:
      case lex_comment_line:
         goto next_char;
      }
      break;
   }
   assert(0);

complete:
error:
   return src - begin;

error_incorrect_function_definition:
   syntax_error(st, "некорректное определение функции (пропущено = или { ?)", line_num, pos, line, end);
   goto error;

error_identifier_missing:
   syntax_error(st, "пропущено имя функции", line_num, pos, line, end);
   goto error;

error_identifier_odd:
   syntax_error(st, "лишний идентификатор (пропущено = или { в определении функции?)", line_num, pos, line, end);
   goto error;

error_identifier_undefined:
   syntax_error(st, "идентификатор не определён", line_num, pos, line, end);
   goto error;

error_utf8_incomplete:
   syntax_error(st, "неполный символ UTF-8", line_num, pos, line, end);
   goto error;
}