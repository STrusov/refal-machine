/**\file
 * \brief Реализация стандартной библиотеки РЕФАЛ-5.
 */

#define _POSIX_C_SOURCE 1
#include <limits.h>
#include <stdio.h>

#include "rtrie.h"
#include "library.h"

const struct refal_import_descriptor library[] = {
   // Mu - реализована в исполнителе и должна быть 0-м элементом.
   { "Mu",        { NULL                } },
   { "Print",     { .cfunction = &Print } },
   { "Prout",     { &Prout              } },
   { "Card",      { &Card               } },
   { "Open",      { &Open               } },
   { "Close",     { &Close              } },
   { "Get",       { &Get                } },
   { "Put",       { &Put                } },
   { "Putout",    { &Putout             } },
   { "Add",       { &Add                } },
   { "Sub",       { &Sub                } },
   { "Mul",       { &Mul                } },
   { "Div",       { &Div                } },
   { "Mod",       { &Mod                } },
   { "Compare",   { &Compare            } },
   { "+",         { &Add                } },
   { "-",         { &Sub                } },
   { "*",         { &Mul                } },
   { "/",         { &Div                } },
   { "Push",      { &Push               } },
   { "Pop",       { &Pop                } },
   { "Type",      { &Type               } },
   { "Numb",      { &Numb               } },
   { "Symb",      { &Symb               } },
   { "Ord",       { &Ord                } },
   { "Chr",       { &Chr                } },
   { "GetEnv",    { &GetEnv             } },
   { "Exit",      { .cfunction = &Exit  } },
   { "System",    { &System             } },
   { NULL,        { NULL                } }
};

#define RF_ESC_COLOR_BLACK      "\33[30m"
#define RF_ESC_COLOR_RED        "\33[31m"
#define RF_ESC_COLOR_GREEN      "\33[32m"
#define RF_ESC_COLOR_YELLOW     "\33[33m"
#define RF_ESC_COLOR_BLUE       "\33[34m"
#define RF_ESC_COLOR_MAGENTA    "\33[35m"
#define RF_ESC_COLOR_CYAN       "\33[36m"
#define RF_ESC_COLOR_GRAY       "\33[37m"
#define RF_ESC_RESET            "\33[0m"

#define RF_COLOR_SYMBOL     RF_ESC_COLOR_BLUE
#define RF_COLOR_BRACKET    RF_ESC_COLOR_RED


/**\ingroup library-aux
 *
 * Вводит строку из потока и размещает её в новой памяти.
 * В случае ошибки чтения или конца потока в поле зрения возвращается число 0.
 * В случае вызова для файла, который предварительно не удалось открыть функцией
 * Open, указатель потока равен NULL, так же возвращает 0.
 * \result номер первой ячейки введённых данных.
 */
static inline
rf_index rf_alloc_input(struct refal_vm *vm, FILE *stream)
{
   rf_index i = vm->free;
   if (!stream)
      goto eof;
   unsigned state = 0;
   while (1) {
      int c = fgetc(stream);
      if (c == '\n') {
         break;
      } else if (c == EOF) {
         // Признак конца файла не включается в строку,
         // возвращается по отдельному запросу.
         if (i == vm->free) {
eof:        rf_alloc_int(vm, 0);
         }
         break;
      }
      rf_alloc_char_decode_utf8(vm, (unsigned char)c, &state);
   }
   return i;
}

int Card(struct refal_vm *vm, rf_index prev, rf_index next)
{
   rf_index s1 = vm->u[prev].next;
   if (s1 != next)
      return s1;
   // Поле зрения пусто, просто продолжаем размещать данные в области
   // формирования результата вызывающей функции (исполнителя).
   rf_alloc_input(vm, stdin);
   return 0;
}

/**\ingroup library-aux
 *
 * Вывод подвыражения в поток.
 * Сохраняет подвыражение в поле зрения.
 */
