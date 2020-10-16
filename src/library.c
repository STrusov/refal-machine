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

void Print(const rf_vm *restrict vm, rf_index prev, rf_index next)
{
//    rf_output(vm, prev, next, stdout);
    putchar('\n'); // в оригинале выводит и при пустом подвыражении.
}

void Prout(rf_vm *restrict vm, rf_index prev, rf_index next)
{
    Print(vm, prev, next);
//    rf_free_evar(vm, prev, next);
}
