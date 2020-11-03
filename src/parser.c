
#include <assert.h>
#include <stddef.h>

#include "rtrie.h"
#include "message.h"
#include "refal.h"

/** При трансляции выдаётся замечание о копировании переменной (дорогая операция).*/
#define REFAL_PERFORMANCE_NOTICE_EVAR_COPY

enum lexer_state {
   lex_leadingspace,    ///< Пробелы в начале строки.
   lex_whitespace,      ///< Пробелы после лексемы.
   lex_comment_line,    ///< Строка комментария, начинается со *
   lex_comment_c,       ///< Комментарии в стиле C /* */
   lex_string_quoted,   ///< Знаковая строка в одинарных кавычках.
   lex_string_dquoted,  ///< Знаковая строка в двойных кавычках.
   lex_number,          ///< Целое число.
   lex_identifier,      ///< Идентификатор (имя функции).
//   lex_operator,        ///< Оператор.
};

enum semantic_state {
   ss_source,           ///< Верхний уровень синтаксиса.
   ss_identifier,       ///< Идентификатор.
   ss_pattern,          ///< Выражение-образец.
   ss_expression,       ///< Общее выражение (простой функции).
};

enum id_type {
   id_global = rf_identifier,
   id_svar   = rf_svar,       ///< s-переменная (один символ).
   id_tvar   = rf_tvar,       ///< t-переменная (s- либо выражение в скобках).
   id_evar   = rf_evar,       ///< e-переменная (произвольное количество элементов.
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

   // последний узел глобального идентификатора (функции),
   // используется как корень для локальных (переменных).
   rtrie_index ident = 0;

   enum id_type id_type = id_global;
   // Локальные идентификаторы храним в таблице символов как продолжение
   // глобальных, отделяясь символом который не может встретиться в Unicode.
   // Для каждого предложения вычисляется новый символ-разделитель (инкрементом),
   // что гарантирует уникальность записей в таблице. Таким образом
   // объявление s.1 в первом предложении не будет видно в следующих.
   wchar_t idc = 1 + 0x10FFFF;   // отделяет локальный идентификатор от корневого.
   rf_index local = 0;           // значение локального идентификатора.
   // Поскольку все кроме одного вхождения e-переменной в выражении-результате
   // приходится копировать, используем массив для отслеживания их компиляций.
   rf_index local_max = 100;
   struct {
#ifdef REFAL_PERFORMANCE_NOTICE_EVAR_COPY
      const char  *src;
      unsigned    line;
      unsigned    pos;
#endif
      rf_index    opcode;
   } var[local_max];

   // Поскольку в общем выражении вызовы функций могут быть вложены,
   // индексы ячеек с командой rf_execute организованы в стек.
   const int exec_max = 100;
   rf_index cmd_exec[exec_max];
   int ep = 0;
   cmd_exec[ep] = 0;

   // Значение задаётся пустым функциям (ENUM в Refal-05) для сопоставления.
   rf_index enum_couner = 0;

   unsigned line_num = 0;     // номер текущей строки
   enum semantic_state semantic = ss_source;