static inline
int rf_output(
      const struct refal_vm *vm,
      rf_index    prev,
      rf_index    next,
      FILE        *stream)
{
   assert(prev != next);
   assert(stream);
   enum rf_opcode prevt = rf_undefined;
   for (rf_index i = prev; (i = vm->u[i].next) != next; ) {
      switch (vm->u[i].tag) {
      case rf_char: {
            char utf8[5];
            utf8[rf_encode_utf8(vm, i, utf8)] = '\0';
            fprintf(stream, "%s", utf8);
            break;
         }
      case rf_number:
         fprintf(stream, prevt == rf_number ? " %li" : "%li", vm->u[i].num);
         break;
      case rf_identifier: ;
         struct rf_id id = vm->u[i].id;
         if (id.tag == rf_id_op_code || id.tag == rf_id_box || id.tag == rf_id_reference) {
            rf_index bytecode = vm->u[id.link].prev;
            if (vm->u[bytecode].tag == rf_name) {
               fprintf(stream, prevt == rf_identifier
                       ? RF_COLOR_SYMBOL" %ls"RF_ESC_RESET
                       : RF_COLOR_SYMBOL"%ls"RF_ESC_RESET,
                       &vm->id.s[vm->u[bytecode].name]);
               break;
            }
         } else if (id.tag == rf_id_mach_code
                 && id.link < vm->library_size && vm->library[id.link].name) {
            fprintf(stream, prevt == rf_identifier
                    ? RF_COLOR_SYMBOL" %s"RF_ESC_RESET
                    : RF_COLOR_SYMBOL"%s"RF_ESC_RESET,
                    vm->library[id.link].name);
            break;
         }
         fprintf(stream, prevt == rf_identifier
                 ? RF_COLOR_SYMBOL" #%x"RF_ESC_RESET
                 : RF_COLOR_SYMBOL"#%x"RF_ESC_RESET,
                 vm->u[i].link);
         break;
      case rf_opening_bracket:
         fprintf(stream, RF_COLOR_BRACKET"("RF_ESC_RESET);
         break;
      case rf_closing_bracket:
         fprintf(stream, RF_COLOR_BRACKET")"RF_ESC_RESET);
         break;
      case rf_open_function:
         fprintf(stream, RF_COLOR_SYMBOL" <"RF_ESC_RESET);
         break;
      case rf_execute:
         fprintf(stream, RF_COLOR_SYMBOL"> "RF_ESC_RESET);
         break;
      case rf_undefined:
      default:
         // TODO ситуация возникать не должна.
         fprintf(stderr, "[%u]: rf_undefined\n", i);
         assert(i);
         return i;
      }
      prevt = vm->u[i].tag;
#ifndef NDEBUG
      fflush(stream);
#endif
   }
   return 0;
}

int Print(const struct refal_vm *vm, rf_index prev, rf_index next)
{
    int r = rf_output(vm, prev, next, stdout);
    fputc('\n', stdout); // в оригинале выводит и при пустом подвыражении.
    return r;
}

int Prout(struct refal_vm *vm, rf_index prev, rf_index next)
{
    int r = Print(vm, prev, next);
    rf_free_evar(vm, prev, next);
    return r;
}

static
FILE *file[REFAL_LIBRARY_LEGACY_FILES];

int Open(struct refal_vm *vm, rf_index prev, rf_index next)
{
   rf_index s = vm->u[prev].next;
   if (s == next || vm->u[s].tag != rf_char)
      return s;
   wchar_t m = vm->u[s].chr;
   if (!(m == 'r' || m == 'w' || m == 'a'))
      return s;

   s = vm->u[s].next;
   if (s == next || vm->u[s].tag != rf_number)
      return s;
   rf_int fno = vm->u[s].num;
   if (!(fno > 0 && fno < REFAL_LIBRARY_LEGACY_FILES))
      return s;

   char path[PATH_MAX + 4];
   unsigned size = 0;
   for (s = vm->u[s].next; s != next; s = vm->u[s].next) {
      if (size >= PATH_MAX)
         return s;
      size += rf_encode_utf8(vm, s, &path[size]);
   }
   if (!size)
      return s;
   path[size] = '\0';

   if (file[fno]) {
      fclose(file[fno]);
   }

   char mode[2] = { (char)m, '\0' };
   file[fno] = fopen(path, mode);

   rf_free_evar(vm, prev, next);
   return 0;
}

