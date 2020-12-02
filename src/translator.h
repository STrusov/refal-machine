/**\file
 * \brief Интерфейс транслятора исходного текста в байт-код.
 *
 * \addtogroup translator Транслятор РЕФАЛ программы.
 * \{
 */

#pragma once

#include "rtrie.h"
#include "refal.h"

/** При трансляции выдаётся замечание о копировании переменной (дорогая операция).*/
#define REFAL_TRANSLATOR_PERFORMANCE_NOTICE_EVAR_COPY

#define REFAL_TRANSLATOR_LOCALS_DEFAULT   128
#define REFAL_TRANSLATOR_EXECS_DEFAULT    128
#define REFAL_TRANSLATOR_BRACKETS_DEFAULT 128

/**
 * Конфигурация транслятора.
 *
 * Транслятор встраиваемый. Что бы обеспечить возможность работы без аллокаций,
 * часть массивов расположена в стеке. Их размер, вероятно, потребуется изменять.
 * TODO нужен ли режим с alloc() + realloc() для произвольного размера?
 */
struct refal_translator_config {
   unsigned locals_limit;  ///< Допустимое количество переменных в предложении.
   unsigned execs_limit;   ///< Допустимое количество вложенных вызовов функций.
   unsigned brackets_limit;///< Допустимое количество вложенных скобок.

   ///\{ Выводить предупреждения
   unsigned warn_implicit_declaration:1;  ///< неявное определение идентификатора.
   ///\}

   ///\{ Выводить замечания
   unsigned notice_copy:1; ///< копирование переменных.
   ///\}
};

/**
 * Заносит в таблицу символов информацию о функциях в машинном коде.
 * \result количество импортированных функций.
 */
static inline
unsigned refal_import(
      struct refal_trie                    *ids,   ///< Таблица символов.
      const struct refal_import_descriptor *lib)   ///< Массив описателей импорта.
{
   unsigned ordinal;
   for (ordinal = 0; lib->name; ++lib, ++ordinal) {
      const char *p = lib->name;
      assert(p);
      rtrie_index idx = rtrie_insert_first(ids, *p++);
      while (*p) {
         idx = rtrie_insert_next(ids, idx, *p++);
      }
      ids->n[idx].val.tag   = rft_machine_code;
      ids->n[idx].val.value = ordinal;
   }
   return ordinal;
}

/**
 * Переводит исходный текст в байт-код для интерпретатора.
 * При этом заполняется таблица символов.
 * \retval cfg->locals_limit если передано 0, корректирует значение (требуется интерпретатору)
 * \return 0 при успехе, -1 в случае отсутствия фала, иначе
 * \see `refal_translate_to_bytecode()`.
 */
int refal_translate_file_to_bytecode(
      struct refal_translator_config   *cfg, ///< Конфигурация.
      struct refal_vm      *vm,     ///< Память для целевого кода.
      struct refal_trie    *ids,    ///< Таблица символов.
      const char           *name,   ///< имя файла с исходным текстом.
      struct refal_message *st
      );

/**
 * Переводит исходный текст в пригодный для интерпретации код.
 * \result количество ошибок.
 */
int refal_translate_to_bytecode(
      struct refal_translator_config   *cfg, ///< Конфигурация.
      struct refal_vm      *vm,     ///< Память для целевого кода.
      struct refal_trie    *ids,    ///< Таблица символов.
      rtrie_index          module,  ///< Пространство имён модуля (0 - глобальное).
      const char           *begin,  ///< Адрес начала исходного текста
      const char           *end,    ///< Адрес за последним символом исходного текста.
      struct refal_message *st
      );

/**\}*/