   rf_index cmd_sentence = 0; // ячейка с командой rf_sentence.
   int function_block = 0;    // подсчитывает блоки в функции (фигурные скобки).

next_line:
   ++line_num;
   const char *line = src;    // начало текущей строки
   unsigned pos = 0;          // номер символа в строке
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
         rf_alloc_int(vm, number);
         lexer = lex_whitespace;
         goto next_char;
      case lex_identifier:
lexem_identifier_complete:
         lexer = lex_whitespace;
         switch (semantic) {
         case ss_identifier:  // TODO 2 идентификатора подряд?
            assert(0);        // Д.б обработано при появлении 2го в error_identifier_odd
         case ss_source:
            semantic = ss_identifier;
            ident = node;
            goto next_char;
         case ss_pattern:
            assert(!cmd_exec[ep]);
            switch (id_type) {
            case id_svar:
            case id_tvar:
            case id_evar:
               if (local == local_max) {
                  syntax_error(st, "превышен лимит переменных", line_num, pos, line, end);
                  goto error;
               }
               if (ids->n[node].val.tag == rft_undefined) {
                  var[local].opcode = 0;
                  ids->n[node].val.tag   = rft_enum;
                  ids->n[node].val.value = local++;
               }
               rf_alloc_value(vm, ids->n[node].val.value, id_type);
               goto next_char;
            case id_global:
               goto lexem_identifier_complete_global;
            }
         case ss_expression:
            switch (id_type) {
            case id_svar:
            case id_tvar:
            case id_evar:
               if (ids->n[node].val.tag == rft_undefined) {
                  goto error_identifier_undefined;
               }
               // При первом вхождении создаём id_evar и запоминаем её индекс.
               // При следующем вхождении меняем значение по сохранённому
               // индексу на id_evar_copy и индекс на текущий.
               rf_index id = ids->n[node].val.value;
               if (id_type == id_evar && var[id].opcode) {
                  vm->cell[var[id].opcode].tag = rf_evar_copy;
#ifdef REFAL_PERFORMANCE_NOTICE_EVAR_COPY
                  performance(st, "создаётся копия e-переменной", var[id].line,
                                  var[id].pos, var[id].src, end);
#endif
               }
               var[id].opcode = rf_alloc_value(vm, id, id_type);
#ifdef REFAL_PERFORMANCE_NOTICE_EVAR_COPY
               var[id].src  = line;
               var[id].line = line_num;
               var[id].pos  = pos;  // TODO указывает на последующий пробел.
#endif
               goto next_char;
            case id_global:
               break;
            }
lexem_identifier_complete_global:
            if (!(node < 0) && ids->n[node].val.tag != rft_undefined) {
               // Если открыта вычислительная скобка, задаём ей адрес
               // первой вычислимой функции из выражения.
               if (ids->n[node].val.tag != rft_enum && cmd_exec[ep]
               && rtrie_val_from_raw(vm->cell[cmd_exec[ep]].data).tag == rft_undefined) {
                  vm->cell[cmd_exec[ep]].data = rtrie_val_to_raw(ids->n[node].val);
               } else {
                  rf_alloc_value(vm, rtrie_val_to_raw(ids->n[node].val), rf_identifier);
               }
               // TODO заменить символы идентификатора связанным значением
               // из префиксного дерева (пока символы не копируются, см. далее).
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
      case lex_comment_line:
         goto next_line;
      case lex_string_quoted:
      case lex_string_dquoted:
         syntax_error(st, "отсутствует закрывающая кавычка", line_num, pos, line, end);
         goto error;
      case lex_number:
         rf_alloc_int(vm, number);
         goto next_line;
      case lex_identifier:
         --src; --pos;
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
      case lex_identifier:
         --src; --pos;
         goto lexem_identifier_complete;
      case lex_number:
         rf_alloc_int(vm, number);
      case lex_leadingspace:
      case lex_whitespace:
         switch (semantic) {
         case ss_source:
            goto error_identifier_missing;
         case ss_identifier:
            assert(function_block == 0);
            if (ids->n[node].val.tag != rft_undefined) {
               // TODO надо бы отобразить прежнее определение
               syntax_error(st, "повторное определение функции", line_num, pos, line, end);
               goto error;
            }
            cmd_sentence = 0;
            ids->n[node].val.value = rf_alloc_command(vm, rf_equal);
            ids->n[node].val.tag   = rft_byte_code;
            lexer = lex_whitespace;
            semantic = ss_expression;
            goto next_char;
         case ss_pattern:
            // TODO проверить скобки ().
            rf_alloc_command(vm, rf_equal);
            lexer = lex_whitespace;
            semantic = ss_expression;
            goto next_char;
         case ss_expression:
            syntax_error(st, "недопустимый оператор в выражении (пропущена ; ?)", line_num, pos, line, end);
            goto error;
         }
      }

   // Начинает блок (перечень предложений) функции.
   case '{':
      switch (lexer) {
      case lex_comment_c:
      case lex_comment_line:
         goto next_char;
      case lex_string_quoted:
      case lex_string_dquoted:
         goto lexem_string;
      case lex_number:
         // TODO Вложенные блоки пока не поддержаны.
         goto error_incorrect_function_definition;
      case lex_identifier:
         --src; --pos;
         goto lexem_identifier_complete;
      case lex_leadingspace:
      case lex_whitespace:
         switch (semantic) {
         case ss_source:
            goto error_identifier_missing;
         case ss_identifier:
            assert(function_block == 0);
            if (ids->n[node].val.tag != rft_undefined) {
               // TODO надо бы отобразить прежнее определение
               syntax_error(st, "повторное определение функции", line_num, pos, line, end);
               goto error;
            }
            cmd_sentence = rf_alloc_command(vm, rf_sentence);
            ids->n[node].val.value = cmd_sentence;
            ids->n[node].val.tag   = rft_byte_code;
            lexer = lex_whitespace;
            semantic = ss_pattern;
            local = 0;
            ++idc;
            ++function_block;
            goto next_char;
         case ss_pattern:
            syntax_error(st, "блок недопустим в выражении (пропущено = ?)", line_num, pos, line, end);
            goto error;
         case ss_expression:
            syntax_error(st, "вложенные блоки {} пока не поддерживаются", line_num, pos, line, end);
            goto error;
         }
      }

   case '}':
      switch (lexer) {
      case lex_comment_c:
      case lex_comment_line:
         goto next_char;
      case lex_string_dquoted:
      case lex_string_quoted:
            goto lexem_string;
      case lex_identifier:
         --src; --pos;
         goto lexem_identifier_complete;
      case lex_number:
         rf_alloc_int(vm, number);
         lexer = lex_whitespace;
      case lex_leadingspace:
      case lex_whitespace:
         switch (semantic) {
         case ss_source:
            goto error_identifier_missing;
         case ss_identifier:
            goto error_incorrect_function_definition;
         // После последнего предложения ; может отсутствовать.
         case ss_expression:
            assert(cmd_sentence);
            assert(function_block > 0);
            --function_block;
            cmd_sentence = 0;
            goto sentence_complete;
         // ; начинает предложение-образец, пустой в случае завершения функции.
         case ss_pattern:
            assert(function_block > 0);
            --function_block;
            if (!rf_is_evar_empty(vm, cmd_sentence, vm->free)) {
               syntax_error(st, "образец без общего выражения (пропущено = ?)", line_num, pos, line, end);
               goto error;
            }
            vm->cell[cmd_sentence].tag = rf_complete;
            semantic = ss_source;
            goto next_char;
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
      case lex_identifier:
         --src; --pos;
         goto lexem_identifier_complete;
      case lex_number:
         rf_alloc_int(vm, number);
      case lex_leadingspace:
      case lex_whitespace:
         switch (semantic) {
         case ss_source:
            goto error_identifier_missing;
         case ss_identifier:
            goto error_incorrect_function_definition;
         case ss_pattern:
            goto error_executor_in_pattern;
         case ss_expression:
            if (!(ep < exec_max)) {
               syntax_error(st, "превышен лимит вложенности вычислительных скобок", line_num, pos, line, end);
            }
            ++ep;
            cmd_exec[ep] = rf_alloc_command(vm, rf_execute);
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
      case lex_identifier:
         --src; --pos;
         goto lexem_identifier_complete;
      case lex_number:
         rf_alloc_int(vm, number);
      case lex_leadingspace:
      case lex_whitespace:
         switch (semantic) {
         case ss_source:
            goto error_identifier_missing;
         case ss_identifier:
            goto error_incorrect_function_definition;
         case ss_pattern:
            goto error_executor_in_pattern;
         case ss_expression:
            if (!ep) {
               syntax_error(st, "непарная вычислительная скобка", line_num, pos, line, end);
               goto error;
            }
            assert(ep > 0);
            // Копируем адрес функции из парной открывающей, для вызова интерпретатором.
            if (rtrie_val_from_raw(vm->cell[cmd_exec[ep]].data).tag == rft_undefined) {
               syntax_error(st, "активное выражение должно содержать имя функции", line_num, pos, line, end);
               goto error;
            }
            rf_alloc_value(vm, vm->cell[cmd_exec[ep]].data, rf_execute_close);
            cmd_exec[ep--] = 0;
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
      case lex_identifier:
         --src; --pos;
         goto lexem_identifier_complete;
      case lex_number:
         rf_alloc_int(vm, number);
         lexer = lex_whitespace;
      case lex_leadingspace:
      case lex_whitespace:
         switch (semantic) {
         case ss_source:
            goto error_identifier_missing;
         // Идентификатор пустой функции (ENUM в Refal-05).
         case ss_identifier:
            ids->n[node].val.tag   = rft_enum;
            ids->n[node].val.value = ++enum_couner;
            lexer = lex_whitespace;
            semantic = ss_source;
            goto next_char;
         case ss_pattern:
            syntax_error(st, "образец без общего выражения (пропущено = ?)", line_num, pos, line, end);
            goto error;
         case ss_expression:
sentence_complete:
            if (ep) {
               syntax_error(st, "не закрыта вычислительная скобка", line_num, pos, line, end);
               goto error;
            }
            // В функциях с блоком сохраняем в маркере текущего предложения
            // ссылку на данные следующего и размещаем новый маркер.
            if (cmd_sentence) {
               rf_index new_sentence = rf_alloc_command(vm, rf_sentence);
               vm->cell[cmd_sentence].data = new_sentence;
               cmd_sentence = new_sentence;
               semantic = ss_pattern;
               local = 0;
               ++idc;
            } else {
               rf_alloc_command(vm, rf_complete);
               semantic = ss_source;
            }
            goto next_char;
         }
      }

   // Начинает и заканчивает строку знаковых символов.
   case '"':
   case '\'':
      switch (lexer) {
      case lex_number:
         rf_alloc_int(vm, number);
         lexer = chr == '"' ? lex_string_dquoted : lex_string_quoted;
         goto next_char;
      case lex_leadingspace:
      case lex_whitespace:
         switch (semantic) {
         case ss_source:
            goto error_identifier_missing;
         case ss_identifier:
            syntax_error(st, "строка неуместна (пропущено = или { в определении функции?)", line_num, pos, line, end);
            goto error;
         case ss_pattern:
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
      case lex_identifier:
         --src; --pos;
         goto lexem_identifier_complete;
      }

   // Начало целого числа, либо продолжение идентификатора.
   case '0'...'9':
      switch (lexer) {
      case lex_leadingspace:
      case lex_whitespace:
         switch (semantic) {
         case ss_source:
         case ss_identifier:
            syntax_error(st, "числа допустимы только в выражениях", line_num, pos, line, end);
            goto error;
         case ss_pattern:
         case ss_expression:
            lexer = lex_number;
            number = chr - '0';
            goto next_char;
         }
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
         case ss_pattern:
            // Возможно объявление и использование переменных.
            // Сохраняем их в таблице символов, отделив от идентификатора
            // текущей функции спецсимволом.
            switch (chr) {
            case 'e':
               id_type = id_evar;
               goto lexem_identifier_local_pat_check;
            case 't':
               id_type = id_tvar;
               goto lexem_identifier_local_pat_check;
            case 's':
               id_type = id_svar;
lexem_identifier_local_pat_check:
               if (src != end && *src == '.') {
                  ++src; ++pos;
                  node = rtrie_insert_next(ids, ident, idc);
                  node = rtrie_insert_next(ids, node, chr);
                  goto next_char;
               }
            default:
               goto lexem_identifier_global;
            }
         case ss_expression:
            // Возможно использование переменных.
            switch (chr) {
            case 'e':
               id_type = id_evar;
               goto lexem_identifier_local_exp_check;
            case 't':
               id_type = id_tvar;
               goto lexem_identifier_local_exp_check;
            case 's':
               id_type = id_svar;
lexem_identifier_local_exp_check:
               if (src != end && *src == '.') {
                  ++src; ++pos;
                  node = rtrie_find_next(ids, ident, idc);
                  node = rtrie_find_next(ids, node, chr);
                  if (node < 0) {
                     goto error_identifier_undefined;
                  }
                  goto next_char;
               }
            default:
lexem_identifier_global:
               id_type = id_global;
               // Если идентификатор уже определён, можно будет заменить его значением.
               node = rtrie_find_first(ids, chr);
#if 0
               rf_alloc_atom(vm, chr);
#endif
               goto next_char;
            }
         }
      case lex_identifier:
lexem_identifier:
         switch (semantic) {
         case ss_source:
            node = rtrie_insert_next(ids, node, chr);
            goto next_char;
         case ss_identifier:
            goto error_identifier_odd;
         case ss_pattern:
            // Переменные объявляются в выражении-образце.
            switch (id_type) {
            case id_svar:
            case id_tvar:
            case id_evar:
               node = rtrie_insert_next(ids, node, chr);
               goto next_char;
            case id_global:
               break;
            }
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
   if (semantic != ss_source) {
      if (function_block)
         syntax_error(st, "не завершено определение функции (пропущена } ?)", line_num, pos, line, end);
      else
         syntax_error(st, "не завершено определение функции (пропущена ; ?)", line_num, pos, line, end);
   }

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

error_executor_in_pattern:
   syntax_error(st, "вычислительные скобки в образце не поддерживаются", line_num, pos, line, end);
   goto error;

error_utf8_incomplete:
   syntax_error(st, "неполный символ UTF-8", line_num, pos, line, end);
   goto error;
}
