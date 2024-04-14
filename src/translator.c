
#define _POSIX_C_SOURCE 1

#include "translator.h"
#include "library.h"

#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>


int refal_translate_file_to_bytecode(
      struct refal_translator_config   *cfg,
      struct refal_vm      *vm,
      struct refal_trie    *ids,
      const char           *name,
      struct refal_message *st)
{
   int r = -1;
   const char *os = refal_message_source(st, name);
   FILE * src = fopen(name, "r");
   if (!src) {
      critical_error(st, "исходный текст недоступен", -errno, 0);
   } else {
      r = refal_translate_istream_to_bytecode(cfg, vm, ids, 0, src, st);
      fclose(src);
   }
   refal_message_source(st, os);
   return r;
}

int refal_translate_module_to_bytecode(
      struct refal_translator_config   *cfg,
      struct refal_vm      *vm,
      struct refal_trie    *ids,
      rtrie_index          module,
      const char           *name,
      size_t               name_length,
      struct refal_message *st)
{
   int r = -1;
   // Ищем в каталоге с исходным текстом файлы модуля.
   static const char ext1[] = ".реф";
   static const char ext2[] = ".ref";
   if (name_length + sizeof ext1 >= NAME_MAX) {
      return -2;
   }
   unsigned pl = 0;
   if (st && st->source) {
      for (unsigned i = 0; st->source[i]; ++i) {
         if (i == PATH_MAX - NAME_MAX) {
            pl = 0;
            break;
         // TODO разделитель может быть другой.
         } else if (st->source[i] == '/')
            pl = i + 1;
      }
   }
   char path[PATH_MAX];
   if (pl)
      strncpy(path, st->source, pl);
   strncpy(&path[pl], name, name_length);
   const char *const ext[2] = { ext2, ext1 };
   for (int i = 1; i >= 0; --i) {
      strcpy(&path[pl + name_length], ext[i]);
      FILE * src = fopen(name, "r");
      if (src != MAP_FAILED) {
         const char *os = refal_message_source(st, path);
         r = refal_translate_istream_to_bytecode(cfg, vm, ids, module, src, st);
         fclose(src);
         refal_message_source(st, os);
         if (r >= 0)
            break;
      }
   }
   if (r == -1) {
      strcpy(&path[pl + name_length], ext1);
      const char *os = refal_message_source(st, path);
      critical_error(st, "исходный текст модуля недоступен", -errno, 0);
      refal_message_source(st, os);
   }
   return r;
}

/**
 * Индекс для адресации ячеек массива, где хранятся строки.
 */
typedef size_t wstr_index;

struct wstr {
   wchar_t     *s;
   wstr_index  size;
   wstr_index  free;
};

/**
 * Резервирует память для хранения строк.
 * \result Ненулевое значение в случае успеха.
 */
static inline
void *wstr_alloc(
      struct wstr *ws, ///< Структура для инициализации.
      wstr_index  size)     ///< Предполагаемый размер (в символах).
{
   ws->s = refal_malloc(size * sizeof(*ws->s));
   ws->size = ws->s ? size : 0;
   ws->free = 0;
   return ws->s;
}

/**
 * Проверяет состояние структуры.
 *
 * \result Ненулевой результат, если память распределена.
 */
static inline
void *wstr_check(
      struct wstr          *ws,
      struct refal_message *status)
{
   assert(ws);
   if (status && !ws->s) {
      critical_error(status, "недостаточно памяти для строк", -errno, ws->size);
   }
   return ws->s;
}

/**
 * Освобождает занятую память.
 */
static inline
void wstr_free(struct wstr *ws)
{
   refal_free(ws->s, ws->size);
   ws->s = 0;
   ws->size = 0;
   ws->free = 0;
}

/**
 * Добавляет символ в хвост массива, при необходимости увеличивая размер буфера.
 */
static inline
wstr_index wstr_append(struct wstr *ws, wchar_t c)
{
   wstr_index new_chr = ws->free++;
   if (new_chr == ws->size) {
      size_t size = ws->size * sizeof(sizeof(*ws->s));
      ws->s = refal_realloc(ws->s, size, 2 * size);
      ws->size *= 2;
   }
   if (ws->s)
      ws->s[new_chr] = c;
   else
      new_chr = -1;
   return new_chr;
}


int refal_translate_istream_to_bytecode(
      struct refal_translator_config   *cfg,
      struct refal_vm      *const vm,
      struct refal_trie    *const ids,
      rtrie_index          module,
      FILE                 *src,
      struct refal_message *st)
{
   // Сообщение об ошибке при завершении.
   const char *error = NULL;

   // Здесь храним обрабатываемый исходный текст для сообщений об ошибках.
   // Поскольку определение идентификаторов возможно после их использование,
   // и после первого прохода производится дополнительный, сохраняется
   // весь входной поток целиком.
   struct wstr buf;
   wstr_alloc(&buf, 1024);

   // При сканировании лексем "число" здесь накапливается результат.
   rf_int number = 0;

   // Идентификаторы заносятся в таблицу символов параллельно разбору исходного
   // текста. Здесь хранится текущий добавленный в префиксное дерево узел.
   rtrie_index node = 0;

   // Последний узел глобального идентификатора (текущей функции).
   // Используется как корень для локальных (переменных)
   // и для возможности изменить тип функции (с пустой на вычислимую).
   rtrie_index ident = 0;

   // Текущее пространство имён, куда заносятся идентификаторы.
   // Имена модулей дублируются так же в глобальном пространстве,
   // при импорте модуля временно меняется на 0.
   rtrie_index namespace = module;

   // Начало и длина имени идентификатора. Для импорта модулей.
   wstr_index  ident_str = 0;
   size_t      ident_strlen = 0;
   // Номер первого символа идентификатора в строке. Так же и для вывода ошибки.
   unsigned    ident_pos = 0;

   // Пространство имён текущего импортируемого модуля.
   // Кроме того, идентификатор модуля может встречаться и в выражениях,
   // определяя область поиска следующего за ним идентификатора.
   rtrie_index imports = 0;

   // Импортированное в текущую область видимости имя модуля.
   // Используется для связи с ветвью идентификаторов модуля.
   rtrie_index imports_local = 0;

   // Для вывода предупреждений об идентификаторах модулей.
   const char *redundant_module_id = "идентификатор модуля без функции не имеет смысла";
   wstr_index im_str = 0; //TODO Несколько импортов в одной строке?
   unsigned   im_line = 0;
   unsigned   im_pos  = 0;

