
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
   FILE *src;
   const char *os;
   if (name) {
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
      if (name)
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

// Состояние лексического анализатора.
enum lexer_state {
   lex_leadingspace,    // Пробелы в начале строки.
   lex_whitespace,      // Пробелы после лексемы.
   lex_number,          // Целое число.
};

enum lexem_type {
   L_unspecified,
   L_identifier,
   L_string,

   L_whitespace   = ' ',
   L_exec_open    = '<',
   L_exec_close   = '>',
   L_block_open   = '{',
   L_block_close  = '}',
   L_term_open    = '(',
   L_term_close   = ')',
   L_equal        = '=',
   L_Dquote       = '"',
   L_quote        = '\'',
   L_semicolon    = ';',
   L_colon        = ':',
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
      case '"':   return L_Dquote;
      case '\'':  return L_quote;
      case ';':   return L_semicolon;
      case ':':   return L_colon;
      default:    return L_unspecified;
   }
}

struct lexer {

   enum lexer_state  state;

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

   // Последний узел глобального идентификатора (текущей функции).
   // Используется как корень для локальных (переменных)
   // и для возможности изменить тип функции (с пустой на вычислимую).
   rtrie_index ident;

   enum identifier_type id_type;

   // Первый символ идентификатора в массиве атомов.
   // Используются при импорте и встраивании ссылки на имя функции в байткод.
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
   lex->state    = lex_leadingspace;
   lex->node     = 0;
   lex->id_type  = id_global;
   lex->ident    = 0;
   lex->id_begin = 0;
   lex->id_line  = 0;
   lex->id_pos   = 0;
   lex->id_line_num = 0;
   lex->line_num = 0;
   lex->pos      = 0;

   wstr_alloc(&lex->buf, 1024);
   lex->line = lex->buf.free;
   read_file(&lex->buf, src);
}

static inline
void lexer_free(struct lexer *lex)
{
   wstr_free(&lex->buf);
}

/**
 * В первой строке могут быть символы #!, тогда игнорируем её.
 */
static inline
void lexer_check_hashbang(struct lexer *lex)
{
   //TODO
}

static inline
void lexer_next_line(struct lexer *lex)
{
   lex->line += lex->pos;
   lex->line_num++;
   lex->pos = 0;
}

