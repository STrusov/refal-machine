/**
 * \file
 * Описание структур данных и имплементация вспомогательных функций,
 * в совокупности образующих РЕФАЛ-машину, способную взаимодействовать
 * с реализованными на языках C и C++ (под)программами.
 */

#pragma once

#include <assert.h>
#include <stdint.h>
#include <wchar.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct refal_message;

/**\addtogroup internal Внутреннее устройство РЕФАЛ-машины.
 * \{
 */

/**
 * Тип данных, хранящихся в ячейке памяти РЕФАЛ-машины.
 */
typedef enum rf_type {
   rf_undefined,        ///< Пустая ячейка (не инициализирована).
   rf_machine_code,     ///< Ссылка на машинный код (функцию).
   _rf_types_count
} rf_type;

/**
 * Адресует ячейки памяти РЕФАЛ-машины.
 */
typedef uint32_t rf_index;


/**
 * Описатель РЕФАЛ-машины.
 */
typedef struct rf_vm {
} rf_vm;

/**\}*/

/**\ingroup library
 *
 * Прототип функции, не изменяющей состояние РЕФАЛ-машины.
 * \param vm   Указатель на объект виртуальной машины.
 * \param prev Элемент перед первым поля зрения функции.
 * \param next Элемент после последнего поля зрения функции.
 */
typedef void rf_cfunction(const rf_vm *restrict vm, rf_index prev, rf_index next);

/**\ingroup library
 *
 * Прототип функции, изменяющей состояние РЕФАЛ-машины.
 * \param vm   Указатель на объект виртуальной машины.
 * \param prev Элемент перед первым поля зрения функции.
 * \param next Элемент после последнего поля зрения функции.
 */
typedef void rf_function(rf_vm *restrict vm, rf_index prev, rf_index next);

/**
 * Связывает имя функции (текстовое) с её реализацией.
 */
struct refal_import_descriptor {
   const char  *name;
   // Вызывающая сторона считает, что функция меняет состояние РЕФАЛ-машины.
   // Вариант с константностью добавлен для наглядности определений
   // и что бы избежать приведений типа.
   union {
      rf_function    *function;
      rf_cfunction   *cfunction;
   };
};


/**\}*/