   // При импорте идентификаторов параллельно с определением в текущей
   // области видимости происходит поиск в пространстве имён модуля.
   rtrie_index import_node = 0;

   // Значение задаётся пустым функциям (ENUM в Refal-05) для сопоставления.
   // Применяется предварительный инкремент, так что нумерация идёт минимум с 1.
   // Значение 0 используется для идентификаторов модулей (при разрешении имён)
   // и в целевой код не переносится.
   // Что бы получить уникальные для каждого модуля значения, используем номер
   // свободной ячейки — количество занятых не превышает количество
   // сгенерированных значений.
   rf_index enum_couner = ids->free;

   // РЕФАЛ позволяет произвольный порядок определения функций.
   // При последовательном проходе не все идентификаторы определены, такие
   // сопровождаются дополнительной информацией и организуются в список,
   // границы хранятся здесь.
   rf_index undefined_fist = 0;
   rf_index undefined_last = 0;

   // Тип текущего идентификатора.
   enum {
      id_global = rf_identifier, // Глобальная область видимости.
      id_svar   = rf_svar,       // s-переменная (один символ).
      id_tvar   = rf_tvar,       // t-переменная (s- либо выражение в скобках).
      id_evar   = rf_evar,       // e-переменная (произвольное количество элементов.
   } id_type = id_global;

   // Локальные идентификаторы храним в таблице символов как продолжение
   // глобальных, отделяясь символом который не может встретиться в Unicode.
   // Для каждого предложения вычисляется новый символ-разделитель (инкрементом),
   // что гарантирует уникальность записей в таблице. Таким образом
   // объявление s.1 в первом предложении не будет видно в следующих.
   wchar_t idc = 1 + 0x10FFFF;   // отделяет локальный идентификатор от корневого.
   rf_index local = 0;           // значение локального идентификатора.
   // Поскольку все кроме одного вхождения e-переменной в выражении-результате
   // приходится копировать, используем массив для отслеживания их компиляций.
   rf_index local_max = REFAL_TRANSLATOR_LOCALS_DEFAULT;
   if (cfg && cfg->locals_limit) {
      local_max = cfg->locals_limit;
   } else if (cfg) {
      // TODO можно определить действительный максимум, но, наверное, не нужно.
      cfg->locals_limit = local_max;
   }
   struct {
#ifdef REFAL_TRANSLATOR_PERFORMANCE_NOTICE_EVAR_COPY
      wstr_index  src;
      unsigned    line;
      unsigned    pos;
#endif
      rf_index    opcode;
   } var[local_max];

   // Поскольку в общем выражении вызовы функций могут быть вложены,
   // индексы ячеек с командой rf_execute организованы в стек.
   // 0й элемент используется при обработке идентификаторов и при отсутствии <>,
   // поэтому реальное количество скобок на 1 меньше, чем размер массива.
   unsigned exec_max = REFAL_TRANSLATOR_EXECS_DEFAULT;
   if (cfg && cfg->execs_limit) {
      exec_max = cfg->execs_limit;
   }
   rf_index cmd_exec[exec_max];
   unsigned ep = 0;  // адресует последний занятый элемент,
   cmd_exec[ep] = 0; // изначально пустой (используется не только при закрытии >)

   // Структурные скобки организованы в стек по той же причине.
   // Запоминается адрес открывающей, что бы связать с парной закрывающей.
   unsigned bracket_max = REFAL_TRANSLATOR_BRACKETS_DEFAULT;
   if (cfg && cfg->brackets_limit) {
      bracket_max = cfg->brackets_limit;
   }
   rf_index bracket[bracket_max];
   unsigned bp = 0;  // адресует свободный элемент.

   rf_index cmd_sentence = 0; // ячейка с командой rf_sentence.
   int function_block = 0;    // подсчитывает блоки в функции (фигурные скобки).

   // Состояние семантического анализатора.
   enum {
      // Верхний уровень синтаксиса. Происходит определение идентификаторов.
      ss_source,
      // Определён идентификатор функции или модуля.
      // Следом идёт выражение-образец, блок {}, список импорта (после :)
      // либо определение пустой функции (сразу ;).
      ss_identifier,
      // Определён идентификатор модуля (для стандартной библиотеки — пустой).
      // Следует перечень импортируемых идентификаторов.
      ss_import,
      // Выражение-образец (левая часть предложения до =).
      // Следует общее выражение (результат).
      ss_pattern,
      // Общее выражение (результат). Далее:
      // ; альтернативное предложение функции (исполняется при неудаче текущего)
      // } конец блока (функции).
      ss_expression,
   } semantic = ss_source;

   // Состояние лексического анализатора.
   enum {
      lex_leadingspace,    // Пробелы в начале строки.
      lex_whitespace,      // Пробелы после лексемы.
      lex_comment_line,    // Строка комментария, начинается со *
      lex_comment_c,       // Комментарии в стиле C /* */
      lex_string_quoted,   // Знаковая строка в одинарных кавычках.
      lex_string_dquoted,  // Знаковая строка в двойных кавычках.
      lex_number,          // Целое число.
      lex_identifier,      // Идентификатор (имя функции).
   } lexer  = lex_leadingspace;

   unsigned line_num = 0;     // номер текущей строки

   // В первой строке могут быть символы #!, тогда игнорируем её.
   wint_t wc = fgetwc(src);
   if (wc == '#')
      lexer = lex_comment_line;
   else
      ungetwc(wc, src);

next_line:
   ++line_num;