int Close(struct refal_vm *vm, rf_index prev, rf_index next)
{
   rf_index s = vm->u[prev].next;
   if (s == next || vm->u[s].tag != rf_number || vm->u[s].next != next)
      return s;

   rf_int fno = vm->u[s].num;
   if (!(fno > 0 && fno < REFAL_LIBRARY_LEGACY_FILES))
      return s;

   if (file[fno]) {
      fclose(file[fno]);
      file[fno] = NULL;
   }
   rf_free_evar(vm, prev, next);
   return 0;
}

int Get(struct refal_vm *vm, rf_index prev, rf_index next)
{
   rf_index s = vm->u[prev].next;
   if (s == next || vm->u[s].tag != rf_number || vm->u[s].next != next)
      return s;

   rf_int fno = vm->u[s].num;
   if (!(fno >= 0 && fno < REFAL_LIBRARY_LEGACY_FILES))
      return s;

   rf_free_evar(vm, prev, next);
   rf_alloc_input(vm, fno ? file[fno] : stdin);
   return 0;
}

int Put(struct refal_vm *vm, rf_index prev, rf_index next)
{
   rf_index s = vm->u[prev].next;
   if (s == next || vm->u[s].tag != rf_number)
      return s;

   rf_int fno = vm->u[s].num;
   if (!(fno >= 0 && fno < REFAL_LIBRARY_LEGACY_FILES))
      return s;

   FILE *f = fno ? file[fno] : stdout;
   int r = rf_output(vm, s, next, f);
   rf_free_evar(vm, prev, vm->u[s].next);
   fputc('\n', f); // в оригинале выводит и при пустом подвыражении.
   return r;
}

int Putout(struct refal_vm *vm, rf_index prev, rf_index next)
{
   int r = Put(vm, prev, next);
   rf_free_evar(vm, prev, next);
   return r;
}


typedef rf_int aop(rf_int s1, rf_int s2);

static inline
int calc(struct refal_vm *vm, rf_index prev, rf_index next, aop *op)
{
   rf_index s1 = vm->u[prev].next;
   if (s1 == next)
      return s1;
   rf_index s2 = vm->u[s1].next;
   if (vm->u[s2].next != next)
      return s2;
   if (vm->u[s1].tag != rf_number)
      return s1;
   if (vm->u[s2].tag != rf_number)
      return s2;
   vm->u[s1].num = op(vm->u[s1].num, vm->u[s2].num);
   rf_free_evar(vm, s1, next);
   return 0;
}

static inline rf_int plus(rf_int s1, rf_int s2) { return s1 + s2; }

int Add(struct refal_vm *vm, rf_index prev, rf_index next)
{
   return calc(vm, prev, next, plus);
}

static inline rf_int minus(rf_int s1, rf_int s2) { return s1 - s2; }

int Sub(struct refal_vm *vm, rf_index prev, rf_index next)
{
   return calc(vm, prev, next, minus);
}

static inline rf_int multiplies(rf_int s1, rf_int s2) { return s1 * s2; }

int Mul(struct refal_vm *vm, rf_index prev, rf_index next)
{
   return calc(vm, prev, next, multiplies);
}

static inline rf_int divides(rf_int s1, rf_int s2) { return s2 ? s1/s2 : s2; }

int Div(struct refal_vm *vm, rf_index prev, rf_index next)
{
   return calc(vm, prev, next, divides);
}

static inline rf_int modulus(rf_int s1, rf_int s2) { return s2 ? s1%s2 : s2; }

int Mod(struct refal_vm *vm, rf_index prev, rf_index next)
{
   return calc(vm, prev, next, modulus);
}

