/**\file
 * \brief Интерфейс транслятора исходного текста в байт-код.
 *
 * \defgroup translator Транслятор РЕФАЛ программы.
 *
 */

#pragma once

#include "message.h"
#include "rtrie.h"
#include "refal.h"

/** При трансляции выдаётся замечание о копировании переменной (дорогая операция).*/
#define REFAL_TRANSLATOR_PERFORMANCE_NOTICE_EVAR_COPY

#define REFAL_TRANSLATOR_LOCALS_DEFAULT 100
#define REFAL_TRANSLATOR_EXECS_DEFAULT 100


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
};

/**
 * Переводит исходный текст в пригодный для интерпретации код.
 * \result Оставшееся необработанным количество байт исходного текста.
 * TODO возможна ситуация, когда функция вернёт 0, но трансляция с ошибкой.
 */
size_t refal_translate_to_bytecode(
      struct refal_trie    *ids,    ///< Таблица символов.
      struct refal_vm      *vm,     ///< Память для целевого кода.
      const char           *begin,  ///< Адрес начала исходного текста
      const char           *end,    ///< Адрес за последним символом исходного текста.
      struct refal_message *st
      );
