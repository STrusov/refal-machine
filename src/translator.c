
#define _POSIX_C_SOURCE 1

#include "translator.h"
#include "library.h"

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
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
   FILE *src;
   const char *os;
   if (name && *name) {
      src = fopen(name, "r");
      os = refal_message_source(st, name);
   }
   else {
      src = stdin;
      os = refal_message_source(st, "//stdin");
   }
   if (!src) {
      critical_error(st, "исходный текст недоступен", -errno, 0);
   } else {
      r = refal_translate_istream_to_bytecode(cfg, vm, ids, 0, src, st);
      if (src != stdin)
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
      const wchar_t        *name,
      struct refal_message *st)
{
   int too_long = 1;
   // Ищем в каталоге с исходным текстом файлы модуля.
   static const char ext1[] = ".реф";
   static const char ext2[] = ".ref";
   unsigned pl = 0;
   if (st && st->source) {
      for (unsigned i = 0; i != PATH_MAX && st->source[i]; ++i)
         // TODO разделитель может быть другой.
         if (st->source[i] == '/')
            pl = i + 1;
   }
   char path[PATH_MAX];
   if (pl)
      strncpy(path, st->source, pl);
   // Считаем, что недействительные символы отсеяны при чтении файла.
   pl += wcstombs(&path[pl], name, PATH_MAX - MB_LEN_MAX);
   if (pl < PATH_MAX - sizeof(ext1)) {
      too_long = 0;
      const char *const ext[2] = { ext2, ext1 };
      for (int i = 1; i >= 0; --i) {
         strcpy(&path[pl], ext[i]);
         FILE *f = fopen(path, "r");
         if (!f)
            continue;
         const char *os = refal_message_source(st, path);
         int r = refal_translate_istream_to_bytecode(cfg, vm, ids, module, f, st);
         refal_message_source(st, os);
         fclose(f);
         return r;
      }
   }
   strcpy(&path[pl], ext1);
   const char *os = refal_message_source(st, path);
   critical_error(st, too_long ? "слишком длинное имя модуля" : "недействительное имя модуля", -errno, 0);
   refal_message_source(st, os);
   return -1;
}

/**
 * Читает поток в буфер.
 * Для простоты разбора завершает L'\0'.
 * Исключает второй символ возможных '\r' '\n' для совместимости.
 */
static inline
void read_file(struct wstr *buf,  FILE *src)
{
   //TODO Хорошо бы заменять управляющие символы и Уникод переводы строки.
   while (1) {
      wint_t wc = fgetwc(src);
      if (wc == WEOF)
         wc = L'\0';
      wstr_append(buf, wc);
      if (wc == L'\0' || !wstr_check(buf, NULL))
         return;
      if (wc == '\r') {
         wc = fgetwc(src);
         if (wc != '\n' && wc != WEOF)
            ungetwc(wc, src);
      }
   }
}

// Тип текущего идентификатора.
enum identifier_type {
   id_global = rf_identifier, // Глобальная область видимости.
   id_svar   = rf_svar,       // s-переменная (один символ).
   id_tvar   = rf_tvar,       // t-переменная (s- либо выражение в скобках).
   id_evar   = rf_evar,       // e-переменная (произвольное количество элементов.
};

enum lexem_type {
   L_EOF,
   L_whitespace,
   L_identifier,
   L_number,
   L_exec_open,
   L_exec_close,
   L_block_open,
   L_block_close,
   L_term_open,
   L_term_close,
   L_equal,
   L_string,
   L_semicolon,
   L_colon,
};

enum lexem_type lex_type(wchar_t c)
{
   if (c <= ' ')  return L_whitespace;
   switch (c) {
      case '<':   return L_exec_open;
      case '>':   return L_exec_close;
      case '{':   return L_block_open;
      case '}':   return L_block_close;
      case '(':   return L_term_open;
      case ')':   return L_term_close;
      case '=':   return L_equal;
      case '"':   return L_string;
      case '\'':  return L_string;
      case ';':   return L_semicolon;
      case ':':   return L_colon;
      case '0'...'9': return L_number;
      default:    return L_identifier;
   }
}

struct lexer {
   // Здесь храним обрабатываемый исходный текст для сообщений об ошибках.
   // Поскольку определение идентификаторов возможно после их использование,
   // и после первого прохода производится дополнительный, сохраняется
   // весь входной поток целиком.
   struct wstr buf;

   // Начало текущей строки в буфере.
   wstr_index  line;

   // Идентификаторы заносятся в таблицу символов параллельно разбору исходного
   // текста. Здесь хранится текущий добавленный в префиксное дерево узел.
   rtrie_index node;

   // Последний узел глобального идентификатора (текущей функции или имени модуля).
   // Используется как корень для локальных (переменных)
   // и для возможности изменить тип функции (с пустой на вычислимую).
   // В случае модуля является корнем пространства имён.
   rtrie_index id_node;

   enum identifier_type id_type;

   // Первый символ идентификатора в массиве атомов.
   // Используются при импорте и встраивании ссылки на имя функции в опкоды.
   wstr_index  id_begin;
   // Начало строки в буфере и позиция в исходном тексте для диагностики.
   wstr_index  id_line;
   unsigned    id_pos;
   unsigned    id_line_num;

   // Номер текущей строки.
   unsigned    line_num;