int Compare(struct refal_vm *vm, rf_index prev, rf_index next)
{
   rf_index s1 = vm->u[prev].next;
   if (s1 == next)
      return s1;
   rf_index s2 = vm->u[s1].next;
   if (vm->u[s2].next != next)
      return s2;
   vm->u[s1].tag = rf_char;
   if (vm->u[s1].num < vm->u[s2].num) {
      vm->u[s1].data = '-';
   } else if (vm->u[s1].num > vm->u[s2].num) {
      vm->u[s1].data = '+';
   } else {
      vm->u[s1].data = '0';
   }
   rf_free_evar(vm, s1, next);
   return 0;
}

// Связываем в односвязный список.
int Push(struct refal_vm *vm, rf_index prev, rf_index next)
{
   struct rf_id id = rtrie_find_value_by_tags(vm->rt, rf_id_reference, rf_id_box, vm, prev, next);
   if ((id.tag == rf_id_reference || id.tag == rf_id_box) && id.link != -1 && id.link) {
      assert(vm->u[id.link].tag == rf_sentence);
      if (vm->u[id.link].tag != rf_sentence)
         return prev;
      //TODO ссылку хорошо бы проверять на выход за пределы, но сейчас опкоды создаём сами.
      rf_index s_next = vm->u[id.link].link;
      // было:  [rf_name][rf_sentence][...][s_next]
      //                       --------------->
      // стало: [rf_name][rf_sentence] +++ [e-var][s_new] +++ [...][s_next]
      //                       -------------------->  --------------->
      rf_index guard = rf_alloc_value(vm, 0, rf_undefined);
      vm->u[id.link].data = rf_alloc_value(vm, s_next, rf_sentence);
      rf_splice_evar_prev(vm, guard, vm->free, vm->u[id.link].next);
      rf_free_last(vm);
      rf_splice_evar_prev(vm, prev, next, vm->u[id.link].next);
      return 0;
   }
   return prev;
}

int Pop(struct refal_vm *vm, rf_index prev, rf_index next)
{
   struct rf_id id = rtrie_find_value_by_tags(vm->rt, rf_id_reference, rf_id_box, vm, prev, next);
   if (!rf_is_evar_empty(vm, prev, next)) {
      return prev;
   }
   //TODO ссылку хорошо бы проверять на выход за пределы, но сейчас опкоды создаём сами.
   if ((id.tag == rf_id_reference || id.tag == rf_id_box) && id.link != -1 && id.link) {
      assert(vm->u[id.link].tag == rf_sentence);
      if (vm->u[id.link].tag != rf_sentence)
         return prev;
      rf_index s_next = vm->u[id.link].link;
      assert(vm->u[s_next].tag == rf_sentence || vm->u[s_next].tag == rf_name);
      rf_splice_evar_prev(vm, id.link, s_next, next);
      // id.value менять нельзя, потому первую rf_sentence не удаляем.
      // Таким образом из пустого ящика всегда извлекается пустая e-переменная.
      if (vm->u[s_next].tag == rf_name)
         return 0;
      if (vm->u[s_next].tag == rf_sentence) {
         vm->u[id.link].data = vm->u[s_next].link;
         rf_free_evar(vm, id.link, vm->u[s_next].next);
         return 0;
      }
   }
   return prev;
}


int Type(struct refal_vm *vm, rf_index prev, rf_index next)
{
   rf_index s = vm->u[prev].next;
   char subtype = '0';
   char type = '?';
   rf_index result = refal_vm_alloc_1(vm);
   if (s == next) {
      rf_alloc_char(vm, '*');
   } else {
      switch (vm->u[s].tag) {
      case rf_identifier:
         type = 'W';
         subtype = 'i';
         break;
      case rf_number:
         type = 'N';
         break;
      case rf_opening_bracket:
         type = 'B';
         break;
      case rf_char:
         switch (vm->u[s].chr) {
         case '0'...'9':
            type = 'D';
            break;
         case 'A'...'Z':
            type = 'L';
            subtype = 'u';
            break;
         case 'a'...'z':
            type = 'L';
            subtype = 'l';
            break;
         case '\0'...' '-1:
            type = 'O';
            break;
         default:
            type = 'P';
            break;
         }
         break;
      default:
         assert(0);
      }
   }
   rf_alloc_char(vm, type);
   rf_alloc_char(vm, subtype);
   rf_splice_evar_prev(vm, result, vm->free, s);
   rf_free_last(vm);
   return 0;
}