   // При выводе сообщений об ошибках требуется строка целиком.
   // Исключаем второй символ возможных '\r' '\n' для совместимости.
   // Для простоты разбора (и что бы не проверять возможный выход за буфер
   // при чтении следующего символа) гарантируем перенос строки в конце.
   wstr_index line = buf.free;
   while (1) {
      if ((wc = fgetwc(src)) == WEOF) {
         if (line == buf.free)
            goto complete;
         wc = '\n';
      }
      wstr_append(&buf, wc);
      if (!wstr_check(&buf, st))
         goto cleanup;
      if (wc == '\n')
         break;
      if (wc == '\r') {
         wc = fgetwc(src);
         if (wc != '\n' && wc != WEOF)
            ungetwc(wc, src);
         break;
      }
   }
   unsigned pos = 0;             // номер символа в строке исходника

next_char:
   const wstr_index str = buf.free;  //TODO для импорта
   wchar_t chr = buf.s[line + pos++];

current_char:
   switch (chr) {

   ///\page    Синтаксис
   ///\ingroup refal-syntax
   ///\section lexem          Лексические единицы
   ///
   /// Лексические единицы РЕФАЛ подразделяются на специальные знаки, символы и
   /// переменные. Между лексическими единицами может проставляться любое
   /// количество как пробелов, так и спец.символов табуляции и переноса строки.
   /// Пробелы становятся лексической единицей, когда они появляются в строке,
   /// заключённой в кавычки.

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
         // Ветка определяет идентификатор, а обработку текущего символа
         // производит последующим переходом на current_char.
lexem_identifier_complete:
         lexer = lex_whitespace;
         switch (semantic) {
         case ss_identifier:  // TODO 2 идентификатора подряд?
            assert(0);        // Д.б обработано при появлении 2го в error_identifier_odd
         case ss_source:
            semantic = ss_identifier;
            ident_strlen = str - ident_str;  //TODO для импорта
            ident = node;
            // Функции определяются во множестве мест, вынесено сюда.
            // Производить определение функций здесь возможно, но придётся
            // распознавать ситуацию с импортом модуля, идентификатор которого
            // может встречаться на верхнем уровне многократно.
            local = 0;
            cmd_sentence = 0;
            goto current_char;
         case ss_import:
            if (import_node < 0) {
               error = "идентификатор не определён в модуле (возможно, взаимно-рекурсивный импорт)";
               goto cleanup;
            }
            if (ids->n[node].val.tag != rft_undefined) {
               error = "импортируемый идентификатор уже определён";
               goto cleanup;
            }
            ids->n[node].val = ids->n[import_node].val;
            goto current_char;
         case ss_pattern:
            assert(!cmd_exec[ep]);
            switch (id_type) {
            case id_svar:
            case id_tvar:
            case id_evar:
               if (local == local_max) {
                  error = "превышен лимит переменных";
                  goto cleanup;
               }
               if (ids->n[node].val.tag == rft_undefined) {
                  var[local].opcode = 0;
                  ids->n[node].val.tag   = rft_enum;
                  ids->n[node].val.value = local++;
               }
               rf_alloc_value(vm, ids->n[node].val.value, id_type);
               goto current_char;
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
               // При первом вхождении создаём переменную и запоминаем её индекс.
               // При следующем вхождении устанавливаем значение tag2 по
               // сохранённому индексу, а индекс заменяем на текущий.
               rf_index id = ids->n[node].val.value;
               if (id_type != id_svar && var[id].opcode) {
                  vm->u[var[id].opcode].tag2 = 1;
#ifdef REFAL_TRANSLATOR_PERFORMANCE_NOTICE_EVAR_COPY
                  if (cfg && cfg->notice_copy) {
                     performance(st, "создаётся копия переменной", var[id].line,
                                     var[id].pos, &buf.s[var[id].src], &buf.s[buf.free]);
                  }
#endif
               }
               var[id].opcode = rf_alloc_value(vm, id, id_type);
#ifdef REFAL_TRANSLATOR_PERFORMANCE_NOTICE_EVAR_COPY
               var[id].src  = line;
               var[id].line = line_num;
               var[id].pos  = pos - (chr != ' ');  //TODO заменить условие на 1.
#endif
               goto current_char;
            case id_global:
               break;
            }
lexem_identifier_complete_global:
            if (node < 0) {
               assert(imports);
               goto error_no_identifier_in_module;
            }
            if (ids->n[node].val.tag != rft_undefined) {
               // Если открыта вычислительная скобка, задаём ей адрес
               // первой вычислимой функции из выражения.
               if (ids->n[node].val.tag != rft_enum && cmd_exec[ep]
               && rtrie_val_from_raw(vm->u[cmd_exec[ep]].data).tag == rft_undefined) {
                  // Если в поле действия данной скобки встретился идентификатор,
                  // который на данный момент не определён, не известно,
                  // вычислим ли он. Возможно, именно определённая для него
                  // функция и должна быть вызвана скобкой. В таком случае
                  // откладываем решение до этапа, когда разрешаются
                  // неопределённые на данном проходе идентификаторы.
                  if (rtrie_val_from_raw(vm->u[cmd_exec[ep]].data).value)
                     goto lexem_identifier_undefined;
                  vm->u[cmd_exec[ep]].data = rtrie_val_to_raw(ids->n[node].val);
                  imports = 0;
               }
               // rft_enum и нулевое значение означают идентификатор модуля.
               // Используем соответствующую ветку для поиска следующего идентификатора.
               else if (ids->n[node].val.tag == rft_enum && !ids->n[node].val.value) {
                  assert(!imports);
                  imports = rtrie_find_next(ids, node, ' ');
                  im_str  = line;
                  im_line = line_num;
                  im_pos  = pos;
                  assert(imports > 0);
               } else {
                  rf_alloc_value(vm, rtrie_val_to_raw(ids->n[node].val), rf_identifier);
                  imports = 0;
               }
            } else {
lexem_identifier_undefined:
               if (imports) {
                  goto error_no_identifier_in_module;
               }
               // После первого прохода поищем, не появилось ли определение.
               rf_alloc_value(vm, node, rf_undefined);
               rf_index l = rf_alloc_value(vm, 0, rf_undefined);
               if (!undefined_fist)
                  undefined_fist = l;
               if (undefined_last)
                  vm->u[undefined_last].link = l;
               undefined_last = l;
               // Если открыта скобка и функция ей не присвоена, добавим
               // rf_execute, а скобке зададим фиктивное значение, что бы при
               // проверке в закрывающей скобке отличить неопределённые
               // идентификаторы от отсутствия таковых.
               if (cmd_exec[ep] && rtrie_val_from_raw(vm->u[cmd_exec[ep]].data).tag == rft_undefined) {
                  rf_alloc_value(vm, cmd_exec[ep], rf_open_function);
                  struct rtrie_val f = { .tag = rft_undefined, .value = 1 };
                  vm->u[cmd_exec[ep]].data = rtrie_val_to_raw(f);
               }
               // На случай ошибки.
               rf_alloc_value(vm, line_num, rf_undefined);
               rf_alloc_value(vm, pos - (chr != ' '), rf_undefined); //TODO заменить условие на 1.
               rf_alloc_value(vm, line, rf_undefined);
            }
            goto current_char;
         } // case lex_identifier: switch (semantic)
      }