   // Номер символа в строке исходника. Используется с двумя целями:
   // 1. Адресация во входном буфере относительно начала строки;
   //    здесь значение соотвествует требованиям к индексам, отсчёт идёт от 0.
   // 2. Вывод сообщений (в т.ч. копией в id_pos) об ошибках наряду с line_num;
   //    в таком случае нумерация символов начинается с 1, а значение
   //    обычно подходит, поскольку pos указывает на последующий символ.
   unsigned    pos;
};

static inline
void lexer_init(struct lexer* lex, FILE *src)
{
   lex->node     = 0;
   lex->id_type  = id_global;
   lex->id_node  = 0;
   lex->id_begin = 0;
   lex->id_line  = 0;
   lex->id_pos   = 0;
   lex->id_line_num = 0;
   lex->line_num = 1;
   lex->pos      = 0;

   wstr_alloc(&lex->buf, REFAL_INITIAL_FILEBUFFER);
   lex->line = lex->buf.free;
   read_file(&lex->buf, src);
}

static inline
void lexer_free(struct lexer *lex)
{
   wstr_free(&lex->buf);
}

/**
 * Первый символ распознанного идентификатора.
 */
static inline
wchar_t lexer_char(const struct lexer *lex)
{
   return lex->buf.s[lex->line + lex->pos - 1];
}

/**
 * Следующий после обработанного идентификатора символ.
 */
static inline
wchar_t lexer_next_char(const struct lexer *lex)
{
   return lex->buf.s[lex->line + lex->pos];
}

///\page    Синтаксис
///\ingroup refal-syntax
///\section lexem          Лексические единицы
///
/// Лексические единицы РЕФАЛ подразделяются на специальные знаки, символы и
/// переменные. Между лексическими единицами может проставляться любое
/// количество как пробелов, так и спец.символов табуляции и переноса строки.
/// Пробелы становятся лексической единицей, когда они появляются в строке,
/// заключённой в кавычки.

///\page    Синтаксис
///\ingroup refal-syntax
///\section Идентификаторы
///
/// Идентификаторы в РЕФАЛ-5 являются строкой алфавитно-цифровых знаков,
/// начинающейся с буквы. Могут включать тире и подчёркивания (они являются
/// эквивалентными).
///
/// В данной реализации идентификаторы могут содержать практически любые
/// символы Уникода, включая знаки операций (кроме скобок, кавычек и прочих
/// специальных знаков РЕФАЛ), тире и подчёркивание различаются (пока?).


/**
 * Заносит в префиксное дерево идентификатор, читая имя из исходного текста.
 * Для корневых (определения функций) устанавливает \c lex->id_node.
 *
 * \param import_node   Указывается для идентификаторов в списке импорта.
 */
static inline
void lexem_identifier(struct lexer *lex, struct refal_trie *ids,
      rtrie_index module, rtrie_index *import_node,
      struct refal_vm *vm, struct refal_message *st)
{
   lex->id_line = lex->line;
   lex->id_pos  = lex->pos;
   lex->id_line_num = lex->line_num;
   wchar_t chr = lexer_char(lex);
   lex->node = rtrie_insert_at(ids, module, chr);
   lex->id_begin = wstr_append(&vm->id, chr);
   while (1) {
      chr = lexer_next_char(lex);
      if (lex_type(chr) != L_identifier && lex_type(chr) != L_number) {
         wstr_append(&vm->id, L'\0');
         if (!import_node)
            lex->id_node = lex->node;
         return;
      }
      ++lex->pos;
      // Определение идентификатора может быть импортом из другого модуля.
      if (import_node)
         *import_node = rtrie_find_next(ids, *import_node, chr);
      lex->node = rtrie_insert_next(ids, lex->node, chr);
      wstr_append(&vm->id, chr);
   }
}

/**
 * Обрабатывает идентификатор выражения-образца или -результата.
 *
 * Для импортируемых поиск происходит в пространстве имён соответствующего модуля.
 * Глобальные идентификаторы не обязательно должны быть определены ранее;
 * в таком случае вносятся в дерево для отложенного поиска.
 *
 * Локальные идентификаторы (s-, e- и t-переменные) выражения-образца
 * могут быть как определены, так и использованы (повторное вхождение).
 * Для простоты определяются всегда, поскольку вызывающая сторона смотрит rf_id_undefined.
 *
 * В случае отсутствия идентификатора lex->node отрицателен.
 */
static inline
void lexem_identifier_exp(struct lexer *lex, struct refal_trie *ids,
      rtrie_index module, rtrie_index imports, wchar_t idc,
      struct refal_vm *vm, struct refal_message *st)
{
   wchar_t chr = lexer_char(lex);
   switch (chr) {
   case L'…':
   case '.': lex->id_type = id_evar; goto local;
   case '?': lex->id_type = id_svar; goto local;
   case '!': lex->id_type = id_tvar; goto local;
   case 'e': lex->id_type = id_evar; goto check_local;
   case 't': lex->id_type = id_tvar; goto check_local;
   case 's': lex->id_type = id_svar;
check_local:
      if (lexer_next_char(lex) == '.') {
        ++lex->pos;
local:   lex->node = rtrie_insert_next(ids, lex->id_node, idc);
         lex->node = rtrie_insert_next(ids, lex->node, chr);
         goto tail;
      }
      [[fallthrough]];
   default:
      lex->id_type = id_global;
      lex->node = imports ? rtrie_find_at(ids, imports, chr)
                          : rtrie_insert_at(ids, module, chr);
   }
   while (1) {
tail: chr = lexer_next_char(lex);
      if (lex_type(chr) != L_identifier && lex_type(chr) != L_number)
         return;
      ++lex->pos;
      lex->node = imports ? rtrie_find_next(ids, lex->node, chr)
                          : rtrie_insert_next(ids, lex->node, chr);
   }
}