int Numb(struct refal_vm *vm, rf_index prev, rf_index next)
{
   rf_int result = 0;
   for (rf_index s = vm->u[prev].next; s != next; s = vm->u[s].next) {
      if (vm->u[s].tag != rf_char)
         break;
      wchar_t c = vm->u[s].chr;
      if (c < '0' || c > '9')
         break;
      result = 10 * result + c - '0';
   }
   rf_free_evar(vm, prev, next);
   rf_alloc_int(vm, result);
   return 0;
}

int Symb(struct refal_vm *vm, rf_index prev, rf_index next)
{
   rf_index s = vm->u[prev].next;
   if (s == next || vm->u[s].tag != rf_number || vm->u[s].next != next)
      return s;

   rf_int num = vm->u[s].num;
   rf_free_evar(vm, prev, next);

   // TODO учесть остальные архитектуры.
   unsigned long unum = num;
   if (num < 0) {
      unum = -num;
      rf_alloc_char(vm, '-');
   }
   char digits[8 * sizeof(unum) * /* lg(2) */ 3/10];
   unsigned n = 0;
   do {
      assert(n < sizeof(digits));
      digits[n++] = unum % 10 + '0';
      unum /= 10;
   } while (unum);
   while (n--) {
      rf_alloc_char(vm, digits[n]);
   }
   return 0;
}

int Ord(struct refal_vm *vm, rf_index prev, rf_index next)
{
   for (rf_index s = vm->u[prev].next; s != next; s = vm->u[s].next) {
      if (vm->u[s].tag == rf_char)
         vm->u[s].tag = rf_number;
   }
   return 0;
}

int Chr(struct refal_vm *vm, rf_index prev, rf_index next)
{
   for (rf_index s = vm->u[prev].next; s != next; s = vm->u[s].next) {
      if (vm->u[s].tag == rf_number)
         vm->u[s].tag = rf_char;
   }
   return 0;
}


int GetEnv(struct refal_vm *vm, rf_index prev, rf_index next)
{
   extern char **environ;
   rf_index name = vm->u[prev].next;
   char **env = environ;
   while (*env) {
      const char *restrict ename = *env;
      for (rf_index s = name; s != next; s = vm->u[s].next) {
         if (vm->u[s].tag != rf_char) {
            return s;
         }
         char utf8[4];
         unsigned n = rf_encode_utf8(vm, s, utf8);
         // Имя не может содержать =
         if (!*utf8 || *utf8 == '=')
            goto exit;
         for (unsigned i = 0; i != n; ++i)
            if (utf8[i] != *ename++)
               goto next_var;
      }
      if (*ename++ == '=') {
         rf_alloc_string(vm, ename);
         goto exit;
      }
next_var:
      ++env;
   }
exit:
   rf_free_evar(vm, prev, next);
   return 0;
}

int Exit(const struct refal_vm *vm, rf_index prev, rf_index next)
{
   rf_index s = vm->u[prev].next;
   if (s == next || vm->u[s].tag != rf_number || vm->u[s].next != next)
      return s;

   rf_int status = vm->u[s].num;
   exit(status);
}

int System(struct refal_vm *vm, rf_index prev, rf_index next)
{
   // TODO PATH_MAX имеет отдалённое отношение к system().
   char path[PATH_MAX + 4];
   unsigned size = 0;
   for (rf_index s = vm->u[prev].next; s != next; s = vm->u[s].next) {
      if (size >= PATH_MAX || vm->u[s].tag != rf_char)
         return s;
      size += rf_encode_utf8(vm, s, &path[size]);
   }
   path[size] = '\0';
   rf_int res;
   if (size) {
      fflush(stdout);
      res = system(path);
   } else {
      res = system(NULL);
   }

   rf_free_evar(vm, prev, next);
   rf_alloc_int(vm, res);
   return 0;
}