static inline
wchar_t lexer_next_char(struct lexer *lex)
{
   return lex->buf.s[lex->line + lex->pos++];
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


static inline
void lexem_identifier(struct lexer *lex, wchar_t *chr, struct refal_trie *ids,
      rtrie_index module, rtrie_index imports, rtrie_index *import_node,
      struct refal_vm *vm, struct refal_message *st)
{
   lex->id_line = lex->line;
   lex->id_pos  = lex->pos - 1;
   lex->id_line_num = lex->line_num;
   // Перечисленные после имени модуля идентификаторы импортируются,
   // для чего ищутся в модуле и определяются в текущей области видимости.
   if (!(imports < 0))
      *import_node = rtrie_find_at(ids, imports, *chr);
   lex->node = rtrie_insert_at(ids, module, *chr);
   lex->id_begin = wstr_append(&vm->id, *chr);
   while (1) {
      *chr = lexer_next_char(lex);
      if (lex_type(*chr) != L_unspecified)
         return;
      if (!(imports < 0))
         *import_node = rtrie_find_next(ids, *import_node, *chr);
      lex->node = rtrie_insert_next(ids, lex->node, *chr);
      wstr_append(&vm->id, *chr);
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
 * Для выражения-результа такие должны быть определены, выполняется поиск.
 * В случае отсутствия lex->node отрицателен.
 */
static inline
void lexem_identifier_exp(struct lexer *lex, wchar_t *chr, struct refal_trie *ids,
      rtrie_index module, rtrie_index imports, wchar_t idc, int pattern,
      struct refal_vm *vm, struct refal_message *st)
{
   switch (*chr) {
   case L'…':
   case '.': lex->id_type = id_evar; goto local;
   case '?': lex->id_type = id_svar; goto local;
   case '!': lex->id_type = id_tvar; goto local;
   case 'e': lex->id_type = id_evar; goto check_local;
   case 't': lex->id_type = id_tvar; goto check_local;
   case 's': lex->id_type = id_svar;
check_local:
      if (lex->buf.s[lex->line + lex->pos] == '.') {
        ++lex->pos;
local:   lex->node = pattern ? rtrie_insert_next(ids, lex->ident, idc)
                             : rtrie_find_next(ids, lex->ident, idc);
         lex->node = pattern ? rtrie_insert_next(ids, lex->node, *chr)
                             : rtrie_find_next(ids, lex->node, *chr);
         goto tail;
      }
      [[fallthrough]];
   default:
      lex->id_type = id_global;
      lex->node = imports ? rtrie_find_at(ids, imports, *chr)
                          : rtrie_insert_at(ids, module, *chr);
   }
   while (1) {
tail: *chr = lexer_next_char(lex);
      if (lex_type(*chr) != L_unspecified)
         return;
      lex->node = imports ? rtrie_find_next(ids, lex->node, *chr)
                          : rtrie_insert_next(ids, lex->node, *chr);
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
 * Переносит строку из исходного текста в байт-код.
 */
static inline
void lexem_string(struct lexer *lex, wchar_t *chr, struct refal_vm *vm, struct refal_message *st)
{
   wchar_t q = *chr;
   while(1) {
      *chr = lexer_next_char(lex);
      switch(*chr) {
      // Кавычка внутри строки кодируется сдвоенной, иначе завершает строку.
      case '"': case '\'':
         if (*chr != q) break;
         if (lex->buf.s[lex->line + lex->pos] == q) {
            ++lex->pos;
            break;
         }
         lex->state = lex_whitespace;
         return;
      case '\\': switch (lex->buf.s[lex->line + lex->pos]) {
         case 't': ++lex->pos; *chr = '\t'; break;
         case 'n': ++lex->pos; *chr = '\n'; break;
         case 'r': ++lex->pos; *chr = '\r'; break;
         default : break;
         }
         break;
      case '\r': case '\n':
         //TODO Позволить многострочные?
         syntax_error(st, "отсутствует закрывающая кавычка", lex->line_num, lex->pos, &lex->buf.s[lex->line], &lex->buf.s[lex->buf.free]);
         lex->state = lex_whitespace;
         return;
      default:
         if (*chr < ' ')
            warning(st, "нечитаемый символ", lex->line_num, lex->pos, &lex->buf.s[lex->line], &lex->buf.s[lex->buf.free]);
         break;
      }
      rf_alloc_char(vm, *chr);
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
 * Переносит макрофицру из исходного текста в байт-код.
 */
static inline
void lexem_number(struct lexer *lex, wchar_t *chr, struct refal_vm *vm, struct refal_message *st)
{
   lex->state = lex_number;
   rf_int   number = 0;
   while ((*chr >= '0') && (*chr <= '9')) {
      number = number * 10 + (*chr - '0');
      if (number < (*chr - '0')) {
         warning(st, "целочисленное переполнение", lex->line_num, lex->pos, &lex->buf.s[lex->line], &lex->buf.s[lex->buf.free]);
      }
     *chr = lexer_next_char(lex);
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
 * Пропускает комментарий.
 */
static inline
void lexem_comment(struct lexer *lex, wchar_t *chr, int multiline, struct refal_vm *vm, struct refal_message *st)
{
   while (1) {
      *chr = lexer_next_char(lex);
      switch(*chr) {
      case '\0':
         if (multiline)
            syntax_error(st, "не закрыт комментарий /* */", lex->line_num, lex->pos, &lex->buf.s[lex->line], &lex->buf.s[lex->buf.free]);
         return;
      case '\r': case '\n':
         lexer_next_line(lex);
         if (!multiline) {
            lex->state = lex_leadingspace;
            return;
         }
         break;
      case '*':
         if (multiline && lex->buf.s[lex->line + lex->pos] == '/') {
            ++lex->pos;
            lex->state = lex_whitespace;
            return;
         }
      default: break;
      }
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

   // Пространство имён текущего импортируемого модуля.
   // Кроме того, идентификатор модуля может встречаться и в выражениях,
   // определяя область поиска следующего за ним идентификатора.
   rtrie_index imports = 0;

   // Для вывода предупреждений об идентификаторах модулей.
   const char *redundant_module_id = "идентификатор модуля без функции не имеет смысла";

   // При импорте идентификаторов параллельно с определением в текущей
   // области видимости происходит поиск в пространстве имён модуля.
   rtrie_index import_node = 0;

   // Пустым функциям (ENUM в Refal-05) для возможности сопоставления
   // присваивается уникальное значение.
   // Значение 0 используется для идентификаторов модулей (при разрешении имён)
   // и в целевой код не переносится.
   // В случае явного определения используем индекс первого символа их имени
   // в таблице атомов \c id_begin. Что бы гарантировать ненулевые значения,
   // заносим лишний символ в начало таблицы.
   wstr_append(&vm->id, module);
   // В случае неявного определения, имена идентификаторов на заносятся в таблицу
   // атомов. Что бы получить уникальные для каждого модуля значения, используем
   // номер свободной ячейки — количество занятых не превышает количество
   // сгенерированных значений.
   // Что бы значения не пересекались, для второго используем дополнительный код.
   struct rtrie_val enum_couner = { .value = -ids->free };

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

   struct lexer lex;
   lexer_init(&lex, src);
   if (!wstr_check(&lex.buf, st))
      goto cleanup;

   lexer_check_hashbang(&lex);

next_line:
   lexer_next_line(&lex);

next_char: ;
   wchar_t chr = lexer_next_char(&lex);

current_char:
   switch (chr) {

   case '\0': goto complete;

   case '\t':  //TODO pos += 7;
   case ' ':
      switch (lex.state) {
      case lex_leadingspace:
      case lex_whitespace:
         goto next_char;
      case lex_number:
         lex.state = lex_whitespace;
         goto next_char;

         // Ветка определяет идентификатор, а обработку текущего символа
         // производит последующим переходом на current_char.
lexem_identifier_complete:
         lex.state = lex_whitespace;
         switch (semantic) {
         case ss_identifier:  // TODO 2 идентификатора подряд?
            assert(0);        // Д.б обработано при появлении 2го в error_identifier_odd
         case ss_source:
            wstr_append(&vm->id, L'\0');
            semantic = ss_identifier;
            lex.ident = lex.node;
            // Функции определяются во множестве мест, вынесено сюда.
            // Производить определение функций здесь возможно, но придётся
            // распознавать ситуацию с импортом модуля, идентификатор которого
            // может встречаться на верхнем уровне многократно.
            local = 0;
            cmd_sentence = 0;
            goto current_char;
         case ss_import:
            wstr_append(&vm->id, L'\0');
            if (import_node < 0) {
               error = "идентификатор не определён в модуле (возможно, взаимно-рекурсивный импорт)";
               goto cleanup;
            }
            if (ids->n[lex.node].val.tag != rft_undefined) {
               error = "импортируемый идентификатор уже определён";
               goto cleanup;
            }
            ids->n[lex.node].val = ids->n[import_node].val;
            goto current_char;
         case ss_pattern:
            assert(!cmd_exec[ep]);
            switch (lex.id_type) {
            case id_svar:
            case id_tvar:
            case id_evar:
               if (local == local_max) {
                  error = "превышен лимит переменных";
                  goto cleanup;
               }
               if (ids->n[lex.node].val.tag == rft_undefined) {
                  var[local].opcode = 0;
                  ids->n[lex.node].val.tag   = rft_enum;
                  ids->n[lex.node].val.value = local++;
               }
               rf_alloc_value(vm, ids->n[lex.node].val.value, lex.id_type);
               goto current_char;
            case id_global:
               goto lexem_identifier_complete_global;
            }
         case ss_expression:
            switch (lex.id_type) {
            case id_svar:
            case id_tvar:
            case id_evar:
               if (ids->n[lex.node].val.tag == rft_undefined) {
                  goto error_identifier_undefined;
               }
               // При первом вхождении создаём переменную и запоминаем её индекс.
               // При следующем вхождении устанавливаем значение tag2 по
               // сохранённому индексу, а индекс заменяем на текущий.
               rf_index id = ids->n[lex.node].val.value;
               if (lex.id_type != id_svar && var[id].opcode) {
                  vm->u[var[id].opcode].tag2 = 1;
#if REFAL_TRANSLATOR_PERFORMANCE_NOTICE_EVAR_COPY
                  if (cfg && cfg->notice_copy) {
                     performance(st, "создаётся копия переменной", var[id].line,
                                     var[id].pos, &lex.buf.s[var[id].src], &lex.buf.s[lex.buf.free]);
                  }
#endif
               }
               var[id].opcode = rf_alloc_value(vm, id, lex.id_type);
#if REFAL_TRANSLATOR_PERFORMANCE_NOTICE_EVAR_COPY
               var[id].src  = lex.line;
               var[id].line = lex.line_num;
               var[id].pos  = lex.pos - 1;
#endif
               goto current_char;
            case id_global:
               break;
            }
lexem_identifier_complete_global:
            if (lex.node < 0) {
               assert(imports);
               goto error_no_identifier_in_module;
            }
            if (ids->n[lex.node].val.tag != rft_undefined) {
               // Если открыта вычислительная скобка, задаём ей адрес
               // первой вычислимой функции из выражения.
               if (ids->n[lex.node].val.tag != rft_enum && cmd_exec[ep]
               && rtrie_val_from_raw(vm->u[cmd_exec[ep]].data).tag == rft_undefined) {
                  // Если в поле действия данной скобки встретился идентификатор,
                  // который на данный момент не определён, не известно,
                  // вычислим ли он. Возможно, именно определённая для него
                  // функция и должна быть вызвана скобкой. В таком случае
                  // откладываем решение до этапа, когда разрешаются
                  // неопределённые на данном проходе идентификаторы.
                  if (rtrie_val_from_raw(vm->u[cmd_exec[ep]].data).value)
                     goto lexem_identifier_undefined;
                  vm->u[cmd_exec[ep]].data = rtrie_val_to_raw(ids->n[lex.node].val);
                  imports = 0;
               }
               // rft_enum и нулевое значение означают идентификатор модуля.
               // Используем соответствующую ветку для поиска следующего идентификатора.
               else if (ids->n[lex.node].val.tag == rft_enum && !ids->n[lex.node].val.value) {
                  assert(!imports);
                  imports = rtrie_find_next(ids, lex.node, ' ');
                  lex.id_line = lex.line;
                  lex.id_pos  = lex.pos - 1;
                  lex.id_line_num = lex.line_num;
                  assert(imports > 0);
               } else {
                  rf_alloc_value(vm, rtrie_val_to_raw(ids->n[lex.node].val), rf_identifier);
                  imports = 0;
               }
            } else {
lexem_identifier_undefined:
               if (imports) {
                  goto error_no_identifier_in_module;
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
               if (cmd_exec[ep] && rtrie_val_from_raw(vm->u[cmd_exec[ep]].data).tag == rft_undefined) {
                  rf_alloc_value(vm, cmd_exec[ep], rf_open_function);
                  struct rtrie_val f = { .tag = rft_undefined, .value = 1 };
                  vm->u[cmd_exec[ep]].data = rtrie_val_to_raw(f);
               }
               // На случай ошибки.
               rf_alloc_value(vm, lex.line_num, rf_undefined);
               rf_alloc_value(vm, lex.pos - 1, rf_undefined);
               rf_alloc_value(vm, lex.line, rf_undefined);
            }
            goto current_char;
         } // case lex_identifier: switch (semantic)
      }

   // Конец строки.
   case '\r':  // возможный последующий '\n' не сохраняется в буфере.
   case '\n':
      switch (lex.state) {
      case lex_number:
      case lex_leadingspace:
      case lex_whitespace:
         lex.state = lex_leadingspace;
         goto next_line;
      }

   // Начинает строку комментариев, либо может завершать комментарий в стиле Си.
   // TODO последовательность */ без предшествующей /* воспринимается как
   //      однострочный комментарий.
   case '*':
      switch (lex.state) {
      case lex_leadingspace:
         lexem_comment(&lex, &chr, 0, vm, st);
         goto next_char;
      case lex_number:
      case lex_whitespace:
         goto symbol;
      }

   case '/':
      switch (lex.state) {
      case lex_leadingspace:
      case lex_whitespace:
         switch(lex.buf.s[lex.line + lex.pos]) {
         case '/':
            ++lex.pos;
            lexem_comment(&lex, &chr, 0, vm, st);
            goto next_char;
         case '*':
            ++lex.pos;
            lexem_comment(&lex, &chr, 1, vm, st);
            goto next_char;
         default:
            goto symbol;
         }
      case lex_number:
         goto symbol;
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
      switch (lex.state) {
      case lex_number:
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
            if (ids->n[lex.ident].val.tag != rft_undefined)  \
               goto error_identifier_already_defined;    \
            ids->n[lex.ident].val.value = vm->free;          \
            ids->n[lex.ident].val.tag   = rft_byte_code;

            DEFINE_SIMPLE_FUNCTION;
            // Содержит ссылку на таблицу атомов.
            rf_alloc_value(vm, lex.id_begin, rf_equal);
            lex.state = lex_whitespace;
            semantic = ss_expression;
            goto next_char;
         case ss_pattern:
            if (bp) {
               error = "не закрыта структурная скобка";
               goto cleanup;
            }
            if (imports) {
               warning(st, redundant_module_id, lex.id_line_num, lex.id_pos, &lex.buf.s[lex.id_line], &lex.buf.s[lex.buf.free]);
               imports = 0;
            }
            if (ids->n[lex.ident].val.tag == rft_enum) {
               assert(cmd_sentence);
               ids->n[lex.ident].val.tag   = rft_byte_code;
               ids->n[lex.ident].val.value = vm->u[cmd_sentence].prev;
            }
            // TODO проверить скобки ().
            rf_alloc_command(vm, rf_equal);
            lex.state = lex_whitespace;
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
      switch (lex.state) {
      case lex_number:
         // TODO Вложенные блоки пока не поддержаны.
         goto error_incorrect_function_definition;
      case lex_leadingspace:
      case lex_whitespace:
         switch (semantic) {
         case ss_source:
            goto error_identifier_missing;
         case ss_import:
            goto error_incorrect_import;
         case ss_identifier:
            assert(function_block == 0);
            if (ids->n[lex.node].val.tag != rft_undefined) {
               goto error_identifier_already_defined;
            }
            // Предварительно считаем функцию невычислимой.
            // При наличии предложений сменим тип и значение.
            // Такой подход приводит к расходу двух лишних ячеек, однако,
            // вряд ли стоит переусложнять код - кому нужны пустые функции,
            // наверняка предпочтёт краткую запись: ид;
            rf_alloc_value(vm, lex.id_begin, rf_nop_name);
            cmd_sentence = rf_alloc_command(vm, rf_sentence);
            assert(lex.ident == lex.node);
            ids->n[lex.node].val.tag   = rft_enum;
            ids->n[lex.node].val.value = lex.id_begin;
            lex.state = lex_whitespace;
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
      switch (lex.state) {
      case lex_number:
         lex.state = lex_whitespace;
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
      switch (lex.state) {
      case lex_number:
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
               warning(st, redundant_module_id, lex.id_line_num, lex.id_pos, &lex.buf.s[lex.id_line], &lex.buf.s[lex.buf.free]);
               imports = 0;
            }
            cmd_exec[ep] = rf_alloc_command(vm, rf_open_function);
            lex.state = lex_whitespace;
            goto next_char;
         }
      }

   case '>':
      switch (lex.state) {
      case lex_number:
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
               warning(st, redundant_module_id, lex.id_line_num, lex.id_pos, &lex.buf.s[lex.id_line], &lex.buf.s[lex.buf.free]);
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
            lex.state = lex_whitespace;
            goto next_char;
         }
      }

   ///\subsection Структуры   Структурные скобки
   ///
   /// Круглые скобки `(` и `)` объединяют данные в группу, которая может
   /// рассматриваться как единое целое, или _терм_.

   case '(':
      switch (lex.state) {
      case lex_number:
      case lex_leadingspace:
      case lex_whitespace:
         switch (semantic) {
         case ss_source:
            goto error_identifier_missing;
         case ss_import:
            goto error_incorrect_import;
         case ss_identifier:
            DEFINE_SIMPLE_FUNCTION;
            rf_alloc_value(vm, lex.id_begin, rf_nop_name);
            semantic = ss_pattern;
         case ss_pattern:
         case ss_expression:
            if (!(bp < bracket_max)) {
               error = "превышен лимит вложенности структурных скобок";
               goto cleanup;
            }
            bracket[bp++] = rf_alloc_command(vm, rf_opening_bracket);
            lex.state = lex_whitespace;
            goto next_char;
         }
      }

   case ')':
      switch (lex.state) {
      case lex_number:
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
            lex.state = lex_whitespace;
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
      switch (lex.state) {
      case lex_number:
         lex.state = lex_whitespace;
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
            if (ids->n[lex.node].val.tag != rft_undefined) {
               goto error_identifier_already_defined;
            }
            ids->n[lex.node].val.tag   = rft_enum;
            ids->n[lex.node].val.value = lex.id_begin;
            lex.state = lex_whitespace;
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
               warning(st, redundant_module_id, lex.id_line_num, lex.id_pos, &lex.buf.s[lex.id_line], &lex.buf.s[lex.buf.free]);
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
      switch (lex.state) {
      case lex_number:
         lex.state = lex_whitespace;
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
            switch (ids->n[lex.node].val.tag) {
            case rft_machine_code:
            case rft_byte_code:
               goto error_identifier_already_defined;
            case rft_enum:
               if (ids->n[lex.node].val.value)
                  goto error_identifier_already_defined;
               // Если идентификатор определён со значением 0, значит трансляция
               // модуля уже выполнена. Импортируем идентификаторы.
               imports = rtrie_find_next(ids, lex.node, ' ');
               assert(!(imports < 0));
               semantic = ss_import;
               goto next_char;
            case rft_undefined:
               break;
            }
            ids->n[lex.node].val.tag   = rft_enum;
            ids->n[lex.node].val.value = 0;
            lex.state = lex_whitespace;
            // Имя модуля внесено в текущее пространство имён, что гарантирует
            // отсутствие иного одноимённого идентификатора и даёт возможность
            // квалифицированного (именем модуля) поиска идентификаторов.
            //
            // Поскольку другие модули могут импортировать этот же модуль,
            // необходимо обеспечить идентичность идентификаторов, а так же нет
            // смысла повторно транслировать уже импортированный модуль.
            // Обе задачи решаются импортом всех модулей в одну (глобальную)
            // область видимости.

            // При включении модуля в исходный файл, имя модуля внесено
            // в глобальное пространство, а идентификаторы модуля - в своё.

            // Про включении модуля в другой модуль, имя включаемого
            // дублируется в глобальном пространстве и в узел "пробел" копируется
            // соответствующий ему из пространства модуля, что обеспечит
            // единообразный импорт идентификаторов модуля.
            imports = rtrie_insert_next(ids, lex.node, ' ');
            if (module) {
               const wchar_t *mn = &vm->id.s[lex.id_begin];
               lex.node = rtrie_insert_at(ids, 0, *mn);
               while (*(++mn))
                  lex.node = rtrie_insert_next(ids, lex.node, *mn);
               lex.node = rtrie_find_next(ids, lex.node, ' ');
               if (lex.node > 0) {
                  semantic = ss_import;
                  ids->n[imports] = ids->n[lex.node];
                  goto next_char;
               }
               // Сейчас это мёртвая ветка.
               lex.node = rtrie_insert_next(ids, lex.node, ' ');
            }
            int r = refal_translate_module_to_bytecode(cfg, vm, ids, imports,
                                                      &vm->id.s[lex.id_begin], st);
            if (r < 0) {
               error = "исходный текст модуля недоступен";
               goto cleanup;
            } else { //TODO пока ошибки трансляции модуля не учитываются
               semantic = ss_import;
               goto next_char;
            }
         case ss_pattern:
         case ss_expression:
            error = "условия не поддерживаются";
            goto cleanup;
         }
      }

   // Начинает и заканчивает строку знаковых символов.
   case '"':
   case '\'':
      switch (lex.state) {
      case lex_number:
         lexem_string(&lex, &chr, vm, st);
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
            rf_alloc_value(vm, lex.id_begin, rf_nop_name);
            semantic = ss_pattern;
         case ss_pattern:
         case ss_expression:
            lexem_string(&lex, &chr, vm, st);
            goto next_char;
         }
      }

   // Начало целого числа, либо продолжение идентификатора.
   case '0'...'9':
      switch (lex.state) {
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
            rf_alloc_value(vm, lex.id_begin, rf_nop_name);
            semantic = ss_pattern;
         case ss_pattern:
         case ss_expression:
            lexem_number(&lex, &chr, vm, st);
            goto current_char;
         }
      case lex_number:
         assert(0);
         goto next_char;
      }

   // Оставшиеся символы считаются допустимыми для идентификаторов.
   default:
symbol:
      switch (lex.state) {
      case lex_number:
         // TODO символ после цифры?
         // Может быть частью шестнадцатеричного числа, что пока не поддержано.
         warning(st, "идентификаторы следует отделять от цифр пробелом", lex.line_num, lex.pos, &lex.buf.s[lex.line], &lex.buf.s[lex.buf.free]);
      case lex_leadingspace:
      case lex_whitespace:
         switch (semantic) {
         case ss_import:
            lexem_identifier(&lex, &chr, ids, module, imports, &import_node, vm, st);
            goto lexem_identifier_complete;
         case ss_source:
            lexem_identifier(&lex, &chr, ids, module, -1, &import_node, vm, st);
            goto lexem_identifier_complete;
         case ss_identifier:
            DEFINE_SIMPLE_FUNCTION;
            rf_alloc_value(vm, lex.id_begin, rf_nop_name);
            semantic = ss_pattern;
         case ss_pattern:
            lexem_identifier_exp(&lex, &chr, ids, module, imports, idc, 1, vm, st);
            goto lexem_identifier_complete;
         case ss_expression:
            lexem_identifier_exp(&lex, &chr, ids, module, imports, idc, 0, vm, st);
            goto lexem_identifier_complete;
         }
      }
   }
   assert(0);

complete:
   if (semantic != ss_source) {
      if (function_block)
         error = "не завершено определение функции (пропущена } ?)";
      else
         error = "не завершено определение функции (пропущена ; ?)";
      lex.line = lex.id_line;
      lex.pos  = lex.id_pos + 1;
      lex.line_num = lex.id_line_num;
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
         lex.line_num = vm->u[s].num;
         s = vm->u[s].next;
         lex.pos  = vm->u[s].num;
         s = vm->u[s].next;
         lex.line = vm->u[s].num;
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
               warning(st, "неявное определение идентификатора", lex.line_num, lex.pos, &lex.buf.s[lex.line], &lex.buf.s[lex.buf.free]);
            }
            if (!(vm->id.free < enum_couner.value)) {
               critical_error(st, "исчерпан диапазон ENUM", vm->id.free, enum_couner.value);
            }
            ids->n[n].val.tag   = rft_enum;
            ids->n[n].val.value = --enum_couner.value;
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
                               "вычислимую функцию", lex.line_num, lex.pos, &lex.buf.s[lex.line], &lex.buf.s[lex.buf.free]);
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
   //TODO error = "неполный символ UTF-8"; "недействительный символ UTF-8";
   if (error)
      syntax_error(st, error, lex.line_num, lex.pos, &lex.buf.s[lex.line], &lex.buf.s[lex.buf.free]);

   lexer_free(&lex);
   //TODO количество ошибок не подсчитывается.
   return error ? 1 : 0;

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