///\page    Синтаксис
///\ingroup refal-syntax
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
///   - \c '\t'  табуляция       (код \c 0x09)
///   - \c '\\n'  новая строка    (код \c 0x0a)
///   - \c '\r'  перевод каретки (код \c 0x0d)
///
/// Символ \c \ перед иными знаками представляет сам себя.

/**
 * Переносит строку из исходного текста в опкоды.
 */
static inline
void lexem_string(struct lexer *lex, struct refal_vm *vm, struct refal_message *st)
{
   wchar_t q = lexer_char(lex);
   while(1) {
      wchar_t chr = lexer_next_char(lex);
      ++lex->pos;
      switch(chr) {
      // Кавычка внутри строки кодируется сдвоенной, иначе завершает строку.
      case '"': case '\'':
         if (chr != q) break;
         if (lexer_next_char(lex) == q) {
            ++lex->pos;
            break;
         }
         return;
      case '\\': switch (lexer_next_char(lex)) {
         case 't': ++lex->pos; chr = '\t'; break;
         case 'n': ++lex->pos; chr = '\n'; break;
         case 'r': ++lex->pos; chr = '\r'; break;
         default : break;
         }
         break;
      case '\r': case '\n':
         //TODO Позволить многострочные?
         syntax_error(st, "отсутствует закрывающая кавычка", lex->line_num, lex->pos, &lex->buf.s[lex->line], &lex->buf.s[lex->buf.free]);
         return;
      default:
         if (chr < ' ')
            warning(st, "нечитаемый символ", lex->line_num, lex->pos, &lex->buf.s[lex->line], &lex->buf.s[lex->buf.free]);
         break;
      }
      rf_alloc_char(vm, chr);
   }
}

///\page    Синтаксис
///\ingroup refal-syntax
///\section Макроцифры
///
/// Макроцифрами являются целые неотрицательные числа. Они представляются
/// строками десятичных цифр. Значение наибольшей макроцифры зависит от
/// платформы (соответствует размеру регистров общего назначения процессора).
/// Если последовательность цифр в исходном тексте определяет число,
/// превосходящее наибольшее допустимое, выводится предупреждение, а
/// результатом являются младшие двоичные разряды (деление по модулю).
///

/**
 * Переносит макрофицру из исходного текста в опкоды.
 */
static inline
void lexem_number(struct lexer *lex, struct refal_vm *vm, struct refal_message *st)
{
   wchar_t chr = lexer_char(lex);
   // Не обязательно, поскольку проверяется перед вызовом.
   // Но вернее ничего не располагать на выходе, если на входе ничего нет.
   if (chr < '0' || chr > '9')
      return;
   rf_int   number = 0;
   while (1) {
      number = number * 10 + (chr - '0');
      if (number < (chr - '0')) {
         warning(st, "целочисленное переполнение", lex->line_num, lex->pos, &lex->buf.s[lex->line], &lex->buf.s[lex->buf.free]);
      }
      chr = lexer_next_char(lex);
      if ((chr < '0' || chr > '9'))
         break;
      ++lex->pos;
   }
   rf_alloc_int(vm, number);
}

///\page    Синтаксис
///\section Комментарии
///\ingroup refal-syntax
///
/// В Базовом РЕФАЛ начинающиеся с символа `*` строки являются комментариями:
///
///      * Комментарий в стиле РЕФАЛ-5
///
/// В данной реализации `*` не обязательно должен располагаться в самом
/// начале строки, допустимы предшествующие пробельные символы.
///
/// Можно использовать комментарии в стиле Си:
///
///      /*
///         Комментарий
///         в несколько строк
///      */
/// В дополнение к Базовому РЕФАЛ, поддерживаются однострочные комментарии:
///
///      // Одна строка с комментарием.

/**
 * Пропускает комментарии, пробелы, переводы строк и т.п. и
 * останавливается на первом символе идентификатора.
 * В первой строке могут быть символы #!, тогда игнорируем, считая # комментарием.
 * return   Тип лексемы.
 */
static inline
enum lexem_type lexer_next_lexem(struct lexer *lex, struct refal_message *st)
{
   bool comment = lex->line_num == 1 && !lex->pos && lexer_next_char(lex) == '#';
   bool multiline = false;
   // * может начинать комментарий, если является первым печатным символом в строке.
   unsigned first_pos = lex->pos;
   while (1) {
      wchar_t chr = lexer_next_char(lex);
      ++lex->pos;
      switch (chr) {
      case '\0':
         --lex->pos;
         if (multiline)
            syntax_error(st, "не закрыт комментарий /* */", lex->line_num, lex->pos, &lex->buf.s[lex->line], &lex->buf.s[lex->buf.free]);
         return L_EOF;
      case '\n': case '\r':
         lex->line += lex->pos;
         ++lex->line_num;
         first_pos = lex->pos = 0;
         comment = multiline;
         continue;
      case '*':
         if (multiline && lexer_next_char(lex) == '/') {
            first_pos = ++lex->pos;
            comment = multiline = false;
            continue;
         } else if (first_pos == 0) {
            // multiline не меняем, поскольку может быть частью многострочного.
            comment = true;
            continue;
         }
         break;
      case '/':
         if (!comment) switch(lexer_next_char(lex)) {
         case '/':
            ++lex->pos;
            comment = true;
            multiline = false;
            break;
         case '*':
            ++lex->pos;
            comment = multiline = true;
         default: break;
         }
         break;
      default: break;
      }
      enum lexem_type t = lex_type(chr);
      if (!comment && t != L_whitespace)
         return t;
   }
}

