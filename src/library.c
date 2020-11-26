/**\file
 * \brief Реализация стандартной библиотеки РЕФАЛ-5.
 */

#include <stdio.h>

#include "library.h"

const struct refal_import_descriptor library[] = {
   // Mu - реализована в интерпретаторе и должна быть 0-м элементом.
   { "Mu",        { NULL                } },
   { "Print",     { .cfunction = &Print } },
   { "Prout",     { &Prout              } },
   { "Card",      { &Card               } },
   { "Add",       { &Add                } },
   { "Sub",       { &Sub                } },
   { "Mul",       { &Mul                } },
   { "Div",       { &Div                } },
   { "+",         { &Add                } },
   { "-",         { &Sub                } },
   { "*",         { &Mul                } },
   { "/",         { &Div                } },
   { "Numb",      { &Numb               } },
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
 * \result номер первой ячейки введённых данных.
 */
static inline
rf_index rf_alloc_input(
      rf_vm *restrict vm, FILE *stream)
{
   rf_index i = vm->free;
   unsigned state = 0;
   while (1) {
      int c = fgetc(stream);
      if (c == '\n') {
         break;
      } else if (c == EOF) {
         // Признак конца файла не включается в строку,
         // возвращается по отдельному запросу.
         if (i == vm->free) {
            rf_alloc_int(vm, 0);
         }
         break;
      }
      rf_alloc_char_decode_utf8(vm, (unsigned char)c, &state);
   }
   return i;
}

int Card(rf_vm *restrict vm, rf_index prev, rf_index next)
{
   rf_index s1 = vm->u[prev].next;
   if (s1 != next)
      return s1;
   // Поле зрения пусто, просто продолжаем размещать данные в области
   // формирования результата вызывающей функции (интерпретатора).
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
      const rf_vm *restrict vm,
      rf_index    prev,
      rf_index    next,
      FILE        *stream)
{
   assert(prev != next);
   enum rf_type prevt = rf_undefined;
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
      case rf_atom:
         fprintf(stream, prevt == rf_atom
                 ? RF_COLOR_SYMBOL" %s"RF_ESC_RESET
                 : RF_COLOR_SYMBOL"%s"RF_ESC_RESET,
                 vm->u[i].atom);
         break;
      case rf_opening_bracket:
         fprintf(stream, RF_COLOR_BRACKET"("RF_ESC_RESET);
         break;
      case rf_closing_bracket:
         fprintf(stream, RF_COLOR_BRACKET")"RF_ESC_RESET);
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

int Print(const rf_vm *restrict vm, rf_index prev, rf_index next)
{
    int r = rf_output(vm, prev, next, stdout);
    putchar('\n'); // в оригинале выводит и при пустом подвыражении.
    return r;
}

int Prout(rf_vm *restrict vm, rf_index prev, rf_index next)
{
    int r = Print(vm, prev, next);
    rf_free_evar(vm, prev, next);
    return r;
}


typedef rf_int aop(rf_int s1, rf_int s2);

static inline
int calc(rf_vm *restrict vm, rf_index prev, rf_index next, aop *op)
{
   rf_index s1 = vm->u[prev].next;
   if (s1 == next)
      return s1;
   rf_index s2 = vm->u[s1].next;
   if (vm->u[s2].next != next)
      return s2;
   vm->u[s1].num = op(vm->u[s1].num, vm->u[s2].num);
   rf_free_evar(vm, s1, next);
   return 0;
}

static inline rf_int plus(rf_int s1, rf_int s2) { return s1 + s2; }

int Add(rf_vm *restrict vm, rf_index prev, rf_index next)
{
   return calc(vm, prev, next, plus);
}

static inline rf_int minus(rf_int s1, rf_int s2) { return s1 - s2; }

int Sub(rf_vm *restrict vm, rf_index prev, rf_index next)
{
   return calc(vm, prev, next, minus);
}

static inline rf_int multiplies(rf_int s1, rf_int s2) { return s1 * s2; }

int Mul(rf_vm *restrict vm, rf_index prev, rf_index next)
{
   return calc(vm, prev, next, multiplies);
}

static inline rf_int divides(rf_int s1, rf_int s2) { return s2 ? s1/s2 : s2; }

int Div(rf_vm *restrict vm, rf_index prev, rf_index next)
{
   return calc(vm, prev, next, divides);
}


int Numb(rf_vm *restrict vm, rf_index prev, rf_index next)
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
