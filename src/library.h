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
 * \defgroup library-io    Функции ввода-вывода.
 * \defgroup library-math  Арифметические функции.
 * \defgroup library-str   Обработка символов и строк.
 */

#include <assert.h>
#include "refal.h"

#pragma once

/**\addtogroup library-aux Вспомогательные функции.
 * Не вызываются из РЕФАЛ-програм непосредственно.
 * \{
 */
enum { refal_library_size = 2 };

extern
const struct refal_import_descriptor library[refal_library_size + 1];

static inline
int refal_library_call(
      rf_vm    *vm,
      rf_index prev,
      rf_index next,
      rf_index ordinal)
{
   assert(ordinal < refal_library_size);
   return library[ordinal].function(vm, prev, next);
}

/**\}*/
/**\}*/

rf_function  Card;
rf_cfunction Print;
rf_function  Prout;

/**\addtogroup library-io
 * \{
 */

/**
 * Вывод подвыражения в стандартный поток вывода с переводом строки.
 * Сохраняет подвыражение в поле зрения.
 *
        <Print e.Expr> == e.Expr
 */
int Print(const rf_vm *restrict vm, rf_index prev, rf_index next);

/**
 * Вывод подвыражения в стандартный поток вывода с переводом строки.
 * Удаляет подвыражение из поля зрения.
 *
        <Prout e.Expr> == []
 */
int Prout(rf_vm *restrict vm, rf_index prev, rf_index next);

/**\}*/

/**\addtogroup library-aux
 * \{
 */

/**
 * Отображает файл в память.
 *
 * \retval size Размер содержимого.
 * \result Указатель на начало файла либо \c MAP_FAILED при ошибке.
 */
static inline
void *mmap_file(
      const char  *name,   ///< Имя файла.
      size_t      *size)   ///< Указатель для сохранения результата.
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