   // Конец строки.
   case '\r':  // возможный последующий '\n' не сохраняется в буфере.
   case '\n':
      switch (lexer) {
      case lex_number:
         rf_alloc_int(vm, number);
      case lex_leadingspace:
      case lex_whitespace:
      case lex_comment_line:
         lexer = lex_leadingspace;
      case lex_comment_c:
         goto next_line;
      case lex_string_quoted:
      case lex_string_dquoted:
         error = "отсутствует закрывающая кавычка";
         goto cleanup;
      case lex_identifier:
         goto lexem_identifier_complete;
      }

   ///\section Комментарии
   ///\ingroup refal-syntax
   ///
   /// В Базовом РЕФАЛ начинающиеся с символа `*` строки являются комментариями:
   ///
   ///     * Комментарий в стиле РЕФАЛ-5
   ///
   /// В данной реализации `*` не обязательно должен располагаться в самом
   /// начале строки, допустимы предшествующие пробельные символы.

   // Начинает строку комментариев, либо может завершать комментарий в стиле Си.
   // TODO последовательность */ без предшествующей /* воспринимается как
   //      однострочный комментарий.
   case '*':
      switch (lexer) {
      case lex_leadingspace:
         lexer = lex_comment_line;
         goto next_char;
      case lex_comment_c:
         if (buf.s[line + pos] == '/') {
            ++pos;
            lexer = lex_whitespace;
         }
         goto next_char;
      case lex_comment_line:
         goto next_char;
      case lex_string_quoted:
      case lex_string_dquoted:
         goto lexem_string;
      case lex_number:
      case lex_whitespace:
      case lex_identifier:
         goto symbol;
      }

   /// Можно использовать комментарии в стиле Си:
   ///
   ///     /*
   ///        Комментарий
   ///        в несколько строк
   ///     */
   /// В дополнение к Базовому РЕФАЛ, поддерживаются однострочные комментарии:
   ///
   ///     // Одна строка с комментарием.

   case '/':
      switch (lexer) {
      case lex_leadingspace:
      case lex_whitespace:
         switch(buf.s[line + pos]) {
         case '/':
            ++pos;
            lexer = lex_comment_line;
            goto next_char;
         case '*':
            ++pos;
            lexer = lex_comment_c;
            goto next_char;
         default:
            goto symbol;
         }
      case lex_number:
      case lex_identifier:
         goto symbol;
      case lex_comment_line:
      case lex_comment_c:
         goto next_char;
      case lex_string_quoted:
      case lex_string_dquoted:
         goto lexem_string;
      }

   ///\section    Спец           Специальные знаки
   ///\subsection Замена
   ///
   /// Знак равенства `=` означает операцию замены.
   /// В _предложении_ РЕФАЛ-5 отделяет _образец_ (слева) от _общего выражения_
   /// (справа), так же называемого _результатом_. Если РЕФАЛ-машина успешно
   /// сопоставляет поле зрения с образцом, происходит замена на результат.
   ///
   /// В данной реализации может стоять сразу после идентификатора, определяя
   /// _простую функцию_ (не имеет альтернативных предложений).

   case '=':
      switch (lexer) {
      case lex_comment_c:
      case lex_comment_line:
         goto next_char;
      case lex_string_quoted:
      case lex_string_dquoted:
         goto lexem_string;
      case lex_identifier:
         goto lexem_identifier_complete;
      case lex_number:
         rf_alloc_int(vm, number);
      case lex_leadingspace:
      case lex_whitespace:
         switch (semantic) {
         case ss_source:
            goto error_identifier_missing;
         case ss_import:
            goto error_incorrect_import;
         case ss_identifier:
#define DEFINE_SIMPLE_FUNCTION                           \
            assert(function_block == 0);                 \
            if (ids->n[ident].val.tag != rft_undefined)  \
               goto error_identifier_already_defined;    \
            ids->n[ident].val.value = vm->free;          \
            ids->n[ident].val.tag   = rft_byte_code;

            DEFINE_SIMPLE_FUNCTION;
            rf_alloc_command(vm, rf_equal);
            lexer = lex_whitespace;
            semantic = ss_expression;
            goto next_char;
         case ss_pattern:
            if (bp) {
               error = "не закрыта структурная скобка";
               goto cleanup;
            }
            if (imports) {
               warning(st, redundant_module_id, im_line, im_pos, &buf.s[im_str], &buf.s[buf.free]);
               imports = 0;
            }
            if (ids->n[ident].val.tag == rft_enum) {
               assert(cmd_sentence);
               ids->n[ident].val.tag   = rft_byte_code;
               ids->n[ident].val.value = cmd_sentence;
            }
            // TODO проверить скобки ().
            rf_alloc_command(vm, rf_equal);
            lexer = lex_whitespace;
            semantic = ss_expression;
            goto next_char;
         case ss_expression:
            error = "недопустимый оператор в выражении (пропущена ; ?)";
            goto cleanup;
         }
      }

   ///\subsection Блок           Начало и конец
   ///
   /// Фигурные скобки `{` и `}` означают начало и конец _блока_ (тела) функции.
   /// Блок может включать произвольное количество предложений.
   /// Пустой блок определяет невычислимую функцию.

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
         goto lexem_identifier_complete;
      case lex_leadingspace:
      case lex_whitespace:
         switch (semantic) {
         case ss_source:
            goto error_identifier_missing;
         case ss_import:
            goto error_incorrect_import;
         case ss_identifier:
            assert(function_block == 0);
            if (ids->n[node].val.tag != rft_undefined) {
               goto error_identifier_already_defined;
            }
            cmd_sentence = rf_alloc_command(vm, rf_sentence);
            // Предварительно считаем функцию невычислимой.
            // При наличии предложений сменим тип и значение.
            assert(ident == node);
            ids->n[node].val.value = ++enum_couner;
            ids->n[node].val.tag   = rft_enum;
            lexer = lex_whitespace;
            semantic = ss_pattern;
            ++idc;
            ++function_block;
            goto next_char;
         case ss_pattern:
            error = "блок недопустим в выражении (пропущено = ?)";
            goto cleanup;
         case ss_expression:
            error = "вложенные блоки {} пока не поддерживаются";
            goto cleanup;
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
         goto lexem_identifier_complete;
      case lex_number:
         rf_alloc_int(vm, number);
         lexer = lex_whitespace;
      case lex_leadingspace:
      case lex_whitespace:
         switch (semantic) {
         case ss_source:
            goto error_identifier_missing;
         case ss_import:
            goto error_incorrect_import;
         case ss_identifier:
            goto error_incorrect_function_definition;
         // После последнего предложения ; может отсутствовать.
         case ss_expression:
            assert(cmd_sentence);
            assert(function_block > 0);
            --function_block;
            // Код операции rf_complete размещается в sentence_complete.
            vm->u[cmd_sentence].data = vm->free;
            cmd_sentence = 0;
            goto sentence_complete;
         // ; начинает предложение-образец, пустой в случае завершения функции.
         case ss_pattern:
            assert(function_block > 0);
            --function_block;
            if (!rf_is_evar_empty(vm, cmd_sentence, vm->free)) {
               error = "образец без общего выражения (пропущено = ?)";
               goto cleanup;
            }
            vm->u[cmd_sentence].tag = rf_complete;
            semantic = ss_source;
            goto next_char;
         }
      }

