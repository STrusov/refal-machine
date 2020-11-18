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
            wchar_t chr = vm->u[i].chr;
            if (chr < 0x80) {
               utf8[0] = chr;
               utf8[1] = '\x0';
            } else if (chr < 0x800) {
               utf8[0] = 0xc0 | (chr >> 6);
               utf8[1] = 0x80 | (chr & 0x3f);
               utf8[2] = '\x0';
            } else if (chr < 0x10000) {
               utf8[0] = 0xe0 | (chr >> 12);
               utf8[1] = 0x80 | ((chr >> 6) & 0x3f);
               utf8[2] = 0x80 | (chr & 0x3f);
               utf8[3] = '\x0';
            } else {
               utf8[0] = 0xf0 | ( chr >> 18);
               utf8[1] = 0x80 | ((chr >> 12) & 0x3f);
               utf8[2] = 0x80 | ((chr >>  6) & 0x3f);
               utf8[3] = 0x80 | (chr & 0x3f);
               utf8[4] = '\x0';
            }
            fprintf(stream, utf8);
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
