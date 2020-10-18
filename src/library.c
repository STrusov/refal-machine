/**\file
 * \brief Реализация стандартной библиотеки РЕФАЛ-5.
 */

#include <stdio.h>

#include "library.h"

const struct refal_import_descriptor library[] = {
   { "Print",     { .cfunction = &Print } },
   { "Prout",     { &Prout              } },
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

/**\ingroup lib-aux
 *
 * Вывод подвыражения в поток.
 * Сохраняет подвыражение в поле зрения.
 */
static inline
void rf_output(
      const rf_vm *restrict vm,
      rf_index    prev,
      rf_index    next,
      FILE        *stream)
{
   assert(prev != next);
   enum rf_type prevt = rf_undefined;
   for (rf_index i = prev; (i = vm->cell[i].next) != next; ) {
      switch (vm->cell[i].tag) {
      case rf_char:
         fprintf(stream, "%C", vm->cell[i].chr);
         break;
      case rf_number:
         fprintf(stream, prevt == rf_number ? " %li" : "%li", vm->cell[i].num);
         break;
      case rf_atom:
         fprintf(stream, prevt == rf_atom
                 ? RF_COLOR_SYMBOL" %s"RF_ESC_RESET
                 : RF_COLOR_SYMBOL"%s"RF_ESC_RESET,
                 vm->cell[i].atom);
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
         assert(0);
      }
      prevt = vm->cell[i].tag;
#ifndef NDEBUG
      fflush(stream);
#endif
   }
}

void Print(const rf_vm *restrict vm, rf_index prev, rf_index next)
{
    rf_output(vm, prev, next, stdout);
    putchar('\n'); // в оригинале выводит и при пустом подвыражении.
}

void Prout(rf_vm *restrict vm, rf_index prev, rf_index next)
{
    Print(vm, prev, next);
    rf_free_evar(vm, prev, next);
}