static inline
bool check_matching(struct refal_message *st, const struct lexer *lex, unsigned ep, unsigned bp)
{
   bool ok = true;
   if (ep) {
      syntax_error(st, "не закрыта вычислительная скобка", lex->line_num, lex->pos, &lex->buf.s[lex->line], &lex->buf.s[lex->buf.free]);
      ok = false;
   }
   if (bp) {
      syntax_error(st, "не закрыта структурная скобка",  lex->line_num, lex->pos, &lex->buf.s[lex->line], &lex->buf.s[lex->buf.free]);
      ok = false;
   }
   return ok;
}

struct mod_msg {
   wstr_index  line;
   unsigned    pos;
   unsigned    line_num;
};

static inline
void check_redundant_module_id(struct refal_message *st, const struct lexer *lex, rtrie_index *imports, const struct mod_msg * mm)
{
   if (*imports) {
      warning(st, "имя модуля без функции не имеет смысла", mm->line_num, mm->pos, &lex->buf.s[mm->line], &lex->buf.s[lex->buf.free]);
      *imports = 0;
   }
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

   // В случае неявного определения, имена идентификаторов на заносятся в таблицу
   // атомов. Что бы получить уникальные для каждого модуля значения, используем
   // номер свободной ячейки таблицы символов.
   // Приведение к дополнительному коду использовано, что бы выделяться в rf_output.
   struct rf_id enum_couner = { .tag = rf_id_enum, .link = -ids->free };

   // РЕФАЛ позволяет произвольный порядок определения функций.
   // При последовательном проходе не все идентификаторы определены, такие
   // сопровождаются дополнительной информацией и организуются в список,
   // границы хранятся здесь.
   rf_index undefined_fist = 0;
   rf_index undefined_last = 0;

   // Локальные идентификаторы храним в таблице символов как продолжение
   // глобальных, отделяясь символом который не может встретиться в Unicode.
   // Для каждого предложения вычисляется новый символ-разделитель (инкрементом),
   // что гарантирует уникальность записей в таблице. Таким образом
   // объявление s.1 в первом предложении не будет видно в следующих.
   wchar_t idc = 1 + 0x10FFFF;   // отделяет локальный идентификатор от корневого.
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
#if REFAL_TRANSLATOR_PERFORMANCE_NOTICE_EVAR_COPY
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

   // Структурные скобки организованы в стек по той же причине.
   // Запоминается адрес открывающей, что бы связать с парной закрывающей.
   unsigned bracket_max = REFAL_TRANSLATOR_BRACKETS_DEFAULT;
   if (cfg && cfg->brackets_limit) {
      bracket_max = cfg->brackets_limit;
   }
   rf_index bracket[bracket_max];

   struct lexer lex;
   lexer_init(&lex, src);
   if (!wstr_check(&lex.buf, st))
      goto cleanup;

   // На верхнем уровне исходного текста возможны три варианта:
   // : Print Prout;       // импорт из глобального пространства имён.
   // Module: id1 id2;     // импорт из файла-модуля.
   // identifier;          // определение функции.
   // identifier ... = ;
   // identifier { ... }
   for (enum lexem_type lexeme; L_EOF != (lexeme = lexer_next_lexem(&lex, st)); ) {
      switch (lexeme) {
      case L_whitespace: assert(0); continue;
      //TODO игнорировать пустую точку с запятой?
      default: error = "ожидается идентификатор модуля или функции"; goto cleanup;
      // Импорт из глобального пространства имён.
      case L_colon: lex.id_node = 0; goto importlist;
      // Определение функции либо импорт модуля.
      case L_identifier:
         lexem_identifier(&lex, ids, module, NULL, vm, st);
         switch (lexeme = lexer_next_lexem(&lex, st)) {
         case L_EOF: error = "не завершено определение функции или импорта";
            goto incomplete;
         // Импорт модуля.
         // Если происходит впервые, транслируем файл с соответствующим именем.
         // Следом просматриваем список импортируемых идентификаторов до ;
         // и вносим имеющиеся в действующую область видимости.
         case L_colon:
            switch (ids->n[lex.id_node].val.tag) {
            case rf_id_mach_code: error = "имя модуля определено ранее как встроенная функция";
               goto cleanup;
            case rf_id_op_code: error = "имя модуля определено ранее как вычислимая функция";
               goto cleanup;
            case rf_id_box:
            case rf_id_reference:
            case rf_id_enum: error = "имя модуля определено ранее как невычислимая функция";
               goto cleanup;
            case rf_id_module:
               // Если идентификатор определён, значит трансляция
               // модуля уже выполнена. Импортируем идентификаторы.
               lex.id_node = rtrie_find_next(ids, lex.id_node, ' ');
               assert(!(lex.id_node < 0));
               break;
            case rf_id_undefined:
               ids->n[lex.id_node].val = (struct rf_id) { rf_id_module, lex.id_begin };
               // Поскольку другие модули могут импортировать такой же модуль,
               // необходимо обеспечить идентичность идентификаторов, а так же
               // нет смысла повторно транслировать уже импортированный модуль.
               // Обе задачи решаются импортом всех модулей в одну (глобальную)
               // область видимости.
               //
               // Про включении модуля в другой модуль, имя включаемого
               // дублируем в глобальном пространстве и в узел "пробел" копируем
               // соответствующий ему из пространства модуля, что обеспечит
               // единообразный импорт идентификаторов модуля.
               rtrie_index global = lex.id_node = rtrie_insert_next(ids, lex.id_node, ' ');
               if (module) {
                  const wchar_t *mn = &vm->id.s[lex.id_begin];
                  rtrie_index node = rtrie_insert_at(ids, 0, *mn);
                  while (*(++mn))
                     node = rtrie_insert_next(ids, node, *mn);
                  global = rtrie_find_next(ids, node, ' ');
                  if (global > 0) {
                     ids->n[lex.id_node] = ids->n[global];
                     break;
                  }
                  ids->n[node].val = (struct rf_id) { rf_id_module, lex.id_begin };
                  global = rtrie_insert_next(ids, node, ' ');
               }
               int r = refal_translate_module_to_bytecode(cfg, vm, ids, lex.id_node,
                                                    &vm->id.s[lex.id_begin], st);
               ids->n[global] = ids->n[lex.id_node];
               //TODO пока ошибки трансляции модуля не учитываются
               if (r < 0) {
                  error = "исходный текст модуля недоступен";
                  goto cleanup;
               }
            }//switch (ids->n[lex.node].val.tag)
            // Список импорта
importlist: while (L_semicolon != (lexeme = lexer_next_lexem(&lex, st))) {
               switch(lexeme) {
               case L_EOF: error = "список импорта должен завершаться ;";
                  goto cleanup;
               default: error = "ожидается идентификатор в списке импорта"; goto cleanup;
               case L_identifier: ;
                  rtrie_index import_node = rtrie_find_at(ids, lex.id_node, lexer_char(&lex));
                  lexem_identifier(&lex, ids, module, &import_node, vm, st);
                  if (import_node < 0) {
                     error = "идентификатор не определён в модуле (возможно, взаимно-рекурсивный импорт)";
                     goto cleanup;
                  }
                  if (ids->n[lex.node].val.tag != rf_id_undefined
                   && ids->n[lex.node].val.link != ids->n[import_node].val.link) {
                     error = "импортируемый идентификатор уже определён";
                     goto cleanup;
                  }
                  ids->n[lex.node].val = ids->n[import_node].val;
                  continue;
               }
            }//importlist
            continue;

         // Идентификатор предполагает последующее определение функции.
         default:
            assert(lex.id_node == lex.node);
            bool expression = false;
            rf_index cmd_sentence = 0; // ячейка с командой rf_sentence.
            int function_block = 0;    // подсчитывает блоки в функции (фигурные скобки).

            //TODO Рефал-5 позволяет переопределить встроенные функции.
            if (ids->n[lex.id_node].val.tag != rf_id_undefined) {
               // TODO надо бы отобразить прежнее определение
               error = "повторное определение идентификатора";
               goto cleanup;
            }
            // Заголовок функции.
            // Создаётся для всех, поскольку пустые во время выполнения
            // могут быть преобразованы в «ящик».
            // Опкод со ссылкой на имя хранится до исполняемых опкодов.
            rf_alloc_value(vm, lex.id_begin, rf_name);
            // Изначально считаем функцию пустой.
            // Изменим при наличии выражения-образца и -результата.
            ids->n[lex.id_node].val = (struct rf_id) { rf_id_reference, vm->free };
            switch (lexeme) {
            case L_semicolon:
               cmd_sentence = rf_alloc_command(vm, rf_sentence);
               vm->u[cmd_sentence].data = vm->free;
               continue;
            case L_equal:
               rf_alloc_command(vm, rf_equal);
               ids->n[lex.id_node].val.tag = rf_id_op_code;
               expression = true;
               lexeme = lexer_next_lexem(&lex, st);
               break;
            case L_block_open:
               cmd_sentence = rf_alloc_command(vm, rf_sentence);
               ++idc;
               ++function_block;
               lexeme = lexer_next_lexem(&lex, st);
               break;
            default:
               break;
            }

            rtrie_index imports = 0;   // корень для поиска идентификаторов модуля.
            rf_index local = 0;
            unsigned ep = 0;  // последний занятый элемент в массиве <>
            cmd_exec[ep] = 0; // изначально пустой (используется как признак, а не только при закрытии >)
            unsigned bp = 0;  // свободный элемент в массиве ()
            struct mod_msg mod = { 0, 0, 0 };
            bool expression_expected = false;
            // Тело функции.
            for (bool complete = false; !complete; lexeme = !complete ? lexer_next_lexem(&lex, st) : L_whitespace)
               switch(lexeme) {
               case L_EOF: error = function_block ? "не завершено определение функции (пропущена } ?)"
                                                  : "не завершено определение функции (пропущена ; ?)";
incomplete:       lex.line = lex.id_line;
                  lex.pos  = lex.id_pos;
                  lex.line_num = lex.id_line_num;
                  goto cleanup;
               case L_whitespace: assert(0); continue;

               case L_string: lexem_string(&lex, vm, st); continue;
               case L_number: lexem_number(&lex, vm, st);
                  if (lex_type(lexer_next_char(&lex)) != L_whitespace && lex_type(lexer_next_char(&lex)) == L_identifier)
                     warning(st, "идентификаторы следует отделять от цифр пробелом", lex.line_num, lex.pos + 1, &lex.buf.s[lex.line], &lex.buf.s[lex.buf.free]);
                  continue;

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
               case L_equal:
                  check_redundant_module_id(st, &lex, &imports, &mod);
                  if (!check_matching(st, &lex, bp, ep))
                     goto cleanup;
                  if (expression) {
                     error = "недопустимый оператор в выражении (пропущена ; ?)";
                     goto cleanup;
                  }
                  if (ids->n[lex.id_node].val.tag == rf_id_reference) {
                     ids->n[lex.id_node].val.tag = rf_id_op_code;
                  } else if (ids->n[lex.id_node].val.tag == rf_id_box) {
                     error = "предложение недопустимо в ящике (в предыдущем пропущен = ?)";
                     goto cleanup;
                  }
                  rf_alloc_command(vm, rf_equal);
                  expression = true;
                  continue;

               ///\subsection Блок           Начало и конец
               ///
               /// Фигурные скобки `{` и `}` означают начало и конец _блока_ (тела) функции.
               /// Блок может включать произвольное количество предложений.
               /// Пустой блок определяет невычислимую функцию.
               case L_block_open:
                  error = expression ? "вложенные блоки {} пока не поддерживаются"
                                     : "блок недопустим в выражении-образце";
                  goto cleanup;

               case L_block_close:
                  if (!function_block) {
                     error = "некорректное определение функции - пропущена {";
                     goto cleanup;
                  }
                  --function_block;
                  complete = true;
                  // После последнего предложения ; может отсутствовать.
                  // Если образец пуст, значит ;}
                  if (rf_is_evar_empty(vm, cmd_sentence, vm->free)) {
                     // Удаляем созданную в L_semicolon команду rf_sentence.
                     rf_free_last(vm);
                     continue;
                  }
                  // Функция закончена, следом должен быть rf_name.
                  vm->u[cmd_sentence].data = vm->free;
                  goto sentence_complete;

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
               case L_exec_open:
                  if (!expression) {
executor_in_pattern: error = "вычислительные скобки в образце не поддерживаются";
                     goto cleanup;
                  }
                  if (!(++ep < exec_max)) {
                     error = "превышен лимит вложенности вычислительных скобок";
                     goto cleanup;
                  }
                  check_redundant_module_id(st, &lex, &imports, &mod);
                  cmd_exec[ep] = rf_alloc_command(vm, rf_open_function);
                  continue;

               case L_exec_close:
                  if (!expression)
                     goto executor_in_pattern;
                  if (!ep) {
                     error = "непарная вычислительная скобка";
                     goto cleanup;
                  }
                  check_redundant_module_id(st, &lex, &imports, &mod);
                  assert(ep > 0);
                  // Копируем адрес функции из парной открывающей, для вызова исполнителем.
                  // Если функция не определена, но между скобок содержатся
                  // идентификаторы (.value == 1) задаём открывающей скобке ссылку на эту.
                  rf_index ec = rf_alloc_value(vm, vm->u[cmd_exec[ep]].data, rf_execute);
                  if (vm->u[cmd_exec[ep]].id.tag == rf_id_undefined) {
                     if (!vm->u[cmd_exec[ep]].id.link) {
                        error = "активное выражение должно содержать имя вычислимой функции";
                        goto cleanup;
                     }
                     vm->u[cmd_exec[ep]].data = ec;
                  }
                  cmd_exec[ep--] = 0;
                  continue;

               ///\subsection Структуры   Структурные скобки
               ///
               /// Круглые скобки `(` и `)` объединяют данные в группу, которая может
               /// рассматриваться как единое целое, или _терм_.
               case L_term_open: if (!(bp < bracket_max)) {
                     error = "превышен лимит вложенности структурных скобок";
                     goto cleanup;
                  }
                  bracket[bp++] = rf_alloc_command(vm, rf_opening_bracket);
                  continue;
               case L_term_close: if (!bp) {
                     error = "непарная структурная скобка";
                     goto cleanup;
                  }
                  rf_link_brackets(vm, bracket[--bp], rf_alloc_command(vm, rf_closing_bracket));
                  continue;

               ///\subsection Иначе
               ///
               /// Точка с запятой `;` разделяет предложения в блоке. Если РЕФАЛ-машина
               /// не распознаёт образец в текущем (первом) предложении блока, пробуется
               /// следующее.
               ///
               /// В данной реализации точка с запятой может идти непосредственно после
               /// идентификатора, определяя пустую функцию.
               case L_semicolon:
sentence_complete:
                  check_redundant_module_id(st, &lex, &imports, &mod);
                  if (!check_matching(st, &lex, bp, ep))
                     goto cleanup;
                  if (!expression) {
                     if (expression_expected) {
                        error = "образец без общего выражения (пропущено = ?)";
                        goto cleanup;
                     }
                     if (ids->n[lex.id_node].val.tag == rf_id_reference) {
                        if (!cmd_sentence) {
                           // В общем случае не следует менять значение идентификатора,
                           // но в данном происходит его определение, т.е.
                           // значение пока никуда не скопировано.
                           cmd_sentence = rf_alloc_command(vm, rf_sentence);
                           rf_splice_evar_prev(vm, vm->u[cmd_sentence].prev, vm->free, ids->n[lex.id_node].val.link);
                           ids->n[lex.id_node].val.link = cmd_sentence;
                        }
                        ids->n[lex.id_node].val.tag = rf_id_box;
                     } else if (ids->n[lex.id_node].val.tag == rf_id_op_code) {
                        error = "данные ящика недопустимы в исполняемой функции (пропущен = ?)";
                        goto cleanup;
                     }
                  } else if (vm->u[vm->u[vm->free].prev].op == rf_execute) {
                     // При хвостовых вызовах нет смысла в парном сохранении и
                     // восстановление контекста функции. Обозначим такие исполнителю.
                     vm->u[vm->u[vm->free].prev].mode = rf_op_exec_tailcall;
                  }
                  // В функциях с блоком и однострочных с выражением-образцом
                  // сохраняем в маркере текущего предложения
                  // ссылку на данные следующего и добавляем новый маркер.
                  if (cmd_sentence)
                     vm->u[cmd_sentence].data = vm->free;
                  if (function_block) {
                     assert(cmd_sentence);
                     rf_alloc_command(vm, rf_sentence);
                     cmd_sentence = vm->u[cmd_sentence].data;
                     local = 0;
                     ++idc;
                  } else {
                     complete = true;
                  }
                  expression = false;
                  expression_expected = false;
                  continue;

               ///\subsection Является
               ///
               /// Двоеточие `:` в Расширенном РЕФАЛ используется в условиях (не реализовано).
               ///
               /// Применяется для импорта модулей, как замена $EXTERN (и *$FROM Refal-05).
               ///
               ///     ИмяМодуля: функция1 функция2;
               ///
               /// В Рефал-М выражение-образец (слева от `=`) может состоять из
               /// нескольких участков (образцов), разделяемых двоеточием `:`.
               /// Каждый сопоставляются с Полем Зрения по очереди,
               /// при этом переменные сохраняют присвоенные ранее значения.
               /// Сопоставление считается успешным, только когда все образцы
               /// подошли (логическое "И").
               case L_colon:
                  check_redundant_module_id(st, &lex, &imports, &mod);
                  if (!check_matching(st, &lex, bp, ep))
                     goto cleanup;
                  if (expression) {
                     error = "условия не поддерживаются"; goto cleanup;
                  }
                  expression_expected = true;
                  rf_alloc_command(vm, rf_colon);
                  continue;

               case L_identifier:
                  lexem_identifier_exp(&lex, ids, module, imports, idc, vm, st);
                  switch (lex.id_type) {
                  case id_svar: case id_tvar: case id_evar:
                     if (local == local_max) {
                        error = "превышен лимит переменных";
                        goto cleanup;
                     }
                     if (ids->n[lex.node].val.tag == rf_id_undefined) {
                        assert(!(lex.node < 0));
                        if (expression) {
                           error = "идентификатор не определён";
                           goto cleanup;
                        }
                        var[local].opcode = 0;
                        ids->n[lex.node].val = (struct rf_id) { rf_id_enum, local++ };
                     }
                     if (!expression) {
                        expression_expected = true;
                        rf_alloc_value(vm, ids->n[lex.node].val.link, (enum rf_opcode)lex.id_type);
                        continue;
                     }
                     // При первом вхождении создаём переменную и запоминаем её индекс.
                     // При следующем вхождении устанавливаем значение mode по
                     // сохранённому индексу, а индекс заменяем на текущий.
                     rf_index id = ids->n[lex.node].val.link;
                     if (lex.id_type != id_svar && var[id].opcode) {
                        vm->u[var[id].opcode].mode = rf_op_var_copy;
#if REFAL_TRANSLATOR_PERFORMANCE_NOTICE_EVAR_COPY
                        if (cfg && cfg->notice_copy) {
                           performance(st, "создаётся копия переменной", var[id].line,
                                           var[id].pos, &lex.buf.s[var[id].src], &lex.buf.s[lex.buf.free]);
                        }
#endif
                     }
                     var[id].opcode = rf_alloc_value(vm, id, (enum rf_opcode)lex.id_type);
#if REFAL_TRANSLATOR_PERFORMANCE_NOTICE_EVAR_COPY
                     var[id].src  = lex.line;
                     var[id].line = lex.line_num;
                     var[id].pos  = lex.pos;
#endif
                     continue;
                  case id_global:
                     if (lex.node < 0) {
                        assert(imports);
                        goto no_identifier_in_module;
                     }
                     switch (ids->n[lex.node].val.tag) {
                     // Используем ветку модуля для поиска следующего идентификатора.
                     case rf_id_module:
                        if (imports) {
                           warning(st, "указание модуля потеряло смысл", mod.line_num, mod.pos, &lex.buf.s[mod.line], &lex.buf.s[lex.buf.free]);
                           warning(st, "повторное имя модуля переопределяет предыдущее", lex.line_num, lex.pos, &lex.buf.s[lex.line], &lex.buf.s[lex.buf.free]);
                        }
                        imports = rtrie_find_next(ids, lex.node, ' ');
                        mod.line = lex.line;
                        mod.pos  = lex.pos;
                        mod.line_num = lex.line_num;
                        assert(imports > 0);
                        continue;
                     // Если открыта вычислительная скобка, задаём ей адрес
                     // первой вычислимой функции из выражения.
                     case rf_id_op_code: case rf_id_mach_code:
                        if (cmd_exec[ep] && vm->u[cmd_exec[ep]].id.tag == rf_id_undefined) {
                           // Если в поле действия данной скобки встретился идентификатор,
                           // который на данный момент не определён, не известно,
                           // вычислим ли он. Возможно, именно определённая для него
                           // функция и должна быть вызвана скобкой. В таком случае
                           // откладываем решение до этапа, когда разрешаются
                           // неопределённые на данном проходе идентификаторы.
                           if (vm->u[cmd_exec[ep]].id.link)
                              goto implicit_declaration;
                           rf_assign_id(vm, cmd_exec[ep], ids->n[lex.node].val);
                           imports = 0;
                           continue;
                        }
                        [[fallthrough]];
                     default:
                        rf_alloc_identifier(vm, ids->n[lex.node].val);
                        imports = 0;
                        continue;
                     case rf_id_undefined:
implicit_declaration:   if (imports) {
no_identifier_in_module:   error = "идентификатор не определён в модуле";
                           goto cleanup;
                        }
                        // После первого прохода поищем, не появилось ли определение.
                        rf_alloc_value(vm, lex.node, rf_undefined);
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
                        if (cmd_exec[ep] && vm->u[cmd_exec[ep]].id.tag == rf_id_undefined) {
                           rf_alloc_value(vm, cmd_exec[ep], rf_open_function);
                           rf_assign_id(vm, cmd_exec[ep], (struct rf_id) { rf_id_undefined, 1 });
                        }
                        // На случай ошибки.
                        rf_alloc_value(vm, lex.line_num, rf_undefined);
                        rf_alloc_value(vm, lex.pos, rf_undefined);
                        rf_alloc_value(vm, lex.line, rf_undefined);
                        continue;
                     }
                  }// switch (lex.id_type)
               }// case L_identifier
         }
      }
   }
   // Завершаем последнюю функцию пустышкой, куда указывает rf_sentence.
   rf_alloc_value(vm, 0, rf_name);

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
         assert(vm->u[undef].op == rf_undefined);
         const rf_index opcode = vm->u[undef].prev;
         rf_index s = vm->u[undef].next;
         undef = vm->u[undef].link;

         rf_index exec_open = 0;
         if (vm->u[s].op == rf_open_function) {
            exec_open = vm->u[s].link;
            s = vm->u[s].next;
         } else if (ex) {
            continue;
         }
         lex.line_num = vm->u[s].num;
         s = vm->u[s].next;
         lex.pos  = vm->u[s].num;
         s = vm->u[s].next;
         lex.line = vm->u[s].num;
         s = vm->u[s].next;

         // В результате трансляции rf_name в данной позиции невозможен,
         // потому выбран в качестве маркера на предыдущей итерации.
         if (vm->u[opcode].op == rf_name) {
            assert(!ex);
            rf_free_evar(vm, vm->u[opcode].prev, s);
            continue;
         }
         rtrie_index n = vm->u[opcode].link;
         if (ids->n[n].val.tag == rf_id_undefined) {
            // TODO опциональное поведение?
            if (ex)
               continue;
            if (cfg && cfg->warn_implicit_declaration) {
               warning(st, "неявное определение идентификатора", lex.line_num, lex.pos, &lex.buf.s[lex.line], &lex.buf.s[lex.buf.free]);
            }
            --enum_couner.link;
            ids->n[n].val = enum_couner;
         }
         // Скобке присваивается первая вычислимая функция, так что порядок
         // обработки списка неопределённых идентификаторов важен.
         if (exec_open) {
            rf_index exec_close = vm->u[exec_open].link;
            if (vm->u[exec_close].id.tag == rf_id_undefined) {
               if (ids->n[n].val.tag == rf_id_op_code || ids->n[n].val.tag == rf_id_mach_code) {
                  assert(ex);
                  rf_assign_id(vm, exec_close, ids->n[n].val);
                  // временный маркер для следующей итерации.
                  vm->u[opcode].op = rf_name;
                  continue;
               } else if (ex) {
                  continue;
               } else if (last_exec != exec_close) {
                  last_exec = exec_close;
                  syntax_error(st, "активное выражение должно содержать "
                               "вычислимую функцию", lex.line_num, lex.pos, &lex.buf.s[lex.line], &lex.buf.s[lex.buf.free]);
               }
            } else if (ex) {
               continue;
            }
         }
         vm->u[opcode].op  = rf_identifier;
         rf_assign_id(vm, opcode, ids->n[n].val);
         rf_free_evar(vm, opcode, s);
      }
   }

cleanup:
   //TODO error = "неполный символ UTF-8"; "недействительный символ UTF-8";
   if (error)
      syntax_error(st, error, lex.line_num, lex.pos, &lex.buf.s[lex.line], &lex.buf.s[lex.buf.free]);

   lexer_free(&lex);
   //TODO количество ошибок не подсчитывается.
   return error ? 1 : 0;
}