   ///\subsection Вычисление     Вычислительные скобки
   ///
   /// Функциональные, или вычислительные скобки `<` и `>` определяют
   /// _активное выражение_. При замене образца на результат, содержащий
   /// такое выражение, вызывается вычислимая функция, определяемая первым
   /// идентификатором внутри скобок. Остальное содержимое скобок передаётся
   /// в качестве поля зрения (но если оно содержит другое активное выражение,
   /// то предварительно вычисляется вложенное). В РЕФАЛ-5 такой идентификатор
   /// должен идти непосредственно после скобки:
   ///
   ///   Go { = <Prout "Вывод.">; }
   ///
   /// Здесь порядок свободный; так, выражения:
   ///
   ///   Go = <"Вывод." Prout>;
   ///
   ///   Go = <"Вы" Prout "вод.">;
   /// эквивалентны.

   case '<':
      switch (lexer) {
      case lex_comment_c:
      case lex_comment_line:
         goto next_char;
      case lex_string_quoted:
      case lex_string_dquoted:
         goto lexem_string;
      case lex_identifier:
         goto lexem_identifier_complete;
      case lex_number:
         rf_alloc_int(vm, number);
      case lex_leadingspace:
      case lex_whitespace:
         switch (semantic) {
         case ss_source:
            goto error_identifier_missing;
         case ss_import:
            goto error_incorrect_import;
         case ss_identifier:
            goto error_incorrect_function_definition;
         case ss_pattern:
            goto error_executor_in_pattern;
         case ss_expression:
            if (!(++ep < exec_max)) {
               error = "превышен лимит вложенности вычислительных скобок";
               goto cleanup;
            }
            if (imports) {
               warning(st, redundant_module_id, im_line, im_pos, &buf.s[im_str], &buf.s[buf.free]);
               imports = 0;
            }
            cmd_exec[ep] = rf_alloc_command(vm, rf_open_function);
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
         goto lexem_identifier_complete;
      case lex_number:
         rf_alloc_int(vm, number);
      case lex_leadingspace:
      case lex_whitespace:
         switch (semantic) {
         case ss_source:
            goto error_identifier_missing;
         case ss_import:
            goto error_incorrect_import;
         case ss_identifier:
            goto error_incorrect_function_definition;
         case ss_pattern:
            goto error_executor_in_pattern;
         case ss_expression:
            if (!ep) {
               error = "непарная вычислительная скобка";
               goto cleanup;
            }
            if (imports) {
               warning(st, redundant_module_id, im_line, im_pos, &buf.s[im_str], &buf.s[buf.free]);
               imports = 0;
            }
            assert(ep > 0);
            // Копируем адрес функции из парной открывающей, для вызова интерпретатором.
            // Если функция не определена, но между скобок содержатся
            // идентификаторы (.value == 1) задаём открывающей скобке ссылку на эту.
            rf_index ec = rf_alloc_value(vm, vm->u[cmd_exec[ep]].data, rf_execute);
            struct rtrie_val f = rtrie_val_from_raw(vm->u[cmd_exec[ep]].data);
            if (f.tag == rft_undefined) {
               if (!f.value) {
                  error = "активное выражение должно содержать имя вычислимой функции";
                  goto cleanup;
               }
               vm->u[cmd_exec[ep]].data = ec;
            }
            cmd_exec[ep--] = 0;
            lexer = lex_whitespace;
            goto next_char;
         }
      }

   ///\subsection Структуры   Структурные скобки
   ///
   /// Круглые скобки `(` и `)` объединяют данные в группу, которая может
   /// рассматриваться как единое целое, или _терм_.

   case '(':
      switch (lexer) {
      case lex_comment_c:
      case lex_comment_line:
         goto next_char;
      case lex_string_quoted:
      case lex_string_dquoted:
         goto lexem_string;
      case lex_identifier:
         goto lexem_identifier_complete;
      case lex_number:
         rf_alloc_int(vm, number);
      case lex_leadingspace:
      case lex_whitespace:
         switch (semantic) {
         case ss_source:
            goto error_identifier_missing;
         case ss_import:
            goto error_incorrect_import;
         case ss_identifier:
            DEFINE_SIMPLE_FUNCTION;
            semantic = ss_pattern;
         case ss_pattern:
         case ss_expression:
            if (!(bp < bracket_max)) {
               error = "превышен лимит вложенности структурных скобок";
               goto cleanup;
            }
            bracket[bp++] = rf_alloc_command(vm, rf_opening_bracket);
            lexer = lex_whitespace;
            goto next_char;
         }
      }

   case ')':
      switch (lexer) {
      case lex_comment_c:
      case lex_comment_line:
         goto next_char;
      case lex_string_quoted:
      case lex_string_dquoted:
         goto lexem_string;
      case lex_identifier:
         goto lexem_identifier_complete;
      case lex_number:
         rf_alloc_int(vm, number);
      case lex_leadingspace:
      case lex_whitespace:
         switch (semantic) {
         case ss_source:
            goto error_identifier_missing;
         case ss_import:
            goto error_incorrect_import;
         case ss_identifier:
            goto error_incorrect_function_definition;
         case ss_pattern:
         case ss_expression:
            if (!bp) {
               error = "непарная структурная скобка";
               goto cleanup;
            }
            rf_link_brackets(vm, bracket[--bp], rf_alloc_command(vm, rf_closing_bracket));
            lexer = lex_whitespace;
            goto next_char;
         }
      }

   ///\subsection Иначе
   ///
   /// Точка с запятой `;` разделяет предложения в блоке. Если РЕФАЛ-машина
   /// не распознаёт образец в текущем (первом) предложении блока, пробуется
   /// следующее.
   ///
   /// В данной реализации точка с запятой может идти непосредственно после
   /// идентификатора, определяя пустую функцию.

   case ';':
      switch (lexer) {
      case lex_comment_c:
      case lex_comment_line:
         goto next_char;
      case lex_string_dquoted:
      case lex_string_quoted:
         goto lexem_string;
      case lex_identifier:
         goto lexem_identifier_complete;
      case lex_number:
         rf_alloc_int(vm, number);
         lexer = lex_whitespace;
      case lex_leadingspace:
      case lex_whitespace:
         switch (semantic) {
         case ss_source:
            goto error_identifier_missing;
         case ss_import:
            semantic = ss_source;
            imports = 0;
            goto next_char;
         // Идентификатор пустой функции (ENUM в Refal-05).
         case ss_identifier:
            if (ids->n[node].val.tag != rft_undefined) {
               goto error_identifier_already_defined;
            }
            ids->n[node].val.tag   = rft_enum;
            ids->n[node].val.value = ++enum_couner;
            lexer = lex_whitespace;
            semantic = ss_source;
            goto next_char;
         case ss_pattern:
            error = "образец без общего выражения (пропущено = ?)";
            goto cleanup;
         case ss_expression:
sentence_complete:
            if (ep) {
               error = "не закрыта вычислительная скобка";
               goto cleanup;
            }
            if (bp) {
               error = "не закрыта структурная скобка";
               goto cleanup;
            }
            if (imports) {
               warning(st, redundant_module_id, im_line, im_pos, &buf.s[im_str], &buf.s[buf.free]);
               imports = 0;
            }
            rf_index sentence_complete;
            // В функциях с блоком сохраняем в маркере текущего предложения
            // ссылку на данные следующего и размещаем новый маркер.
            if (function_block && cmd_sentence) {
               sentence_complete = rf_alloc_command(vm, rf_sentence);
               vm->u[cmd_sentence].data = sentence_complete;
               cmd_sentence = sentence_complete;
               semantic = ss_pattern;
               local = 0;
               ++idc;
            } else if (cmd_sentence) {
               assert(0);
               sentence_complete = rf_alloc_command(vm, rf_complete);
               vm->u[cmd_sentence].data = sentence_complete;
               semantic = ss_source;
            } else {
               // См. переход сюда из case '}' где подразумевается данный опкод.
               // Может показаться, что достаточно проверять function_block на 0,
               // но планируется поддержка вложенных блоков.
               sentence_complete = rf_alloc_command(vm, rf_complete);
               semantic = ss_source;
            }
            // При хвостовых вызовах нет смысла в парном сохранении и
            // восстановление контекста функции. Обозначим такие интерпретатору.
            if (vm->u[vm->u[sentence_complete].prev].tag == rf_execute) {
               vm->u[vm->u[sentence_complete].prev].tag2 = rf_complete;
            }
            goto next_char;
         }
      }

   // Знак доллара в кириллической раскладке отсутствует, соответственно
   // принципиально не используется, как и директивы $ENTRY, $EXTERN и т.п.
#if 0
   case '$':
      assert(0);
#endif

   ///\subsection Является
   ///
   /// Двоеточие `:` в Расширенном РЕФАЛ используется в условиях (не реализовано).
   ///
   /// Применяется для импорта модулей, как замена $EXTERN (и *$FROM Refal-05).
   ///
   ///     ИмяМодуля: функция1 функция2;
   case ':':
      switch (lexer) {
      case lex_comment_c:
      case lex_comment_line:
         goto next_char;
      case lex_string_dquoted:
      case lex_string_quoted:
         goto lexem_string;
      case lex_identifier:
         goto lexem_identifier_complete;
      case lex_number:
         rf_alloc_int(vm, number);  //TODO 123: ?
         lexer = lex_whitespace;
      case lex_leadingspace:
      case lex_whitespace:
         switch (semantic) {
         // Импорт из глобального пространства имён.
         case ss_source:
            imports = 0;
            semantic = ss_import;
            goto next_char;
         case ss_import:
            goto error_incorrect_import;
         // Идентификатор внешнего модуля.
         case ss_identifier:
            switch (ids->n[node].val.tag) {
            case rft_machine_code:
            case rft_byte_code:
               goto error_identifier_already_defined;
            case rft_enum:
               if (ids->n[node].val.value)
                  goto error_identifier_already_defined;
               // Если идентификатор определён со значением 0, значит трансляция
               // модуля уже выполнена. Импортируем идентификаторы.
               imports = rtrie_find_next(ids, node, ' ');
               assert(!(imports < 0));
               semantic = ss_import;
               namespace = module;
               if (imports_local) {
                  ids->n[imports_local] = ids->n[imports];
                  imports_local = 0;
               }
               goto next_char;
            case rft_undefined:
               break;
            }
            ids->n[node].val.tag   = rft_enum;
            ids->n[node].val.value = 0;
            lexer = lex_whitespace;
            assert(!"module");
#if 01 //TODO
            // Имя модуля внесено в текущее пространство имён, что гарантирует
            // отсутствие иного одноимённого идентификатора и даёт возможность
            // квалифицированного (именем модуля) поиска идентификаторов.
            //
            // Поскольку другие модули могут импортировать этот же модуль,
            // необходимо обеспечить идентичность идентификаторов, а так же нет
            // смысла повторно транслировать уже импортированный модуль.
            // Обе задачи решаются импортом всех модулей в одну (глобальную)
            // область видимости. Выполняется откат, трансляция имени модуля
            // происходит повторно, что дублирует его имя в корне таблицы
            // символов. Содержимое модуля транслируется, идентификаторы
            // заносятся в соответствующую ветку.
            // После чего в данный узел "пробел" следует скопировать
            // соответствующий ему из глобального пространства, что обеспечит
            // единообразный импорт идентификаторов модуля.
            imports = rtrie_insert_next(ids, node, ' ');
            if (module && namespace == module) {
               namespace = 0;
               imports_local = imports;
               imports = 0;
               src = ident_str; //TODO если двоеточие на следующей строке???
               pos = ident_pos;
               semantic = ss_source;
               goto next_char;
            }
            assert(!namespace);
            if (imports_local) {
               ids->n[imports_local] = ids->n[imports];
               imports_local = 0;
            }
            namespace = module;
            int r = refal_translate_module_to_bytecode(cfg, vm, ids, imports,
                                                   ident_str, ident_strlen, st);
            switch (r) {
            case -2:
               error = "слишком длинное имя модуля";
               goto cleanup;
            case -1:
               error = "недействительное имя модуля";
               goto cleanup;
            default:
               semantic = ss_import;
               goto next_char;
            }
#endif
         case ss_pattern:
         case ss_expression:
            error = "условия не поддерживаются";
            goto cleanup;
         }
      }

   ///\section Строки
   ///
   /// Знаковые символы заключаются в одинарные или двойные кавычки.
   /// Цепочка (строка) знаковых символов заключается в кавычки целиком; так:
   ///
   ///      'да в'
   /// есть последовательность четырёх знаковых символов. Недопустим перенос
   /// цепочки знаковых символов со строки на строку. Кавычка внутри цепочки,
   /// которая ограничена кавычками того же вида, представляется сдвоенной
   /// кавычкой. Следующие две строки-цепочки:
   ///
   ///      '''слова в кавычках'''
   ///      "'слова в кавычках'"
   /// представляют один и тот же РЕФАЛ-объект. Для того, что бы избежать
   /// ложных кавычек, следует разделять заключённые в кавычки цепочки
   /// пробелами в том случае, когда они следуют непосредственно одна за другой.
   ///
   /// Что бы включить в строку специальные символы, используются
   /// экранированные последовательности:
   ///   - '\t'  табуляция       (код 0x09)
   ///   - '\n'  новая строка    (код 0x0a)
   ///   - '\r'  перевод каретки (код 0x0d)
   /// Символ \ перед иными знаками представляет сам себя.

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
         case ss_import:
            goto error_incorrect_import;
         case ss_identifier:
            DEFINE_SIMPLE_FUNCTION;
            semantic = ss_pattern;
         case ss_pattern:
         case ss_expression:
            lexer = chr == '"' ? lex_string_dquoted : lex_string_quoted;
            goto next_char;
         }
      case lex_string_dquoted:
      case lex_string_quoted:
         if ((lexer == lex_string_dquoted && chr == '\'')
          || (lexer == lex_string_quoted && chr == '"'))
            goto lexem_string;
         // Кавычка внутри строки кодируется сдвоенной, иначе завершает строку.
         if (buf.s[line + pos] == chr) {
            ++pos;
            goto lexem_string;
         }
         lexer = lex_whitespace;
         goto next_char;
      case lex_comment_c:
      case lex_comment_line:
         goto next_char;
      case lex_identifier:
         goto lexem_identifier_complete;
      }

   ///\section Макроцифры
   ///
   /// Макроцифрами являются целые неотрицательные числа. Они представляются
   /// строками десятичных цифр. Значение наибольшей макроцифры зависит от
   /// платформы (соответствует размеру регистров общего назначения процессора).
   /// Если последовательность цифр в исходном тексте определяет число,
   /// превосходящее наибольшее допустимое, выводится предупреждение, а
   /// результатом являются младшие двоичные разряды (деление по модулю).

   // Начало целого числа, либо продолжение идентификатора.
   case '0'...'9':
      switch (lexer) {
      case lex_leadingspace:
      case lex_whitespace:
         switch (semantic) {
         case ss_source:
            error = "числа допустимы только в выражениях";
            goto cleanup;
         case ss_import:
            goto error_incorrect_import;
         case ss_identifier:
            DEFINE_SIMPLE_FUNCTION;
            semantic = ss_pattern;
         case ss_pattern:
         case ss_expression:
            lexer = lex_number;
            number = chr - '0';
            goto next_char;
         }
      case lex_number:
         number = number * 10 + (chr - '0');
         if (number < (chr - '0')) {
            warning(st, "целочисленное переполнение", line_num, pos, &buf.s[line], &buf.s[buf.free]);
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

   ///\page    Синтаксис
   ///\section Идентификаторы
   ///
   /// Идентификаторы в РЕФАЛ-5 являются строкой алфавитно-цифровых знаков,
   /// начинающейся с буквы. Могут включать тире и подчёркивания (они являются
   /// эквивалентными).
   ///
   /// В данной реализации идентификаторы могут содержать практически любые
   /// символы Юникода, включая знаки операций (кроме скобок, кавычек и прочих
   /// специальных знаков РЕФАЛ), тире и подчёркивание различаются (пока?).

   // Оставшиеся символы считаются допустимыми для идентификаторов.
   default:
symbol:
      switch (lexer) {
      case lex_number:
         // TODO символ после цифры?
         // Может быть частью шестнадцатеричного числа, что пока не поддержано.
         rf_alloc_int(vm, number);
         warning(st, "идентификаторы следует отделять от цифр пробелом", line_num, pos, &buf.s[line], &buf.s[buf.free]);
      case lex_leadingspace:
      case lex_whitespace:
         lexer = lex_identifier;
         switch (semantic) {
         // Перечисленные после имени модуля идентификаторы импортируются,
         // для чего ищутся в модуле и определяются в текущей области видимости.
         case ss_import:
            import_node = rtrie_find_at(ids, imports, chr);
         case ss_source:
            node = rtrie_insert_at(ids, namespace, chr);
            ident_str = str;
            ident_pos = pos;
            goto next_char;
         case ss_identifier:
            DEFINE_SIMPLE_FUNCTION;
            semantic = ss_pattern;
         case ss_pattern:
            // Возможно объявление и использование переменных.
            // Сохраняем их в таблице символов, отделив от идентификатора
            // текущей функции спецсимволом.
            switch (chr) {
            case L'…':
            case '.':
               id_type = id_evar;
               goto lexem_identifier_local_pat;
            case '?':
               id_type = id_svar;
               goto lexem_identifier_local_pat;
            case '!':
               id_type = id_tvar;
               goto lexem_identifier_local_pat;
            case 'e':
               id_type = id_evar;
               goto lexem_identifier_local_pat_check;
            case 't':
               id_type = id_tvar;
               goto lexem_identifier_local_pat_check;
            case 's':
               id_type = id_svar;
lexem_identifier_local_pat_check:
               if (buf.s[line + pos] == '.') {
                  ++pos;
lexem_identifier_local_pat:
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
            case L'…':
            case '.':
               id_type = id_evar;
               goto lexem_identifier_local_exp;
            case '?':
               id_type = id_svar;
               goto lexem_identifier_local_exp;
            case '!':
               id_type = id_tvar;
               goto lexem_identifier_local_exp;
            case 'e':
               id_type = id_evar;
               goto lexem_identifier_local_exp_check;
            case 't':
               id_type = id_tvar;
               goto lexem_identifier_local_exp_check;
            case 's':
               id_type = id_svar;
lexem_identifier_local_exp_check:
               if (buf.s[line + pos] == '.') {
                 ++pos;
lexem_identifier_local_exp:
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
               if (imports) {
                  // Ищем в пространстве имён другого модуля.
                  node = rtrie_find_at(ids, imports, chr);
               } else {
                  // Если идентификатор не существует, добавляем.
                  // После первого прохода проверим, не появилось ли определение.
                  assert(namespace == module);
                  node = rtrie_insert_at(ids, namespace, chr);
               }
               goto next_char;
            }
         }
      case lex_identifier:
lexem_identifier:
         switch (semantic) {
         case ss_import:
            import_node = rtrie_find_next(ids, import_node, chr);
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
            if (imports) {
               node = rtrie_find_next(ids, node, chr);
            } else {
               node = rtrie_insert_next(ids, node, chr);
            }
            goto next_char;
         }
      case lex_string_quoted:
      case lex_string_dquoted:
lexem_string:
         if (chr == '\\') {
            switch (buf.s[line + pos]) {
            case 't':
               ++pos;
               chr = '\t';
               break;
            case 'n':
               ++pos;
               chr = '\n';
               break;
            case 'r':
               ++pos;
               chr = '\r';
               break;
            default:
               break;
            }
         }
         rf_alloc_char(vm, chr);
         goto next_char;
      case lex_comment_c:
      case lex_comment_line:
         goto next_char;
      }
   }
   assert(0);

complete:
   if (semantic != ss_source) {
      if (function_block)
         error = "не завершено определение функции (пропущена } ?)";
      else
         error = "не завершено определение функции (пропущена ; ?)";
      goto cleanup;
   }

   // Ищем, не появилось ли определение идентификаторов.
   // На первой итерации проверяем только идентификаторы внутри скобок,
   // что бы присвоить адрес вычислимой функции. Подходящий идентификатор
   // маркируется для удаления на следующей итерации (список односвязный).
   // На второй итерации разрешаем оставшиеся идентификаторы, удаляя их.
   // Если какой-то оказывается внутри скобки, которой так и не присвоен адрес,
   // значит активное выражение не может быть вычислено, сообщаем об ошибке.
   for (int ex = 1; ex >= 0; --ex) {
      rf_index last_exec = 0;
      for (rf_index undef = undefined_fist; undef; ) {
         assert(vm->u[undef].tag == rf_undefined);
         const rf_index opcode = vm->u[undef].prev;
         rf_index s = vm->u[undef].next;
         undef = vm->u[undef].link;

         rf_index exec_open = 0;
         if (vm->u[s].tag == rf_open_function) {
            exec_open = vm->u[s].link;
            s = vm->u[s].next;
         } else if (ex) {
            continue;
         }
         line_num = vm->u[s].num;
         s = vm->u[s].next;
         pos  = vm->u[s].num;
         s = vm->u[s].next;
         line = vm->u[s].num;
         s = vm->u[s].next;

         // В результате трансляции rf_complete в данной позиции невозможен,
         // потому выбран в качестве маркера на предыдущей итерации.
         if (vm->u[opcode].tag == rf_complete) {
            assert(!ex);
            rf_free_evar(vm, vm->u[opcode].prev, s);
            continue;
         }
         rtrie_index n = vm->u[opcode].link;
         if (ids->n[n].val.tag == rft_undefined) {
            // TODO опциональное поведение?
            if (ex)
               continue;
            if (cfg && cfg->warn_implicit_declaration) {
               warning(st, "неявное определение идентификатора", line_num, pos, &buf.s[line], &buf.s[buf.free]);
            }
            ids->n[n].val.tag   = rft_enum;
            ids->n[n].val.value = ++enum_couner;
         }
         // Скобке присваивается первая вычислимая функция, так что порядок
         // обработки списка неопределённых идентификаторов важен.
         if (exec_open) {
            rf_index exec_close = vm->u[exec_open].link;
            if (rtrie_val_from_raw(vm->u[exec_close].data).tag == rft_undefined) {
               if (ids->n[n].val.tag != rft_enum) {
                  assert(ex);
                  vm->u[exec_close].data = rtrie_val_to_raw(ids->n[n].val);
                  // временный маркер для следующей итерации.
                  vm->u[opcode].tag = rf_complete;
                  continue;
               } else if (ex) {
                  continue;
               } else if (last_exec != exec_close) {
                  last_exec = exec_close;
                  syntax_error(st, "активное выражение должно содержать "
                               "вычислимую функцию", line_num, pos, &buf.s[line], &buf.s[buf.free]);
               }
            } else if (ex) {
               continue;
            }
         }
         vm->u[opcode].tag  = rf_identifier;
         vm->u[opcode].link = rtrie_val_to_raw(ids->n[n].val);
         rf_free_evar(vm, opcode, s);
      }
   }

cleanup:
   wstr_free(&buf);

   //TODO error = "неполный символ UTF-8"; "недействительный символ UTF-8";

   if (error) {
      syntax_error(st, error, line_num, pos, &buf.s[line], &buf.s[buf.free]);
      return 1;
   }
   return 0;

error_no_identifier_in_module:
   error = "идентификатор не определён в модуле";
   goto cleanup;

error_identifier_already_defined:
   // TODO надо бы отобразить прежнее определение
   error = "повторное определение идентификатора";
   goto cleanup;

error_incorrect_import:
   error = "некорректное описание импорта (пропущено ; ?)";
   goto cleanup;

error_incorrect_function_definition:
   error = "некорректное определение функции (пропущено = или { ?)";
   goto cleanup;

error_identifier_missing:
   error = "пропущено имя функции";
   goto cleanup;

error_identifier_odd:
   error = "лишний идентификатор (пропущено = или { в определении функции?)";
   goto cleanup;

error_identifier_undefined:
   error = "идентификатор не определён";
   goto cleanup;

error_executor_in_pattern:
   error = "вычислительные скобки в образце не поддерживаются";
   goto cleanup;
}
