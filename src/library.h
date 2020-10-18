/**\file
 * \brief Интерфейс стандартной библиотеки РЕФАЛ.
 *
 * \addtogroup library Библиотечные функции РЕФАЛ-машины.
 *
 * В базовую библиотеку входят функции, которые затруднительно выразить на РЕФАЛ.
 *
 * Часть поля зрения, обрабатываемая функцией, называется _подвыражением_.
 * _Активное подвыражение_ включает в себя имя функции.
 *
 * Реализации функций принимают параметрами:
 * - адрес объекта с состоянием РЕФАЛ-машины;
 * - номер ячейки, предшествующей начальной обрабатываемого подвыражения;
 * - номер ячейки, следующей за конечной ячейкой обрабатываемого подвыражения.
 *
 * \{
 */

#include <assert.h>
#include "refal.h"

#pragma once

enum { refal_library_size = 2 };

extern
const struct refal_import_descriptor library[refal_library_size + 1];

static inline
void refal_library_call(
      rf_vm    *vm,
      rf_index prev,
      rf_index next,
      rf_index ordinal)
{
   assert(ordinal < refal_library_size);
   library[ordinal].function(vm, prev, next);
}

rf_cfunction Print;
rf_function  Prout;

/**
 * Вывод подвыражения в стандартный поток вывода с переводом строки.
 * Сохраняет подвыражение в поле зрения.
 *
        <Print e.Expr> == e.Expr
 */
void Print(const rf_vm *restrict vm, rf_index prev, rf_index next);

/**
 * Вывод подвыражения в стандартный поток вывода с переводом строки.
 * Удаляет подвыражение из поля зрения.
 *
        <Prout e.Expr> == []
 */
void Prout(rf_vm *restrict vm, rf_index prev, rf_index next);

/**\}*/

/**\defgroup lib-aux Вспомогательные функции.
 * \{
 */

/**
 * Отображает файл в память.
 *
 * \result Указатель на начало файла либо \c MAP_FAILED при ошибке.
 */
static inline
void *mmap_file(
      const char  *name,   ///< Имя файла.
      size_t      *size)   ///\retval size Размер содержимого.
{
   void *addr = MAP_FAILED;
   const int fd = open(name, O_RDONLY);
   if (fd >= 0) {
      struct stat sb;
      if (fstat(fd, &sb) >= 0) {
         *size = sb.st_size;
         addr = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
      }
      close(fd);
   }
   return addr;
}

/**\}*/
